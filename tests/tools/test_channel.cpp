// M0.4: the channel simulator, and specifically its SNR calibration.
//
// This is the test that protects every sensitivity number Revenant will ever
// publish. The handoff document is blunt about the stakes: overclaiming
// sensitivity is the one thing this community does not forgive, and a
// comparative figure computed against the wrong noise bandwidth is an
// overclaim whether or not anyone meant it.
//
// The specific trap is a factor of 10*log10(fs / 2500). At a 48 kHz sample
// rate that is 12.8 dB, which is most of the distance between a mediocre
// decoder and a world-class one. A decoder reported at -24 dB in the wrong
// convention is really at -11 dB, and the mistake looks like success.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "core/dsp/types.h"
#include "tools/siggen/channel.h"

using namespace revenant;
using Catch::Approx;

namespace {

// A deterministic unit-power complex tone. Not white, but the calibration
// measures whatever power it is handed, which is the property under test.
std::vector<dsp::Complex32> tone(std::size_t count, dsp::Hertz frequency, dsp::SampleRate rate) {
    const double increment = dsp::phase_increment(frequency, rate);
    std::vector<dsp::Complex32> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double phase = increment * static_cast<double>(i);
        out[i] = dsp::Complex32{static_cast<float>(std::cos(phase)),
                                static_cast<float>(std::sin(phase))};
    }
    return out;
}

}  // namespace

TEST_CASE("the reference bandwidth conversion is the WSJT-X one", "[channel][m0.4]") {
    constexpr dsp::SampleRate kRate = 48000;

    // 10*log10(48000/2500) = 12.8330 dB. Hardcoded rather than recomputed from
    // the same expression the implementation uses, because a test that repeats
    // the implementation's arithmetic cannot catch the implementation's
    // arithmetic being wrong.
    constexpr double kExpectedOffset = 12.8330;

    const auto full = siggen::reference_bandwidth_to_full_band_db(-24.0, kRate);
    REQUIRE(full.has_value());
    CHECK(*full == Approx(-24.0 - kExpectedOffset).margin(0.001));

    const auto back = siggen::full_band_to_reference_bandwidth_db(*full, kRate);
    REQUIRE(back.has_value());
    CHECK(*back == Approx(-24.0).margin(1e-9));

    // The direction matters and is easy to invert. Noise measured in a narrow
    // slice is less noise, so the reference-bandwidth SNR is always the higher
    // number whenever the sample rate exceeds 2500 Hz.
    CHECK(*full < -24.0);
}

TEST_CASE("Eb/N0 conversion does not depend on the sample rate", "[channel][m0.4]") {
    // The reference-bandwidth figure and Eb/N0 both describe the channel rather
    // than the buffer, so converting between them must give the same answer at
    // any rate. If this fails, a sample rate has leaked into a formula that
    // should not see one.
    constexpr double kBitsPerSecond = 31.25;  // PSK31

    const auto at_low = siggen::reference_bandwidth_to_eb_over_n0_db(-10.0, kBitsPerSecond);
    REQUIRE(at_low.has_value());

    const auto round_trip = siggen::eb_over_n0_to_reference_bandwidth_db(*at_low, kBitsPerSecond);
    REQUIRE(round_trip.has_value());
    CHECK(*round_trip == Approx(-10.0).margin(1e-9));

    // Cross-check the chain through the full-band basis at two different rates.
    for (const dsp::SampleRate rate : {12000, 48000, 192000}) {
        const auto full = siggen::reference_bandwidth_to_full_band_db(-10.0, rate);
        REQUIRE(full.has_value());
        const auto ebn0 = siggen::full_band_to_eb_over_n0_db(*full, rate, kBitsPerSecond);
        REQUIRE(ebn0.has_value());
        CHECK(*ebn0 == Approx(*at_low).margin(1e-6));
    }
}

TEST_CASE("add_awgn delivers the SNR it was asked for", "[channel][m0.4]") {
    // The M0.4 exit criterion. Ask for a level, measure what came out, and
    // require the two to agree across the useful range.
    constexpr dsp::SampleRate kRate = 48000;
    constexpr std::size_t kCount = 1 << 17;

    for (const double requested_db : {-24.0, -10.0, 0.0, 10.0, 20.0}) {
        const auto clean = tone(kCount, 1500, kRate);
        std::vector<dsp::Complex32> impaired = clean;

        const auto report = siggen::add_awgn(impaired, siggen::NoiseLevel::snr_in_2500_hz_db(requested_db),
                                             kRate, 0xC0FFEEULL);
        REQUIRE(report.has_value());

        const auto measured = siggen::measure_snr(clean, impaired, kRate);
        REQUIRE(measured.has_value());

        INFO("requested " << requested_db << " dB in 2500 Hz, measured "
                          << measured->snr_in_2500_hz_db << " dB");

        // The tolerance is the statistical spread of a finite noise sample, not
        // slack for a calibration error. At 131072 samples the standard error
        // of the power estimate is well under 0.05 dB, so 0.2 dB is generous
        // and still far tighter than the 12.8 dB mistake this guards against.
        CHECK(measured->snr_in_2500_hz_db == Approx(requested_db).margin(0.2));
    }
}

TEST_CASE("the noise generator is reproducible from its seed", "[channel][m0.4]") {
    // Every failure the BER harness reports is reproduced from a printed seed.
    // If the channel is not deterministic, none of those reports are actionable.
    constexpr dsp::SampleRate kRate = 48000;
    constexpr std::size_t kCount = 4096;

    const auto clean = tone(kCount, 1000, kRate);

    auto run = [&](std::uint64_t seed) {
        std::vector<dsp::Complex32> buffer = clean;
        const auto report =
            siggen::add_awgn(buffer, siggen::NoiseLevel::snr_in_2500_hz_db(0.0), kRate, seed);
        REQUIRE(report.has_value());
        return buffer;
    };

    const auto first = run(12345);
    const auto second = run(12345);
    const auto different = run(12346);

    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        // Bit-exact, not approximately equal. A generator that is only nearly
        // reproducible is not reproducible.
        CHECK(first[i].real() == second[i].real());
        CHECK(first[i].imag() == second[i].imag());
    }

    CHECK(first != different);
}

TEST_CASE("each stage draws from its own seed stream", "[channel][m0.4]") {
    // Stages derive independent streams from one master seed so that adding a
    // multipath tap does not shift the noise and invalidate every previously
    // recorded reproduction.
    const std::uint64_t master = 0x1234'5678'9ABC'DEF0ULL;

    const auto noise_stream = siggen::derive_seed(master, 1);
    const auto fading_stream = siggen::derive_seed(master, 2);
    const auto impulse_stream = siggen::derive_seed(master, 3);

    CHECK(noise_stream != fading_stream);
    CHECK(fading_stream != impulse_stream);
    CHECK(noise_stream != impulse_stream);

    // Deterministic across runs and across machines.
    CHECK(siggen::derive_seed(master, 1) == noise_stream);

    // A different master must move every stream, or two campaigns with
    // different seeds would share noise.
    CHECK(siggen::derive_seed(master + 1, 1) != noise_stream);
}

TEST_CASE("mean_power measures the buffer rather than assuming unit amplitude",
          "[channel][m0.4]") {
    constexpr dsp::SampleRate kRate = 48000;
    auto samples = tone(8192, 1000, kRate);

    CHECK(siggen::mean_power(samples) == Approx(1.0).margin(1e-5));

    // Halving the amplitude quarters the power. If the calibration assumed
    // unit amplitude anywhere, a quiet input would come out at the wrong SNR
    // and nothing would say so.
    for (auto& sample : samples) {
        sample *= 0.5F;
    }
    CHECK(siggen::mean_power(samples) == Approx(0.25).margin(1e-5));

    CHECK(siggen::mean_power({}) == 0.0);
}
