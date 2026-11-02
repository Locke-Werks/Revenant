#include "core/dsp/synth/wsjt_slow_mod.h"

#include <cmath>
#include <numbers>

#include "core/decode/wspr.h"

namespace revenant::siggen {

Status slow_add(const SlowModConfig& config, const decode::SlowProtocol& protocol, std::span<const int> tones,
                std::span<float> audio) {
    if (config.rate <= 0) return fail("transmitter rate must be positive");
    if (static_cast<int>(tones.size()) != protocol.symbols) return fail("wrong number of channel symbols");
    const auto ref = decode::slow_reference(protocol, tones, config.rate, config.drift_hz);
    const auto start = static_cast<std::int64_t>(std::llround(config.start_seconds * static_cast<double>(config.rate)));
    const double w = 2.0 * std::numbers::pi * config.frequency_hz / static_cast<double>(config.rate);
    for (std::size_t i = 0; i < ref.size(); ++i) {
        const std::int64_t j = start + static_cast<std::int64_t>(i);
        if (j < 0 || j >= static_cast<std::int64_t>(audio.size())) continue;
        const double ph = w * static_cast<double>(i);
        const double v = ref[i].real() * std::cos(ph) - ref[i].imag() * std::sin(ph);
        audio[static_cast<std::size_t>(j)] += static_cast<float>(config.amplitude * v);
    }
    return {};
}

Status wspr_add(const SlowModConfig& config, std::string_view call, std::string_view grid, int power_dbm,
                std::span<float> audio) {
    auto bits = decode::wspr_pack(call, grid, power_dbm);
    if (!bits) return std::unexpected(bits.error());
    const auto tones = decode::wspr_channel_tones(*bits);
    return slow_add(config, decode::wspr_protocol(), tones, audio);
}

}  // namespace revenant::siggen
