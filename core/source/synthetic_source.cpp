// The synthetic backend.
//
// The delivery loop is the file source's loop with the read replaced by a
// render, and it is deliberately the same shape: fill a block, call the sink,
// stop if the sink complained. Nothing here measures time.
//
// BUILD DEPENDENCY, which needs a decision from whoever wires the CMake.
// core/source now depends on tools/siggen, and core/dsp/synth/CMakeLists.txt
// already links revenant_siggen against revenant_core. Adding this file to
// revenant_core closes that into a cycle. Two ways out, and the choice is not
// mine to make:
//
//   1. Move the scene generator into core. siggen's three translation units
//      include nothing from core but headers (reference_fp.h, types.h,
//      error.h), so revenant_siggen does not actually need to link
//      revenant_core at all and the PUBLIC link there could become an
//      include-directory dependency. Then revenant_core links revenant_siggen
//      and the cycle is gone.
//   2. Put this file in its own target that links both, leaving revenant_core
//      free of siggen. The registry then has to reach it through a
//      registration seam rather than a direct call.
//
// Option 1 is smaller and matches the design's file layout, which puts
// synthetic_source under core/source. It is written here on that assumption.

#include "core/source/synthetic_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "core/dsp/denormal_mode.h"
#include "core/source/clock_model.h"
#include "core/dsp/synth/modulators.h"
#include "core/dsp/synth/wideband.h"

namespace revenant::source {
namespace {

constexpr std::size_t kMaxBlockSamples = 1u << 24;
constexpr std::size_t kDefaultBlockSamples = 1u << 18;
constexpr dsp::SampleIndex kNoPendingSeek = std::numeric_limits<dsp::SampleIndex>::max();

// The only scene kind the backend knows. Named so that a URI asking for
// something else gets told what does exist rather than a parse error.
constexpr std::string_view kSceneKind = "wideband";

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

[[nodiscard]] Status validate(const SyntheticSourceConfig& config)
{
    if (config.rate <= 0 || config.rate > siggen::kMaxSampleRate) {
        return fail(std::format("synthetic rate {} is outside 1 to {} S/s", config.rate,
                                siggen::kMaxSampleRate));
    }
    if (!std::isfinite(config.noise_power_full_band_dbfs)) {
        return fail("noise_dbfs must be finite");
    }
    if (!std::isfinite(config.snr_min_db) || !std::isfinite(config.snr_max_db)) {
        return fail("snr_min and snr_max must be finite");
    }
    if (config.snr_min_db > config.snr_max_db) {
        return fail(std::format("snr_min {} is above snr_max {}", config.snr_min_db,
                                config.snr_max_db));
    }
    if (!std::isfinite(config.min_burst_seconds) || !std::isfinite(config.max_burst_seconds) ||
        config.min_burst_seconds <= 0.0 || config.max_burst_seconds <= 0.0) {
        return fail("min_burst and max_burst must be positive numbers of seconds");
    }
    if (config.min_burst_seconds > config.max_burst_seconds) {
        return fail(std::format("min_burst {} s is above max_burst {} s", config.min_burst_seconds,
                                config.max_burst_seconds));
    }
    if (config.bursts_per_emitter == 0) {
        return fail("bursts must be at least one");
    }
    if (!config.add_noise && config.emitters > 0) {
        // Scene::create refuses this too, several layers down and with a
        // message about a placement rule the caller never mentioned. The
        // random population places every emitter by signal-to-noise ratio,
        // and a ratio against no noise is not a level. Saying so here means
        // a device list finds out without building a scene, and the message
        // names the two query parameters actually involved.
        return fail("noise=off leaves the random emitters with nothing to be placed against: "
                    "their levels are set by snr_min and snr_max, which are ratios against the "
                    "noise floor. Use emitters=0 for a silent scene, or leave the noise on and "
                    "push it down with noise_dbfs.");
    }
    if (config.bursts_per_emitter > 1 && config.duration_samples == 0) {
        // Scene::create says the same thing, but saying it here means a device
        // list does not have to build a scene to find out.
        return fail("bursts above one need a bounded scene, so give duration= as well");
    }
    if (config.span_given && config.span_low_hz >= config.span_high_hz) {
        return fail(std::format("span_low {} Hz must be below span_high {} Hz", config.span_low_hz,
                                config.span_high_hz));
    }
    for (const std::string& mode : config.modes) {
        if (auto known = siggen::modulation_from_name(mode); !known) {
            return std::unexpected(known.error());
        }
    }
    return {};
}

[[nodiscard]] SourceCapabilities capabilities_of(const SyntheticSourceConfig& config)
{
    SourceCapabilities caps;
    caps.uri = config.uri;
    caps.backend = "synthetic";
    caps.display_name = config.display_name.empty()
                            ? std::format("Synthetic {} scene, seed {}", kSceneKind, config.seed)
                            : config.display_name;

    // No tune range, for the same reason the file source has none. The
    // scene's centre frequency is a label on a baseband that was generated
    // around DC; moving it would relabel a stream mid-flight without changing
    // a sample, which is exactly the kind of quiet metadata corruption the
    // integer-hertz rule exists to prevent. A different centre is a different
    // URI.
    caps.tune_ranges.clear();

    caps.sample_rates = {config.rate};
    caps.min_rate = config.rate;
    caps.max_rate = config.rate;

    caps.native_format = SampleFormat::Cf32;
    caps.bits_per_component = 32;

    caps.flow = FlowControl::Demand;
    caps.seekable = true;
    caps.length_samples = config.duration_samples;
    caps.preferred_block_samples = kDefaultBlockSamples;
    caps.clock_sources = {ClockSource::Internal};

    // Exact by construction rather than measured. Sample zero of a synthesised
    // stream IS the declared anchor: there is no acquisition delay to estimate
    // and no oscillator to drift, because nothing was acquired and no clock
    // was read. Zero here means "no uncertainty", not "unknown".
    caps.timestamp_accuracy_ns = 0;

    return caps;
}

[[nodiscard]] Expected<siggen::SceneSpec> scene_spec_of(const SyntheticSourceConfig& config)
{
    siggen::SceneSpec spec;
    spec.rate = config.rate;
    spec.center_hz = config.center_hz;
    spec.duration_samples = config.duration_samples;
    spec.epoch_anchor_ns = config.anchor_ns;
    spec.seed = config.seed;
    spec.add_noise = config.add_noise;
    spec.noise_power_full_band_dbfs = config.noise_power_full_band_dbfs;

    // One, and see the header. The engine owns the pool; a Scene that spins up
    // its own would put two pools sized to the same machine against each
    // other, and the output does not depend on the worker count either way.
    spec.worker_threads = 1;

    spec.random.emitter_count = config.emitters;
    spec.random.bursts_per_emitter = config.bursts_per_emitter;
    spec.random.span_low_hz = config.span_given ? config.span_low_hz : -config.rate * 9 / 20;
    spec.random.span_high_hz = config.span_given ? config.span_high_hz : config.rate * 9 / 20;
    spec.random.snr_in_occupied_bandwidth_db_min = config.snr_min_db;
    spec.random.snr_in_occupied_bandwidth_db_max = config.snr_max_db;
    spec.random.min_burst_seconds = config.min_burst_seconds;
    spec.random.max_burst_seconds = config.max_burst_seconds;

    for (const std::string& mode : config.modes) {
        auto kind = siggen::modulation_from_name(mode);
        if (!kind) {
            return std::unexpected(kind.error());
        }
        spec.random.palette.push_back(*kind);
    }

    return spec;
}

class SyntheticSource final : public Source {
public:
    SyntheticSource() = default;
    ~SyntheticSource() override;

    [[nodiscard]] Status open(const SyntheticSourceConfig& config, SourceCapabilities caps,
                              siggen::Scene scene);

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
    std::optional<siggen::Scene> scene_{};
    dsp::SampleRate rate_ = 0;
    dsp::Hertz center_hz_ = 0;
    dsp::SampleIndex length_samples_ = 0;
    std::int64_t anchor_ns_ = 0;
    ClockModel clock_model_{};

    mutable std::mutex control_{};
    std::thread thread_{};
    BlockSink sink_{};
    std::vector<dsp::Complex32> buffer_{};
    std::size_t block_samples_ = 0;
    double pace_ = 0.0;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<dsp::SampleIndex> pending_seek_{kNoPendingSeek};

    dsp::SampleIndex position_ = 0;
    Error stop_error_{};
    bool has_stop_error_ = false;

    std::atomic<std::uint64_t> blocks_delivered_{0};
    std::atomic<std::uint64_t> samples_delivered_{0};
    std::atomic<dsp::SampleIndex> write_index_{0};
};

SyntheticSource::~SyntheticSource()
{
    std::scoped_lock lock(control_);
    join_locked();
}

void SyntheticSource::join_locked()
{
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

Status SyntheticSource::open(const SyntheticSourceConfig& config, SourceCapabilities caps,
                             siggen::Scene scene)
{
    caps_ = std::move(caps);
    scene_.emplace(std::move(scene));
    rate_ = config.rate;
    center_hz_ = config.center_hz;
    length_samples_ = config.duration_samples;
    anchor_ns_ = config.anchor_ns;

    ClockModelConfig clock_config;
    clock_config.source = ClockSource::Internal;
    clock_config.rate = rate_;

    // Both zero, and both honest. There is no acquisition to time and no
    // oscillator to drift: the stream is defined by its index, so the
    // uncertainty in placing an index on the wall clock is exactly the
    // uncertainty the caller declared when it chose the anchor, which is none.
    clock_config.anchor_accuracy_ns = 0;
    clock_config.oscillator_tolerance_ppm = 0.0;

    auto model = ClockModel::create(clock_config);
    if (!model) {
        return std::unexpected(with_context(model.error(), "synthetic source clock"));
    }
    clock_model_ = std::move(*model);
    if (auto anchored = clock_model_.set_anchor(anchor_ns_); !anchored) {
        return std::unexpected(with_context(anchored.error(), "synthetic source clock"));
    }
    return {};
}

Expected<dsp::Hertz> SyntheticSource::tune(dsp::Hertz center)
{
    return fail(std::format(
        "the synthetic source cannot tune: its centre is a label on a scene generated around "
        "baseband DC, so moving it to {} Hz would change what the stream claims to be without "
        "changing a sample. It declares no tune range. Reopen the URI with a different "
        "center=, or place the emitters where you want them with span_low and span_high.",
        center));
}

Expected<dsp::SampleRate> SyntheticSource::set_sample_rate(dsp::SampleRate rate)
{
    if (rate == rate_) {
        return rate_;
    }
    return fail(std::format(
        "the synthetic scene was built at {} S/s and its emitter placement, burst layout and "
        "noise all derive from that; {} S/s is a different scene. Reopen the URI with rate=.",
        rate_, rate));
}

Expected<double> SyntheticSource::set_gain(std::string_view stage, double)
{
    return fail(std::format(
        "the synthetic source has no gain stage '{}': emitter levels are set against the noise "
        "floor at scene creation, through snr_min and snr_max.",
        stage));
}

Status SyntheticSource::set_gain_auto(std::string_view stage, bool)
{
    return fail(std::format("the synthetic source has no gain stage '{}' to put into automatic "
                            "mode",
                            stage));
}

Status SyntheticSource::seek(dsp::SampleIndex index)
{
    std::scoped_lock lock(control_);

    if (length_samples_ > 0 && index > length_samples_) {
        return fail(std::format("cannot seek to sample {}: the scene is {} samples long", index,
                                length_samples_));
    }
    if (index == kNoPendingSeek) {
        // The bounded case is caught above. An unbounded scene has no length
        // to check against, so the sentinel has to be refused by name rather
        // than being published and then read back as "no seek was pending",
        // which would make the call silently do nothing.
        return fail(std::format("cannot seek to sample {}: that index is reserved", index));
    }

    if (running_.load(std::memory_order_acquire)) {
        pending_seek_.store(index, std::memory_order_release);
        return {};
    }

    position_ = index;
    write_index_.store(index, std::memory_order_relaxed);
    return {};
}

Status SyntheticSource::start(const StreamOptions& options, BlockSink sink)
{
    std::scoped_lock lock(control_);

    if (running_.load(std::memory_order_acquire) || thread_.joinable()) {
        return fail("the synthetic source is already streaming");
    }
    if (!sink) {
        return fail("a source cannot be started without a sink");
    }
    if (!scene_) {
        return fail("the synthetic source has no scene");
    }
    if (!std::isfinite(options.pace) || options.pace < 0.0) {
        return fail(std::format("pace must be zero for unthrottled or a positive multiple of "
                                "realtime, got {}",
                                options.pace));
    }
    if (length_samples_ > 0 && options.start_index > length_samples_) {
        return fail(std::format("cannot start at sample {}: the scene is {} samples long",
                                options.start_index, length_samples_));
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

    buffer_.assign(block, dsp::Complex32{});
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
        return fail(std::format("could not start the synthetic source thread: {}", error.what()),
                    error.code().value());
    }
    return {};
}

Status SyntheticSource::stop()
{
    std::scoped_lock lock(control_);
    join_locked();
    if (has_stop_error_) {
        return std::unexpected(stop_error_);
    }
    return {};
}

SourceStats SyntheticSource::stats() const
{
    SourceStats out;
    out.blocks_delivered = blocks_delivered_.load(std::memory_order_relaxed);
    out.samples_delivered = samples_delivered_.load(std::memory_order_relaxed);
    out.overrun_events = 0;
    out.samples_lost = 0;
    out.last_loss_index = 0;
    out.write_index = write_index_.load(std::memory_order_relaxed);
    return out;
}

ClockQuality SyntheticSource::clock() const
{
    ClockQuality quality = clock_model_.quality();
    quality.accuracy_ns =
        clock_model_.accuracy_ns_at(write_index_.load(std::memory_order_relaxed));
    return quality;
}

void SyntheticSource::run()
{
    using Clock = std::chrono::steady_clock;

    // Project policy, for the whole life of this thread rather than per
    // block. Scene::render produces nothing near the denormal range at any
    // level a scene is worth generating at, so this changes no sample the
    // siggen command line would have written; it is here so that every
    // floating point thread in the engine is in the same mode, which is what
    // makes a CPU result comparable with a GPU one. See denormal_mode.h.
    const dsp::ScopedDenormalFlush flush_denormals;

    std::uint64_t sequence = 0;
    Clock::time_point pace_origin = Clock::now();
    dsp::SampleIndex pace_origin_index = position_;

    while (!stop_requested_.load(std::memory_order_acquire)) {
        const dsp::SampleIndex requested =
            pending_seek_.exchange(kNoPendingSeek, std::memory_order_acq_rel);
        if (requested != kNoPendingSeek) {
            // Free, because render is a pure function of the absolute index.
            // No state is carried between blocks, so there is nothing to
            // rewind and nothing to warm up.
            position_ = requested;
            pace_origin = Clock::now();
            pace_origin_index = position_;
        }

        if (length_samples_ > 0 && position_ >= length_samples_) {
            break;
        }

        std::size_t want = block_samples_;
        if (length_samples_ > 0) {
            const dsp::SampleIndex remaining = length_samples_ - position_;
            if (remaining < static_cast<dsp::SampleIndex>(want)) {
                want = static_cast<std::size_t>(remaining);
            }
        }

        scene_->render(position_, dsp::ComplexSpan(buffer_.data(), want));

        SourceBlock block;
        block.stamp = dsp::BlockTimestamp{position_, anchor_ns_, rate_};
        block.format = SampleFormat::Cf32;
        block.sample_count = want;
        block.bytes = std::as_bytes(std::span<const dsp::Complex32>(buffer_.data(), want));
        block.dropped_before = 0;
        block.sequence = sequence;

        if (pace_ > 0.0) {
            const dsp::SampleIndex since = position_ - pace_origin_index;
            const double nominal_ns = static_cast<double>(duration_ns_of(since, rate_)) / pace_;
            const auto target =
                pace_origin + std::chrono::nanoseconds(static_cast<std::int64_t>(nominal_ns));
            if (target > Clock::now()) {
                std::this_thread::sleep_until(target);
            }
        }

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

Expected<SourceCapabilities> describe_synthetic_source(const SyntheticSourceConfig& config)
{
    if (auto ok = validate(config); !ok) {
        return std::unexpected(ok.error());
    }
    return capabilities_of(config);
}

Expected<std::unique_ptr<Source>> open_synthetic_source(const SyntheticSourceConfig& config)
{
    if (auto ok = validate(config); !ok) {
        return std::unexpected(ok.error());
    }

    auto spec = scene_spec_of(config);
    if (!spec) {
        return std::unexpected(spec.error());
    }

    auto scene = siggen::Scene::create(*spec);
    if (!scene) {
        return std::unexpected(with_context(scene.error(), "synthetic source"));
    }

    auto source = std::make_unique<SyntheticSource>();
    if (auto opened = source->open(config, capabilities_of(config), std::move(*scene)); !opened) {
        return std::unexpected(opened.error());
    }
    return std::unique_ptr<Source>(std::move(source));
}

}  // namespace revenant::source
