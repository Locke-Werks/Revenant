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

// The receiver's audio de-emphasis curve.
//
// THIS IS A DEFECT REPORT, NOT A FEATURE FLAG. Until 2026-09-20 nothing in
// the demodulation path applied any de-emphasis at all: a grep for deemph,
// emphasis or tau across core/shaders, core/dsp and core/engine returned
// nothing, and every broadcast FM reception this program had ever produced
// was several decibels too bright at the top of the audio band. On an
// operator's first real station it read as harsh and unclear speech rather
// than as distortion, which is why a year of synthetic testing did not find
// it: tests/decode/test_rds_bits.cpp renders a station, demodulates it and
// decodes RDS, and it passed throughout, because RDS rides 57 kHz and an
// audio curve never touches it.
//
// Broadcast FM boosts the top of the audio band at the transmitter so the
// discriminator's rising noise spectrum is cut back along with the boost at
// the receiver. A receiver that does not apply the matching cut delivers the
// boost. core/dsp/synth/wfm_mod.h has the transmit half and has had it since
// before this existed, which is how the two halves came to disagree.
//
// ONE GEOGRAPHY, TWO CONSUMERS. Us75 and Eu50 are the same split as RDS
// against RBDS, which core/decode/rds_groups.h already carries as
// decode::Region: Us75 goes with Region::kRbds, NRSC-4-B and ITU region 2,
// and Eu50 goes with Region::kRds, EN 50067 and ITU regions 1 and 3. A
// client that has already asked an operator which region they are in has
// already answered this question and must not ask a second time. This header
// does not include core/decode to say so: core/engine reaches into
// core/decode nowhere else and one enumerator pair is not worth the edge.
//
// None is a legitimate setting and not an off switch for something broken. A
// composite tap feeding an RDS decoder must not be de-emphasised, and neither
// must a measurement; see the note on Default below for which paths can
// reach a curve at all.
enum class Deemphasis : std::uint8_t {
    // "I did not say", answered by the mode's own channel plan, exactly as
    // VrxParams::bandwidth of zero is answered by default_passband. It is
    // the zero value so a caller that never heard of this field gets the
    // right curve rather than none.
    Default = 0,

    None,

    // 75 microseconds. North America, 47 CFR 73.333.
    //
    // NOT OPENED FOR THIS WORK. The rule prints the curve as a figure rather
    // than as a time constant; 75 us is the constant that curve is
    // universally quoted as and is what is implemented. docs/clean-room.md
    // asks a citation to name a document somebody read, so this one names
    // the document and says plainly that nobody here read it.
    // core/dsp/synth/wfm_mod.h states the transmit side on the same terms.
    Us75,

    // 50 microseconds. Europe and most of the rest of the world, ITU-R
    // BS.450. NOT OPENED FOR THIS WORK, on the same terms as Us75.
    Eu50,
};

[[nodiscard]] constexpr const char* deemphasis_name(Deemphasis curve) {
    switch (curve) {
        case Deemphasis::Default: return "default";
        case Deemphasis::None: return "none";
        case Deemphasis::Us75: return "75us";
        case Deemphasis::Eu50: return "50us";
    }
    return "unknown";
}

// Inline here rather than beside demod_from_name in core/engine/vrx_place.cpp
// because it needs nothing that file has. Same spellings the transmit side
// accepts in siggen::preemphasis_from_name, so a test that renders a station
// and receives it names the curve once.
[[nodiscard]] inline Expected<Deemphasis> deemphasis_from_name(std::string_view name) {
    if (name == "default") return Deemphasis::Default;
    if (name == "none" || name == "off") return Deemphasis::None;
    if (name == "75us" || name == "us") return Deemphasis::Us75;
    if (name == "50us" || name == "eu") return Deemphasis::Eu50;
    return fail(std::string("no de-emphasis curve is called '") + std::string(name) +
                "'. The curves are default, none, 75us and 50us");
}

// The audio rate at and above which a WFM receiver is handing out the
// MULTIPLEX rather than programme audio, and so takes no curve by default.
//
// Twice the 57 kHz RDS subcarrier. Below it the subcarrier cannot be
// represented at all and whatever comes out is programme audio; at or above
// it the receiver is a composite tap, which is what core/decode/rds_bits.h
// is fed and what tools/cli --rds opens at 171000 S/s.
//
// THIS BOUND IS WHY THE FIRST ON-AIR RDS DECODE STILL WORKS. De-emphasising
// a composite pulls the data band down 28.6 dB at 75 us and tilts it by
// 1.6 dB across its own width, and the decoder would report a quality
// figure for whatever survived rather than faulting. The number is spelled
// out here rather than taken from decode::kSubcarrierHz because core/engine
// depends on core/decode nowhere and one constant is not worth the edge;
// core/decode/rds_bits.h is the definition and this is a copy that says so.
inline constexpr dsp::SampleRate kCompositeAudioRateHz = 114'000;

// The curve a receiver actually runs: the request, or the mode's own when
// the request is Default.
//
// The switch is over Demod with every enumerator spelled out and no default
// label, which is the shape /w14062 diagnoses, for the same reason
// dsp::fm_deviation, dsp::minimum_demod_rate and dsp::vrx_demod_gain are
// written that way. None is correct for seven of the eight modes, which is
// exactly what makes it dangerous to hand over by omission: a ninth mode in
// the FM family would inherit a flat response and sound wrong in a way
// nothing measures.
//
// audio_rate of zero means the receiver took the engine's default, which is
// programme audio, so zero and 48000 answer the same.
//
// Here in core/engine rather than in core/dsp because VrxStatus has to call
// it and core/dsp already depends on core/engine, so the other direction is
// a cycle.
[[nodiscard]] constexpr Deemphasis resolve_deemphasis(Demod mode, Deemphasis requested,
                                                      dsp::SampleRate audio_rate) {
    if (requested != Deemphasis::Default) {
        // Raw is complex baseband at the receiver's bandwidth and is not
        // audio, so there is no curve to apply and a request for one is not
        // refused, it is simply not a thing the raw tap has. Making it a
        // refusal would mean a client that sets a curve once and sweeps
        // modes gets an error on one of them.
        //
        // An EXPLICIT curve on a composite tap is honoured, unlike the
        // default below. Explicit is explicit, and a request refused for
        // being unwise is a request the operator cannot make; what stops it
        // being a trap is that nothing has to make it, because Default
        // already gives that receiver the right answer.
        return (mode == Demod::Raw) ? Deemphasis::None : requested;
    }

    switch (mode) {
        // North America, because that is where the radio is. An operator
        // elsewhere sets Eu50 the same way they set Region::kRds.
        case Demod::Wfm:
            return (audio_rate >= kCompositeAudioRateHz) ? Deemphasis::None
                                                         : Deemphasis::Us75;

        // Nfm is the one that looks like it should be here and is not. Land
        // mobile does run a 750 microsecond curve, but nothing in this tree
        // transmits one, so a de-emphasis nobody has ever measured against a
        // matching transmitter would be a guess shipped as a correction. It
        // is requestable and it is not a default.
        //
        // NFM is also the other half of the composite tap: core/rpc's RDS
        // surface admits an NFM receiver at 171000 beside a WFM one, because
        // both reach the same atan2 and only the gain differs. A default
        // curve here would have broken that path too.
        case Demod::Nfm:
        case Demod::Raw:
        case Demod::Am:
        case Demod::Usb:
        case Demod::Lsb:
        case Demod::Dsb:
        case Demod::Cw: return Deemphasis::None;
    }

    // Not an enumerator at all. Nothing is known about the mode, so nothing
    // is known about its channel plan.
    return Deemphasis::None;
}

// The curve's time constant in seconds, and zero for None and Default.
//
// Default is zero rather than 75 microseconds on purpose: everything that
// designs a filter goes through resolve_deemphasis first, so a Default
// arriving here is a caller that skipped it, and a flat response is the
// answer that makes that visible rather than the one that hides it.
[[nodiscard]] constexpr double deemphasis_seconds(Deemphasis curve) {
    switch (curve) {
        case Deemphasis::Us75: return 75.0e-6;
        case Deemphasis::Eu50: return 50.0e-6;
        case Deemphasis::None:
        case Deemphasis::Default: return 0.0;
    }
    return 0.0;
}

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

    // The audio de-emphasis curve, or Default to take the mode's own.
    //
    // Default resolves to Us75 for Wfm and to None for every other mode,
    // including Nfm: land mobile does run a 750 us curve, but nothing in
    // this tree transmits one and a de-emphasis nobody measured is worse
    // than none. dsp::resolve_deemphasis is the one place that decides, and
    // it switches over Demod with no default label so a ninth mode has to
    // say what it wants.
    //
    // Raw is not audio and never carries a curve whatever this says. That
    // matters concretely: tools/cli --rds taps the 171000 S/s composite
    // through the raw mode and decoded a real station on 2026-09-20. A curve
    // on that path would lift the 57 kHz subcarrier by 28.6 dB at 75 us and
    // the decoder would report a clean eye while every sensitivity figure
    // measured against it was optimistic by that much.
    Deemphasis deemphasis = Deemphasis::Default;

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

    // The de-emphasis curve this receiver is ACTUALLY running, which
    // Default never is: Default is a question and this is the answer. A
    // client that sent Default and reads Us75 back is being told which curve
    // it got rather than being left to work it out from the sound, which is
    // the whole complaint this exists to answer.
    //
    // A FUNCTION AND NOT A FIELD, DELIBERATELY. Every other result on this
    // struct is written by core/engine/graph.cpp, and a field here would be
    // one more thing that file has to remember: a receiver whose curve
    // changed and whose status still said Us75 would be the same silence
    // this whole change is about. The answer is a pure function of two
    // fields that are already echoed verbatim above, so there is nothing to
    // forget and nothing to get stale.
    [[nodiscard]] constexpr Deemphasis applied_deemphasis() const {
        return resolve_deemphasis(params.demod, params.deemphasis, params.audio_rate);
    }

    // Signal level in the passband, dBFS, updated per block. This is what
    // drives the meter in the receiver rack.
    double level_dbfs = -200.0;

    bool squelch_open = false;

    // The tuning epoch this receiver will be at once every retune QUEUED so
    // far has been applied, which is the number of retunes the control plane
    // has accepted for it. Zero for a receiver that has never been retuned.
    //
    // THIS IS THE TARGET AudioChunk::tuning_epoch CONVERGES UP TO, and it is
    // the only honest way for a consumer to fence its own state against a
    // retune. set_vrx_params only queues a control op, so a consumer that
    // clears itself when the call returns is then handed the old tuning's
    // frames, which is worse than not clearing at all. It reads this number
    // after the call and discards every chunk carrying less.
    //
    // A TARGET AND NOT A COUNT OF EVENTS TO WAIT FOR, which is the whole
    // reason it is here. core/rpc/server.cpp used to count the retunes it
    // had asked for and take one off per observed change of chunk epoch.
    // Two retunes applied in one control drain move the epoch by two and
    // produce ONE change, so the count never reached zero and that consumer
    // discarded for the rest of the run, silently. A comparison cannot go
    // wrong that way: it is right however many retunes land in one drain,
    // and it is right when the epoch a chunk would have carried lands on a
    // frame that produced no samples and so was never delivered.
    //
    // Read under the same lock as `params`, so it is at least as new as the
    // retune the caller just queued.
    std::uint64_t tuning_epoch = 0;

    // Audio FRAMES this receiver produced, and frames it produced that
    // reached nothing.
    //
    // audio_dropped is the engine's own loss and only the engine's: frames
    // this receiver produced that this engine could not deliver. Two sites
    // add to it and both end the run, so it is normally zero and a non-zero
    // value means the run is on its way out rather than that a listener
    // missed a syllable. A sink was handed the chunk and refused it, which
    // core/engine/graph.cpp turns into a failing dispatch; or the frames
    // never reached the host at all, because the readback out of the mapped
    // region failed and there was nothing to hand anybody.
    //
    // WHAT THIS PARAGRAPH USED TO SAY. Until 2026-09-20 it read "frames
    // handed to this receiver's sink and refused by it. That is the whole of
    // it". It was not: the readback failure path moved neither this counter
    // nor the stream index, so the next chunk carried the index the lost
    // frames should have had and the hole in AudioChunk::start closed over
    // itself silently.
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
