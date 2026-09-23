// Where the receivers are drawn: the panel's arrangement and the dock's
// height.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows.

#include <catch2/catch_test_macros.hpp>

#include "models/receiver_placement.h"

using revenant::ui::dock_height;
using revenant::ui::kDockMinHeight;
using revenant::ui::kSpanKeepsHeight;
using revenant::ui::receiver_panel_side_by_side;

// Rejects stacking RDS, decoding and audio under the fine-tuning display in
// the docked strip, which leaves the display no height at the main window's
// default size.
TEST_CASE("the docked strip puts RDS, decoding and audio beside the receiver", "[placement]")
{
    // The main window's default, 1280 by 800, with the dock at its default.
    CHECK(receiver_panel_side_by_side(1280.0, 314.0));

    // Maximised on 1920 by 1080.
    CHECK(receiver_panel_side_by_side(1920.0, 420.0));
}

// Rejects the side-by-side arrangement in the popped-out window, whose shape
// is the one the stacked arrangement was designed for.
TEST_CASE("the popped-out window stacks them as it always did", "[placement]")
{
    CHECK_FALSE(receiver_panel_side_by_side(1040.0, 680.0));
    CHECK_FALSE(receiver_panel_side_by_side(1600.0, 1000.0));
}

// Rejects three columns in a strip too narrow for them, however flat it is.
TEST_CASE("a narrow strip stacks too", "[placement]")
{
    CHECK_FALSE(receiver_panel_side_by_side(900.0, 300.0));
    CHECK_FALSE(receiver_panel_side_by_side(1200.0, 0.0));
}

TEST_CASE("the dock starts at a fraction of the height it shares", "[placement]")
{
    // 800 less the top bar.
    CHECK(dock_height(0.0, 748.0) == 0.42 * 748.0);
}

// Rejects a remembered height applied as it was, which on a smaller window
// than the one it was dragged on leaves the span a sliver, or the dock one.
TEST_CASE("a remembered height is kept inside both floors", "[placement]")
{
    CHECK(dock_height(400.0, 748.0) == 400.0);
    CHECK(dock_height(700.0, 748.0) == 748.0 - kSpanKeepsHeight);
    CHECK(dock_height(90.0, 748.0) == kDockMinHeight);

    // A window too short for both: the dock keeps its floor.
    CHECK(dock_height(0.0, 300.0) == kDockMinHeight);
}
