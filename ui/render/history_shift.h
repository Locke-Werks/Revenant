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
// A DIFFERENT SPAN IS RESAMPLED, NOT DISCARDED. A zoom, a wider or narrower
// pane, or the pane's display rate crossing a rung all change hertz per pixel.
// The rows are kept as level codes on a known axis, so resample_row below
// moves each one onto the new axis. Where a new pixel covers more hertz than
// an old one it takes the loudest old pixel it covers, which is what the peak
// reduction would have drawn from the same bins. Where it covers fewer it
// takes the old pixel under its centre: the row stays at the resolution it
// was measured at, coarser than a fresh row but never at a frequency it was
// not at. What the old axis never covered is the empty level.
//
// WHAT THAT PARAGRAPH USED TO SAY: "A DIFFERENT SPAN IS A DIFFERENT PICTURE
// ... a stored row cannot be rescaled without drawing a signal at a width it
// never had, so the history is discarded." That made every zoom and every
// width change of the pane a blank waterfall, which the owner asked to be rid
// of on 2026-10-26.
//
// WHAT THAT PARAGRAPH USED TO SAY: "A width change moves the demodulation
// rate and with it hertz per pixel", which made every width change a fresh
// history while the pane was the fine stream.
//
// THE SPAN WATERFALL USES IT TOO, since 2026-09-23. A retune of the front end
// moves the main span's axis the way a receiver move moves the pane's, and
// the owner saw the main waterfall's history wiped at every tune: rows drawn
// against the old centre were thrown away rather than moved. Both waterfalls
// now plan with plan_history_shift and move their pixels with shift_row, so
// there is one way this is done.
//
// This header holds no Qt; ui/tests links it.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

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

    // Set with reset when the scale is the same and the axis moved a whole
    // width or more. Nothing stored is still on screen, but the rows still
    // stand for the times they were written at, so a display that keeps a
    // time record beside its pixels (the span waterfall's sample ranges) can
    // blank the pixels and keep the record. False for a change of scale,
    // which is a different picture altogether.
    bool beyond = false;

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
        out.beyond = true;
        out.axis = HistoryAxis{new_low_hz, hz_per_px};
        return out;
    }

    out.shift_px = -whole;
    out.axis = HistoryAxis{stored.low_hz + static_cast<double>(whole) * stored.hz_per_px,
                           stored.hz_per_px};
    return out;
}

// Moves one row of pixels, or of the span waterfall's 16-bit level codes,
// `pixels` to the RIGHT, negative for left,
// and fills what shifts in from outside with `fill`. A shift of the whole
// width or more fills the row. This is the pixel half of a plan above: every
// stored row takes the same shift, and what arrives from outside the old axis
// was never measured, so it is drawn as the bottom of the colour map rather
// than as anything that could be read as a signal.
template <typename Cell>
inline void shift_row(Cell* line, int width_px, int pixels, Cell fill)
{
    if (line == nullptr || width_px <= 0 || pixels == 0) {
        return;
    }
    const int moved = std::min(std::abs(pixels), width_px);
    const auto keep = static_cast<std::size_t>(width_px - moved);
    if (pixels > 0) {
        std::memmove(line + moved, line, keep * sizeof(Cell));
        std::fill(line, line + moved, fill);
    } else {
        std::memmove(line, line + moved, keep * sizeof(Cell));
        std::fill(line + keep, line + width_px, fill);
    }
}

// The axis a history of `width_px` columns has across [low_hz, high_hz).
[[nodiscard]] inline HistoryAxis history_axis(double low_hz, double high_hz, int width_px)
{
    if (width_px <= 0 || !(high_hz > low_hz)) {
        return {};
    }
    return HistoryAxis{low_hz, (high_hz - low_hz) / static_cast<double>(width_px)};
}

// Redraws one stored row, `from` on `from_axis`, onto `to_axis` as `to`. The
// rule is in the header comment: the loudest covered pixel when the new pixel
// is wider, the one under its centre when it is narrower, `fill` where the old
// axis has nothing. Cells compare by value with louder greater, which the
// span waterfall's level codes do and kNoLevel, zero, loses to everything.
template <typename Cell>
inline void resample_row(const Cell* from, int from_width, const HistoryAxis& from_axis, Cell* to,
                         int to_width, const HistoryAxis& to_axis, Cell fill)
{
    if (to == nullptr || to_width <= 0) {
        return;
    }
    if (from == nullptr || from_width <= 0 || !from_axis.valid() || !to_axis.valid()) {
        std::fill(to, to + to_width, fill);
        return;
    }
    const double scale = to_axis.hz_per_px / from_axis.hz_per_px;
    const double offset = (to_axis.low_hz - from_axis.low_hz) / from_axis.hz_per_px;
    if (scale > 1.0) {
        // EACH OLD PIXEL GOES TO ONE NEW PIXEL, the one its centre falls in,
        // and a new pixel keeps the loudest of those it was given.
        //
        // WHAT THIS USED TO DO: every new pixel took every old pixel it
        // overlapped at all. A drag of the pane narrower arrives as one-pixel
        // steps, and at 1400 to 1399 nearly every new pixel overlaps two old
        // ones, so each step widened every signal by a pixel and a drag
        // smeared a carrier into a block hundreds of pixels wide.
        std::fill(to, to + to_width, fill);
        for (int k = 0; k < from_width; ++k) {
            const double centre = (static_cast<double>(k) + 0.5 - offset) / scale;
            const auto c = static_cast<long long>(std::floor(centre));
            if (c >= 0 && c < to_width) {
                to[c] = std::max(to[c], from[k]);
            }
        }
        return;
    }
    for (int c = 0; c < to_width; ++c) {
        // The old pixel under the new pixel's centre.
        const double centre = offset + (static_cast<double>(c) + 0.5) * scale;
        const auto k = static_cast<long long>(std::floor(centre));
        to[c] = k >= 0 && k < from_width ? from[k] : fill;
    }
}

}  // namespace revenant::ui
