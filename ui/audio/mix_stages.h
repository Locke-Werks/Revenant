// The three per-sample stages AudioMix runs besides the resampler: a level
// AGC for the receivers whose audio comes out at the level the signal came in
// at, a de-emphasis for a wfm multiplex played as programme audio, and the
// soft limiter on the sum.
//
// Qt-free and header-only, so ui/tests holds each one on its own.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace revenant::ui {

// Brings amplitude-detected audio to a listening level.
//
// WHY THE CLIENT HAS ONE. AM, SSB and CW produced no audio the owner could
// hear, and the cause is not the wire: measured 2026-09-23 through
// tests/rpc/test_rpc_audio.cpp's harness, every one of those modes streams
// 48000 S/s mono chunks with the gate open, and their peaks read 8.7e-7 for
// am, 2.8e-7 for usb, 1.7e-6 for lsb, 1.2e-6 for dsb and 1.1e-6 for cw
// against 9.97 for nfm and 4.61 for wfm, on the fixture's emitters 30 to 40
// dB over a -100 dBFS floor. An envelope detector and a product detector hand
// out audio in the input's own units, core/dsp/vrx_reference.cpp's
// vrx_demod_gain is unity for all five, and the receiver AGC VrxParams
// declares, agc_enabled with a 10 ms attack and a 500 ms decay, is applied
// nowhere in core/engine. A discriminator is level-independent, which is why
// nfm and wfm were heard and nothing else was.
//
// Here rather than in the engine because what it changes is only what a
// person hears: the decoders on the same receiver, a recording and the RDS
// route all read the receiver's output as the engine made it, and an AGC in
// front of a CW or RTTY decoder pumps the noise up between elements. The
// attack and decay are VrxParams' own defaults.
//
// A peak follower with a fast attack and a slow decay, and a gain that
// brings the peak to kTarget, never above kMaxGainDb. A channel with nothing
// on it comes up to the ceiling's worth of noise, as on any radio with an
// AGC.
class LevelAgc {
public:
    // The peak level the AGC holds audio at: -12 dBFS, leaving the rest for
    // several receivers summed under the limiter.
    static constexpr double kTarget = 0.25;

    // The most gain it applies. 70 dB brings a -82 dBFS peak to kTarget.
    static constexpr double kMaxGainDb = 70.0;

    void configure(std::uint32_t rate, double attack_ms = 10.0, double decay_ms = 500.0)
    {
        const double fs = static_cast<double>(std::max<std::uint32_t>(rate, 1));
        attack_ = 1.0 - std::exp(-1000.0 / (attack_ms * fs));
        decay_ = 1.0 - std::exp(-1000.0 / (decay_ms * fs));
        floor_ = kTarget / std::pow(10.0, kMaxGainDb / 20.0);
        envelope_ = 0.0;
    }

    // In place, `frames` interleaved frames of `channels`, one gain for all
    // the channels of a frame.
    void process(float* samples, std::size_t frames, int channels)
    {
        const auto width = static_cast<std::size_t>(std::max(channels, 1));
        for (std::size_t i = 0; i < frames; ++i) {
            float* frame = samples + i * width;
            double peak = 0.0;
            for (std::size_t c = 0; c < width; ++c) {
                peak = std::max(peak, std::abs(static_cast<double>(frame[c])));
            }
            envelope_ += (peak - envelope_) * (peak > envelope_ ? attack_ : decay_);
            const double gain = kTarget / std::max(envelope_, floor_);
            for (std::size_t c = 0; c < width; ++c) {
                frame[c] = static_cast<float>(static_cast<double>(frame[c]) * gain);
            }
        }
    }

    [[nodiscard]] double gain() const { return kTarget / std::max(envelope_, floor_); }

private:
    double attack_ = 1.0;
    double decay_ = 1.0;
    double floor_ = 1.0;
    double envelope_ = 0.0;
};

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

// The soft limiter on the mix.
//
// Several receivers summed can pass full scale, and an AGC settling
// overshoots on its own; the float sink used to hand that to the device,
// which clipped it. This holds the sum under kThreshold with a gain that
// drops at once to whatever the loudest frame needs and recovers over
// kReleaseMs, so a burst is turned down rather than squared off. Below the
// threshold with the gain recovered it multiplies by exactly one, so a
// single receiver at an ordinary level reaches the card bit for bit.
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
