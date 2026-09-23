// Whether the pane's receiver can be made to carry an RDS composite, and the
// throwaway receiver that asks the engine instead of guessing.
//
// WHY THE WINDOW HAS TO ASK RATHER THAN DECIDE
//
// RDS rides a 57 kHz subcarrier, so the receiver feeding the decoder has to
// run at an audio rate that can carry 57 kHz. A receiver created the ordinary
// way takes the engine's default, which is 48000, and 24 kHz of Nyquist has
// already destroyed the subcarrier before the decoder is built. Raising the
// rate is therefore not an optimisation, it is the whole of what makes the
// switch work.
//
// Whether the engine will grant that rate depends on the channel the receiver
// landed in, and this process links no part of the DSP: the arithmetic that
// decides it is in dsp::plan_vrx and the grid sizing behind it is
// engine::place. So the client cannot compute the answer, and asking blind
// costs the operator their receiver, because a rate change is a remove and an
// add and a refused add leaves the pane empty.
//
// Client::add_vrx either answers with an id or refuses and destroys nothing.
// That is the probe: add a SECOND receiver with the pane receiver's params and
// the composite rate, keep it only long enough to read what it was granted,
// and remove it. A refusal is the engine's own sentence naming which condition
// failed, on a receiver nobody was using.
//
// WHY THIS HOLDS NO Qt. ui/tests links it. Which conditions the window can
// answer on its own, what the probe asks for, and the words a refusal is
// written in are all pure functions of a params struct and two integers.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/rpc/types.h"
#include "models/receiver_match.h"

namespace revenant::ui {

// The audio rate a receiver has to run at to hand out an RDS composite.
//
// THREE TIMES THE 57 kHz SUBCARRIER AND 144 TIMES THE 1187.5 bit/s BIT RATE,
// both exact, which is why it is this number and not merely something above
// 148438. It is also what decode::RdsBitsConfig::rate defaults to and what
// tools/cli --rds opens, so a composite from a receiver at this rate reaches
// the decoder with no resampling anywhere.
//
// Named here once. core/rpc/revenant.capnp's RDS section is the definition and
// this is the client's copy of it; core/engine/vrx.h carries a third for the
// de-emphasis bound, which is 114000 and a different question.
inline constexpr std::uint32_t kRdsCompositeRateHz = 171'000;

// How far the GRANTED passband has to reach either side of the mix centre.
//
// The fourth of the four conditions core/rpc/revenant.capnp lists, and the one
// the rate does not imply: 59375 Hz is the top of the RDS data band, and a
// filter that does not pass it has nothing for the decoder whatever the rate
// is. Necessary and not sufficient, because Carson for a multiplex reaching
// 59375 Hz is about 268750 Hz and the ordinary 200 kHz broadcast passband
// already truncates the sidebands. The health counters are what say whether a
// receiver at the bare minimum is actually decoding; this bar will not.
inline constexpr std::int64_t kRdsCompositeReachHz = 59'375;

// The receiver is already running at the composite rate, so there is nothing
// to raise and nothing to probe.
//
// A comparison against the exact rate and not a floor. The client only ever
// asks for this one value, so anything else is a receiver this window did not
// raise, and treating "high enough" as done would leave a receiver at some
// other rate polling a decoder built for a rate it is not delivering.
[[nodiscard]] inline bool carries_composite(const rpc::VrxParams& params)
{
    return params.audio_rate == kRdsCompositeRateHz;
}

// Conditions one and two of the four, which are the demodulator alone.
//
// Only a discriminator produces an FM composite multiplex, so nfm and wfm are
// the two modes that can carry one at any rate: core/shaders/vrx_demod.comp
// reaches the same atan2 branch for both and the decoder is scale invariant.
// An envelope detector and the four product detectors produce audio with no
// subcarrier in it however fast it is sampled, and the raw tap writes an
// interleaved complex pair, which a decoder expecting a real span reads as
// consecutive samples of a signal that does not exist.
[[nodiscard]] inline bool demod_makes_composite(rpc::Demod demod)
{
    return demod == rpc::Demod::Nfm || demod == rpc::Demod::Wfm;
}

// Whether the receiver window offers RDS on this receiver at all.
//
// PROGRESSIVE DISCLOSURE, decided on 2026-09-22: the RDS section appears only
// for a receiver that could carry a composite and was granted enough filter to
// pass the subcarrier. For anything else it is absent, not greyed and not
// explained, because a switch that can only refuse is a paragraph of refusal
// waiting to happen, which is what the pane had become.
//
// WFM ONLY, where demod_makes_composite also allows nfm. A discriminator on nfm
// does produce a multiplex, but an nfm receiver is a two-way radio channel with
// no RDS on it, and offering the switch there would be offering it on every
// receiver an operator opens on a repeater.
//
// A grant of nothing is a receiver the engine has not placed yet, and nothing
// is offered until it has.
[[nodiscard]] constexpr bool rds_offered(rpc::Demod demod, std::int64_t granted_low,
                                         std::int64_t granted_high)
{
    if (demod != rpc::Demod::Wfm || granted_high <= granted_low) {
        return false;
    }
    return -granted_low >= kRdsCompositeReachHz && granted_high >= kRdsCompositeReachHz;
}

// What to do about a switch the operator has just turned on.
struct CompositeProbe {
    // Ask the engine. False when the window has already answered on its own,
    // in which case refusal says why and no receiver is added.
    bool worth_asking = false;

    // Empty when worth_asking. Otherwise the sentence to put in front of the
    // operator, which is this window's own and not the engine's: the two
    // conditions below are facts about the request, which the client owns in
    // full, so probing for them would spend a round trip to be told something
    // it already knew.
    std::string refusal;

    // The throwaway receiver's params. Everything the pane asked for, with the
    // rate raised, so the probe stands or falls on the one thing being
    // changed: same centre, same passband, same mode, same channel.
    rpc::VrxParams params;
};

// demod_name is the spelling this window already has for params.demod, passed
// in rather than derived, because the table lives in models/engine_link.h
// behind QString and this header holds no Qt.
[[nodiscard]] inline CompositeProbe plan_composite_probe(const rpc::VrxParams& live,
                                                         std::string_view demod_name)
{
    CompositeProbe probe;

    if (!demod_makes_composite(live.demod)) {
        probe.refusal = "RDS is decoded from an FM discriminator's output and this "
                        "receiver is on ";
        probe.refusal += demod_name;
        probe.refusal += ". Only nfm and wfm produce a composite multiplex, at any "
                         "audio rate. Change the mode and the switch can raise the "
                         "rate.";
        return probe;
    }

    probe.params = live;
    probe.params.audio_rate = kRdsCompositeRateHz;
    probe.worth_asking = true;
    return probe;
}

// Condition four, read off the GRANT and not off the request.
//
// Each edge is fitted on its own, so a receiver that asked for 200 kHz and
// landed in a channel that could carry 71 can be off-centre as well as narrow,
// and the request says nothing about either. Empty when the grant reaches far
// enough, which is the ordinary answer on a grid sized for broadcast FM.
//
// A grant of nothing either way is a receiver the engine has not placed yet,
// and this says nothing about it: the caller reads the probe's status
// immediately after adding it, and a status that arrived empty is a question
// to leave to the engine's own refusal rather than to answer with a sentence
// about a filter width nobody has.
[[nodiscard]] inline std::string composite_grant_refusal(std::int64_t granted_low,
                                                         std::int64_t granted_high)
{
    if (granted_high <= granted_low) {
        return {};
    }
    if (-granted_low >= kRdsCompositeReachHz && granted_high >= kRdsCompositeReachHz) {
        return {};
    }

    std::string out = "the engine granted this receiver ";
    out += format_width(granted_low);
    out += " to ";
    out += format_width(granted_high);
    out += " around its centre, and an RDS composite needs at least ";
    out += format_width(kRdsCompositeReachHz);
    out += " either side to pass the 57 kHz subcarrier. The channel is too narrow "
           "for it: fewer channels on the engine's grid would make each one wide "
           "enough.";
    return out;
}

}  // namespace revenant::ui
