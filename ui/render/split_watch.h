// Whether the receiver waterfall's before/after split has anything to show.
//
// The processed rows arrive beside the frames, not inside them, so a split
// turned on with no engine half behind it would draw the raw frame across
// the whole width and look like processing that changed nothing. This keeps
// count of the frames since one last came with its processed row, and the
// waterfall says "processed: no data" once that count passes kSplitStaleFrames.
// A single dropped row does not flicker the caption: the engine can miss one
// under load, and a caption that blinks reads as a fault.
//
// No Qt in here, so the test binary can hold it without a scene graph.

#pragma once

#include <algorithm>

namespace revenant::ui {

inline constexpr int kSplitStaleFrames = 16;

class SplitWatch {
public:
    // One frame taken; `matched` when its processed row came with it.
    // Returns whether live() changed.
    bool frame(bool matched)
    {
        const bool before = live();
        missed_ = matched ? 0 : std::min(missed_ + 1, kSplitStaleFrames + 1);
        return live() != before;
    }

    // Back to no data, for a split turned off or a history thrown away.
    bool reset()
    {
        const bool before = live();
        missed_ = kSplitStaleFrames + 1;
        return live() != before;
    }

    [[nodiscard]] bool live() const { return missed_ <= kSplitStaleFrames; }

private:
    int missed_ = kSplitStaleFrames + 1;
};

// Where the divider goes, in logical pixels: on the first column of the
// right half, which takeFrame puts at wide / 2 device columns.
[[nodiscard]] inline double split_divider_x(int device_columns, double logical_width)
{
    if (device_columns <= 0) {
        return 0.0;
    }
    return logical_width * static_cast<double>(device_columns / 2) /
           static_cast<double>(device_columns);
}

}  // namespace revenant::ui
