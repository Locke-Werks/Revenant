// A device's calibration over the wire.
//
// The engine's half is tests/engine/test_calibration.cpp and
// test_front_end_correction.cpp. What these add is the seam: every field of
// CalibrationSettings arrives, every field of Calibration comes back rather
// than its default, a new correction moves EngineInfo::sourceCenter as a
// client reads it, and the front-end stage's estimate crosses once the engine
// has run. The synthetic scene carries no serial, so nothing is written to a
// file here; the file is the engine's and is tested there.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "core/rpc/client.h"
#include "core/rpc/types.h"
#include "core/source/frequency_correction.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"

using namespace revenant;
using test::Harness;
using test::HarnessOptions;

namespace {

// A centre that is not a round number, so a correction of it is not either.
constexpr std::int64_t kCenterHz = 145'512'345;

}  // namespace

TEST_CASE("a calibration is set and read back whole over the wire", "[gpu][rpc][calibration]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    HarnessOptions options;
    options.center_hz = kCenterHz;
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    auto first = harness.client().calibration();
    INFO(test::message_of(first));
    REQUIRE(first.has_value());
    CHECK(first->open);
    CHECK(first->key.empty());
    CHECK(first->settings == rpc::CalibrationSettings{});
    CHECK(first->correction_applied);
    CHECK(!first->persisted);
    CHECK(first->note.find("no serial") != std::string::npos);
    CHECK(first->device_center_hz == kCenterHz);
    CHECK(!first->measured);

    // Every field away from its default.
    const rpc::CalibrationSettings wanted{
        .correction_ppb = -12'345, .dc_removal = true, .iq_correction = true};
    auto set = harness.client().set_calibration(wanted);
    INFO(test::message_of(set));
    REQUIRE(set.has_value());
    CHECK(set->settings == wanted);
    CHECK(set->device_center_hz == kCenterHz);

    // The numbers move and the radio does not: the device is where it was and
    // the centre a client reads is where it really listens.
    auto info = harness.client().info();
    REQUIRE(info.has_value());
    CHECK(info->source_center == source::device_to_true(kCenterHz, -12'345));
    CHECK(info->source_center == 145'510'549);

    auto read_back = harness.client().calibration();
    REQUIRE(read_back.has_value());
    CHECK(read_back->settings == wanted);

    // Refused in the engine's words past a thousand ppm, and nothing changes.
    auto refused = harness.client().set_calibration({.correction_ppb = 2'000'000});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.find("crystal error") != std::string::npos);
    auto unchanged = harness.client().calibration();
    REQUIRE(unchanged.has_value());
    CHECK(unchanged->settings == wanted);
}

TEST_CASE("the front-end estimate crosses the wire once the engine has run",
          "[gpu][rpc][calibration]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    const auto ready = harness.open(HarnessOptions{});
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    REQUIRE(harness.client().set_calibration({.dc_removal = true}).has_value());
    REQUIRE(harness.start_engine().has_value());
    const std::uint64_t blocks = harness.wait_for_blocks(40, 20'000);
    INFO("blocks delivered " << blocks);
    REQUIRE(blocks >= 40);

    auto state = harness.client().calibration();
    INFO(test::message_of(state));
    REQUIRE(state.has_value());
    INFO("measured over " << state->blocks_measured << " blocks: DC " << state->dc_dbfs
                          << " dBFS, gain " << state->gain_error_db << " dB, phase "
                          << state->phase_error_deg << " deg, image "
                          << state->image_rejection_db << " dB");
    CHECK(state->measured);
    CHECK(state->blocks_measured > 0);

    // A synthetic scene is generated around an exact zero, so its DC is its
    // noise's mean: far below anything a receiver shows, and not the -300 an
    // unmeasured stream carries.
    CHECK(state->dc_dbfs < -60.0);
    CHECK(state->dc_dbfs > -300.0);

    REQUIRE(harness.stop_engine().has_value());
}
