#include "core/dsp/synth/ft8_mod.h"

#include <cmath>
#include <numbers>

namespace revenant::siggen {

Status ft_add(const FtModConfig& config, const decode::Wsjt77& payload, std::span<float> audio) {
    if (config.rate < decode::kWsjtRate || config.rate % decode::kWsjtRate != 0) {
        return fail("FT transmitter rate must be a whole multiple of 12 kHz");
    }
    const auto tones = decode::ft_channel_tones(config.protocol, payload);
    const auto ref = decode::ft_reference(config.protocol, tones, config.rate);
    const auto start = static_cast<std::int64_t>(std::llround(config.start_seconds * static_cast<double>(config.rate)));
    const double w = 2.0 * std::numbers::pi * config.frequency_hz / static_cast<double>(config.rate);
    for (std::size_t i = 0; i < ref.size(); ++i) {
        const std::int64_t j = start + static_cast<std::int64_t>(i);
        if (j < 0 || j >= static_cast<std::int64_t>(audio.size())) continue;
        const double ph = w * static_cast<double>(j);
        const double v = ref[i].real() * std::cos(ph) - ref[i].imag() * std::sin(ph);
        audio[static_cast<std::size_t>(j)] += static_cast<float>(config.amplitude * v);
    }
    return {};
}

Status ft_add_text(const FtModConfig& config, std::string_view text, std::span<float> audio) {
    auto bits = decode::wsjt_pack(text);
    if (!bits) return std::unexpected(bits.error());
    return ft_add(config, *bits, audio);
}

}  // namespace revenant::siggen
