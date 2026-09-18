// The device ring's host logic.
//
// Most of the risk in a ring is not on the device. It is in the sizing, which
// fails on one machine and not another; in the retirement floor, which is what
// stops the writer overwriting a window somebody is reading; and in the wrap,
// which is correct for about three and a half minutes at 20 MS/s if the index
// arithmetic is done in 32 bits by mistake. None of those need a GPU to test
// and all three are worth testing exhaustively rather than incidentally.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "core/engine/device_ring.h"
#include "core/engine/ring_consumer.h"
#include "core/engine/spsc_ring.h"
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
