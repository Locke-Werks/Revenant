// A recording opened over the wire plays at realtime, measured through the
// engine's own pacing figure.
//
// WHY THESE EXIST. Pace was EngineConfig::pace, revenant-engine's --pace, and
// nothing on the wire could set it. The engine a window talks to is usually
// the one started for the dongle, at --pace 0, so a recording the owner
// opened from the radio panel played at 319x realtime and was heard as a
// burst. openSource now adds pace=1 to a file URI that states none, a file's
// own pace= wins over --pace, and setSourcePace changes it while it plays.
//
// Every case starts an engine at pace 0, the dongle engine's setting, so a
// pass cannot come from the host's configuration.
//
// THE MEASUREMENT AND ITS TOLERANCE. EngineInfo::realtimeFactor, the figure a
// client reads, over its two-second window, read once the window is full.
// Within 3 percent of the pace asked for. The window's edges move by a block
// and by the platform's sleep granularity: 4096 samples at 256000 S/s is 16 ms
// of capture, and a Windows sleep lands up to a 15.6 ms tick late, each under
// 1 percent of two seconds. core/source/file_source.cpp times every block from
// a fixed origin, so neither accumulates. A synthetic scene paced at realtime
// read 0.9986 to 1.0013 in tests/engine/test_engine_pacing.cpp.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"
#include "core/rpc/client.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"
#include "tests/support/temp_path.h"

using namespace revenant;
using test::Harness;
using test::HarnessOptions;

namespace {

// 256000 over the suite's 64 channels puts a channel at 8000 S/s: the grid's
// decimation of 32 divides it, which engine::place requires. Low so that
// sixteen seconds of recording is 8 MB of cs8.
constexpr dsp::SampleRate kRate = 256'000;
constexpr dsp::SampleIndex kSeconds = 16;
constexpr std::uint32_t kBlockSamples = 4096;

// VHF, so the source asks for no HF grid and the engine keeps the suite's 64
// channels. The samples are zeros: pace is about how many arrive, not what
// they hold.
constexpr dsp::Hertz kCenter = 100'000'000;

constexpr double kTolerance = 0.03;

// Sixteen seconds of silence as raw cs8, removed afterwards.
class Recording {
public:
    Recording() : path_(test::unique_temp_path("revenant_rpc_pace", ".cs8")) {
        const std::vector<char> zeros(static_cast<std::size_t>(kRate * kSeconds) * 2, 0);
        std::FILE* file = std::fopen(path_.string().c_str(), "wb");
        written_ = file != nullptr &&
                   std::fwrite(zeros.data(), 1, zeros.size(), file) == zeros.size();
        if (file != nullptr) {
            written_ = std::fclose(file) == 0 && written_;
        }
    }

    ~Recording() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    Recording(const Recording&) = delete;
    Recording& operator=(const Recording&) = delete;

    [[nodiscard]] bool written() const { return written_; }

    // `extra` is appended to the query, "&pace=4" or nothing.
    [[nodiscard]] std::string uri(std::string_view extra = {}) const {
        return std::format("file:///{}?rate={}&format=cs8&center={}{}", path_.generic_string(),
                           kRate, kCenter, extra);
    }

private:
    std::filesystem::path path_;
    bool written_ = false;
};

// An engine at pace 0 serving a short synthetic scene, which the case closes
// and replaces with the recording through the wire, as the radio panel does.
void open_over_the_wire(Harness& harness, const std::string& uri) {
    HarnessOptions options;
    options.pace = 0.0;
    options.samples = 65'536;
    options.block_samples = kBlockSamples;
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    REQUIRE(harness.client().close_source().has_value());
    const auto opened = harness.client().open_source(uri);
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());
}

// The factor once the window has filled, or the last reading at the deadline.
// `since` is when the pace to be measured took effect: the window has to be
// clear of anything before it.
[[nodiscard]] rpc::EngineInfo settled_reading(Harness& harness,
                                              std::chrono::steady_clock::time_point since) {
    constexpr auto kFill = std::chrono::milliseconds(2'300);
    const auto deadline = since + std::chrono::seconds(15);
    rpc::EngineInfo last;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto info = harness.client().info();
        REQUIRE(info.has_value());
        last = *info;
        if (std::chrono::steady_clock::now() - since >= kFill &&
            last.realtime_window_seconds >= 2.0) {
            break;
        }
    }
    return last;
}

}  // namespace

TEST_CASE("a recording a client opens without pace= plays at realtime", "[gpu][rpc][m2]") {
    REVENANT_NEEDS_GPU();

    const Recording recording;
    REQUIRE(recording.written());

    Harness harness;
    open_over_the_wire(harness, recording.uri());

    // Configuration and not measurement, so it is right before a block moves.
    auto before = harness.client().info();
    REQUIRE(before.has_value());
    CHECK(before->source_paced_by == 1.0);

    // And the URI the engine opened says so, which is what a session written
    // down from it replays at.
    auto descriptor = harness.client().source_descriptor();
    REQUIRE(descriptor.has_value());
    REQUIRE(descriptor->has_value());
    CHECK((*descriptor)->uri == recording.uri("&pace=1"));

    const auto started_at = std::chrono::steady_clock::now();
    REQUIRE(harness.start_engine().has_value());
    const rpc::EngineInfo settled = settled_reading(harness, started_at);
    INFO(std::format("{:.4f}x over {:.2f} s", settled.realtime_factor,
                     settled.realtime_window_seconds));

    CHECK(settled.realtime_window_seconds >= 2.0);
    CHECK(settled.realtime_factor >= 1.0 - kTolerance);
    CHECK(settled.realtime_factor <= 1.0 + kTolerance);
    CHECK(settled.source_paced_by == 1.0);

    REQUIRE(harness.stop_engine().has_value());
}

TEST_CASE("a recording opened with pace=4 plays at four times realtime", "[gpu][rpc][m2]") {
    REVENANT_NEEDS_GPU();

    const Recording recording;
    REQUIRE(recording.written());

    Harness harness;
    open_over_the_wire(harness, recording.uri("&pace=4"));

    const auto started_at = std::chrono::steady_clock::now();
    REQUIRE(harness.start_engine().has_value());
    const rpc::EngineInfo settled = settled_reading(harness, started_at);
    INFO(std::format("{:.4f}x over {:.2f} s", settled.realtime_factor,
                     settled.realtime_window_seconds));

    CHECK(settled.source_paced_by == 4.0);
    CHECK(settled.realtime_factor >= 4.0 * (1.0 - kTolerance));
    CHECK(settled.realtime_factor <= 4.0 * (1.0 + kTolerance));

    REQUIRE(harness.stop_engine().has_value());
}

TEST_CASE("setSourcePace changes a playing recording's pace and nothing else",
          "[gpu][rpc][m2]") {
    REVENANT_NEEDS_GPU();

    const Recording recording;
    REQUIRE(recording.written());

    Harness harness;
    open_over_the_wire(harness, recording.uri());

    auto opened = harness.client().info();
    REQUIRE(opened.has_value());

    REQUIRE(harness.start_engine().has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto sped = harness.client().set_source_pace(4.0);
    INFO(test::message_of(sped));
    REQUIRE(sped.has_value());
    CHECK(*sped == 4.0);
    const auto changed_at = std::chrono::steady_clock::now();

    const rpc::EngineInfo settled = settled_reading(harness, changed_at);
    INFO(std::format("{:.4f}x over {:.2f} s", settled.realtime_factor,
                     settled.realtime_window_seconds));
    CHECK(settled.source_paced_by == 4.0);
    CHECK(settled.realtime_factor >= 4.0 * (1.0 - kTolerance));
    CHECK(settled.realtime_factor <= 4.0 * (1.0 + kTolerance));

    // Not a source change: the same stream, still counting.
    CHECK(settled.source_epoch == opened->source_epoch);

    // A pace that is not one is refused and leaves the one in force.
    CHECK_FALSE(harness.client().set_source_pace(-1.0).has_value());
    auto kept = harness.client().info();
    REQUIRE(kept.has_value());
    CHECK(kept->source_paced_by == 4.0);

    REQUIRE(harness.stop_engine().has_value());

    // With nothing open there is nothing to pace.
    REQUIRE(harness.client().close_source().has_value());
    auto closed = harness.client().set_source_pace(1.0);
    REQUIRE_FALSE(closed.has_value());
    INFO(closed.error().message);
    CHECK(closed.error().message.find("before a source is open") != std::string::npos);

    // And a source that cannot change its pace says so in its own words
    // rather than answering with a pace it is not running at.
    REQUIRE(harness.client().open_source(test::scene_uri(65'536)).has_value());
    auto scene = harness.client().set_source_pace(1.0);
    REQUIRE_FALSE(scene.has_value());
    INFO(scene.error().message);
    CHECK(scene.error().message.find("does not take a pace") != std::string::npos);
}
