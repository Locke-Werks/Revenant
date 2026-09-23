#include "core/dsp/synth/psk31_mod.h"

#include <cmath>
#include <complex>
#include <format>
#include <numbers>

#include "core/decode/varicode.h"
#include "core/dsp/synth/channel.h"

namespace revenant::siggen {
namespace {

constexpr double kPi = std::numbers::pi;

}  // namespace

Expected<std::vector<std::uint8_t>> psk31_message_bits(const Psk31ModConfig& config,
                                                       std::string_view text) {
    auto body = decode::varicode_encode(text);
    if (!body) {
        return std::unexpected(with_context(body.error(), "encoding PSK31 text"));
    }
    std::vector<std::uint8_t> bits(config.preamble_symbols, 0);
    bits.insert(bits.end(), body->begin(), body->end());
    bits.insert(bits.end(), config.postamble_symbols, static_cast<std::uint8_t>(1));
    return bits;
}

std::vector<std::uint8_t> psk31_symbol_shifts(decode::Psk31Mode mode,
                                              std::span<const std::uint8_t> bits) {
    std::vector<std::uint8_t> shifts;
    shifts.reserve(bits.size());
    std::uint32_t run = 0;
    for (const std::uint8_t bit : bits) {
        if (mode == decode::Psk31Mode::Qpsk31) {
            // QEX page 9: each shift comes from the run of the last five
            // bits, left bit first, so the newest bit enters at the right.
            run = ((run << 1U) | (bit & 1U)) & 0x1FU;
            shifts.push_back(decode::kQpsk31PhaseTable[run]);
        } else {
            // QEX page 6: zero is a reversal, one a steady carrier.
            shifts.push_back(bit != 0U ? decode::kPskShiftNone : decode::kPskShiftReverse);
        }
    }
    return shifts;
}

Expected<std::vector<Complex32>> psk31_render_analytic(const Psk31ModConfig& config,
                                                       std::span<const std::uint8_t> bits) {
    if (config.rate <= 0 || config.tone_hz <= 0 ||
        2 * config.tone_hz >= config.rate) {
        return fail(std::format(
            "psk31_render_analytic needs a tone inside the audio band; got {} Hz at {} Hz",
            config.tone_hz, config.rate));
    }
    const double symbol_rate = decode::psk31_symbol_rate(config.mode);
    const std::vector<std::uint8_t> shifts = psk31_symbol_shifts(config.mode, bits);

    // Symbol phasors. Symbol k is centred at (k + 1) * T, with silence
    // before the first and after the last, so the transmission ramps up and
    // down through the same raised cosine as every reversal inside it.
    std::vector<std::complex<double>> phasor(shifts.size());
    unsigned quarter_turns = 0;
    for (std::size_t k = 0; k < shifts.size(); ++k) {
        quarter_turns = (quarter_turns + shifts[k]) % 4U;
        phasor[k] = std::polar(config.amplitude, 0.5 * kPi * quarter_turns);
    }
    const auto at = [&](std::ptrdiff_t k) -> std::complex<double> {
        if (k < 0 || static_cast<std::size_t>(k) >= phasor.size()) {
            return {0.0, 0.0};
        }
        return phasor[static_cast<std::size_t>(k)];
    };

    const double rate = static_cast<double>(config.rate);
    const double duration = static_cast<double>(shifts.size() + 1) / symbol_rate;
    const auto samples = static_cast<std::size_t>(std::ceil(duration * rate));
    std::vector<Complex32> out(samples);
    const auto rate_u = static_cast<std::uint64_t>(config.rate);
    const auto tone_u = static_cast<std::uint64_t>(config.tone_hz);

    for (std::size_t n = 0; n < samples; ++n) {
        // The core/decode/psk31.h pulse: between symbol centres the carrier
        // crossfades from one phasor to the next along (1 +/- cos(pi*tau))/2,
        // which for a reversal is a half cosine through zero and for no
        // change is a steady carrier.
        const double position = static_cast<double>(n) / rate * symbol_rate;
        const double whole = std::floor(position);
        const double tau = position - whole;
        const auto left = static_cast<std::ptrdiff_t>(whole) - 1;
        const double fade = 0.5 * (1.0 + std::cos(kPi * tau));
        const std::complex<double> envelope = at(left) * fade + at(left + 1) * (1.0 - fade);

        const std::uint64_t cycle = (static_cast<std::uint64_t>(n) % rate_u) * tone_u % rate_u;
        const std::complex<double> carrier =
            std::polar(1.0, 2.0 * kPi * static_cast<double>(cycle) / rate);
        const std::complex<double> value = envelope * carrier;
        out[n] = Complex32{static_cast<float>(value.real()), static_cast<float>(value.imag())};
    }
    return out;
}

std::vector<float> analytic_to_audio(std::span<const Complex32> analytic) {
    std::vector<float> audio(analytic.size());
    for (std::size_t i = 0; i < analytic.size(); ++i) {
        audio[i] = analytic[i].real();
    }
    return audio;
}

Expected<std::vector<float>> analytic_to_noisy_audio(std::span<const Complex32> analytic,
                                                     double snr_in_reference_bandwidth_db,
                                                     Hertz reference_bandwidth_hz,
                                                     SampleRate rate, std::uint64_t seed) {
    // The header's 3 dB: asking the complex simulator for 10*log10(2) more
    // than the audio should have puts exactly the stated figure on the real
    // part.
    const double compensation_db = 10.0 * std::log10(2.0);
    std::vector<Complex32> noisy(analytic.begin(), analytic.end());
    const auto level = NoiseLevel::snr_in_reference_bandwidth_db(
        snr_in_reference_bandwidth_db + compensation_db, reference_bandwidth_hz);
    auto report = add_awgn(noisy, level, rate, seed);
    if (!report) {
        return std::unexpected(with_context(report.error(), "adding noise to audio"));
    }
    return analytic_to_audio(noisy);
}

}  // namespace revenant::siggen
