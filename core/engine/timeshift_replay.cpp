#include "core/engine/timeshift_replay.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <format>
#include <utility>

#include "core/engine/device_ring.h"
#include "core/engine/scheduler.h"

namespace revenant::engine {
namespace {

[[nodiscard]] double clamp_speed(double speed) {
    if (!std::isfinite(speed) || speed < 1.0) {
        return 1.0;
    }
    return std::min(speed, kMaxTimeShiftSpeed);
}

[[nodiscard]] std::int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

TimeShiftReplay::TimeShiftReplay(const TimeShiftBuffer& buffer) : buffer_(buffer) {}

TimeShiftReplay::~TimeShiftReplay() {
    stop();
    // The graph stops its own completion thread; the ring outlives it and
    // the scheduler outlives both, which is the engine's order too.
    graph_.reset();
    ring_.reset();
    scheduler_.reset();
}

Expected<std::unique_ptr<TimeShiftReplay>> TimeShiftReplay::create(
    const gpu::Context& context, const TimeShiftBuffer& buffer,
    const TimeShiftReplayConfig& config) {
    const TimeShiftExtent extent = buffer.extent();
    if (!extent.available) {
        return fail("the time-shift buffer holds nothing to replay yet");
    }
    if (config.graph.format != buffer.format() || config.graph.source_rate != buffer.rate()) {
        return fail("the time-shift buffer was kept for another stream than the graph's");
    }

    std::unique_ptr<TimeShiftReplay> out(new TimeShiftReplay(buffer));

    // Two workers. The live graph has the machine; a replay is one stream's
    // worth of recording and a pool its size would double the thread count
    // for nothing.
    SchedulerConfig scheduler_config;
    scheduler_config.worker_threads = 2;
    auto scheduler = Scheduler::create(context, scheduler_config);
    if (!scheduler) {
        return std::unexpected(with_context(scheduler.error(), "the time-shift replay"));
    }
    out->scheduler_ = std::move(*scheduler);

    // The engine's floor for a ring: a dispatch's whole support plus a block,
    // four times over. Nothing reads further back than that, so a deeper ring
    // would be VRAM held for no reader.
    const auto span = static_cast<dsp::SampleIndex>(config.graph.grid.prototype_length()) +
                      static_cast<dsp::SampleIndex>(config.graph.block_samples);
    RingConfig ring_config;
    ring_config.rate = config.graph.source_rate;
    ring_config.capacity_samples = std::bit_ceil(span * 4);
    auto ring = DeviceRing::create(context, ring_config);
    if (!ring) {
        return std::unexpected(with_context(ring.error(), "the time-shift replay"));
    }
    out->ring_ = std::make_unique<DeviceRing>(std::move(*ring));

    GraphConfig graph_config = config.graph;
    graph_config.flow = source::FlowControl::Demand;
    auto graph = Graph::create(context, *out->ring_, *out->scheduler_, graph_config);
    if (!graph) {
        return std::unexpected(with_context(graph.error(), "the time-shift replay"));
    }
    out->graph_ = std::move(*graph);
    if (auto prepared = out->graph_->prepare(config.prototype, config.twiddles); !prepared) {
        return std::unexpected(with_context(prepared.error(), "the time-shift replay"));
    }

    out->block_samples_ = out->graph_->geometry().block_samples;
    out->format_ = config.graph.format;
    out->rate_ = config.graph.source_rate;
    const dsp::SampleIndex start =
        std::clamp(config.start_index, extent.oldest_index, extent.newest_index);
    out->cursor_.store(start, std::memory_order_relaxed);
    out->speed_.store(clamp_speed(config.speed), std::memory_order_relaxed);
    out->thread_ = std::thread([raw = out.get()] { raw->run(); });
    return out;
}

Status TimeShiftReplay::set_spectrum_sink(SpectrumSink sink) {
    if (!sink) {
        return graph_->set_spectrum_sink({});
    }
    return graph_->set_spectrum_sink([this, sink = std::move(sink)](const SpectrumFrame& frame) {
        SpectrumFrame marked = frame;
        marked.timeshift_replay = true;
        marked.timeshift_wall_ns = buffer_.wall_ns_at(live_index_of(frame.start));
        return sink(marked);
    });
}

void TimeShiftReplay::seek(dsp::SampleIndex live_index, double speed) {
    speed_.store(clamp_speed(speed), std::memory_order_release);
    seek_to_.store(live_index, std::memory_order_relaxed);
    seek_pending_.store(true, std::memory_order_release);
}

dsp::SampleIndex TimeShiftReplay::position() const {
    return cursor_.load(std::memory_order_acquire);
}

double TimeShiftReplay::speed() const { return speed_.load(std::memory_order_acquire); }

dsp::SampleIndex TimeShiftReplay::live_index_of(dsp::SampleIndex replay_index) const {
    const dsp::SampleIndex cursor = cursor_.load(std::memory_order_acquire);
    const dsp::SampleIndex fed = replay_index_.load(std::memory_order_acquire);
    // cursor and fed advance together, so their difference is the offset in
    // force for whatever is in flight.
    return cursor >= fed ? replay_index + (cursor - fed) : replay_index;
}

void TimeShiftReplay::stop() {
    stopping_.store(true, std::memory_order_release);
    if (graph_ != nullptr) {
        graph_->cancel();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void TimeShiftReplay::run() {
    const std::size_t bps = source::bytes_per_sample(format_);
    std::vector<std::byte> staging(block_samples_ * bps);
    std::uint64_t sequence = 0;
    // The anchor of the replay stream is the live wall clock of its first
    // sample, so a frame's own index already reads as roughly the right time;
    // the spectrum sink corrects it exactly through live_index_of.
    const std::int64_t anchor_ns = buffer_.wall_ns_at(cursor_.load(std::memory_order_acquire));

    double due = 0.0;
    std::int64_t last = steady_ns();
    while (!stopping_.load(std::memory_order_acquire)) {
        if (seek_pending_.exchange(false, std::memory_order_acq_rel)) {
            const TimeShiftExtent extent = buffer_.extent();
            const dsp::SampleIndex to = std::clamp(seek_to_.load(std::memory_order_relaxed),
                                                   extent.oldest_index, extent.newest_index);
            cursor_.store(to, std::memory_order_release);
        }

        const std::int64_t now = steady_ns();
        const double speed = speed_.load(std::memory_order_acquire);
        due += speed * static_cast<double>(rate_) * static_cast<double>(now - last) / 1.0e9;
        last = now;
        // A burst after a stall is capped, so a replay that waited on live does
        // not then rush a whole wait's worth through the GPU at once.
        due = std::min(due, 4.0 * static_cast<double>(block_samples_));

        if (due < static_cast<double>(block_samples_)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        dsp::SampleIndex cursor = cursor_.load(std::memory_order_acquire);
        const TimeShiftExtent extent = buffer_.extent();
        if (!extent.available) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (cursor < extent.oldest_index) {
            // Overwritten under us, which only a stalled graph can cause at a
            // pace of one or more. The oldest sample still held is the honest
            // place to carry on from.
            cursor = extent.oldest_index;
        }
        const std::size_t got = buffer_.read(cursor, block_samples_, staging);
        if (got < block_samples_) {
            // At live: wait for a whole block so the graph is fed the block
            // size it was built for, which keeps the spectrum's row rate the
            // same as live's.
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        const dsp::SampleIndex replay_index = replay_index_.load(std::memory_order_relaxed);
        source::SourceBlock block;
        block.stamp.start = replay_index;
        block.stamp.epoch_anchor_ns = anchor_ns;
        block.stamp.rate = rate_;
        block.format = format_;
        block.sample_count = got;
        block.bytes = std::span<const std::byte>(staging.data(), got * bps);
        block.sequence = sequence++;

        cursor_.store(cursor, std::memory_order_release);
        if (auto fed = graph_->on_block(block); !fed) {
            // A stop cancels the graph and this is that stop arriving; any
            // other refusal ends the replay the same way, with nothing left
            // to report it to but the receivers going quiet.
            break;
        }
        replay_index_.store(replay_index + got, std::memory_order_release);
        cursor_.store(cursor + got, std::memory_order_release);
        due -= static_cast<double>(got);
    }
}

}  // namespace revenant::engine
