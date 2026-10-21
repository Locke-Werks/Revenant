// Where the span's overlay labels go: one plate per label, stacked into rows
// so no two overlap, with the lowest priority hidden when a row is full.
//
// Qt-free so ui/tests can hold it. The caller measures each plate's width and
// works out the x of the frequency it names; this decides only the rows.
//
// HIGHEST PRIORITY FIRST, THEN LEFT TO RIGHT. Placing in priority order means
// a crowded stretch keeps the labels that matter and drops the rest, rather
// than keeping whichever happened to be leftmost. Ties go to the lower x so
// the result never depends on the order the caller listed them in.
//
// A PLATE IS CENTRED ON ITS FREQUENCY AND SLID TO STAY ON SCREEN. A label for
// a signal at the very edge still names it; its leader tick stays at the
// exact frequency, so a slid plate does not move the claim it makes.

#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <vector>

namespace revenant::ui {

inline constexpr int kLabelLayoutMaxRows = 3;

// Clear space between two plates in one row, so "FT8WSPR" never reads as one.
inline constexpr double kLabelLayoutGapPx = 4.0;

struct LabelCandidate {
    double anchor_x = 0.0;
    double width_px = 0.0;
    double priority = 0.0;
};

struct LabelPlacement {
    // Index into the candidates handed in.
    std::size_t index = 0;
    double x0 = 0.0;
    double x1 = 0.0;
    int row = 0;
    double anchor_x = 0.0;
};

struct LabelLayout {
    // In candidate order, so a model built from it keeps a stable row order.
    std::vector<LabelPlacement> placed;

    // Candidates on screen that found no room.
    std::size_t hidden = 0;
};

[[nodiscard]] inline LabelLayout layout_labels(const std::vector<LabelCandidate>& candidates,
                                               double view_width_px,
                                               int max_rows = kLabelLayoutMaxRows,
                                               double gap_px = kLabelLayoutGapPx)
{
    LabelLayout out;
    if (!(view_width_px > 0.0) || max_rows <= 0) {
        return out;
    }

    std::vector<std::size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (candidates[a].priority != candidates[b].priority) {
            return candidates[a].priority > candidates[b].priority;
        }
        return candidates[a].anchor_x < candidates[b].anchor_x;
    });

    std::vector<std::vector<LabelPlacement>> rows(static_cast<std::size_t>(max_rows));
    for (const std::size_t i : order) {
        const LabelCandidate& c = candidates[i];
        // Off screen is not crowded, so it counts toward nothing.
        if (c.anchor_x < 0.0 || c.anchor_x > view_width_px || !(c.width_px > 0.0)) {
            continue;
        }
        const double w = std::min(c.width_px, view_width_px);
        const double x0 = std::clamp(c.anchor_x - w / 2.0, 0.0, view_width_px - w);
        const double x1 = x0 + w;

        bool placed = false;
        for (std::size_t r = 0; r < rows.size() && !placed; ++r) {
            const bool clear = std::none_of(rows[r].begin(), rows[r].end(),
                                            [&](const LabelPlacement& p) {
                                                return x0 < p.x1 + gap_px && p.x0 < x1 + gap_px;
                                            });
            if (clear) {
                rows[r].push_back(LabelPlacement{i, x0, x1, static_cast<int>(r), c.anchor_x});
                placed = true;
            }
        }
        if (!placed) {
            ++out.hidden;
        }
    }

    for (const auto& row : rows) {
        out.placed.insert(out.placed.end(), row.begin(), row.end());
    }
    std::sort(out.placed.begin(), out.placed.end(),
              [](const LabelPlacement& a, const LabelPlacement& b) { return a.index < b.index; });
    return out;
}

}  // namespace revenant::ui
