// The band plan's region checkboxes as mask arithmetic, Qt-free so ui/tests
// can hold it. The mask is models/band_plan.h's Region.

#pragma once

#include <cstdint>

#include "models/band_plan.h"

namespace revenant::ui {

// The mask with one region switched on or off, except that the last region is
// never switched off. An empty mask is a band bar and a band menu with nothing
// in them, which reads as a fault rather than a choice, and the checkbox that
// produced it would be the only way back.
[[nodiscard]] constexpr std::uint32_t toggle_region(std::uint32_t mask, std::uint32_t region,
                                                    bool on)
{
    const std::uint32_t next = on ? (mask | region) : (mask & ~region);
    return (next & kRegionUSCA) == 0 ? mask : next;
}

// A stored mask as the bar should take it: bits no region names are dropped,
// and nothing left is read as both, the install default, for the reason above.
[[nodiscard]] constexpr std::uint32_t restore_region(std::uint32_t stored)
{
    const std::uint32_t known = stored & kRegionUSCA;
    return known == 0 ? kRegionUSCA : known;
}

}  // namespace revenant::ui
