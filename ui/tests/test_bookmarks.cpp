// Bookmarks: what gets saved about a place an operator named, and what recalling
// one asks of the source that happens to be open.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS, in its own
// comment, for the reason test_audio_ring.cpp gives: a test that only asserts
// what the code already does certifies one reachable shape and reads as though
// it certified the behaviour.
//
// The wrong implementations worth rejecting here share one property, which is
// why they are asserted rather than tried out: a bookmark recalled wrongly still
// produces a receiver that plays. It is tuned to the wrong thing, or sitting on
// the radio's own local oscillator, or refusing on a dongle that could hear it
// perfectly well. None of the three throws and none of them looks broken, so
// each one gets found weeks later by an operator who concludes their notes are
// wrong.
//
// WHAT IS NOT HERE. Where bookmarks are written to disk, the QML list, and the
// two-turn retune. The first two are Qt; the third is a sequence across the
// supervisor thread rather than a rule, so it is asserted where the link is.

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "models/bookmarks.h"

using revenant::ui::Bookmark;
using revenant::ui::bookmark_at;
using revenant::ui::bookmark_label;
using revenant::ui::plan_recall;
using revenant::ui::RecallAction;

namespace {

// The shipped geometry, near enough: an R820T on 2.4 MS/s sitting on broadcast
// FM, which is where most of this session's live testing happened. Doubles,
// because EngineLink::spanLowHz and spanHighHz are doubles off
// frequencyAtFraction and this rule is fed them directly.
constexpr double kSpanLow = 96'900'000.0;
constexpr double kSpanHigh = 99'300'000.0;
constexpr double kSpan = kSpanHigh - kSpanLow;

[[nodiscard]] Bookmark wfm_at(std::int64_t hz, std::string name = {})
{
    Bookmark mark;
    mark.name = std::move(name);
    mark.freq_hz = hz;
    mark.demod = "wfm";
    mark.passband_low = -80'000;
    mark.passband_high = 80'000;
    return mark;
}

}  // namespace

// Rejects a bookmark that stored the BASEBAND offset, which is the number
// VrxParams::center actually carries and so the one a caller copying the
// receiver's own fields would save. The claim under test is that a bookmark's
// frequency is a fact about the air and not about any radio's tuning: the same
// entry answers PlaceHere on two spans that have no centre, no width and no
// device in common, and it does so without being told where either front end
// sits. An offset could not survive the second span.
TEST_CASE("a bookmark's frequency does not depend on any source's centre", "[bookmarks]")
{
    const Bookmark mark = wfm_at(98'500'000, "KXYZ");

    CHECK(plan_recall(mark, kSpanLow, kSpanHigh, true).action == RecallAction::PlaceHere);

    // A different radio on a different rate, wide enough to swallow the band.
    CHECK(plan_recall(mark, 90'000'000.0, 110'000'000.0, true).action ==
          RecallAction::PlaceHere);

    // And a narrow one sitting almost exactly on it, which is what an SDR with a
    // 250 kHz rate looks like. Still the same entry, still untouched.
    CHECK(plan_recall(mark, 98'400'000.0, 98'600'000.0, false).action ==
          RecallAction::PlaceHere);
}

// Rejects a recall that centres the front end on the bookmark, which is the
// obvious implementation and reads correctly in a diff. An RTL-SDR mixes with a
// local oscillator at the centre of its own span and leaks it into its own
// output, so centring puts every recalled bookmark on top of the one feature of
// the display that is not coming from the antenna. The receiver still
// demodulates, which is why this is a test and not something anybody notices.
TEST_CASE("recalling out of span retunes beside the bookmark, not onto it", "[bookmarks]")
{
    const Bookmark mark = wfm_at(435'000'000, "satellite downlink");

    const auto plan = plan_recall(mark, kSpanLow, kSpanHigh, true);
    REQUIRE(plan.action == RecallAction::RetuneThenPlace);

    // A quarter of a 2.4 MHz span is 600 kHz, so the front end goes below the
    // bookmark and the bookmark lands a quarter span up.
    CHECK(plan.retune_center_hz == 434'400'000);
    CHECK(mark.freq_hz - plan.retune_center_hz == static_cast<std::int64_t>(kSpan / 4.0));

    // Clear of the centre bin, and nowhere near the edge either: the offset is a
    // quarter span, so there is another quarter span of margin above it.
    CHECK(plan.retune_center_hz != mark.freq_hz);
    CHECK(static_cast<double>(mark.freq_hz) < plan.retune_center_hz + kSpan / 2.0);

    // The retune target is derived from the BOOKMARK and the span width, not from
    // wherever the front end happens to be. Moving the current span without
    // changing its width must not move the answer.
    const auto elsewhere = plan_recall(mark, 400'000'000.0, 400'000'000.0 + kSpan, true);
    CHECK(elsewhere.retune_center_hz == plan.retune_center_hz);
}

// Rejects a plan that retunes a source which cannot retune. A file's samples were
// digitised at a centre no client can change, so asking is not a failure to be
// retried, it is a bookmark this source will never hear. The distinction matters
// because the two want different sentences on screen: one says wait, the other
// says open a radio.
TEST_CASE("a fixed source is told out of reach rather than asked to move", "[bookmarks]")
{
    const Bookmark mark = wfm_at(435'000'000);

    const auto fixed = plan_recall(mark, kSpanLow, kSpanHigh, false);
    CHECK(fixed.action == RecallAction::OutOfReach);
    CHECK(fixed.retune_center_hz == 0);

    // Inside the span, a fixed source is as good as a tunable one. A rule that
    // refused every bookmark on a file would make bookmarks useless for exactly
    // the case they suit best, which is going back through a recording.
    CHECK(plan_recall(wfm_at(98'500'000), kSpanLow, kSpanHigh, false).action ==
          RecallAction::PlaceHere);
}

// Rejects a plan that reads a zero-width span as "out of reach", which is what
// comparing against an unset low and high does by accident. The engine reports a
// source's geometry a moment after opening it, so during that moment every
// bookmark in the list would be marked unreachable and the operator would be told
// their notes were bad by a window that had not looked yet.
TEST_CASE("a span with no width means ask again, not unreachable", "[bookmarks]")
{
    const Bookmark mark = wfm_at(98'500'000);

    CHECK(plan_recall(mark, 0.0, 0.0, true).action == RecallAction::NoSource);
    CHECK(plan_recall(mark, 0.0, 0.0, false).action == RecallAction::NoSource);

    // Reversed ends are the same answer: it is not a span, so there is nothing to
    // measure against. Silently swapping them would invent a span from two
    // numbers that were never a pair.
    CHECK(plan_recall(mark, kSpanHigh, kSpanLow, true).action == RecallAction::NoSource);
}

// Rejects a plan that tries to act on a bookmark with no mode, which is what a
// half-filled row read back from the registry looks like. Sent on, it becomes a
// receiver creation with an empty demod name, and empty means "keep the current
// mode" to EngineLink::tuneReceiver, so the bookmark would silently be recalled
// in whatever mode the pane was already in.
TEST_CASE("a bookmark missing its frequency or its mode is unusable", "[bookmarks]")
{
    Bookmark no_mode = wfm_at(98'500'000);
    no_mode.demod.clear();
    CHECK(plan_recall(no_mode, kSpanLow, kSpanHigh, true).action == RecallAction::Unusable);

    const Bookmark no_freq = wfm_at(0);
    CHECK(plan_recall(no_freq, kSpanLow, kSpanHigh, true).action == RecallAction::Unusable);

    // Unusable outranks NoSource, because a bookmark that can never be recalled
    // should say so while the list is being read rather than wait for a source to
    // open and then still refuse.
    CHECK(plan_recall(no_mode, 0.0, 0.0, true).action == RecallAction::Unusable);
}

// The edges, asserted because the span ends are exactly where an operator
// bookmarks a signal they had to tune to the corner of the display to hear.
TEST_CASE("a bookmark exactly on a span edge is inside it", "[bookmarks]")
{
    CHECK(plan_recall(wfm_at(96'900'000), kSpanLow, kSpanHigh, true).action ==
          RecallAction::PlaceHere);
    CHECK(plan_recall(wfm_at(99'300'000), kSpanLow, kSpanHigh, true).action ==
          RecallAction::PlaceHere);
    CHECK(plan_recall(wfm_at(96'899'999), kSpanLow, kSpanHigh, true).action ==
          RecallAction::RetuneThenPlace);
    CHECK(plan_recall(wfm_at(99'300'001), kSpanLow, kSpanHigh, true).action ==
          RecallAction::RetuneThenPlace);
}

// Rejects a reachability test run against the PASSBAND rather than the centre,
// which is the more careful-looking rule and disagrees with the engine. The
// engine removes a receiver whose centre leaves the span, so a bookmark judged by
// its passband would be refused here while being perfectly placeable there, and a
// wide mode near an edge is where the two part company. This bookmark's centre is
// inside the span and its upper skirt is not.
TEST_CASE("reachability is judged on the centre, as the engine judges it", "[bookmarks]")
{
    const Bookmark wide = wfm_at(99'290'000);
    REQUIRE(static_cast<double>(wide.freq_hz + wide.passband_high) > kSpanHigh);

    CHECK(plan_recall(wide, kSpanLow, kSpanHigh, true).action == RecallAction::PlaceHere);
}

// Rejects a label built by formatting a double, which is the one-line
// implementation. 100.3 MHz is not representable, and a list where one entry
// renders as 100.299 once looks like a different station rather than like a
// rounding artifact. Integer arithmetic on hertz has no such mode.
TEST_CASE("an unnamed bookmark is labelled from its frequency exactly", "[bookmarks]")
{
    CHECK(bookmark_label(wfm_at(100'300'000)) == "100.300 MHz");
    CHECK(bookmark_label(wfm_at(98'100'000)) == "98.100 MHz");
    CHECK(bookmark_label(wfm_at(146'520'000)) == "146.520 MHz");

    // Sub-kilohertz detail is below what three places can show, and truncating is
    // right rather than rounding: a label is an identity, and 7.199 rounding up to
    // 7.200 would collide with the bookmark actually on 7.200.
    CHECK(bookmark_label(wfm_at(7'199'900)) == "7.199 MHz");

    // Leading zeros in the fraction, which a naive to_string of the remainder
    // drops: 162.005 would read as 162.5 and sort beside a station 495 kHz away.
    CHECK(bookmark_label(wfm_at(162'550'000)) == "162.550 MHz");
    CHECK(bookmark_label(wfm_at(162'005'000)) == "162.005 MHz");
    CHECK(bookmark_label(wfm_at(162'000'000)) == "162.000 MHz");

    // A name always wins, whatever the frequency reads as.
    CHECK(bookmark_label(wfm_at(100'300'000, "KXYZ")) == "KXYZ");
}

// Rejects a lookup returning the FIRST bookmark inside the tolerance rather than
// the nearest. Clicking a wide signal twice saves two entries a few kilohertz
// apart, and a first-match lookup then answers with whichever was saved earlier
// rather than the one under the pointer, so "already bookmarked" points at the
// wrong row.
TEST_CASE("looking up a frequency finds the nearest bookmark in tolerance", "[bookmarks]")
{
    const std::vector<Bookmark> marks{
        wfm_at(98'000'000, "low edge"),
        wfm_at(98'100'000, "centre"),
        wfm_at(98'200'000, "high edge"),
    };

    const Bookmark* hit = bookmark_at(marks, 98'120'000, 150'000);
    REQUIRE(hit != nullptr);
    CHECK(hit->name == "centre");

    // Nothing within tolerance is a null answer, not the closest one anyway. A
    // lookup that always returned something would report every new signal as
    // already bookmarked.
    CHECK(bookmark_at(marks, 100'300'000, 150'000) == nullptr);
    CHECK(bookmark_at({}, 98'100'000, 150'000) == nullptr);

    // Exactly at the tolerance counts as a hit. The caller passes a receiver's
    // own width, so the bound is the edge of one passband and a signal sitting on
    // it is the same station.
    const Bookmark* edge = bookmark_at(marks, 98'250'000, 50'000);
    REQUIRE(edge != nullptr);
    CHECK(edge->name == "high edge");
    CHECK(bookmark_at(marks, 98'250'001, 50'000) == nullptr);

    // A zero tolerance is an exact-frequency lookup and still works, which is
    // what a caller reaping a bookmark by its own frequency wants.
    const Bookmark* exact = bookmark_at(marks, 98'000'000, 0);
    REQUIRE(exact != nullptr);
    CHECK(exact->name == "low edge");
}
