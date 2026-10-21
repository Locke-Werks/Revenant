// The audio spectrum strip's transform, core/engine/audio_spectrum.h. No
// device: tones built here, and the notch run through its own design so the
// strip is checked against the section that actually runs.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

#include "core/dsp/noise_reference.h"
#include "core/engine/audio_spectrum.h"

using namespace revenant;
using Catch::Approx;

namespace {

constexpr dsp::SampleRate kRate = 48'000;

[[nodiscard]] std::vector<float> tone(double hz, double amplitude, std::size_t frames) {
    std::vector<float> out(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        out[i] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) /
                                 static_cast<double>(kRate)));
    }
    return out;
}

[[nodiscard]] std::size_t peak_bin(std::span<const float> power) {
    return static_cast<std::size_t>(std::ranges::max_element(power) - power.begin());
}

// Direct form I in double, the section's design as the kernel would run it.
void run_notch(const dsp::NotchDesign& d, std::span<float> audio) {
    double x1 = 0.0;
    double x2 = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;
    for (float& sample : audio) {
        const double x = sample;
        const double y = d.b0 * x + d.b1 * x1 + d.b2 * x2 - d.a1 * y1 - d.a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        sample = static_cast<float>(y);
    }
}

}  // namespace

TEST_CASE("the audio strip waits for a whole window", "[engine][audio_spectrum]") {
    engine::AudioSpectrum strip;
    const auto audio = tone(1'000.0, 0.5, engine::kAudioSpectrumPoints - 1);
    strip.push(audio, 1, kRate);
    CHECK_FALSE(strip.compute());
    const std::array<float, 1> one{0.0F};
    strip.push(one, 1, kRate);
    CHECK(strip.compute());
    CHECK(strip.power_db().size() == engine::kAudioSpectrumBins);
    CHECK(strip.bin_hz() == Approx(48'000.0 / 1'024.0));
}

TEST_CASE("a tone peaks at its frequency at its level", "[engine][audio_spectrum]") {
    engine::AudioSpectrum strip;
    // On a bin centre, so the level is the window's coherent gain exactly.
    const double bin_hz = static_cast<double>(kRate) / engine::kAudioSpectrumPoints;
    const double hz = 32.0 * bin_hz;
    strip.push(tone(hz, 0.5, 4'096), 1, kRate);
    REQUIRE(strip.compute());
    const auto power = strip.power_db();
    CHECK(peak_bin(power) == 32);
    CHECK(power[32] == Approx(20.0 * std::log10(0.5)).margin(0.05));
    // Hann sidelobes are down more than 31 dB; ten bins out is far below.
    CHECK(power[42] < power[32] - 60.0F);
}

TEST_CASE("stereo is downmixed and a rate change restarts", "[engine][audio_spectrum]") {
    engine::AudioSpectrum strip;
    const auto mono = tone(3'000.0, 0.25, 2'048);
    std::vector<float> stereo(mono.size() * 2);
    for (std::size_t i = 0; i < mono.size(); ++i) {
        stereo[2 * i] = mono[i];
        stereo[2 * i + 1] = mono[i];
    }
    strip.push(stereo, 2, kRate);
    REQUIRE(strip.compute());
    CHECK(static_cast<double>(peak_bin(strip.power_db())) * strip.bin_hz() ==
          Approx(3'000.0).margin(strip.bin_hz()));

    strip.push(std::span(mono).first(100), 1, 8'000);
    CHECK_FALSE(strip.compute());
    CHECK(strip.rate() == 8'000);
}

TEST_CASE("a notched tone reads about the notch's depth lower",
          "[engine][audio_spectrum][notch]") {
    const double bin_hz = static_cast<double>(kRate) / engine::kAudioSpectrumPoints;
    const double hz = 40.0 * bin_hz;
    const double depth = 30.0;
    const auto design = dsp::design_notch(hz, depth, 100.0, kRate);
    REQUIRE(design.has_value());

    // Long enough that the section has settled before the last window.
    auto clean = tone(hz, 0.5, 16'384);
    auto notched = clean;
    run_notch(*design, notched);

    engine::AudioSpectrum before;
    engine::AudioSpectrum after;
    before.push(clean, 1, kRate);
    after.push(notched, 1, kRate);
    REQUIRE(before.compute());
    REQUIRE(after.compute());

    const float drop = before.power_db()[40] - after.power_db()[40];
    CHECK(drop == Approx(depth).margin(2.0));
    CHECK(peak_bin(before.power_db()) == 40);
}

TEST_CASE("the strip averages successive windows", "[engine][audio_spectrum]") {
    engine::AudioSpectrum strip(0.5F);
    const double bin_hz = static_cast<double>(kRate) / engine::kAudioSpectrumPoints;
    strip.push(tone(16.0 * bin_hz, 1.0, 1'024), 1, kRate);
    REQUIRE(strip.compute());
    const float loud = strip.power_db()[16];
    strip.push(std::vector<float>(1'024, 0.0F), 1, kRate);
    REQUIRE(strip.compute());
    // Half the power gone in one step: 3 dB down, not to the floor.
    CHECK(strip.power_db()[16] == Approx(loud - 3.01F).margin(0.05));
    strip.reset();
    CHECK_FALSE(strip.compute());
}

TEST_CASE("notch ticks land where each mode runs the notch", "[engine][audio_spectrum][notch]") {
    engine::PassbandNotchRequest request;
    request.audio_rate = 8'000;
    request.cw_pitch = 700;
    request.notches[0] = dsp::NotchSpec{true, 1'000, 40.0, 100};
    request.notches[1] = dsp::NotchSpec{false, 2'000, 40.0, 100};
    request.notches[3] = dsp::NotchSpec{true, -600, 20.0, 50};
    std::array<double, 4> ticks{};

    request.demod = engine::Demod::Nfm;
    REQUIRE(engine::audio_notch_frequencies(request, ticks) == 2);
    CHECK(ticks[0] == 1'000.0);
    CHECK(ticks[1] == 600.0);

    request.demod = engine::Demod::Lsb;
    REQUIRE(engine::audio_notch_frequencies(request, ticks) == 2);
    CHECK(ticks[0] == 1'000.0);
    CHECK(ticks[1] == 600.0);

    request.demod = engine::Demod::Cw;
    REQUIRE(engine::audio_notch_frequencies(request, ticks) == 2);
    CHECK(ticks[0] == 1'700.0);
    CHECK(ticks[1] == 100.0);

    std::array<double, 1> one{};
    CHECK(engine::audio_notch_frequencies(request, one) == 1);
}
