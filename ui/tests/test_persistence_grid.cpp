// PersistenceGrid: the persistence spectrum's density histogram, its fade and
// its shading.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

#include "render/persistence_grid.h"

using Catch::Matchers::WithinAbs;
using revenant::ui::PersistenceGrid;

TEST_CASE("row_for puts the ceiling on the top row and the floor on the bottom", "[persistence]")
{
    // Rejects a grid laid out bottom up, which would draw the spectrum
    // upside down once it reached the texture.
    CHECK(PersistenceGrid::row_for(-40.0F, -100.0F, -40.0F, 10) == 0);
    CHECK(PersistenceGrid::row_for(-100.0F, -100.0F, -40.0F, 10) == 9);
    CHECK(PersistenceGrid::row_for(-70.0F, -100.0F, -40.0F, 11) == 5);
}

TEST_CASE("row_for holds levels outside the ends on the edge rows", "[persistence]")
{
    // Rejects dropping them, which would hide a signal above a pinned ceiling.
    CHECK(PersistenceGrid::row_for(0.0F, -100.0F, -40.0F, 10) == 0);
    CHECK(PersistenceGrid::row_for(-200.0F, -100.0F, -40.0F, 10) == 9);
    CHECK(PersistenceGrid::row_for(-50.0F, -40.0F, -40.0F, 10) == 9);
}

TEST_CASE("a trace adds one hit per cell it passes through", "[persistence]")
{
    PersistenceGrid grid;
    grid.resize(3, 11);
    const std::vector<float> flat{-70.0F, -70.0F, -70.0F};
    grid.add_trace(flat, -100.0F, -40.0F);
    grid.add_trace(flat, -100.0F, -40.0F);
    for (int c = 0; c < 3; ++c) {
        CHECK(grid.at(c, 5) == 2.0F);
        CHECK(grid.at(c, 4) == 0.0F);
    }
    CHECK(grid.peak() == 2.0F);
}

TEST_CASE("a step between columns is filled, half in each", "[persistence]")
{
    // Rejects counting end points only, which draws a carrier's skirt as two
    // dots.
    PersistenceGrid grid;
    grid.resize(2, 11);
    grid.add_trace(std::vector<float>{-100.0F, -40.0F}, -100.0F, -40.0F);
    CHECK(grid.at(0, 10) == 1.0F);
    for (int r = 0; r <= 5; ++r) {
        CHECK(grid.at(1, r) == 1.0F);
    }
    CHECK(grid.at(1, 6) == 0.0F);
}

TEST_CASE("the fade is exponential in seconds and keeps the peak exact", "[persistence]")
{
    PersistenceGrid grid;
    grid.resize(1, 4);
    grid.add_trace(std::vector<float>{-40.0F}, -100.0F, -40.0F);
    grid.decay(2.0, 2.0);
    CHECK_THAT(grid.at(0, 0), WithinAbs(std::exp(-1.0), 1e-6));
    CHECK_THAT(grid.peak(), WithinAbs(std::exp(-1.0), 1e-6));

    // Rejects a per-frame factor: two halves fade as much as one whole.
    PersistenceGrid halves;
    halves.resize(1, 4);
    halves.add_trace(std::vector<float>{-40.0F}, -100.0F, -40.0F);
    halves.decay(1.0, 2.0);
    halves.decay(1.0, 2.0);
    CHECK_THAT(halves.at(0, 0), WithinAbs(grid.at(0, 0), 1e-6));
}

TEST_CASE("a steady carrier settles at one over one minus the factor", "[persistence]")
{
    PersistenceGrid grid;
    grid.resize(1, 4);
    const double k = std::exp(-0.1 / 1.0);
    for (int i = 0; i < 400; ++i) {
        grid.decay(0.1, 1.0);
        grid.add_trace(std::vector<float>{-40.0F}, -100.0F, -40.0F);
    }
    CHECK_THAT(grid.at(0, 0), WithinAbs(1.0 / (1.0 - k), 1e-3));
}

TEST_CASE("one hit under a saturated carrier still shades visibly", "[persistence]")
{
    // Rejects a linear shade, under which one hit against a carrier's ten is
    // a tenth and reads as empty against the background.
    const float one = PersistenceGrid::shade(1.0F, 10.0F);
    CHECK(one > 0.25F);
    CHECK(PersistenceGrid::shade(10.0F, 10.0F) == 1.0F);
    CHECK(PersistenceGrid::shade(0.0F, 10.0F) == 0.0F);
    CHECK(PersistenceGrid::shade(1.0F, 0.0F) == 0.0F);
}

TEST_CASE("a trace of the wrong width is ignored", "[persistence]")
{
    PersistenceGrid grid;
    grid.resize(4, 4);
    grid.add_trace(std::vector<float>{-40.0F, -40.0F}, -100.0F, -40.0F);
    CHECK(grid.peak() == 0.0F);
}

TEST_CASE("the decay setting is held to its range", "[persistence]")
{
    using revenant::ui::clamp_persistence_decay;
    CHECK(clamp_persistence_decay(0.0) == revenant::ui::kMinPersistenceDecaySeconds);
    CHECK(clamp_persistence_decay(1e6) == revenant::ui::kMaxPersistenceDecaySeconds);
    CHECK(clamp_persistence_decay(std::nan("")) ==
          revenant::ui::kDefaultPersistenceDecaySeconds);
}
