// The surface the schema carries and the engine does not serve.
//
// WHAT THIS CASE IS FOR, WHICH IS NOT WHAT IT ASSERTS
//
// It asserts that rdsStation and setRdsRegion reach the server and come back
// refused in a sentence saying the surface exists and is not wired. That is a
// small thing to check. What it buys is that the branch serving them starts
// from a red test rather than from nothing: it has to delete a case here, and
// deleting a case is a decision somebody makes rather than a gap nobody
// notices.
//
// It also pins the shape of the refusal. A method that answered with a struct
// of zeros would look like a broken engine and send whoever saw it to the
// radio, so "there is nothing here yet" has to be a refusal and has to say so
// in words.
//
// THE ORDER THE REFUSAL COMES IN IS PART OF THE CONTRACT
//
// The server refuses before it looks at the arguments, so a receiver id that
// does not exist gets the not-wired sentence and not "no receiver 9 is
// registered". The second reads as though a good id would have worked, and on
// a surface that does nothing that is the one thing a caller must not be told.
//
// SUBSCRIBEAUDIO USED TO BE HERE AND IS NOT
//
// This file carried a third case asserting the same refusal from
// subscribeAudio, and that is exactly what it was for: the branch serving
// audio had to come here and delete it. It did, on 2026-09-20.
// tests/rpc/test_rpc_audio.cpp is what replaced it, and the assertion that
// used to be "it says it is not wired" is now the stream itself, the drop
// counter moving under a slow subscriber, and the eight reachable shapes the
// refusal never had to distinguish.
//
// The one thing that changed shape rather than moving: subscribeAudio now
// reads its arguments and answers about them, so "no receiver 9999 is
// registered" is the right answer there. That inversion is why the two
// remaining cases still assert the opposite here.

#include <cstdint>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/types.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"

using namespace revenant;
using test::Harness;
using test::HarnessOptions;

namespace {

void bring_up(Harness& harness, const HarnessOptions& options) {
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());
}

// The phrase both refusals share, so a client matches one string and the
// branch that serves a surface has one string to delete.
constexpr const char* kNotWired = "is not wired to the engine yet";

}  // namespace

TEST_CASE("the RDS surface is on the wire and says it is not served", "[gpu][rpc][unwired]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    // A receiver built the way the schema says an RDS one will have to be:
    // WFM at 171000, which is three times the 57 kHz subcarrier and 144 times
    // the 1187.5 bit/s bit rate, and clears the 148438 the receiver's own
    // audio filter imposes. It is here so the case sends a request that will
    // still be legal when the surface is served, rather than one the branch
    // turning this green would have to rewrite.
    auto vrx = harness.client().add_vrx(rpc::VrxParams{.center = 131'072,
                                                       .bandwidth = 200'000,
                                                       .demod = rpc::Demod::Wfm,
                                                       .audio_rate = 171'000});
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto station = harness.client().rds_station(*vrx);
    REQUIRE_FALSE(station.has_value());
    INFO(station.error().message);
    CHECK(station.error().message.find("rdsStation") != std::string::npos);
    CHECK(station.error().message.find(kNotWired) != std::string::npos);

    // Both regions, because the argument is read nowhere and a refusal that
    // depended on it would mean the region had reached something.
    for (rpc::RdsRegion region : {rpc::RdsRegion::Rds, rpc::RdsRegion::Rbds}) {
        auto set = harness.client().set_rds_region(*vrx, region);
        REQUIRE_FALSE(set.has_value());
        INFO(set.error().message);
        CHECK(set.error().message.find("setRdsRegion") != std::string::npos);
        CHECK(set.error().message.find(kNotWired) != std::string::npos);
    }

    // And the receiver is still there afterwards. A refusal that had torn
    // something down on the way out would be worse than no surface at all.
    auto ids = harness.client().vrx_ids();
    INFO(test::message_of(ids));
    REQUIRE(ids.has_value());
    CHECK(ids->size() == 1);
}
