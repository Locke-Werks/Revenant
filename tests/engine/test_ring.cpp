// The device ring's host logic.
//
// Most of the risk in a ring is not on the device. It is in the sizing, which
// fails on one machine and not another; in the retirement floor, which is what
// stops the writer overwriting a window somebody is reading; and in the wrap,
// which is correct for about three and a half minutes at 20 MS/s if the index
// arithmetic is done in 32 bits by mistake. All three are worth testing
// exhaustively rather than incidentally.
//
// The sizing and the wrap need no GPU at all. The retirement floor is
// exercised through DeviceRing, because that is the API a producer and a
// consumer actually hold, and DeviceRing::create allocates a buffer: those
// cases take the shared device and skip without one.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "core/engine/device_ring.h"
#include "core/engine/ring_consumer.h"
#include "core/engine/spsc_ring.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

namespace {

// The two devices in the conformance matrix, as measured through DeviceInfo
// rather than taken from a specification. The difference between them is the
// whole reason the ring plans instead of assuming: sizing that works on the
// first fails on the second, and it fails on one CI leg only, which reads as a
// driver problem rather than an arithmetic one.
constexpr engine::RingLimits kDiscrete{
    .max_memory_allocation_size = 0xFFFF'FFFF'FFFF'FFFFULL,
    .max_storage_buffer_range = 0xFFFF'FFFFULL,
};

constexpr engine::RingLimits kIntegrated{
    .max_memory_allocation_size = 2ULL * 1024 * 1024 * 1024,
    .max_storage_buffer_range = 0xFFFF'FFFFULL,
};

}  // namespace

TEST_CASE("the ring shrinks to fit the device rather than failing", "[engine][ring][m1]") {
    // Never fail because the device is small. A user who asked for thirty
    // seconds and got six should be told, not discover it when a scrub runs
    // off the end of history that was never there.
    engine::RingConfig config;
    config.rate = 20'000'000;
    config.seconds_wanted = 30.0;

    const auto on_discrete = engine::plan_ring_geometry(config, kDiscrete);
    REQUIRE(on_discrete.has_value());
    const auto on_integrated = engine::plan_ring_geometry(config, kIntegrated);
    REQUIRE(on_integrated.has_value());

    INFO("discrete:   " << on_discrete->capacity_samples << " samples, "
                        << on_discrete->seconds_retained << " s, " << on_discrete->clamp_reason);
    INFO("integrated: " << on_integrated->capacity_samples << " samples, "
                        << on_integrated->seconds_retained << " s, "
                        << on_integrated->clamp_reason);

    CHECK(on_discrete->capacity_samples > 0);
    CHECK(on_integrated->capacity_samples > 0);

    // Both clamped below the request, and each says so.
    CHECK(on_discrete->clamped);
    CHECK(on_integrated->clamped);
    CHECK_FALSE(on_discrete->clamp_reason.empty());
    CHECK_FALSE(on_integrated->clamp_reason.empty());

    // A power of two, because index-to-offset is one AND per shader
    // invocation. A non-power-of-two capacity would need a 64-bit modulo per
    // invocation instead.
    CHECK(std::has_single_bit(static_cast<std::uint64_t>(on_discrete->capacity_samples)));
    CHECK(std::has_single_bit(static_cast<std::uint64_t>(on_integrated->capacity_samples)));

    CHECK(on_discrete->capacity_mask == on_discrete->capacity_samples - 1);
    CHECK(on_integrated->capacity_mask == on_integrated->capacity_samples - 1);

    // The integrated part cannot hold more than the discrete one.
    CHECK(on_integrated->capacity_samples <= on_discrete->capacity_samples);
}

TEST_CASE("a modest request is honoured and not reported as clamped",
          "[engine][ring][m1]") {
    engine::RingConfig config;
    config.rate = 2'400'000;
    config.capacity_samples = 1024;

    const auto planned = engine::plan_ring_geometry(config, kIntegrated);
    REQUIRE(planned.has_value());

    CHECK(planned->capacity_samples == 1024);

    // Rounding a request up is not a clamp, and reporting it as one would
    // train people to ignore the flag.
    CHECK_FALSE(planned->clamped);
}

TEST_CASE("a request that is not a power of two is rounded down and said so",
          "[engine][ring][m1]") {
    engine::RingConfig config;
    config.rate = 2'400'000;
    config.capacity_samples = 3000;

    const auto planned = engine::plan_ring_geometry(config, kIntegrated);
    REQUIRE(planned.has_value());

    CHECK(planned->capacity_samples == 2048);
    CHECK(planned->clamped);
    INFO(planned->clamp_reason);
    CHECK_FALSE(planned->clamp_reason.empty());
}

TEST_CASE("a rate of zero is refused rather than producing a ring with no retention",
          "[engine][ring][m1]") {
    engine::RingConfig config;
    config.rate = 0;
    config.seconds_wanted = 8.0;

    const auto planned = engine::plan_ring_geometry(config, kDiscrete);
    CHECK_FALSE(planned.has_value());
}

TEST_CASE("index to offset stays correct past the 32-bit boundary", "[engine][ring][m1]") {
    // The trap this guards. SampleIndex is 64 bits and the ring offset is 32,
    // so an implementation that reduces the index to 32 bits before masking is
    // correct until the index passes 2^32, which at 20 MS/s is three and a
    // half minutes. A capture is fine for one coffee break and wrong for the
    // rest of the session.
    constexpr std::uint64_t kCapacity = 1ULL << 20;
    constexpr std::uint64_t kMask = kCapacity - 1;

    const dsp::SampleIndex probes[] = {
        0,
        1,
        kCapacity - 1,
        kCapacity,
        kCapacity + 7,
        (1ULL << 32) - 1,
        1ULL << 32,
        (1ULL << 32) + 12345,
        (1ULL << 40) + 999,
        0xFFFF'FFFF'FFFF'FFFFULL,
    };

    for (const dsp::SampleIndex index : probes) {
        const auto expected = static_cast<std::uint32_t>(index % kCapacity);
        const auto masked = static_cast<std::uint32_t>(index & kMask);
        INFO("index " << index);
        CHECK(masked == expected);
    }
}

TEST_CASE("the SPSC ring loses nothing under two real threads", "[engine][ring][m1]") {
    // This carries audio from the GPU readback to the egress thread. An
    // underrun is a dropout the operator hears, so it is treated as a
    // correctness structure and tested as one: a single missed or duplicated
    // item here is an audible click.
    auto created = engine::SpscRing<std::uint32_t>::create(1024);
    REQUIRE(created.has_value());
    auto& ring = **created;

    constexpr std::uint32_t kItems = 2'000'000;

    std::thread producer([&ring] {
        std::uint32_t next = 0;
        std::vector<std::uint32_t> batch;
        while (next < kItems) {
            batch.clear();
            const std::uint32_t chunk = 1 + (next % 97);
            for (std::uint32_t i = 0; i < chunk && next + i < kItems; ++i) {
                batch.push_back(next + i);
            }
            std::size_t written = 0;
            while (written < batch.size()) {
                written += ring.write(std::span<const std::uint32_t>(batch).subspan(written));
            }
            next += static_cast<std::uint32_t>(batch.size());
        }
    });

    std::uint64_t received = 0;
    std::uint32_t expected = 0;
    bool ordered = true;
    std::vector<std::uint32_t> out(256);
    while (received < kItems) {
        const std::size_t got = ring.read(std::span<std::uint32_t>(out));
        for (std::size_t i = 0; i < got; ++i) {
            if (out[i] != expected) {
                ordered = false;
            }
            ++expected;
        }
        received += got;
    }
    producer.join();

    // Nothing lost, nothing duplicated, nothing reordered. Any of the three
    // would be an audible fault.
    CHECK(received == kItems);
    CHECK(ordered);
    CHECK(expected == kItems);

    // The ring's own accounting must agree with what the threads observed. A
    // structure that loses an item and does not know it has lost one is worse
    // than a structure that loses an item, because the second can be
    // diagnosed and the first cannot.
    CHECK(ring.total_written() == kItems);
    CHECK(ring.total_read() == kItems);
    CHECK(ring.dropped_samples() == 0);
    CHECK(ring.underrun_samples() == 0);
}

TEST_CASE("the SPSC ring counts what it drops rather than hiding it",
          "[engine][ring][m1]") {
    // write_or_drop is the Paced path: the audio thread must never block, so
    // when the consumer is behind the samples are gone. That is a dropout the
    // operator hears, so it is counted and readable rather than logged.
    auto created = engine::SpscRing<float>::create(64);
    REQUIRE(created.has_value());
    auto& ring = **created;

    const std::vector<float> batch(100, 1.0F);
    const std::size_t wrote = ring.write_or_drop(batch);

    CHECK(wrote == ring.capacity());
    CHECK(ring.dropped_samples() == batch.size() - ring.capacity());
    CHECK(ring.dropped_events() == 1);

    // And an underrun on the read side is counted too: an audio callback that
    // gets silence because nothing was ready is the same fault heard from the
    // other end.
    std::vector<float> out(ring.capacity() + 32);
    const std::size_t got = ring.read_or_fill(out, 0.0F);
    CHECK(got == ring.capacity());
    CHECK(ring.underrun_samples() == 32);
    CHECK(ring.underrun_events() == 1);
}

TEST_CASE("the SPSC ring capacity is a power of two", "[engine][ring][m1]") {
    auto created = engine::SpscRing<float>::create(1000);
    REQUIRE(created.has_value());
    CHECK(std::has_single_bit((*created)->capacity()));
    CHECK((*created)->capacity() >= 1000);
}

// ---------------------------------------------------------------------------
// The retirement contract, through the ring itself
// ---------------------------------------------------------------------------
//
// These need a device, unlike everything above, because DeviceRing::create
// allocates one. The arithmetic under test is host-side and the buffer is
// eight kilobytes that nothing ever reads, so the device is a precondition of
// the constructor rather than part of what is being measured.
//
// They go through DeviceRing and not through ConsumerTable directly, on
// purpose. The cursor arithmetic was correct while the producer kept its own
// table and the ring's cursors sat at zero for the life of the process, so a
// test that reaches past the ring's API is a test that would have passed
// throughout the defect it is meant to catch.

namespace {

// Small on purpose. 1024 samples is the smallest capacity a ring will take,
// so lapping one costs a couple of reserve calls rather than a couple of
// million, and the lap is what most of these cases are about.
[[nodiscard]] Expected<engine::DeviceRing> make_ring(dsp::SampleIndex capacity) {
    engine::RingConfig config;
    config.rate = 1'000'000;
    config.capacity_samples = capacity;
    return engine::DeviceRing::create(test::shared_context(), config);
}

}  // namespace

TEST_CASE("the write cursor reports what the producer published", "[engine][ring][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    auto created = make_ring(1024);
    REQUIRE(created.has_value());
    auto& ring = *created;

    CHECK(ring.write_index() == 0);
    CHECK(ring.reserved_index() == 0);

    auto granted = ring.reserve(256);
    REQUIRE(granted.has_value());
    CHECK(*granted == 256);

    // Taken, not published. The producer is writing into that window, so
    // nothing may read it and the write cursor has not moved.
    CHECK(ring.reserved_index() == 256);
    CHECK(ring.write_index() == 0);

    auto published = ring.publish(256);
    REQUIRE(published.has_value());
    CHECK(*published == 256);
    CHECK(ring.write_index() == 256);
}

TEST_CASE("the oldest sample counts the window the producer has open",
          "[engine][ring][m1]") {
    REVENANT_NEEDS_GPU();

    // The reason the producer has two cursors rather than one. Writing sample
    // i destroys sample i minus one capacity, and that happens before the
    // publish that would have announced it. Deriving the oldest sample from
    // the write cursor would still name sample 0 as present here, while the
    // producer is in the middle of overwriting it, and a Lossy consumer at
    // that edge reads a window stitched from two eras with nothing saying so.
    auto created = make_ring(1024);
    REQUIRE(created.has_value());
    auto& ring = *created;

    REQUIRE(ring.reserve(1024).has_value());
    REQUIRE(ring.publish(1024).has_value());
    CHECK(ring.first_available() == 0);

    REQUIRE(ring.reserve(64).has_value());
    CHECK(ring.write_index() == 1024);
    CHECK(ring.first_available() == 64);
}

TEST_CASE("a claim cannot reach past what has been published", "[engine][ring][m1]") {
    REVENANT_NEEDS_GPU();

    auto created = make_ring(1024);
    REQUIRE(created.has_value());
    auto& ring = *created;

    auto consumer = ring.add_consumer(engine::ConsumerKind::Lossy);
    REQUIRE(consumer.has_value());

    REQUIRE(ring.reserve(128).has_value());

    // Reserved is not readable. A consumer allowed to claim against the
    // reservation would be reading the samples the producer is mid-write on,
    // which is the tearing the two cursors exist to prevent.
    CHECK_FALSE(ring.claim(*consumer, 128).has_value());

    REQUIRE(ring.publish(128).has_value());
    CHECK(ring.claim(*consumer, 128).has_value());

    // And retiring more than was claimed would hand the writer a window
    // nobody ever opened.
    CHECK_FALSE(ring.retire(*consumer, 200).has_value());
    CHECK(ring.retire(*consumer, 128).has_value());

    const auto cursor = ring.cursor(*consumer);
    REQUIRE(cursor.has_value());
    CHECK(cursor->claimed == 128);
    CHECK(cursor->retired == 128);
}

TEST_CASE("a Blocking consumer holds the writer until it retires", "[engine][ring][m1]") {
    REVENANT_NEEDS_GPU();

    auto created = make_ring(1024);
    REQUIRE(created.has_value());
    auto& ring = *created;

    auto consumer = ring.add_consumer(engine::ConsumerKind::Blocking);
    REQUIRE(consumer.has_value());

    auto first = ring.reserve(1024);
    REQUIRE(first.has_value());
    CHECK(*first == 1024);
    REQUIRE(ring.publish(1024).has_value());
    REQUIRE(ring.claim(*consumer, 1024).has_value());

    // The floor is this consumer's retired cursor, still at zero, so the
    // whole ring is spoken for and the non-blocking reserve grants nothing.
    auto refused = ring.reserve(1);
    REQUIRE(refused.has_value());
    CHECK(*refused == 0);

    REQUIRE(ring.retire(*consumer, 512).has_value());

    auto after = ring.reserve(512);
    REQUIRE(after.has_value());
    CHECK(*after == 512);

    // Nothing was lost: a Blocking consumer that is behind costs throughput,
    // never samples. That is the difference between the two kinds.
    CHECK(ring.overrun_events() == 0);
    CHECK(ring.samples_lost() == 0);
}

TEST_CASE("a lapped Lossy consumer is moved forward and counted", "[engine][ring][m1]") {
    REVENANT_NEEDS_GPU();

    auto created = make_ring(1024);
    REQUIRE(created.has_value());
    auto& ring = *created;

    auto consumer = ring.add_consumer(engine::ConsumerKind::Lossy);
    REQUIRE(consumer.has_value());

    REQUIRE(ring.reserve(1024).has_value());
    REQUIRE(ring.publish(1024).has_value());

    // Lossy does not hold the writer. That is the definition of the kind and
    // the only legal one against a radio, whose clock does not wait.
    auto lapped = ring.reserve(256);
    REQUIRE(lapped.has_value());
    CHECK(*lapped == 256);

    // The consumer finds out two ways: its cursors have been snapped up to
    // the oldest sample still present, and the loss is counted. A ring that
    // lost samples and did not know it is worse than one that lost samples.
    const auto cursor = ring.cursor(*consumer);
    REQUIRE(cursor.has_value());
    CHECK(cursor->claimed == 256);
    CHECK(cursor->retired == 256);
    CHECK(ring.first_available() == 256);
    CHECK(ring.overrun_events() == 1);
    CHECK(ring.samples_lost() == 256);
}

TEST_CASE("a Demand producer parks until a Blocking consumer retires",
          "[engine][ring][m1]") {
    REVENANT_NEEDS_GPU();

    // The backpressure, which is the whole of faster than realtime: there is
    // no throttle anywhere in the engine, only this wait.
    auto created = make_ring(1024);
    REQUIRE(created.has_value());
    auto& ring = *created;

    auto consumer = ring.add_consumer(engine::ConsumerKind::Blocking);
    REQUIRE(consumer.has_value());

    REQUIRE(ring.reserve_blocking(1024).has_value());
    REQUIRE(ring.publish(1024).has_value());
    REQUIRE(ring.claim(*consumer, 1024).has_value());

    std::atomic<bool> returned{false};
    std::atomic<bool> took_it{false};
    std::thread producer([&] {
        const auto reserved = ring.reserve_blocking(512);
        took_it.store(reserved.has_value(), std::memory_order_relaxed);
        returned.store(true, std::memory_order_release);
    });

    // A sleep is the only way to observe "has not returned yet". It can
    // produce a false pass on a machine too loaded to have started the thread
    // at all, and it cannot produce a false failure, which is the right way
    // round for a test of something that must not happen.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(returned.load(std::memory_order_acquire));

    REQUIRE(ring.retire(*consumer, 512).has_value());
    producer.join();

    CHECK(took_it.load(std::memory_order_relaxed));
    CHECK(ring.reserved_index() == 1536);
}

TEST_CASE("the producer refuses a request no retirement could satisfy",
          "[engine][ring][m1]") {
    REVENANT_NEEDS_GPU();

    auto created = make_ring(1024);
    REQUIRE(created.has_value());
    auto& ring = *created;

    auto consumer = ring.add_consumer(engine::ConsumerKind::Blocking);
    REQUIRE(consumer.has_value());

    // Larger than the whole ring. Parking on it would be a deadlock wearing
    // backpressure's clothes, and it is refused before the wait rather than
    // discovered inside it.
    CHECK_FALSE(ring.reserve_blocking(2048).has_value());
}

TEST_CASE("a ring that no longer exists says so rather than reading full",
          "[engine][ring][m1]") {
    REVENANT_NEEDS_GPU();

    auto created = make_ring(1024);
    REQUIRE(created.has_value());
    auto& ring = *created;

    const engine::DeviceRing moved = std::move(ring);

    // Reaching through the moved-from object on purpose, because that is the
    // state under test. Zero granted is what a full ring returns and a Paced
    // producer counts it as an overrun, so a ring that was moved from has to
    // be distinguishable from one that is merely busy. Otherwise a recording
    // made against the wrong object reads as lossy rather than as a bug.
    CHECK_FALSE(ring.reserve(8).has_value());
    CHECK_FALSE(ring.publish(8).has_value());
    CHECK_FALSE(ring.reserve_blocking(8).has_value());
    CHECK(moved.write_index() == 0);
}
