// The two surfaces the schema carries and the engine does not serve.
//
// WHAT THESE CASES ARE FOR, WHICH IS NOT WHAT THEY ASSERT
//
// They assert that subscribeAudio, rdsStation and setRdsRegion reach the
// server and come back refused in a sentence saying the surface exists and is
// not wired. That is a small thing to check. What it buys is that the branch
// serving either one starts from a red test rather than from nothing: it has
// to delete a case here, and deleting a case is a decision somebody makes
// rather than a gap nobody notices.
//
// They also pin the shape of the refusal. A method that answered with an
// empty stream or a struct of zeros would look like a broken engine and send
// whoever saw it to the radio, so "there is nothing here yet" has to be a
// refusal and has to say so in words.
//
// THE ORDER THE REFUSAL COMES IN IS PART OF THE CONTRACT
//
// The server refuses before it looks at the arguments, so a receiver id that
// does not exist gets the not-wired sentence and not "no receiver 9 is
// registered". The second reads as though a good id would have worked, and on
// a surface that does nothing that is the one thing a caller must not be told.
// The case below sends a live receiver's id and a nonsense one and asserts
// they get the same answer.

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

// The phrase all three refusals share, so a client matches one string and the
// branch that serves a surface has one string to delete.
constexpr const char* kNotWired = "is not wired to the engine yet";

}  // namespace

TEST_CASE("subscribeAudio is on the wire and says it is not served", "[gpu][rpc][unwired]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    auto vrx = harness.client().add_vrx(
        rpc::VrxParams{.center = 131'072, .bandwidth = 12'000, .demod = rpc::Demod::Nfm});
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto refused = harness.client().subscribe_audio(*vrx, 500);
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);
    CHECK(refused.error().message.find("subscribeAudio") != std::string::npos);
    CHECK(refused.error().message.find(kNotWired) != std::string::npos);

    // A receiver that does not exist gets the SAME answer, because the
    // refusal comes before the arguments are read. Telling this caller "no
    // receiver 9999 is registered" would say a good id works.
    auto nonsense = harness.client().subscribe_audio(9'999, 500);
    REQUIRE_FALSE(nonsense.has_value());
    INFO(nonsense.error().message);
    CHECK(nonsense.error().message.find(kNotWired) != std::string::npos);
    CHECK(nonsense.error().message.find("is registered") == std::string::npos);
}

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
