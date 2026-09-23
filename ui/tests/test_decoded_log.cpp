// The decode section's rules: which decoders a receiver is offered, what one
// line of the log says, and what the cap does.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/rpc/types.h"
#include "models/decoded_log.h"

using revenant::ui::auto_decoders;
using revenant::ui::decoded_line_detail;
using revenant::ui::decoded_line_text;
using revenant::ui::DecodedLine;
using revenant::ui::DecodedLog;
using revenant::ui::decoder_choices;
using revenant::ui::effective_decoder_choice;
using revenant::ui::encrypted_chip;
using revenant::ui::field_value_text;
using revenant::ui::make_decoded_line;
using revenant::ui::resolve_decoder_choice;
using revenant::ui::stream_time_text;
using revenant::ui::view_at_tail;

namespace {

using Strings = std::vector<std::string>;

[[nodiscard]] revenant::rpc::DecoderInfo info_of(std::string name,
                                                 revenant::rpc::DecoderInput input,
                                                 Strings modes)
{
    revenant::rpc::DecoderInfo info;
    info.name = std::move(name);
    info.input = input;
    info.modes = std::move(modes);
    return info;
}

// The engine's registry as Session.decoders answers it on 2026-09-23, in its
// order, with the modes core/rpc/convert.cpp writes out.
[[nodiscard]] std::vector<revenant::rpc::DecoderInfo> engine_list()
{
    using revenant::rpc::DecoderInput;
    const Strings complex = {"raw", "p25p1", "dstar", "tetra"};
    const Strings sideband = {"usb", "lsb"};
    return {
        info_of("p25p1", DecoderInput::ComplexBaseband, complex),
        info_of("dstar", DecoderInput::ComplexBaseband, complex),
        info_of("tetra", DecoderInput::ComplexBaseband, complex),
        info_of("rtty", DecoderInput::RealAudio, sideband),
        info_of("ax25", DecoderInput::RealAudio, {"nfm"}),
        info_of("pocsag", DecoderInput::RealAudio, {"nfm"}),
        info_of("sitor_b", DecoderInput::RealAudio, sideband),
        info_of("navtex", DecoderInput::RealAudio, sideband),
        info_of("psk31", DecoderInput::RealAudio, sideband),
        info_of("psk63", DecoderInput::RealAudio, sideband),
        info_of("qpsk31", DecoderInput::RealAudio, sideband),
        info_of("cw", DecoderInput::RealAudio, {"cw", "usb", "lsb"}),
        info_of("m17", DecoderInput::ComplexBaseband, {"p25p1", "raw"}),
    };
}

// The same list from an engine built before DecoderInfo::modes existed.
[[nodiscard]] std::vector<revenant::rpc::DecoderInfo> old_engine_list()
{
    std::vector<revenant::rpc::DecoderInfo> out = engine_list();
    for (revenant::rpc::DecoderInfo& info : out) {
        info.modes.clear();
    }
    return out;
}

[[nodiscard]] revenant::rpc::DecodedField integer(std::string key, std::int64_t value)
{
    return {std::move(key), revenant::rpc::DecodedValue{std::in_place_index<0>, value}};
}

[[nodiscard]] revenant::rpc::DecodedField flag(std::string key, bool value)
{
    return {std::move(key), revenant::rpc::DecodedValue{std::in_place_index<2>, value}};
}

// A P25 Header Data Unit as core/rpc/decoders.h's adapter sends one.
[[nodiscard]] revenant::rpc::DecodedMessage p25_header(bool encrypted)
{
    revenant::rpc::DecodedMessage message;
    message.vrx = 3;
    message.decoder = "p25p1";
    message.kind = "hdu";
    message.start_sample = 479'664;
    message.end_sample = 480'000;
    message.sample_rate = 48'000;
    message.sequence = 7;
    message.text = encrypted ? "NAC 0x293 hdu TG 1201 ALGID 0x84 KID 0x0001 encrypted"
                             : "NAC 0x293 hdu TG 1201 ALGID 0x80 KID 0x0000 clear";
    message.fields = {integer("nac", 0x293), integer("talkgroup", 1201),
                      integer("algorithm_id", encrypted ? 0x84 : 0x80),
                      flag("encrypted", encrypted)};
    return message;
}

[[nodiscard]] DecodedLine line_with(std::uint64_t serial, std::string text,
                                    std::uint64_t dropped_before = 0)
{
    DecodedLine line;
    line.serial = serial;
    line.time = "00:00:01.0";
    line.decoder = "rtty";
    line.text = std::move(text);
    line.dropped_before = dropped_before;
    return line;
}

[[nodiscard]] std::vector<DecodedLine> batch_of(std::uint64_t first, std::size_t count)
{
    std::vector<DecodedLine> out;
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(line_with(first + i, "line " + std::to_string(first + i)));
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The menu
// ---------------------------------------------------------------------------

// Rejects a menu of every decoder the engine has. A usb receiver offered
// ax25 or p25p1 is offered a subscription the engine will refuse, which is a
// chip of refusal waiting to happen.
TEST_CASE("a sideband receiver is offered auto and the seven that read it", "[decoded]")
{
    CHECK(decoder_choices(engine_list(), "usb") ==
          Strings{"auto", "rtty", "sitor_b", "navtex", "psk31", "psk63", "qpsk31", "cw"});
    CHECK(decoder_choices(engine_list(), "lsb") == decoder_choices(engine_list(), "usb"));
}

TEST_CASE("an nfm receiver is offered auto, ax25 and pocsag", "[decoded]")
{
    CHECK(decoder_choices(engine_list(), "nfm") == Strings{"auto", "ax25", "pocsag"});
    CHECK(auto_decoders(engine_list(), "nfm") == Strings{"ax25", "pocsag"});
}

// Rejects an auto that attaches every reader even when a decoder is named
// after the mode, which is what the empty name on the wire and the CLI's auto
// both refuse to do: a cw receiver gets the cw decoder.
TEST_CASE("auto on a cw receiver is the cw decoder", "[decoded]")
{
    CHECK(decoder_choices(engine_list(), "cw") == Strings{"auto", "cw"});
    CHECK(auto_decoders(engine_list(), "cw") == Strings{"cw"});
}

// Rejects an auto on a raw tap. Four decoders read one and nothing in the
// samples says which protocol is there, so the CLI attaches none and the
// window offers none.
TEST_CASE("a raw tap is offered the complex decoders and no auto", "[decoded]")
{
    CHECK(decoder_choices(engine_list(), "raw") == Strings{"p25p1", "dstar", "tetra", "m17"});
    CHECK(auto_decoders(engine_list(), "raw").empty());
}

// Rejects treating the digital modes as a raw tap, with no auto, which is
// what they were while the window could only reach them as one. A p25p1
// receiver's auto is the decoder named after it, and so for dstar and tetra;
// the others the engine lists as reading the tap stay in the menu, M17 on
// p25p1 among them, because the engine says they read it.
TEST_CASE("a digital receiver's auto is the decoder named after its mode", "[decoded]")
{
    CHECK(decoder_choices(engine_list(), "p25p1") ==
          Strings{"auto", "p25p1", "dstar", "tetra", "m17"});
    CHECK(auto_decoders(engine_list(), "p25p1") == Strings{"p25p1"});
    CHECK(resolve_decoder_choice("auto", engine_list(), "p25p1") == Strings{"p25p1"});
    CHECK(auto_decoders(engine_list(), "dstar") == Strings{"dstar"});
    CHECK(auto_decoders(engine_list(), "tetra") == Strings{"tetra"});
    CHECK(decoder_choices(engine_list(), "tetra") == Strings{"auto", "p25p1", "dstar", "tetra"});
}

// Rejects a section drawn for a mode nothing reads. WFM keeps its RDS
// section and has no decoder here; AM and DSB have neither.
TEST_CASE("am, dsb and wfm are offered nothing", "[decoded]")
{
    CHECK(decoder_choices(engine_list(), "am").empty());
    CHECK(decoder_choices(engine_list(), "dsb").empty());
    CHECK(decoder_choices(engine_list(), "wfm").empty());
}

// Rejects reading an empty list as "reads nothing", which would hide the
// section on every receiver against an older engine, and rejects reading it
// as "reads everything", which would offer rtty on a raw tap. The input kind
// is what an old engine does say. Auto stays off there: on input kind alone
// it would attach all nine audio decoders to a wfm receiver.
TEST_CASE("an engine that does not list modes is read by input kind", "[decoded]")
{
    const Strings nfm = decoder_choices(old_engine_list(), "nfm");
    CHECK(nfm == Strings{"rtty", "ax25", "pocsag", "sitor_b", "navtex", "psk31", "psk63",
                         "qpsk31", "cw"});
    CHECK(decoder_choices(old_engine_list(), "raw") ==
          Strings{"p25p1", "dstar", "tetra", "m17"});
}

// Rejects a choice that survives a mode change as a name the menu no longer
// lists. The subscription would be refused and the combo would show a name
// that is not in it.
TEST_CASE("a choice the receiver does not offer falls to the head of its menu", "[decoded]")
{
    const Strings nfm = decoder_choices(engine_list(), "nfm");
    CHECK(effective_decoder_choice("rtty", nfm) == "auto");
    CHECK(effective_decoder_choice("pocsag", nfm) == "pocsag");
    CHECK(effective_decoder_choice("rtty", {}).empty());

    CHECK(resolve_decoder_choice("rtty", engine_list(), "nfm") == Strings{"ax25", "pocsag"});
    CHECK(resolve_decoder_choice("psk31", engine_list(), "usb") == Strings{"psk31"});
    CHECK(resolve_decoder_choice("auto", engine_list(), "raw") == Strings{"p25p1"});
    CHECK(resolve_decoder_choice("auto", engine_list(), "wfm").empty());
}

// ---------------------------------------------------------------------------
// One line
// ---------------------------------------------------------------------------

// Rejects a time taken from start_sample, which is the beginning of the
// chunk that completed the message and not when it ended, and rejects a
// double division that prints 12.299999 for a whole number of tenths.
TEST_CASE("the time is where the message ended in the receiver's stream", "[decoded]")
{
    CHECK(stream_time_text(592'320, 48'000) == "00:00:12.3");
    CHECK(stream_time_text(0, 48'000) == "00:00:00.0");
    CHECK(stream_time_text(3661ULL * 48'000 + 47'999, 48'000) == "01:01:01.9");
    CHECK(stream_time_text(100ULL * 3600 * 48'000, 48'000) == "100:00:00.0");

    // A rate of zero is a count with no clock behind it.
    CHECK(stream_time_text(592'320, 0) == "--:--:--.-");
}

// Rejects an encrypted call drawn as an error. The decoder read the header
// and is reporting what it says; the chip is quiet and carries the talkgroup.
TEST_CASE("an encrypted P25 call is a chip naming its talkgroup", "[decoded]")
{
    CHECK(encrypted_chip(p25_header(true)) == "encrypted: talkgroup 1201");
    CHECK(encrypted_chip(p25_header(false)).empty());

    // M17's link setup sets the flag with no talkgroup to name.
    revenant::rpc::DecodedMessage m17;
    m17.decoder = "m17";
    m17.fields = {flag("encrypted", true)};
    CHECK(encrypted_chip(m17) == "encrypted");

    // A message that does not carry the flag says nothing about it.
    CHECK(encrypted_chip(revenant::rpc::DecodedMessage{}).empty());
}

// Rejects printing every integer in decimal, which puts NAC 659 in the
// expansion under a line that says NAC 0x293, and rejects decoding bytes as
// text, which renders a D-STAR flag byte as whatever glyph it lands on.
TEST_CASE("field values read the way the line above them does", "[decoded]")
{
    using revenant::rpc::DecodedValue;
    CHECK(field_value_text("nac", DecodedValue{std::in_place_index<0>, std::int64_t{0x293}}) ==
          "0x293 (659)");
    CHECK(field_value_text("talkgroup", DecodedValue{std::in_place_index<0>,
                                                     std::int64_t{1201}}) == "1201");
    CHECK(field_value_text("wpm", DecodedValue{std::in_place_index<1>, 19.1}) == "19.1");
    CHECK(field_value_text("encrypted", DecodedValue{std::in_place_index<2>, true}) == "yes");
    CHECK(field_value_text("text", DecodedValue{std::in_place_index<3>,
                                                std::string("A\rB\nC")}) == "A B C");
    CHECK(field_value_text("message_indicator",
                           DecodedValue{std::in_place_index<4>,
                                        std::vector<std::uint8_t>{0x0A, 0xFF, 0x00}}) ==
          "0A FF 00");
    CHECK(field_value_text("flags", DecodedValue{std::in_place_index<4>,
                                                 std::vector<std::uint8_t>{}}) == "(none)");
}

// Rejects a line that shows only the text and throws the fields away, and
// one whose expansion leaves out where in the stream the message was.
TEST_CASE("a line carries the text, the chip and every field behind it", "[decoded]")
{
    const DecodedLine line = make_decoded_line(p25_header(true), 42, "21:04:13");
    CHECK(line.serial == 42);
    CHECK(line.vrx == 3);
    CHECK(line.time == "00:00:10.0");
    CHECK(line.decoder == "p25p1");
    CHECK(line.text == "NAC 0x293 hdu TG 1201 ALGID 0x84 KID 0x0001 encrypted");
    CHECK(line.chip == "encrypted: talkgroup 1201");

    REQUIRE(line.fields.size() == 9);
    CHECK(line.fields[0].key == "kind");
    CHECK(line.fields[0].value == "hdu");
    CHECK(line.fields[2].value == "479664 to 480000 at 48000 S/s");
    CHECK(line.fields[4].key == "arrived");
    CHECK(line.fields[4].value == "21:04:13");
    CHECK(line.fields[8].key == "encrypted");
    CHECK(line.fields[8].value == "yes");

    // A message with no text still says what it was.
    revenant::rpc::DecodedMessage bare;
    bare.decoder = "m17";
    bare.kind = "eot";
    CHECK(make_decoded_line(bare, 1, "").text == "eot");
    CHECK(make_decoded_line(bare, 1, "").fields.size() == 4);
}

// Rejects columns that follow the decoder name's length, which makes every
// pasted log ragged, and a chip that disappears from a copy.
TEST_CASE("a copied line keeps its columns", "[decoded]")
{
    DecodedLine line = line_with(1, "CQ DE N0CALL");
    CHECK(decoded_line_text(line) == "00:00:01.0  rtty     CQ DE N0CALL");

    line.decoder = "sitor_b";
    CHECK(decoded_line_text(line) == "00:00:01.0  sitor_b  CQ DE N0CALL");

    const DecodedLine header = make_decoded_line(p25_header(true), 2, "");
    CHECK(decoded_line_text(header) ==
          "00:00:10.0  p25p1    [encrypted: talkgroup 1201] NAC 0x293 hdu TG 1201 ALGID 0x84 "
          "KID 0x0001 encrypted");

    const std::string detail = decoded_line_detail(header);
    CHECK(detail.find("\n    talkgroup: 1201") != std::string::npos);
    CHECK(detail.find("\n    nac: 0x293 (659)") != std::string::npos);
}

// ---------------------------------------------------------------------------
// The log
// ---------------------------------------------------------------------------

// Rejects an unbounded log, and one that drops lines without counting them,
// which is a log that quietly stops being the whole record.
TEST_CASE("the log keeps the newest lines and counts what it let go", "[decoded]")
{
    DecodedLog log(3);
    log.append(batch_of(1, 2));
    CHECK(log.size() == 2);
    CHECK(log.dropped_label().empty());
    CHECK(log.dropped_detail().empty());

    log.append(batch_of(3, 3));
    REQUIRE(log.size() == 3);
    CHECK(log.lines().front().serial == 3);
    CHECK(log.lines().back().serial == 5);
    CHECK(log.evicted() == 2);
    CHECK(log.dropped_label() == "2 dropped");
    CHECK(log.dropped_detail() ==
          "2 older lines left the log, which keeps the newest 3. Copy the log before clearing "
          "it to keep a longer record.");
}

// Rejects a plan that evicts the whole existing log to make room for a batch
// larger than the cap and then adds the batch whole, which leaves the log
// over its cap. A batch that big keeps its own tail.
TEST_CASE("a batch larger than the log keeps its own newest lines", "[decoded]")
{
    DecodedLog log(3);
    log.append(batch_of(1, 2));

    const DecodedLog::AppendPlan plan = log.plan_append(5);
    CHECK(plan.skip_incoming == 2);
    CHECK(plan.evict_front == 2);

    log.append(batch_of(3, 5));
    REQUIRE(log.size() == 3);
    CHECK(log.lines().front().serial == 5);
    CHECK(log.lines().back().serial == 7);
    CHECK(log.evicted() == 4);
}

// Rejects folding the engine's queue losses into the cap's count. They have
// different causes, and the sentence names both.
TEST_CASE("messages the engine's queue lost are counted apart from the cap", "[decoded]")
{
    DecodedLog log(10);
    std::vector<DecodedLine> batch;
    batch.push_back(line_with(1, "one"));
    batch.push_back(line_with(2, "two", 5));
    log.append(std::move(batch));

    CHECK(log.evicted() == 0);
    CHECK(log.lost_on_wire() == 5);
    CHECK(log.dropped_label() == "5 dropped");
    CHECK(log.dropped_detail() ==
          "5 messages never reached this window: the engine holds 256 per decoder for a client "
          "and let the oldest go when this one fell behind.");
}

// Rejects a clear that empties the view and keeps the counts, which leaves a
// "dropped" chip on an empty log with nothing it could be about.
TEST_CASE("a copy of the whole log says what is missing, and a clear starts again",
          "[decoded]")
{
    DecodedLog log(2);
    log.append(batch_of(1, 3));
    CHECK(log.copy_all() ==
          "(1 older line not kept)\n"
          "00:00:01.0  rtty     line 2\n"
          "00:00:01.0  rtty     line 3\n");

    log.clear();
    CHECK(log.size() == 0);
    CHECK(log.copy_all().empty());
    CHECK(log.dropped_label().empty());
}

// Rejects following the tail only at the exact bottom pixel, which a
// fractional scroll position misses and the log then stops following on its
// own, and rejects following while the operator has scrolled up to read.
TEST_CASE("the log follows the newest line unless the operator scrolled up", "[decoded]")
{
    CHECK(view_at_tail(0.0, 200.0, 150.0));
    CHECK(view_at_tail(800.0, 200.0, 1000.0));
    CHECK(view_at_tail(797.5, 200.0, 1000.0));
    CHECK_FALSE(view_at_tail(700.0, 200.0, 1000.0));
}
