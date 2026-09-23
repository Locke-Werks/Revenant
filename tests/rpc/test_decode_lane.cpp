// core/rpc/decode_lane.h on its own: no engine and no socket.
//
// WHAT THESE CASES CLAIM
//
// A decode lane is where the server's decoders run since 2026-09-23, off the
// completion thread. The properties the server leans on are the ones a
// decoder cannot survive without: every chunk run exactly once and in the
// order posted, a full lane on a clock dropping and counting rather than
// waiting, a full lane on an unthrottled replay waiting rather than dropping,
// drain() returning only once what was posted has run, and a stopped lane
// refusing rather than running. The posting thread stands in for the
// completion thread, which is the lane's one producer.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/engine/engine.h"
#include "core/rpc/decode_lane.h"

using namespace revenant;

namespace {

[[nodiscard]] engine::AudioChunk chunk_at(std::uint64_t start, std::span<const float> samples) {
    engine::AudioChunk chunk;
    chunk.start = start;
    chunk.rate = 8'000;
    chunk.channels = 1;
    chunk.samples = samples;
    return chunk;
}

}  // namespace

TEST_CASE("a decode lane runs every chunk once, in order, on its own copy", "[rpc][lane]") {
    auto made = rpc::DecodeLane::create(L"test lane", 64);
    REQUIRE(made.has_value());
    rpc::DecodeLane& lane = **made;

    std::mutex lock;
    std::vector<std::uint64_t> starts;
    std::vector<float> firsts;
    std::thread::id ran_on;
    auto work = std::make_shared<const rpc::LaneWork>([&](const engine::AudioChunk& chunk) {
        const std::scoped_lock held(lock);
        starts.push_back(chunk.start);
        firsts.push_back(chunk.samples.front());
        ran_on = std::this_thread::get_id();
    });

    // The caller's buffer is overwritten straight after each post, as the
    // completion thread's readback scratch is, so a lane that read it late
    // rather than copying would see the wrong first sample.
    std::vector<float> scratch(160, 0.0F);
    for (std::uint64_t i = 0; i < 200; ++i) {
        scratch.assign(scratch.size(), static_cast<float>(i));
        REQUIRE(lane.post(work, chunk_at(i * 160, scratch), true));
        scratch.assign(scratch.size(), -1.0F);
    }
    REQUIRE(lane.drain(std::chrono::seconds(10)));

    const std::scoped_lock held(lock);
    REQUIRE(starts.size() == 200);
    for (std::uint64_t i = 0; i < 200; ++i) {
        CHECK(starts[i] == i * 160);
        CHECK(firsts[i] == static_cast<float>(i));
    }
    CHECK(ran_on != std::this_thread::get_id());

    const rpc::DecodeLaneStats stats = lane.stats();
    CHECK(stats.posted == 200);
    CHECK(stats.run == 200);
    CHECK(stats.dropped == 0);
}

TEST_CASE("a full lane drops for a source on a clock and waits for an unthrottled one",
          "[rpc][lane]") {
    auto made = rpc::DecodeLane::create(L"test lane", 4);
    REQUIRE(made.has_value());
    rpc::DecodeLane& lane = **made;

    // Held shut until the case opens it, so the lane fills.
    std::atomic<bool> open{false};
    std::atomic<std::uint64_t> ran{0};
    auto work = std::make_shared<const rpc::LaneWork>([&](const engine::AudioChunk&) {
        while (!open.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ran.fetch_add(1);
    });

    const std::vector<float> samples(16, 0.5F);
    std::uint64_t accepted = 0;
    for (int i = 0; i < 12; ++i) {
        accepted += lane.post(work, chunk_at(static_cast<std::uint64_t>(i) * 16, samples), false)
                        ? 1U
                        : 0U;
    }

    // Four jobs, one of them running and stuck: everything past the fourth
    // is dropped and counted, and nothing waited.
    CHECK(accepted == 4);
    CHECK(lane.stats().dropped == 8);
    CHECK(lane.stats().waits == 0);

    // An unthrottled post on the full lane waits until a job comes back, then
    // goes in. Opened from another thread after a pause, so the post has to
    // have waited for it.
    std::thread opener([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        open.store(true);
    });
    CHECK(lane.post(work, chunk_at(12 * 16, samples), true));
    opener.join();
    CHECK(lane.stats().waits >= 1);

    REQUIRE(lane.drain(std::chrono::seconds(10)));
    CHECK(ran.load() == 5);
    CHECK(lane.stats().dropped == 8);
}

TEST_CASE("a stopped lane refuses what it is given and runs nothing more", "[rpc][lane]") {
    auto made = rpc::DecodeLane::create(L"test lane", 8);
    REQUIRE(made.has_value());
    rpc::DecodeLane& lane = **made;

    std::atomic<std::uint64_t> ran{0};
    auto work =
        std::make_shared<const rpc::LaneWork>([&](const engine::AudioChunk&) { ran.fetch_add(1); });
    const std::vector<float> samples(16, 0.5F);
    REQUIRE(lane.post(work, chunk_at(0, samples), false));
    REQUIRE(lane.drain(std::chrono::seconds(10)));
    CHECK(ran.load() == 1);

    lane.stop();
    lane.stop();
    CHECK_FALSE(lane.post(work, chunk_at(16, samples), true));
    CHECK_FALSE(lane.post(work, chunk_at(32, samples), false));
    CHECK(ran.load() == 1);

    // The work's only other owner is this case, so the lane let go of it.
    CHECK(work.use_count() == 1);
}
