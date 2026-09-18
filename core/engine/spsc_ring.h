// A single-producer single-consumer lock-free ring of trivially copyable
// elements.
//
// This is what carries audio PCM from the readback thread to the audio egress
// thread. An underrun here is a dropout the listener hears, so it is treated
// as a correctness structure and not as a convenience: every memory ordering
// below is stated with the reason it is where it is, and every way a sample
// can go missing is counted rather than logged.
//
// docs/conventions.md requires four things at the declaration of any ring in
// this engine. For the audio case they are:
//
//   writer     the readback thread, one only. It memcpys out of the mapped
//              Readback buffer after the ring timeline value for that frame
//              is observed complete.
//   reader     the audio thread, one only. MMCSS "Pro Audio" priority, never
//              allocates, never locks, never waits on a fence.
//   ownership  the writer owns [head, head + n) from the moment it computes
//              the free count until its release store of head. The reader
//              owns [tail, tail + n) from its acquire load of head until its
//              release store of tail. Never both, and neither holds anything
//              between calls.
//   behind     a reader finding less than it asked for is an UNDERRUN. It
//              fills the shortfall with silence and counts it; the M1
//              acceptance test asserts the count is zero over sixty seconds
//              at fifty receivers. A writer finding no room counts a drop.
//
// The capacity is a power of two for the same reason the device ring's is:
// index-to-offset is one AND rather than a 64-bit modulo. The cursors
// themselves are free-running uint64 counters and are never wrapped, so
// head - tail is the occupancy with no ambiguity between full and empty and
// no reserved slot needed to tell them apart.

#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <new>
#include <span>
#include <type_traits>

#include "core/error.h"

namespace revenant::engine {

// A ceiling, not a device limit. 2^30 elements is 4 GiB of float, about six
// hours of mono audio at 48 kHz, and a request past it is an arithmetic
// mistake in the caller rather than a buffer anyone wants.
inline constexpr std::size_t kMaxSpscCapacity = std::size_t{1} << 30;

template <class T>
class SpscRing {
public:
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscRing moves elements with memcpy, so T must be trivially copyable");
    static_assert(std::is_trivially_default_constructible_v<T>,
                  "SpscRing zero-fills its storage at construction rather than running "
                  "constructors, so T must be trivially default constructible");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "the cursors are read from the audio thread and must not take a lock");

    // Rounds the request up to a power of two.
    //
    // Returns a unique_ptr rather than the ring by value, and the ring itself
    // is neither copyable nor movable. Two threads hold a pointer to this
    // object; a Vrx sitting in a std::vector that reallocates would otherwise
    // move the ring out from under the audio thread, which is a use-after-move
    // that reproduces once a week under load and never in a test.
    [[nodiscard]] static Expected<std::unique_ptr<SpscRing>> create(std::size_t capacity_hint) {
        if (capacity_hint == 0) {
            return fail("SpscRing::create with a capacity of zero");
        }
        if (capacity_hint > kMaxSpscCapacity) {
            return fail(std::format("SpscRing::create asked for {} elements, past the {} ceiling",
                                    capacity_hint, kMaxSpscCapacity));
        }

        const std::size_t capacity = std::bit_ceil(capacity_hint);

        auto* raw = new (std::nothrow) SpscRing();
        if (raw == nullptr) {
            return fail("SpscRing::create could not allocate the ring object");
        }
        std::unique_ptr<SpscRing> ring(raw);

        // Value-initialised, so a reader that races ahead of the first write
        // sees silence rather than whatever the allocator handed back.
        auto* storage = new (std::nothrow) T[capacity]();
        if (storage == nullptr) {
            return fail(std::format("SpscRing::create could not allocate {} elements of {} bytes",
                                    capacity, sizeof(T)));
        }
        ring->shared_.storage.reset(storage);
        ring->shared_.capacity = capacity;
        ring->shared_.mask = capacity - 1;
        return ring;
    }

    ~SpscRing() = default;

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&) = delete;
    SpscRing& operator=(SpscRing&&) = delete;

    [[nodiscard]] std::size_t capacity() const { return shared_.capacity; }

    // --- the writer, one thread only ---------------------------------------

    // Copies what fits and returns how many elements that was.
    //
    // A short return is not a loss and is not counted as one. It is the
    // signal to a caller that can retry, which is what a writer holding the
    // remainder does. Only a caller that discards the remainder has lost
    // anything, and only that caller knows it did, which is why the counting
    // lives in write_or_drop below and not here. Counting it here reports a
    // drop every time a retrying writer fills the ring, which is the normal
    // steady state of a faster-than-realtime chain.
    std::size_t write(std::span<const T> items) {
        if (items.empty()) {
            return 0;
        }

        // Relaxed: this thread is the only writer of head, so its own load
        // cannot be stale.
        const std::uint64_t head = head_.value.load(std::memory_order_relaxed);

        // Acquire, pairing with the reader's release store of tail. It is what
        // guarantees the reader has finished copying out of the slots this
        // call is about to overwrite. Relaxed here would let the memcpy below
        // be reordered ahead of the reader's last read of the same bytes.
        const std::uint64_t tail = tail_.value.load(std::memory_order_acquire);

        const auto used = static_cast<std::size_t>(head - tail);
        const std::size_t room = shared_.capacity - used;
        const std::size_t count = std::min(items.size(), room);
        if (count == 0) {
            return 0;
        }

        copy_in(head, items.data(), count);

        // Release: every byte written by copy_in happens before a reader
        // observes the new head. This is the store that publishes the audio.
        head_.value.store(head + count, std::memory_order_release);
        return count;
    }

    // What the readback thread calls. It is holding a frame that came out of
    // a mapped Vulkan buffer and has nowhere else to put the remainder, so
    // whatever does not fit is lost and is counted here.
    //
    // DIVERGENCE FROM THE DESIGN, deliberately, and stated rather than
    // buried. The design says a full writer drops the OLDEST element, on the
    // grounds that stale audio is worse than skipped audio. Dropping the
    // oldest means the writer advances the read cursor, and then two threads
    // are moving it: the reader can be part-way through copying a region the
    // writer then overwrites, which hands the mixer a buffer stitched from
    // two eras instead of a counted gap. That is worse than either drop
    // policy. The latency argument behind the design's choice is real, so the
    // reader gets trim_to() below, which discards the oldest from the side
    // that owns the read cursor and therefore cannot tear anything.
    std::size_t write_or_drop(std::span<const T> items) {
        const std::size_t wrote = write(items);
        if (wrote < items.size()) {
            note_drop(items.size() - wrote);
        }
        return wrote;
    }

    // Room for the writer right now. A lower bound by the time it is read,
    // never an upper one, because only the reader frees space.
    [[nodiscard]] std::size_t writable() const {
        const std::uint64_t head = head_.value.load(std::memory_order_acquire);
        const std::uint64_t tail = tail_.value.load(std::memory_order_acquire);
        return shared_.capacity - static_cast<std::size_t>(head - tail);
    }

    // --- the reader, one thread only ---------------------------------------

    // Copies out up to out.size() elements and returns how many there were.
    std::size_t read(std::span<T> out) {
        if (out.empty()) {
            return 0;
        }

        // Relaxed: this thread is the only writer of tail.
        const std::uint64_t tail = tail_.value.load(std::memory_order_relaxed);

        // Acquire, pairing with the writer's release store of head. This is
        // what makes the samples visible; without it the memcpy below can read
        // bytes the writer has not published.
        const std::uint64_t head = head_.value.load(std::memory_order_acquire);

        const auto ready = static_cast<std::size_t>(head - tail);
        const std::size_t count = std::min(out.size(), ready);
        if (count == 0) {
            return 0;
        }

        copy_out(tail, out.data(), count);

        // Release: the copy above happens before the writer observes the new
        // tail and starts overwriting those slots.
        tail_.value.store(tail + count, std::memory_order_release);
        return count;
    }

    // What the audio thread calls. Fills the shortfall with silence, counts
    // the underrun, and returns how many real elements it got.
    //
    // An underrun is not recoverable and not maskable: the listener has
    // already heard it. Counting it is the only useful response, and the
    // acceptance test asserts the count stays at zero.
    std::size_t read_or_fill(std::span<T> out, T fill = T{}) {
        const std::size_t got = read(out);
        if (got < out.size()) {
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(got), out.end(), fill);
            shared_.underrun_events.fetch_add(1, std::memory_order_relaxed);
            shared_.underrun_samples.fetch_add(out.size() - got, std::memory_order_relaxed);
        }
        return got;
    }

    // Discards up to `count` of the oldest elements. Reader-side, so the read
    // cursor still has exactly one owner.
    std::size_t skip(std::size_t count) {
        if (count == 0) {
            return 0;
        }
        const std::uint64_t tail = tail_.value.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.value.load(std::memory_order_acquire);
        const auto ready = static_cast<std::size_t>(head - tail);
        const std::size_t dropped = std::min(count, ready);
        if (dropped == 0) {
            return 0;
        }
        tail_.value.store(tail + dropped, std::memory_order_release);
        note_drop(dropped);
        return dropped;
    }

    // Caps the backlog, discarding the oldest. This is the race-free form of
    // the design's drop-the-oldest policy: a ring that has been allowed to
    // fill is holding audio the listener will hear late, and trimming it from
    // the reader costs one release store and cannot tear anything, where
    // trimming it from the writer would.
    std::size_t trim_to(std::size_t max_backlog) {
        const std::size_t ready = readable();
        return ready > max_backlog ? skip(ready - max_backlog) : 0;
    }

    [[nodiscard]] std::size_t readable() const {
        const std::uint64_t tail = tail_.value.load(std::memory_order_acquire);
        const std::uint64_t head = head_.value.load(std::memory_order_acquire);
        return static_cast<std::size_t>(head - tail);
    }

    // --- counters, readable from any thread ---------------------------------

    [[nodiscard]] std::uint64_t dropped_samples() const {
        return shared_.dropped_samples.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t dropped_events() const {
        return shared_.dropped_events.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t underrun_samples() const {
        return shared_.underrun_samples.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t underrun_events() const {
        return shared_.underrun_events.load(std::memory_order_relaxed);
    }

    // Total elements the writer has ever published and the reader has ever
    // consumed. Free-running, and their difference is the occupancy, so a
    // test can assert nothing was lost or duplicated by comparing them against
    // its own totals.
    [[nodiscard]] std::uint64_t total_written() const {
        return head_.value.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t total_read() const {
        return tail_.value.load(std::memory_order_acquire);
    }

private:
    SpscRing() = default;

    void copy_in(std::uint64_t head, const T* source, std::size_t count) {
        const auto offset = static_cast<std::size_t>(head & shared_.mask);
        const std::size_t first = std::min(count, shared_.capacity - offset);
        std::memcpy(shared_.storage.get() + offset, source, first * sizeof(T));
        if (count > first) {
            // The wrap. Two copies because the destination is contiguous
            // memory and the window is not, which is the same reason the
            // device ring's host writer splits a straddling vkCmdCopyBuffer.
            std::memcpy(shared_.storage.get(), source + first, (count - first) * sizeof(T));
        }
    }

    void copy_out(std::uint64_t tail, T* destination, std::size_t count) const {
        const auto offset = static_cast<std::size_t>(tail & shared_.mask);
        const std::size_t first = std::min(count, shared_.capacity - offset);
        std::memcpy(destination, shared_.storage.get() + offset, first * sizeof(T));
        if (count > first) {
            std::memcpy(destination + first, shared_.storage.get(), (count - first) * sizeof(T));
        }
    }

    void note_drop(std::size_t count) {
        shared_.dropped_events.fetch_add(1, std::memory_order_relaxed);
        shared_.dropped_samples.fetch_add(count, std::memory_order_relaxed);
    }

    // One cache line each. The producer writes head every call and the
    // consumer writes tail every call; sharing a line between them costs a
    // coherence round trip per sample block on a structure whose entire job is
    // to be cheap. Each block is sized to exactly 64 bytes so the alignment
    // specifier introduces no padding of its own, which would be C4324 and so
    // an error under /WX.
    struct alignas(64) Cursor {
        std::atomic<std::uint64_t> value{0};
        std::uint64_t pad_[7]{};
    };

    // Written once at construction, plus the counters, which only move on the
    // abnormal paths. Keeping them together off the two hot lines is what
    // matters; that they share a line with each other does not, because a
    // drop and an underrun are the two things that are not supposed to happen.
    struct alignas(64) Shared {
        std::unique_ptr<T[]> storage;
        std::size_t capacity = 0;
        std::uint64_t mask = 0;
        std::atomic<std::uint64_t> dropped_samples{0};
        std::atomic<std::uint64_t> dropped_events{0};
        std::atomic<std::uint64_t> underrun_samples{0};
        std::atomic<std::uint64_t> underrun_events{0};
        std::uint64_t pad_[1]{};
    };

    static_assert(sizeof(Cursor) == 64, "a cursor occupies exactly one cache line");
    static_assert(sizeof(Shared) == 64, "the cold block occupies exactly one cache line");

    Shared shared_;
    Cursor head_;
    Cursor tail_;
};

}  // namespace revenant::engine
