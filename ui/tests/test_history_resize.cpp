// plan_history_resize, which is the part of the waterfall's height resize
// that can be off by one.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS, in its own
// comment, for the reason test_audio_ring.cpp gives: a test that only
// asserts what the code already does certifies one reachable shape and
// reads as though it certified the behaviour.
//
// The shape under test is a ring whose write cursor DECREMENTS, so the
// newest row is at (write_row + 1) % rows and walking forward from there
// walks back through time. render/waterfall_item.h has the argument for
// that direction; render/history_resize.h restates the two lines of it this
// arithmetic depends on.
//
// WHAT IS NOT HERE. The pixels. WaterfallItem::resizeRows copies scanlines
// and row spans through this plan, and a QImage in a test binary would
// assert the memcpy rather than the decision. The decision is the index
// arithmetic and it is all below.

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "render/history_resize.h"

using revenant::ui::history_source_row;
using revenant::ui::HistoryRemap;
using revenant::ui::plan_history_resize;

namespace {

// The ring as the item drives it: write at the cursor, then decrement. The
// value stored is an age, so 0 is the newest row written.
struct Ring {
    std::vector<int> rows;
    int write_row = 0;
    int filled = 0;

    explicit Ring(int height) : rows(static_cast<std::size_t>(height), -1)
    {
        write_row = height - 1;
    }

    void write(int value)
    {
        const auto tall = static_cast<int>(rows.size());
        rows[static_cast<std::size_t>(write_row)] = value;
        write_row = (write_row + tall - 1) % tall;
        filled = filled + 1 > tall ? tall : filled + 1;
    }

    // Newest to oldest, which is top to bottom on the display.
    [[nodiscard]] std::vector<int> displayed() const
    {
        const auto tall = static_cast<int>(rows.size());
        const int read = (write_row + 1) % tall;
        std::vector<int> out;
        out.reserve(rows.size());
        for (int i = 0; i < tall; ++i) {
            out.push_back(rows[static_cast<std::size_t>((read + i) % tall)]);
        }
        return out;
    }
};

// What resizeRows does with the plan, on ints instead of scanlines.
Ring apply(const Ring& before, int new_height)
{
    const auto old_tall = static_cast<int>(before.rows.size());
    const HistoryRemap plan =
        plan_history_resize(old_tall, before.write_row, before.filled, new_height);

    Ring after(new_height);
    for (int row = 0; row < plan.rows_kept; ++row) {
        after.rows[static_cast<std::size_t>(row)] =
            before.rows[static_cast<std::size_t>(history_source_row(plan, old_tall, row))];
    }
    after.write_row = plan.write_row;
    after.filled = plan.rows_kept;
    return after;
}

}  // namespace

// REJECTS: a plan that copies from index 0 upwards rather than from the read
// cursor. That is the obvious implementation and it is right only while the
// cursor has not wrapped, so it passes every hand-run of a fresh window and
// scrambles the history of one that has been up a while. Five rows into a
// ring of four is one wrap.
TEST_CASE("a grown ring keeps its rows in order after the cursor has wrapped", "[history]")
{
    Ring before(4);
    for (int age = 4; age >= 0; --age) {
        before.write(age);
    }
    REQUIRE(before.displayed() == std::vector<int>{0, 1, 2, 3});

    const Ring after = apply(before, 6);
    const std::vector<int> shown = after.displayed();

    REQUIRE(after.filled == 4);
    REQUIRE(shown[0] == 0);
    REQUIRE(shown[1] == 1);
    REQUIRE(shown[2] == 2);
    REQUIRE(shown[3] == 3);

    // The rows the taller ring gained are empty and sit BELOW the history,
    // because they are older than everything in it.
    REQUIRE(shown[4] == -1);
    REQUIRE(shown[5] == -1);
}

// REJECTS: a plan that keeps min(old height, new height) rows rather than
// min(filled, new height). A ring that has taken three frames into a height
// of eight holds five rows that were never written, and copying them across
// as though they were history claims the display was full.
TEST_CASE("a partly filled ring carries over only what was written", "[history]")
{
    Ring before(8);
    before.write(2);
    before.write(1);
    before.write(0);

    const Ring after = apply(before, 5);
    REQUIRE(after.filled == 3);

    const std::vector<int> shown = after.displayed();
    REQUIRE(shown[0] == 0);
    REQUIRE(shown[1] == 1);
    REQUIRE(shown[2] == 2);
    REQUIRE(shown[3] == -1);
    REQUIRE(shown[4] == -1);
}

// REJECTS: a shrink that keeps the OLDEST rows, which is what copying the
// first new_height entries of the old ring does when the cursor has wrapped.
// The whole point of this display is that the top is now.
//
// Seven frames into a ring of six on purpose. A ring written exactly as many
// times as it is tall has its read cursor back at index zero, so it cannot
// tell the two implementations apart; one more write moves it and can.
TEST_CASE("a shrunk ring drops the oldest rows and keeps the newest", "[history]")
{
    Ring before(6);
    for (int age = 6; age >= 0; --age) {
        before.write(age);
    }
    REQUIRE(before.displayed() == std::vector<int>{0, 1, 2, 3, 4, 5});

    const Ring after = apply(before, 3);
    REQUIRE(after.filled == 3);
    REQUIRE(after.displayed() == std::vector<int>{0, 1, 2});
}

// REJECTS: a write cursor left wherever the old ring had it. The next frame
// has to land at the index that puts it at the TOP of the display, and
// after a resize the kept rows start at index zero, so the only cursor that
// works is the last index. A plan that carried write_row across would write
// the next frame into the middle of the history.
TEST_CASE("the next frame after a resize lands at the top", "[history]")
{
    Ring before(4);
    for (int age = 4; age >= 0; --age) {
        before.write(age);
    }

    Ring after = apply(before, 6);
    after.write(-2);

    const std::vector<int> shown = after.displayed();
    REQUIRE(shown[0] == -2);
    REQUIRE(shown[1] == 0);
    REQUIRE(shown[2] == 1);
    REQUIRE(shown[3] == 2);
    REQUIRE(shown[4] == 3);
    REQUIRE(after.filled == 5);
}

// REJECTS: a plan that keeps a row when the ring can hold exactly as many as
// it already has, and then lets the next write land on top of the NEWEST one
// instead of the oldest. Same height in and out is the case a resize handler
// can reach through a device pixel ratio change, and it must be a no-op
// followed by an ordinary write.
TEST_CASE("a resize to the same height leaves the ring where it was", "[history]")
{
    // Six writes into a height of four, so the cursor is neither where it
    // started nor where an exactly-full ring would leave it. A plan that
    // carried the old cursor across passes this at four writes and fails
    // here, which is why it is six.
    Ring before(4);
    for (int age = 5; age >= 0; --age) {
        before.write(age);
    }

    Ring after = apply(before, 4);
    REQUIRE(after.filled == 4);
    REQUIRE(after.displayed() == std::vector<int>{0, 1, 2, 3});

    after.write(-2);
    REQUIRE(after.displayed() == std::vector<int>{-2, 0, 1, 2});
}

// REJECTS: a plan that reads the cursor of a ring with no rows in it. The
// first resize of a freshly built item arrives before any frame, and a
// height of zero is what an item that has never been laid out reports, so
// both have to come back as an empty ring rather than as a copy of nothing
// from an index nobody allocated.
TEST_CASE("an empty or unallocated ring plans no copy", "[history]")
{
    const HistoryRemap fresh = plan_history_resize(8, 7, 0, 5);
    REQUIRE(fresh.rows_kept == 0);
    REQUIRE(fresh.write_row == 4);

    const HistoryRemap none = plan_history_resize(0, 0, 0, 5);
    REQUIRE(none.rows_kept == 0);
    REQUIRE(none.write_row == 4);

    // A new height of zero is clamped to one, the same way rebuild() clamps
    // it, so the cursor is a valid index rather than minus one.
    const HistoryRemap flat = plan_history_resize(4, 1, 4, 0);
    REQUIRE(flat.write_row == 0);
    REQUIRE(flat.rows_kept == 1);
}
