// BoxHistory: the detection boxes the span waterfall keeps as history.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "render/box_history.h"

using revenant::ui::BoxHistory;
using revenant::ui::BoxSegment;
using revenant::ui::kBoxHistoryLimit;
using revenant::ui::TrackSighting;

namespace {

// Two pixels on a 2.4 MHz span 1600 pixels wide.
constexpr double kTolerance = 3000.0;

// A frame is 65536 samples, and the detector stamps both of its bounds one
// past the last sample of the frame the evidence came from.
constexpr std::uint64_t kFrame = 65'536;

[[nodiscard]] TrackSighting track(std::uint64_t id, std::int64_t low, std::int64_t high,
                                  std::uint64_t first_frame, std::uint64_t last_frame,
                                  bool merged = false)
{
    TrackSighting out;
    out.id = id;
    out.low_hz = low;
    out.high_hz = high;
    out.first_seen = first_frame * kFrame;
    out.last_detected = last_frame * kFrame;
    out.merged = merged;
    return out;
}

}  // namespace

TEST_CASE("a new track opens one segment on the rows it has been seen in", "[boxhistory]")
{
    BoxHistory history;
    history.observe({track(7, 99'990'000, 100'010'000, 10, 12)}, kTolerance);

    REQUIRE(history.segments().size() == 1);
    const BoxSegment& box = history.segments().front();
    CHECK(box.id == 7);
    CHECK(box.from_sample == 10 * kFrame);
    CHECK(box.to_sample == 12 * kFrame);
    CHECK(box.open);
}

// Rejects redrawing a box per decision, which fills the history with copies.
TEST_CASE("a live track with a steady extent grows its one segment", "[boxhistory]")
{
    BoxHistory history;
    history.observe({track(7, 99'990'000, 100'010'000, 10, 12)}, kTolerance);
    history.observe({track(7, 99'990'500, 100'009'500, 10, 20)}, kTolerance);

    REQUIRE(history.segments().size() == 1);
    CHECK(history.segments().front().to_sample == 20 * kFrame);

    // And the edges it was first drawn with, not the newest estimate: the
    // half-kilohertz wobble is inside two pixels.
    CHECK(history.segments().front().low_hz == 99'990'000);
}

// The owner's request: a box does not vanish when the detector's hold expires.
// Rejects drawing only the live list, which is what the waterfall did.
TEST_CASE("a track the detector lets go stays, fixed where it was", "[boxhistory]")
{
    BoxHistory history;
    history.observe({track(7, 99'990'000, 100'010'000, 10, 30)}, kTolerance);
    history.observe({}, kTolerance);

    REQUIRE(history.segments().size() == 1);
    const BoxSegment& box = history.segments().front();
    CHECK_FALSE(box.open);
    CHECK(box.from_sample == 10 * kFrame);
    CHECK(box.to_sample == 30 * kFrame);
}

// Rejects moving a box's past to the newest estimate, which redraws rows
// already drawn at a frequency the signal was not at when they were written.
TEST_CASE("a moved extent closes the segment and starts another after it", "[boxhistory]")
{
    BoxHistory history;
    history.observe({track(7, 99'990'000, 100'010'000, 10, 30)}, kTolerance);
    history.observe({track(7, 100'000'000, 100'020'000, 10, 40)}, kTolerance);

    REQUIRE(history.segments().size() == 2);
    const BoxSegment& before = history.segments()[0];
    const BoxSegment& after = history.segments()[1];
    CHECK_FALSE(before.open);
    CHECK(before.low_hz == 99'990'000);
    CHECK(before.to_sample == 30 * kFrame);

    CHECK(after.open);
    CHECK(after.low_hz == 100'000'000);
    CHECK(after.to_sample == 40 * kFrame);

    // One past where the first ended, so the two never claim the same row.
    CHECK(after.from_sample == before.to_sample + 1);
}

// Rejects starting a segment with no rows under it, which draws the new
// extent over rows the old one was already drawn on.
TEST_CASE("an extent that moves without new evidence changes nothing", "[boxhistory]")
{
    BoxHistory history;
    history.observe({track(7, 99'990'000, 100'010'000, 10, 30)}, kTolerance);
    history.observe({track(7, 100'050'000, 100'070'000, 10, 30)}, kTolerance);

    REQUIRE(history.segments().size() == 1);
    CHECK(history.segments().front().low_hz == 99'990'000);
    CHECK(history.segments().front().open);
}

TEST_CASE("becoming merged is a new segment, since it is drawn in its own hue", "[boxhistory]")
{
    BoxHistory history;
    history.observe({track(7, 99'990'000, 100'010'000, 10, 30)}, kTolerance);
    history.observe({track(7, 99'990'000, 100'010'000, 10, 35, true)}, kTolerance);

    REQUIRE(history.segments().size() == 2);
    CHECK_FALSE(history.segments()[0].merged);
    CHECK(history.segments()[1].merged);
}

// Rejects drawing a reappearing track from its first_seen again, which
// stacks a second box over the rows the first one covered.
TEST_CASE("a track reported again after it was let go continues after it", "[boxhistory]")
{
    BoxHistory history;
    history.observe({track(7, 99'990'000, 100'010'000, 10, 30)}, kTolerance);
    history.observe({}, kTolerance);
    history.observe({track(7, 99'990'000, 100'010'000, 10, 50)}, kTolerance);

    REQUIRE(history.segments().size() == 2);
    CHECK_FALSE(history.segments()[0].open);
    CHECK(history.segments()[1].from_sample == 30 * kFrame + 1);
    CHECK(history.segments()[1].to_sample == 50 * kFrame);
}

TEST_CASE("tracks are kept apart by id", "[boxhistory]")
{
    BoxHistory history;
    history.observe({track(1, 99'000'000, 99'010'000, 10, 20),
                     track(2, 101'000'000, 101'010'000, 12, 20)},
                    kTolerance);
    history.observe({track(2, 101'000'000, 101'010'000, 12, 25)}, kTolerance);

    REQUIRE(history.segments().size() == 2);
    CHECK_FALSE(history.segments()[0].open);
    CHECK(history.segments()[1].open);
    CHECK(history.segments()[1].to_sample == 25 * kFrame);
}

// Rejects keeping boxes for rows the waterfall no longer holds, which grows
// without bound on a busy band, and rejects forgetting a box still being
// drawn, whose first rows may have scrolled off while its newest have not.
TEST_CASE("what ended before the oldest row is forgotten, and nothing open is", "[boxhistory]")
{
    BoxHistory history;
    history.observe({track(1, 99'000'000, 99'010'000, 10, 20),
                     track(2, 101'000'000, 101'010'000, 12, 40)},
                    kTolerance);
    history.observe({track(2, 101'000'000, 101'010'000, 12, 60)}, kTolerance);

    history.forget_before(30 * kFrame);
    REQUIRE(history.segments().size() == 1);
    CHECK(history.segments().front().id == 2);

    history.forget_before(100 * kFrame);
    CHECK(history.segments().size() == 1);
}

TEST_CASE("the count is bounded, oldest closed first", "[boxhistory]")
{
    BoxHistory history;
    for (std::uint64_t id = 1; id <= kBoxHistoryLimit + 10; ++id) {
        history.observe({track(id, 99'000'000, 99'010'000, id, id + 1)}, kTolerance);
    }
    CHECK(history.segments().size() == kBoxHistoryLimit);
    CHECK(history.segments().back().open);
    CHECK(history.segments().front().id == 11);
}

TEST_CASE("clear forgets everything", "[boxhistory]")
{
    BoxHistory history;
    history.observe({track(1, 99'000'000, 99'010'000, 10, 20)}, kTolerance);
    history.clear();
    CHECK(history.segments().empty());
}
