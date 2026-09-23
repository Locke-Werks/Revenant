// DC removal and I/Q correction, measured against the impairments put in.
//
// TWO HALVES. The first runs the whole correction on the host: the stream
// from tests/engine/impaired_front_end.h, the two kernels' CPU twins, and
// FrontEndCorrector read with the same frames-in-flight lag the graph reads it
// with. tests/reference/test_iq_correction.cpp holds the kernels to those
// twins bit for bit, so what is measured here is what the device does. It
// measures with a single-bin DFT over a whole number of the tone's cycles, so
// the DC term, the tone and its image are exactly orthogonal and each reading
// is limited by the noise rather than by leakage.
//
// The second runs the same stream through the engine on the device and reads
// the result off the spectrum, which is the check that the graph actually
// records the stage where it claims to, in the order it claims to.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <numbers>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "core/dsp/front_end_correction.h"
#include "core/dsp/types.h"
#include "core/engine/engine.h"
#include "core/engine/open_built_source.h"
#include "tests/engine/impaired_front_end.h"
#include "tests/engine/spectrum_probe.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 2'400'000;
constexpr dsp::Hertz kToneOffsetHz = 5'000;

// 480 samples is one cycle of 5 kHz at 2.4 MS/s, so the measurement window is
// a whole number of cycles: 2500 of them, half a second.
constexpr std::size_t kMeasureSamples = 480 * 2'500;

// Two seconds: twenty time constants of the DC estimate and two of the I/Q
// one, with the last half second measured.
constexpr dsp::SampleIndex kStreamSamples = 2 * kRate;

constexpr std::uint32_t kBlock = 32'768;
constexpr std::uint32_t kFramesInFlight = 3;

// -33.0 dBFS: 0.02^2 + 0.01^2 = 5e-4.
constexpr std::complex<double> kDc{0.02, 0.01};

// A gain ratio of 1.05, 0.42 dB, and 3 degrees: an image rejection of 28.9 dB.
constexpr double kGain = 1.05;
constexpr double kPhaseRad = 3.0 * std::numbers::pi / 180.0;

test::ImpairedFrontEndConfig impaired(std::complex<double> dc, double gain, double phase_rad) {
    test::ImpairedFrontEndConfig config;
    config.rate = kRate;
    config.device_center = 100'000'000;
    config.clock_error_ppb = 0;
    config.carrier_hz = config.device_center + kToneOffsetHz;
    config.carrier_amplitude = 0.1;
    config.dc = dc;
    config.gain = gain;
    config.phase_rad = phase_rad;
    config.noise_rms = 1.0e-4;
    config.length = kStreamSamples;
    config.block_samples = kBlock;
    return config;
}

// The whole stream, collected off the source's own thread.
std::vector<dsp::Complex32> render(const test::ImpairedFrontEndConfig& config) {
    std::vector<dsp::Complex32> out;
    out.reserve(static_cast<std::size_t>(config.length));
    std::mutex lock;
    test::ImpairedFrontEnd front(config);
    auto started = front.start({}, [&](const source::SourceBlock& block) -> Status {
        const auto* first = reinterpret_cast<const dsp::Complex32*>(block.bytes.data());
        const std::scoped_lock held(lock);
        out.insert(out.end(), first, first + block.sample_count);
        return {};
    });
    REQUIRE(started.has_value());
    while (front.running()) {
        std::this_thread::yield();
    }
    REQUIRE(front.stop().has_value());
    return out;
}

// The graph's schedule on the host: block k's moments are read when block
// k + kFramesInFlight is recorded, and block k is corrected with whatever the
// estimate held before its own moments were taken.
struct Corrected {
    std::vector<dsp::Complex32> stream;
    dsp::FrontEndEstimate estimate;
};

Corrected correct_on_host(std::span<const dsp::Complex32> stream, bool dc, bool iq) {
    constexpr std::uint32_t kRing = 1U << 16;
    std::vector<dsp::Complex32> ring(kRing);
    std::vector<dsp::Complex32> scratch(kRing);
    std::vector<std::vector<float>> pending(kFramesInFlight);
    std::vector<std::uint32_t> pending_count(kFramesInFlight, 0);

    dsp::FrontEndCorrector corrector;
    Corrected out;
    out.stream.reserve(stream.size());

    std::uint32_t offset = 0;
    for (std::size_t first = 0, k = 0; first < stream.size(); first += kBlock, ++k) {
        const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(kBlock, stream.size() - first));
        const std::size_t slot = k % kFramesInFlight;
        if (pending_count[slot] > 0) {
            corrector.update(dsp::total_moments(pending[slot], pending_count[slot]), kRate);
            pending_count[slot] = 0;
        }

        for (std::uint32_t n = 0; n < count; ++n) {
            ring[(offset + n) & (kRing - 1)] = stream[first + n];
        }

        dsp::IqMomentsParams moments{.ring_mask = kRing - 1, .src_offset = offset, .count = count};
        pending[slot].assign(static_cast<std::size_t>(dsp::iq_moments_chunks(count)) *
                                 dsp::kIqMomentsPerChunk,
                             0.0F);
        REQUIRE(dsp::reference_iq_moments(ring, moments, pending[slot]).has_value());
        pending_count[slot] = count;

        if (corrector.estimate().measured) {
            dsp::IqCorrectParams params = corrector.correction(dc, iq);
            params.ring_mask = kRing - 1;
            params.offset = offset;
            params.count = count;
            scratch = ring;
            REQUIRE(dsp::reference_iq_correct(scratch, params, ring).has_value());
        }

        for (std::uint32_t n = 0; n < count; ++n) {
            out.stream.push_back(ring[(offset + n) & (kRing - 1)]);
        }
        offset = (offset + count) & (kRing - 1);
    }
    out.estimate = corrector.estimate();
    return out;
}

// The complex amplitude of the component at f, over the stream's last
// kMeasureSamples.
std::complex<double> component(std::span<const dsp::Complex32> stream, double f_hz) {
    const std::size_t first = stream.size() - kMeasureSamples;
    std::complex<double> sum{0.0, 0.0};
    const double step = -2.0 * std::numbers::pi * f_hz / static_cast<double>(kRate);
    for (std::size_t n = 0; n < kMeasureSamples; ++n) {
        const double angle = step * static_cast<double>(n % 480);
        const dsp::Complex32 x = stream[first + n];
        sum += std::complex<double>(x.real(), x.imag()) *
               std::complex<double>(std::cos(angle), std::sin(angle));
    }
    return sum / static_cast<double>(kMeasureSamples);
}

double db(std::complex<double> amplitude) {
    return 10.0 * std::log10(std::max(std::norm(amplitude), 1.0e-30));
}

}  // namespace

TEST_CASE("DC removal takes a -33 dBFS spike down by 40 dB and leaves a tone 5 kHz away alone",
          "[calibration][front-end]") {
    const std::vector<dsp::Complex32> raw = render(impaired(kDc, 1.0, 0.0));
    const Corrected fixed = correct_on_host(raw, true, false);

    const double dc_before = db(component(raw, 0.0));
    const double dc_after = db(component(fixed.stream, 0.0));
    const double tone_before = db(component(raw, kToneOffsetHz));
    const double tone_after = db(component(fixed.stream, kToneOffsetHz));
    INFO("DC " << dc_before << " dBFS before, " << dc_after << " dBFS after, a drop of "
               << dc_before - dc_after << " dB; tone " << tone_before << " dBFS before, "
               << tone_after << " dBFS after; estimate " << fixed.estimate.dc_dbfs() << " dBFS");

    CHECK(std::abs(dc_before - -33.01) < 0.05);
    CHECK(dc_before - dc_after >= 40.0);
    CHECK(std::abs(tone_after - tone_before) < 0.001);
    CHECK(std::abs(fixed.estimate.dc_dbfs() - -33.01) < 0.05);

    // The identity correction, which is what a switched-off half applies,
    // leaves the stream's value where it was. The graph goes further and does
    // not dispatch the stage at all while both halves are off.
    const Corrected off = correct_on_host(raw, false, false);
    CHECK(db(component(off.stream, 0.0)) == dc_before);
}

TEST_CASE("I/Q correction lifts image rejection from 28.9 dB to past 60 dB",
          "[calibration][front-end]") {
    const std::vector<dsp::Complex32> raw = render(impaired({0.0, 0.0}, kGain, kPhaseRad));
    const Corrected fixed = correct_on_host(raw, false, true);

    const double irr_before =
        db(component(raw, kToneOffsetHz)) - db(component(raw, -kToneOffsetHz));
    const double irr_after =
        db(component(fixed.stream, kToneOffsetHz)) - db(component(fixed.stream, -kToneOffsetHz));
    const dsp::FrontEndEstimate& estimate = fixed.estimate;
    INFO("image rejection " << irr_before << " dB before, " << irr_after
                            << " dB after; estimate gain " << estimate.gain_error_db()
                            << " dB, phase " << estimate.phase_error_deg() << " deg, image "
                            << estimate.image_rejection_db() << " dB; tone "
                            << db(component(raw, kToneOffsetHz)) << " dBFS before, "
                            << db(component(fixed.stream, kToneOffsetHz)) << " dBFS after");

    CHECK(std::abs(irr_before - 28.93) < 0.1);
    CHECK(irr_after >= 60.0);
    CHECK(estimate.iq_plausible);
    CHECK(std::abs(estimate.gain_error_db() - 20.0 * std::log10(kGain)) < 0.01);
    CHECK(std::abs(estimate.phase_error_deg() - 3.0) < 0.05);
    CHECK(std::abs(estimate.image_rejection_db() - 28.93) < 0.1);

    // The wanted tone comes back at the amplitude that went in, 0.1, which is
    // -20 dBFS: the correction restores Q rather than scaling both arms.
    CHECK(std::abs(db(component(fixed.stream, kToneOffsetHz)) - -20.0) < 0.01);
}

TEST_CASE("both corrections together on a stream with both defects",
          "[calibration][front-end]") {
    const std::vector<dsp::Complex32> raw = render(impaired(kDc, kGain, kPhaseRad));
    const Corrected fixed = correct_on_host(raw, true, true);

    const double dc_drop = db(component(raw, 0.0)) - db(component(fixed.stream, 0.0));
    const double irr_after =
        db(component(fixed.stream, kToneOffsetHz)) - db(component(fixed.stream, -kToneOffsetHz));
    INFO("DC drop " << dc_drop << " dB, image rejection after " << irr_after << " dB");
    CHECK(dc_drop >= 40.0);
    CHECK(irr_after >= 60.0);
}

TEST_CASE("an implausible imbalance is measured and left unapplied", "[calibration][front-end]") {
    // A real-valued stream is its own mirror image, which is the case the
    // estimator's model does not hold for: Q is nothing like I in power.
    dsp::FrontEndCorrector corrector;
    dsp::IqMoments moments;
    moments.samples = 1000;
    moments.sum_ii = 500.0;
    moments.sum_qq = 1.0;
    corrector.update(moments, kRate);
    CHECK(corrector.estimate().measured);
    CHECK(!corrector.estimate().iq_plausible);
    const dsp::IqCorrectParams params = corrector.correction(true, true);
    CHECK(params.cross == 0.0F);
    CHECK(params.scale == 1.0F);
}

TEST_CASE("the engine's stage removes the spike and the image on the device",
          "[gpu][engine][calibration][front-end]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    const auto run = [](bool on) {
        engine::EngineConfig config;
        config.channels = 64;
        config.taps_per_branch = 17;
        config.ring_seconds = 0.5;
        config.block_samples = kBlock;
        config.spectrum_transform = 2048;
        config.gpu_index = -1;
        auto created = engine::Engine::create(config);
        REQUIRE(created.has_value());
        engine::Engine& eng = **created;
        REQUIRE(engine::open_built_source(eng, std::make_unique<test::ImpairedFrontEnd>(
                                                   impaired(kDc, kGain, kPhaseRad)))
                    .has_value());
        REQUIRE(eng.set_calibration({.dc_removal = on, .iq_correction = on}).has_value());

        test::AveragedSpectrum spectrum = test::average_spectrum(eng, kRate);
        INFO(test::message_of(spectrum.run_status));
        REQUIRE(spectrum.run_status.has_value());
        REQUIRE(spectrum.frames > 0);

        auto state = eng.calibration();
        REQUIRE(state.has_value());
        return std::pair{std::move(spectrum), *state};
    };

    const auto [off, off_state] = run(false);
    const auto [on, on_state] = run(true);

    const auto reading = [](const test::AveragedSpectrum& s) {
        struct {
            double dc;
            double tone;
            double image;
        } r{s.peak_db_near(s.bin_of(0.0), 1), s.peak_db_near(s.bin_of(kToneOffsetHz), 2),
            s.peak_db_near(s.bin_of(-kToneOffsetHz), 2)};
        return r;
    };
    const auto before = reading(off);
    const auto after = reading(on);
    INFO("off: DC " << before.dc << ", tone " << before.tone << ", image " << before.image
                    << " dB; on: DC " << after.dc << ", tone " << after.tone << ", image "
                    << after.image << " dB; " << on.frames << " frames; estimate "
                    << on_state.front_end.estimate.dc_dbfs() << " dBFS, gain "
                    << on_state.front_end.estimate.gain_error_db() << " dB, phase "
                    << on_state.front_end.estimate.phase_error_deg() << " deg over "
                    << on_state.front_end.blocks_measured << " blocks");

    CHECK(!off_state.front_end.estimate.measured);
    CHECK(on_state.front_end.estimate.measured);
    CHECK(on_state.front_end.blocks_measured > 0);

    // The spectrum's own floor and leakage bound what it can show, so these
    // are looser than the host measurement: the spike is at least 30 dB down
    // and the image at least 25 dB further down than it was.
    CHECK(before.dc - after.dc >= 30.0);
    CHECK((after.tone - after.image) - (before.tone - before.image) >= 25.0);
}
