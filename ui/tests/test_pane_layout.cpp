// models/pane_layout.h: the tiled pane tree.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS. The owner's rule is
// that no pane is ever cut off or covered, so most of these are about a pane
// that ends up under its minimum, missing, or doubled.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <numeric>
#include <string>
#include <vector>

#include "models/pane_layout.h"

using Catch::Matchers::WithinAbs;
using namespace revenant::ui;

// Rejects: weights scaled straight to pixels, which squeezes a small-share
// pane under its minimum in a short window.
TEST_CASE("lay_out never puts a child under its minimum", "[pane_layout]")
{
    const std::vector<double> sizes = lay_out({0.9, 0.1}, {100.0, 150.0}, 400.0);
    REQUIRE(sizes[1] >= 150.0);
    REQUIRE(sizes[0] >= 100.0);
    REQUIRE_THAT(sizes[0] + sizes[1] + kPaneHandle, WithinAbs(400.0, 1e-9));
}

// Rejects: handles left out of the length, which overflows the split by
// their width.
TEST_CASE("lay_out shares what the handles leave", "[pane_layout]")
{
    const std::vector<double> sizes = lay_out({1.0, 1.0, 1.0}, {0.0, 0.0, 0.0}, 310.0);
    const double total = std::accumulate(sizes.begin(), sizes.end(), 0.0);
    REQUIRE_THAT(total, WithinAbs(300.0, 1e-9));
    REQUIRE_THAT(sizes[0], WithinAbs(100.0, 1e-9));
}

// Rejects: a short split that shrinks every child proportionally, cutting
// all of them, rather than holding each at its minimum.
TEST_CASE("lay_out below the minimums holds every child at its minimum", "[pane_layout]")
{
    const std::vector<double> sizes = lay_out({1.0, 1.0}, {200.0, 200.0}, 100.0);
    REQUIRE(sizes[0] == 200.0);
    REQUIRE(sizes[1] == 200.0);
}

// Rejects: a drag that trades past the neighbour's minimum.
TEST_CASE("drag_border stops at the neighbour's minimum", "[pane_layout]")
{
    const std::vector<double> after = drag_border({200.0, 200.0}, {50.0, 150.0}, 0, 500.0);
    REQUIRE(after[1] == 150.0);
    REQUIRE(after[0] == 250.0);
    const std::vector<double> back = drag_border({200.0, 200.0}, {50.0, 150.0}, 0, -500.0);
    REQUIRE(back[0] == 50.0);
    REQUIRE(back[1] == 350.0);
}

// Rejects: a minimum that takes the largest child along the axis, or that
// leaves out the handles, either of which lets the window shrink too far.
TEST_CASE("min_size sums along the axis and takes the largest across it", "[pane_layout]")
{
    const PaneMinimums mins = {{"a", {100.0, 50.0}}, {"b", {200.0, 80.0}}, {"c", {60.0, 300.0}}};
    const PaneNode tree = pane_split(
        false, {pane_split(true, {pane_leaf("a"), pane_leaf("b")}, {1.0, 1.0}), pane_leaf("c")},
        {1.0, 1.0});
    const PaneSize size = min_size(tree, mins);
    REQUIRE(size.width == 100.0 + 200.0 + kPaneHandle);
    REQUIRE(size.height == 80.0 + 300.0 + kPaneHandle);
}

// Rejects: a move that copies the pane rather than moving it, or loses the
// target.
TEST_CASE("move_pane keeps every pane exactly once", "[pane_layout]")
{
    PaneNode tree = default_receiver_tree();
    REQUIRE(move_pane(tree, "sections", "detail", PaneEdge::Bottom));
    REQUIRE(holds_exactly(tree, receiver_tree_panes()));
    REQUIRE(tree.horizontal);
    REQUIRE(tree.children.size() == 2);
    const PaneNode& column = tree.children[1];
    REQUIRE_FALSE(column.horizontal);
    REQUIRE(column.children[0].pane == "detail");
    REQUIRE(column.children[1].pane == "sections");
}

// Rejects: a split nested inside another split running the same way, which
// draws a border with nothing to divide on one side.
TEST_CASE("move_pane folds a same-direction split into its parent", "[pane_layout]")
{
    PaneNode tree = default_receiver_tree();
    REQUIRE(move_pane(tree, "rack", "sections", PaneEdge::Right));
    REQUIRE(tree.horizontal);
    REQUIRE(tree.children.size() == 3);
    REQUIRE(tree.children[0].pane == "detail");
    REQUIRE(tree.children[1].pane == "sections");
    REQUIRE(tree.children[2].pane == "rack");
}

// Rejects: the main tree turned sideways by dropping the receivers on the
// span's right, which is what replaced the automatic dock-beside rule.
TEST_CASE("move_pane can put the dock beside the span", "[pane_layout]")
{
    PaneNode tree = default_main_tree();
    REQUIRE(move_pane(tree, "receivers", "span", PaneEdge::Right));
    REQUIRE(tree.horizontal);
    REQUIRE(tree.children[0].pane == "span");
    REQUIRE(tree.children[1].pane == "receivers");
}

// Rejects: a move onto itself or onto a pane that is not there that still
// changes the tree.
TEST_CASE("move_pane refuses a pane onto itself or a missing one", "[pane_layout]")
{
    PaneNode tree = default_receiver_tree();
    const std::string before = write_pane_tree(tree);
    REQUIRE_FALSE(move_pane(tree, "rack", "rack", PaneEdge::Left));
    REQUIRE_FALSE(move_pane(tree, "rack", "nowhere", PaneEdge::Left));
    REQUIRE(write_pane_tree(tree) == before);
}

// Rejects: a settings form that does not read back to the same tree.
TEST_CASE("the settings form round-trips", "[pane_layout]")
{
    PaneNode tree = default_receiver_tree();
    REQUIRE(move_pane(tree, "rack", "detail", PaneEdge::Top));
    const std::string text = write_pane_tree(tree);
    const auto read = read_pane_tree(text);
    REQUIRE(read.has_value());
    REQUIRE(write_pane_tree(*read) == text);
}

// Rejects: a stale or damaged setting taken as the layout, which could leave
// a pane with nowhere to be drawn.
TEST_CASE("a damaged or stale settings form is refused", "[pane_layout]")
{
    REQUIRE_FALSE(read_pane_tree("v(0.5 span").has_value());
    REQUIRE_FALSE(read_pane_tree("v(0.5 span)").has_value());
    REQUIRE_FALSE(read_pane_tree("v(-1 span 1 receivers)").has_value());
    REQUIRE_FALSE(read_pane_tree("v(0.5 span 0.5 receivers) junk").has_value());
    const auto old = read_pane_tree("h(1 rack 1 detail)");
    REQUIRE(old.has_value());
    REQUIRE_FALSE(holds_exactly(*old, receiver_tree_panes()));
}
