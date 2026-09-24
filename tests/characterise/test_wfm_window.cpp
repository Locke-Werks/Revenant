// A broadcast FM station seen through a window narrower than it, which is all
// a probe on the engine's grid can collect.
//
// THE QUESTION. docs/detection.md, "What is not done": a broadcast station
// measures about 140 kHz and a probe's bucket is capped at the grid's channel
// rate, 75 kS/s on the owner's 2.4 MS/s grid over 64 channels, so no probe
// fits and WFM is never labelled. Could a probe at the centre of the station,
// at the widest bucket the grid allows, still name it? Two readings are asked
// of the window: what the characteriser makes of it, and whether the stereo
// pilot, a 19 kHz tone on the composite, stands out of the discriminator's
// output, which is where a receiver finds it.
//
// Hidden, and printed rather than asserted: it is the measurement
// docs/detection.md quotes, not a gate.
//
//     revenant_characterise_tests.exe "[.wfm-window]"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <print>
#include <vector>

#include "core/characterise/characterise.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/modulators.h"
#include "core/dsp/synth/wfm_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRenderRate = 768'000;
constexpr double kSeconds = 2.0;
constexpr std::uint64_t kSeed = 20260924;

// Decimated to `rate` through a windowed sinc passing a quarter of it either
// side, which is the half of the bucket a probe passes.
[[nodiscard]] std::vector<dsp::Complex32> window_of(const std::vector<dsp::Complex32>& wide,
                                                    dsp::SampleRate rate) {
    const auto factor = static_cast<std::size_t>(kRenderRate / rate);
    const int half = static_cast<int>(16 * factor);
    const double cutoff = 0.25 * static_cast<double>(rate) / static_cast<double>(kRenderRate);
    std::vector<double> taps(static_cast<std::size_t>(2 * half + 1));
    double sum = 0.0;
    for (int k = -half; k <= half; ++k) {
        const double x = 2.0 * cutoff * static_cast<double>(k);
        const double sinc = k == 0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
        const double window = 0.54 + 0.46 * std::cos(std::numbers::pi * k / half);
        taps[static_cast<std::size_t>(k + half)] = sinc * window;
        sum += sinc * window;
    }
    const std::size_t count = wide.size() / factor;
    std::vector<dsp::Complex32> out(count);
    for (std::size_t m = 0; m < count; ++m) {
        std::complex<double> acc(0.0, 0.0);
        const auto centre = static_cast<std::ptrdiff_t>(m * factor);
        for (int k = -half; k <= half; ++k) {
            const std::ptrdiff_t j = centre + k;
            if (j >= 0 && j < static_cast<std::ptrdiff_t>(wide.size())) {
                acc += taps[static_cast<std::size_t>(k + half)] / sum *
                       std::complex<double>(wide[static_cast<std::size_t>(j)]);
            }
        }
        out[m] = dsp::Complex32(static_cast<float>(acc.real()), static_cast<float>(acc.imag()));
    }
    return out;
}

// The discriminator's output power at 19 kHz over the mean of its power at
// 20 frequencies 10 to 200 Hz either side, in dB, from Goertzel sums over the
// whole extract. Samples under a tenth of the mean power, and the one after
// each, count as zero deviation, the gate core/characterise uses.
[[nodiscard]] double pilot_over_neighbours_db(const std::vector<dsp::Complex32>& samples,
                                              dsp::SampleRate rate) {
    double mean = 0.0;
    for (const dsp::Complex32 s : samples) {
        mean += std::norm(std::complex<double>(s));
    }
    mean /= static_cast<double>(samples.size());
    std::vector<double> frequency(samples.size(), 0.0);
    for (std::size_t n = 1; n < samples.size(); ++n) {
        const std::complex<double> a(samples[n]);
        const std::complex<double> b(samples[n - 1]);
        if (std::norm(a) < 0.1 * mean || std::norm(b) < 0.1 * mean) {
            continue;
        }
        frequency[n] = std::arg(a * std::conj(b));
    }
    const auto power_at = [&](double hz) {
        std::complex<double> acc(0.0, 0.0);
        const double step = -2.0 * std::numbers::pi * hz / static_cast<double>(rate);
        for (std::size_t n = 0; n < frequency.size(); ++n) {
            acc += frequency[n] * std::polar(1.0, std::fmod(step * static_cast<double>(n),
                                                            2.0 * std::numbers::pi));
        }
        return std::norm(acc);
    };
    double neighbours = 0.0;
    int count = 0;
    for (int k = 1; k <= 10; ++k) {
        for (const int sign : {-1, 1}) {
            neighbours += power_at(19'000.0 + sign * 20.0 * k - sign * 10.0);
            ++count;
        }
    }
    neighbours /= count;
    return 10.0 * std::log10(power_at(19'000.0) / neighbours);
}

}  // namespace

TEST_CASE("wfm window survey: a station through a probe narrower than it", "[.wfm-window]") {
    const auto total = static_cast<std::size_t>((kSeconds + 0.1) * kRenderRate);
    struct Station {
        const char* name;
        bool stereo;
        bool pilot;
        dsp::Hertz audio_deviation_hz;
    };
    const Station stations[] = {
        {"stereo, 52.5 kHz audio", true, true, 52'500},
        {"stereo, 30 kHz audio", true, true, 30'000},
        {"stereo, 10 kHz audio", true, true, 10'000},
        {"mono, no pilot, 52.5 kHz", false, false, 52'500},
    };
    for (const Station& station : stations) {
        siggen::WfmSpec spec;
        spec.rate = kRenderRate;
        spec.programme.stereo = station.stereo;
        spec.programme.left_tone_hz = 1000;
        spec.programme.right_tone_hz = 2500;
        spec.programme.audio_deviation_hz = station.audio_deviation_hz;
        spec.rds.pilot_enabled = station.pilot;
        // One group's worth of alternating bits: the RDS layer needs
        // something to send, and nothing here reads it.
        spec.rds.bits.assign(104, 0);
        for (std::size_t i = 0; i < spec.rds.bits.size(); i += 2) {
            spec.rds.bits[i] = 1;
        }
        auto modulator = siggen::WfmModulator::create(spec);
        INFO((modulator.has_value() ? std::string() : modulator.error().message));
        REQUIRE(modulator.has_value());
        std::vector<dsp::Complex32> wide(total);
        modulator->render(0, dsp::ComplexSpan(wide));
        for (const double level : {40.0, 25.0}) {
            std::vector<dsp::Complex32> noisy = wide;
            auto added = siggen::add_awgn(dsp::ComplexSpan(noisy),
                                          siggen::NoiseLevel::snr_in_2500_hz_db(level),
                                          kRenderRate, kSeed);
            REQUIRE(added.has_value());
            for (const dsp::SampleRate rate : {48'000, 96'000, 192'000}) {
                auto extract = window_of(noisy, rate);
                extract.resize(static_cast<std::size_t>(kSeconds * rate));
                characterise::CharacteriseConfig config;
                config.rate = rate;
                config.detection_bandwidth_hz = 140'000.0;
                auto result = characterise::characterise(dsp::ConstComplexSpan(extract), config);
                REQUIRE(result.has_value());
                std::println("  {:<26} {:>4.0f} dB {:>6} S/s  {:<20} {:.2f}  env {:.3f}  "
                             "pilot {:+6.1f} dB",
                             station.name, level, rate,
                             characterise::modulation_family_name(result->family),
                             result->family_confidence,
                             result->envelope.normalised_power_variance,
                             pilot_over_neighbours_db(extract, rate));
            }
        }
    }

    // A control with no pilot to find: narrowband FM on a tone, at the
    // bucket's own rate.
    for (const dsp::SampleRate rate : {48'000, 96'000}) {
        siggen::ModulatorConfig common;
        common.rate = rate;
        common.seed = kSeed;
        siggen::NfmParams nfm;
        nfm.deviation = 5'000;
        const auto count = static_cast<std::size_t>(kSeconds * rate);
        std::vector<float> audio(count);
        for (std::size_t n = 0; n < count; ++n) {
            audio[n] = static_cast<float>(
                0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(n) / rate));
        }
        auto made = siggen::generate_nfm(common, nfm, count, audio);
        REQUIRE(made.has_value());
        auto added = siggen::add_awgn(dsp::ComplexSpan(made->samples),
                                      siggen::NoiseLevel::snr_in_2500_hz_db(25.0), rate, kSeed);
        REQUIRE(added.has_value());
        std::println("  {:<26} {:>4.0f} dB {:>6} S/s  {:<20} pilot {:+6.1f} dB",
                     "nfm 5 kHz on 1 kHz", 25.0, rate, "(control)",
                     pilot_over_neighbours_db(made->samples, rate));
    }
}
