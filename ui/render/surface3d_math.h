// The 3D waterfall's geometry: the recent history as a surface over
// frequency and time, height the level, seen through a perspective camera
// that orbits the middle of the box.
//
// THE WORLD. Frequency runs along x from -1 at the low edge to +1 at the
// high. Time runs along z from the newest row at the front, nearest the
// camera, to the oldest at the back, kSurfaceDepth away. Level runs up y from
// 0 at the map's floor to kSurfaceHeight at its ceiling. The box is centred
// on the origin before it is turned, so orbiting turns it about its own
// middle and the picture does not swing off the item.
//
// WHY THE TRIANGLES ARE SORTED AND NOT DEPTH TESTED. The scene graph draws a
// blended node in the order its indices say, with no depth buffer of its
// own, so the surface is drawn back to front: the painter's rule. A height
// field seen from above its base and within a quarter turn either side is
// drawn correctly that way row by row, because rows and columns are each
// monotonic in depth. The orbit is held inside those limits for that reason.
//
// WHY THE PROJECTION IS DONE HERE AND NOT IN A SHADER. The mesh is coarse,
// kSurfaceColumns by kSurfaceRows, so projecting it is a few thousand small
// sums a frame, well under the cost of the tile upload the flat waterfall
// does. A custom shader would add a shader toolchain step to a client that has
// none, for no measurable gain at this size.
//
// Pure arithmetic and std types, so ui/tests covers it without a window.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

namespace revenant::ui {

inline constexpr int kSurfaceColumns = 160;
inline constexpr int kSurfaceRows = 72;
inline constexpr float kSurfaceDepth = 1.8F;
inline constexpr float kSurfaceHeight = 0.55F;

// The camera sits this far from the middle of the box, in box units. Far
// enough that the front row is not much larger than the back, near enough
// that the recession reads as depth.
inline constexpr float kCameraDistance = 4.2F;

// The pleasing default: a little from the left and well above, so the
// newest rows face the operator and the history recedes up the item.
inline constexpr float kDefaultYawDegrees = -22.0F;
inline constexpr float kDefaultPitchDegrees = 34.0F;

inline constexpr float kMinYawDegrees = -70.0F;
inline constexpr float kMaxYawDegrees = 70.0F;
inline constexpr float kMinPitchDegrees = 10.0F;
inline constexpr float kMaxPitchDegrees = 80.0F;

// Degrees of orbit per pixel dragged.
inline constexpr float kOrbitDegreesPerPixel = 0.35F;

struct Orbit {
    float yaw_degrees = kDefaultYawDegrees;
    float pitch_degrees = kDefaultPitchDegrees;

    // Within the limits the painter's rule needs; see the top of this file.
    [[nodiscard]] Orbit clamped() const
    {
        return Orbit{std::clamp(yaw_degrees, kMinYawDegrees, kMaxYawDegrees),
                     std::clamp(pitch_degrees, kMinPitchDegrees, kMaxPitchDegrees)};
    }

    [[nodiscard]] Orbit dragged(float dx_pixels, float dy_pixels) const
    {
        return Orbit{yaw_degrees + dx_pixels * kOrbitDegreesPerPixel,
                     pitch_degrees + dy_pixels * kOrbitDegreesPerPixel}
            .clamped();
    }
};

struct Vec3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

struct Projected {
    float x = 0.0F;
    float y = 0.0F;
    // Distance along the view direction; larger is further away.
    float depth = 0.0F;
};

// World to screen for one orbit and one item size, set up once per frame so a
// vertex costs a few multiplies.
class SurfaceCamera {
public:
    SurfaceCamera(const Orbit& orbit, float width, float height)
        : centre_x_(width * 0.5F),
          centre_y_(height * 0.5F)
    {
        const Orbit o = orbit.clamped();
        constexpr float kRadians = std::numbers::pi_v<float> / 180.0F;
        cos_yaw_ = std::cos(o.yaw_degrees * kRadians);
        sin_yaw_ = std::sin(o.yaw_degrees * kRadians);
        cos_pitch_ = std::cos(o.pitch_degrees * kRadians);
        sin_pitch_ = std::sin(o.pitch_degrees * kRadians);

        // The box's half diagonal on the ground is what has to fit across
        // the width, and its depth, foreshortened, down the height.
        const float half_diagonal = std::sqrt(1.0F + 0.25F * kSurfaceDepth * kSurfaceDepth);
        scale_ = 0.92F * std::min(width * 0.5F / half_diagonal, height * 0.5F / half_diagonal);
    }

    // A point in world units, as described at the top of this file.
    [[nodiscard]] Projected project(const Vec3& world) const
    {
        // Centre the box on the origin.
        const float x = world.x;
        const float y = world.y - 0.5F * kSurfaceHeight;
        const float z = world.z - 0.5F * kSurfaceDepth;

        // Yaw about the vertical, then pitch about the horizontal: the
        // camera looks down the +z axis from in front, so a positive pitch
        // raises it and tips the far rows up the screen.
        const float xr = cos_yaw_ * x + sin_yaw_ * z;
        const float zy = -sin_yaw_ * x + cos_yaw_ * z;
        const float yr = cos_pitch_ * y + sin_pitch_ * zy;
        const float zr = -sin_pitch_ * y + cos_pitch_ * zy;

        const float depth = kCameraDistance + zr;
        const float s = kCameraDistance / std::max(depth, 0.05F);
        return Projected{centre_x_ + scale_ * s * xr, centre_y_ - scale_ * s * yr, depth};
    }

private:
    float centre_x_;
    float centre_y_;
    float scale_ = 1.0F;
    float cos_yaw_ = 1.0F;
    float sin_yaw_ = 0.0F;
    float cos_pitch_ = 1.0F;
    float sin_pitch_ = 0.0F;
};

// Where grid cell (column, age) sits in the world, before the level lifts it.
// Age 0 is the newest row.
[[nodiscard]] inline Vec3 surface_point(int column, int columns, int age, int rows,
                                        float level01)
{
    const float x = columns > 1 ? -1.0F + 2.0F * static_cast<float>(column) /
                                              static_cast<float>(columns - 1)
                                : 0.0F;
    const float z = rows > 1 ? kSurfaceDepth * static_cast<float>(age) /
                                   static_cast<float>(rows - 1)
                             : 0.0F;
    return Vec3{x, std::clamp(level01, 0.0F, 1.0F) * kSurfaceHeight, z};
}

// Which way to walk the grid so the far quads are drawn first. Rows are
// walked oldest first unless the newest are further away, which the orbit
// limits rule out but which costs nothing to answer honestly; columns are
// walked from whichever edge is further.
struct DrawOrder {
    bool oldest_first = true;
    bool high_first = false;
};

[[nodiscard]] inline DrawOrder draw_order(const SurfaceCamera& camera)
{
    const float newest = camera.project(Vec3{0.0F, 0.0F, 0.0F}).depth;
    const float oldest = camera.project(Vec3{0.0F, 0.0F, kSurfaceDepth}).depth;
    const float low = camera.project(Vec3{-1.0F, 0.0F, 0.5F * kSurfaceDepth}).depth;
    const float high = camera.project(Vec3{1.0F, 0.0F, 0.5F * kSurfaceDepth}).depth;
    return DrawOrder{oldest >= newest, high > low};
}

// Triangle indices over a grid of `columns` by `rows` vertices laid out row
// by row, vertex (c, a) at a * columns + c, two triangles per cell, in the
// order `order` says. Rebuilt only when the order flips, which is a drag
// across the middle, not per frame.
[[nodiscard]] inline std::vector<std::uint16_t> grid_indices(int columns, int rows,
                                                             const DrawOrder& order)
{
    std::vector<std::uint16_t> indices;
    if (columns < 2 || rows < 2) {
        return indices;
    }
    indices.reserve(static_cast<std::size_t>(columns - 1) * static_cast<std::size_t>(rows - 1) *
                    6U);
    for (int step_a = 0; step_a < rows - 1; ++step_a) {
        const int a = order.oldest_first ? rows - 2 - step_a : step_a;
        for (int step_c = 0; step_c < columns - 1; ++step_c) {
            const int c = order.high_first ? columns - 2 - step_c : step_c;
            const auto v00 = static_cast<std::uint16_t>(a * columns + c);
            const auto v01 = static_cast<std::uint16_t>(a * columns + c + 1);
            const auto v10 = static_cast<std::uint16_t>((a + 1) * columns + c);
            const auto v11 = static_cast<std::uint16_t>((a + 1) * columns + c + 1);
            indices.insert(indices.end(), {v00, v10, v01, v01, v10, v11});
        }
    }
    return indices;
}

// The surface's history: kSurfaceRows rows of kSurfaceColumns levels in dBFS,
// newest first when read through row(). A ring, so a frame writes one row and
// moves a cursor rather than shifting the rest.
class SurfaceHistory {
public:
    explicit SurfaceHistory(int columns = kSurfaceColumns, int rows = kSurfaceRows)
        : columns_(columns),
          rows_(rows),
          levels_(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows), kEmpty)
    {
    }

    // Below any real level, so an empty row draws as the floor.
    static constexpr float kEmpty = -1000.0F;

    [[nodiscard]] int columns() const { return columns_; }
    [[nodiscard]] int rows() const { return rows_; }
    [[nodiscard]] int filled() const { return filled_; }

    void clear()
    {
        std::fill(levels_.begin(), levels_.end(), kEmpty);
        newest_ = 0;
        filled_ = 0;
    }

    // A row whose width is not the history's is ignored.
    void push(std::span<const float> row_db)
    {
        if (static_cast<int>(row_db.size()) != columns_ || rows_ <= 0) {
            return;
        }
        newest_ = (newest_ + rows_ - 1) % rows_;
        std::copy(row_db.begin(), row_db.end(),
                  levels_.begin() + static_cast<std::ptrdiff_t>(newest_) * columns_);
        filled_ = std::min(filled_ + 1, rows_);
    }

    // The row `age` frames old; age 0 is the newest.
    [[nodiscard]] std::span<const float> row(int age) const
    {
        const int at = (newest_ + age) % rows_;
        return std::span<const float>(levels_).subspan(
            static_cast<std::size_t>(at) * static_cast<std::size_t>(columns_),
            static_cast<std::size_t>(columns_));
    }

private:
    int columns_;
    int rows_;
    std::vector<float> levels_;
    int newest_ = 0;
    int filled_ = 0;
};

}  // namespace revenant::ui
