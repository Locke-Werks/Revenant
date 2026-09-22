// plan_receiver_marker, which decides where the receiver's passband is drawn
// on the span displays and when it is not drawn at all.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS, in its own
// comment, for the reason test_audio_ring.cpp gives: a test that only asserts
// what the code already does certifies one reachable shape and reads as
// though it certified the behaviour.
//
// The wrong implementations worth rejecting are all variations on being
// plausible. A highlight placed at the receiver's baseband offset instead of
// its absolute frequency lands somewhere on the display and looks like a
// mark. A band stretched to fit the item covers the whole span and looks like
// a wide filter. An off-span edge pinned to the boundary looks like a filter
// that ends at the edge of the band. None of the three is visibly broken on
// screen, which is why they are asserted here rather than looked at.
//
// WHAT IS NOT HERE. The colours, the quads and which node they go in.
// build_receiver_quads lives in a file that includes QColor, and what it
// decides is how faint a fill is, which is a judgement to be looked at rather
// than asserted.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "models/receiver_marker.h"

using Catch::Approx;
using revenant::ui::plan_receiver_marker;
using revenant::ui::ReceiverBand;
using revenant::ui::ReceiverMarker;

namespace {

// The shipped geometry, near enough: a 2.4 MHz span centred on 98.1 MHz, on a
// display 1200 logical pixels wide. One pixel is 2 kHz.
constexpr double kSpanLow = 96'900'000.0;
constexpr double kSpanHigh = 99'300'000.0;
constexpr double kWidth = 1200.0;

// A receiver at this absolute frequency with this signed passband pair, which
// is how EngineLink carries one: the pair is hertz from the centre, and the
// centre is what turns it into a band plan frequency.
[[nodiscard]] ReceiverBand band_at(double center_hz, double low_hz, double high_hz)
{
    return ReceiverBand{center_hz + low_hz, center_hz + high_hz, center_hz};
}

[[nodiscard]] ReceiverMarker plan(const ReceiverBand& band, double width_px = kWidth)
{
    return plan_receiver_marker(band, kSpanLow, kSpanHigh, width_px);
}

}  // namespace

// Rejects a marker placed from the receiver's BASEBAND centre, which is what
// rpc::VrxParams carries and what a caller reaching for the nearest number
// would pass. At the span's own centre the two differ by exactly the source
// centre, so a baseband placement puts the mark 49050 pixels off the left
// edge of a 1200 pixel display and the marker reports nothing to draw. The
// failure then reads as "no receiver" rather than as a units mistake.
TEST_CASE("a receiver at the middle of the span marks the middle of the item")
{
    // 98.1 MHz is the midpoint of the span above.
    const ReceiverMarker marker = plan(band_at(98'100'000.0, -8'000.0, 8'000.0));

    REQUIRE(marker.visible);
    CHECK(marker.center_px == Approx(600.0));

    // 8 kHz is four pixels at 2 kHz a pixel.
    CHECK(marker.low_edge_px == Approx(596.0));
    CHECK(marker.high_edge_px == Approx(604.0));
    CHECK(marker.fill_left_px == Approx(596.0));
    CHECK(marker.fill_right_px == Approx(604.0));
    CHECK(marker.low_edge_visible);
    CHECK(marker.high_edge_visible);
    CHECK(marker.center_visible);
}

// Rejects a marker built from the width the mode asked for rather than the
// band handed in. The caller is what reads the granted pair off the
// placement, so what this checks is that the function marks the band it was
// given and does not symmetrise it around the centre: a 71 kHz grant on a
// 200 kHz request is the case models/receiver_match.h exists for, and a
// highlight that drew 200 kHz would contradict the sentence under it.
TEST_CASE("the marked region is the band handed in and not a width around the centre")
{
    // Clamped low, as the engine's per-edge fit produces: 40 kHz below the
    // carrier and 12 kHz above it.
    const ReceiverMarker marker = plan(band_at(98'100'000.0, -40'000.0, 12'000.0));

    REQUIRE(marker.visible);
    CHECK(marker.low_edge_px == Approx(580.0));
    CHECK(marker.high_edge_px == Approx(606.0));

    // The tuned frequency is inside the band and is not its midpoint, which
    // is the whole reason the centre is carried separately.
    CHECK(marker.center_px == Approx(600.0));
    CHECK(marker.center_px > marker.low_edge_px);
    CHECK(marker.center_px < marker.high_edge_px);
}

// Rejects a marker that clamps an off-span band onto the display edge. A
// front-end retune moves the span and leaves the receiver where it was, so
// this is the ordinary state and not an error. A clamped implementation draws
// a filter sitting on the first pixel of the band, which is a signal claim
// about a frequency the receiver is nowhere near.
TEST_CASE("a receiver wholly below the span draws nothing")
{
    const ReceiverMarker marker = plan(band_at(95'000'000.0, -8'000.0, 8'000.0));
    CHECK_FALSE(marker.visible);
}

TEST_CASE("a receiver wholly above the span draws nothing")
{
    const ReceiverMarker marker = plan(band_at(101'000'000.0, -8'000.0, 8'000.0));
    CHECK_FALSE(marker.visible);
}

// Rejects the same clamp in the case that is harder to see: the band
// overlaps, so something is drawn, and the edge that is off the display is
// the one that must not be drawn. An implementation that clipped the rules
// along with the fill would put the low edge rule at x = 0, which reads as a
// filter that ends exactly at the bottom of the span.
TEST_CASE("a band running off the low edge is clipped and loses only that rule")
{
    // Centred 4 kHz above the span's low end with a 16 kHz filter, so the low
    // edge is 4 kHz below the span and the high edge is 12 kHz inside it.
    const ReceiverMarker marker = plan(band_at(kSpanLow + 4'000.0, -8'000.0, 8'000.0));

    REQUIRE(marker.visible);
    CHECK(marker.fill_left_px == Approx(0.0));
    CHECK(marker.fill_right_px == Approx(6.0));

    // Unclipped, so the caller can see how far off it is, and flagged as not
    // to be drawn.
    CHECK(marker.low_edge_px == Approx(-2.0));
    CHECK_FALSE(marker.low_edge_visible);
    CHECK(marker.high_edge_visible);
    CHECK(marker.center_visible);
}

TEST_CASE("a band running off the high edge is clipped and loses only that rule")
{
    const ReceiverMarker marker = plan(band_at(kSpanHigh - 4'000.0, -8'000.0, 8'000.0));

    REQUIRE(marker.visible);
    CHECK(marker.fill_left_px == Approx(kWidth - 6.0));
    CHECK(marker.fill_right_px == Approx(kWidth));
    CHECK(marker.high_edge_px == Approx(kWidth + 2.0));
    CHECK_FALSE(marker.high_edge_visible);
    CHECK(marker.low_edge_visible);
}

// Rejects the tuned frequency being drawn whenever the band is. A filter can
// be entirely to one side of the carrier, which is what CW and SSB do, so a
// band that overlaps the span does not mean the carrier does.
TEST_CASE("an off-span carrier is not marked even when its passband is on screen")
{
    // A USB receiver on the span's low edge: 300 to 2700 Hz above a carrier
    // that is itself 1 kHz below the bottom of the span.
    const ReceiverMarker marker = plan(band_at(kSpanLow - 1'000.0, 300.0, 2'700.0));

    REQUIRE(marker.visible);
    CHECK(marker.high_edge_visible);
    CHECK(marker.center_px == Approx(-0.5));
    CHECK_FALSE(marker.center_visible);
}

// Rejects a marker that stretches to fit. A receiver wider than the span is
// reachable on a narrow grid, and the answer is a fill across the whole item
// with neither edge drawn, which says "wider than everything you can see".
// Two rules at the display's edges would say the opposite: that the filter
// ends there.
TEST_CASE("a band wider than the span fills the item and draws no edge")
{
    const ReceiverMarker marker = plan(band_at(98'100'000.0, -2'000'000.0, 2'000'000.0));

    REQUIRE(marker.visible);
    CHECK(marker.fill_left_px == Approx(0.0));
    CHECK(marker.fill_right_px == Approx(kWidth));
    CHECK_FALSE(marker.low_edge_visible);
    CHECK_FALSE(marker.high_edge_visible);
    CHECK(marker.center_visible);
}

// Rejects drawing a receiver whose passband is a pair of zeros. That is the
// state between a tune and the engine's answer: the client sends an empty
// passband to ask for the mode's own default, so until the status arrives the
// only pair it holds is zeros. An implementation that drew it would put a
// zero-width filter on the tuned frequency, which is a filter nobody asked
// for rather than a filter not yet known.
TEST_CASE("a band with no width draws nothing")
{
    CHECK_FALSE(plan(band_at(98'100'000.0, 0.0, 0.0)).visible);

    // And a pair that arrived crossed, which no engine should produce and
    // which a subtraction elsewhere could.
    CHECK_FALSE(plan(band_at(98'100'000.0, 4'000.0, -4'000.0)).visible);
}

// Rejects dividing by a span or a width that is not there yet. Both happen
// during startup: an item is laid out at zero width before its first frame,
// and the span is zero until an EngineInfo has arrived. A divide there
// produces an infinity that compares as inside the item, so the marker would
// be drawn across the whole display on a link that has said nothing.
TEST_CASE("no span and no width draw nothing")
{
    const ReceiverBand band = band_at(98'100'000.0, -8'000.0, 8'000.0);

    CHECK_FALSE(plan(band, 0.0).visible);
    CHECK_FALSE(plan_receiver_marker(band, kSpanLow, kSpanLow, kWidth).visible);

    // A reversed pair, which is what an axis read from an engine that has not
    // published a geometry can look like.
    CHECK_FALSE(plan_receiver_marker(band, kSpanHigh, kSpanLow, kWidth).visible);
}
