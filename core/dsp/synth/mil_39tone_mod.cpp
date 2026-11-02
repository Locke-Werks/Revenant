#include "core/dsp/synth/mil_39tone_mod.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace revenant::siggen {

namespace {
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kDeg = std::numbers::pi / 180.0;
}  // namespace

Expected<std::vector<float>> tone39_render(const Tone39ModConfig& config, std::span<const std::uint8_t> data) {
    using namespace decode;
    if (!tone39_valid(config.mode)) {
        return fail("39-tone modulator: rate and degree not in table B-III");
    }
    if (config.rate < 6400) {
        return fail("39-tone modulator: sample rate too low");
    }
    const double fs = static_cast<double>(config.rate);
    const double e = fs * kTone39ElementSeconds;
    // Data portion power: 39 tones at 1 and Doppler at 2, 21.5 units.
    const double unit = config.amplitude / std::sqrt(43.0);
    const auto& parts = config.extended_preamble ? kTone39ExtendedPreamble : kTone39Preamble;
    const auto stream = tone39_stream(config.mode, data);
    const auto elements = tone39_elements(config.mode, stream);
    const int total = parts[0] + parts[1] + parts[2] + static_cast<int>(elements.size());

    const double lead = config.lead_in_samples;
    const auto n_samples = static_cast<std::size_t>(std::ceil(lead + total * e + config.tail_samples));
    std::vector<float> out(n_samples, 0.0F);
    const double onset_p3 = lead + (parts[0] + parts[1]) * e;

    // Data tone phases at the current onset, radians, index 1..39.
    std::array<double, 40> phase{};
    for (const auto& t : tone39_preamble_part3()) {
        phase[static_cast<std::size_t>(t.tone)] = t.phase_deg * kDeg;
    }

    for (int n = 0; n < total; ++n) {
        const double onset = lead + n * e;
        const auto first = static_cast<std::size_t>(std::ceil(onset));
        const auto last = std::min(n_samples, static_cast<std::size_t>(std::ceil(onset + e)));
        struct Tone {
            double hz;
            double amp;
            double phase;     // at reference
            double ref;       // reference sample
        };
        std::vector<Tone> tones;
        if (n < parts[0]) {
            for (const auto& t : tone39_preamble_part1()) {
                tones.push_back({tone39_frequency(t.tone), t.amplitude, t.phase_deg * kDeg, lead});
            }
        } else if (n < parts[0] + parts[1]) {
            const int m = n - parts[0];
            for (const auto& t : tone39_preamble_part2()) {
                tones.push_back({tone39_frequency(t.tone), t.amplitude, t.phase_deg * kDeg + m * std::numbers::pi, onset});
            }
        } else {
            const int m = n - parts[0] - parts[1];
            if (m >= parts[2]) {
                const auto& bits = elements[static_cast<std::size_t>(m - parts[2])];
                for (int k = 1; k <= kTone39DataTones; ++k) {
                    const int step = tone39_phase_step(bits[static_cast<std::size_t>(2 * (k - 1))],
                                                       bits[static_cast<std::size_t>(2 * (k - 1) + 1)]);
                    phase[static_cast<std::size_t>(k)] += step * std::numbers::pi / 4.0;
                }
            }
            for (int k = 1; k <= kTone39DataTones; ++k) {
                tones.push_back({tone39_frequency(k), 1.0, phase[static_cast<std::size_t>(k)], onset});
            }
            // Doppler: continuous, phase zero at the onset of part 3.
            tones.push_back({kTone39DopplerHz, 2.0, 0.0, onset_p3});
        }
        for (std::size_t i = first; i < last; ++i) {
            double v = 0.0;
            const double abs_ph = kTwoPi * config.offset_hz * static_cast<double>(i) / fs;
            for (const auto& t : tones) {
                v += t.amp * std::cos(kTwoPi * t.hz * (static_cast<double>(i) - t.ref) / fs + t.phase + abs_ph);
            }
            out[i] = static_cast<float>(unit * v);
        }
    }
    return out;
}

}  // namespace revenant::siggen
