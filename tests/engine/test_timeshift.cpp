// The time-shift buffer and its replay, core/engine/timeshift.h and
// core/engine/timeshift_replay.h.
//
// The buffer cases need no device: the risk there is the wrap arithmetic, the
// eviction, a gap's zeros and the rule that a retune or a restarted stream
// empties it, and all of that is host logic that fails silently. The last case
// does need one, because "a receiver opened on the past hears something" is a
// claim about a second graph actually running.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/engine/engine.h"
#include "core/engine/timeshift.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 1'000;
constexpr std::int64_t kAnchorNs = 1'700'000'000'000'000'000;

// A cu8 block whose every byte pair is the low 8 bits of its absolute index
// twice, so a read can be checked sample by sample against the index.
struct Block {
    std::vector<std::byte> bytes;
    source::SourceBlock block;
};

Block make_block(dsp::SampleIndex start, std::size_t count) {
    Block out;
    out.bytes.resize(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        const auto value = static_cast<std::byte>((start + i) & 0xFF);
        out.bytes[2 * i] = value;
        out.bytes[2 * i + 1] = value;
    }
    out.block.stamp.start = start;
    out.block.stamp.epoch_anchor_ns = kAnchorNs;
    out.block.stamp.rate = kRate;
    out.block.format = source::SampleFormat::Cu8;
    out.block.sample_count = count;
    out.block.bytes = out.bytes;
    return out;
}

void push(engine::TimeShiftBuffer& buffer, dsp::SampleIndex start, std::size_t count,
          dsp::Hertz center = 100'000'000) {
    const Block made = make_block(start, count);
    buffer.push(made.block, center);
}

engine::TimeShiftBuffer& ten_second_buffer(engine::TimeShiftBuffer& buffer) {
    buffer.set_limits(engine::TimeShiftLimits{.seconds = 10.0, .budget_bytes = 1ULL << 20});
    buffer.begin_stream(kRate, source::SampleFormat::Cu8);
    return buffer;
}

}  // namespace

TEST_CASE("the time-shift capacity is the smaller of the two limits",
          "[engine][timeshift]") {
    using engine::TimeShiftLimits;
    const auto cu8 = source::SampleFormat::Cu8;
    const auto cf32 = source::SampleFormat::Cf32;

    // Two minutes of 2.4 MS/s cu8 fits a 2 GiB budget, so time decides.
    CHECK(engine::timeshift_capacity_samples(TimeShiftLimits{}, 2'400'000, cu8) ==
          120ULL * 2'400'000);

    // At 20 MS/s cf32 two minutes is 19.2 GB, so the budget decides.
    CHECK(engine::timeshift_capacity_samples(TimeShiftLimits{}, 20'000'000, cf32) ==
          (2ULL << 30) / 8);

    CHECK(engine::timeshift_capacity_samples(TimeShiftLimits{.seconds = 0.0}, kRate, cu8) == 0);
    CHECK(engine::timeshift_capacity_samples(TimeShiftLimits{.seconds = -1.0}, kRate, cu8) ==
          0);
    CHECK(engine::timeshift_capacity_samples(TimeShiftLimits{}, 0, cu8) == 0);
}

TEST_CASE("the time-shift buffer reads back what it was given, across the wrap",
          "[engine][timeshift]") {
    engine::TimeShiftBuffer buffer;
    ten_second_buffer(buffer);
    CHECK_FALSE(buffer.extent().available);

    // 10 000 samples of capacity, fed 25 000: the oldest 15 000 are gone.
    for (dsp::SampleIndex at = 0; at < 25'000; at += 1'000) {
        push(buffer, at, 1'000);
    }
    const engine::TimeShiftExtent extent = buffer.extent();
    REQUIRE(extent.available);
    CHECK(extent.oldest_index == 15'000);
    CHECK(extent.newest_index == 25'000);
    CHECK(extent.oldest_ns == kAnchorNs + 15'000'000'000);
    CHECK(extent.newest_ns == kAnchorNs + 25'000'000'000);
    CHECK(extent.capacity_seconds == 10.0);
    CHECK(extent.center == 100'000'000);

    std::vector<std::byte> out(3'000 * 2);
    // Straddles the ring's wrap at 20 000.
    REQUIRE(buffer.read(19'000, 3'000, out) == 3'000);
    for (std::size_t i = 0; i < 3'000; ++i) {
        INFO("sample " << i);
        REQUIRE(out[2 * i] == static_cast<std::byte>((19'000 + i) & 0xFF));
    }

    CHECK(buffer.read(14'999, 10, out) == 0);
    CHECK(buffer.read(25'000, 10, out) == 0);
    CHECK(buffer.read(24'995, 10, out) == 5);
}

TEST_CASE("a gap in the stream reads back as zeros", "[engine][timeshift]") {
    engine::TimeShiftBuffer buffer;
    ten_second_buffer(buffer);
    push(buffer, 0, 1'000);
    push(buffer, 1'500, 500);

    const engine::TimeShiftExtent extent = buffer.extent();
    CHECK(extent.oldest_index == 0);
    CHECK(extent.newest_index == 2'000);

    std::vector<std::byte> out(500 * 2);
    REQUIRE(buffer.read(1'000, 500, out) == 500);
    for (const std::byte value : out) {
        REQUIRE(value == std::byte{0});
    }
    REQUIRE(buffer.read(1'500, 1, out) == 1);
    CHECK(out[0] == static_cast<std::byte>(1'500 & 0xFF));
}

TEST_CASE("a retune or a restarted stream empties the time-shift buffer",
          "[engine][timeshift]") {
    engine::TimeShiftBuffer buffer;
    ten_second_buffer(buffer);
    push(buffer, 0, 1'000);
    push(buffer, 1'000, 1'000, 101'000'000);
    engine::TimeShiftExtent extent = buffer.extent();
    CHECK(extent.oldest_index == 1'000);
    CHECK(extent.center == 101'000'000);

    push(buffer, 0, 500, 101'000'000);
    extent = buffer.extent();
    CHECK(extent.oldest_index == 0);
    CHECK(extent.newest_index == 500);

    buffer.clear();
    CHECK_FALSE(buffer.extent().available);
}

TEST_CASE("a time-shift instant maps to the sample taken then", "[engine][timeshift]") {
    engine::TimeShiftBuffer buffer;
    ten_second_buffer(buffer);
    push(buffer, 5'000, 5'000);

    CHECK(buffer.index_at(0) == 5'000);
    CHECK(buffer.index_at(kAnchorNs + 7'000'000'000) == 7'000);
    CHECK(buffer.index_at(kAnchorNs + 7'000'500'000) == 7'001);
    CHECK(buffer.index_at(kAnchorNs + 99'000'000'000) == 10'000);
    CHECK(buffer.wall_ns_at(8'000) == kAnchorNs + 8'000'000'000);
}

TEST_CASE("new limits reallocate and a format the buffer was not kept for is ignored",
          "[engine][timeshift]") {
    engine::TimeShiftBuffer buffer;
    ten_second_buffer(buffer);
    push(buffer, 0, 1'000);
    buffer.set_limits(engine::TimeShiftLimits{.seconds = 2.0, .budget_bytes = 1ULL << 20});
    engine::TimeShiftExtent extent = buffer.extent();
    CHECK_FALSE(extent.available);
    CHECK(extent.capacity_seconds == 2.0);
    CHECK(extent.capacity_bytes == 4'000);

    Block wrong = make_block(0, 10);
    wrong.block.format = source::SampleFormat::Cs16;
    buffer.push(wrong.block, 100'000'000);
    CHECK_FALSE(buffer.extent().available);

    buffer.release();
    push(buffer, 0, 10);
    CHECK_FALSE(buffer.extent().available);
}

TEST_CASE("a receiver opened on the time-shift replay hears the past",
          "[gpu][engine][timeshift]") {
    REVENANT_NEEDS_GPU();

    engine::EngineConfig config;
    config.channels = 64;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = 8'192;
    config.spectrum_transform = 512;
    config.gpu_index = -1;
    config.pace = 1.0;
    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;

    // Six seconds of 2.4 MS/s at real time, so a replay has history to start
    // from well before the stream ends.
    const std::string uri =
        "synthetic:wideband?rate=2400000&emitters=1&modes=am&seed=7&noise_dbfs=-90"
        "&snr_min=30&snr_max=30&samples=14400000";
    REQUIRE(eng.open_source(uri).has_value());

    // Before any stream the buffer holds nothing and a replay is refused.
    auto empty = eng.timeshift(std::nullopt);
    REQUIRE(empty.has_value());
    CHECK_FALSE(empty->available);
    CHECK_FALSE(eng.start_timeshift_replay(0, 1.0).has_value());

    std::mutex lock;
    std::size_t live_frames = 0;
    std::size_t replay_frames = 0;
    std::int64_t first_replay_ns = 0;
    REQUIRE(eng.set_spectrum_sink([&](const engine::SpectrumFrame& frame) -> Status {
                   const std::scoped_lock held(lock);
                   if (frame.timeshift_replay) {
                       if (replay_frames++ == 0) {
                           first_replay_ns = frame.timeshift_wall_ns;
                       }
                   } else {
                       ++live_frames;
                   }
                   return {};
               })
                .has_value());

    std::thread runner([&] { static_cast<void>(eng.run()); });

    // Two seconds of history.
    engine::TimeShiftExtent extent;
    for (int i = 0; i < 400; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto now = eng.timeshift(std::nullopt);
        REQUIRE(now.has_value());
        extent = *now;
        if (extent.available && extent.newest_ns - extent.oldest_ns > 2'000'000'000) {
            break;
        }
    }
    REQUIRE(extent.available);

    const std::int64_t from_ns = extent.oldest_ns + 100'000'000;
    auto started = eng.start_timeshift_replay(from_ns, 2.0);
    INFO(test::message_of(started));
    REQUIRE(started.has_value());
    CHECK(started->replaying);
    CHECK(started->replay_speed == 2.0);

    engine::VrxParams params;
    params.demod = engine::Demod::Am;
    params.bandwidth = 0;
    params.timeshift_replay = true;
    auto id = eng.add_vrx(params);
    INFO(test::message_of(id));
    REQUIRE(id.has_value());
    CHECK(eng.is_timeshift_vrx(*id));
    CHECK(eng.vrx_status(*id).has_value());

    // A live receiver keeps its own graph, and cannot be moved onto the replay.
    engine::VrxParams live = params;
    live.timeshift_replay = false;
    auto live_id = eng.add_vrx(live);
    REQUIRE(live_id.has_value());
    CHECK_FALSE(eng.is_timeshift_vrx(*live_id));
    CHECK_FALSE(eng.set_vrx_params(*live_id, params).has_value());

    std::atomic<std::size_t> audio{0};
    REQUIRE(eng.attach_audio_sink(*id, [&](const engine::AudioChunk& chunk) -> Status {
                   audio.fetch_add(chunk.samples.size(), std::memory_order_relaxed);
                   return {};
               })
                .has_value());

    for (int i = 0; i < 300 && audio.load() < 24'000; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(audio.load() >= 24'000);

    auto during = eng.timeshift(std::nullopt);
    REQUIRE(during.has_value());
    CHECK(during->replaying);
    CHECK(during->replay_ns >= from_ns);

    REQUIRE(eng.stop_timeshift_replay().has_value());
    CHECK_FALSE(eng.vrx_status(*id).has_value());
    CHECK_FALSE(eng.timeshift(std::nullopt)->replaying);

    static_cast<void>(eng.stop());
    runner.join();

    const std::scoped_lock held(lock);
    CHECK(live_frames > 0);
    CHECK(replay_frames > 0);
    // The replay's first frame is from about where it was asked to start,
    // allowing for the spectrum window and the filter's delay.
    CHECK(first_replay_ns >= from_ns - 50'000'000);
    CHECK(first_replay_ns < from_ns + 500'000'000);
}

TEST_CASE("the time-shift replay's spectrum is of the samples it says it is",
          "[gpu][engine][timeshift]") {
    REVENANT_NEEDS_GPU();

    engine::EngineConfig config;
    config.channels = 64;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = 8'192;
    config.spectrum_transform = 512;
    config.gpu_index = -1;
    config.pace = 1.0;
    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;

    // Several bursty emitters, so the band changes from moment to moment and
    // a spectrum names the instant it was taken.
    const std::string uri =
        "synthetic:wideband?rate=2400000&emitters=6&modes=am&seed=11&noise_dbfs=-90"
        "&snr_min=20&snr_max=40&samples=19200000";
    REQUIRE(eng.open_source(uri).has_value());

    struct Row {
        std::uint64_t start = 0;
        std::int64_t wall_ns = 0;
        std::vector<float> bins;
    };
    std::mutex lock;
    std::vector<Row> live;
    std::vector<Row> replay;
    REQUIRE(eng.set_spectrum_sink([&](const engine::SpectrumFrame& frame) -> Status {
                   const std::scoped_lock held(lock);
                   Row row{frame.start, frame.timeshift_wall_ns,
                           std::vector<float>(frame.power_db.begin(), frame.power_db.end())};
                   (frame.timeshift_replay ? replay : live).push_back(std::move(row));
                   return {};
               })
                .has_value());

    std::thread runner([&] { static_cast<void>(eng.run()); });

    engine::TimeShiftExtent extent;
    for (int i = 0; i < 600; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto now = eng.timeshift(std::nullopt);
        REQUIRE(now.has_value());
        extent = *now;
        if (extent.available && extent.newest_ns - extent.oldest_ns > 4'000'000'000) {
            break;
        }
    }
    REQUIRE(extent.available);

    // Three seconds behind live.
    const std::int64_t from_ns = extent.newest_ns - 3'000'000'000;
    REQUIRE(eng.start_timeshift_replay(from_ns, 1.0).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(1'000));
    REQUIRE(eng.stop_timeshift_replay().has_value());
    static_cast<void>(eng.stop());
    runner.join();

    const std::scoped_lock held(lock);
    REQUIRE(replay.size() > 10);
    REQUIRE(live.size() > 100);

    // Each replay row against every live row: the one it matches best is the
    // instant it was really computed from, whatever it is labelled.
    const auto distance = [](const std::vector<float>& a, const std::vector<float>& b) {
        double sum = 0.0;
        const std::size_t n = std::min(a.size(), b.size());
        for (std::size_t i = 0; i < n; ++i) {
            sum += std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        }
        return n == 0 ? 1.0e30 : sum / static_cast<double>(n);
    };
    const double rate = 2'400'000.0;
    std::size_t checked = 0;
    for (std::size_t r = replay.size() / 2; r < replay.size() && checked < 5; r += 3, ++checked) {
        std::size_t best = 0;
        double best_d = 1.0e30;
        for (std::size_t l = 0; l < live.size(); ++l) {
            const double d = distance(replay[r].bins, live[l].bins);
            if (d < best_d) {
                best_d = d;
                best = l;
            }
        }
        // Live index the replay row claims, from its wall clock.
        const double claimed =
            static_cast<double>(extent.oldest_index) +
            static_cast<double>(replay[r].wall_ns - extent.oldest_ns) * rate / 1.0e9;
        const double matched = static_cast<double>(live[best].start);
        const double seconds_off = (matched - claimed) / rate;
        UNSCOPED_INFO("replay row " << r << ": best live match is " << seconds_off
                                    << " s from where it claims to be, distance " << best_d
                                    << " dB");
        CHECK(std::abs(seconds_off) < 0.2);
    }
}
