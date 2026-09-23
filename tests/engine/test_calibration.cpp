// A crystal's frequency error, corrected in integer hertz, and a device's
// calibration kept between sessions by its serial.
//
// No case here touches a radio. The front end is tests/engine/impaired_front_end.h,
// whose crystal is off by a number this file chooses, so a label the engine
// publishes can be compared with the carrier's true frequency rather than with
// what some other part of the engine believes.
//
// The arithmetic and the file cases need no device. The engine cases do,
// because an engine needs a GPU to exist at all, and one of them reads the
// corrected frequency off a spectrum frame the device computed.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "core/engine/engine.h"
#include "core/engine/open_built_source.h"
#include "core/source/calibration.h"
#include "core/source/corrected_source.h"
#include "core/source/frequency_correction.h"
#include "tests/engine/impaired_front_end.h"
#include "tests/engine/spectrum_probe.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

namespace {

// NOAA weather radio's 162.550 MHz, heard through a dongle whose crystal runs
// 20 ppm fast. Uncorrected, it reads 3251 Hz low: 162550000 / (1 + 20e-6) is
// 162546749.07.
constexpr dsp::Hertz kCarrierHz = 162'550'000;
constexpr std::int64_t kClockErrorPpb = 20'000;
constexpr dsp::Hertz kDeviceCenterHz = 162'500'000;
constexpr dsp::SampleRate kRate = 2'400'000;

test::ImpairedFrontEndConfig skewed_front_end(std::string serial, dsp::SampleIndex length) {
    test::ImpairedFrontEndConfig config;
    config.rate = kRate;
    config.device_center = kDeviceCenterHz;
    config.clock_error_ppb = kClockErrorPpb;
    config.carrier_hz = kCarrierHz;
    config.carrier_amplitude = 0.1;
    config.noise_rms = 1.0e-4;
    config.length = length;
    config.serial = std::move(serial);
    return config;
}

engine::EngineConfig calibration_engine_config(std::string calibration_path) {
    engine::EngineConfig config;
    config.channels = 64;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = 32'768;
    config.spectrum_transform = 2048;
    config.gpu_index = -1;  // honours REVENANT_GPU_INDEX
    config.calibration_path = std::move(calibration_path);
    return config;
}

// A file under the build's temporary directory, removed when the case ends.
struct ScratchFile {
    std::filesystem::path path;

    explicit ScratchFile(std::string_view name) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               std::format("revenant-calibration-{}-{}", stamp, name) / "calibration.txt";
    }
    ~ScratchFile() {
        std::error_code ignored;
        std::filesystem::remove_all(path.parent_path(), ignored);
    }
    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;

    [[nodiscard]] std::string text() const {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream contents;
        contents << in.rdbuf();
        return contents.str();
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// The arithmetic. Integer hertz in, integer hertz out, one rounding each.
// ---------------------------------------------------------------------------

TEST_CASE("a correction in parts per billion moves a frequency by an exact number of hertz",
          "[calibration]") {
    // 20 ppm at 162.5 MHz is 3250 Hz, exactly.
    CHECK(source::device_to_true(162'500'000, 20'000) == 162'503'250);
    CHECK(source::true_to_device(162'503'250, 20'000) == 162'500'000);

    // Negative is a slow crystal and labels read high.
    CHECK(source::device_to_true(100'000'000, -1'234) == 99'999'877);

    // A fractional ppm is representable, which a whole-ppm correction is not:
    // 0.4 ppm at 100 MHz is 40 Hz.
    CHECK(source::device_to_true(100'000'000, 400) == 100'000'040);

    // The largest frequency and correction anything accepts stay inside 64
    // bits: a remainder under 10^9 times a factor under 1.001 * 10^9.
    CHECK(source::device_to_true(6'000'000'000, source::kMaxCorrectionPpb) == 6'006'000'000);
    CHECK(source::true_to_device(6'006'000'000, source::kMaxCorrectionPpb) == 6'000'000'000);
    CHECK(source::device_to_true(-100'000, 20'000) == -100'002);
}

TEST_CASE("a round trip through the correction lands within a hertz", "[calibration]") {
    // The device takes whole hertz of its own scale, each 1 + e true hertz
    // wide, so the round trip cannot be exact everywhere. It is never off by
    // more than one.
    std::int64_t worst = 0;
    for (std::int64_t f = 24'000'000; f < 1'766'000'000; f += 7'777'777) {
        for (const std::int64_t ppb : {-150'000, -20'000, -1, 1, 437, 20'000, 150'000}) {
            const std::int64_t back = source::device_to_true(source::true_to_device(f, ppb), ppb);
            worst = std::max(worst, back > f ? back - f : f - back);
        }
    }
    INFO("worst round trip " << worst << " Hz");
    CHECK(worst <= 1);
}

TEST_CASE("a carrier's measured correction is the crystal's error wherever it sits in the span",
          "[calibration]") {
    // The label is the corrected centre plus a baseband offset counted on the
    // device's sample clock, so measuring from a carrier at the edge of the
    // span gives the same answer as one on the centre.
    const test::ImpairedFrontEndConfig config = skewed_front_end("", 0);
    for (const dsp::Hertz device_center : {162'549'000, 162'500'000, 161'500'000}) {
        const double offset = test::ImpairedFrontEnd::baseband_hz(config, device_center);
        for (const std::int64_t current : {std::int64_t{0}, std::int64_t{5'000},
                                           std::int64_t{-30'000}}) {
            const std::int64_t centre = source::device_to_true(device_center, current);
            const auto observed = centre + static_cast<std::int64_t>(std::llround(offset));
            const std::int64_t measured =
                source::measured_correction_ppb(kCarrierHz, observed, centre, current);
            INFO("device at " << device_center << " Hz, offset " << offset << " Hz, current "
                              << current << " ppb: measured " << measured << " ppb");
            // A label rounded to the hertz is 0.5 Hz in 162.5 MHz, 3 ppb.
            CHECK(std::llabs(measured - kClockErrorPpb) <= 4);
        }
    }
}

// ---------------------------------------------------------------------------
// The file.
// ---------------------------------------------------------------------------

TEST_CASE("the calibration file keeps each device's settings and survives a round trip",
          "[calibration]") {
    source::CalibrationTable table;
    table["rtlsdr:00000001"] = {.correction_ppb = -1'250, .dc_removal = true, .iq_correction = false};
    table["rtlsdr:SPARE 2"] = {.correction_ppb = 450, .dc_removal = false, .iq_correction = true};

    const std::string text = source::format_calibration_table(table);
    CHECK(text.find("rtlsdr:00000001\tppb=-1250\tdc=on\tiq=off\n") != std::string::npos);

    auto parsed = source::parse_calibration_table(text);
    INFO(test::message_of(parsed));
    REQUIRE(parsed.has_value());
    CHECK(*parsed == table);

    ScratchFile file("round-trip");
    REQUIRE(source::save_calibration_table(file.path, table).has_value());
    auto loaded = source::load_calibration_table(file.path);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == table);
    CHECK(file.text() == text);
}

TEST_CASE("a missing calibration file is an empty table and a broken one is refused by line",
          "[calibration]") {
    auto missing = source::load_calibration_table(std::filesystem::temp_directory_path() /
                                                  "revenant-no-such-dir" / "calibration.txt");
    REQUIRE(missing.has_value());
    CHECK(missing->empty());

    // A later engine's field is skipped rather than refused.
    auto later = source::parse_calibration_table("rtlsdr:1\tppb=5\tagc=fast\n");
    REQUIRE(later.has_value());
    CHECK(later->at("rtlsdr:1").correction_ppb == 5);

    const auto refused_line = [](std::string_view text) {
        auto parsed = source::parse_calibration_table(text);
        return parsed.has_value() ? std::string() : parsed.error().message;
    };
    CHECK(refused_line("# header\nrtlsdr:1\tppb=five\n").find("line 2") != std::string::npos);
    CHECK(refused_line("rtlsdr:1\tdc=maybe\n").find("dc='maybe'") != std::string::npos);
    CHECK(refused_line("rtlsdr:1\tppb=2000000\n").find("past") != std::string::npos);
    CHECK(refused_line("rtlsdr:1\nrtlsdr:1\n").find("twice") != std::string::npos);
    CHECK(!source::validate_calibration_key("rtlsdr:a\tb").has_value());
}

// ---------------------------------------------------------------------------
// The wrapper, with no engine.
// ---------------------------------------------------------------------------

TEST_CASE("a corrected source tunes the real oscillator onto the frequency asked for",
          "[calibration]") {
    auto front = std::make_unique<test::ImpairedFrontEnd>(skewed_front_end("", 0));
    test::ImpairedFrontEnd& inner = *front;
    source::CorrectedSource corrected(std::move(front), kClockErrorPpb);

    // Opened at 162.5 MHz on its own scale, which is 162503250 Hz really.
    CHECK(corrected.center() == 162'503'250);
    CHECK(corrected.device_center() == kDeviceCenterHz);

    // Asked for the carrier, the device is told 162546749 and its oscillator,
    // 20 ppm fast, lands on the carrier to within a hertz.
    auto landed = corrected.tune(kCarrierHz);
    REQUIRE(landed.has_value());
    CHECK(inner.center() == 162'546'749);
    CHECK(*landed == kCarrierHz);
    const double carrier_in_stream =
        test::ImpairedFrontEnd::baseband_hz(skewed_front_end("", 0), inner.center());
    INFO("the carrier sits " << carrier_in_stream << " Hz from the stream's DC");
    CHECK(std::abs(carrier_in_stream) < 1.0);

    // A new correction moves the label and not the device.
    const std::uint64_t tunes = inner.tunes();
    REQUIRE(corrected.set_correction_ppb(0).has_value());
    CHECK(corrected.center() == 162'546'749);
    CHECK(inner.tunes() == tunes);
    CHECK(!corrected.set_correction_ppb(source::kMaxCorrectionPpb + 1).has_value());
}

// ---------------------------------------------------------------------------
// The engine.
// ---------------------------------------------------------------------------

TEST_CASE("a skewed crystal's carrier reads at its true frequency once the correction is set",
          "[gpu][engine][calibration]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // Two runs of the same half second: uncorrected, then corrected. A run
    // ends the stream for good, so each has its own engine.
    struct Reading {
        dsp::Hertz source_center = 0;
        double peak_label_hz = 0.0;
        double bin_width_hz = 0.0;
    };
    const auto measure = [](std::int64_t ppb) -> Reading {
        auto created = engine::Engine::create(calibration_engine_config(""));
        REQUIRE(created.has_value());
        engine::Engine& eng = **created;
        auto opened = engine::open_built_source(
            eng, std::make_unique<test::ImpairedFrontEnd>(skewed_front_end("", kRate / 2)));
        INFO(test::message_of(opened));
        REQUIRE(opened.has_value());
        auto set = eng.set_calibration({.correction_ppb = ppb});
        INFO(test::message_of(set));
        REQUIRE(set.has_value());

        const test::AveragedSpectrum spectrum = test::average_spectrum(eng, kRate / 8);
        INFO(test::message_of(spectrum.run_status));
        REQUIRE(spectrum.run_status.has_value());
        REQUIRE(spectrum.frames > 0);

        Reading out;
        out.source_center = eng.info().source_center;
        out.bin_width_hz = spectrum.geometry.bin_width_hz();
        out.peak_label_hz = static_cast<double>(out.source_center) +
                            spectrum.offset_of(spectrum.peak_bin());
        return out;
    };

    const Reading raw = measure(0);
    const Reading fixed = measure(kClockErrorPpb);
    INFO(std::format("uncorrected: centre {} Hz, carrier at {:.1f} Hz; corrected: centre {} Hz, "
                     "carrier at {:.1f} Hz; bins {:.2f} Hz",
                     raw.source_center, raw.peak_label_hz, fixed.source_center,
                     fixed.peak_label_hz, raw.bin_width_hz));

    CHECK(raw.source_center == kDeviceCenterHz);
    CHECK(fixed.source_center == 162'503'250);

    // Uncorrected it is 3251 Hz low, which is 89 bins. Corrected it is on the
    // carrier to within the bin it falls in; the offset term the correction
    // does not reach is 0.93 Hz here, 46.7 kHz times 20 ppm.
    CHECK(std::abs(raw.peak_label_hz - 162'546'749.07) <= raw.bin_width_hz);
    CHECK(std::abs(fixed.peak_label_hz - static_cast<double>(kCarrierHz)) <= fixed.bin_width_hz);

    // And the carrier measured off the uncorrected reading gives the crystal's
    // error back, to the bin's width over the frequency: 36.6 Hz in 162.5 MHz
    // is 225 ppb.
    const std::int64_t measured = source::measured_correction_ppb(
        kCarrierHz, std::llround(raw.peak_label_hz), raw.source_center, 0);
    INFO("measured " << measured << " ppb against " << kClockErrorPpb);
    CHECK(std::llabs(measured - kClockErrorPpb) <= 250);
}

TEST_CASE("a device's calibration is kept by its serial and restored when it opens again",
          "[gpu][engine][calibration]") {
    REVENANT_NEEDS_GPU();
    ScratchFile file("restore");

    auto created = engine::Engine::create(calibration_engine_config(file.path.string()));
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    // First session: nothing stored yet.
    REQUIRE(engine::open_built_source(
                eng, std::make_unique<test::ImpairedFrontEnd>(skewed_front_end("CAL-7", 0)))
                .has_value());
    auto first = eng.calibration();
    REQUIRE(first.has_value());
    CHECK(first->open);
    CHECK(first->key == "impaired:CAL-7");
    CHECK(first->persisted);
    CHECK(first->settings == source::DeviceCalibration{});
    CHECK(eng.info().source_center == kDeviceCenterHz);

    const source::DeviceCalibration wanted{
        .correction_ppb = kClockErrorPpb, .dc_removal = true, .iq_correction = true};
    auto set = eng.set_calibration(wanted);
    INFO(test::message_of(set));
    REQUIRE(set.has_value());
    CHECK(set->persisted);
    CHECK(set->note.empty());
    CHECK(set->settings == wanted);
    CHECK(set->device_center == kDeviceCenterHz);
    CHECK(eng.info().source_center == 162'503'250);
    CHECK(file.text().find("impaired:CAL-7\tppb=20000\tdc=on\tiq=on") != std::string::npos);

    REQUIRE(eng.close_source().has_value());
    auto closed = eng.calibration();
    REQUIRE(closed.has_value());
    CHECK(!closed->open);

    // Second session, same serial: the settings come back and the centre is
    // corrected from the first block.
    REQUIRE(engine::open_built_source(
                eng, std::make_unique<test::ImpairedFrontEnd>(skewed_front_end("CAL-7", 0)))
                .has_value());
    auto restored = eng.calibration();
    REQUIRE(restored.has_value());
    CHECK(restored->settings == wanted);
    CHECK(restored->front_end.dc_removal);
    CHECK(restored->front_end.iq_correction);
    CHECK(eng.info().source_center == 162'503'250);
    REQUIRE(eng.close_source().has_value());

    // Another serial is a different device and starts uncalibrated, and its
    // line is added beside the first rather than over it.
    REQUIRE(engine::open_built_source(
                eng, std::make_unique<test::ImpairedFrontEnd>(skewed_front_end("CAL-8", 0)))
                .has_value());
    auto other = eng.calibration();
    REQUIRE(other.has_value());
    CHECK(other->settings == source::DeviceCalibration{});
    REQUIRE(eng.set_calibration({.correction_ppb = -500}).has_value());
    CHECK(file.text().find("impaired:CAL-7\t") != std::string::npos);
    CHECK(file.text().find("impaired:CAL-8\tppb=-500") != std::string::npos);
}

TEST_CASE("a calibration is not applied twice, not kept without a serial, and not written over "
          "a file it cannot read",
          "[gpu][engine][calibration]") {
    REVENANT_NEEDS_GPU();

    SECTION("a device that corrects its own crystal keeps the stored figure and ignores it") {
        ScratchFile file("device-corrects");
        source::CalibrationTable table;
        table["impaired:OWN"] = {.correction_ppb = kClockErrorPpb};
        REQUIRE(source::save_calibration_table(file.path, table).has_value());

        auto created = engine::Engine::create(calibration_engine_config(file.path.string()));
        REQUIRE(created.has_value());
        test::ImpairedFrontEndConfig config = skewed_front_end("OWN", 0);
        config.device_corrects_frequency = true;
        REQUIRE(engine::open_built_source(**created,
                                          std::make_unique<test::ImpairedFrontEnd>(config))
                    .has_value());
        auto state = (**created).calibration();
        REQUIRE(state.has_value());
        CHECK(state->settings.correction_ppb == kClockErrorPpb);
        CHECK(!state->correction_applied);
        CHECK(state->note.find("ppm=") != std::string::npos);
        CHECK((**created).info().source_center == kDeviceCenterHz);
    }

    SECTION("a source with no serial is calibrated for the session only") {
        ScratchFile file("no-serial");
        auto created = engine::Engine::create(calibration_engine_config(file.path.string()));
        REQUIRE(created.has_value());
        REQUIRE(engine::open_built_source(
                    **created, std::make_unique<test::ImpairedFrontEnd>(skewed_front_end("", 0)))
                    .has_value());
        auto state = (**created).set_calibration({.correction_ppb = 1'000});
        REQUIRE(state.has_value());
        CHECK(state->key.empty());
        CHECK(!state->persisted);
        CHECK(state->note.find("no serial") != std::string::npos);
        CHECK(!std::filesystem::exists(file.path));
        CHECK((**created).info().source_center == 162'500'163);
    }

    SECTION("an unreadable file is left as it is") {
        ScratchFile file("unreadable");
        std::filesystem::create_directories(file.path.parent_path());
        {
            std::ofstream out(file.path, std::ios::binary);
            out << "impaired:OTHER\tppb=12\n" << "impaired:BROKEN\tppb=twelve\n";
        }
        const std::string before = file.text();

        auto created = engine::Engine::create(calibration_engine_config(file.path.string()));
        REQUIRE(created.has_value());
        REQUIRE(engine::open_built_source(
                    **created,
                    std::make_unique<test::ImpairedFrontEnd>(skewed_front_end("OTHER", 0)))
                    .has_value());
        auto state = (**created).set_calibration({.correction_ppb = 3'000});
        REQUIRE(state.has_value());
        CHECK(!state->persisted);
        CHECK(state->note.find("could not be read") != std::string::npos);
        CHECK(state->settings.correction_ppb == 3'000);
        CHECK(file.text() == before);
    }

    SECTION("a correction past a thousand ppm is refused") {
        auto created = engine::Engine::create(calibration_engine_config(""));
        REQUIRE(created.has_value());
        CHECK(!(**created).set_calibration({.correction_ppb = 5}).has_value());
        REQUIRE(engine::open_built_source(
                    **created, std::make_unique<test::ImpairedFrontEnd>(skewed_front_end("", 0)))
                    .has_value());
        CHECK(!(**created).set_calibration({.correction_ppb = 1'000'001}).has_value());
    }
}
