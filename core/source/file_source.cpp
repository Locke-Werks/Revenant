// The file backend.
//
// Two things here are worth reading before the code. The first is the delivery
// loop, which is four lines and is the whole flow-control story: fill a block,
// call the sink, stop if it complained. The sink is allowed to take as long as
// it likes and that is the backpressure. Nothing measures elapsed time unless
// StreamOptions::pace asks for it.
//
// The second is the anchor. A raw IQ file carries no header, so the moment
// sample zero was captured is either stated by the caller or guessed from the
// filesystem, and the difference between those two has to be visible
// downstream. An anchor with no stated accuracy is a lie, so the guess travels
// with an accuracy wide enough to actually contain the truth.

#include "core/source/file_source.h"

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
#include <span>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "core/source/clock_model.h"

namespace revenant::source {
namespace {

// std::FILE rather than std::ifstream, and the difference is not small.
//
// Measured on this machine reading a 256 MiB file already in the page cache,
// 2 MiB at a time, three runs each:
//
//   std::ifstream::read   1.37 GB/s
//   std::fread            8.02 GB/s
//   ReadFile              8.17 GB/s
//
// MSVC's basic_filebuf puts a copy through its own buffer even for reads far
// larger than that buffer, and at 20 MS/s cf32 the difference is 8.6x
// realtime against 50x. The file source is supposed to be so far from the
// bottleneck that the GPU chain is the limit; through iostreams it is the
// limit. This is also worth knowing when reading the design's "~1.4 GB/s on
// this box" figure, which was measured through siggen's ofstream writer and
// is the same ceiling rather than the disk's.
struct FileCloser {
    void operator()(std::FILE* file) const noexcept
    {
        if (file != nullptr) {
            static_cast<void>(std::fclose(file));
        }
    }
};

using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

// Percent-decoded octets in a URI are UTF-8. filesystem::path's narrow
// constructor reads a std::string in the active code page instead, so a
// capture under a name with an accent in it would be looked for at a path
// nobody ever wrote. The char8_t overload says UTF-8 explicitly, and on
// Windows converts to the wide form the filesystem actually uses.
[[nodiscard]] std::filesystem::path path_of(const std::string& utf8)
{
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

// Everything after the last separator. Taken off the text rather than through
// filesystem::path::filename().string(), which converts back out of the
// native encoding and can throw on a name it cannot represent.
[[nodiscard]] std::string basename_of(const std::string& path)
{
    const std::size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

[[nodiscard]] Expected<FilePtr> open_binary(const std::string& path)
{
    std::FILE* raw = nullptr;
#if defined(_MSC_VER)
    // The wide form, so a path with a character outside the active code page
    // opens rather than failing with a message about a file that plainly
    // exists.
    const errno_t error = _wfopen_s(&raw, path_of(path).c_str(), L"rb");
    if (error != 0 || raw == nullptr) {
        return fail(std::format("could not open '{}' for reading", path), error);
    }
#else
    raw = std::fopen(path.c_str(), "rb");
    if (raw == nullptr) {
        return fail(std::format("could not open '{}' for reading", path));
    }
#endif
    return FilePtr(raw);
}

// 64-bit positioning. std::fseek takes a long, which is 32 bits on Windows
// and would cap a capture at 2 GiB.
[[nodiscard]] bool seek_absolute(std::FILE* file, std::int64_t offset)
{
#if defined(_MSC_VER)
    return _fseeki64(file, offset, SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

// 128 MiB of cf32. Past this a caller has confused a block with a buffer, and
// the allocation is large enough that failing loudly beats discovering it as a
// bad_alloc on a smaller machine.
constexpr std::size_t kMaxBlockSamples = 1u << 24;

// A quarter of a second at 20 MS/s, which is large enough that the per-block
// overhead disappears against the read and small enough that a blocking
// consumer's latency is still a fraction of a second.
constexpr std::size_t kDefaultBlockSamples = 1u << 18;

// Sentinel in the pending-seek slot. A seek to this index is not expressible
// in any file that fits on any filesystem, so it costs nothing to reserve.
constexpr dsp::SampleIndex kNoPendingSeek = std::numeric_limits<dsp::SampleIndex>::max();

[[nodiscard]] std::int64_t duration_ns_of(dsp::SampleIndex samples, dsp::SampleRate rate)
{
    if (rate <= 0) {
        return 0;
    }
    // Split, for the same overflow reason BlockTimestamp::wall_clock_ns
    // splits: samples * 1e9 leaves 64 bits fifteen minutes into a 20 MS/s
    // capture.
    const auto rate_u = static_cast<dsp::SampleIndex>(rate);
    const auto whole_seconds = static_cast<std::int64_t>(samples / rate_u);
    const auto remainder = static_cast<std::int64_t>(samples % rate_u);
    return whole_seconds * 1'000'000'000 + (remainder * 1'000'000'000) / rate;
}

struct DerivedAnchor {
    std::int64_t anchor_ns = 0;
    std::int64_t accuracy_ns = 0;
};

// The anchor when the caller did not supply one.
//
// The filesystem records when the file was last written, which for a file
// produced by a streaming writer is when the capture ENDED. So the estimate
// for sample zero is that time less the file's own duration. Every part of
// that is a guess: the file may have been copied, in which case the timestamp
// is when the copy ran; it may have been written in one burst from a buffer,
// in which case the duration subtraction is wrong by the whole duration; and
// a FAT volume stores modification times to two seconds.
//
// So the accuracy is the file's duration plus two seconds, which is wide
// enough to contain every one of those cases except an outright copy. It is
// deliberately not a small number. A confident anchor that is wrong by an hour
// is worse than a vague one that says so, because only the vague one stops a
// slotted-mode decoder from trusting it.
[[nodiscard]] Expected<DerivedAnchor> anchor_from_modification_time(const std::string& path,
                                                                    dsp::SampleIndex samples,
                                                                    dsp::SampleRate rate)
{
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(path_of(path), ec);
    if (ec) {
        return fail(std::format("could not read the modification time of '{}': {}", path,
                                ec.message()),
                    ec.value());
    }

    const auto as_system = std::chrono::clock_cast<std::chrono::system_clock>(written);
    const auto since_epoch =
        std::chrono::duration_cast<std::chrono::nanoseconds>(as_system.time_since_epoch());

    const std::int64_t duration_ns = duration_ns_of(samples, rate);

    DerivedAnchor out;
    out.anchor_ns = since_epoch.count() - duration_ns;
    out.accuracy_ns = duration_ns + 2'000'000'000;
    return out;
}

// The capability description and the anchor come out of one pass over the
// filesystem, so describe_sources() and open_source() cannot disagree about
// how long the file is or when it started.
struct ResolvedFile {
    SourceCapabilities caps;
    std::int64_t anchor_ns = 0;
    dsp::SampleIndex length_samples = 0;
};

[[nodiscard]] Expected<ResolvedFile> resolve_file(const FileSourceConfig& config);

class FileSource final : public Source {
public:
    FileSource() = default;
    ~FileSource() override;

    [[nodiscard]] Status open(const FileSourceConfig& config, ResolvedFile resolved);

    [[nodiscard]] const SourceCapabilities& capabilities() const override { return caps_; }

    [[nodiscard]] Expected<dsp::Hertz> tune(dsp::Hertz center) override;
    [[nodiscard]] dsp::Hertz center() const override { return center_hz_; }

    [[nodiscard]] Expected<dsp::SampleRate> set_sample_rate(dsp::SampleRate rate) override;
    [[nodiscard]] dsp::SampleRate sample_rate() const override { return rate_; }

    [[nodiscard]] Expected<double> set_gain(std::string_view stage, double db) override;
    [[nodiscard]] Status set_gain_auto(std::string_view stage, bool on) override;

    [[nodiscard]] Status start(const StreamOptions& options, BlockSink sink) override;
    [[nodiscard]] Status stop() override;
    [[nodiscard]] bool running() const override { return running_.load(std::memory_order_acquire); }

    [[nodiscard]] Status seek(dsp::SampleIndex index) override;

    [[nodiscard]] SourceStats stats() const override;
    [[nodiscard]] ClockQuality clock() const override;

private:
    void run();
    void join_locked();

    SourceCapabilities caps_{};
    std::string path_{};
    dsp::SampleRate rate_ = 0;
    dsp::Hertz center_hz_ = 0;
    SampleFormat format_ = SampleFormat::Cf32;
    std::size_t bytes_per_sample_ = 0;
    dsp::SampleIndex length_samples_ = 0;
    std::int64_t anchor_ns_ = 0;

    // Written once at open, read-only afterwards, so clock() needs no
    // synchronisation with the source thread. The one value that does move,
    // how far the stream has run, is read from write_index_ below instead of
    // being pushed into the model from the delivery loop.
    ClockModel clock_model_{};

    // Control plane only: start, stop, seek and the setters. The delivery loop
    // never takes it, which is what keeps the sample path free of a mutex.
    mutable std::mutex control_{};
    std::thread thread_{};
    BlockSink sink_{};
    FilePtr file_{};
    std::vector<std::byte> buffer_{};
    std::size_t block_samples_ = 0;
    double pace_ = 0.0;

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

FileSource::~FileSource()
{
    std::scoped_lock lock(control_);
    join_locked();
}

void FileSource::join_locked()
{
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

Status FileSource::open(const FileSourceConfig& config, ResolvedFile resolved)
{
    caps_ = std::move(resolved.caps);
    path_ = config.path;
    rate_ = config.rate;
    center_hz_ = config.center_hz;
    format_ = config.format;
    bytes_per_sample_ = bytes_per_sample(config.format);
    length_samples_ = resolved.length_samples;
    anchor_ns_ = resolved.anchor_ns;

    ClockModelConfig clock_config;
    clock_config.source = ClockSource::Internal;
    clock_config.rate = rate_;
    clock_config.anchor_accuracy_ns = caps_.timestamp_accuracy_ns;
    clock_config.oscillator_tolerance_ppm = config.ppm_uncertainty;

    auto model = ClockModel::create(clock_config);
    if (!model) {
        return std::unexpected(with_context(model.error(), "file source clock"));
    }
    clock_model_ = std::move(*model);
    if (auto anchored = clock_model_.set_anchor(anchor_ns_); !anchored) {
        return std::unexpected(with_context(anchored.error(), "file source clock"));
    }

    auto opened = open_binary(path_);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    file_ = std::move(*opened);
    return {};
}

Expected<dsp::Hertz> FileSource::tune(dsp::Hertz center)
{
    return fail(std::format(
        "the file source cannot tune: a recording's centre frequency is a property of the "
        "samples already on disk, not a setting. It declares no tune range, so "
        "SourceCapabilities::can_tune({}) is false. Reopen the URI with a different "
        "center= if the recorded value was wrong.",
        center));
}

Expected<dsp::SampleRate> FileSource::set_sample_rate(dsp::SampleRate rate)
{
    if (rate == rate_) {
        return rate_;
    }
    return fail(std::format(
        "the file source is fixed at {} S/s, which is what its URI declared the recording "
        "to be; {} S/s would reinterpret the same bytes as a different signal. Reopen with "
        "rate= if the declared value was wrong.",
        rate_, rate));
}

Expected<double> FileSource::set_gain(std::string_view stage, double)
{
    return fail(std::format(
        "the file source has no gain stage '{}': it declares none at all, because the gain "
        "that produced these samples was applied before they were written.",
        stage));
}

Status FileSource::set_gain_auto(std::string_view stage, bool)
{
    return fail(std::format("the file source has no gain stage '{}' to put into automatic mode",
                            stage));
}

Status FileSource::seek(dsp::SampleIndex index)
{
    std::scoped_lock lock(control_);

    if (length_samples_ > 0 && index > length_samples_) {
        return fail(std::format("cannot seek to sample {}: '{}' holds {} samples", index, path_,
                                length_samples_));
    }

    if (running_.load(std::memory_order_acquire)) {
        // Taken up by the delivery loop before it fills the next block, so a
        // seek never tears a block in half. The block that lands after it
        // carries the new index in its stamp, which is how a consumer sees
        // exactly where the seek went.
        pending_seek_.store(index, std::memory_order_release);
        return {};
    }

    position_ = index;
    write_index_.store(index, std::memory_order_relaxed);
    return {};
}

Status FileSource::start(const StreamOptions& options, BlockSink sink)
{
    std::scoped_lock lock(control_);

    if (running_.load(std::memory_order_acquire) || thread_.joinable()) {
        return fail(std::format("'{}' is already streaming", path_));
    }
    if (!sink) {
        return fail("a source cannot be started without a sink");
    }
    if (!std::isfinite(options.pace) || options.pace < 0.0) {
        return fail(std::format("pace must be zero for unthrottled or a positive multiple of "
                                "realtime, got {}",
                                options.pace));
    }
    if (length_samples_ > 0 && options.start_index > length_samples_) {
        return fail(std::format("cannot start at sample {}: '{}' holds {} samples",
                                options.start_index, path_, length_samples_));
    }

    std::size_t block = options.block_samples;
    if (block == 0) {
        block = caps_.preferred_block_samples;
    }
    if (block == 0) {
        block = kDefaultBlockSamples;
    }
    if (block > kMaxBlockSamples) {
        return fail(std::format("a block of {} samples is past the {} sample ceiling; that is a "
                                "buffer, not a block",
                                block, kMaxBlockSamples));
    }

    buffer_.assign(block * bytes_per_sample_, std::byte{0});
    block_samples_ = block;
    pace_ = options.pace;
    sink_ = std::move(sink);

    position_ = options.start_index;
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
        return fail(std::format("could not start the source thread for '{}': {}", path_,
                                error.what()),
                    error.code().value());
    }
    return {};
}

Status FileSource::stop()
{
    std::scoped_lock lock(control_);
    join_locked();
    if (has_stop_error_) {
        return std::unexpected(stop_error_);
    }
    return {};
}

SourceStats FileSource::stats() const
{
    SourceStats out;
    out.blocks_delivered = blocks_delivered_.load(std::memory_order_relaxed);
    out.samples_delivered = samples_delivered_.load(std::memory_order_relaxed);

    // A Demand source cannot overrun. There is nowhere for a sample to be lost:
    // the sink either takes the block or stops the stream. These three stay
    // zero for the life of the source and the acceptance test asserts it.
    out.overrun_events = 0;
    out.samples_lost = 0;
    out.last_loss_index = 0;

    out.write_index = write_index_.load(std::memory_order_relaxed);
    return out;
}

ClockQuality FileSource::clock() const
{
    // The model is immutable after open, so this reads it without a lock. The
    // only moving part is how far the stream has run, which the model would
    // otherwise have to be told from the delivery loop; taking it from the
    // published write index instead keeps the loop out of the model entirely.
    ClockQuality quality = clock_model_.quality();
    quality.accuracy_ns =
        clock_model_.accuracy_ns_at(write_index_.load(std::memory_order_relaxed));
    return quality;
}

void FileSource::run()
{
    using Clock = std::chrono::steady_clock;

    std::uint64_t sequence = 0;
    bool seeked = true;  // forces the initial positioning of the stream

    // Wall clock at the edge, and only when pace was asked for. An unthrottled
    // Demand source never reads a clock, which is the reason it can outrun
    // realtime without anything in the code saying so.
    Clock::time_point pace_origin{};
    dsp::SampleIndex pace_origin_index = 0;

    while (!stop_requested_.load(std::memory_order_acquire)) {
        const dsp::SampleIndex requested = pending_seek_.exchange(kNoPendingSeek,
                                                                  std::memory_order_acq_rel);
        if (requested != kNoPendingSeek) {
            position_ = requested;
            seeked = true;
        }

        if (seeked) {
            const auto offset = static_cast<std::int64_t>(
                position_ * static_cast<dsp::SampleIndex>(bytes_per_sample_));
            if (!seek_absolute(file_.get(), offset)) {
                stop_error_ = Error{std::format("could not seek '{}' to sample {}", path_,
                                                position_)};
                has_stop_error_ = true;
                break;
            }
            seeked = false;

            // A seek resets the pacing origin so the throttle does not try to
            // catch up to where an unseeked stream would have been by now.
            pace_origin = Clock::now();
            pace_origin_index = position_;
        }

        if (length_samples_ > 0 && position_ >= length_samples_) {
            break;  // end of the recording, which is not an error
        }

        std::size_t want = block_samples_;
        if (length_samples_ > 0) {
            const dsp::SampleIndex remaining = length_samples_ - position_;
            if (remaining < static_cast<dsp::SampleIndex>(want)) {
                want = static_cast<std::size_t>(remaining);
            }
        }

        const std::size_t want_bytes = want * bytes_per_sample_;
        const std::size_t got_bytes = std::fread(buffer_.data(), 1, want_bytes, file_.get());

        if (got_bytes != want_bytes) {
            // The size was read at open, so a short read means the file
            // shrank underneath us. Reporting it beats delivering a truncated
            // block that looks like a clean end of stream.
            stop_error_ = Error{std::format(
                "'{}' returned {} of the {} bytes wanted at sample {}: the file changed size "
                "after it was opened",
                path_, got_bytes, want_bytes, position_)};
            has_stop_error_ = true;
            break;
        }

        SourceBlock block;
        block.stamp = dsp::BlockTimestamp{position_, anchor_ns_, rate_};
        block.format = format_;
        block.sample_count = want;
        block.bytes = std::span<const std::byte>(buffer_.data(), want_bytes);
        block.dropped_before = 0;
        block.sequence = sequence;

        if (pace_ > 0.0) {
            const dsp::SampleIndex since = position_ - pace_origin_index;
            const double nominal_ns =
                static_cast<double>(duration_ns_of(since, rate_)) / pace_;
            const auto target =
                pace_origin + std::chrono::nanoseconds(static_cast<std::int64_t>(nominal_ns));
            if (target > Clock::now()) {
                std::this_thread::sleep_until(target);
            }
        }

        // Allowed to block for as long as it likes. That is the backpressure,
        // and it is the entire mechanism by which this source runs at exactly
        // the rate its consumer retires work.
        if (Status delivered = sink_(block); !delivered) {
            stop_error_ = delivered.error();
            has_stop_error_ = true;
            break;
        }

        position_ += want;
        ++sequence;
        blocks_delivered_.fetch_add(1, std::memory_order_relaxed);
        samples_delivered_.fetch_add(want, std::memory_order_relaxed);
        write_index_.store(position_, std::memory_order_relaxed);
    }

    running_.store(false, std::memory_order_release);
}

}  // namespace

Expected<SampleFormat> sample_format_from_name(std::string_view name)
{
    if (name == "cu8") {
        return SampleFormat::Cu8;
    }
    if (name == "cs8") {
        return SampleFormat::Cs8;
    }
    if (name == "cs16") {
        return SampleFormat::Cs16;
    }
    if (name == "cf32") {
        return SampleFormat::Cf32;
    }
    return fail(std::format("unknown sample format '{}', expected cu8, cs8, cs16 or cf32", name));
}

Expected<SampleFormat> sample_format_from_extension(std::string_view path)
{
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 >= path.size()) {
        return fail(std::format(
            "'{}' has no extension to infer a sample format from, so the URI has to say "
            "format=cu8, cs8, cs16 or cf32",
            path));
    }

    const std::string_view extension = path.substr(dot + 1);
    auto known = sample_format_from_name(extension);
    if (known) {
        return known;
    }
    return fail(std::format(
        "'{}' does not name a sample format, so the URI has to say format=cu8, cs8, cs16 or "
        "cf32 rather than leaving it to be guessed from the extension",
        extension));
}

namespace {

Expected<ResolvedFile> resolve_file(const FileSourceConfig& config)
{
    if (config.path.empty()) {
        return fail("a file source needs a path");
    }
    if (config.rate <= 0) {
        return fail(std::format(
            "'{}' needs rate= in its URI: a raw IQ file carries no header, so nothing in the "
            "bytes says what rate they were taken at",
            config.path));
    }
    if (!std::isfinite(config.ppm_uncertainty) || config.ppm_uncertainty < 0.0) {
        return fail("ppm_uncertainty must be a finite number of parts per million, zero or above");
    }
    if (config.anchor_accuracy_given && config.anchor_accuracy_ns < 0) {
        return fail("anchor_accuracy_ns must not be negative");
    }

    const std::filesystem::path path = path_of(config.path);
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec) {
        return fail(std::format("could not stat '{}': {}", config.path, ec.message()), ec.value());
    }
    if (!std::filesystem::is_regular_file(status)) {
        return fail(std::format("'{}' is not a regular file", config.path));
    }

    const auto size_bytes = std::filesystem::file_size(path, ec);
    if (ec) {
        return fail(std::format("could not read the size of '{}': {}", config.path, ec.message()),
                    ec.value());
    }

    const std::size_t bps = bytes_per_sample(config.format);
    if (bps == 0) {
        return fail("unsupported sample format");
    }

    const auto length_samples = static_cast<dsp::SampleIndex>(size_bytes / bps);
    if (length_samples == 0) {
        return fail(std::format("'{}' holds {} bytes, which is less than one {} sample",
                                config.path, size_bytes, format_name(config.format)));
    }
    if (size_bytes % bps != 0) {
        // A trailing partial sample means either the wrong format was declared
        // or the writer was interrupted. Both are worth stopping for: the
        // second is recoverable by truncating the file, and the first produces
        // a capture whose every sample is wrong.
        return fail(std::format(
            "'{}' holds {} bytes, which is not a whole number of {} samples ({} bytes each). "
            "Either the declared format is wrong or the file is truncated.",
            config.path, size_bytes, format_name(config.format), bps));
    }

    std::int64_t accuracy_ns = 0;
    std::int64_t anchor_ns = config.anchor_ns;
    if (!config.anchor_given) {
        auto derived = anchor_from_modification_time(config.path, length_samples, config.rate);
        if (!derived) {
            return std::unexpected(derived.error());
        }
        anchor_ns = derived->anchor_ns;
        accuracy_ns = derived->accuracy_ns;
    }

    if (config.anchor_accuracy_given) {
        accuracy_ns = config.anchor_accuracy_ns;
    } else if (config.anchor_given) {
        // The caller stated the anchor and said nothing about how good it is.
        // One sample period is the floor: a stated anchor still cannot place
        // sample zero finer than the grid the samples sit on.
        accuracy_ns = std::max<std::int64_t>(1, 1'000'000'000 / config.rate);
    }

    SourceCapabilities caps;
    caps.uri = config.uri;
    caps.backend = "file";
    caps.display_name =
        config.display_name.empty() ? basename_of(config.path) : config.display_name;

    // No tune ranges at all, deliberately. A recording's centre frequency is a
    // fact about bytes already on disk, so can_tune() answers false for every
    // frequency including the one the file was recorded at, and tune() fails
    // with a message naming the capability rather than the symptom.
    caps.tune_ranges.clear();

    caps.sample_rates = {config.rate};
    caps.min_rate = config.rate;
    caps.max_rate = config.rate;

    caps.native_format = config.format;
    caps.bits_per_component = static_cast<std::uint8_t>((bps / 2) * 8);

    caps.flow = FlowControl::Demand;
    caps.seekable = true;
    caps.length_samples = length_samples;
    caps.preferred_block_samples = kDefaultBlockSamples;
    caps.timestamp_accuracy_ns = accuracy_ns;
    caps.clock_sources = {ClockSource::Internal};

    ResolvedFile out;
    out.caps = std::move(caps);
    out.anchor_ns = anchor_ns;
    out.length_samples = length_samples;
    return out;
}

}  // namespace

Expected<SourceCapabilities> describe_file_source(const FileSourceConfig& config)
{
    auto resolved = resolve_file(config);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    return std::move(resolved->caps);
}

Expected<std::unique_ptr<Source>> open_file_source(const FileSourceConfig& config)
{
    auto resolved = resolve_file(config);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }

    auto source = std::make_unique<FileSource>();
    if (auto opened = source->open(config, std::move(*resolved)); !opened) {
        return std::unexpected(opened.error());
    }
    return std::unique_ptr<Source>(std::move(source));
}

}  // namespace revenant::source
