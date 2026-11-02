#include "core/dsp/synth/hrpt_mod.h"

#include <cmath>
#include <numbers>

namespace revenant::siggen {

using namespace decode;

std::vector<HrptFrameWords> hrpt_format(const HrptImage& image) {
    const std::uint64_t sync = hrpt_frame_sync();
    std::vector<HrptFrameWords> out(image.rows);
    for (std::size_t r = 0; r < image.rows; ++r) {
        HrptFrameWords& w = out[r];
        w.fill(0);
        for (std::size_t i = 0; i < kHrptSyncWords; ++i) {
            w[i] = static_cast<std::uint16_t>((sync >> (50 - 10 * i)) & 0x3FFU);
        }
        // Word 7: AVHRR sync, minor frame 1..3, address, stable, normal
        // input, channel 3A. Table 4.1.3.1-1.
        const auto minor = static_cast<std::uint16_t>(r % 3 + 1);
        w[kHrptIdWord] = static_cast<std::uint16_t>((1U << 9U) | (minor << 7U) |
                                                    ((image.spacecraft & 0xFU) << 3U) |
                                                    (1U << 1U) | 1U);
        const std::uint32_t msec =
            image.first_msec + static_cast<std::uint32_t>(std::lround(r * 1000.0 / 6.0));
        w[kHrptTimeWord] = static_cast<std::uint16_t>((image.day & 0x1FFU) << 1U);
        w[kHrptTimeWord + 1] = static_cast<std::uint16_t>((0b101U << 7U) | ((msec >> 20U) & 0x7FU));
        w[kHrptTimeWord + 2] = static_cast<std::uint16_t>((msec >> 10U) & 0x3FFU);
        w[kHrptTimeWord + 3] = static_cast<std::uint16_t>(msec & 0x3FFU);
        for (std::size_t s = 0; s < kHrptEarthSamples; ++s) {
            for (std::size_t ch = 0; ch < kHrptChannels; ++ch) {
                w[kHrptEarthWord + kHrptChannels * s + ch] =
                    image.channels[ch][r * kHrptEarthSamples + s] & 0x3FFU;
            }
        }
    }
    return out;
}

std::vector<dsp::Complex32> hrpt_baseband(const std::vector<HrptFrameWords>& frames,
                                          const HrptBasebandSpec& spec) {
    const std::size_t bits = frames.size() * kHrptFrameWords * kHrptWordBits;
    const double rate = kHrptBitRate * (1.0 - spec.clock_ppm * 1e-6);
    const auto lead = static_cast<std::size_t>(spec.lead_seconds * spec.sample_rate);
    const auto body = static_cast<std::size_t>(static_cast<double>(bits) * spec.sample_rate / rate);
    std::vector<dsp::Complex32> out(lead + body + lead);
    const double dev = spec.phase_deg * std::numbers::pi / 180.0;
    double carrier = 0.0;
    for (std::size_t n = 0; n < out.size(); ++n) {
        const double t = static_cast<double>(n) / spec.sample_rate;
        double phase = 0.0;
        if (n >= lead && n < lead + body) {
            const double pos = static_cast<double>(n - lead) * rate / spec.sample_rate;
            const auto b = static_cast<std::size_t>(pos);
            const auto& f = frames[b / (kHrptFrameWords * kHrptWordBits)];
            const std::size_t in = b % (kHrptFrameWords * kHrptWordBits);
            const unsigned bit = (f[in / kHrptWordBits] >> (9 - in % kHrptWordBits)) & 1U;
            const bool first = pos - static_cast<double>(b) < 0.5;
            // "0": +68 then -68; "1": -68 then +68. Table 4.1.2-2.
            phase = ((bit == 0U) == first) ? dev : -dev;
        }
        out[n] = std::polar(1.0F, static_cast<float>(carrier + phase));
        carrier += 2.0 * std::numbers::pi * (spec.carrier_hz + spec.drift_hz_per_s * t) /
                   spec.sample_rate;
        carrier = std::remainder(carrier, 2.0 * std::numbers::pi);
    }
    return out;
}

}  // namespace revenant::siggen
