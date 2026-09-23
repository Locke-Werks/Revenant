// AftLoop and its measurements: the rules docs/ui-spectrum.md sets for
// automatic frequency tracking, asserted without a window or an engine.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "models/aft.h"

using Catch::Matchers::WithinAbs;
using revenant::ui::aft_rule_for;
using revenant::ui::AftFrame;
using revenant::ui::AftLoop;
using revenant::ui::AftRule;
using revenant::ui::AftSettings;
using revenant::ui::AftState;
using revenant::ui::measure_centroid;
using revenant::ui::measure_peak;

namespace {

constexpr double kCentre = 7'074'000.0;
constexpr double kBinHz = 10.0;
constexpr int kBins = 512;
constexpr float kFloorDb = -100.0F;
constexpr double kFrameS = 1.0 / 30.0;

// A frame of kBins around a receiver at centre_hz, with its filter at
// +/- half_width_hz, flat at the floor except for what the caller adds.
struct FrameBuilder {
    double centre_hz = kCentre;
    double half_width_hz = 1500.0;
    std::vector<float> bins = std::vector<float>(kBins, kFloorDb);

    [[nodiscard]] double first_bin_hz() const { return centre_hz - (kBins / 2) * kBinHz; }

    [[nodiscard]] double bin_hz_at(int i) const { return first_bin_hz() + i * kBinHz; }

    // A carrier whose lobe is a parabola in decibels, which is what the peak
    // interpolation assumes, so the estimate can be checked to a fraction of
    // a bin.
    FrameBuilder& carrier(double hz, float snr_db)
    {
        for (int i = 0; i < kBins; ++i) {
            const double d = (bin_hz_at(i) - hz) / kBinHz;
            const auto level = static_cast<float>(kFloorDb + snr_db - 3.0 * d * d);
            bins[static_cast<std::size_t>(i)] = std::max(bins[static_cast<std::size_t>(i)], level);
        }
        return *this;
    }

    // A flat-topped occupied band, the shape a deviation swing averages to.
    FrameBuilder& band(double low_hz, double high_hz, float snr_db)
    {
        for (int i = 0; i < kBins; ++i) {
            if (bin_hz_at(i) >= low_hz && bin_hz_at(i) <= high_hz) {
                bins[static_cast<std::size_t>(i)] = kFloorDb + snr_db;
            }
        }
        return *this;
    }

    [[nodiscard]] AftFrame frame() const
    {
        AftFrame f;
        f.power_db = bins;
        f.first_bin_hz = first_bin_hz();
        f.bin_hz = kBinHz;
        f.floor_db = kFloorDb;
        f.search_low_hz = centre_hz - half_width_hz;
        f.search_high_hz = centre_hz + half_width_hz;
        return f;
    }
};

// Runs the loop against a carrier at an absolute frequency given by
// carrier_at(t), moving the receiver whenever the loop says to, and returns
// the receiver's final centre. Every move is recorded.
struct Run {
    double centre_hz = kCentre;
    std::vector<double> move_times;
    std::vector<double> move_sizes;
    bool jumped = false;
};

template <typename CarrierAt>
Run simulate(AftLoop& loop, AftRule rule, double seconds, CarrierAt carrier_at, float snr = 30.0F)
{
    Run run;
    for (double t = 0.0; t < seconds; t += kFrameS) {
        FrameBuilder b;
        b.centre_hz = run.centre_hz;
        b.carrier(carrier_at(t), snr);
        const auto step = loop.step(t, run.centre_hz, rule, b.frame());
        run.jumped = run.jumped || step.state == AftState::Jumped;
        if (step.move) {
            run.move_times.push_back(t);
            run.move_sizes.push_back(step.centre_hz - run.centre_hz);
            run.centre_hz = step.centre_hz;
        }
    }
    return run;
}

AftLoop enabled_loop()
{
    AftLoop loop;
    loop.set_enabled(true);
    return loop;
}

}  // namespace

// Rejects a table that offers a stand-in centre on a mode the doc says has
// none to measure: SSB and DSB have no carrier, and raw has no statement of
// what the signal is.
TEST_CASE("each mode gets the doc's rule, and the carrierless ones hold", "[aft]")
{
    CHECK(aft_rule_for("am") == AftRule::Peak);
    CHECK(aft_rule_for("cw") == AftRule::KeyedPeak);
    CHECK(aft_rule_for("nfm") == AftRule::Centroid);
    CHECK(aft_rule_for("wfm") == AftRule::Centroid);
    CHECK(aft_rule_for("usb") == AftRule::Hold);
    CHECK(aft_rule_for("lsb") == AftRule::Hold);
    CHECK(aft_rule_for("dsb") == AftRule::Hold);
    CHECK(aft_rule_for("raw") == AftRule::Hold);
    CHECK(aft_rule_for("") == AftRule::Hold);
}

// Rejects a loop that starts enabled.
TEST_CASE("AFT is off until it is turned on", "[aft]")
{
    AftLoop loop;
    CHECK_FALSE(loop.enabled());
    const Run run = simulate(loop, AftRule::Peak, 5.0, [](double) { return kCentre + 800.0; });
    CHECK(run.move_times.empty());
}

// Rejects nearest-bin peak picking, which makes the effective deadband a bin
// wide whatever it is set to.
TEST_CASE("the peak lands between bins", "[aft]")
{
    FrameBuilder b;
    b.carrier(kCentre + 123.4, 30.0F);
    const auto m = measure_peak(b.frame());
    REQUIRE(m.present);
    CHECK_THAT(m.hz, WithinAbs(kCentre + 123.4, 0.5));
    CHECK_THAT(m.snr_db, WithinAbs(30.0, 0.5));
}

// Rejects searching the whole frame: a louder signal next to the filter is
// not the one the receiver is on.
TEST_CASE("the peak is looked for inside the filter only", "[aft]")
{
    FrameBuilder b;
    b.carrier(kCentre + 300.0, 20.0F).carrier(kCentre + 2000.0, 40.0F);
    const auto m = measure_peak(b.frame());
    REQUIRE(m.present);
    CHECK_THAT(m.hz, WithinAbs(kCentre + 300.0, 1.0));
}

TEST_CASE("the centroid of a symmetric band is its middle", "[aft]")
{
    FrameBuilder b;
    b.band(kCentre + 200.0, kCentre + 1000.0, 25.0F);
    const auto m = measure_centroid(b.frame(), 12.0F);
    REQUIRE(m.present);
    CHECK_THAT(m.hz, WithinAbs(kCentre + 600.0, kBinHz));
}

// Rejects a loop with no deadband, which hunts.
TEST_CASE("an error inside the deadband moves nothing", "[aft]")
{
    AftLoop loop = enabled_loop();
    const Run run = simulate(loop, AftRule::Peak, 10.0, [](double) { return kCentre + 20.0; });
    CHECK(run.move_times.empty());
}

// Rejects a loop that jumps straight to the measurement, and one that moves
// on every frame: moves are spaced by the interval and each is bounded by the
// slew, and it still arrives.
TEST_CASE("a large error is closed at the rate limit", "[aft]")
{
    const AftSettings settings;
    AftLoop loop(settings);
    loop.set_enabled(true);
    const double carrier = kCentre + 1000.0;
    const Run run = simulate(loop, AftRule::Peak, 20.0, [&](double) { return carrier; });

    REQUIRE_FALSE(run.move_times.empty());
    const double largest = settings.max_slew_hz_per_s * settings.min_interval_s;
    for (const double size : run.move_sizes) {
        CHECK(size > 0.0);
        CHECK(size <= largest + 1e-9);
    }
    for (std::size_t i = 1; i < run.move_times.size(); ++i) {
        CHECK(run.move_times[i] - run.move_times[i - 1] >= settings.min_interval_s - 1e-9);
    }
    CHECK(std::fabs(carrier - run.centre_hz) <= settings.deadband_hz);
    CHECK_FALSE(run.jumped);
}

// Rejects a loop that moves on an empty frame, where the "peak" is whichever
// noise bin was loudest.
TEST_CASE("with no signal it holds still", "[aft]")
{
    AftLoop loop = enabled_loop();
    FrameBuilder b;
    for (int i = 0; i < kBins; ++i) {
        b.bins[static_cast<std::size_t>(i)] = kFloorDb + static_cast<float>((i * 7) % 5);
    }
    for (double t = 0.0; t < 5.0; t += kFrameS) {
        const auto step = loop.step(t, kCentre, AftRule::Peak, b.frame());
        CHECK_FALSE(step.move);
        CHECK(step.state == AftState::NoSignal);
    }
}

// Rejects a loop that fights the dial: tuning by hand wins for the hold, and
// the loop then starts again from where the receiver was left.
TEST_CASE("tuning by hand holds the loop, then it resumes", "[aft]")
{
    const AftSettings settings;
    AftLoop loop(settings);
    loop.set_enabled(true);
    loop.operator_tuned(0.0);

    FrameBuilder b;
    b.carrier(kCentre + 600.0, 30.0F);
    bool moved_during_hold = false;
    bool moved_after = false;
    for (double t = 0.0; t < settings.operator_hold_s + 2.0; t += kFrameS) {
        const auto step = loop.step(t, kCentre, AftRule::Peak, b.frame());
        if (t < settings.operator_hold_s) {
            CHECK(step.state == AftState::Yielding);
            moved_during_hold = moved_during_hold || step.move;
        } else {
            moved_after = moved_after || step.move;
        }
    }
    CHECK_FALSE(moved_during_hold);
    CHECK(moved_after);
}

// Rejects forget() clearing the hold. A mode change is tuning by hand and a
// new receiver underneath, so the new receiver's first frame arrives just
// after operator_tuned; the loop was seen on screen correcting two seconds
// after a mode change when forget() took the hold with it.
TEST_CASE("a new receiver does not end the operator's hold", "[aft]")
{
    const AftSettings settings;
    AftLoop loop(settings);
    loop.set_enabled(true);
    loop.operator_tuned(0.0);
    loop.forget();

    FrameBuilder b;
    b.carrier(kCentre + 600.0, 30.0F);
    const auto step = loop.step(0.1, kCentre, AftRule::Peak, b.frame());
    CHECK(step.state == AftState::Yielding);
    CHECK_FALSE(step.move);
}

// Rejects a loop that chases whatever is loudest: a signal a kilohertz away
// appearing in a frame is not drift, and it is refused however long it stays.
TEST_CASE("a jump is not chased, even after a long wait", "[aft]")
{
    AftLoop loop = enabled_loop();
    double centre = kCentre;
    for (double t = 0.0; t < 3.0; t += kFrameS) {
        FrameBuilder b;
        b.centre_hz = centre;
        b.carrier(kCentre + 10.0, 30.0F);
        const auto step = loop.step(t, centre, AftRule::Peak, b.frame());
        if (step.move) {
            centre = step.centre_hz;
        }
    }
    const double settled = centre;
    for (double t = 3.0; t < 60.0; t += kFrameS) {
        FrameBuilder b;
        b.centre_hz = centre;
        b.carrier(kCentre + 1010.0, 30.0F);
        const auto step = loop.step(t, centre, AftRule::Peak, b.frame());
        CHECK(step.state == AftState::Jumped);
        CHECK_FALSE(step.move);
    }
    CHECK(centre == settled);
}

// Rejects a jump threshold tight enough to trip on real drift.
TEST_CASE("slow drift is followed", "[aft]")
{
    AftLoop loop = enabled_loop();
    const auto carrier_at = [](double t) { return kCentre + 5.0 * t; };
    const Run run = simulate(loop, AftRule::Peak, 60.0, carrier_at);
    CHECK_FALSE(run.jumped);
    CHECK(std::fabs(carrier_at(60.0) - run.centre_hz) <= 60.0);
}

// Rejects CW tracking through the gaps, where the loudest bin is noise or a
// key click rather than the carrier.
TEST_CASE("CW holds while the key is up", "[aft]")
{
    const AftSettings settings;
    AftLoop loop(settings);
    loop.set_enabled(true);
    FrameBuilder b;
    b.carrier(kCentre + 400.0, settings.min_snr_db + 2.0F);
    for (double t = 0.0; t < 5.0; t += kFrameS) {
        const auto step = loop.step(t, kCentre, AftRule::KeyedPeak, b.frame());
        CHECK(step.state == AftState::KeyUp);
        CHECK_FALSE(step.move);
    }
}

// Rejects a centroid rule that acts on single frames, which on FM wanders
// with the modulation: it averages first and only then moves.
TEST_CASE("the centroid rule averages before it moves", "[aft]")
{
    const AftSettings settings;
    AftLoop loop(settings);
    loop.set_enabled(true);
    FrameBuilder b;
    b.band(kCentre - 400.0, kCentre + 1200.0, 25.0F);

    bool moved_early = false;
    bool moved_later = false;
    double first_move = 0.0;
    for (double t = 0.0; t < settings.centroid_tau_s + 2.0; t += kFrameS) {
        const auto step = loop.step(t, kCentre, AftRule::Centroid, b.frame());
        if (t < settings.centroid_tau_s) {
            moved_early = moved_early || step.move;
            if (t > 0.0) {
                CHECK(step.state == AftState::Averaging);
            }
        } else if (step.move && !moved_later) {
            moved_later = true;
            first_move = step.centre_hz - kCentre;
        }
    }
    CHECK_FALSE(moved_early);
    CHECK(moved_later);
    CHECK(first_move > 0.0);
}

// Rejects inventing a centre for sideband: the suppressed carrier is the
// receiver's centre, so there is nothing to track and nothing moves.
TEST_CASE("a mode with no rule never moves", "[aft]")
{
    AftLoop loop = enabled_loop();
    const Run run = simulate(loop, AftRule::Hold, 5.0, [](double) { return kCentre + 800.0; });
    CHECK(run.move_times.empty());
}

TEST_CASE("turning it off stops it at once", "[aft]")
{
    AftLoop loop = enabled_loop();
    FrameBuilder b;
    b.carrier(kCentre + 800.0, 30.0F);
    CHECK(loop.step(0.0, kCentre, AftRule::Peak, b.frame()).move);
    loop.set_enabled(false);
    const auto step = loop.step(1.0, kCentre, AftRule::Peak, b.frame());
    CHECK(step.state == AftState::Off);
    CHECK_FALSE(step.move);
}
