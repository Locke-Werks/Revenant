// Signal processing shared by the slotted weak-signal modes (FT8, FT4 and
// the rest of the family): a mixed-radix FFT, the 12 kHz input stage, and the
// slot clock that cuts a continuous stream into UTC-aligned intervals.
//
// No constant here comes from a mode document. The FFT is the textbook
// Cooley-Tukey decomposition for any length whose factors are small (J. W.
// Cooley and J. W. Tukey, Math. Comp. 19, 1965), done recursively. The input
// filter is a windowed-sinc low-pass. Each mode's own constants live in its
// own file with their citations.
//
// WHY 12 kHz
//
// Every one of these modes keys its symbol lengths in whole samples at 12 kHz
// (FT8's 0.160 s symbol is 1920 samples, FT4's 0.048 s is 576), and every one
// fits in under 3 kHz of USB audio, so the decoders all work there and the
// input stage brings any integer multiple of 12 kHz down to it.

#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::ConstRealSpan;
using dsp::SampleIndex;
using dsp::SampleRate;

inline constexpr SampleRate kWsjtRate = 12000;

// In-place discrete Fourier transform of any length. Lengths whose prime
// factors are all small are fast; a large prime factor falls back to a direct
// sum over that factor. The forward transform is unscaled, the inverse is
// scaled by 1/n so that the two are exact inverses.
void wsjt_fft(std::vector<std::complex<double>>& data, bool inverse);

// Brings audio at any integer multiple of 12 kHz down to 12 kHz. Output depends
// only on the concatenated input, never on how it was split into calls.
class WsjtInputStage {
public:
    [[nodiscard]] static Expected<WsjtInputStage> create(SampleRate input_rate);

    void process(ConstRealSpan audio, std::vector<float>& out);

    [[nodiscard]] int factor() const { return factor_; }

private:
    WsjtInputStage() = default;

    int factor_ = 1;
    std::vector<float> taps_;
    std::vector<float> history_;
    std::int64_t phase_ = 0;
};

// Returns the UTC time, in seconds since the Unix epoch, of an input sample.
// The decoders call it once, for sample 0, and count samples from there.
using WsjtUtcOfSample = std::function<double(SampleIndex)>;

// The UTC time of sample 0 taken from the wall clock at the moment it is
// asked, which is what a live receiver wants: the first block handed over is
// treated as arriving now.
[[nodiscard]] WsjtUtcOfSample wsjt_wall_clock();

// Cuts a 12 kHz stream into slots of a fixed length that start on whole
// multiples of that length in UTC, the way every slotted mode schedules its
// transmissions.
class WsjtSlotClock {
public:
    WsjtSlotClock(double slot_seconds, WsjtUtcOfSample utc_of_sample, SampleRate input_rate);

    // 12 kHz sample index of the first slot boundary at or after sample 0,
    // and the UTC time of that boundary. Settled on the first call.
    [[nodiscard]] std::int64_t first_boundary();
    [[nodiscard]] double first_boundary_utc();

    [[nodiscard]] std::int64_t slot_samples() const { return slot_samples_; }

private:
    void settle();

    double slot_seconds_;
    WsjtUtcOfSample utc_of_sample_;
    SampleRate input_rate_;
    std::int64_t slot_samples_;
    bool settled_ = false;
    std::int64_t first_boundary_ = 0;
    double first_boundary_utc_ = 0.0;
};

}  // namespace revenant::decode
