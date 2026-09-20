// The colour map's two ends: the behaviour docs/ui-spectrum.md specifies,
// checked as behaviour.
//
// None of this needs a GPU. The measurement is on the device and the
// recurrence that turns it into a scale is two floats on the host, which is
// the whole reason this file can step thirty seconds of source time in a
// microsecond and ask what the display would have looked like.
//
// The cases are the sentences in the specification, one each:
//
//   both ends track, and neither is fixed
//   expansion is a frame or two and contraction is thirty seconds
//   a frame whose content is uniform does not saturate
//   a signal appearing suddenly is not clipped for thirty seconds
//   the operator can still pin either end
//
// The last two are the ones the placeholder failed, so they are written as
// the failure rather than as the fix: a frame count and a decibel margin,
// both of which a regression would move.
//
// The outlier guard is not in that specification and its cases are written
// differently. It exists because of a defect the specification's own
// asymmetry produces, so each of its cases runs the same input twice, once
// with the guard switched off and once with it on, and prints both. That is
// the only way a reader can tell the number that matters, which is the
// difference, from the number the arithmetic happens to give.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <string>

#include "core/engine/spectrum_scale.h"

using namespace revenant;
using Catch::Approx;

namespace {

// The frame interval the engine ships at: 2.4 MS/s in 65536-sample blocks is
// a frame every 27.3 ms. Every case steps in these so that "a frame or two"
// and "thirty seconds" are the same units the operator sees.
constexpr double kFrameSeconds = 65'536.0 / 2'400'000.0;

engine::SpectrumScale make_scale(const engine::SpectrumScaleConfig& config) {
    auto made = engine::SpectrumScale::create(config);
    const std::string why = made.has_value() ? std::string{"ok"} : made.error().message;
    INFO(why);
    REQUIRE(made.has_value());
    return std::move(*made);
}

// Runs the scale for a stretch of source time at a steady measurement, which
// is what a band that is not changing looks like.
engine::SpectrumScaleLevels settle(engine::SpectrumScale& scale, float low_db, float high_db,
                                   double seconds) {
    engine::SpectrumScaleLevels ends = scale.current();
    const auto frames = static_cast<std::size_t>(seconds / kFrameSeconds);
    for (std::size_t i = 0; i < frames; ++i) {
        ends = scale.update(low_db, high_db, kFrameSeconds);
    }
    return ends;
}

// A scale with the outlier guard switched off, which is every version of this
// class before the guard existed. The defect cases run this beside the
// default so the fix is measured against the behaviour it replaced rather
// than against an assertion about it.
engine::SpectrumScaleConfig unguarded() {
    engine::SpectrumScaleConfig config;
    config.outlier_window_frames = 1;
    return config;
}

}  // namespace

TEST_CASE("the first frame jumps to the measurement rather than ramping to it",
          "[spectrum][scale][m1]") {
    // A display fading in from whatever the members were initialised to,
    // over half a minute, is a display that looks broken for half a minute
    // every time it is opened.
    auto scale = make_scale({});
    const auto ends = scale.update(-92.0F, -40.0F, 0.0);

    CHECK(ends.floor_db == Approx(-92.0));
    CHECK(ends.ceiling_db == Approx(-40.0));
}

TEST_CASE("both ends track the signal", "[spectrum][scale][m1]") {
    // "The colour map's floor and ceiling both track the signal
    // automatically." Not a fixed floor with an automatic ceiling, which is
    // the version this is most likely to decay into.
    auto scale = make_scale({});
    static_cast<void>(scale.update(-90.0F, -30.0F, 0.0));

    // The whole band comes up twenty decibels, which is what switching in a
    // preamplifier does.
    const auto ends = settle(scale, -70.0F, -10.0F, 120.0);

    INFO(std::format("after two minutes at -70/-10 the map is {} to {}", ends.floor_db,
                     ends.ceiling_db));
    CHECK(ends.floor_db == Approx(-70.0).margin(0.5));
    CHECK(ends.ceiling_db == Approx(-10.0).margin(0.5));
}

TEST_CASE("a signal appearing suddenly is not clipped for thirty seconds",
          "[spectrum][scale][m1]") {
    // The case the asymmetry exists for, and the one the per-row placeholder
    // could not have: at a symmetric thirty seconds a transmission that keys
    // up is a solid bar with no structure in it until it has been on the air
    // for half a minute, which on a waterfall is the whole of the event.
    constexpr float kQuietHigh = -78.0F;
    constexpr float kSignalHigh = -38.0F;  // a carrier forty decibels up
    constexpr float kStepDb = kSignalHigh - kQuietHigh;

    auto scale = make_scale({});
    auto bare = make_scale(unguarded());
    const auto quiet = settle(scale, -95.0F, kQuietHigh, 60.0);
    static_cast<void>(settle(bare, -95.0F, kQuietHigh, 60.0));
    INFO(std::format("quiet band settled at {} to {}", quiet.floor_db, quiet.ceiling_db));
    REQUIRE(quiet.ceiling_db == Approx(-78.0).margin(0.5));

    float guarded_ceiling[5] = {quiet.ceiling_db, 0.0F, 0.0F, 0.0F, 0.0F};
    float bare_ceiling[5] = {quiet.ceiling_db, 0.0F, 0.0F, 0.0F, 0.0F};
    for (std::size_t frame = 1; frame < 5; ++frame) {
        guarded_ceiling[frame] = scale.update(-95.0F, kSignalHigh, kFrameSeconds).ceiling_db;
        bare_ceiling[frame] = bare.update(-95.0F, kSignalHigh, kFrameSeconds).ceiling_db;
    }

    // One string rather than an INFO per frame: a Catch2 INFO expires with
    // its scope, so one written inside the loop is gone before any assertion
    // below can carry it.
    std::string report;
    for (std::size_t frame = 1; frame < 5; ++frame) {
        report += std::format("frame {}: guarded {:.2f} dB ({:.1f}% of the step), unguarded "
                              "{:.2f} dB ({:.1f}%). ",
                              frame, guarded_ceiling[frame],
                              100.0F * (guarded_ceiling[frame] - kQuietHigh) / kStepDb,
                              bare_ceiling[frame],
                              100.0F * (bare_ceiling[frame] - kQuietHigh) / kStepDb);
    }
    INFO(report);

    // The outlier guard costs the first frame and nothing after it. The
    // second largest of the last three is still the quiet level until two
    // frames have seen the signal, so frame one moves the ceiling not at all
    // and frames two and three move at full attack speed.
    CHECK(guarded_ceiling[1] == Approx(quiet.ceiling_db).margin(0.01));
    CHECK(bare_ceiling[1] > quiet.ceiling_db + 0.5F * kStepDb);

    // Three frames in, which is 82 ms at the shipped geometry, the step is
    // substantially covered and what is left is a small fraction of one step
    // of a ten-step colour map, so the signal is drawn with structure in it
    // rather than as a solid bar.
    const float left_at_three = kSignalHigh - guarded_ceiling[3];
    INFO(std::format("{:.2f} dB of the {:.0f} dB step still uncovered at three frames",
                     left_at_three, kStepDb));
    CHECK(left_at_three < 5.0F);
    CHECK((guarded_ceiling[3] - kQuietHigh) / kStepDb > 0.85F);

    // And the delay is exactly one frame, not approximately one: the guarded
    // ceiling on frame n is the unguarded ceiling on frame n-1, to four
    // decimal places of a decibel. This is the whole cost of the guard,
    // written as an identity rather than as a tolerance on the coverage,
    // because a tolerance is a number the delay could grow inside.
    for (std::size_t frame = 2; frame < 5; ++frame) {
        INFO(std::format("guarded frame {} is {:.4f}, unguarded frame {} is {:.4f}", frame,
                         guarded_ceiling[frame], frame - 1, bare_ceiling[frame - 1]));
        CHECK(guarded_ceiling[frame] == Approx(bare_ceiling[frame - 1]).margin(1e-4));
    }

    // The contrast with the decay, measured rather than asserted: the same
    // step in the other direction barely moves, which is what stops the scale
    // rescaling under the operator between transmissions. The guard holds the
    // old level for two more frames first, because the signal is still the
    // second largest in the window until it has fallen out of it, so this is
    // read three frames after the carrier stops rather than one.
    static_cast<void>(scale.update(-95.0F, kQuietHigh, kFrameSeconds));
    static_cast<void>(scale.update(-95.0F, kQuietHigh, kFrameSeconds));
    const auto settled_down = scale.update(-95.0F, kQuietHigh, kFrameSeconds);
    const auto one_frame_down = scale.update(-95.0F, kQuietHigh, kFrameSeconds);
    INFO(std::format("three frames after the carrier stops the ceiling is {}, one frame later {}",
                     settled_down.ceiling_db, one_frame_down.ceiling_db));
    CHECK(settled_down.ceiling_db - one_frame_down.ceiling_db < 0.05F);
}

TEST_CASE("one anomalous frame does not hold the colour map for thirty seconds",
          "[spectrum][scale][m1]") {
    // The defect the outlier guard exists for, and the reason the fix could
    // not be a slower attack. A single frame measuring forty decibels high
    // lifts the ceiling most of the way in that one frame, because the attack
    // is one frame period, and the thirty-second decay then holds the
    // displacement long after the frame that caused it is gone.
    //
    // Both devices produce that frame. docs/fft.md has the integrated Radeon
    // corrupting an isolated spectrum dispatch with the correct answer either
    // side of it; on the discrete card the same shape arrives from a
    // transmitter keying up nearby for one frame, which is a correct
    // measurement rather than a fault. Neither is a reason to rescale the map
    // for half a minute.
    constexpr float kQuietHigh = -78.0F;
    constexpr float kAnomalyHigh = -38.0F;

    auto scale = make_scale({});
    auto bare = make_scale(unguarded());

    static_cast<void>(settle(scale, -95.0F, kQuietHigh, 60.0));
    static_cast<void>(settle(bare, -95.0F, kQuietHigh, 60.0));

    // One frame, and the band is exactly as quiet either side of it.
    const auto spike = scale.update(-95.0F, kAnomalyHigh, kFrameSeconds);
    const auto bare_spike = bare.update(-95.0F, kAnomalyHigh, kFrameSeconds);

    const auto after_thirty = settle(scale, -95.0F, kQuietHigh, 30.0);
    const auto bare_after_thirty = settle(bare, -95.0F, kQuietHigh, 30.0);
    const auto after_ninety = settle(scale, -95.0F, kQuietHigh, 60.0);
    const auto bare_after_ninety = settle(bare, -95.0F, kQuietHigh, 60.0);

    INFO(std::format("displacement above the quiet ceiling, guarded then unguarded: "
                     "immediately {:.2f} / {:.2f} dB, at 30 s {:.2f} / {:.2f} dB, at 90 s "
                     "{:.2f} / {:.2f} dB",
                     spike.ceiling_db - kQuietHigh, bare_spike.ceiling_db - kQuietHigh,
                     after_thirty.ceiling_db - kQuietHigh,
                     bare_after_thirty.ceiling_db - kQuietHigh,
                     after_ninety.ceiling_db - kQuietHigh,
                     bare_after_ninety.ceiling_db - kQuietHigh));

    // The unguarded scale is the defect, measured, so that the numbers below
    // it are a comparison rather than a claim. Half a minute after one bad
    // frame the map is still displaced by most of a colour-map step.
    CHECK(bare_spike.ceiling_db - kQuietHigh > 20.0F);
    CHECK(bare_after_thirty.ceiling_db - kQuietHigh > 5.0F);

    // The guarded scale never moves at all, which is the property the order
    // statistic buys and a per-frame cap could not: a value seen once is
    // discarded rather than attenuated, so its size does not matter.
    CHECK(spike.ceiling_db == Approx(kQuietHigh).margin(0.01));
    CHECK(after_thirty.ceiling_db == Approx(kQuietHigh).margin(0.01));
    CHECK(after_ninety.ceiling_db == Approx(kQuietHigh).margin(0.01));
}

TEST_CASE("the rejection does not depend on where in the window the bad frame lands",
          "[spectrum][scale][m1]") {
    // The window is a ring, so a bad frame overwrites whichever slot is next
    // and the held samples arrive at the selection in any rotation. The first
    // version of the selection answered correctly for two of the three
    // rotations and propped the ceiling up on the third, and the case above
    // happened to land on a good one: 2197 frames of settling put the write
    // cursor at slot 1 every time, so it never saw the rotation that failed.
    //
    // Priming with a few more quiet frames first walks the bad one through
    // every slot, which is the cheapest way to make a ring's rotation part of
    // the test rather than part of the luck.
    constexpr float kQuietHigh = -78.0F;
    constexpr float kAnomalyHigh = -38.0F;

    for (std::uint32_t prime = 0; prime <= engine::kSpectrumMaxOutlierWindow; ++prime) {
        auto scale = make_scale({});
        static_cast<void>(settle(scale, -95.0F, kQuietHigh, 60.0));
        for (std::uint32_t i = 0; i < prime; ++i) {
            static_cast<void>(scale.update(-95.0F, kQuietHigh, kFrameSeconds));
        }

        const auto spike = scale.update(-95.0F, kAnomalyHigh, kFrameSeconds);
        const auto after_thirty = settle(scale, -95.0F, kQuietHigh, 30.0);

        INFO(std::format("primed with {} extra quiet frames: ceiling {} at the bad frame and "
                         "{} thirty seconds later",
                         prime, spike.ceiling_db, after_thirty.ceiling_db));
        CHECK(spike.ceiling_db == Approx(kQuietHigh).margin(0.01));
        CHECK(after_thirty.ceiling_db == Approx(kQuietHigh).margin(0.01));
    }
}

TEST_CASE("the floor is guarded the same way and in the other direction",
          "[spectrum][scale][m1]") {
    // The floor expands downwards, so its outlier is a frame reading low, and
    // the guard has to take the second SMALLEST rather than the second
    // largest. Written as its own case because an implementation that
    // guarded the ceiling and left the floor on the raw measurement would
    // pass every other case in this file.
    constexpr float kQuietLow = -95.0F;
    constexpr float kAnomalyLow = -135.0F;

    auto scale = make_scale({});
    auto bare = make_scale(unguarded());
    static_cast<void>(settle(scale, kQuietLow, -78.0F, 60.0));
    static_cast<void>(settle(bare, kQuietLow, -78.0F, 60.0));

    const auto spike = scale.update(kAnomalyLow, -78.0F, kFrameSeconds);
    const auto bare_spike = bare.update(kAnomalyLow, -78.0F, kFrameSeconds);

    const auto after_thirty = settle(scale, kQuietLow, -78.0F, 30.0);
    const auto bare_after_thirty = settle(bare, kQuietLow, -78.0F, 30.0);

    INFO(std::format("floor displacement below the quiet floor, guarded then unguarded: "
                     "immediately {:.2f} / {:.2f} dB, at 30 s {:.2f} / {:.2f} dB",
                     kQuietLow - spike.floor_db, kQuietLow - bare_spike.floor_db,
                     kQuietLow - after_thirty.floor_db, kQuietLow - bare_after_thirty.floor_db));

    CHECK(kQuietLow - bare_spike.floor_db > 20.0F);
    CHECK(kQuietLow - bare_after_thirty.floor_db > 5.0F);
    CHECK(spike.floor_db == Approx(kQuietLow).margin(0.01));
    CHECK(after_thirty.floor_db == Approx(kQuietLow).margin(0.01));
}

TEST_CASE("two consecutive anomalous frames are a signal and are followed",
          "[spectrum][scale][m1]") {
    // The other half of the rule, and the reason it is an order statistic
    // rather than a rejection threshold. Nothing here decides whether a
    // measurement is true. It decides whether more than one frame saw it, so
    // a real transmission that lasts two frames is followed at full speed
    // while an isolated frame is discarded whatever its size.
    constexpr float kQuietHigh = -78.0F;
    constexpr float kSignalHigh = -38.0F;

    auto scale = make_scale({});
    const auto quiet = settle(scale, -95.0F, kQuietHigh, 60.0);

    const auto first = scale.update(-95.0F, kSignalHigh, kFrameSeconds);
    const auto second = scale.update(-95.0F, kSignalHigh, kFrameSeconds);

    INFO(std::format("ceiling {} after one frame of signal, {} after two", first.ceiling_db,
                     second.ceiling_db));

    CHECK(first.ceiling_db == Approx(quiet.ceiling_db).margin(0.01));
    CHECK(second.ceiling_db > quiet.ceiling_db + 0.5F * (kSignalHigh - quiet.ceiling_db));
}

TEST_CASE("a seek forgets the outlier window as well as the level",
          "[spectrum][scale][m1]") {
    // reset() is for a seek, where the band on the far side of the cut has
    // nothing to do with the near side. Keeping the window across it would
    // let two stale measurements outvote the first real one, which is the
    // single input the order statistic has no defence against.
    auto scale = make_scale({});
    static_cast<void>(settle(scale, -95.0F, -78.0F, 60.0));

    scale.reset();
    const auto first = scale.update(-60.0F, -20.0F, 0.0);

    INFO(std::format("first frame after the seek gives {} to {}", first.floor_db,
                     first.ceiling_db));
    CHECK(first.floor_db == Approx(-60.0));
    CHECK(first.ceiling_db == Approx(-20.0));
}

TEST_CASE("a non-finite percentile is dropped rather than carried",
          "[spectrum][scale][m1]") {
    // A corrupted frame is not guaranteed to be merely wrong. One NaN through
    // the recurrence makes every later frame NaN for the life of the process,
    // and the display never recovers, so the measurement is refused entry
    // rather than clamped to something invented.
    auto scale = make_scale({});
    const auto quiet = settle(scale, -95.0F, -78.0F, 60.0);

    const auto poisoned = scale.update(std::numeric_limits<float>::quiet_NaN(),
                                       std::numeric_limits<float>::infinity(), kFrameSeconds);
    INFO(std::format("after a NaN floor and an infinite ceiling the map is {} to {}",
                     poisoned.floor_db, poisoned.ceiling_db));
    CHECK(std::isfinite(poisoned.floor_db));
    CHECK(std::isfinite(poisoned.ceiling_db));
    CHECK(poisoned.floor_db == Approx(quiet.floor_db));
    CHECK(poisoned.ceiling_db == Approx(quiet.ceiling_db));

    // And the frame after it is scaled normally, so one bad readback costs
    // one row rather than the session.
    const auto recovered = scale.update(-95.0F, -78.0F, kFrameSeconds);
    CHECK(recovered.ceiling_db == Approx(quiet.ceiling_db).margin(0.01));
}

TEST_CASE("the decay is about thirty seconds", "[spectrum][scale][m1]") {
    // The number docs/ui-spectrum.md names. Checked at the two points that
    // pin an exponential: one time constant, where 63 percent of the step is
    // gone, and three, where 95 percent is.
    auto scale = make_scale({});
    static_cast<void>(scale.update(-90.0F, -20.0F, 0.0));

    constexpr float kFrom = -20.0F;
    constexpr float kTo = -60.0F;
    constexpr float kStep = kFrom - kTo;

    const auto at_ten = settle(scale, -90.0F, kTo, 10.0);
    const auto at_thirty = settle(scale, -90.0F, kTo, 20.0);
    const auto at_ninety = settle(scale, -90.0F, kTo, 60.0);

    const float gone_at_ten = (kFrom - at_ten.ceiling_db) / kStep;
    const float gone_at_thirty = (kFrom - at_thirty.ceiling_db) / kStep;
    const float gone_at_ninety = (kFrom - at_ninety.ceiling_db) / kStep;

    INFO(std::format("{:.1f}% of the step gone at 10 s, {:.1f}% at 30 s, {:.1f}% at 90 s",
                     gone_at_ten * 100.0F, gone_at_thirty * 100.0F, gone_at_ninety * 100.0F));

    // Ten seconds is a third of a time constant, so about 28 percent.
    CHECK(gone_at_ten == Approx(0.283).margin(0.03));
    // One time constant.
    CHECK(gone_at_thirty == Approx(0.632).margin(0.03));
    // Three.
    CHECK(gone_at_ninety == Approx(0.950).margin(0.02));

    // Still well inside the map after ten seconds, which is the property that
    // matters to a person watching: the scale is not visibly hunting between
    // one transmission and the next.
    CHECK(at_ten.ceiling_db > kTo);
}

TEST_CASE("a frame whose content is uniform does not saturate", "[spectrum][scale][m1]") {
    // A band with no structure in it, or a front end in compression, reports
    // both percentiles in the same histogram bucket. A display handed a span
    // of zero divides by zero or clamps everything to one end of the ramp,
    // and either way the whole row goes solid and says nothing.
    auto scale = make_scale({});
    constexpr float kLevel = -64.5F;

    const auto ends = settle(scale, kLevel, kLevel, 120.0);
    const float span = ends.ceiling_db - ends.floor_db;

    INFO(std::format("a uniform frame at {} gives a map of {} to {}", kLevel, ends.floor_db,
                     ends.ceiling_db));
    CHECK(span >= engine::kSpectrumMinimumSpanDb);

    // And the level the frame is actually at sits strictly inside the map
    // rather than on either end of it, so a bin at that level draws as a
    // middle of the ramp rather than as blank or as solid. This is the
    // invariant every consumer of SpectrumFrame can rely on and the reason
    // the minimum span opens about the midpoint.
    CHECK(kLevel > ends.floor_db);
    CHECK(kLevel < ends.ceiling_db);

    const float position = (kLevel - ends.floor_db) / span;
    INFO(std::format("it draws at {:.2f} of the way up the ramp", position));
    CHECK(position == Approx(0.5).margin(0.01));
}

TEST_CASE("a pinned end does not move", "[spectrum][scale][m1]") {
    // "The operator can still pin either end." Pinning one leaves the other
    // tracking, which is the case a fixed noise floor with live traffic
    // wants.
    SECTION("the floor alone") {
        engine::SpectrumScaleConfig config;
        config.pinned_floor_db = -80.0F;
        auto scale = make_scale(config);

        const auto ends = settle(scale, -120.0F, -30.0F, 120.0);
        INFO(std::format("map {} to {}", ends.floor_db, ends.ceiling_db));
        CHECK(ends.floor_db == Approx(-80.0));
        CHECK(ends.ceiling_db == Approx(-30.0).margin(0.5));
    }

    SECTION("the ceiling alone") {
        engine::SpectrumScaleConfig config;
        config.pinned_ceiling_db = -10.0F;
        auto scale = make_scale(config);

        const auto ends = settle(scale, -90.0F, -55.0F, 120.0);
        INFO(std::format("map {} to {}", ends.floor_db, ends.ceiling_db));
        CHECK(ends.ceiling_db == Approx(-10.0));
        CHECK(ends.floor_db == Approx(-90.0).margin(0.5));
    }

    SECTION("both, which is what comparing two captures needs") {
        engine::SpectrumScaleConfig config;
        config.pinned_floor_db = -100.0F;
        config.pinned_ceiling_db = -20.0F;
        auto scale = make_scale(config);

        // Two very different bands through the same pinned scale give the
        // same map, which is the entire point of the feature.
        const auto first = settle(scale, -110.0F, -25.0F, 60.0);
        const auto second = settle(scale, -60.0F, -21.0F, 60.0);

        CHECK(first.floor_db == Approx(-100.0));
        CHECK(first.ceiling_db == Approx(-20.0));
        CHECK(second.floor_db == Approx(-100.0));
        CHECK(second.ceiling_db == Approx(-20.0));
    }

    SECTION("a pinned pair narrower than the minimum span is honoured as written") {
        // The operator asked for eight decibels across the whole map, which
        // is a legitimate thing to ask for when comparing two nearly
        // identical captures. Widening it would be overriding the
        // instruction rather than protecting the display.
        engine::SpectrumScaleConfig config;
        config.pinned_floor_db = -70.0F;
        config.pinned_ceiling_db = -62.0F;
        auto scale = make_scale(config);

        const auto ends = settle(scale, -90.0F, -30.0F, 30.0);
        CHECK(ends.floor_db == Approx(-70.0));
        CHECK(ends.ceiling_db == Approx(-62.0));
    }
}

TEST_CASE("the scale refuses a configuration it cannot serve", "[spectrum][scale][m1]") {
    {
        engine::SpectrumScaleConfig config;
        config.pinned_floor_db = -20.0F;
        config.pinned_ceiling_db = -40.0F;
        CHECK_FALSE(engine::SpectrumScale::create(config).has_value());
    }
    {
        // An attack slower than the decay is docs/ui-spectrum.md backwards
        // and clips every signal that appears suddenly, which is the one
        // thing the asymmetry exists to prevent.
        engine::SpectrumScaleConfig config;
        config.attack_seconds = 60.0;
        config.decay_seconds = 30.0;
        CHECK_FALSE(engine::SpectrumScale::create(config).has_value());
    }
    {
        engine::SpectrumScaleConfig config;
        config.decay_seconds = 0.0;
        CHECK_FALSE(engine::SpectrumScale::create(config).has_value());
    }
    {
        engine::SpectrumScaleConfig config;
        config.minimum_span_db = 0.0F;
        CHECK_FALSE(engine::SpectrumScale::create(config).has_value());
    }
    {
        // Zero is the one value of the outlier window that means nothing. One
        // is no guard, which is a thing to ask for and is accepted; zero is a
        // window with no measurement in it.
        engine::SpectrumScaleConfig config;
        config.outlier_window_frames = 0;
        CHECK_FALSE(engine::SpectrumScale::create(config).has_value());
    }
    {
        engine::SpectrumScaleConfig config;
        config.outlier_window_frames = engine::kSpectrumMaxOutlierWindow + 1;
        CHECK_FALSE(engine::SpectrumScale::create(config).has_value());
    }
    {
        engine::SpectrumScaleConfig config;
        config.outlier_window_frames = 1;
        CHECK(engine::SpectrumScale::create(config).has_value());
    }
    {
        engine::SpectrumScaleConfig config;
        config.outlier_window_frames = engine::kSpectrumMaxOutlierWindow;
        CHECK(engine::SpectrumScale::create(config).has_value());
    }
}

TEST_CASE("a gap between frames is crossed in one step", "[spectrum][scale][m1]") {
    // A dispatch whose window is not yet contiguous produces no frame, which
    // GraphStats::spectrum_skipped counts. The scale is handed the real
    // elapsed time rather than a frame count, so a two second gap moves it
    // as far as seventy-three frames would have.
    auto stepwise = make_scale({});
    auto single = make_scale({});

    static_cast<void>(stepwise.update(-90.0F, -20.0F, 0.0));
    static_cast<void>(single.update(-90.0F, -20.0F, 0.0));

    const auto frames = static_cast<std::size_t>(2.0 / kFrameSeconds);
    for (std::size_t i = 0; i < frames; ++i) {
        static_cast<void>(stepwise.update(-90.0F, -60.0F, kFrameSeconds));
    }
    const auto jumped =
        single.update(-90.0F, -60.0F, static_cast<double>(frames) * kFrameSeconds);

    INFO(std::format("{} small steps reach {}, one big step reaches {}", frames,
                     stepwise.current().ceiling_db, jumped.ceiling_db));

    // Not bit-identical, because the one-pole recurrence is only exactly
    // composable in the limit, but within a tenth of a decibel over two
    // seconds, which is a tenth of what a colour map step is.
    CHECK(jumped.ceiling_db == Approx(stepwise.current().ceiling_db).margin(0.1));
}
