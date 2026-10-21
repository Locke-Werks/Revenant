// CarrierTracker and find_nearest_peak on synthetic passband frames: a noise
// floor with deterministic ripple, and lines drawn as parabolas in dB, which
// is the shape a windowed transform gives a tone near its peak.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>
#include <vector>

#include "core/dsp/carrier_tracker.h"

using revenant::dsp::CarrierTracker;
using revenant::dsp::find_nearest_peak;

namespace {

constexpr std::size_t kBins = 512;
constexpr double kBinHz = 10.0;
constexpr double kBinZeroHz = -2560.0;
constexpr double kDt = 0.05;
constexpr float kFloorDb = -100.0f;

struct Scene {
    std::mt19937 rng{1234};
    std::vector<float> frame = std::vector<float>(kBins);

    void noise() {
        std::uniform_real_distribution<float> ripple(-1.5f, 1.5f);
        for (float& v : frame) {
            v = kFloorDb + ripple(rng);
        }
    }

    // A line whose -20 dB width is 2 * sqrt(20 / curvature) bins.
    void line(double centre_bin, float peak_db, float curvature = 12.0f) {
        for (std::size_t k = 0; k < kBins; ++k) {
            const auto d = static_cast<float>(static_cast<double>(k) - centre_bin);
            frame[k] = std::max(frame[k], peak_db - curvature * d * d);
        }
    }
};

}  // namespace

TEST_CASE("CarrierTracker reports a steady carrier after the persistence time",
          "[carrier_tracker]") {
    CarrierTracker tracker(kBins);
    Scene s;
    for (int f = 0; f < 100; ++f) {
        s.noise();
        s.line(200.3, -70.0f);
        tracker.update(s.frame, kBinHz, kBinZeroHz, kDt);
        if (f == 40) {
            CHECK(tracker.carriers().empty());
        }
    }
    const auto c = tracker.carriers();
    REQUIRE(c.size() == 1);
    CHECK(c[0].offset_hz == Catch::Approx(kBinZeroHz + 200.3 * kBinHz).margin(2.0));
    CHECK(c[0].width_hz == Catch::Approx(2.0 * std::sqrt(20.0 / 12.0) * kBinHz).margin(4.0));
    CHECK(c[0].level_db == Catch::Approx(30.0).margin(2.0));
    CHECK(c[0].age_seconds >= 3.0f);
}

TEST_CASE("CarrierTracker rejects a drifting carrier", "[carrier_tracker]") {
    CarrierTracker tracker(kBins);
    Scene s;
    for (int f = 0; f < 120; ++f) {
        s.noise();
        s.line(150.0 + 1.0 * f * kDt, -70.0f);  // one bin a second
        tracker.update(s.frame, kBinHz, kBinZeroHz, kDt);
        CHECK(tracker.carriers().empty());
    }
}

TEST_CASE("CarrierTracker rejects a short burst and a wide hump", "[carrier_tracker]") {
    CarrierTracker tracker(kBins);
    Scene s;
    for (int f = 0; f < 120; ++f) {
        s.noise();
        if (f < 30) {
            s.line(300.0, -65.0f);  // 1.5 s, then gone
        }
        s.line(100.0, -75.0f, 0.1f);  // speech-like width, steady
        tracker.update(s.frame, kBinHz, kBinZeroHz, kDt);
        CHECK(tracker.carriers().empty());
    }
}

TEST_CASE("CarrierTracker expires a carrier after the hold time", "[carrier_tracker]") {
    CarrierTracker tracker(kBins);
    Scene s;
    for (int f = 0; f < 100; ++f) {
        s.noise();
        s.line(250.0, -70.0f);
        tracker.update(s.frame, kBinHz, kBinZeroHz, kDt);
    }
    REQUIRE(tracker.carriers().size() == 1);
    for (int f = 0; f < 30; ++f) {  // 1.5 s, inside the 2 s hold
        s.noise();
        tracker.update(s.frame, kBinHz, kBinZeroHz, kDt);
    }
    CHECK(tracker.carriers().size() == 1);
    for (int f = 0; f < 20; ++f) {
        s.noise();
        tracker.update(s.frame, kBinHz, kBinZeroHz, kDt);
    }
    CHECK(tracker.carriers().empty());
}

TEST_CASE("CarrierTracker keeps at most eight, strongest first", "[carrier_tracker]") {
    CarrierTracker tracker(kBins);
    Scene s;
    for (int f = 0; f < 80; ++f) {
        s.noise();
        for (int i = 0; i < 10; ++i) {
            s.line(40.0 + 45.0 * i, -80.0f + 2.0f * static_cast<float>(i));
        }
        tracker.update(s.frame, kBinHz, kBinZeroHz, kDt);
    }
    const auto c = tracker.carriers();
    REQUIRE(c.size() == CarrierTracker::kMaxReports);
    for (std::size_t i = 1; i < c.size(); ++i) {
        CHECK(c[i - 1].level_db >= c[i].level_db);
    }
    CHECK(c[0].offset_hz == Catch::Approx(kBinZeroHz + (40.0 + 45.0 * 9) * kBinHz).margin(2.0));
}

TEST_CASE("find_nearest_peak fits the carrier beside a click", "[carrier_tracker]") {
    Scene s;
    s.noise();
    s.line(220.4, -60.0f, 5.0f);
    std::vector<float> scratch(64);

    const double carrier_hz = kBinZeroHz + 220.4 * kBinHz;
    const auto fit = find_nearest_peak(s.frame, kBinHz, kBinZeroHz, carrier_hz + 35.0, scratch);
    REQUIRE(fit.found);
    CHECK(fit.offset_hz == Catch::Approx(carrier_hz).margin(2.0));
    CHECK(fit.width_hz == Catch::Approx(2.0 * std::sqrt(20.0 / 5.0) * kBinHz).margin(4.0));
    CHECK(fit.level_db == Catch::Approx(40.0).margin(2.0));

    const auto miss = find_nearest_peak(s.frame, kBinHz, kBinZeroHz, kBinZeroHz + 50.0 * kBinHz,
                                        scratch);
    CHECK_FALSE(miss.found);
}
