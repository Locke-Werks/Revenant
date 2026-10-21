// The arithmetic behind the constellation and eye panel (#17): how a symbol
// scope's values land on a square of pixels, how old a dot looks, and the
// level histogram beside an FSK scatter.
//
// No Qt in here, so ui/tests can assert it without a window.
// render/constellation_item.h and render/eye_item.h draw with it.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include "core/rpc/types.h"

namespace revenant::ui {

// Half the shown range in the scope's own units: a level past the outer
// ideal by one level, or a constellation half again past unit radius, so an
// overdriven point still lands on the plot.
[[nodiscard]] inline double symbol_extent(const rpc::SymbolScope& scope)
{
    if (scope.kind == rpc::SymbolKind::Constellation) {
        return 1.5;
    }
    float outer = 1.0F;
    for (const float v : scope.ideal) {
        outer = std::max(outer, std::abs(v));
    }
    return static_cast<double>(outer) + 1.0;
}

// A value in [-extent, extent] to a pixel along `span_px`, the top or the
// left edge at +extent's opposite: x grows with the value, y shrinks.
[[nodiscard]] inline double symbol_x(double value, double extent, double span_px)
{
    return (0.5 + 0.5 * std::clamp(value / extent, -1.0, 1.0)) * span_px;
}

[[nodiscard]] inline double symbol_y(double value, double extent, double span_px)
{
    return (0.5 - 0.5 * std::clamp(value / extent, -1.0, 1.0)) * span_px;
}

// The newest dot is opaque and the oldest a fifth of it, so a moving cluster
// shows where it is going and a still one reads by density.
[[nodiscard]] inline float symbol_dot_alpha(std::size_t index, std::size_t count)
{
    if (count <= 1) {
        return 1.0F;
    }
    const auto age = static_cast<float>(count - 1 - index) / static_cast<float>(count - 1);
    return 1.0F - 0.8F * age;
}

// The levels' histogram over [-extent, extent], peak at one. A level outside
// lands in the edge bin.
[[nodiscard]] inline std::vector<float> symbol_level_histogram(std::span<const float> levels,
                                                               double extent, std::size_t bins)
{
    std::vector<float> out(bins, 0.0F);
    if (bins == 0 || extent <= 0.0) {
        return out;
    }
    for (const float v : levels) {
        const double at = (static_cast<double>(v) / extent * 0.5 + 0.5) * static_cast<double>(bins);
        const auto bin = static_cast<std::size_t>(
            std::clamp(at, 0.0, static_cast<double>(bins) - 1.0));
        out[bin] += 1.0F;
    }
    const float peak = *std::max_element(out.begin(), out.end());
    if (peak > 0.0F) {
        for (float& v : out) {
            v /= peak;
        }
    }
    return out;
}

// Older eye frames fade by this much each, so persistence reads as a trail
// rather than a smear.
[[nodiscard]] inline float symbol_eye_alpha(std::size_t age, std::size_t persistence)
{
    if (persistence == 0) {
        return 0.0F;
    }
    return std::max(0.0F, 1.0F - static_cast<float>(age) / static_cast<float>(persistence));
}

}  // namespace revenant::ui
