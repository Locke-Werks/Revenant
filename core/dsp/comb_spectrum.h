// The comb detection's own spectrum: an average of Hann-windowed power
// spectra at a fixed fine resolution, taken from the raw stream rather than
// from the display spectrum.
//
// WHY ITS OWN. The display spectrum's size follows what the client asked for,
// and at 8192 bins over 2.4 MS/s a bin is 293 Hz. The owner's HF comb carries
// supply-ripple sidebands 100.7 Hz either side of every line, a comb of their
// own (core/dsp/comb_cancel.h, kCombMaxCombs); at 293 Hz they fall in the
// line's bin, detect_combs sees one comb, and cancelling its carriers leaves
// the bin a dB or two lower and the waterfall unchanged. Live on 80 m the
// ladder is every 59.5 Hz, mains ripple, and the first rung sits inside a
// carrier's window response at 36.6 Hz a bin; 131072 points is 18.3 Hz at
// 2.4 MS/s, where it is three bins out and a peak of its own.
//
// PROVENANCE, per docs/clean-room.md: an iterative radix-2 transform and a
// Hann window, textbook; this project's own arrangement.

#pragma once

#include <complex>
#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/types.h"

namespace revenant::dsp {

inline constexpr std::uint32_t kCombSpectrumPoints = 131072;

// The resolution aimed for, and the transform size that gives it at a rate:
// the power of two nearest rate / kCombSpectrumBinHz, at most
// kCombSpectrumPoints. 131072 at 2.4 MS/s, 65536 at 1.152 MS/s.
inline constexpr double kCombSpectrumBinHz = 18.0;
[[nodiscard]] std::uint32_t comb_spectrum_points(SampleRate rate);

// Frames averaged into one detection. Eight puts a bin's dB scatter near
// 1.6 dB, well under detect_comb's 4 dB minimum height, and costs 0.44 s of
// stream at 2.4 MS/s, so a detection comes about once a second.
inline constexpr std::uint32_t kCombSpectrumFrames = 8;

class CombSpectrumAverager {
public:
    explicit CombSpectrumAverager(std::uint32_t points = kCombSpectrumPoints);

    // One frame of exactly points() samples. Any other length is ignored.
    void add(std::span<const Complex32> frame);

    [[nodiscard]] std::uint32_t points() const { return points_; }
    [[nodiscard]] std::uint32_t frames() const { return frames_; }

    // The average so far, linear power in natural order with DC at
    // points / 2, the order detect_combs reads; then starts over.
    [[nodiscard]] std::vector<float> take();

private:
    std::uint32_t points_ = 0;
    std::uint32_t frames_ = 0;
    std::vector<double> window_;
    std::vector<std::uint32_t> reversed_;
    std::vector<std::complex<double>> twiddles_;
    std::vector<std::complex<double>> work_;
    std::vector<double> sum_;
};

}  // namespace revenant::dsp
