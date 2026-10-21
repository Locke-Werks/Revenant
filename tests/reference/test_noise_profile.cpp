// The adaptive noise profile, core/dsp/noise_profile.h, against what it
// promises: unit reconstruction at zero strength, hum cut by roughly the
// strength once learned, and a tone above the noise kept within 1 dB.
//
// Host only: the stage runs on the completion thread over audio already read
// back, so there is no kernel to hold it to.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <span>
#include <vector>

#include "core/dsp/noise_profile.h"

namespace {

using revenant::dsp::NoiseProfile;
using revenant::dsp::NoiseProfileSettings;

constexpr double kRate = 48'000.0;
constexpr double kHum1 = 120.0;
constexpr double kHum3 = 360.0;
constexpr double kTone = 1'500.0;

struct Source {
    std::mt19937 rng{20261020};
    std::normal_distribution<double> noise{0.0, 0.01};
    std::size_t n = 0;

    std::vector<float> next(std::size_t count, bool tone) {
        std::vector<float> out(count);
        for (auto& s : out) {
            const double t = static_cast<double>(n++) / kRate;
            double v = 0.05 * std::sin(2.0 * std::numbers::pi * kHum1 * t) +
                       0.03 * std::sin(2.0 * std::numbers::pi * kHum3 * t + 0.4) +
                       noise(rng);
            if (tone) {
                v += 0.1 * std::sin(2.0 * std::numbers::pi * kTone * t);
            }
            s = static_cast<float>(v);
        }
        return out;
    }
};

// Power of one frequency's component, by correlation over a whole number of
// its cycles' worth of samples (long enough that the leftover does not matter).
double tone_power(std::span<const float> x, double hz) {
    std::complex<double> acc{};
    for (std::size_t i = 0; i < x.size(); ++i) {
        acc += static_cast<double>(x[i]) *
               std::polar(1.0, -2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate);
    }
    const double a = 2.0 * std::abs(acc) / static_cast<double>(x.size());
    return a * a;
}

double db(double ratio) { return 10.0 * std::log10(ratio); }

// Feeds `seconds` in blocks of an awkward length and returns the input and
// output of the last `keep` seconds.
struct Run {
    std::vector<float> in;
    std::vector<float> out;
};

Run feed(NoiseProfile& profile, Source& src, double seconds, double keep, bool tone, bool quiet,
         const NoiseProfileSettings& settings) {
    Run run;
    const auto total = static_cast<std::size_t>(seconds * kRate);
    const auto from = total - static_cast<std::size_t>(keep * kRate);
    std::size_t done = 0;
    while (done < total) {
        const std::size_t block = std::min<std::size_t>(733, total - done);
        std::vector<float> chunk = src.next(block, tone);
        std::vector<float> original = chunk;
        profile.process(chunk, kRate, quiet, settings);
        for (std::size_t i = 0; i < block; ++i) {
            if (done + i >= from) {
                run.in.push_back(original[i]);
                run.out.push_back(chunk[i]);
            }
        }
        done += block;
    }
    return run;
}

}  // namespace

TEST_CASE("noise profile at zero strength reconstructs its input", "[noise_profile]") {
    NoiseProfile profile;
    Source src;
    NoiseProfileSettings settings;
    settings.enabled = true;
    settings.strength_db = 0.0;
    const Run run = feed(profile, src, 1.0, 1.0, true, false, settings);
    const std::size_t lag = profile.latency();
    double worst = 0.0;
    for (std::size_t i = 2 * lag; i < run.out.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(run.out[i]) - run.in[i - lag]));
    }
    CHECK(worst < 1e-6);
}

TEST_CASE("learn now cuts hum by the strength and keeps the tone", "[noise_profile]") {
    for (const double strength : {6.0, 12.0, 20.0}) {
        NoiseProfile profile;
        Source src;
        NoiseProfileSettings settings;
        settings.enabled = true;
        settings.strength_db = strength;
        settings.auto_learn = false;
        (void)feed(profile, src, 0.1, 0.1, false, false, settings);
        settings.learn_count = 1;
        (void)feed(profile, src, 1.5, 0.1, false, false, settings);
        REQUIRE(profile.learned());

        const Run run = feed(profile, src, 3.0, 2.0, true, false, settings);
        const double hum1 = db(tone_power(run.out, kHum1) / tone_power(run.in, kHum1));
        const double hum3 = db(tone_power(run.out, kHum3) / tone_power(run.in, kHum3));
        const double tone = db(tone_power(run.out, kTone) / tone_power(run.in, kTone));
        INFO("strength " << strength << " hum1 " << hum1 << " hum3 " << hum3 << " tone "
                         << tone);
        CHECK(hum1 < -(strength - 3.0));
        CHECK(hum1 > -(strength + 3.0));
        CHECK(hum3 < -(strength - 3.0));
        CHECK(hum3 > -(strength + 3.0));
        CHECK(std::abs(tone) < 1.0);
    }
}

TEST_CASE("auto learn from a shut squelch holds through the open passage",
          "[noise_profile]") {
    NoiseProfile profile;
    Source src;
    NoiseProfileSettings settings;
    settings.enabled = true;
    settings.strength_db = 12.0;
    (void)feed(profile, src, 1.5, 0.1, false, true, settings);
    const Run run = feed(profile, src, 4.0, 2.0, true, false, settings);
    const double hum1 = db(tone_power(run.out, kHum1) / tone_power(run.in, kHum1));
    const double tone = db(tone_power(run.out, kTone) / tone_power(run.in, kTone));
    INFO("hum1 " << hum1 << " tone " << tone);
    CHECK(hum1 < -9.0);
    CHECK(std::abs(tone) < 1.0);
}

TEST_CASE("minimum statistics finds hum with no squelch", "[noise_profile]") {
    NoiseProfile profile;
    Source src;
    NoiseProfileSettings settings;
    settings.enabled = true;
    settings.strength_db = 12.0;
    const Run run = feed(profile, src, 5.0, 2.0, false, false, settings);
    const double hum1 = db(tone_power(run.out, kHum1) / tone_power(run.in, kHum1));
    INFO("hum1 " << hum1);
    CHECK(hum1 < -9.0);
    CHECK(hum1 > -15.0);
}

TEST_CASE("reset drops a learned profile", "[noise_profile]") {
    NoiseProfile profile;
    Source src;
    NoiseProfileSettings settings;
    settings.enabled = true;
    settings.auto_learn = false;
    (void)feed(profile, src, 0.1, 0.1, false, false, settings);
    settings.learn_count = 1;
    (void)feed(profile, src, 1.5, 0.1, false, false, settings);
    REQUIRE(profile.learned());
    settings.reset_count = 1;
    (void)feed(profile, src, 0.1, 0.1, false, false, settings);
    CHECK_FALSE(profile.learned());
    CHECK(profile.profile().empty());
}
