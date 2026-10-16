// The span-wide comb cancellation through the engine, end to end: a capture
// carrying a drifting comb of interference lines over white noise goes in
// through the file source, and the test asserts that the stage locks to it
// and that the comb is gone from the spectrum the engine hands out.
//
// THE CAPTURE. 41 lines, k in [-20, 20], at c(t) + k * d(t) with the spacing
// starting at 15 kHz and drifting 0.5 Hz a second and the offset drifting
// 0.2 Hz a second, so line 20 moves 10 Hz a second and the lines move in the
// proportion core/dsp/comb_cancel.h relies on to confirm a comb. Each line
// stands about 20 dB over the floor in a spectrum bin. Seven seconds at
// 1.152 MS/s: one second of averaging before the first detection, then
// acquisition, then the last two seconds measured.
//
// THE MEASUREMENT. The same capture twice, with the stage off and on, and in
// each the mean over lines of a line's bin above the median of the frame,
// averaged over the last two seconds of spectrum frames. The assertion is
// that the stage takes most of that away. Loose, for the reason
// tests/decode/CMakeLists.txt gives: a figure asserted to a decimal fails the
// day somebody improves the stage.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <mutex>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "core/dsp/comb_cancel.h"
#include "core/engine/engine.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/support/temp_path.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 1'152'000;
constexpr std::uint32_t kChannels = 16;
constexpr std::uint32_t kTransform = 256;
constexpr double kSeconds = 7.0;
constexpr double kMeasureFrom = 5.0;
constexpr std::uint64_t kSeed = 0x434F4D4243414E43ULL;

constexpr std::int32_t kLow = -20;
constexpr std::int32_t kHigh = 20;
constexpr double kOffsetHz = 1'234.0;
constexpr double kSpacingHz = 15'000.0;
constexpr double kOffsetDrift = 0.2;   // Hz/s
constexpr double kSpacingDrift = 0.5;  // Hz/s

constexpr double kNoiseRms = 0.01;     // per component
constexpr double kLineAmplitude = 0.004;

[[nodiscard]] double offset_at(double t) { return kOffsetHz + kOffsetDrift * t; }
[[nodiscard]] double spacing_at(double t) { return kSpacingHz + kSpacingDrift * t; }

// The phase of line k at time t, in cycles: the integral of its frequency.
[[nodiscard]] double cycles_at(std::int32_t k, double t) {
    return kOffsetHz * t + 0.5 * kOffsetDrift * t * t +
           static_cast<double>(k) * (kSpacingHz * t + 0.5 * kSpacingDrift * t * t);
}

[[nodiscard]] std::vector<dsp::Complex32> render_capture() {
    const auto count = static_cast<std::size_t>(kSeconds * static_cast<double>(kRate));
    std::vector<dsp::Complex32> out(count);
    std::mt19937_64 random(kSeed);
    std::normal_distribution<double> noise(0.0, kNoiseRms);
    for (auto& sample : out) {
        sample = dsp::Complex32(static_cast<float>(noise(random)),
                                static_cast<float>(noise(random)));
    }

    // Each line as a rotating phasor re-anchored to the exact phase every
    // 256 samples, so rounding never accumulates and the frequency follows
    // the drift in steps far finer than any bin.
    //
    // Each line starts at a random phase. Lines that all start in phase sum
    // to a train of narrow pulses, kLineAmplitude * 41 high every 67 us,
    // which the impulse excision upstream rightly takes for impulses and cuts
    // out, and what it cuts carries most of the low-index lines with it. A
    // comb measured here is one the excision leaves alone.
    constexpr std::size_t kStride = 256;
    const auto rate = static_cast<double>(kRate);
    std::uniform_real_distribution<double> start_phase(0.0, 1.0);
    for (std::int32_t k = kLow; k <= kHigh; ++k) {
        const double phase0 = start_phase(random);
        for (std::size_t base = 0; base < count; base += kStride) {
            const double t = static_cast<double>(base) / rate;
            const double cycles = cycles_at(k, t) + phase0;
            const double frequency = offset_at(t) + static_cast<double>(k) * spacing_at(t);
            std::complex<double> phasor =
                std::polar(kLineAmplitude,
                           2.0 * std::numbers::pi * (cycles - std::floor(cycles)));
            const std::complex<double> step =
                std::polar(1.0, 2.0 * std::numbers::pi * frequency / rate);
            const std::size_t end = std::min(count, base + kStride);
            for (std::size_t n = base; n < end; ++n) {
                out[n] += dsp::Complex32(static_cast<float>(phasor.real()),
                                         static_cast<float>(phasor.imag()));
                phasor *= step;
            }
        }
    }
    return out;
}

struct Run {
    engine::CombCancelStatus comb{};

    // Mean over lines and frames of a line's bin over the frame's median, dB.
    double line_height_db = 0.0;
    std::size_t frames_measured = 0;
};

[[nodiscard]] Run run_capture(const std::string& uri, bool comb_on) {
    engine::EngineConfig config;
    config.channels = kChannels;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = 32'768;
    config.spectrum_transform = kTransform;
    config.comb_cancel = comb_on;
    config.gpu_index = -1;

    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;
    const auto opened = eng.open_source(uri);
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    std::mutex lock;
    Run out;
    double height_sum = 0.0;
    std::size_t height_count = 0;
    std::vector<float> sorted;
    const auto attached =
        eng.set_spectrum_sink([&](const engine::SpectrumFrame& frame) -> Status {
            const double t = static_cast<double>(frame.start) / static_cast<double>(kRate);
            if (t < kMeasureFrom) {
                return {};
            }
            const std::lock_guard<std::mutex> guard(lock);
            sorted.assign(frame.power_db.begin(), frame.power_db.end());
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            const double median = sorted[sorted.size() / 2];
            const double bin_width = frame.geometry.bin_width_hz();
            const double bin_zero = frame.geometry.bin_zero_hz();
            // The frame's own middle, which is where the comb was over it.
            const double mid = t + static_cast<double>(frame.count) /
                                       (2.0 * static_cast<double>(kRate));
            for (std::int32_t k = kLow; k <= kHigh; ++k) {
                const double f = offset_at(mid) + static_cast<double>(k) * spacing_at(mid);
                const auto bin = static_cast<std::ptrdiff_t>(std::lround((f - bin_zero) / bin_width));
                if (bin < 0 || bin >= static_cast<std::ptrdiff_t>(frame.power_db.size())) {
                    continue;
                }
                height_sum += frame.power_db[static_cast<std::size_t>(bin)] - median;
                ++height_count;
            }
            ++out.frames_measured;
            return {};
        });
    INFO(test::message_of(attached));
    REQUIRE(attached.has_value());

    const auto ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    out.comb = eng.graph_conditions().comb_cancel;
    out.line_height_db = height_count == 0 ? 0.0 : height_sum / static_cast<double>(height_count);
    return out;
}

[[nodiscard]] const char* lock_name(dsp::CombLock lock) {
    switch (lock) {
        case dsp::CombLock::kIdle: return "idle";
        case dsp::CombLock::kAcquiring: return "acquiring";
        case dsp::CombLock::kLocked: return "locked";
        case dsp::CombLock::kLost: return "lost";
    }
    return "unknown";
}

}  // namespace

TEST_CASE("the comb cancellation locks to a drifting comb and takes it out of the spectrum",
          "[gpu][engine][comb]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    const std::vector<dsp::Complex32> capture = render_capture();
    const std::filesystem::path path = test::unique_temp_path("revenant_test_comb", ".cf32");
    {
        std::FILE* file = std::fopen(path.string().c_str(), "wb");
        REQUIRE(file != nullptr);
        const std::size_t wrote =
            std::fwrite(capture.data(), sizeof(dsp::Complex32), capture.size(), file);
        std::fclose(file);
        REQUIRE(wrote == capture.size());
    }
    struct Remove {
        std::filesystem::path path;
        ~Remove() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } remove{path};

    const std::string uri = "file:///" + path.generic_string() +
                            "?rate=" + std::to_string(kRate) + "&format=cf32&center=7100000";

    const Run off = run_capture(uri, false);
    const Run on = run_capture(uri, true);

    std::printf("comb off: line height %.1f dB over %zu frames\n", off.line_height_db,
                off.frames_measured);
    std::printf("comb on:  line height %.1f dB over %zu frames; %s, %u lines cancelled at "
                "%.1f dB, spacing %.3f Hz, offset %.3f Hz, drift %.3f Hz/s, found in %llu of "
                "%llu detections\n",
                on.line_height_db, on.frames_measured, lock_name(on.comb.status.lock),
                on.comb.status.lines_cancelled, static_cast<double>(on.comb.status.suppression_db),
                on.comb.status.comb.spacing_hz, on.comb.status.comb.offset_hz,
                static_cast<double>(on.comb.status.spacing_drift_hz_per_s),
                static_cast<unsigned long long>(on.comb.detections_found),
                static_cast<unsigned long long>(on.comb.detections));

    REQUIRE(off.frames_measured > 0);
    REQUIRE(on.frames_measured > 0);

    // The comb is there to be removed: well over the floor with the stage off.
    CHECK(off.line_height_db > 10.0);

    // The stage found it, locked, and holds most of its lines.
    CHECK_FALSE(off.comb.on);
    CHECK(on.comb.on);
    CHECK(on.comb.detections_found > 0);
    REQUIRE(on.comb.status.lock == dsp::CombLock::kLocked);
    CHECK(on.comb.status.lines_cancelled >= dsp::kCombMinLines);
    // Held whole or as its two halves (detect_combs), so the comb reported is
    // at the spacing or twice it.
    const double spacing = on.comb.status.comb.spacing_hz;
    CHECK((std::abs(spacing - spacing_at(kSeconds)) < 5.0 ||
           std::abs(spacing - 2.0 * spacing_at(kSeconds)) < 10.0));

    // And the spectrum shows it gone: most of the lines' height taken away.
    CHECK(on.line_height_db < off.line_height_db - 10.0);
    CHECK(on.line_height_db < 6.0);
}
