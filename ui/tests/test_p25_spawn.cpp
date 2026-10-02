// Which P25 detections get a receiver of their own. Each case names the wrong
// implementation it rejects.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "models/p25_spawn.h"

namespace rpc = revenant::rpc;
using revenant::ui::plan_p25_spawns;

namespace {

[[nodiscard]] rpc::Detection p25(std::int64_t hz, double snr = 20.0,
                                 rpc::TrackState state = rpc::TrackState::Live)
{
    rpc::Detection d;
    d.center_hz = hz;
    d.bandwidth_hz = 9'000;
    d.snr_2500_db = snr;
    d.state = state;
    d.label.kind = rpc::LabelKind::Protocol;
    d.label.name = "P25";
    d.label.may_drive = true;
    return d;
}

}  // namespace

TEST_CASE("every P25 frequency nobody covers gets a receiver")
{
    const std::vector<rpc::Detection> seen{p25(852'312'500), p25(853'712'500)};
    const auto plan = plan_p25_spawns(seen, {}, 64);
    REQUIRE(plan.open.size() == 2);
    CHECK(plan.no_room_hz.empty());
}

TEST_CASE("a P25 frequency a receiver already covers is left alone")
{
    // Rejects doubling a receiver the operator placed by hand, or one this
    // spawned last pass whose detection has drifted a few hundred hertz.
    const std::vector<rpc::Detection> seen{p25(852'312'500)};
    const std::vector<double> covered{852'312'900.0};
    CHECK(plan_p25_spawns(seen, covered, 64).open.empty());
}

TEST_CASE("the next channel over is a new frequency")
{
    // Rejects a tolerance as wide as a channel, which would refuse the
    // neighbour 12.5 kHz away.
    const std::vector<rpc::Detection> seen{p25(852'325'000)};
    const std::vector<double> covered{852'312'500.0};
    CHECK(plan_p25_spawns(seen, covered, 64).open.size() == 1);
}

TEST_CASE("two detections of one channel in a pass get one receiver, on the stronger")
{
    const std::vector<rpc::Detection> seen{p25(852'312'000, 10.0), p25(852'313'000, 25.0)};
    const auto plan = plan_p25_spawns(seen, {}, 64);
    REQUIRE(plan.open.size() == 1);
    CHECK(plan.open.front().centre_hz == 852'313'000.0);
}

TEST_CASE("only a verified P25 label spawns")
{
    // Rejects spawning on anything with P25 in its name, on a label that may
    // not drive a receiver, and on a merged track.
    rpc::Detection nfm = p25(851'000'000);
    nfm.label.kind = rpc::LabelKind::AnalogModulation;
    nfm.label.name = "NFM";
    rpc::Detection undriven = p25(851'100'000);
    undriven.label.may_drive = false;
    const rpc::Detection merged = p25(851'200'000, 20.0, rpc::TrackState::Merged);
    const rpc::Detection held = p25(851'300'000, 20.0, rpc::TrackState::Held);

    const std::vector<rpc::Detection> seen{nfm, undriven, merged, held};
    const auto plan = plan_p25_spawns(seen, {}, 64);
    REQUIRE(plan.open.size() == 1);
    CHECK(plan.open.front().centre_hz == 851'300'000.0);
}

TEST_CASE("a full rack reports what it could not open, strongest first")
{
    const std::vector<rpc::Detection> seen{p25(851'000'000, 10.0), p25(852'000'000, 30.0),
                                           p25(853'000'000, 20.0)};
    const auto plan = plan_p25_spawns(seen, {}, 1);
    REQUIRE(plan.open.size() == 1);
    CHECK(plan.open.front().centre_hz == 852'000'000.0);
    REQUIRE(plan.no_room_hz.size() == 2);
    CHECK(plan.no_room_hz.front() == 853'000'000.0);
}
