// The phosphor persistence spectrum's accumulator: a density histogram of the
// span's trace, x frequency and y level, that every frame adds its trace to
// and that fades between frames.
//
// WHAT IT IS FOR. A steady carrier lands in the same cell frame after frame
// and saturates; a burst that keyed for a tenth of a second lands a handful
// of times and leaves a faint ghost that lingers for the decay time. The
// instantaneous trace shows the burst for one frame, the waterfall shows it
// as one row, and both are easy to miss under a carrier at the same
// frequency. A density picture keeps both, at different brightnesses.
//
// WHY THE FADE IS EXPONENTIAL AND IN SECONDS. A phosphor decays that way, and
// a time constant in seconds means the same picture at any frame rate: the
// per-frame factor is exp(-dt / tau) for the source time the frame covered.
//
// WHY A CELL GETS FILLED BETWEEN COLUMNS. A trace that drops forty decibels
// from one column to the next is a line on the spectrum, and a histogram that
// only counted the end points would draw it as two dots. Each column fills
// the rows between its level and its left neighbour's, which is what makes a
// carrier's skirt read as a skirt.
//
// Pure arithmetic and std types, so ui/tests covers it without a window.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace revenant::ui {

// Rows of level resolution. A texture this tall stretched over the display
// height is a few screen pixels per row, which is finer than the trace's own
// noise and keeps the per-frame fade and upload small.
inline constexpr int kPersistenceRows = 160;

// The widest the grid gets, in columns. Past this the texture is stretched,
// which a density picture tolerates and the per-frame cost appreciates.
inline constexpr int kPersistenceMaxColumns = 1280;

inline constexpr double kDefaultPersistenceDecaySeconds = 2.0;
inline constexpr double kMinPersistenceDecaySeconds = 0.25;
inline constexpr double kMaxPersistenceDecaySeconds = 30.0;

[[nodiscard]] inline double clamp_persistence_decay(double seconds)
{
    return seconds == seconds
               ? std::clamp(seconds, kMinPersistenceDecaySeconds, kMaxPersistenceDecaySeconds)
               : kDefaultPersistenceDecaySeconds;
}

class PersistenceGrid {
public:
    void resize(int columns, int rows)
    {
        columns_ = std::max(columns, 0);
        rows_ = std::max(rows, 0);
        cells_.assign(static_cast<std::size_t>(columns_) * static_cast<std::size_t>(rows_), 0.0F);
        peak_ = 0.0F;
    }

    void clear()
    {
        std::fill(cells_.begin(), cells_.end(), 0.0F);
        peak_ = 0.0F;
    }

    [[nodiscard]] int columns() const { return columns_; }
    [[nodiscard]] int rows() const { return rows_; }

    // Row 0 is the TOP of the display, the ceiling, the way the texture is
    // laid out. A level outside the ends is held on the edge row rather than
    // dropped, so a signal above a pinned ceiling still shows as being there.
    [[nodiscard]] static int row_for(float db, float floor_db, float ceiling_db, int rows)
    {
        if (rows <= 0) {
            return 0;
        }
        const float span = ceiling_db - floor_db;
        if (!(span > 0.0F) || !(db == db)) {
            return rows - 1;
        }
        const float t = std::clamp((db - floor_db) / span, 0.0F, 1.0F);
        const int up = static_cast<int>(std::lround(t * static_cast<float>(rows - 1)));
        return rows - 1 - up;
    }

    // Fades every cell by the time constant over `seconds` of source time.
    void decay(double seconds, double tau_seconds)
    {
        if (!(seconds > 0.0) || !(tau_seconds > 0.0)) {
            return;
        }
        const auto k = static_cast<float>(std::exp(-seconds / tau_seconds));
        for (float& cell : cells_) {
            cell *= k;
        }
        // Every cell scaled by the same factor, so the largest stays the
        // largest and the peak is exact without a scan.
        peak_ *= k;
    }

    // Adds one hit per cell the trace passes through. A trace whose width is
    // not the grid's is ignored: the caller resizes on a width change.
    void add_trace(std::span<const float> columns_db, float floor_db, float ceiling_db)
    {
        if (static_cast<int>(columns_db.size()) != columns_ || rows_ <= 0) {
            return;
        }
        int previous = -1;
        for (int c = 0; c < columns_; ++c) {
            const int row =
                row_for(columns_db[static_cast<std::size_t>(c)], floor_db, ceiling_db, rows_);
            int from = row;
            int to = row;
            if (previous >= 0 && previous != row) {
                // Half the jump belongs to each column, so a step is drawn
                // as a vertical edge between them rather than inside one.
                const int middle = (previous + row) / 2;
                from = std::min(row, middle);
                to = std::max(row, middle);
            }
            for (int r = from; r <= to; ++r) {
                float& cell = cells_[index(c, r)];
                cell += 1.0F;
                peak_ = std::max(peak_, cell);
            }
            previous = row;
        }
    }

    [[nodiscard]] float at(int column, int row) const { return cells_[index(column, row)]; }
    [[nodiscard]] float peak() const { return peak_; }

    // A cell's brightness, 0 to 1, against the brightest cell. Logarithmic,
    // because a carrier's cell holds 1 / (1 - k) hits and a burst's holds a
    // few, and on a straight line the burst would be indistinguishable from
    // empty. One hit against a saturated carrier still reads.
    [[nodiscard]] static float shade(float density, float peak)
    {
        if (!(density > 0.0F) || !(peak > 0.0F)) {
            return 0.0F;
        }
        return std::clamp(std::log1p(density) / std::log1p(peak), 0.0F, 1.0F);
    }

private:
    [[nodiscard]] std::size_t index(int column, int row) const
    {
        return static_cast<std::size_t>(row) * static_cast<std::size_t>(columns_) +
               static_cast<std::size_t>(column);
    }

    int columns_ = 0;
    int rows_ = 0;
    std::vector<float> cells_;
    float peak_ = 0.0F;
};

}  // namespace revenant::ui
