// Where a receiver attaches to the coarse grid, and what the fine stage has
// to undo to get from there to what was asked for.
//
// Two functions, both pure, both integer. Nothing here rounds a frequency:
// the whole reason core/dsp/pfb.h carries ChannelCentre as an exact rational
// is that k*rate/M is frequently not a whole hertz, and a receiver that
// rounded it at the point the channel was described would carry a tuning
// offset nobody could later source. The residual this computes is the input
// to the fine stage's NCO, and it stays exact right up to the single
// reduction into fixed-point turns in core/dsp/vrx_reference.cpp.
//
// WHAT FRAME params.center IS IN.
//
// place() is handed the grid, the source rate and the request, and nothing
// else. It is not told where the source is tuned, so it cannot convert an
// absolute radio frequency into a position on the grid, and it therefore
// reads VrxParams::center as a frequency in the SOURCE'S OWN BASEBAND FRAME:
// hertz from the source's centre, in [-rate/2, +rate/2].
//
// That is the whole contract, and there is no rebase hidden behind it.
// Engine::add_vrx passes the caller's params to this function untouched and
// stores those same params, so VrxStatus reads back in baseband as well.
// Converting from an absolute frequency belongs to the caller, which is the
// only party that has EngineInfo::source_center: tools/cli/main.cpp does the
// subtraction for a frequency the operator typed, and the bench and the
// tests build offsets directly. core/engine/vrx.h argues why the offset is
// the API rather than the absolute frequency the name "center" suggests.
//
// This is stated in both places because the two readings differ by the whole
// local oscillator and the failure is silent on a source that declares no
// centre, where source_center is zero and the two numbers coincide.
//
// The two places have not always agreed. Until 2026-09-19 core/engine/vrx.h
// documented center as an absolute radio frequency, and core/rpc/
// revenant.capnp, core/rpc/types.h and docs/detection.md each recorded that
// disagreement, one of them saying the engine was expected to rebase. The
// header was the side that was wrong. Nothing below changed when it was
// corrected, because nothing below has ever rebased: this function is handed
// no tuned centre to rebase against, which is the argument the paragraph
// above makes and the reason the offset reading is the one that survived.

#include "core/engine/vrx.h"

#include <bit>
#include <cstdlib>
#include <format>
#include <numeric>
#include <string>
#include <string_view>

#include "core/dsp/noise_reference.h"
#include "core/dsp/pfb.h"
#include "core/dsp/vrx_reference.h"
#include "core/engine/listener_level.h"

namespace revenant::engine {
namespace {

// Every demodulator's name, in enumerator order, so the table and the enum
// cannot drift: demod_name() is the single spelling and this reads it back.
constexpr Demod kAllDemods[] = {Demod::Raw,   Demod::Am,    Demod::Nfm,
                                Demod::Wfm,   Demod::Usb,   Demod::Lsb,
                                Demod::Dsb,   Demod::Cw,    Demod::P25p1,
                                Demod::Dstar, Demod::Tetra, Demod::Dmr,
                                Demod::Sam};

// Rounds a quotient of integers to the nearest integer, halves away from zero.
//
// Written out rather than done in double. At a 20 MS/s source with 2048
// channels the numerator is around 2e13, which a double still represents
// exactly, but the margin is four bits and a wider grid or a faster source
// spends them. Integer division has no margin to spend.
[[nodiscard]] std::int64_t divide_nearest(std::int64_t numerator, std::int64_t denominator) {
    const std::int64_t quotient = numerator / denominator;
    const std::int64_t remainder = numerator % denominator;

    // Both operands of the comparison are magnitudes, so the sign of the
    // numerator does not change which way the half rounds.
    const std::int64_t twice = 2 * (remainder < 0 ? -remainder : remainder);
    if (twice < (denominator < 0 ? -denominator : denominator)) {
        return quotient;
    }
    return (remainder < 0) ? quotient - 1 : quotient + 1;
}

// The widest occupied bandwidth that is still one narrowband FM channel.
//
// Not a new number. dsp::default_passband(Demod::Nfm) is +/-8 kHz and
// core/dsp/vrx_reference.cpp derives it from land mobile in a 25 kHz
// channel: 5 kHz deviation plus 3 kHz of audio, doubled by Carson, is
// 16 kHz of occupied bandwidth inside a 25 kHz allocation. 16 kHz is what
// the transmitter occupies at full modulation and 25 kHz is the channel it
// is entitled to, and a detector measuring where the energy is reports
// something between the two. So the channel, not the Carson figure, is the
// line: a signal wider than a whole allocation is not living in one.
constexpr dsp::Hertz kNarrowbandChannelHz = 25'000;

}  // namespace

// ---------------------------------------------------------------------------
// Choosing a demodulator from a measurement
// ---------------------------------------------------------------------------

// THE DECISION RULE, AND WHAT IT GETS WRONG.
//
// Two inputs, and only one of them is ever populated today.
//
// `occupied_hz` is what core/detect measures per track and is present on
// every click-to-tune. The rule over it is one threshold:
// kNarrowbandChannelHz above. At or below it the signal fits a narrowband
// channel and the answer is Nfm; above it the answer is Wfm. Both ends of
// that are sourced from dsp::default_passband's own table rather than
// picked: Nfm's passband is derived from a 25 kHz channel and Wfm's is the
// 200 kHz the broadcast band plan allocates, so the threshold sits at the
// top of the narrower mode's channel rather than anywhere between the two.
//
// `family` is core/characterise's answer and is Unknown on every path that
// exists today, because that stage reads complex baseband and is not wired
// into the engine. Where it IS known it overrides the width, because it is
// a measurement of the modulation and the width is a measurement of the
// occupancy: Unmodulated is a carrier and wants Cw, and Psk and Ofdm have
// no analogue demodulator that does anything useful with them, so the
// honest answer there is the raw tap rather than a mode that will produce
// noise confidently. Fsk falls through to the width, because a discriminator
// is the front end of every FSK voice mode this project will decode and
// the width is what says which one.
//
// WHAT THIS GETS WRONG, IN THE ORDER IT WILL BITE.
//
// One: AM. Broadcast AM occupies 10 kHz and comes back Nfm, which is a
// discriminator on an amplitude-modulated carrier and produces noise. Width
// cannot separate them and nothing else here tries. The separation exists
// and is measured: characterise::EnvelopeStats against
// CharacteriseConfig::constant_envelope_variance answers it in one number,
// and that number arrives here the day an extract does.
//
// Two: SSB and CW. Both come back Nfm for the same reason, and CW is worse
// because a keyed carrier is narrow enough to look like a quiet FM channel.
// A sideband is also not centred on what the detector calls the centre, so
// even the right mode would need the detector to say which side the energy
// is on, which it does not.
//
// Three: the band between one narrowband channel and a broadcast station.
// Nothing standard lives from 25 kHz to about 150 kHz, and everything in it
// is called Wfm. That is the deliberate direction to be wrong in: a WFM
// receiver on a narrow signal passes the whole signal plus noise and the
// audio is quiet but intact, while an NFM receiver on a wide signal
// truncates it and the audio is wrong at full strength. One of those an
// operator can hear past.
//
// Four: digital voice in a 25 kHz channel. DMR, P25 and the rest come back
// Nfm, which is right about the front end and wrong about the result: the
// discriminator output is correct and there is no vocoder behind it, so
// what comes out is a buzz. The modes exist, p25p1, dstar, tetra and dmr,
// and neither input here can name one: occupancy does not separate a P25
// carrier from an NFM one, and characterise::ModulationFamily says Fsk or
// Psk but not which standard.
//
// WHAT THIS PARAGRAPH USED TO SAY: it ended "core/engine has no mode to
// name yet", written before the three digital voice modes were appended to
// Demod.
Demod demod_for_signal(const SignalEvidence& evidence) {
    switch (evidence.family) {
        case characterise::ModulationFamily::Unmodulated: return Demod::Cw;

        // A linear constellation or a multicarrier waveform. No analogue
        // detector in the table demodulates either, so the receiver hands
        // out baseband and whatever is listening decides.
        case characterise::ModulationFamily::Psk:
        case characterise::ModulationFamily::Ofdm: return Demod::Raw;

        case characterise::ModulationFamily::AnalogueFm:
        case characterise::ModulationFamily::Fsk:
        case characterise::ModulationFamily::Unknown: break;
    }

    // Nothing measured. The struct default is what a caller with no
    // evidence would have got anyway, and this says so in one place
    // instead of leaving it implicit in core/rpc/types.h.
    if (evidence.occupied_hz <= 0) {
        return Demod::Nfm;
    }

    return evidence.occupied_hz > kNarrowbandChannelHz ? Demod::Wfm : Demod::Nfm;
}

std::uint32_t channel_count_for(dsp::SampleRate rate, dsp::Hertz widest_receiver_hz) {
    if (rate <= 0 || widest_receiver_hz <= 0) {
        return 2;
    }
    const auto guaranteed_at_one = static_cast<std::uint64_t>(rate) /
                                   static_cast<std::uint64_t>(widest_receiver_hz);
    if (guaranteed_at_one < 2) {
        return 2;
    }
    return static_cast<std::uint32_t>(std::bit_floor(guaranteed_at_one));
}

Expected<Demod> demod_from_name(std::string_view name) {
    for (const Demod mode : kAllDemods) {
        if (name == demod_name(mode)) {
            return mode;
        }
    }

    std::string known;
    for (const Demod mode : kAllDemods) {
        if (!known.empty()) {
            known += ", ";
        }
        known += demod_name(mode);
    }
    return fail(std::format("unknown demodulator '{}', expected one of: {}", name, known));
}

Expected<VrxPlacement> place(const dsp::GridParams& grid, dsp::SampleRate rate,
                             const VrxParams& params) {
    // The grid's own shape is core/dsp/pfb.h's business: a power-of-two
    // channel count, a decimation that divides it, a nonzero tap count.
    // Calling validate here rather than restating those rules means a
    // receiver is rejected by exactly the grid the kernels reject, and keeps
    // being rejected if the rules move.
    if (const Status valid = dsp::validate(grid); !valid) {
        return std::unexpected(with_context(valid.error(), "place"));
    }

    if (rate <= 0) {
        return fail(std::format("place: sample rate must be positive, got {}", rate));
    }

    // The channel stream's rate is rate/D, and it has to be a whole number of
    // samples per second because SampleRate is integral and because the
    // resampler's recurrence is exact integer arithmetic over it. D is a power
    // of two in every grid validate() accepts, so this rejects an odd source
    // rate rather than a misconfigured grid, and the fix is the source's rate
    // rather than anything here.
    if (rate % static_cast<dsp::SampleRate>(grid.decimation) != 0) {
        return fail(std::format(
            "place: source rate {} is not divisible by the grid's decimation {}, so the "
            "channel rate would not be a whole number of samples per second",
            rate, grid.decimation));
    }
    const dsp::SampleRate channel_rate = rate / static_cast<dsp::SampleRate>(grid.decimation);

    const dsp::Hertz nyquist = rate / 2;
    if (params.center > nyquist || params.center < -nyquist) {
        return fail(std::format(
            "place: centre {} Hz is outside the source's baseband span of +/-{} Hz. This "
            "frequency is relative to the source's tuned centre, not absolute; subtract "
            "EngineInfo::source_center from an absolute frequency before passing it",
            params.center, nyquist));
    }

    // The noise fields, here because this is the one function add_vrx and
    // set_vrx_params both call before anything is queued. A stage that found
    // a bad figure at retune would be refusing on the recording thread,
    // where nobody hears it; see dsp::validate_noise_request.
    if (const Status noise = dsp::validate_noise_request(params); !noise) {
        return std::unexpected(with_context(noise.error(), "place"));
    }

    // The AGC's time constants, on the same terms and for the same reason:
    // the stage that reads them runs on the completion thread, where a
    // figure it could not use would have nobody to refuse.
    if (const Status agc = validate_agc_request(params); !agc) {
        return std::unexpected(with_context(agc.error(), "place"));
    }

    // The passband the request resolves to, before anything is known about
    // where it lands. Resolved here rather than after the placement so that
    // a request nobody can fill is refused before the grid arithmetic runs,
    // which is what the bandwidth check this replaces did.
    auto requested = dsp::resolve_passband(params);
    if (!requested) {
        return std::unexpected(with_context(requested.error(), "place"));
    }

    // Nearest grid channel. Minimising |centre - s*rate/M| over the signed
    // channel index s is minimising |centre*M - s*rate|, so s is the nearest
    // integer to centre*M/rate and the whole choice is integer arithmetic.
    // centre is bounded by rate/2 above, so the product is at most
    // rate*M/2: 2e13 for a 20 MS/s source on a 2048-channel grid, eleven
    // orders of magnitude inside int64.
    const auto channels = static_cast<std::int64_t>(grid.channels);
    const std::int64_t signed_index =
        divide_nearest(static_cast<std::int64_t>(params.center) * channels,
                       static_cast<std::int64_t>(rate));

    // Reduce into [0, M). centre == +rate/2 lands on s == M/2, which
    // channel_centre folds back to -rate/2: the same channel, and the same
    // frequency, because the spectrum is periodic in rate.
    std::int64_t wrapped = signed_index % channels;
    if (wrapped < 0) {
        wrapped += channels;
    }
    const auto channel = static_cast<std::uint32_t>(wrapped);

    VrxPlacement placement;
    placement.channel = channel;
    placement.channel_centre = dsp::channel_centre(grid, rate, channel);
    placement.channel_rate = channel_rate;

    // The residual the fine stage mixes out: requested minus actual, as the
    // exact rational it is. channel_centre is already reduced, so its
    // denominator divides M and the product below is bounded by rate*M/2.
    const std::int64_t centre_denominator = placement.channel_centre.denominator;
    placement.residual_numerator = static_cast<std::int64_t>(params.center) * centre_denominator -
                                   placement.channel_centre.numerator;
    placement.residual_denominator = centre_denominator;

    // Fold into (-rate/2, +rate/2]. This is not defensive: a request at
    // exactly +rate/2 lands on channel M/2, whose centre channel_centre
    // reports as -rate/2 because that is the representative it folds to, and
    // the difference between the two is a whole span. They are the same
    // frequency, and the residual the fine stage mixes has to be the small
    // one rather than the span.
    const std::int64_t span =
        static_cast<std::int64_t>(rate) * placement.residual_denominator;
    while (2 * placement.residual_numerator > span) {
        placement.residual_numerator -= span;
    }
    while (2 * placement.residual_numerator <= -span) {
        placement.residual_numerator += span;
    }

    const std::int64_t divisor =
        std::gcd(placement.residual_numerator, placement.residual_denominator);
    if (divisor > 1) {
        placement.residual_numerator /= divisor;
        placement.residual_denominator /= divisor;
    }

    // What one channel can actually deliver to this receiver, given where
    // the receiver landed inside it. The definition and its derivation are
    // in core/dsp/vrx_reference.h so that place() and the planner cannot
    // disagree about it: both call the same clamp against the same
    // placement, and the edges the caller reads here are the edges the
    // planner will build a filter for.
    //
    // This used to be one comparison against max_channel_bandwidth, which
    // could say that something did not fit and never which side. Each edge
    // is fitted on its own now and both are carried out.
    const dsp::Passband granted = dsp::clamp_to_channel(placement, *requested);
    placement.granted_low = granted.low;
    placement.granted_high = granted.high;
    placement.bandwidth_clamped = granted != *requested;

    // WHERE THE CLAMP STOPS BEING A FIT AND BECOMES A DIFFERENT RECEIVER.
    //
    // Everything above narrows a request to what one channel can carry and
    // reports the pair, which is right for every mode that is linear in its
    // passband: an AM receiver given 9 kHz instead of 10 has less audio
    // bandwidth and nothing else. It was wrong for the two FM modes, and
    // wrong in the way that is hardest to diagnose from the operator's
    // chair. Clicking a 145 kHz broadcast station on a 64-channel grid over
    // 2.4 MS/s produced a receiver granted 37.5 kHz, the discriminator was
    // fed a fifth of the signal, and what came out was distortion at full
    // strength while the waterfall showed a strong clean carrier. Every
    // number on every surface was correct and the radio sounded broken.
    //
    // TWO CONDITIONS, BOTH NEEDED.
    //
    // clamp_breaks_demodulator is the first: it is the two FM modes and the
    // three digital voice modes, so a linear mode keeps the behaviour its
    // callers already have and its tests already pin. WHAT THIS USED TO SAY:
    // "it is the FM modes and nothing else", which was the list until the
    // digital voice modes got a fine stage on 2026-09-22 and a passband that
    // is actually applied to them.
    //
    // The second is that the grant fell below the mode's OWN channel plan,
    // dsp::default_passband. A WFM receiver asking for 300 kHz and granted
    // 250 still has the whole broadcast channel and works; one asking for
    // 200 and granted 37.5 does not have a channel at all. Using the
    // request rather than the table would refuse the first of those, and
    // using "clamped at all" would refuse a receiver that is fine.
    //
    // WHY THIS IS A REFUSAL AND NOT A REGRID.
    //
    // The grid is chosen once, in Engine::open_source, and
    // engine::default_channel_count already sizes it from the widest
    // receiver a source rate can carry, which is why a source opened
    // without a named channel count never reaches this branch. Changing it
    // after that is not a parameter edit. grid.channels fixes the prototype
    // filter and the twiddle table, the whole Graph and its pipelines and
    // descriptor sets, the DeviceRing's floor capacity (four times the
    // prototype span plus a block), the block size that was reduced to fit
    // that ring, and EngineInfo::spectrum, whose bin count and bin width
    // every spectrum subscriber read once when it subscribed. It also
    // invalidates every placement already handed out, because a channel
    // index, a residual and a channel rate are all relative to the grid the
    // receiver was placed on. Rebuilding that set while samples are flowing
    // means tearing down the graph under the scheduler and renumbering
    // subscriptions that have no way to be told, so the engine refuses and
    // names the count instead.
    //
    // The message says what to pass because the fix is one argument at
    // startup and an operator who is not told will not find it.
    if (placement.bandwidth_clamped && clamp_breaks_demodulator(params.demod)) {
        const dsp::Passband plan = dsp::default_passband(params.demod);
        const std::int64_t got = granted.high - granted.low;
        if (plan.width() > 0 && got < plan.width()) {
            const std::uint32_t would_carry = channel_count_for(rate, plan.width());
            const auto guaranteed = static_cast<std::int64_t>(rate) /
                                    static_cast<std::int64_t>(would_carry);

            std::string advice;
            if (guaranteed >= plan.width()) {
                advice = std::format(
                    "{} channels on this source guarantee {} Hz to a receiver placed "
                    "anywhere, so --channels {} carries it. The grid is sized when the "
                    "source is opened and cannot be changed while it is running",
                    would_carry, guaranteed, would_carry);
            } else {
                advice = std::format(
                    "No channel count helps: at {} S/s even a two-channel grid guarantees "
                    "only {} Hz, so the source rate itself is the limit",
                    rate, static_cast<std::int64_t>(rate) / 2);
            }

            // Two reasons, one per kind of receiver. The digital voice modes
            // joined clamp_breaks_demodulator when they got a fine stage,
            // and TETRA has no discriminator to blame, so the sentence that
            // names one is kept for the modes that end in audio.
            const std::string why =
                produces_audio(params.demod)
                    ? std::format("a discriminator recovers the instantaneous frequency of "
                                  "whatever reaches it, so a {} receiver on a truncated "
                                  "passband produces the wrong audio rather than less of the "
                                  "right audio",
                                  demod_name(params.demod))
                    : std::format("its decoder's receive filter is matched to the whole "
                                  "channel, so a {} receiver on a truncated passband hands it "
                                  "symbols smeared into their neighbours rather than a weaker "
                                  "copy of the same symbols",
                                  demod_name(params.demod));
            return fail(std::format(
                "place: a {} receiver needs {} Hz of passband and one channel of this {} "
                "channel grid could carry {} Hz of it, {} to {} about the receiver's "
                "centre. This is refused rather than narrowed because {}, at full strength "
                "and with nothing on the display saying so. {}",
                demod_name(params.demod), plan.width(), grid.channels, got, granted.low,
                granted.high, why, advice));
        }
    }

    return placement;
}

}  // namespace revenant::engine
