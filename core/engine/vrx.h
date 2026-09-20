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
//
// Every frequency in this header is in the source's baseband frame, hertz
// from wherever the source is tuned. See VrxParams::center, which is the one
// field a caller is likely to get wrong.

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
// Integer hertz throughout, which is why nothing here is a double: a
// frequency that has been through a float is a frequency nobody can source.
struct VrxParams {
    // Hertz from the SOURCE'S BASEBAND DC. Not an absolute radio frequency,
    // and bounded by plus and minus half the source rate. The field is called
    // "center" and that name says nothing about which frame the centre is in,
    // so it is said here: a caller holding an absolute frequency converts
    // with `center = absolute - EngineInfo::source_center`, and place()
    // rejects anything outside the span rather than wrapping it.
    //
    // THIS HEADER IS THE DOCUMENT THAT WAS WRONG. Until 2026-09-19 it called
    // the field an absolute radio frequency while place() read an offset,
    // and core/rpc/revenant.capnp, core/rpc/types.h and docs/detection.md
    // each cited that disagreement as a live one. Correcting it changed no
    // code, because there was never any rebasing to remove: a caller who
    // believed this header before today was tuning by the whole local
    // oscillator and the engine was doing exactly what it does now.
    //
    // Baseband is the API because it is the only frame the grid has. place()
    // is handed the grid, the rate and this struct, and is never told where
    // the source is tuned, so resolving an absolute frequency would mean
    // passing it the tuned centre, which is the one thing that stops it being
    // a pure function of the grid and the request. Rebasing a layer up inside
    // Engine::add_vrx is worse rather than better: VrxStatus carries this
    // same struct back out, so a subtraction on the way in with none on the
    // way out moves a receiver by the whole local oscillator every time
    // vrx_status() is fed back to set_vrx_params().
    //
    // What that costs is a receiver that does not follow a retuned source on
    // its own. No choice of frame for this field closes that: source_center
    // is read once when the source is opened, Engine exposes no tune at all,
    // and the day one arrives every receiver has to be re-placed and the ones
    // that fall outside the new span parked and said to be parked.
    // docs/ui-spectrum.md has the argument.
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

    // Exactly what was handed to add_vrx or set_vrx_params, in the same
    // frame: params.center is still baseband here. A display naming an
    // absolute frequency adds EngineInfo::source_center back.
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
