#include "core/dsp/synth/link11_mod.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace revenant::siggen {

namespace {
constexpr double kTwoPi = 2.0 * std::numbers::pi;
}  // namespace

std::vector<std::uint32_t> link11_picket_reply(std::span<const std::uint32_t> words) {
    const auto start = decode::link11_start_code();
    std::vector<std::uint32_t> f = {start.first, start.second};
    for (const auto w : words) {
        f.push_back(decode::link11_encode_word(w));
    }
    f.push_back(decode::kLink11AllOnes);
    f.push_back(decode::kLink11AllOnes);
    return f;
}

std::vector<std::uint32_t> link11_interrogation(std::span<const std::uint32_t> words, int address) {
    const auto start = decode::link11_start_code();
    std::vector<std::uint32_t> f = {start.first, start.second};
    for (const auto w : words) {
        f.push_back(decode::link11_encode_word(w));
    }
    f.push_back(0);
    f.push_back(0);
    const auto a = decode::link11_address_code(address);
    f.push_back(a.first);
    f.push_back(a.second);
    return f;
}

Expected<std::vector<float>> link11_render(const Link11ModConfig& config, std::span<const std::uint32_t> frames) {
    using namespace decode;
    if (config.rate < 6600) {
        return fail("Link-11 modulator: sample rate too low");
    }
    const double fs = static_cast<double>(config.rate);
    const double t = fs * link11_frame_seconds(config.frame_rate);
    const int total = config.preamble_frames + 1 + static_cast<int>(frames.size());
    const auto n = static_cast<std::size_t>(std::ceil(config.lead_in_samples + total * t + config.tail_samples));
    std::vector<float> out(n, 0.0F);
    // 5.1.3.3 volts, taken as tone amplitudes: the composite then has the
    // power of a sine of amplitude 0.775, scaled to the configured one.
    const double k = config.amplitude / 0.775;
    const double lead = config.lead_in_samples;
    std::array<double, 15> phase{};
    for (int d = 0; d < 15; ++d) {
        phase[static_cast<std::size_t>(d)] = std::numbers::pi * d * d / 15.0;
    }
    for (int f = 0; f < total; ++f) {
        const double onset = lead + f * t;
        const auto first = static_cast<std::size_t>(std::ceil(onset));
        const auto last = std::min(n, static_cast<std::size_t>(std::ceil(onset + t)));
        const bool pre = f < config.preamble_frames;
        if (f > config.preamble_frames) {
            const std::uint32_t bits = frames[static_cast<std::size_t>(f - config.preamble_frames - 1)];
            for (int d = 0; d < 15; ++d) {
                const int step = link11_phase_step(static_cast<int>((bits >> (2U * d)) & 1U),
                                                   static_cast<int>((bits >> (2U * d + 1U)) & 1U));
                phase[static_cast<std::size_t>(d)] -= step * std::numbers::pi / 4.0;
            }
        }
        for (std::size_t i = first; i < last; ++i) {
            const double tau = (static_cast<double>(i) - onset) / fs;
            const double abs_t = static_cast<double>(i) / fs;
            const double rf = kTwoPi * config.offset_hz * abs_t;
            double v = (pre ? 0.692 : 0.356) * std::cos(kTwoPi * kLink11DopplerHz * abs_t + rf);
            if (pre) {
                // The 2915 Hz tone reversed every frame.
                v += 0.346 * std::cos(kTwoPi * kLink11SyncHz * tau + std::numbers::pi * f + rf);
            } else {
                for (int d = 0; d < 15; ++d) {
                    const double hz = link11_tone_hz(d);
                    const double base = config.continuous_phase ? kTwoPi * hz * abs_t : kTwoPi * hz * tau;
                    v += 0.178 * std::cos(base + phase[static_cast<std::size_t>(d)] + rf);
                }
            }
            out[i] = static_cast<float>(k * v);
        }
    }
    return out;
}

}  // namespace revenant::siggen
