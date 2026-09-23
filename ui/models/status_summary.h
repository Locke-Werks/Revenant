// How loudly the main window speaks about the engine, and what it says first.
//
// The window used to print every diagnostic it had as a row of its own, all
// the time: frame counters, the source's pace, the engine's clamp reason, the
// front end's note, receivers other windows hold, the reconnect error. Each
// row was there for a reason written beside it, and together they pushed the
// spectrum down the window and made a healthy engine look like a list of
// complaints. The owner's word for it on 2026-09-22 was cluttered.
//
// So the rows now live in a status drawer that opens on demand, and a pill in
// the top bar summarises them in a word or two. What this header decides is
// the part that can be wrong: which conditions are faults that come forward
// on their own, which are notes that wait to be asked for, and which one the
// pill names when several are true at once. A fault demoted to a note is one
// an operator does not find until they wonder why the band is empty; a note
// promoted to a fault is a banner that is always up and soon ignored.
//
// THE LEVELS
//
// Bad: the picture on screen is not the radio's. No engine, a source that is
// behind real time, a front end that is overloaded, a detector that refuses.
// Warn: something the operator did not get. An engine that is stopped, no
// source open, a refused tune or radio, a receiver the engine let go.
// Note: true and ordinary. A clamp, frames the engine dropped, receivers
// other windows hold. None of these is a fault, and each one's own row says
// why.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace revenant::ui {

enum class StatusLevel : std::uint8_t {
    Quiet,
    Note,
    Warn,
    Bad,
};

struct StatusInputs {
    bool connected = false;
    bool engine_running = false;
    bool source_open = false;
    bool source_behind = false;
    bool front_end_fault = false;
    bool detection_fault = false;
    bool tune_fault = false;
    bool source_fault = false;
    bool receiver_gone = false;
    bool clamped = false;
    bool stranded = false;
    std::uint64_t frames_dropped_by_engine = 0;
    double frame_rate = 0.0;
};

struct StatusSummary {
    StatusLevel level = StatusLevel::Quiet;

    // A word or two for the pill. The sentences are the rows' own and are in
    // the drawer or the banner; this only says which one to look at.
    std::string headline;
};

[[nodiscard]] inline StatusSummary summarise_status(const StatusInputs& in)
{
    // In order of what the operator most needs to know first. The first true
    // one names the pill; the level is the worst of them all, which is the
    // same thing because the list is sorted by level.
    if (!in.connected) {
        return {StatusLevel::Bad, "no engine"};
    }
    if (in.front_end_fault) {
        return {StatusLevel::Bad, "front end overloaded"};
    }
    if (in.source_behind) {
        return {StatusLevel::Bad, "source behind"};
    }
    if (in.detection_fault) {
        return {StatusLevel::Bad, "detector refused"};
    }
    if (!in.engine_running) {
        return {StatusLevel::Warn, "engine stopped"};
    }
    if (!in.source_open) {
        return {StatusLevel::Warn, "no source open"};
    }
    if (in.source_fault) {
        return {StatusLevel::Warn, "radio refused"};
    }
    if (in.tune_fault) {
        return {StatusLevel::Warn, "tune refused"};
    }
    if (in.receiver_gone) {
        return {StatusLevel::Warn, "receiver let go"};
    }

    // Quiet or a note: the pill says the rate, which is the one number that
    // says the engine is alive, and the level says whether the drawer holds
    // anything worth opening it for.
    char rate[32] = {};
    std::snprintf(rate, sizeof rate, "%.1f rows/s", in.frame_rate);
    const bool note = in.clamped || in.stranded || in.frames_dropped_by_engine > 0;
    return {note ? StatusLevel::Note : StatusLevel::Quiet, rate};
}

}  // namespace revenant::ui
