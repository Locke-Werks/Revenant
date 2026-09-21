// The per-receiver signal meter's divisor, which was wrong for stereo.
//
// No GPU. core/engine/signal_meter.h exists so that this arithmetic can be
// asserted at all: it used to sit inside Graph::complete, behind a Vulkan
// readback, where the only way to check it was to run a receiver on a device
// and believe the number.
//
// THE PROPERTY THESE CASES REST ON. A WFM station with no 19 kHz pilot
// cannot be decoded in stereo by anything, so core/shaders/vrx_demod.comp
// gates the difference channel by multiplying it by zero and hands the sum
// channel out in both. L and R are then bit-identical sample for sample,
// which means the meter has a correct answer that is not a matter of taste:
// it has to read exactly what the same programme reads as mono. Anything
// else moves the number an operator sets a squelch against when a station
// stops transmitting a pilot.

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/engine/signal_meter.h"

using revenant::engine::kSilenceFloorDbfs;
using revenant::engine::meter_dbfs;

namespace {

// One cycle of a sine at an amplitude, as mono audio.
[[nodiscard]] std::vector<float> tone(std::size_t frames, double amplitude) {
    std::vector<float> out(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const double phase = 2.0 * 3.14159265358979323846 *
                             static_cast<double>(i) / static_cast<double>(frames);
        out[i] = static_cast<float>(amplitude * std::sin(phase));
    }
    return out;
}

// The same programme in both channels, which is what the kernel produces on
// a station with no pilot.
[[nodiscard]] std::vector<float> duplicated(std::span<const float> mono) {
    std::vector<float> out;
    out.reserve(mono.size() * 2);
    for (const float value : mono) {
        out.push_back(value);
        out.push_back(value);
    }
    return out;
}

}  // namespace

TEST_CASE("a stereo pair meters as the same programme in mono", "[engine][meter]") {
    const std::vector<float> mono = tone(512, 0.5);
    const std::vector<float> stereo = duplicated(mono);

    const double mono_level = meter_dbfs(mono, 512, false);
    const double stereo_level = meter_dbfs(stereo, 512, false);

    // The defect this case was written for: reading the pair as complex
    // divides the sum of squares by frames rather than by samples, which is
    // 10*log10(2) high. The tolerance is far below that, so the two cannot
    // both pass.
    CHECK(stereo_level == Catch::Approx(mono_level).margin(1e-9));

    const double as_complex = meter_dbfs(stereo, 512, true);
    CHECK(as_complex == Catch::Approx(mono_level + 20.0 * std::log10(std::sqrt(2.0)))
                            .margin(1e-9));
    CHECK(as_complex - stereo_level == Catch::Approx(3.0103).margin(1e-3));
}

TEST_CASE("a complex tap meters on magnitude", "[engine][meter]") {
    // A constant-envelope complex sample at magnitude 1: every frame is
    // (cos, sin) of something, so |z| is 1 and the meter reads 0 dBFS. Read
    // as two real channels it would read the RMS of the components, 1/sqrt2,
    // which is 3.01 dB low and is the mirror image of the stereo defect.
    std::vector<float> iq;
    iq.reserve(256 * 2);
    for (std::size_t i = 0; i < 256; ++i) {
        const double phase = 2.0 * 3.14159265358979323846 *
                             static_cast<double>(i) / 256.0;
        iq.push_back(static_cast<float>(std::cos(phase)));
        iq.push_back(static_cast<float>(std::sin(phase)));
    }

    CHECK(meter_dbfs(iq, 256, true) == Catch::Approx(0.0).margin(1e-5));
    CHECK(meter_dbfs(iq, 256, false) == Catch::Approx(-3.0103).margin(1e-3));
}

TEST_CASE("a full-scale mono tone reads -3.01 dBFS", "[engine][meter]") {
    // The convention check, so the two cases above are anchored to something
    // rather than only to each other. A sine at full scale has an RMS of
    // 1/sqrt2 whatever the channel count.
    const std::vector<float> mono = tone(1024, 1.0);
    CHECK(meter_dbfs(mono, 1024, false) == Catch::Approx(-3.0103).margin(1e-3));
}

TEST_CASE("silence and an empty buffer both floor", "[engine][meter]") {
    const std::vector<float> quiet(64, 0.0F);
    CHECK(meter_dbfs(quiet, 64, false) == kSilenceFloorDbfs);
    CHECK(meter_dbfs(quiet, 32, true) == kSilenceFloorDbfs);

    // A dispatch that produced nothing must not divide by zero. Graph
    // skips it before reaching here and this is the belt.
    CHECK(meter_dbfs(std::span<const float>{}, 0, false) == kSilenceFloorDbfs);
    CHECK(meter_dbfs(quiet, 0, true) == kSilenceFloorDbfs);
}
