// What to say when the engine let a receiver go, and what not to claim about
// why.
//
// Pure and Qt-free so ui/tests can cover it, the same reason
// models/bookmarks.h and models/gain_control.h are headers.
//
// WHY THERE IS A SENTENCE AT ALL. On 2026-09-21 receivers stopped riding the
// baseband frame: one is pinned to the absolute frequency it was tuned to, and
// a retune that leaves its centre outside the new span removes it rather than
// dragging it to a frequency nobody chose. That is the right behaviour and it
// is silent. An operator sweeping the front end with the wheel watches the
// detail pane empty, with nothing naming the frequency that went or saying the
// radio moved out from under it.
//
// receiver_link.cpp's own note says why the existing fault line could not
// carry it: "a sentence posted now would outlive the pane it describes,
// because removeReceiver empties the pane and does not clear receiverFault".
// So this is a separate line with a separate lifetime, cleared when the
// operator tunes somewhere rather than when the pane goes.
//
// WHAT IT MUST NOT DO IS GUESS. The client learns a receiver is gone by asking
// the engine for its inventory and not finding it. That says the receiver has
// gone; it does not say why. A retune putting it outside the span is the
// common cause and the only one this window can check for itself, so the
// sentence says so ONLY when the frequency really is outside the span the
// source now covers. When it is not, something else happened and the honest
// sentence names the frequency and stops.

#pragma once

#include <cstdint>
#include <string>

namespace revenant::ui {

// Megahertz to three places, which resolves a kilohertz and is how an operator
// writes a frequency down.
//
// Integer arithmetic for the reason models/bookmarks.h gives at
// bookmark_label: a test compares the exact string, and a frequency that
// renders one way here and another way in the bookmark list beside it reads as
// two different frequencies.
[[nodiscard]] inline std::string megahertz_text(std::int64_t hz)
{
    const bool negative = hz < 0;
    const std::int64_t magnitude = negative ? -hz : hz;
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

// The sentence for a receiver the engine no longer has, or empty when there is
// nothing to say.
//
// gone_hz is the absolute frequency the receiver was tuned to, which the
// window records when it tunes rather than deriving at teardown. DERIVING IT
// WOULD BE WRONG AT EXACTLY THIS MOMENT: the pane holds a baseband offset
// measured against the source centre it was tuned at, and the whole reason the
// receiver went is that the source centre has moved, so adding the new centre
// to the old offset names a frequency the receiver never had.
//
// span_low_hz and span_high_hz are what the source covers NOW, the same pair
// models/bookmarks.h is fed and for the same reason: they are what the display
// is drawing, and a span rebuilt from a centre and a rate parts company with
// it at the edges.
[[nodiscard]] inline std::string receiver_gone_sentence(std::int64_t gone_hz,
                                                        double span_low_hz,
                                                        double span_high_hz)
{
    if (gone_hz == 0) {
        return {};
    }

    const std::string where = megahertz_text(gone_hz);
    const auto frequency = static_cast<double>(gone_hz);

    // A span with no width is a source that has not reported its geometry, so
    // there is nothing to test against and no cause to offer.
    const bool have_span = span_high_hz > span_low_hz;
    if (have_span && (frequency < span_low_hz || frequency > span_high_hz)) {
        return where + " is outside the span now, so the receiver there was let go";
    }

    // Inside the span, or no span to check. Something removed the receiver and
    // this window does not know what, so it says what it does know. Offering
    // the retune as the reason here would be a guess that reads as a finding,
    // and it would be wrong in the one case that matters: another client
    // removing a receiver this one was holding.
    return where + ": the engine no longer has that receiver";
}

// The sentence when the ENGINE said the retune removed the receiver, which
// Session.setSourceCenter now does: its answer lists every receiver the move
// left outside the span, with the frequency each was on. Nothing here is
// inferred, so the cause is stated outright and the frequency is the engine's
// rather than the one this window recorded at tune time.
[[nodiscard]] inline std::string receiver_retuned_away_sentence(std::int64_t frequency_hz)
{
    if (frequency_hz == 0) {
        return {};
    }
    return "the front end moved off " + megahertz_text(frequency_hz) +
           ", so the receiver there was let go";
}

}  // namespace revenant::ui
