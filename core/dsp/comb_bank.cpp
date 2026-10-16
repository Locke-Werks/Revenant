// CombBank from core/dsp/comb_cancel.h: several CombTrackers sharing one
// project and one subtract dispatch.
//
// SLOTS. kCombMaxLines lines in all. A tracker started from a detection gets
// an even share of what the running trackers leave free, divided among the
// estimates in that detection still to be placed, so a pair found together
// splits the budget and a comb found later takes what remains. A tracker
// keeps its share until it is lost; trimming inside it is the tracker's own.
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

// An estimate whose lines sit this much on a running tracker's lines is a
// detection of what that tracker holds, at another spacing or a sub-grid.
constexpr double kOverlapFraction = 0.25;

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
    std::vector<const CombEstimate*> fresh;
    for (const CombEstimate& estimate : estimates) {
        if (estimate.line_count() < kCombMinLines || !(estimate.spacing_hz > 0.0)) {
            continue;
        }
        bool held = false;
        for (const CombTracker& tracker : trackers_) {
            if (tracker.active() &&
                (tracker.agrees(estimate) || tracker.overlap(estimate) > kOverlapFraction)) {
                held = true;
                break;
            }
        }
        // Two estimates of one detection never overlap each other: the
        // detector removes a comb's peaks before looking for the next.
        if (!held) {
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
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        if (trackers_.size() >= kCombMaxCombs || used >= kCombMaxLines) {
            break;
        }
        const auto left = static_cast<std::uint32_t>(fresh.size() - i);
        const std::uint32_t share = (kCombMaxLines - used) / left;
        if (share < kCombMinLines) {
            break;
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

CombStatus CombBank::status() const
{
    CombStatus out;
    double suppression = 0.0;
    std::uint32_t best_cancelled = 0;
    bool any = false;
    for (const CombTracker& tracker : trackers_) {
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
