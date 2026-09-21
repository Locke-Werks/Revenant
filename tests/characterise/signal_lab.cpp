#include "tests/characterise/signal_lab.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <random>

#include "core/characterise/transform.h"
#include "core/dsp/synth/modulators.h"

namespace revenant::characterise_test {
namespace {

// A uniform double in (0, 1]. Zero is excluded because Box-Muller takes a
// logarithm of it.
[[nodiscard]] double uniform(std::mt19937_64& engine) {
    // 53 bits, which is the mantissa, so every representable double in the
    // range is reachable and none is reachable twice.
    const std::uint64_t word = engine() >> 11;
    return (static_cast<double>(word) + 1.0) * (1.0 / 9007199254740992.0);
}

}  // namespace

std::vector<Complex32> gaussian_noise(std::size_t count, double variance, std::uint64_t seed)
{
    std::vector<Complex32> out(count);
    std::mt19937_64 engine(seed);
    // Half the total power in each quadrature, so mean(|x|^2) is variance.
    const double sigma = std::sqrt(std::max(variance, 0.0) / 2.0);
    for (std::size_t n = 0; n < count; ++n) {
        const double u1 = uniform(engine);
        const double u2 = uniform(engine);
        const double magnitude = sigma * std::sqrt(-2.0 * std::log(u1));
        const double angle = 2.0 * std::numbers::pi * u2;
        out[n] = Complex32(static_cast<float>(magnitude * std::cos(angle)),
                           static_cast<float>(magnitude * std::sin(angle)));
    }
    return out;
}

std::vector<Complex32> pure_tone(std::size_t count,
                                 SampleRate rate,
                                 Hertz frequency,
                                 double amplitude)
{
    std::vector<Complex32> out(count);
    for (std::size_t n = 0; n < count; ++n) {
        const double phase = siggen::exact_phase(frequency, rate, n);
        out[n] = Complex32(static_cast<float>(amplitude * std::cos(phase)),
                           static_cast<float>(amplitude * std::sin(phase)));
    }
    return out;
}

std::vector<Complex32> ofdm_signal(const OfdmSpec& spec)
{
    const std::size_t useful = spec.useful_samples;
    if (!characterise::is_power_of_two(useful) || useful < 16) {
        return {};
    }
    if (spec.used_carriers == 0 || spec.used_carriers + 1 > useful) {
        return {};
    }
    if (spec.symbol_count == 0) {
        return {};
    }

    const std::size_t block = spec.prefix_samples + useful;
    std::vector<Complex32> out;
    out.reserve(block * spec.symbol_count);

    std::mt19937_64 engine(spec.seed);
    std::vector<characterise::Complex64> frequency_domain(useful);
    std::vector<characterise::Complex64> time_domain(useful);

    // Subcarriers -half .. +half excluding DC, so the burst has a null at
    // the centre and empty guard bands at both edges.
    const auto half = static_cast<long long>(spec.used_carriers / 2);
    const double level = 1.0 / std::sqrt(2.0 * static_cast<double>(spec.used_carriers));

    for (std::size_t symbol = 0; symbol < spec.symbol_count; ++symbol) {
        std::fill(frequency_domain.begin(), frequency_domain.end(),
                  characterise::Complex64(0.0, 0.0));
        for (long long k = -half; k <= half; ++k) {
            if (k == 0) {
                continue;
            }
            const std::uint64_t word = engine();
            const double in_phase = (word & 1U) != 0 ? level : -level;
            const double quadrature = (word & 2U) != 0 ? level : -level;
            const auto index = static_cast<std::size_t>(
                k < 0 ? k + static_cast<long long>(useful) : k);
            frequency_domain[index] = characterise::Complex64(in_phase, quadrature);
        }

        // Inverse transform out of the forward one: conj(fft(conj(X)))/N.
        // One transform to maintain rather than two, and the identity is
        // exact rather than a tolerance.
        for (std::size_t k = 0; k < useful; ++k) {
            time_domain[k] = std::conj(frequency_domain[k]);
        }
        characterise::fft_in_place(time_domain);
        for (std::size_t n = 0; n < useful; ++n) {
            time_domain[n] = std::conj(time_domain[n]);
        }

        for (std::size_t n = 0; n < spec.prefix_samples; ++n) {
            const characterise::Complex64 value = time_domain[useful - spec.prefix_samples + n];
            out.emplace_back(static_cast<float>(value.real()), static_cast<float>(value.imag()));
        }
        for (std::size_t n = 0; n < useful; ++n) {
            out.emplace_back(static_cast<float>(time_domain[n].real()),
                             static_cast<float>(time_domain[n].imag()));
        }
    }
    return out;
}

std::vector<Complex32> mfsk_signal(const MfskSpec& spec, std::size_t sample_count)
{
    if (spec.tone_count < 2 || spec.rate <= 0 || !(spec.symbol_rate > 0.0)) {
        return {};
    }
    const double exact = static_cast<double>(spec.rate) / spec.symbol_rate;
    const auto samples_per_symbol = static_cast<std::size_t>(std::llround(exact));
    if (samples_per_symbol == 0 || std::abs(exact - static_cast<double>(samples_per_symbol)) >
                                       1e-9) {
        return {};
    }

    const double half = 0.5 * static_cast<double>(spec.tone_count - 1);
    const double edge = std::abs(static_cast<double>(spec.carrier_offset)) +
                        half * std::abs(static_cast<double>(spec.spacing_hz));
    if (edge >= 0.5 * static_cast<double>(spec.rate)) {
        return {};
    }

    std::vector<Complex32> out(sample_count);
    std::mt19937_64 engine(spec.seed);

    // One running phase, wrapped every sample. Continuous by construction:
    // the increment changes at a symbol boundary and the phase does not.
    double phase = 0.0;
    double increment = 0.0;
    for (std::size_t n = 0; n < sample_count; ++n) {
        if (n % samples_per_symbol == 0) {
            const auto level = static_cast<double>(engine() % spec.tone_count);
            const double tone = static_cast<double>(spec.carrier_offset) +
                                (level - half) * static_cast<double>(spec.spacing_hz);
            increment = 2.0 * std::numbers::pi * tone / static_cast<double>(spec.rate);
        }
        out[n] = Complex32(static_cast<float>(spec.amplitude * std::cos(phase)),
                           static_cast<float>(spec.amplitude * std::sin(phase)));
        phase += increment;
        if (phase > std::numbers::pi) {
            phase -= 2.0 * std::numbers::pi;
        } else if (phase < -std::numbers::pi) {
            phase += 2.0 * std::numbers::pi;
        }
    }
    return out;
}

std::vector<Complex32> add_buffers(const std::vector<Complex32>& a,
                                   const std::vector<Complex32>& b)
{
    const std::size_t count = std::min(a.size(), b.size());
    std::vector<Complex32> out(count);
    for (std::size_t n = 0; n < count; ++n) {
        out[n] = a[n] + b[n];
    }
    return out;
}

}  // namespace revenant::characterise_test
