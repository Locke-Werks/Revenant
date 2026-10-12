// What is in a capture root, read back from the files on disk.
//
// The catalog is the reading half of core/capture/format.h. It lists the day
// directories, reads every closed segment's .sigmf-meta into a SegmentInfo and
// its annotations, and answers the questions replay and search ask: which
// segments cover this stretch of wall clock, which one holds this sample of
// this stream, how much disk the capture is using.
//
// THE FILES ARE THE INDEX. There is no database beside the segments and no
// manifest the writer has to keep in step with them. A manifest is a second
// copy of the truth, and the first time the writer dies between writing a
// segment and updating the manifest the two disagree with nothing to say which
// is right. Reading the metas costs a directory walk and a few kilobytes of
// JSON per segment, which at the format's segment sizes is a few thousand
// files for a full ring and well under a second; refresh() then parses only
// the metas it has not seen before, so a long-lived catalog that is refreshed
// once a second costs one directory listing a second.
//
// A BAD FILE IS A FINDING, NOT A FAILURE. One malformed meta in a ring of
// thousands does not stop the other thousands being replayable, so the scan
// carries on and the meta is reported with its path and the reason. Only a
// root that cannot be listed at all fails the scan. An open data file (the
// stem ends in kOpenSuffix) is a segment still being written and is reported
// as in progress; a closed-looking data file with no meta beside it is a
// segment whose writer died before the rename, and is reported as orphaned
// rather than guessed at. Neither is ever presented as a segment.
//
// A plain value. Lookups return copies, so a result stays valid across a
// refresh and a caller on another thread never holds a pointer into a vector
// that refresh is rebuilding. A Catalog is not itself synchronised: one
// thread refreshes it, or the caller locks.

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

// One closed segment as the catalog read it.
struct CatalogSegment {
    SegmentInfo info;

    // UTF-8 paths, as every path in this tree is.
    std::string data_path;
    std::string meta_path;

    // Size of the data file. sample_count is this divided by the sample
    // width, and a remainder is a malformed segment rather than a rounding.
    std::uint64_t data_bytes = 0;
    std::uint64_t meta_bytes = 0;

    // revenant:close_reason, empty when the meta did not carry one.
    std::string close_reason;

    std::vector<Annotation> annotations;

    // Wall clock one sample past the last, on the same derivation as
    // SegmentInfo::start_ns, so a contiguous successor starts exactly here.
    [[nodiscard]] std::int64_t end_ns() const;
};

// A data file the catalog will not present as a segment.
struct UnfinishedSegment {
    std::string data_path;
    std::uint64_t data_bytes = 0;

    // True for <stem>.open.sigmf-data, a segment still being written. False
    // for a closed name with no meta beside it, which is a writer that died.
    bool in_progress = false;
};

// A meta that could not be read, and why.
struct CatalogProblem {
    std::string path;
    std::string reason;
};

// What a meta says, without the filesystem around it. Exposed so a test can
// check the parsing of one awkward sidecar without building a whole root, and
// so the writer's tests can round-trip what they wrote.
struct ParsedMeta {
    SegmentInfo info;
    std::string close_reason;
    std::vector<Annotation> annotations;
};

// `data_bytes` is the size of the data file beside the meta, which is where
// sample_count comes from: SigMF does not state a length, and the format adds
// none, so the extent on disk is the length.
[[nodiscard]] Expected<ParsedMeta> parse_segment_meta(std::string_view json,
                                                      std::uint64_t data_bytes);

// An instant as a capture URI or a caller states one: an ISO 8601 UTC time,
// 2026-10-12T14:03:07.250Z, with the Z required, or a whole number of
// nanoseconds since the Unix epoch. A time with an offset or no zone is
// refused for the reason file_source.cpp refuses one in core:datetime: read as
// UTC it is wrong by whole hours and looks right.
[[nodiscard]] Expected<std::int64_t> parse_capture_instant(std::string_view text);

class Catalog {
public:
    // Lists the root once. Fails only when the root does not exist or cannot
    // be listed; everything inside it that is wrong lands in problems().
    [[nodiscard]] static Expected<Catalog> open(std::string root);

    // Picks up segments closed since the last scan and drops ones the ring
    // has deleted. Parses only metas it has not seen; a meta already reported
    // as a problem is read again only if its size changed, which is what a
    // rewrite by an offline index pass looks like.
    [[nodiscard]] Status refresh();

    [[nodiscard]] const std::string& root() const { return root_; }

    // Every closed segment, ordered by start time, then stream_epoch, then
    // first index, so the order is total even when two start together.
    [[nodiscard]] std::span<const CatalogSegment> segments() const { return segments_; }

    [[nodiscard]] std::span<const UnfinishedSegment> unfinished() const { return unfinished_; }
    [[nodiscard]] std::span<const CatalogProblem> problems() const { return problems_; }

    // Segments with any sample in [from_ns, to_ns), in segments() order.
    [[nodiscard]] std::vector<CatalogSegment> overlapping(std::int64_t from_ns,
                                                          std::int64_t to_ns) const;

    // The segment of `stream_epoch` holding sample `index`, or nothing when
    // the index falls in a gap or outside what is on disk.
    [[nodiscard]] std::optional<CatalogSegment> containing(std::uint64_t stream_epoch,
                                                           dsp::SampleIndex index) const;

    // The segment whose samples cover the instant, or nothing in a gap. When
    // two streams overlap in time, which takes two writers on one root, the
    // one that started later wins, because it is the one still being added to.
    [[nodiscard]] std::optional<CatalogSegment> at_time(std::int64_t instant_ns) const;

    // Everything on disk under the root that the capture owns: closed
    // segments' data and meta, plus unfinished data files. This is the number
    // a ring budget is measured against, and an orphan uses disk whether or
    // not it is replayable.
    [[nodiscard]] std::uint64_t total_bytes() const;

private:
    Catalog() = default;

    struct KnownProblem {
        std::string path;
        std::uint64_t size = 0;
        std::string reason;
    };

    std::string root_;
    std::vector<CatalogSegment> segments_;
    std::vector<UnfinishedSegment> unfinished_;
    std::vector<CatalogProblem> problems_;
    std::vector<KnownProblem> known_problems_;
};

}  // namespace revenant::capture
