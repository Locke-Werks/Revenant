// The RTL-SDR's front end, measured: the probes behind docs/calibration.md.
//
// Every case here is hidden, [.probe], and none of them asserts much. They are
// instruments: each one runs the real dongle under the machine-wide lock and
// writes what it saw to a directory, and docs/calibration.md's figures were read
// off those files. They are kept so a later dongle, a later librtlsdr or a
// later engine can be measured the same way rather than by memory.
//
// REVENANT_PROBE_DIR names the output directory and every case skips without
// it, so a bare `[.probe]` run writes nothing anywhere by accident.
//
// Two instruments and one check:
//
//   raw capture   the dongle's own bytes, cu8, straight off the source with no
//                 engine in the way. One file per capture, named for the time
//                 it started and the frequency it was taken at, so a warm-up
//                 run is a directory of files in time order. The DC offset,
//                 the I/Q imbalance and a carrier's frequency are all read off
//                 these, because each is a property of the samples before
//                 anything corrects them.
//
//   engine trace  the engine's spectrum around the centre, frame by frame,
//                 while a script switches the correction and retunes and
//                 changes gain underneath it, with the front-end estimate
//                 sampled beside it. This is what the waterfall shows, which
//                 is the question "is there a visible transient" is about.
//
//   restart       a calibration set through one engine and read back by the
//                 next, on the real dongle and a real file.
//
// Both write little-endian binary with a text header beside it; the formats
// are described where each is written.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/dsp/types.h"
#include "core/engine/engine.h"
#include "core/source/registry.h"
#include "core/source/source.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/support/dongle_lock.h"

using namespace revenant;

namespace {

[[nodiscard]] std::string env_or(const char* name, std::string_view fallback) {
    const char* value = std::getenv(name);
    return (value == nullptr || *value == '\0') ? std::string(fallback) : std::string(value);
}

[[nodiscard]] double env_seconds(const char* name, double fallback) {
    const char* value = std::getenv(name);
    return (value == nullptr || *value == '\0') ? fallback : std::strtod(value, nullptr);
}

[[nodiscard]] std::filesystem::path probe_dir() {
    const std::string dir = env_or("REVENANT_PROBE_DIR", "");
    if (dir.empty()) {
        SKIP("REVENANT_PROBE_DIR is not set, and this probe only writes files");
    }
    std::filesystem::create_directories(dir);
    return dir;
}

[[nodiscard]] std::int64_t unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// "98100000,99300000" or "98.1M" style is not needed here: plain hertz.
[[nodiscard]] std::vector<dsp::Hertz> parse_hertz_list(std::string_view text) {
    std::vector<dsp::Hertz> out;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string item(text.substr(0, comma));
        if (!item.empty()) {
            out.push_back(std::strtoll(item.c_str(), nullptr, 10));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return out;
}

// What the sink hands the probe's own thread. The sink runs on the source's
// delivery thread and must not call back into the source, so it only copies.
struct Capture {
    std::mutex lock;
    bool wanted = false;
    std::size_t target_samples = 0;
    std::vector<std::uint8_t> bytes;
    std::uint64_t dropped_inside = 0;
    dsp::SampleIndex first_index = 0;
    bool done = false;
};

}  // namespace

// REVENANT_PROBE_URI       the dongle, default rtlsdr://0?freq=98100000&rate=2400000&gain=20
// REVENANT_PROBE_FREQS     comma-separated hertz to capture at in turn, default the URI's own
// REVENANT_PROBE_SECONDS   length of one capture, default 4
// REVENANT_PROBE_SETTLE_S  discarded after a retune before capturing, default 0.5
// REVENANT_PROBE_PERIOD_S  a pass over the frequencies starts this often, default 0 (once)
// REVENANT_PROBE_TOTAL_S   stop starting passes after this long, default 0 (one pass)
//
// Writes <unix_ms>_<hz>.cu8, interleaved I then Q, one byte each, exactly as the
// dongle delivered them, and appends a line per capture to captures.txt:
// file, unix ms, requested hz, device centre hz, rate, samples, samples the
// source reported lost inside the capture. The dongle streams for the whole
// run, between captures too, so a warm-up run keeps the parts as warm as an
// operator's session does.
TEST_CASE("the dongle's raw samples are written to disk", "[.probe][source][rtlsdr][dongle]") {
    const std::filesystem::path dir = probe_dir();
    const source::DeviceLock held = test::hold_the_dongle();

    const std::string uri =
        env_or("REVENANT_PROBE_URI", "rtlsdr://0?freq=98100000&rate=2400000&gain=20");
    auto opened = source::open_source(uri);
    REQUIRE(opened.has_value());
    source::Source& radio = **opened;

    std::vector<dsp::Hertz> freqs = parse_hertz_list(env_or("REVENANT_PROBE_FREQS", ""));
    if (freqs.empty()) {
        freqs.push_back(radio.center());
    }
    const double seconds = env_seconds("REVENANT_PROBE_SECONDS", 4.0);
    const double settle = env_seconds("REVENANT_PROBE_SETTLE_S", 0.5);
    const double period = env_seconds("REVENANT_PROBE_PERIOD_S", 0.0);
    const double total = env_seconds("REVENANT_PROBE_TOTAL_S", 0.0);

    const source::SourceCapabilities& caps = radio.capabilities();
    {
        std::ofstream about(dir / "device.txt", std::ios::app);
        about << std::format("{}\turi={}\tname={}\tserial='{}'\trate={}\tcentre={}\n", unix_ms(),
                             uri, caps.display_name, caps.serial, radio.sample_rate(),
                             radio.center());
    }

    Capture capture;
    auto started = radio.start({}, [&capture](const source::SourceBlock& block) -> Status {
        std::scoped_lock guard(capture.lock);
        if (!capture.wanted || capture.done) {
            return {};
        }
        if (capture.bytes.empty()) {
            capture.first_index = block.stamp.start;
        } else {
            capture.dropped_inside += block.dropped_before;
        }
        const auto* first = reinterpret_cast<const std::uint8_t*>(block.bytes.data());
        const std::size_t room = capture.target_samples * 2 - capture.bytes.size();
        const std::size_t take = std::min(room, block.bytes.size());
        capture.bytes.insert(capture.bytes.end(), first, first + take);
        if (capture.bytes.size() >= capture.target_samples * 2) {
            capture.done = true;
        }
        return {};
    });
    REQUIRE(started.has_value());

    const auto run_start = std::chrono::steady_clock::now();
    std::ofstream log(dir / "captures.txt", std::ios::app);
    std::size_t passes = 0;
    while (true) {
        const auto pass_start = std::chrono::steady_clock::now();
        for (const dsp::Hertz freq : freqs) {
            if (radio.center() != freq) {
                auto tuned = radio.tune(freq);
                INFO("tune to " << freq);
                REQUIRE(tuned.has_value());
            }
            std::this_thread::sleep_for(std::chrono::duration<double>(settle));

            const auto samples = static_cast<std::size_t>(seconds * static_cast<double>(radio.sample_rate()));
            const std::int64_t began = unix_ms();
            {
                std::scoped_lock guard(capture.lock);
                capture.bytes.clear();
                capture.bytes.reserve(samples * 2);
                capture.target_samples = samples;
                capture.dropped_inside = 0;
                capture.done = false;
                capture.wanted = true;
            }
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                std::scoped_lock guard(capture.lock);
                if (capture.done) {
                    capture.wanted = false;
                    break;
                }
                REQUIRE(radio.running());
            }

            const std::string name = std::format("{}_{}.cu8", began, freq);
            {
                std::scoped_lock guard(capture.lock);
                std::ofstream out(dir / name, std::ios::binary);
                out.write(reinterpret_cast<const char*>(capture.bytes.data()),
                          static_cast<std::streamsize>(capture.bytes.size()));
                log << std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\n", name, began, freq,
                                   radio.center(), radio.sample_rate(), capture.bytes.size() / 2,
                                   capture.dropped_inside);
                log.flush();
            }
            WARN("captured " << name);
        }
        ++passes;

        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - run_start).count();
        if (period <= 0.0 || elapsed >= total) {
            break;
        }
        const auto next = pass_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                           std::chrono::duration<double>(period));
        std::this_thread::sleep_until(next);
    }

    const source::SourceStats stats = radio.stats();
    REQUIRE(radio.stop().has_value());
    WARN(passes << " passes, " << stats.samples_delivered << " samples delivered, "
                << stats.samples_lost << " lost in " << stats.overrun_events << " events");
}

// REVENANT_PROBE_URI       as above
// REVENANT_PROBE_PPB       the correction to store, default -500
//
// One engine opens the dongle with a calibration file in REVENANT_PROBE_DIR,
// sets a calibration and is destroyed; a second engine opens the same URI on
// the same file and must find it in force. What the file held between the two
// is written to restart.txt beside what each engine reported.
TEST_CASE("a calibration set on the dongle is in force after the engine restarts",
          "[.probe][gpu][engine][rtlsdr][dongle]") {
    REVENANT_NEEDS_GPU();
    const std::filesystem::path dir = probe_dir();
    const source::DeviceLock held = test::hold_the_dongle();

    const std::string uri =
        env_or("REVENANT_PROBE_URI", "rtlsdr://0?freq=98500000&rate=2400000&gain=20");
    const std::int64_t ppb = std::strtoll(env_or("REVENANT_PROBE_PPB", "-500").c_str(), nullptr, 10);
    const std::filesystem::path file = dir / "calibration.txt";
    std::filesystem::remove(file);

    engine::EngineConfig config;
    config.gpu_index = -1;
    config.channels = 64;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = 65'536;
    config.calibration_path = file.string();

    std::ofstream report(dir / "restart.txt");
    const source::DeviceCalibration wanted{.correction_ppb = ppb, .dc_removal = true,
                                           .iq_correction = true};
    {
        auto created = engine::Engine::create(config);
        REQUIRE(created.has_value());
        engine::Engine& first = **created;
        auto opened = first.open_source(uri);
        INFO((opened ? std::string() : opened.error().message));
        REQUIRE(opened.has_value());
        auto before = first.calibration();
        REQUIRE(before.has_value());
        report << std::format("first engine at open: key={} ppb={} dc={} iq={} persisted={} "
                              "source_center={} device_center={} note={}\n",
                              before->key, before->settings.correction_ppb,
                              before->settings.dc_removal, before->settings.iq_correction,
                              before->persisted, first.info().source_center,
                              before->device_center, before->note);
        auto set = first.set_calibration(wanted);
        REQUIRE(set.has_value());
        report << std::format("first engine after set: key={} ppb={} dc={} iq={} persisted={} "
                              "applied={} source_center={} device_center={}\n",
                              set->key, set->settings.correction_ppb, set->settings.dc_removal,
                              set->settings.iq_correction, set->persisted,
                              set->correction_applied, first.info().source_center,
                              set->device_center);
        CHECK(set->persisted);
        REQUIRE(first.close_source().has_value());
    }

    {
        std::ifstream in(file);
        report << "file between the two engines:\n" << in.rdbuf() << "\n";
    }

    auto created = engine::Engine::create(config);
    REQUIRE(created.has_value());
    engine::Engine& second = **created;
    auto opened = second.open_source(uri);
    INFO((opened ? std::string() : opened.error().message));
    REQUIRE(opened.has_value());
    auto after = second.calibration();
    REQUIRE(after.has_value());
    report << std::format("second engine at open: key={} ppb={} dc={} iq={} persisted={} "
                          "applied={} source_center={} device_center={} note={}\n",
                          after->key, after->settings.correction_ppb, after->settings.dc_removal,
                          after->settings.iq_correction, after->persisted,
                          after->correction_applied, second.info().source_center,
                          after->device_center, after->note);
    CHECK(after->settings == wanted);
    CHECK(after->persisted);
    CHECK(after->correction_applied);
    CHECK(after->front_end.dc_removal);
    CHECK(after->front_end.iq_correction);
    CHECK(second.info().source_center != after->device_center);
}

// REVENANT_PROBE_URI       as above
// REVENANT_PROBE_SCRIPT    "t:action;t:action", t in seconds from the start, actions
//                          dc=on|off, iq=on|off, tune=<hz>, gain=<db>, stop
// REVENANT_PROBE_HALF_BINS bins kept either side of the centre bin, default 2048
// REVENANT_PROBE_CHANNELS  channelizer channels, default 0, the engine's choice:
//                          8 at 2.4 MS/s, 293 Hz bins; 64 gives 36.6 Hz bins
//
// Writes trace.bin, one record per spectrum frame: int64 frame start, int64
// frame count, then 2 * half + 1 float32 dBFS bins centred on baseband zero;
// trace.txt, the geometry and every scripted event with the stream index it
// took effect near; and estimate.txt, the engine's front-end estimate sampled
// every 100 ms.
TEST_CASE("the engine's spectrum at the centre follows a scripted session",
          "[.probe][gpu][engine][rtlsdr][dongle]") {
    REVENANT_NEEDS_GPU();
    const std::filesystem::path dir = probe_dir();
    const source::DeviceLock held = test::hold_the_dongle();

    const std::string uri =
        env_or("REVENANT_PROBE_URI", "rtlsdr://0?freq=98100000&rate=2400000&gain=20");
    const std::string script =
        env_or("REVENANT_PROBE_SCRIPT", "3:dc=on;6:tune=99300000;9:gain=40;12:gain=10;15:stop");
    const auto half = static_cast<std::size_t>(
        std::strtoull(env_or("REVENANT_PROBE_HALF_BINS", "2048").c_str(), nullptr, 10));

    // The geometry revenant-engine uses by default, so the bins are the ones
    // an operator's waterfall has.
    engine::EngineConfig config;
    config.gpu_index = -1;
    config.channels = static_cast<std::uint32_t>(
        std::strtoul(env_or("REVENANT_PROBE_CHANNELS", "0").c_str(), nullptr, 10));
    config.taps_per_branch = 17;
    config.ring_seconds = 2.0;
    config.block_samples = 65'536;
    config.spectrum_transform = 2048;
    config.spectrum_rows_per_second = 30.0;
    auto created = engine::Engine::create(config);
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;
    auto opened = eng.open_source(uri);
    INFO((opened ? std::string() : opened.error().message));
    REQUIRE(opened.has_value());

    source::DeviceCalibration calibration{};
    REQUIRE(eng.set_calibration(calibration).has_value());

    std::ofstream trace(dir / "trace.bin", std::ios::binary);
    std::ofstream text(dir / "trace.txt");
    std::mutex trace_lock;
    bool wrote_geometry = false;
    auto sink = eng.set_spectrum_sink([&](const engine::SpectrumFrame& frame) -> Status {
        std::scoped_lock guard(trace_lock);
        const double zero = frame.geometry.bin_zero_hz();
        const double width = frame.geometry.bin_width_hz();
        const auto centre = static_cast<std::size_t>(std::llround(-zero / width));
        if (!wrote_geometry) {
            text << std::format("geometry\tbins={}\tbin_width_hz={:.6f}\tcentre_bin={}\thalf={}\n",
                                frame.power_db.size(), width, centre, half);
            wrote_geometry = true;
        }
        if (centre < half || centre + half >= frame.power_db.size()) {
            return {};
        }
        const auto start = static_cast<std::int64_t>(frame.start);
        const auto count = static_cast<std::int64_t>(frame.count);
        trace.write(reinterpret_cast<const char*>(&start), sizeof start);
        trace.write(reinterpret_cast<const char*>(&count), sizeof count);
        trace.write(reinterpret_cast<const char*>(frame.power_db.data() + (centre - half)),
                    static_cast<std::streamsize>((2 * half + 1) * sizeof(float)));
        return {};
    });
    REQUIRE(sink.has_value());

    Status run_status;
    std::thread runner([&] { run_status = eng.run(); });

    std::atomic<bool> sampling{true};
    std::thread sampler([&] {
        std::ofstream estimates(dir / "estimate.txt");
        while (sampling.load()) {
            if (auto state = eng.calibration(); state.has_value()) {
                const dsp::FrontEndEstimate& e = state->front_end.estimate;
                estimates << std::format(
                    "{}\t{}\t{}\t{}\t{}\t{:.9e}\t{:.9e}\t{:.4f}\t{:.5f}\t{:.4f}\t{}\t{}\n",
                    unix_ms(), eng.source_stats().write_index, state->front_end.dc_removal,
                    state->front_end.iq_correction, state->front_end.blocks_measured, e.dc_i,
                    e.dc_q, e.dc_dbfs(), e.gain_error_db(), e.phase_error_deg(), e.iq_plausible,
                    e.image_rejection_db());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    const auto begin = std::chrono::steady_clock::now();
    std::string_view rest = script;
    while (!rest.empty()) {
        const std::size_t semi = rest.find(';');
        const std::string step(rest.substr(0, semi));
        rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
        const std::size_t colon = step.find(':');
        REQUIRE(colon != std::string::npos);
        const double at = std::strtod(step.substr(0, colon).c_str(), nullptr);
        const std::string action = step.substr(colon + 1);
        std::this_thread::sleep_until(
            begin + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(at)));

        const dsp::SampleIndex before = eng.source_stats().write_index;
        std::string outcome = "ok";
        if (action == "stop") {
            break;
        }
        if (action.starts_with("dc=") || action.starts_with("iq=")) {
            const bool on = action.ends_with("on");
            (action.starts_with("dc=") ? calibration.dc_removal : calibration.iq_correction) = on;
            if (auto set = eng.set_calibration(calibration); !set) {
                outcome = set.error().message;
            }
        } else if (action.starts_with("tune=")) {
            const dsp::Hertz to = std::strtoll(action.c_str() + 5, nullptr, 10);
            auto tuned = eng.set_source_center(to);
            outcome = tuned ? std::format("centre {}", tuned->center) : tuned.error().message;
        } else if (action.starts_with("gain=")) {
            const double db = std::strtod(action.c_str() + 5, nullptr);
            auto set = eng.set_source_gain("tuner", db);
            outcome = set ? std::format("gain {}", *set) : set.error().message;
        } else {
            outcome = "unknown action";
        }
        const dsp::SampleIndex after = eng.source_stats().write_index;
        std::scoped_lock guard(trace_lock);
        text << std::format("event\tat_s={}\t{}\tindex_before={}\tindex_after={}\t{}\n", at,
                            action, before, after, outcome);
        text.flush();
    }

    REQUIRE(eng.stop().has_value());
    runner.join();
    sampling.store(false);
    sampler.join();
    (void)eng.set_spectrum_sink(nullptr);
    INFO(test::message_of(run_status));
    CHECK(run_status.has_value());
    const source::SourceStats stats = eng.source_stats();
    WARN(stats.samples_delivered << " samples delivered, " << stats.samples_lost << " lost in "
                                 << stats.overrun_events << " events");
}
