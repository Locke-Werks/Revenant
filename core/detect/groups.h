// Tracks followed as a set: the lines one emitter puts on a spectrum.
//
// WHY THIS EXISTS
//
// docs/detection.md measured it: five of the eight families in
// tests/detect/test_front_end.cpp are reported as separate spectral lines,
// one track each, about five bins wide at any grid. An AM station is three
// tracks, a narrowband FM one is fifteen, and no number measuring ONE band
// tells cw from am from usb, because what separates them is how many lines
// there are and where they sit relative to each other. That is a property of
// the set, and the detector publishes a flat list.
//
// This follows the set. A group is two or more tracks whose centres chain
// together within a stated gap, and it carries an id that survives from one
// decision to the next, so a reader can ask how long a set of lines has been
// there TOGETHER rather than read a coincidence off one snapshot. The HF
// corpus showed why that matters: grouped once, at the instant a table was
// sampled, most of what came out was an old carrier with a line a few seconds
// old beside it, and two runs over the same file grouped differently.
//
// WHAT IT DOES NOT DO
//
// It classifies nothing and names no family, and a group goes on the wire as
// nothing: what reaches a client is each track's own label, core/detect/
// label.h. WHAT THIS SENTENCE USED TO SAY after "names no family": "and
// docs/detection.md decided that nothing goes on the wire as a family",
// which the owner reversed on 2026-09-23 for tracks.
//
// The gap is the caller's, for the
// same reason both detector thresholds are the operator's: no measurement has
// chosen one, and a constant chosen here would be a classification smuggled
// in as a grouping rule. A group means "these lines sit within the gap of each
// other", and the ages beside it are what say whether that is structure.
//
// It cannot tell USB from LSB. Relative to the carrier the two differ only in
// which side the lines sit, and a suppressed-carrier signal has no carrier in
// the group to be relative to. Offsets here are from the strongest member,
// which on a two-tone SSB signal is whichever tone measured louder.
//
// WHY IT IS BESIDE THE DETECTOR AND NOT INSIDE IT
//
// Its input is exactly Detector::tracks() and the decision index, and nothing
// in the detector's own arithmetic reads a group. Kept separate the way
// FrontEndMonitor is, it can be tested against a hand-built track list, where
// every centre and birth is chosen, and the detector's contract does not grow
// a field that only a display reads.
//
// PURITY
//
// No clock. Every time here is a sample index handed in by the caller, the
// same frame Track::first_seen is in, so a replay at forty times realtime
// groups identically to the live run.
//
// THREADING
//
// One thread, the one that owns the Detector feeding it.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/detect/detector.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::detect {

struct LineGroupConfig {
    // Two tracks whose centres sit this far apart or closer are in one group,
    // and the rule chains: three lines 1 kHz apart are one group at a 1 kHz
    // gap even though the outer two are 2 kHz apart. Centre to centre, in
    // hertz, and required positive.
    //
    // THE CALLER'S NUMBER. What the family scene needs is recorded in
    // tests/detect/test_groups.cpp: at the shipped 36.6 Hz the widest spacing
    // any analogue family there produces is 1191 Hz, the two-tone SSB pair, so
    // a gap under that splits it, and the case runs at 3 kHz. No measurement
    // against real air has chosen a value; docs/detection.md has what three
    // gaps did across the HF corpus.
    dsp::Hertz gap_hz = 0;
};

// One track, as a member of a group.
struct GroupMember {
    std::uint64_t track = 0;
    TrackState state = TrackState::Live;

    // Straight off the track, absolute. A Held member's are the last ones a
    // decision measured, as Track documents, so a line that faded is still
    // where it was.
    dsp::Hertz center = 0;
    dsp::Hertz bandwidth = 0;
    double snr_2500_db = 0.0;

    // From the anchor's centre. Zero on the anchor itself.
    dsp::Hertz offset = 0;

    // From the member below it, zero on the lowest. On a comb this is the
    // modulation frequency, and on a carrier with a matched pair it is the
    // same number twice.
    dsp::Hertz spacing = 0;

    // When the track was born, which is Track::first_seen.
    dsp::SampleIndex first_seen = 0;

    // When it entered THIS group, by id. A line that leaves and comes back
    // joins again at the decision it came back.
    dsp::SampleIndex joined = 0;
};

struct LineGroup {
    // Issued in order from one and never reused, like Track::id.
    std::uint64_t id = 0;

    // The strongest member by snr_2500_db, lower track id on a tie. Offsets
    // are measured from it because that is the line a reader can find again:
    // on a carrier with sidebands it is the carrier. It can change between
    // decisions when two members read within their own measurement noise of
    // each other, and every offset moves with it when it does.
    std::uint64_t anchor = 0;
    dsp::Hertz anchor_center = 0;

    // Highest member centre minus lowest.
    dsp::Hertz span = 0;

    // The decision this id was first issued at.
    dsp::SampleIndex formed = 0;

    // The latest GroupMember::joined, so the decision since which every
    // current member has been in the group. THIS IS THE NUMBER TO READ FIRST.
    // A carrier with a line that arrived two seconds ago has a group that is
    // two seconds old however long the carrier has been up.
    dsp::SampleIndex together_since = 0;

    // Latest member birth minus earliest, in samples. Lines one emitter
    // produces are born together, within birth_hits decisions of each other;
    // a spread of tens of seconds is two things that happen to sit close.
    dsp::SampleIndex birth_spread = 0;

    // Where this group's members sit in LineGrouper::members(), ascending in
    // frequency.
    std::uint32_t first_member = 0;
    std::uint32_t member_count = 0;
};

struct LineGroupStats {
    std::uint64_t decisions = 0;

    // Ids issued, and ids that stopped existing because their group fell
    // under two members or was absorbed into another.
    std::uint64_t groups_formed = 0;
    std::uint64_t groups_ended = 0;

    // Member arrivals into an existing group and departures from one. A
    // group that is stable in id and churning underneath shows up here and
    // nowhere else.
    std::uint64_t joins = 0;
    std::uint64_t leaves = 0;

    // The longest any group has held every one of its members at once, in
    // samples, and which group that was. The run-level answer to "did any
    // set of lines persist", which one table cannot give: a band where this
    // stays at a few seconds had neighbours and no structure.
    dsp::SampleIndex longest_together = 0;
    std::uint64_t longest_together_group = 0;
};

class LineGrouper {
public:
    [[nodiscard]] static Expected<LineGrouper> create(const LineGroupConfig& config);

    LineGrouper(LineGrouper&&) noexcept = default;
    LineGrouper& operator=(LineGrouper&&) noexcept = default;
    LineGrouper(const LineGrouper&) = delete;
    LineGrouper& operator=(const LineGrouper&) = delete;
    ~LineGrouper() = default;

    // One decision. tracks is what Detector::tracks() returned after it and
    // now is Detector::last_decision().
    //
    // Live and Held tracks are grouped. Held because following a line through
    // a fade is the point of doing this at the tracker rather than off one
    // snapshot, and the tracker already decided how long a fade may last.
    // Merged tracks are not, because the track that swallowed one is in the
    // list carrying the same energy, and counting both would count one line
    // twice. Pending tracks are not tracks.
    //
    // Refuses a decision earlier than the last one, which is a different
    // capture rather than this one continuing: reset() first.
    [[nodiscard]] Status observe(std::span<const Track> tracks, dsp::SampleIndex now);

    // Groups at the last decision, ascending by their lowest member's centre.
    // Valid until the next observe().
    [[nodiscard]] std::span<const LineGroup> groups() const { return groups_; }

    // The members of one group from groups().
    [[nodiscard]] std::span<const GroupMember> members(const LineGroup& group) const;

    [[nodiscard]] const LineGroupStats& stats() const { return stats_; }
    [[nodiscard]] const LineGroupConfig& config() const { return config_; }

    // Forgets every group, for a retune or a new capture. Ids keep counting
    // from where they were, so an id in a log still names one group.
    void reset();

private:
    LineGrouper() = default;

    LineGroupConfig config_{};

    std::vector<LineGroup> groups_;
    std::vector<GroupMember> members_;

    // The previous decision's, which this one matches against.
    std::vector<LineGroup> previous_groups_;
    std::vector<GroupMember> previous_members_;

    // Scratch, rebuilt per decision.
    std::vector<const Track*> eligible_;
    struct Overlap {
        std::uint32_t current = 0;
        std::uint32_t previous = 0;
        std::uint32_t shared = 0;
    };
    std::vector<Overlap> overlaps_;
    std::vector<std::uint8_t> previous_taken_;

    dsp::SampleIndex last_now_ = 0;
    bool have_decision_ = false;

    std::uint64_t next_id_ = 1;
    LineGroupStats stats_{};
};

}  // namespace revenant::detect
