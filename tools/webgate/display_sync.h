// The phone's spectrum and waterfall drawn the way the desktop draws them.
//
// The owner's decision of 2026-11-03: the phone has no display settings of its
// own. Its scale, contrast and colours are the desktop client's, read from
// where that client stores them, HKCU\Software\Locke Werks\Revenant\display,
// and applied here with the desktop's own rules from ui/render, so the phone
// only quantises and looks colours up.
//
// What the phone is sent, per spectrum row: the waterfall row quantised to the
// desktop waterfall's ends, and the trace row, smoothed as the desktop smooths
// it, quantised to the desktop spectrum's ends. Each carries its own window in
// the frame header (protocol.h). And a "display" event with the colours, sent
// on connect, on resume and whenever they change:
//
//   {"event":"display", "waterfall":"<hex>", "spectrum":"<hex>",
//    "trace":"#rrggbb", "background":"#rrggbb"}
//
// waterfall and spectrum are 256 colours each, 1536 lowercase hex digits with
// entry i at [6i, 6i + 6) as rrggbb, indexed by a row's byte. waterfall is the
// theme through the contrast curve at the pivot in force, which is what the
// desktop waterfall colours with; spectrum is the theme alone, the gradient
// the desktop fills under its trace. trace is the trace line, background the
// plot behind it.
//
// Only a desktop on this machine is followed: the settings are this account's
// registry, and a desktop running on another PC writes its own.

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "render/display_settings.h"

namespace revenant::webgate {

// One value of the desktop's display key, by its name under that key
// ("waterfallContrast"), as the text QSettings stored; nullopt when absent.
// Qt stores doubles and bools as REG_SZ text ("1.44", "true") and integers as
// REG_DWORD, which a reader returns in decimal.
using DisplayValueReader = std::function<std::optional<std::string>(std::string_view name)>;

// The settings from a reader. A value missing or not parseable takes the
// desktop's default, and every value goes through the desktop's clamps.
[[nodiscard]] ui::DisplaySettings read_display_settings(const DisplayValueReader& read);

// Reads HKCU\Software\Locke Werks\Revenant\display. Answers nothing off
// Windows.
[[nodiscard]] DisplayValueReader registry_display_reader();

// The settings, re-read at most once an interval however often they are
// asked for. Any thread.
class DisplaySettingsSource {
public:
    using Clock = std::chrono::steady_clock;

    explicit DisplaySettingsSource(DisplayValueReader reader,
                                   std::function<Clock::time_point()> clock = {},
                                   std::chrono::milliseconds interval = std::chrono::seconds(2));

    [[nodiscard]] ui::DisplaySettings current();

private:
    DisplayValueReader reader_;
    std::function<Clock::time_point()> clock_;
    std::chrono::milliseconds interval_;
    std::mutex mutex_;
    std::optional<Clock::time_point> read_at_;
    ui::DisplaySettings settings_;
};

using Palette = std::array<ui::colour_map::Srgb8, 256>;

// What a waterfall byte b, a fraction b / 255 of its window, is coloured: the
// theme through the curve, sampled as CodeColourer samples it.
[[nodiscard]] Palette waterfall_palette(std::size_t theme, bool black,
                                        const ui::ContrastCurve& curve);

// The desktop trace's fill gradient, colour_at at b / 255.
[[nodiscard]] Palette spectrum_palette(std::size_t theme, bool black);

// The "display" event for these settings with the waterfall coloured at this
// pivot, which is the automatic one when that is on.
[[nodiscard]] std::string display_event(const ui::DisplaySettings& settings, float pivot);

// The frame's facts the scale rules read.
struct DisplayFrame {
    std::uint64_t start = 0;
    std::uint64_t count = 0;
    std::uint32_t rate = 0;
    // Where bin zero is in hertz; a change is a retune, and restarts the
    // trace's averages as the desktop's does.
    double first_bin_hz = 0.0;
    float floor_db = 0.0F;
    float ceiling_db = 0.0F;
};

struct DisplayRow {
    ui::MapEnds trace;
    ui::MapEnds waterfall;
    // The columns as the trace draws them, smoothed.
    std::vector<float> trace_columns;
};

// One phone's share of the desktop's display state: the trace's averages and
// peak hold, the view offsets, and the waterfall's history of levels. Not
// thread-safe; it lives on the phone's strand.
class PhoneDisplay {
public:
    // The phone's ring, web/src/waterfall.ts RING_ROWS, so the history the
    // ends are measured over is the history on its screen.
    static constexpr std::size_t kHistoryRows = 1024;

    explicit PhoneDisplay(const ui::DisplaySettings& settings = {});

    void set_settings(const ui::DisplaySettings& settings);
    [[nodiscard]] const ui::DisplaySettings& settings() const { return settings_; }

    // The phone's view moved, so the zoomed view's offsets start again.
    void view_changed();

    // One row. columns are the view's reduced pixels over the spectrum, the
    // largest bin of each, bins_per_column of them; whole is the frame's bins
    // and shown the ones the view covers.
    [[nodiscard]] DisplayRow take(const DisplayFrame& frame, std::span<const float> whole,
                                  std::span<const float> shown, std::span<const float> columns,
                                  std::size_t bins_per_column);

    // The pivot the waterfall palette is drawn at.
    [[nodiscard]] float drawn_pivot() const { return drawn_pivot_; }

    // The display event for the state now, rebuilt only when it changes.
    [[nodiscard]] const std::string& event();

private:
    ui::DisplaySettings settings_;

    // Trace.
    std::vector<float> smooth_state_;
    ui::PeakHold peak_;
    ui::ViewOffsets view_offsets_;
    std::vector<float> scratch_;
    bool have_last_ = false;
    std::uint64_t last_start_ = 0;
    double last_first_bin_hz_ = 0.0;

    // Waterfall.
    std::deque<std::vector<std::uint16_t>> rows_;
    ui::LevelHistogram histogram_;
    ui::HistoryLevels levels_;
    std::uint64_t last_sample_end_ = 0;
    ui::AutoPivotFollower follower_;
    float drawn_pivot_ = ui::kDefaultCurvePivot;

    bool event_dirty_ = true;
    std::string event_;
};

}  // namespace revenant::webgate
