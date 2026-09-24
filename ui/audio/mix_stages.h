// The two per-sample stages AudioMix runs besides the resampler: a
// de-emphasis for a wfm multiplex played as programme audio, and the soft
// limiter on the sum.
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
