// A virtual receiver.
//
// A VRX owns a centre frequency, a bandwidth, a filter shape, a demodulator,
// AGC and squelch settings, and an audio sink. It has an identity, a colour
// and a label, and it survives a session.
//
// WHAT THIS SENTENCE USED TO SAY. Until 2026-09-20 it ended "an audio sink
// and an optional decoder chain". There is no decoder chain. Nothing in
// core/engine builds, holds or feeds a decoder: no field on this struct, no
// stage in core/engine/vrx_stage.cpp, no hook in core/engine/graph.cpp. The
// decoders live in core/decode and are driven by whatever has PCM, which
// today is a caller holding an AudioSink.
//
// Retracted rather than built, which is a decision and not a shrug. A seam
// invented here would be a second one: core/rpc/revenant.capnp already
// states that the RDS surface installs a decoder on a receiver's audio when
// that branch lands, and two decoder seams in core/engine means the one
// written first gets deleted. Engine::attach_audio_sink is the composition
// point a decoder will attach through, and it exists now, so the sentence
// this replaces was describing something the tree can do rather than
// something it has.
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

    // Passband width, and a SHORTHAND rather than the request itself. The
    // fine stage resamples to whatever this needs, so it is not constrained
    // to the grid's channel spacing.
    //
    // WHAT THIS FIELD USED TO MEAN. Until this change it was the whole
    // passband request and every derived quantity read it as a half-width
    // either side of `center`. That reading was never true for all eight
    // modes: plan_vrx already put USB's passband at [center, center + B] and
    // LSB's at [center - B, center], so a caller that set a bandwidth and
    // expected a filter centred on the tuned frequency was already not
    // getting one on those two modes, and had no way to say what it did
    // want.
    //
    // It is now the input the mode's shorthand rule is expanded from when
    // passband_low and passband_high are both zero, and that expansion
    // reproduces the old geometry exactly for every mode:
    // dsp::resolve_passband is the one place it happens. It is IGNORED when
    // the pair is given.
    //
    // ON THE WAY BACK OUT IT IS THE ECHO, NOT THE GRANTED WIDTH. THIS
    // PARAGRAPH SAID THE OPPOSITE AND THE CODE NEVER DID IT. It claimed the
    // field came back as "the granted width, high minus low, so a caller
    // that only knows about this field gets the right width for an
    // asymmetric filter and loses only the offset". VrxStatus hands back the
    // params it was given, verbatim, so what a reader gets here is whatever
    // it sent: 12000 if it sent 12000, and zero if it sent a pair.
    //
    // The echo is right and stays. Reading a status out and passing it
    // straight back into set_vrx_params has to leave the receiver exactly
    // where it was, and it cannot if the engine rewrites a field the request
    // is resolved from: a client that sent edges and read back a bandwidth
    // would, on the next round trip, still be sending its edges and would be
    // fine, but a client that sent a bandwidth and was handed the CLAMPED
    // one back would ratchet its own receiver narrower on every poll.
    // core/rpc/revenant.capnp says the same thing about the wire field.
    //
    // So the granted width is VrxPlacement::granted_high minus granted_low,
    // which is the figure the engine built a filter for, and comparing that
    // pair against the requested one is how a display says WHICH edge moved.
    //
    // Kept rather than deleted because a saved request from before the pair
    // existed should still open, and because retiring the field would free
    // its wire ordinal for something else to reuse later.
    dsp::Hertz bandwidth = 12'000;

    // The passband, as signed hertz from `center`, with low strictly below
    // high. Both zero means "not stated" and `bandwidth` is expanded
    // instead.
    //
    // Two numbers because a width cannot say where the band sits, and where
    // it sits is most of what a receiver's filter is. USB is the carrier
    // plus 300 to plus 2700 and has nothing at the carrier; a transceiver
    // running 20 kHz of transmit audio wants the edges pulled out and stays
    // anchored on the same carrier while it happens. Neither is a width.
    //
    // The frame is `center` for every mode with no exceptions, so the filter
    // passes [center + passband_low, center + passband_high]. CW's sidetone
    // is NOT in here: the pitch translates what the fine stage mixes to DC
    // and leaves the filter where it is, so a CW window offset from the
    // carrier is stated as offset edges and the pitch stays the separate
    // thing it is.
    //
    // dsp::default_passband has the per-mode defaults a client applies when
    // the operator has not moved the edges.
    dsp::Hertz passband_low = 0;
    dsp::Hertz passband_high = 0;

    Demod demod = Demod::Nfm;

    // Audio output rate. 0 takes the engine's default.
    dsp::SampleRate audio_rate = 0;

    // Squelch threshold in dB relative to full scale. Below this the audio
    // output is muted rather than the receiver being stopped, so the meter,
    // the passband display and the sample index all keep running: a gate is a
    // thing the operator hears and not a thing that stops a receiver.
    //
    // WHAT THIS COMMENT USED TO SAY: "because a squelched receiver still
    // feeds its decoder chain and its signal meter". The meter half is true
    // and core/engine/graph.cpp stores the level before it mutes. The decoder
    // half was never true twice over: there is no decoder chain, per the
    // retraction at the top of this file, and the mute is written into the
    // shared readback buffer before any sink is called, so a consumer wanting
    // ungated audio could not have one. It went unnoticed because the default
    // below never opens the gate's other side: -200 dBFS is under the
    // arithmetic's own floor, so every receiver built from the defaults is
    // permanently open and nothing is ever muted.
    //
    // A consumer that needs to tell the gate from a dead band reads
    // AudioChunk::squelch_open, which is on every chunk for that reason.
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

    // The passband the receiver was actually given, in the same frame
    // VrxParams::passband_low and passband_high are in: signed hertz from
    // params.center.
    //
    // Equal to the resolved request unless the channel could not carry it.
    // Each edge is fitted on its own, so a request too wide on one side
    // keeps the other edge where it was; a display draws the granted pair
    // and the requested pair in two shades and the difference is the clamp.
    dsp::Hertz granted_low = 0;
    dsp::Hertz granted_high = 0;

    // True when EITHER edge was pulled in to fit one grid channel. The
    // caller is told rather than quietly receiving less than it asked for.
    //
    // WHAT THIS FIELD USED TO MEAN: "the requested bandwidth did not fit and
    // the receiver was given the widest that does", which was one width
    // against one limit. The clamp is per edge now, so this says only that
    // something moved and the granted pair above says what.
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
    //
    // A VERBATIM ECHO, WHICH MEANS NO FIELD HERE IS A RESULT. Nothing in
    // this struct is rewritten with what the engine decided, deliberately,
    // so that reading a status and passing it back into set_vrx_params
    // leaves the receiver where it was. In particular `bandwidth` reads
    // back as whatever was sent, including zero, and is not the width the
    // filter was built for. `placement` below is where the results are:
    // granted_low, granted_high and bandwidth_clamped, plus demod_rate
    // here.
    VrxParams params;

    VrxPlacement placement;

    // The rate the fine stage resampled to, which is what the passband
    // display's axis spans and what decides whether a change to this
    // receiver can be applied in place.
    //
    // Here because a client cannot derive it: it is minimum_demod_rate
    // rounded up to a multiple of the audio rate, and the audio rate a
    // receiver ended up with is the engine's default when the request named
    // none. A retune that changes it is a remove and an add, so a surface
    // dragging the passband edges reads this to tell a change it can send
    // live from one that will break the audio.
    dsp::SampleRate demod_rate = 0;

    // Signal level in the passband, dBFS, updated per block. This is what
    // drives the meter in the receiver rack.
    double level_dbfs = -200.0;

    bool squelch_open = false;

    // Audio FRAMES this receiver produced, and frames it produced that
    // reached nothing.
    //
    // audio_dropped is the engine's own loss and only the engine's: frames
    // handed to this receiver's sink and refused by it. That is the whole of
    // it, and it is normally zero, because core/engine/graph.cpp turns a
    // refusing sink into a failing dispatch and ends the run. A non-zero
    // value here means the run is on its way out, not that a listener missed
    // a syllable.
    //
    // WHAT THIS PAIR USED TO SAY, AND WHY THE NARROWER READING IS THE HONEST
    // ONE. Until 2026-09-20 the comment read "audio samples produced and
    // dropped. A drop is a dropout the operator hears", and the only site
    // that incremented it was the squelch mute, which is not a dropout at
    // all: the chunk is delivered at the full rate with the index unbroken
    // and AudioChunk::squelch_open saying why it is silent. So the field
    // named one thing and counted another.
    //
    // It is also not the number a listener wants, and no field on this struct
    // can be. A drop belongs to a CONSUMER: a recording's disk stalling and a
    // remote subscriber's socket stalling are two different losses on one
    // receiver, and there is one field here to put them in. They are counted
    // where they happen, in AudioEgressStats::frames_dropped and in the
    // AudioStats a subscription answers with.
    //
    // They are frames rather than interleaved samples, which the old name
    // "audio_samples" also got wrong: a stereo stream counted in samples
    // reports every instant twice. core/engine/graph.cpp has always added
    // StageOutput::frames here.
    std::uint64_t audio_samples = 0;
    std::uint64_t audio_dropped = 0;
};

}  // namespace revenant::engine
