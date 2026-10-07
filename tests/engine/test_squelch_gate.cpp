// The receiver squelch gate, core/engine/squelch_gate.h.
//
// No GPU. The gate is arithmetic over a level sequence and a span of audio,
// so these cases drive it with levels chosen to sit exactly where the
// behaviour changes: under the threshold, over it, inside the hysteresis
// band, and dithering across the threshold the way a weak station does.

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/engine/squelch_gate.h"

using revenant::engine::SquelchGate;
using revenant::engine::SquelchTiming;

namespace {

constexpr double kRate = 48'000.0;
// 10 ms dispatches, which is the order the graph delivers at.
constexpr std::size_t kFrames = 480;
constexpr double kThreshold = -60.0;

[[nodiscard]] double power_of(double dbfs) { return std::pow(10.0, dbfs / 10.0); }

struct Step {
    bool audible = false;
    bool open = false;
    std::vector<float> audio;
};

Step feed(SquelchGate& gate, double level_dbfs, double threshold = kThreshold,
          const SquelchTiming& timing = {}) {
    Step step;
    step.audio.assign(kFrames, 1.0F);
    step.audible = gate.process(power_of(level_dbfs), step.audio, 1, kRate, threshold, timing);
    step.open = gate.open();
    return step;
}

// Instant averaging so a case can place the level exactly.
[[nodiscard]] SquelchTiming crisp() {
    SquelchTiming t;
    t.average_s = 0.0;
    return t;
}

}  // namespace

TEST_CASE("the squelch stays shut on a level under the threshold", "[engine][squelch]") {
    SquelchGate gate;
    for (int i = 0; i < 100; ++i) {
        const auto step = feed(gate, -75.0, kThreshold, crisp());
        CHECK_FALSE(step.open);
        CHECK_FALSE(step.audible);
        for (const float s : step.audio) {
            REQUIRE(s == 0.0F);
        }
    }
}

TEST_CASE("the squelch opens on a level over the threshold and fades in", "[engine][squelch]") {
    SquelchGate gate;
    feed(gate, -75.0, kThreshold, crisp());
    const auto step = feed(gate, -50.0, kThreshold, crisp());
    REQUIRE(step.open);
    REQUIRE(step.audible);

    // The fade: no step from zero to one at the first sample, and fully up
    // by the end of the 5 ms ramp.
    CHECK(step.audio.front() < 0.1F);
    CHECK(step.audio[kFrames / 2] < 1.0F + 1e-6F);
    CHECK(step.audio.back() == 1.0F);
    for (std::size_t i = 1; i < step.audio.size(); ++i) {
        REQUIRE(step.audio[i] >= step.audio[i - 1]);
    }
}

TEST_CASE("the attack time ignores a one-dispatch spike", "[engine][squelch]") {
    SquelchGate gate;
    SquelchTiming timing = crisp();
    timing.attack_s = 0.03;
    feed(gate, -75.0, kThreshold, timing);
    CHECK_FALSE(feed(gate, -40.0, kThreshold, timing).open);
    CHECK_FALSE(feed(gate, -75.0, kThreshold, timing).open);
    CHECK_FALSE(feed(gate, -40.0, kThreshold, timing).open);
    CHECK_FALSE(feed(gate, -40.0, kThreshold, timing).open);
    CHECK(feed(gate, -40.0, kThreshold, timing).open);
}

TEST_CASE("hysteresis holds the gate open inside the band", "[engine][squelch]") {
    SquelchGate gate;
    const SquelchTiming timing = crisp();
    feed(gate, -50.0, kThreshold, timing);
    REQUIRE(gate.open());

    // Two dB under the threshold is inside the default 3 dB band, so it
    // never starts the tail, however long it lasts.
    for (int i = 0; i < 200; ++i) {
        REQUIRE(feed(gate, -62.0, kThreshold, timing).open);
    }
}

TEST_CASE("the tail keeps the gate open through a gap and then shuts it",
          "[engine][squelch]") {
    SquelchGate gate;
    // 245 ms rather than 250 so 25 dispatches of 10 ms cross it whatever the
    // rounding of the sum.
    SquelchTiming timing = crisp();
    timing.tail_s = 0.245;
    feed(gate, -50.0, kThreshold, timing);
    REQUIRE(gate.open());

    // 200 ms of gap between syllables: still open, audio untouched.
    for (int i = 0; i < 20; ++i) {
        const auto step = feed(gate, -80.0, kThreshold, timing);
        REQUIRE(step.open);
        REQUIRE(step.audio.back() == 1.0F);
    }
    // Speech again resets the tail.
    REQUIRE(feed(gate, -50.0, kThreshold, timing).open);
    for (int i = 0; i < 24; ++i) {
        REQUIRE(feed(gate, -80.0, kThreshold, timing).open);
    }
    // A tail after the last syllable it shuts, fading out rather than
    // stepping, and is silent after that.
    const auto closing = feed(gate, -80.0, kThreshold, timing);
    REQUIRE_FALSE(closing.open);
    CHECK(closing.audible);
    CHECK(closing.audio.front() > 0.9F);
    CHECK(closing.audio.back() == 0.0F);
    const auto after = feed(gate, -80.0, kThreshold, timing);
    CHECK_FALSE(after.audible);
    CHECK(after.audio.front() == 0.0F);
}

TEST_CASE("a signal dithering across the threshold does not chatter", "[engine][squelch]") {
    SquelchGate gate;
    // Default averaging and timing: a weak station whose level swings 2 dB
    // either side of the threshold every dispatch, which the old compare
    // turned into a 50 Hz click train.
    int transitions = 0;
    bool was_open = false;
    for (int i = 0; i < 1000; ++i) {
        const double level = kThreshold + ((i % 2 == 0) ? 2.0 : -2.0);
        const auto step = feed(gate, level);
        if (i > 0 && step.open != was_open) {
            ++transitions;
        }
        was_open = step.open;
    }
    CHECK(transitions <= 1);
    CHECK(was_open);
}

TEST_CASE("noise sitting just under the threshold never opens the gate", "[engine][squelch]") {
    SquelchGate gate;
    // Spiky noise: mean power 6 dB under the threshold, single dispatches
    // reaching 3 dB over it. Averaging and the attack time keep it shut.
    int opened = 0;
    for (int i = 0; i < 1000; ++i) {
        const double level = (i % 7 == 3) ? kThreshold + 3.0 : kThreshold - 9.0;
        if (feed(gate, level).open) {
            ++opened;
        }
    }
    CHECK(opened == 0);
}

TEST_CASE("an off squelch passes the audio untouched from the first sample",
          "[engine][squelch]") {
    SquelchGate gate;
    const auto step = feed(gate, -150.0, revenant::engine::kSquelchOffDbfs);
    CHECK(step.open);
    CHECK(step.audible);
    for (const float s : step.audio) {
        REQUIRE(s == 1.0F);
    }
}

TEST_CASE("the fade is applied per frame across stereo lanes", "[engine][squelch]") {
    SquelchGate gate;
    std::vector<float> audio(2 * kFrames, 1.0F);
    gate.process(power_of(-40.0), audio, 2, kRate, kThreshold, crisp());
    for (std::size_t f = 0; f < kFrames; ++f) {
        REQUIRE(audio[2 * f] == audio[2 * f + 1]);
    }
    CHECK(audio.back() == 1.0F);
}
