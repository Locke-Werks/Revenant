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
// AN EMITTER IS PROBED WHOLE, NOT LINE BY LINE
//
// The detector reports a talker on AM or FM as several tracks, a carrier and
// the stretches of sideband the averaged spectrum shows beside it, and one on
// single sideband as the stretches of its voice channel. docs/detection.md
// measured what probing those one at a time does: a probe sized to one
// sideband sees the carrier off centre with one sideband beside it and calls
// it a carrier, so an AM talker's brackets read CW and an FM talker's read AM,
// CW or BPSK. So tier two follows the tracks with a detect::LineGrouper at
// TierTwoConfig::emitter_gap_hz and treats a group as one emitter: one probe
// at the centre of the group's extent and as wide as it, and the answer
// recorded on every line in it. A line that joins the group later, which is
// what a sideband does when the talker starts again after a pause, is given
// the emitter's answer without a probe of its own. A line that is in a group
// is never probed on its own.
//
// What reaches the wire is still one detection per track, each carrying the
// emitter's label; publishing one detection per emitter is core/rpc's, and
// emitters() below is what it would read.
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
#include <unordered_set>
#include <vector>

#include "core/detect/detector.h"
#include "core/detect/groups.h"
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

    // Tracks whose occupied bands come within this of each other are one
    // emitter, through LineGroupConfig::edge_gap_hz. Zero probes every track
    // on its own, which is what tier two did before it.
    //
    // WHY 400 Hz. The voice channel starts 300 Hz from the carrier, so an AM
    // or FM talker leaves a hole at most 300 Hz wide either side of its
    // carrier's line, plus whatever the edges of the two bands the detector
    // measured leave; the sideband stretches of one voice channel otherwise
    // touch. Measured on tools/siggen/voice.h's scene through
    // tests/detect/test_voice_survey.cpp: the widest hole inside one emitter
    // and the narrowest between two are in docs/detection.md.
    dsp::Hertz emitter_gap_hz = 400;

    // A group is probed as one emitter only when its lines between them
    // cover at least this share of its extent: one filled band the detector
    // cut into pieces, not lines that happen to sit near each other.
    // Otherwise its lines are probed one at a time, as before.
    //
    // WHY. A talker's pieces touch or nearly touch: the holes between them are
    // the 300 Hz under the voice channel and the stretches the averaged
    // spectrum left under the growth level. Carriers a few hundred hertz apart
    // are lines with floor between them, and one probe over the pair reads
    // them as two tones. THIS FILE'S CHOICE, not a measured bar: in the voice
    // survey an AM talker's five lines at 30 dB covered 0.75 of their extent,
    // and nothing yet has measured a cluster of independent carriers. The HF
    // corpus is where that measurement belongs; docs/detection.md, "Voice".
    double emitter_min_fill = 0.6;
};

// One emitter tier two is following: a LineGrouper group, by its id, its
// strongest line, its extent and its lines. For a caller that publishes one
// detection per emitter rather than one per line.
struct TierTwoEmitter {
    std::uint64_t group = 0;
    std::uint64_t anchor = 0;
    dsp::Hertz low_edge = 0;
    dsp::Hertz high_edge = 0;
    std::vector<std::uint64_t> tracks;

    // Whether an emitter-wide probe has answered for it yet, and the answer
    // every line carries when one has.
    bool answered = false;
    ProbeFinding finding;
};

struct TierTwoStats {
    std::uint64_t submitted = 0;
    std::uint64_t refused = 0;

    // Of submitted, the identification schedule's long dwells, and the
    // probes of a whole emitter.
    std::uint64_t identify_submitted = 0;
    std::uint64_t emitter_submitted = 0;

    // An emitter's answer recorded on one of its lines other than the one the
    // probe was tagged with, either when it came back or when the line joined
    // later.
    std::uint64_t inherited = 0;

    // Outcomes taken off the pool, by what became of them. recorded is
    // characterised and handed to the detector; orphaned is characterised for
    // a track that had gone by the time it came back. The other four are the
    // pool's own statuses and are never recorded against a track.
    std::uint64_t recorded = 0;
    std::uint64_t orphaned = 0;

    // Outcomes for a probe this TierTwo never submitted, which is a probe a
    // TierTwo before it submitted: dropped unread. See TierTwo::take.
    std::uint64_t stale = 0;
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

    // THE IDENTIFICATION SCHEDULE, which runs on what the one above leaves.
    // A narrow track, no wider than engine::kProbeIdentifyNarrowHz, that has
    // had its first probe and has no protocol yet gets one probe of
    // engine::kProbeIdentifyDwellSeconds, once, oldest first, because the
    // narrow modes frame too slowly for two seconds; core/engine/probe.h has
    // the measurement. After every track not yet probed at all, so a pool on
    // a busy band spends itself on coverage first. `tried` holds the tracks
    // already given theirs and `already` the ids the classification schedule
    // just picked.
    [[nodiscard]] static std::vector<std::uint64_t> pick_identify(
        std::span<const Track> tracks, const std::unordered_set<std::uint64_t>& tried,
        std::span<const std::uint64_t> in_flight, std::span<const std::uint64_t> already,
        std::size_t free);

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

    // The emitters at the last step, ascending in frequency, and the grouper
    // that found them. Empty, and null, with emitter_gap_hz at zero.
    [[nodiscard]] std::vector<TierTwoEmitter> emitters() const;
    [[nodiscard]] const LineGrouper* grouper() const {
        return grouper_.has_value() ? &*grouper_ : nullptr;
    }

private:
    TierTwo() = default;

    void take(Detector& detector, engine::Engine& engine);

    // Gives each group's lines its emitter's answer. See the header.
    void propagate(Detector& detector);

    // Whether a group is an emitter: TierTwoConfig::emitter_min_fill.
    [[nodiscard]] bool is_emitter(const LineGroup& group) const;

    TierTwoConfig config_{};
    TierTwoStats stats_{};

    std::vector<std::uint64_t> in_flight_;
    std::unordered_map<std::uint64_t, dsp::SampleIndex> attempts_;
    std::unordered_map<std::uint64_t, engine::ProbeStatus> statuses_;
    std::unordered_set<std::uint64_t> identify_tried_;
    std::vector<engine::ProbeOutcome> outcomes_;

    // The emitters. A probe of a group is tagged with its anchor's id and
    // remembers the group and the lines it had when it was submitted.
    std::optional<LineGrouper> grouper_;
    struct GroupProbe {
        std::uint64_t group = 0;
        std::vector<std::uint64_t> tracks;
    };
    std::unordered_map<std::uint64_t, GroupProbe> group_probes_;
    std::unordered_map<std::uint64_t, dsp::SampleIndex> group_attempts_;
    std::unordered_map<std::uint64_t, ProbeFinding> group_findings_;
    std::unordered_set<std::uint64_t> group_identify_tried_;

    // The emitter answer each line carries, so one that leaves and rejoins,
    // or turns up in a group with a new id after a pause, keeps it.
    std::unordered_map<std::uint64_t, ProbeFinding> inherited_;
};

}  // namespace revenant::detect
