// AudioMix: the rack's mix at the sound card's rate, against real AudioRings
// and no sound card.
//
// Each case names the wrong implementation it rejects, and three of them are
// the mix this replaced, which opened the sink at the focused receiver's
// format, left out any ring at another rate or channel count, played every
// ring from its own head and summed with no limit.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "audio/audio_mix.h"
#include "audio/audio_ring.h"

using revenant::rpc::AudioChunk;
using revenant::ui::AudioMix;
using revenant::ui::AudioRing;
using revenant::ui::FrameSource;
using revenant::ui::MixControl;
using revenant::ui::MixPull;
using revenant::ui::MixSlot;
using revenant::ui::SoftLimiter;

namespace {

constexpr std::uint32_t kDevice = 48'000;

// Deep enough that nothing a case writes is evicted.
constexpr std::uint32_t kDepthMs = 5'000;

[[nodiscard]] AudioChunk chunk_of(std::uint64_t index, std::vector<float> samples,
                                  std::uint32_t rate, std::uint16_t channels = 1)
{
    AudioChunk chunk;
    chunk.sample_rate = rate;
    chunk.channel_count = channels;
    chunk.sample_index = index;
    chunk.squelch_open = true;
    chunk.samples = std::move(samples);
    return chunk;
}

// `frames` of a function of the stream index, at `rate`.
template <typename F>
[[nodiscard]] std::vector<float> signal(std::uint64_t first, std::size_t frames, F&& f)
{
    std::vector<float> out(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        out[n] = static_cast<float>(f(first + n));
    }
    return out;
}

[[nodiscard]] double sine_at(double hz, std::uint32_t rate, std::uint64_t index,
                             double amplitude)
{
    return amplitude *
           std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(index) /
                    static_cast<double>(rate));
}

// The amplitude of `hz` in `samples` at `rate`, by correlation over a whole
// number of its cycles.
[[nodiscard]] double amplitude_of(const std::vector<float>& samples, std::size_t stride,
                                  std::size_t channel, double hz, std::uint32_t rate)
{
    double re = 0.0;
    double im = 0.0;
    const std::size_t frames = samples.size() / stride;
    for (std::size_t n = 0; n < frames; ++n) {
        const double angle = 2.0 * std::numbers::pi * hz * static_cast<double>(n) /
                             static_cast<double>(rate);
        re += static_cast<double>(samples[n * stride + channel]) * std::cos(angle);
        im += static_cast<double>(samples[n * stride + channel]) * std::sin(angle);
    }
    return 2.0 * std::hypot(re, im) / static_cast<double>(frames);
}

struct Rack {
    std::array<AudioRing, 4> rings;
    std::array<MixSlot, 4> slots{};
    std::array<AudioRing*, 4> pointers{};

    Rack()
    {
        for (std::size_t i = 0; i < rings.size(); ++i) {
            rings[i].set_depth_millis(kDepthMs);
            pointers[i] = &rings[i];
        }
    }

    MixPull pull(AudioMix& mix, int lead, std::vector<float>& out, std::size_t frames)
    {
        out.assign(frames * static_cast<std::size_t>(mix.out_channels()), -99.0F);
        const MixControl control{lead, slots};
        return mix.pull(pointers, control, out.data(), frames);
    }
};

}  // namespace

TEST_CASE("a composite-rate receiver plays at the device's rate", "[audio][mix]")
{
    // REJECTS: opening the sink at the stream's own rate, which is what the
    // player did. RDS raises a wfm receiver to 171000 S/s, the device the
    // owner listens on refused 32-bit float at that rate, and the audio died
    // with the sentence saying so. The mix is made at the device's rate and
    // the receiver's rate plays no part in that.
    Rack rack;
    rack.slots[0].heard = true;
    rack.rings[0].write(chunk_of(
        0, signal(0, 171'000, [](std::uint64_t n) { return sine_at(1'000.0, 171'000, n, 0.5); }),
        171'000));

    AudioMix mix(kDevice, 2);
    std::vector<float> out;
    const MixPull first = rack.pull(mix, 0, out, 4'800);
    CHECK(first.lead == 0);
    CHECK(first.frames_played == 4'800);

    // The second tenth of a second, clear of the kernel's start.
    const MixPull second = rack.pull(mix, 0, out, 4'800);
    CHECK(second.frames_played == 4'800);
    const double left = amplitude_of(out, 2, 0, 1'000.0, kDevice);
    const double right = amplitude_of(out, 2, 1, 1'000.0, kDevice);
    INFO("1 kHz at " << left << " and " << right << " against 0.5 in");
    CHECK(std::abs(left - 0.5) < 1e-3);
    CHECK(left == right);
}

TEST_CASE("a wfm multiplex is played as programme audio", "[audio][mix]")
{
    // REJECTS: resampling the multiplex as it comes, which plays the
    // pre-emphasised programme bright and hissy with the 19 kHz pilot under
    // it. A wfm receiver above 114000 S/s is filtered to 15 kHz and
    // de-emphasised at 75 us on the way into the mix.
    Rack rack;
    rack.slots[0] = MixSlot{.heard = true, .wfm = true};
    rack.rings[0].write(chunk_of(0, signal(0, 171'000, [](std::uint64_t n) {
        return sine_at(1'000.0, 171'000, n, 0.5) + sine_at(19'000.0, 171'000, n, 0.1) +
               sine_at(57'000.0, 171'000, n, 0.05);
    }), 171'000));

    AudioMix mix(kDevice, 1);
    std::vector<float> out;
    static_cast<void>(rack.pull(mix, 0, out, 4'800));
    static_cast<void>(rack.pull(mix, 0, out, 4'800));

    const double programme = amplitude_of(out, 1, 0, 1'000.0, kDevice);
    const double pilot = amplitude_of(out, 1, 0, 19'000.0, kDevice);
    INFO("1 kHz at " << programme << ", 19 kHz at " << pilot);
    // 0.5 through a 75 us one-pole at 1 kHz, which is 0.9 of it.
    CHECK(programme > 0.43);
    CHECK(programme < 0.47);
    CHECK(pilot < 1e-4);
}

TEST_CASE("a mono receiver is heard under a stereo lead", "[audio][mix]")
{
    // REJECTS: leaving out a ring whose channel count is not the lead's,
    // which is what the mix did. With a stereo wfm receiver focused, every
    // am, ssb, cw and nfm receiver in the rack was silent.
    Rack rack;
    rack.slots[0].heard = true;
    rack.slots[1].heard = true;
    std::vector<float> stereo(2 * 4'800);
    for (std::size_t n = 0; n < 4'800; ++n) {
        stereo[2 * n] = 0.1F;
        stereo[2 * n + 1] = 0.05F;
    }
    rack.rings[0].write(chunk_of(0, stereo, kDevice, 2));
    rack.rings[1].write(chunk_of(0, std::vector<float>(4'800, 0.2F), kDevice));

    AudioMix mix(kDevice, 2);
    std::vector<float> out;
    const MixPull pulled = rack.pull(mix, 0, out, 4'800);
    CHECK(pulled.frames_played == 4'800);
    for (std::size_t n = 0; n < 4'800; ++n) {
        CHECK(out[2 * n] == 0.1F + 0.2F);
        CHECK(out[2 * n + 1] == 0.05F + 0.2F);
    }
}

TEST_CASE("receivers at different rates are summed at the device's", "[audio][mix]")
{
    // REJECTS: leaving out a ring at another rate than the lead's, which is
    // what the mix did, and which a P25 receiver's 8000 S/s voice beside a
    // 48000 S/s one would always have been.
    Rack rack;
    rack.slots[0].heard = true;
    rack.slots[1].heard = true;
    rack.rings[0].write(chunk_of(0, std::vector<float>(9'600, 0.1F), kDevice));
    rack.rings[1].write(chunk_of(0, std::vector<float>(1'600, 0.2F), 8'000));

    AudioMix mix(kDevice, 1);
    std::vector<float> out;
    static_cast<void>(rack.pull(mix, 0, out, 4'800));
    static_cast<void>(rack.pull(mix, 0, out, 2'400));
    for (std::size_t n = 0; n < 2'400; ++n) {
        CHECK(std::abs(out[n] - 0.3F) < 1e-4F);
    }
}

TEST_CASE("two receivers on one instant play together whatever their indices",
          "[audio][mix][align]")
{
    // REJECTS: playing each ring from its own head, which is what the mix
    // did. Receiver B below subscribed four chunks before A, so its ring
    // holds 40 ms A's does not, and from their heads B's click lands 1920
    // frames after A's. Their indices share no origin either, 1000 against
    // 50000, which is ordinary: each receiver counts from its own start.
    //
    // What ties them is arrival: chunk j of each covers the same 10 ms of air
    // and reaches the client at the same moment.
    Rack rack;
    rack.slots[0].heard = true;
    rack.slots[1].heard = true;

    constexpr std::size_t kChunk = 480;
    constexpr std::int64_t kT0 = 5'000'000'000;
    const auto arrival = [](int j) { return kT0 + static_cast<std::int64_t>(j + 1) * 10'000'000; };
    const std::uint64_t click = 2 * kChunk + 100;

    for (int j = -4; j < 6; ++j) {
        const auto offset = static_cast<std::uint64_t>(j + 4) * kChunk;
        const std::uint64_t b_index = 50'000 - 4 * kChunk + offset;
        rack.rings[1].write(chunk_of(b_index, signal(b_index, kChunk, [&](std::uint64_t n) {
                                return n == 50'000 + click ? 0.2 : 0.0;
                            }),
                                     kDevice),
                            arrival(j));
        if (j >= 0) {
            const std::uint64_t a_index = 1'000 + static_cast<std::uint64_t>(j) * kChunk;
            rack.rings[0].write(chunk_of(a_index, signal(a_index, kChunk, [&](std::uint64_t n) {
                                    return n == 1'000 + click ? 0.3 : 0.0;
                                }),
                                         kDevice),
                                arrival(j));
        }
    }

    AudioMix mix(kDevice, 1);
    std::vector<float> out;
    const MixPull pulled = rack.pull(mix, 0, out, 5 * kChunk);
    CHECK(pulled.frames_played == 5 * kChunk);

    std::vector<std::size_t> heard;
    for (std::size_t n = 0; n < out.size(); ++n) {
        if (out[n] != 0.0F) {
            heard.push_back(n);
        }
    }
    REQUIRE(heard.size() == 1);
    CHECK(heard.front() == click);
    CHECK(out[click] == 0.3F + 0.2F);

    // B's forty milliseconds nobody else was playing were passed over.
    CHECK(rack.rings[1].counts().frames_skipped == 4 * kChunk);
}

TEST_CASE("a receiver at another rate lines up with the lead to a frame",
          "[audio][mix][align]")
{
    // REJECTS: aligning on the index alone, which treats an 8000 S/s index
    // as a 48000 S/s one and puts the second receiver's click six times too
    // late. The instant is index over rate, per stream.
    Rack rack;
    rack.slots[0].heard = true;
    rack.slots[1].heard = true;

    constexpr std::int64_t kT0 = 9'000'000'000;
    for (int j = 0; j < 8; ++j) {
        const std::int64_t at = kT0 + static_cast<std::int64_t>(j + 1) * 10'000'000;
        const auto a_index = static_cast<std::uint64_t>(j) * 480;
        rack.rings[0].write(chunk_of(a_index, signal(a_index, 480, [](std::uint64_t n) {
                                return n == 1'500 ? 0.3 : 0.0;
                            }),
                                     kDevice),
                            at);
        // Voice-rate indices from 7000, the click at the same instant as
        // A's: 1500 / 48000 = 250 / 8000 seconds after each stream's start.
        const std::uint64_t b_index = 7'000 + static_cast<std::uint64_t>(j) * 80;
        rack.rings[1].write(chunk_of(b_index, signal(b_index, 80, [](std::uint64_t n) {
                                return n == 7'250 ? 0.2 : 0.0;
                            }),
                                     8'000),
                            at);
    }

    AudioMix mix(kDevice, 1);
    std::vector<float> out;
    static_cast<void>(rack.pull(mix, 0, out, 3'000));

    // B's click, band-limited by the resampler, peaks where A's does.
    std::size_t b_peak = 0;
    double best = 0.0;
    for (std::size_t n = 0; n < out.size(); ++n) {
        const double value = n == 1'500 ? static_cast<double>(out[n]) - 0.3 : out[n];
        if (std::abs(value) > best) {
            best = std::abs(value);
            b_peak = n;
        }
    }
    INFO("B's click peaks at output frame " << b_peak << " with " << best);
    CHECK(b_peak == 1'500);
}

TEST_CASE("the sum of loud receivers is limited", "[audio][mix]")
{
    // REJECTS: a plain sum, which is what the mix did, handing 1.4 to a float
    // sink that clipped it in the device.
    Rack rack;
    rack.slots[0].heard = true;
    rack.slots[1].heard = true;
    rack.rings[0].write(chunk_of(0, std::vector<float>(4'800, 0.7F), kDevice));
    rack.rings[1].write(chunk_of(0, std::vector<float>(4'800, 0.7F), kDevice));

    AudioMix mix(kDevice, 2);
    std::vector<float> out;
    const MixPull pulled = rack.pull(mix, 0, out, 4'800);
    CHECK(pulled.limited);
    CHECK(std::ranges::all_of(out, [](float v) {
        return std::abs(static_cast<double>(v)) <= SoftLimiter::kThreshold + 1e-6;
    }));
}

TEST_CASE("a receiver is mixed at the level the engine sent it", "[audio][mix]")
{
    // REJECTS: a second AGC here. The engine levels what a subscription
    // carries, core/engine/listener_level.h, and a switch in the receiver
    // panel turns that off to hold the gain; a mix that levelled again would
    // undo the hold and pump what the engine had already set.
    //
    // WHAT THIS CASE USED TO ASSERT: that an am, ssb or cw receiver's 1e-3
    // tone came out within a fifth of LevelAgc::kTarget, the mix's own AGC,
    // which existed because the engine applied none. It is the level it came
    // in at now, whatever the mode.
    Rack rack;
    rack.slots[0] = MixSlot{.heard = true};
    rack.rings[0].write(chunk_of(
        0, signal(0, 2 * kDevice, [](std::uint64_t n) { return sine_at(700.0, kDevice, n, 1e-3); }),
        kDevice));

    AudioMix mix(kDevice, 1);
    std::vector<float> out;
    static_cast<void>(rack.pull(mix, 0, out, kDevice));
    static_cast<void>(rack.pull(mix, 0, out, kDevice));
    const double level = amplitude_of(out, 1, 0, 700.0, kDevice);
    INFO("700 Hz at " << level << " from 1e-3 in");
    CHECK(std::abs(level - 1e-3) < 1e-5);

    // And a constant is left exactly as it came.
    Rack plain;
    plain.slots[0].heard = true;
    plain.rings[0].write(chunk_of(0, std::vector<float>(480, 0.01F), kDevice));
    AudioMix untouched(kDevice, 1);
    static_cast<void>(plain.pull(untouched, 0, out, 480));
    CHECK(std::ranges::all_of(out, [](float v) { return v == 0.01F; }));
}

TEST_CASE("the lead holds when its audio runs out, and the rest hold with it",
          "[audio][mix]")
{
    // REJECTS: reading past the lead's newest frame and skipping what
    // arrives late, which turns every network hiccup into lost audio rather
    // than a pause. The lead is read as the single ring always was.
    Rack rack;
    rack.slots[0].heard = true;
    rack.rings[0].write(chunk_of(0, std::vector<float>(1'000, 0.25F), kDevice));

    AudioMix mix(kDevice, 1);
    std::vector<float> out;
    const MixPull short_pull = rack.pull(mix, 0, out, 1'500);
    CHECK(short_pull.frames_played == 1'000);
    CHECK(short_pull.lead_source == FrameSource::starved);
    CHECK(out[999] == 0.25F);
    CHECK(out[1'000] == 0.0F);
    CHECK(rack.rings[0].counts().frames_starved == 500);

    // The rest arrives, and plays from where the mix stopped rather than
    // from where the card had got to.
    rack.rings[0].write(chunk_of(1'000, std::vector<float>(500, 0.5F), kDevice));
    const MixPull resumed = rack.pull(mix, 0, out, 500);
    CHECK(resumed.frames_played == 500);
    CHECK(out.front() == 0.5F);
    CHECK(rack.rings[0].counts().frames_skipped == 0);
}

TEST_CASE("nothing heard is silence and names no lead", "[audio][mix]")
{
    Rack rack;
    AudioMix mix(kDevice, 2);
    std::vector<float> out;
    const MixPull pulled = rack.pull(mix, 0, out, 64);
    CHECK(pulled.lead == -1);
    CHECK(std::ranges::all_of(out, [](float v) { return v == 0.0F; }));
}
