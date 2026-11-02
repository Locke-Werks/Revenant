// Feld-Hellschreiber: receiver audio to columns of pixels.
//
// SPECIFICATION
//
// M. Greenman ZL1BPU, "Feld-Hell", https://www.qsl.net/zl1bpu/HELL/Feld.htm,
// (c) 1997-2009, fetched 2026-11-02. Cited as "Feld-Hell" with its section
// heading. The modes.md row names "Hellschreiber Modes - Technical
// Specifications" (1998); the page at the address that title used to live
// at (HELL/tech.htm) now answers 404, and Feld.htm, by the same author, is
// what the site serves for the mode.
//
// WHAT THE PAGE FIXES
//
//   - On-off keying, a dot is key down and a blank key up, "How Characters
//     are Transmitted" and "Transmission Timing".
//   - 122.5 pixels a second for pixels of full height, 150 characters a
//     minute of seven columns (five and two blank), so 17.5 columns a second,
//     "How Characters are Transmitted" and "Character Generation" ("so long
//     as there are exactly 17.5 columns per second ... it will make no
//     difference to the receiver").
//   - Fourteen dot positions a column, sent as half-height pixels of half the
//     duration, "The Two Pixel Rule". So 245 half-pixels a second, 4.082 ms
//     each, 14 to a column.
//   - Each column is sent bottom to top, columns left to right.
//   - Raised cosine dot shaping, "Dot Shaping"; the receiver does not depend
//     on it.
//   - No synchronism is sent; the receiver is quasi-synchronous and the
//     reader's eye takes the vertical phase, "Transmission Timing". So a
//     column here starts wherever the receiver's clock put it, and the
//     double printing that hides that is the display's business.
//
// WHAT THIS FILE CHOSE
//
// An envelope detector at a given audio carrier, integrated over each
// half-pixel slot on a fixed clock from the first sample, and a threshold at
// half a slowly decaying peak for the bits. The levels are reported as well,
// because the page's own advice ("Black and white ... and grey-scale
// presentation") is that the eye reads grey better than any slicer. Text is
// not produced: Hell carries pictures of characters, and no font table is
// in this tree or needed to receive.
//
// CLEAN ROOM
//
// No Hellschreiber implementation was read.

#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <deque>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::ConstRealSpan;
using dsp::SampleIndex;
using dsp::SampleRate;

// Feld-Hell, "How Characters are Transmitted" and "The Two Pixel Rule".
inline constexpr double kFeldHellPixelRate = 122.5;
inline constexpr int kFeldHellHalfPixelsPerColumn = 14;
inline constexpr double kFeldHellSlotRate = 2.0 * kFeldHellPixelRate;  // 245
inline constexpr double kFeldHellColumnRate = kFeldHellSlotRate / kFeldHellHalfPixelsPerColumn;

struct FeldHellConfig {
    SampleRate rate = 8000;
    double carrier_hz = 1000.0;
    // Half-width of the envelope detector's passband, an engineering choice:
    // the page's raised-cosine signal is about 245 Hz wide.
    double bandwidth_hz = 250.0;
};

struct FeldHellColumn {
    // Audio sample index of the column's first (bottom) slot.
    SampleIndex position = 0;
    // Bit i set: slot i, counted from the bottom, was key down.
    std::uint16_t bits = 0;
    // Envelope per slot, the same order, scaled to the detector's peak.
    std::array<float, kFeldHellHalfPixelsPerColumn> levels{};
};

class FeldHellDecoder {
public:
    [[nodiscard]] static Expected<FeldHellDecoder> create(const FeldHellConfig& config);
    // One column per 14 slots on a fixed clock, a column after its last
    // slot was received. Chunking does not change it.
    void process(ConstRealSpan audio, std::vector<FeldHellColumn>& out);
    void reset();

private:
    FeldHellDecoder() = default;

    FeldHellConfig config_{};
    double samples_per_slot_ = 0.0;
    std::vector<float> taps_;
    std::vector<std::complex<float>> history_;
    std::size_t history_at_ = 0;
    std::complex<double> phasor_{1.0, 0.0};
    std::complex<double> step_{1.0, 0.0};
    SampleIndex index_ = 0;
    std::int64_t slot_ = 0;
    double slot_sum_ = 0.0;
    int slot_count_ = 0;
    double peak_ = 0.0;
    FeldHellColumn column_{};
    int in_column_ = 0;
    std::deque<double> pending_;
};

}  // namespace revenant::decode
