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
// first claim on the slots: a comb that starts takes back what it needs from
// the newest spurs. A spur on any line a comb plans, sidebands included, is
// let go, so no line is subtracted twice. A spur that is lost, dropped or
// withdrawn is not taken again for kSpurHoldOffSeconds, so a spur that will
// not hold does not start a tracker every decision.
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
// recorded per block start names each row run by the id of the tracker that
// planned it, so a tracker started, lost or dropped between plan and
// observation leaves every other tracker its own rows, and blocks in flight
// survive a change to the set of trackers.
//
// PROVENANCE, per docs/clean-room.md: this project's own.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "core/dsp/comb_cancel.h"

namespace revenant::dsp {

namespace {

constexpr double kOverlapFraction = kCombOverlapFraction;
constexpr double kSameLineHz = kCombSameLineHz;

constexpr std::size_t kMaxSplits = 32;

// How long a spur that went is kept from starting again.
constexpr double kSpurHoldOffSeconds = 3.0;

}  // namespace

CombBank::CombBank(SampleRate rate) : rate_(rate) {}

void CombBank::reset()
{
    trackers_.clear();
    splits_.clear();
    hold_off_.clear();
}

void CombBank::add(CombTracker tracker)
{
    tracker.set_id(next_id_++);
    trackers_.push_back(std::move(tracker));
}

void CombBank::hold_off(const CombTracker& tracker)
{
    const auto samples =
        static_cast<SampleIndex>(kSpurHoldOffSeconds * static_cast<double>(std::max<SampleRate>(rate_, 1)));
    hold_off_.push_back({tracker.detected_offset_hz(), now_ + samples});
}

void CombBank::erase_spurs_if(const std::function<bool(const CombTracker&)>& drop)
{
    std::erase_if(trackers_, [&](const CombTracker& tracker) {
        if (!tracker.spur() || !drop(tracker)) {
            return false;
        }
        hold_off(tracker);
        return true;
    });
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
            // carrier's. Spurs are not counted: one line of a comb's is not
            // the comb.
            if (!tracker.spur() && tracker.active() &&
                tracker.overlap(estimate, kSameLineHz) > kOverlapFraction) {
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

    // A comb tracker that is not running gives its slots back.
    std::erase_if(trackers_,
                  [](const CombTracker& tracker) { return !tracker.spur() && !tracker.active(); });

    // Combs have first claim: the slots spurs hold count as free here, and a
    // comb that starts takes them back below.
    std::uint32_t used = 0;
    for (const CombTracker& tracker : trackers_) {
        if (!tracker.spur()) {
            used += tracker.lines();
        }
    }
    // What a comb asks for: its lines, each with its sidebands.
    auto need = [](const CombEstimate& e) {
        const auto orders = static_cast<std::uint32_t>(std::max(e.sideband_orders, 0));
        return e.line_count() * (2 * orders + 1);
    };
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
        add(std::move(tracker));

        // Only now, for a comb that started: the newest spurs give back what
        // it took.
        std::uint32_t total = 0;
        for (const CombTracker& t : trackers_) {
            total += t.lines();
        }
        for (std::size_t j = trackers_.size(); j-- > 0 && total > kCombMaxLines;) {
            if (trackers_[j].spur()) {
                total -= trackers_[j].lines();
                hold_off(trackers_[j]);
                trackers_.erase(trackers_.begin() + static_cast<std::ptrdiff_t>(j));
            }
        }
    }
}

CombBlockPlan CombBank::plan(SampleIndex start, std::uint32_t count)
{
    CombBlockPlan out;
    out.start = start;
    out.count = count;
    out.sub_blocks = 1;
    now_ = start;
    std::erase_if(hold_off_, [&](const HoldOff& h) { return h.until <= now_; });

    // A spur that lost its line goes, and is held off.
    erase_spurs_if([](const CombTracker& tracker) { return !tracker.active(); });

    // Two trackers that have come to hold the same lines: the later one goes.
    // Overlap is checked at start too, but a tracker started beside a much
    // stronger comb can be pulled onto it during acquisition, whose pull-in
    // is a few hundred hertz, and then cancels lines another already does.
    for (std::size_t j = trackers_.size(); j-- > 1;) {
        if (trackers_[j].spur()) {
            continue;
        }
        const CombStatus later = trackers_[j].status();
        for (std::size_t i = 0; i < j; ++i) {
            if (!trackers_[i].spur() && trackers_[i].active() && trackers_[j].active() &&
                trackers_[i].overlap(later.comb, kSameLineHz) > kOverlapFraction) {
                trackers_.erase(trackers_.begin() + static_cast<std::ptrdiff_t>(j));
                break;
            }
        }
    }
    // A spur on a line some comb has come to hold, its sidebands included,
    // whichever started first: the spur goes, so the line is subtracted once.
    erase_spurs_if([this](const CombTracker& spur) {
        const double at = spur.estimate().offset_hz;
        return std::ranges::any_of(trackers_, [&](const CombTracker& tracker) {
            return !tracker.spur() && tracker.holds_frequency(at, kSameLineHz);
        });
    });

    std::vector<CombBlockPlan> plans;
    plans.reserve(trackers_.size());
    for (CombTracker& tracker : trackers_) {
        plans.push_back(tracker.plan(start, count));
        out.sub_blocks = std::max(out.sub_blocks, plans.back().sub_blocks);
    }

    Split split;
    split.start = start;
    split.sub_blocks = out.sub_blocks;
    for (std::size_t t = 0; t < plans.size(); ++t) {
        const CombBlockPlan& plan = plans[t];
        split.rows.push_back(Row{trackers_[t].id(), static_cast<std::uint32_t>(plan.lines.size())});
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
    for (const Row& r : split.rows) {
        total += r.lines;
    }
    if (observation.projections.size() != total * S) {
        return;
    }
    std::size_t row = 0;
    for (const Row& r : split.rows) {
        if (r.lines > 0) {
            auto owner = std::ranges::find_if(
                trackers_, [&](const CombTracker& tracker) { return tracker.id() == r.id; });
            if (owner != trackers_.end()) {
                CombObservation part;
                part.start = observation.start;
                part.count = observation.count;
                part.sub_blocks = S;
                part.projections =
                    observation.projections.subspan(row * S, static_cast<std::size_t>(r.lines) * S);
                owner->observe(part);
            }
        }
        row += r.lines;
    }
}

void CombBank::spurs(std::span<const CombEstimate> estimates)
{
    if (rate_ <= 0) {
        return;
    }
    erase_spurs_if([](const CombTracker& tracker) { return !tracker.active(); });
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
        const double at = estimate.offset_hz;
        const bool waiting = std::ranges::any_of(
            hold_off_, [&](const HoldOff& h) { return std::abs(h.offset_hz - at) < kSameLineHz; });
        // On any line a comb plans, sidebands included, or a spur already
        // held: the line is cancelled.
        const bool held = std::ranges::any_of(trackers_, [&](const CombTracker& tracker) {
            return tracker.holds_frequency(at, kSameLineHz);
        });
        if (waiting || held) {
            continue;
        }
        CombTracker tracker(rate_, 1);
        tracker.detected(estimate);
        if (!tracker.active()) {
            continue;
        }
        used += tracker.lines();
        ++held_spurs;
        add(std::move(tracker));
    }
}

void CombBank::withdraw_spurs(std::span<const double> frequencies_hz)
{
    erase_spurs_if([&](const CombTracker& tracker) {
        const double at = tracker.detected_offset_hz();
        return std::ranges::any_of(frequencies_hz,
                                   [&](double f) { return std::abs(f - at) < kSameLineHz; });
    });
}

void CombBank::drop_spurs_in(std::span<const SpurBand> bands)
{
    erase_spurs_if([&](const CombTracker& tracker) {
        const double at = tracker.detected_offset_hz();
        return std::ranges::any_of(bands, [&](const SpurBand& b) { return b.contains(at); });
    });
}

void CombBank::drop_spurs()
{
    erase_spurs_if([](const CombTracker&) { return true; });
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
