// The 3D waterfall's projection, draw order, mesh and history ring.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <set>
#include <vector>

#include "render/surface3d_math.h"

using Catch::Matchers::WithinAbs;
using namespace revenant::ui;

TEST_CASE("the middle of the box projects to the middle of the item", "[surface3d]")
{
    // Rejects orbiting about a corner, which swings the picture off the item.
    const SurfaceCamera camera(Orbit{}, 800.0F, 400.0F);
    const Projected middle =
        camera.project(Vec3{0.0F, 0.5F * kSurfaceHeight, 0.5F * kSurfaceDepth});
    CHECK_THAT(middle.x, WithinAbs(400.0, 1e-3));
    CHECK_THAT(middle.y, WithinAbs(200.0, 1e-3));
}

TEST_CASE("the newest row is nearer and lower on screen than the oldest", "[surface3d]")
{
    // Rejects a camera behind the box, which would put the history in front
    // of the newest rows.
    const SurfaceCamera camera(Orbit{}, 800.0F, 400.0F);
    const Projected newest = camera.project(Vec3{0.0F, 0.0F, 0.0F});
    const Projected oldest = camera.project(Vec3{0.0F, 0.0F, kSurfaceDepth});
    CHECK(newest.depth < oldest.depth);
    CHECK(newest.y > oldest.y);
}

TEST_CASE("perspective makes the front row wider than the back", "[surface3d]")
{
    // Rejects an orthographic projection, under which nothing recedes.
    const SurfaceCamera camera(Orbit{0.0F, 30.0F}, 800.0F, 400.0F);
    const float front = camera.project(Vec3{1.0F, 0.0F, 0.0F}).x -
                        camera.project(Vec3{-1.0F, 0.0F, 0.0F}).x;
    const float back = camera.project(Vec3{1.0F, 0.0F, kSurfaceDepth}).x -
                       camera.project(Vec3{-1.0F, 0.0F, kSurfaceDepth}).x;
    CHECK(front > back);
    CHECK(back > 0.0F);
}

TEST_CASE("a higher level is drawn higher on screen", "[surface3d]")
{
    const SurfaceCamera camera(Orbit{}, 800.0F, 400.0F);
    const Projected low = camera.project(surface_point(80, 160, 0, 72, 0.0F));
    const Projected high = camera.project(surface_point(80, 160, 0, 72, 1.0F));
    CHECK(high.y < low.y);
}

TEST_CASE("the whole box fits inside the item at the default camera", "[surface3d]")
{
    const SurfaceCamera camera(Orbit{}, 800.0F, 400.0F);
    for (const float x : {-1.0F, 1.0F}) {
        for (const float y : {0.0F, kSurfaceHeight}) {
            for (const float z : {0.0F, kSurfaceDepth}) {
                const Projected p = camera.project(Vec3{x, y, z});
                CHECK(p.x >= 0.0F);
                CHECK(p.x <= 800.0F);
                CHECK(p.y >= 0.0F);
                CHECK(p.y <= 400.0F);
            }
        }
    }
}

TEST_CASE("the orbit is held inside the painter's limits", "[surface3d]")
{
    // Rejects an unlimited orbit, past which back-to-front by rows is wrong.
    const Orbit wild = Orbit{}.dragged(10000.0F, -10000.0F);
    CHECK(wild.yaw_degrees == kMaxYawDegrees);
    CHECK(wild.pitch_degrees == kMinPitchDegrees);
}

TEST_CASE("the draw order walks from the far side", "[surface3d]")
{
    const DrawOrder left = draw_order(SurfaceCamera(Orbit{-40.0F, 30.0F}, 800.0F, 400.0F));
    const DrawOrder right = draw_order(SurfaceCamera(Orbit{40.0F, 30.0F}, 800.0F, 400.0F));
    CHECK(left.oldest_first);
    CHECK(right.oldest_first);
    // Turning the other way swaps which frequency edge is further.
    CHECK(left.high_first != right.high_first);
}

TEST_CASE("grid indices cover every cell once with two triangles", "[surface3d]")
{
    const int columns = 4;
    const int rows = 3;
    const std::vector<std::uint16_t> indices =
        grid_indices(columns, rows, DrawOrder{true, false});
    REQUIRE(indices.size() == static_cast<std::size_t>((columns - 1) * (rows - 1) * 6));
    std::set<std::uint16_t> used(indices.begin(), indices.end());
    CHECK(used.size() == static_cast<std::size_t>(columns * rows));
    CHECK(*used.rbegin() == columns * rows - 1);

    // Oldest first: the first cell drawn is in the last row pair.
    CHECK(indices.front() == static_cast<std::uint16_t>((rows - 2) * columns));
    CHECK(grid_indices(1, 5, DrawOrder{}).empty());
}

TEST_CASE("the history reads newest first and wraps", "[surface3d]")
{
    // Rejects a ring read in write order, which would put the oldest row at
    // the front of the surface.
    SurfaceHistory history(2, 3);
    CHECK(history.filled() == 0);
    CHECK(history.row(0)[0] == SurfaceHistory::kEmpty);
    for (int i = 1; i <= 4; ++i) {
        const float v = static_cast<float>(i);
        history.push(std::vector<float>{v, v});
    }
    CHECK(history.filled() == 3);
    CHECK(history.row(0)[0] == 4.0F);
    CHECK(history.row(1)[0] == 3.0F);
    CHECK(history.row(2)[1] == 2.0F);

    history.push(std::vector<float>{9.0F});
    CHECK(history.row(0)[0] == 4.0F);

    history.clear();
    CHECK(history.filled() == 0);
    CHECK(history.row(0)[0] == SurfaceHistory::kEmpty);
}
