// CombBank from core/dsp/comb_cancel.h: several CombTrackers sharing one
// project and one subtract dispatch.
//
// SLOTS. kCombMaxLines lines in all. A tracker started from a detection gets
// an even share of what the running trackers leave free, divided among the
// estimates in that detection still to be placed, so a pair found together
// splits the budget and a comb found later takes what remains. A tracker
// keeps its share until it is lost; trimming inside it is the tracker's own.
//
// SPURS. A spur is a tracker of one line beside the combs, at most
// kCombMaxSpurs of them, each taking one slot from what the combs leave. They
// do not count toward kCombMaxCombs and are left out of status(). Combs have
// first claim on the slots, and a spur on a line a comb holds is let go, so no
// line is subtracted twice.
//
// SUB-BLOCKS. Each tracker picks its own sub-block count: short ones while it
// pulls in, long ones once it has the comb within a hertz or so, because a
// comb with supply-ripple sidebands needs the long ones to lock at all
// (core/dsp/comb_tracker.cpp, kMinSubBlockSeconds). The dispatch runs at the
// largest count any tracker asked for. The counts are powers of two dividing
// the block, so the largest is a multiple of every other, and a tracker folds
// the finer projections it is handed back into its own.
//
// ORDER. The trackers' lines are concatenated in tracker order, and the split
// recorded per block start is what hands each tracker its own rows of the
// observation, so a tracker started or lost between plan and observation is
// still given exactly the rows it planned, or none.
//
// PROVENANCE, per docs/clean-room.md: this project's own.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/comb_cancel.h"

namespace revenant::dsp {

namespace {

constexpr double kOverlapFraction = kCombOverlapFraction;
constexpr double kSameLineHz = kCombSameLineHz;

constexpr std::size_t kMaxSplits = 32;

}  // namespace

CombBank::CombBank(SampleRate rate) : rate_(rate) {}

void CombBank::reset()
{
    trackers_.clear();
    splits_.clear();
}

void CombBank::detected(std::span<const CombEstimate> estimates)
{
    if (rate_ <= 0) {
        return;
    }
    // Held is checked again as each tracker starts, against every running
    // tracker including the ones this call started: a line subtracted twice
    // is a line put back at full strength in antiphase, and on the 80 m
    // capture of 2026-10-16 the detector handed one comb over as several
    // sub-grids of itself, which without this cancelled each line three to
    // five times and left them louder than before.
    auto held = [this](const CombEstimate& estimate) {
        for (const CombTracker& tracker : trackers_) {
            // Not agrees(): its 150 Hz allowance is for a coarse detection
            // restarting one tracker, and would take a sideband comb for its
            // carrier's.
            if (tracker.active() && tracker.overlap(estimate, kSameLineHz) > kOverlapFraction) {
                return true;
            }
        }
        return false;
    };
    std::vector<const CombEstimate*> fresh;
    for (const CombEstimate& estimate : estimates) {
        if (!estimate.spur && estimate.line_count() >= kCombMinLines &&
            estimate.spacing_hz > 0.0 && !held(estimate)) {
            fresh.push_back(&estimate);
        }
    }
    if (fresh.empty()) {
        return;
    }

    // A tracker that is not running gives its slots back. The set of trackers
    // changes here, so blocks in flight can no longer be split by position:
    // they are let go, which each tracker meets as one dropped block.
    bool changed =
        std::erase_if(trackers_, [](const CombTracker& tracker) { return !tracker.active(); }) > 0;

    std::uint32_t used = 0;
    for (const CombTracker& tracker : trackers_) {
        used += tracker.lines();
    }
    // What a comb asks for: its lines, each with its sidebands.
    auto need = [](const CombEstimate& e) {
        const auto orders = static_cast<std::uint32_t>(std::max(e.sideband_orders, 0));
        return e.line_count() * (2 * orders + 1);
    };
    // Combs have first claim on the slots: spurs give theirs back, newest
    // first, until the fresh combs fit. A spur still there is offered again
    // with the detector's next decision and takes whatever is left.
    std::uint32_t wanted = 0;
    for (const CombEstimate* estimate : fresh) {
        wanted += need(*estimate);
    }
    for (std::size_t j = trackers_.size(); j-- > 0 && used + wanted > kCombMaxLines;) {
        if (trackers_[j].spur()) {
            used -= trackers_[j].lines();
            trackers_.erase(trackers_.begin() + static_cast<std::ptrdiff_t>(j));
            changed = true;
        }
    }
    auto combs = [this] {
        return static_cast<std::size_t>(std::ranges::count_if(
            trackers_, [](const CombTracker& tracker) { return !tracker.spur(); }));
    };
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        if (combs() >= kCombMaxCombs || used >= kCombMaxLines) {
            break;
        }
        // Strongest first takes what it needs if the rest still fit, and an
        // even share of what is free otherwise.
        const std::uint32_t free = kCombMaxLines - used;
        std::uint32_t rest = 0;
        for (std::size_t j = i + 1; j < fresh.size(); ++j) {
            rest += need(*fresh[j]);
        }
        const auto left = static_cast<std::uint32_t>(fresh.size() - i);
        const std::uint32_t even = free / left;
        const std::uint32_t room = rest < free ? free - rest : 0;
        const std::uint32_t share = std::min(need(*fresh[i]), std::max(even, room));
        if (share < kCombMinLines) {
            break;
        }
        if (held(*fresh[i])) {
            continue;
        }
        CombTracker tracker(rate_, share);
        tracker.detected(*fresh[i]);
        if (!tracker.active()) {
            continue;
        }
        used += tracker.lines();
        trackers_.push_back(std::move(tracker));
        changed = true;
    }
    if (changed) {
        splits_.clear();
    }
}

CombBlockPlan CombBank::plan(SampleIndex start, std::uint32_t count)
{
    CombBlockPlan out;
    out.start = start;
    out.count = count;
    out.sub_blocks = 1;

    // Two trackers that have come to hold the same lines: the later one goes.
    // Overlap is checked at start too, but a tracker started beside a much
    // stronger comb can be pulled onto it during acquisition, whose pull-in
    // is a few hundred hertz, and then cancels lines another already does.
    bool merged = false;
    for (std::size_t j = trackers_.size(); j-- > 1;) {
        const CombStatus later = trackers_[j].status();
        for (std::size_t i = 0; i < j; ++i) {
            if (trackers_[i].active() && trackers_[j].active() &&
                trackers_[i].overlap(later.comb, kSameLineHz) > kOverlapFraction) {
                trackers_.erase(trackers_.begin() + static_cast<std::ptrdiff_t>(j));
                merged = true;
                break;
            }
        }
    }
    // A spur on a line some comb has come to hold, whichever started first:
    // the spur goes, so the line is subtracted once.
    for (std::size_t j = trackers_.size(); j-- > 0;) {
        if (!trackers_[j].spur()) {
            continue;
        }
        const CombEstimate spur = trackers_[j].estimate();
        const bool on_comb = std::ranges::any_of(trackers_, [&](const CombTracker& tracker) {
            return !tracker.spur() && tracker.active() && tracker.overlap(spur, kSameLineHz) > 0.0;
        });
        if (on_comb) {
            trackers_.erase(trackers_.begin() + static_cast<std::ptrdiff_t>(j));
            merged = true;
        }
    }
    if (merged) {
        splits_.clear();
    }

    std::vector<CombBlockPlan> plans;
    plans.reserve(trackers_.size());
    for (CombTracker& tracker : trackers_) {
        plans.push_back(tracker.plan(start, count));
        out.sub_blocks = std::max(out.sub_blocks, plans.back().sub_blocks);
    }

    Split split;
    split.start = start;
    split.sub_blocks = out.sub_blocks;
    for (const CombBlockPlan& plan : plans) {
        split.lines.push_back(static_cast<std::uint32_t>(plan.lines.size()));
        split.tracker_sub.push_back(plan.sub_blocks);
        out.subtract = out.subtract || plan.subtract;
        out.lines.insert(out.lines.end(), plan.lines.begin(), plan.lines.end());
    }
    if (out.lines.empty()) {
        return out;
    }
    splits_.push_back(std::move(split));
    if (splits_.size() > kMaxSplits) {
        splits_.erase(splits_.begin());
    }
    return out;
}

void CombBank::observe(const CombObservation& observation)
{
    auto it = std::find_if(splits_.begin(), splits_.end(),
                           [&](const Split& s) { return s.start == observation.start; });
    if (it == splits_.end()) {
        return;
    }
    const Split split = std::move(*it);
    splits_.erase(splits_.begin(), it + 1);

    const std::uint32_t S = observation.sub_blocks;
    if (S != split.sub_blocks) {
        return;
    }
    std::size_t total = 0;
    for (const std::uint32_t n : split.lines) {
        total += n;
    }
    if (observation.projections.size() != total * S) {
        return;
    }
    if (split.lines.size() != trackers_.size()) {
        return;
    }
    std::size_t row = 0;
    for (std::size_t t = 0; t < trackers_.size(); ++t) {
        const std::uint32_t n = split.lines[t];
        if (n > 0) {
            CombObservation part;
            part.start = observation.start;
            part.count = observation.count;
            part.sub_blocks = S;
            part.projections = observation.projections.subspan(row * S, static_cast<std::size_t>(n) * S);
            trackers_[t].observe(part);
        }
        row += n;
    }
}

void CombBank::spurs(std::span<const CombEstimate> estimates)
{
    if (rate_ <= 0) {
        return;
    }
    bool changed =
        std::erase_if(trackers_, [](const CombTracker& tracker) { return !tracker.active(); }) > 0;
    std::uint32_t used = 0;
    std::uint32_t held_spurs = 0;
    for (const CombTracker& tracker : trackers_) {
        used += tracker.lines();
        held_spurs += tracker.spur() ? 1U : 0U;
    }
    for (const CombEstimate& estimate : estimates) {
        if (!estimate.spur || estimate.line_count() != 1) {
            continue;
        }
        if (held_spurs >= kCombMaxSpurs || used >= kCombMaxLines) {
            break;
        }
        // On a comb's line, or a spur already held: the line is cancelled.
        const bool held = std::ranges::any_of(trackers_, [&](const CombTracker& tracker) {
            return tracker.active() && tracker.overlap(estimate, kSameLineHz) > 0.0;
        });
        if (held) {
            continue;
        }
        CombTracker tracker(rate_, 1);
        tracker.detected(estimate);
        if (!tracker.active()) {
            continue;
        }
        used += tracker.lines();
        ++held_spurs;
        trackers_.push_back(std::move(tracker));
        changed = true;
    }
    if (changed) {
        splits_.clear();
    }
}

void CombBank::drop_spurs_near(std::span<const double> frequencies_hz, double guard_hz)
{
    const std::size_t erased = std::erase_if(trackers_, [&](const CombTracker& tracker) {
        if (!tracker.spur()) {
            return false;
        }
        const double at = tracker.estimate().offset_hz;
        return std::ranges::any_of(frequencies_hz,
                                   [&](double f) { return std::abs(f - at) <= guard_hz; });
    });
    if (erased > 0) {
        splits_.clear();
    }
}

void CombBank::drop_spurs()
{
    if (std::erase_if(trackers_, [](const CombTracker& tracker) { return tracker.spur(); }) > 0) {
        splits_.clear();
    }
}

std::uint32_t CombBank::spurs_cancelled() const
{
    return static_cast<std::uint32_t>(std::ranges::count_if(trackers_, [](const CombTracker& t) {
        return t.spur() && t.status().lock == CombLock::kLocked;
    }));
}

std::vector<CombEstimate> CombBank::active_estimates() const
{
    std::vector<CombEstimate> out;
    for (const CombTracker& tracker : trackers_) {
        if (tracker.active()) {
            out.push_back(tracker.estimate());
        }
    }
    return out;
}

CombStatus CombBank::status() const
{
    CombStatus out;
    double suppression = 0.0;
    std::uint32_t best_cancelled = 0;
    bool any = false;
    for (const CombTracker& tracker : trackers_) {
        if (tracker.spur()) {
            continue;
        }
        const CombStatus s = tracker.status();
        if (s.lock == CombLock::kIdle) {
            continue;
        }
        // Locked outranks acquiring outranks lost.
        auto rank = [](CombLock lock) {
            switch (lock) {
                case CombLock::kLocked:
                    return 3;
                case CombLock::kAcquiring:
                    return 2;
                case CombLock::kLost:
                    return 1;
                case CombLock::kIdle:
                    break;
            }
            return 0;
        };
        if (rank(s.lock) > rank(out.lock)) {
            out.lock = s.lock;
        }
        out.lines_cancelled += s.lines_cancelled;
        suppression += static_cast<double>(s.suppression_db) * s.lines_cancelled;
        if (!any || s.lines_cancelled > best_cancelled) {
            const std::uint32_t found = out.comb.lines_found;
            out.comb = s.comb;
            out.comb.lines_found = found;
            out.spacing_drift_hz_per_s = s.spacing_drift_hz_per_s;
            best_cancelled = s.lines_cancelled;
            any = true;
        }
        out.comb.lines_found += s.comb.lines_found;
    }
    out.suppression_db = out.lines_cancelled > 0
                             ? static_cast<float>(suppression / out.lines_cancelled)
                             : 0.0F;
    return out;
}

}  // namespace revenant::dsp
