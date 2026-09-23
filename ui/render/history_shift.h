// What the passband waterfall does with its history when a frame arrives on a
// different axis from the rows already stored.
//
// The fine-tuning display's frames carry their own geometry, in absolute
// hertz, and the receiver moves: a wheel notch, AFT, a drag of the band. A
// waterfall that threw its history away on every move would be empty whenever
// it is useful, which is while someone is following a drifting signal. One
// that kept it unmoved would draw every stored row at a frequency it was never
// at, which render/waterfall_item.h calls the one thing a waterfall must not
// do. So a move shifts the stored rows sideways by the whole number of pixels
// the axis moved, and the history stays where it was in absolute terms.
//
// THE SHIFT IS WHOLE PIXELS AND THE REMAINDER IS CARRIED. The rows are pixels,
// so a move of 37 Hz on a display at 20 Hz a pixel is one pixel and 17 Hz
// over. The history's anchor, the absolute frequency its left edge now shows,
// moves by exactly the pixels shifted, so the 17 Hz is not lost: the next move
// is measured from the anchor and the error never exceeds half a pixel however
// many moves there are. A row drawn from a new frame goes on that frame's own
// axis, which is within that same half pixel of the anchor.
//
// A DIFFERENT SPAN IS A DIFFERENT PICTURE. A width change moves the
// demodulation rate and with it hertz per pixel, and a stored row cannot be
// rescaled without drawing a signal at a width it never had, so the history
// is discarded, the same rule the span waterfall applies to a width change.
//
// This header holds no Qt; ui/tests links it.

#pragma once

#include <cmath>

namespace revenant::ui {

// Two spans closer than this fraction are the same span. Frames of one
// receiver at one rate carry exactly the same geometry, so this only has to
// absorb the arithmetic of turning rationals into doubles.
inline constexpr double kSameSpanTolerance = 1e-6;

struct HistoryAxis {
    // The absolute hertz at the left edge of the stored rows, and how many
    // hertz one stored pixel covers. A zero span is no history yet.
    double low_hz = 0.0;
    double hz_per_px = 0.0;

    [[nodiscard]] bool valid() const { return hz_per_px > 0.0; }
};

struct HistoryShift {
    // Throw the stored rows away and start again on the new axis.
    bool reset = false;

    // Otherwise move every stored row this many pixels to the RIGHT, which is
    // negative for an axis that moved up in frequency: the same signal is
    // further left on a display whose left edge is now higher.
    int shift_px = 0;

    // Where the stored rows' left edge is after the shift.
    HistoryAxis axis;
};

[[nodiscard]] inline HistoryShift plan_history_shift(const HistoryAxis& stored, double new_low_hz,
                                                     double new_high_hz, int width_px)
{
    HistoryShift out;
    if (width_px <= 0 || !(new_high_hz > new_low_hz)) {
        out.reset = true;
        return out;
    }
    const double hz_per_px = (new_high_hz - new_low_hz) / static_cast<double>(width_px);

    if (!stored.valid() ||
        std::fabs(hz_per_px - stored.hz_per_px) > kSameSpanTolerance * stored.hz_per_px) {
        out.reset = true;
        out.axis = HistoryAxis{new_low_hz, hz_per_px};
        return out;
    }

    const double moved_px = (new_low_hz - stored.low_hz) / stored.hz_per_px;
    const auto whole = static_cast<int>(std::lround(moved_px));

    // A move of the whole width or more leaves nothing in common, and a shift
    // that large is a reset by another name.
    if (whole >= width_px || whole <= -width_px) {
        out.reset = true;
        out.axis = HistoryAxis{new_low_hz, hz_per_px};
        return out;
    }

    out.shift_px = -whole;
    out.axis = HistoryAxis{stored.low_hz + static_cast<double>(whole) * stored.hz_per_px,
                           stored.hz_per_px};
    return out;
}

}  // namespace revenant::ui
