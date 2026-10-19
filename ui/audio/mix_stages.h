// The per-sample stages AudioMix runs besides the resampler: a stereo
// decoder and a de-emphasis for a wfm multiplex played as programme audio,
// and the soft limiter on the sum.
//
// WHAT THE FIRST SENTENCE USED TO SAY, before StereoDecoder: "The two
// per-sample stages", the multiplex played as mono.
//
// Qt-free and header-only, so ui/tests holds each one on its own.
//
// WHAT THE FIRST SENTENCE USED TO SAY: "The three per-sample stages", the
// first of them "a level AGC for the receivers whose audio comes out at the
// level the signal came in at". That was LevelAgc, a stopgap for an engine
// that applied no AGC, and it is gone; see the note where it was.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace revenant::ui {

// WHERE LevelAgc WAS, and why it went. It brought am, usb, lsb, dsb and cw
// to a -12 dBFS peak here in the mix, with VrxParams' 10 ms attack and
// 500 ms decay and at most 70 dB of gain, because those five arrived from
// the engine at the level the signal came in at: 3e-7 to 2e-6 peak through
// tests/rpc's harness on 2026-09-23, which the owner heard as nothing.
//
// The engine applies the receiver AGC to what a subscription carries now,
// core/engine/listener_level.h, with the same target, the same ceiling and
// the receiver's own time constants, and holds the gain when the receiver
// panel switches it off. A second AGC here would re-level what the engine
// had set and undo that hold, so the mix takes every stream at the level it
// arrives and the limiter below is the only thing between the sum and the
// card.
//
// WHAT ITS NOTE USED TO SAY about the engine: that "the receiver AGC
// VrxParams declares, agc_enabled with a 10 ms attack and a 500 ms decay, is
// applied nowhere in core/engine".

// A one-pole de-emphasis, for a wfm receiver whose output is the multiplex.
//
// A receiver raised to the 171000 S/s composite for RDS hands out the
// multiplex with no curve, because core/engine/vrx.h's resolve_deemphasis
// takes none above 114000 S/s so the subcarrier reaches the decoder intact.
// Played as it comes, that is programme audio with its pre-emphasis still on,
// bright and hissy, plus the 19 kHz pilot. AudioMix filters it to 15 kHz in
// the resampler and applies this after, 75 us by default, which is the curve
// resolve_deemphasis gives a wfm receiver below the composite rate.
class Deemphasis {
public:
    void configure(std::uint32_t rate, double tau_us = 75.0)
    {
        const double fs = static_cast<double>(std::max<std::uint32_t>(rate, 1));
        alpha_ = 1.0 - std::exp(-1.0 / (tau_us * 1e-6 * fs));
        state_[0] = 0.0;
        state_[1] = 0.0;
    }

    void process(float* samples, std::size_t frames, int channels)
    {
        // Two states, so a third channel and beyond pass untouched; nothing
        // this is used on has more than two.
        const auto stride = static_cast<std::size_t>(std::max(channels, 1));
        const std::size_t filtered = std::min<std::size_t>(stride, 2);
        for (std::size_t i = 0; i < frames; ++i) {
            for (std::size_t c = 0; c < filtered; ++c) {
                double& y = state_[c];
                y += alpha_ * (static_cast<double>(samples[i * stride + c]) - y);
                samples[i * stride + c] = static_cast<float>(y);
            }
        }
    }

private:
    double alpha_ = 1.0;
    double state_[2] = {0.0, 0.0};
};

// FM stereo from a wfm multiplex, at the multiplex's own rate.
//
// A wfm receiver at the composite rate hands out the multiplex, which still
// carries the 19 kHz pilot and the L-R channel as a 38 kHz DSB-SC subcarrier
// coherent with it. This turns each mono multiplex frame into a stereo pair
// ahead of the resampler: a phase-locked loop on the pilot gives the 38 kHz
// reference as its doubled phase, the multiplex times that reference is L-R,
// and the matrix is applied at once. Nothing here lowpasses. The resampler's
// 15 kHz programme filter, 80 dB down at the pilot, takes the pilot, the
// products at 19, 57 and 76 kHz and the RDS subcarrier off both channels,
// which is the L+R and L-R lowpass in one, and Deemphasis then runs on each
// channel at the curve it already used.
//
// MONO WITHOUT A PILOT, BIT FOR BIT. With the loop unlocked the difference is
// scaled by exactly zero, so both channels are the same samples through the
// same filters, and a station with no pilot comes back as two identical
// channels, which is how a consumer tells mono from stereo. Locking takes
// the pilot clearing kLockOn for kLockOnSeconds, and losing it takes falling
// under kLockOff for kLockOffSeconds, so a pilot near the threshold does not
// flap; the difference is faded in and out over kBlendSeconds so the switch
// does not click.
//
// THE LOCK MEASURE is the pilot's amplitude over the multiplex's RMS, both
// averaged over about 30 ms, with the loop's phase error under about 30
// degrees. A pilot at its nominal 9 percent of deviation under programme at
// a typical level reads 0.1 to 0.3; broadband noise with no pilot reads
// around 0.01; silence reads zero, since the RMS is floored rather than
// divided into.
//
// The loop is a second-order PI at about 20 Hz natural frequency, with its
// error normalised by the pilot's measured amplitude so its bandwidth does
// not move with the station's level. That pulls in the few hertz an engine's
// clock puts the pilot off 19 kHz in well under a second.
class StereoDecoder {
public:
    static constexpr double kPilotHz = 19'000.0;
    static constexpr double kLockOn = 0.05;
    static constexpr double kLockOff = 0.03;
    static constexpr double kLockOnSeconds = 0.1;
    static constexpr double kLockOffSeconds = 0.3;
    static constexpr double kBlendSeconds = 0.03;

    void configure(std::uint32_t rate)
    {
        constexpr double tau = 2.0 * 3.14159265358979323846;
        fs_ = static_cast<double>(std::max<std::uint32_t>(rate, 1));
        const double wn = tau * 20.0 / fs_;
        omega_ = tau * kPilotHz / fs_;
        kp_ = 2.0 * 0.707 * wn;
        ki_ = wn * wn;
        // 50 Hz of pull either way, far more than two crystals disagree by.
        freq_limit_ = tau * 50.0 / fs_;
        loop_alpha_ = 1.0 - std::exp(-tau * 300.0 / fs_);
        lock_alpha_ = 1.0 - std::exp(-1.0 / (0.03 * fs_));
        blend_alpha_ = 1.0 - std::exp(-1.0 / (kBlendSeconds * fs_));
        reset();
    }

    void reset()
    {
        phase_ = 0.0;
        freq_ = 0.0;
        i_loop_ = q_loop_ = 0.0;
        i_slow_ = q_slow_ = power_ = 0.0;
        held_ = 0.0;
        locked_ = false;
        blend_ = 0.0;
    }

    // Reads `frames` mono multiplex samples from `in` and writes `frames`
    // interleaved L, R pairs to `out`.
    void process(const float* in, std::size_t frames, float* out)
    {
        constexpr double tau = 2.0 * 3.14159265358979323846;
        const double on_frames = kLockOnSeconds * fs_;
        const double off_frames = kLockOffSeconds * fs_;
        for (std::size_t n = 0; n < frames; ++n) {
            const double x = static_cast<double>(in[n]);
            const double s = std::sin(phase_);
            const double c = std::cos(phase_);

            // The pilot is sin(phase) when locked: x * sin averages to half
            // its amplitude and x * cos to half its amplitude times the sine
            // of the phase error.
            i_loop_ += loop_alpha_ * (x * s - i_loop_);
            q_loop_ += loop_alpha_ * (x * c - q_loop_);
            const double magnitude = std::sqrt(i_loop_ * i_loop_ + q_loop_ * q_loop_);
            const double error = magnitude > 1e-12 ? q_loop_ / magnitude : 0.0;
            freq_ = std::clamp(freq_ + ki_ * error, -freq_limit_, freq_limit_);
            phase_ += omega_ + freq_ + kp_ * error;
            if (phase_ >= tau) {
                phase_ -= tau;
            } else if (phase_ < 0.0) {
                phase_ += tau;
            }

            i_slow_ += lock_alpha_ * (x * s - i_slow_);
            q_slow_ += lock_alpha_ * (x * c - q_slow_);
            power_ += lock_alpha_ * (x * x - power_);
            const double rms = std::sqrt(std::max(power_, 1e-20));
            const double pilot = 2.0 * i_slow_;
            const double measure = pilot / rms;
            const bool in_phase = i_slow_ > 0.0 && std::abs(q_slow_) < 0.58 * i_slow_;
            if (!locked_) {
                held_ = measure > kLockOn && in_phase ? held_ + 1.0 : 0.0;
                if (held_ >= on_frames) {
                    locked_ = true;
                    held_ = 0.0;
                }
            } else {
                held_ = measure < kLockOff || !in_phase ? held_ + 1.0 : 0.0;
                if (held_ >= off_frames) {
                    locked_ = false;
                    held_ = 0.0;
                }
            }
            blend_ += blend_alpha_ * ((locked_ ? 1.0 : 0.0) - blend_);
            // Snapped, so mono is reached exactly and stays bit for bit.
            if (!locked_ && blend_ < 1e-4) {
                blend_ = 0.0;
            } else if (locked_ && blend_ > 1.0 - 1e-4) {
                blend_ = 1.0;
            }

            // The subcarrier is sin(2 phase) = 2 s c; x times it averages to
            // half of the L-R it carries, so twice that is L-R at the scale
            // L+R has in x.
            const double difference = blend_ == 0.0 ? 0.0 : blend_ * 4.0 * x * s * c;
            out[2 * n] = static_cast<float>(x + difference);
            out[2 * n + 1] = static_cast<float>(x - difference);
        }
    }

    [[nodiscard]] bool stereo() const { return locked_; }

private:
    double fs_ = 1.0;
    double omega_ = 0.0;
    double kp_ = 0.0;
    double ki_ = 0.0;
    double freq_limit_ = 0.0;
    double loop_alpha_ = 1.0;
    double lock_alpha_ = 1.0;
    double blend_alpha_ = 1.0;

    double phase_ = 0.0;
    double freq_ = 0.0;
    double i_loop_ = 0.0;
    double q_loop_ = 0.0;
    double i_slow_ = 0.0;
    double q_slow_ = 0.0;
    double power_ = 0.0;
    double held_ = 0.0;
    bool locked_ = false;
    double blend_ = 0.0;
};

// The soft limiter on the mix.
//
// Several receivers summed can pass full scale, and an AGC settling
// overshoots on its own; the float sink used to hand that to the device,
// which clipped it. This holds the sum under kThreshold with a gain that
// drops at once to whatever the loudest frame needs and recovers over
// kReleaseMs, so a burst is turned down rather than squared off. Below the
// threshold with the gain recovered it multiplies by exactly one, so a
// single receiver at an ordinary level leaves it exactly as the resampler
// handed it over.
//
// WHAT THE SENTENCE BEFORE THIS USED TO SAY: "so a single receiver at an
// ordinary level reaches the card bit for bit". The drift trim reads a
// receiver at the card's rate through the resampler's kernel once the trim
// moves off zero, since 2026-09-23, so what reaches the card is the kernel's
// output; audio/drift_trim.h.
class SoftLimiter {
public:
    // -1 dBFS.
    static constexpr double kThreshold = 0.891;
    static constexpr double kReleaseMs = 100.0;

    void configure(std::uint32_t rate)
    {
        const double fs = static_cast<double>(std::max<std::uint32_t>(rate, 1));
        release_ = 1.0 - std::exp(-1000.0 / (kReleaseMs * fs));
        gain_ = 1.0;
    }

    // Answers whether any frame was turned down.
    bool process(float* samples, std::size_t frames, int channels)
    {
        const auto width = static_cast<std::size_t>(std::max(channels, 1));
        bool limited = false;
        for (std::size_t i = 0; i < frames; ++i) {
            float* frame = samples + i * width;
            double peak = 0.0;
            for (std::size_t c = 0; c < width; ++c) {
                peak = std::max(peak, std::abs(static_cast<double>(frame[c])));
            }
            if (peak * gain_ > kThreshold) {
                gain_ = kThreshold / peak;
            }
            if (gain_ < 1.0) {
                limited = true;
                for (std::size_t c = 0; c < width; ++c) {
                    frame[c] = static_cast<float>(static_cast<double>(frame[c]) * gain_);
                }
                gain_ += (1.0 - gain_) * release_;
                // Snapped to one rather than approached for ever, so the
                // multiply by exactly one below the threshold is reached.
                // 1e-4 is a thousandth of a decibel, and from the deepest
                // reduction an overload of a few times full scale asks for
                // it is reached in about nine release time constants.
                if (gain_ > 1.0 - 1e-4) {
                    gain_ = 1.0;
                }
            }
        }
        return limited;
    }

    [[nodiscard]] double gain() const { return gain_; }

private:
    double release_ = 1.0;
    double gain_ = 1.0;
};

}  // namespace revenant::ui
