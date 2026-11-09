// models/pane_layout.h: the tiled pane tree.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS. The owner's rule is
// that no pane is ever cut off or covered, so most of these are about a pane
// that ends up under its minimum, missing, or doubled.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <numeric>
#include <set>
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
    PaneNode tree = default_main_tree();
    REQUIRE(move_pane(tree, "audio", "detail", PaneEdge::Bottom));
    REQUIRE(holds_exactly(tree, main_tree_panes()));
    REQUIRE(move_pane(tree, "rack", "span", PaneEdge::Left));
    REQUIRE(holds_exactly(tree, main_tree_panes()));
}

// Rejects: a split nested inside another split running the same way, which
// draws a border with nothing to divide on one side.
TEST_CASE("move_pane folds a same-direction split into its parent", "[pane_layout]")
{
    PaneNode tree = pane_split(true, {pane_leaf("a"), pane_leaf("b"), pane_leaf("c")},
                               {1.0, 1.0, 1.0});
    REQUIRE(move_pane(tree, "a", "c", PaneEdge::Right));
    REQUIRE(tree.horizontal);
    REQUIRE(tree.children.size() == 3);
    REQUIRE(tree.children[0].pane == "b");
    REQUIRE(tree.children[1].pane == "c");
    REQUIRE(tree.children[2].pane == "a");
}

// Rejects: receiver panes held inside a block of their own, which was the
// first version: the rack could not go beside the spectrum.
TEST_CASE("move_pane puts a receiver pane beside the spectrum", "[pane_layout]")
{
    PaneNode tree = default_main_tree();
    REQUIRE(move_pane(tree, "rack", "span", PaneEdge::Right));
    const PaneNode& top = tree.children[0];
    REQUIRE(top.horizontal);
    REQUIRE(top.children[0].pane == "span");
    REQUIRE(top.children[1].pane == "rack");
}

// Rejects: a move onto itself or onto a pane that is not there that still
// changes the tree.
TEST_CASE("move_pane refuses a pane onto itself or a missing one", "[pane_layout]")
{
    PaneNode tree = default_main_tree();
    const std::string before = write_pane_tree(tree);
    REQUIRE_FALSE(move_pane(tree, "rack", "rack", PaneEdge::Left));
    REQUIRE_FALSE(move_pane(tree, "rack", "nowhere", PaneEdge::Left));
    REQUIRE(write_pane_tree(tree) == before);
}

// Rejects: a settings form that does not read back to the same tree.
TEST_CASE("the settings form round-trips", "[pane_layout]")
{
    PaneNode tree = default_main_tree();
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

// Rejects: a hidden pane leaving a hole, or a split of one drawn as a split.
TEST_CASE("build_view drops hidden panes and collapses what they leave", "[pane_layout]")
{
    const PaneNode tree = default_main_tree();
    std::set<std::string, std::less<>> hidden;
    for (const std::string& pane : receiver_tree_panes()) {
        hidden.insert(pane);
    }
    const auto view = build_view(tree, hidden);
    REQUIRE(view.has_value());
    REQUIRE(view->leaf());
    REQUIRE(view->pane == "span");

    hidden.insert("span");
    REQUIRE_FALSE(build_view(tree, hidden).has_value());
}

// Rejects: a drag on a view with hidden panes writing its sizes to the wrong
// stored children, or spending the hidden ones' share.
TEST_CASE("store_view_sizes keeps hidden children's weights", "[pane_layout]")
{
    PaneNode tree = pane_split(false, {pane_leaf("a"), pane_leaf("b"), pane_leaf("c")},
                               {0.2, 0.3, 0.5});
    const auto view = build_view(tree, {"b"});
    REQUIRE(view.has_value());
    store_view_sizes(tree, *view, {300.0, 100.0});
    REQUIRE_THAT(tree.weights[1], WithinAbs(0.3, 1e-12));
    REQUIRE_THAT(tree.weights[0], WithinAbs(0.525, 1e-12));
    REQUIRE_THAT(tree.weights[2], WithinAbs(0.175, 1e-12));
}

// Rejects: a collapsed pane given a share like any other, which leaves its
// strip with a blank area under it.
TEST_CASE("a collapsed pane keeps exactly its minimum", "[pane_layout]")
{
    const std::vector<double> sizes =
        lay_out({1.0, 1.0, 1.0}, {20.0, 50.0, 50.0}, 610.0, {true, false, false});
    REQUIRE(sizes[0] == 20.0);
    REQUIRE_THAT(sizes[1], WithinAbs(290.0, 1e-9));
}

// Rejects: a border beside a collapsed pane that cannot be dragged.
TEST_CASE("drag_border skips a collapsed neighbour", "[pane_layout]")
{
    const std::vector<double> after =
        drag_border({200.0, 20.0, 200.0}, {50.0, 20.0, 50.0}, 0, 30.0, {false, true, false});
    REQUIRE(after[0] == 230.0);
    REQUIRE(after[1] == 20.0);
    REQUIRE(after[2] == 170.0);
}

// Rejects: a pane that can only split another pane, never go down the side
// of everything, which is the nesting the owner asked for.
TEST_CASE("move_pane_beside_area puts a pane full height beside the rest", "[pane_layout]")
{
    PaneNode tree = default_main_tree();
    std::vector<std::string> all;
    collect_panes(tree, all);
    REQUIRE(move_pane_beside_area(tree, "rack", {all.begin(), all.end()}, PaneEdge::Left));
    REQUIRE(holds_exactly(tree, main_tree_panes()));
    REQUIRE(tree.horizontal);
    REQUIRE(tree.children.size() == 2);
    REQUIRE(tree.children[0].pane == "rack");
    REQUIRE_FALSE(tree.children[1].leaf());
}
