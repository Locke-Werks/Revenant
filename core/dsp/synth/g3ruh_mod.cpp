#include "core/dsp/synth/g3ruh_mod.h"

#include <cmath>
#include <numbers>

#include "core/dsp/synth/fsk_mod.h"

namespace revenant::siggen {

std::vector<std::uint8_t> g3ruh_bits(const G3ruhModConfig& config,
                                     std::span<const std::vector<std::uint8_t>> frames) {
    Ax25ModConfig hdlc;
    hdlc.leading_flags = config.leading_flags;
    hdlc.flags_between = config.flags_between;
    hdlc.trailing_flags = config.trailing_flags;
    const std::vector<std::uint8_t> line = nrzi_levels(hdlc_bits(frames, hdlc));
    decode::G3ruhScrambler scrambler;
    std::vector<std::uint8_t> out;
    out.reserve(line.size());
    for (const std::uint8_t b : line) {
        out.push_back(scrambler.push(b));
    }
    return out;
}

Expected<std::vector<float>> g3ruh_render_audio(const G3ruhModConfig& config,
                                                std::span<const std::uint8_t> bits) {
    if (config.rate < 2 * static_cast<dsp::SampleRate>(config.baud)) {
        return fail("a G3RUH transmitter needs at least two samples a bit");
    }
    const double spb = static_cast<double>(config.rate) / (config.baud * (1.0 + config.baud_error));
    const auto n = static_cast<std::size_t>(std::ceil(static_cast<double>(bits.size()) * spb));
    // Each bit is a raised-cosine pulse of roll-off 0.5 centred on the bit,
    // six bits either side. Its spectrum is half amplitude at 4800 Hz and
    // nothing past 7200 Hz, the article's "-6 db at 4800 Hz, -60db at 7500
    // Hz", and it crosses zero at every other bit's centre.
    constexpr double kRollOff = 0.5;
    constexpr double kReachBits = 6.0;
    const auto pulse = [&](double t_bits) {
        const double sinc = t_bits == 0.0 ? 1.0 : std::sin(std::numbers::pi * t_bits) / (std::numbers::pi * t_bits);
        const double d = 1.0 - (2.0 * kRollOff * t_bits) * (2.0 * kRollOff * t_bits);
        const double shape = std::abs(d) < 1e-9 ? std::numbers::pi / 4.0
                                                : std::cos(std::numbers::pi * kRollOff * t_bits) / d;
        return sinc * shape;
    };
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / spb - 0.5;  // in bits, from bit 0's centre
        const auto first = static_cast<std::ptrdiff_t>(std::ceil(t - kReachBits));
        const auto last = static_cast<std::ptrdiff_t>(std::floor(t + kReachBits));
        double acc = 0.0;
        for (std::ptrdiff_t k = std::max<std::ptrdiff_t>(first, 0);
             k <= last && k < static_cast<std::ptrdiff_t>(bits.size()); ++k) {
            acc += (bits[static_cast<std::size_t>(k)] != 0U ? 1.0 : -1.0) * pulse(t - static_cast<double>(k));
        }
        out[i] = static_cast<float>(config.amplitude * acc);
    }
    return out;
}

Expected<std::vector<dsp::Complex32>> g3ruh_render_baseband(const G3ruhModConfig& config,
                                                            std::span<const std::uint8_t> bits) {
    auto audio = g3ruh_render_audio(config, bits);
    if (!audio) {
        return std::unexpected(audio.error());
    }
    std::vector<dsp::Complex32> out(audio->size());
    double phase = 0.0;
    const double k = 2.0 * std::numbers::pi * config.deviation_hz / (config.amplitude * config.rate);
    for (std::size_t i = 0; i < out.size(); ++i) {
        phase = std::fmod(phase + k * (*audio)[i], 2.0 * std::numbers::pi);
        out[i] = dsp::Complex32(static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase)));
    }
    return out;
}

}  // namespace revenant::siggen
