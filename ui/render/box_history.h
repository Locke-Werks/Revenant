// The detection boxes the span waterfall has drawn, kept as history.
//
// WHAT THE OWNER ASKED FOR, 2026-09-23. A box on the waterfall is fixed once
// it is drawn: it stays on the rows it was drawn on and scrolls off with them.
// Until then the waterfall drew only the tracks the detector was still
// reporting, so a box vanished from the history the moment the detector's
// hold ran out, which is exactly when the signal it marked had become a
// finished thing worth reading off the history. The spectrum's bracket and its
// label are a different display and still follow the live tracks alone.
//
// WHAT A RECORD IS. A segment: one track's frequency extent in absolute hertz
// and the stretch of the engine's sample clock it covers. A live track grows
// its newest segment upward as the detector keeps finding it, which is the box
// being drawn. Once drawn, a segment never changes: when the track's extent
// moves by more than the caller's tolerance, the segment is closed where it
// was and a new one carries on from the next frame, so a drifting track leaves
// a staircase rather than a rectangle that redraws its own past at today's
// estimate. When the detector stops reporting the track, its last segment is
// closed and stays exactly as it was.
//
// ABSOLUTE HERTZ, SO A RETUNE MOVES NOTHING HERE. The waterfall's pixels shift
// with the front end (render/history_shift.h), and a segment placed on the
// current span's axis lands on the rows it was drawn on, shifted with them.
//
// WHAT CLOSES A SEGMENT'S TIME, WHICH IS last_detected. Never last_seen: the
// two differ by the hold for a held track, and a box drawn to last_seen would
// claim the signal was there while it was being held. The waterfall's header
// has the long form of this; it is the same rule.
//
// Bounded two ways: the waterfall forgets whatever ended before the oldest
// row it still holds, and there is a ceiling on the count in case something
// holds a history of pathological size.
//
// This header holds no Qt; ui/tests links it.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace revenant::ui {

// One track as the detector reports it now.
struct TrackSighting {
    std::uint64_t id = 0;

    // The occupied band's edges, absolute hertz.
    std::int64_t low_hz = 0;
    std::int64_t high_hz = 0;

    // The detector's own clock. Both are one past the last sample of the frame
    // the evidence came from, as rpc::Detection carries them.
    std::uint64_t first_seen = 0;
    std::uint64_t last_detected = 0;

    // Inside another track's band, which is drawn in its own hue.
    bool merged = false;
};

// One drawn stretch of one track.
struct BoxSegment {
    std::uint64_t id = 0;
    std::int64_t low_hz = 0;
    std::int64_t high_hz = 0;

    // The samples it covers, in the same one-past convention as the sighting.
    // A segment after the first starts one sample past the one before it
    // ended, so the two never claim the same row.
    std::uint64_t from_sample = 0;
    std::uint64_t to_sample = 0;

    bool merged = false;

    // Still being drawn: the track is reported and this is its newest
    // segment. Everything else is fixed.
    bool open = true;
};

// Past this many segments the oldest closed ones go first.
inline constexpr std::size_t kBoxHistoryLimit = 4096;

class BoxHistory {
public:
    // Folds in the detector's current list. tolerance_hz is how far either
    // edge may move, or the merged flag change, before the extent counts as a
    // different one and a new segment starts; the waterfall passes two
    // pixels' worth, so an estimate that wanders inside what the eye can see
    // does not cut a box into slices.
    void observe(const std::vector<TrackSighting>& live, double tolerance_hz)
    {
        // Which open segments this list still reports. Anything left false at
        // the end belongs to a track the detector let go, and is fixed.
        std::vector<bool> seen(segments_.size(), false);

        for (const TrackSighting& track : live) {
            const std::size_t at = newest_of(track.id);
            if (at == kNone) {
                BoxSegment first;
                first.id = track.id;
                first.low_hz = track.low_hz;
                first.high_hz = track.high_hz;
                first.from_sample = track.first_seen;
                first.to_sample = std::max(track.last_detected, track.first_seen);
                first.merged = track.merged;
                segments_.push_back(first);
                seen.push_back(true);
                continue;
            }

            BoxSegment& newest = segments_[at];
            if (!newest.open) {
                // Reported again after it was let go, which a filter on the
                // list can do. What was drawn stays drawn; a new segment starts
                // after it, never over it.
                if (track.last_detected > newest.to_sample) {
                    segments_.push_back(next_after(newest, track));
                    seen.push_back(true);
                }
                continue;
            }
            seen[at] = true;

            const bool moved =
                static_cast<double>(std::llabs(track.low_hz - newest.low_hz)) > tolerance_hz ||
                static_cast<double>(std::llabs(track.high_hz - newest.high_hz)) > tolerance_hz ||
                track.merged != newest.merged;
            if (!moved) {
                newest.to_sample = std::max(newest.to_sample, track.last_detected);
                continue;
            }

            // A new extent needs new rows to be drawn on. Without any, the
            // estimate changed while the signal was not being found, and there
            // is nothing to draw it over.
            if (track.last_detected > newest.to_sample) {
                newest.open = false;
                segments_.push_back(next_after(newest, track));
                seen.push_back(true);
            }
        }

        for (std::size_t i = 0; i < seen.size(); ++i) {
            if (!seen[i]) {
                segments_[i].open = false;
            }
        }
        trim();
    }

    // Forgets every closed segment that ended before `oldest_sample`, which
    // the waterfall passes as the first sample of the oldest row it holds:
    // those have scrolled off the bottom and nothing will draw them again.
    void forget_before(std::uint64_t oldest_sample)
    {
        std::erase_if(segments_, [oldest_sample](const BoxSegment& segment) {
            return !segment.open && segment.to_sample <= oldest_sample;
        });
    }

    // A new stream, or a history that has been thrown away.
    void clear() { segments_.clear(); }

    [[nodiscard]] const std::vector<BoxSegment>& segments() const { return segments_; }

private:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    [[nodiscard]] std::size_t newest_of(std::uint64_t id) const
    {
        for (std::size_t i = segments_.size(); i > 0; --i) {
            if (segments_[i - 1].id == id) {
                return i - 1;
            }
        }
        return kNone;
    }

    [[nodiscard]] static BoxSegment next_after(const BoxSegment& before, const TrackSighting& track)
    {
        BoxSegment next;
        next.id = track.id;
        next.low_hz = track.low_hz;
        next.high_hz = track.high_hz;
        next.from_sample = before.to_sample + 1;
        next.to_sample = std::max(track.last_detected, next.from_sample);
        next.merged = track.merged;
        return next;
    }

    void trim()
    {
        if (segments_.size() <= kBoxHistoryLimit) {
            return;
        }
        // Oldest first, which is the order they were appended in, and never an
        // open one: that is a box still being drawn.
        std::size_t excess = segments_.size() - kBoxHistoryLimit;
        std::erase_if(segments_, [&excess](const BoxSegment& segment) {
            if (excess == 0 || segment.open) {
                return false;
            }
            --excess;
            return true;
        });
    }

    std::vector<BoxSegment> segments_;
};

}  // namespace revenant::ui
