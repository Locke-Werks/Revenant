// The manual notches on the passband display, and the carrier tracker beside
// them.
//
// The first cases need no device: core/engine/passband_notch.h's mapping,
// shaping and measurement on frames built here. The last runs a real engine
// on a synthetic AM carrier with two identical USB receivers, one notched on
// the carrier and one not, and reads what their passband frames carry.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include "core/dsp/noise_reference.h"
#include "core/dsp/spectrum_levels_reference.h"
#include "core/dsp/synth/wideband.h"
#include "core/engine/engine.h"
#include "core/engine/passband_notch.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;
using Catch::Approx;

namespace {

[[nodiscard]] engine::PassbandNotchRequest usb_request(dsp::Hertz hz, double depth,
                                                       dsp::Hertz width) {
    engine::PassbandNotchRequest request;
    request.demod = engine::Demod::Usb;
    request.audio_rate = 8'000;
    request.notches[0] = dsp::NotchSpec{true, hz, depth, width};
    return request;
}

}  // namespace

TEST_CASE("each mode maps its notches onto the passband where the audio came from",
          "[engine][passband][notch]") {
    engine::PassbandNotchRequest request;
    request.audio_rate = 8'000;
    request.cw_pitch = 700;
    request.notches[0] = dsp::NotchSpec{true, 1'000, 40.0, 100};
    request.notches[2] = dsp::NotchSpec{true, 1'600, 20.0, 50};

    request.demod = engine::Demod::Usb;
    auto marks = engine::map_passband_notches(request);
    REQUIRE(marks.count == 2);
    CHECK_FALSE(marks.audio_only);
    CHECK(marks.marks[0].offset_hz == 1'000.0);
    CHECK(marks.marks[1].offset_hz == 1'600.0);
    CHECK(marks.marks[1].notch == 2);
    CHECK(marks.marks[1].depth_db == 20.0);

    // LSB: the control is in the passband's frame, so -1000 Hz is 1 kHz
    // audio and is drawn at -1000 Hz.
    request.demod = engine::Demod::Lsb;
    request.notches[0].hz = -1'000;
    request.notches[2].enabled = false;
    marks = engine::map_passband_notches(request);
    REQUIRE(marks.count == 1);
    CHECK(marks.marks[0].offset_hz == -1'000.0);

    // AM places the one notch on both sidebands.
    request.demod = engine::Demod::Am;
    request.notches[0].hz = 1'000;
    marks = engine::map_passband_notches(request);
    REQUIRE(marks.count == 2);
    CHECK(marks.marks[0].offset_hz == -1'000.0);
    CHECK(marks.marks[1].offset_hz == 1'000.0);
    CHECK(marks.marks[0].notch == 0);
    CHECK(marks.marks[1].notch == 0);

    // CW: -200 Hz from the centre is 500 Hz of audio at a 700 Hz pitch.
    request.demod = engine::Demod::Cw;
    request.notches[0].hz = -200;
    marks = engine::map_passband_notches(request);
    REQUIRE(marks.count == 1);
    CHECK(marks.marks[0].offset_hz == Approx(-200.0));

    // NFM has no passband place for audio: a marker at the control.
    request.demod = engine::Demod::Nfm;
    request.notches[0].hz = 1'200;
    marks = engine::map_passband_notches(request);
    CHECK(marks.audio_only);
    REQUIRE(marks.count == 1);
    CHECK(marks.marks[0].offset_hz == 1'200.0);

    // Nothing enabled, nothing drawn.
    request.notches[0].enabled = false;
    marks = engine::map_passband_notches(request);
    CHECK(marks.count == 0);
    CHECK_FALSE(marks.audio_only);
}

TEST_CASE("a notch shapes a flat passband by the section's own response",
          "[engine][passband][notch]") {
    constexpr std::size_t kBins = 256;
    constexpr double kBinWidth = 50.0;
    constexpr double kBinZero = -6'400.0;
    constexpr float kFlat = -100.0F;

    const auto request = usb_request(1'000, 40.0, 100);
    const auto marks = engine::map_passband_notches(request);

    std::vector<float> power(kBins, kFlat);
    std::vector<std::uint8_t> excluded(kBins, 7);
    const std::size_t notched =
        engine::shape_passband(power, kBinWidth, kBinZero, request, marks, excluded);

    const auto bin_of = [&](double offset) {
        return static_cast<std::size_t>(std::lround((offset - kBinZero) / kBinWidth));
    };

    // At the centre the frame drops by what the section does there, which is
    // the depth asked for to within the float rounding of its coefficients.
    const auto expected = dsp::notch_response_db(1'000.0, 40.0, 100.0, 8'000, 1'000.0);
    REQUIRE(expected.has_value());
    CHECK(static_cast<double>(power[bin_of(1'000.0)] - kFlat) == Approx(*expected).margin(0.01));
    CHECK(*expected == Approx(-40.0).margin(1.0));

    // A kilohertz away it barely touches the band.
    CHECK(std::abs(power[bin_of(2'500.0)] - kFlat) < 0.5F);

    // The lower sideband is not USB audio and is left exactly alone.
    CHECK(power[bin_of(-1'000.0)] == kFlat);

    // 950, 1000 and 1050 Hz are inside the 100 Hz width, and nothing else.
    CHECK(notched == 3);
    for (std::size_t b = 0; b < kBins; ++b) {
        const double offset = kBinZero + kBinWidth * static_cast<double>(b);
        INFO(std::format("bin {} at {} Hz", b, offset));
        CHECK(excluded[b] == (std::abs(offset - 1'000.0) <= 50.0 ? 1 : 0));
    }
}

TEST_CASE("an audio-only notch is listed and applies nothing", "[engine][passband][notch]") {
    engine::PassbandNotchRequest request = usb_request(1'000, 40.0, 100);
    request.demod = engine::Demod::Nfm;
    const auto marks = engine::map_passband_notches(request);
    REQUIRE(marks.audio_only);

    std::vector<float> power(64, -90.0F);
    std::vector<std::uint8_t> excluded(64, 0);
    CHECK(engine::shape_passband(power, 100.0, -3'200.0, request, marks, excluded) == 0);
    for (const float value : power) {
        CHECK(value == -90.0F);
    }
}

TEST_CASE("the notched bins are left out of the scale's percentiles",
          "[engine][passband][notch]") {
    // A noise ramp from -110 to -90 dB with a carrier 60 dB above it in the
    // bins a notch covers. Measured whole, the high percentile is the
    // carrier; measured outside the notch it is the ramp's top.
    constexpr std::size_t kBins = 400;
    std::vector<float> power(kBins);
    std::vector<std::uint8_t> excluded(kBins, 0);
    for (std::size_t b = 0; b < kBins; ++b) {
        power[b] = -110.0F + 20.0F * static_cast<float>(b) / static_cast<float>(kBins);
    }
    for (std::size_t b = 195; b < 205; ++b) {
        power[b] = -40.0F;
        excluded[b] = 1;
    }

    std::array<float, 2> whole{};
    dsp::SpectrumLevelsParams params;
    params.bins = kBins;
    REQUIRE(dsp::reference_spectrum_levels(params, power, whole).has_value());
    CHECK(whole[1] > -45.0F);

    std::vector<float> scratch(kBins);
    std::array<float, 2> outside{};
    REQUIRE(engine::measure_outside(power, excluded, scratch, outside));
    CHECK(outside[1] < -89.0F);
    CHECK(outside[0] < -108.0F);

    // Every bin excluded leaves nothing to measure, and says so.
    std::vector<std::uint8_t> all(kBins, 1);
    CHECK_FALSE(engine::measure_outside(power, all, scratch, outside));
}

namespace {

constexpr dsp::SampleRate kSourceRate = 2'400'000;
constexpr dsp::Hertz kSpacing = kSourceRate / 64;
constexpr dsp::Hertz kSceneCentre = kSpacing * 5;
constexpr dsp::Hertz kSlack = 1'600;
constexpr dsp::SampleIndex kSamples = 10'800'000;
constexpr dsp::Hertz kCarrierOffset = 1'500;

[[nodiscard]] std::string notch_scene_uri() {
    return std::format(
        "synthetic:wideband?rate={}&emitters=1&bursts=1&seed=515151&samples={}&modes=am"
        "&noise=on&noise_dbfs=-120&snr_min=60&snr_max=60&span_low={}&span_high={}"
        "&min_burst=4.2&max_burst=4.4",
        kSourceRate, kSamples, kSceneCentre - kSlack, kSceneCentre + kSlack);
}

[[nodiscard]] Expected<siggen::Scene> notch_scene() {
    siggen::SceneSpec spec;
    spec.rate = kSourceRate;
    spec.center_hz = 0;
    spec.duration_samples = kSamples;
    spec.epoch_anchor_ns = 0;
    spec.seed = 515'151;
    spec.add_noise = true;
    spec.noise_power_full_band_dbfs = -120.0;
    spec.worker_threads = 1;
    spec.random.emitter_count = 1;
    spec.random.bursts_per_emitter = 1;
    spec.random.span_low_hz = kSceneCentre - kSlack;
    spec.random.span_high_hz = kSceneCentre + kSlack;
    spec.random.snr_in_occupied_bandwidth_db_min = 60.0;
    spec.random.snr_in_occupied_bandwidth_db_max = 60.0;
    spec.random.min_burst_seconds = 4.2;
    spec.random.max_burst_seconds = 4.4;
    spec.random.palette = {siggen::Modulation::Am};
    return siggen::Scene::create(spec);
}

struct Seen {
    engine::VrxId vrx;
    double seconds = 0.0;
    float at_carrier = 0.0F;
    float raw_at_carrier = 0.0F;
    bool has_raw = false;
    double peak_hz = 0.0;
    float peak_db = -1.0e30F;
    std::size_t marks = 0;
    bool audio_only = false;
    std::vector<engine::PassbandCarrier> carriers;
};

}  // namespace

TEST_CASE("a notched carrier drops in the passband frame and stays tracked",
          "[gpu][engine][passband][notch]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    auto scene = notch_scene();
    INFO(test::message_of(scene));
    REQUIRE(scene.has_value());
    REQUIRE(scene->truth().size() == 1);
    const dsp::Hertz carrier = scene->truth().front().carrier_offset_hz;
    const double burst_start =
        static_cast<double>(scene->truth().front().start_sample) / kSourceRate;

    engine::EngineConfig config;
    config.channels = 64;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = 8'192;
    config.audio_rate = 8'000;
    config.passband_transform = 512;
    config.gpu_index = -1;

    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;
    REQUIRE(eng.open_source(notch_scene_uri()).has_value());

    std::mutex lock;
    std::vector<Seen> seen;

    // Two USB receivers with the carrier at +1500 Hz, one notched on it with
    // the split row on, one plain.
    std::array<engine::VrxId, 2> ids{};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        engine::VrxParams params;
        params.center = carrier - kCarrierOffset;
        params.demod = engine::Demod::Usb;
        params.passband_low = 100;
        params.passband_high = 2'900;
        params.squelch_dbfs = -300.0;
        if (i == 0) {
            params.notch_enabled = true;
            params.notch_hz = kCarrierOffset;
            params.notch_depth_db = 40.0;
            params.notch_width_hz = 100;
            params.display_split = true;
        }
        auto added = eng.add_vrx(params);
        INFO(test::message_of(added));
        REQUIRE(added.has_value());
        ids[i] = *added;

        auto attached =
            eng.set_passband_sink(*added, [&lock, &seen, center = params.center](
                                              const engine::PassbandFrame& frame) -> Status {
                Seen out;
                out.vrx = frame.vrx;
                out.seconds = static_cast<double>(frame.start) / kSourceRate;
                const double width = frame.geometry.bin_width_hz();
                const double zero =
                    frame.geometry.bin_zero_hz() - static_cast<double>(center);
                const auto bin = static_cast<std::size_t>(
                    std::lround((static_cast<double>(kCarrierOffset) - zero) / width));
                if (bin < frame.power_db.size()) {
                    out.at_carrier = frame.power_db[bin];
                    if (!frame.raw_power_db.empty()) {
                        out.has_raw = true;
                        out.raw_at_carrier = frame.raw_power_db[bin];
                    }
                }
                const auto& peak_row = frame.raw_power_db.empty() ? frame.power_db
                                                                   : frame.raw_power_db;
                for (std::size_t b = 0; b < peak_row.size(); ++b) {
                    if (peak_row[b] > out.peak_db) {
                        out.peak_db = peak_row[b];
                        out.peak_hz = zero + width * static_cast<double>(b);
                    }
                }
                out.marks = frame.notch_marks.size();
                out.audio_only = frame.notch_audio_only;
                out.carriers.assign(frame.carriers.begin(), frame.carriers.end());
                const std::lock_guard<std::mutex> guard(lock);
                seen.push_back(std::move(out));
                return {};
            });
        INFO(test::message_of(attached));
        REQUIRE(attached.has_value());
    }

    const auto ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    // Frames well inside the burst, so both receivers see the carrier.
    double notched_sum = 0.0;
    double raw_sum = 0.0;
    double plain_sum = 0.0;
    std::size_t notched_count = 0;
    std::size_t plain_count = 0;
    bool tracked = false;
    std::string peak_note;
    std::string carrier_note;
    for (const Seen& frame : seen) {
        if (frame.seconds < burst_start + 0.5 || frame.seconds > burst_start + 4.0) {
            continue;
        }
        if (frame.vrx == ids[0]) {
            CHECK(frame.marks == 1);
            CHECK_FALSE(frame.audio_only);
            REQUIRE(frame.has_raw);
            notched_sum += frame.at_carrier;
            raw_sum += frame.raw_at_carrier;
            ++notched_count;
            carrier_note = std::format("{} carriers listed at {:.2f} s", frame.carriers.size(),
                                       frame.seconds);
            for (const engine::PassbandCarrier& c : frame.carriers) {
                carrier_note += std::format(", {:.0f} Hz {:.1f} dB {:.0f} Hz wide", c.offset_hz,
                                            c.level_db, c.width_hz);
                if (std::abs(c.offset_hz - static_cast<double>(kCarrierOffset)) < 150.0) {
                    tracked = true;
                }
            }
        } else {
            CHECK(frame.marks == 0);
            CHECK_FALSE(frame.has_raw);
            plain_sum += frame.at_carrier;
            ++plain_count;
            peak_note = std::format("plain receiver's peak {:.1f} dB at {:.0f} Hz",
                                    frame.peak_db, frame.peak_hz);
        }
    }
    REQUIRE(notched_count > 16);
    REQUIRE(plain_count > 16);
    const double notched_db = notched_sum / static_cast<double>(notched_count);
    const double raw_db = raw_sum / static_cast<double>(notched_count);
    const double plain_db = plain_sum / static_cast<double>(plain_count);
    INFO(peak_note);
    INFO(carrier_note);
    INFO(std::format("burst from {:.2f} s, {} frames", burst_start, seen.size()));
    INFO(std::format("at the carrier: notched {:.1f} dB, its raw row {:.1f} dB, plain {:.1f} dB",
                     notched_db, raw_db, plain_db));

    // The raw row is the same air the plain receiver sees, and the notched
    // row is the notch's depth below it, give or take where the carrier sits
    // in its bin.
    CHECK(std::abs(raw_db - plain_db) < 1.5);
    CHECK(raw_db - notched_db > 30.0);

    // The tracker reads the un-notched power, so the carrier is still listed
    // once it has stood still for the persistence time.
    CHECK(tracked);
}
