// How the receiver's own waterfall colours a row, so the operator's one
// contrast setting drives it the way it drives the span waterfall.
//
// The span waterfall's ends come from its history (render/waterfall_scale.h);
// this one's come from each frame, since the engine already measures the
// passband per frame. Padded is the engine's floor and ceiling, map_ends with
// no headroom as before; tight is the frame's own low and high percentiles.
// From there the rule is the span's exactly: contrast 0 to 1 blends padded
// toward tight, and past 1 the S curve about the pivot does the rest. The
// colouring itself is the span's CodeColourer, so for the same ends and curve
// the two waterfalls draw a level in the same colour.
//
// The pins stay span-only. They are absolute decibels chosen against the wide
// view and mean nothing on a narrow passband with its own floor.
//
// This header holds no Qt; ui/tests links it.

#pragma once

#include <algorithm>
#include <cstdint>

#include "render/spectrum_scale.h"
#include "render/waterfall_scale.h"

namespace revenant::ui {

// One row's two pairs of ends, kept with the row so a contrast change can
// recolour the history from its codes.
struct PassbandRowScale {
    MapEnds padded;
    MapEnds tight;
};

[[nodiscard]] inline PassbandRowScale passband_row_scale(float floor_db, float ceiling_db,
                                                         float low_db, float high_db)
{
    PassbandRowScale scale;
    scale.padded = map_ends(floor_db, ceiling_db, 0.0F);
    // A frame with no percentiles measured carries zeros, which would make a
    // one-decibel map at 0 dBFS; it falls back to padded instead.
    scale.tight = high_db > low_db
                      ? tight_ends(low_db, high_db, kWaterfallTightGuardDb, ScalePins{})
                      : scale.padded;
    return scale;
}

[[nodiscard]] inline MapEnds passband_ends(const PassbandRowScale& scale, float contrast)
{
    return blend_ends(scale.padded, scale.tight, std::min(contrast, 1.0F));
}

inline void colour_passband_row(const std::uint16_t* codes, int count,
                                const PassbandRowScale& scale, float contrast, float pivot,
                                std::uint32_t* pixels)
{
    CodeColourer(passband_ends(scale, contrast), curve_for(contrast, pivot))
        .row(codes, pixels, count);
}

}  // namespace revenant::ui
