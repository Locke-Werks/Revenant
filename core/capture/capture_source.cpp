// The capture replay source. capture_source.h has what it plays and why it
// stops where it does; this file has the delivery loop.
//
// It reads the segments directly rather than nesting a FileSource per
// segment. A FileSource owns a thread, a sink and a stop protocol of its own,
// and chaining them would put a second delivery thread and a second mutex
// under every block for the sake of one fread. What it does share with the
// file backend is the shape of the loop, line for line where it can: fill a
// block, honour the pace, call the sink, stop if it complained.

#include "core/capture/capture_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "core/capture/catalog.h"
#include "core/thread_role.h"

namespace revenant::capture {
namespace {

using source::SampleFormat;

// Same figures as the file backend, for the same reasons: a quarter second at
// 20 MS/s per block, and a ceiling past which a block is a buffer.
constexpr std::size_t kDefaultBlockSamples = 1u << 18;
constexpr std::size_t kMaxBlockSamples = 1u << 24;

// The format carries no statement of how good the writer's anchor was, so
// this is the recorder-clock figure file_source.cpp gives a SigMF datetime:
// an NTP-held PC clock, one second, sitting exactly on the boundary a slotted
// mode needs so a decoder can see it is only just enough.
constexpr std::int64_t kCaptureAnchorAccuracyNs = 1'000'000'000;

constexpr dsp::SampleIndex kNoPendingSeek = std::numeric_limits<dsp::SampleIndex>::max();

struct FileCloser {
    void operator()(std::FILE* file) const noexcept
    {
        if (file != nullptr) {
            static_cast<void>(std::fclose(file));
        }
    }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

[[nodiscard]] Expected<FilePtr> open_binary(const std::string& utf8)
{
    std::FILE* raw = nullptr;
#if defined(_MSC_VER)
    const std::filesystem::path path(
        std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
    const errno_t error = _wfopen_s(&raw, path.c_str(), L"rb");
    if (error != 0 || raw == nullptr) {
        return fail(std::format("could not open segment '{}' for reading. The ring may have "
                                "removed it since the capture was opened.",
                                utf8),
                    error);
    }
#else
    raw = std::fopen(utf8.c_str(), "rb");
    if (raw == nullptr) {
        return fail(std::format("could not open segment '{}' for reading. The ring may have "
                                "removed it since the capture was opened.",
                                utf8));
    }
#endif
    return FilePtr(raw);
}

[[nodiscard]] bool seek_absolute(std::FILE* file, std::int64_t offset)
{
#if defined(_MSC_VER)
    return _fseeki64(file, offset, SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

[[nodiscard]] std::int64_t duration_ns_of(dsp::SampleIndex samples, dsp::SampleRate rate)
{
    if (rate <= 0) {
        return 0;
    }
    const auto rate_u = static_cast<dsp::SampleIndex>(rate);
    const auto whole_seconds = static_cast<std::int64_t>(samples / rate_u);
    const auto remainder = static_cast<std::int64_t>(samples % rate_u);
    return whole_seconds * 1'000'000'000 + (remainder * 1'000'000'000) / rate;
}

// The first sample at or after `ns` past a segment's start, rounding up so
// that from= never plays a sample captured before the instant asked for.
// Split for the overflow reason duration_ns_of splits.
[[nodiscard]] dsp::SampleIndex samples_at_or_after(std::int64_t ns, dsp::SampleRate rate)
{
    if (ns <= 0 || rate <= 0) {
        return 0;
    }
    const auto seconds = static_cast<dsp::SampleIndex>(ns / 1'000'000'000);
    const auto rest = static_cast<dsp::SampleIndex>(ns % 1'000'000'000);
    const auto rate_u = static_cast<dsp::SampleIndex>(rate);
    return seconds * rate_u + (rest * rate_u + 999'999'999) / 1'000'000'000;
}

// The index within `segment` of the first sample at or after `instant_ns`,
// clamped to the segment.
[[nodiscard]] dsp::SampleIndex index_at(const SegmentInfo& segment, std::int64_t instant_ns)
{
    const std::int64_t start = segment.start_ns();
    if (instant_ns <= start) {
        return segment.first_index;
    }
    const dsp::SampleIndex offset = samples_at_or_after(instant_ns - start, segment.rate);
    return segment.first_index + std::min<dsp::SampleIndex>(offset, segment.sample_count);
}

struct RunSegment {
    std::string data_path;
    dsp::SampleIndex first = 0;
    dsp::SampleIndex end = 0;
};

// Everything open() settles from the catalog, so describe and open cannot
// disagree about what a URI plays.
struct Run {
    std::vector<RunSegment> segments;
    std::uint64_t stream_epoch = 0;
    std::int64_t anchor_ns = 0;
    dsp::SampleRate rate = 0;
    dsp::Hertz center = 0;
    SampleFormat format = SampleFormat::Cu8;
    dsp::SampleIndex start_index = 0;
    dsp::SampleIndex end_index = 0;
    source::SourceCapabilities caps;
};

[[nodiscard]] std::string describe_change(const SegmentInfo& was, const CatalogSegment& next)
{
    if (next.info.stream_epoch != was.stream_epoch) {
        return std::format("a new stream_epoch {:016x} begins at '{}'", next.info.stream_epoch,
                           next.data_path);
    }
    if (next.info.center != was.center) {
        return std::format("the capture retunes from {} Hz to {} Hz at '{}'", was.center,
                           next.info.center, next.data_path);
    }
    if (next.info.rate != was.rate) {
        return std::format("the sample rate changes from {} to {} S/s at '{}'", was.rate,
                           next.info.rate, next.data_path);
    }
    if (next.info.format != was.format) {
        return std::format("the sample format changes from {} to {} at '{}'",
                           source::format_name(was.format), source::format_name(next.info.format),
                           next.data_path);
    }
    return std::format("'{}' overlaps the segment before it, starting at index {} before {} "
                       "ended",
                       next.data_path, next.info.first_index, was.end_index());
}

[[nodiscard]] Expected<Run> resolve_run(const CaptureSourceConfig& config)
{
    if (config.root.empty()) {
        return fail("a capture URI needs a root: capture:///D:/revenant/capture");
    }
    if (config.from_given && config.to_given && config.to_ns <= config.from_ns) {
        return fail(std::format("to= ({} ns) is not after from= ({} ns)", config.to_ns,
                                config.from_ns));
    }

    auto catalog = Catalog::open(config.root);
    if (!catalog) {
        return std::unexpected(catalog.error());
    }
    const std::span<const CatalogSegment> all = catalog->segments();
    if (all.empty()) {
        return fail(std::format("capture root '{}' holds no closed segments ({} still being "
                                "written or orphaned, {} unreadable metas)",
                                config.root, catalog->unfinished().size(),
                                catalog->problems().size()));
    }

    // The first segment with any sample at or after from=. Segments sort by
    // start, so this is the earliest one that has not wholly ended before it.
    const CatalogSegment* first = nullptr;
    for (const CatalogSegment& segment : all) {
        if (!config.from_given || segment.end_ns() > config.from_ns) {
            first = &segment;
            break;
        }
    }
    if (first == nullptr) {
        return fail(std::format("capture root '{}' has nothing at or after from= ({} ns); its "
                                "newest segment ends at {} ns",
                                config.root, config.from_ns, all.back().end_ns()));
    }

    // The rest of the run, walked in index order within the epoch.
    std::vector<const CatalogSegment*> epoch;
    for (const CatalogSegment& segment : all) {
        if (segment.info.stream_epoch == first->info.stream_epoch &&
            segment.info.first_index >= first->info.first_index) {
            epoch.push_back(&segment);
        }
    }
    std::sort(epoch.begin(), epoch.end(), [](const CatalogSegment* a, const CatalogSegment* b) {
        return a->info.first_index < b->info.first_index;
    });

    Run run;
    run.stream_epoch = first->info.stream_epoch;
    run.anchor_ns = first->info.epoch_anchor_ns;
    run.rate = first->info.rate;
    run.center = first->info.center;
    run.format = first->info.format;

    std::vector<std::string> notes;
    const CatalogSegment* last = nullptr;
    std::uint64_t gaps = 0;
    std::uint64_t lost = 0;
    for (const CatalogSegment* segment : epoch) {
        if (config.to_given && segment->info.start_ns() >= config.to_ns) {
            break;
        }
        if (last != nullptr) {
            const bool same = segment->info.center == run.center &&
                              segment->info.rate == run.rate &&
                              segment->info.format == run.format &&
                              segment->info.epoch_anchor_ns == run.anchor_ns &&
                              segment->info.first_index >= last->info.end_index();
            if (!same) {
                notes.push_back(std::format(
                    "replay stops at index {}: {}. The engine cannot follow a retune, a rate or "
                    "format change or a new stream from a source mid-stream; open the capture "
                    "again with from= at that segment to continue.",
                    last->info.end_index(), describe_change(last->info, *segment)));
                break;
            }
            if (segment->info.first_index > last->info.end_index()) {
                ++gaps;
                lost += segment->info.first_index - last->info.end_index();
            }
        }
        run.segments.push_back(RunSegment{segment->data_path, segment->info.first_index,
                                          segment->info.end_index()});
        last = segment;
    }

    // A later stream in the root says so, so a replay that ends early is not
    // mistaken for the end of the capture.
    if (notes.empty() && last != nullptr && !config.to_given) {
        for (const CatalogSegment& segment : all) {
            if (segment.info.stream_epoch != run.stream_epoch &&
                segment.info.start_ns() >= last->info.start_ns()) {
                notes.push_back(std::format("replay stops at index {}: {}", last->info.end_index(),
                                            describe_change(last->info, segment)));
                break;
            }
        }
    }

    if (run.segments.empty()) {
        return fail(std::format("capture root '{}' has nothing between from= and to=",
                                config.root));
    }

    run.start_index = config.from_given ? index_at(first->info, config.from_ns)
                                        : first->info.first_index;
    run.end_index = last->info.end_index();
    if (config.to_given) {
        run.end_index = std::min(run.end_index, index_at(last->info, config.to_ns));
    }
    if (run.start_index >= run.end_index) {
        return fail(std::format("capture root '{}' has no samples between from= and to=",
                                config.root));
    }

    if (gaps > 0) {
        notes.push_back(std::format("the run has {} gap{} totalling {} lost samples, each "
                                    "reported as dropped_before on the block after it",
                                    gaps, gaps == 1 ? "" : "s", lost));
    }
    notes.push_back(std::format("stream_epoch {:016x}, {} segment{}, indices {} to {}",
                                run.stream_epoch, run.segments.size(),
                                run.segments.size() == 1 ? "" : "s", run.start_index,
                                run.end_index));

    source::SourceCapabilities& caps = run.caps;
    caps.uri = config.uri;
    caps.backend = "capture";
    caps.display_name = config.display_name.empty()
                            ? std::format("Capture at {}", config.root)
                            : config.display_name;
    caps.notes = std::move(notes);
    caps.sample_rates = {run.rate};
    caps.min_rate = run.rate;
    caps.max_rate = run.rate;
    caps.native_format = run.format;
    caps.clock_sources = {source::ClockSource::Internal};
    caps.flow = source::FlowControl::Demand;
    caps.seekable = true;
    // The stream's own indices are the stamps, so the length a caller checks
    // a seek against is the index one past the last sample, not a count.
    caps.length_samples = run.end_index;
    caps.preferred_block_samples = kDefaultBlockSamples;
    caps.timestamp_accuracy_ns = kCaptureAnchorAccuracyNs;
    return run;
}

class CaptureSource final : public source::Source {
public:
    explicit CaptureSource(Run run, std::optional<double> own_pace)
        : run_(std::move(run)),
          bytes_per_sample_(source::bytes_per_sample(run_.format)),
          own_pace_(own_pace)
    {
        if (own_pace_) {
            pace_.store(*own_pace_, std::memory_order_relaxed);
        }
        position_ = run_.start_index;
        write_index_.store(position_, std::memory_order_relaxed);
    }

    ~CaptureSource() override
    {
        std::scoped_lock lock(control_);
        join_locked();
    }

    [[nodiscard]] const source::SourceCapabilities& capabilities() const override
    {
        return run_.caps;
    }

    [[nodiscard]] Expected<dsp::Hertz> tune(dsp::Hertz center) override
    {
        return fail(std::format(
            "a capture cannot tune to {} Hz: its centre is where the radio was pointed when the "
            "samples were written. Open the capture at a time it was tuned there instead.",
            center));
    }
    [[nodiscard]] dsp::Hertz center() const override { return run_.center; }

    [[nodiscard]] Expected<dsp::SampleRate> set_sample_rate(dsp::SampleRate rate) override
    {
        if (rate == run_.rate) {
            return run_.rate;
        }
        return fail(std::format("a capture plays at the {} S/s it was written at, not {} S/s",
                                run_.rate, rate));
    }
    [[nodiscard]] dsp::SampleRate sample_rate() const override { return run_.rate; }

    [[nodiscard]] Expected<double> set_gain(std::string_view stage, double) override
    {
        return fail(std::format("a capture has no gain stage '{}': the gain was applied before "
                                "the samples were written",
                                stage));
    }
    [[nodiscard]] Status set_gain_auto(std::string_view stage, bool) override
    {
        return fail(std::format("a capture has no gain stage '{}' to put into automatic mode",
                                stage));
    }

    [[nodiscard]] Status start(const source::StreamOptions& options,
                               source::BlockSink sink) override;
    [[nodiscard]] Status stop() override
    {
        std::scoped_lock lock(control_);
        join_locked();
        if (has_stop_error_) {
            return std::unexpected(stop_error_);
        }
        return {};
    }
    [[nodiscard]] bool running() const override
    {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] Status seek(dsp::SampleIndex index) override;

    [[nodiscard]] source::SourceStats stats() const override
    {
        source::SourceStats out;
        out.blocks_delivered = blocks_delivered_.load(std::memory_order_relaxed);
        out.samples_delivered = samples_delivered_.load(std::memory_order_relaxed);
        // Lost samples in the capture are reported on the blocks, not here.
        // These count what this source lost, and a Demand source loses
        // nothing: the writer's loss is history being replayed, not a fault
        // of the replay.
        out.write_index = write_index_.load(std::memory_order_relaxed);
        return out;
    }

    [[nodiscard]] source::ClockQuality clock() const override
    {
        source::ClockQuality quality;
        quality.source = source::ClockSource::Internal;
        quality.accuracy_ns = kCaptureAnchorAccuracyNs;
        return quality;
    }

    [[nodiscard]] std::optional<double> own_pace() const override
    {
        std::scoped_lock lock(control_);
        return own_pace_;
    }

    [[nodiscard]] Status set_pace(double pace) override
    {
        if (!std::isfinite(pace) || pace < 0.0) {
            return fail(std::format("pace must be zero for unthrottled or a positive multiple "
                                    "of realtime, got {}",
                                    pace));
        }
        std::scoped_lock lock(control_);
        own_pace_ = pace;
        pace_.store(pace, std::memory_order_release);
        return {};
    }

private:
    void run();
    void join_locked()
    {
        stop_requested_.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
        running_.store(false, std::memory_order_release);
    }

    [[nodiscard]] bool in_range(dsp::SampleIndex index) const
    {
        return index >= run_.segments.front().first && index <= run_.end_index;
    }

    // The segment that holds `index`, or the first one after it when the
    // index falls in a gap. Never past the end, because callers check the
    // range first.
    [[nodiscard]] std::size_t segment_for(dsp::SampleIndex index) const
    {
        for (std::size_t i = 0; i < run_.segments.size(); ++i) {
            if (index < run_.segments[i].end) {
                return i;
            }
        }
        return run_.segments.size() - 1;
    }

    [[nodiscard]] bool sleep_for_pace(std::chrono::steady_clock::time_point target,
                                      double pace_in_force)
    {
        using Clock = std::chrono::steady_clock;
        constexpr auto kSlice = std::chrono::milliseconds(20);
        for (auto now = Clock::now(); now < target; now = Clock::now()) {
            if (stop_requested_.load(std::memory_order_acquire)) {
                return false;
            }
            if (pace_.load(std::memory_order_acquire) != pace_in_force) {
                return true;
            }
            std::this_thread::sleep_until(std::min(target, now + kSlice));
        }
        return false;
    }

    void fail_stream(std::string message)
    {
        stop_error_ = Error{std::move(message)};
        has_stop_error_ = true;
    }

    const Run run_;
    const std::size_t bytes_per_sample_;

    mutable std::mutex control_{};
    std::thread thread_{};
    source::BlockSink sink_{};
    std::vector<std::byte> buffer_{};
    std::size_t block_samples_ = 0;

    std::optional<double> own_pace_{};
    std::atomic<double> pace_{0.0};

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<dsp::SampleIndex> pending_seek_{kNoPendingSeek};

    // Owned by the source thread while it runs and by the control plane when
    // it does not; the join is the handoff in both directions.
    dsp::SampleIndex position_ = 0;
    Error stop_error_{};
    bool has_stop_error_ = false;

    std::atomic<std::uint64_t> blocks_delivered_{0};
    std::atomic<std::uint64_t> samples_delivered_{0};
    std::atomic<dsp::SampleIndex> write_index_{0};
};

Status CaptureSource::seek(dsp::SampleIndex index)
{
    std::scoped_lock lock(control_);
    if (!in_range(index)) {
        return fail(std::format("cannot seek to sample {}: this replay of stream {:016x} holds "
                                "indices {} to {}",
                                index, run_.stream_epoch, run_.segments.front().first,
                                run_.end_index));
    }
    if (running_.load(std::memory_order_acquire)) {
        pending_seek_.store(index, std::memory_order_release);
        return {};
    }
    position_ = index;
    write_index_.store(index, std::memory_order_relaxed);
    return {};
}

Status CaptureSource::start(const source::StreamOptions& options, source::BlockSink sink)
{
    std::scoped_lock lock(control_);

    if (running_.load(std::memory_order_acquire) || thread_.joinable()) {
        return fail("this capture is already streaming");
    }
    if (!sink) {
        return fail("a source cannot be started without a sink");
    }
    if (!std::isfinite(options.pace) || options.pace < 0.0) {
        return fail(std::format("pace must be zero for unthrottled or a positive multiple of "
                                "realtime, got {}",
                                options.pace));
    }

    // Zero is every caller that did not ask, and takes from= or a seek made
    // before start. Anything else is a stream index, checked like a seek.
    if (options.start_index != 0) {
        if (!in_range(options.start_index)) {
            return fail(std::format("cannot start at sample {}: this replay holds indices {} to "
                                    "{}",
                                    options.start_index, run_.segments.front().first,
                                    run_.end_index));
        }
        position_ = options.start_index;
    }

    std::size_t block = options.block_samples == 0 ? kDefaultBlockSamples : options.block_samples;
    if (block > kMaxBlockSamples) {
        return fail(std::format("a block of {} samples is past the {} sample ceiling; that is a "
                                "buffer, not a block",
                                block, kMaxBlockSamples));
    }

    buffer_.assign(block * bytes_per_sample_, std::byte{0});
    block_samples_ = block;
    pace_.store(own_pace_.value_or(options.pace), std::memory_order_release);
    sink_ = std::move(sink);
    pending_seek_.store(kNoPendingSeek, std::memory_order_relaxed);
    stop_error_ = Error{};
    has_stop_error_ = false;
    blocks_delivered_.store(0, std::memory_order_relaxed);
    samples_delivered_.store(0, std::memory_order_relaxed);
    write_index_.store(position_, std::memory_order_relaxed);
    stop_requested_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);

    try {
        thread_ = std::thread([this] { run(); });
    } catch (const std::system_error& error) {
        running_.store(false, std::memory_order_release);
        return fail(std::format("could not start the capture replay thread: {}", error.what()),
                    error.code().value());
    }
    return {};
}

void CaptureSource::run()
{
    describe_this_thread(L"revenant source capture", ThreadClass::Listening);
    using Clock = std::chrono::steady_clock;

    std::uint64_t sequence = 0;
    bool seeked = true;  // forces the initial positioning
    std::size_t current = 0;
    FilePtr file;
    std::uint64_t dropped = 0;

    Clock::time_point pace_origin{};
    dsp::SampleIndex pace_origin_index = 0;
    double pace = pace_.load(std::memory_order_acquire);

    // Opens segment `index` with its file positioned at `position_`, and
    // restarts the pacing stopwatch: after a seek for the reason the file
    // source gives, and after a gap because timing the next block from
    // before it would sleep through every second the writer lost.
    const auto enter = [&](std::size_t index) -> bool {
        const RunSegment& segment = run_.segments[index];
        if (index != current || !file) {
            auto opened = open_binary(segment.data_path);
            if (!opened) {
                stop_error_ = opened.error();
                has_stop_error_ = true;
                return false;
            }
            file = std::move(*opened);
            current = index;
        }
        const auto offset = static_cast<std::int64_t>((position_ - segment.first) *
                                                      bytes_per_sample_);
        if (!seek_absolute(file.get(), offset)) {
            fail_stream(std::format("could not seek '{}' to sample {}", segment.data_path,
                                    position_));
            return false;
        }
        pace_origin = Clock::now();
        pace_origin_index = position_;
        return true;
    };

    while (!stop_requested_.load(std::memory_order_acquire)) {
        const dsp::SampleIndex requested =
            pending_seek_.exchange(kNoPendingSeek, std::memory_order_acq_rel);
        if (requested != kNoPendingSeek) {
            position_ = requested;
            seeked = true;
            dropped = 0;
        }
        if (position_ >= run_.end_index) {
            break;  // end of the run, which is not an error
        }

        if (seeked) {
            const std::size_t index = segment_for(position_);
            // A seek into a gap lands on the next sample that exists, and
            // as a seek it reports no loss: the stamp already shows the jump.
            position_ = std::max(position_, run_.segments[index].first);
            if (!enter(index)) {
                break;
            }
            seeked = false;
        } else if (position_ >= run_.segments[current].end) {
            const std::size_t next = current + 1;
            if (next >= run_.segments.size()) {
                break;
            }
            dropped = run_.segments[next].first - position_;
            position_ = run_.segments[next].first;
            if (!enter(next)) {
                break;
            }
        }
        if (position_ >= run_.end_index) {
            break;
        }

        const RunSegment& segment = run_.segments[current];
        std::size_t want = block_samples_;
        want = static_cast<std::size_t>(
            std::min<dsp::SampleIndex>(want, segment.end - position_));
        want = static_cast<std::size_t>(
            std::min<dsp::SampleIndex>(want, run_.end_index - position_));

        const std::size_t want_bytes = want * bytes_per_sample_;
        const std::size_t got_bytes = std::fread(buffer_.data(), 1, want_bytes, file.get());
        if (got_bytes != want_bytes) {
            fail_stream(std::format("segment '{}' returned {} of the {} bytes wanted at sample "
                                    "{}: it changed size after the catalog read it",
                                    segment.data_path, got_bytes, want_bytes, position_));
            break;
        }

        source::SourceBlock block;
        block.stamp = dsp::BlockTimestamp{position_, run_.anchor_ns, run_.rate};
        block.format = run_.format;
        block.sample_count = want;
        block.bytes = std::span<const std::byte>(buffer_.data(), want_bytes);
        block.dropped_before = dropped;
        block.sequence = sequence;

        for (bool changed = true; changed;) {
            changed = false;
            if (const double now_pace = pace_.load(std::memory_order_acquire); now_pace != pace) {
                pace = now_pace;
                pace_origin = Clock::now();
                pace_origin_index = position_;
            }
            if (pace > 0.0) {
                const dsp::SampleIndex since = position_ - pace_origin_index;
                const double nominal_ns =
                    static_cast<double>(duration_ns_of(since, run_.rate)) / pace;
                const auto target =
                    pace_origin + std::chrono::nanoseconds(static_cast<std::int64_t>(nominal_ns));
                changed = sleep_for_pace(target, pace);
            }
        }
        if (stop_requested_.load(std::memory_order_acquire)) {
            break;
        }

        if (Status delivered = sink_(block); !delivered) {
            stop_error_ = delivered.error();
            has_stop_error_ = true;
            break;
        }

        dropped = 0;
        position_ += want;
        ++sequence;
        blocks_delivered_.fetch_add(1, std::memory_order_relaxed);
        samples_delivered_.fetch_add(want, std::memory_order_relaxed);
        write_index_.store(position_, std::memory_order_relaxed);
    }

    running_.store(false, std::memory_order_release);
}

}  // namespace

Expected<source::SourceCapabilities> describe_capture_source(const CaptureSourceConfig& config)
{
    auto run = resolve_run(config);
    if (!run) {
        return std::unexpected(run.error());
    }
    return std::move(run->caps);
}

Expected<std::unique_ptr<source::Source>> open_capture_source(const CaptureSourceConfig& config)
{
    if (config.pace_given && (!std::isfinite(config.pace) || config.pace < 0.0)) {
        return fail(std::format("pace={} is not a pace", config.pace));
    }
    auto run = resolve_run(config);
    if (!run) {
        return std::unexpected(run.error());
    }
    std::optional<double> pace;
    if (config.pace_given) {
        pace = config.pace;
    }
    return std::unique_ptr<source::Source>(new CaptureSource(std::move(*run), pace));
}

}  // namespace revenant::capture
