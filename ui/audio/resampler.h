// A windowed-sinc resampler from any stream rate to the sound card's.
//
// THE CLIENT HOLDS DSP NOW, AND THIS IS WHY. Until 2026-09-23 the player
// opened the sound card at whatever rate the focused receiver delivered and
// left out of the mix every receiver at another rate, on the argument that
// "this process holds no DSP" and ui/CMakeLists.txt links no part of
// core/dsp. Two things the owner hit came straight out of that: RDS raises a
// wfm receiver to 171000 S/s, which no shared-mode Windows output takes, so
// turning RDS on killed the audio with a sentence about 32-bit float; and a
// rack with a stereo wfm receiver focused left every mono receiver out of the
// mix. The owner approved the best-effort mix on 2026-09-23: every receiver
// resampled to the output device's rate, aligned by sample index, summed
// under a soft limiter. This is the first of those three.
//
// It is written here rather than linked from core/dsp because the client
// still links no part of the engine: core/dsp's filters are the engine's GPU
// twins, sized for channelisation, and pulling them in would pull in the
// reference build rules with them. What this needs is small enough to hold
// in one file and test in ui/tests.
//
// WHAT IT IS. Band-limited interpolation: an output frame is the input
// evaluated at a fractional position p, as the sum over the input frames n
// near p of x[n] h(p - n), with h a Kaiser-windowed sinc. The cutoff sits
// below the lower of the two Nyquists, so the same kernel is the anti-alias
// filter going down and the anti-image filter going up. The kernel is
// tabulated at kPhases points per input sample and read with linear
// interpolation between them.
//
// THE DEFAULT RESPONSE, per the lower of the two rates: flat to 0.42 of it,
// 80 dB down from 0.5. At a 48000 S/s output that is flat to 20160 Hz. A
// caller can ask for a lower cutoff with a transition of its own, which is how
// AudioMix turns a wfm multiplex into programme audio: flat to 15 kHz and
// 80 dB down at the 19 kHz pilot.
//
// WHAT IT COSTS. 2 * half_width multiplies a channel per output frame. From
// 171000 to 48000 that is 224, about 10.8 million a second for a mono
// receiver, on the sound card's pull thread. From 8000 up to 48000 it is 64.
// Equal rates with no cutoff asked for are a copy, so the ordinary case of a
// 48000 S/s receiver on a 48000 S/s card reaches the card bit for bit.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace revenant::ui {

class Resampler {
public:
    // Kernel points per input sample. 256 puts the error of reading the
    // table by linear interpolation under 2e-5 at the highest cutoff it is
    // used at, which is below the 80 dB stopband it serves.
    static constexpr int kPhases = 256;

    // The stopband depth every kernel here is designed to, in dB.
    static constexpr double kStopbandDb = 80.0;

    // cutoff_hz and transition_hz of zero take the default response above.
    // A cutoff above the default's is clamped to it: nothing here may pass a
    // frequency the slower of the two rates cannot carry.
    void configure(std::uint32_t in_rate, std::uint32_t out_rate, double cutoff_hz = 0.0,
                   double transition_hz = 0.0);

    [[nodiscard]] std::uint32_t in_rate() const { return in_rate_; }
    [[nodiscard]] std::uint32_t out_rate() const { return out_rate_; }

    // Equal rates and nothing asked of the filter: a copy.
    [[nodiscard]] bool passthrough() const { return passthrough_; }

    // Input frames advanced per output frame.
    [[nodiscard]] double step() const { return step_; }

    // An output frame at position p reads input frames floor(p) - half_width
    // + 1 through floor(p) + half_width. Zero for a passthrough.
    [[nodiscard]] int half_width() const { return half_width_; }

    // One output frame at absolute input position p, every channel.
    //
    // history holds `frames` interleaved input frames of `channels` each, the
    // first of them at absolute index `first`, and must cover the span
    // half_width() gives. out receives `channels` floats.
    void evaluate(const float* history, std::uint64_t first, std::size_t frames, int channels,
                  double p, float* out) const;

    // The kernel's value at an offset of t input samples, for the tests.
    [[nodiscard]] double kernel(double t) const;

private:
    std::uint32_t in_rate_ = 0;
    std::uint32_t out_rate_ = 0;
    bool passthrough_ = true;
    double step_ = 1.0;
    int half_width_ = 0;
    std::vector<float> table_;
};

}  // namespace revenant::ui
