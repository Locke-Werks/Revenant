// Speech on the span waterfall: where a caption goes, how it rides the rows,
// where it holds, how it fades, and what happens when two would land on each
// other.
//
// WHAT THE OWNER ASKED FOR, 2026-10-03. A caption is pinned to the
// transmission it belongs to: at the rows where the utterance ENDED, beside
// the receiver's passband in frequency, with a short leader to it. It scrolls
// down with the rows, so it always lines up with the time the speech was on
// the air. When it reaches the bottom of the waterfall it does not scroll
// off: it holds there and fades out over the same length of time it took to
// cross the waterfall. Captions that would overlap stack rather than being
// dropped. The owner first asked for half-speed scrolling and chose this
// instead, because text that moves at half the rows' speed is no longer next
// to the time it describes.
//
// ROWS, NOT SECONDS. A transcript carries [source_start, source_end) in the
// engine's source sample index, the clock SpectrumFrame::start and every
// Detection count in, and the waterfall keeps the sample range of every row
// it holds beside the pixels (render/waterfall_item.h). So a caption is put on
// the row whose range holds its last sample, found the way render/
// box_history.h's boxes are, and never by multiplying a latency by a row
// rate. The row rate is not a constant: the engine paces rows at its --rows
// setting, a fast source makes one per block, and the client's latest-wins
// hand-off can drop rows when the GUI thread is late (docs/ui-spectrum.md,
// "Frame budget"). A caption placed by arithmetic on a nominal rate would
// drift off its rows by exactly the rows that were dropped.
//
// THE CROSSING TIME IS THE HISTORY'S OWN SPAN, in samples, newest row's end
// less oldest row's start, at the moment the caption reaches the hold. For a
// caption that rode the rows down that is the time it took to get there; for
// one whose rows had already gone by the time its text arrived, a late
// transcript or a waterfall made shorter, it is still the time the waterfall
// takes to cross, which is the number the owner's rule is about. Measured in
// samples rather than in wall time for the reason the rows are: a waterfall
// that stops because the engine stopped stops fading too, and a test can say
// exactly when a caption has gone.
//
// ABSOLUTE HERTZ, so a retune moves nothing here. A caption keeps the
// receiver's passband as it was when the receiver heard the speech, which the
// schema promises is "not necessarily where it is now", and is placed on the
// span's axis at every layout, so it slides with the rows render/
// history_shift.h moves. A caption whose passband the span no longer reaches
// is held inside the edge nearer to it, with no leader, rather than dropped:
// its rows are still on screen.
//
// WIDTHS IN COLUMNS, NOT PIXELS. A caption is set in Cascadia Mono, the
// Theme's monospace, so a line of N characters is N advances wide and the
// wrap can be decided here without a font. The caller measures the plate with
// the real font afterwards, and the layout is handed those measurements; the
// column count only decides where lines break.
//
// This header holds no Qt; ui/tests/test_caption_layout.cpp links it.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace revenant::ui {

// ---------------------------------------------------------------------------
// The shape of a caption
// ---------------------------------------------------------------------------

// Forty columns and three lines. A caption is a few words at a glance, and
// the hover card and the decode log carry the whole text: forty columns is
// about two hundred and sixty pixels at the 11 px monospace, which sits beside
// a passband on a 1600 pixel span without covering the next receiver's, and
// three lines is what an operator reads without stopping.
inline constexpr std::size_t kCaptionColumns = 40;
inline constexpr std::size_t kCaptionLines = 3;

// U+2026, written as its UTF-8 bytes so the header stays ASCII.
inline constexpr std::string_view kCaptionEllipsis = "\xE2\x80\xA6";

// How many captions are kept at once. A waterfall at the engine's 30 rows a
// second and a thousand rows tall crosses in about half a minute and then
// takes as long again to fade, so a minute of speech is on screen at most,
// and sixty-four is a busy band's minute. Past it the oldest goes first.
inline constexpr std::size_t kCaptionLimit = 64;

// From the passband's edge to the plate, which is the leader's length when
// the plate is not pushed anywhere.
inline constexpr double kCaptionLeaderPx = 14.0;

// Kept inside the item's edges by this much.
inline constexpr double kCaptionMarginPx = 4.0;

// Between two stacked plates.
inline constexpr double kCaptionGapPx = 3.0;

// ---------------------------------------------------------------------------
// Wrapping
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr bool utf8_continuation(char byte)
{
    return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
}

// Code points, which is columns in a monospace for everything but the wide
// East Asian forms and combining marks. Those are measured off by this; the
// plate is measured with the real font, so a line that is wider than its
// count says is still drawn whole.
[[nodiscard]] constexpr std::size_t utf8_columns(std::string_view text)
{
    std::size_t count = 0;
    for (const char byte : text) {
        if (!utf8_continuation(byte)) {
            ++count;
        }
    }
    return count;
}

// The bytes of the first `columns` code points, so a cut never lands inside
// a character.
[[nodiscard]] constexpr std::size_t utf8_prefix_bytes(std::string_view text, std::size_t columns)
{
    std::size_t seen = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (!utf8_continuation(text[i])) {
            if (seen == columns) {
                return i;
            }
            ++seen;
        }
    }
    return text.size();
}

// The text as at most `max_lines` lines of at most `columns` columns, broken
// at spaces. A word longer than a line is cut at the column, which is the
// only place a word is split. Text that does not fit ends its last line with
// an ellipsis, so a cut caption says it was cut. Control characters count as
// spaces, so a carriage return in what the recogniser wrote cannot start a
// line of its own.
[[nodiscard]] inline std::vector<std::string> wrap_caption(std::string_view text,
                                                           std::size_t columns = kCaptionColumns,
                                                           std::size_t max_lines = kCaptionLines)
{
    std::vector<std::string> lines;
    if (columns == 0 || max_lines == 0) {
        return lines;
    }

    const auto is_space = [](char byte) {
        const auto value = static_cast<unsigned char>(byte);
        return value <= 0x20U || value == 0x7FU;
    };

    std::string line;
    std::size_t line_columns = 0;
    const auto flush = [&] {
        lines.push_back(std::move(line));
        line.clear();
        line_columns = 0;
    };

    std::size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && is_space(text[at])) {
            ++at;
        }
        std::size_t end = at;
        while (end < text.size() && !is_space(text[end])) {
            ++end;
        }
        std::string_view word = text.substr(at, end - at);
        at = end;
        std::size_t word_columns = utf8_columns(word);

        while (word_columns > columns) {
            if (line_columns > 0) {
                flush();
            }
            const std::size_t bytes = utf8_prefix_bytes(word, columns);
            line.assign(word.substr(0, bytes));
            line_columns = columns;
            flush();
            word.remove_prefix(bytes);
            word_columns -= columns;
        }
        if (word_columns == 0) {
            continue;
        }
        if (line_columns > 0 && line_columns + 1 + word_columns > columns) {
            flush();
        }
        if (line_columns > 0) {
            line += ' ';
            line_columns += 1;
        }
        line.append(word);
        line_columns += word_columns;
    }
    if (line_columns > 0) {
        flush();
    }

    if (lines.size() > max_lines) {
        lines.resize(max_lines);
        std::string& last = lines.back();
        if (utf8_columns(last) + 1 > columns) {
            last.resize(utf8_prefix_bytes(last, columns - 1));
        }
        while (!last.empty() && last.back() == ' ') {
            last.pop_back();
        }
        last.append(kCaptionEllipsis);
    }
    return lines;
}

// ---------------------------------------------------------------------------
// From a sample to a row
// ---------------------------------------------------------------------------

// What one stored row covers, [start, start + count) in the engine's source
// sample index. A row never written is all zeros.
struct CaptionRowSpan {
    std::uint64_t start = 0;
    std::uint64_t count = 0;
};

enum class CaptionAnchorPlace : std::uint8_t {
    // The history holds the row the sample is in.
    Inside,

    // Newer than the newest row, by less than the history is long: rows the
    // hand-off has not delivered yet. Placed on the newest row.
    Newer,

    // Older than the oldest row: scrolled off, or never on this history.
    Older,

    // Newer than the newest row by more than the whole history spans, which
    // no transcript of this stream can be: its rows are from another stream.
    Beyond,

    // Nothing written yet.
    Empty,
};

struct CaptionAnchor {
    CaptionAnchorPlace place = CaptionAnchorPlace::Empty;

    // Display row from the top, newest first. Meaningful for Inside and Newer.
    int row = 0;
};

// How many display rows have been written. The waterfall writes from the top
// and keeps written rows contiguous across a height change
// (render/history_resize.h), so the written rows are a prefix and a binary
// search finds the end of it.
template <class SpanAt>
[[nodiscard]] int caption_filled_rows(const SpanAt& span_at, int tall)
{
    int low = 0;
    int high = tall;
    while (low < high) {
        const int mid = low + (high - low) / 2;
        if (span_at(mid).count > 0) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low;
}

// The display row holding the last sample of [.., end_sample).
//
// end_sample is ONE PAST the utterance, as the schema's half-open
// [sourceStart, sourceEnd) has it, so the row searched for is the one holding
// end_sample - 1. Searching for end_sample itself puts the caption a row
// newer than the speech whenever the utterance ends on a row boundary: the
// mistake WaterfallItem::rowsForSamples records making with detections.
template <class SpanAt>
[[nodiscard]] CaptionAnchor caption_anchor(const SpanAt& span_at, int tall,
                                           std::uint64_t end_sample)
{
    CaptionAnchor out;
    if (tall <= 0) {
        return out;
    }
    const CaptionRowSpan newest = span_at(0);
    if (newest.count == 0) {
        return out;
    }
    const std::uint64_t newest_end = newest.start + newest.count;
    const std::uint64_t last_in = end_sample == 0 ? 0 : end_sample - 1;

    if (last_in >= newest_end) {
        const int filled = caption_filled_rows(span_at, tall);
        const std::uint64_t oldest = span_at(filled - 1).start;
        out.place = last_in - newest_end < newest_end - oldest ? CaptionAnchorPlace::Newer
                                                               : CaptionAnchorPlace::Beyond;
        out.row = 0;
        return out;
    }

    // Display rows run newest to oldest, so starts are non-increasing down
    // the display and the first row starting at or before the sample is the
    // one that holds it, or the next older one when the sample fell in a gap
    // the hand-off left between two rows.
    int low = 0;
    int high = tall;
    while (low < high) {
        const int mid = low + (high - low) / 2;
        if (span_at(mid).start <= last_in) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    if (low >= tall || span_at(low).count == 0) {
        out.place = CaptionAnchorPlace::Older;
        out.row = low;
        return out;
    }
    out.place = CaptionAnchorPlace::Inside;
    out.row = low;
    return out;
}

// ---------------------------------------------------------------------------
// One caption, and where it is drawn
// ---------------------------------------------------------------------------

struct Caption {
    // This window's own count, never reused, so a drawn node can be keyed on
    // it across evictions. The engine's sequence restarts with the engine.
    std::uint64_t serial = 0;

    // The end of the utterance in the source sample index, and the receiver's
    // passband when it heard it, absolute hertz.
    std::uint64_t source_end = 0;
    std::int64_t low_hz = 0;
    std::int64_t high_hz = 0;

    // The receiver's rack colour slot, -1 when it is not in the rack.
    int slot = -1;

    // The recogniser doubted it; drawn dimmer, as the log line is.
    bool doubtful = false;

    std::vector<std::string> lines;

    // The hover card's text, the whole transcript.
    std::string card;

    // The plate as the caller measured it with the font, logical pixels.
    double box_width = 0.0;
    double box_height = 0.0;

    // The hold. Written by CaptionHistory::layout and nothing else: the
    // source sample the newest row ended at when the caption reached the
    // bottom, and the crossing time it fades over.
    bool held = false;
    std::uint64_t held_at = 0;
    std::uint64_t fade_samples = 0;
};

// What the layout needs to know about the waterfall.
struct CaptionGeometry {
    double width_px = 0.0;
    double height_px = 0.0;

    // Rows in the ring, which is the display's height in device pixels.
    int tall = 0;

    double span_low_hz = 0.0;
    double span_high_hz = 0.0;

    // Inside the plate, and one line of text, as the caller measured them.
    double pad_px = 4.0;
    double line_height_px = 14.0;
};

struct PlacedCaption {
    std::uint64_t serial = 0;

    // The plate.
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    // The leader: horizontal at leader_y from leader_from_x to leader_to_x,
    // then, when stacking pushed the plate off its own row, a vertical run at
    // the plate's edge from leader_y to elbow_to_y. No leader when from and
    // to meet.
    double leader_from_x = 0.0;
    double leader_to_x = 0.0;
    double leader_y = 0.0;
    double elbow_x = 0.0;
    double elbow_to_y = 0.0;

    // Where the end of the utterance is on the display, before the plate was
    // clamped, held or stacked. Meaningless for a held caption whose rows
    // have gone.
    double anchor_y = 0.0;
    CaptionAnchorPlace place = CaptionAnchorPlace::Empty;

    // -1 when the passband is left of the span, +1 when it is right of it.
    int offside = 0;

    double opacity = 1.0;
    bool held = false;

    int slot = -1;
    bool doubtful = false;

    [[nodiscard]] bool has_leader() const { return leader_to_x - leader_from_x > 0.5; }
    [[nodiscard]] bool has_elbow() const { return std::abs(elbow_to_y - leader_y) > 0.5; }
};

// The serial of the plate under (x, y), or zero.
[[nodiscard]] inline std::uint64_t caption_at(const std::vector<PlacedCaption>& placed, double x,
                                              double y)
{
    for (const PlacedCaption& one : placed) {
        if (x >= one.x && x <= one.x + one.width && y >= one.y && y <= one.y + one.height) {
            return one.serial;
        }
    }
    return 0;
}

namespace caption_detail {

[[nodiscard]] inline bool overlaps_x(const PlacedCaption& a, const PlacedCaption& b)
{
    return a.x < b.x + b.width + kCaptionGapPx && b.x < a.x + a.width + kCaptionGapPx;
}

[[nodiscard]] inline bool overlaps_y(double top, double height, const PlacedCaption& other)
{
    return top < other.y + other.height + kCaptionGapPx &&
           other.y < top + height + kCaptionGapPx;
}

// Beside the passband: right of it when the plate fits there, left of it when
// it fits there instead, and against whichever edge has more room when it
// fits on neither side, with no leader since the plate is then over the band.
inline void place_beside(PlacedCaption& out, double low_px, double high_px, double width_px)
{
    const double w = out.width;
    const double right_limit = width_px - kCaptionMarginPx - w;
    const auto clamped = [&](double x) {
        return right_limit < kCaptionMarginPx ? kCaptionMarginPx
                                              : std::clamp(x, kCaptionMarginPx, right_limit);
    };

    if (high_px < 0.0) {
        out.offside = -1;
        out.x = clamped(kCaptionMarginPx);
        return;
    }
    if (low_px > width_px) {
        out.offside = 1;
        out.x = clamped(right_limit);
        return;
    }

    const double right_x = high_px + kCaptionLeaderPx;
    const double left_x = low_px - kCaptionLeaderPx - w;
    if (right_x <= right_limit) {
        out.x = right_x;
        out.leader_from_x = high_px;
        out.leader_to_x = right_x;
        return;
    }
    if (left_x >= kCaptionMarginPx) {
        out.x = left_x;
        out.leader_from_x = left_x + w;
        out.leader_to_x = low_px;
        return;
    }
    const double room_right = width_px - high_px;
    const double room_left = low_px;
    out.x = clamped(room_right >= room_left ? right_limit : kCaptionMarginPx);
}

}  // namespace caption_detail

// The captions on the waterfall, oldest first.
class CaptionHistory {
public:
    void add(Caption caption)
    {
        captions_.push_back(std::move(caption));
        if (captions_.size() > kCaptionLimit) {
            captions_.erase(captions_.begin(),
                            captions_.begin() +
                                static_cast<std::ptrdiff_t>(captions_.size() - kCaptionLimit));
        }
    }

    // A new stream, or a history thrown away: the rows these were pinned to
    // are gone, the same reason render/box_history.h's boxes go with them.
    void clear() { captions_.clear(); }

    [[nodiscard]] const std::vector<Caption>& captions() const { return captions_; }
    [[nodiscard]] bool empty() const { return captions_.empty(); }

    [[nodiscard]] const Caption* find(std::uint64_t serial) const
    {
        for (const Caption& caption : captions_) {
            if (caption.serial == serial) {
                return &caption;
            }
        }
        return nullptr;
    }

    // Places every caption against the rows the waterfall holds now, starts
    // the hold on each that has reached the bottom, forgets each that has
    // faded out or belongs to another stream, and returns the rest. Called
    // whenever a row arrives or the axis or the size moves; calling it twice
    // with nothing changed changes nothing.
    template <class SpanAt>
    std::vector<PlacedCaption> layout(const SpanAt& span_at, const CaptionGeometry& geometry)
    {
        std::vector<PlacedCaption> placed;
        if (captions_.empty() || geometry.tall <= 0 || !(geometry.height_px > 0.0) ||
            !(geometry.width_px > 0.0)) {
            return placed;
        }
        const CaptionRowSpan newest = span_at(0);
        if (newest.count == 0) {
            return placed;
        }
        const std::uint64_t now = newest.start + newest.count;
        const int filled = caption_filled_rows(span_at, geometry.tall);
        const std::uint64_t oldest = span_at(filled - 1).start;
        const std::uint64_t crossing = std::max<std::uint64_t>(now - oldest, 1);

        const double h = geometry.height_px;
        const double span_hz = geometry.span_high_hz - geometry.span_low_hz;
        const double first_line = geometry.pad_px + geometry.line_height_px / 2.0;

        std::vector<Caption> kept;
        kept.reserve(captions_.size());
        for (Caption& caption : captions_) {
            const CaptionAnchor anchor = caption_anchor(span_at, geometry.tall, caption.source_end);
            if (anchor.place == CaptionAnchorPlace::Beyond ||
                anchor.place == CaptionAnchorPlace::Empty) {
                continue;
            }
            const bool on_rows = anchor.place == CaptionAnchorPlace::Inside ||
                                 anchor.place == CaptionAnchorPlace::Newer;
            const double anchor_y =
                on_rows ? h * static_cast<double>(anchor.row) / static_cast<double>(geometry.tall)
                        : h;
            const double max_top = std::max(0.0, h - caption.box_height);
            const double wanted_top = anchor_y - first_line;

            if (!caption.held && (!on_rows || wanted_top >= max_top)) {
                caption.held = true;
                caption.held_at = now;
                caption.fade_samples = crossing;
            }

            double opacity = 1.0;
            if (caption.held && now > caption.held_at) {
                opacity = 1.0 - static_cast<double>(now - caption.held_at) /
                                    static_cast<double>(caption.fade_samples);
            }
            if (opacity <= 0.0) {
                continue;
            }

            PlacedCaption one;
            one.serial = caption.serial;
            one.width = caption.box_width;
            one.height = caption.box_height;
            one.anchor_y = anchor_y;
            one.place = anchor.place;
            one.held = caption.held;
            one.opacity = std::min(opacity, 1.0);
            one.slot = caption.slot;
            one.doubtful = caption.doubtful;
            one.y = caption.held ? max_top : std::clamp(wanted_top, 0.0, max_top);

            if (span_hz > 0.0) {
                double low_px = (static_cast<double>(caption.low_hz) - geometry.span_low_hz) /
                                span_hz * geometry.width_px;
                double high_px = (static_cast<double>(caption.high_hz) - geometry.span_low_hz) /
                                 span_hz * geometry.width_px;
                if (high_px < low_px) {
                    std::swap(low_px, high_px);
                }
                caption_detail::place_beside(one, low_px, high_px, geometry.width_px);
            }
            placed.push_back(one);
            kept.push_back(std::move(caption));
        }
        captions_ = std::move(kept);

        stack(placed, h);

        for (PlacedCaption& one : placed) {
            const double row_y = one.y + first_line;
            if (one.held || one.place != CaptionAnchorPlace::Inside) {
                one.leader_y = row_y;
                one.elbow_to_y = row_y;
            } else {
                // The leader stays on the utterance's own row, which is the
                // whole point of it, and when stacking moved the plate off
                // that row an elbow at the plate's edge joins the two.
                one.leader_y = one.anchor_y;
                one.elbow_to_y =
                    std::clamp(one.anchor_y, one.y + geometry.pad_px,
                               one.y + std::max(geometry.pad_px, one.height - geometry.pad_px));
            }
            one.elbow_x = one.leader_to_x == one.x ? one.x : one.x + one.width;
            if (!one.has_leader()) {
                one.elbow_to_y = one.leader_y;
            }
        }
        return placed;
    }

private:
    // Two passes. Down from the top, newest first, each plate pushed below any
    // it would land on, which keeps every plate at or below its own row; then
    // up from the bottom, each plate pulled back inside the item and any it
    // now lands on pushed above it. So plates that crowd the hold stack
    // upward from the bottom edge, the oldest lowest, and nothing is dropped
    // for want of room unless the item cannot hold them all.
    static void stack(std::vector<PlacedCaption>& placed, double height)
    {
        std::vector<std::size_t> order(placed.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(), [&placed](std::size_t a, std::size_t b) {
            if (placed[a].held != placed[b].held) {
                return !placed[a].held;
            }
            if (placed[a].anchor_y != placed[b].anchor_y) {
                return placed[a].anchor_y < placed[b].anchor_y;
            }
            return placed[a].serial > placed[b].serial;
        });

        std::vector<std::size_t> done;
        for (const std::size_t at : order) {
            PlacedCaption& one = placed[at];
            bool moved = true;
            while (moved) {
                moved = false;
                for (const std::size_t other : done) {
                    const PlacedCaption& them = placed[other];
                    if (caption_detail::overlaps_x(one, them) &&
                        caption_detail::overlaps_y(one.y, one.height, them)) {
                        one.y = them.y + them.height + kCaptionGapPx;
                        moved = true;
                    }
                }
            }
            done.push_back(at);
        }

        std::stable_sort(order.begin(), order.end(), [&placed](std::size_t a, std::size_t b) {
            return placed[a].y > placed[b].y;
        });
        done.clear();
        for (const std::size_t at : order) {
            PlacedCaption& one = placed[at];
            one.y = std::min(one.y, std::max(0.0, height - one.height));
            bool moved = true;
            while (moved) {
                moved = false;
                for (const std::size_t other : done) {
                    const PlacedCaption& them = placed[other];
                    if (caption_detail::overlaps_x(one, them) &&
                        caption_detail::overlaps_y(one.y, one.height, them)) {
                        one.y = them.y - kCaptionGapPx - one.height;
                        moved = true;
                    }
                }
            }
            one.y = std::max(one.y, 0.0);
            done.push_back(at);
        }
    }

    std::vector<Caption> captions_;
};

}  // namespace revenant::ui
