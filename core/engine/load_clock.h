// Where the host's time goes, counted rather than guessed.
//
// Added 2026-09-23 for the contention measurement under "Threading" in
// docs/rpc.md.
// The owner reported the engine chugging and P25 voice breaking up with
// signal identification busy, and the first question that report raises is
// which thread was doing what, and which waited on which. A per-thread CPU
// figure answers the first half. These answer the second: time spent inside
// each kind of sink on the completion thread, time the completion thread
// spent waiting on the GPU, and time the recording thread spent waiting for
// the completion thread to hand a frame slot back.
//
// A steady-clock read is about 20 ns on this machine against sinks that run
// for microseconds to milliseconds, so the counters cost nothing measurable.
// They are host bookkeeping and not a DSP function: nothing here feeds a
// sample, and docs/conventions.md's rule against reading a clock is about
// the arithmetic, which stays pure.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace revenant::engine {

[[nodiscard]] inline std::uint64_t load_clock_ns()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Adds the time between construction and destruction to a counter, and
// optionally raises a maximum. Relaxed, because every reader wants a total
// and nothing is published through it.
class LoadTimer {
public:
    explicit LoadTimer(std::atomic<std::uint64_t>& total,
                       std::atomic<std::uint64_t>* maximum = nullptr)
        : total_(total), maximum_(maximum), began_(load_clock_ns())
    {
    }

    ~LoadTimer()
    {
        const std::uint64_t spent = load_clock_ns() - began_;
        total_.fetch_add(spent, std::memory_order_relaxed);
        if (maximum_ != nullptr) {
            std::uint64_t seen = maximum_->load(std::memory_order_relaxed);
            while (spent > seen &&
                   !maximum_->compare_exchange_weak(seen, spent, std::memory_order_relaxed)) {
            }
        }
    }

    LoadTimer(const LoadTimer&) = delete;
    LoadTimer& operator=(const LoadTimer&) = delete;
    LoadTimer(LoadTimer&&) = delete;
    LoadTimer& operator=(LoadTimer&&) = delete;

private:
    std::atomic<std::uint64_t>& total_;
    std::atomic<std::uint64_t>* maximum_;
    std::uint64_t began_;
};

}  // namespace revenant::engine
