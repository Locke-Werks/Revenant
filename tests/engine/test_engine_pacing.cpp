// The realtime factor through the real engine, across a front end that stops
// delivering the way an RTL-SDR does around a control transfer.
//
// WHY A SOURCE OF ITS OWN. A file and a synthetic scene cannot pause like the
// dongle: a Demand source that blocks in its sink resumes where it stopped,
// so the samples arrive late rather than go missing, and neither backend
// retunes at all. So a synthetic scene paced at realtime is wrapped, the way
// tests/engine/movable_centre.h wraps one, and the wrapper imitates
// with_transfers_paused in core/source/rtlsdr_source.cpp: for 330 ms, the
// figure measured on an R820T, every block the scene produces is thrown away
// and counted in samples_lost, the next block carries the gap in
// dropped_before, and tune() does not return until the pause is over. A stall
// can also be started from outside, which is the same pause with no control
// call behind it, and the engine cannot tell it from a consumer too slow to
// keep up.
//
// tests/engine/test_pacing_window.cpp holds the arithmetic to the client's
// thresholds on a simulated clock; this holds the engine's wiring to them on
// a real one: that run()'s loop feeds the window, that set_source_center
// marks its pause, and that a poll inside the pause sees none of it.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>

#include "core/dsp/types.h"
#include "core/engine/engine.h"
#include "core/engine/open_built_source.h"
#include "core/engine/pacing_window.h"
#include "core/error.h"
#include "core/source/registry.h"
#include "core/source/source.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 2'000'000;
constexpr dsp::Hertz kOpenedAt = 100'000'000;
constexpr auto kPause = std::chrono::milliseconds(330);

// The client's thresholds, from ui/models/source_pacing.h: behind below the
// first, caught up above the second.
constexpr double kClientBehindEnter = 0.97;
constexpr double kClientBehindLeave = 0.99;

class PausingFrontEnd final : public source::Source {
public:
    explicit PausingFrontEnd(std::unique_ptr<source::Source> inner)
        : inner_(std::move(inner)), caps_(inner_->capabilities()), center_(inner_->center()) {
        caps_.tune_ranges = {source::TuneRange{.low = 0, .high = 6'000'000'000, .step = 0}};
    }

    [[nodiscard]] const source::SourceCapabilities& capabilities() const override {
        return caps_;
    }

    // A retune the engine asked for: the transfers stop for the pause and the
    // call returns when they are running again, as the dongle's does.
    [[nodiscard]] Expected<dsp::Hertz> tune(dsp::Hertz center) override {
        pause();
        center_.store(center, std::memory_order_release);
        return center;
    }
    [[nodiscard]] dsp::Hertz center() const override {
        return center_.load(std::memory_order_acquire);
    }
    [[nodiscard]] Expected<dsp::SampleRate> set_sample_rate(dsp::SampleRate rate) override {
        return inner_->set_sample_rate(rate);
    }
    [[nodiscard]] dsp::SampleRate sample_rate() const override { return inner_->sample_rate(); }
    [[nodiscard]] Expected<double> set_gain(std::string_view stage, double db) override {
        return inner_->set_gain(stage, db);
    }
    [[nodiscard]] Status set_gain_auto(std::string_view stage, bool on) override {
        return inner_->set_gain_auto(stage, on);
    }

    [[nodiscard]] Status start(const source::StreamOptions& options,
                               source::BlockSink sink) override {
        return inner_->start(options, [this, sink = std::move(sink)](
                                          const source::SourceBlock& block) -> Status {
            if (dropping_.load(std::memory_order_acquire)) {
                lost_.fetch_add(block.sample_count, std::memory_order_relaxed);
                overruns_.fetch_add(1, std::memory_order_relaxed);
                gap_ += block.sample_count;
                return {};
            }
            source::SourceBlock passed = block;
            passed.dropped_before = gap_;
            gap_ = 0;
            Status delivered = sink(passed);
            if (delivered) {
                delivered_.fetch_add(block.sample_count, std::memory_order_relaxed);
            }
            return delivered;
        });
    }
    [[nodiscard]] Status stop() override { return inner_->stop(); }
    [[nodiscard]] bool running() const override { return inner_->running(); }
    [[nodiscard]] Status seek(dsp::SampleIndex index) override { return inner_->seek(index); }

    [[nodiscard]] source::SourceStats stats() const override {
        source::SourceStats out = inner_->stats();
        out.samples_delivered = delivered_.load(std::memory_order_relaxed);
        out.samples_lost = lost_.load(std::memory_order_relaxed);
        out.overrun_events = overruns_.load(std::memory_order_relaxed);
        return out;
    }
    [[nodiscard]] source::ClockQuality clock() const override { return inner_->clock(); }

    // The same pause with no control call behind it.
    void stall() { pause(); }

private:
    void pause() {
        dropping_.store(true, std::memory_order_release);
        std::this_thread::sleep_for(kPause);
        dropping_.store(false, std::memory_order_release);
    }

    std::unique_ptr<source::Source> inner_;
    source::SourceCapabilities caps_;
    std::atomic<dsp::Hertz> center_;
    std::atomic<bool> dropping_{false};
    std::atomic<std::uint64_t> delivered_{0};
    std::atomic<std::uint64_t> lost_{0};
    std::atomic<std::uint64_t> overruns_{0};

    // The delivery thread's alone.
    std::uint64_t gap_ = 0;
};

struct Range {
    double low = 1e9;
    double high = 0.0;
};

// The factor every 50 ms for `span`, which is twenty times as often as the
// client asks.
[[nodiscard]] Range watch(const engine::Engine& eng, std::chrono::milliseconds span) {
    Range out;
    const auto until = std::chrono::steady_clock::now() + span;
    while (std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const double factor = eng.source_pacing().realtime_factor;
        out.low = std::min(out.low, factor);
        out.high = std::max(out.high, factor);
    }
    return out;
}

}  // namespace

// Rejects the lifetime mean, under which five retunes in six seconds of a
// nine-second run read about (9 - 5 x 0.33) / 9 = 0.82 for the rest of it, and
// a window that charges a retune to the source or shows it while it runs.
// And, on the same run, rejects a factor that hides a stall nobody asked for
// or holds on to one: it has to fall below the client's line and come back
// above the other within the stated window.
TEST_CASE("a retune through the engine is not the source falling behind, a stall is",
          "[gpu][engine][pacing]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    engine::EngineConfig config;
    config.channels = 64;
    config.taps_per_branch = 17;
    // Deeper than the 660,000 samples a pause throws away at this rate, or
    // the graph refuses the gap as a source that ran a whole ring ahead.
    config.ring_seconds = 2.0;
    config.audio_rate = 48'000;
    config.probe_receivers = 0;
    config.gpu_index = -1;  // honours REVENANT_GPU_INDEX
    config.pace = 1.0;

    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    auto scene = source::open_source(std::format(
        "synthetic:wideband?rate={}&center={}&emitters=0&seed=20260923", kRate, kOpenedAt));
    INFO(test::message_of(scene));
    REQUIRE(scene.has_value());
    auto owned = std::make_unique<PausingFrontEnd>(std::move(*scene));
    PausingFrontEnd& front = *owned;
    auto opened = engine::open_built_source(eng, std::move(owned));
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    // run() answers "the engine was stopped" when stop() ends it, which is
    // how this case ends it, so its answer is not the subject here.
    std::thread runner([&] { static_cast<void>(eng.run()); });
    struct Stopper {
        engine::Engine& eng;
        std::thread& runner;
        ~Stopper() {
            static_cast<void>(eng.stop());
            if (runner.joinable()) {
                runner.join();
            }
        }
    } stopper{eng, runner};

    // Past start-up and a whole window into the run.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    const Range steady = watch(eng, std::chrono::milliseconds(1500));
    WARN("steady: factor " << steady.low << " to " << steady.high);
    CHECK(steady.low > kClientBehindEnter);

    // Five retunes, 700 ms apart, from another thread, polled throughout,
    // including inside every pause.
    std::thread tuner([&] {
        for (int i = 1; i <= 5; ++i) {
            static_cast<void>(eng.set_source_center(kOpenedAt + i * 100'000));
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
        }
    });
    const Range retuning = watch(eng, std::chrono::milliseconds(5 * 1030 + 300));
    tuner.join();
    const std::uint64_t lost_to_retunes = eng.source_stats().samples_lost;
    WARN("across five retunes: factor " << retuning.low << " to " << retuning.high << ", "
                                        << lost_to_retunes << " samples lost");

    // The pauses really happened and really cost samples, which the stats say.
    CHECK(lost_to_retunes > 5ULL * 500'000ULL);
    CHECK(retuning.low > kClientBehindEnter);

    // A stall nobody asked for. It shows.
    std::thread staller([&] { front.stall(); });
    const Range stalling = watch(eng, std::chrono::milliseconds(700));
    staller.join();
    const auto stall_ended = std::chrono::steady_clock::now();
    WARN("stall nobody asked for: factor down to " << stalling.low);
    CHECK(stalling.low < kClientBehindEnter);

    // And it goes, within the stated window and the snapshot interval its
    // start can lag by.
    double recovered_after = -1.0;
    const auto deadline = stall_ended + std::chrono::nanoseconds(engine::kPacingWindowNs) +
                          std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (eng.source_pacing().realtime_factor > kClientBehindLeave) {
            recovered_after = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                            stall_ended)
                                  .count();
            break;
        }
    }
    WARN("back above " << kClientBehindLeave << " " << recovered_after << " s after the stall");
    CHECK(recovered_after >= 0.0);

    const engine::SourcePacing last = eng.source_pacing();
    CHECK(last.window_seconds >= engine::kPacingWindowSeconds);
}
