// Bookmarks: places an operator named, and the rule for getting back to one.
//
// Pure and Qt-free so ui/tests can cover it, the same reason
// models/gain_control.h and models/receiver_marker.h are headers.
//
// A BOOKMARK IS NOT A REMEMBERED RECEIVER, and the difference is the whole
// reason this is allowed to exist. models/settings.h lists the receiver under
// "WHAT IS DELIBERATELY NOT REMEMBERED", because "a window that came up already
// tuned to last night's frequency would be making a claim about a band it has
// not looked at". That still holds and is not retracted here. Nothing in this
// file restores a receiver at startup. A bookmark is recalled when somebody asks
// for it by name, so the claim is theirs, and the window is doing what it was
// told rather than guessing what it was doing yesterday.
//
// WHAT IS SAVED IS ABSOLUTE. VrxParams::center is hertz from baseband DC and
// means nothing without the source centre it was measured against, so a saved
// offset recalled at a different centre is a receiver on the wrong station,
// silently. The absolute frequency is what an operator would write on a card, it
// survives the front end moving, and it is what the reachability rule below
// needs.
//
// ---------------------------------------------------------------------------
//
// TWO THINGS THE CALLER OWNS, recorded here because both are easy to get wrong
// from QML and neither is visible in this file's signatures.
//
// The offset arithmetic is not here. EngineLink::tuneReceiver takes an ABSOLUTE
// frequency and subtracts EngineInfo::source_center itself, in the one place
// that holds the source centre. A bookmark hands it mark.freq_hz unmodified. A
// second subtraction here would be the same mistake twice and would land a
// receiver at twice the offset.
//
// RetuneThenPlace IS TWO TURNS, NOT ONE. EngineLink::tuneSourceHz posts the
// retune to the supervisor thread and returns before the radio has moved, so
// source_center still holds the old value on the next line and a receiver placed
// there goes to the wrong baseband offset. The recall has to hold the bookmark,
// wait for the granted centre to come back, and place the receiver then. Doing
// both in one click handler produces a receiver that plays the wrong thing,
// which is why this is written down rather than left to be discovered.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace revenant::ui {

// One place worth going back to, in the terms an operator would use.
struct Bookmark {
    // What to call it in a list. May be empty, and an empty one is not an error:
    // clicking a signal and hitting save is a reasonable thing to do before
    // knowing what it is. bookmark_label() below is what a display shows.
    std::string name;

    std::int64_t freq_hz = 0;
    std::string demod;

    // Hertz from the bookmark's own centre, signed, exactly as VrxParams carries
    // them. Both zero means the mode's own default was never moved, and the
    // default is what gets recalled: a width copied out of one night's grid is
    // worse than a default when the grid differs.
    std::int32_t passband_low = 0;
    std::int32_t passband_high = 0;

    [[nodiscard]] bool empty() const { return freq_hz == 0 || demod.empty(); }
};

// WHAT A BOOKMARK DELIBERATELY DOES NOT CARRY, so that adding it later is a
// decision rather than an oversight.
//
// The source it was heard on. A bookmark that refused to work on a second dongle
// would be a worse bookmark than a sticky note: 146.52 MHz is 146.52 MHz on
// anything that can reach it, and the reachability rule below already answers the
// only question that actually matters. Provenance would be pleasant to show next
// to an entry nobody remembers making, and it is left out because the client
// cannot fill it in: EngineInfo has no uri field and openSource keeps the string
// it was given to itself, so the field would be a column of empty strings.

// What recalling a bookmark requires of the source that is open now.
enum class RecallAction : std::uint8_t {
    // Inside the span already. Place the receiver and touch nothing else.
    PlaceHere,

    // Reachable, but the front end has to move first. Two turns, see above.
    RetuneThenPlace,

    // A source that cannot move and does not already cover this frequency, so
    // the bookmark is a place this source will never hear. A file is the case
    // that matters: its samples were digitised somewhere no client can change.
    OutOfReach,

    // No source open, or its geometry has not been reported yet. A caller asks
    // again rather than telling the operator the bookmark is bad.
    NoSource,

    // The bookmark itself is not usable: no frequency or no mode.
    Unusable,
};

// What to do about one bookmark, and the number to do it with.
struct RecallPlan {
    RecallAction action = RecallAction::NoSource;

    // The absolute frequency to put the front end on, set only for
    // RetuneThenPlace. Zero otherwise, which is not a frequency any source is
    // asked to tune to.
    std::int64_t retune_center_hz = 0;
};

// Where a recalled bookmark sits in the span it lands in, as a fraction of the
// span above the centre.
//
// NOT ON THE CENTRE, which is the obvious choice and the wrong one. An RTL-SDR is
// a zero-IF receiver: it mixes with a local oscillator at the centre of its own
// span and leaks that oscillator into its own output, so the middle bin carries a
// spike belonging to the radio rather than to the air. Recalling a bookmark
// straight onto it would put every saved station on top of the one artifact an
// operator cannot tune away from. A quarter span up is clear of it and still far
// from the filter roll-off at the edges, which is the other place a signal gets
// quietly attenuated.
inline constexpr double kRecallOffsetFraction = 0.25;

// Whether this bookmark can be heard on the source that is open now, and what it
// would take.
//
// span_low_hz and span_high_hz are the ends of what the source currently covers.
// DOUBLES, because that is what the client holds: EngineLink::spanLowHz and
// spanHighHz are frequencyAtFraction(0.0) and (1.0), and that function is the
// client's only rationals-to-hertz conversion. Passing those two in rather than
// rebuilding a span from a centre and a rate keeps this answering the same
// question the display answers; two derivations of a span agree in the middle and
// part company at the edges, which is exactly where this gets asked.
//
// tunable is EngineLink::sourceCanRetune. A device is tunable, a file is not.
//
// THE CENTRE AND NOT THE PASSBAND, matching what the engine does to a receiver
// across a retune. A filter hanging over the edge is a receiver that works and
// sounds wrong, which the passband highlight and the fit sentence both already
// report; a centre outside the span is not a receiver at all.
[[nodiscard]] inline RecallPlan plan_recall(const Bookmark& mark,
                                            double span_low_hz,
                                            double span_high_hz,
                                            bool tunable)
{
    if (mark.empty()) {
        return {RecallAction::Unusable, 0};
    }

    // A span with no width is an engine that has opened a source and not yet
    // reported its geometry. Nothing to measure against, so it is the same
    // answer as no source: ask again rather than call the bookmark unreachable.
    if (span_high_hz <= span_low_hz) {
        return {RecallAction::NoSource, 0};
    }

    const auto freq = static_cast<double>(mark.freq_hz);
    if (freq >= span_low_hz && freq <= span_high_hz) {
        return {RecallAction::PlaceHere, 0};
    }
    if (!tunable) {
        return {RecallAction::OutOfReach, 0};
    }

    const double offset = (span_high_hz - span_low_hz) * kRecallOffsetFraction;
    return {RecallAction::RetuneThenPlace,
            mark.freq_hz - static_cast<std::int64_t>(offset)};
}

// What to show in a list for a bookmark the operator never named. Megahertz to
// three places, which resolves a kilohertz and is how broadcast and amateur
// frequencies are written down.
[[nodiscard]] inline std::string bookmark_label(const Bookmark& mark)
{
    if (!mark.name.empty()) {
        return mark.name;
    }
    if (mark.freq_hz == 0) {
        return {};
    }

    // Integer arithmetic rather than a formatted double, for the reason
    // receiver_match.h's format_width gives: a test compares the exact string.
    // It matters more here, because a label is an identity in a list and one
    // entry rendering as 100.299 looks like a different station rather than like
    // a rounding artifact.
    const bool negative = mark.freq_hz < 0;
    const std::int64_t magnitude = negative ? -mark.freq_hz : mark.freq_hz;
    const std::int64_t whole = magnitude / 1'000'000;
    const std::int64_t thousandths = (magnitude % 1'000'000) / 1'000;

    std::string out;
    if (negative) {
        out += '-';
    }
    out += std::to_string(whole);
    out += '.';
    const std::string frac = std::to_string(thousandths);
    out.append(3 - frac.size(), '0');
    out += frac;
    out += " MHz";
    return out;
}

// The bookmark already sitting at this frequency, or nullptr.
//
// Clicking the same signal twice lands a few hundred hertz apart, so a list
// without this fills up with near-duplicates of one station. tolerance_hz is the
// caller's to choose because the right answer is a receiver's own width: two
// frequencies inside one passband are the same station by definition, and on CW
// they are three orders of magnitude closer together than on broadcast FM.
[[nodiscard]] inline const Bookmark* bookmark_at(const std::vector<Bookmark>& marks,
                                                  std::int64_t freq_hz,
                                                  std::int64_t tolerance_hz)
{
    const Bookmark* best = nullptr;
    std::int64_t best_distance = 0;
    for (const Bookmark& mark : marks) {
        const std::int64_t distance =
            mark.freq_hz > freq_hz ? mark.freq_hz - freq_hz : freq_hz - mark.freq_hz;
        if (distance > tolerance_hz) {
            continue;
        }
        // The nearest and not the first, so a list holding both edges of one
        // wide signal answers with the one actually under the pointer.
        if (best == nullptr || distance < best_distance) {
            best = &mark;
            best_distance = distance;
        }
    }
    return best;
}

}  // namespace revenant::ui
