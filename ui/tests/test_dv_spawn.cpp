// Which digital voice detections get a receiver of their own. Each case names
// the wrong implementation it rejects.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "models/dv_spawn.h"
#include "models/label_tune.h"

namespace rpc = revenant::rpc;
using revenant::ui::plan_dv_spawns;

namespace {

[[nodiscard]] rpc::Detection dv(std::int64_t hz, const std::string& protocol = "P25",
                                double snr = 20.0, rpc::TrackState state = rpc::TrackState::Live)
{
    rpc::Detection d;
    d.center_hz = hz;
    d.bandwidth_hz = 9'000;
    d.snr_2500_db = snr;
    d.state = state;
    d.label.kind = rpc::LabelKind::Protocol;
    d.label.name = protocol;
    d.label.may_drive = true;
    return d;
}

}  // namespace

TEST_CASE("every P25 frequency nobody covers gets a receiver")
{
    const std::vector<rpc::Detection> seen{dv(852'312'500), dv(853'712'500)};
    const auto plan = plan_dv_spawns(seen, {}, 64);
    REQUIRE(plan.open.size() == 2);
    CHECK(plan.no_room.empty());
}

TEST_CASE("every digital voice protocol spawns, in the mode a click would set")
{
    // Rejects a switch that still spawns P25 alone, and a spawned receiver whose
    // mode disagrees with the one label_tune gives a click on the same signal:
    // M17 in particular is received on p25p1, not on a mode of its own.
    const std::vector<rpc::Detection> seen{dv(440'000'000, "P25"), dv(441'000'000, "DMR"),
                                           dv(442'000'000, "D-STAR"), dv(443'000'000, "TETRA"),
                                           dv(444'000'000, "M17")};
    const auto plan = plan_dv_spawns(seen, {}, 64);
    REQUIRE(plan.open.size() == 5);
    for (const auto& spawn : plan.open) {
        const rpc::Detection* source = nullptr;
        for (const auto& d : seen) {
            if (static_cast<double>(d.center_hz) == spawn.centre_hz) {
                source = &d;
            }
        }
        REQUIRE(source != nullptr);
        const auto tune = revenant::ui::label_tune(source->label, 9'000.0);
        CHECK(revenant::ui::kDemodNames[static_cast<std::size_t>(spawn.demod)] == tune.mode);
        CHECK(spawn.protocol == source->label.name);
    }
}

TEST_CASE("a protocol that is not digital voice does not spawn")
{
    // Rejects spawning on every verified protocol: a POCSAG pager or an AX.25
    // packet is data, and an RDS station is broadcast FM.
    const std::vector<rpc::Detection> seen{dv(152'000'000, "POCSAG"), dv(144'390'000, "AX.25"),
                                           dv(98'100'000, "RDS"), dv(14'080'000, "RTTY")};
    CHECK(plan_dv_spawns(seen, {}, 64).open.empty());
}

TEST_CASE("a frequency a receiver already covers is left alone")
{
    // Rejects doubling a receiver the operator placed by hand, or one this
    // spawned last pass whose detection has drifted a few hundred hertz.
    const std::vector<rpc::Detection> seen{dv(852'312'500)};
    const std::vector<double> covered{852'312'900.0};
    CHECK(plan_dv_spawns(seen, covered, 64).open.empty());
}

TEST_CASE("the next channel over is a new frequency")
{
    // Rejects a tolerance as wide as a channel, which would refuse the
    // neighbour 12.5 kHz away.
    const std::vector<rpc::Detection> seen{dv(852'325'000)};
    const std::vector<double> covered{852'312'500.0};
    CHECK(plan_dv_spawns(seen, covered, 64).open.size() == 1);
}

TEST_CASE("the tolerance is the detected protocol's channel, not P25's")
{
    // Rejects one 6.25 kHz tolerance for every protocol. D-STAR's channels
    // are 6.25 kHz apart, so a receiver 6.25 kHz away is the neighbour; a
    // TETRA receiver 10 kHz off a TETRA detection is inside its 25 kHz channel.
    const std::vector<rpc::Detection> dstar{dv(145'006'250, "D-STAR")};
    const std::vector<double> dstar_covered{145'000'000.0};
    CHECK(plan_dv_spawns(dstar, dstar_covered, 64).open.size() == 1);

    const std::vector<rpc::Detection> tetra{dv(390'010'000, "TETRA")};
    const std::vector<double> tetra_covered{390'000'000.0};
    CHECK(plan_dv_spawns(tetra, tetra_covered, 64).open.empty());
}

TEST_CASE("two detections of one channel in a pass get one receiver, on the stronger")
{
    const std::vector<rpc::Detection> seen{dv(852'312'000, "P25", 10.0),
                                           dv(852'313'000, "P25", 25.0)};
    const auto plan = plan_dv_spawns(seen, {}, 64);
    REQUIRE(plan.open.size() == 1);
    CHECK(plan.open.front().centre_hz == 852'313'000.0);
}

TEST_CASE("only a verified digital voice label spawns")
{
    // Rejects spawning on anything with a protocol's name, on a label that may
    // not drive a receiver, and on a merged track.
    rpc::Detection nfm = dv(851'000'000);
    nfm.label.kind = rpc::LabelKind::AnalogModulation;
    nfm.label.name = "NFM";
    rpc::Detection undriven = dv(851'100'000, "DMR");
    undriven.label.may_drive = false;
    const rpc::Detection merged = dv(851'200'000, "D-STAR", 20.0, rpc::TrackState::Merged);
    const rpc::Detection held = dv(851'300'000, "DMR", 20.0, rpc::TrackState::Held);

    const std::vector<rpc::Detection> seen{nfm, undriven, merged, held};
    const auto plan = plan_dv_spawns(seen, {}, 64);
    REQUIRE(plan.open.size() == 1);
    CHECK(plan.open.front().centre_hz == 851'300'000.0);
}

TEST_CASE("a full rack reports what it could not open, strongest first")
{
    const std::vector<rpc::Detection> seen{dv(851'000'000, "P25", 10.0),
                                           dv(852'000'000, "DMR", 30.0),
                                           dv(853'000'000, "D-STAR", 20.0)};
    const auto plan = plan_dv_spawns(seen, {}, 1);
    REQUIRE(plan.open.size() == 1);
    CHECK(plan.open.front().centre_hz == 852'000'000.0);
    REQUIRE(plan.no_room.size() == 2);
    CHECK(plan.no_room.front().centre_hz == 853'000'000.0);
    CHECK(plan.no_room.front().protocol == "D-STAR");
}
