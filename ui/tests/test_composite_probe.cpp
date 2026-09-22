// plan_composite_probe, carries_composite and composite_grant_refusal: which
// of the four RDS conditions this window answers without a round trip, what
// the throwaway receiver is asked to be, and the sentence for a channel too
// narrow to hold the subcarrier.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS, in its own
// comment, for the reason test_audio_ring.cpp gives.
//
// The wrong implementations on this path are the ones that shipped or nearly
// did: a switch that polls without raising the rate at all, which refuses
// forever because 48000 has 24 kHz of Nyquist; a rate raised by asking the
// engine blind, which loses the operator's receiver to a refused add; a probe
// built from anything but the pane's own params, which answers a question
// about a receiver nobody has; a rate treated as a floor rather than the one
// exact value, which admits a receiver the decoder was not built for; and a
// fourth condition read off the REQUEST, which is the one the add succeeding
// does not answer.
//
// WHAT CANNOT BE TESTED HERE, said rather than faked. The probe itself is two
// RPC calls and a receiver created on a live engine, and the rebuild behind it
// is Qt property plumbing across three threads. Neither reaches this binary:
// ui/tests links Catch2 and pure headers, no Qt and no Cap'n Proto. What a
// green run here means is that the arithmetic and the words are right. Whether
// the probe actually protects the operator's receiver is answered by running
// the client against an engine.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "models/composite_probe.h"

using revenant::ui::carries_composite;
using revenant::ui::composite_grant_refusal;
using revenant::ui::CompositeProbe;
using revenant::ui::demod_makes_composite;
using revenant::ui::kRdsCompositeRateHz;
using revenant::ui::kRdsCompositeReachHz;
using revenant::ui::plan_composite_probe;

TEST_CASE("the composite rate is three times the subcarrier and 144 bit periods",
          "[composite]")
{
    // The whole reason the number is 171000 and not simply something above
    // the 148438 the receiver's own bound works out to. Both divisions are
    // exact, which is what lets the decoder run with no resampling anywhere.
    //
    // The wrong implementation is a client that picks the lowest rate that
    // clears the bound. It decodes, badly, and nothing says why.
    CHECK(kRdsCompositeRateHz == 3U * 57'000U);
    CHECK(kRdsCompositeRateHz % 57'000U == 0U);

    // 1187.5 bit/s, in integers: 171000 * 2 == 342000 == 144 * 2375, and
    // 2375 is twice the bit rate.
    CHECK(2U * kRdsCompositeRateHz == 144U * 2'375U);
}

TEST_CASE("the engine's default is not a composite rate", "[composite]")
{
    // THE BUG THIS WHOLE PATH EXISTS FOR. A receiver created the ordinary way
    // takes 48000, whose Nyquist is 24 kHz, so the 57 kHz subcarrier is gone
    // before the decoder is built and the switch refuses every time.
    //
    // The wrong implementation is the one that shipped: leave audio_rate at
    // zero and poll.
    revenant::rpc::VrxParams params;
    CHECK(params.audio_rate == 0U);
    CHECK_FALSE(carries_composite(params));

    params.audio_rate = 48'000;
    CHECK_FALSE(carries_composite(params));

    params.audio_rate = kRdsCompositeRateHz;
    CHECK(carries_composite(params));
}

TEST_CASE("a rate above the bound is still not the composite rate", "[composite]")
{
    // THE EXACT VALUE AND NOT A FLOOR. A receiver at 192000 clears the
    // receiver's 148438 bound and the decoder's 125000, so a floor test calls
    // it done, and then the decoder's loops are sized for 171000 and are being
    // fed something else.
    //
    // The wrong implementation is `audio_rate >= 148438`.
    revenant::rpc::VrxParams params;
    params.audio_rate = 192'000;
    CHECK_FALSE(carries_composite(params));
}

TEST_CASE("only a discriminator produces a composite", "[composite]")
{
    // Conditions one and two of the four, which are the demodulator alone:
    // nfm and wfm reach the same atan2 branch, the product detectors produce
    // audio with no subcarrier in it at any rate, and the raw tap writes an
    // interleaved complex pair.
    CHECK(demod_makes_composite(revenant::rpc::Demod::Wfm));
    CHECK(demod_makes_composite(revenant::rpc::Demod::Nfm));

    // NFM is in, and this is the case a reader expects to be out. It is
    // admitted deliberately: the decoder is scale invariant, so the only
    // difference is gain.
    CHECK(demod_makes_composite(revenant::rpc::Demod::Nfm));

    CHECK_FALSE(demod_makes_composite(revenant::rpc::Demod::Am));
    CHECK_FALSE(demod_makes_composite(revenant::rpc::Demod::Usb));
    CHECK_FALSE(demod_makes_composite(revenant::rpc::Demod::Lsb));
    CHECK_FALSE(demod_makes_composite(revenant::rpc::Demod::Dsb));
    CHECK_FALSE(demod_makes_composite(revenant::rpc::Demod::Cw));
    CHECK_FALSE(demod_makes_composite(revenant::rpc::Demod::Raw));
}

TEST_CASE("the probe changes the rate and nothing else", "[composite]")
{
    // THE PROBE HAS TO BE THE SAME RECEIVER. It is answering "can THIS
    // receiver be rebuilt at 171000", so a centre, a passband or a mode of its
    // own makes it answer a question about a receiver nobody has: the channel
    // a receiver lands in comes from its centre, and the fold it has to leave
    // a transition band against comes from its passband.
    //
    // The wrong implementation is a probe built from a default VrxParams with
    // the rate set, which lands in whatever channel baseband DC is in and
    // reports that channel's answer for a receiver five megahertz away.
    revenant::rpc::VrxParams live;
    live.center = 1'234'567;
    live.bandwidth = 200'000;
    live.passband_low = -100'000;
    live.passband_high = 100'000;
    live.demod = revenant::rpc::Demod::Wfm;
    live.audio_rate = 0;
    live.squelch_dbfs = -70.0;
    live.agc_enabled = false;
    live.cw_pitch = 800;

    const CompositeProbe probe = plan_composite_probe(live, "wfm");
    REQUIRE(probe.worth_asking);
    CHECK(probe.refusal.empty());

    CHECK(probe.params.audio_rate == kRdsCompositeRateHz);
    CHECK(probe.params.center == live.center);
    CHECK(probe.params.bandwidth == live.bandwidth);
    CHECK(probe.params.passband_low == live.passband_low);
    CHECK(probe.params.passband_high == live.passband_high);
    CHECK(probe.params.demod == live.demod);
    CHECK(probe.params.squelch_dbfs == live.squelch_dbfs);
    CHECK(probe.params.agc_enabled == live.agc_enabled);
    CHECK(probe.params.cw_pitch == live.cw_pitch);
}

TEST_CASE("a mode that cannot carry a composite is refused without asking",
          "[composite]")
{
    // A ROUND TRIP THIS WINDOW DOES NOT NEED TO SPEND. The demodulator is a
    // field on the request and the client holds the request, so probing an am
    // receiver creates a receiver on the engine to be told something readable
    // off a struct, and then rebuilds the operator's receiver at 171000 for an
    // answer that was never going to be yes.
    //
    // The wrong implementation is a gate that probes on the rate alone: the
    // add succeeds, because 171000 is perfectly buildable for am, and the
    // refusal arrives from the decoder guard after the audio has already been
    // replaced by 171 kHz of nothing anybody wants.
    revenant::rpc::VrxParams live;
    live.demod = revenant::rpc::Demod::Am;

    const CompositeProbe probe = plan_composite_probe(live, "am");
    CHECK_FALSE(probe.worth_asking);
    REQUIRE_FALSE(probe.refusal.empty());

    // THE MODE IT IS ON, BY NAME. "this receiver cannot carry RDS" sends the
    // operator nowhere; naming am and naming the two modes that work is the
    // difference between a refusal and an instruction.
    CHECK(probe.refusal.find("am") != std::string::npos);
    CHECK(probe.refusal.find("nfm") != std::string::npos);
    CHECK(probe.refusal.find("wfm") != std::string::npos);

    // And it says nothing about the rate, which is not what failed. A
    // sentence about 171000 here would send the operator to the grid.
    CHECK(probe.refusal.find("171000") == std::string::npos);
}

TEST_CASE("the grant has to reach the subcarrier either side", "[composite]")
{
    // CONDITION FOUR, AND THE ONE THE ADD SUCCEEDING DOES NOT ANSWER. A
    // narrow receiver at 171000 is admitted by the planner: the fold it has to
    // clear is half the channel rate, and a 16 kHz passband clears a 37500 S/s
    // channel's fold comfortably. The decoder guard then refuses it, because
    // the granted filter does not pass 59375 Hz.
    //
    // The wrong implementation is trusting the add. It rebuilds the operator's
    // receiver at 171000, destroying its audio, for a decoder that is refused
    // on the next call.
    CHECK(composite_grant_refusal(-100'000, 100'000).empty());
    CHECK(composite_grant_refusal(-kRdsCompositeReachHz, kRdsCompositeReachHz).empty());

    // One hertz short on one edge is short. The bar is per edge because each
    // edge is fitted on its own, so an off-centre grant can reach on one side
    // and not the other and a width comparison would call it fine.
    CHECK_FALSE(composite_grant_refusal(-kRdsCompositeReachHz + 1,
                                        kRdsCompositeReachHz)
                    .empty());
    CHECK_FALSE(composite_grant_refusal(-kRdsCompositeReachHz,
                                        kRdsCompositeReachHz - 1)
                    .empty());

    // THE CASE A WIDTH TEST GETS WRONG. 140 kHz wide, which is more than twice
    // 59375, and it reaches only 20 kHz below the centre.
    CHECK_FALSE(composite_grant_refusal(-20'000, 120'000).empty());
}

TEST_CASE("an unplaced probe is not a refusal", "[composite]")
{
    // A grant of nothing either way is a receiver the engine has not placed,
    // or a status this client could not read, and neither is a statement about
    // a filter width. Answering it with a sentence about a narrow channel
    // would turn one unanswered round trip into a switch that does nothing.
    //
    // The wrong implementation is `granted_high < 59375 -> refuse`, which
    // refuses a default-constructed placement.
    CHECK(composite_grant_refusal(0, 0).empty());

    // And a reversed pair, which is not a passband at all.
    CHECK(composite_grant_refusal(100'000, -100'000).empty());
}

TEST_CASE("the narrow-channel refusal names both numbers and the fix",
          "[composite]")
{
    // The sentence an operator acts on. It has to carry what the channel gave,
    // what a composite needs, and that the fix is the engine's channel count
    // rather than anything they can drag.
    //
    // The wrong implementation is "this receiver cannot carry RDS", which is
    // true, unactionable, and indistinguishable from the wrong-mode case.
    const std::string refusal = composite_grant_refusal(-18'000, 18'000);
    REQUIRE_FALSE(refusal.empty());

    CHECK(refusal.find("-18 kHz") != std::string::npos);
    CHECK(refusal.find("18 kHz") != std::string::npos);
    CHECK(refusal.find("59.4 kHz") != std::string::npos);
    CHECK(refusal.find("57 kHz") != std::string::npos);
    CHECK(refusal.find("channels") != std::string::npos);
}
