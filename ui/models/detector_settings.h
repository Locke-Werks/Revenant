// What the operator set on the detector, and what survives a tune, a new
// source, a new engine and a restart of this window.
//
// THREE VALUES, TWO OWNERS. The confidence bar ("held for") and the margin bar
// ("stronger than") are this window's: they are the arguments to each
// detections() poll and filter what comes back, so nothing on the engine
// changes when they move. The detection threshold is the engine's, one value
// for every client, and the last writer wins.
//
// WHAT RESET, AND WHERE IT IS FIXED. On 2026-09-23 the owner reported the
// detector's settings not holding between tunes. The two bars never reset on
// a tune: they live in EngineLink and are sent with every poll. The threshold
// did. Every retune throws the engine's detector away, because its tracks
// describe a band it is no longer looking at, and core/rpc/server.cpp built
// the next one at DetectorConfig's default of 6 dB, so the slider went on
// showing the operator's value over a detector deciding at another. The
// engine keeps the threshold across a rebuild now (detection_threshold_db_ in
// core/rpc/server.cpp, with its case in tests/rpc/test_rpc_detect.cpp), which
// is the right owner: the reset hit every client, and a client re-sending
// after each tune would be one window papering over a fault all of them had.
//
// WHAT THIS FILE ADDS is the other half the owner asked for, a restart. The
// engine forgets everything when it stops, and this window forgot the margin
// bar and never held the threshold at all. All three are remembered now, and a
// new connection re-sends the threshold the operator last chose, so an engine
// started fresh decides at the operator's value rather than at 6 dB.
//
// Qt-free on the rule the rest of ui/models follows, so ui/tests can hold the
// restore and the re-send to cases.

#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace revenant::ui {

// The interval core/rpc/server.cpp accepts a threshold in. A remembered value
// outside it is a settings file somebody edited, and sending it would put a
// refusal on screen at every connection for a value nobody chose this session.
inline constexpr double kDetectionThresholdFloorDb = -60.0;
inline constexpr double kDetectionThresholdCeilingDb = 120.0;

// A remembered bar, as the link should hold it. Clamped into [0, top] where
// top is the largest value the engine takes, NaN and anything unreadable to
// the floor, which passes everything and is what nobody having touched the
// control means.
[[nodiscard]] inline double restore_detection_bar(std::optional<double> stored, double top)
{
    if (!stored.has_value() || std::isnan(*stored)) {
        return 0.0;
    }
    return std::clamp(*stored, 0.0, top);
}

// A remembered threshold, or none. Refused rather than clamped: a bar clamped
// to its stop still filters in the direction the operator meant, and a
// threshold clamped from 500 to 120 is a detector that finds nothing, which
// is a claim about the band nobody made.
[[nodiscard]] inline std::optional<double> restore_detection_threshold(
    std::optional<double> stored)
{
    if (!stored.has_value() || !std::isfinite(*stored) ||
        *stored < kDetectionThresholdFloorDb || *stored > kDetectionThresholdCeilingDb) {
        return std::nullopt;
    }
    return stored;
}

// What the operator last set, as this window holds it between launches.
struct DetectorMemory {
    double confidence_bar = 0.0;
    double margin_bar = 0.0;

    // Empty until the operator has moved the threshold from this window on
    // this machine. Until then the engine's own value stands and nothing is
    // sent, so a first run does not impose a number on an engine another
    // client has already set.
    std::optional<double> threshold_db;
};

[[nodiscard]] inline DetectorMemory restore_detector_memory(std::optional<double> confidence,
                                                            std::optional<double> margin,
                                                            std::optional<double> threshold,
                                                            double top)
{
    DetectorMemory out;
    out.confidence_bar = restore_detection_bar(confidence, top);
    out.margin_bar = restore_detection_bar(margin, top);
    out.threshold_db = restore_detection_threshold(threshold);
    return out;
}

// The threshold a new connection should send before its first poll, or none.
//
// Every connection and not only the first, because a connection is also what
// a restarted engine looks like from here, and that engine starts at 6 dB.
// Against an engine that was already running at this value the write changes
// nothing. Against one another client has set since, this window's value wins,
// which is the last-writer rule the threshold has always had: the operator
// set it here, and this is them setting it again.
[[nodiscard]] inline std::optional<double> threshold_for_connection(const DetectorMemory& memory)
{
    return memory.threshold_db;
}

// The handle's value for the threshold control: what the operator asked for,
// or what is in force when they never have.
[[nodiscard]] inline double threshold_wanted(const DetectorMemory& memory, double in_force)
{
    return memory.threshold_db.value_or(in_force);
}

// Whether the value in force has parted from what this window asked for by
// more than the control's own step. The engine answers a write on the next
// poll, so a gap that survives a decision is another client, not this one in
// flight. Nothing to report when this window has asked for nothing.
[[nodiscard]] inline bool threshold_overridden(const DetectorMemory& memory, double in_force,
                                               bool decided)
{
    return decided && memory.threshold_db.has_value() &&
           std::abs(*memory.threshold_db - in_force) > 0.6;
}

}  // namespace revenant::ui
