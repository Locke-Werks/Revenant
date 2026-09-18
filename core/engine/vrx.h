// A virtual receiver.
//
// A VRX owns a centre frequency, a bandwidth, a filter shape, a demodulator,
// AGC and squelch settings, an audio sink and an optional decoder chain. It
// has an identity, a colour and a label, and it survives a session.
//
// There is no "main" receiver and the first VRX is not privileged. Every other
// SDR application treats multi-VFO as an accessory to a primary tuner and the
// entire interface inherits that assumption; the whole point of a channelizer
// that makes channels nearly free is that the assumption stops being necessary.
// Nothing in this header knows which VRX was created first.
//
// A VRX attaches to the nearest coarse grid channel and does its own fine
// mixing, filtering and resampling from there. Adding one must not disturb the
// grid: no field here appears in GridParams, and that is deliberate rather
// than incidental.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/dsp/pfb.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::engine {

// v1 demodulators. The raw tap is not a demodulator and is listed here because
// it is selected the same way: it hands out complex baseband at the VRX's
// bandwidth, which is what lets external tooling and the decoder framework
// attach before any decoder exists.
enum class Demod : std::uint8_t { Raw, Am, Nfm, Wfm, Usb, Lsb, Dsb, Cw };

[[nodiscard]] constexpr const char* demod_name(Demod mode) {
    switch (mode) {
        case Demod::Raw: return "raw";
        case Demod::Am: return "am";
        case Demod::Nfm: return "nfm";
        case Demod::Wfm: return "wfm";
        case Demod::Usb: return "usb";
        case Demod::Lsb: return "lsb";
        case Demod::Dsb: return "dsb";
        case Demod::Cw: return "cw";
    }
    return "unknown";
}

[[nodiscard]] Expected<Demod> demod_from_name(std::string_view name);

// True when the mode produces real audio rather than complex baseband.
[[nodiscard]] constexpr bool produces_audio(Demod mode) { return mode != Demod::Raw; }

struct VrxId {
    std::uint32_t value = 0;

    [[nodiscard]] constexpr bool valid() const { return value != 0; }
    friend constexpr bool operator==(VrxId, VrxId) = default;
};

// What a receiver is tuned to and how it is configured.
//
// Frequency is absolute and in integer hertz, not an offset from the grid
// channel. A receiver that stored an offset would silently retune itself when
// the source's centre moved, which is exactly the class of bug integer hertz
// exists to make impossible.
struct VrxParams {
    dsp::Hertz center = 0;

    // Passband width. The fine stage resamples to whatever this needs, so it
    // is not constrained to the grid's channel spacing.
    dsp::Hertz bandwidth = 12'000;

    Demod demod = Demod::Nfm;

    // Audio output rate. 0 takes the engine's default.
    dsp::SampleRate audio_rate = 0;

    // Squelch threshold in dB relative to full scale. Below this the audio
    // output is muted rather than the receiver being stopped, because a
    // squelched receiver still feeds its decoder chain and its signal meter.
    double squelch_dbfs = -200.0;

    double agc_attack_ms = 10.0;
    double agc_decay_ms = 500.0;
    bool agc_enabled = true;

    // CW only. The offset the carrier is translated to so it is audible.
    dsp::Hertz cw_pitch = 700;
};

// How a receiver was actually placed on the grid, which the caller needs to
// see rather than infer.
struct VrxPlacement {
    // Coarse channel this receiver reads.
    std::uint32_t channel = 0;

    // The grid channel's exact centre, as a rational, never rounded. See
    // dsp::ChannelCentre: k*rate/M is frequently not a whole hertz, and
    // rounding it here would introduce a tuning offset nobody could source.
    dsp::ChannelCentre channel_centre{};

    // Residual the fine stage must mix out: the requested centre minus the
    // channel centre. Carried as a rational for the same reason.
    std::int64_t residual_numerator = 0;
    std::int64_t residual_denominator = 1;

    // Rate the channel arrives at, which is the source rate over the grid's
    // decimation.
    dsp::SampleRate channel_rate = 0;

    // True when the requested bandwidth does not fit in one grid channel and
    // the receiver was given the widest that does. The caller is told rather
    // than quietly receiving less than it asked for.
    bool bandwidth_clamped = false;
};

// Work out which channel a receiver attaches to and what the fine stage has to
// undo. Pure: it depends on the grid and the request and nothing else.
[[nodiscard]] Expected<VrxPlacement> place(const dsp::GridParams& grid, dsp::SampleRate rate,
                                            const VrxParams& params);

// What a receiver reports about itself while running.
struct VrxStatus {
    VrxId id;
    VrxParams params;
    VrxPlacement placement;

    // Signal level in the passband, dBFS, updated per block. This is what
    // drives the meter in the receiver rack.
    double level_dbfs = -200.0;

    bool squelch_open = false;

    // Audio samples produced and dropped. A drop is a dropout the operator
    // hears, so it is counted and never merely logged.
    std::uint64_t audio_samples = 0;
    std::uint64_t audio_dropped = 0;
};

}  // namespace revenant::engine
