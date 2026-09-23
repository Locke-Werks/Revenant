// models/frame_stats.h, the arithmetic behind `revenant-ui --frame-stats`.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS, in its own
// comment, for the reason test_audio_ring.cpp gives.
//
// The wrong implementations on this path are an interpolated percentile,
// which reports frame times no frame had; a miss threshold at one refresh
// period, which calls every jittered on-time frame late; counting frames
// from before the measured window opened, which puts shader compilation into
// the steady state; and counting every take as a frame drawn, which hides
// the frames the display threw away.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "models/frame_stats.h"

using Catch::Approx;
using revenant::ui::budget_met;
using revenant::ui::frame_budget_ms;
using revenant::ui::frame_stats_json;
using revenant::ui::frame_stats_line;
using revenant::ui::FrameCost;
using revenant::ui::FrameItem;
using revenant::ui::FramePhase;
using revenant::ui::FrameRecorder;
using revenant::ui::FrameReport;
using revenant::ui::FrameRunInfo;
using revenant::ui::FrameWindowReport;
using revenant::ui::g_frame_recorder;
using revenant::ui::json_escape;
using revenant::ui::missed_refresh;
using revenant::ui::refreshes_missed;
using revenant::ui::summarise;

namespace {

constexpr std::int64_t kMs = 1'000'000;

// A window swapping every period_ms from start for count frames.
void swap_run(FrameRecorder& recorder, int window, std::int64_t start_ns, double period_ms,
              int count)
{
    for (int i = 0; i < count; ++i) {
        recorder.swapped(window, start_ns + static_cast<std::int64_t>(period_ms * i * 1.0e6));
    }
}

[[nodiscard]] FrameRunInfo at_hz(double hz)
{
    FrameRunInfo run;
    run.refresh_hz = hz;
    return run;
}

}  // namespace

// Rejects an interpolated percentile. Over 1..100 nearest rank gives whole
// samples; interpolation would give 95.05 for p95, a frame nobody drew.
TEST_CASE("percentiles are nearest rank and name a sample that happened", "[frame_stats]")
{
    std::vector<double> samples;
    for (int i = 100; i >= 1; --i) {
        samples.push_back(static_cast<double>(i));
    }
    const auto s = summarise(samples);
    CHECK(s.count == 100);
    CHECK(s.mean == Approx(50.5));
    CHECK(s.p50 == 50.0);
    CHECK(s.p95 == 95.0);
    CHECK(s.p99 == 99.0);
    CHECK(s.max == 100.0);

    const std::vector<double> two{2.0, 1.0};
    CHECK(summarise(two).p50 == 1.0);
    CHECK(summarise(two).p99 == 2.0);
}

// Rejects a summary that divides by zero or invents a sample for an empty
// series; a window that drew nothing has to say so.
TEST_CASE("an empty series summarises to nothing", "[frame_stats]")
{
    const auto s = summarise(std::vector<double>{});
    CHECK(s.count == 0);
    CHECK(s.max == 0.0);
}

TEST_CASE("the budget is one refresh period", "[frame_stats]")
{
    CHECK(frame_budget_ms(60.0) == Approx(16.6667).epsilon(1e-4));
    CHECK(frame_budget_ms(144.0) == Approx(6.9444).epsilon(1e-4));
    CHECK(frame_budget_ms(0.0) == 0.0);
}

// Rejects a threshold at one period: 17 ms at 60 Hz is a frame on time with
// a little jitter, and calling it late would fail every display that keeps up.
TEST_CASE("a miss is an interval past one and a half periods", "[frame_stats]")
{
    const double budget = frame_budget_ms(60.0);
    CHECK_FALSE(missed_refresh(17.0, budget));
    CHECK_FALSE(missed_refresh(24.9, budget));
    CHECK(missed_refresh(25.1, budget));
    CHECK(missed_refresh(33.4, budget));

    CHECK(refreshes_missed(16.7, budget) == 0);
    CHECK(refreshes_missed(33.4, budget) == 1);
    CHECK(refreshes_missed(50.0, budget) == 2);

    // No budget, no verdict: a screen that reported no refresh rate cannot
    // have a frame be late against it.
    CHECK_FALSE(missed_refresh(1000.0, 0.0));
}

// Rejects keeping frames from before open(): the first seconds are shader
// compilation and receivers opening, and they are not the steady state.
TEST_CASE("only frames inside the measured window are kept", "[frame_stats]")
{
    FrameRecorder recorder;
    const int w = recorder.add_window("main");
    swap_run(recorder, w, 0, 50.0, 20);  // slow start-up frames, never opened
    recorder.open(1000 * kMs);
    recorder.close(1100 * kMs);
    // Eleven swaps 10 ms apart from 1000 ms; the last lands on the close and
    // is outside, as are the slow frames of a window closing after it.
    swap_run(recorder, w, 1000 * kMs, 10.0, 11);
    swap_run(recorder, w, 1150 * kMs, 50.0, 5);

    const FrameReport report = recorder.report(at_hz(100.0));
    REQUIRE(report.windows.size() == 1);
    const FrameWindowReport& main = report.windows.front();
    CHECK(main.frames == 10);
    // The first frame inside has its predecessor outside, so its interval is
    // not one the window measured.
    CHECK(main.interval_ms.count == 9);
    CHECK(main.interval_ms.max == Approx(10.0));
    CHECK(main.over_budget == 0);
    CHECK(budget_met(main));
}

// Rejects a verdict that ignores how often the budget was missed, and one
// that fails on a single stutter in a hundred frames.
TEST_CASE("the budget is met with at most one interval in a hundred over it", "[frame_stats]")
{
    const auto run_with_misses = [](int misses) {
        FrameRecorder recorder;
        const int w = recorder.add_window("main");
        recorder.open(0);
        std::int64_t t = 0;
        recorder.swapped(w, t);
        for (int i = 0; i < 200; ++i) {
            t += (i < misses ? 20 : 10) * kMs;
            recorder.swapped(w, t);
        }
        return recorder.report(at_hz(100.0)).windows.front();
    };

    const auto two = run_with_misses(2);
    CHECK(two.interval_ms.count == 200);
    CHECK(two.over_budget == 2);
    CHECK(two.missed_refreshes == 2);
    CHECK(budget_met(two));

    const auto three = run_with_misses(3);
    CHECK(three.over_budget == 3);
    CHECK_FALSE(budget_met(three));
}

// Rejects adding sync and render from different frames, and counting work
// against 1.5 periods: work has no vsync to be quantised by.
TEST_CASE("frame work is the frame's own sync plus render against one period",
          "[frame_stats]")
{
    FrameRecorder recorder;
    const int w = recorder.add_window("main");
    recorder.open(0);
    std::int64_t t = 0;
    const auto frame = [&](int sync_ms, int render_ms) {
        recorder.sync_began(w, t);
        t += sync_ms * kMs;
        recorder.sync_ended(w, t);
        recorder.render_began(w, t);
        t += render_ms * kMs;
        recorder.render_ended(w, t);
        recorder.swapped(w, t);
    };
    frame(2, 3);
    frame(6, 5);  // 11 ms against a 10 ms period
    frame(1, 1);

    const auto main = recorder.report(at_hz(100.0)).windows.front();
    CHECK(main.sync_ms.count == 3);
    CHECK(main.sync_ms.max == Approx(6.0));
    CHECK(main.render_ms.max == Approx(5.0));
    CHECK(main.work_over_budget == 1);
}

// Rejects attributing a late frame to the whole run's averages. The late
// interval here was spent waiting to be asked for the frame, and only the
// missed_ series can say so; the on-time frames spent theirs in beginFrame.
TEST_CASE("a late frame is split into waiting to be asked, beginning and presenting",
          "[frame_stats]")
{
    FrameRecorder recorder;
    const int w = recorder.add_window("main");
    recorder.open(0);
    std::int64_t t = 0;
    recorder.swapped(w, t);
    const auto frame = [&](int request_ms, int begin_ms, int present_ms) {
        t += request_ms * kMs;
        recorder.frame_began(w, t);
        t += begin_ms * kMs;
        recorder.sync_began(w, t);
        recorder.sync_ended(w, t);
        recorder.render_began(w, t);
        recorder.render_ended(w, t);
        t += present_ms * kMs;
        recorder.swapped(w, t);
    };
    frame(1, 8, 1);   // 10 ms, on time at 100 Hz
    frame(17, 2, 1);  // 20 ms, late, and nobody asked for it until 17 ms in
    frame(1, 8, 1);

    const auto main = recorder.report(at_hz(100.0)).windows.front();
    CHECK(main.interval_ms.count == 3);
    CHECK(main.over_budget == 1);
    CHECK(main.begin_ms.max == Approx(8.0));
    CHECK(main.missed_request_ms.count == 1);
    CHECK(main.missed_request_ms.max == Approx(17.0));
    CHECK(main.missed_begin_ms.max == Approx(2.0));
    CHECK(main.missed_present_ms.max == Approx(1.0));
}

// Rejects inventing a split the loop did not report. With no frame begin,
// the whole wait is counted as waiting to be asked.
TEST_CASE("a loop with no frame begin counts the whole wait as the request",
          "[frame_stats]")
{
    FrameRecorder recorder;
    const int w = recorder.add_window("main");
    recorder.open(0);
    recorder.swapped(w, 0);
    recorder.sync_began(w, 6 * kMs);
    recorder.swapped(w, 10 * kMs);
    const auto main = recorder.report(at_hz(100.0)).windows.front();
    CHECK(main.request_ms.max == Approx(6.0));
    CHECK(main.begin_ms.max == Approx(0.0));
}

// Rejects calling every take a frame drawn. Two takes before one sync are
// one frame on screen and one the display never showed.
TEST_CASE("a take followed by a sync is drawn and a take replaced first is not",
          "[frame_stats]")
{
    FrameRecorder recorder;
    recorder.open(0);
    const auto take = [&](std::int64_t t) {
        recorder.item_cost(FrameItem::Waterfall, FramePhase::Take, t, t + kMs);
    };
    const auto sync = [&](std::int64_t t) {
        recorder.item_cost(FrameItem::Waterfall, FramePhase::Sync, t, t + 2 * kMs);
    };
    take(10 * kMs);
    take(20 * kMs);
    sync(30 * kMs);  // draws the second; the first was superseded
    sync(40 * kMs);  // a resize or an overlay change, no new frame
    take(50 * kMs);
    sync(60 * kMs);

    const FrameReport report = recorder.report(at_hz(60.0));
    const auto& waterfall = report.items[static_cast<std::size_t>(FrameItem::Waterfall)];
    CHECK(waterfall.takes == 3);
    CHECK(waterfall.syncs == 3);
    CHECK(waterfall.drawn == 2);
    CHECK(waterfall.superseded == 1);
    CHECK(waterfall.take_ms.max == Approx(1.0));
    CHECK(waterfall.sync_ms.max == Approx(2.0));

    const auto& spectrum = report.items[static_cast<std::size_t>(FrameItem::Spectrum)];
    CHECK(spectrum.takes == 0);
}

// Rejects a cost scope that measures when nobody installed a recorder,
// which is the promise that the instrument costs nothing when it is off.
TEST_CASE("a cost scope reports only to an installed recorder", "[frame_stats]")
{
    FrameRecorder recorder;
    recorder.open(0);
    {
        const FrameCost cost(FrameItem::Spectrum, FramePhase::Take);
    }
    g_frame_recorder.store(&recorder);
    {
        const FrameCost cost(FrameItem::Spectrum, FramePhase::Take);
    }
    g_frame_recorder.store(nullptr);
    {
        const FrameCost cost(FrameItem::Spectrum, FramePhase::Take);
    }
    const auto report = recorder.report(at_hz(60.0));
    CHECK(report.items.front().takes == 1);
}

TEST_CASE("the JSON names every field and writes null for a series with nothing in it",
          "[frame_stats]")
{
    FrameRecorder recorder;
    const int w = recorder.add_window("main \"span\"");
    recorder.set_window_size(w, 2560, 1369);
    recorder.open(0);
    swap_run(recorder, w, 0, 6.944, 10);

    FrameRunInfo run = at_hz(144.0);
    run.platform = "windows";
    run.graphics_api = "d3d11";
    run.engine_frames_received = 3050;
    run.engine_frames_to_display = 1440;
    const std::string json = frame_stats_json(recorder.report(run));

    CHECK(json.find(R"("budget_ms": 6.944)") != std::string::npos);
    CHECK(json.find(R"("name": "main \"span\"")") != std::string::npos);
    CHECK(json.find(R"("width": 2560)") != std::string::npos);
    CHECK(json.find(R"("budget_met": true)") != std::string::npos);
    CHECK(json.find(R"("gpu_ms": null)") != std::string::npos);
    CHECK(json.find(R"("frames_received": 3050)") != std::string::npos);
    CHECK(json.find(R"("name": "passband_waterfall")") != std::string::npos);
    CHECK(json.front() == '{');

    const std::string line = frame_stats_line(recorder.report(run));
    CHECK(line.find("MET") != std::string::npos);
    CHECK(line.find("144.000 Hz") != std::string::npos);
}

TEST_CASE("strings are escaped for JSON", "[frame_stats]")
{
    CHECK(json_escape("a\\b") == R"("a\\b")");
    CHECK(json_escape("tab\there") == R"("tab\there")");
    CHECK(json_escape(std::string(1, '\x01')) == R"("\u0001")");
}
