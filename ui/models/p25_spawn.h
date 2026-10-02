// Automatic P25 receivers: which detections get a receiver of their own.
//
// THE OWNER'S REQUEST OF 2026-10-02. "spawn vrx's for all detected P25 on the
// spectrum. Once detected, the vrx remains. New ones get added on new
// frequencies as they're detected." So while the switch is on, every P25
// detection on a frequency no receiver already covers gets a held p25p1
// receiver, heard, and nothing this rule does ever removes one. The rack limit
// went the same day so that "all" can mean all; models/receiver_rack.h.
//
// WHAT COUNTS AS P25. A track the label rule named the P25 protocol and allowed
// to drive a receiver, which means core/identify checked a P25 frame sync on it:
// a verified protocol, not a modulation guess. Live or held, because a held
// track is one the detector still stands behind and P25 conventional channels
// are on the air only for a call; a merged one has been folded into another
// track and is that track's to report.
//
// WHAT COUNTS AS COVERED. A receiver of any mode, placed by hand or by this,
// whose centre is within half a P25 channel of the detection's. A receiver the
// operator put there on purpose is theirs and is not doubled, and a detection
// a few hundred hertz off a spawned receiver's centre is the same transmitter
// measured again. Two detections in one pass inside half a channel of each
// other get one receiver, the stronger's.
//
// NOT HERE: what a spawned receiver is called, its mode, its audio. The caller
// opens a p25p1 receiver, which plays decoded voice in place of discriminator
// audio, core/rpc/voice_audio.h, and is silent between calls.
//
// Qt-free and header-only, so ui/tests holds the rule on its own.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "core/rpc/types.h"

namespace revenant::ui {

// Half the 12.5 kHz P25 Phase 1 channel.
inline constexpr double kP25SpawnToleranceHz = 6250.0;

// The protocol name core/identify gives P25 Phase 1, and the one label_tune
// maps to the p25p1 mode.
inline constexpr std::string_view kP25LabelName = "P25";

[[nodiscard]] inline bool is_spawnable_p25(const rpc::Detection& detection)
{
    return detection.label.kind == rpc::LabelKind::Protocol &&
           detection.label.name == kP25LabelName && detection.label.may_drive &&
           (detection.state == rpc::TrackState::Live || detection.state == rpc::TrackState::Held);
}

// One receiver to open.
struct P25Spawn {
    double centre_hz = 0.0;
    double bandwidth_hz = 0.0;
};

struct P25SpawnPlan {
    std::vector<P25Spawn> open;

    // Wanted and not opened because the rack had no room, strongest first.
    std::vector<double> no_room_hz;
};

// `covered_hz` is the centre of every receiver the rack holds, and `room` how
// many more it will take.
[[nodiscard]] inline P25SpawnPlan plan_p25_spawns(std::span<const rpc::Detection> detections,
                                                  std::span<const double> covered_hz,
                                                  std::size_t room)
{
    std::vector<const rpc::Detection*> wanted;
    for (const rpc::Detection& detection : detections) {
        if (is_spawnable_p25(detection)) {
            wanted.push_back(&detection);
        }
    }

    // Strongest first, so the receiver two close detections share is on the
    // stronger, and so a rack with room for some takes the clearest.
    std::stable_sort(wanted.begin(), wanted.end(), [](const auto* a, const auto* b) {
        return a->snr_2500_db > b->snr_2500_db;
    });

    auto near = [](double a, double b) { return std::abs(a - b) <= kP25SpawnToleranceHz; };

    P25SpawnPlan plan;
    for (const rpc::Detection* detection : wanted) {
        const auto centre = static_cast<double>(detection->center_hz);
        const bool taken =
            std::any_of(covered_hz.begin(), covered_hz.end(),
                        [&](double hz) { return near(hz, centre); }) ||
            std::any_of(plan.open.begin(), plan.open.end(),
                        [&](const P25Spawn& s) { return near(s.centre_hz, centre); }) ||
            std::any_of(plan.no_room_hz.begin(), plan.no_room_hz.end(),
                        [&](double hz) { return near(hz, centre); });
        if (taken) {
            continue;
        }
        if (plan.open.size() < room) {
            plan.open.push_back({centre, static_cast<double>(detection->bandwidth_hz)});
        } else {
            plan.no_room_hz.push_back(centre);
        }
    }
    return plan;
}

}  // namespace revenant::ui
