// Tier two of docs/detection.md: which tracks get a probe receiver, and what
// happens to the answer.
//
// core/engine/probe.h is the receiver side, a pool of receivers the engine
// places on detections for itself. This is the other side: it reads the
// detector's tracks after each decision, decides which of them to probe,
// submits them, and hands every answer that comes back to
// Detector::record_probe, which keeps it and lets it change the reported
// family only when characterise::may_drive_detection says it may.
//
// THE SCHEDULE, WHICH IS A CHOICE MADE HERE
//
// Oldest unclassified first, and not round robin. A track that has never been
// probed outranks every track that has, and among those the one born first
// goes first. Round robin was the alternative and it spends the pool on
// tracks already answered: a steady station reprobed every cycle is a
// receiver not looking at the track born a second ago. After that, a track
// that was probed and came back without a family it may drive with waits
// TierTwoConfig::reprobe_seconds before it is eligible again, oldest answer
// first. A track that has a family is not probed again at all: a transmission
// does not change modulation halfway through, and the pool is small.
//
// Only Live tracks. A Held one has no transmission to collect and a Merged
// one is inside another track's band, where a probe would characterise the
// other track.
//
// THREADING
//
// step() is the one producer and the one consumer core/engine/probe.h asks
// for, so it has to be called from one thread. revenant-cli calls it from the
// spectrum sink, right after the decision it reads, which is the thread that
// owns the detector anyway.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "core/detect/detector.h"
#include "core/dsp/types.h"
#include "core/engine/engine.h"
#include "core/error.h"

namespace revenant::detect {

struct TierTwoConfig {
    // Needed to turn sample indices into seconds; DetectorConfig::source_rate.
    dsp::SampleRate source_rate = 0;

    // How long a track whose answer could not drive anything waits before it
    // is probed again. Ten seconds is five dwells at the floor bucket, so a
    // pool of four gets through twenty other tracks before it comes back to
    // one it has already asked about.
    double reprobe_seconds = 10.0;
};

struct TierTwoStats {
    std::uint64_t submitted = 0;
    std::uint64_t refused = 0;

    // Outcomes taken off the pool, by what became of them. recorded is
    // characterised and handed to the detector; orphaned is characterised for
    // a track that had gone by the time it came back. The other four are the
    // pool's own statuses and are never recorded against a track.
    std::uint64_t recorded = 0;
    std::uint64_t orphaned = 0;
    std::uint64_t too_wide = 0;
    std::uint64_t unplaced = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t failed = 0;

    // Recorded answers the detector accepted as a family, and the ones it
    // kept without letting them change anything.
    std::uint64_t accepted = 0;
    std::uint64_t refused_by_characterise = 0;

    // Recorded answers by the family they named, indexed by Classification,
    // and the part of each the detector accepted. Unknown is in the first
    // and never in the second.
    std::array<std::uint64_t, kClassificationCount> named{};
    std::array<std::uint64_t, kClassificationCount> accepted_as{};

    // Recorded answers that verified a protocol, indexed by identify::Protocol.
    // None is never counted.
    std::array<std::uint64_t, identify::kProtocolCount> protocols{};

    // Seconds from a track's first sighting to the decision its first
    // accepted family was recorded at, over the tracks that got one.
    std::uint64_t first_classifications = 0;
    double first_classification_seconds_total = 0.0;
    double first_classification_seconds_min = 0.0;
    double first_classification_seconds_max = 0.0;
};

class TierTwo {
public:
    [[nodiscard]] static Expected<TierTwo> create(const TierTwoConfig& config);

    // Takes every finished probe into the detector, then submits probes for
    // the tracks the schedule picks, up to what the pool has free.
    //
    // Call after a decision: the answers land on the tracks that decision
    // published, and the schedule reads them.
    [[nodiscard]] Status step(Detector& detector, engine::Engine& engine);

    // The schedule on its own, for a test to referee. Track ids to probe, in
    // the order they would be submitted, at most `free` of them.
    //
    // attempts maps a track id to the decision it was last submitted at. A
    // track in it and in in_flight is not eligible; one in it and not in
    // flight is eligible again reprobe_seconds after that decision, unless it
    // has a family.
    [[nodiscard]] static std::vector<std::uint64_t> pick(
        std::span<const Track> tracks,
        const std::unordered_map<std::uint64_t, dsp::SampleIndex>& attempts,
        std::span<const std::uint64_t> in_flight, std::size_t free, dsp::SampleIndex now,
        dsp::SampleRate source_rate, double reprobe_seconds);

    [[nodiscard]] const TierTwoStats& stats() const { return stats_; }

    // Tracks submitted and not yet answered. Never more than the pool holds.
    [[nodiscard]] std::size_t in_flight() const { return in_flight_.size(); }
    [[nodiscard]] bool probing(std::uint64_t track_id) const {
        return std::find(in_flight_.begin(), in_flight_.end(), track_id) != in_flight_.end();
    }

    // How the most recent probe of a track ended, or nothing when none has.
    // Only Characterised reaches the track itself, through record_probe; the
    // other statuses are the pool's and live here, so a display can say "too
    // wide" instead of implying nobody tried.
    [[nodiscard]] std::optional<engine::ProbeStatus> last_status(std::uint64_t track_id) const;

private:
    TierTwo() = default;

    void take(Detector& detector, engine::Engine& engine);

    TierTwoConfig config_{};
    TierTwoStats stats_{};

    std::vector<std::uint64_t> in_flight_;
    std::unordered_map<std::uint64_t, dsp::SampleIndex> attempts_;
    std::unordered_map<std::uint64_t, engine::ProbeStatus> statuses_;
    std::vector<engine::ProbeOutcome> outcomes_;
};

}  // namespace revenant::detect
