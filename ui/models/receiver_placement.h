// Where the receivers are drawn, and how their panel lays itself out there.
//
// THE OWNER'S CALL, 2026-09-23, after a live playtest: the receivers start
// docked in the main window, under the span, and pop out into a window of
// their own on demand and back in again. They had been a second top-level
// window since 2026-09-22, which is the right place for a second screen and
// the wrong default for one: two windows to arrange before the radio is
// usable, and two windows the one GUI thread has to pace. The choice is
// remembered across runs (models/settings.h), and a smoke run neither reads
// nor writes it, so its photographs start docked.
//
// ONE PANEL, TWO ARRANGEMENTS. Docked, the panel is a strip across the bottom
// of the main window, much wider than it is tall, and stacking RDS, decoding
// and audio under the fine-tuning display would leave the display no height.
// So a wide panel puts those three in a column of their own on the right, and
// a panel shaped like the popped-out window stacks them as it always did.
//
// This header holds no Qt; ui/tests links it.

#pragma once

#include <algorithm>

namespace revenant::ui {

// Below this width the rack, the receiver's controls and a third column do not
// all fit at a readable width, whatever the height.
inline constexpr double kPanelSideBySideMinWidth = 1000.0;

// How much wider than tall the panel has to be before the third column pays
// for itself. The docked strip at its default is about four to one; the
// popped-out window at its default, 1040 by 680, is about one and a half.
inline constexpr double kPanelSideBySideAspect = 2.2;

// RDS, decoding and audio in a column of their own beside the receiver's
// controls, rather than under them.
[[nodiscard]] constexpr bool receiver_panel_side_by_side(double width, double height)
{
    return height > 0.0 && width >= kPanelSideBySideMinWidth &&
           width >= kPanelSideBySideAspect * height;
}

// THE DOCK'S HEIGHT. A fraction of what the span and the dock share until the
// operator drags the divider, then what they dragged it to, held inside two
// floors: the dock keeps enough for the receiver's dial, mode, fine-tuning
// display and its waterfall, and the span keeps enough to be a spectrum and a
// waterfall rather than a strip.
inline constexpr double kDockMinHeight = 240.0;
inline constexpr double kDockDefaultFraction = 0.42;
inline constexpr double kSpanKeepsHeight = 220.0;

// remembered is the height the operator last dragged the dock to, zero when
// they never have; available is the height the span and the dock share.
[[nodiscard]] constexpr double dock_height(double remembered, double available)
{
    const double wanted = remembered > 0.0 ? remembered : kDockDefaultFraction * available;
    const double most = std::max(kDockMinHeight, available - kSpanKeepsHeight);
    return std::clamp(wanted, kDockMinHeight, most);
}

}  // namespace revenant::ui
