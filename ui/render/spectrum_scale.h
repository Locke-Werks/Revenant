// Turning a frame's bins into what a column of pixels draws.
//
// Three things live here because both the spectrum trace and the waterfall
// need all three and neither owns them: the column reduction, the correction
// that reduction forces on the floor, and the colour map.
//
// WHAT THIS DELIBERATELY DOES NOT DO
//
// It does not auto-scale. docs/ui-spectrum.md puts both ends of the colour
// map on the device, tracked as percentiles with a fast attack and a thirty
// second decay, and SpectrumFrame carries the result in floor_db and
// ceiling_db. Every consumer of a frame draws against the same two numbers,
// which is what makes two rows of a waterfall comparable for absolute level.
// A display that measured its own ends would be a second scale disagreeing
// with the first, and the operator would have no way to tell which one the
// picture was drawn against.
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

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

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

// Largest bin in each column's share of the span.
//
// columns may be longer or shorter than bins. Fewer columns than bins is the
// ordinary case and each column takes a run; more columns than bins repeats
// a bin across several, which draws a stepped trace rather than interpolating
// one. Interpolation would invent structure between two measured bins, and
// on a spectrum that reads as a signal.
void reduce_peak(std::span<const float> bins, std::span<float> columns);

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
[[nodiscard]] float peak_reduction_headroom_db(std::size_t bins_per_column);

struct MapEnds {
    float floor_db = kSpectrumFloorDb;
    float ceiling_db = kSpectrumFloorDb + kSpectrumMinimumSpanDb;

    [[nodiscard]] constexpr float span_db() const { return ceiling_db - floor_db; }
};

// The two ends a row is actually drawn against: the frame's, plus the
// reduction correction, held at least a minimum span apart.
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
// job pinning has. There is no pin control in this client yet; the rule is
// carried here so that adding one is a control and not a rewrite.
[[nodiscard]] MapEnds map_ends(float frame_floor_db, float frame_ceiling_db,
                               float headroom_db, bool floor_pinned = false,
                               bool ceiling_pinned = false);

struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

// Darkest to brightest, for a level already normalised to [0, 1]. Values
// outside that range are clamped rather than wrapped: a frame briefly above
// its own ceiling should saturate white, not fold back to black.
[[nodiscard]] Rgb colour_at(float level);

// The same map as a 0xAARRGGBB word, which is what QImage::Format_RGB32 and
// QRgb want. Kept beside colour_at so the two cannot disagree.
[[nodiscard]] std::uint32_t colour_argb_at(float level);

}  // namespace revenant::ui
