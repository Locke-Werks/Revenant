// Search over a stored capture: what was there, where, and when.
//
// Two halves with nothing in common but the types. This file is the part that
// needs no GPU and no samples: reading and writing a segment's annotations,
// and answering a query over annotations already in hand. index.h is the part
// that produces annotations by running the detector over a segment's samples.
//
// THE INPUT IS A LIST OF SEGMENTS, NOT A CAPTURE ROOT. Whatever lists the
// root (catalog.h) hands its SegmentInfo and the annotations it read into
// IndexedSegment, and everything here is a pure function of that list. So a
// query is testable over hand-made annotations, and a caller that already has
// the catalog in memory never re-reads a meta file to search it.
//
// WALL CLOCK IS DERIVED HERE AND NOWHERE ELSE. An annotation is stored in
// sample indices relative to its segment, which is what format.h says and
// what keeps it exact. A hit converts once, through the segment's
// BlockTimestamp, which is the same arithmetic the writer used to name the
// file.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/capture/format.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::capture {

// core:generator values, format.h's.
inline constexpr std::string_view kDetectGenerator = "revenant-detect";
inline constexpr std::string_view kIndexGenerator = "revenant-index";

// One core:annotations entry as read back from a meta file.
//
// format.h's Annotation carries what an annotation says about the signal and
// not who said it, so the generator and the live track id ride beside it.
// A query filters on the generator, which is how a caller asks for the live
// detector's view, the offline index's, or both.
struct StoredAnnotation {
    Annotation annotation;
    std::string generator;

    // The live detector's track id, zero when the entry carried none.
    std::uint64_t track_id = 0;
};

// One revenant:indexed entry: a pass that ran over the segment, whether or not
// it found anything. Annotations alone cannot say a segment was indexed,
// because a pass over a quiet segment leaves none, and without this record a
// quiet segment is indistinguishable from one nobody looked at.
struct IndexStamp {
    std::string generator;
    // ISO 8601 UTC, when the pass wrote its result.
    std::string at;
    // Free text naming the settings the pass ran with, so a later pass can
    // tell whether it would see the segment any differently.
    std::string settings;
};

// One closed segment and what is known about it, the unit every query reads.
struct IndexedSegment {
    // The .sigmf-data file. Carried into every hit so a caller can open the
    // samples without asking the catalog again.
    std::string data_path;
    SegmentInfo info;
    std::vector<StoredAnnotation> annotations;
    std::vector<IndexStamp> stamps;
};

// True when a pass under `generator` is recorded, by its stamp or, for a meta
// written before stamps existed, by an annotation it left.
[[nodiscard]] bool indexed_by(std::span<const IndexStamp> stamps,
                              std::span<const StoredAnnotation> annotations,
                              std::string_view generator = kIndexGenerator);

// Reads core:annotations out of a .sigmf-meta file. An entry missing
// core:sample_start is refused, since an annotation with no position in the
// segment cannot be searched; every other key is optional and defaults.
// A file with no annotations array reads as none.
[[nodiscard]] Expected<std::vector<StoredAnnotation>> read_annotations(
    const std::string& meta_path);

// Reads global revenant:indexed. A file without it reads as none; an entry
// with no generator is refused, since it cannot say which pass it records.
[[nodiscard]] Expected<std::vector<IndexStamp>> read_index_stamps(const std::string& meta_path);

// Replaces every entry whose core:generator is `generator` with `annotations`,
// written under that generator, and leaves the rest of the file alone: every
// other top-level key, every entry from another generator, byte for byte.
// The merged array is ordered by core:sample_start, as SigMF asks.
//
// Also records the pass in global revenant:indexed, replacing any earlier
// stamp from the same generator, and does so for an empty `annotations` too:
// that is the case the stamp exists for. `settings` goes into the stamp as
// given. Every other global key is left byte for byte.
//
// Written to a temporary file beside the meta and renamed over it, so a
// reader never sees half a file and a crash mid-write leaves the old one.
//
// Re-indexing a segment is therefore idempotent: the second run replaces the
// first run's entries rather than adding to them, and never touches what the
// live detector wrote.
[[nodiscard]] Status write_annotations(const std::string& meta_path,
                                       std::span<const Annotation> annotations,
                                       std::string_view generator = kIndexGenerator,
                                       std::string_view settings = {});

// The same merge on text in memory, for a caller that holds the meta already
// and for tests. Without a stamp the global object is not touched; with one,
// the stamp's generator should match `generator`.
[[nodiscard]] Expected<std::string> merge_annotations(
    std::string_view meta_text, std::span<const Annotation> annotations,
    std::string_view generator, const std::optional<IndexStamp>& stamp = std::nullopt);

// Adds `annotations` to the meta's array, each under its own generator and
// with its track id when nonzero, and removes nothing: every existing entry
// and every other key stays byte for byte. The writer files the part of a
// live track that fell in an already-closed segment this way, so a second
// call adds to the first rather than replacing it.
[[nodiscard]] Expected<std::string> append_annotations(
    std::string_view meta_text, std::span<const StoredAnnotation> annotations);

// The same on a file, replaced through a temporary and a rename as
// write_annotations does.
[[nodiscard]] Status append_annotations_to(const std::string& meta_path,
                                           std::span<const StoredAnnotation> annotations);

// Every field is a filter, and an unset one passes everything.
//
// Time and frequency are overlap tests, not containment: a hit is anything
// that was present for any part of [from_ns, to_ns) and occupied any part of
// [freq_low, freq_high]. A question of the form "what was on 146.52 between
// two and three" wants the transmission that started at 1:59.
struct Query {
    // Unix epoch nanoseconds.
    std::optional<std::int64_t> from_ns;
    std::optional<std::int64_t> to_ns;

    std::optional<dsp::Hertz> freq_low;
    std::optional<dsp::Hertz> freq_high;

    double min_confidence = 0.0;
    std::optional<double> min_snr_db;

    // Nanoseconds of wall clock, so one number means the same at any rate.
    std::int64_t min_duration_ns = 0;

    // Matched case-insensitively anywhere in the family. Non-empty excludes
    // every annotation with no family at all.
    std::string family;

    // Exact. Empty takes every generator.
    std::string generator;
};

struct Hit {
    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0;
    dsp::Hertz freq_lower = 0;
    dsp::Hertz freq_upper = 0;

    std::string data_path;

    // Relative to the segment, as stored. stream_epoch and first_index make
    // them absolute within the stream.
    std::uint64_t sample_start = 0;
    std::uint64_t sample_count = 0;
    std::uint64_t stream_epoch = 0;
    dsp::SampleIndex first_index = 0;

    double confidence = 0.0;
    double peak_db = 0.0;
    double snr_db = 0.0;
    std::string family;
    std::string generator;
};

// Every annotation passing the query, ordered by start, then by lower edge,
// then by path, so the order is total and two runs print the same list.
[[nodiscard]] std::vector<Hit> run_query(std::span<const IndexedSegment> segments,
                                         const Query& query);

// "What happened in this band, and when": a time range cut into equal buckets
// and, for each, how much of it something was present in the frequency range.
struct OccupancyRequest {
    std::int64_t from_ns = 0;
    std::int64_t to_ns = 0;
    std::int64_t bucket_ns = 0;
    dsp::Hertz freq_low = 0;
    dsp::Hertz freq_high = 0;

    // Applied to each annotation before it counts. Its own time and frequency
    // fields are ignored in favour of the ones above.
    Query filter;
};

struct OccupancyBucket {
    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0;

    // Share of the bucket in which at least one annotation was present.
    // Overlapping annotations count once: two generators that saw the same
    // burst, or two signals at once, are one stretch of occupied time.
    double occupied = 0.0;

    // Share of the bucket a segment covered at all. Without it an empty
    // bucket cannot say whether the band was quiet or nothing was recording,
    // and those are opposite answers.
    double captured = 0.0;

    // Annotations that overlapped the bucket.
    std::uint32_t hits = 0;
};

// Refuses an empty or inverted range and a bucket that is not positive. The
// last bucket is cut short at to_ns rather than running past it.
[[nodiscard]] Expected<std::vector<OccupancyBucket>> occupancy(
    std::span<const IndexedSegment> segments, const OccupancyRequest& request);

}  // namespace revenant::capture
