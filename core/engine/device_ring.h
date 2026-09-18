// The device-resident IQ ring.
//
// The single source of truth. The channelizer reads from it, the full-span FFT
// reads from it, and at M5 the disk writer will too. One copy, many consumers,
// no duplication: that sentence is the architecture, and every design choice
// below follows from refusing to break it.
//
// Lives in core/engine rather than core/capture. core/capture is reserved for
// M5's segment writer and seek index, and conflating a live GPU structure with
// a disk one would make both harder to reason about.

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"
#include "core/gpu/buffer.h"
#include "core/gpu/context.h"
#include "core/source/capabilities.h"

namespace revenant::engine {

// How a consumer behaves when it falls behind the writer.
//
// Lossy: the writer advances regardless and the consumer's reads become
// invalid. It is told, through a counted overrun, and it resynchronises. This
// is the only legal choice against a Paced source, because a radio's clock
// does not wait.
//
// Blocking: the writer stalls until this consumer retires. This is the
// backpressure that makes a Demand source run at exactly its consumer's rate,
// which is where faster than realtime comes from. Registering one against a
// Paced source is rejected at registration rather than deadlocking later.
enum class ConsumerKind : std::uint8_t { Lossy, Blocking };

struct RingConfig {
    dsp::SampleRate rate = 0;

    // How much history to keep. The achieved figure is frequently smaller and
    // is reported rather than silently substituted.
    double seconds_wanted = 8.0;

    // Overrides seconds_wanted when non-zero. Rounded down to a power of two.
    dsp::SampleIndex capacity_samples = 0;
};

// What the ring actually got, which is not always what was asked for.
//
// A device ring sized from "several seconds of full-rate IQ" without consulting
// the device's limits configures on one GPU and fails on another: the discrete
// card here allows a 16 TiB allocation where the integrated part caps at 2 GiB.
// So the ring shrinks to fit and says so. A user who asked for thirty seconds
// and got six is told, rather than discovering it when a scrub runs off the
// end of history that was never there.
struct RingGeometry {
    dsp::SampleIndex capacity_samples = 0;
    std::uint64_t capacity_mask = 0;
    double seconds_retained = 0.0;
    std::uint64_t bytes = 0;

    // True when the request was reduced to fit the device.
    bool clamped = false;
    std::string clamp_reason;
};

// A registered reader's position.
//
// Two cursors, not one, and the distinction is the whole retirement contract.
// `claimed` is how far the consumer has been handed work; `retired` is how far
// it has finished with. The writer may only overwrite below the slowest
// retired cursor. A single cursor cannot express "I have this window open
// right now", and a consumer whose window is overwritten mid-dispatch reads
// samples from the future with no indication that it did.
struct ConsumerCursor {
    dsp::SampleIndex claimed = 0;
    dsp::SampleIndex retired = 0;
};

class DeviceRing;

// Handle a consumer holds. Registration order is not significant and a
// consumer may be dropped at any time; the writer recomputes its floor.
class RingConsumer {
public:
    RingConsumer() = default;

    [[nodiscard]] std::uint32_t id() const { return id_; }
    [[nodiscard]] ConsumerKind kind() const { return kind_; }
    [[nodiscard]] bool valid() const { return ring_ != nullptr; }

private:
    friend class DeviceRing;

    DeviceRing* ring_ = nullptr;
    std::uint32_t id_ = 0;
    ConsumerKind kind_ = ConsumerKind::Lossy;
};

class DeviceRing {
public:
    // Sizes against the device's real limits and reports what it achieved.
    // Never fails because the device is small: it shrinks.
    [[nodiscard]] static Expected<DeviceRing> create(const gpu::Context& context,
                                                      const RingConfig& config);

    DeviceRing() = default;
    ~DeviceRing();

    DeviceRing(const DeviceRing&) = delete;
    DeviceRing& operator=(const DeviceRing&) = delete;
    DeviceRing(DeviceRing&& other) noexcept;
    DeviceRing& operator=(DeviceRing&& other) noexcept;

    [[nodiscard]] const RingGeometry& geometry() const { return geometry_; }

    // The device buffer every consumer kernel binds. Complex32 throughout: one
    // canonical format, agreed once, so the conversion from a source's native
    // width happens exactly once during upload and the bus still carries
    // native width.
    [[nodiscard]] VkBuffer buffer() const;

    // Index to ring offset. A consumer kernel does this per invocation with a
    // single AND, which is why the capacity is a power of two. The payoff is
    // that a window straddling the wrap needs no special case in any consumer:
    // each invocation masks its own index. That matters most for the
    // channelizer, whose window overlaps the previous block by design.
    [[nodiscard]] constexpr std::uint32_t offset_of(dsp::SampleIndex index) const {
        return static_cast<std::uint32_t>(index & geometry_.capacity_mask);
    }

    [[nodiscard]] Expected<RingConsumer> add_consumer(ConsumerKind kind);
    [[nodiscard]] Status remove_consumer(RingConsumer& consumer);

    // Marks a window as being worked on. The writer will not overwrite it
    // until it is retired.
    [[nodiscard]] Status claim(const RingConsumer& consumer, dsp::SampleIndex through);

    // Releases everything below `through`. The writer's floor is the minimum
    // retired cursor across all consumers.
    [[nodiscard]] Status retire(const RingConsumer& consumer, dsp::SampleIndex through);

    // Oldest sample still guaranteed present, and the newest written. A
    // consumer that finds its position below the first has been overrun.
    [[nodiscard]] dsp::SampleIndex first_available() const;
    [[nodiscard]] dsp::SampleIndex write_index() const;

    [[nodiscard]] std::uint64_t overrun_events() const;
    [[nodiscard]] std::uint64_t samples_lost() const;

private:
    void destroy() noexcept;

    const gpu::Context* context_ = nullptr;
    gpu::Buffer storage_;
    RingGeometry geometry_{};
    dsp::SampleRate rate_ = 0;

    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace revenant::engine
