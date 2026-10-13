// capture_rows and capture_window: what the capture panel asks for and how it
// lists the answer.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS. Here they are a
// list oldest first, which buries the hit the operator came for; an
// unclassified hit with a blank label, which reads as a row that failed; and a
// span not yet known sent as a zero-wide filter, which answers "nothing".

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "models/capture_browser.h"

using revenant::ui::capture_row;
using revenant::ui::capture_rows;
using revenant::ui::capture_window;
using revenant::ui::CaptureHitIn;
using revenant::ui::format_capture_duration;
using revenant::ui::format_capture_range;
using revenant::ui::capture_choice_ns;
using revenant::ui::kCaptureMinDurations;
using revenant::ui::kCaptureWindowNs;
using revenant::ui::kCaptureWindows;
using revenant::ui::parse_capture_band;

namespace {

constexpr std::int64_t kSecond = 1'000'000'000LL;

[[nodiscard]] CaptureHitIn hit_at(std::int64_t start_s, const char* uri)
{
    CaptureHitIn hit;
    hit.start_ns = start_s * kSecond;
    hit.end_ns = hit.start_ns + 3 * kSecond;
    hit.freq_lower_hz = 146'520'000;
    hit.freq_upper_hz = 146'533'000;
    hit.confidence = 0.874;
    hit.replay_uri = uri;
    return hit;
}

}  // namespace

TEST_CASE("rows come out newest first, carrying their replay URI", "[capture_browser]")
{
    // Rejects: the engine's order passed through, which is oldest first.
    const auto rows =
        capture_rows({hit_at(100, "capture://a"), hit_at(300, "capture://c"), hit_at(200, "capture://b")}, 0);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].replay_uri == "capture://c");
    CHECK(rows[1].replay_uri == "capture://b");
    CHECK(rows[2].replay_uri == "capture://a");
}

TEST_CASE("a row reads time, duration, range, label and confidence", "[capture_browser]")
{
    CaptureHitIn hit = hit_at(3600 * 13 + 62, "capture://x");
    hit.family = "nfm";
    const auto row = capture_row(hit, -6 * 3600);
    CHECK(row.time == "07:01:02");
    CHECK(row.duration == "3.0 s");
    CHECK(row.frequency == "146.520 to 146.533 MHz");
    CHECK(row.label == "nfm");
    CHECK(row.confidence == "87%");
}

TEST_CASE("an unclassified hit is called a signal", "[capture_browser]")
{
    // Rejects: an empty family drawn as an empty cell.
    CHECK(capture_row(hit_at(0, ""), 0).label == "signal");
}

TEST_CASE("durations and ranges stay short", "[capture_browser]")
{
    CHECK(format_capture_duration(42 * kSecond) == "42 s");
    CHECK(format_capture_duration(125 * kSecond) == "2:05");
    CHECK(format_capture_duration(-kSecond) == "0.0 s");
    // Rejects: "146.520 to 146.520 MHz" for a carrier.
    CHECK(format_capture_range(146'520'000, 146'520'200) == "146.520 MHz");
}

TEST_CASE("the window is the last hour over the span", "[capture_browser]")
{
    const auto window = capture_window(10 * kCaptureWindowNs, kCaptureWindowNs, 144e6, 146e6);
    CHECK(window.to_ns == 10 * kCaptureWindowNs);
    CHECK(window.from_ns == 9 * kCaptureWindowNs);
    CHECK(window.freq_low_hz == 144'000'000);
    CHECK(window.freq_high_hz == 146'000'000);
}

TEST_CASE("an unknown span drops the frequency filter", "[capture_browser]")
{
    // Rejects: a zero-wide filter, which matches nothing.
    const auto window = capture_window(kCaptureWindowNs, kCaptureWindowNs, 0.0, 0.0);
    CHECK(window.freq_low_hz == 0);
    CHECK(window.freq_high_hz == 0);
}

TEST_CASE("the window is the one chosen", "[capture_browser]")
{
    const std::int64_t day = capture_choice_ns(kCaptureWindows, "24 h", 1);
    CHECK(day == 24 * kCaptureWindowNs);
    const auto window = capture_window(100 * kCaptureWindowNs, day, 0.0, 0.0);
    CHECK(window.from_ns == 76 * kCaptureWindowNs);
    CHECK(capture_choice_ns(kCaptureWindows, "15 min", 1) == kCaptureWindowNs / 4);
    CHECK(capture_choice_ns(kCaptureMinDurations, "5 s", 0) == 5 * kSecond);
}

TEST_CASE("an unknown choice falls back to the default", "[capture_browser]")
{
    // Rejects: a stale label read as zero, an empty window that finds nothing.
    CHECK(capture_choice_ns(kCaptureWindows, "2 h", 1) == kCaptureWindowNs);
    CHECK(capture_choice_ns(kCaptureMinDurations, "", 0) == 0);
}

TEST_CASE("a typed band reads like the dial", "[capture_browser]")
{
    const auto band = parse_capture_band("162.4M:162.6M");
    REQUIRE(band.has_value());
    CHECK(band->low_hz == 162'400'000);
    CHECK(band->high_hz == 162'600'000);

    // A bare number is megahertz, as on the dial, and order does not matter.
    const auto bare = parse_capture_band(" 146.6 : 146.4 ");
    REQUIRE(bare.has_value());
    CHECK(bare->low_hz == 146'400'000);
    CHECK(bare->high_hz == 146'600'000);

    const auto kilo = parse_capture_band("7100k:7200k");
    REQUIRE(kilo.has_value());
    CHECK(kilo->low_hz == 7'100'000);
}

TEST_CASE("a band that cannot be read is refused, not guessed", "[capture_browser]")
{
    // Rejects: one frequency taken as a zero-wide band, and junk as no filter.
    CHECK_FALSE(parse_capture_band("162.4M").has_value());
    CHECK_FALSE(parse_capture_band("162.4M:162.4M").has_value());
    CHECK_FALSE(parse_capture_band("162.4M:").has_value());
    CHECK_FALSE(parse_capture_band("a:b").has_value());
    CHECK_FALSE(parse_capture_band("1M:2M:3M").has_value());
}
