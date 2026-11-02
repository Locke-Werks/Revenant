// Synthetic GNSS constellations. See gnss_mod.h.

#include "core/dsp/synth/gnss_mod.h"

#include <cmath>
#include <numbers>
#include <utility>

namespace revenant::siggen {

Expected<GnssSynth> GnssSynth::create(dsp::SampleRate rate,
                                      std::vector<GnssSatelliteSpec> satellites,
                                      std::uint64_t noise_seed, bool add_noise) {
    if (rate <= 0) {
        return fail("gnss synth: sample rate must be positive");
    }
    GnssSynth s;
    s.fs_ = static_cast<double>(rate);
    s.rng_.seed(noise_seed);
    s.noise_ = add_noise;
    for (auto& spec : satellites) {
        if (spec.code.empty() || spec.periods_per_symbol == 0) {
            return fail("gnss synth: a satellite has no code or no symbol length");
        }
        State st;
        st.amplitude = std::sqrt(std::pow(10.0, spec.cn0_dbhz / 10.0) / s.fs_);
        st.chips_per_sample = spec.chip_rate_hz * (1.0 + spec.doppler_hz / spec.carrier_hz) / s.fs_;
        st.cycles_per_sample = (spec.offset_hz + spec.doppler_hz) / s.fs_;
        st.code_len = static_cast<double>(spec.code.size());
        st.spec = std::move(spec);
        s.sats_.push_back(std::move(st));
    }
    return s;
}

void GnssSynth::generate(std::span<dsp::Complex32> out) {
    constexpr double kTwoPi = 2.0 * std::numbers::pi;
    std::normal_distribution<float> gauss(0.0F, std::sqrt(0.5F));
    for (auto& v : out) {
        v = noise_ ? dsp::Complex32{gauss(rng_), gauss(rng_)} : dsp::Complex32{};
    }
    for (const auto& st : sats_) {
        const auto& sp = st.spec;
        const auto len = sp.code.size();
        // Positions are recomputed from the absolute sample index, so a
        // satellite's waveform does not depend on how generate() is called.
        for (std::size_t i = 0; i < out.size(); ++i) {
            const double n = static_cast<double>(n_ + i);
            const double chips = sp.code_phase_chips + n * st.chips_per_sample;
            const double period_f = std::floor(chips / st.code_len);
            const double within = chips - period_f * st.code_len;
            auto chip_idx = static_cast<std::size_t>(within);
            if (chip_idx >= len) {
                chip_idx = len - 1;
            }
            double sign = sp.code[chip_idx];
            if (period_f >= 0.0) {
                const auto period = static_cast<std::uint64_t>(period_f);
                if (!sp.overlay.empty()) {
                    sign *= sp.overlay[period % sp.overlay.size()];
                }
                const std::uint64_t sym = period / sp.periods_per_symbol;
                if (sym < sp.symbols.size() && sp.symbols[sym] != 0) {
                    sign = -sign;
                }
            }
            double cyc = sp.carrier_phase_cycles + n * st.cycles_per_sample;
            cyc -= std::floor(cyc);
            const double ph = kTwoPi * cyc;
            out[i] += dsp::Complex32{static_cast<float>(st.amplitude * sign * std::cos(ph)),
                                     static_cast<float>(st.amplitude * sign * std::sin(ph))};
        }
    }
    n_ += out.size();
}

std::size_t gnss_samples(dsp::SampleRate rate, double seconds) {
    return static_cast<std::size_t>(std::llround(static_cast<double>(rate) * seconds));
}

}  // namespace revenant::siggen
