// AudioRing, which is every decision on the audio path that can be wrong in
// a way nobody hears until much later.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS, in its own
// comment, because a test that only asserts what the code already does
// certifies one reachable shape and reads as though it certified the
// behaviour. The shapes the ring can be in are enumerated at the top of
// audio/audio_ring.h; the ones reached below are the first chunk, a
// contiguous run, a gap from the wire, a gap from upstream, a gap that does
// not fit, an overrun, a starve, a closed gate, a format change, a read at
// a format the ring has already left, a malformed chunk, a backwards
// index, and a snapshot taken while the format moves under it.
//
// WHAT IS NOT HERE. Nothing with a sound card in it: see the block above
// the target in ui/CMakeLists.txt. That is also why the consumer half of
// the format-snapshot defect, AudioPlayer deciding whether to reopen its
// QAudioSink, is asserted nowhere. The accessor that decision rests on is
// asserted below, and that is as far into the path as a test binary with
// no audio device can reach.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

#include "audio/audio_ring.h"

using revenant::rpc::AudioChunk;
using revenant::ui::AudioRing;
using revenant::ui::FrameSource;
using revenant::ui::ReadResult;
using revenant::ui::RingCounts;
using revenant::ui::RingFormat;

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kDepthMs = 100;  // 4800 frames at kRate
constexpr std::size_t kCapacity = 4800;

[[nodiscard]] AudioChunk make_chunk(std::uint64_t index, std::size_t frames, float value,
                                    bool squelch_open = true,
                                    std::uint64_t dropped_before = 0,
                                    std::uint32_t rate = kRate,
                                    std::uint16_t channels = 1)
{
    AudioChunk chunk;
    chunk.sample_rate = rate;
    chunk.channel_count = channels;
    chunk.sample_index = index;
    chunk.frames_dropped_before = dropped_before;
    chunk.squelch_open = squelch_open;
    chunk.samples.assign(frames * channels, value);
    return chunk;
}

// Reads at whatever format the ring currently holds, which is what a test
// about the arithmetic wants. The two calls cannot disagree here the way
// they can in RingSource: nothing is writing to the ring on another thread.
[[nodiscard]] ReadResult drain(AudioRing& ring, std::vector<float>& out, std::size_t frames)
{
    const revenant::ui::RingFormat format = ring.format();
    out.assign(frames * std::max<std::size_t>(format.channel_count, 1), -99.0F);
    return ring.read(out.data(), frames, format);
}

}  // namespace

TEST_CASE("the first chunk sets the baseline and is not a gap", "[audio][ring]")
{
    // REJECTS: measuring the first chunk's gap against zero. The schema is
    // explicit that sampleIndex is zero on the first chunk "which arrives
    // later than a client expects", because a demodulator produces nothing
    // until its filter support is inside real samples. An implementation
    // that treated index - 0 as a gap would write that whole silent
    // interval into the ring on the first chunk of every stream, and at a
    // receiver created a few seconds into a capture that is a resync before
    // a single frame of audio is played.
    AudioRing ring;
    ring.set_depth_millis(kDepthMs);

    ring.write(make_chunk(96000, 480, 0.5F));

    const RingCounts counts = ring.counts();
    CHECK(counts.frames_filled == 0);
    CHECK(counts.gap_events == 0);
    CHECK(counts.resyncs == 0);
    CHECK(counts.frames_written == 480);
    CHECK(ring.frames_buffered() == 480);
    CHECK(ring.capacity_frames() == kCapacity);
}

TEST_CASE("a contiguous run writes no silence", "[audio][ring]")
{
    // REJECTS: an off-by-one in the expected index, either
    // previous_index + 1 or previous_index alone rather than
    // previous_index + previous frame count. Both produce a gap or an
    // overlap on every single chunk, which at 27 ms a chunk is a click
    // thirty-seven times a second, and the counters would be the only
    // thing distinguishing that from a healthy stream.
    AudioRing ring;
    ring.set_depth_millis(kDepthMs);

    ring.write(make_chunk(1000, 480, 0.1F));
    ring.write(make_chunk(1480, 480, 0.2F));
    ring.write(make_chunk(1960, 480, 0.3F));

    const RingCounts counts = ring.counts();
    CHECK(counts.frames_filled == 0);
    CHECK(counts.gap_events == 0);
    CHECK(counts.restarts == 0);
    CHECK(counts.frames_written == 1440);
    CHECK(ring.frames_buffered() == 1440);
}

TEST_CASE("a gap is filled with exactly the missing frames", "[audio][ring]")
{
    // REJECTS: splicing the chunk on where the previous one ended, which is
    // the obvious implementation and the one that drifts. With it the ring
    // would hold 960 frames here rather than 1240, the audio after the gap
    // would play 300 frames early, and every later gap would add to that.
    // Drift is inaudible per event and cumulative, which is why the click
    // was chosen over it. See the gap note on AudioRing::write.
    AudioRing ring;
    ring.set_depth_millis(kDepthMs);

    ring.write(make_chunk(0, 480, 0.1F));
    ring.write(make_chunk(780, 480, 0.2F, true, 300));

    const RingCounts counts = ring.counts();
    CHECK(counts.frames_filled == 300);
    CHECK(counts.gap_events == 1);
    CHECK(ring.frames_buffered() == 480 + 300 + 480);

    // And the silence is where the gap was, not at the end.
    std::vector<float> out;
    const ReadResult got = drain(ring, out, 1260);
    REQUIRE(got.frames_from_ring == 1260);
    CHECK(out[479] == 0.1F);
    CHECK(out[480] == 0.0F);
    CHECK(out[779] == 0.0F);
    CHECK(out[780] == 0.2F);
}

TEST_CASE("a gap is split between the wire and upstream", "[audio][ring]")
{
    // REJECTS: charging the whole gap to framesDroppedBefore, or ignoring
    // that field and charging the whole gap upstream. The schema states
    // outright that the two are not interchangeable: framesDroppedBefore is
    // what the SERVER's queue evicted because this client was too slow, and
    // a larger sampleIndex gap lost the remainder in the engine. Two causes
    // with two different fixes. A display that added them would send
    // someone to tune the buffer depth for a fault in the engine's own
    // audio egress.
    AudioRing ring;
    ring.set_depth_millis(kDepthMs);

    SECTION("all of it on the wire") {
        ring.write(make_chunk(0, 480, 0.1F));
        ring.write(make_chunk(980, 480, 0.2F, true, 500));
        const RingCounts counts = ring.counts();
        CHECK(counts.frames_gap_wire == 500);
        CHECK(counts.frames_gap_upstream == 0);
    }

    SECTION("some of it upstream") {
        ring.write(make_chunk(0, 480, 0.1F));
        ring.write(make_chunk(980, 480, 0.2F, true, 200));
        const RingCounts counts = ring.counts();
        CHECK(counts.frames_gap_wire == 200);
        CHECK(counts.frames_gap_upstream == 300);
    }

    SECTION("a reported drop larger than the gap is clamped to the gap") {
        // The engine's invariant says this cannot happen. If it ever does,
        // the gap is what was actually missing and the wire share cannot
        // exceed it, or the two counters stop summing to the gap and the
        // display's arithmetic quietly stops adding up.
        ring.write(make_chunk(0, 480, 0.1F));
        ring.write(make_chunk(580, 480, 0.2F, true, 9999));
        const RingCounts counts = ring.counts();
        CHECK(counts.frames_gap_wire == 100);
        CHECK(counts.frames_gap_upstream == 0);
    }
}

TEST_CASE("a gap longer than the ring resyncs instead of filling", "[audio][ring]")
{
    // REJECTS: filling any gap whatever its size. A ten second stall
    // against a 100 ms ring would write 480000 frames of silence through a
    // 4800 frame buffer, evicting the live audio in front of it and then
    // evicting all but the last 4800 frames of its own silence. The
    // operator would hear the stall, then a tenth of a second of silence
    // that was never transmitted, then the audio. The counters would read
    // as a clean fill.
    //
    // Also rejects growing the ring to hold the gap, which allocates for
    // the worst stall the network ever has and keeps it for the stream.
    AudioRing ring;
    ring.set_depth_millis(kDepthMs);

    ring.write(make_chunk(0, 480, 0.1F));
    REQUIRE(ring.frames_buffered() == 480);

    ring.write(make_chunk(480 + 480000, 480, 0.2F, true, 480000));

    const RingCounts counts = ring.counts();
    CHECK(counts.resyncs == 1);
    CHECK(counts.frames_filled == 0);
    CHECK(counts.frames_gap_discarded == 480000);
    CHECK(counts.frames_overrun == 0);

    // Everything buffered went, and the chunk that triggered it is all
    // that is there.
    CHECK(ring.frames_buffered() == 480);
    std::vector<float> out;
    const ReadResult got = drain(ring, out, 480);
    CHECK(got.frames_from_ring == 480);
    CHECK(out[0] == 0.2F);
}

TEST_CASE("an overrun evicts the oldest frames", "[audio][ring]")
{
    // REJECTS: dropping the incoming chunk instead, which is the other
    // obvious choice and is the wrong one for the same reason the engine's
    // own queue evicts from the front: late audio is worse than no audio
    // when the point is to hear what the radio is doing now. Dropping the
    // newest would hold a stale second of audio and never catch up.
    //
    // Also rejects growing the ring on overflow, which would make the
    // granted depth advisory and let latency climb without bound behind a
    // slow card.
    AudioRing ring;
    ring.set_depth_millis(kDepthMs);

    // Fill it exactly, then push one more chunk in with nothing draining.
    for (std::uint64_t i = 0; i < 10; ++i) {
        ring.write(make_chunk(i * 480, 480, static_cast<float>(i) + 1.0F));
    }
    REQUIRE(ring.frames_buffered() == kCapacity);
    REQUIRE(ring.counts().frames_overrun == 0);

    ring.write(make_chunk(4800, 480, 99.0F));

    CHECK(ring.counts().frames_overrun == 480);
    CHECK(ring.frames_buffered() == kCapacity);

    std::vector<float> out;
    const ReadResult got = drain(ring, out, kCapacity);
    REQUIRE(got.frames_from_ring == kCapacity);

    // The first chunk is gone and the newest is at the end.
    CHECK(out[0] == 2.0F);
    CHECK(out[kCapacity - 1] == 99.0F);
}

TEST_CASE("a starved read is filled and reported rather than short", "[audio][ring]")
{
    // REJECTS: returning only what is buffered. QAudioSink reads a short
    // answer from its pull device as the end of the stream: it goes Idle
    // and stops. So a network hiccup would END the audio rather than dip
    // it, and the operator would have to notice silence on a channel that
    // sounds like every other quiet channel and restart the stream by hand.
    AudioRing ring;
    ring.set_depth_millis(kDepthMs);
    ring.write(make_chunk(0, 100, 0.5F));

    std::vector<float> out;
    const ReadResult got = drain(ring, out, 400);

    CHECK(got.frames_from_ring == 100);
    CHECK(got.frames_starved == 300);
    CHECK(got.last_source == FrameSource::starved);
    CHECK(ring.counts().frames_starved == 300);
    CHECK(out[99] == 0.5F);
    CHECK(out[100] == 0.0F);
    CHECK(out[399] == 0.0F);
}

TEST_CASE("a read before any stream leaves the buffer alone", "[audio][ring]")
{
    // REJECTS: writing zeros anyway, at some assumed channel count. With
    // no format there is no channel count, so the ring does not know how
    // many floats the caller's buffer holds, and guessing one is a write
    // past the end of it on any stream that turns out to be stereo. The
    // caller cannot have an open sink at this point either, because it
    // takes its format from here.
    AudioRing ring;
    ring.set_depth_millis(kDepthMs);

    std::vector<float> out(64, -99.0F);
    const ReadResult got = ring.read(out.data(), 64, RingFormat{});

    CHECK(got.frames_from_ring == 0);
    CHECK(got.frames_starved == 64);
    CHECK(out[0] == -99.0F);
    CHECK(out[63] == -99.0F);
}

TEST_CASE("a read at the wrong format takes nothing and says so", "[audio][ring]")
{
    // REJECTS: asking format() and then read(), which is what this class
    // used to make a reader do. The reader is QAudioSink's pull thread and
    // the writer is the Cap'n Proto event loop, so a chunk at a new channel
    // count lands between those two calls often enough to matter: the
    // reader sizes its buffer for one channel and the ring fills it for
    // two, which is a write of twice the frames past the end of it. The
    // format is an argument now so the compare and the copy are one locked
    // call, and a reader that is behind gets nothing rather than the wrong
    // thing.
    AudioRing ring;
    ring.set_depth_millis(kDepthMs);

    ring.write(make_chunk(0, 480, 0.5F));
    const RingFormat opened = ring.format();

    // The receiver came back at another rate, which is a remove and an add
    // on the engine's side and re-establishes the ring.
    ring.write(make_chunk(0, 480, 0.75F, true, 0, 16000, 1));
    REQUIRE(ring.format() != opened);

    std::vector<float> out(240, -99.0F);
    const ReadResult got = ring.read(out.data(), 240, opened);

    CHECK(got.format_moved);
    CHECK(got.format == ring.format());
    CHECK(got.frames_from_ring == 0);
    CHECK(got.frames_starved == 240);

    // Untouched, because only the caller knows how many floats it holds:
    // the sink's channel count is not always the ring's.
    CHECK(out[0] == -99.0F);
    CHECK(out[239] == -99.0F);

    // And nothing was consumed, so the audio at the new format is still
    // there for the reader that reopens at it.
    CHECK(ring.frames_buffered() == 480);

    const ReadResult again = drain(ring, out, 240);
    CHECK_FALSE(again.format_moved);
    CHECK(again.frames_from_ring == 240);
    CHECK(out[0] == 0.75F);
}

TEST_CASE("a closed gate reads back as gated and not as audio", "[audio][ring]")
{
    // REJECTS: dropping squelch_open on the floor. Zeros from a shut gate
    // and zeros from a dead band are the same samples, and the chunk is the
    // only thing that says which. Without this an operator watching a
    // silent channel cannot tell a squelch set too tight from a receiver
    // tuned to nothing, which is the one question a squelch indicator
    // exists to answer.
    AudioRing ring;
    ring.set_depth_millis(kDepthMs);

    ring.write(make_chunk(0, 240, 0.0F, /*squelch_open=*/false));

    std::vector<float> out;
    const ReadResult got = drain(ring, out, 240);
    CHECK(got.frames_from_ring == 240);
    CHECK(got.last_source == FrameSource::gated);
}

TEST_CASE("the reported source follows the card and not the wire", "[audio][ring]")
{
    // REJECTS: publishing the most recently WRITTEN chunk's flag, which is
    // what a single member updated in write() gives and is what an
    // indicator naturally gets wired to. The ring is a buffer: at the
    // shipped depth the newest chunk is up to 200 ms ahead of what is
    // coming out of the speaker, so the light would open before the audio
    // did and shut while the last of the speech was still playing. Here
    // three chunks are written and only the first is read, and the answer
    // has to be the first one's.
    AudioRing ring;
    ring.set_depth_millis(kDepthMs);

    ring.write(make_chunk(0, 480, 0.5F, /*squelch_open=*/true));
    ring.write(make_chunk(480, 480, 0.0F, /*squelch_open=*/false));
    ring.write(make_chunk(960, 480, 0.0F, /*squelch_open=*/false));

    std::vector<float> out;
    const ReadResult got = drain(ring, out, 480);
    REQUIRE(got.frames_from_ring == 480);
    CHECK(got.last_source == FrameSource::audio);

    // And it follows on the next pull, rather than being latched.
    const ReadResult next = drain(ring, out, 480);
    REQUIRE(next.frames_from_ring == 480);
    CHECK(next.last_source == FrameSource::gated);
}

TEST_CASE("gap silence is not reported as a closed gate", "[audio][ring]")
{
    // REJECTS: one bool for "is this silence". A shut squelch is the radio
    // working as configured and a gap is audio that was lost. Merging them
    // makes a lossy link look like a quiet channel, which is precisely the
    // confusion the squelch flag is on the wire to prevent.
    AudioRing ring;
    ring.set_depth_millis(kDepthMs);

    ring.write(make_chunk(0, 480, 0.5F));
    ring.write(make_chunk(980, 480, 0.5F, true, 500));

    std::vector<float> out;
    REQUIRE(drain(ring, out, 480).frames_from_ring == 480);
    const ReadResult in_gap = drain(ring, out, 500);
    CHECK(in_gap.last_source == FrameSource::gap_fill);
}

TEST_CASE("a rate change re-establishes the ring", "[audio][ring]")
{
    // REJECTS: caching the format from the first chunk. The schema puts
    // sampleRate and channelCount on EVERY chunk and says why: a pane
    // outlives the receiver behind it, because receiver_link.cpp turns a
    // refused width into a remove and an add, and the next receiver can be
    // at another rate. Playing the new stream at the old rate is a tape at
    // the wrong speed, and it sounds like a broken demodulator.
    AudioRing ring;
    ring.set_depth_millis(kDepthMs);

    ring.write(make_chunk(0, 480, 0.5F));
    const std::uint64_t first_generation = ring.format_generation();
    REQUIRE(ring.format().sample_rate == kRate);
    REQUIRE(ring.frames_buffered() == 480);

    ring.write(make_chunk(0, 480, 0.5F, true, 0, 16000, 1));

    CHECK(ring.format_generation() == first_generation + 1);
    CHECK(ring.format().sample_rate == 16000);

    // The depth is honoured at the new rate rather than carried across in
    // frames: 100 ms of 16 kHz is 1600 frames and not 4800.
    CHECK(ring.capacity_frames() == 1600);

    // What was buffered at the old rate went with it.
    CHECK(ring.frames_buffered() == 480);
}

TEST_CASE("the snapshot's format and generation come from the same write",
          "[audio][ring][threads]")
{
    // REJECTS: reading format() and then format_generation(), which is
    // what AudioPlayer::tick did. Both are honest calls and the pair is
    // not: the Cap'n Proto event loop can establish a new format between
    // them, and the caller then holds the OLD shape stamped with the NEW
    // generation. tick opened a sink at the old rate, recorded the new
    // generation against it, and the reopen branch never fired again
    // because it only fires on a generation PAST the recorded one. Silent
    // for the rest of the stream, with no fault and a status line saying
    // "starving".
    //
    // The invariant that catches it without needing to catch the race in
    // the act: every establish bumps the generation by exactly one, so the
    // Nth establish's format is known in advance. Any (format, generation)
    // pair that does not satisfy expected_rate came from two different
    // writes.
    constexpr int kWrites = 20000;

    // Consecutive values always differ, so every write re-establishes.
    const auto expected_rate = [](std::uint64_t generation) -> std::uint32_t {
        return 8000U * static_cast<std::uint32_t>(1U + (generation % 5U));
    };

    AudioRing ring;
    ring.set_depth_millis(20);

    std::atomic<bool> writing{true};

    std::thread writer([&] {
        for (std::uint64_t generation = 1; generation <= kWrites; ++generation) {
            ring.write(make_chunk(0, 64, 0.25F, true, 0, expected_rate(generation), 1));
        }
        writing.store(false, std::memory_order_release);
    });

    std::uint64_t snapshot_reads = 0;
    std::uint64_t snapshot_tears = 0;
    std::uint64_t split_reads = 0;
    std::uint64_t split_tears = 0;

    while (writing.load(std::memory_order_acquire)) {
        const AudioRing::Snapshot state = ring.snapshot();
        if (state.generation > 0) {
            ++snapshot_reads;
            if (state.format.sample_rate != expected_rate(state.generation)) {
                ++snapshot_tears;
            }
        }

        // The old shape, run beside the new one against the same writer so
        // the comparison is not two different runs. Its result is reported
        // and not asserted: a tear is a race, and a machine that happened
        // to serialise the two threads for a whole run would see none,
        // which would make an assertion here fail for the wrong reason.
        const RingFormat split_format = ring.format();
        const std::uint64_t split_generation = ring.format_generation();
        if (split_generation > 0) {
            ++split_reads;
            if (split_format.sample_rate != expected_rate(split_generation)) {
                ++split_tears;
            }
        }
    }

    writer.join();

    REQUIRE(snapshot_reads > 0);
    CHECK(snapshot_tears == 0);

    INFO("snapshot: " << snapshot_tears << " torn of " << snapshot_reads);
    INFO("two calls: " << split_tears << " torn of " << split_reads);
    if (split_tears == 0) {
        // Measured over five runs on 2026-09-20, RelWithDebInfo: the
        // two-call form tore on 444 to 585 reads of the 8300 to 13000 it
        // managed, about one in twenty, while the snapshot tore on none of
        // roughly fifty thousand. A run that sees no split tear has not
        // shown the defect gone, only that this machine did not
        // interleave; the assertion above is what holds.
        WARN("the two-call form did not tear in this run");
    }
}

TEST_CASE("a malformed chunk does not tear down a healthy stream", "[audio][ring]")
{
    // THIS CASE USED TO SAY IT REJECTED A DIVIDE BY ZERO, and that was
    // never the hazard. It read "AudioChunk::frames divides by
    // channel_count, and a chunk with a channel count of zero from a
    // damaged or hostile peer is a divide by zero inside the RPC event
    // loop's callback". core/rpc/types.h guards that division and returns
    // zero frames, so there was no division to guard and the case was
    // certifying a danger that did not exist.
    //
    // REJECTS, for real: letting an invalid format reach establish().
    // write() establishes on any format that differs from the current one,
    // and an invalid format differs. So ONE bad chunk in the middle of a
    // running stream would drop everything buffered with no counter
    // charged for it, bump the format generation, and leave format()
    // invalid, which AudioPlayer::tick reads as no stream at all and
    // closes the sink for. A single malformed chunk from a damaged or
    // hostile peer would cost the ring's whole depth of real audio and a
    // sink reopen, and nothing but malformed_chunks would say why.
    AudioRing ring;
    ring.set_depth_millis(kDepthMs);

    // A stream that is running and has audio in it, which is the state the
    // refusal has to protect.
    ring.write(make_chunk(0, 480, 0.5F));
    const RingFormat established = ring.format();
    const std::uint64_t generation = ring.format_generation();
    REQUIRE(ring.frames_buffered() == 480);

    AudioChunk no_channels = make_chunk(480, 4, 0.25F);
    no_channels.channel_count = 0;
    ring.write(no_channels);

    ring.write(make_chunk(480, 4, 0.25F, true, 0, 0, 1));

    CHECK(ring.counts().malformed_chunks == 2);

    // Nothing of the running stream moved: not the audio, not the format,
    // not the generation the player reopens on, and not the written count.
    CHECK(ring.frames_buffered() == 480);
    CHECK(ring.format() == established);
    CHECK(ring.format_generation() == generation);
    CHECK(ring.counts().frames_written == 480);

    // And the timeline is untouched, so the next real chunk is contiguous
    // rather than being charged a gap for the frames the bad ones claimed.
    ring.write(make_chunk(480, 480, 0.75F));
    CHECK(ring.counts().gap_events == 0);
    CHECK(ring.counts().restarts == 0);
    CHECK(ring.frames_buffered() == 960);
}

TEST_CASE("an index that goes backwards restarts rather than wrapping", "[audio][ring]")
{
    // REJECTS: computing the gap as an unsigned subtraction without the
    // ordering test. sample_index is a uint64, so 100 - 5000 is about
    // eighteen quintillion, and the resync branch would fire with
    // frames_gap_discarded set to a number that makes the display
    // meaningless for the life of the stream. The engine's invariant says
    // this cannot arise inside one receiver's stream, which is exactly why
    // nothing would notice it was possible.
    AudioRing ring;
    ring.set_depth_millis(kDepthMs);

    ring.write(make_chunk(5000, 480, 0.1F));
    ring.write(make_chunk(100, 480, 0.2F));

    const RingCounts counts = ring.counts();
    CHECK(counts.restarts == 1);
    CHECK(counts.resyncs == 0);
    CHECK(counts.frames_filled == 0);
    CHECK(counts.frames_gap_discarded == 0);
    CHECK(ring.frames_buffered() == 960);
}

TEST_CASE("stereo is counted in frames and not in samples", "[audio][ring]")
{
    // REJECTS: treating samples.size() as the frame count. A WFM receiver
    // decoding stereo sends channelCount 2, so a ring that counted
    // interleaved samples reports double the depth, measures every gap at
    // half its size, and places the fill half a frame out of phase between
    // the two channels. That was a prediction when this case was written and
    // stopped being one in f3c0544.
    AudioRing ring;
    ring.set_depth_millis(kDepthMs);

    ring.write(make_chunk(0, 240, 0.25F, true, 0, kRate, 2));
    CHECK(ring.frames_buffered() == 240);
    CHECK(ring.capacity_frames() == kCapacity);

    std::vector<float> out;
    out.assign(240 * 2, -99.0F);
    const ReadResult got = ring.read(out.data(), 240, ring.format());
    CHECK(got.frames_from_ring == 240);
    CHECK(out[0] == 0.25F);
    CHECK(out[479] == 0.25F);
}

TEST_CASE("reset clears the format so a new grant takes effect", "[audio][ring]")
{
    // REJECTS: keeping the format across a reset. The ring is sized from
    // the depth the engine GRANTED, and the grant is only known after
    // subscribe_audio returns, which is after the chunk callback has
    // already been installed and can already have established the ring at
    // the provisional depth. If reset did not clear the format, the next
    // chunk would match and the ring would keep running at the depth that
    // was asked for rather than the one that was granted, which is the
    // exact failure bufferMillisGranted is reported to prevent.
    AudioRing ring;
    ring.set_depth_millis(kDepthMs);
    ring.write(make_chunk(0, 480, 0.5F));
    REQUIRE(ring.capacity_frames() == kCapacity);

    const std::uint64_t before = ring.format_generation();

    ring.set_depth_millis(500);
    ring.reset();

    // The generation does NOT move here, and a consumer must not be
    // written as though it does. reset() leaves the format invalid, which
    // is what says there is no stream; the bump comes from establish() on
    // the next chunk. The header used to claim reset() bumped it and never
    // has, so this pins which of the two was wrong.
    CHECK(ring.format_generation() == before);
    CHECK_FALSE(ring.format().valid());

    ring.write(make_chunk(0, 480, 0.5F));

    CHECK(ring.capacity_frames() == 24000);
    CHECK(ring.format_generation() == before + 1);
}

TEST_CASE("a chunk longer than the ring grows it rather than resyncing", "[audio][ring]")
{
    // REJECTS: treating an oversized chunk as a gap that does not fit. A
    // client that asked for the engine's 20 ms minimum against a source
    // whose blocks are longer than that gets chunks bigger than its own
    // ring, and resyncing on every one of them plays nothing at all while
    // every counter except resyncs reads healthy. The engine applies a
    // two-chunk floor to its own queue for the same reason.
    AudioRing ring;
    ring.set_depth_millis(20);  // 960 frames at kRate

    ring.write(make_chunk(0, 4096, 0.5F));

    CHECK(ring.counts().resyncs == 0);
    CHECK(ring.counts().frames_written == 4096);
    CHECK(ring.frames_buffered() == 4096);
    CHECK(ring.capacity_frames() >= 4096);
}

TEST_CASE("the timeline accounts for every frame the engine indexed", "[audio][ring]")
{
    // The invariant the gap policy exists to hold, over a randomised run
    // rather than one hand-built case: between the first chunk's index and
    // the end of the last, every frame is either one this ring wrote, one
    // it filled, or one it discarded at a resync. A policy that skipped
    // gaps instead of filling them fails this by exactly the frames it
    // skipped, and skipping is the implementation this test exists to
    // reject.
    //
    // SEED IS FIXED AND PRINTED, per the house rule, so a failure here is
    // one command away from being reproduced.
    constexpr unsigned kSeed = 20260920U;
    INFO("seed " << kSeed);

    std::mt19937 rng(kSeed);
    std::uniform_int_distribution<int> chunk_frames(64, 2048);
    std::uniform_int_distribution<int> gap_frames(0, 8000);
    std::uniform_int_distribution<int> read_frames(0, 3000);
    std::uniform_int_distribution<int> gate(0, 4);

    AudioRing ring;
    ring.set_depth_millis(kDepthMs);

    std::uint64_t index = 123456;

    // Taken from the FIRST CHUNK and not from where the counter started.
    // A gap before the first chunk of a stream is not a gap: the ring has
    // no previous index to measure against and the schema says the stream
    // opens late by design. Measuring from before it would charge the ring
    // for silence it correctly refused to invent.
    std::uint64_t first_index = 0;
    bool have_first = false;
    std::vector<float> out;

    for (int step = 0; step < 500; ++step) {
        const auto gap = static_cast<std::uint64_t>(gap_frames(rng));
        const auto frames = static_cast<std::size_t>(chunk_frames(rng));
        index += gap;
        if (!have_first) {
            first_index = index;
            have_first = true;
        }
        ring.write(make_chunk(index, frames, 0.5F, gate(rng) != 0, gap));
        index += frames;

        const auto want = static_cast<std::size_t>(read_frames(rng));
        if (want > 0) {
            // The read's own result is not what this case is about; the
            // point is that draining at a rate unrelated to the write rate
            // is what makes the overrun and starve branches both fire.
            static_cast<void>(drain(ring, out, want));
        }
    }

    const RingCounts counts = ring.counts();
    CHECK(counts.frames_written + counts.frames_filled + counts.frames_gap_discarded ==
          index - first_index);

    // And the two halves of the gap always sum to the gap that was seen.
    CHECK(counts.frames_gap_wire + counts.frames_gap_upstream ==
          counts.frames_filled + counts.frames_gap_discarded);
}
