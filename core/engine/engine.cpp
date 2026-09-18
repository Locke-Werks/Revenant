// The engine, assembled.
//
// This file is deliberately thin. Everything hard is in graph.cpp, which owns
// the submission model, and in scheduler.cpp, which owns the threads. What is
// left here is the part the frozen core/engine/engine.h actually describes:
// opening a source, sizing the grid and the ring against it, reporting what
// was settled on, and driving the whole thing from run() to stop().
//
// TWO DECISIONS THAT SHOW UP IN EngineInfo AND ARE NOT OBVIOUS
//
// The grid is sized against the DEVICE before it is sized against the source.
// A 4096-channel grid is arithmetic that fits comfortably and shared memory
// that does not: the FFT holds a whole transform resident, the AMD integrated
// part in the conformance matrix caps maxComputeSharedMemorySize at 32768
// where the RTX 4090 offers 49152, and a grid that exceeds it cannot be
// launched at all. So the channel count is clamped to what the device holds
// and EngineInfo::grid says what happened.
//
// EngineInfo::channel_rate and channel_spacing are integer hertz, and
// rate / D and rate / M are frequently not whole numbers: 2,500,000 over 64
// channels is 39062.5 Hz of spacing. These two fields therefore truncate, and
// they are for display. Nothing in the engine computes from them. The exact
// value is dsp::ChannelCentre, carried as the rational it is, and
// VrxPlacement::channel_centre is what a receiver actually tunes against. See
// the comment on ChannelCentre in core/dsp/pfb.h for why rounding it anywhere
// else is the unsourceable tuning offset the conventions exist to prevent.
//
// WHAT STOP() COSTS
//
// stop() ends the stream for good. Waking a source thread parked in the
// ring's blocking reserve means stopping the cursor table, and a stopped
// cursor table does not restart. Running again means opening the source
// again, which costs a file handle and a grid design and nothing else. The
// alternative is a restart path that exists to be tested and never used, and
// a teardown that can hang when the GPU does.

#include "core/engine/engine.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <format>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "core/dsp/pfb_fft_reference.h"
#include "core/engine/graph.h"
#include "core/engine/ring_consumer.h"
#include "core/engine/scheduler.h"

namespace revenant::engine {
namespace {

// How often run() looks to see whether the source thread has finished.
//
// A poll and not a signal, because core/source/source.h offers no completion
// callback: Source::running() goes false when the delivery loop exits and
// nothing notifies. Two milliseconds is below any latency a person can
// perceive at the end of a stream and costs five hundred atomic loads a
// second on one thread. The integrator note asks for an end-of-stream
// callback on Source, which would delete this.
constexpr std::chrono::milliseconds kRunPollInterval{2};

// The 2x-oversampled design. A critically sampled bank splits a signal sitting
// on a channel edge across two channels and neither one is usable, which is
// the whole reason core/dsp/pfb.h calls D = M/2 the project's choice rather
// than a tuning parameter.
[[nodiscard]] std::uint32_t oversampled_decimation(std::uint32_t channels) {
    return channels > 1 ? channels / 2 : 1;
}

class EngineImpl final : public Engine {
public:
    EngineImpl() = default;

    ~EngineImpl() override {
        (void)stop();
        // Order matters. The source's thread calls into the graph, the graph's
        // completion thread calls into the receivers' sinks, and the ring
        // outlives both. Destroying them in declaration order would take the
        // graph out from under a source thread that has not joined.
        source_.reset();
        graph_.reset();
        scheduler_.reset();
    }

    [[nodiscard]] Status configure(const EngineConfig& config) {
        config_ = config;

        gpu::Context::Options options;
        options.device_index = config.gpu_index;
        auto context = gpu::Context::create(options);
        if (!context) {
            return std::unexpected(with_context(context.error(), "Engine::create"));
        }
        context_ = std::move(*context);
        info_.device = context_.info();

        SchedulerConfig scheduler_config;
        auto scheduler = Scheduler::create(context_, scheduler_config);
        if (!scheduler) {
            return std::unexpected(with_context(scheduler.error(), "Engine::create"));
        }
        scheduler_ = std::move(*scheduler);
        return {};
    }

    [[nodiscard]] Status open_source(std::string_view uri) override {
        if (source_ != nullptr) {
            return fail("this engine already has a source open. A source is owned for the life "
                        "of the engine, because a source outliving the graph that reads it is a "
                        "use-after-free waiting for a scheduling accident");
        }

        auto opened = source::open_source(uri);
        if (!opened) {
            return std::unexpected(with_context(opened.error(), "Engine::open_source"));
        }
        auto source = std::move(*opened);

        const dsp::SampleRate rate = source->sample_rate();
        if (rate <= 0) {
            return fail(std::format("'{}' reports a sample rate of {}", uri, rate));
        }

        // --- the grid -------------------------------------------------------

        dsp::GridParams grid;
        grid.channels = config_.channels;
        grid.taps_per_branch = config_.taps_per_branch;

        if (grid.channels == 0 || !std::has_single_bit(grid.channels)) {
            return fail(std::format("channels is {} and the channelizer needs a power of two",
                                    grid.channels));
        }

        // The device's shared memory, not a guess. See the note at the top of
        // the file and max_fft_transform_size in core/dsp/pfb_fft_reference.h.
        const std::uint32_t transform_ceiling =
            dsp::max_fft_transform_size(info_.device.max_workgroup_shared_memory);
        if (transform_ceiling == 0) {
            return fail(std::format("'{}' reports {} bytes of workgroup shared memory, which "
                                    "holds no transform at all",
                                    info_.device.name, info_.device.max_workgroup_shared_memory));
        }
        if (grid.channels > transform_ceiling) {
            clamp_note_ = std::format(
                "{} channels need {} bytes of shared memory and '{}' offers {}, so the grid was "
                "reduced to {} channels",
                grid.channels, dsp::fft_shared_bytes(grid.channels), info_.device.name,
                info_.device.max_workgroup_shared_memory, transform_ceiling);
            grid.channels = transform_ceiling;
        }
        grid.decimation = oversampled_decimation(grid.channels);

        if (auto ok = dsp::validate(grid); !ok) {
            return std::unexpected(with_context(ok.error(), "Engine::open_source"));
        }

        auto prototype = dsp::design_prototype(grid);
        if (!prototype) {
            return std::unexpected(with_context(prototype.error(), "Engine::open_source"));
        }
        auto twiddles = dsp::build_twiddles(grid.channels);
        if (!twiddles) {
            return std::unexpected(with_context(twiddles.error(), "Engine::open_source"));
        }

        // --- the block size --------------------------------------------------

        std::size_t block_samples = config_.block_samples;
        if (block_samples == 0) {
            block_samples = source->capabilities().preferred_block_samples;
        }
        if (block_samples == 0) {
            block_samples = 65'536;
        }
        if (block_samples < grid.decimation) {
            block_samples = grid.decimation;
        }

        // --- the ring ---------------------------------------------------------

        // One dispatch reads the filter's whole support plus a block, so a
        // ring smaller than that can never assemble a window. Four times that
        // is the floor rather than exactly that, so the writer is not
        // permanently pressed against the retirement floor.
        const auto span = static_cast<dsp::SampleIndex>(grid.prototype_length()) +
                          static_cast<dsp::SampleIndex>(block_samples);
        const dsp::SampleIndex floor_capacity = std::bit_ceil(span * 4);

        RingConfig ring_config;
        ring_config.rate = rate;
        ring_config.seconds_wanted = config_.ring_seconds;
        const auto from_seconds =
            static_cast<dsp::SampleIndex>(config_.ring_seconds * static_cast<double>(rate));
        if (from_seconds < floor_capacity) {
            ring_config.capacity_samples = floor_capacity;
        }

        auto ring = DeviceRing::create(context_, ring_config);
        if (!ring) {
            return std::unexpected(with_context(ring.error(), "Engine::open_source"));
        }
        ring_ = std::make_unique<DeviceRing>(std::move(*ring));

        // A ring clamped by the device can end up smaller than one dispatch
        // needs, and the honest answer then is a smaller block rather than a
        // failure: the whole sizing contract is that the engine shrinks to fit
        // and says so.
        const dsp::SampleIndex capacity = ring_->geometry().capacity_samples;
        if (static_cast<dsp::SampleIndex>(grid.prototype_length()) + block_samples > capacity / 2) {
            const auto room = capacity / 2;
            if (room <= grid.prototype_length()) {
                return fail(std::format(
                    "the device ring holds {} samples and the prototype alone spans {}. The grid "
                    "is too large for the memory this device will allocate",
                    capacity, grid.prototype_length()));
            }
            const std::size_t reduced =
                static_cast<std::size_t>(room - grid.prototype_length());
            if (!clamp_note_.empty()) {
                clamp_note_.append("; then ");
            }
            clamp_note_.append(std::format(
                "a {}-sample block plus {} of filter support would not fit half a {}-sample "
                "ring, so the block was reduced to {}",
                block_samples, grid.prototype_length(), capacity, reduced));
            block_samples = reduced;
        }

        // --- the graph ---------------------------------------------------------

        GraphConfig graph_config;
        graph_config.grid = grid;
        graph_config.source_rate = rate;
        graph_config.format = source->capabilities().native_format;
        graph_config.flow = source->capabilities().flow;
        graph_config.block_samples = block_samples;
        graph_config.audio_rate = config_.audio_rate;

        auto graph = Graph::create(context_, *ring_, *scheduler_, graph_config);
        if (!graph) {
            return std::unexpected(with_context(graph.error(), "Engine::open_source"));
        }
        graph_ = std::move(*graph);

        if (auto prepared = graph_->prepare(*prototype, *twiddles); !prepared) {
            graph_.reset();
            return std::unexpected(with_context(prepared.error(), "Engine::open_source"));
        }

        source_ = std::move(source);
        capabilities_ = source_->capabilities();
        block_samples_ = block_samples;
        prototype_ = std::move(*prototype);

        info_.ring = ring_->geometry();
        info_.grid = grid;
        info_.source_rate = rate;

        // Truncating division, deliberately and only for display. See the note
        // at the top of this file.
        info_.channel_rate = rate / static_cast<dsp::SampleRate>(grid.decimation);
        info_.channel_spacing =
            static_cast<dsp::Hertz>(rate / static_cast<dsp::SampleRate>(grid.channels));

        if (!graph_->geometry().clamp_reason.empty()) {
            if (!clamp_note_.empty()) {
                clamp_note_.append("; then ");
            }
            clamp_note_.append(graph_->geometry().clamp_reason);
        }
        if (!clamp_note_.empty() && !info_.ring.clamped) {
            // The ring is the only field in the frozen EngineInfo that can
            // carry a sentence, so a grid or block clamp rides along in it
            // rather than going unreported. The alternative is a caller who
            // asked for 4096 channels, got 2048, and finds out when a
            // frequency lands in the wrong channel.
            info_.ring.clamped = true;
            info_.ring.clamp_reason = clamp_note_;
        } else if (!clamp_note_.empty()) {
            info_.ring.clamp_reason.append("; also ");
            info_.ring.clamp_reason.append(clamp_note_);
        }

        return {};
    }

    [[nodiscard]] const source::SourceCapabilities& source_capabilities() const override {
        return capabilities_;
    }

    [[nodiscard]] const EngineInfo& info() const override { return info_; }

    [[nodiscard]] Expected<VrxId> add_vrx(const VrxParams& params) override {
        if (graph_ == nullptr) {
            return fail("Engine::add_vrx before a source is open: a receiver is placed on the "
                        "grid, and there is no grid until the source's rate is known");
        }

        // Pure: the grid, the rate and the request, and nothing else. This is
        // the whole of what adding a receiver computes against the coarse
        // stage, and it reads it rather than changing it.
        auto placement = place(info_.grid, info_.source_rate, params);
        if (!placement) {
            return std::unexpected(with_context(placement.error(), "Engine::add_vrx"));
        }

        const VrxId id{next_id_.fetch_add(1, std::memory_order_relaxed)};
        auto added = graph_->add_vrx(id, params, *placement);
        if (!added) {
            return std::unexpected(added.error());
        }
        return id;
    }

    [[nodiscard]] Status remove_vrx(VrxId id) override {
        if (graph_ == nullptr) {
            return fail("Engine::remove_vrx before a source is open");
        }
        return graph_->remove_vrx(id);
    }

    [[nodiscard]] Status set_vrx_params(VrxId id, const VrxParams& params) override {
        if (graph_ == nullptr) {
            return fail("Engine::set_vrx_params before a source is open");
        }
        auto placement = place(info_.grid, info_.source_rate, params);
        if (!placement) {
            return std::unexpected(with_context(placement.error(), "Engine::set_vrx_params"));
        }
        return graph_->set_vrx_params(id, params, *placement);
    }

    [[nodiscard]] Expected<VrxStatus> vrx_status(VrxId id) const override {
        if (graph_ == nullptr) {
            return fail("Engine::vrx_status before a source is open");
        }
        return graph_->vrx_status(id);
    }

    [[nodiscard]] std::vector<VrxId> vrx_ids() const override {
        return graph_ == nullptr ? std::vector<VrxId>{} : graph_->vrx_ids();
    }

    [[nodiscard]] Status set_audio_sink(VrxId id, AudioSink sink) override {
        if (graph_ == nullptr) {
            return fail("Engine::set_audio_sink before a source is open");
        }
        return graph_->set_audio_sink(id, std::move(sink));
    }

    [[nodiscard]] Status run() override {
        if (source_ == nullptr || graph_ == nullptr) {
            return fail("Engine::run before a source is open");
        }
        if (running_.load(std::memory_order_acquire)) {
            return fail("this engine is already running");
        }

        stop_requested_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);

        source::StreamOptions options;
        options.block_samples = block_samples_;

        // Unthrottled. Faster than realtime is not a mode and there is no code
        // path for it: it is what happens when nothing is holding a stopwatch,
        // and the only thing setting the pace is the graph's blocking reserve.
        options.pace = 0.0;

        Graph* graph = graph_.get();
        auto started = source_->start(options, [graph](const source::SourceBlock& block) {
            return graph->on_block(block);
        });
        if (!started) {
            running_.store(false, std::memory_order_release);
            return std::unexpected(with_context(started.error(), "Engine::run"));
        }

        {
            std::unique_lock lock(run_lock_);
            while (!stop_requested_.load(std::memory_order_acquire) && source_->running()) {
                run_signal_.wait_for(lock, kRunPollInterval);
            }
        }

        // stop() on a source that has already finished is the join plus
        // whatever error ended the stream, which is what this wants either
        // way.
        Status ended = source_->stop();
        Status flushed = graph_->flush();
        running_.store(false, std::memory_order_release);

        if (!ended) {
            return ended;
        }
        if (!flushed) {
            return flushed;
        }
        if (scheduler_ != nullptr && scheduler_->failed()) {
            return std::unexpected(scheduler_->error());
        }
        return {};
    }

    [[nodiscard]] Status stop() override {
        stop_requested_.store(true, std::memory_order_release);
        if (graph_ != nullptr) {
            graph_->cancel();
        }
        run_signal_.notify_all();
        return {};
    }

    [[nodiscard]] bool running() const override {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] source::SourceStats source_stats() const override {
        source::SourceStats out;
        if (source_ != nullptr) {
            out = source_->stats();
        }
        if (graph_ == nullptr) {
            return out;
        }

        // The ring's losses are folded in on purpose.
        //
        // The frozen Engine has exactly one place to report a counted loss,
        // and a sample the graph could not place is gone in precisely the way
        // a sample the device dropped is gone. Reporting only the source's
        // view would produce a run that looks continuous and is not, with
        // nothing downstream able to tell, which is the failure this counter
        // exists to prevent. The two are distinguishable: a Demand source
        // reports zero of its own, by construction, so anything here came
        // from the graph.
        const GraphStats graph_stats = graph_->stats();
        out.overrun_events += graph_stats.overrun_events;
        out.samples_lost += graph_stats.samples_dropped;
        return out;
    }

    [[nodiscard]] const Graph* graph() const { return graph_.get(); }
    [[nodiscard]] const Scheduler* scheduler() const { return scheduler_.get(); }
    [[nodiscard]] const dsp::PrototypeFilter& prototype() const { return prototype_; }

private:
    EngineConfig config_{};
    EngineInfo info_{};
    std::string clamp_note_;

    gpu::Context context_;
    std::unique_ptr<Scheduler> scheduler_;

    // Behind a unique_ptr because DeviceRing's State is an incomplete type in
    // the frozen header and its defaulted default constructor cannot be
    // instantiated outside device_ring.cpp. The out-of-line move constructor
    // and destructor are declared, so holding one by pointer works where
    // holding one by value does not. Declared before graph_, so the graph is
    // destroyed first: the graph's frames reference the ring's buffer.
    std::unique_ptr<DeviceRing> ring_;
    std::unique_ptr<Graph> graph_;
    std::unique_ptr<source::Source> source_;
    source::SourceCapabilities capabilities_{};
    dsp::PrototypeFilter prototype_{};
    std::size_t block_samples_ = 0;

    // Ids start at one so that a default-constructed VrxId is never a live
    // receiver; see VrxId::valid().
    std::atomic<std::uint32_t> next_id_{1};

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    // Control plane only. run() parks on it and stop() wakes it; the sample
    // path never touches either.
    mutable std::mutex run_lock_;
    std::condition_variable run_signal_;
};

}  // namespace

Expected<std::unique_ptr<Engine>> Engine::create(const EngineConfig& config) {
    auto engine = std::make_unique<EngineImpl>();
    if (auto configured = engine->configure(config); !configured) {
        return std::unexpected(configured.error());
    }
    return std::unique_ptr<Engine>(std::move(engine));
}

}  // namespace revenant::engine
