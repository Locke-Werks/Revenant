// The device-resident IQ ring.
//
// Four things in this file are not visible from the header, because each one
// is a place where the obvious implementation is wrong in a way that does not
// show up until something is already recorded badly.
//
//
// 1. SIZING, AND WHY IT NEVER FAILS BECAUSE THE DEVICE IS SMALL
//
// The two GPUs in the development machine differ by a factor of 512 in what
// they will allocate: the discrete card reports maxMemoryAllocationSize of
// 0xFFFFFFFFFFFFFFFF and maxBufferSize of 1 TiB, and the integrated part caps
// both at exactly 2 GiB. maxStorageBufferRange is 4 GiB minus one on both. A
// ring sized from "eight seconds of full-rate IQ" without reading any of that
// configures on one CI leg and fails on the other, and the failure reads like
// a driver problem rather than an arithmetic one.
//
// So the ring shrinks instead. plan_ring_geometry in ring_consumer.h picks the
// largest power of two that fits every limit, sets RingGeometry::clamped, and
// puts the binding limit's name and value in clamp_reason. A caller who asked
// for thirty seconds and got six is told, in a sentence, rather than finding
// out when a scrub runs off the end of history that was never there.
//
// If the allocation is refused anyway, which is what a heap that is already
// full looks like and what VK_EXT_memory_budget would have predicted had the
// Context exposed it, the capacity is halved and tried again. Same rule: never
// fail because the device is small.
//
//
// 2. THE PRODUCER, AND WHY TEARING IS TWO PROBLEMS AND NOT ONE
//
// Conflating these is the bug, so they are named separately and solved
// separately.
//
//   (a) The memory hazard. A consumer kernel reads a region while the copy
//       that fills it is still in flight. This is a Vulkan ordering problem
//       and the cursors have nothing to say about it. It is solved by the ring
//       timeline semaphore plus a barrier: the write submission for block k
//       signals timeline value k, a consumer wanting samples up to index N
//       waits for the value of the block containing N before its dispatch
//       runs, and each write submission ends with a VkMemoryBarrier2 carrying
//       srcStage COPY | COMPUTE_SHADER, srcAccess TRANSFER_WRITE |
//       SHADER_STORAGE_WRITE, dstStage COMPUTE_SHADER, dstAccess
//       SHADER_STORAGE_READ. The COMPUTE_SHADER source stage is there because
//       the upload is a convert kernel, not a raw copy, whenever the source
//       format is not cf32.
//
//   (b) The logical hazard. The producer laps a consumer and overwrites
//       samples that consumer has not read yet. No barrier helps: the copy is
//       correctly ordered and the data is simply gone. This is cursor
//       arithmetic, it is what ConsumerTable implements, and it is entirely on
//       the host.
//
// The host-memory half of (a), making the producer's own stores visible to
// another host thread that observes the new write cursor, is the release store
// in ConsumerTable::publish. That is a third thing again and it protects host
// memory only. A release store says nothing about a device copy.
//
// The producer carries two cursors of its own, and for the same reason a
// consumer does. `reserved` is the window it has taken and is writing into;
// `write_cursor` is what it has finished with and published. Writing sample i
// destroys sample i minus one capacity, and that destruction happens before
// the publish, so a single producer cursor would report the oldest samples as
// present for exactly as long as they were being demolished. first_available()
// is therefore derived from the reserved cursor. This is the producer-side
// mirror of the claim-against-retire distinction, and it was found by a probe
// rather than by reading: a Lossy consumer at the oldest edge read cells from
// two different eras until the reservation cursor was added.
//
// Everything on the host is exactly three jobs and no more: cursor arithmetic
// in atomics, command recording and submission, and timeline waits. There is
// no device-side cursor, because a device cursor has to be read back to be
// useful and the readback is the latency the architecture exists to avoid.
//
//
// 3. THE WRAP, WHICH THE HOST SPLITS AND A KERNEL DOES NOT
//
// A consumer kernel never special-cases the wrap. It masks per invocation,
// offset = (base_offset + i) & ring_mask, which is one AND because the
// capacity is a power of two. A window straddling the wrap is therefore
// contiguous in index space no matter where it lands in the buffer, and that
// is what lets the channelizer overlap the previous block by design without
// any of its arithmetic knowing the ring exists.
//
// WHAT THIS PARAGRAPH USED TO SAY. Until 2026-09-20 it read "each invocation
// masks its own absolute index, offset = uint32(index & capacity_mask)". The
// index a kernel masks is NOT absolute and cannot be: GLSL has no 64-bit
// integer without an extension nothing in core/shaders enables, so the host
// masks the absolute index down to a 32-bit ring offset and hands that down.
// core/shaders/pfb_branch.comp says the same thing from the other side, and
// says what a truncated absolute index costs: it works for three and a half
// minutes at 20 MS/s and then does not.
//
// The host-side writer is the one place that does split. vkCmdCopyBuffer takes
// a contiguous destination range, so a block whose destination crosses the
// wrap becomes two VkBufferCopy regions in the same command buffer:
//
//   const uint32_t offset = ring.offset_of(block_start);
//   const uint64_t first  = min(count, capacity - offset);
//   region[0] = { src_offset,                    offset * 8, first  * 8 };
//   region[1] = { src_offset + first * 8,        0,          (count - first) * 8 };
//
// Two regions, one vkCmdCopyBuffer, one barrier, one timeline signal. The
// split is a property of the copy API and not of the ring, which is why it
// lives in the writer and appears nowhere in a consumer.
//
//
// 4. WHAT A LOSSY CONSUMER SEES WHEN IT IS OVERRUN
//
// The writer's floor is the minimum retired cursor across Blocking consumers
// only. A Lossy consumer does not hold the writer back: that is the definition
// of the kind, and it is the only legal kind against a Paced source, because
// a radio's clock does not wait and stalling the writer would turn a slow
// consumer into lost radio samples with no way to tell afterwards which of the
// two happened.
//
// When the writer laps a Lossy consumer it snaps that consumer's claim and
// retire cursors up to the oldest sample still present, adds the jump to that
// consumer's samples_skipped, increments forced_advances, and counts a ring
// overrun event. The consumer finds out on its next claim: ScopedClaim::open
// starts the window at the oldest available sample and reports the size of the
// hole through skipped(). Resynchronising is carrying on from the index it was
// given. There is no recovery state to enter and leave, because time is an
// absolute sample index and a loss is a hole in it, which is the same gap
// encoding a source overrun uses. One gap concept, end to end.
//
//
// WHAT IS STILL NOT HERE, STATED RATHER THAN DISCOVERED AT INTEGRATION
//
// The ring owns no VkSemaphore. Section 2(a) describes the ordering half of
// the producer's job in terms of a ring timeline, and the timeline that
// actually plays that part belongs to the graph: core/engine/graph.cpp creates
// it, signals it once per submission and the scheduler waits on it. A
// consumer that dispatches therefore takes its timeline value from the graph
// and has nowhere to ask the ring for one.
//
// That last sentence used to read "which is why ring_consumer.h has the
// caller fill in ReadLease::timeline_value rather than the ring filling it
// in". ReadLease was deleted on 2026-09-20 along with the push-constant block
// it carried, unused by any kernel and uncompilable by one; the note standing
// in its place in ring_consumer.h has the reasoning.
//
// It holds while one graph is both the ring's only producer and its only
// consumer, which is the case today. A second consumer dispatching against the
// same ring would need a timeline value it has no way to ask the ring for, and
// moving the semaphore in here is the change to make then rather than now.

#include "core/engine/device_ring.h"

#include <format>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "core/engine/ring_consumer.h"

namespace revenant::engine {
namespace {

constexpr VkBufferUsageFlags kRingUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

void reset_geometry(RingGeometry& geometry) noexcept {
    geometry.capacity_samples = 0;
    geometry.capacity_mask = 0;
    geometry.seconds_retained = 0.0;
    geometry.bytes = 0;
    geometry.clamped = false;

    // clear() rather than assigning a fresh RingGeometry: this runs from a
    // noexcept destructor path and a string assignment is not formally
    // noexcept, so the tidy version terminates the process on a hypothetical
    // allocation failure.
    geometry.clamp_reason.clear();
}

// Recomputes the derived fields after a shrink, and appends the reason rather
// than replacing it: a ring that was clamped by a device limit and then
// shrunk again by a refused allocation was reduced twice and both reductions
// are things the operator wants to see.
void shrink_geometry(RingGeometry& geometry, dsp::SampleIndex capacity, dsp::SampleRate rate,
                     std::string_view reason) {
    geometry.capacity_samples = capacity;
    geometry.capacity_mask = capacity - 1;
    geometry.bytes = capacity * kRingSampleBytes;
    geometry.seconds_retained = static_cast<double>(capacity) / static_cast<double>(rate);
    geometry.clamped = true;
    if (!geometry.clamp_reason.empty()) {
        geometry.clamp_reason.append("; then ");
    }
    geometry.clamp_reason.append(reason);
}

}  // namespace

// Everything mutable about the ring's bookkeeping. Behind a pimpl because the
// cursor table is four kilobytes of cache-line-padded atomics and no consumer
// of device_ring.h needs to see it.
struct DeviceRing::State {
    explicit State(dsp::SampleIndex capacity) : consumers(capacity) {}

    ConsumerTable consumers;
};

// Thread safety: call once, before the ring is published to any other thread.
Expected<DeviceRing> DeviceRing::create(const gpu::Context& context, const RingConfig& config) {
    if (!context.valid()) {
        return fail("DeviceRing::create called with an invalid gpu::Context");
    }

    auto planned = plan_ring_geometry(config, limits_of(context.info()));
    if (!planned) {
        return std::unexpected(with_context(planned.error(), "DeviceRing::create"));
    }

    DeviceRing ring;
    ring.geometry_ = std::move(*planned);
    ring.rate_ = config.rate;

    // The ring is bound as one SSBO and is both a copy destination, for the
    // upload, and a copy source, so a readback or M5's segment writer can pull
    // a window out of it without a second staging pass.
    for (;;) {
        auto attempt = gpu::Buffer::create(context, ring.geometry_.bytes, kRingUsage,
                                           gpu::MemoryKind::DeviceLocal);
        if (attempt) {
            ring.storage_ = std::move(*attempt);
            break;
        }

        const auto halved = ring.geometry_.capacity_samples / 2;
        if (halved < kMinRingCapacitySamples) {
            return std::unexpected(with_context(
                attempt.error(),
                std::format("DeviceRing::create could not allocate a {}-byte ring, and halving "
                            "further would take it below the {}-sample minimum",
                            ring.geometry_.bytes, kMinRingCapacitySamples)));
        }

        // The limits said this would fit and the allocator disagreed, which is
        // what a heap already occupied by another process looks like. Shrink
        // and retry rather than failing: a smaller ring is a shorter time
        // machine, and no ring at all is no engine.
        shrink_geometry(ring.geometry_, halved, config.rate,
                        std::format("the device refused a {}-byte allocation ({}), so the ring "
                                    "was halved to {} samples ({:.3f} s)",
                                    ring.geometry_.bytes, attempt.error().message, halved,
                                    static_cast<double>(halved) /
                                        static_cast<double>(config.rate)));
    }

    ring.context_ = &context;
    ring.state_ = std::make_unique<State>(ring.geometry_.capacity_samples);
    return ring;
}

DeviceRing::~DeviceRing() { destroy(); }

DeviceRing::DeviceRing(DeviceRing&& other) noexcept
    : context_(other.context_),
      storage_(std::move(other.storage_)),
      geometry_(std::move(other.geometry_)),
      rate_(other.rate_),
      state_(std::move(other.state_)) {
    other.context_ = nullptr;
    other.rate_ = 0;
    reset_geometry(other.geometry_);
}

DeviceRing& DeviceRing::operator=(DeviceRing&& other) noexcept {
    if (this != &other) {
        destroy();
        context_ = other.context_;
        storage_ = std::move(other.storage_);
        geometry_ = std::move(other.geometry_);
        rate_ = other.rate_;
        state_ = std::move(other.state_);

        other.context_ = nullptr;
        other.rate_ = 0;
        reset_geometry(other.geometry_);
    }
    return *this;
}

// Moving a ring that already has consumers leaves every RingConsumer pointing
// at the moved-from object. That object's state_ is null afterwards, so a
// stale consumer's claim or retire fails with a message naming the problem
// instead of corrupting the live ring's cursors. Move a ring before it has
// consumers; the failure mode if you do not is loud, which is the most that
// can be arranged while RingConsumer holds a raw back-pointer and the move
// has no way to reach the handles already issued.
void DeviceRing::destroy() noexcept {
    if (state_ != nullptr) {
        // Wakes any producer parked on a Blocking consumer that will now never
        // retire. A process that cannot exit is worse than one that exits
        // having dropped samples.
        state_->consumers.stop();
    }
    state_.reset();
    storage_ = gpu::Buffer{};
    context_ = nullptr;
    rate_ = 0;
    reset_geometry(geometry_);
}

// Thread safety: any thread. The handle is fixed for the ring's lifetime.
VkBuffer DeviceRing::buffer() const { return storage_.handle(); }

// Thread safety: any thread, concurrently with anything including the sample
// path. Registration is a CAS into a free slot.
Expected<RingConsumer> DeviceRing::add_consumer(ConsumerKind kind) {
    if (state_ == nullptr) {
        return fail("DeviceRing::add_consumer on a ring that was never created, or was moved from");
    }

    // Start at the current write cursor. A consumer registering with a cursor
    // more than one capacity behind the writer is overrun before it has read
    // anything, and a Blocking one would wedge the writer along with it.
    //
    // This is also where a Blocking registration against a Paced source would
    // be rejected, and is not, because RingConfig does not carry the source's
    // flow control. engine::register_consumer in ring_consumer.h does that
    // check instead, and is the call a caller who knows the source should make.
    auto handle = state_->consumers.acquire_slot(kind, state_->consumers.write_index());
    if (!handle) {
        return std::unexpected(with_context(handle.error(), "DeviceRing::add_consumer"));
    }

    RingConsumer out;
    out.ring_ = this;
    out.id_ = *handle;
    out.kind_ = kind;
    return out;
}

// Thread safety: any thread. Removing the slowest Blocking consumer raises the
// writer's floor, and a producer parked on it is woken by release_slot.
Status DeviceRing::remove_consumer(RingConsumer& consumer) {
    if (state_ == nullptr) {
        return fail("DeviceRing::remove_consumer on a ring that was never created, or was moved "
                    "from");
    }
    if (consumer.ring_ != this) {
        return fail(consumer.ring_ == nullptr
                        ? "DeviceRing::remove_consumer on a consumer that is not registered"
                        : "DeviceRing::remove_consumer on a consumer registered with another ring");
    }
    if (auto status = state_->consumers.release_slot(consumer.id_); !status) {
        return std::unexpected(with_context(status.error(), "DeviceRing::remove_consumer"));
    }

    consumer.ring_ = nullptr;
    consumer.id_ = 0;
    return {};
}

// Thread safety: the owning consumer's thread. Two threads driving one
// RingConsumer is a caller error; the monotone compare-exchange keeps it from
// corrupting the table but the resulting window is nobody's.
Status DeviceRing::claim(const RingConsumer& consumer, dsp::SampleIndex through) {
    if (state_ == nullptr) {
        return fail("DeviceRing::claim on a ring that was never created, or was moved from");
    }
    if (consumer.ring_ != this) {
        return fail("DeviceRing::claim on a consumer that is not registered with this ring");
    }
    return state_->consumers.claim(consumer.id_, through);
}

// Thread safety: the thread that observes the dispatch complete, which is the
// readback thread and not the thread that recorded it.
//
// Retiring at record time or at submit time is the failure this cursor exists
// to prevent: the samples are still being read by an in-flight dispatch, the
// writer is told they are free, and the symptom is noise in one demodulated
// channel with nothing in any log.
Status DeviceRing::retire(const RingConsumer& consumer, dsp::SampleIndex through) {
    if (state_ == nullptr) {
        return fail("DeviceRing::retire on a ring that was never created, or was moved from");
    }
    if (consumer.ring_ != this) {
        return fail("DeviceRing::retire on a consumer that is not registered with this ring");
    }
    return state_->consumers.retire(consumer.id_, through);
}

// Thread safety: any thread. The two cursors are read with two loads and are
// not a snapshot of the pair: claimed can be observed ahead of a retire that
// had already happened on the consumer's own thread. Anything computing a
// floor reads retired, which only ever rises.
std::optional<ConsumerCursor> DeviceRing::cursor(const RingConsumer& consumer) const {
    if (state_ == nullptr || consumer.ring_ != this) {
        return std::nullopt;
    }
    const auto report = state_->consumers.report(consumer.id_);
    return report ? std::optional<ConsumerCursor>{report->cursor} : std::nullopt;
}

// The producer. Each of these is the single upload thread's, and each one is
// the ring's own cursor arithmetic rather than a copy of it kept somewhere
// else: a producer holding its own table publishes into cursors no consumer of
// this ring can see, so write_index() answers zero while samples are flowing.
//
// Thread safety: the producer thread only, for all five mutators.
Expected<std::uint64_t> DeviceRing::reserve(std::uint64_t count) {
    if (state_ == nullptr) {
        return fail("DeviceRing::reserve on a ring that was never created, or was moved from");
    }
    return state_->consumers.reserve(count);
}

// Two ways this can fail and they are not the same event, so they do not share
// a message. A stopped ring is an orderly shutdown reaching a parked producer.
// A request larger than the whole ring is arithmetic that no retirement can
// ever satisfy, so waiting on it is a deadlock dressed as backpressure, and it
// is refused before the wait rather than discovered inside it.
Status DeviceRing::reserve_blocking(std::uint64_t count) {
    if (state_ == nullptr) {
        return fail("DeviceRing::reserve_blocking on a ring that was never created, or was moved "
                    "from");
    }
    if (count > geometry_.capacity_samples) {
        return fail(std::format(
            "a producer asked to reserve {} samples from a ring that holds {}. No amount of "
            "retiring can make that fit, so waiting for it would never return",
            count, geometry_.capacity_samples));
    }
    if (!state_->consumers.reserve_blocking(count)) {
        return fail(std::format(
            "the ring was stopped while the producer waited for room for {} samples", count));
    }
    return {};
}

void DeviceRing::release_reservation(std::uint64_t unused) {
    if (state_ != nullptr) {
        state_->consumers.release_reservation(unused);
    }
}

Expected<std::uint64_t> DeviceRing::publish(std::uint64_t count) {
    if (state_ == nullptr) {
        return fail("DeviceRing::publish on a ring that was never created, or was moved from");
    }
    return state_->consumers.publish(count);
}

void DeviceRing::note_dropped(std::uint64_t samples) {
    if (state_ != nullptr) {
        state_->consumers.note_dropped(samples);
    }
}

// Thread safety: any thread. A snapshot, and the producer may have moved by
// the time a caller acts on it.
dsp::SampleIndex DeviceRing::reserved_index() const {
    return state_ == nullptr ? dsp::SampleIndex{0} : state_->consumers.reserved_index();
}

// Thread safety: any thread, and idempotent. A ring that was never created has
// nobody parked on it, so this is a no-op there rather than an error.
void DeviceRing::stop() {
    if (state_ != nullptr) {
        state_->consumers.stop();
    }
}

bool DeviceRing::stopped() const { return state_ != nullptr && state_->consumers.stopped(); }

// The oldest sample a consumer may still read, counting the window the
// producer currently has open rather than only what it has published.
//
// Writing sample i destroys sample i minus one capacity, and that happens
// before the publish that would have announced it. Deriving this from the
// published cursor alone would name a sample as present while it was being
// overwritten, and a Lossy consumer sitting at the oldest edge would read a
// window stitched from two different eras with nothing saying so. So
// ConsumerTable keeps a reserved cursor beside the published one, for the
// same reason a consumer keeps a claim cursor beside its retire cursor.
//
// Thread safety: any thread. A snapshot, and by the time a caller acts on it
// the writer may have moved; a consumer treats it as a lower bound.
dsp::SampleIndex DeviceRing::first_available() const {
    return state_ == nullptr ? dsp::SampleIndex{0} : state_->consumers.first_available();
}

// The exclusive end of what has been published: the index of the next sample
// the writer will produce, which is also the count of samples published. Every
// window in the engine is half-open, so returning the exclusive end is what
// makes a comparison against it read correctly; the newest sample actually in
// the ring is write_index() - 1.
//
// Thread safety: any thread.
dsp::SampleIndex DeviceRing::write_index() const {
    return state_ == nullptr ? dsp::SampleIndex{0} : state_->consumers.write_index();
}

// Counts both ways samples go missing at the ring: a producer that found no
// room, and a Lossy consumer the writer lapped. Both are correctness events
// and neither is a log line, because a log line produces a recording that
// looks continuous and is not.
//
// Thread safety: any thread.
std::uint64_t DeviceRing::overrun_events() const {
    return state_ == nullptr ? 0 : state_->consumers.overrun_events();
}

std::uint64_t DeviceRing::samples_lost() const {
    return state_ == nullptr ? 0 : state_->consumers.samples_lost();
}

}  // namespace revenant::engine
