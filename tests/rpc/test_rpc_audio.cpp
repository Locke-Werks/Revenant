// One receiver's audio, over a real socket, against a running engine.
//
// This file replaced the subscribeAudio case in tests/rpc/test_rpc_unwired.cpp
// on 2026-09-20. That case asserted the server said the surface was not
// wired, and existed so that the branch wiring it would have to come here and
// delete something. This is what it was deleted for.
//
// THE REACHABLE SHAPES, ENUMERATED BEFORE THE BAR AND NOT AFTER
//
// A subscription that certifies one path while reading as though it covers
// the surface is the failure this project keeps having, so the shapes were
// listed first and each one has a case:
//
//   1. no such receiver                          refused, in the engine's words
//   2. subscribe twice to one receiver           the second replaces the first
//   3. two clients on one receiver               both stream, one engine sink
//   4. the receiver removed mid-stream           ended() with a reason
//   5. a slow consumer                           drops, counted, and exact
//   6. the capability dropped without a cancel   the subscription ends anyway
//   7. a raw tap with no demodulator             refused, in the server's words
//   8. a squelched receiver                      zeros at the full rate
//
// Two of them share one case where sharing is the point: shape 3 is asserted
// alongside a sink attached directly to the engine, because "two clients
// work" and "a client does not take the local consumer's audio" are the same
// property seen from two sides. Shape 5's control arm is a fast subscriber on
// the SAME receiver in the SAME run, which is what separates a drop counter
// that moved because of the drop from one that moved because of the run.
//
// WHAT SHAPE 3'S CASE USED TO CLAIM, WHICH WAS NOT A HAZARD AT ALL
//
// It said the local sink was the loudspeaker-theft case: "before the
// composition seam, the first subscribeAudio below would have replaced this
// and nothing anywhere would have said so". Nothing there can be stolen.
// Both the local sink and the server's are members of the same AudioFanout,
// which tests/engine/test_audio_fanout.cpp referees property by property, so
// the case certified a bar that could not fail while its own comment cited
// it as the guard.
//
// The shape that can still lose audio is a caller reaching
// Engine::set_audio_sink directly, which displaces the whole fan-out and
// tells nobody. core/engine/engine.h says so in as many words and, until
// 2026-09-20, tools/cli/main.cpp did exactly that. The case below the shapes
// pins the displacement instead of asserting around it.
//
// FOUR CASES THAT ARE NOT SHAPES OF A SUBSCRIPTION
//
// These are properties of the server and of the engine's seam rather than of
// one stream, so they are listed apart rather than renumbered in:
//
//   the receiver removed, then polled     audio_stats refuses, and does not
//                                         answer from the dead subscription
//   set_audio_sink reached directly       every other consumer goes silent,
//                                         with no ended() and no error
//   a chunk longer than the depth asked   the two-chunk floor is what is
//                                         enforced, not the milliseconds
//   a fan-out outliving its receiver      a later attach is refused rather
//                                         than joining a dead one
//   a receiver that THREW on a chunk      ended() carries the call's own
//                                         failure, and nothing else stops
//
// THE SOURCE IS PACED, as it is in test_rpc_spectrum.cpp and nowhere else in
// this suite. An unthrottled synthetic source retires in tens of
// milliseconds, which is not a window a case can subscribe inside, fall
// behind in and then read counters out of. pace = 1 makes the chunk rate the
// one a radio would produce: at 16384-sample blocks on a 2400032 S/s source
// that is about 146 chunks a second, and at the engine's default 48 kHz audio
// rate a chunk is about 328 frames and 6.8 ms of sound.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/dsp/types.h"
#include "core/engine/engine.h"
#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/types.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"

using namespace revenant;
using test::Harness;
using test::HarnessOptions;

namespace {

// Two seconds of capture at 16384-sample blocks. Enough that a slow
// subscriber has something to fall behind by and that a fast one on the same
// receiver collects a couple of hundred chunks.
constexpr dsp::SampleIndex kRunSamples = 4'800'064;
constexpr std::uint32_t kBlockSamples = 16'384;

// Blocks long enough that two chunks are more audio than the shortest depth
// a client can ask for, which is the only way to reach the two-chunk floor.
//
// The floor binds when 2 * frames_per_chunk exceeds millis * rate / 1000.
// frames_per_chunk is block_samples * rate / source_rate, so the audio rate
// cancels out of both sides and the condition is 2 * block_samples /
// source_rate > millis / 1000, which at the clamp's own 20 ms floor and this
// fixture's source rate is block_samples above 24000. At 16384 it is not
// even close, which is why every other case here reads the millisecond depth
// straight back out.
constexpr std::uint32_t kLongBlockSamples = 32'768;

// A receiver on one of the scene's emitters, in the baseband frame the whole
// suite uses.
constexpr std::int64_t kReceiverCenter = 131'072;

[[nodiscard]] HarnessOptions streaming_options() {
    HarnessOptions options;
    options.samples = kRunSamples;
    options.pace = 1.0;
    options.block_samples = kBlockSamples;
    return options;
}

void bring_up(Harness& harness, const HarnessOptions& options) {
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());
}

void bring_up_running(Harness& harness, const HarnessOptions& options) {
    bring_up(harness, options);
    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());
}

// A local consumer's frame count, and the sink that fills it.
//
// Held through a shared_ptr the sink captures BY VALUE, for the reason
// AudioLog gives and one of its own. A sink handed to the engine outlives
// the statement that attached it and is released through a control operation
// the completion thread applies, so a count that was an ordinary local would
// be a pointer into a stack frame the engine has not finished with: locals
// declared after the Harness are destroyed before it, and the engine is
// still running at that point.
using FrameCount = std::shared_ptr<std::atomic<std::uint64_t>>;

[[nodiscard]] FrameCount counting() {
    return std::make_shared<std::atomic<std::uint64_t>>(0);
}

[[nodiscard]] engine::AudioSink count_into(FrameCount count) {
    return [count](const engine::AudioChunk& chunk) -> Status {
        count->fetch_add(chunk.samples.size() / std::max(chunk.channels, 1U),
                         std::memory_order_relaxed);
        return {};
    };
}

[[nodiscard]] engine::VrxId engine_id(std::uint64_t vrx) {
    return engine::VrxId{static_cast<std::uint32_t>(vrx)};
}

[[nodiscard]] rpc::VrxParams nfm_receiver() {
    rpc::VrxParams params;
    params.center = kReceiverCenter;
    params.bandwidth = 12'000;
    params.demod = rpc::Demod::Nfm;
    return params;
}

// One chunk, reduced to what a case asserts on. The samples themselves are
// summarised rather than kept: at 146 chunks a second for two seconds,
// keeping every buffer is megabytes nobody reads, and what the cases ask is
// whether the stream was contiguous, whether the gate was open and whether
// anything was in it.
struct ChunkRecord {
    std::uint64_t sample_index = 0;
    std::uint64_t frames = 0;
    std::uint64_t dropped_before = 0;
    std::uint32_t rate = 0;
    std::uint16_t channels = 0;
    bool squelch_open = false;
    float peak = 0.0F;
};

// A subscriber's record of what arrived, safe to read from the test thread
// while the callback is still running on the client's event loop thread.
//
// Held through a shared_ptr the callbacks capture by value, for the reason
// FrameLog is: the callbacks outlive the statement that installed them and
// are dropped by the client's loop thread, which is a worse place to find a
// dangling reference than a compile error would have been.
class AudioLog {
public:
    // Client event loop thread.
    void record(const rpc::AudioChunk& chunk) {
        std::chrono::milliseconds delay{0};
        {
            const std::lock_guard<std::mutex> held(lock_);
            delay = delay_;

            ChunkRecord entry;
            entry.sample_index = chunk.sample_index;
            entry.frames = chunk.frames();
            entry.dropped_before = chunk.frames_dropped_before;
            entry.rate = chunk.sample_rate;
            entry.channels = chunk.channel_count;
            entry.squelch_open = chunk.squelch_open;
            for (const float sample : chunk.samples) {
                entry.peak = std::max(entry.peak, std::abs(sample));
            }
            chunks_.push_back(entry);
        }

        // Outside the lock, so the test thread reading chunks() is not held
        // for the length of a deliberately slow callback. The client does not
        // answer the chunk() call until this returns, so the engine's queue
        // for this subscription is what fills.
        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }
    }

    // Client event loop thread.
    void end(const std::string& reason) {
        const std::lock_guard<std::mutex> held(lock_);
        ended_ = true;
        reason_ = reason;
    }

    [[nodiscard]] std::vector<ChunkRecord> chunks() const {
        const std::lock_guard<std::mutex> held(lock_);
        return chunks_;
    }

    [[nodiscard]] std::size_t size() const {
        const std::lock_guard<std::mutex> held(lock_);
        return chunks_.size();
    }

    [[nodiscard]] bool ended() const {
        const std::lock_guard<std::mutex> held(lock_);
        return ended_;
    }

    [[nodiscard]] std::string reason() const {
        const std::lock_guard<std::mutex> held(lock_);
        return reason_;
    }

    void set_callback_delay(std::chrono::milliseconds delay) {
        const std::lock_guard<std::mutex> held(lock_);
        delay_ = delay;
    }

private:
    mutable std::mutex lock_;
    std::vector<ChunkRecord> chunks_;
    std::chrono::milliseconds delay_{0};
    bool ended_ = false;
    std::string reason_;
};

[[nodiscard]] rpc::Client::AudioCallback into(std::shared_ptr<AudioLog> log) {
    return [log](const rpc::AudioChunk& chunk) { log->record(chunk); };
}

[[nodiscard]] rpc::Client::AudioEndedCallback ending(std::shared_ptr<AudioLog> log) {
    return [log](const std::string& reason) { log->end(reason); };
}

// A receiver that THROWS, which is not the same failure as one that is slow
// or one that went away, and the server has to tell the three apart.
//
// core/rpc/client.cpp calls this from inside AudioReceiverImpl::chunk and
// returns only once it comes back, so an exception out of here leaves the
// chunk() call as a remote failure on the server's send. The connection is
// fine, the client is still holding the subscription, and what failed is
// this one call.
[[nodiscard]] rpc::Client::AudioCallback throwing_after(std::shared_ptr<AudioLog> log,
                                                        std::size_t good) {
    return [log, good](const rpc::AudioChunk& chunk) {
        log->record(chunk);
        if (log->size() > good) {
            throw std::runtime_error("the test receiver refused this chunk");
        }
    };
}

[[nodiscard]] std::size_t wait_for_chunks(const AudioLog& log, std::size_t wanted,
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

[[nodiscard]] bool wait_for_ended(const AudioLog& log, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (log.ended()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return log.ended();
}

// The invariant the schema states on AudioChunk::framesDroppedBefore, checked
// on every consecutive pair:
//
//   sampleIndex == previous.sampleIndex + previous frame count
//                  + framesDroppedBefore
//
// It is the whole reason a drop counted on this wire can be told apart from
// anything else that might have moved a counter. The engine's chunk stream
// for one receiver is contiguous by construction, so every frame missing from
// what arrived was evicted by this subscription's queue, and front eviction
// puts those frames exactly between the chunk before and the chunk after.
// Returns the total that was accounted for as an eviction.
[[nodiscard]] std::uint64_t check_contiguous(const std::vector<ChunkRecord>& chunks) {
    std::uint64_t accounted = 0;
    for (std::size_t i = 1; i < chunks.size(); ++i) {
        const ChunkRecord& previous = chunks[i - 1];
        const ChunkRecord& current = chunks[i];
        INFO("chunk " << i << " at index " << current.sample_index << " follows index "
                      << previous.sample_index << " of " << previous.frames
                      << " frames, with " << current.dropped_before << " dropped between");
        REQUIRE(current.sample_index ==
                previous.sample_index + previous.frames + current.dropped_before);
        accounted += current.dropped_before;
    }
    return accounted;
}

}  // namespace

// --- shape 1, shape 7 -------------------------------------------------------

TEST_CASE("subscribeAudio refuses a receiver that does not exist",
          "[gpu][rpc][audio]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    auto log = std::make_shared<AudioLog>();
    auto refused = harness.client().subscribe_audio(9'999, 0, into(log), ending(log));
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);

    // The engine's own sentence, and the OPPOSITE of what this call used to
    // answer. While the surface was unwired it refused before reading its
    // arguments, on purpose, because "no receiver 9999 is registered" would
    // have implied a good id worked. Now one does, so naming the receiver is
    // the true answer rather than the misleading one.
    CHECK(refused.error().message.find("9999") != std::string::npos);
    CHECK(refused.error().message.find("is registered") != std::string::npos);
    CHECK(refused.error().message.find("is not wired") == std::string::npos);
}

TEST_CASE("subscribeAudio refuses a raw tap", "[gpu][rpc][audio]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    rpc::VrxParams params = nfm_receiver();
    params.demod = rpc::Demod::Raw;
    auto vrx = harness.client().add_vrx(params);
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto log = std::make_shared<AudioLog>();
    auto refused = harness.client().subscribe_audio(*vrx, 0, into(log), ending(log));
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);

    // Refused by the server rather than by the engine, which would install a
    // sink on a raw tap happily. What comes out of one is interleaved complex
    // I/Q at the coarse channel rate, which a client playing it as
    // two-channel PCM renders as noise at the wrong speed.
    CHECK(refused.error().message.find("raw tap") != std::string::npos);

    // And the receiver survives the refusal.
    auto ids = harness.client().vrx_ids();
    INFO(test::message_of(ids));
    REQUIRE(ids.has_value());
    CHECK(ids->size() == 1);
}

// --- the stream itself, plus the depth clamp --------------------------------

TEST_CASE("audio streams as contiguous float32 PCM and reports the depth it granted",
          "[gpu][rpc][audio]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up_running(harness, streaming_options());

    auto vrx = harness.client().add_vrx(nfm_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto log = std::make_shared<AudioLog>();

    // Zero asks for the default and must come back as the default rather
    // than as zero: a client cannot tell "you got what you asked for" from
    // "the field was ignored" otherwise.
    auto granted = harness.client().subscribe_audio(*vrx, 0, into(log), ending(log));
    INFO(test::message_of(granted));
    REQUIRE(granted.has_value());
    CHECK(*granted == 500);

    const std::size_t seen = wait_for_chunks(*log, 40, 4000);
    INFO("chunks received: " << seen);
    REQUIRE(seen >= 40);

    const auto chunks = log->chunks();
    REQUIRE_FALSE(chunks.empty());

    // The rate and the channel count are on every chunk rather than cached
    // from VrxStatus, and they have to be the engine's.
    for (const ChunkRecord& chunk : chunks) {
        CHECK(chunk.rate == 48'000);
        CHECK(chunk.channels == 1);
        CHECK(chunk.frames > 0);
    }

    // A fast subscriber loses nothing, so every chunk butts against the one
    // before it and no frames are accounted for as evictions. This is the
    // control the slow case below is read against.
    CHECK(check_contiguous(chunks) == 0);

    // The gate is open by default: VrxParams::squelch_dbfs defaults to
    // -200 dBFS, which is under the arithmetic's own floor.
    CHECK(chunks.front().squelch_open);

    // Something is actually in the samples. A stream of correctly indexed
    // silence would pass every check above.
    const bool any_signal = std::ranges::any_of(
        chunks, [](const ChunkRecord& chunk) { return chunk.peak > 1e-6F; });
    CHECK(any_signal);

    auto stats = harness.client().audio_stats(*vrx);
    INFO(test::message_of(stats));
    REQUIRE(stats.has_value());
    CHECK(stats->frames_sent > 0);
    CHECK(stats->frames_dropped == 0);
    CHECK(stats->drop_events == 0);

    // Non-zero only once a chunk has arrived to set it, which is the
    // retraction the schema carries: the server cannot turn milliseconds
    // into frames before it knows the receiver's audio rate and a chunk's
    // length. 500 ms at 48 kHz is 24000 frames, and the two-chunk floor is
    // far below that here.
    CHECK(stats->buffer_frames == 24'000);

    harness.client().unsubscribe_audio(*vrx);

    // Never for a cancel this client asked for.
    CHECK_FALSE(log->ended());
}

TEST_CASE("a buffer depth outside the clamp is reported rather than applied silently",
          "[gpu][rpc][audio]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    auto vrx = harness.client().add_vrx(nfm_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto log = std::make_shared<AudioLog>();

    // Under the floor. A depth of one millisecond is 48 frames, which is a
    // seventh of a chunk, and a queue enforcing it would evict everything
    // while every other check still passed.
    auto low = harness.client().subscribe_audio(*vrx, 1, into(log), ending(log));
    INFO(test::message_of(low));
    REQUIRE(low.has_value());
    CHECK(*low == 20);

    // Over the ceiling.
    auto high = harness.client().subscribe_audio(*vrx, 60'000, into(log), ending(log));
    INFO(test::message_of(high));
    REQUIRE(high.has_value());
    CHECK(*high == 5'000);

    // And a value inside it comes back untouched, which is the control: a
    // clamp that returned a constant would satisfy both checks above.
    auto inside = harness.client().subscribe_audio(*vrx, 250, into(log), ending(log));
    INFO(test::message_of(inside));
    REQUIRE(inside.has_value());
    CHECK(*inside == 250);

    harness.client().unsubscribe_audio(*vrx);
}

// --- shape 5, with its control arm ------------------------------------------

TEST_CASE("a slow subscriber loses chunks, and the count is exactly what it lost",
          "[gpu][rpc][audio]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up_running(harness, streaming_options());

    auto vrx = harness.client().add_vrx(nfm_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    // The control arm, on the SAME receiver and in the SAME run. Without it a
    // non-zero drop count says only that something was lost somewhere; with
    // it, the two subscriptions saw one identical stream and only one of them
    // lost anything.
    auto fast_client = harness.connect_another();
    INFO(test::message_of(fast_client));
    REQUIRE(fast_client.has_value());

    auto fast_log = std::make_shared<AudioLog>();
    auto fast = (*fast_client)->subscribe_audio(*vrx, 0, into(fast_log), ending(fast_log));
    INFO(test::message_of(fast));
    REQUIRE(fast.has_value());

    auto slow_log = std::make_shared<AudioLog>();

    // 20 ms of depth against a chunk that is 6.8 ms of sound, so the queue
    // holds about three chunks, and a callback that takes 50 ms per chunk
    // against a stream producing one every 6.8. The queue fills within a few
    // chunks and evicts from the front from then on.
    slow_log->set_callback_delay(std::chrono::milliseconds(50));
    auto slow = harness.client().subscribe_audio(*vrx, 20, into(slow_log), ending(slow_log));
    INFO(test::message_of(slow));
    REQUIRE(slow.has_value());
    CHECK(*slow == 20);

    REQUIRE(wait_for_chunks(*fast_log, 150, 6000) >= 150);
    REQUIRE(wait_for_chunks(*slow_log, 8, 6000) >= 8);

    // Stop delaying before anything is read back: audio_stats runs on the
    // client's event loop thread and would otherwise queue behind a sleep.
    slow_log->set_callback_delay(std::chrono::milliseconds(0));

    const auto slow_chunks = slow_log->chunks();
    const auto fast_chunks = fast_log->chunks();

    // THE CONTROL. The fast subscriber on the same receiver lost nothing at
    // all, so the stream itself had no gap in it and the engine dropped
    // nothing upstream. Anything the slow one is missing was evicted from its
    // own queue.
    CHECK(check_contiguous(fast_chunks) == 0);

    auto fast_stats = (*fast_client)->audio_stats(*vrx);
    INFO(test::message_of(fast_stats));
    REQUIRE(fast_stats.has_value());
    CHECK(fast_stats->frames_dropped == 0);
    CHECK(fast_stats->drop_events == 0);

    // THE COUNTER MOVED. Not structurally-always-zero like the one
    // core/rpc/client.h retracts: a chunk is taken on the engine's completion
    // thread and drained at the client's pace, so two threads run
    // concurrently with the slow one outside the process.
    const std::uint64_t evicted = check_contiguous(slow_chunks);
    INFO("slow subscriber received " << slow_chunks.size() << " chunks and is missing "
                                     << evicted << " frames between them");
    CHECK(evicted > 0);

    auto slow_stats = harness.client().audio_stats(*vrx);
    INFO(test::message_of(slow_stats));
    REQUIRE(slow_stats.has_value());
    CHECK(slow_stats->frames_dropped > 0);
    CHECK(slow_stats->drop_events > 0);

    // AND THE TWO AGREE. framesDroppedBefore is reset when a chunk goes out,
    // so the frames the client can see between the chunks it received are
    // those evicted up to the last send; the counter also holds whatever was
    // evicted after it. Equal would be wrong and less would mean the counter
    // was moved by something other than these evictions.
    CHECK(slow_stats->frames_dropped >= evicted);

    // The depth the queue is actually enforcing: 20 ms at 48 kHz is 960
    // frames, comfortably above the two-chunk floor of about 656.
    CHECK(slow_stats->buffer_frames == 960);

    // Neither subscription's loss reached the receiver's own counter. That
    // is the narrowing in core/engine/vrx.h: a drop belongs to a consumer,
    // and one receiver here has two of them with different answers.
    auto status = harness.client().vrx_status(*vrx);
    INFO(test::message_of(status));
    REQUIRE(status.has_value());
    CHECK(status->audio_dropped == 0);
}

// --- shape 2 ----------------------------------------------------------------

TEST_CASE("subscribing to one receiver twice replaces the first subscription",
          "[gpu][rpc][audio]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up_running(harness, streaming_options());

    auto vrx = harness.client().add_vrx(nfm_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto first = std::make_shared<AudioLog>();
    auto opened = harness.client().subscribe_audio(*vrx, 0, into(first), ending(first));
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());
    REQUIRE(wait_for_chunks(*first, 20, 4000) >= 20);

    auto second = std::make_shared<AudioLog>();
    auto replaced = harness.client().subscribe_audio(*vrx, 0, into(second), ending(second));
    INFO(test::message_of(replaced));
    REQUIRE(replaced.has_value());

    const std::size_t first_at_swap = first->size();
    REQUIRE(wait_for_chunks(*second, 20, 4000) >= 20);

    // The replacement is fed and the original is not. A handful more may have
    // reached the first between the replace and the read, because the cancel
    // and a chunk already on the wire can cross, so this is a small margin
    // rather than equality.
    CHECK(first->size() <= first_at_swap + 4);

    // The first was cancelled by this client, not ended by the engine, so it
    // hears nothing about it.
    CHECK_FALSE(first->ended());

    // And the replacement's own stream is whole rather than starting mid-gap.
    CHECK(check_contiguous(second->chunks()) == 0);

    harness.client().unsubscribe_audio(*vrx);
}

// --- shape 3, and the loudspeaker ------------------------------------------

TEST_CASE("two clients and a local sink share one receiver's audio",
          "[gpu][rpc][audio]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up_running(harness, streaming_options());

    auto vrx = harness.client().add_vrx(nfm_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    // Attached FIRST and directly on the engine, which is what a host
    // playing audio out of a sound card in the same process looks like.
    //
    // THIS COMMENT USED TO SAY the first subscribeAudio below would have
    // replaced this before the composition seam and that nothing anywhere
    // would have said so. That is history and it is not what this case
    // checks, because after the seam neither of these can displace the
    // other: both are members of one AudioFanout and joining one is all
    // either of them does. The case that can lose audio reaches
    // set_audio_sink and is the next one in this file.
    //
    // What is left here is worth checking on its own terms: a host consumer
    // and two wire subscribers on one receiver, all three fed from a single
    // engine sink, each leaving without taking the others with it.
    auto local_frames = counting();
    auto token =
        harness.engine().attach_audio_sink(engine_id(*vrx), count_into(local_frames));
    INFO(test::message_of(token));
    REQUIRE(token.has_value());

    auto other = harness.connect_another();
    INFO(test::message_of(other));
    REQUIRE(other.has_value());

    auto mine = std::make_shared<AudioLog>();
    auto theirs = std::make_shared<AudioLog>();

    auto one = harness.client().subscribe_audio(*vrx, 0, into(mine), ending(mine));
    INFO(test::message_of(one));
    REQUIRE(one.has_value());

    auto two = (*other)->subscribe_audio(*vrx, 0, into(theirs), ending(theirs));
    INFO(test::message_of(two));
    REQUIRE(two.has_value());

    REQUIRE(wait_for_chunks(*mine, 40, 4000) >= 40);
    REQUIRE(wait_for_chunks(*theirs, 40, 4000) >= 40);

    const std::uint64_t local_at_check = local_frames->load(std::memory_order_relaxed);
    CHECK(local_at_check > 0);

    // Both wire subscriptions saw the same stream, whole.
    CHECK(check_contiguous(mine->chunks()) == 0);
    CHECK(check_contiguous(theirs->chunks()) == 0);

    // One client leaving takes nothing else with it.
    (*other)->unsubscribe_audio(*vrx);
    const std::size_t mine_at_leave = mine->size();
    REQUIRE(wait_for_chunks(*mine, mine_at_leave + 10, 4000) >= mine_at_leave + 10);

    // And the local sink is still being fed, which is the whole point.
    harness.client().unsubscribe_audio(*vrx);
    const std::uint64_t local_after = local_frames->load(std::memory_order_relaxed);
    CHECK(local_after > local_at_check);

    // The last wire subscription going away detached the server's entry and
    // only the server's: this token is still live, so the fan-out is still
    // there and still holding it.
    auto detached = harness.engine().detach_audio_sink(engine_id(*vrx), *token);
    INFO(test::message_of(detached));
    CHECK(detached.has_value());

    // With nothing left attached the fan-out is gone, so the same token
    // detaches nothing rather than finding a stale one.
    auto again = harness.engine().detach_audio_sink(engine_id(*vrx), *token);
    CHECK_FALSE(again.has_value());
}

// --- the seam's remaining hazard, which is set_audio_sink ------------------

TEST_CASE("a caller reaching set_audio_sink directly silences everything else",
          "[gpu][rpc][audio]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up_running(harness, streaming_options());

    auto vrx = harness.client().add_vrx(nfm_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    // A host consumer and a wire subscriber, both through the seam, both
    // being fed. This is the state the case above leaves the world in.
    auto attached_frames = counting();
    auto token =
        harness.engine().attach_audio_sink(engine_id(*vrx), count_into(attached_frames));
    INFO(test::message_of(token));
    REQUIRE(token.has_value());

    auto log = std::make_shared<AudioLog>();
    auto opened = harness.client().subscribe_audio(*vrx, 0, into(log), ending(log));
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    REQUIRE(wait_for_chunks(*log, 20, 4000) >= 20);
    const std::uint64_t attached_before = attached_frames->load(std::memory_order_relaxed);
    CHECK(attached_before > 0);

    // THE HAZARD, EXERCISED RATHER THAN DESCRIBED. The slot holds one sink
    // and this replaces it, so the fan-out with both consumers in it comes
    // off the receiver entirely. core/engine/engine.h says a caller reaching
    // here displaces everything the fan-out was holding and is not told it
    // did; this is that sentence as a case.
    auto thief_frames = counting();
    const auto stolen = harness.engine().set_audio_sink(engine_id(*vrx),
                                                        count_into(thief_frames));
    INFO(test::message_of(stolen));
    REQUIRE(stolen.has_value());

    // The displacement travels as a control operation the completion thread
    // applies, so both consumers may see a chunk or two after the call
    // returns. Read a settled state rather than an instantaneous one.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const std::uint64_t attached_at_rest = attached_frames->load(std::memory_order_relaxed);
    const std::size_t wire_at_rest = log->size();
    const std::uint64_t thief_at_rest = thief_frames->load(std::memory_order_relaxed);
    REQUIRE(thief_at_rest > 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Both go silent. The attached consumer and the wire subscriber are
    // members of one fan-out and the fan-out is what was displaced, so this
    // is one fact seen twice rather than two.
    CHECK(attached_frames->load(std::memory_order_relaxed) == attached_at_rest);
    CHECK(log->size() == wire_at_rest);

    // AND NOBODY IS TOLD. No ended(), because the server was not asked to
    // end anything and does not know: the engine stopped calling its sink
    // and a sink that is not called looks exactly like a receiver with
    // nothing on it. The subscription is still live and still reports itself
    // healthy, which is precisely why a VU meter or a decoder tap wired with
    // set_audio_sink would kill every wire subscriber with a green suite.
    CHECK_FALSE(log->ended());

    auto stats = harness.client().audio_stats(*vrx);
    INFO(test::message_of(stats));
    REQUIRE(stats.has_value());
    CHECK(stats->frames_dropped == 0);
    CHECK(stats->drop_events == 0);

    // The thief, meanwhile, is the only consumer there is.
    CHECK(thief_frames->load(std::memory_order_relaxed) > thief_at_rest);

    harness.client().unsubscribe_audio(*vrx);

    // Cleared through the seam, which takes the displacing sink off with it:
    // the token's fan-out is empty once this returns, so detach_audio_sink
    // puts an empty sink in the slot. Done here rather than left to teardown
    // so nothing is still being called while this case's locals unwind.
    auto detached = harness.engine().detach_audio_sink(engine_id(*vrx), *token);
    INFO(test::message_of(detached));
    CHECK(detached.has_value());

    const auto stopped = harness.stop_engine();
    INFO(test::message_of(stopped));
    CHECK(stopped.has_value());
}

// --- the fan-out that outlives its receiver --------------------------------

TEST_CASE("attaching to a receiver that has been removed is refused",
          "[gpu][rpc][audio]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    auto doomed = harness.client().add_vrx(nfm_receiver());
    INFO(test::message_of(doomed));
    REQUIRE(doomed.has_value());

    auto survivor = harness.client().add_vrx(nfm_receiver());
    INFO(test::message_of(survivor));
    REQUIRE(survivor.has_value());

    auto frames = counting();
    auto token = harness.engine().attach_audio_sink(engine_id(*doomed), count_into(frames));
    INFO(test::message_of(token));
    REQUIRE(token.has_value());

    // Removed with the consumer still attached, which is the ordinary
    // teardown order rather than a mistake, and the one that leaves the
    // fan-out in Engine's map: nothing prunes it there, because remove_vrx
    // does not pass through the code that owns the map.
    const auto removed = harness.client().remove_vrx(*doomed);
    INFO(test::message_of(removed));
    REQUIRE(removed.has_value());

    // Until 2026-09-20 this handed back a token and reported success,
    // because a fan-out that already exists is joined without anything being
    // asked of the graph. The consumer would then have waited for audio from
    // a receiver that no longer existed, for the life of the engine.
    auto late = harness.engine().attach_audio_sink(engine_id(*doomed), count_into(frames));
    INFO(test::message_of(late));
    REQUIRE_FALSE(late.has_value());
    CHECK(late.error().message.find(std::to_string(*doomed)) != std::string::npos);
    CHECK(late.error().message.find("is registered") != std::string::npos);

    // AND THE STALE ENTRY IS GONE, which is what separates a refusal from a
    // refusal plus a prune. The token issued before the removal named a
    // fan-out that was still in the map; the refusal above dropped it, so
    // detaching now finds no fan-out at all rather than an orphaned one.
    auto orphan = harness.engine().detach_audio_sink(engine_id(*doomed), *token);
    INFO(test::message_of(orphan));
    REQUIRE_FALSE(orphan.has_value());
    CHECK(orphan.error().message.find("no attached audio consumers") != std::string::npos);

    // THE CONTROL. A receiver that is still there takes a second consumer
    // exactly as it did before, so the check above refuses the dead receiver
    // rather than every second attach.
    auto first = harness.engine().attach_audio_sink(engine_id(*survivor), count_into(frames));
    INFO(test::message_of(first));
    REQUIRE(first.has_value());

    auto second = harness.engine().attach_audio_sink(engine_id(*survivor), count_into(frames));
    INFO(test::message_of(second));
    REQUIRE(second.has_value());
    CHECK(*first != *second);

    CHECK(harness.engine().detach_audio_sink(engine_id(*survivor), *first).has_value());
    CHECK(harness.engine().detach_audio_sink(engine_id(*survivor), *second).has_value());
}

// --- the two-chunk floor ----------------------------------------------------

TEST_CASE("the two-chunk floor overrides a depth shorter than two chunks",
          "[gpu][rpc][audio]") {
    REVENANT_NEEDS_GPU();

    HarnessOptions options = streaming_options();
    options.block_samples = kLongBlockSamples;

    Harness harness;
    bring_up_running(harness, options);

    auto vrx = harness.client().add_vrx(nfm_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto log = std::make_shared<AudioLog>();

    // The shortest depth the millisecond clamp allows, so the answer on the
    // wire is 20 and the floor applied later is the only thing that can
    // change what is enforced.
    auto granted = harness.client().subscribe_audio(*vrx, 20, into(log), ending(log));
    INFO(test::message_of(granted));
    REQUIRE(granted.has_value());
    CHECK(*granted == 20);

    REQUIRE(wait_for_chunks(*log, 20, 6000) >= 20);
    const auto chunks = log->chunks();
    REQUIRE_FALSE(chunks.empty());

    // A chunk is not a whole number of frames at this fixture's rates: 1024
    // channel samples resampled from 75001 to 48000 is 655.36, so the stream
    // alternates between two lengths and the floor is twice whichever one
    // the server saw last.
    //
    // THE FIRST CHUNK IS SHORT and is left out of the range on purpose. A
    // demodulator produces no frames until its filter support is inside real
    // samples, so the stream opens with a partial chunk (592 frames against
    // a steady 655 or 656, measured 2026-09-20). Counting it would widen the
    // window below by a tenth and admit a floor taken from a length this
    // stream produces exactly once.
    REQUIRE(chunks.size() > 2);
    std::uint64_t shortest = chunks[1].frames;
    std::uint64_t longest = chunks[1].frames;
    for (std::size_t i = 1; i < chunks.size(); ++i) {
        shortest = std::min(shortest, chunks[i].frames);
        longest = std::max(longest, chunks[i].frames);
    }
    for (const ChunkRecord& chunk : chunks) {
        CHECK(chunk.rate == 48'000);
    }
    INFO("the first chunk was " << chunks.front().frames << " frames and the rest ran "
                                << shortest << " to " << longest);

    auto stats = harness.client().audio_stats(*vrx);
    INFO(test::message_of(stats));
    REQUIRE(stats.has_value());

    // What 20 ms converts to at this receiver's rate, which is what the
    // queue would enforce if the floor did not exist. The slow-subscriber
    // case above reads exactly this number back at the default block size,
    // where two chunks are well under it; that case is this one's control
    // arm and the reason a constant cannot satisfy both.
    const std::uint64_t millisecond_depth = 20ULL * 48'000 / 1000;
    INFO("enforced " << stats->buffer_frames << " frames against a millisecond depth of "
                     << millisecond_depth);
    CHECK(stats->buffer_frames > millisecond_depth);

    // And it is two chunks rather than some other number of them. Even, and
    // half of it is a length this stream actually produced.
    REQUIRE(stats->buffer_frames % 2 == 0);
    const std::uint64_t implied = stats->buffer_frames / 2;
    CHECK(implied >= shortest);
    CHECK(implied <= longest);

    // WHY THE FLOOR IS THERE AT ALL. A depth under two chunks cannot hold
    // the one being sent and the one that arrived behind it, so a subscriber
    // keeping up perfectly would still evict on every push. This one is
    // keeping up, and the counters say so.
    CHECK(check_contiguous(chunks) == 0);
    CHECK(stats->frames_dropped == 0);
    CHECK(stats->drop_events == 0);

    harness.client().unsubscribe_audio(*vrx);
}

TEST_CASE("a subscription killed by a receiver that threw is told it was killed",
          "[gpu][rpc][audio]") {
    // NOT A SHAPE OF A SUBSCRIPTION, like the four listed at the top: it is
    // a property of the server's own send path.
    //
    // A chunk call that comes back failed ends the subscription, and until
    // 2026-09-20 it ended it in silence. The comment where it happened said
    // no ended() was sent because "the capability it would travel on is the
    // one that failed", which is true of a dropped connection and false of
    // a receiver that threw: the client is still there, the capability
    // still works, and what it got instead was a stream that stopped
    // looking exactly like a quiet channel. AudioSubscription::stats then
    // refused, blaming the engine for a failure the engine had no part in.
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up_running(harness, streaming_options());

    auto vrx = harness.client().add_vrx(nfm_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    // The control arm, as in the removal case below: a second subscription
    // on this client that nothing touches. Without it these assertions are
    // satisfied by a server that tore down every audio subscription it had
    // over one bad call.
    auto keeper = harness.client().add_vrx(nfm_receiver());
    INFO(test::message_of(keeper));
    REQUIRE(keeper.has_value());

    auto keeper_log = std::make_shared<AudioLog>();
    auto kept =
        harness.client().subscribe_audio(*keeper, 0, into(keeper_log), ending(keeper_log));
    INFO(test::message_of(kept));
    REQUIRE(kept.has_value());

    // Five good chunks first, so the failure lands on a stream that was
    // working rather than on a subscription that never started.
    constexpr std::size_t kGoodChunks = 5;
    auto log = std::make_shared<AudioLog>();
    auto opened = harness.client().subscribe_audio(*vrx, 0, throwing_after(log, kGoodChunks),
                                                   ending(log));
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());
    REQUIRE(wait_for_chunks(*log, kGoodChunks + 1, 4000) >= kGoodChunks + 1);

    // THE WHOLE OF THE FINDING. Silence here is what a quiet channel sounds
    // like, and the client cannot otherwise tell that its subscription is
    // over.
    REQUIRE(wait_for_ended(*log, 4000));

    // And the reason is the receiver's own failure rather than a sentence
    // about the engine. The text the callback threw crosses back, because
    // the server has nothing better to say about a call it did not make.
    INFO(log->reason());
    CHECK(log->reason().find("refused this chunk") != std::string::npos);

    // Really finished rather than merely quiet.
    const std::size_t at_end = log->size();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(log->size() == at_end);

    // THE CONTROL. The other subscription on the same client and the same
    // connection never noticed, so one failed call ended one subscription.
    const std::size_t keeper_at_end = keeper_log->size();
    REQUIRE(wait_for_chunks(*keeper_log, keeper_at_end + 10, 4000) >= keeper_at_end + 10);
    CHECK_FALSE(keeper_log->ended());

    harness.client().unsubscribe_audio(*keeper);
}

// --- shape 4 ----------------------------------------------------------------

TEST_CASE("removing a receiver mid-stream tells its listeners in words",
          "[gpu][rpc][audio]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up_running(harness, streaming_options());

    auto vrx = harness.client().add_vrx(nfm_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    // A second receiver, subscribed on the SAME client, which nothing in
    // this case touches. It is the control arm: the assertions below are all
    // about a subscription going away, and without a live one beside it they
    // would be satisfied by a teardown that took every audio subscription
    // this client held.
    auto keeper = harness.client().add_vrx(nfm_receiver());
    INFO(test::message_of(keeper));
    REQUIRE(keeper.has_value());

    auto keeper_log = std::make_shared<AudioLog>();
    auto kept =
        harness.client().subscribe_audio(*keeper, 0, into(keeper_log), ending(keeper_log));
    INFO(test::message_of(kept));
    REQUIRE(kept.has_value());

    auto log = std::make_shared<AudioLog>();
    auto opened = harness.client().subscribe_audio(*vrx, 0, into(log), ending(log));
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());
    REQUIRE(wait_for_chunks(*log, 20, 4000) >= 20);
    REQUIRE(wait_for_chunks(*keeper_log, 20, 4000) >= 20);

    // Answering before the removal, so the refusal below is a change of
    // answer rather than a call that never worked.
    auto live = harness.client().audio_stats(*vrx);
    INFO(test::message_of(live));
    REQUIRE(live.has_value());
    CHECK(live->frames_sent > 0);

    const auto removed = harness.client().remove_vrx(*vrx);
    INFO(test::message_of(removed));
    REQUIRE(removed.has_value());

    // Silence is what a quiet channel with the squelch shut sounds like, so a
    // stream that simply stopped would be indistinguishable from one nobody
    // is talking on. This is the only thing in the stream that says which.
    REQUIRE(wait_for_ended(*log, 4000));
    INFO(log->reason());
    CHECK(log->reason().find("removed") != std::string::npos);

    // And the subscription is really finished rather than merely quiet.
    const std::size_t at_end = log->size();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(log->size() == at_end);

    // THE COUNTERS GO WITH IT, which is what this case gained on 2026-09-20.
    // The ended path cleared the callbacks and kept the subscription
    // capability, so this call found it, asked the server, and came back Ok
    // with the dead stream's last counts. A UI polling a status line saw a
    // healthy subscription on a receiver that had been removed, and the
    // surviving reference held the server's node and its queue open until
    // the connection dropped.
    auto dead = harness.client().audio_stats(*vrx);
    REQUIRE_FALSE(dead.has_value());
    INFO(dead.error().message);
    CHECK(dead.error().message.find("holds no audio subscription") != std::string::npos);

    // Idempotent afterwards rather than an error, the same as any other
    // unsubscribe of something that is already over.
    harness.client().unsubscribe_audio(*vrx);

    // THE CONTROL. The other receiver on this client never stopped and its
    // counters still answer, so the teardown above took one subscription and
    // not the map it lived in.
    const std::size_t keeper_at_end = keeper_log->size();
    REQUIRE(wait_for_chunks(*keeper_log, keeper_at_end + 10, 4000) >= keeper_at_end + 10);
    CHECK_FALSE(keeper_log->ended());

    auto keeper_stats = harness.client().audio_stats(*keeper);
    INFO(test::message_of(keeper_stats));
    REQUIRE(keeper_stats.has_value());
    CHECK(keeper_stats->frames_sent > 0);

    harness.client().unsubscribe_audio(*keeper);
}

// --- shape 6 ----------------------------------------------------------------

TEST_CASE("a client that goes away without cancelling takes its subscription and nothing else",
          "[gpu][rpc][audio]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up_running(harness, streaming_options());

    auto vrx = harness.client().add_vrx(nfm_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto mine = std::make_shared<AudioLog>();
    auto one = harness.client().subscribe_audio(*vrx, 0, into(mine), ending(mine));
    INFO(test::message_of(one));
    REQUIRE(one.has_value());

    auto leaver_log = std::make_shared<AudioLog>();
    {
        auto leaver = harness.connect_another();
        INFO(test::message_of(leaver));
        REQUIRE(leaver.has_value());

        auto two =
            (*leaver)->subscribe_audio(*vrx, 0, into(leaver_log), ending(leaver_log));
        INFO(test::message_of(two));
        REQUIRE(two.has_value());
        REQUIRE(wait_for_chunks(*leaver_log, 20, 4000) >= 20);

        // Destroyed without unsubscribing, which is what a client that
        // crashed looks like from here: the AudioSubscription capability is
        // released with the connection and the server's destructor is what
        // ends the subscription.
    }

    const std::size_t leaver_at_exit = leaver_log->size();
    const std::size_t mine_at_exit = mine->size();

    // The survivor keeps streaming.
    REQUIRE(wait_for_chunks(*mine, mine_at_exit + 20, 4000) >= mine_at_exit + 20);

    // And the one that left is finished.
    CHECK(leaver_log->size() == leaver_at_exit);

    harness.client().unsubscribe_audio(*vrx);
}

// --- shape 8 ----------------------------------------------------------------

TEST_CASE("a squelched receiver sends silence at the full rate rather than stopping",
          "[gpu][rpc][audio]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up_running(harness, streaming_options());

    rpc::VrxParams params = nfm_receiver();

    // Above full scale, so the gate is shut whatever the scene is doing. The
    // default is -200 dBFS, which is under the arithmetic's own floor and so
    // never closes; that default is why the mute path went unexercised for as
    // long as it did.
    params.squelch_dbfs = 20.0;

    auto vrx = harness.client().add_vrx(params);
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto log = std::make_shared<AudioLog>();
    auto opened = harness.client().subscribe_audio(*vrx, 0, into(log), ending(log));
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    REQUIRE(wait_for_chunks(*log, 40, 4000) >= 40);
    const auto chunks = log->chunks();

    for (const ChunkRecord& chunk : chunks) {
        // The gate, said on the chunk rather than left to a vrxStatus poll,
        // so an indicator follows the audio instead of lagging it.
        CHECK_FALSE(chunk.squelch_open);

        // Zeros the engine wrote, not a quiet band.
        CHECK(chunk.peak == 0.0F);
    }

    // AT THE FULL RATE AND WITH THE TIMELINE WHOLE, which is the difference
    // between a closed gate and a drop. Nothing here is missing and nothing
    // had to be filled.
    CHECK(check_contiguous(chunks) == 0);

    auto stats = harness.client().audio_stats(*vrx);
    INFO(test::message_of(stats));
    REQUIRE(stats.has_value());
    CHECK(stats->frames_dropped == 0);

    // The regression this case exists for. Until 2026-09-20 the squelch mute
    // was the only site that incremented this counter, so a receiver with the
    // gate shut reported a dropout for every frame of a channel that was
    // simply quiet.
    auto status = harness.client().vrx_status(*vrx);
    INFO(test::message_of(status));
    REQUIRE(status.has_value());
    CHECK_FALSE(status->squelch_open);
    CHECK(status->audio_samples > 0);
    CHECK(status->audio_dropped == 0);

    harness.client().unsubscribe_audio(*vrx);
}
