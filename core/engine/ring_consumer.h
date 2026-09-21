// The device ring's companion header: the parts of the ring that the frozen
// contract in core/engine/device_ring.h declares no home for.
//
// device_ring.h fixes the shape a consumer sees (ConsumerKind, RingConsumer,
// ConsumerCursor, claim and retire) and nothing else. What has to exist
// around that shape lives here, because none of it can be added to that
// header without editing a committed contract that three other work packages
// compile against:
//
//   plan_ring_geometry
//                   the sizing arithmetic, taking a device's limits as plain
//                   integers. Separated from DeviceRing::create so that both
//                   GPUs' real limits can be fed through it without a GPU
//                   present, which is the only way the clamp that fires on one
//                   CI leg gets tested on the other.
//   ConsumerTable   the lock-free cursor table DeviceRing is built out of, and
//                   the single place the retirement floor is computed.
//   ScopedClaim     a claim that retires when it leaves scope, so a consumer
//                   that returns early between claim and retire cannot stall
//                   the writer for the rest of the process's life.
//
// Nothing here takes a lock. The cursors are atomics, the blocking producer
// parks on std::atomic::wait, which is WaitOnAddress on Windows, and every
// ordering below is stated with the reason it is what it is rather than being
// raised to seq_cst because that is easier to defend.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "core/dsp/types.h"
#include "core/engine/device_ring.h"
#include "core/error.h"
#include "core/gpu/context.h"
#include "core/source/capabilities.h"

namespace revenant::engine {

// One canonical element type. The conversion from a source's native width
// happens on the device during upload, so the bus carries native width exactly
// once and everything downstream of the ring agrees on the format.
inline constexpr std::uint64_t kRingSampleBytes = sizeof(dsp::Complex32);

// Below this the ring is not a ring, it is a scratch buffer, and the sizing
// arithmetic stops meaning anything. 1024 samples is 8 KiB. A request smaller
// than this is raised rather than refused, and the raise is not a clamp
// because nothing was taken away.
inline constexpr dsp::SampleIndex kMinRingCapacitySamples = 1024;

// DeviceRing::offset_of returns uint32_t, so the mask must fit in 32 bits.
// 2^31 samples is 16 GiB, three orders of magnitude past what any device in
// the matrix will allocate, so this bound never fires in practice. It is here
// so that if a device ever does allow it, the failure is a reported clamp and
// not a silently truncated offset.
inline constexpr dsp::SampleIndex kMaxRingCapacitySamples = dsp::SampleIndex{1} << 31;

// How many consumers one ring will carry. The channelizer is one consumer for
// every VRX in the machine, the full-span FFT is a second, and M5's disk
// writer is a third, so the real number is single digits. Sixty-four slots is
// 4 KiB of cursors and removes any need to grow the table while the sample
// path is running.
inline constexpr std::uint32_t kMaxRingConsumers = 64;

// ---------------------------------------------------------------------------
// THE KERNEL SEAM THAT WAS HERE, AND WHY IT IS GONE
// ---------------------------------------------------------------------------
//
// This section declared `RingWindow`, called "the push-constant block every
// consumer kernel takes" and "the seam with the channelizer", and `ReadLease`
// around it. It was removed on 2026-09-20 because no kernel ever took it and
// none could.
//
// RingWindow's first two members were uint64. GLSL has no 64-bit integer
// without GL_ARB_gpu_shader_int64, which nothing in core/shaders enables, so
// a shader declaring that block would not compile against this tree. The
// channelizer the comment named settled the question in the other direction
// and wrote down why: core/shaders/pfb_branch.comp takes `uint ring_mask` and
// `uint base_offset` and says "this kernel never sees an absolute sample
// index ... the host owns the index and hands down a 32-bit ring offset. Pass
// a truncated absolute index instead and it works for three and a half
// minutes at 20 MS/s and then does not." core/engine/graph.cpp does exactly
// that, in dsp::ConvertParams, dsp::PfbBranchParams and every other push
// block it fills.
//
// A frozen contract nothing implements is worse than no contract: the next
// consumer kernel would have been written against a 24-byte block that has
// never once crossed to a device. What the real seam looks like is in
// core/dsp, one params struct per kernel, each asserted against its own
// shader.
//
// ---------------------------------------------------------------------------
// Sizing
// ---------------------------------------------------------------------------

// A device's buffer limits, as plain integers, so the sizing can be tested
// against numbers read off a machine that is not the one running the test.
//
// A zero means "not reported", and an unreported limit does not bind. Every
// device in the matrix reports both.
struct RingLimits {
    std::uint64_t max_memory_allocation_size = 0;
    std::uint64_t max_storage_buffer_range = 0;
};

[[nodiscard]] inline RingLimits limits_of(const gpu::DeviceInfo& info) {
    RingLimits limits;
    limits.max_memory_allocation_size = info.max_memory_allocation_size;
    limits.max_storage_buffer_range = info.max_storage_buffer_range;
    return limits;
}

// Works out the largest power-of-two capacity that fits, and says what stopped
// it being larger.
//
// The device's shared-memory limit (DeviceInfo::max_workgroup_shared_memory)
// deliberately does not appear here. It bounds how much of a transform a
// single workgroup can hold, which is the FFT stage's problem; the ring is an
// SSBO that consumer kernels stream through registers and never stage in
// shared memory, so it does not bind the ring's size in either direction.
//
// Thread safety: pure. Depends on nothing but its arguments.
[[nodiscard]] inline Expected<RingGeometry> plan_ring_geometry(const RingConfig& config,
                                                               const RingLimits& limits) {
    if (config.rate <= 0) {
        return fail(std::format(
            "a device ring needs a positive sample rate to report its retention, got {}",
            config.rate));
    }
    const auto rate = static_cast<double>(config.rate);

    dsp::SampleIndex requested = config.capacity_samples;
    if (requested == 0) {
        if (!(config.seconds_wanted > 0.0)) {
            return fail(std::format("a device ring needs either RingConfig::capacity_samples or a "
                                    "positive RingConfig::seconds_wanted, got {}",
                                    config.seconds_wanted));
        }
        const double wanted_samples = config.seconds_wanted * rate;

        // Convert through a bound rather than straight into uint64: the
        // conversion of a double larger than 2^64 is undefined, and a caller
        // asking for a century of retention is a caller making an arithmetic
        // mistake, not one that should get undefined behaviour for it.
        constexpr double kHighestExactlyRepresentable = 18446744073709549568.0;
        requested = wanted_samples >= kHighestExactlyRepresentable
                        ? std::numeric_limits<dsp::SampleIndex>::max()
                        : static_cast<dsp::SampleIndex>(wanted_samples);
    }
    requested = std::max(requested, kMinRingCapacitySamples);

    // Track which limit was the binding one, because "the ring is smaller than
    // you asked for" is not actionable and "maxMemoryAllocationSize of
    // 2147483648 bytes capped it" is.
    dsp::SampleIndex ceiling = requested;
    const char* bound_by = nullptr;
    std::uint64_t bound_value = 0;

    const auto apply = [&](const char* name, std::uint64_t limit_bytes) {
        if (limit_bytes == 0) {
            return;
        }
        const dsp::SampleIndex limit_samples = limit_bytes / kRingSampleBytes;
        if (limit_samples < ceiling) {
            ceiling = limit_samples;
            bound_by = name;
            bound_value = limit_bytes;
        }
    };

    // maxStorageBufferRange first because it is the one that binds on the
    // discrete card, where the allocation limit is 1 TiB and the SSBO binding
    // range is 4 GiB minus one. maxMemoryAllocationSize second because it is
    // the one that binds on the integrated part, where both are 2 GiB. Either
    // can win; the order only decides which name is reported when they tie,
    // and a tie means both are true.
    apply("maxStorageBufferRange", limits.max_storage_buffer_range);
    apply("maxMemoryAllocationSize", limits.max_memory_allocation_size);
    if (kMaxRingCapacitySamples < ceiling) {
        ceiling = kMaxRingCapacitySamples;
        bound_by = "the 32-bit ring offset";
        bound_value = kMaxRingCapacitySamples * kRingSampleBytes;
    }

    const dsp::SampleIndex capacity = std::bit_floor(ceiling);
    if (capacity < kMinRingCapacitySamples) {
        return fail(std::format(
            "the device caps a storage buffer at {} bytes, which is {} samples, below the "
            "{}-sample minimum a ring needs to be a ring",
            bound_value, ceiling, kMinRingCapacitySamples));
    }

    RingGeometry geometry;
    geometry.capacity_samples = capacity;
    geometry.capacity_mask = capacity - 1;
    geometry.bytes = capacity * kRingSampleBytes;
    geometry.seconds_retained = static_cast<double>(capacity) / rate;
    geometry.clamped = capacity < requested;

    if (geometry.clamped) {
        if (bound_by != nullptr && ceiling < requested) {
            geometry.clamp_reason = std::format(
                "{} of {} bytes caps this ring at {} samples, so the {} samples asked for became "
                "{} ({:.3f} s at {} S/s) after rounding down to a power of two",
                bound_by, bound_value, ceiling, requested, capacity, geometry.seconds_retained,
                config.rate);
        } else {
            geometry.clamp_reason = std::format(
                "{} samples asked for, rounded down to the power of two {} ({:.3f} s at {} S/s); "
                "the capacity is a power of two so a consumer kernel's index-to-offset is one AND "
                "per invocation instead of a 64-bit modulo",
                requested, capacity, geometry.seconds_retained, config.rate);
        }
    }
    return geometry;
}

// ---------------------------------------------------------------------------
// The cursor table
// ---------------------------------------------------------------------------

namespace detail {

// Raises a cursor to `target` and never lowers it. Both the owning consumer
// and the writer's force-advance path move a Lossy consumer's cursors, so the
// raise has to be a CAS loop rather than a store; making it monotone is what
// keeps two writers from fighting each other backwards.
inline dsp::SampleIndex raise_to(std::atomic<dsp::SampleIndex>& cursor, dsp::SampleIndex target,
                                 std::memory_order success) {
    auto current = cursor.load(std::memory_order_relaxed);
    while (current < target) {
        if (cursor.compare_exchange_weak(current, target, success, std::memory_order_relaxed)) {
            return target;
        }
    }
    return current;
}

[[nodiscard]] inline dsp::SampleIndex saturating_add(dsp::SampleIndex a, dsp::SampleIndex b) {
    const auto limit = std::numeric_limits<dsp::SampleIndex>::max();
    return a > limit - b ? limit : a + b;
}

}  // namespace detail

// What one registered consumer's cursors and counters look like from outside.
struct ConsumerReport {
    ConsumerKind kind = ConsumerKind::Lossy;
    ConsumerCursor cursor{};

    // Samples the writer skipped this consumer past because it had been
    // lapped. The hole is also visible as a jump in the index it is next
    // handed, which is the same gap encoding a source overrun uses: one gap
    // concept end to end, and nothing has to tell a loss from a seek by
    // subtracting two indices and guessing.
    std::uint64_t samples_skipped = 0;
    std::uint64_t forced_advances = 0;
};

// The retirement contract, implemented.
//
// THREAD SAFETY, per method, because a structure like this is unreviewable
// without it:
//
//   acquire_slot / release_slot   any thread, concurrently with anything.
//                                 Control plane, not the sample path.
//   claim / retire                the owning consumer's thread. Two threads
//                                 driving one handle is a caller bug, though
//                                 the monotone CAS keeps it from corrupting
//                                 the table.
//   reserve / reserve_blocking /
//   publish / note_dropped        the single producer thread only.
//   everything else               any thread.
//
// There is no mutex. reserve_blocking parks on std::atomic::wait, which is
// WaitOnAddress on Windows, so a blocked producer costs no spinning and the
// wake is a direct hand-off.
class ConsumerTable {
public:
    explicit ConsumerTable(dsp::SampleIndex capacity_samples) noexcept {
        producer_.capacity = capacity_samples;
    }

    ConsumerTable(const ConsumerTable&) = delete;
    ConsumerTable& operator=(const ConsumerTable&) = delete;
    ConsumerTable(ConsumerTable&&) = delete;
    ConsumerTable& operator=(ConsumerTable&&) = delete;

    [[nodiscard]] dsp::SampleIndex capacity() const { return producer_.capacity; }

    // --- registration -------------------------------------------------------

    // `start_at` is where this consumer's cursors begin. The caller passes the
    // current write cursor: a consumer that registers with a cursor more than
    // one capacity behind the writer is wedged from its first instant, and a
    // Blocking one would wedge the writer with it.
    [[nodiscard]] Expected<std::uint32_t> acquire_slot(ConsumerKind kind,
                                                       dsp::SampleIndex start_at) {
        for (std::uint32_t slot = 0; slot < kMaxRingConsumers; ++slot) {
            auto& entry = slots_[slot];
            std::uint32_t expected = kSlotFree;
            if (!entry.state.compare_exchange_strong(expected, kSlotBusy,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
                continue;
            }

            entry.claimed.store(start_at, std::memory_order_relaxed);
            entry.retired.store(start_at, std::memory_order_relaxed);
            entry.samples_skipped.store(0, std::memory_order_relaxed);
            entry.forced_advances.store(0, std::memory_order_relaxed);
            entry.kind.store(static_cast<std::uint32_t>(kind), std::memory_order_relaxed);
            const std::uint32_t generation =
                entry.generation.fetch_add(1, std::memory_order_relaxed) + 1;

            // Release: every store above must be visible to any thread that
            // later observes this slot as active. Without it the producer can
            // compute its floor from the previous registration's retired
            // cursor, which is the one number that must never read stale.
            entry.state.store(kSlotActive, std::memory_order_release);

            // A fresh Blocking consumer lowers the floor, never raises it, so
            // no producer needs waking. A fresh Lossy one changes nothing.
            return make_handle(slot, generation);
        }
        return fail(std::format("a device ring carries at most {} consumers and all {} slots are "
                                "in use",
                                kMaxRingConsumers, kMaxRingConsumers));
    }

    [[nodiscard]] Status release_slot(std::uint32_t handle) {
        auto* entry = resolve(handle);
        if (entry == nullptr) {
            return fail("the consumer handle is not registered on this ring");
        }

        // Release: the departing consumer's last retire must be visible before
        // the slot is seen free, or a producer racing the removal can read a
        // half-torn-down slot.
        entry->state.store(kSlotFree, std::memory_order_release);

        // Removing the slowest Blocking consumer raises the floor, and a
        // producer may be parked on exactly that. Waking it is not optional:
        // nothing else will ever bump the epoch if this was the last consumer.
        bump_retire_epoch();
        return {};
    }

    [[nodiscard]] bool handle_is_live(std::uint32_t handle) const {
        return resolve(handle) != nullptr;
    }

    [[nodiscard]] std::uint32_t active_consumers() const {
        std::uint32_t count = 0;
        for (const auto& entry : slots_) {
            if (entry.state.load(std::memory_order_acquire) == kSlotActive) {
                ++count;
            }
        }
        return count;
    }

    // --- the consumer side --------------------------------------------------

    // Moves this consumer's claim cursor up to `through`.
    //
    // Claiming is not a read barrier and grants nothing on its own: it says
    // "work up to here has been handed to me", which is what lets a report
    // distinguish a consumer that is behind from one that is stuck. What
    // protects the window is the retire cursor, which stays where it was until
    // the consumer says the work is finished with.
    [[nodiscard]] Status claim(std::uint32_t handle, dsp::SampleIndex through) {
        auto* entry = resolve(handle);
        if (entry == nullptr) {
            return fail("the consumer handle is not registered on this ring");
        }
        const auto head = producer_.write_cursor.load(std::memory_order_acquire);
        if (through > head) {
            return fail(std::format(
                "a consumer claimed through sample {} but only {} samples have been published",
                through, head));
        }
        detail::raise_to(entry->claimed, through, std::memory_order_acq_rel);
        return {};
    }

    // Releases everything below `through`.
    //
    // This is the moment the writer is allowed to overwrite those samples, so
    // it is the moment the GPU has finished with them and not the moment the
    // host finished recording the dispatch. Retiring on submission instead of
    // on completion is the bug this whole two-cursor arrangement exists to
    // make impossible: it overwrites samples an in-flight dispatch is still
    // reading, and the symptom is noise in one demodulated channel with
    // nothing anywhere in the logs.
    [[nodiscard]] Status retire(std::uint32_t handle, dsp::SampleIndex through) {
        auto* entry = resolve(handle);
        if (entry == nullptr) {
            return fail("the consumer handle is not registered on this ring");
        }
        const auto claimed = entry->claimed.load(std::memory_order_acquire);
        if (through > claimed) {
            return fail(std::format(
                "a consumer retired through sample {} having only claimed through {}", through,
                claimed));
        }

        // Release: everything this consumer did with the window happens before
        // the producer observes the retirement. The producer's acquire load in
        // blocking_floor() is the other half, and together they are what stops
        // the producer's copy from being reordered ahead of the consumer's
        // last read.
        detail::raise_to(entry->retired, through, std::memory_order_release);

        // Only a Blocking consumer gates the producer, so only a Blocking
        // consumer's retirement can be what a parked producer is waiting for.
        // Skipping the notify for a Lossy consumer keeps the common case free
        // of a needless WakeByAddressAll on every dispatch.
        if (entry->kind.load(std::memory_order_relaxed) ==
            static_cast<std::uint32_t>(ConsumerKind::Blocking)) {
            bump_retire_epoch();
        }
        return {};
    }

    [[nodiscard]] std::optional<ConsumerReport> report(std::uint32_t handle) const {
        const auto* entry = resolve(handle);
        if (entry == nullptr) {
            return std::nullopt;
        }
        ConsumerReport out;
        out.kind = static_cast<ConsumerKind>(
            static_cast<std::uint8_t>(entry->kind.load(std::memory_order_relaxed)));
        out.cursor.claimed = entry->claimed.load(std::memory_order_acquire);
        out.cursor.retired = entry->retired.load(std::memory_order_acquire);
        out.samples_skipped = entry->samples_skipped.load(std::memory_order_relaxed);
        out.forced_advances = entry->forced_advances.load(std::memory_order_relaxed);
        return out;
    }

    // --- the producer side --------------------------------------------------

    // The exclusive end of what the producer may write: the slowest Blocking
    // consumer's retired cursor plus one capacity.
    //
    // Lossy consumers do not appear. That is the definition of Lossy: the
    // writer advances regardless and the consumer is told afterwards. Putting
    // a Lossy consumer in the floor would turn a slow disk into lost radio
    // samples, which is the exact substitution the two kinds exist to prevent.
    [[nodiscard]] dsp::SampleIndex writable_limit() const {
        const auto floor_value = blocking_floor();
        if (!floor_value) {
            return std::numeric_limits<dsp::SampleIndex>::max();
        }
        return detail::saturating_add(*floor_value, producer_.capacity);
    }

    [[nodiscard]] std::uint64_t writable_now() const {
        const auto limit = writable_limit();
        const auto reserved = producer_.reserved.load(std::memory_order_relaxed);
        return limit > reserved ? limit - reserved : 0;
    }

    // How far the producer has taken the ring, published or not. The producer
    // needs two cursors for the same reason a consumer does, and the pair is
    // the mirror image: `reserved` is the window the producer has open right
    // now, `write_cursor` is what it has finished with.
    //
    // Without the pair, first_available() would name samples the producer is
    // in the middle of overwriting. Writing index i destroys index i minus one
    // capacity, and that destruction happens before the publish that would
    // have told anyone. A consumer sitting at the oldest edge would read a
    // window that reported itself present and was being demolished under it.
    [[nodiscard]] dsp::SampleIndex reserved_index() const {
        return producer_.reserved.load(std::memory_order_acquire);
    }

    // The Paced adapter. Never blocks, never waits, takes what there is room
    // for and returns how much that was. A short return is an overrun: a
    // radio's clock does not wait, so the samples the caller could not place
    // are gone, and the only question is whether anybody is told. The caller
    // records the shortfall with note_dropped.
    std::uint64_t reserve(std::uint64_t wanted) {
        if (wanted == 0) {
            return 0;
        }
        const auto granted = std::min(wanted, writable_now());
        if (granted > 0) {
            commit_reservation(granted);
        }
        return granted;
    }

    // The Demand adapter. Parks until the slowest Blocking consumer has
    // retired enough, which is the backpressure that makes a file source run
    // at exactly its consumer's rate. Returns false only when the ring has
    // been stopped, or when `wanted` is larger than the whole ring and no
    // amount of retiring could ever satisfy it.
    [[nodiscard]] bool reserve_blocking(std::uint64_t wanted) {
        if (wanted == 0) {
            return true;
        }
        if (wanted > producer_.capacity) {
            return false;
        }
        for (;;) {
            // Read the epoch BEFORE testing the limit. The other order loses a
            // retirement that lands between the test and the park, and the
            // producer then sleeps forever with room available. That is the
            // classic lost wakeup and it is not reproducible on demand, so it
            // gets designed out rather than tested for.
            const auto epoch = shared_.retire_epoch.load(std::memory_order_acquire);
            if (shared_.stopped.load(std::memory_order_acquire)) {
                return false;
            }
            if (writable_now() >= wanted) {
                commit_reservation(wanted);
                return true;
            }
            shared_.retire_epoch.wait(epoch, std::memory_order_acquire);
        }
    }

    // Hands back the tail of a reservation the producer did not use. Only the
    // producer thread may call it, and only before publishing into that tail.
    // Lowering the reservation after a lap is conservative and harmless: the
    // consumers it advanced were advanced a little early and their cursors
    // only ever rise.
    void release_reservation(std::uint64_t unused) {
        const auto reserved = producer_.reserved.load(std::memory_order_relaxed);
        const auto published = producer_.write_cursor.load(std::memory_order_relaxed);
        const auto floor_value = std::max(published, reserved > unused ? reserved - unused
                                                                       : dsp::SampleIndex{0});
        producer_.reserved.store(floor_value, std::memory_order_release);
    }

    // Publishes samples the producer has already placed in the ring, and
    // returns how many became readable. A return short of `count` means the
    // caller published more than it reserved, which is a producer bug.
    //
    // The host-memory hazard and the logical hazard are two different problems
    // and this solves exactly one of them. The release store below is what
    // makes the producer's writes visible to a consumer that observes the new
    // cursor. It says nothing about whether the device has finished a copy:
    // that is the ring timeline semaphore plus a VkMemoryBarrier2, and the two
    // must not be conflated. See device_ring.cpp.
    std::uint64_t publish(std::uint64_t count) {
        if (count == 0) {
            return 0;
        }
        const auto head = producer_.write_cursor.load(std::memory_order_relaxed);
        const auto reserved = producer_.reserved.load(std::memory_order_relaxed);
        const auto room = reserved > head ? reserved - head : 0;
        const auto published = std::min(count, room);
        if (published == 0) {
            return 0;
        }

        producer_.write_cursor.store(head + published, std::memory_order_release);
        producer_.write_cursor.notify_all();
        return published;
    }

    // Records samples the producer could not place because a Blocking
    // consumer, or a Paced source outrunning the ring, left no room.
    void note_dropped(std::uint64_t samples) {
        if (samples == 0) {
            return;
        }
        shared_.overrun_events.fetch_add(1, std::memory_order_relaxed);
        shared_.samples_lost.fetch_add(samples, std::memory_order_relaxed);
    }

    // --- observation and shutdown -------------------------------------------

    // The exclusive end of what has been published: the index of the next
    // sample the producer will write, and therefore the count of samples
    // published so far.
    [[nodiscard]] dsp::SampleIndex write_index() const {
        return producer_.write_cursor.load(std::memory_order_acquire);
    }

    // The oldest sample still in the ring, counting the window the producer
    // currently has open. A consumer sitting below this has been overrun and
    // has to resynchronise.
    //
    // Derived from the reserved cursor and not the write cursor, because
    // writing index i destroys index i minus one capacity and that happens
    // before the publish. Deriving it from the write cursor names a sample as
    // present while the producer is halfway through overwriting it, and the
    // consumer that trusts it reads a mixture of two eras with nothing
    // indicating that it did.
    [[nodiscard]] dsp::SampleIndex first_available() const {
        const auto reserved = reserved_index();
        return reserved > producer_.capacity ? reserved - producer_.capacity : dsp::SampleIndex{0};
    }

    [[nodiscard]] std::uint64_t overrun_events() const {
        return shared_.overrun_events.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t samples_lost() const {
        return shared_.samples_lost.load(std::memory_order_relaxed);
    }

    // Parks until the write cursor moves past `known_end`. This is the
    // scheduler's tick: a counter and a wait on it, no mutex and no condition
    // variable. Returns the cursor it woke on, which may equal `known_end`
    // when the ring has been stopped.
    [[nodiscard]] dsp::SampleIndex wait_for_data(dsp::SampleIndex known_end) const {
        for (;;) {
            const auto head = producer_.write_cursor.load(std::memory_order_acquire);
            if (head != known_end || shared_.stopped.load(std::memory_order_acquire)) {
                return head;
            }
            producer_.write_cursor.wait(head, std::memory_order_acquire);
        }
    }

    // Wakes everyone parked, once, permanently. A Blocking consumer that never
    // retires would otherwise hold the producer at teardown, and a process
    // that cannot exit is worse than one that exits having dropped samples.
    void stop() {
        shared_.stopped.store(true, std::memory_order_release);
        bump_retire_epoch();
        producer_.write_cursor.notify_all();
    }

    [[nodiscard]] bool stopped() const { return shared_.stopped.load(std::memory_order_acquire); }

private:
    static constexpr std::uint32_t kSlotFree = 0;
    static constexpr std::uint32_t kSlotBusy = 1;
    static constexpr std::uint32_t kSlotActive = 2;

    // One cache line per consumer. Two consumers retiring on two cores must
    // not invalidate each other's line: at a few hundred dispatches a second
    // across eight consumers that is measurable, and it is free to avoid.
    // Sized to exactly 64 bytes so alignas adds no padding of its own.
    struct alignas(64) Slot {
        std::atomic<dsp::SampleIndex> claimed{0};
        std::atomic<dsp::SampleIndex> retired{0};
        std::atomic<std::uint64_t> samples_skipped{0};
        std::atomic<std::uint64_t> forced_advances{0};
        std::atomic<std::uint32_t> generation{0};
        std::atomic<std::uint32_t> state{kSlotFree};
        std::atomic<std::uint32_t> kind{0};
        std::uint32_t pad_[5]{};
    };

    static_assert(sizeof(Slot) == 64, "one consumer slot per cache line, with no implicit padding");
    static_assert(std::atomic<dsp::SampleIndex>::is_always_lock_free,
                  "the cursors are read from the sample path and must not take a lock");

    // Slot index in the low eight bits, generation above it, so a handle from
    // a removed consumer cannot be mistaken for the next consumer to take that
    // slot. Zero is never a valid handle.
    [[nodiscard]] static std::uint32_t make_handle(std::uint32_t slot, std::uint32_t generation) {
        return ((generation & 0xFFFFFFu) << 8) | (slot + 1u);
    }

    [[nodiscard]] Slot* resolve(std::uint32_t handle) {
        return const_cast<Slot*>(std::as_const(*this).resolve(handle));
    }

    [[nodiscard]] const Slot* resolve(std::uint32_t handle) const {
        const std::uint32_t encoded_slot = handle & 0xFFu;
        if (encoded_slot == 0 || encoded_slot > kMaxRingConsumers) {
            return nullptr;
        }
        const auto& entry = slots_[encoded_slot - 1u];
        if (entry.state.load(std::memory_order_acquire) != kSlotActive) {
            return nullptr;
        }
        if ((entry.generation.load(std::memory_order_relaxed) & 0xFFFFFFu) != (handle >> 8)) {
            return nullptr;
        }
        return &entry;
    }

    [[nodiscard]] std::optional<dsp::SampleIndex> blocking_floor() const {
        auto floor_value = std::numeric_limits<dsp::SampleIndex>::max();
        bool any = false;
        for (const auto& entry : slots_) {
            if (entry.state.load(std::memory_order_acquire) != kSlotActive) {
                continue;
            }
            if (entry.kind.load(std::memory_order_relaxed) !=
                static_cast<std::uint32_t>(ConsumerKind::Blocking)) {
                continue;
            }
            any = true;
            // Acquire, pairing with the release in retire(). This load is the
            // one that licenses the producer to overwrite, so it is the one
            // that must not be reordered with the copy that follows it.
            floor_value = std::min(floor_value, entry.retired.load(std::memory_order_acquire));
        }
        return any ? std::optional<dsp::SampleIndex>{floor_value} : std::nullopt;
    }

    // What a Lossy consumer observes when it is overrun, and how it
    // resynchronises.
    //
    // The writer snaps the lapped consumer's cursors up to the oldest sample
    // still present and counts the jump. The consumer's next claim therefore
    // starts at an index higher than the one it expected, and it learns two
    // ways: ScopedClaim::skipped() reports the size of the hole, and the
    // index sequence it is handed has a gap in it. Resynchronising is nothing
    // more than carrying on from the index it was given, which is why the
    // whole system encodes time as an absolute sample index: there is no
    // separate "I lost something" state to enter and leave.
    void lap_lossy_consumers(dsp::SampleIndex oldest) {
        if (oldest == 0) {
            return;
        }
        for (auto& entry : slots_) {
            if (entry.state.load(std::memory_order_acquire) != kSlotActive) {
                continue;
            }
            if (entry.kind.load(std::memory_order_relaxed) !=
                static_cast<std::uint32_t>(ConsumerKind::Lossy)) {
                continue;
            }
            const auto claimed = entry.claimed.load(std::memory_order_acquire);
            if (claimed >= oldest) {
                // Not lapped on the claim cursor, but an open window's tail
                // may still have gone under. Raising retired keeps the two
                // cursors consistent; it does not gate anything, because a
                // Lossy consumer is not in the floor.
                detail::raise_to(entry.retired, std::min(oldest, claimed),
                                 std::memory_order_acq_rel);
                continue;
            }

            const auto jump = oldest - claimed;
            detail::raise_to(entry.claimed, oldest, std::memory_order_acq_rel);
            detail::raise_to(entry.retired, oldest, std::memory_order_acq_rel);
            entry.samples_skipped.fetch_add(jump, std::memory_order_relaxed);
            entry.forced_advances.fetch_add(1, std::memory_order_relaxed);
            shared_.overrun_events.fetch_add(1, std::memory_order_relaxed);
            shared_.samples_lost.fetch_add(jump, std::memory_order_relaxed);
        }
    }

    // Takes `count` samples of the ring for the producer, then tells anyone it
    // just lapped.
    //
    // The reservation is raised before the lap, not after. Raising it first is
    // the conservative order: for the instant between the two, first_available
    // names more samples as gone than are actually gone yet, and a consumer
    // that resynchronises on that reading loses a few samples it could have
    // had. The other order reports samples as present while they are being
    // demolished, and a consumer that trusts that reading gets a window
    // stitched together from two different eras.
    void commit_reservation(std::uint64_t count) {
        const auto reserved = producer_.reserved.load(std::memory_order_relaxed);
        const auto taken = reserved + count;
        producer_.reserved.store(taken, std::memory_order_release);
        lap_lossy_consumers(taken > producer_.capacity ? taken - producer_.capacity
                                                       : dsp::SampleIndex{0});
    }

    void bump_retire_epoch() {
        // Release, so the retirement it announces is visible to the producer
        // that wakes on it. The waiter's load is the matching acquire.
        shared_.retire_epoch.fetch_add(1, std::memory_order_release);
        shared_.retire_epoch.notify_all();
    }

    // The hot state is grouped into 64-byte blocks rather than given member
    // alignas directives one at a time. Both forms keep the producer's cursor
    // off the consumers' line; only this one leaves the enclosing class with
    // no implicit padding, and implicit padding from an alignment specifier is
    // C4324, which is an error under /WX.
    struct alignas(64) ProducerBlock {
        // Both written by the single producer and read by everyone. The
        // producer's own reads are relaxed because it is the only writer;
        // everyone else loads acquire, pairing with the release stores in
        // publish() and commit_reservation().
        //
        // write_cursor is what is readable. reserved is what the producer has
        // taken, published or not, and is always at or ahead of write_cursor.
        std::atomic<dsp::SampleIndex> write_cursor{0};
        std::atomic<dsp::SampleIndex> reserved{0};
        dsp::SampleIndex capacity = 0;
        std::uint64_t pad_[5]{};
    };

    struct alignas(64) SharedBlock {
        // Bumped on every retirement that could move the floor. The value
        // itself means nothing; it exists so a parked producer has an edge to
        // wait on that cannot be missed.
        std::atomic<std::uint64_t> retire_epoch{0};
        std::atomic<std::uint64_t> overrun_events{0};
        std::atomic<std::uint64_t> samples_lost{0};
        std::atomic<bool> stopped{false};
        std::uint64_t pad_[4]{};
    };

    static_assert(sizeof(ProducerBlock) == 64);
    static_assert(sizeof(SharedBlock) == 64);

    ProducerBlock producer_;
    SharedBlock shared_;
    std::array<Slot, kMaxRingConsumers> slots_;
};

// ---------------------------------------------------------------------------
// Registration against a source whose flow control the ring cannot see
// ---------------------------------------------------------------------------

// Rejects a Blocking consumer on a ring fed by a Paced source, at
// registration, which is the whole point: the alternative is a deadlock later,
// on a machine where a disk happened to be slow, with a stack trace that names
// the wrong thread.
//
// This is a free function and not part of DeviceRing because the frozen
// RingConfig carries no flow control, so DeviceRing::add_consumer has nothing
// to make the decision from. device_ring.h includes core/source/capabilities.h
// and uses nothing out of it, which reads like the field was meant to be
// there. See the note at the bottom of device_ring.cpp.
//
// Thread safety: as safe as DeviceRing::add_consumer, which is any thread.
[[nodiscard]] inline Expected<RingConsumer> register_consumer(DeviceRing& ring,
                                                              source::FlowControl flow,
                                                              ConsumerKind kind) {
    if (flow == source::FlowControl::Paced && kind == ConsumerKind::Blocking) {
        return fail("a Blocking consumer cannot be registered on a ring fed by a Paced source: "
                    "the source's clock does not wait, so stalling the writer would turn this "
                    "consumer being slow into lost radio samples with no way to tell afterwards "
                    "which of the two happened. Register it Lossy and handle the counted "
                    "overrun, or drive it from a Demand source");
    }
    return ring.add_consumer(kind);
}

// ---------------------------------------------------------------------------
// A claim that cannot be forgotten
// ---------------------------------------------------------------------------

// Holds a window open and retires it when it goes out of scope.
//
// Two exits, and the difference matters more than the RAII does:
//
//   destroyed without release()  nothing is reading the window, so retiring
//                                immediately is correct. This is the early
//                                return, the error path, and the stack unwind.
//   release()                    a dispatch now owns the window and the
//                                readback thread will retire it when the
//                                timeline value completes. Retiring here
//                                instead would hand the writer permission to
//                                overwrite samples the GPU is still reading,
//                                which is the failure the retire cursor exists
//                                to prevent.
//
// Thread safety: not shared. One consumer thread opens it, uses it and
// destroys or releases it. The DeviceRing and the RingConsumer must both
// outlive it.
class ScopedClaim {
public:
    // Opens a window of up to `count` samples starting at `from`.
    //
    // `from` is the consumer's own bookkeeping, because the frozen DeviceRing
    // exposes no per-consumer cursor to read it back from. If `from` has
    // already been overwritten the window starts at the oldest sample still
    // present instead, skipped() reports the size of the hole, and a Blocking
    // consumer fails outright, because the writer is never allowed to lap one
    // and a Blocking consumer finding itself lapped means the contract broke.
    [[nodiscard]] static Expected<ScopedClaim> open(DeviceRing& ring, const RingConsumer& consumer,
                                                    dsp::SampleIndex from, std::uint64_t count) {
        if (!consumer.valid()) {
            return fail("ScopedClaim::open with a consumer that is not registered");
        }
        if (count == 0) {
            return fail("ScopedClaim::open with a zero-sample window");
        }

        const auto oldest = ring.first_available();
        const auto published = ring.write_index();

        dsp::SampleIndex begin = from;
        std::uint64_t skipped = 0;
        if (begin < oldest) {
            if (consumer.kind() == ConsumerKind::Blocking) {
                return fail(std::format(
                    "a Blocking consumer was lapped: it sits at sample {} and the oldest sample "
                    "still in the ring is {}. The writer is not permitted to pass a Blocking "
                    "consumer, so either the retire cursor was advanced before the dispatch "
                    "completed or this consumer was registered after the writer had already run",
                    from, oldest));
            }
            skipped = oldest - from;
            begin = oldest;
        }

        if (begin >= published) {
            return fail(std::format(
                "nothing to claim: the window starts at sample {} and only {} samples have been "
                "published",
                begin, published));
        }

        const std::uint64_t available = published - begin;
        const std::uint64_t taken = std::min(count, available);

        ScopedClaim claim;
        claim.ring_ = &ring;
        claim.consumer_ = &consumer;
        claim.begin_ = begin;
        claim.end_ = begin + taken;
        claim.skipped_ = skipped;

        if (auto status = ring.claim(consumer, claim.end_); !status) {
            return std::unexpected(with_context(status.error(), "ScopedClaim::open"));
        }
        claim.held_ = true;
        return claim;
    }

    ScopedClaim() = default;

    ScopedClaim(const ScopedClaim&) = delete;
    ScopedClaim& operator=(const ScopedClaim&) = delete;

    ScopedClaim(ScopedClaim&& other) noexcept { adopt(std::move(other)); }

    ScopedClaim& operator=(ScopedClaim&& other) noexcept {
        if (this != &other) {
            retire_if_held();
            adopt(std::move(other));
        }
        return *this;
    }

    ~ScopedClaim() { retire_if_held(); }

    [[nodiscard]] dsp::SampleIndex begin() const { return begin_; }
    [[nodiscard]] dsp::SampleIndex end() const { return end_; }
    [[nodiscard]] std::uint64_t count() const { return end_ - begin_; }

    // Samples the writer passed this consumer before the window opened. Zero
    // on every claim that kept up, and a hole in the index sequence otherwise.
    [[nodiscard]] std::uint64_t skipped() const { return skipped_; }

    [[nodiscard]] bool held() const { return held_; }

    // Hands retirement to whoever completes the dispatch. After this the
    // destructor does nothing, and the window stays open until someone calls
    // DeviceRing::retire for it. Forgetting to is a stalled writer, which is
    // why the readback thread and not the recording thread owns that call.
    void release() { held_ = false; }

    // Retires now, explicitly. Safe to call once; a second call is a no-op.
    [[nodiscard]] Status retire_now() {
        if (!held_) {
            return {};
        }
        held_ = false;
        return ring_->retire(*consumer_, end_);
    }

private:
    void adopt(ScopedClaim&& other) noexcept {
        ring_ = other.ring_;
        consumer_ = other.consumer_;
        begin_ = other.begin_;
        end_ = other.end_;
        skipped_ = other.skipped_;
        held_ = other.held_;
        other.held_ = false;
        other.ring_ = nullptr;
        other.consumer_ = nullptr;
    }

    void retire_if_held() noexcept {
        if (!held_) {
            return;
        }
        held_ = false;
        // Discarded on purpose. A destructor has nobody to hand an error to,
        // and the only failures retire() can report here are a removed
        // consumer or a destroyed ring, both of which already mean the window
        // no longer gates anything.
        (void)ring_->retire(*consumer_, end_);
    }

    DeviceRing* ring_ = nullptr;
    const RingConsumer* consumer_ = nullptr;
    dsp::SampleIndex begin_ = 0;
    dsp::SampleIndex end_ = 0;
    std::uint64_t skipped_ = 0;
    bool held_ = false;
};

}  // namespace revenant::engine
