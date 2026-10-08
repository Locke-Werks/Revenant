// Turning a frame's bins into what a column of pixels draws.
//
// Three things live here because both the spectrum trace and the waterfall
// need all three and neither owns them: the column reduction, the correction
// that reduction forces on the floor, and the colour map. A fourth, the
// operator's pins on either end of the map, joined them on 2026-09-22.
//
// WHAT THIS DELIBERATELY DOES NOT DO
//
// It does not measure the spectrum's ends. docs/ui-spectrum.md puts both ends
// of the colour map on the device, tracked as percentiles with a fast attack
// and a thirty second decay, and SpectrumFrame carries the result in floor_db
// and ceiling_db. The spectrum trace draws against those.
//
// The span waterfall no longer does, since 2026-10-07: the owner asked for it
// to re-contrast "all together as signals come into the waterfall", which a
// per-frame pair cannot do, so it measures its own history in
// render/waterfall_scale.h. What still makes two rows comparable for absolute
// level is that every row on screen is drawn against the same one pair at any
// moment. What this paragraph used to say was that "every consumer of a frame
// draws against the same two numbers, which is what makes two rows of a
// waterfall comparable", and it was not true: each row kept the ends of the
// moment it was written, so a waterfall whose scale moved was a patchwork.
//
// Both displays place their ends by the same rule, place_ends below, so the
// noise sits at the same height on each.
//
// WHAT IT DOES ADD, AND WHY THAT IS NOT A SECOND SCALE
//
// One correction, to the floor only, for this display's own reduction. A
// column here covers many bins and is drawn as the largest of them, because
// a narrow carrier in one bin of seven hundred is invisible in a mean and is
// the whole point of looking. The largest of K samples is not distributed
// like one sample, so an empty band's columns all draw well above the
// frame's low percentile and the display is a solid wall. The size of that
// gap is a property of the reduction, not of the signal, which is why it is
// here: a display at one bin per pixel has a different K from one showing a
// twenty megahertz span in fifteen hundred columns.
//
// tools/cli/main.cpp's SpectrumView carries the long form of the same
// reasoning and the same arithmetic. This is that logic ported, not a
// second derivation of it.
//
// HEADER ONLY, AND HOLDING NO Qt. It was a .cpp compiled into the client and
// nothing else, so none of it was tested: the reduction's tiling, the
// headroom's harmonic number and the pin rule were asserted only by looking
// at the picture. ui/tests links headers of this shape and not .cpp files,
// for the reason ui/CMakeLists.txt gives above the test target, so the
// functions moved here whole.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "render/colour_map.h"

namespace revenant::ui {

// Duplicated from core/dsp/spectrum_levels_reference.h,
// core/dsp/spectrum_reference.h and core/engine/spectrum_scale.h rather than
// included from them.
//
// This process links no part of the engine, for the runtime reason
// core/rpc/types.h sets out, so the headers that define these are not
// reachable here. Naming them is the alternative to burying the same
// literals in the arithmetic below, where a change on the engine side would
// leave this drawing against numbers that no longer match the frames it is
// given, with nothing to say so.
inline constexpr std::uint32_t kSpectrumLowPermille = 50;
inline constexpr float kSpectrumMinimumSpanDb = 12.0F;
inline constexpr float kSpectrumFloorDb = -200.0F;

// WHERE THE NOISE SITS, AND THE NARROWEST MAP THE SPAN DISPLAYS DRAW
//
// These two are the client's own and not the engine's. The engine's minimum
// span, kSpectrumMinimumSpanDb above, keeps its two percentiles apart; these
// decide how the picture is laid out around them.
//
// The owner, 2026-10-07, at 145 MHz with the tuner on auto gain and no strong
// signal present: "the spectrum looks high", noise spiking from the bottom of
// the plot to the top. Under the old rule the floor went exactly where an
// empty band's columns draw, so the noise sat ON the bottom edge, and on a
// band with no signal the engine's ceiling decays down into the noise, which
// left the twelve decibel minimum span as the whole height of the plot. Noise
// filled it.
//
// kNoiseFraction: the drawn noise level goes this far up the map, about an
// eighth. Low enough that an empty band reads as an empty band, high enough
// that the noise is a visible line and not clipped flat on the bottom edge,
// where a weak signal climbing out of it has nothing to climb out of.
//
// kDisplayMinimumSpanDb, 40 dB, from the noise itself. At one bin per column,
// the zoomed-in case and the noisiest, a column's power is exponential: its
// 99.9th percentile is ln 1000 = 6.9 times its mean, 8.4 dB over it. With the
// mean placed 12% up a 40 dB map, 4.8 dB above the floor, one column in a
// thousand reaches 13.2 dB, a third of the height, and the top two thirds are
// left for anything that is not noise. At 30 dB the same spike reaches 40%;
// at the old 12 dB it leaves the top of the plot, which is the wall the owner
// saw. Wider than 40 compresses a real 20 dB carrier into a quarter of the
// plot for no gain on an empty band.
inline constexpr float kNoiseFraction = 0.12F;
inline constexpr float kDisplayMinimumSpanDb = 40.0F;

// Largest bin in each column's share of the span.
//
// columns may be longer or shorter than bins. Fewer columns than bins is the
// ordinary case and each column takes a run; more columns than bins repeats
// a bin across several, which draws a stepped trace rather than interpolating
// one. Interpolation would invent structure between two measured bins, and
// on a spectrum that reads as a signal.
inline void reduce_peak(std::span<const float> bins, std::span<float> columns)
{
    const std::size_t bin_count = bins.size();
    const std::size_t column_count = columns.size();
    if (bin_count == 0 || column_count == 0) {
        return;
    }

    for (std::size_t c = 0; c < column_count; ++c) {
        // Integer arithmetic on the numerator so the runs tile the span
        // exactly: a float step accumulates and leaves the last column
        // covering one bin more or fewer than it should, which is a
        // frequency error at the right-hand edge that nothing else explains.
        const std::size_t begin = bin_count * c / column_count;
        std::size_t end = bin_count * (c + 1) / column_count;
        if (end <= begin) {
            end = begin + 1;
        }
        end = std::min(end, bin_count);

        float peak = bins[begin];
        for (std::size_t i = begin + 1; i < end; ++i) {
            peak = std::max(peak, bins[i]);
        }
        columns[c] = peak;
    }
}

// How far above the frame's low percentile a noise-only column draws, when
// that column is the largest of `bins_per_column` bins.
//
// On an empty band a bin's power is the squared magnitude of complex
// Gaussian noise, which is exponential. The expected largest of K
// independent exponentials is the K-th harmonic number times their mean, and
// the p-th percentile of one of them is -ln(1-p) times it, so the gap
// between the two is the ratio of those, in decibels.
//
// H_K from the Euler expansion, which is better than a ten-thousandth from
// K = 2 upwards. K = 1 is the exact answer rather than the limit, and it is
// not a degenerate case: a display with one bin per column still draws a
// value a mean above the fifth percentile.
[[nodiscard]] inline float peak_reduction_headroom_db(std::size_t bins_per_column)
{
    constexpr double kEulerMascheroni = 0.577215664901532861;
    const double count = static_cast<double>(bins_per_column);
    const double harmonic =
        bins_per_column <= 1 ? 1.0 : std::log(count) + kEulerMascheroni + 0.5 / count;

    const double fraction = static_cast<double>(kSpectrumLowPermille) / 1000.0;
    const double percentile_of_mean = -std::log(1.0 - fraction);

    return static_cast<float>(10.0 * std::log10(harmonic / percentile_of_mean));
}

struct MapEnds {
    float floor_db = kSpectrumFloorDb;
    float ceiling_db = kSpectrumFloorDb + kSpectrumMinimumSpanDb;

    [[nodiscard]] constexpr float span_db() const { return ceiling_db - floor_db; }
};

// The two ends the PASSBAND displays draw against: the frame's, plus the
// reduction correction, held at least a minimum span apart. The span displays
// used this too until 2026-10-07 and now go through place_ends below, which
// lifts the noise off the bottom edge; the passband pane and its waterfall
// were not part of that request and keep this rule unchanged.
//
// The correction moves the floor and not the ceiling. A column containing a
// real signal draws that signal's own bin, and a maximum over K does not
// inflate a value that was already the largest, so the top of the map stays
// where the device put it.
//
// A pinned end takes no correction and is never moved, including by the
// minimum-span rule. A pin is an instruction in dBFS about where the map
// should end, not a measured percentile, so it is obeyed as written; when
// something has to give it comes out of the end that is still automatic, and
// if both are pinned nothing gives. The CLI got this wrong first and drew a
// pinned ceiling well above where it was asked for, which defeats the one
// job pinning has. The span displays offer the pins since 2026-09-22; see
// ScalePins below and resolve_ends.
//
// WHAT THIS PARAGRAPH USED TO SAY. It ended "There is no pin control in this
// client yet; the rule is carried here so that adding one is a control and
// not a rewrite." It was, and the control is qml/SpanView.qml's.
[[nodiscard]] inline MapEnds map_ends(float frame_floor_db, float frame_ceiling_db,
                                      float headroom_db, bool floor_pinned = false,
                                      bool ceiling_pinned = false)
{
    MapEnds ends;
    ends.floor_db = floor_pinned ? frame_floor_db : frame_floor_db + headroom_db;
    ends.ceiling_db = frame_ceiling_db;

    // The engine already holds its own two ends apart, and the correction
    // above has just eaten into that gap, so what is drawn against gets the
    // same bound re-applied. It comes out of whichever end is not pinned.
    if (ends.span_db() < kSpectrumMinimumSpanDb) {
        if (ceiling_pinned && !floor_pinned) {
            ends.floor_db = ends.ceiling_db - kSpectrumMinimumSpanDb;
        } else if (!ceiling_pinned) {
            ends.ceiling_db = ends.floor_db + kSpectrumMinimumSpanDb;
        }
        // Both pinned: the operator has said exactly what they want,
        // including a narrow span, and gets it.
    }
    return ends;
}

// ---------------------------------------------------------------------------
// The operator's pins
// ---------------------------------------------------------------------------
//
// docs/ui-spectrum.md: automatic is the default because it is right almost
// always, and the exception is comparing two captures, where a scale that
// moves is a scale that lies about which signal was stronger. So either end
// can be pinned at a level in dBFS, and the display draws against that level
// instead of the frame's.

// The narrowest map two pins may make. map_ends obeys a pair of pins however
// close, so the control that sets them is what keeps them apart; a map one
// decibel wide is already a two-colour picture, and a map of none or less
// divides by zero or draws upside down.
inline constexpr float kMinPinnedSpanDb = 1.0F;

// How far one press of a pin's nudge moves it.
inline constexpr float kPinStepDb = 1.0F;

struct ScalePins {
    bool floor_pinned = false;
    float floor_db = 0.0F;
    bool ceiling_pinned = false;
    float ceiling_db = 0.0F;
};

// The ends a span display draws against, from where its noise is drawn and
// where its strong signals reach. Both span displays use this: the spectrum
// from the engine's two percentiles, the waterfall from its own history.
//
// Unpinned, the noise goes kNoiseFraction up the map and the strong level at
// the top, with the map never narrower than kDisplayMinimumSpanDb; see those
// two for the numbers. A strong signal far above the noise widens the map and
// the noise stays where it is.
//
// The pin rule is map_ends' and for the same reason: a pinned end is drawn
// where it was pinned, and whatever has to give comes out of the automatic
// end. Under a pinned ceiling the floor still keeps the noise an eighth up
// whatever the ceiling leaves above it, so pinning the top does not throw the
// noise to the bottom edge.
[[nodiscard]] inline MapEnds place_ends(float noise_db, float strong_db, const ScalePins& pins)
{
    MapEnds ends;
    if (pins.floor_pinned && pins.ceiling_pinned) {
        ends.floor_db = pins.floor_db;
        ends.ceiling_db = pins.ceiling_db;
        return ends;
    }
    if (pins.floor_pinned) {
        ends.floor_db = pins.floor_db;
        ends.ceiling_db = std::max(strong_db, ends.floor_db + kDisplayMinimumSpanDb);
        return ends;
    }
    constexpr float kBelowPerAbove = kNoiseFraction / (1.0F - kNoiseFraction);
    if (pins.ceiling_pinned) {
        ends.ceiling_db = pins.ceiling_db;
        const float above = std::max(ends.ceiling_db - noise_db, 0.0F);
        ends.floor_db = std::min(noise_db - kBelowPerAbove * above,
                                 ends.ceiling_db - kDisplayMinimumSpanDb);
        return ends;
    }
    const float span =
        std::max(kDisplayMinimumSpanDb, (strong_db - noise_db) / (1.0F - kNoiseFraction));
    ends.floor_db = noise_db - kNoiseFraction * span;
    ends.ceiling_db = ends.floor_db + span;
    return ends;
}

// The spectrum's ends for this frame, with each pinned end in place of the
// frame's.
//
// The drawn noise is the engine's floor plus the reduction headroom: the
// floor is the fifth percentile of single bins, and an empty band's columns
// draw that headroom above it. The strong level is the engine's ceiling.
//
// WHAT THIS USED TO DO. It drew the floor AT floor plus headroom, so the
// noise sat on the bottom edge, and held the ends twelve decibels apart, so a
// band of noise with its ceiling decayed down onto it filled the plot. See
// kNoiseFraction for what the owner saw.
[[nodiscard]] inline MapEnds resolve_ends(float frame_floor_db, float frame_ceiling_db,
                                          float headroom_db, const ScalePins& pins)
{
    return place_ends(frame_floor_db + headroom_db, frame_ceiling_db, pins);
}

// Where a pin lands when the operator pins an end: at the level drawn at that
// moment, to a tenth of a decibel, which is what the plate beside it prints.
// Pinning where the map already is means the picture does not jump when the
// pin goes in.
[[nodiscard]] inline float pin_level(float drawn_db)
{
    return std::round(drawn_db * 10.0F) / 10.0F;
}

// A pin set or moved, kept at least kMinPinnedSpanDb from the other end when
// that end is pinned too. The end being moved is the one that gives: the
// operator is holding the other one where they put it.
[[nodiscard]] inline ScalePins set_floor_pin(ScalePins pins, float floor_db)
{
    pins.floor_pinned = true;
    pins.floor_db = pins.ceiling_pinned
                        ? std::min(floor_db, pins.ceiling_db - kMinPinnedSpanDb)
                        : floor_db;
    return pins;
}

[[nodiscard]] inline ScalePins set_ceiling_pin(ScalePins pins, float ceiling_db)
{
    pins.ceiling_pinned = true;
    pins.ceiling_db = pins.floor_pinned
                          ? std::max(ceiling_db, pins.floor_db + kMinPinnedSpanDb)
                          : ceiling_db;
    return pins;
}

// ---------------------------------------------------------------------------
// The colour map
// ---------------------------------------------------------------------------

struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

// THE MAP ITSELF IS render/colour_map.h, with lightness rising in a straight
// line in OKLab from the window's background to a warm near-white. Both span
// displays read it through colour_at below, so there is one source of colour.
//
// WHAT THIS PARAGRAPH USED TO SAY. The map was seven sRGB stops here, "chosen
// for monotone luminance", and they were not: Rec. 709 luma fell from about
// 182 at the yellow-green stop to about 168 at the orange one. The retraction
// of that claim was recorded here on 2026-09-22, and the owner asked the same
// day for the map to be made monotone, which colour_map.h does.

// Darkest to brightest, for a level already normalised to [0, 1]. Values
// outside that range are clamped rather than wrapped: a frame briefly above
// its own ceiling should saturate white, not fold back to black.
[[nodiscard]] inline Rgb colour_at(float level)
{
    const colour_map::Srgb8 c = colour_map::colour_at(level);
    return Rgb{c.r, c.g, c.b};
}

// The same map as a 0xAARRGGBB word, which is what QImage::Format_RGB32 and
// QRgb want. Kept beside colour_at so the two cannot disagree.
[[nodiscard]] inline std::uint32_t colour_argb_at(float level)
{
    const Rgb rgb = colour_at(level);
    return 0xFF000000U | (static_cast<std::uint32_t>(rgb.r) << 16U) |
           (static_cast<std::uint32_t>(rgb.g) << 8U) | static_cast<std::uint32_t>(rgb.b);
}

}  // namespace revenant::ui
