// Automatic digital voice receivers: which detections get a receiver of their
// own.
//
// THE OWNER'S REQUEST OF 2026-10-02. "spawn vrx's for all detected P25 on the
// spectrum. Once detected, the vrx remains. New ones get added on new
// frequencies as they're detected." So while the switch is on, every
// detection on a frequency no receiver already covers gets a held receiver,
// heard, and nothing this rule does ever removes one. The rack limit went the
// same day so that "all" can mean all; models/receiver_rack.h.
//
// AND OF 2026-10-03: "I want AutoP25 to become AutoDV and it should spawn a
// receiver for any detected DV protocol." So the rule is the same and the
// protocols are every digital voice one core/identify verifies: P25, DMR,
// D-STAR, TETRA and M17.
//
// WHAT COUNTS AS DIGITAL VOICE. A track the label rule named one of those
// protocols and allowed to drive a receiver, which means core/identify checked
// that protocol's frame sync on it: a verified protocol, not a modulation
// guess. Live or held, because a held track is one the detector still stands
// behind and conventional voice channels are on the air only for a call; a
// merged one has been folded into another track and is that track's to report.
//
// THE RECEIVER'S MODE is the one a click on the same detection sets,
// label_tune in models/label_tune.h, so a spawned receiver and a clicked one
// cannot disagree: p25p1, dmr, dstar and tetra for their own protocols, and
// p25p1 for M17, whose 48000 S/s complex baseband is what the m17 decoder
// reads.
//
// WHAT COUNTS AS COVERED. A receiver of any mode, placed by hand or by this,
// whose centre is within half the detected protocol's channel of the
// detection's. A receiver the operator put there on purpose is theirs and is
// not doubled, and a detection a few hundred hertz off a spawned receiver's
// centre is the same transmitter measured again. Two detections in one pass
// inside that distance of each other get one receiver, the stronger's.
//
// AND OF 2026-10-08: "autodv should ignore control channels." A P25 control
// channel carries TSBKs and no voice:
//   - a detection core/detect/refine.h has named "P25 control", or a P25
//     whose next step it leans to is "control", gets no receiver. That second
//     clause matters because the bracket reads plain "P25" for the first
//     probes, and a receiver opened then was never taken back;
//   - a receiver this rule opened that a later pass finds on one is handed
//     back by plan_dv_retirements, so the early misreading heals.
// All of it from the detector's own reading of the TSBK census, not from a
// frequency list.
//
// AND OF 2026-10-10: "It should ignore trunking when it's enabled and just
// spawn vrx's for every detected voice channel that pops up." A trunked
// system's voice channels are spawned like any other; a receiver the trunk
// tracker already holds on one covers it, so the call is not doubled.
//
// NOT HERE: what a spawned receiver is called, its audio, its decoder. The
// caller opens a held receiver in the mode above. P25 plays decoded voice in
// place of discriminator audio, core/rpc/voice_audio.h, and D-STAR and DMR
// play through a vocoder plugin when one is loaded; every one of them is
// silent between calls. TETRA makes no audio and M17's receiver plays nothing,
// so those two are for the decode log once focused.
//
// Qt-free and header-only, so ui/tests holds the rule on its own.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/rpc/types.h"
#include "models/label_tune.h"
#include "models/mode_choice.h"

namespace revenant::ui {

struct DvProtocol {
    // As core/identify/identify.cpp's protocol_name gives it.
    std::string_view name;

    // Half the protocol's channel: how far a receiver's centre may sit from a
    // detection and still be the same transmitter. The channels are the ones
    // dsp::default_passband in core/dsp/vrx_reference.cpp cites for each mode.
    double tolerance_hz = 0.0;
};

inline constexpr std::array<DvProtocol, 6> kDvProtocols = {{
    // TIA-102.BAAA-A: a 12.5 kHz FDMA channel.
    {"P25", 6'250.0},
    // A P25 voice channel, as core/detect/refine.h names one once its data
    // units say so. A P25 control channel is left out on purpose: it carries
    // no voice for a spawned receiver to play.
    {"P25 voice", 6'250.0},
    // TS 102 361-1 clause 10.1.2: a 12.5 kHz RF carrier bandwidth.
    {"DMR", 6'250.0},
    // The JARL system specification: carrier spacing 6.25 kHz or more.
    {"D-STAR", 3'125.0},
    // EN 300 392-2 clauses 5.3 and 5.5: a 25 kHz channel.
    {"TETRA", 12'500.0},
    // M17 is received on the p25p1 mode's 12.5 kHz window, and its 4FSK at
    // 4800 symbols a second deviating 2.4 kHz at the outer symbols (M17
    // Table 1.1, cited in core/decode/m17.h) is about 9.6 kHz by Carson,
    // inside it.
    {"M17", 6'250.0},
}};

[[nodiscard]] constexpr const DvProtocol* find_dv_protocol(std::string_view name)
{
    for (const DvProtocol& protocol : kDvProtocols) {
        if (protocol.name == name) {
            return &protocol;
        }
    }
    return nullptr;
}

// The receiver mode a click on this detection would set, when it is one of
// the protocols above and may drive a receiver. Nothing otherwise.
[[nodiscard]] inline std::optional<rpc::Demod> dv_spawn_demod(const rpc::Detection& detection)
{
    if (detection.label.kind != rpc::LabelKind::Protocol ||
        find_dv_protocol(detection.label.name) == nullptr) {
        return std::nullopt;
    }
    const LabelTune tune =
        label_tune(detection.label, static_cast<double>(detection.bandwidth_hz));
    if (!tune.drives) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < kDemodNames.size(); ++i) {
        if (kDemodNames[i] == tune.mode) {
            return static_cast<rpc::Demod>(i);
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline bool is_current_track(const rpc::Detection& detection)
{
    return detection.state == rpc::TrackState::Live || detection.state == rpc::TrackState::Held;
}

// A P25 track the refinement has confirmed as a control channel, or a plain
// "P25" whose leading unconfirmed step is "control": refine.h's census has
// already counted TSBKs on it, or it has been on the air unbroken, and the
// bar has not been reached yet. The step names are refine.cpp's.
[[nodiscard]] inline bool is_p25_control(const rpc::Detection& detection)
{
    if (detection.label.name == "P25 control") {
        return true;
    }
    if (detection.label.name != "P25") {
        return false;
    }
    const auto& path = detection.label.path;
    const std::size_t confirmed = std::min<std::size_t>(detection.label.confirmed_depth, path.size());
    return confirmed < path.size() && !path[confirmed].confirmed && path[confirmed].name == "control";
}

[[nodiscard]] inline bool is_spawnable_dv(const rpc::Detection& detection)
{
    return is_current_track(detection) && dv_spawn_demod(detection).has_value() &&
           !is_p25_control(detection);
}

// One receiver to open, or one there was no room for.
struct DvSpawn {
    double centre_hz = 0.0;
    double bandwidth_hz = 0.0;
    rpc::Demod demod = rpc::Demod::Raw;
    std::string_view protocol;
    double tolerance_hz = 0.0;
};

struct DvSpawnPlan {
    std::vector<DvSpawn> open;

    // Wanted and not opened because the rack had no room, strongest first.
    std::vector<DvSpawn> no_room;
};

// `covered_hz` is the centre of every receiver the rack holds, and `room` how
// many more it will take.
[[nodiscard]] inline DvSpawnPlan plan_dv_spawns(std::span<const rpc::Detection> detections,
                                                std::span<const double> covered_hz,
                                                std::size_t room)
{
    std::vector<const rpc::Detection*> wanted;
    for (const rpc::Detection& detection : detections) {
        if (is_spawnable_dv(detection)) {
            wanted.push_back(&detection);
        }
    }

    // Strongest first, so the receiver two close detections share is on the
    // stronger, and so a rack with room for some takes the clearest.
    std::stable_sort(wanted.begin(), wanted.end(), [](const auto* a, const auto* b) {
        return a->snr_2500_db > b->snr_2500_db;
    });

    DvSpawnPlan plan;
    for (const rpc::Detection* detection : wanted) {
        const DvProtocol& protocol = *find_dv_protocol(detection->label.name);
        const auto centre = static_cast<double>(detection->center_hz);
        auto near = [&](double hz) { return std::abs(hz - centre) <= protocol.tolerance_hz; };
        auto near_spawn = [&](const DvSpawn& s) { return near(s.centre_hz); };
        const bool taken = std::any_of(covered_hz.begin(), covered_hz.end(), near) ||
                           std::any_of(plan.open.begin(), plan.open.end(), near_spawn) ||
                           std::any_of(plan.no_room.begin(), plan.no_room.end(), near_spawn);
        if (taken) {
            continue;
        }
        const DvSpawn spawn{centre, static_cast<double>(detection->bandwidth_hz),
                            *dv_spawn_demod(*detection), protocol.name, protocol.tolerance_hz};
        if (plan.open.size() < room) {
            plan.open.push_back(spawn);
        } else {
            plan.no_room.push_back(spawn);
        }
    }
    return plan;
}

// A receiver this rule opened, by rack key and centre.
struct DvSpawned {
    std::uint64_t key = 0;
    double centre_hz = 0.0;
};

// The receivers this rule opened that now sit on a control channel, to be
// removed. Only its own: a receiver the operator placed on a control channel
// is theirs. A receiver is handed back only on a current detection that says
// so; one whose detection has gone quiet keeps its place, as the 2026-10-02
// rule wants.
//
// The distance is P25's half channel, since every case here is P25.
[[nodiscard]] inline std::vector<std::uint64_t>
plan_dv_retirements(std::span<const rpc::Detection> detections, std::span<const DvSpawned> spawned)
{
    const double tolerance = find_dv_protocol("P25")->tolerance_hz;
    std::vector<std::uint64_t> retire;
    for (const DvSpawned& receiver : spawned) {
        const bool owned =
            std::any_of(detections.begin(), detections.end(), [&](const rpc::Detection& d) {
                return is_current_track(d) &&
                       std::abs(static_cast<double>(d.center_hz) - receiver.centre_hz) <=
                           tolerance &&
                       is_p25_control(d);
            });
        if (owned) {
            retire.push_back(receiver.key);
        }
    }
    return retire;
}

}  // namespace revenant::ui
