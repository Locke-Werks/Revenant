// The relation between a sample index and the wall clock, and how well it is
// known.
//
// Every source establishes an anchor once at stream start: the Unix epoch
// nanosecond at which sample zero was captured. From then on the index is the
// authority and wall clock is derived from it. This file owns that derivation,
// the uncertainty that travels with it, and the loop that corrects it against
// a reference where a device offers one.
//
// THE RULE THAT MATTERS: the anchor is corrected, the sample index is never
// renumbered. Renumbering would invalidate every stored reference to a moment
// in a capture, which is the one thing a capture is for. So a correction
// appends a segment here and no block's BlockTimestamp is ever rewritten.
// BlockTimestamp::epoch_anchor_ns keeps the stream-start value forever, and a
// question of the form "with today's best knowledge, when was index N" is
// asked of this class instead.
//
// M1 has no disciplined source. The file and synthetic backends both
// free-run, and this says so with a number rather than a silence: accuracy
// grows linearly with elapsed samples at the declared oscillator tolerance,
// so a caller can see the anchor getting worse instead of assuming it is
// holding. observe() and the fit behind it exist now so that a PPS or GPS
// input is a registration and a call, not a rewrite of everything that reads
// a timestamp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"
#include "core/source/capabilities.h"

namespace revenant::source {

// One observation of the relation between the sample stream and wall clock.
//
// A PPS input gives one of these a second: the index at which the edge landed
// and the UTC second it marked. A GPS receiver with a serial time message
// gives the same pair. Nothing else about the two hardware paths reaches this
// class, which is why they can be added later without touching it.
struct ReferenceEdge {
    // Unix epoch nanoseconds the edge marked. For a PPS this is an exact
    // second boundary; the field is nanoseconds so a GPS message that is not
    // on a second boundary needs no second type.
    std::int64_t unix_ns = 0;

    // Sample index at which the edge was observed. The uncertainty in placing
    // the edge against the sample stream is the dominant error term in the
    // whole model, so it is stated per edge rather than assumed.
    dsp::SampleIndex sample_index = 0;

    // One-sigma uncertainty of this observation. Zero means unstated, which
    // the model treats as equal weight with every other unstated edge.
    std::int64_t accuracy_ns = 0;
};

struct ClockModelConfig {
    // What is actually driving the sample clock. Reported verbatim in
    // ClockQuality so a reader can tell an undisciplined TCXO from a
    // disciplined GPS without inferring it from the numbers.
    ClockSource source = ClockSource::Internal;

    // Nominal sample rate. Integer, per docs/conventions.md: the whole point
    // of the discipline loop is measuring how far the real rate is from this
    // one, and that difference is only exact if this end of it is.
    dsp::SampleRate rate = 0;

    // One-sigma uncertainty in the anchor at the moment it is established,
    // before any drift. For a file this is how well the recording's start time
    // is known; for a radio it is the residual after the known buffering
    // latency has been subtracted.
    std::int64_t anchor_accuracy_ns = 0;

    // The oscillator's specified fractional frequency tolerance, parts per
    // million, one sigma. An undisciplined clock's anchor accuracy grows at
    // this rate for as long as the stream runs. An RTL-SDR v3's TCXO is about
    // 1 ppm, which is one microsecond a second and 3.6 ms an hour.
    //
    // Zero means the index-to-time mapping is exact by construction, which is
    // true of a synthesised stream and of a recording whose declared rate is
    // taken as its definition.
    double oscillator_tolerance_ppm = 0.0;

    // How many edges the fit looks back over. Long enough that the rate term
    // is well determined, short enough that an oscillator step is followed
    // rather than averaged away. Sixty-four seconds of PPS.
    std::size_t fit_window_edges = 64;

    // An edge whose residual against the current model exceeds this many
    // sigma is counted as rejected and left out of the fit. A GPS receiver
    // reporting a bad fix, or a missed edge counted as the next one, both
    // arrive as a residual of roughly a whole second and would drag the fit
    // for the length of the window if they were let in.
    double outlier_sigma = 5.0;
};

// One interval over which a single (anchor, rate error) pair holds.
//
// A correction appends one of these rather than mutating the last, so a
// question dated before the correction still resolves with what was known
// then. That is what makes a stored capture's timestamps auditable after the
// fact instead of quietly improving behind the reader's back.
struct ClockSegment {
    dsp::SampleIndex effective_from_index = 0;

    // Wall clock of effective_from_index under this segment.
    std::int64_t anchor_ns = 0;

    // Fractional frequency error of the sample clock, parts per million.
    // Positive means the device's sample clock runs fast, so a given count of
    // samples takes less real time than the nominal rate says.
    double rate_error_ppm = 0.0;

    // One-sigma uncertainty at effective_from_index.
    std::int64_t accuracy_ns = 0;

    // How fast the uncertainty above grows as the stream runs on, in parts
    // per million. Undisciplined this is the oscillator's open-loop
    // tolerance; disciplined it is the measured uncertainty of the rate
    // estimate, which is smaller and is why discipline is worth having. One
    // field for both because both answer the same question.
    double rate_error_accuracy_ppm = 0.0;
};

class ClockModel {
public:
    [[nodiscard]] static Expected<ClockModel> create(const ClockModelConfig& config);

    ClockModel() = default;

    // Fixes sample zero on the wall clock. Called once, on the source thread,
    // at stream start. Calling it again after edges have been observed is an
    // error rather than a silent reset: it would discard a disciplined fit in
    // favour of a worse number with no record that it happened.
    [[nodiscard]] Status set_anchor(std::int64_t epoch_anchor_ns);

    [[nodiscard]] bool anchored() const { return anchored_; }

    // The value a source writes into every BlockTimestamp. Immutable once set,
    // whatever the discipline loop later concludes, because a block is
    // self-describing and a block that changes is not.
    [[nodiscard]] std::int64_t stream_anchor_ns() const { return stream_anchor_ns_; }

    // How far the stream has run, which is what an undisciplined model needs
    // to report its accuracy honestly. Cheap enough to call per block.
    void note_progress(dsp::SampleIndex write_index);

    // Feeds the discipline loop one observation. Returns whether the edge was
    // used; a rejected outlier is not an error, it is a counted event, so the
    // Status is reserved for an edge that cannot be interpreted at all.
    [[nodiscard]] Status observe(const ReferenceEdge& edge);

    // Wall clock of an index with today's best knowledge. This is the query
    // that gets a different answer after a correction, and the reason no
    // block's own stamp is allowed to.
    [[nodiscard]] std::int64_t wall_clock_ns(dsp::SampleIndex index) const;

    // One-sigma uncertainty of the answer above, at that index.
    [[nodiscard]] std::int64_t accuracy_ns_at(dsp::SampleIndex index) const;

    [[nodiscard]] ClockQuality quality() const;

    [[nodiscard]] const std::vector<ClockSegment>& segments() const { return segments_; }
    [[nodiscard]] std::uint64_t edges_used() const { return edges_used_; }
    [[nodiscard]] std::uint64_t edges_rejected() const { return edges_rejected_; }

private:
    void refit();
    [[nodiscard]] const ClockSegment& segment_for(dsp::SampleIndex index) const;
    [[nodiscard]] std::int64_t elapsed_ns(dsp::SampleIndex samples, double rate_error_ppm) const;

    ClockModelConfig config_{};
    bool anchored_ = false;
    std::int64_t stream_anchor_ns_ = 0;
    dsp::SampleIndex write_index_ = 0;

    // Always at least one entry once anchored. Sorted by effective_from_index
    // and never reordered.
    std::vector<ClockSegment> segments_{};

    // The fit window, oldest first, trimmed to fit_window_edges.
    std::vector<ReferenceEdge> window_{};

    std::uint64_t edges_used_ = 0;
    std::uint64_t edges_rejected_ = 0;
    std::int64_t last_residual_ns_ = 0;
    bool disciplined_ = false;
};

}  // namespace revenant::source
