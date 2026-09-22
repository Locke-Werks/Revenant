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

// For ModulationFamily alone, which SignalEvidence carries. catalogue.h
// pulls in nothing of this project's, so the engine does not acquire a
// dependency on the characteriser's estimators by naming its vocabulary.
#include "core/characterise/catalogue.h"
#include "core/dsp/pfb.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::engine {

// v1 demodulators. The raw tap is not a demodulator and is listed here because
// it is selected the same way: it hands out complex baseband at the VRX's
// bandwidth, which is what lets external tooling and the decoder framework
// attach before any decoder exists.
// The three digital voice modes are APPENDED, never inserted. Their ordinals
// cross the wire and reach a specialization constant, and core/rpc/convert.h
// asserts every pair; a reorder would retune every receiver in a saved
// session and nothing about the failure would point at this line.
//
// DMR is deliberately absent from a list that names its three neighbours.
// ETSI TS 102 361 carries a live Motorola patent whose claim 1 receives a
// burst and compares its synchronisation pattern, which is what a framing
// decoder does. docs/modes.md has the query and the reasoning, and the
// exclusion lifts on 2027-02-19 when that patent expires.
enum class Demod : std::uint8_t {
    Raw,
    Am,
    Nfm,
    Wfm,
    Usb,
    Lsb,
    Dsb,
    Cw,
    P25p1,
    Dstar,
    Tetra,
};

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
        case Demod::P25p1: return "p25p1";
        case Demod::Dstar: return "dstar";
        case Demod::Tetra: return "tetra";
    }
    return "unknown";
}

[[nodiscard]] Expected<Demod> demod_from_name(std::string_view name);

// True when the mode hands out complex baseband rather than real audio.
//
// The raw tap was the only one of these until the digital voice modes
// arrived, and they join it rather than getting a kernel of their own. What
// each of them needs from the receiver is a channel filter of the right width
// and a sample rate above twice its symbol rate; the recovery, the framing
// and the metadata are pure functions of a span of complex samples and live
// in core/decode. A kernel that discriminated for them would be doing work
// core/decode/dv_phy.cpp has to do anyway, and doing it at the wrong rate:
// P25 wants its discriminator after the receive filter, not before.
//
// The consequence a caller has to know is that these four produce two floats
// per sample and no audio. produces_audio below is the predicate for that and
// this is the predicate for the tap path.
[[nodiscard]] constexpr bool is_complex_tap(Demod mode) {
    switch (mode) {
        case Demod::Raw:
        case Demod::P25p1:
        case Demod::Dstar:
        case Demod::Tetra: return true;
        case Demod::Am:
        case Demod::Nfm:
        case Demod::Wfm:
        case Demod::Usb:
        case Demod::Lsb:
        case Demod::Dsb:
        case Demod::Cw: return false;
    }
    return false;
}

// True when the mode produces real audio rather than complex baseband.
[[nodiscard]] constexpr bool produces_audio(Demod mode) { return !is_complex_tap(mode); }

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
// audio_rate is the rate the receiver RUNS at and never the zero a request
// carries for "the engine's default". Resolve it first: the graph does that
// in with_audio_rate before it plans anything, and VrxStatus carries the
// resolved value beside the echo so a caller has it without asking.
//
// WHAT THIS PARAGRAPH USED TO SAY: "audio_rate of zero means the receiver
// took the engine's default, which is programme audio, so zero and 48000
// answer the same". EngineConfig::audio_rate is a field, so the default is
// not always programme audio, and on an engine configured at 171000 a zero
// here answered Us75 for a composite tap.
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
        return is_complex_tap(mode) ? Deemphasis::None : requested;
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
        case Demod::Cw:

        // The three digital modes carry no analogue audio at all, so there is
        // no curve to apply and no transmitter that applied one.
        case Demod::P25p1:
        case Demod::Dstar:
        case Demod::Tetra: return Deemphasis::None;
    }

    // Not an enumerator at all. Nothing is known about the mode, so nothing
    // is known about its channel plan.
    return Deemphasis::None;
}

// Whether this receiver decodes FM stereo.
//
// WHAT WAS THERE BEFORE. Nothing. Until 2026-09-20 the WFM branch took the
// discriminator's sum channel and stopped, so every broadcast FM reception
// was mono while the 19 kHz pilot and the 38 kHz difference channel sat in
// the composite untouched. core/dsp/synth/wfm_mod.cpp had been generating
// both since earlier the same day, which is to say the transmitter for the
// test existed before the receiver did.
//
// True only for a WFM receiver delivering programme audio, on the same
// predicate as the de-emphasis curve above, and for the same reason: a
// composite tap's whole job is to hand the multiplex out intact, and
// matrixing it into two channels would take the pilot and the data with it.
//
// It is a REQUEST and not a promise, because the transmitter decides. A
// station with no pilot cannot be decoded in stereo by anything, so the
// kernel gates the difference channel on the pilot's own level and hands out
// the sum channel in both. What it does NOT do is hide that: the gate
// multiplies by zero rather than fading, so L and R come back bit-identical
// on every sample, which a consumer can test for exactly rather than
// threshold. core/shaders/vrx_demod.comp states the floor and the figures.
[[nodiscard]] constexpr bool resolve_stereo(Demod mode, bool requested,
                                            dsp::SampleRate audio_rate) {
    if (!requested) {
        return false;
    }
    switch (mode) {
        case Demod::Wfm: return audio_rate < kCompositeAudioRateHz;

        // Not a refusal for the other seven. Nothing else in the tree
        // carries a pilot-tone multiplex, and a client that sets this once
        // and sweeps modes should not get an error on the other seven.
        case Demod::Raw:
        case Demod::Am:
        case Demod::Nfm:
        case Demod::Usb:
        case Demod::Lsb:
        case Demod::Dsb:
        case Demod::Cw:
        case Demod::P25p1:
        case Demod::Dstar:
        case Demod::Tetra: return false;
    }
    return false;
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
    // its own, and the day that costs something has arrived.
    // Engine::set_source_center moves the front end and writes the landed
    // centre into EngineInfo::source_center, so this field's frame moves
    // under a receiver nobody told. The engine re-places every receiver with
    // the params it already held, which keeps its BASEBAND offset and
    // therefore slides its absolute frequency by the whole retune. Nothing
    // falls out of the span doing it, because the span is baseband too and
    // the retune does not move it. Holding an absolute frequency across a
    // retune is the client's job, and it is the client that knows whether the
    // operator meant the receiver to follow the radio or stay where it was.
    //
    // WHAT THIS PARAGRAPH USED TO SAY. Until 2026-09-21 it read
    // "source_center is read once when the source is opened, Engine exposes
    // no tune at all, and the day one arrives every receiver has to be
    // re-placed and the ones that fall outside the new span parked and said
    // to be parked". The tune call landed in 4463967 and
    // core/engine/engine.h was corrected then; this was not, so a reader
    // working out how to hold a receiver's frequency was told there was
    // nothing here to follow. docs/ui-spectrum.md has the argument.
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
    // Raw is not audio and never carries a curve whatever this says.
    //
    // The composite tap is the case that matters and it is NOT the raw mode.
    // tools/cli --rds builds an ordinary Wfm receiver at
    // decode::RdsBitsConfig::rate, 171000 S/s, with the mode's own 200 kHz
    // passband, and decoded a real station on 2026-09-20. What keeps the
    // curve off that path is kCompositeAudioRateHz above: resolve_deemphasis
    // answers None for a Wfm receiver at or above 114000, because at that
    // rate the receiver is handing out the multiplex rather than programme
    // audio. A curve there would lift the 57 kHz subcarrier by 28.6 dB at
    // 75 us and the decoder would report a clean eye while every sensitivity
    // figure measured against it was optimistic by that much.
    //
    // WHAT THIS PARAGRAPH USED TO SAY. Until 2026-09-21 it read "tools/cli
    // --rds taps the 171000 S/s composite through the raw mode". It does
    // not, and it could not: the raw tap is one coarse channel copied out
    // of the channel ring with no fine mixing and no resampling, so it has
    // no audio rate to set to 171000 and produces complex baseband rather
    // than the real composite core/decode/rds_bits.h is fed. The sentence
    // credited the wrong branch of resolve_deemphasis with the decode, so a
    // reader changing the composite bound would have looked at Raw and
    // found nothing to change.
    Deemphasis deemphasis = Deemphasis::Default;

    // Decode FM stereo when the mode and the rate allow it, which
    // engine::resolve_stereo decides. On by default, because a broadcast
    // station in mono is the defect and not the safe option; a receiver
    // that wants one channel says so, and gets the sum channel, which is
    // what mono has always meant on this band.
    //
    // The output is then TWO interleaved floats per frame and
    // AudioChunk::channels says so. A consumer that assumes one channel
    // reads a stereo stream as mono at double speed, which is why this is
    // stated on the chunk rather than inferred from the mode.
    //
    // THE DOCUMENT THIS FIELD MADE FALSE IS CORRECTED WHERE IT IS READ.
    // core/rpc/revenant.capnp's AudioChunk described channelCount as always
    // reading 1, which stopped being true the day this landed. The lane that
    // broke it could not edit the schema and left the replacement text here;
    // the schema carries it now, with its own retraction naming the commit,
    // and this header no longer holds a second copy. A correction filed
    // where it was discovered rather than where it is read is how a schema
    // goes on lying to every client author for a day.
    bool stereo = true;

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
//
// REFUSES RATHER THAN CLAMPS WHERE THE CLAMP WOULD BREAK THE DEMODULATOR.
// See clamp_breaks_demodulator below and the long note in
// core/engine/vrx_place.cpp on what the grid can and cannot be asked to do
// once a source is open. Every other clamp still comes back as a granted
// pair with bandwidth_clamped set, which is what the linear modes want.
[[nodiscard]] Expected<VrxPlacement> place(const dsp::GridParams& grid, dsp::SampleRate rate,
                                            const VrxParams& params);

// Whether narrowing this mode's passband changes what the demodulator
// produces rather than how much of it.
//
// The FM modes are the two where it does. A discriminator recovers the
// instantaneous frequency of everything that reaches it, so a truncated
// passband does not hand back a narrower version of the same audio: it
// hands back different audio, and the operator hears distortion at full
// strength rather than a quiet signal. Every other mode here is linear in
// its passband, so less bandwidth is less audio bandwidth and nothing is
// broken.
//
// Raw is grouped with the honest ones because it is not a demodulator: a
// narrower raw tap is a narrower raw tap, and whatever is reading it knows
// what to do about that.
//
// The digital modes are grouped there for the same reason. Nothing in the
// engine demodulates them; the fine stage hands complex baseband to
// core/decode, so a clamped passband costs intersymbol interference rather
// than a different signal, and the decoder reports that as a bit error rate
// instead of hiding it.
//
// No default case. A twelfth demodulator has to answer this question here
// rather than inherit an answer, which is the same rule
// dsp::default_passband states for its own table.
[[nodiscard]] constexpr bool clamp_breaks_demodulator(Demod mode) {
    switch (mode) {
        case Demod::Nfm:
        case Demod::Wfm: return true;
        case Demod::Raw:
        case Demod::Am:
        case Demod::Usb:
        case Demod::Lsb:
        case Demod::Dsb:
        case Demod::Cw:
        case Demod::P25p1:
        case Demod::Dstar:
        case Demod::Tetra: return false;
    }
    return false;
}

// What the detector and, when one has been run, the characteriser have to
// say about a signal somebody clicked on.
//
// Two fields and not one because they come from different stages and
// either can be absent. core/detect publishes an occupied bandwidth per
// track and always has one; core/characterise reads complex baseband and is
// not wired into the engine at all, so `family` is Unknown on every path
// that exists today and is here so that the path which does run it has
// somewhere to put the answer rather than a second decision rule.
struct SignalEvidence {
    // Occupied bandwidth in hertz, as detect::Track measures it. Zero or
    // negative means nothing was measured.
    dsp::Hertz occupied_hz = 0;

    characterise::ModulationFamily family = characterise::ModulationFamily::Unknown;
};

// The demodulator to open on a signal, from what was measured about it.
//
// This is what click-to-tune asks instead of taking VrxParams::demod's
// struct default, which is Nfm and is right for one band out of several.
// The rule and everything it gets wrong are written out in
// core/engine/vrx_place.cpp; read that before changing a threshold here,
// because the thresholds are derived from dsp::default_passband's table
// rather than chosen.
[[nodiscard]] Demod demod_for_signal(const SignalEvidence& evidence);

// The largest channel count whose narrowest guaranteed channel still
// carries a receiver this wide, at this source rate.
//
// GUARANTEED, which is the word doing the work. The grid is 2x
// oversampled, so one channel's stream is 2*rate/M wide and a receiver
// sits up to half a spacing off its channel's centre;
// dsp::max_channel_bandwidth turns that into rate/M for a receiver landing
// anywhere, and twice that for one landing exactly on a centre. This
// answers against the first figure, because a receiver's frequency is the
// operator's choice and not the grid's.
//
// Two is the floor rather than one: a source too narrow to carry the width
// at any M cannot be helped by this function, and one channel is a
// channelizer that channelizes nothing. Zero or a non-positive width
// answers the floor as well rather than dividing by it.
//
// engine::default_channel_count is this function at kWidestReceiverHz, and
// place() calls it to name the count that would have carried a receiver it
// had to refuse. Two copies of the arithmetic would be two policies.
[[nodiscard]] std::uint32_t channel_count_for(dsp::SampleRate rate,
                                              dsp::Hertz widest_receiver_hz);

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

    // The audio rate this receiver ACTUALLY runs at, which params.audio_rate
    // is not when the request named none.
    //
    // A RESULT, ON A STRUCT WHOSE OTHER RATE IS AN ECHO. VrxParams::
    // audio_rate of zero means "the engine's default", the graph resolves it
    // once in with_audio_rate and builds the stage and the plan from the
    // resolved value, and this is that value. The echo above still reads
    // zero, because feeding a status back into set_vrx_params must leave the
    // receiver on the default rather than pinning it to whatever the default
    // happened to be.
    //
    // Everything below that asks a question OF the audio rate asks it here.
    // applied_deemphasis and decoding_stereo both turn on
    // kCompositeAudioRateHz, and resolving them against the echo answered
    // "programme audio" for a receiver that took a default the engine had
    // been configured to put above that bound: EngineConfig::audio_rate is a
    // field and not a constant, and an engine built at 171000 hands every
    // receiver that named no rate a composite tap. The two functions then
    // reported Us75 and stereo on a receiver running neither, on the one
    // surface that exists to stop an operator working the curve out from the
    // sound.
    //
    // Zero means nobody filled it in, which is a default-constructed status
    // and not a receiver. The two functions fall back to the echo there,
    // because it is the only number in hand.
    dsp::SampleRate resolved_audio_rate = 0;

    // The audio rate the two answers below are resolved against.
    [[nodiscard]] constexpr dsp::SampleRate effective_audio_rate() const {
        return resolved_audio_rate != 0 ? resolved_audio_rate : params.audio_rate;
    }

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
    // this whole change is about. The answer is a pure function of the mode
    // and the curve echoed verbatim above and of the resolved rate beside
    // them, so there is nothing to forget and nothing to get stale.
    [[nodiscard]] constexpr Deemphasis applied_deemphasis() const {
        return resolve_deemphasis(params.demod, params.deemphasis, effective_audio_rate());
    }

    // Whether the stereo decoder is running, on the same terms and for the
    // same reason it is a function rather than a field. It answers whether
    // the receiver is DECODING stereo and not whether the station is
    // transmitting it: the pilot decides that, per sample, and a consumer
    // reads it off two channels that are bit-identical.
    [[nodiscard]] constexpr bool decoding_stereo() const {
        return resolve_stereo(params.demod, params.stereo, effective_audio_rate());
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

    // Times this receiver's input samples were overwritten before it could
    // filter them, and the audio frames those restarts skipped past.
    //
    // A receiver in that position restarts from the oldest channel block
    // still in the ring, because the samples it wanted have gone and no
    // answer for them exists either. core/engine/vrx_stage.cpp states at the
    // branch why it restarts instead of refusing. What a reader needs here is
    // the consequence: the audio has a hole in it, and nothing else anywhere
    // says so. The frames between the two cursors were never produced, so
    // audio_samples cannot show them, and AudioChunk::start counts frames
    // delivered rather than naming a stream position the stage agrees with, so
    // it closes over the hole instead of stepping across it. That is the
    // difference from audio_dropped, whose two sites move the index on
    // deliberately so that the gap is declared: frames that were produced and
    // lost can be declared, and frames that were never produced cannot.
    //
    // THIS IS THE COUNTER THAT SEPARATES A DECLARED LOSS FROM AN UNDECLARED
    // ONE, which is the whole reason it is here. A device retune stops the
    // transfers for about a third of a second and says so, in
    // SourceStats::samples_lost and on the block's dropped_before, so a
    // re-anchor after one is the consequence of a loss somebody has already
    // been told about. A receiver also arrives there when this device cannot
    // keep up with the channelizer, and that case moves no source counter at
    // all: before these two fields it skipped, sounded choppy, and left every
    // counter in the engine reading clean. Read them beside SourceStats and
    // the two cases come apart: re-anchors climbing while samples_lost stands
    // still is this machine losing the race rather than the radio retuning.
    //
    // FRAMES, EXACTLY, NOT AN ESTIMATE OF THEM. The stage carries an audio
    // frame cursor and the restart moves it, so the skipped count is the
    // difference between where the cursor was and where it resumed: precisely
    // the frame indices nothing will ever produce. They are frames on the
    // same terms audio_samples is, so the two are comparable and a choppy
    // receiver is one where the second is a noticeable fraction of the first.
    //
    // Zero for a raw tap for as long as it stays a buffer copy: it holds no
    // cursor of its own to fall behind, so it cannot re-anchor. Zero is
    // therefore "this did not happen" rather than "this is not measured".
    std::uint64_t reanchors = 0;
    std::uint64_t reanchor_frames_skipped = 0;
};

}  // namespace revenant::engine
