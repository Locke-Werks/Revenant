// LevelAgc, Deemphasis and SoftLimiter, each on its own.
//
// Each case names the wrong implementation it rejects.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "audio/mix_stages.h"

using revenant::ui::Deemphasis;
using revenant::ui::LevelAgc;
using revenant::ui::SoftLimiter;

namespace {

constexpr std::uint32_t kRate = 48'000;

[[nodiscard]] std::vector<float> tone(double hz, std::size_t frames, double amplitude)
{
    std::vector<float> out(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        out[n] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz *
                                                         static_cast<double>(n) / kRate));
    }
    return out;
}

[[nodiscard]] double peak_of(const std::vector<float>& samples, std::size_t from)
{
    double peak = 0.0;
    for (std::size_t n = from; n < samples.size(); ++n) {
        peak = std::max(peak, std::abs(static_cast<double>(samples[n])));
    }
    return peak;
}

}  // namespace

TEST_CASE("the AGC brings a -60 dBFS tone and a full-scale one to the same level",
          "[audio][mix][agc]")
{
    // REJECTS: no AGC at all, which is what the mix had. An AM or SSB
    // receiver's audio comes out at the level the signal came in at, and a
    // -60 dBFS signal played at -60 dBFS is silence to a listener.
    for (const double amplitude : {1e-3, 1.0}) {
        LevelAgc agc;
        agc.configure(kRate);
        auto samples = tone(1'000.0, kRate * 2, amplitude);
        agc.process(samples.data(), samples.size(), 1);
        const double settled = peak_of(samples, kRate);
        INFO("input peak " << amplitude << ", output peak over the second second " << settled);
        CHECK(settled > 0.8 * LevelAgc::kTarget);
        CHECK(settled < 1.2 * LevelAgc::kTarget);
    }
}

TEST_CASE("the AGC never applies more than its ceiling", "[audio][mix][agc]")
{
    // REJECTS: a gain of target over envelope with nothing under the
    // envelope, which divides by zero on silence and hands the card infinity.
    LevelAgc agc;
    agc.configure(kRate);
    std::vector<float> silence(kRate, 0.0F);
    agc.process(silence.data(), silence.size(), 1);
    CHECK(std::ranges::all_of(silence, [](float v) { return v == 0.0F; }));
    CHECK(agc.gain() <= std::pow(10.0, LevelAgc::kMaxGainDb / 20.0) * (1.0 + 1e-9));

    // A -100 dBFS tone comes up by the ceiling and no further.
    auto faint = tone(1'000.0, kRate, 1e-5);
    agc.process(faint.data(), faint.size(), 1);
    const double lifted = peak_of(faint, kRate / 2);
    INFO("-100 dBFS came out at peak " << lifted);
    CHECK(lifted < 1e-5 * std::pow(10.0, LevelAgc::kMaxGainDb / 20.0) * 1.01);
}

TEST_CASE("the AGC takes one gain for both channels of a frame", "[audio][mix][agc]")
{
    // REJECTS: a gain per channel, which pulls a stereo image to the centre
    // by levelling the quiet side up to the loud one.
    LevelAgc agc;
    agc.configure(kRate);
    std::vector<float> stereo(2 * kRate);
    const auto loud = tone(1'000.0, kRate, 0.01);
    for (std::size_t n = 0; n < loud.size(); ++n) {
        stereo[2 * n] = loud[n];
        stereo[2 * n + 1] = 0.5F * loud[n];
    }
    agc.process(stereo.data(), kRate, 2);
    for (std::size_t n = kRate / 2; n < kRate; ++n) {
        CHECK(std::abs(stereo[2 * n + 1] - 0.5F * stereo[2 * n]) <= 1e-6F);
    }
}

TEST_CASE("de-emphasis passes DC and is 75 us down at 10 kHz", "[audio][mix][deemphasis]")
{
    // REJECTS: playing a multiplex as it comes, pre-emphasis and all. A 75 us
    // curve is -13.7 dB at 10 kHz in the analogue prototype; the one-pole
    // here is within a decibel and a half of that at 48000 S/s.
    Deemphasis dc;
    dc.configure(kRate);
    std::vector<float> ones(kRate, 1.0F);
    dc.process(ones.data(), ones.size(), 1);
    CHECK(std::abs(ones.back() - 1.0F) < 1e-6F);

    Deemphasis curve;
    curve.configure(kRate);
    auto high = tone(10'000.0, kRate, 1.0);
    curve.process(high.data(), high.size(), 1);
    const double db = 20.0 * std::log10(peak_of(high, kRate / 2));
    INFO("10 kHz through the curve: " << db << " dB");
    CHECK(db < -12.2);
    CHECK(db > -15.2);
}

TEST_CASE("the limiter holds the sum under its threshold and releases after",
          "[audio][mix][limiter]")
{
    // REJECTS: a plain sum, which is what the mix had, and hard clipping,
    // which flattens every peak of an overload into the same square edge.
    SoftLimiter limiter;
    limiter.configure(kRate);

    auto burst = tone(1'000.0, kRate / 2, 2.4);
    CHECK(limiter.process(burst.data(), burst.size(), 1));
    CHECK(peak_of(burst, 0) <= SoftLimiter::kThreshold + 1e-6);

    // A second of quiet after it and the gain is back to one, so the next
    // ordinary frame reaches the card as it came. Half a second was measured
    // at 0.9958, five time constants from the 0.371 this burst asked for.
    std::vector<float> quiet(kRate, 0.0F);
    limiter.process(quiet.data(), quiet.size(), 1);
    CHECK(limiter.gain() == 1.0);

    auto ordinary = tone(1'000.0, 480, 0.5);
    const auto before = ordinary;
    CHECK_FALSE(limiter.process(ordinary.data(), ordinary.size(), 1));
    CHECK(ordinary == before);
}

TEST_CASE("the limiter's gain is one for every channel of a frame", "[audio][mix][limiter]")
{
    // REJECTS: limiting each channel on its own peak, which moves a loud
    // stereo source towards the quieter side.
    SoftLimiter limiter;
    limiter.configure(kRate);
    std::vector<float> stereo = {2.0F, 0.5F, 1.8F, 0.45F};
    limiter.process(stereo.data(), 2, 2);
    CHECK(std::abs(stereo[1] / stereo[0] - 0.25F) < 1e-6F);
    CHECK(std::abs(stereo[3] / stereo[2] - 0.25F) < 1e-6F);
}
