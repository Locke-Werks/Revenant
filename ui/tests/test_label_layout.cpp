// layout_labels: rows, priority and clamping for the span overlay labels.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS, in its own
// comment, for the reason test_audio_ring.cpp gives.

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "render/label_layout.h"

using revenant::ui::LabelCandidate;
using revenant::ui::layout_labels;

// Rejects a layout that draws overlapping plates in one row.
TEST_CASE("overlapping labels stack into rows", "[label_layout]")
{
    const std::vector<LabelCandidate> c{{100.0, 40.0, 1.0}, {110.0, 40.0, 1.0}};
    const auto out = layout_labels(c, 500.0);
    REQUIRE(out.placed.size() == 2);
    CHECK(out.placed[0].row != out.placed[1].row);
    CHECK(out.hidden == 0);
}

// Rejects placing in list order: the stronger label must survive a full row.
TEST_CASE("the lower priority label is hidden when crowded", "[label_layout]")
{
    const std::vector<LabelCandidate> c{{100.0, 40.0, 1.0}, {105.0, 40.0, 9.0}};
    const auto out = layout_labels(c, 500.0, 1);
    REQUIRE(out.placed.size() == 1);
    CHECK(out.placed[0].index == 1);
    CHECK(out.hidden == 1);
}

// Rejects a plate pushed off screen, and a tick moved with the plate.
TEST_CASE("a label at the edge is slid on screen and keeps its anchor", "[label_layout]")
{
    const std::vector<LabelCandidate> c{{2.0, 40.0, 1.0}, {498.0, 40.0, 1.0}};
    const auto out = layout_labels(c, 500.0);
    REQUIRE(out.placed.size() == 2);
    CHECK(out.placed[0].x0 == 0.0);
    CHECK(out.placed[0].anchor_x == 2.0);
    CHECK(out.placed[1].x1 == 500.0);
}

// Rejects counting labels outside the view as crowded out.
TEST_CASE("labels off screen are neither placed nor hidden", "[label_layout]")
{
    const std::vector<LabelCandidate> c{{-10.0, 40.0, 1.0}, {600.0, 40.0, 1.0}};
    const auto out = layout_labels(c, 500.0);
    CHECK(out.placed.empty());
    CHECK(out.hidden == 0);
}

// Rejects a layout that ignores zoom: the same labels spread wider share a row.
TEST_CASE("zooming in spreads labels back into one row", "[label_layout]")
{
    const std::vector<LabelCandidate> narrow{{100.0, 40.0, 1.0}, {120.0, 40.0, 1.0}, {140.0, 40.0, 1.0}};
    const std::vector<LabelCandidate> wide{{100.0, 40.0, 1.0}, {300.0, 40.0, 1.0}, {500.0, 40.0, 1.0}};
    const auto a = layout_labels(narrow, 1000.0, 2);
    const auto b = layout_labels(wide, 1000.0, 2);
    CHECK(a.hidden == 1);
    REQUIRE(b.placed.size() == 3);
    for (const auto& p : b.placed) {
        CHECK(p.row == 0);
    }
}

// Rejects a result that depends on the order candidates were listed in.
TEST_CASE("equal priorities resolve by position, not list order", "[label_layout]")
{
    const std::vector<LabelCandidate> ab{{100.0, 40.0, 1.0}, {110.0, 40.0, 1.0}};
    const std::vector<LabelCandidate> ba{{110.0, 40.0, 1.0}, {100.0, 40.0, 1.0}};
    const auto x = layout_labels(ab, 500.0, 1);
    const auto y = layout_labels(ba, 500.0, 1);
    REQUIRE(x.placed.size() == 1);
    REQUIRE(y.placed.size() == 1);
    CHECK(x.placed[0].anchor_x == y.placed[0].anchor_x);
}
