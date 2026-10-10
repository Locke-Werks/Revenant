// models/span_view.h: the band menu's fit decision and tune target, the view
// window it narrows to, when that view lets go, and the zoom and pan
// arithmetic the Ctrl and Shift wheel and the view keys share.
//
// Each case names the wrong implementation it rejects.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "models/span_view.h"

using Catch::Approx;
using namespace revenant::ui;

namespace {

// An RTL-SDR at 2.4 MS/s with the R820T's tuning range and the shipped bins.
constexpr double kSpan = 2'400'000.0;
constexpr double kTuneLow = 24'000'000.0;
constexpr double kTuneHigh = 1'766'000'000.0;
constexpr std::size_t kBins = 65536;

}  // namespace

TEST_CASE("a band that fits is zoomed to its edges plus the margin", "[span_view]")
{
    // 40 m: 7.000 to 7.300 MHz, landing at the table's 7.150.
    const BandViewPlan plan =
        plan_band_view(7'000'000.0, 7'300'000.0, 7'150'000.0, kSpan, 1'000'000.0, kTuneHigh);
    REQUIRE(plan.zoom);
    CHECK(plan.tune_hz == Approx(7'150'000.0));
    // 5% of 300 kHz either side.
    CHECK(plan.view_low_hz == Approx(6'985'000.0));
    CHECK(plan.view_high_hz == Approx(7'315'000.0));

    // Rejects a view computed from the span rather than the band: on the
    // tuned span the window is about an eighth of it, not all of it.
    const SpanWindow view = window_for_hz(plan.view_low_hz, plan.view_high_hz,
                                          plan.tune_hz - kSpan / 2, plan.tune_hz + kSpan / 2);
    CHECK(view.width() == Approx(330'000.0 / kSpan));
    CHECK(view.low > 0.0);
    CHECK(view.high < 1.0);
}

TEST_CASE("a table centre near one edge is moved only as far as keeps the band in", "[span_view]")
{
    // A 2 MHz band whose landing frequency is near its low edge. Rejects
    // tuning to the table centre unchanged, which would push the top of the
    // band off the span.
    const double low = 100'000'000.0;
    const double high = 102'000'000.0;
    const BandViewPlan plan = plan_band_view(low, high, low + 100'000.0, kSpan, kTuneLow, kTuneHigh);
    REQUIRE(plan.zoom);
    CHECK(plan.tune_hz - kSpan / 2 <= plan.view_low_hz);
    CHECK(plan.tune_hz + kSpan / 2 >= plan.view_high_hz);
    // And no further than that: the top of the padded band sits on the
    // span's top edge. Rejects centring on the band's midpoint.
    CHECK(plan.tune_hz + kSpan / 2 == Approx(plan.view_high_hz));
}

TEST_CASE("a band wider than the span starts at the left edge with no zoom", "[span_view]")
{
    // FM broadcast, 87.5 to 108 MHz. Rejects zooming anyway, and rejects
    // landing on the table centre, which would put the band's start off
    // screen to the left.
    const BandViewPlan plan =
        plan_band_view(87'500'000.0, 108'000'000.0, 98'000'000.0, kSpan, kTuneLow, kTuneHigh);
    CHECK_FALSE(plan.zoom);
    const double left = plan.tune_hz - kSpan / 2;
    CHECK(left == Approx(87'500'000.0 - kBandStartMargin * kSpan));
    CHECK(left < 87'500'000.0);
}

TEST_CASE("a band whose padded width just exceeds the span is not zoomed", "[span_view]")
{
    // Band exactly the span: the margin does not fit. Rejects a fit test on
    // the bare band width.
    const BandViewPlan plan = plan_band_view(50e6, 50e6 + kSpan, 50e6 + kSpan / 2, kSpan,
                                             kTuneLow, kTuneHigh);
    CHECK_FALSE(plan.zoom);
}

TEST_CASE("the tune target stays inside the source's range", "[span_view]")
{
    // A band below the tuner's floor. Rejects sending a request the engine
    // will refuse.
    const BandViewPlan plan =
        plan_band_view(1'800'000.0, 2'000'000.0, 1'900'000.0, kSpan, kTuneLow, kTuneHigh);
    CHECK(plan.tune_hz == Approx(kTuneLow));

    // An unknown range (inverted or equal) is not a clamp to zero.
    const BandViewPlan open = plan_band_view(1'800'000.0, 2'000'000.0, 1'900'000.0, kSpan, 0, 0);
    CHECK(open.tune_hz == Approx(1'900'000.0));
}

TEST_CASE("a band view holds only while the band is inside the span", "[span_view]")
{
    const double lo = 6'985'000.0;
    const double hi = 7'315'000.0;
    CHECK(hz_window_inside(lo, hi, 5'950'000.0, 8'350'000.0));
    // Tuned so the top of the band leaves the span: released. Rejects a test
    // that only checks the band's centre.
    CHECK_FALSE(hz_window_inside(lo, hi, 5'000'000.0, 7'300'000.0));
    CHECK_FALSE(hz_window_inside(lo, hi, 7'000'000.0, 9'400'000.0));
    // No span (disconnected) holds nothing.
    CHECK_FALSE(hz_window_inside(lo, hi, 0.0, 0.0));
}

TEST_CASE("clamping keeps the width and slides back inside", "[span_view]")
{
    const double min_width = min_view_width(kBins);
    // Rejects cutting the window at the edge, which would silently zoom in.
    const SpanWindow over = clamp_view({0.9, 1.1}, min_width);
    CHECK(over.high == Approx(1.0));
    CHECK(over.width() == Approx(0.2));
    const SpanWindow under = clamp_view({-0.05, 0.15}, min_width);
    CHECK(under.low == Approx(0.0));
    CHECK(under.width() == Approx(0.2));
    // Wider than the span is the span.
    const SpanWindow wide = clamp_view({-1.0, 2.0}, min_width);
    CHECK(wide.full());
    // Narrower than the minimum widens to it.
    const SpanWindow thin = clamp_view({0.5, 0.5 + 1e-9}, min_width);
    CHECK(thin.width() == Approx(min_width));
    CHECK(min_width == Approx(static_cast<double>(kMinViewBins) / kBins));
}

TEST_CASE("zooming about the pointer keeps the frequency under it", "[span_view]")
{
    const double min_width = min_view_width(kBins);
    const SpanWindow start{0.2, 0.6};
    const double pointer = 0.25;
    const double anchor = start.low + pointer * start.width();

    // Rejects zooming about the view's centre.
    const SpanWindow in = zoom_about(start, pointer, 2.0, min_width);
    CHECK(in.width() == Approx(0.2));
    CHECK(in.low + pointer * in.width() == Approx(anchor));

    const SpanWindow out = zoom_about(in, pointer, 0.5, min_width);
    CHECK(out.low == Approx(start.low));
    CHECK(out.high == Approx(start.high));

    // Out past the whole span stops at the whole span.
    const SpanWindow all = zoom_about(start, 0.5, 0.01, min_width);
    CHECK(all.full());

    // In past the minimum stops at the minimum, still about the pointer
    // where the edges allow.
    const SpanWindow deep = zoom_about(start, pointer, 1e9, min_width);
    CHECK(deep.width() == Approx(min_width));
    CHECK(deep.low + pointer * deep.width() == Approx(anchor));

    // A zoom at the edge slides rather than leaving the span.
    const SpanWindow edge = zoom_about({0.0, 0.1}, 0.0, 0.5, min_width);
    CHECK(edge.low == Approx(0.0));
    CHECK(edge.width() == Approx(0.2));
}

TEST_CASE("panning moves by view widths and stops at the ends", "[span_view]")
{
    const double min_width = min_view_width(kBins);
    const SpanWindow moved = pan_view({0.4, 0.6}, kPanStep, min_width);
    // A tenth of the VIEW, not of the span. Rejects the span fraction.
    CHECK(moved.low == Approx(0.42));
    CHECK(moved.width() == Approx(0.2));
    const SpanWindow stopped = pan_view({0.7, 0.9}, 5.0, min_width);
    CHECK(stopped.high == Approx(1.0));
    CHECK(stopped.width() == Approx(0.2));
}

TEST_CASE("the view rounds outward to whole bins and maps through them", "[span_view]")
{
    // Rejects rounding inward, which would drop a partly visible bin, and
    // rejects mapping through the unrounded fractions, which would put the
    // axis up to half a bin away from the trace at each edge.
    const ViewBins drawn = view_bins({0.10001, 0.2}, 1000);
    CHECK(drawn.first == 100);
    CHECK(drawn.last == 200);
    CHECK(span_fraction_at(drawn, 1000, 0.0) == Approx(0.1));
    CHECK(span_fraction_at(drawn, 1000, 1.0) == Approx(0.2));

    // Exact bin edges do not round out by a whole bin on float noise.
    const ViewBins exact = view_bins({0.25, 0.5}, kBins);
    CHECK(exact.first == kBins / 4);
    CHECK(exact.last == kBins / 2);

    // The full view is every bin and the identity mapping.
    const ViewBins all = view_bins({}, kBins);
    CHECK(all.first == 0);
    CHECK(all.last == kBins);
    CHECK(span_fraction_at(all, kBins, 0.37) == Approx(0.37));

    // Never empty.
    const ViewBins tiny = view_bins({0.5, 0.5}, 10);
    CHECK(tiny.count() == 1);
}
