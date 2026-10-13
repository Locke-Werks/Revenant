// The capture panel's rows: which search to ask the engine for, and how each
// hit it answers with reads in a list.
//
// Qt-free and wire-free, so ui/tests can link it, on the argument
// models/capture_note.h makes. capture_link.cpp copies rpc::CaptureHit into
// CaptureHitIn and hands the rows to QML as they come out of here.
//
// THE DEFAULT QUESTION IS "WHAT JUST HAPPENED HERE": the last hour, over the
// span the engine is capturing now. Longer windows, a typed band and a
// minimum duration are offered, but the panel opens on that default, because
// hits outside the span are off the screen the operator is looking at.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "models/frequency_entry.h"

namespace revenant::ui {

inline constexpr std::int64_t kCaptureWindowNs = 3'600'000'000'000LL;

// The windows and minimum durations the panel offers, short to long. A fixed
// few rather than a typed number: the question is "how far back", and four
// answers cover it without a box to get wrong.
struct CaptureChoice {
    std::string_view label;
    std::int64_t ns;
};

inline constexpr std::array kCaptureWindows{
    CaptureChoice{"15 min", kCaptureWindowNs / 4},
    CaptureChoice{"1 h", kCaptureWindowNs},
    CaptureChoice{"6 h", 6 * kCaptureWindowNs},
    CaptureChoice{"24 h", 24 * kCaptureWindowNs},
};

inline constexpr std::array kCaptureMinDurations{
    CaptureChoice{"any", 0},
    CaptureChoice{"1 s", 1'000'000'000LL},
    CaptureChoice{"5 s", 5'000'000'000LL},
    CaptureChoice{"30 s", 30'000'000'000LL},
};

// A label not in the list reads as the default rather than as zero, so a
// stale label from QML cannot turn into an empty window.
template <std::size_t N>
[[nodiscard]] constexpr std::int64_t capture_choice_ns(const std::array<CaptureChoice, N>& choices,
                                                       std::string_view label,
                                                       std::size_t fallback)
{
    for (const CaptureChoice& choice : choices) {
        if (choice.label == label) {
            return choice.ns;
        }
    }
    return choices[fallback].ns;
}

struct CaptureBand {
    std::int64_t low_hz = 0;
    std::int64_t high_hz = 0;
};

// "162.4M:162.6M", read with the tuning box's parse_frequency so a bare
// number is megahertz here as it is on the dial. Either order is taken.
// Anything else, a single frequency included, is nullopt: the panel says it
// could not read the text rather than searching for something not typed.
[[nodiscard]] inline std::optional<CaptureBand> parse_capture_band(std::string_view text)
{
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos || text.find(':', colon + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto low = parse_frequency(text.substr(0, colon));
    const auto high = parse_frequency(text.substr(colon + 1));
    if (!low || !high || low->hertz == high->hertz) {
        return std::nullopt;
    }
    return CaptureBand{std::min(low->hertz, high->hertz), std::max(low->hertz, high->hertz)};
}

// The occupancy strip's columns: enough to see a burst in a day's window,
// few enough that the answer is a small message.
inline constexpr std::int64_t kCaptureOccupancyBuckets = 120;

struct CaptureWindow {
    std::int64_t from_ns = 0;
    std::int64_t to_ns = 0;
    std::int64_t freq_low_hz = 0;
    std::int64_t freq_high_hz = 0;
};

// A span the engine has not reported yet is zero wide; asking with it would
// match nothing and read as "no signals", so the frequency filter is dropped
// instead and the caller sees every hit in the window.
[[nodiscard]] inline CaptureWindow capture_window(std::int64_t now_ns, std::int64_t window_ns,
                                                  double span_low_hz, double span_high_hz)
{
    CaptureWindow window;
    window.to_ns = now_ns;
    window.from_ns = now_ns - window_ns;
    if (span_high_hz > span_low_hz) {
        window.freq_low_hz = static_cast<std::int64_t>(span_low_hz);
        window.freq_high_hz = static_cast<std::int64_t>(span_high_hz);
    }
    return window;
}

struct CaptureHitIn {
    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0;
    std::int64_t freq_lower_hz = 0;
    std::int64_t freq_upper_hz = 0;
    double confidence = 0.0;
    std::string family;
    std::string replay_uri;
};

struct CaptureRow {
    std::string time;
    std::string duration;
    std::string frequency;
    std::string label;
    std::string confidence;
    std::string replay_uri;
};

// Local wall clock, HH:MM:SS. The offset comes from the caller because the
// zone database is the caller's business and a test needs a fixed one.
[[nodiscard]] inline std::string format_capture_clock(std::int64_t epoch_ns,
                                                      std::int64_t utc_offset_s)
{
    constexpr std::int64_t kDay = 86'400;
    std::int64_t seconds = epoch_ns / 1'000'000'000LL + utc_offset_s;
    seconds = ((seconds % kDay) + kDay) % kDay;
    return std::format("{:02}:{:02}:{:02}", seconds / 3600, (seconds / 60) % 60, seconds % 60);
}

[[nodiscard]] inline std::string format_capture_duration(std::int64_t ns)
{
    if (ns < 0) {
        ns = 0;
    }
    const double seconds = static_cast<double>(ns) / 1e9;
    if (seconds < 10.0) {
        return std::format("{:.1f} s", seconds);
    }
    const auto whole = static_cast<std::int64_t>(seconds);
    if (whole < 60) {
        return std::format("{} s", whole);
    }
    return std::format("{}:{:02}", whole / 60, whole % 60);
}

// MHz to the kilohertz, which is how the tune bar reads a frequency, so a
// row can be matched to the dial by eye. A hit narrower than a kilohertz is
// one number rather than a range that repeats itself.
[[nodiscard]] inline std::string format_capture_range(std::int64_t low_hz, std::int64_t high_hz)
{
    const double low = static_cast<double>(low_hz) / 1e6;
    const double high = static_cast<double>(high_hz) / 1e6;
    if (high_hz - low_hz < 1000) {
        return std::format("{:.3f} MHz", (low + high) / 2.0);
    }
    return std::format("{:.3f} to {:.3f} MHz", low, high);
}

[[nodiscard]] inline CaptureRow capture_row(const CaptureHitIn& hit, std::int64_t utc_offset_s)
{
    CaptureRow row;
    row.time = format_capture_clock(hit.start_ns, utc_offset_s);
    row.duration = format_capture_duration(hit.end_ns - hit.start_ns);
    row.frequency = format_capture_range(hit.freq_lower_hz, hit.freq_upper_hz);
    // An unclassified hit is still a signal; an empty cell would read as a
    // row that failed to load.
    row.label = hit.family.empty() ? std::string{"signal"} : hit.family;
    row.confidence = std::format("{}%", static_cast<int>(std::clamp(hit.confidence, 0.0, 1.0) * 100.0 + 0.5));
    row.replay_uri = hit.replay_uri;
    return row;
}

// Newest first, because the panel answers "what did I just miss". Stable, so
// two hits starting together keep the engine's order.
[[nodiscard]] inline std::vector<CaptureRow> capture_rows(std::vector<CaptureHitIn> hits,
                                                          std::int64_t utc_offset_s)
{
    std::stable_sort(hits.begin(), hits.end(), [](const CaptureHitIn& a, const CaptureHitIn& b) {
        return a.start_ns > b.start_ns;
    });
    std::vector<CaptureRow> rows;
    rows.reserve(hits.size());
    for (const CaptureHitIn& hit : hits) {
        rows.push_back(capture_row(hit, utc_offset_s));
    }
    return rows;
}

}  // namespace revenant::ui
