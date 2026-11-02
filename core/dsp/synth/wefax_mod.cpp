#include "core/dsp/synth/wefax_mod.h"

#include <cmath>
#include <numbers>

#include "core/decode/wefax.h"

namespace revenant::siggen {

Expected<std::vector<float>> wefax_render(const WefaxModConfig& config,
                                          std::span<const std::uint8_t> gray) {
    if (config.ioc != 576 && config.ioc != 288) {
        return fail("wefax_render: the IOC must be 576 or 288");
    }
    if (config.lpm != 60 && config.lpm != 90 && config.lpm != 120 && config.lpm != 240) {
        return fail("wefax_render: the rate must be 60, 90, 120 or 240 lines per minute");
    }
    const std::size_t width = decode::wefax_width(config.ioc);
    if (gray.empty() || gray.size() % width != 0) {
        return fail("wefax_render: the image must be whole rows of IOC x pi bytes");
    }
    const double black = decode::kWefaxCentreHz -
                         (config.white_high ? 1.0 : -1.0) * decode::kWefaxDeviationHz;
    const double white = 2.0 * decode::kWefaxCentreHz - black;
    const double line = 60.0 / config.lpm;
    const double rows = static_cast<double>(gray.size() / width);
    const double start_hz = config.ioc == 576 ? decode::kWefaxStart576Hz : decode::kWefaxStart288Hz;
    const double white_part = config.asymmetric_phasing ? 0.05 : 0.5;

    // Section times, seconds from the end of the leading pad.
    const double t_phasing = config.start_seconds;
    const double t_image = t_phasing + std::round(config.phasing_seconds / line) * line;
    const double t_stop = t_image + rows * line;
    const double t_end = config.stop ? t_stop + 15.0 : t_stop;

    const auto hz_at = [&](double t) {
        if (t < t_phasing) {
            // III 5.2.2: alternating black and white at the IOC frequency.
            return std::fmod(t * start_hz, 1.0) < 0.5 ? white : black;
        }
        if (t < t_image) {
            // III 5.2.3: white from the line's start, then black.
            return std::fmod((t - t_phasing) / line, 1.0) < white_part ? white : black;
        }
        if (t < t_stop) {
            const double at = (t - t_image) / line;
            const auto r = static_cast<std::size_t>(at);
            const auto x = std::min(width - 1, static_cast<std::size_t>((at - static_cast<double>(r)) *
                                                                        static_cast<double>(width)));
            const double v = gray[r * width + x] / 255.0;
            return black + v * (white - black);
        }
        // III 5.2.5: 450 Hz for 5 s, then 10 s of black.
        if (t < t_stop + 5.0) {
            return std::fmod((t - t_stop) * decode::kWefaxStopHz, 1.0) < 0.5 ? white : black;
        }
        return black;
    };

    const auto pad = static_cast<std::size_t>(config.pad_seconds * config.rate);
    const auto body = static_cast<std::size_t>(t_end * config.rate);
    std::vector<float> out(pad, 0.0F);
    out.reserve(2 * pad + body);
    double phase = 0.0;
    for (std::size_t n = 0; n < body; ++n) {
        const double t = static_cast<double>(n) / config.rate;
        phase += 2.0 * std::numbers::pi * (hz_at(t) + config.offset_hz) / config.rate;
        if (phase > 2.0 * std::numbers::pi) {
            phase -= 2.0 * std::numbers::pi;
        }
        out.push_back(static_cast<float>(config.amplitude * std::sin(phase)));
    }
    out.insert(out.end(), pad, 0.0F);
    return out;
}

}  // namespace revenant::siggen
