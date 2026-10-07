// The receiver squelch: when a receiver's audio passes and when it is muted.
//
// WHAT IT COMPARES. The level is the receiver's channel power after its own
// filter and before the detector, in dBFS, which core/shaders/vrx_demod.comp
// writes beside the audio (StageOutput::channel_power). Until 2026-10-07 the
// gate compared the DEMODULATED AUDIO's level instead. For AM and SSB that is
// roughly the signal; for FM it is backwards. A limiting discriminator's
// output does not depend on the input level, and with no carrier the phase
// between samples is uniform over a full turn, so an empty NFM channel read
// several dB above full scale and a station read around -10 dBFS. No threshold
// the slider could reach shut on noise, and any that did shut on noise also
// shut on the station. AM's DC removal took the carrier off the same way, so
// an AM carrier between words read as silence.
//
// WHAT IT DOES WITH IT, which is what an operator expects from SDR++ or a
// hardware receiver:
//
//   averaging   the level is a one-pole average of power with time constant
//               average_s, so one noisy dispatch does not flip the gate.
//   hysteresis  opens at the threshold, shuts only below threshold minus
//               hysteresis_db. A signal sitting on the threshold does not
//               chatter.
//   attack      the level has to stay over the threshold for attack_s before
//               the gate opens, so a noise spike does not pop it.
//   tail        once open, the level has to stay under the close point for
//               tail_s before the gate shuts, so the gaps between syllables
//               and a fade do not chop the audio.
//   fade        the audio gain ramps between 0 and 1 over fade_s rather than
//               stepping, which is what removes the click at each edge.
//
// Pure arithmetic over spans, no device and no clock, so tests/engine/
// test_squelch_gate.cpp can drive it with any level sequence it likes.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

#include "core/engine/signal_meter.h"

namespace revenant::engine {

struct SquelchTiming {
    double hysteresis_db = 3.0;
    double average_s = 0.02;
    double attack_s = 0.01;
    double tail_s = 0.25;
    double fade_s = 0.005;
};

// At or under this the squelch is off: VrxParams::squelch_dbfs's default, and
// the UI slider's bottom stop.
inline constexpr double kSquelchOffDbfs = -200.0;

class SquelchGate {
public:
    // Forget the averaged level and the timers. The gate's open state and its
    // fade gain are kept, so a reset never introduces a click by itself.
    void reset_level() { primed_ = false; }

    // One dispatch. `mean_power` is the channel's mean |z|^2 over these
    // frames; `audio` is the frames themselves, interleaved over `channels`,
    // and is faded in place. Returns whether anything audible passes in this
    // chunk, which is what AudioChunk::squelch_open reports.
    bool process(double mean_power, std::span<float> audio, std::uint32_t channels,
                 double rate, double threshold_dbfs, const SquelchTiming& timing = {}) {
        const std::uint32_t lanes = std::max<std::uint32_t>(1, channels);
        const std::size_t frames = audio.size() / lanes;
        const double dt = (rate > 0.0) ? static_cast<double>(frames) / rate : 0.0;

        update_level(mean_power, dt, timing.average_s);

        if (!(threshold_dbfs > kSquelchOffDbfs)) {
            // Off is a straight wire, not a gate that happens to be open: no
            // fade-in at the start of a stream and no multiply at all.
            open_ = true;
            gain_ = 1.0;
            above_s_ = 0.0;
            below_s_ = 0.0;
            return true;
        }
        if (!open_) {
            below_s_ = 0.0;
            if (level_dbfs_ >= threshold_dbfs) {
                above_s_ += dt;
                if (above_s_ >= timing.attack_s) {
                    open_ = true;
                    above_s_ = 0.0;
                }
            } else {
                above_s_ = 0.0;
            }
        } else {
            above_s_ = 0.0;
            if (level_dbfs_ < threshold_dbfs - timing.hysteresis_db) {
                below_s_ += dt;
                if (below_s_ >= timing.tail_s) {
                    open_ = false;
                    below_s_ = 0.0;
                }
            } else {
                below_s_ = 0.0;
            }
        }

        const bool audible = open_ || gain_ > 0.0;
        apply_fade(audio, lanes, frames, rate, timing.fade_s);
        return audible;
    }

    [[nodiscard]] bool open() const { return open_; }
    [[nodiscard]] double level_dbfs() const { return level_dbfs_; }
    [[nodiscard]] double gain() const { return gain_; }

private:
    void update_level(double mean_power, double dt, double average_s) {
        const double power = (mean_power > 0.0 && std::isfinite(mean_power)) ? mean_power : 0.0;
        if (!primed_) {
            averaged_ = power;
            primed_ = true;
        } else if (average_s > 0.0) {
            const double alpha = 1.0 - std::exp(-dt / average_s);
            averaged_ += alpha * (power - averaged_);
        } else {
            averaged_ = power;
        }
        level_dbfs_ = dbfs_of(std::sqrt(averaged_));
    }

    void apply_fade(std::span<float> audio, std::uint32_t lanes, std::size_t frames,
                    double rate, double fade_s) {
        const double target = open_ ? 1.0 : 0.0;
        const double step =
            (fade_s > 0.0 && rate > 0.0) ? 1.0 / (fade_s * rate) : 1.0;
        for (std::size_t f = 0; f < frames; ++f) {
            if (gain_ < target) {
                gain_ = std::min(target, gain_ + step);
            } else if (gain_ > target) {
                gain_ = std::max(target, gain_ - step);
            }
            if (gain_ >= 1.0) {
                continue;
            }
            const auto g = static_cast<float>(gain_);
            for (std::uint32_t c = 0; c < lanes; ++c) {
                audio[f * lanes + c] *= g;
            }
        }
    }

    bool primed_ = false;
    double averaged_ = 0.0;
    double level_dbfs_ = kSilenceFloorDbfs;
    bool open_ = false;
    double above_s_ = 0.0;
    double below_s_ = 0.0;
    double gain_ = 0.0;
};

}  // namespace revenant::engine
