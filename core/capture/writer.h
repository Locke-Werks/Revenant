// The rolling capture's writer and the ring that bounds it.
//
// format.h is the contract and this file follows it; what is stated here is
// only what format.h leaves to the writer.
//
// THE SINK THREAD NEVER TOUCHES THE DISK. A live RTL-SDR is a Paced source:
// its sink has to return on the device's schedule, and a write that stalls on
// a busy disk, an antivirus scan or a directory being created would turn into
// an overrun in the device, which is lost samples in the live graph as well as
// in the capture. consume() copies the block into a pooled buffer and queues
// it; one thread of the writer's own does every open, write, rename and
// delete. The queue is a mutex and a deque rather than a lock-free ring
// because the critical section is a pointer swap, the copy happens outside it,
// and the alternative is a ring of variable-size byte blocks for no measured
// gain. The four facts docs/conventions.md asks of a ring:
//
//   writer     the source's delivery thread, through consume(). One only.
//   reader     the writer thread, started by start() and joined by stop().
//   ownership  a buffer taken from the free pool belongs to the producer
//              until it is queued, then to the writer thread until it is
//              returned to the pool. Never both.
//   behind     the queue holds WriterConfig::queue_blocks blocks. A block
//              arriving to a full queue is DROPPED and counted in
//              WriterStats::blocks_dropped, and the open segment is closed
//              with close_reason "loss" before the next block that does get
//              in, whose segment then carries the gap in
//              revenant:dropped_before. A segment stays gapless whatever the
//              disk does.
//
// WHAT FORMAT.H'S STEM DOES NOT CARRY, AND THE SIDE FILE THAT DOES. format.h
// says the open name carries everything the meta needs except the length.
// The stem it specifies carries the first sample's wall clock, the centre,
// the stream epoch and the first index. It does not carry the sample rate,
// the sample format or the exact epoch anchor (the stem's time is rounded to
// the millisecond, the anchor is nanoseconds and is the stream's, not the
// segment's), nor dropped_before, the gain or core:hw. Without the rate and
// the format a dead writer's data file cannot be turned back into samples at
// all. So beside <stem>.open.sigmf-data the writer puts <stem>.open.revenant,
// a few lines of key=value text written before the first sample, and recovery
// reads it. It is removed when the segment closes, so a closed segment is the
// two files format.h names and nothing else. format.h itself is unchanged;
// extending the stem would be the alternative and is that file's decision.
//
// WHAT THE SEGMENT DOES NOT KNOW FROM THE BLOCK. SourceBlock carries no centre
// frequency, so the caller passes the centre in force with every block, and a
// retune is closed at the first block the caller attributes to the new centre.
// The writer can be no more exact about a retune than the caller is.
//
// ANNOTATIONS ARRIVE LATE. add_annotation is keyed by absolute stream index and
// held until the segment containing that index closes. One that arrives after
// its segment closed is discarded and counted, because rewriting a closed meta
// would race the catalog reading it; an offline index pass is where that
// segment gets its annotations back.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/capture/format.h"
#include "core/dsp/types.h"
#include "core/error.h"
#include "core/source/capabilities.h"
#include "core/source/source.h"

namespace revenant::capture {

// The side file beside an open data file. See the header comment.
inline constexpr std::string_view kSideExtension = ".revenant";

struct WriterConfig {
    std::filesystem::path root;

    // A segment closes with reason "size" at the first block boundary past
    // either limit. Zero disables that limit. 256 MiB is about 56 seconds of
    // cu8 at 2.4 MS/s: small enough that retention frees space in useful
    // steps, large enough that the directory stays short.
    std::uint64_t max_segment_bytes = 256ULL * 1024 * 1024;
    double max_segment_seconds = 0.0;

    // Retention, applied after every close. Zero disables that limit. The
    // budget counts closed segments and the open one; only closed ones are
    // ever deleted.
    std::uint64_t retention_bytes = 0;
    // Age is measured from a segment's first sample.
    std::int64_t retention_max_age_ns = 0;

    // Blocks the queue holds before it drops. At 2.4 MS/s cu8 with the
    // RTL-SDR's 16384-sample transfers, 256 blocks is about 1.7 seconds of
    // disk stall absorbed.
    std::size_t queue_blocks = 256;

    // Zero draws one at start(). Sample indices are comparable only within an
    // epoch, so a caller that restarts its source restarts the writer too.
    std::uint64_t stream_epoch = 0;

    // core:hw, the source's description.
    std::string hw;

    // Finalise any segment a dead writer left before writing the first new
    // one, so a crash costs nothing but the samples still in the queue.
    bool recover_on_start = true;

    // The wall clock retention ages against. Injected so a test can age a
    // segment without waiting; the default reads the system clock.
    std::function<std::int64_t()> now_ns;
};

struct WriterStats {
    std::uint64_t segments_written = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t blocks_dropped = 0;
    std::uint64_t samples_dropped = 0;
    // Blocks discarded because writing had stopped on an error.
    std::uint64_t blocks_unwritten = 0;
    std::uint64_t segments_deleted = 0;
    // Ring deletes the filesystem refused, usually because a reader holds the
    // file without sharing delete. Each one stays on the pending list, is
    // retried at every later close, and moves to segments_deleted only once
    // both files are actually gone.
    std::uint64_t delete_failures = 0;
    std::uint64_t pending_deletes = 0;
    // Data files a previous run's ring had already given up on, found and
    // deleted at start(). Also counted in segments_deleted.
    std::uint64_t orphans_deleted = 0;
    std::uint64_t annotations_discarded = 0;
    std::uint64_t retained_bytes = 0;
    // Empty while healthy. Once set, the writer has stopped writing and stays
    // stopped until it is restarted; the stream it is attached to carries on.
    std::string last_error;
};

struct RecoveryReport {
    std::uint64_t recovered = 0;
    // Zero-length leftovers, removed rather than finalised.
    std::uint64_t removed_empty = 0;
    // One line per open data file that could not be finalised, naming it.
    std::vector<std::string> failures;
};

// The stem format.h specifies, and its inverse.
[[nodiscard]] std::string make_stem(const SegmentInfo& info);
struct StemFields {
    std::int64_t start_ns = 0;  // millisecond precision, from the name
    dsp::Hertz center = 0;
    std::uint64_t stream_epoch = 0;
    dsp::SampleIndex first_index = 0;
};
[[nodiscard]] Expected<StemFields> parse_stem(std::string_view stem);

// The <YYYY-MM-DD> day directory for a wall clock.
[[nodiscard]] std::string day_directory(std::int64_t start_ns);

// The SigMF meta for a closed segment, as text.
struct MetaFields {
    SegmentInfo info;
    std::string close_reason;
    std::string hw;
    std::optional<double> gain_db;
    struct Entry {
        Annotation annotation;
        std::optional<std::uint64_t> track_id;
        std::string generator = "revenant-detect";
    };
    std::vector<Entry> annotations;
};
[[nodiscard]] std::string render_meta(const MetaFields& fields);

// Finalises every <stem>.open.sigmf-data under root with close_reason
// "recovered", from the stem, the side file and the data file's length. A
// trailing partial sample is truncated so the extent equals the bytes on disk.
[[nodiscard]] Expected<RecoveryReport> recover_open_segments(const std::filesystem::path& root);

class SegmentWriter {
public:
    explicit SegmentWriter(WriterConfig config);
    ~SegmentWriter();

    SegmentWriter(const SegmentWriter&) = delete;
    SegmentWriter& operator=(const SegmentWriter&) = delete;
    SegmentWriter(SegmentWriter&&) = delete;
    SegmentWriter& operator=(SegmentWriter&&) = delete;

    // Recovers, indexes what is already on disk for retention, and starts the
    // writer thread. Fails only on a root that cannot be created or read.
    [[nodiscard]] Status start();

    // Drains the queue, closes the open segment with reason "stop", applies
    // retention and joins. Safe to call twice.
    void stop();

    // Call from the source's sink with the centre in force for this block.
    // Never blocks on I/O and never fails the stream: a full queue or a dead
    // disk is counted in stats() instead.
    void consume(const source::SourceBlock& block, dsp::Hertz center);

    // A BlockSink that forwards to consume() at a centre read from `center`
    // on every block. The callable must be safe on the delivery thread.
    [[nodiscard]] source::BlockSink sink(std::function<dsp::Hertz()> center);

    // The gain in force, written as revenant:gain_db on segments that open
    // after this call. A gain change does not close a segment, because
    // format.h does not make gain part of what a segment holds constant, so
    // the value is the one at the segment's first sample. Nothing (the
    // default) writes no key.
    void set_gain_db(std::optional<double> gain_db);

    // `annotation.sample_start` is ignored; `absolute_start` is the stream
    // index of the annotation's first sample. Converted to segment-relative
    // when the segment that contains it closes, with the count clipped to the
    // segment.
    void add_annotation(dsp::SampleIndex absolute_start, const Annotation& annotation,
                        std::optional<std::uint64_t> track_id = std::nullopt);

    [[nodiscard]] WriterStats stats() const;
    [[nodiscard]] std::uint64_t stream_epoch() const { return config_.stream_epoch; }

private:
    struct Item {
        dsp::BlockTimestamp stamp;
        source::SampleFormat format = source::SampleFormat::Cu8;
        std::uint64_t sample_count = 0;
        dsp::Hertz center = 0;
        // Samples lost immediately before this block, by the device or by
        // this queue.
        std::uint64_t lost_before = 0;
        bool loss = false;
        std::vector<std::byte> bytes;
    };

    struct PendingAnnotation {
        dsp::SampleIndex absolute_start = 0;
        Annotation annotation;
        std::optional<std::uint64_t> track_id;
    };

    struct OpenSegment;
    struct RetainedSegment {
        std::filesystem::path data;
        std::filesystem::path meta;
        std::uint64_t bytes = 0;
        std::int64_t start_ns = 0;
        std::string order_key;
    };

    void run();
    void process(Item& item);
    void open_segment(const Item& item, std::uint64_t dropped_before);
    void close_segment(std::string_view reason);
    void apply_retention();
    [[nodiscard]] bool remove_pair(const RetainedSegment& segment);
    void retry_pending_deletes();
    void index_existing();
    void adopt_orphans();
    void fail_writing(std::string message);
    [[nodiscard]] std::int64_t now() const;

    WriterConfig config_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Item> queue_;
    std::vector<std::vector<std::byte>> pool_;
    std::size_t buffers_out_ = 0;
    bool stopping_ = false;
    bool running_ = false;
    bool errored_ = false;
    std::uint64_t pending_loss_samples_ = 0;
    bool pending_loss_ = false;
    std::optional<double> gain_db_;
    std::vector<PendingAnnotation> annotations_;
    WriterStats stats_;

    // Writer thread only.
    std::unique_ptr<OpenSegment> open_;
    std::vector<RetainedSegment> retained_;
    // Pairs the ring chose to delete and could not. Their bytes are still on
    // disk, so they still count against the budget and in retained_bytes.
    std::vector<RetainedSegment> pending_delete_;
    bool have_last_ = false;
    dsp::SampleIndex last_end_ = 0;

    std::thread thread_;
};

}  // namespace revenant::capture
