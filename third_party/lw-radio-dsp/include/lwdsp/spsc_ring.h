// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Locke Werks
//
// Single-producer single-consumer lock-free ring of trivially copyable
// elements, for handing receiver audio from the DSP thread to the audio
// callback. Ported from Revenant core/engine/spsc_ring.h with its error type
// replaced by a null return.
//
// Writer owns [head, head + n) from computing the free count until its
// release store of head; reader owns [tail, tail + n) from its acquire load
// of head until its release store of tail. Cursors are free-running uint64,
// so head - tail is the occupancy with no reserved slot. A reader finding
// less than it asked for is an underrun: it fills silence and counts it.
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <type_traits>

namespace lwdsp {

inline constexpr std::size_t kMaxSpscCapacity = std::size_t{1} << 30;

template <class T>
class SpscRing {
public:
    static_assert(std::is_trivially_copyable_v<T>, "elements move by memcpy");
    static_assert(std::is_trivially_default_constructible_v<T>, "storage is zero filled");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "cursors are read from the audio thread and must not lock");

    // Capacity rounds up to a power of two so index-to-offset is one AND.
    // Null on a zero or oversized request or allocation failure.
    [[nodiscard]] static std::unique_ptr<SpscRing> create(std::size_t capacity_hint) {
        if (capacity_hint == 0 || capacity_hint > kMaxSpscCapacity) return nullptr;
        const std::size_t capacity = std::bit_ceil(capacity_hint);
        std::unique_ptr<SpscRing> ring(new (std::nothrow) SpscRing());
        if (!ring) return nullptr;
        ring->storage_.reset(new (std::nothrow) T[capacity]());
        if (!ring->storage_) return nullptr;
        ring->capacity_ = capacity;
        ring->mask_ = capacity - 1;
        return ring;
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    [[nodiscard]] std::size_t capacity() const { return capacity_; }

    // Producer side. Returns how many were written.
    std::size_t write(std::span<const T> items) {
        if (items.empty()) return 0;
        const std::uint64_t head = head_.value.load(std::memory_order_relaxed);
        // Acquire pairs with the reader's release of tail, so the slots it
        // freed are really done being read before they are overwritten.
        const std::uint64_t tail = tail_.value.load(std::memory_order_acquire);
        const std::size_t room = capacity_ - static_cast<std::size_t>(head - tail);
        const std::size_t count = std::min(items.size(), room);
        if (count == 0) return 0;
        copy_in(head, items.data(), count);
        head_.value.store(head + count, std::memory_order_release);
        return count;
    }

    std::size_t write_or_drop(std::span<const T> items) {
        const std::size_t wrote = write(items);
        if (wrote < items.size()) note_drop(items.size() - wrote);
        return wrote;
    }

    [[nodiscard]] std::size_t writable() const {
        return capacity_ - static_cast<std::size_t>(head_.value.load(std::memory_order_acquire) -
                                                    tail_.value.load(std::memory_order_acquire));
    }

    // Consumer side.
    std::size_t read(std::span<T> out) {
        if (out.empty()) return 0;
        const std::uint64_t tail = tail_.value.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.value.load(std::memory_order_acquire);
        const std::size_t count = std::min(out.size(), static_cast<std::size_t>(head - tail));
        if (count == 0) return 0;
        copy_out(tail, out.data(), count);
        tail_.value.store(tail + count, std::memory_order_release);
        return count;
    }

    std::size_t read_or_fill(std::span<T> out, T fill = T{}) {
        const std::size_t got = read(out);
        if (got < out.size()) {
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(got), out.end(), fill);
            underrun_events_.fetch_add(1, std::memory_order_relaxed);
            underrun_samples_.fetch_add(out.size() - got, std::memory_order_relaxed);
        }
        return got;
    }

    // Consumer drops the oldest count elements to cap latency.
    std::size_t skip(std::size_t count) {
        const std::uint64_t tail = tail_.value.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.value.load(std::memory_order_acquire);
        const std::size_t dropped = std::min(count, static_cast<std::size_t>(head - tail));
        if (dropped == 0) return 0;
        tail_.value.store(tail + dropped, std::memory_order_release);
        note_drop(dropped);
        return dropped;
    }

    std::size_t trim_to(std::size_t max_backlog) {
        const std::size_t ready = readable();
        return ready > max_backlog ? skip(ready - max_backlog) : 0;
    }

    [[nodiscard]] std::size_t readable() const {
        return static_cast<std::size_t>(head_.value.load(std::memory_order_acquire) -
                                        tail_.value.load(std::memory_order_acquire));
    }

    [[nodiscard]] std::uint64_t dropped_samples() const { return dropped_samples_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t dropped_events() const { return dropped_events_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t underrun_samples() const { return underrun_samples_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t underrun_events() const { return underrun_events_.load(std::memory_order_relaxed); }

private:
    SpscRing() = default;

    void copy_in(std::uint64_t head, const T* src, std::size_t count) {
        const auto off = static_cast<std::size_t>(head & mask_);
        const std::size_t first = std::min(count, capacity_ - off);
        std::memcpy(storage_.get() + off, src, first * sizeof(T));
        if (count > first) std::memcpy(storage_.get(), src + first, (count - first) * sizeof(T));
    }

    void copy_out(std::uint64_t tail, T* dst, std::size_t count) const {
        const auto off = static_cast<std::size_t>(tail & mask_);
        const std::size_t first = std::min(count, capacity_ - off);
        std::memcpy(dst, storage_.get() + off, first * sizeof(T));
        if (count > first) std::memcpy(dst + first, storage_.get(), (count - first) * sizeof(T));
    }

    void note_drop(std::size_t count) {
        dropped_events_.fetch_add(1, std::memory_order_relaxed);
        dropped_samples_.fetch_add(count, std::memory_order_relaxed);
    }

    // Each cursor on its own cache line so producer and consumer do not
    // false-share.
    struct alignas(64) Cursor {
        std::atomic<std::uint64_t> value{0};
    };

    std::unique_ptr<T[]> storage_;
    std::size_t capacity_ = 0;
    std::uint64_t mask_ = 0;
    std::atomic<std::uint64_t> dropped_samples_{0};
    std::atomic<std::uint64_t> dropped_events_{0};
    std::atomic<std::uint64_t> underrun_samples_{0};
    std::atomic<std::uint64_t> underrun_events_{0};
    Cursor head_;
    Cursor tail_;
};

} // namespace lwdsp
