// A thread of its own for the work a server does on a receiver's audio:
// the event decoders in core/rpc/decoders.h, a P25 receiver's voice stream
// in core/rpc/voice_audio.h, and RDS.
//
// WHY THIS EXISTS
//
// Until 2026-09-23 every one of those ran inside its audio sink, which the
// engine calls on its completion thread: the one thread that retires every
// GPU frame, carries every receiver's audio home and hands the spectrum to
// the display. A decoder there is decoding standing between a frame and the
// next one, and on a Paced source a completion thread that falls three frames
// behind loses the block the radio delivers next. The owner's precedence
// after that day's playtest was decoding and audio first, the display next,
// signal identification last, and disconnected so none can contend with
// another. So the sink now copies the chunk and returns, and the decoding
// happens here, on a thread at the Listening priority core/thread_role.h
// gives the sample path.
//
// ORDER. One lane serves many routes, first in first out, so one route's
// chunks are decoded in the order the engine delivered them, which is the
// only order a decoder can take. A server with several lanes pins each route
// to one.
//
// WHAT FULL MEANS. The completion thread never waits on a lane that is
// keeping up with the radio. When every job is taken it either drops the
// chunk and counts it, for a source running on a clock, or waits for a job to
// come back, for a Demand source running unthrottled. The second is the
// graph's own rule: an unthrottled file has no clock to fall behind, so
// waiting is backpressure and loses nothing, and a replay decodes every
// chunk however fast it runs. A dropped chunk reaches the decoder as a gap in
// AudioChunk::start, which every decoder here already handles as one.
//
// WHY A HEADER, on core/rpc/decoders.h's argument: a Catch2 case can drive it
// with no engine and no socket in the way.

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "core/engine/engine.h"
#include "core/engine/load_clock.h"
#include "core/engine/spsc_ring.h"
#include "core/error.h"
#include "core/thread_role.h"

namespace revenant::rpc {

// What a lane runs for one chunk. Built once per route and co-owned by every
// job queued for it, so a route taken off while its chunks are still queued
// is still there when they run: the same argument that has every route in
// core/rpc/server.cpp co-owned by its sink.
using LaneWork = std::function<void(const engine::AudioChunk&)>;

struct DecodeLaneStats {
    std::uint64_t posted = 0;
    std::uint64_t run = 0;

    // Chunks a full lane could not take from a source on a clock. Each is a
    // gap in some route's stream.
    std::uint64_t dropped = 0;

    // Times the completion thread waited for a job to come back, which only
    // an unthrottled Demand source does. See WHAT FULL MEANS above.
    std::uint64_t waits = 0;

    // Nanoseconds this lane spent running work, and between a job being
    // posted and starting to run, summed over jobs.
    std::uint64_t busy_ns = 0;
    std::uint64_t latency_ns = 0;
    std::uint64_t latency_max_ns = 0;
};

class DecodeLane {
public:
    // Jobs the lane holds, which is how far behind it may fall before a
    // chunk is dropped. 512 is about fourteen seconds of one receiver's
    // chunks at revenant-engine's 36.6 blocks a second.
    static constexpr std::size_t kDefaultJobs = 512;

    [[nodiscard]] static Expected<std::unique_ptr<DecodeLane>> create(
        std::wstring name, std::size_t jobs = kDefaultJobs)
    {
        std::unique_ptr<DecodeLane> lane(new (std::nothrow) DecodeLane());
        if (lane == nullptr) {
            return fail("could not allocate a decode lane");
        }
        auto queued = engine::SpscRing<Job*>::create(jobs);
        if (!queued) {
            return std::unexpected(with_context(queued.error(), "a decode lane's queue"));
        }
        auto free = engine::SpscRing<Job*>::create(jobs);
        if (!free) {
            return std::unexpected(with_context(free.error(), "a decode lane's free list"));
        }
        lane->queued_ = std::move(*queued);
        lane->free_ = std::move(*free);
        lane->jobs_.reserve(jobs);
        for (std::size_t i = 0; i < jobs; ++i) {
            lane->jobs_.push_back(std::make_unique<Job>());
            Job* job = lane->jobs_.back().get();
            static_cast<void>(lane->free_->write(std::span<Job* const>(&job, 1)));
        }
        try {
            lane->thread_ = std::thread([raw = lane.get(), name = std::move(name)] {
                describe_this_thread(name, ThreadClass::Listening);
                raw->run();
            });
        } catch (const std::system_error& error) {
            return fail(std::format("could not start a decode lane: {}", error.what()),
                        error.code().value());
        }
        return lane;
    }

    ~DecodeLane() { stop(); }

    DecodeLane(const DecodeLane&) = delete;
    DecodeLane& operator=(const DecodeLane&) = delete;
    DecodeLane(DecodeLane&&) = delete;
    DecodeLane& operator=(DecodeLane&&) = delete;

    // The engine's completion thread, and nothing else: this is the producer
    // half of two SPSC rings. Copies the chunk, queues `work` to run on it,
    // and returns. `wait_when_full` is the caller's reading of WHAT FULL
    // MEANS above: true for an unthrottled Demand source.
    //
    // Returns false for a dropped chunk, and for a lane already stopped.
    bool post(const std::shared_ptr<const LaneWork>& work, const engine::AudioChunk& chunk,
              bool wait_when_full)
    {
        Job* job = nullptr;
        for (;;) {
            // Snapshot first, for the reason Scheduler::Impl::loop gives: a
            // return that lands between the read and the wait moves the
            // counter and the wait falls straight through.
            const std::uint64_t observed = returned_.load(std::memory_order_acquire);
            if (stopping_.load(std::memory_order_acquire)) {
                return false;
            }
            if (free_->read(std::span<Job*>(&job, 1)) == 1) {
                break;
            }
            if (!wait_when_full) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            waits_.fetch_add(1, std::memory_order_relaxed);
            returned_.wait(observed, std::memory_order_acquire);
        }

        job->work = work;
        job->samples.assign(chunk.samples.begin(), chunk.samples.end());
        job->chunk = chunk;
        job->chunk.samples = std::span<const float>(job->samples);

        // The levelled copy is not taken: nothing on a lane is a listener,
        // and the span points into the completion thread's buffer, which
        // the next dispatch overwrites. See AudioChunk::heard.
        job->chunk.heard = {};
        job->posted_ns = engine::load_clock_ns();

        // Cannot fail: the queue holds as many as there are jobs.
        static_cast<void>(queued_->write(std::span<Job* const>(&job, 1)));
        posted_.fetch_add(1, std::memory_order_relaxed);
        signal_.fetch_add(1, std::memory_order_release);
        signal_.notify_one();
        return true;
    }

    // Stops the thread and joins it. Idempotent. Work still queued is
    // released without running, which is right for the one caller, a server
    // that has already taken every sink off and cleared every route's owner.
    void stop()
    {
        if (stopping_.exchange(true, std::memory_order_acq_rel)) {
            if (thread_.joinable()) {
                thread_.join();
            }
            return;
        }
        signal_.fetch_add(1, std::memory_order_release);
        signal_.notify_all();
        returned_.fetch_add(1, std::memory_order_release);
        returned_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
        for (auto& job : jobs_) {
            job->work.reset();
        }
    }

    // Returns once everything posted before the call has run, or once
    // `limit` has passed, and says which. For a caller about to read or flush
    // a decoder's state, which has to see every chunk the engine had already
    // delivered: a lane is one hand-off behind the engine, and a decoder read
    // straight after the engine stops would otherwise be missing whatever was
    // still queued. The caller is the loop thread, which is below this lane's
    // priority, so the wait is the precedence working rather than inverted;
    // the limit is what keeps a lane that has fallen behind a radio from
    // holding every client's delivery for as long as it is behind.
    bool drain(std::chrono::nanoseconds limit) const
    {
        const std::uint64_t target = posted_.load(std::memory_order_acquire);
        const auto deadline = std::chrono::steady_clock::now() + limit;
        for (;;) {
            const std::uint64_t done = run_.load(std::memory_order_acquire);
            if (done >= target) {
                return true;
            }
            if (stopping_.load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            // Short sleeps rather than an atomic wait, which has no timeout.
            // A drain is a control-plane call and a millisecond here is
            // nothing to it.
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    [[nodiscard]] DecodeLaneStats stats() const
    {
        DecodeLaneStats out;
        out.posted = posted_.load(std::memory_order_relaxed);
        out.run = run_.load(std::memory_order_relaxed);
        out.dropped = dropped_.load(std::memory_order_relaxed);
        out.waits = waits_.load(std::memory_order_relaxed);
        out.busy_ns = busy_ns_.load(std::memory_order_relaxed);
        out.latency_ns = latency_ns_.load(std::memory_order_relaxed);
        out.latency_max_ns = latency_max_ns_.load(std::memory_order_relaxed);
        return out;
    }

private:
    // Built once, by create, and handed between the two threads by pointer
    // through the two rings; every field is written by post() before the
    // lane can see it. The samples keep their capacity from job to job, so a
    // lane in its steady state copies a chunk without reaching the allocator.
    struct Job {
        std::shared_ptr<const LaneWork> work;
        std::vector<float> samples;
        engine::AudioChunk chunk;
        std::uint64_t posted_ns = 0;
    };

    DecodeLane() = default;

    void run()
    {
        for (;;) {
            const std::uint64_t observed = signal_.load(std::memory_order_acquire);
            if (stopping_.load(std::memory_order_acquire)) {
                return;
            }
            Job* job = nullptr;
            if (queued_->read(std::span<Job*>(&job, 1)) == 0) {
                signal_.wait(observed, std::memory_order_acquire);
                continue;
            }

            const std::uint64_t began = engine::load_clock_ns();
            const std::uint64_t waited = began - job->posted_ns;
            latency_ns_.fetch_add(waited, std::memory_order_relaxed);
            std::uint64_t seen = latency_max_ns_.load(std::memory_order_relaxed);
            while (waited > seen && !latency_max_ns_.compare_exchange_weak(
                                        seen, waited, std::memory_order_relaxed)) {
            }

            if (job->work != nullptr && *job->work) {
                (*job->work)(job->chunk);
            }
            busy_ns_.fetch_add(engine::load_clock_ns() - began, std::memory_order_relaxed);

            // The route's reference goes here, on this thread, so a route
            // whose last reference this was is destroyed off the completion
            // thread.
            job->work.reset();
            static_cast<void>(free_->write(std::span<Job* const>(&job, 1)));
            returned_.fetch_add(1, std::memory_order_release);
            returned_.notify_one();
            run_.fetch_add(1, std::memory_order_release);
        }
    }

    std::vector<std::unique_ptr<Job>> jobs_;
    std::unique_ptr<engine::SpscRing<Job*>> queued_;
    std::unique_ptr<engine::SpscRing<Job*>> free_;
    std::thread thread_;

    std::atomic<std::uint64_t> signal_{0};
    std::atomic<std::uint64_t> returned_{0};
    std::atomic<bool> stopping_{false};

    std::atomic<std::uint64_t> posted_{0};
    std::atomic<std::uint64_t> run_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> waits_{0};
    std::atomic<std::uint64_t> busy_ns_{0};
    std::atomic<std::uint64_t> latency_ns_{0};
    std::atomic<std::uint64_t> latency_max_ns_{0};
};

}  // namespace revenant::rpc
