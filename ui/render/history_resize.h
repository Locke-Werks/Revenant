// Where the rows of a decrementing row ring land when the ring changes
// height.
//
// WHY THIS IS ITS OWN HEADER AND HOLDS NO Qt
//
// render/waterfall_item.cpp is a QQuickItem, a QImage and a scene graph, and
// none of that can be instantiated in a test binary with no window. The part
// of a height resize that can be WRONG is none of those things: it is which
// ring index a kept row comes from, how many rows survive, and where the
// write cursor has to land so that reading forward from just past it is
// still newest to oldest. That is integer arithmetic over a wrapped index,
// which is exactly the shape of thing that is off by one for a year.
//
// So it lives here, ui/tests links it, and the item does the pixels.
//
// THE RING THIS DESCRIBES
//
// render/waterfall_item.h has the full argument. The short form: the write
// cursor DECREMENTS, the newest row is the one just past it, and reading
// forward from there with the wrap gives newest to oldest with no mirror.
// So the newest row is at (write_row + 1) % rows and walking forward from it
// walks back through time.

#pragma once

#include <algorithm>

namespace revenant::ui {

// What a height change does to the rows already stored.
struct HistoryRemap {
    // How many of the old rows are carried over, newest first. Bounded by
    // what was actually written and by what the new ring can hold.
    int rows_kept = 0;

    // Ring index of the newest OLD row, which is where the copy starts. It
    // then walks forward with the wrap, one step per kept row.
    int read_row = 0;

    // Where the next frame is written in the NEW ring. The kept rows are
    // laid down at indices 0 upwards with the newest at 0, so the cursor
    // has to sit at the last index for readRow() to answer 0.
    int write_row = 0;
};

// The destination index of the i-th kept row is i, so only the source needs
// a function. i counts from the newest.
[[nodiscard]] constexpr int history_source_row(const HistoryRemap& plan, int old_rows,
                                               int index)
{
    return old_rows <= 0 ? 0 : (plan.read_row + index) % old_rows;
}

// old_write_row and old_filled are the ring's own two cursors; new_rows is
// the height it is becoming. A ring that has never been written, or one
// being grown from nothing, comes back with rows_kept zero and a cursor
// identical to the one a fresh allocation would get, so the caller needs no
// special case for the first resize.
//
// This says nothing about WIDTH. A width change means a column covers a
// different run of bins, so the same stored row is a different frequency per
// pixel and nothing may be kept; the caller decides that and does not call
// this.
[[nodiscard]] constexpr HistoryRemap plan_history_resize(int old_rows, int old_write_row,
                                                         int old_filled, int new_rows)
{
    const int tall = std::max(new_rows, 1);

    HistoryRemap plan;

    // The same cursor rebuild() sets, so the empty case below and a fresh
    // allocation agree without either knowing about the other.
    plan.write_row = tall - 1;

    if (old_rows <= 0 || old_filled <= 0) {
        return plan;
    }

    plan.read_row = ((old_write_row + 1) % old_rows + old_rows) % old_rows;

    // Clamped by the old ring as well as by the new one. filled_rows_ is
    // capped at the ring's height by the writer, and the clamp is repeated
    // rather than assumed because this function is the thing that would
    // read past the end if it were ever not true.
    plan.rows_kept = std::min(std::min(old_filled, old_rows), tall);
    return plan;
}

}  // namespace revenant::ui
