// The receiver AGC, and the one stage between a receiver's output and the
// audio a person listens to.
//
// WHY IT SITS AFTER THE RECEIVER AND NOT IN IT. A receiver's output has more
// readers than a loudspeaker: every decoder in core/rpc/decoders.h, the RDS
// route, a recording and the probe pool's extracts all take the samples the
// demodulator handed out. An AGC in front of a CW or RTTY decoder lifts the
// noise between elements, and one in front of a recording writes the AGC's
// gain into the file where nothing can take it back out. So the graph hands
// out both: AudioChunk::samples is the receiver's output as it always was,
// and AudioChunk::heard is the same frames through this stage. The engine's
// consumers that are a person listening read the second, and nothing else
// does. core/engine/graph.cpp runs it once per receiver per dispatch, on the
// completion thread, whether or not anybody is listening, so the envelope is
// warm when somebody starts.
//
// WHAT IT DOES PER MODE, which levelling_for below decides:
//
//   am, usb, lsb, dsb, cw   the AGC. An envelope detector and a product
//                           detector hand out audio in the input's own units,
//                           dsp::vrx_demod_gain is unity for all five, so
//                           without this a -60 dBFS station is -60 dBFS of
//                           audio. Measured through tests/rpc's harness on
//                           2026-09-23, before this existed: those five
//                           peaked at 3e-7 to 2e-6 and the owner heard
//                           nothing from any of them.
//
//   nfm, wfm                a fixed gain, kFmHeardGain. A discriminator's
//                           output is set by the deviation and not by the
//                           input level: tests/engine/test_engine_agc.cpp
//                           measures both modes from inputs 30 dB apart and
//                           the two peaks agree to 0.01 dB. An AGC there
//                           would ride the programme's own loudness and pump
//                           the noise between transmissions, and it would buy
//                           nothing the discriminator does not already give.
//                           The gain puts full deviation, which the demod gain
//                           scales to +/-1, at the AGC's own target, so an FM
//                           receiver and an AGC receiver beside it in the rack
//                           play at the same peak.
//
//   raw and the digital     nothing, and AudioChunk::heard is the samples
//   voice taps              themselves. They are complex baseband for a
//                           decoder, not audio, and a P25 receiver's voice
//                           reaches a person through the vocoder, which
//                           writes its own level.
//
// AGC OFF HOLDS THE GAIN. VrxParams has no manual gain, and a fixed number
// cannot be a sane level for every station: a -30 and a -80 dBFS signal are
// both ordinary on one band. So agc_enabled false stops the AGC tracking and
// keeps the gain it last had, which is the gain that made the station being
// heard audible, and switching it back on starts the AGC afresh from the
// level of the next block. A receiver that starts with its AGC off takes the
// gain its first block with any signal in it calls for, and holds that. It
// holds through a retune as well, since off means the operator has taken the
// level into their own hands.
//
// A RETUNE RESTARTS IT. A receiver moved further than its own passband's
// width is listening to something else, and an envelope carried over from the
// last station is wrong by however much louder that station was: 30 dB low
// for the length of a decay, half a second at the default, or 30 dB high
// until the attack catches it. restart() makes the next block PRIME the
// envelope from its own peak over one attack time instead, so the first
// frame of the new station is already at the level. A nudge inside the
// passband keeps the envelope, because it is the same signal. graph.cpp
// decides which is which on the recording thread, where the retune lands.
//
// SQUELCH CLOSED FREEZES IT. The graph writes zeros for a closed gate, and an
// AGC following them would decay to its ceiling, so the first syllable after
// the gate opened would arrive 70 dB up. A closed chunk passes zeros and
// touches no state.
//
// Qt-free and header-only, with no device in it, so tests/engine holds it on
// its own. The arithmetic is double throughout: the envelope decays toward
// the input for as long as a station is quiet, and a float would reach its
// denormal range on a long open silence where a double does not.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>

#include "core/dsp/types.h"
#include "core/engine/vrx.h"
#include "core/error.h"

namespace revenant::engine {

// What stands between a receiver's output and the audio a person hears.
enum class Levelling : std::uint8_t {
    None,
    Agc,
    Fixed,
};

// Which of the three a mode gets. Exhaustive over Demod and with no default,
// for the reason resolve_deemphasis gives: None is right for five modes and a
// thirteenth would otherwise inherit an inaudible stream.
[[nodiscard]] constexpr Levelling levelling_for(Demod mode) {
    switch (mode) {
        case Demod::Am:
        case Demod::Usb:
        case Demod::Lsb:
        case Demod::Dsb:
        case Demod::Cw: return Levelling::Agc;
        case Demod::Nfm:
        case Demod::Wfm: return Levelling::Fixed;
        case Demod::Raw:
        case Demod::P25p1:
        case Demod::Dstar:
        case Demod::Tetra:
        case Demod::Dmr: return Levelling::None;
    }
    return Levelling::None;
}

// The peak level the AGC holds audio at: -12 dBFS, leaving the rest for
// several receivers summed under the client's limiter.
inline constexpr double kHeardTarget = 0.25;

// The most gain the AGC applies. 70 dB brings a -82 dBFS peak to the target;
// a channel with nothing on it comes up to that much noise, as on any radio
// with an AGC.
inline constexpr double kAgcCeilingDb = 70.0;

// An FM receiver's gain: full deviation, which dsp::vrx_demod_gain scales to
// +/-1, to kHeardTarget.
inline constexpr float kFmHeardGain = static_cast<float>(kHeardTarget);

// The time constants the engine accepts, in milliseconds. The floor keeps a
// constant from being shorter than a sample at any audio rate this engine
// runs, where the AGC stops following an envelope and follows the waveform,
// which is a clipper. The ceilings are a minute's decay and a second's
// attack, past which the control does nothing an operator could hear.
inline constexpr double kAgcAttackMinMs = 0.1;
inline constexpr double kAgcAttackMaxMs = 1'000.0;
inline constexpr double kAgcDecayMinMs = 1.0;
inline constexpr double kAgcDecayMaxMs = 60'000.0;

// The time constants, when the AGC is on. Checked by engine::place, which
// add_vrx and set_vrx_params both call before anything is queued, so a bad
// figure is refused with a sentence on the call rather than found on the
// recording thread where nobody hears it.
//
// Off is not checked, because nothing reads the figures then: a client that
// has never heard of these fields sends zeros and the AGC off, which is a
// receiver holding its gain.
[[nodiscard]] inline Status validate_agc_request(const VrxParams& params) {
    if (!params.agc_enabled) {
        return {};
    }
    const bool attack_ok = std::isfinite(params.agc_attack_ms) &&
                           params.agc_attack_ms >= kAgcAttackMinMs &&
                           params.agc_attack_ms <= kAgcAttackMaxMs;
    if (!attack_ok) {
        return fail(std::format("the AGC's attack of {} ms is outside {} to {} ms",
                                params.agc_attack_ms, kAgcAttackMinMs, kAgcAttackMaxMs));
    }
    const bool decay_ok = std::isfinite(params.agc_decay_ms) &&
                          params.agc_decay_ms >= kAgcDecayMinMs &&
                          params.agc_decay_ms <= kAgcDecayMaxMs;
    if (!decay_ok) {
        return fail(std::format("the AGC's decay of {} ms is outside {} to {} ms",
                                params.agc_decay_ms, kAgcDecayMinMs, kAgcDecayMaxMs));
    }
    return {};
}

// The three fields of VrxParams the AGC reads, snapshotted per dispatch.
struct AgcSettings {
    bool enabled = true;
    double attack_ms = 10.0;
    double decay_ms = 500.0;
};

[[nodiscard]] constexpr AgcSettings agc_settings_of(const VrxParams& params) {
    return AgcSettings{params.agc_enabled, params.agc_attack_ms, params.agc_decay_ms};
}

// A peak follower with a separate attack and decay, and a gain that brings
// the followed peak to kHeardTarget, never above kAgcCeilingDb.
//
// The follower moves toward each frame's peak across its channels by the
// attack coefficient when the peak is above it and by the decay coefficient
// when it is below, so a step in level moves it with exactly the time
// constant named: tests/engine/test_listener_level.cpp holds a constant
// magnitude against both. On a sinusoid the attack acts only near the crests,
// so the settled envelope sits a little under the crest and a rise takes a
// few attack times rather than one; tests/engine/test_engine_agc.cpp measures
// both on demodulated audio.
//
// One gain per frame for all its channels, so a stereo pair keeps its image.
//
// Completion thread only. The graph owns one per receiver and nothing else
// touches it.
class ListenerAgc {
public:
    // Switching the AGC back on does not step the gain: it moves from the
    // held gain to the AGC's over this long, so the switch is a fade of a
    // few milliseconds rather than a click at a block edge.
    static constexpr double kSwitchRampMs = 5.0;

    // Forgets the envelope, so the next open chunk primes it from its own
    // level. A receiver holding its gain keeps it.
    void restart() {
        primed_ = false;
        ramp_left_ = 0;
    }

    // `out` is `in` levelled, frame for frame. The two are the same size and
    // do not overlap. `open` is the squelch gate on this chunk.
    void process(std::span<const float> in, std::span<float> out, std::uint32_t channels,
                 dsp::SampleRate rate, bool open, const AgcSettings& settings) {
        const std::size_t count = std::min(in.size(), out.size());
        if (!open) {
            std::fill(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(count), 0.0F);
            return;
        }

        const std::size_t width = std::max<std::size_t>(channels, 1);
        const std::size_t frames = count / width;
        configure(rate, settings);

        if (!settings.enabled) {
            if (!have_gain_) {
                // A receiver that has never run its AGC takes the gain its
                // first block with a signal in it calls for. A block of pure
                // zeros is no guide and comes out as zeros whatever the gain.
                const double peak = peak_of(in, frames, frames, width);
                if (peak > 0.0) {
                    gain_ = kHeardTarget / std::max(peak, floor_);
                    have_gain_ = true;
                }
            }
            holding_ = true;
            primed_ = false;
            ramp_left_ = 0;
            for (std::size_t i = 0; i < width * frames; ++i) {
                out[i] = static_cast<float>(static_cast<double>(in[i]) * gain_);
            }
            return;
        }

        if (holding_) {
            // Switched back on. The envelope is re-primed below, and the gain
            // fades from where it was held to wherever that lands.
            holding_ = false;
            primed_ = false;
            if (have_gain_) {
                ramp_from_ = gain_;
                ramp_total_ = std::max<std::size_t>(ramp_frames_, 1);
                ramp_left_ = ramp_total_;
            }
        }
        if (!primed_) {
            envelope_ = peak_of(in, frames, attack_frames_, width);
            primed_ = true;
        }

        for (std::size_t i = 0; i < frames; ++i) {
            const float* frame = in.data() + i * width;
            double peak = 0.0;
            for (std::size_t c = 0; c < width; ++c) {
                peak = std::max(peak, std::abs(static_cast<double>(frame[c])));
            }
            envelope_ += (peak - envelope_) * (peak > envelope_ ? attack_ : decay_);
            gain_ = kHeardTarget / std::max(envelope_, floor_);

            double applied = gain_;
            if (ramp_left_ > 0) {
                const double left =
                    static_cast<double>(ramp_left_) / static_cast<double>(ramp_total_);
                applied = gain_ + (ramp_from_ - gain_) * left;
                --ramp_left_;
            }
            float* to = out.data() + i * width;
            for (std::size_t c = 0; c < width; ++c) {
                to[c] = static_cast<float>(static_cast<double>(frame[c]) * applied);
            }
        }
        have_gain_ = true;
    }

    // The gain the last frame took, before any switching fade.
    [[nodiscard]] double gain() const { return gain_; }

    [[nodiscard]] double envelope() const { return envelope_; }

    // True while the AGC is off and the gain is held.
    [[nodiscard]] bool holding() const { return holding_; }

private:
    void configure(dsp::SampleRate rate, const AgcSettings& settings) {
        if (rate == rate_ && settings.attack_ms == attack_ms_ && settings.decay_ms == decay_ms_) {
            return;
        }
        rate_ = rate;
        attack_ms_ = settings.attack_ms;
        decay_ms_ = settings.decay_ms;

        const double fs = static_cast<double>(std::max<dsp::SampleRate>(rate, 1));
        const double attack_s = std::max(settings.attack_ms, kAgcAttackMinMs) / 1000.0;
        const double decay_s = std::max(settings.decay_ms, kAgcDecayMinMs) / 1000.0;
        attack_ = 1.0 - std::exp(-1.0 / (attack_s * fs));
        decay_ = 1.0 - std::exp(-1.0 / (decay_s * fs));
        attack_frames_ = std::max<std::size_t>(static_cast<std::size_t>(attack_s * fs), 1);
        ramp_frames_ = static_cast<std::size_t>(kSwitchRampMs / 1000.0 * fs);
    }

    // The largest magnitude in the first `limit` frames.
    [[nodiscard]] static double peak_of(std::span<const float> in, std::size_t frames,
                                        std::size_t limit, std::size_t width) {
        const std::size_t upto = std::min(frames, limit) * width;
        double peak = 0.0;
        for (std::size_t i = 0; i < upto; ++i) {
            peak = std::max(peak, std::abs(static_cast<double>(in[i])));
        }
        return peak;
    }

    // Below this envelope the gain stops rising: kHeardTarget over the
    // ceiling.
    double floor_ = kHeardTarget / std::pow(10.0, kAgcCeilingDb / 20.0);

    dsp::SampleRate rate_ = 0;
    double attack_ms_ = -1.0;
    double decay_ms_ = -1.0;
    double attack_ = 1.0;
    double decay_ = 1.0;
    std::size_t attack_frames_ = 1;
    std::size_t ramp_frames_ = 0;

    double envelope_ = 0.0;
    double gain_ = 1.0;
    bool primed_ = false;
    bool have_gain_ = false;
    bool holding_ = false;

    double ramp_from_ = 1.0;
    std::size_t ramp_total_ = 1;
    std::size_t ramp_left_ = 0;
};

}  // namespace revenant::engine
