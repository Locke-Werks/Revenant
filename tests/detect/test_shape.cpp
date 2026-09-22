// measure_band: what a band looks like, from the excess the detector holds.
//
// EVERY CASE HERE IS A SHAPE WHOSE ANSWER IS KNOWN BY ARITHMETIC rather than
// measured, which is the same split tests/detect/CMakeLists.txt draws between
// test_detector.cpp and test_detector_scene.cpp. A rectangle's peak over its
// mean is one because every bin is the mean. A band with nothing in its lower
// half is asymmetric by exactly the whole of its power. Getting those wrong is
// an arithmetic bug and it should fail here, where the expected number is
// written down, rather than in a scene where it would read as the detector
// disagreeing with a modulator.
//
// WHAT IS DELIBERATELY NOT ASSERTED HERE: that any of these numbers separates
// a signal from interference. They are measurements and no threshold on them
// has been chosen. Scoring them against scenes with known truth is a separate
// job and this file would be the wrong place to pretend it had been done.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

#include "core/detect/shape.h"

using Catch::Approx;
using revenant::detect::BandShape;
using revenant::detect::kSkirtEndFraction;
using revenant::detect::kSkirtReachWidths;
using revenant::detect::measure_band;

namespace {

// A spectrum of `bins` zeroes, so a case states only the shape it cares about.
[[nodiscard]] std::vector<double> flat(std::size_t bins) { return std::vector<double>(bins, 0.0); }

// The whole array as the walk's bounds, which is what a band with no
// neighbours gets from the detector.
[[nodiscard]] BandShape measure(const std::vector<double>& excess,
                                std::size_t first,
                                std::size_t last)
{
    return measure_band(excess, first, last, 0, excess.size());
}

}  // namespace

// A flat band is the reference every other case is read against: every bin is
// the mean, both halves are equal, and nothing reaches past the edges.
TEST_CASE("a rectangular band is flat, balanced and has no skirts", "[detect][shape]")
{
    std::vector<double> excess = flat(200);
    for (std::size_t i = 80; i < 90; ++i) {
        excess[i] = 1.0;
    }

    const BandShape shape = measure(excess, 80, 89);
    REQUIRE(shape.measured);

    CHECK(shape.peak_to_mean == Approx(1.0));
    CHECK(shape.lower_fraction == Approx(0.5));
    CHECK(shape.skirt_fraction == Approx(0.0));

    // Two widths each side were available to walk, so a zero skirt is a
    // measurement rather than a band that had nowhere to look.
    CHECK(shape.skirt_bins_available == 40);
}

// Rejects a peak-to-mean taken against the peak rather than the mean, which is
// one by construction and would make every band look flat. A carrier puts its
// whole power in one bin, so the ratio is the bin count.
TEST_CASE("a carrier reads as peaked by its own width", "[detect][shape]")
{
    std::vector<double> excess = flat(200);
    excess[104] = 9.0;

    const BandShape shape = measure(excess, 100, 108);
    REQUIRE(shape.measured);

    // Nine bins, all the power in one: mean is one ninth of the peak.
    CHECK(shape.peak_to_mean == Approx(9.0));
}

// Rejects a concentration that grows with the band's width, which is the
// defect peak_to_mean has and the reason this field exists beside it. A
// rectangle spreads its power evenly, so three bins hold exactly three bins'
// worth however many there are.
TEST_CASE("a rectangular band concentrates by its bin count alone", "[detect][shape]")
{
    for (const std::size_t width : {std::size_t{10}, std::size_t{40}, std::size_t{160}}) {
        std::vector<double> excess = flat(1000);
        for (std::size_t i = 400; i < 400 + width; ++i) {
            excess[i] = 1.0;
        }

        const BandShape shape = measure(excess, 400, 400 + width - 1);
        REQUIRE(shape.measured);

        CAPTURE(width, shape.peak_to_mean, shape.concentration);

        // Three of `width` equal bins, exactly.
        CHECK(shape.concentration == Approx(3.0 / static_cast<double>(width)));

        // And the number this one is here to be better than: flat at one
        // whatever the width, which is true and says nothing.
        CHECK(shape.peak_to_mean == Approx(1.0));
    }
}

// Rejects a concentration that cannot tell a band which IS a carrier from a
// band that merely CONTAINS one. Both put all their power in three bins; what
// differs is the width the detector reported, and the field's comment says to
// read the two together. This is the arithmetic behind that instruction.
TEST_CASE("concentration is one for a carrier at any reported width", "[detect][shape]")
{
    std::vector<double> excess = flat(1000);
    excess[499] = 1.0;
    excess[500] = 8.0;
    excess[501] = 1.0;

    // Sized to the carrier, which is a detection that got the width right.
    const BandShape tight = measure(excess, 495, 505);
    REQUIRE(tight.measured);
    CHECK(tight.concentration == Approx(1.0));

    // The same carrier inside a band a hundred times too wide, which is the
    // shape of the 4.3 kHz detection on 20 m that the characteriser then
    // called an unmodulated carrier.
    const BandShape loose = measure(excess, 100, 900);
    REQUIRE(loose.measured);
    CHECK(loose.concentration == Approx(1.0));

    // peak_to_mean is what moved, and it moved by the width rather than by
    // anything about the signal: eight over the mean in each case.
    CHECK(loose.peak_to_mean > 50.0 * tight.peak_to_mean);
}

// REJECTS a window that runs off the end of the band and reads a neighbour.
// The strongest three bins have to be three bins OF THE BAND, so a carrier
// sitting just outside it must not raise the number.
TEST_CASE("the concentration window stays inside the band", "[detect][shape]")
{
    std::vector<double> excess = flat(200);
    for (std::size_t i = 80; i < 90; ++i) {
        excess[i] = 1.0;
    }

    // A spike one bin past each edge, far larger than the band itself.
    excess[79] = 100.0;
    excess[90] = 100.0;

    const BandShape shape = measure(excess, 80, 89);
    REQUIRE(shape.measured);
    CHECK(shape.concentration == Approx(0.3));
}

// A band of one, two or three bins has every bin it owns inside the window, so
// the answer is one by construction and carries no information. Asserted so
// that it is a stated behaviour a reader can find rather than a surprise, and
// because the alternative, refusing to measure, would lose the other fields
// with it.
TEST_CASE("a band no wider than the window reads one", "[detect][shape]")
{
    std::vector<double> excess = flat(200);
    excess[100] = 4.0;
    excess[101] = 1.0;

    CHECK(measure(excess, 100, 100).concentration == Approx(1.0));
    CHECK(measure(excess, 100, 101).concentration == Approx(1.0));
    CHECK(measure(excess, 99, 101).concentration == Approx(1.0));

    // Past the window it starts measuring. Five equal bins hold three fifths
    // of themselves in any three of them.
    std::vector<double> spread = flat(200);
    for (std::size_t i = 100; i < 105; ++i) {
        spread[i] = 1.0;
    }
    CHECK(measure(spread, 100, 104).concentration == Approx(0.6));
}

// Rejects a half-split that puts the middle bin in one side. A band of odd
// width with everything dead centre is balanced, and counting the centre bin
// as "lower" would report the most symmetric shape there is as asymmetric.
TEST_CASE("a centred carrier of odd width is balanced", "[detect][shape]")
{
    std::vector<double> excess = flat(200);
    excess[104] = 9.0;

    CHECK(measure(excess, 100, 108).lower_fraction == Approx(0.5));
}

// The sideband case docs/detection.md names: an asymmetric block is SSB, and
// which side it is on is the difference between USB and LSB.
TEST_CASE("a one-sided band reads as one-sided", "[detect][shape]")
{
    std::vector<double> upper = flat(200);
    for (std::size_t i = 105; i < 110; ++i) {
        upper[i] = 2.0;
    }
    CHECK(measure(upper, 100, 109).lower_fraction == Approx(0.0));

    std::vector<double> lower = flat(200);
    for (std::size_t i = 100; i < 105; ++i) {
        lower[i] = 2.0;
    }
    CHECK(measure(lower, 100, 109).lower_fraction == Approx(1.0));

    // And a band leaning rather than committed, which is what a real sideband
    // measured through a filter looks like: three quarters below, one above.
    std::vector<double> leaning = flat(200);
    for (std::size_t i = 100; i < 105; ++i) {
        leaning[i] = 3.0;
    }
    for (std::size_t i = 105; i < 110; ++i) {
        leaning[i] = 1.0;
    }
    CHECK(measure(leaning, 100, 109).lower_fraction == Approx(0.75));
}

// THE CASE THE MEASUREMENT EXISTS FOR. A band that stops dead reports nothing
// outside it; one that sags away reports how far the sag ran. The two bands
// here carry the same power over the same width and differ only in what
// happens past their edges, which is exactly the difference between a filtered
// transmission and a patch of floor the estimator did not track.
TEST_CASE("a band that sags away reports its skirts", "[detect][shape]")
{
    std::vector<double> sagging = flat(200);
    for (std::size_t i = 80; i < 90; ++i) {
        sagging[i] = 1.0;
    }
    // Six bins each side at half the band's level, which is well above the
    // tenth of the mean the walk measures to.
    for (std::size_t i = 74; i < 80; ++i) {
        sagging[i] = 0.5;
    }
    for (std::size_t i = 90; i < 96; ++i) {
        sagging[i] = 0.5;
    }

    const BandShape shape = measure(sagging, 80, 89);
    REQUIRE(shape.measured);

    // Twelve bins of skirt on a ten bin band.
    CHECK(shape.skirt_fraction == Approx(1.2));

    // And the sharp-edged band of the same width and power reports none, so
    // the number is about the edges and not about the level.
    std::vector<double> sharp = flat(200);
    for (std::size_t i = 80; i < 90; ++i) {
        sharp[i] = 1.0;
    }
    CHECK(measure(sharp, 80, 89).skirt_fraction == Approx(0.0));
}

// Rejects a walk that stops at the first bin below the band rather than at the
// level. A skirt an order of magnitude down is not a skirt, and a walk that
// counted it would report the sharpest possible edge as gradual.
TEST_CASE("a skirt ends at a tenth of the band, not at the first dip", "[detect][shape]")
{
    std::vector<double> excess = flat(200);
    for (std::size_t i = 80; i < 90; ++i) {
        excess[i] = 1.0;
    }
    // Just under a tenth of the mean: below the level, so not skirt.
    excess[79] = kSkirtEndFraction * 0.9;
    excess[90] = kSkirtEndFraction * 0.9;
    CHECK(measure(excess, 80, 89).skirt_fraction == Approx(0.0));

    // Just over it, so one bin each side counts.
    excess[79] = kSkirtEndFraction * 1.1;
    excess[90] = kSkirtEndFraction * 1.1;
    CHECK(measure(excess, 80, 89).skirt_fraction == Approx(0.2));
}

// Rejects an unbounded walk, which on a band that never comes down runs to the
// end of the spectrum and reports a fraction that says more about where the
// band sat than about its shape.
TEST_CASE("a skirt walk is capped at two widths each side", "[detect][shape]")
{
    std::vector<double> excess(400, 1.0);

    const BandShape shape = measure(excess, 180, 189);
    REQUIRE(shape.measured);

    // Ten bins wide, two widths each side, so twenty each way and forty in
    // total however far the level actually runs.
    CHECK(shape.skirt_fraction == Approx(2.0 * kSkirtReachWidths));
    CHECK(shape.skirt_bins_available == 40);
}

// Rejects a walk that reads its neighbour's power as its own edge. Two signals
// side by side each stop where the other starts, and a band bounded by a
// neighbour has not measured its own edge at all.
TEST_CASE("a walk stops at the bounds it was given", "[detect][shape]")
{
    std::vector<double> excess(400, 1.0);

    // Two bins of room below and none above, which is what the detector's own
    // accepted-peak bounds look like for a band wedged against its neighbour.
    const BandShape shape = measure_band(excess, 180, 189, 178, 190);
    REQUIRE(shape.measured);

    CHECK(shape.skirt_bins_available == 2);
    CHECK(shape.skirt_fraction == Approx(0.2));
}

// A band against the end of the spectrum has nowhere to walk, and the reported
// zero is the most signal-like answer the measurement can give. The available
// count is what stops a consumer believing it.
TEST_CASE("a band against the edge reports that it could not look", "[detect][shape]")
{
    std::vector<double> excess = flat(20);
    for (std::size_t i = 0; i < 10; ++i) {
        excess[i] = 1.0;
    }

    const BandShape shape = measure_band(excess, 0, 9, 0, 20);
    REQUIRE(shape.measured);

    CHECK(shape.skirt_fraction == Approx(0.0));

    // Nothing below, ten above: the zero is half a measurement.
    CHECK(shape.skirt_bins_available == 10);
}

// Degenerate inputs answer rather than throw, and say they measured nothing,
// on the same rule the ui models follow: a zero that means "not measured" and
// a zero that means "measured zero" have to be distinguishable.
TEST_CASE("a band with nothing in it is not measured", "[detect][shape]")
{
    const std::vector<double> empty;
    CHECK_FALSE(measure_band(empty, 0, 0, 0, 0).measured);

    // All zero excess: there is a band but no power to describe.
    const std::vector<double> silent = flat(200);
    CHECK_FALSE(measure(silent, 80, 89).measured);

    // Reversed bounds, and a band running off the end.
    std::vector<double> excess = flat(200);
    excess[80] = 1.0;
    CHECK_FALSE(measure(excess, 89, 80).measured);
    CHECK_FALSE(measure(excess, 195, 205).measured);

    // Bounds that do not contain the band, which is a caller error rather than
    // a shape, and is refused rather than clamped into a different band.
    CHECK_FALSE(measure_band(excess, 80, 89, 85, 200).measured);
    CHECK_FALSE(measure_band(excess, 80, 89, 0, 85).measured);
}

// Negative excess is clamped rather than summed. The detector's own excess is
// already clamped at zero, so this is about a caller handing over a raw
// difference, where a bin below its floor would otherwise subtract from the
// band's power and could drive the total negative.
TEST_CASE("bins below the floor do not subtract from a band", "[detect][shape]")
{
    std::vector<double> excess = flat(200);
    for (std::size_t i = 80; i < 90; ++i) {
        excess[i] = 1.0;
    }
    excess[84] = -5.0;

    const BandShape shape = measure(excess, 80, 89);
    REQUIRE(shape.measured);

    // Nine bins of one and one of nothing.
    CHECK(shape.peak_to_mean == Approx(10.0 / 9.0));
}
