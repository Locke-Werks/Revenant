// The captions on the span waterfall, render/caption_layout.h: the row a
// caption is pinned to, where it sits beside its passband, how it rides the
// rows down, the hold at the bottom and the fade, the stacking, the wrap, and
// what a retune and a resize do to it.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows.
//
// The waterfall here is 1000 by 500 logical pixels over a 500-row ring, so a
// row is a pixel and a row's y is its index, and the span is 100 to 102 MHz,
// two kilohertz a pixel. Rows hold a thousand samples each unless a case says
// otherwise.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "render/caption_layout.h"

using Catch::Approx;
using revenant::ui::Caption;
using revenant::ui::caption_anchor;
using revenant::ui::caption_at;
using revenant::ui::CaptionAnchorPlace;
using revenant::ui::CaptionGeometry;
using revenant::ui::CaptionHistory;
using revenant::ui::CaptionRowSpan;
using revenant::ui::kCaptionColumns;
using revenant::ui::kCaptionEllipsis;
using revenant::ui::kCaptionGapPx;
using revenant::ui::kCaptionLeaderPx;
using revenant::ui::kCaptionLimit;
using revenant::ui::kCaptionLines;
using revenant::ui::kCaptionMarginPx;
using revenant::ui::PlacedCaption;
using revenant::ui::utf8_columns;
using revenant::ui::wrap_caption;

namespace {

// The rows as the waterfall hands them over: display row 0 is the newest, and
// a row the ring has never written is all zeros.
struct Rows {
    int tall = 500;
    std::vector<CaptionRowSpan> rows;

    void push(std::uint64_t start, std::uint64_t count)
    {
        rows.insert(rows.begin(), CaptionRowSpan{start, count});
        if (rows.size() > static_cast<std::size_t>(tall)) {
            rows.pop_back();
        }
    }

    // n more rows of `count` samples each, carrying on from the newest.
    void advance(int n, std::uint64_t count = 1000)
    {
        for (int i = 0; i < n; ++i) {
            push(end(), count);
        }
    }

    // A height change: the newest `kept` rows survive, as
    // render/history_resize.h carries them over.
    void resize(int new_tall)
    {
        tall = new_tall;
        if (rows.size() > static_cast<std::size_t>(tall)) {
            rows.resize(static_cast<std::size_t>(tall));
        }
    }

    [[nodiscard]] std::uint64_t end() const
    {
        return rows.empty() ? 0 : rows.front().start + rows.front().count;
    }

    [[nodiscard]] CaptionRowSpan operator()(int row) const
    {
        return row < static_cast<int>(rows.size()) ? rows[static_cast<std::size_t>(row)]
                                                   : CaptionRowSpan{};
    }
};

[[nodiscard]] CaptionGeometry geometry(const Rows& rows, double height = 500.0)
{
    CaptionGeometry out;
    out.width_px = 1000.0;
    out.height_px = height;
    out.tall = rows.tall;
    out.span_low_hz = 100'000'000.0;
    out.span_high_hz = 102'000'000.0;
    out.pad_px = 4.0;
    out.line_height_px = 14.0;
    return out;
}

// Where the first line's middle sits below the plate's top, which is what
// lands on the utterance's row.
constexpr double kFirstLine = 4.0 + 14.0 / 2.0;

[[nodiscard]] Caption caption(std::uint64_t serial, std::uint64_t end, std::int64_t low_hz,
                              std::int64_t high_hz, double width = 200.0, double height = 40.0)
{
    Caption out;
    out.serial = serial;
    out.source_end = end;
    out.low_hz = low_hz;
    out.high_hz = high_hz;
    out.lines = {"words"};
    out.box_width = width;
    out.box_height = height;
    return out;
}

// A 12.5 kHz receiver at 101 MHz, which is x 500 to 506.25.
constexpr std::int64_t kLow = 101'000'000;
constexpr std::int64_t kHigh = 101'012'500;

[[nodiscard]] const PlacedCaption* placed(const std::vector<PlacedCaption>& all,
                                          std::uint64_t serial)
{
    for (const PlacedCaption& one : all) {
        if (one.serial == serial) {
            return &one;
        }
    }
    return nullptr;
}

}  // namespace

// Rejects searching for source_end itself. It is one past the utterance, so
// the row holding it is the row after the speech ended, and an utterance that
// ends on a row boundary would be captioned a row newer than it was.
TEST_CASE("a caption is pinned to the row holding the utterance's last sample")
{
    Rows rows;
    rows.advance(100);  // row k holds [(99 - k) * 1000, (100 - k) * 1000)

    const auto anchor = caption_anchor(rows, rows.tall, 95'000);
    CHECK(anchor.place == CaptionAnchorPlace::Inside);
    CHECK(anchor.row == 5);

    CaptionHistory history;
    history.add(caption(1, 95'000, kLow, kHigh));
    const auto out = history.layout(rows, geometry(rows));
    REQUIRE(out.size() == 1);
    CHECK(out[0].anchor_y == Approx(5.0));
    CHECK(out[0].leader_y == Approx(5.0));
}

// Rejects a caption that is placed once and left: it has to ride the rows, one
// row down for every row that arrives, or it detaches from its time.
TEST_CASE("a caption scrolls down with the rows")
{
    Rows rows;
    rows.advance(300);
    CaptionHistory history;
    history.add(caption(1, 250'000, kLow, kHigh));

    const auto first = history.layout(rows, geometry(rows));
    REQUIRE(first.size() == 1);
    const double y0 = first[0].y;

    rows.advance(7);
    const auto later = history.layout(rows, geometry(rows));
    REQUIRE(later.size() == 1);
    CHECK(later[0].y == Approx(y0 + 7.0));
    CHECK(later[0].anchor_y == Approx(first[0].anchor_y + 7.0));
}

// Rejects placing a caption by its age times a nominal row rate. The
// client's latest-wins hand-off drops rows when the GUI thread is late, so
// rows do not cover equal stretches of time, and only the rows' own sample
// ranges say where an utterance is.
TEST_CASE("rows the hand-off dropped do not move a caption off its time")
{
    Rows rows;
    for (const std::uint64_t start : {0ULL, 1000ULL, 4000ULL, 5000ULL, 8000ULL, 9000ULL}) {
        rows.push(start, 1000);
    }
    // Newest first: 9000, 8000, 5000, 4000, 1000, 0. The last sample of an
    // utterance ending at 1500 is in the row starting at 1000, row 4. By rate
    // it would be (10000 - 1500) / 1000, row 8.
    const auto anchor = caption_anchor(rows, rows.tall, 1500);
    CHECK(anchor.place == CaptionAnchorPlace::Inside);
    CHECK(anchor.row == 4);

    // A sample in a gap belongs to the older row beside it, never the newer.
    CHECK(caption_anchor(rows, rows.tall, 3001).row == 4);
}

// Rejects a caption drawn on the passband, which would hide the very signal
// it describes, and one with no leader, which would leave which receiver it
// belongs to to the colour alone.
TEST_CASE("a caption sits beside its passband with a leader to it")
{
    Rows rows;
    rows.advance(100);
    CaptionHistory history;
    history.add(caption(1, 60'000, kLow, kHigh));
    const auto out = history.layout(rows, geometry(rows));
    REQUIRE(out.size() == 1);
    const PlacedCaption& one = out[0];
    CHECK(one.x == Approx(506.25 + kCaptionLeaderPx));
    CHECK(one.leader_from_x == Approx(506.25));
    CHECK(one.leader_to_x == Approx(one.x));
    CHECK(one.has_leader());
    CHECK(one.offside == 0);
}

// Rejects a plate pushed off the right edge, or clamped back over its own
// passband, when there is room on the left.
TEST_CASE("a caption goes left of the passband when the right has no room")
{
    Rows rows;
    rows.advance(100);
    CaptionHistory history;
    // 101.9 MHz is x 950.
    history.add(caption(1, 60'000, 101'900'000, 101'912'500));
    const auto out = history.layout(rows, geometry(rows));
    REQUIRE(out.size() == 1);
    CHECK(out[0].x == Approx(950.0 - kCaptionLeaderPx - 200.0));
    CHECK(out[0].leader_from_x == Approx(950.0 - kCaptionLeaderPx));
    CHECK(out[0].leader_to_x == Approx(950.0));
    CHECK(out[0].x + out[0].width <= 1000.0 - kCaptionMarginPx);
}

// Rejects dropping a caption whose receiver the span no longer reaches after
// a retune: its rows are still on screen, so it is held at the nearer edge,
// with no leader to a passband that is not drawn.
TEST_CASE("a caption whose passband is off the span holds at the nearer edge")
{
    Rows rows;
    rows.advance(100);
    CaptionHistory history;
    history.add(caption(1, 60'000, 99'000'000, 99'012'500));
    history.add(caption(2, 50'000, 103'000'000, 103'012'500));
    const auto out = history.layout(rows, geometry(rows));
    REQUIRE(out.size() == 2);
    const PlacedCaption* left = placed(out, 1);
    const PlacedCaption* right = placed(out, 2);
    REQUIRE(left != nullptr);
    REQUIRE(right != nullptr);
    CHECK(left->offside == -1);
    CHECK(left->x == Approx(kCaptionMarginPx));
    CHECK_FALSE(left->has_leader());
    CHECK(right->offside == 1);
    CHECK(right->x == Approx(1000.0 - kCaptionMarginPx - 200.0));
    CHECK_FALSE(right->has_leader());
}

// Rejects the half-speed scroll the owner turned down, and a caption that
// scrolls off with its rows: at the bottom it holds, on the bottom edge,
// whatever more rows arrive.
TEST_CASE("a caption holds at the bottom edge rather than scrolling off")
{
    Rows rows;
    rows.advance(500);
    CaptionHistory history;
    // At row 400, so its plate top is at 389 and the hold is at 460.
    history.add(caption(1, rows.end() - 400 * 1000, kLow, kHigh));

    auto out = history.layout(rows, geometry(rows));
    REQUIRE(out.size() == 1);
    CHECK_FALSE(out[0].held);
    CHECK(out[0].y == Approx(400.0 - kFirstLine));

    rows.advance(70);  // row 470: plate top 459, one short of the hold
    out = history.layout(rows, geometry(rows));
    REQUIRE(out.size() == 1);
    CHECK_FALSE(out[0].held);

    rows.advance(1);
    out = history.layout(rows, geometry(rows));
    REQUIRE(out.size() == 1);
    CHECK(out[0].held);
    CHECK(out[0].y == Approx(460.0));

    rows.advance(40);  // its rows are gone now
    out = history.layout(rows, geometry(rows));
    REQUIRE(out.size() == 1);
    CHECK(out[0].held);
    CHECK(out[0].y == Approx(460.0));
}

// Rejects a fade of some fixed length, and one that starts before the hold.
// The owner's rule is that a caption fades over the time it took to cross the
// waterfall, which is the history's own span: 500 rows of a thousand samples.
TEST_CASE("the fade lasts as long as the crossing and begins at the hold")
{
    Rows rows;
    rows.advance(500);
    CaptionHistory history;
    history.add(caption(1, rows.end() - 480 * 1000, kLow, kHigh));

    auto out = history.layout(rows, geometry(rows));
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].held);
    CHECK(out[0].opacity == Approx(1.0));
    CHECK(history.captions().front().fade_samples == 500'000);

    rows.advance(250);
    out = history.layout(rows, geometry(rows));
    REQUIRE(out.size() == 1);
    CHECK(out[0].opacity == Approx(0.5));

    rows.advance(249);
    out = history.layout(rows, geometry(rows));
    REQUIRE(out.size() == 1);
    CHECK(out[0].opacity == Approx(1.0 / 500.0));

    rows.advance(1);
    out = history.layout(rows, geometry(rows));
    CHECK(out.empty());
    CHECK(history.empty());
}

// Rejects a crossing counted in rows. With rows of two thousand samples the
// waterfall spans twice the time, and the fade does too.
TEST_CASE("the crossing is counted in samples, not rows")
{
    Rows rows;
    rows.advance(500, 2000);
    CaptionHistory history;
    history.add(caption(1, rows.end() - 480 * 2000, kLow, kHigh));
    const auto out = history.layout(rows, geometry(rows));
    REQUIRE(out.size() == 1);
    CHECK(history.captions().front().fade_samples == 1'000'000);
}

// Rejects captions that stay put when the front end retunes. A retune moves
// frequency and not time, so the caption keeps its row and slides sideways by
// exactly what the axis moved, as the rows do in render/history_shift.h.
TEST_CASE("a retune slides a caption with the rows")
{
    Rows rows;
    rows.advance(100);
    CaptionHistory history;
    history.add(caption(1, 60'000, kLow, kHigh));
    const auto before = history.layout(rows, geometry(rows));
    REQUIRE(before.size() == 1);

    // Up by 400 kHz: the passband is 200 pixels further left.
    CaptionGeometry tuned = geometry(rows);
    tuned.span_low_hz += 400'000.0;
    tuned.span_high_hz += 400'000.0;
    const auto after = history.layout(rows, tuned);
    REQUIRE(after.size() == 1);
    CHECK(after[0].x == Approx(before[0].x - 200.0));
    CHECK(after[0].leader_from_x == Approx(before[0].leader_from_x - 200.0));
    CHECK(after[0].y == Approx(before[0].y));
}

// Rejects dropping a caption that would land on another, and rejects plates
// drawn over each other. The newer keeps its row; the older moves below it,
// and its leader stays on its own row with an elbow to the plate.
TEST_CASE("captions that would overlap stack rather than being dropped")
{
    Rows rows;
    rows.advance(300);
    CaptionHistory history;
    history.add(caption(1, rows.end() - 110 * 1000, kLow, kHigh));  // row 110
    history.add(caption(2, rows.end() - 100 * 1000, kLow, kHigh));  // row 100, newer
    const auto out = history.layout(rows, geometry(rows));
    REQUIRE(out.size() == 2);
    const PlacedCaption* older = placed(out, 1);
    const PlacedCaption* newer = placed(out, 2);
    REQUIRE(older != nullptr);
    REQUIRE(newer != nullptr);
    CHECK(newer->y == Approx(100.0 - kFirstLine));
    CHECK(older->y == Approx(newer->y + newer->height + kCaptionGapPx));
    CHECK(older->leader_y == Approx(110.0));
    CHECK(older->has_elbow());
    CHECK(older->elbow_to_y >= older->y);
}

// Rejects stacking every caption against every other: plates beside two
// receivers far apart do not touch, so each keeps its own row.
TEST_CASE("captions beside different receivers do not push each other")
{
    Rows rows;
    rows.advance(300);
    CaptionHistory history;
    history.add(caption(1, rows.end() - 100 * 1000, 100'200'000, 100'212'500));  // x 100
    history.add(caption(2, rows.end() - 100 * 1000, kLow, kHigh));               // x 500
    const auto out = history.layout(rows, geometry(rows));
    REQUIRE(out.size() == 2);
    CHECK(out[0].y == Approx(out[1].y));
}

// Rejects held captions piled on one spot, and rejects keeping only the
// newest at the bottom. They stack up from the bottom edge, oldest lowest, so
// the one fading out soonest is the one nearest the edge.
TEST_CASE("captions crowding the hold stack upward, oldest lowest")
{
    Rows rows;
    rows.advance(500);
    CaptionHistory history;
    history.add(caption(1, rows.end() - 499 * 1000, kLow, kHigh));
    history.add(caption(2, rows.end() - 495 * 1000, kLow, kHigh));
    history.add(caption(3, rows.end() - 490 * 1000, kLow, kHigh));
    const auto out = history.layout(rows, geometry(rows));
    REQUIRE(out.size() == 3);
    const PlacedCaption* oldest = placed(out, 1);
    const PlacedCaption* middle = placed(out, 2);
    const PlacedCaption* newest = placed(out, 3);
    REQUIRE(oldest != nullptr);
    REQUIRE(middle != nullptr);
    REQUIRE(newest != nullptr);
    CHECK(oldest->held);
    CHECK(middle->held);
    CHECK(newest->held);
    CHECK(oldest->y == Approx(460.0));
    CHECK(middle->y == Approx(oldest->y - kCaptionGapPx - 40.0));
    CHECK(newest->y == Approx(middle->y - kCaptionGapPx - 40.0));
}

// Rejects a caption that keeps a screen position across a height change. A
// taller waterfall gains empty rows below the history and keeps every row
// where it was, so a caption on row 100 is still on row 100.
TEST_CASE("a taller waterfall keeps a caption on its row")
{
    Rows rows;
    rows.advance(500);
    CaptionHistory history;
    history.add(caption(1, rows.end() - 100 * 1000, kLow, kHigh));

    rows.resize(800);
    const auto out = history.layout(rows, geometry(rows, 800.0));
    REQUIRE(out.size() == 1);
    CHECK(out[0].anchor_y == Approx(100.0));
    CHECK_FALSE(out[0].held);
}

// Rejects dropping a caption whose rows a shorter waterfall let go: it was
// already said and is still worth reading, so it goes to the hold and fades
// like any other that reached the bottom.
TEST_CASE("a shorter waterfall holds the captions whose rows it dropped")
{
    Rows rows;
    rows.advance(500);
    CaptionHistory history;
    history.add(caption(1, rows.end() - 300 * 1000, kLow, kHigh));

    rows.resize(200);
    const auto out = history.layout(rows, geometry(rows, 200.0));
    REQUIRE(out.size() == 1);
    CHECK(out[0].held);
    CHECK(out[0].place == CaptionAnchorPlace::Older);
    CHECK(out[0].y == Approx(160.0));
    CHECK(history.captions().front().fade_samples == 200'000);
}

// Rejects drawing a transcript from another stream on the newest row: one
// that claims to end further ahead of the newest row than the whole history
// is long cannot be from the stream these rows are of. One a little ahead is
// rows the hand-off has not delivered yet, and goes on the newest row.
TEST_CASE("a caption ahead of the rows is drawn on the newest, one far ahead is not")
{
    Rows rows;
    rows.advance(100);
    CHECK(caption_anchor(rows, rows.tall, rows.end() + 5000).place == CaptionAnchorPlace::Newer);
    CHECK(caption_anchor(rows, rows.tall, rows.end() + 200'000).place ==
          CaptionAnchorPlace::Beyond);

    CaptionHistory history;
    history.add(caption(1, rows.end() + 5000, kLow, kHigh));
    history.add(caption(2, rows.end() + 200'000, kLow, kHigh));
    const auto out = history.layout(rows, geometry(rows));
    REQUIRE(out.size() == 1);
    CHECK(out[0].serial == 1);
    CHECK(out[0].y == Approx(0.0));
    CHECK(history.captions().size() == 1);
}

// Rejects placing anything before a row has been written: there is no time
// axis to pin to, and the captions are kept for when there is.
TEST_CASE("nothing is placed on an empty history and nothing is lost")
{
    Rows rows;
    CaptionHistory history;
    history.add(caption(1, 1000, kLow, kHigh));
    CHECK(history.layout(rows, geometry(rows)).empty());
    CHECK(history.captions().size() == 1);
}

// Rejects an unbounded history.
TEST_CASE("past the limit the oldest caption goes first")
{
    CaptionHistory history;
    for (std::uint64_t serial = 1; serial <= kCaptionLimit + 6; ++serial) {
        history.add(caption(serial, serial * 1000, kLow, kHigh));
    }
    CHECK(history.captions().size() == kCaptionLimit);
    CHECK(history.captions().front().serial == 7);
    CHECK(history.find(6) == nullptr);
    CHECK(history.find(kCaptionLimit + 6) != nullptr);
}

// Rejects a hit test that misses the plate's edges or finds a caption where
// there is none.
TEST_CASE("the caption under the pointer is the plate it is on")
{
    PlacedCaption one;
    one.serial = 9;
    one.x = 100.0;
    one.y = 50.0;
    one.width = 200.0;
    one.height = 40.0;
    const std::vector<PlacedCaption> all = {one};
    CHECK(caption_at(all, 100.0, 50.0) == 9);
    CHECK(caption_at(all, 300.0, 90.0) == 9);
    CHECK(caption_at(all, 301.0, 60.0) == 0);
    CHECK(caption_at(all, 150.0, 49.0) == 0);
}

// Rejects a wrap that splits words, runs past the column, keeps more than
// three lines, or cuts text without saying so.
TEST_CASE("long text wraps at spaces to the column and three lines, cut with an ellipsis")
{
    const auto short_one = wrap_caption("engine two on scene");
    REQUIRE(short_one.size() == 1);
    CHECK(short_one[0] == "engine two on scene");

    const std::string sentence =
        "medic seven responding to the north side of the lake road with two units and a "
        "ladder truck behind them, ETA four minutes, requesting the helicopter be put on "
        "standby for a possible transport";
    const auto lines = wrap_caption(sentence);
    REQUIRE(lines.size() == kCaptionLines);
    for (const std::string& line : lines) {
        CHECK(utf8_columns(line) <= kCaptionColumns);
        CHECK(line.front() != ' ');
        CHECK(line.back() != ' ');
    }
    CHECK(lines[0] == "medic seven responding to the north side");
    CHECK(lines.back().ends_with(kCaptionEllipsis));

    // Exactly three lines' worth is not cut.
    const auto fits = wrap_caption("aaaa bbbb cccc", 4, 3);
    REQUIRE(fits.size() == 3);
    CHECK(fits[2] == "cccc");
}

// Rejects a word longer than a line overflowing the plate, and a cut that
// lands inside a multi-byte character.
TEST_CASE("a word longer than a line is cut at the column, never inside a character")
{
    const auto cut = wrap_caption("abcdefghij", 4, 5);
    REQUIRE(cut.size() == 3);
    CHECK(cut[0] == "abcd");
    CHECK(cut[1] == "efgh");
    CHECK(cut[2] == "ij");

    // Five e-acute, two bytes each, in lines of two columns.
    const std::string accents = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";
    const auto lines = wrap_caption(accents, 2, 5);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "\xC3\xA9\xC3\xA9");
    CHECK(utf8_columns(lines[2]) == 1);
}

// Rejects a carriage return or a run of spaces in what the recogniser wrote
// shaping the caption: they are spaces, and runs of them are one.
TEST_CASE("control characters and runs of spaces wrap as single spaces")
{
    const auto lines = wrap_caption("  copy\r\nthat \t  ten four  ");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "copy that ten four");
    CHECK(wrap_caption("   ").empty());
}
