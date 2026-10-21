// Time markers down the left edge of both waterfalls: a tick and a clock time
// every N seconds, so a row can be read as "about when" without counting.
//
// The time is the wall clock when the row arrived, not the source's sample
// time. A live source has no other clock the client knows, and a recording
// carries no start time over the wire. A recording replayed faster than real
// time therefore gets markers closer together than N seconds of its own; the
// plan for this feature dropped replay, so that is the honest reading.
//
// A marker sits on the first row at or after each multiple of the interval,
// so the ticks land on round clock times (12:00:10, 12:00:20) rather than N
// seconds after whenever the display happened to start.
//
// This header holds no Qt; ui/tests links it.

#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "render/history_resize.h"

namespace revenant::ui {

inline constexpr int kDefaultTimeMarkerSeconds = 10;

// The multiple of the interval crossed between two rows' times, which is the
// marker the later row carries, or 0 for none. A first row (previous 0) never
// carries one: there is nothing above it to have crossed from.
[[nodiscard]] inline std::int64_t marker_between(std::int64_t previous_ms, std::int64_t now_ms,
                                                 int interval_s)
{
    if (interval_s <= 0 || previous_ms <= 0 || now_ms <= previous_ms) {
        return 0;
    }
    const std::int64_t period = static_cast<std::int64_t>(interval_s) * 1000;
    const std::int64_t boundary = (now_ms / period) * period;
    return boundary > previous_ms ? boundary : 0;
}

// The marker each ring row carries, in the same order as the history's rows.
class TimeMarkerRing {
public:
    void assign(int rows)
    {
        marks_.assign(static_cast<std::size_t>(rows > 0 ? rows : 0), 0);
        last_ms_ = 0;
    }

    void clear()
    {
        std::fill(marks_.begin(), marks_.end(), 0);
        last_ms_ = 0;
    }

    // Records the row written at ring index `row` and returns its marker.
    std::int64_t write(int row, std::int64_t now_ms, int interval_s)
    {
        const std::int64_t mark = marker_between(last_ms_, now_ms, interval_s);
        last_ms_ = now_ms;
        if (row >= 0 && static_cast<std::size_t>(row) < marks_.size()) {
            marks_[static_cast<std::size_t>(row)] = mark;
        }
        return mark;
    }

    // Carries the marks across a height change, as the history's rows are.
    void remap(const HistoryRemap& plan, int old_rows, int new_rows)
    {
        std::vector<std::int64_t> fresh(static_cast<std::size_t>(new_rows > 0 ? new_rows : 0), 0);
        for (int row = 0; row < plan.rows_kept && row < new_rows; ++row) {
            const int from = history_source_row(plan, old_rows, row);
            if (from >= 0 && static_cast<std::size_t>(from) < marks_.size()) {
                fresh[static_cast<std::size_t>(row)] = marks_[static_cast<std::size_t>(from)];
            }
        }
        marks_ = std::move(fresh);
    }

    [[nodiscard]] std::size_t size() const { return marks_.size(); }

    // Every marked row as (display row from the top, marker ms), given the
    // ring index of the newest row.
    [[nodiscard]] std::vector<std::pair<int, std::int64_t>> visible(int read_row) const
    {
        std::vector<std::pair<int, std::int64_t>> out;
        const auto tall = static_cast<int>(marks_.size());
        for (int display = 0; display < tall; ++display) {
            const std::int64_t mark = marks_[static_cast<std::size_t>((read_row + display) % tall)];
            if (mark != 0) {
                out.emplace_back(display, mark);
            }
        }
        return out;
    }

private:
    std::vector<std::int64_t> marks_;
    std::int64_t last_ms_ = 0;
};

}  // namespace revenant::ui
