// The capture panel's rows: which search to ask the engine for, and how each
// hit it answers with reads in a list.
//
// Qt-free and wire-free, so ui/tests can link it, on the argument
// models/capture_note.h makes. capture_link.cpp copies rpc::CaptureHit into
// CaptureHitIn and hands the rows to QML as they come out of here.
//
// THE DEFAULT QUESTION IS "WHAT JUST HAPPENED HERE": the last hour, over the
// span the engine is capturing now. A wider window is a longer list nobody
// reads, and hits outside the span are off the screen the operator is
// looking at when they open the panel.

#pragma once

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace revenant::ui {

inline constexpr std::int64_t kCaptureWindowNs = 3'600'000'000'000LL;

struct CaptureWindow {
    std::int64_t from_ns = 0;
    std::int64_t to_ns = 0;
    std::int64_t freq_low_hz = 0;
    std::int64_t freq_high_hz = 0;
};

// A span the engine has not reported yet is zero wide; asking with it would
// match nothing and read as "no signals", so the frequency filter is dropped
// instead and the caller sees every hit in the hour.
[[nodiscard]] inline CaptureWindow capture_window(std::int64_t now_ns, double span_low_hz,
                                                  double span_high_hz)
{
    CaptureWindow window;
    window.to_ns = now_ns;
    window.from_ns = now_ns - kCaptureWindowNs;
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
