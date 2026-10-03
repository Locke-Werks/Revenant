// core/transcribe/segmenter.h: one receiver's chunks in, utterances out.
//
// Chunks are built here the way the engine delivers them, a run of frames
// with a gate, a stream index and a place on the source clock. Where the
// utterance's audio itself is the point, the rate is 16000 so the resampler
// passes it through and the comparison can be exact; where the resampler is
// part of what is under test, the rate is a real one. The level detector is
// fed generated noise, tones and amplitude-modulated noise standing in for
// speech, from fixed seeds.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "core/transcribe/segmenter.h"

namespace {

using revenant::transcribe::kRecogniserRateHz;
using revenant::transcribe::Segmenter;
using revenant::transcribe::SegmenterChunk;
using revenant::transcribe::SegmenterConfig;
using revenant::transcribe::segmenter_config_for;
using revenant::transcribe::SegmentBy;
using revenant::transcribe::Utterance;

constexpr std::uint64_t kSeed = 20261003;

// A P25 Logical Link Data Unit at 8000 S/s: nine 20 ms IMBE frames.
constexpr std::uint32_t kVoiceRate = 8000;
constexpr std::size_t kLdu = 1440;

struct Stream {
    Segmenter segmenter;
    std::vector<Utterance> out;
    std::uint32_t rate = kVoiceRate;
    std::uint32_t channels = 1;
    std::uint64_t epoch = 0;
    std::uint64_t next = 0;

    explicit Stream(SegmenterConfig config, std::uint32_t r = kVoiceRate) : segmenter(config), rate(r) {}

    void push_at(std::uint64_t start, std::span<const float> samples, bool gate)
    {
        SegmenterChunk chunk;
        chunk.samples = samples;
        chunk.channels = channels;
        chunk.rate = rate;
        chunk.start = start;
        chunk.gate = gate;
        chunk.tuning_epoch = epoch;
        segmenter.push(chunk, out);
        next = start + samples.size() / channels;
    }

    void push(std::span<const float> samples, bool gate) { push_at(next, samples, gate); }

    // Skips `frames` frames, as a chunk dropped upstream does.
    void drop(std::size_t frames) { next += frames; }
};

std::vector<float> voice(std::size_t frames, std::uint32_t rate = kVoiceRate)
{
    std::vector<float> x(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        x[i] = static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * 500.0 * static_cast<double>(i) / rate));
    }
    return x;
}

std::vector<float> silence(std::size_t frames)
{
    return std::vector<float>(frames, 0.0F);
}

// Exact in float, and different at every sample within a period of 4096, so
// a lost or repeated sample cannot line up with its neighbour by chance.
std::vector<float> ramp(std::size_t frames, std::size_t from = 0)
{
    std::vector<float> x(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        x[i] = static_cast<float>(static_cast<double>((from + i) % 4096) - 2048.0) / 2048.0F;
    }
    return x;
}

}  // namespace

TEST_CASE("the per-kind configs carry the hangovers the header states", "[transcribe][segmenter]") {
    // REJECTS: one config for all three, which either splits a P25 call at
    // every lost data unit or holds a squelch-flutter receiver open across
    // two transmissions.
    CHECK(segmenter_config_for(SegmentBy::Call).hangover_seconds == 0.5);
    CHECK(segmenter_config_for(SegmentBy::Squelch).hangover_seconds == 0.6);
    CHECK(segmenter_config_for(SegmentBy::Level).hangover_seconds == 0.6);
    CHECK(segmenter_config_for(SegmentBy::Level).pre_roll_seconds == 0.2);
    for (const SegmentBy by : {SegmentBy::Call, SegmentBy::Squelch, SegmentBy::Level}) {
        CHECK(segmenter_config_for(by).by == by);
        CHECK(segmenter_config_for(by).max_seconds < 30.0);
    }
}

TEST_CASE("a call is cut by its gate, with the hangover bridging a lost data unit", "[transcribe][segmenter]") {
    // REJECTS: closing on the first gate-shut chunk, which turns this call
    // into two pieces of 0.36 s and 0.54 s that are both then dropped as too
    // short; a hangover counted in chunks rather than time; and dropping the
    // bridged gap's frames, which moves the second half of the call 180 ms
    // earlier than it was said.
    Stream s(segmenter_config_for(SegmentBy::Call));
    const auto v = voice(kLdu);
    const auto q = silence(kLdu);
    s.push(v, true);
    s.push(v, true);
    s.push(q, false);
    s.push(v, true);
    s.push(v, true);
    s.push(v, true);
    CHECK(s.segmenter.open());
    s.push(q, false);
    s.push(q, false);
    CHECK(s.out.empty());  // 360 ms shut is inside the 500 ms hangover
    s.push(q, false);      // 540 ms is not
    REQUIRE(s.out.size() == 1);
    CHECK_FALSE(s.segmenter.open());

    const Utterance& u = s.out[0];
    CHECK(u.start_sample == 0);
    CHECK(u.end_sample == 6 * kLdu);
    CHECK(u.sample_rate == kVoiceRate);
    CHECK(u.cut_by == SegmentBy::Call);
    CHECK(u.pcm.size() == 2 * 6 * kLdu);

    // The lost data unit is silence where it was, and voice either side.
    const std::size_t middle = 2 * (2 * kLdu + kLdu / 2);
    CHECK(std::abs(u.pcm[middle]) < 1e-3F);
    float loud = 0.0F;
    for (std::size_t i = 2 * 4 * kLdu; i < 2 * 5 * kLdu; ++i) {
        loud = std::max(loud, std::abs(u.pcm[i]));
    }
    CHECK(loud > 0.29F);
}

TEST_CASE("a dropped chunk is silence if the hangover covers it and the end if not", "[transcribe][segmenter]") {
    // REJECTS: ignoring the jump in the stream index, which joins the audio
    // either side of a dropout as though nothing were missing, and bridging
    // a dropout of any length, which makes one utterance of two calls.
    const auto v = voice(kLdu);
    SECTION("one data unit missing") {
        Stream s(segmenter_config_for(SegmentBy::Call));
        s.push(v, true);
        s.push(v, true);
        s.drop(kLdu);
        s.push(v, true);
        s.push(v, true);
        s.segmenter.flush(s.out);
        REQUIRE(s.out.size() == 1);
        CHECK(s.out[0].start_sample == 0);
        CHECK(s.out[0].end_sample == 5 * kLdu);
        CHECK(s.out[0].pcm.size() == 2 * 5 * kLdu);
        CHECK(std::abs(s.out[0].pcm[2 * (2 * kLdu + kLdu / 2)]) < 1e-3F);
    }
    SECTION("four missing, 720 ms") {
        Stream s(segmenter_config_for(SegmentBy::Call));
        for (int i = 0; i < 4; ++i) {
            s.push(v, true);
        }
        s.drop(4 * kLdu);
        for (int i = 0; i < 4; ++i) {
            s.push(v, true);
        }
        REQUIRE(s.out.size() == 1);  // closed by the gap, before the new chunk
        s.segmenter.flush(s.out);
        REQUIRE(s.out.size() == 2);
        CHECK(s.out[0].start_sample == 0);
        CHECK(s.out[0].end_sample == 4 * kLdu);
        CHECK(s.out[1].start_sample == 8 * kLdu);
        CHECK(s.out[1].end_sample == 12 * kLdu);
    }
}

TEST_CASE("an utterance shorter than min_seconds is dropped", "[transcribe][segmenter]") {
    // REJECTS: no minimum at all, which sends every key-up to the
    // recogniser, and a minimum measured with the hangover's silence
    // counted in, which keeps a 0.36 s key-up because the gate took another
    // half second to give up on it.
    SECTION("two data units, 0.36 s") {
        Stream s(segmenter_config_for(SegmentBy::Call));
        s.push(voice(2 * kLdu), true);
        for (int i = 0; i < 4; ++i) {
            s.push(silence(kLdu), false);
        }
        s.segmenter.flush(s.out);
        CHECK(s.out.empty());
        CHECK_FALSE(s.segmenter.open());
    }
    SECTION("one frame under and exactly at 0.6 s") {
        Stream s(segmenter_config_for(SegmentBy::Call));
        s.push(voice(4799), true);
        s.segmenter.flush(s.out);
        CHECK(s.out.empty());
        s.drop(kVoiceRate);
        s.push(voice(4800), true);
        s.segmenter.flush(s.out);
        REQUIRE(s.out.size() == 1);
        CHECK(s.out[0].end_sample - s.out[0].start_sample == 4800);
    }
}

TEST_CASE("max_seconds cuts a long transmission into contiguous utterances", "[transcribe][segmenter]") {
    // REJECTS: cutting only at a chunk boundary, which overruns the limit by
    // up to a chunk; dropping the rest of the chunk that crossed the limit;
    // repeating it in the next utterance; and dropping the short last piece
    // as under min_seconds when it is the end of a long call.
    SECTION("16000 in, so the audio itself can be compared") {
        constexpr std::size_t kChunk = 1234;
        constexpr std::size_t kChunks = 778;  // 960052 frames, a hair over 60 s
        Stream s(segmenter_config_for(SegmentBy::Call), kRecogniserRateHz);
        for (std::size_t i = 0; i < kChunks; ++i) {
            s.push(ramp(kChunk, i * kChunk), true);
        }
        s.segmenter.flush(s.out);
        REQUIRE(s.out.size() == 3);
        CHECK(s.out[0].start_sample == 0);
        CHECK(s.out[0].end_sample == 448000);
        CHECK(s.out[1].start_sample == 448000);
        CHECK(s.out[1].end_sample == 896000);
        CHECK(s.out[2].start_sample == 896000);
        CHECK(s.out[2].end_sample == kChunk * kChunks);
        std::vector<float> joined;
        for (const Utterance& u : s.out) {
            CHECK(u.pcm.size() == u.end_sample - u.start_sample);
            joined.insert(joined.end(), u.pcm.begin(), u.pcm.end());
        }
        CHECK(joined == ramp(kChunk * kChunks));
    }
    SECTION("8000 in, a short last piece") {
        SegmenterConfig config = segmenter_config_for(SegmentBy::Call);
        config.max_seconds = 2.0;
        Stream s(config);
        for (int i = 0; i < 24; ++i) {  // 4.32 s: two of 2 s and 0.32 s left over
            s.push(voice(kLdu), true);
        }
        s.segmenter.flush(s.out);
        REQUIRE(s.out.size() == 3);
        CHECK(s.out[0].end_sample == 16000);
        CHECK(s.out[1].start_sample == 16000);
        CHECK(s.out[1].end_sample == 32000);
        CHECK(s.out[2].start_sample == 32000);
        CHECK(s.out[2].end_sample == 24 * kLdu);
        for (const Utterance& u : s.out) {
            CHECK(u.pcm.size() == 2 * (u.end_sample - u.start_sample));
        }
    }
}

TEST_CASE("a new tuning epoch closes the open utterance first", "[transcribe][segmenter]") {
    // REJECTS: one utterance across a retune, which hands the recogniser two
    // stations as one sentence, and a retune that discards what was open
    // instead of emitting it.
    Stream s(segmenter_config_for(SegmentBy::Call));
    for (int i = 0; i < 5; ++i) {
        s.push(voice(kLdu), true);
    }
    s.epoch = 1;
    s.push(voice(kLdu), true);
    REQUIRE(s.out.size() == 1);
    CHECK(s.out[0].end_sample == 5 * kLdu);
    for (int i = 0; i < 4; ++i) {
        s.push(voice(kLdu), true);
    }
    s.segmenter.flush(s.out);
    REQUIRE(s.out.size() == 2);
    CHECK(s.out[1].start_sample == 5 * kLdu);
    CHECK(s.out[1].end_sample == 10 * kLdu);
}

TEST_CASE("a stream index that goes backwards closes the open utterance", "[transcribe][segmenter]") {
    // REJECTS: reading a restarted stream as one long call, with an
    // end_sample before its start_sample.
    Stream s(segmenter_config_for(SegmentBy::Call));
    for (int i = 0; i < 5; ++i) {
        s.push(voice(kLdu), true);
    }
    s.push_at(0, voice(kLdu * 5), true);
    s.segmenter.flush(s.out);
    REQUIRE(s.out.size() == 2);
    CHECK(s.out[0].end_sample == 5 * kLdu);
    CHECK(s.out[1].start_sample == 0);
    CHECK(s.out[1].end_sample == 5 * kLdu);
}

TEST_CASE("flush emits what is open and reset forgets it", "[transcribe][segmenter]") {
    // REJECTS: a flush that drops the open utterance, which loses the last
    // call when the switch goes off, and a reset that emits it, which sends
    // half a call from a receiver that has gone.
    Stream s(segmenter_config_for(SegmentBy::Call));
    CHECK_FALSE(s.segmenter.open());
    for (int i = 0; i < 5; ++i) {
        s.push(voice(kLdu), true);
    }
    CHECK(s.segmenter.open());
    s.segmenter.flush(s.out);
    CHECK_FALSE(s.segmenter.open());
    REQUIRE(s.out.size() == 1);
    CHECK(s.out[0].end_sample == 5 * kLdu);

    for (int i = 0; i < 5; ++i) {
        s.push(voice(kLdu), true);
    }
    CHECK(s.segmenter.open());
    s.segmenter.reset();
    CHECK_FALSE(s.segmenter.open());
    s.segmenter.flush(s.out);
    CHECK(s.out.size() == 1);

    // And the stream after a reset is a fresh one, whatever index it starts at.
    s.push_at(1000, voice(5 * kLdu), true);
    s.segmenter.flush(s.out);
    REQUIRE(s.out.size() == 2);
    CHECK(s.out[1].start_sample == 1000);
    CHECK(s.out[1].end_sample == 1000 + 5 * kLdu);
}

TEST_CASE("stereo is mixed to mono, half of each channel", "[transcribe][segmenter]") {
    // REJECTS: taking the left channel alone, summing without halving,
    // which doubles the level of anything in both, and reading interleaved
    // pairs as consecutive mono frames, which doubles the length and halves
    // every frequency.
    constexpr std::size_t kFrames = 16000;
    std::vector<float> left(kFrames);
    std::vector<float> right(kFrames);
    std::vector<float> stereo(2 * kFrames);
    std::vector<float> expected(kFrames);
    for (std::size_t i = 0; i < kFrames; ++i) {
        left[i] = static_cast<float>(i % 1024) / 1024.0F;
        right[i] = static_cast<float>(i % 7) / 8.0F;
        stereo[2 * i] = left[i];
        stereo[2 * i + 1] = right[i];
        expected[i] = (left[i] + right[i]) * 0.5F;
    }
    Stream s(segmenter_config_for(SegmentBy::Squelch), kRecogniserRateHz);
    s.channels = 2;
    s.push(stereo, true);
    s.segmenter.flush(s.out);
    REQUIRE(s.out.size() == 1);
    CHECK(s.out[0].end_sample == kFrames);
    CHECK(s.out[0].cut_by == SegmentBy::Squelch);
    CHECK(s.out[0].pcm == expected);
}

TEST_CASE("an utterance is placed on the source clock through each chunk's own source_end",
          "[transcribe][segmenter]") {
    // REJECTS: placing on a fixed origin times the stream index, which
    // ignores that each engine block reaches the receiver with its own
    // latency; placing a chunk's first frame at its source_end rather than
    // its last; and placing the utterance's end from the chunk that closed
    // it, three chunks of hangover later, rather than from its last frame.
    constexpr std::size_t kChunk = 800;
    constexpr double kSourceRate = 2'400'000.0;  // 300 source samples a frame at 8000
    constexpr std::uint64_t kPerFrame = 300;
    const auto source_end = [](std::size_t k) {
        return std::uint64_t{1'000'000} + (k + 1) * kChunk * kPerFrame + (k * 37) % 101;
    };
    const auto chunk = [&](std::size_t k, std::span<const float> samples, bool gate) {
        SegmenterChunk c;
        c.samples = samples;
        c.rate = kVoiceRate;
        c.start = k * kChunk;
        c.gate = gate;
        c.source_end = source_end(k);
        c.source_rate = kSourceRate;
        return c;
    };
    const auto v = voice(kChunk);
    const auto q = silence(kChunk);

    SECTION("opened and closed by the gate") {
        Segmenter seg(segmenter_config_for(SegmentBy::Call));
        std::vector<Utterance> out;
        for (std::size_t k = 0; k < 16; ++k) {
            const bool voiced = k >= 2 && k <= 9;
            seg.push(chunk(k, voiced ? v : q, voiced), out);
        }
        REQUIRE(out.size() == 1);
        CHECK(out[0].start_sample == 2 * kChunk);
        CHECK(out[0].end_sample == 10 * kChunk);
        CHECK(out[0].source_start == source_end(2) - kChunk * kPerFrame);
        CHECK(out[0].source_end == source_end(9));
    }
    SECTION("cut by max_seconds in the middle of a chunk") {
        SegmenterConfig config = segmenter_config_for(SegmentBy::Call);
        config.max_seconds = 1.05;  // 8400 frames, half way through chunk 10
        Segmenter seg(config);
        std::vector<Utterance> out;
        for (std::size_t k = 0; k < 20; ++k) {
            seg.push(chunk(k, v, true), out);
        }
        seg.flush(out);
        REQUIRE(out.size() == 2);
        CHECK(out[0].end_sample == 8400);
        CHECK(out[1].start_sample == 8400);
        const std::uint64_t cut = source_end(10) - (11 * kChunk - 8400) * kPerFrame;
        CHECK(out[0].source_start == source_end(0) - kChunk * kPerFrame);
        CHECK(out[0].source_end == cut);
        CHECK(out[1].source_start == cut);
        CHECK(out[1].source_end == source_end(19));
    }
}

TEST_CASE("squelch flutter inside a transmission is bridged at 48000", "[transcribe][segmenter]") {
    // REJECTS: the same defects as the call case, through the resampler at
    // a receiver's own audio rate: an utterance's 16 kHz length must be its
    // stream length scaled by a third.
    constexpr std::uint32_t kRate = 48000;
    Stream s(segmenter_config_for(SegmentBy::Squelch), kRate);
    s.push(voice(kRate, kRate), true);
    s.push(silence(kRate * 3 / 10), false);
    s.push(voice(kRate, kRate), true);
    s.push(silence(kRate), false);
    REQUIRE(s.out.size() == 1);
    CHECK(s.out[0].start_sample == 0);
    CHECK(s.out[0].end_sample == kRate * 23 / 10);
    CHECK(s.out[0].pcm.size() == 16000 * 23 / 10);
}

namespace {

// Background noise at -40 dBFS, then whatever `shape` adds at each frame.
template <typename Shape>
std::vector<float> scene(std::uint32_t rate, double seconds, std::uint64_t seed, Shape shape)
{
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);
    const auto n = static_cast<std::size_t>(std::llround(seconds * rate));
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / rate;
        const double g = gauss(rng);
        const double h = gauss(rng);
        x[i] = static_cast<float>(shape(t, g, h));
    }
    return x;
}

// Fed in an awkward chunk size so frames straddle chunks.
std::vector<Utterance> level(std::uint32_t rate, std::span<const float> x)
{
    Stream s(segmenter_config_for(SegmentBy::Level), rate);
    constexpr std::size_t kChunk = 517;
    for (std::size_t at = 0; at < x.size(); at += kChunk) {
        s.push(x.subspan(at, std::min(kChunk, x.size() - at)), true);
    }
    s.segmenter.flush(s.out);
    return std::move(s.out);
}

// Speech stand-in: noise 20 dB over the background under a raised-cosine
// envelope at four syllables a second, which falls to nothing between them.
double syllables(double t, double from, double to)
{
    if (t < from || t >= to) {
        return 0.0;
    }
    return 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * 4.0 * (t - from));
}

}  // namespace

TEST_CASE("level: steady noise and a steady carrier do not open, a few dB of step included",
          "[transcribe][segmenter][level]") {
    // REJECTS: a fixed threshold, which either never opens or opens on the
    // noise of a busier band; a floor that does not follow the level up,
    // which takes two 6 dB steps four seconds apart as one of 12 dB and
    // opens on the second; and a margin of a few dB, which opens on noise's
    // own frame-to-frame wander.
    const std::uint32_t rate = GENERATE(8000U, 48000U);
    INFO("rate " << rate << ", seed " << kSeed);
    const double step = std::pow(10.0, 6.0 / 20.0);
    const auto stepped = [step](double t) { return t < 4.0 ? 1.0 : t < 8.0 ? step : step * step; };
    const auto noise = scene(rate, 12.0, kSeed, [&](double t, double g, double) {
        return 0.01 * g * stepped(t);
    });
    CHECK(level(rate, noise).empty());
    const auto carrier = scene(rate, 12.0, kSeed + 1, [&](double t, double g, double) {
        return 0.03 * std::sin(2.0 * std::numbers::pi * 1000.0 * t) * stepped(t) + 0.001 * g;
    });
    CHECK(level(rate, carrier).empty());
    const auto from_zeros = scene(rate, 6.0, kSeed + 2, [&](double t, double g, double) {
        return t < 2.0 ? 0.0 : 0.01 * g;
    });
    CHECK(level(rate, from_zeros).empty());
}

TEST_CASE("level: a carrier keyed up 20 dB over the noise is let go within seconds",
          "[transcribe][segmenter][level]") {
    // REJECTS: a floor that never rises, which holds the detector open on a
    // dead carrier until max_seconds cuts it, and then again, for as long
    // as the carrier stays up. At 3 dB/s the floor is within the margin
    // (20 - 10) / 3 s after the key-up and the hangover closes it 0.6 s
    // later; the bound below leaves two seconds over that.
    const std::uint32_t rate = GENERATE(8000U, 48000U);
    INFO("rate " << rate << ", seed " << kSeed);
    const auto keyed = scene(rate, 14.0, kSeed + 5, [&](double t, double g, double) {
        const double carrier = t >= 2.0 ? 0.1 * std::sin(2.0 * std::numbers::pi * 1000.0 * t) : 0.0;
        return 0.00707 * g + carrier;
    });
    const auto heard = level(rate, keyed);
    REQUIRE(heard.size() == 1);
    const double r = rate;
    CHECK(heard[0].start_sample <= static_cast<std::uint64_t>(2.0 * r));
    CHECK(heard[0].end_sample < static_cast<std::uint64_t>((2.0 + 10.0 / 3.0 + 0.6 + 2.0) * r));
}

TEST_CASE("level: speech-like bursts and a tone burst open, with the pre-roll in front",
          "[transcribe][segmenter][level]") {
    // REJECTS: a detector that never opens; one that closes between
    // syllables, which shreds a sentence into pieces each dropped as too
    // short; one with no pre-roll, whose utterance starts at the first loud
    // frame, after the word's onset; and one that never closes, which joins
    // two transmissions 1.5 s apart.
    const std::uint32_t rate = GENERATE(8000U, 48000U);
    INFO("rate " << rate << ", seed " << kSeed);
    const auto speech = scene(rate, 9.0, kSeed + 3, [&](double t, double g, double h) {
        const double envelope = syllables(t, 2.0, 4.5) + syllables(t, 6.0, 7.5);
        return 0.01 * g + 0.1 * envelope * h;
    });
    const auto heard = level(rate, speech);
    REQUIRE(heard.size() == 2);
    const double r = rate;
    CHECK(heard[0].start_sample <= static_cast<std::uint64_t>(2.0 * r));
    CHECK(heard[0].start_sample >= static_cast<std::uint64_t>(1.75 * r));
    CHECK(std::abs(static_cast<double>(heard[0].end_sample) - 4.5 * r) < 0.1 * r);
    CHECK(heard[1].start_sample <= static_cast<std::uint64_t>(6.0 * r));
    CHECK(std::abs(static_cast<double>(heard[1].end_sample) - 7.5 * r) < 0.1 * r);
    for (const Utterance& u : heard) {
        CHECK(u.cut_by == SegmentBy::Level);
        CHECK(u.sample_rate == rate);
        const std::uint64_t frames = u.end_sample - u.start_sample;
        CHECK(u.pcm.size() == (frames * kRecogniserRateHz + rate - 1) / rate);
    }

    const auto tone = scene(rate, 5.0, kSeed + 4, [&](double t, double g, double) {
        const bool on = t >= 2.0 && t < 3.5;
        return 0.01 * g + (on ? 0.1 * std::sin(2.0 * std::numbers::pi * 1000.0 * t) : 0.0);
    });
    const auto beeped = level(rate, tone);
    REQUIRE(beeped.size() == 1);
    CHECK(beeped[0].start_sample < static_cast<std::uint64_t>(2.0 * r));
    CHECK(std::abs(static_cast<double>(beeped[0].end_sample) - 3.5 * r) < 0.05 * r);
}
