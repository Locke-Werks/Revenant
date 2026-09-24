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

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

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

    // A word or two for the pill. When it names a condition that has a chip,
    // the pill IS that chip: the top bar gives it the chip's sentence on hover
    // and does not draw the chip again.
    std::string headline;

    // Every other condition that is a chip and is true, in the order below,
    // for the top bar to draw beside the pill. Never the headline.
    std::vector<std::string> chips;
};

// ONE PLACE FOR EACH NOTICE, IN THE TOP BAR.
//
// Until 2026-09-23 the faults also came forward in a strip under the top bar,
// ui/qml/NoticeBanner.qml, as chips, and the pill named the first of them as
// well: a receiver the engine let go read "receiver let go" beside the
// receivers button and again in the strip under it. The owner asked for the
// strip's row to hold controls instead. So a condition with a chip is now
// either the pill's headline, with the sentence on the pill's hover, or a
// chip beside the pill; never both, and never under the bar.
struct StatusCondition {
    bool on = false;
    StatusLevel level = StatusLevel::Quiet;
    const char* label = "";

    // Whether it is a chip with a sentence behind it. "engine stopped" and "no
    // source open" are states the pill names and the drawer explains; they
    // had no chip in the strip either.
    bool chip = true;
};

[[nodiscard]] inline std::array<StatusCondition, 9> status_conditions(const StatusInputs& in)
{
    // In order of what the operator most needs to know first. The first true
    // one names the pill; the level is the worst of them all, which is the
    // same thing because the list is sorted by level. Everything but the
    // engine's own absence is about an engine that is there, except the three
    // refusals and the let-go receiver, which the strip showed whatever the
    // connection was doing and still show.
    return {{
        {!in.connected, StatusLevel::Bad, "no engine"},
        {in.connected && in.front_end_fault, StatusLevel::Bad, "front end overloaded"},
        {in.connected && in.source_behind, StatusLevel::Bad, "source behind"},
        {in.connected && in.detection_fault, StatusLevel::Bad, "detector refused"},
        {in.connected && !in.engine_running, StatusLevel::Warn, "engine stopped", false},
        {in.connected && !in.source_open, StatusLevel::Warn, "no source open", false},
        {in.source_fault, StatusLevel::Warn, "radio refused"},
        {in.tune_fault, StatusLevel::Warn, "tune refused"},
        {in.receiver_gone, StatusLevel::Warn, "receiver let go"},
    }};
}

[[nodiscard]] inline StatusSummary summarise_status(const StatusInputs& in)
{
    StatusSummary out;
    for (const StatusCondition& condition : status_conditions(in)) {
        if (!condition.on) {
            continue;
        }
        if (out.headline.empty()) {
            out.level = condition.level;
            out.headline = condition.label;
        } else if (condition.chip) {
            out.chips.emplace_back(condition.label);
        }
    }
    if (!out.headline.empty()) {
        return out;
    }

    // Quiet or a note: the pill says the rate, which is the one number that
    // says the engine is alive, and the level says whether the drawer holds
    // anything worth opening it for.
    char rate[32] = {};
    std::snprintf(rate, sizeof rate, "%.1f rows/s", in.frame_rate);
    const bool note = in.clamped || in.stranded || in.frames_dropped_by_engine > 0;
    out.level = note ? StatusLevel::Note : StatusLevel::Quiet;
    out.headline = rate;
    return out;
}

}  // namespace revenant::ui
