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

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <format>
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
    auto scale = make_scale({});
    const auto quiet = settle(scale, -95.0F, -78.0F, 60.0);
    INFO(std::format("quiet band settled at {} to {}", quiet.floor_db, quiet.ceiling_db));
    REQUIRE(quiet.ceiling_db == Approx(-78.0).margin(0.5));

    // A carrier appears, forty decibels above the band.
    constexpr float kSignalHigh = -38.0F;

    const auto after_one = scale.update(-95.0F, kSignalHigh, kFrameSeconds);
    const auto after_two = scale.update(-95.0F, kSignalHigh, kFrameSeconds);
    const auto after_three = scale.update(-95.0F, kSignalHigh, kFrameSeconds);

    INFO(std::format("ceiling after one frame {}, two {}, three {}", after_one.ceiling_db,
                     after_two.ceiling_db, after_three.ceiling_db));

    // Within three frames, which is eighty milliseconds, the ceiling is
    // within three decibels of the new level: under a third of one step of a
    // ten-step colour map, so the signal is drawn with structure in it from
    // essentially the moment it appears.
    CHECK(kSignalHigh - after_three.ceiling_db < 3.0F);

    // And it moved most of the way on the very first frame, which is what
    // "expand within a frame or two" means.
    CHECK(after_one.ceiling_db > quiet.ceiling_db + 0.5F * (kSignalHigh - quiet.ceiling_db));

    // The contrast with the decay, measured rather than asserted: the same
    // step in the other direction barely moves in one frame, which is what
    // stops the scale rescaling under the operator between transmissions.
    const auto one_frame_down = scale.update(-95.0F, -78.0F, kFrameSeconds);
    INFO(std::format("one frame after the signal stops the ceiling moved from {} to {}",
                     after_three.ceiling_db, one_frame_down.ceiling_db));
    CHECK(after_three.ceiling_db - one_frame_down.ceiling_db < 0.05F);
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
