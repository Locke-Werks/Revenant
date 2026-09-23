// The spectrum subscription, over a real socket, against a running engine.
//
// This is the half of the RPC surface with a clock in it, and every defect it
// can have is a timing defect: a frame queued instead of dropped, a sequence
// renumbered per subscription, a subscription that outlives the client that
// made it. None of those is visible without frames actually moving, so every
// case here runs the engine on its own thread and drives the client while it
// does.
//
// THE SOURCE IS PACED, WHICH IT IS NOT ANYWHERE ELSE IN THE SUITE
//
// A synthetic source is unthrottled by default and the engine retires it as
// fast as the GPU will, which for the other suites is the point. Here it
// would make a run a few tens of milliseconds long, which is not a window a
// case can subscribe inside, let a callback fall behind in, and then
// unsubscribe from. pace = 1 makes the frame rate the one the engine would
// have on a radio, so the numbers below are in seconds rather than in
// whatever the GPU happened to manage.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/dsp/spectrum_levels_reference.h"
#include "core/dsp/types.h"
#include "core/engine/engine.h"
#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/server.h"
#include "core/rpc/types.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"

using namespace revenant;
using Catch::Approx;
using test::FrameLog;
using test::FrameRecord;
using test::Harness;
using test::HarnessOptions;
using test::kChannels;
using test::kSpectrumTransform;

namespace {

// Two seconds of capture at 16384-sample blocks, which is about 146 frames a
// second and so about 290 frames a run. Enough that a decimated subscription
// still sees dozens and that a slow callback has something to fall behind.
constexpr dsp::SampleIndex kRunSamples = 4'800'064;
constexpr std::uint32_t kBlockSamples = 16'384;

[[nodiscard]] HarnessOptions streaming_options() {
    HarnessOptions options;
    options.spectrum_transform = kSpectrumTransform;
    options.samples = kRunSamples;
    options.pace = 1.0;
    options.block_samples = kBlockSamples;
    return options;
}

void bring_up(Harness& harness, const HarnessOptions& options) {
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());
}

// Waits until the log holds this many frames, or the deadline passes.
// Returns what it saw rather than a bool, so a case that did not get there
// can say how far it got.
[[nodiscard]] std::size_t wait_for_frames(const FrameLog& log, std::size_t wanted,
                                          int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (log.size() >= wanted) {
            return log.size();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return log.size();
}

[[nodiscard]] rpc::Client::FrameCallback into(std::shared_ptr<FrameLog> log) {
    // Captured by value, so the callback does not depend on which local the
    // case declared first. It is dropped by the client's event loop thread,
    // which is a worse place to find a dangling reference than here.
    return [log](const rpc::SpectrumFrame& frame) { log->record(frame); };
}

}  // namespace

TEST_CASE("spectrum frames stream, arrive in order, and carry their bins intact",
          "[gpu][rpc][spectrum][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    Harness harness;
    bring_up(harness, streaming_options());

    auto log = std::make_shared<FrameLog>();
    const auto subscribed = harness.client().subscribe_spectrum(1, into(log));
    INFO(test::message_of(subscribed));
    REQUIRE(subscribed.has_value());

    const std::size_t got = wait_for_frames(*log, 32, 6000);
    INFO(got << " frames arrived");
    REQUIRE(got >= 32);

    harness.client().unsubscribe_spectrum();
    const auto ran = harness.stop_engine();
    INFO(test::message_of(ran));
    CHECK(ran.has_value());

    const std::vector<FrameRecord> frames = log->frames();
    REQUIRE(frames.size() >= 32);

    const engine::SpectrumGeometry& geometry = harness.engine().info().spectrum;
    REQUIRE(geometry.enabled());

    for (std::size_t i = 0; i < frames.size(); ++i) {
        INFO("frame " << i << " of " << frames.size() << ", sequence " << frames[i].sequence);

        // Every bin of the span, no gaps and nothing counted twice. A copy
        // that stopped at a segment boundary would arrive short here and
        // nowhere else: a display would draw it as a band that ends early.
        CHECK(frames[i].bins == geometry.bins);
        CHECK(frames[i].geometry.bins == geometry.bins);
        CHECK(frames[i].geometry.channels == geometry.channels);
        CHECK(frames[i].geometry.transform == geometry.transform);
        CHECK(frames[i].geometry.bins_per_channel == geometry.bins_per_channel);
        CHECK(frames[i].geometry.bin_width.numerator == geometry.bin_width_numerator);
        CHECK(frames[i].geometry.bin_width.denominator == geometry.bin_width_denominator);
        CHECK(frames[i].geometry.bin_zero.numerator == geometry.bin_zero_numerator);
        CHECK(frames[i].geometry.bin_zero.denominator == geometry.bin_zero_denominator);

        // The window this frame covers, in source samples. A frame whose
        // count is zero covers nothing and is a frame the sink invented.
        CHECK(frames[i].count > 0);

        if (i > 0) {
            // In order and never repeated. Cap'n Proto delivers calls on one
            // connection in order, so a sequence that went backwards would
            // mean the server fanned out from the wrong slot.
            CHECK(frames[i].sequence > frames[i - 1].sequence);
            CHECK(frames[i].start > frames[i - 1].start);
        }
    }

    // Nothing re-entered the callback, which is what makes the counter below
    // readable. See the backpressure case for why.
    CHECK_FALSE(log->reentered());
    CHECK(harness.client().frames_received() >= frames.size());

    // The bins themselves, as far as a check that holds on any device can
    // take it. The referee that can say more is the case below, and it needs
    // a device that reproduces a shared-memory transform.
    std::size_t inspected = 0;
    for (const FrameRecord& frame : frames) {
        if (frame.power_db.empty()) {
            continue;  // past kKeptFrames, only the summary was kept
        }
        ++inspected;
        INFO("sequence " << frame.sequence);

        REQUIRE(frame.power_db.size() == geometry.bins);

        // Finite, and inside the range the kernel's own clamp guarantees:
        // power is clamped at 1e-20 before the logarithm, so nothing is
        // below -200 dB, and the histogram that measures the frame cannot
        // represent anything above +40 dBFS. A buffer that arrived as
        // uninitialised memory lands outside this almost every time.
        CHECK(std::ranges::all_of(frame.power_db, [](float value) {
            return std::isfinite(value) && value >= dsp::kSpectrumLevelsRangeFloorDb &&
                   value <= dsp::kSpectrumLevelsRangeCeilingDb;
        }));

        // Structure rather than one value repeated, which is what a scene
        // with six emitters over a quiet floor produces and what a
        // zero-filled or default-filled buffer would not.
        CHECK(std::ranges::any_of(frame.power_db, [&](float value) {
            return value != frame.power_db.front();
        }));

        // The four level fields, checked for the one property that is about
        // the copy rather than about the measurement: that each one arrived.
        // A float dropped in conversion reads as exactly 0.0, and no real
        // value here can be: the percentiles are bucket centres, which are
        // -200 + 0.234375 * (k + 0.5) and so never land on zero, and the
        // smoothed ends are floats tracking them.
        //
        // Nothing stronger can be asserted here, and the reason is the
        // device rather than the wire. On the integrated part in this
        // machine, eight runs in forty, one frame of about thirty-five
        // carries a percentile from the wrong histogram bucket: +5.195,
        // +4.258, -199.883 dBFS on a scene whose bins are all near -150.
        // Every one of those is an exact bucket centre, buckets 875, 871
        // and 0, which is what says the wire carried precisely what the
        // device wrote and the device wrote the wrong bucket. Ordering and
        // the value itself are therefore asked in the refereed case below,
        // which runs where that measurement can be trusted.
        struct NamedLevel {
            const char* what;
            float value;
        };
        for (const NamedLevel& level :
             {NamedLevel{"floor_db", frame.floor_db},
              NamedLevel{"ceiling_db", frame.ceiling_db},
              NamedLevel{"percentile_low_db", frame.percentile_low_db},
              NamedLevel{"percentile_high_db", frame.percentile_high_db}}) {
            INFO(level.what << " was " << level.value << " dBFS");
            CHECK(std::isfinite(level.value));
            CHECK(level.value != 0.0F);
        }
    }

    INFO(inspected << " frames had their bins kept whole");
    CHECK(inspected >= 8);
}

TEST_CASE("the bins a frame carries are the bins its percentiles were measured from",
          "[gpu][rpc][spectrum][m1]") {
    REVENANT_NEEDS_GPU();

    // The strongest thing that can be said about a copy: a frame carries the
    // two percentiles core/shaders/spectrum_levels.comp measured of that
    // frame's own bins, on the device, before anything was copied.
    // Recomputing them here from the bins that arrived asks whether these
    // are the values the measurement was made of. An array truncated,
    // reordered, byte-swapped or filled with a default misses by tens of
    // decibels; a check that the floats are finite would not notice any of
    // it.
    //
    // WHY THIS IS GATED ON THE DEVICE AND THE CASE ABOVE IS NOT
    //
    // It makes the device's own measurement the referee, so it can only run
    // where that measurement is reproducible.
    //
    // Measured 2026-09-19 with the gate lifted for the measurement. Six runs
    // on the RTX 4090 passed every assertion, 103 a run. Five runs on the
    // integrated AMD part each failed one to six assertions of about a
    // hundred, a minority of the frames every time. Both failures inspected
    // in detail were the high percentile, out by 10.8 dB and by 1.2 dB, with
    // the low percentile of the same frame matching exactly and the bins
    // themselves reading the same as a healthy run's.
    //
    // A copy that lost or reordered bins could not miss one percentile and
    // hit the other, could not do it on a minority of frames, and could not
    // do it on one device and not the other. This is the fault
    // tests/reference/gpu_fixture.h and docs/fft.md record for this device,
    // reached through a different kernel, and asserting it here would report
    // a driver defect as an RPC regression. The twelve cases in
    // tests/reference that gate on the same probe skip for the same reason.
    REVENANT_NEEDS_REPRODUCIBLE_SHARED_MEMORY();
    INFO("running on " << test::shared_context_description());

    Harness harness;
    bring_up(harness, streaming_options());

    auto log = std::make_shared<FrameLog>();
    REQUIRE(harness.client().subscribe_spectrum(1, into(log)).has_value());

    const std::size_t got = wait_for_frames(*log, 24, 6000);
    INFO(got << " frames arrived");
    REQUIRE(got >= 24);

    harness.client().unsubscribe_spectrum();
    CHECK(harness.stop_engine().has_value());

    const engine::SpectrumGeometry& geometry = harness.engine().info().spectrum;
    REQUIRE(geometry.enabled());

    dsp::SpectrumLevelsParams params;
    params.bins = geometry.bins;

    std::size_t checked = 0;
    for (const FrameRecord& frame : log->frames()) {
        if (frame.power_db.empty()) {
            continue;
        }
        REQUIRE(frame.power_db.size() == geometry.bins);

        float levels[dsp::kSpectrumLevelsOutputs] = {0.0F, 0.0F};
        const auto measured = dsp::reference_spectrum_levels(
            params, dsp::ConstRealSpan(frame.power_db),
            dsp::RealSpan(levels, dsp::kSpectrumLevelsOutputs));
        INFO(test::message_of(measured));
        REQUIRE(measured.has_value());

        INFO(std::format("sequence {} said {} / {} dBFS, its bins say {} / {}", frame.sequence,
                         frame.percentile_low_db, frame.percentile_high_db, levels[0],
                         levels[1]));

        // Within one histogram bucket. Whether the kernel and its host twin
        // agree to the bit is tests/reference/test_spectrum_levels.cpp's
        // question; the one here is whether the bins that arrived are the
        // bins the device measured, and a quarter of a decibel of
        // quantisation is the only gap this tolerates.
        CHECK(std::abs(levels[0] - frame.percentile_low_db) <=
              dsp::kSpectrumLevelsDbPerBucket);
        CHECK(std::abs(levels[1] - frame.percentile_high_db) <=
              dsp::kSpectrumLevelsDbPerBucket);

        // Two fields, carried the right way round. A copy that swapped them
        // would reproduce both values and still hand a display a colour map
        // with its ends inverted.
        CHECK(frame.percentile_high_db > frame.percentile_low_db);

        // And inside the range the histogram can represent, which is a
        // statement about the device's answer and so belongs here rather
        // than in the case above.
        CHECK(frame.percentile_low_db >= dsp::kSpectrumLevelsRangeFloorDb);
        CHECK(frame.percentile_high_db <= dsp::kSpectrumLevelsRangeCeilingDb);
        ++checked;
    }

    INFO(checked << " frames were refereed against their own percentiles");
    CHECK(checked >= 24);
}

TEST_CASE("every_nth decimates, and the sequence is the engine's count not the subscription's",
          "[gpu][rpc][spectrum][m1]") {
    REVENANT_NEEDS_GPU();

    constexpr std::uint32_t kEveryNth = 4;

    Harness harness;
    bring_up(harness, streaming_options());

    // Subscribed late on purpose. A sequence numbered per subscription would
    // start at zero here however long the engine had been running, and that
    // is the difference the schema's comment is about: a client knows what
    // decimation it asked for and cannot otherwise know whether the engine
    // also skipped.
    const std::uint64_t blocks_before = harness.wait_for_blocks(40, 4000);
    INFO(blocks_before << " blocks delivered before subscribing");
    REQUIRE(blocks_before >= 40);

    auto log = std::make_shared<FrameLog>();
    const auto subscribed = harness.client().subscribe_spectrum(kEveryNth, into(log));
    INFO(test::message_of(subscribed));
    REQUIRE(subscribed.has_value());

    const std::size_t got = wait_for_frames(*log, 16, 6000);
    INFO(got << " frames arrived");
    REQUIRE(got >= 16);

    harness.client().unsubscribe_spectrum();
    CHECK(harness.stop_engine().has_value());

    const std::vector<FrameRecord> frames = log->frames();
    REQUIRE(frames.size() >= 16);

    // The engine drops before it copies, so what arrives is exactly the
    // frames whose sequence the stride divides. A client that filtered its
    // own would have paid for every copy first, which is what everyNth
    // exists to avoid.
    for (const FrameRecord& frame : frames) {
        INFO("sequence " << frame.sequence);
        CHECK(frame.sequence % kEveryNth == 0);
    }

    std::uint64_t smallest_gap = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t i = 1; i < frames.size(); ++i) {
        const std::uint64_t gap = frames[i].sequence - frames[i - 1].sequence;
        CHECK(gap % kEveryNth == 0);
        smallest_gap = std::min(smallest_gap, gap);
    }

    // It decimated rather than delivering everything and numbering it in
    // fours: consecutive frames are four of the engine's apart at the
    // closest, never one.
    INFO("closest two frames were " << smallest_gap << " engine frames apart");
    CHECK(smallest_gap == kEveryNth);

    // And the count is the engine's. The first frame this subscription saw
    // is numbered somewhere near the number of blocks the engine had already
    // delivered, not near zero. Half of it rather than all of it because the
    // spectrum stage and the source's block counter are read at different
    // instants and one is allowed to be ahead.
    INFO(std::format("first sequence {} against {} blocks already delivered",
                     frames.front().sequence, blocks_before));
    CHECK(frames.front().sequence >= blocks_before / 2);
}

TEST_CASE("a slow subscriber makes the engine drop frames rather than queue them",
          "[gpu][rpc][spectrum][m1]") {
    REVENANT_NEEDS_GPU();

    // At the shipped geometry a frame is 256 KiB and the engine makes 305 a
    // second, so a client that stalled for two seconds would be 150 MB
    // behind if anything buffered on its behalf. server.h decides that at
    // most one frame is in flight per subscription and that the rest are
    // dropped and counted. The failure this case exists to catch is the
    // obvious implementation: a queue, which passes every other case in this
    // file and then eats the machine on a display that hitched.
    constexpr auto kCallbackDelay = std::chrono::milliseconds(25);

    Harness harness;
    bring_up(harness, streaming_options());

    auto log = std::make_shared<FrameLog>();
    log->set_callback_delay(kCallbackDelay);

    const auto subscribed = harness.client().subscribe_spectrum(1, into(log));
    INFO(test::message_of(subscribed));
    REQUIRE(subscribed.has_value());

    // The engine produces a frame every 6.8 ms at this block size and pace,
    // so a 25 ms callback cannot keep up and the gap is not marginal.
    const std::size_t got = wait_for_frames(*log, 16, 8000);
    INFO(got << " frames were handled by a " << kCallbackDelay.count() << " ms callback");
    REQUIRE(got >= 16);

    const std::uint64_t sent = harness.server().frames_sent();
    const std::uint64_t dropped = harness.server().frames_dropped();
    INFO(std::format("the server sent {} frames and dropped {}", sent, dropped));

    // Dropped, and counted. A display that is behind has to be able to say
    // so rather than lying about the band, which is the whole reason the
    // counter is on the interface at all.
    CHECK(dropped > 0);
    CHECK(sent >= got);

    // Newest wins, so the frames that did arrive skipped forward rather than
    // being a queue drained in order. At least one gap has to be bigger than
    // one for a drop to have happened at all.
    const std::vector<FrameRecord> frames = log->frames();
    REQUIRE(frames.size() >= 2);
    std::uint64_t largest_gap = 0;
    for (std::size_t i = 1; i < frames.size(); ++i) {
        REQUIRE(frames[i].sequence > frames[i - 1].sequence);
        largest_gap = std::max(largest_gap, frames[i].sequence - frames[i - 1].sequence);
    }
    INFO("the largest jump between two delivered frames was " << largest_gap);
    CHECK(largest_gap > 1);

    // The connection survived all of it. Backpressure that killed the
    // session would look like backpressure that worked, right up until the
    // display went blank.
    harness.client().unsubscribe_spectrum();

    auto still_there = harness.client().info();
    INFO(test::message_of(still_there));
    REQUIRE(still_there.has_value());
    CHECK(still_there->grid.channels == kChannels);

    auto counters = harness.client().source_stats();
    INFO(test::message_of(counters));
    REQUIRE(counters.has_value());

    CHECK(harness.stop_engine().has_value());

    // client.h describes this counter as frames dropped because the callback
    // was still running when the next one arrived, and it reads zero here on
    // purpose rather than by accident. The callback runs inline inside
    // frame(), which is not answered until it returns, so the engine cannot
    // start a second frame while the first is being handled. That is exactly
    // the backpressure server.h asks for, and it means the drop belongs to
    // the engine and is counted there. Asserting it is zero is what stops a
    // future client growing a queue to make it non-zero.
    CHECK(harness.client().frames_dropped() == 0);
    CHECK_FALSE(log->reentered());
}

TEST_CASE("a client that goes away mid-stream takes its subscription and nothing else",
          "[gpu][rpc][spectrum][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    {
        HarnessOptions options = streaming_options();
        options.samples = 9'600'128;  // four seconds, so the engine outlives the client
        options.connect_client = false;
        const auto ready = harness.open(options);
        INFO(test::message_of(ready));
        REQUIRE(ready.has_value());

        const auto started = harness.start_engine();
        INFO(test::message_of(started));
        REQUIRE(started.has_value());
    }

    auto log = std::make_shared<FrameLog>();
    std::uint64_t sent_while_subscribed = 0;

    {
        auto doomed = harness.connect_another();
        INFO(test::message_of(doomed));
        REQUIRE(doomed.has_value());

        const auto subscribed = (*doomed)->subscribe_spectrum(1, into(log));
        INFO(test::message_of(subscribed));
        REQUIRE(subscribed.has_value());

        const std::size_t got = wait_for_frames(*log, 16, 6000);
        INFO(got << " frames arrived before the client was destroyed");
        REQUIRE(got >= 16);

        sent_while_subscribed = harness.server().frames_sent();

        // No unsubscribe. The client is destroyed with a subscription live
        // and frames on the wire, which is what a display that crashed looks
        // like from here. Dropping the capability is what ends it, and the
        // schema says so precisely because a client that died cannot call
        // cancel.
    }

    // Waits for the counter to settle rather than asserting on one fixed
    // window. The socket has to be noticed closed, the last send has to
    // resolve and the subscription has to be dropped, and how long that
    // takes is not this case's business. What is, is that it stops: so this
    // waits for it to stop and then requires it to stay stopped.
    std::uint64_t settled = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        const std::uint64_t before = harness.server().frames_sent();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (harness.server().frames_sent() == before) {
            settled = before;
            break;
        }
    }

    // The subscription is gone: the counter stopped moving. A server that
    // kept fanning out to a dead capability would keep incrementing this
    // and would keep paying for the copy on every frame.
    INFO(std::format("{} sent while subscribed, settled at {}", sent_while_subscribed,
                     settled));
    REQUIRE(settled != 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const std::uint64_t later = harness.server().frames_sent();
    INFO(later << " half a second after that");
    CHECK(settled == later);

    // And it stopped promptly rather than eventually. The engine makes about
    // 146 frames a second at this block size and pace, so a subscription
    // that outlived its client by even the length of the settling loop above
    // would show hundreds here.
    CHECK(later < sent_while_subscribed + 40);

    // The engine is still running and the server is still serving. A
    // disconnect that took either down is the failure a multi-client service
    // cannot have, and nothing about it would be visible to the client that
    // caused it.
    CHECK(harness.engine().running());

    auto survivor = harness.connect_another();
    INFO(test::message_of(survivor));
    REQUIRE(survivor.has_value());

    auto info = (*survivor)->info();
    INFO(test::message_of(info));
    REQUIRE(info.has_value());
    CHECK(info->grid.channels == kChannels);

    // And a new subscription still works, so what went away was the
    // subscription rather than the server's ability to make one.
    auto second_log = std::make_shared<FrameLog>();
    const auto resubscribed = (*survivor)->subscribe_spectrum(1, into(second_log));
    INFO(test::message_of(resubscribed));
    REQUIRE(resubscribed.has_value());

    const std::size_t after = wait_for_frames(*second_log, 8, 6000);
    INFO(after << " frames arrived on the replacement subscription");
    CHECK(after >= 8);

    (*survivor)->unsubscribe_spectrum();
    CHECK(harness.stop_engine().has_value());
}

TEST_CASE("subscribing again replaces the first subscription", "[gpu][rpc][spectrum][m1]") {
    REVENANT_NEEDS_GPU();

    // WHY THIS ASKS THE SERVER AND NOT THE OLD CALLBACK
    //
    // The obvious case watches the first callback go quiet after the second
    // subscribe and calls that a replacement. It cannot fail. ClientImpl
    // holds one callback_, so the instant the second subscribe installs its
    // own the first log is unreachable whatever the server did: a server
    // that kept both subscriptions alive would fan out to two receivers,
    // both of which route into that same one callback_, and the first log
    // would sit at exactly the same size. A check that passes against the
    // failure it is named after is worse than no check, because it reads as
    // coverage.
    //
    // So the two assertions below are about the SERVER's state. Neither is
    // reachable through the old callback and both are things a second live
    // subscription would break.
    //
    //   Sequence numbers on the live log stay strictly increasing. Two
    //   subscriptions on one connection deliver the same frame twice, and
    //   both deliveries land on the one callback, so the log would hold each
    //   engine sequence twice over.
    //
    //   Server::frames_sent counts one delivery per subscription per frame.
    //   Measured against the engine's own frame numbers, which the frames
    //   themselves carry, it says how many subscriptions the fan-out walked.
    //
    // A subscription count on the Server interface would say this in one
    // line. frames_sent is on the interface already and answers the same
    // question, so this does not ask for a new accessor.
    constexpr auto kWindow = std::chrono::milliseconds(500);

    // One delivery per engine frame is the whole claim. The two slack frames
    // are because the counter and the log cannot be read at one instant: a
    // frame counted as sent is recorded by the client a moment later. Two
    // live subscriptions double the count; they do not exceed it by two.
    //
    // Measured 2026-09-19, one subscription: 73 engine frames and 73
    // deliveries in this window on the RTX 4090, 73 to 75 of each over three
    // runs on the integrated AMD part. The bound below is therefore clear of
    // the real figure by two and clear of a doubled one by about seventy.
    constexpr std::uint64_t kReadSlack = 2;

    Harness harness;
    {
        HarnessOptions options = streaming_options();
        // Four seconds. The window below is measured rather than assumed, so
        // the run has to outlast two subscriptions settling plus half a
        // second of counting.
        options.samples = 9'600'128;
        bring_up(harness, options);
    }

    auto first = std::make_shared<FrameLog>();
    REQUIRE(harness.client().subscribe_spectrum(1, into(first)).has_value());
    REQUIRE(wait_for_frames(*first, 8, 6000) >= 8);

    auto second = std::make_shared<FrameLog>();
    const auto replaced = harness.client().subscribe_spectrum(1, into(second));
    INFO(test::message_of(replaced));
    REQUIRE(replaced.has_value());

    REQUIRE(wait_for_frames(*second, 8, 6000) >= 8);

    // The sequence is read before the counter and the counter before the
    // sequence on the way out, so the counter's window sits inside the span
    // of engine frames it is compared against rather than straddling it.
    const std::vector<FrameRecord> opening = second->frames();
    REQUIRE_FALSE(opening.empty());
    const std::uint64_t first_sequence = opening.back().sequence;
    const std::uint64_t sent_at_start = harness.server().frames_sent();

    std::this_thread::sleep_for(kWindow);

    const std::uint64_t sent_at_end = harness.server().frames_sent();
    const std::vector<FrameRecord> closing = second->frames();
    REQUIRE(closing.size() > opening.size());
    const std::uint64_t last_sequence = closing.back().sequence;

    const std::uint64_t engine_frames = last_sequence - first_sequence;
    const std::uint64_t sent = sent_at_end - sent_at_start;

    // A window with frames in it, so nothing below passes because the engine
    // had already finished.
    //
    // SIXTEEN, AND IT USED TO BE THIRTY-TWO, WHICH NO DEBUG BUILD REACHES. The
    // 73 frames in the kReadSlack note above were measured in RelWithDebInfo.
    // The dev preset is Debug and the same window holds 30 on the same 4090, so
    // a floor of 32 failed every Debug run of this case by two frames while CI
    // reported it green.
    //
    // Sixteen rather than a second number under NDEBUG, because unlike the cost
    // ceiling in test_rpc_rds.cpp this floor is not a budget and does not have
    // to track the build. What it guards is that the window was not empty and
    // the engine was still producing, and the assertion it protects bites from
    // three frames upward: a second live subscription roughly doubles `sent`,
    // so `sent <= engine_frames + 2` fails for any engine_frames above two.
    // Sixteen is clear of that by five times and clear of both builds'
    // measurements by a wide margin in the other direction.
    INFO(std::format("{} engine frames over {} ms, {} deliveries, {} records",
                     engine_frames, kWindow.count(), sent,
                     closing.size() - opening.size()));
    REQUIRE(engine_frames >= 16);
    REQUIRE(sent > 0);

    CHECK(sent <= engine_frames + kReadSlack);

    // Every frame the live callback saw, once. A second subscription would
    // show up here as a repeated sequence rather than as a gap.
    for (std::size_t i = 1; i < closing.size(); ++i) {
        INFO("record " << i << " of " << closing.size());
        CHECK(closing[i].sequence > closing[i - 1].sequence);
    }

    harness.client().unsubscribe_spectrum();
    CHECK(harness.stop_engine().has_value());
}

TEST_CASE("one receiver's passband streams and carries its own axis",
          "[gpu][rpc][spectrum][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The surface an operator drags a filter edge over. It has to arrive with
    // its own geometry rather than the span's, because its width is set by
    // the receiver's passband and steps when the passband crosses a rung of
    // the display rate's ladder. (That width "is the receiver's demodulation
    // rate and moves whenever the filter does", this used to say.)
    constexpr std::uint32_t kPassbandTransform = 512;
    constexpr std::int64_t kLow = 300;
    constexpr std::int64_t kHigh = 2'700;

    Harness harness;
    HarnessOptions options = streaming_options();
    options.passband_transform = kPassbandTransform;

    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    rpc::VrxParams params;
    params.center = 187'500;
    params.demod = rpc::Demod::Usb;
    params.passband_low = kLow;
    params.passband_high = kHigh;

    auto id = harness.client().add_vrx(params);
    INFO(test::message_of(id));
    REQUIRE(id.has_value());

    auto status = harness.client().vrx_status(*id);
    INFO(test::message_of(status));
    REQUIRE(status.has_value());
    const std::uint32_t demod_rate = status->demod_rate;
    REQUIRE(demod_rate > 0);

    struct Seen {
        std::mutex lock;
        std::vector<rpc::PassbandFrame> frames;
    };
    auto seen = std::make_shared<Seen>();

    const auto subscribed = harness.client().subscribe_passband(
        *id, 1, [seen](const rpc::PassbandFrame& frame) {
            std::scoped_lock held(seen->lock);
            if (seen->frames.size() < 64) {
                seen->frames.push_back(frame);
            }
        });
    INFO(test::message_of(subscribed));
    REQUIRE(subscribed.has_value());

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(8000);
    std::size_t got = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::scoped_lock held(seen->lock);
            got = seen->frames.size();
        }
        if (got >= 8) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    INFO(got << " passband frames arrived");
    REQUIRE(got >= 8);

    harness.client().unsubscribe_passband(*id);
    CHECK(harness.stop_engine().has_value());

    std::vector<rpc::PassbandFrame> frames;
    {
        std::scoped_lock held(seen->lock);
        frames = seen->frames;
    }
    REQUIRE(frames.size() >= 8);

    for (std::size_t i = 0; i < frames.size(); ++i) {
        INFO("frame " << i << " of " << frames.size());
        const rpc::PassbandFrame& frame = frames[i];

        CHECK(frame.vrx == *id);
        CHECK(frame.geometry.transform == kPassbandTransform);
        CHECK(frame.geometry.bins == kPassbandTransform / 2);
        CHECK(frame.power_db.size() == frame.geometry.bins);

        // The axis is the receiver's own and crosses the wire whole: the
        // frame is the central half of a transform of the display stream,
        // so it spans half the rate it carries, and bin zero sits a quarter
        // of that rate below what the fine stage mixed to DC, which for USB
        // is the receiver's own centre.
        //
        // WHAT THIS USED TO CHECK: that the frame's rate was the
        // demodulation rate vrxStatus reported, and that the frame spanned
        // all of it. The pane was the fine stream then. The display stream
        // runs at a rate of its own, chosen from the passband by the rule in
        // core/dsp/vrx_reference.h, and the frame's geometry is the only
        // place it is reported, which is why a client reads the axis from
        // the frame and never from vrxStatus.
        REQUIRE(frame.geometry.rate > 0);
        REQUIRE(frame.geometry.bin_width.denominator != 0);
        const double bin_width = frame.geometry.bin_width.hertz();
        const double span = bin_width * static_cast<double>(frame.geometry.bins);
        CHECK(span == Approx(0.5 * static_cast<double>(frame.geometry.rate)).epsilon(1.0e-9));

        REQUIRE(frame.geometry.bin_zero.denominator != 0);
        CHECK(frame.geometry.bin_zero.hertz() ==
              Approx(static_cast<double>(params.center) -
                     0.25 * static_cast<double>(frame.geometry.rate))
                  .margin(1.0));

        // And the pane holds the whole passband with room either side, which
        // is what a display dragging its edges needs of it.
        const double pane_low = frame.geometry.bin_zero.hertz() - 0.5 * bin_width;
        CHECK(pane_low < static_cast<double>(params.center + kLow));
        CHECK(pane_low + span > static_cast<double>(params.center + kHigh));

        CHECK(frame.count > 0);
        CHECK(std::ranges::all_of(frame.power_db, [](float value) {
            return std::isfinite(value);
        }));

        if (i > 0) {
            CHECK(frame.sequence > frames[i - 1].sequence);
        }
    }
}

TEST_CASE("a passband subscription is refused when there is nothing to transform",
          "[gpu][rpc][spectrum][m1]") {
    REVENANT_NEEDS_GPU();

    // passband_transform defaults to zero, so this engine has no passband
    // stage at all. Handing back a subscription that can never produce a
    // frame would leave a detail display waiting forever with nothing to say
    // why.
    Harness harness;
    HarnessOptions options;
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    rpc::VrxParams params;
    params.center = 187'500;
    params.demod = rpc::Demod::Nfm;

    auto id = harness.client().add_vrx(params);
    INFO(test::message_of(id));
    REQUIRE(id.has_value());

    const auto refused = harness.client().subscribe_passband(
        *id, 1, [](const rpc::PassbandFrame&) {});
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);
    CHECK(refused.error().message.find("passband") != std::string::npos);

    // An empty callback is not an unsubscribe here either.
    const auto empty =
        harness.client().subscribe_passband(*id, 1, rpc::Client::PassbandCallback{});
    REQUIRE_FALSE(empty.has_value());
    INFO(empty.error().message);
    CHECK(empty.error().message.find("callback") != std::string::npos);

    // And a receiver that does not exist is refused by id rather than by
    // being given a stream nothing will ever write to.
    const auto unknown = harness.client().subscribe_passband(
        *id + 9'999, 1, [](const rpc::PassbandFrame&) {});
    CHECK_FALSE(unknown.has_value());
}

TEST_CASE("an engine with no spectrum stage refuses a subscription with its reason",
          "[gpu][rpc][spectrum][m1]") {
    REVENANT_NEEDS_GPU();

    // spectrum_transform defaults to zero, which builds no spectrum stage at
    // all. Handing back a subscription that can never produce a frame would
    // leave a display waiting forever with nothing to say why.
    Harness harness;
    HarnessOptions options;
    options.spectrum_transform = 0;
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    auto log = std::make_shared<FrameLog>();
    const auto refused = harness.client().subscribe_spectrum(1, into(log));
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);
    CHECK(refused.error().message.find("spectrum") != std::string::npos);

    CHECK(harness.server().frames_sent() == 0);
    CHECK(log->size() == 0);

    // An empty callback is not an unsubscribe, and is refused rather than
    // silently treated as one.
    const auto empty = harness.client().subscribe_spectrum(1, rpc::Client::FrameCallback{});
    REQUIRE_FALSE(empty.has_value());
    INFO(empty.error().message);
    CHECK(empty.error().message.find("callback") != std::string::npos);
}
