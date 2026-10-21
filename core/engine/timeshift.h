// The time-shift buffer: the last few minutes of the wideband source, kept in
// host memory so a receiver can be opened on a signal that has already ended.
//
// WHAT IS KEPT, AND WHY IT IS THE SOURCE'S OWN BYTES
//
// Blocks are copied as the source delivered them, in its native format, before
// the graph converts anything. A cu8 dongle costs two bytes a sample here where
// Complex32 would cost eight, which is the difference between two minutes of
// 2.4 MS/s fitting in 576 MB and needing 2.3 GB. It also means a replay feeds a
// graph exactly the bytes the live one saw, so nothing about the replay path
// needs a second convert stage or a second set of rules.
//
// The size is the smaller of two settings: seconds wanted, and a byte budget.
// The budget wins on a wide source, and extent() says how much time that bought
// rather than the caller inferring it.
//
// ONE CENTRE AT A TIME
//
// A retune clears the buffer. Samples taken at another centre would place a
// replay receiver on the wrong frequency with nothing to say so, and keeping a
// table of centres per span is a feature nobody has asked for. A stream that
// restarts (its index going backwards) clears it for the same reason.
//
// THREADS
//
// push() is the delivery thread's; everything else is any thread. One mutex,
// held for a memcpy of one block on either side. The delivery thread never
// waits on anything else here, and a reader holds the lock for exactly one
// block's copy, so the cost to the sample path is bounded by a block.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

#include "core/dsp/types.h"
#include "core/source/capabilities.h"
#include "core/source/source.h"

namespace revenant::engine {

// The two settings. Both bound the buffer and the smaller wins.
struct TimeShiftLimits {
    double seconds = 120.0;
    std::uint64_t budget_bytes = 2ULL << 30;
};

// What the buffer holds now, and what a replay is doing with it.
struct TimeShiftExtent {
    // False when nothing is held: no stream yet, limits of zero, or the
    // allocation was refused. `note` says which when it is not obvious.
    bool available = false;

    // Absolute source indices, [oldest_index, newest_index), and their wall
    // clock in Unix epoch nanoseconds, [oldest_ns, newest_ns).
    dsp::SampleIndex oldest_index = 0;
    dsp::SampleIndex newest_index = 0;
    std::int64_t oldest_ns = 0;
    std::int64_t newest_ns = 0;

    dsp::SampleRate rate = 0;
    dsp::Hertz center = 0;

    // What the limits bought at this rate and format.
    double capacity_seconds = 0.0;
    std::uint64_t capacity_bytes = 0;

    // The replay, when one runs. replay_ns is the wall clock of the sample the
    // replay graph is being fed now.
    bool replaying = false;
    std::int64_t replay_ns = 0;
    double replay_speed = 0.0;

    TimeShiftLimits limits{};
};

// Seconds and bytes both clamped to what the buffer can mean: a negative or
// non-finite seconds is zero, and the result is the capacity in samples.
[[nodiscard]] dsp::SampleIndex timeshift_capacity_samples(const TimeShiftLimits& limits,
                                                          dsp::SampleRate rate,
                                                          source::SampleFormat format);

class TimeShiftBuffer {
public:
    TimeShiftBuffer() = default;
    TimeShiftBuffer(const TimeShiftBuffer&) = delete;
    TimeShiftBuffer& operator=(const TimeShiftBuffer&) = delete;

    // Takes effect at the next begin_stream, or at once when a stream is
    // already being kept: the buffer is reallocated and starts empty.
    void set_limits(const TimeShiftLimits& limits);
    [[nodiscard]] TimeShiftLimits limits() const;

    // A stream is starting at this rate and format. Allocates the buffer, or
    // keeps the one held when it is already the right size, and empties it.
    void begin_stream(dsp::SampleRate rate, source::SampleFormat format);

    // Drops everything held and releases the memory.
    void release();

    // Empties the buffer and keeps the memory.
    void clear();

    // The delivery thread. `center` is the absolute centre the block was taken
    // at; a change from the last block's clears the buffer first. A gap in the
    // stream's index is filled with zeros so index arithmetic stays exact.
    void push(const source::SourceBlock& block, dsp::Hertz center);

    [[nodiscard]] TimeShiftExtent extent() const;

    // Copies samples [start, start + count) into `out`, which holds at least
    // count * bytes_per_sample bytes. Returns the samples copied, which is
    // fewer than asked when the range runs past the newest sample and zero
    // when `start` has already been overwritten or not yet written.
    [[nodiscard]] std::size_t read(dsp::SampleIndex start, std::size_t count,
                                   std::span<std::byte> out) const;

    // The index of the sample taken at or just after `wall_ns`, clamped into
    // what is held. Zero when nothing is.
    [[nodiscard]] dsp::SampleIndex index_at(std::int64_t wall_ns) const;

    // The wall clock of an absolute index on the current stream.
    [[nodiscard]] std::int64_t wall_ns_at(dsp::SampleIndex index) const;

    [[nodiscard]] source::SampleFormat format() const;
    [[nodiscard]] dsp::SampleRate rate() const;

private:
    void allocate_locked();
    void write_locked(dsp::SampleIndex index, std::span<const std::byte> bytes);
    [[nodiscard]] std::int64_t wall_ns_locked(dsp::SampleIndex index) const;

    mutable std::mutex lock_;
    TimeShiftLimits limits_{};
    dsp::SampleRate rate_ = 0;
    source::SampleFormat format_ = source::SampleFormat::Cf32;
    std::size_t bytes_per_sample_ = 0;

    std::unique_ptr<std::byte[]> storage_;
    dsp::SampleIndex capacity_ = 0;
    bool allocation_failed_ = false;

    // Valid when held_ is true. newest_ is one past the last sample written.
    bool held_ = false;
    dsp::SampleIndex oldest_ = 0;
    dsp::SampleIndex newest_ = 0;
    std::int64_t anchor_ns_ = 0;
    dsp::Hertz center_ = 0;
};

}  // namespace revenant::engine
