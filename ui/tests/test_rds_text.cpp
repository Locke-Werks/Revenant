// The Annex E mapping, the segment placeholders and the four states an
// empty RDS pane can be in.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS, in its own
// comment, for the reason test_audio_ring.cpp gives.
//
// The wrong implementations on this path are QString::fromLatin1 over the
// raw bytes, which renders a different letter rather than a broken one and
// never faults; a renderer that treats an unreceived segment as spaces,
// which shows a half-arrived call sign as a complete short one; and a pane
// that draws the station's fields without checking fault and discarding
// first, which core/rpc/types.h asks for twice in as many words.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "core/rpc/types.h"
#include "models/rds_text.h"
#include "models/rds_view.h"

using revenant::ui::annex_e_to_utf8;
using revenant::ui::block_error_rate;
using revenant::ui::make_rds_view;
using revenant::ui::RdsState;
using revenant::ui::render_rds_text;

namespace {

[[nodiscard]] std::vector<std::uint8_t> bytes_of(const char* ascii)
{
    std::vector<std::uint8_t> out;
    for (const char* p = ascii; *p != '\0'; ++p) {
        out.push_back(static_cast<std::uint8_t>(*p));
    }
    return out;
}

// The station the operator saw on 2026-09-20: PI 0x2AF6, KKFM, PS "KKFM",
// RadioText "98.1 KKFM Weekends", PTY 6 Classic Rock.
[[nodiscard]] revenant::rpc::RdsStation kkfm()
{
    revenant::rpc::RdsStation station;
    station.pi = 0x2AF6;
    station.pi_valid = true;
    station.call_sign = "KKFM";
    station.pty = 6;
    station.pty_valid = true;
    station.pty_short_name = "Classic";
    station.pty_long_name = "Classic Rock";
    station.tp = true;
    station.tp_valid = true;

    station.ps = bytes_of("KKFM    ");
    station.ps_received = 0x0F;

    station.rt = bytes_of("98.1 KKFM Weekends");
    station.rt_received = 0xFFFF;
    station.rt_length = 18;
    station.rt_version_b = false;

    station.health.lock = revenant::rpc::RdsLock::Locked;
    station.health.sync = revenant::rpc::RdsSync::Synced;
    station.health.blocks_good = 873;
    station.health.blocks_corrected = 90;
    station.health.blocks_dropped = 37;
    station.health.bit_rate_hz = 1187.49;
    return station;
}

}  // namespace

TEST_CASE("the upper half is Annex E and not Latin-1", "[rds]")
{
    // 0xE0 IS THE ONE THAT MAKES THE ACCIDENT INVISIBLE. Latin-1 renders it
    // à and Annex E renders it Ã, so a byte handed to QString::fromLatin1
    // produces a plausible letter in the wrong place and nothing faults.
    // U+00C3 is "\xC3\x83" in UTF-8 and U+00E0 is "\xC3\xA0".
    CHECK(annex_e_to_utf8(0xE0) == "\xC3\x83");
    CHECK(annex_e_to_utf8(0xE0) != "\xC3\xA0");

    // Three more where the two tables disagree, chosen because each is a
    // real letter under both readings.
    CHECK(annex_e_to_utf8(0x80) == "\xC3\xA1");  // á, which Latin-1 has no glyph for
    CHECK(annex_e_to_utf8(0xC0) == "\xC3\x81");  // Á, Latin-1 À
    CHECK(annex_e_to_utf8(0xD6) == "\xC3\x94");  // Ô, Latin-1 Ö

    // Positions that are not in Latin-1 at all, so an implementation that
    // got this far by luck stops here. U+20AC is the euro sign and U+2192
    // is a rightwards arrow.
    CHECK(annex_e_to_utf8(0xA9) == "\xE2\x82\xAC");
    CHECK(annex_e_to_utf8(0xAE) == "\xE2\x86\x92");

    // 0xFF is a no-break space, which is two bytes in UTF-8 and one in
    // Latin-1.
    CHECK(annex_e_to_utf8(0xFF) == "\xC2\xA0");

    // Every position in the upper half produces something. An
    // implementation with a gap in the table would render a hole as a
    // space and show a message as complete.
    for (int byte = 0x80; byte <= 0xFF; ++byte) {
        CHECK_FALSE(annex_e_to_utf8(static_cast<std::uint8_t>(byte)).empty());
    }
}

TEST_CASE("the graphic ASCII range passes through", "[rds]")
{
    for (int byte = 0x20; byte <= 0x7E; ++byte) {
        const std::string mapped = annex_e_to_utf8(static_cast<std::uint8_t>(byte));
        REQUIRE(mapped.size() == 1);
        CHECK(mapped[0] == static_cast<char>(byte));
    }

    // Control codes render as nothing, including the RadioText terminator.
    // An implementation that passed them through puts a carriage return
    // into a QString and the label ends mid-word with no explanation.
    CHECK(annex_e_to_utf8(0x00).empty());
    CHECK(annex_e_to_utf8(0x0A).empty());
    CHECK(annex_e_to_utf8(0x0D).empty());
    CHECK(annex_e_to_utf8(0x1F).empty());
    CHECK(annex_e_to_utf8(0x7F).empty());
}

TEST_CASE("an unreceived segment is not a space", "[rds]")
{
    // THE FAILURE THIS EXISTS TO STOP. "KKFM" with only the first two
    // segments received is "KK" plus four bytes nobody sent, and an
    // implementation that rendered them as spaces shows "KK" as a finished
    // call sign. U+00B7 is "\xC2\xB7" in UTF-8.
    const auto ps = bytes_of("KKFM    ");
    const auto half = render_rds_text(ps, 0x03, 2, 8);

    CHECK(half.text == "KKFM\xC2\xB7\xC2\xB7\xC2\xB7\xC2\xB7");
    CHECK(half.segments_received == 2);
    CHECK(half.segments_total == 4);
    CHECK_FALSE(half.empty());

    // Fully received, the padding goes and nothing is left behind.
    const auto whole = render_rds_text(ps, 0x0F, 2, 8);
    CHECK(whole.text == "KKFM");
    CHECK(whole.segments_received == 4);

    // Nothing at all, which is four placeholders and not an empty string:
    // an empty string is what a station transmitting eight spaces gives,
    // and those are different claims.
    const auto none = render_rds_text(ps, 0x00, 2, 8);
    CHECK(none.text == "\xC2\xB7\xC2\xB7\xC2\xB7\xC2\xB7"
                       "\xC2\xB7\xC2\xB7\xC2\xB7\xC2\xB7");
    CHECK(none.empty());
}

TEST_CASE("a placeholder holds the spaces before it open", "[rds]")
{
    // A gap in the middle. AN IMPLEMENTATION THAT TRIMMED TRAILING SPACES
    // BEFORE PLACING THE PLACEHOLDERS loses the space between the words and
    // renders "ABCD" + placeholders, which reads as one word.
    const auto text = bytes_of("AB  EFGH");
    const auto rendered = render_rds_text(text, 0x0B, 2, 8);  // segment 2 missing
    CHECK(rendered.text == "AB  \xC2\xB7\xC2\xB7GH");
    CHECK(rendered.segments_received == 3);
}

TEST_CASE("RadioText segment width follows the group version", "[rds]")
{
    // 2A groups carry four characters per segment and 2B carry two.
    // GETTING IT WRONG MISPLACES EVERY PLACEHOLDER rather than failing, so
    // there is nothing to notice at runtime: the text looks decoded and the
    // holes are in the wrong places.
    const auto text = bytes_of("ABCDEFGH");

    const auto as_2a = render_rds_text(text, 0x01, 4, 8);
    CHECK(as_2a.text == "ABCD\xC2\xB7\xC2\xB7\xC2\xB7\xC2\xB7");
    CHECK(as_2a.segments_total == 2);

    const auto as_2b = render_rds_text(text, 0x01, 2, 8);
    CHECK(as_2b.text == "AB\xC2\xB7\xC2\xB7\xC2\xB7\xC2\xB7\xC2\xB7\xC2\xB7");
    CHECK(as_2b.segments_total == 4);
}

TEST_CASE("rt_length is where the message stops", "[rds]")
{
    // The terminator is inside the payload and a client scanning for it
    // cannot tell an unreceived byte from a transmitted one, which is why
    // the length comes over the wire. An implementation that rendered the
    // whole buffer would append whatever the previous, longer message left
    // behind.
    auto buffer = bytes_of("98.1 KKFM Weekends...leftover");
    const auto rendered = render_rds_text(buffer, 0xFFFF, 4, 18);
    CHECK(rendered.text == "98.1 KKFM Weekends");

    // A length past the buffer is clamped rather than read off the end.
    const auto over = render_rds_text(buffer, 0xFFFF, 4, 500);
    CHECK(over.text == "98.1 KKFM Weekends...leftover");
}

TEST_CASE("the four empty panes are four different sentences", "[rds]")
{
    // core/rpc/types.h asks for this check twice, on fault and on
    // discarding, and both notes end with an instruction to make it before
    // drawing anything else. A PANE THAT SKIPPED IT renders the struct's
    // defaults and shows a faulted decoder, a retuning one, an unlocked one
    // and a station with no RDS identically.
    revenant::rpc::RdsStation station;

    const auto idle = make_rds_view(station, false);
    CHECK(idle.state == RdsState::Idle);
    CHECK_FALSE(idle.status.empty());

    const auto unlocked = make_rds_view(station, true);
    CHECK(unlocked.state == RdsState::Unlocked);
    CHECK_FALSE(unlocked.is_fault);

    station.discarding = true;
    CHECK(make_rds_view(station, true).state == RdsState::Retuning);

    // fault outranks discarding, because the fields beside it are frozen
    // and every counter would otherwise be read as current.
    station.fault = "the decoder was built for mono and the receiver delivered a pair";
    const auto faulted = make_rds_view(station, true);
    CHECK(faulted.state == RdsState::Faulted);
    CHECK(faulted.is_fault);
    CHECK(faulted.status.find("mono") != std::string::npos);

    // Every one of the four says something. An empty status is the state
    // this replaces.
    for (const auto& view : {idle, unlocked, faulted}) {
        CHECK_FALSE(view.status.empty());
    }
}

TEST_CASE("a pane with nothing behind it does not blame the operator", "[rds]")
{
    // THE WRONG IMPLEMENTATION THIS REJECTS: reading answered=false as
    // "the operator has the switch off". EngineLink::clear_rds is called
    // on three paths and only one of them is that. On the other two the
    // window is asking for RDS as hard as it can, and "not asking for RDS
    // on this receiver." is the exact opposite of what is true: it sends
    // the operator to a switch that is already on.
    const revenant::rpc::RdsStation empty;

    const auto off = make_rds_view(empty, false);
    CHECK(off.state == RdsState::Idle);
    CHECK_FALSE(off.is_fault);
    CHECK(off.status == "not asking for RDS on this receiver.");

    const auto disconnected =
        make_rds_view(empty, false, "the engine went away, so nothing is decoding RDS.");
    CHECK(disconnected.state == RdsState::Faulted);
    CHECK(disconnected.is_fault);
    CHECK(disconnected.status == "the engine went away, so nothing is decoding RDS.");
    CHECK(disconnected.status != off.status);

    // And the reason outranks a station struct that still has contents in
    // it, because the contents are one connection old.
    const auto stale = make_rds_view(kkfm(), false, "the engine went away.");
    CHECK(stale.status == "the engine went away.");
    CHECK(stale.identity.empty());
    CHECK(stale.ps.text.empty());
}

TEST_CASE("locked but not synced is its own state", "[rds]")
{
    // A decoder can be locked to the subcarrier and still hunting for the
    // offset words, which is what the first second after a tune looks like.
    // An implementation keying on lock alone calls this decoding and draws
    // an empty station as a complete one.
    auto station = kkfm();
    station.health.sync = revenant::rpc::RdsSync::Hunting;
    CHECK(make_rds_view(station, true).state == RdsState::Syncing);

    station.health.sync = revenant::rpc::RdsSync::PreSync;
    CHECK(make_rds_view(station, true).state == RdsState::Syncing);

    // And acquiring is not unlocked: no bits are emitted in either, but
    // they are different answers about the band.
    station.health.lock = revenant::rpc::RdsLock::Acquiring;
    CHECK(make_rds_view(station, true).state == RdsState::Acquiring);
}

TEST_CASE("the station the operator saw", "[rds]")
{
    const auto view = make_rds_view(kkfm(), true);

    CHECK(view.state == RdsState::Decoding);
    CHECK_FALSE(view.is_fault);
    CHECK(view.identity == "KKFM");
    CHECK(view.ps.text == "KKFM");
    CHECK(view.radio_text.text == "98.1 KKFM Weekends");
    CHECK(view.programme_type == "6 Classic Rock");

    // 90 corrected plus 37 dropped out of 1000 looked at is 12.7 percent.
    //
    // WHAT THIS COMMENT USED TO CLAIM. It ended "which is the figure that came
    // back off the air", and the number did: docs/rds-first-decode.md records
    // 12.7 percent from the 2026-09-20 KKFM run. These counts are not how it
    // got there. That figure was revenant-cli's, which at the time computed
    // dropped over total, and the same capture CORRECTED TWO BLOCKS. This
    // fixture reaches the same 12.7 under the window's formula by carrying
    // ninety corrections the capture never had.
    //
    // So the arithmetic below is right and the provenance was not. The counts
    // are a constructed case that exercises the formula, which is what a unit
    // test wants; the on-air run is in the document, where the antenna and the
    // hour are recorded beside it.
    CHECK(view.block_error_rate > 0.126);
    CHECK(view.block_error_rate < 0.128);
    CHECK(view.status == "decoding.  12.7% of blocks needed correcting or were dropped.");
}

TEST_CASE("a corrected block is an error", "[rds]")
{
    // core/rpc/types.h says a corrected block had a burst repaired rather
    // than being received clean and is trusted less than a good one. AN
    // IMPLEMENTATION COUNTING ONLY DROPPED BLOCKS reports a fading station
    // with a working error corrector as perfect, which is the one reading
    // that sends nobody to check the antenna.
    revenant::rpc::RdsHealth health;
    health.blocks_good = 900;
    health.blocks_corrected = 100;
    health.blocks_dropped = 0;
    CHECK(block_error_rate(health) > 0.099);
    CHECK(block_error_rate(health) < 0.101);

    // No blocks looked at is not a rate of zero. An implementation
    // returning 0.0 there prints a perfect link for the first moment after
    // every tune.
    revenant::rpc::RdsHealth fresh;
    CHECK(block_error_rate(fresh) < 0.0);
}

TEST_CASE("the PI is shown when no call sign derives from it", "[rds]")
{
    // core/rpc/types.h: an empty call_sign means NO CALL SIGN IS DERIVABLE
    // from this PI in this region, which is a different thing from a PI
    // that has not arrived. An implementation that showed an empty string
    // for the first case leaves the identity line blank on every European
    // station.
    auto station = kkfm();
    station.call_sign.clear();
    CHECK(make_rds_view(station, true).identity == "0x2AF6");

    // And nothing at all before a PI has arrived.
    station.pi_valid = false;
    CHECK(make_rds_view(station, true).identity.empty());
}
