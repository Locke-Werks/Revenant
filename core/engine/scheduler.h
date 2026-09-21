// The engine's threads: one work-stealing pool, one completion thread, and
// nothing else.
//
// WHY A POOL AND NOT A THREAD PER CHAIN
//
// The obvious arrangement for a multi-receiver SDR is a thread per receiver.
// It is also the ceiling every existing implementation runs into: fifty
// receivers is fifty threads on a machine with thirty-two hardware threads,
// the scheduler timeslices them, and the cost is not the arithmetic but the
// context switches and the cache lines each switch throws away. Worse, it
// couples the receiver count to a resource the operating system rations, so
// the two hundredth receiver fails for a reason that has nothing to do with
// the radio.
//
// Here the arithmetic is on the GPU, so what the host actually has to do per
// block is small and bursty: partition a memcpy into the staging buffer,
// record a command buffer, and hand audio to sinks once it comes back. That is
// a fork-join shape, which is what a work-stealing pool is for. The receiver
// count then costs receivers, not threads.
//
// THE THREE KINDS OF THREAD, AND WHY THEY ARE SEPARATE
//
//   the source thread   Source::start spawns it and the source owns it. A
//                       Paced device must be serviced on its own clock and
//                       cannot wait behind DSP work; a Demand source blocks
//                       in its sink on purpose, which would deadlock a pool
//                       that the blocking consumer also needs. See the
//                       comment on Source::start in core/source/source.h.
//   the completion      One, here. It waits on the ring timeline semaphore
//                       and finishes frames in submission order. It is
//                       separate from the recording thread so that the host
//                       never has to wait for the GPU before recording the
//                       next block, which is the whole reason the submission
//                       model uses a timeline semaphore at all.
//   the pool            hardware_concurrency minus the two above, minimum
//                       one. Fork-join only.
//
// NO MUTEX ON THE SAMPLE PATH. The deques are Chase-Lev, the completion queue
// is the project's SpscRing, and an idle worker parks on std::atomic::wait,
// which is WaitOnAddress on Windows. The one mutex in this file guards the
// first error and the thread handles, both control plane.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include <vulkan/vulkan.h>

#include "core/error.h"
#include "core/gpu/context.h"

namespace revenant::engine {

// hardware_concurrency less the threads the engine runs outside the pool,
// floored at one. A machine that reports nothing gets one worker rather than
// zero, because a pool with no workers silently becomes a serial executor and
// the fact would never surface.
[[nodiscard]] std::size_t default_worker_count(std::size_t reserved_threads);

// A fork-join pool over per-worker Chase-Lev deques.
//
// The only entry point is parallel_for, deliberately. An arbitrary task queue
// invites exactly the design this class exists to prevent: a long-lived task
// per receiver, which is a thread per chain wearing a different name.
//
// Work stealing rather than a shared queue because the chunks are not equal.
// A receiver whose demodulator is a sign flip and one running a resampler cost
// different amounts, and a static split leaves the cheap worker idle while the
// expensive one finishes. Stealing rebalances without anybody measuring
// anything.
class WorkStealingPool {
public:
    // Called with a half-open sub-range of [0, items). Must be safe to call
    // concurrently on disjoint ranges, which is the only contract; nothing
    // here serialises anything for a body that shares state.
    using RangeBody = std::function<void(std::size_t begin, std::size_t end)>;

    [[nodiscard]] static Expected<std::unique_ptr<WorkStealingPool>> create(std::size_t workers);

    ~WorkStealingPool();

    WorkStealingPool(const WorkStealingPool&) = delete;
    WorkStealingPool& operator=(const WorkStealingPool&) = delete;
    WorkStealingPool(WorkStealingPool&&) = delete;
    WorkStealingPool& operator=(WorkStealingPool&&) = delete;

    [[nodiscard]] std::size_t worker_count() const;

    // Runs body over [0, items) split no finer than `grain`, and returns once
    // every element has been covered. The calling thread participates rather
    // than blocking, so a caller that is itself the only producer does not
    // idle while the pool works.
    //
    // Reentrancy: one submission at a time. A second thread calling this while
    // a job is live runs its own body inline on the calling thread. That is
    // correct, merely serial, and it is a deliberate simplification: the graph
    // has exactly one recording thread and one completion thread, and neither
    // forks while the other is forking often enough to matter.
    void parallel_for(std::size_t items, std::size_t grain, const RangeBody& body);

    // Chunks executed and chunks taken from another deque. The ratio is the
    // only evidence that the stealing is doing anything, so it is counted
    // rather than assumed.
    [[nodiscard]] std::uint64_t chunks_run() const;
    [[nodiscard]] std::uint64_t steals() const;
    [[nodiscard]] std::uint64_t inline_runs() const;

private:
    WorkStealingPool();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// What the completion thread is told about one submitted frame.
//
// A ticket and not a callback, because this crosses an SpscRing and the ring
// takes trivially copyable elements only. The handler resolves the ticket
// against whatever the caller keyed its frames by.
using CompletionHandler = std::function<Status(std::uint64_t timeline_value,
                                               std::uint32_t ticket)>;

struct SchedulerConfig {
    // 0 derives from hardware_concurrency less reserved_threads.
    std::size_t worker_threads = 0;

    // The source thread and the completion thread. Named rather than baked in
    // so an integrator adding an audio egress thread can say so here.
    std::size_t reserved_threads = 2;

    // Frames that may be awaiting completion at once. Must exceed the graph's
    // frames in flight or the recording thread stalls on a full queue rather
    // than on the GPU.
    std::size_t completion_capacity = 16;

    // A generous but finite wait. An infinite wait on a hung GPU takes the
    // process down with it and reports nothing, which is strictly worse than a
    // timeout that names the stage.
    std::uint64_t wait_timeout_ns = 30ULL * 1000ULL * 1000ULL * 1000ULL;
};

struct SchedulerStats {
    std::uint64_t completions_posted = 0;
    std::uint64_t completions_finished = 0;

    // The number that matters for the architecture's central claim. One per
    // block: the single wait for the audio readback. Anything above the block
    // count means a stage is round-tripping the host.
    std::uint64_t host_waits = 0;

    std::uint64_t chunks_run = 0;
    std::uint64_t steals = 0;
    std::uint64_t inline_runs = 0;
};

// The pool plus the completion thread, and the counter that proves how many
// times the host waited on the device.
class Scheduler {
public:
    [[nodiscard]] static Expected<std::unique_ptr<Scheduler>> create(
        const gpu::Context& context, const SchedulerConfig& config);

    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    [[nodiscard]] WorkStealingPool& pool();

    // Starts the completion thread against one timeline semaphore. Called
    // once, before any post.
    [[nodiscard]] Status start(VkSemaphore timeline, CompletionHandler handler);

    // Queues a frame for completion. The recording thread only: this is the
    // producer half of an SPSC ring and a second producer corrupts it.
    //
    // Blocks only when the queue is full, which means the GPU is more than
    // completion_capacity frames behind and the caller has nothing useful to
    // do anyway.
    [[nodiscard]] Status post(std::uint64_t timeline_value, std::uint32_t ticket);

    // Returns once every posted frame has completed and its handler has run.
    [[nodiscard]] Status drain();

    // Wakes the completion thread and joins it. Idempotent.
    void stop();

    [[nodiscard]] SchedulerStats stats() const;

    // The first error a handler or a semaphore wait produced, if any. The
    // completion thread carries on after one so that a single bad frame does
    // not wedge the ring's retirement, and the error surfaces here.
    //
    // A handler that THROWS is caught and reported the same way, which it
    // was not until 2026-09-20. The handler ends up calling every audio,
    // passband and spectrum sink an integrator attached, this thread is a
    // std::thread, and an exception leaving its callable is std::terminate:
    // the sentence above was a promise the code could not keep for the one
    // failure mode a caller's code is most likely to produce.
    // core/engine/audio_wasapi.cpp has always caught on its own render
    // thread, which runs nothing a caller supplied, and this one did not.
    [[nodiscard]] bool failed() const;
    [[nodiscard]] Error error() const;

private:
    Scheduler();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace revenant::engine
