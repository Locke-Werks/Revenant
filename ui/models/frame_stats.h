// How long the client takes to draw a frame, and whether that fits in one
// refresh of the screen it is on.
//
// WHY THIS EXISTS
//
// M2 closes when "the render pipeline hits its frame budget with the full
// target load", and the target is the spectrum and the waterfall locked to
// monitor refresh. Until this header nothing in the tree measured the
// client's frame time at any load, so the criterion could be neither met nor
// failed. This is the arithmetic half of the instrument: timestamps in,
// percentiles and counts out, and the JSON that `revenant-ui --frame-stats
// FILE` writes. render/frame_probe.cpp is the Qt half, which reads the
// timestamps off QQuickWindow and hands them here.
//
// WHY IT HOLDS NO Qt. ui/tests links it, for the reason
// render/history_resize.h gives: the rules that can be wrong here are a
// percentile, a threshold and a count, and a test with no window has to be
// able to reach all three.
//
// WHAT "OVER BUDGET" MEANS
//
// The budget is one refresh period, 1000 / refresh_hz milliseconds. Under
// vsync a frame interval lands on a whole number of periods plus jitter, so
// an interval of 1.02 periods is a frame on time and one of 1.98 periods is a
// frame that missed a refresh. The line is drawn halfway, at 1.5 periods,
// which is the only place that separates the two populations without
// counting jitter as a miss. A threshold at exactly one period would call
// every other frame late on a display that is keeping up.
//
// The budget is MET when no more than one interval in a hundred is over it,
// which is the same statement as a p99 interval under 1.5 periods. One in a
// hundred is a stutter a second at 100 Hz; the choice is a line somebody can
// argue with and is written down here so it is argued with rather than
// assumed.
//
// Frame WORK, the CPU time the scene graph spends synchronising and
// recording a frame, is reported beside the interval and counted over the
// budget at one period, not 1.5: work has no vsync quantisation to allow for,
// and a frame whose work alone is longer than a period cannot be on time.
//
// WHAT A FRAME ARRIVING AND A FRAME BEING DRAWN MEAN
//
// Each display item reports a TAKE when it ingests a frame from the engine,
// on the GUI thread, and a SYNC when the scene graph asks it for its node,
// on the render thread. A take followed by a sync is a frame that reached
// the screen. A second take before the sync replaced the first, which was
// then never drawn. Those two counts are the item's drawn and superseded,
// and they are the client-side answer to "the engine sent it, did anybody
// see it".

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace revenant::ui {

// Seconds of frames thrown away after the first engine frame reaches the
// display. The first second of a run is shader compilation, the first
// texture uploads at full size, the rack's receivers opening one by one and
// the passband subscription starting; none of it is the steady state the
// criterion is about. Three rather than one because the rack's eight
// receivers were measured opening over about two seconds of a smoke run.
inline constexpr double kFrameWarmupSeconds = 3.0;

// An interval counts as a missed refresh above this many periods. See the
// header note.
inline constexpr double kMissedRefreshPeriods = 1.5;

// The budget is met with at most this fraction of intervals over it.
inline constexpr double kBudgetMissAllowance = 0.01;

// Samples held per series. A 60 s run at 240 Hz is 14400, so this covers an
// hour at that rate; past it a series stops growing and says so, rather
// than a forgotten run eating memory.
inline constexpr std::size_t kFrameSampleCap = 1'000'000;

[[nodiscard]] inline std::int64_t frame_clock_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// One refresh period in milliseconds, or zero when the screen did not say.
[[nodiscard]] inline double frame_budget_ms(double refresh_hz)
{
    return refresh_hz > 0.0 ? 1000.0 / refresh_hz : 0.0;
}

// Whether an interval missed at least one refresh.
[[nodiscard]] inline bool missed_refresh(double interval_ms, double budget_ms)
{
    return budget_ms > 0.0 && interval_ms > kMissedRefreshPeriods * budget_ms;
}

// How many refreshes an interval missed: 0 on time, 1 for a frame shown one
// period late, and so on. Rounded, because jitter moves an interval a little
// off the whole period it landed on.
[[nodiscard]] inline std::uint64_t refreshes_missed(double interval_ms, double budget_ms)
{
    if (!missed_refresh(interval_ms, budget_ms)) {
        return 0;
    }
    const double periods = std::round(interval_ms / budget_ms);
    return periods > 1.0 ? static_cast<std::uint64_t>(periods) - 1 : 0;
}

struct FrameSummary {
    std::size_t count = 0;
    double mean = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
};

// Nearest rank: the smallest sample with at least p percent of the samples
// at or below it. Not interpolated, so every percentile reported is a frame
// that happened; an interpolated p99 between two frames is a frame time no
// frame had.
[[nodiscard]] inline double percentile_sorted(std::span<const double> sorted, double p)
{
    if (sorted.empty()) {
        return 0.0;
    }
    const double rank = std::ceil(p / 100.0 * static_cast<double>(sorted.size()));
    const auto index = static_cast<std::size_t>(std::max(rank, 1.0)) - 1;
    return sorted[std::min(index, sorted.size() - 1)];
}

[[nodiscard]] inline FrameSummary summarise(std::span<const double> samples)
{
    FrameSummary out;
    out.count = samples.size();
    if (samples.empty()) {
        return out;
    }
    std::vector<double> sorted(samples.begin(), samples.end());
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (const double value : sorted) {
        sum += value;
    }
    out.mean = sum / static_cast<double>(sorted.size());
    out.p50 = percentile_sorted(sorted, 50.0);
    out.p95 = percentile_sorted(sorted, 95.0);
    out.p99 = percentile_sorted(sorted, 99.0);
    out.max = sorted.back();
    return out;
}

enum class FrameItem : std::uint8_t {
    Spectrum,
    Waterfall,
    Passband,
    PassbandWaterfall,

    // The ruler is QML, not a scene graph item of ours, so what is timed for
    // it is the tick plan models/ruler.h makes when the span or the width
    // moves. Its text nodes are drawn inside the window's render time with
    // everything else and cannot be separated from it.
    Ruler,
};

inline constexpr std::size_t kFrameItemCount = 5;

[[nodiscard]] inline std::string_view frame_item_name(FrameItem item)
{
    switch (item) {
    case FrameItem::Spectrum:
        return "spectrum";
    case FrameItem::Waterfall:
        return "waterfall";
    case FrameItem::Passband:
        return "passband";
    case FrameItem::PassbandWaterfall:
        return "passband_waterfall";
    case FrameItem::Ruler:
        return "ruler";
    }
    return "unknown";
}

enum class FramePhase : std::uint8_t {
    // Ingesting a frame from the engine, on the GUI thread.
    Take,
    // Building or updating the scene graph node, on the render thread while
    // the GUI thread is blocked.
    Sync,
};

// What the Qt side knows about the run that this header cannot find out.
struct FrameRunInfo {
    std::string platform;
    std::string graphics_api;
    // The device the scene graph renders on, as QRhi names it. Not always the
    // one driving the screen: on a machine whose only display is a virtual
    // one, the frames are drawn on a real card and copied out.
    std::string adapter;
    bool render_thread = false;

    std::string screen_name;
    double refresh_hz = 0.0;
    int screen_width = 0;
    int screen_height = 0;
    double device_pixel_ratio = 1.0;

    double warmup_seconds = kFrameWarmupSeconds;
    double measured_seconds = 0.0;

    // Engine frames over the measured window, from EngineLink's counters
    // differenced across it.
    std::uint64_t engine_frames_received = 0;
    std::uint64_t engine_frames_dropped_by_engine = 0;
    std::uint64_t engine_frames_dropped_by_client = 0;
    std::uint64_t engine_frames_skipped = 0;
    std::uint64_t engine_frames_to_display = 0;

    int source_rate = 0;
    int bins = 0;
    double realtime_factor = 0.0;
    int receivers = 0;
    unsigned detections = 0;
};

struct FrameWindowReport {
    std::string name;
    int width = 0;
    int height = 0;
    std::uint64_t frames = 0;
    FrameSummary interval_ms;
    std::uint64_t over_budget = 0;
    std::uint64_t missed_refreshes = 0;
    FrameSummary sync_ms;
    FrameSummary render_ms;
    FrameSummary gpu_ms;

    // Where an interval went besides the work. request is the render thread
    // waiting to be asked for the frame, from the last swap to the frame
    // begin; begin is QRhi::beginFrame, from the frame begin to the sync,
    // which is where a latency-limited swap chain waits for the display;
    // present is from the end of recording to the swap. The missed_ series
    // are the same three over only the intervals that missed a refresh,
    // which is what says where a late frame was late.
    FrameSummary request_ms;
    FrameSummary begin_ms;
    FrameSummary present_ms;
    FrameSummary missed_request_ms;
    FrameSummary missed_begin_ms;
    FrameSummary missed_present_ms;

    std::uint64_t work_over_budget = 0;
    bool capped = false;
};

struct FrameItemReport {
    FrameItem item = FrameItem::Spectrum;
    std::uint64_t takes = 0;
    std::uint64_t syncs = 0;
    std::uint64_t drawn = 0;
    std::uint64_t superseded = 0;
    FrameSummary take_ms;
    FrameSummary sync_ms;
};

struct FrameReport {
    FrameRunInfo run;
    double budget_ms = 0.0;
    std::vector<FrameWindowReport> windows;
    std::vector<FrameItemReport> items;
};

// Whether a window held its budget. See the header note for the line.
[[nodiscard]] inline bool budget_met(const FrameWindowReport& window)
{
    if (window.frames == 0 || window.interval_ms.count == 0) {
        return false;
    }
    return static_cast<double>(window.over_budget) <=
           kBudgetMissAllowance * static_cast<double>(window.interval_ms.count);
}

// Collects timestamps from every window's render thread and every item, and
// turns them into a FrameReport.
//
// One mutex over everything. Each window has its own render thread under
// Qt's threaded loop and the items' takes arrive on the GUI thread, so there
// are at least three writers. The lock is taken a handful of times a frame
// and is uncontended almost always, which costs tens of nanoseconds against
// a budget of milliseconds.
class FrameRecorder {
public:
    // Adds a window and returns the index its events are reported under.
    int add_window(std::string name)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        windows_.push_back(WindowTrack{});
        windows_.back().name = std::move(name);
        return static_cast<int>(windows_.size()) - 1;
    }

    void set_window_size(int window, int width, int height)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (WindowTrack* track = find(window)) {
            track->width = width;
            track->height = height;
        }
    }

    // Opens the measured window at from_ns. Nothing is kept before it.
    void open(std::int64_t from_ns)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        from_ns_ = from_ns;
    }

    // Closes it at until_ns. Nothing is kept after it, which is what stops
    // the frames of a closing window being counted as the steady state.
    void close(std::int64_t until_ns)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        until_ns_ = until_ns;
    }

    // The render thread is about to begin a frame on the swap chain. Under
    // Qt 6's threaded loop this comes after the GUI thread asked for the
    // frame and before the sync, and QRhi::beginFrame between the two is
    // where a swap chain with a frame latency limit waits for the display.
    void frame_began(int window, std::int64_t now_ns)
    {
        mark(window, &WindowTrack::frame_began, now_ns);
    }

    // The sync starts. The time since the last swap splits at frame_began:
    // before it the render thread was waiting to be asked for a frame, after
    // it for beginFrame. A loop that reports no frame begin, or reports it
    // after the sync, has the whole of it counted as the first.
    void sync_began(int window, std::int64_t now_ns)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        WindowTrack* track = find(window);
        if (track == nullptr) {
            return;
        }
        track->sync_began = now_ns;
        if (track->last_swap < 0) {
            track->frame_request_ms = 0.0;
            track->frame_begin_ms = 0.0;
            return;
        }
        const bool split = track->frame_began > track->last_swap && track->frame_began <= now_ns;
        const std::int64_t at = split ? track->frame_began : now_ns;
        track->frame_request_ms = ms(at - track->last_swap);
        track->frame_begin_ms = ms(now_ns - at);
    }

    void render_began(int window, std::int64_t now_ns)
    {
        mark(window, &WindowTrack::render_began, now_ns);
    }

    void sync_ended(int window, std::int64_t now_ns)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        WindowTrack* track = find(window);
        if (track == nullptr || track->sync_began < 0) {
            return;
        }
        track->frame_sync_ms = ms(now_ns - track->sync_began);
        track->sync_began = -1;
    }

    void render_ended(int window, std::int64_t now_ns)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        WindowTrack* track = find(window);
        if (track == nullptr || track->render_began < 0) {
            return;
        }
        track->frame_render_ms = ms(now_ns - track->render_began);
        track->render_began = -1;
        track->render_ended = now_ns;
    }

    // The GPU time of the last frame the device finished, which Qt reports
    // a frame or two behind the one being recorded. Zero means Qt did not
    // measure it and is not kept.
    void gpu_time(int window, double gpu_ms)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        WindowTrack* track = find(window);
        if (track != nullptr && gpu_ms > 0.0 && measuring(track->last_swap)) {
            push(track->gpu_ms, gpu_ms, track->capped);
        }
    }

    // The frame reached the screen. The interval since the previous one is
    // kept when both ends are inside the measured window, and the frame's
    // sync and render times with it.
    void swapped(int window, std::int64_t now_ns)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        WindowTrack* track = find(window);
        if (track == nullptr) {
            return;
        }
        if (measuring(now_ns)) {
            ++track->frames;
            if (track->last_swap >= 0 && measuring(track->last_swap)) {
                // Idle and present are kept beside the interval they sit in,
                // index for index, so a late frame can be taken apart.
                push(track->interval_ms, ms(now_ns - track->last_swap), track->capped);
                push(track->request_ms, track->frame_request_ms, track->capped);
                push(track->begin_ms, track->frame_begin_ms, track->capped);
                push(track->present_ms,
                     track->render_ended >= 0 ? ms(now_ns - track->render_ended) : 0.0,
                     track->capped);
            }
            push(track->sync_ms, track->frame_sync_ms, track->capped);
            push(track->render_ms, track->frame_render_ms, track->capped);
        }
        track->last_swap = now_ns;
        track->render_ended = -1;
        track->frame_request_ms = 0.0;
        track->frame_begin_ms = 0.0;
        track->frame_sync_ms = 0.0;
        track->frame_render_ms = 0.0;
    }

    void item_cost(FrameItem item, FramePhase phase, std::int64_t begin_ns, std::int64_t end_ns)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ItemTrack& track = items_[static_cast<std::size_t>(item)];
        const bool in = measuring(begin_ns);
        if (phase == FramePhase::Take) {
            if (in) {
                ++track.takes;
                // The ruler's take is a tick plan and it has no sync of its
                // own, so a second plan is not a frame thrown away.
                if (track.pending_take && item != FrameItem::Ruler) {
                    ++track.superseded;
                }
                push(track.take_ms, ms(end_ns - begin_ns), track.capped);
            }
            track.pending_take = in;
            return;
        }
        if (in) {
            ++track.syncs;
            if (track.pending_take) {
                ++track.drawn;
            }
            push(track.sync_ms, ms(end_ns - begin_ns), track.capped);
        }
        track.pending_take = false;
    }

    [[nodiscard]] FrameReport report(const FrameRunInfo& run) const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        FrameReport out;
        out.run = run;
        out.budget_ms = frame_budget_ms(run.refresh_hz);
        for (const WindowTrack& track : windows_) {
            FrameWindowReport window;
            window.name = track.name;
            window.width = track.width;
            window.height = track.height;
            window.frames = track.frames;
            window.interval_ms = summarise(track.interval_ms);
            std::vector<double> missed_request;
            std::vector<double> missed_begin;
            std::vector<double> missed_present;
            for (std::size_t i = 0; i < track.interval_ms.size(); ++i) {
                const double interval = track.interval_ms[i];
                if (missed_refresh(interval, out.budget_ms)) {
                    ++window.over_budget;
                    window.missed_refreshes += refreshes_missed(interval, out.budget_ms);
                    if (i < track.request_ms.size() && i < track.begin_ms.size() &&
                        i < track.present_ms.size()) {
                        missed_request.push_back(track.request_ms[i]);
                        missed_begin.push_back(track.begin_ms[i]);
                        missed_present.push_back(track.present_ms[i]);
                    }
                }
            }
            window.request_ms = summarise(track.request_ms);
            window.begin_ms = summarise(track.begin_ms);
            window.present_ms = summarise(track.present_ms);
            window.missed_request_ms = summarise(missed_request);
            window.missed_begin_ms = summarise(missed_begin);
            window.missed_present_ms = summarise(missed_present);
            window.sync_ms = summarise(track.sync_ms);
            window.render_ms = summarise(track.render_ms);
            window.gpu_ms = summarise(track.gpu_ms);
            const std::size_t both = std::min(track.sync_ms.size(), track.render_ms.size());
            for (std::size_t i = 0; i < both; ++i) {
                if (out.budget_ms > 0.0 && track.sync_ms[i] + track.render_ms[i] > out.budget_ms) {
                    ++window.work_over_budget;
                }
            }
            window.capped = track.capped;
            out.windows.push_back(std::move(window));
        }
        for (std::size_t i = 0; i < kFrameItemCount; ++i) {
            const ItemTrack& track = items_[i];
            FrameItemReport item;
            item.item = static_cast<FrameItem>(i);
            item.takes = track.takes;
            item.syncs = track.syncs;
            item.drawn = track.drawn;
            item.superseded = track.superseded;
            item.take_ms = summarise(track.take_ms);
            item.sync_ms = summarise(track.sync_ms);
            out.items.push_back(std::move(item));
        }
        return out;
    }

private:
    static constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::max();

    struct WindowTrack {
        std::string name;
        int width = 0;
        int height = 0;
        std::int64_t sync_began = -1;
        std::int64_t render_began = -1;
        std::int64_t last_swap = -1;
        std::int64_t render_ended = -1;
        std::int64_t frame_began = -1;
        double frame_request_ms = 0.0;
        double frame_begin_ms = 0.0;
        double frame_sync_ms = 0.0;
        double frame_render_ms = 0.0;
        std::uint64_t frames = 0;
        std::vector<double> interval_ms;
        std::vector<double> request_ms;
        std::vector<double> begin_ms;
        std::vector<double> present_ms;
        std::vector<double> sync_ms;
        std::vector<double> render_ms;
        std::vector<double> gpu_ms;
        bool capped = false;
    };

    struct ItemTrack {
        std::uint64_t takes = 0;
        std::uint64_t syncs = 0;
        std::uint64_t drawn = 0;
        std::uint64_t superseded = 0;
        bool pending_take = false;
        std::vector<double> take_ms;
        std::vector<double> sync_ms;
        bool capped = false;
    };

    [[nodiscard]] static double ms(std::int64_t ns) { return static_cast<double>(ns) / 1.0e6; }

    static void push(std::vector<double>& series, double value, bool& capped)
    {
        if (series.size() < kFrameSampleCap) {
            series.push_back(value);
        } else {
            capped = true;
        }
    }

    [[nodiscard]] bool measuring(std::int64_t t) const { return t >= from_ns_ && t < until_ns_; }

    WindowTrack* find(int window)
    {
        if (window < 0 || static_cast<std::size_t>(window) >= windows_.size()) {
            return nullptr;
        }
        return &windows_[static_cast<std::size_t>(window)];
    }

    void mark(int window, std::int64_t WindowTrack::*field, std::int64_t now_ns)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (WindowTrack* track = find(window)) {
            track->*field = now_ns;
        }
    }

    mutable std::mutex mutex_;
    std::vector<WindowTrack> windows_;
    std::array<ItemTrack, kFrameItemCount> items_{};
    std::int64_t from_ns_ = kNever;
    std::int64_t until_ns_ = kNever;
};

// The recorder the items report to, or null when --frame-stats was not given.
// A global rather than a property threaded through every item, because the
// items are built by QML and a hook that needed wiring per item would be one
// forgotten binding away from measuring nothing.
inline std::atomic<FrameRecorder*> g_frame_recorder{nullptr};

// Times one take or sync of one item. With no recorder installed this is one
// relaxed atomic load and a branch, which is the whole of the cost of the
// instrument when nobody asked for it.
class FrameCost {
public:
    FrameCost(FrameItem item, FramePhase phase) noexcept
        : recorder_(g_frame_recorder.load(std::memory_order_relaxed)), item_(item), phase_(phase)
    {
        if (recorder_ != nullptr) {
            begin_ns_ = frame_clock_ns();
        }
    }

    ~FrameCost()
    {
        if (recorder_ != nullptr) {
            recorder_->item_cost(item_, phase_, begin_ns_, frame_clock_ns());
        }
    }

    FrameCost(const FrameCost&) = delete;
    FrameCost& operator=(const FrameCost&) = delete;
    FrameCost(FrameCost&&) = delete;
    FrameCost& operator=(FrameCost&&) = delete;

private:
    FrameRecorder* recorder_;
    FrameItem item_;
    FramePhase phase_;
    std::int64_t begin_ns_ = 0;
};

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::string json_escape(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                const auto code = static_cast<unsigned>(static_cast<unsigned char>(c));
                out += std::format("\\u{:04x}", code);
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
    return out;
}

// Three decimals of a millisecond is a microsecond: as fine as a frame
// timestamp taken from a signal handler means anything, and short enough to
// read.
[[nodiscard]] inline std::string json_number(double value)
{
    if (!std::isfinite(value)) {
        return "null";
    }
    return std::format("{:.3f}", value);
}

[[nodiscard]] inline std::string json_summary(const FrameSummary& s)
{
    if (s.count == 0) {
        return "null";
    }
    return std::format(R"({{"count": {}, "mean": {}, "p50": {}, "p95": {}, "p99": {}, "max": {}}})",
                       s.count, json_number(s.mean), json_number(s.p50), json_number(s.p95),
                       json_number(s.p99), json_number(s.max));
}

[[nodiscard]] inline std::string frame_stats_json(const FrameReport& report)
{
    const FrameRunInfo& run = report.run;
    std::string out = "{\n";
    out += std::format("  \"platform\": {},\n", json_escape(run.platform));
    out += std::format("  \"graphics_api\": {},\n", json_escape(run.graphics_api));
    out += std::format("  \"adapter\": {},\n", json_escape(run.adapter));
    out += std::format("  \"render_thread\": {},\n", run.render_thread ? "true" : "false");
    out += std::format(
        "  \"screen\": {{\"name\": {}, \"refresh_hz\": {}, \"width\": {}, \"height\": {}, "
        "\"device_pixel_ratio\": {}}},\n",
        json_escape(run.screen_name), json_number(run.refresh_hz), run.screen_width,
        run.screen_height, json_number(run.device_pixel_ratio));
    out += std::format("  \"budget_ms\": {},\n", json_number(report.budget_ms));
    out += std::format("  \"missed_refresh_periods\": {},\n", json_number(kMissedRefreshPeriods));
    out += std::format("  \"warmup_seconds\": {},\n", json_number(run.warmup_seconds));
    out += std::format("  \"measured_seconds\": {},\n", json_number(run.measured_seconds));
    out += std::format(
        "  \"engine\": {{\"source_rate\": {}, \"bins\": {}, \"realtime_factor\": {}, "
        "\"receivers\": {}, \"detections\": {}, \"frames_received\": {}, "
        "\"frames_dropped_by_engine\": {}, \"frames_dropped_by_client\": {}, "
        "\"frames_skipped\": {}, \"frames_to_display\": {}}},\n",
        run.source_rate, run.bins, json_number(run.realtime_factor), run.receivers,
        run.detections, run.engine_frames_received, run.engine_frames_dropped_by_engine,
        run.engine_frames_dropped_by_client, run.engine_frames_skipped,
        run.engine_frames_to_display);

    out += "  \"windows\": [\n";
    for (std::size_t i = 0; i < report.windows.size(); ++i) {
        const FrameWindowReport& w = report.windows[i];
        out += std::format(
            "    {{\"name\": {}, \"width\": {}, \"height\": {}, \"frames\": {}, "
            "\"budget_met\": {}, \"over_budget\": {}, \"missed_refreshes\": {}, "
            "\"work_over_budget\": {}, \"capped\": {},\n"
            "     \"interval_ms\": {},\n"
            "     \"sync_ms\": {},\n"
            "     \"render_ms\": {},\n"
            "     \"gpu_ms\": {},\n"
            "     \"request_ms\": {},\n"
            "     \"begin_ms\": {},\n"
            "     \"present_ms\": {},\n"
            "     \"missed_request_ms\": {},\n"
            "     \"missed_begin_ms\": {},\n"
            "     \"missed_present_ms\": {}}}{}\n",
            json_escape(w.name), w.width, w.height, w.frames, budget_met(w) ? "true" : "false",
            w.over_budget, w.missed_refreshes, w.work_over_budget, w.capped ? "true" : "false",
            json_summary(w.interval_ms), json_summary(w.sync_ms), json_summary(w.render_ms),
            json_summary(w.gpu_ms), json_summary(w.request_ms), json_summary(w.begin_ms),
            json_summary(w.present_ms), json_summary(w.missed_request_ms),
            json_summary(w.missed_begin_ms), json_summary(w.missed_present_ms),
            i + 1 < report.windows.size() ? "," : "");
    }
    out += "  ],\n";

    out += "  \"items\": [\n";
    for (std::size_t i = 0; i < report.items.size(); ++i) {
        const FrameItemReport& item = report.items[i];
        out += std::format(
            "    {{\"name\": {}, \"takes\": {}, \"syncs\": {}, \"drawn\": {}, "
            "\"superseded\": {},\n"
            "     \"take_ms\": {},\n"
            "     \"sync_ms\": {}}}{}\n",
            json_escape(frame_item_name(item.item)), item.takes, item.syncs, item.drawn,
            item.superseded, json_summary(item.take_ms), json_summary(item.sync_ms),
            i + 1 < report.items.size() ? "," : "");
    }
    out += "  ]\n}\n";
    return out;
}

// The line printed to stderr: the first window's verdict and its numbers,
// which is the main window and the one the criterion is about.
[[nodiscard]] inline std::string frame_stats_line(const FrameReport& report)
{
    if (report.windows.empty()) {
        return "frame-stats: no window was measured";
    }
    const FrameWindowReport& w = report.windows.front();
    if (w.interval_ms.count == 0) {
        return std::format("frame-stats: {} drew no frame inside the measured window", w.name);
    }
    return std::format(
        "frame-stats: {} {} at {} Hz, budget {} ms: {} frames, interval mean {} p50 {} p95 {} "
        "p99 {} max {} ms, {} over budget; engine {} frames in, {} to display",
        w.name, budget_met(w) ? "MET" : "MISSED", json_number(report.run.refresh_hz),
        json_number(report.budget_ms), w.frames, json_number(w.interval_ms.mean),
        json_number(w.interval_ms.p50), json_number(w.interval_ms.p95),
        json_number(w.interval_ms.p99), json_number(w.interval_ms.max), w.over_budget,
        report.run.engine_frames_received, report.run.engine_frames_to_display);
}

}  // namespace revenant::ui
