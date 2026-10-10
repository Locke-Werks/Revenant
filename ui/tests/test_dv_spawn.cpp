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

namespace {

// A P25 track as core/detect/label.cpp reports a refinement: the confirmed
// steps, then the leading unconfirmed one when it has support.
[[nodiscard]] rpc::Detection p25(std::int64_t hz, const std::string& name,
                                 const std::string& next = {})
{
    rpc::Detection d = dv(hz, name);
    d.label.path = {{"NFM", 0.9, true}, {"4FSK", 0.9, true}, {"P25", 0.9, true}};
    if (name == "P25 control") {
        d.label.path.push_back({"control", 0.8, true});
    } else if (name == "P25 voice") {
        d.label.path.push_back({"voice", 0.8, true});
    }
    d.label.confirmed_depth = static_cast<std::uint32_t>(d.label.path.size());
    if (!next.empty()) {
        d.label.path.push_back({next, 0.4, false});
    }
    return d;
}

}  // namespace

TEST_CASE("a P25 control channel gets no receiver, named or leaning")
{
    // Rejects a rule that waits for the bracket to say "P25 control" while the
    // plain "P25" of the first probes opens a receiver on a TSBK carrier.
    const std::vector<rpc::Detection> seen{p25(853'050'100, "P25 control"),
                                           p25(852'000'000, "P25", "control")};
    CHECK(plan_dv_spawns(seen, {}, 64).open.empty());
}

TEST_CASE("plain P25 with no lean, or leaning to voice, spawns")
{
    // Rejects holding every conventional P25 channel until its census is in.
    const std::vector<rpc::Detection> seen{p25(852'000'000, "P25"),
                                           p25(852'500'000, "P25", "voice"),
                                           p25(853'000'000, "P25 voice")};
    CHECK(plan_dv_spawns(seen, {}, 64).open.size() == 3);
}

TEST_CASE("P25 voice beside a control channel spawns like any other")
{
    // Rejects deferring a trunk's voice channels to the tracker: auto DV
    // ignores trunking and opens a receiver on every voice channel it sees.
    const std::vector<rpc::Detection> seen{p25(853'050'100, "P25 control"),
                                           p25(852'312'500, "P25 voice"),
                                           p25(852'712'500, "P25"), dv(441'000'000, "DMR")};
    CHECK(plan_dv_spawns(seen, {}, 64).open.size() == 3);
}

TEST_CASE("a trunk voice channel the tracker already holds is not doubled")
{
    const std::vector<rpc::Detection> seen{p25(853'050'100, "P25 control"),
                                           p25(852'312'500, "P25 voice")};
    const std::vector<double> covered{852'312'400.0};
    CHECK(plan_dv_spawns(seen, covered, 64).open.empty());
}

TEST_CASE("a receiver opened on what turned out to be a control channel is handed back")
{
    using revenant::ui::DvSpawned;
    using revenant::ui::plan_dv_retirements;
    const std::vector<rpc::Detection> seen{p25(853'050'100, "P25 control"),
                                           p25(852'312'500, "P25 voice"), dv(441'000'000, "DMR")};
    const std::vector<DvSpawned> spawned{{1, 853'050'000.0},  // control, a little off
                                         {2, 852'312'500.0},  // trunk voice, kept
                                         {3, 441'000'000.0},  // DMR, kept
                                         {4, 851'000'000.0}}; // nothing there now, kept
    const auto retire = plan_dv_retirements(seen, spawned);
    REQUIRE(retire.size() == 1);
    CHECK(retire[0] == 1);
}

TEST_CASE("a quiet or merged control channel retires nothing")
{
    // Rejects acting on a track the detector no longer stands behind.
    using revenant::ui::DvSpawned;
    rpc::Detection merged = p25(853'050'100, "P25 control");
    merged.state = rpc::TrackState::Merged;
    const std::vector<rpc::Detection> seen{merged, p25(852'312'500, "P25 voice")};
    const std::vector<DvSpawned> spawned{{1, 853'050'100.0}, {2, 852'312'500.0}};
    CHECK(revenant::ui::plan_dv_retirements(seen, spawned).empty());
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
