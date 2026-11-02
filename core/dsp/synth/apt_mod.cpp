#include "core/dsp/synth/apt_mod.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace revenant::siggen {

using namespace decode;

std::vector<AptLineWords> apt_format(const AptImage& image) {
    const auto sync_a = apt_sync_a();
    const auto sync_b = apt_sync_b();
    std::vector<AptLineWords> out(image.rows);
    for (std::size_t r = 0; r < image.rows; ++r) {
        AptLineWords& w = out[r];
        const std::size_t line = image.first_line + r;
        std::copy(sync_a.begin(), sync_a.end(), w.begin() + kAptChannelASync);
        std::copy(sync_b.begin(), sync_b.end(), w.begin() + kAptChannelBSync);

        const std::size_t minute = line % 120;
        std::uint8_t space_a = 0;
        std::uint8_t space_b = 255;
        if (minute < 2) {
            space_a = space_b = 0;
        } else if (minute < 4) {
            space_a = space_b = 255;
        }
        std::fill_n(w.begin() + kAptChannelASpace, kAptSpaceWords, space_a);
        std::fill_n(w.begin() + kAptChannelBSpace, kAptSpaceWords, space_b);

        std::copy_n(image.channel_a.begin() + static_cast<std::ptrdiff_t>(r * kAptChannelWords),
                    kAptChannelWords, w.begin() + kAptChannelAVideo);
        std::copy_n(image.channel_b.begin() + static_cast<std::ptrdiff_t>(r * kAptChannelWords),
                    kAptChannelWords, w.begin() + kAptChannelBVideo);

        const std::size_t wedge = (line % kAptFrameLines) / 8;  // 0..15
        const auto grey = [](int n) {
            return static_cast<std::uint8_t>(std::lround(255.0 * n / 8.0));
        };
        for (std::size_t ch = 0; ch < 2; ++ch) {
            std::uint8_t v = 0;
            if (wedge < 8) {
                v = grey(static_cast<int>(wedge) + 1);
            } else if (wedge == 8) {
                v = 0;
            } else if (wedge <= 12) {
                v = static_cast<std::uint8_t>(100 + 10 * (wedge - 9));  // thermistors
            } else if (wedge == 13) {
                v = 90;                                                  // patch
            } else if (wedge == 14) {
                v = ch == 0 ? 60 : 200;                                  // back scan
            } else {
                v = grey(ch == 0 ? image.channel_a_wedge : image.channel_b_wedge);
            }
            std::fill_n(w.begin() + (ch == 0 ? kAptChannelATelemetry : kAptChannelBTelemetry),
                        kAptTelemetryWords, v);
        }
    }
    return out;
}

std::vector<float> apt_audio(const std::vector<AptLineWords>& lines, const AptAudioSpec& spec) {
    const double word_rate = kAptWordRate / (1.0 + spec.clock_ppm * 1e-6);
    const std::size_t words = lines.size() * kAptLineWords;
    const auto lead = static_cast<std::size_t>(spec.lead_seconds * spec.sample_rate);
    const auto body = static_cast<std::size_t>(static_cast<double>(words) * spec.sample_rate /
                                               word_rate);
    std::vector<float> out(lead + body + lead, 0.0F);
    const auto word = [&](std::size_t k) {
        k = std::min(k, words - 1);
        return static_cast<double>(lines[k / kAptLineWords][k % kAptLineWords]);
    };
    for (std::size_t i = 0; i < body; ++i) {
        // Linear between word centres.
        const double pos = static_cast<double>(i) * word_rate / spec.sample_rate - 0.5;
        const double fl = std::floor(pos);
        const double f = pos - fl;
        const std::size_t k = fl < 0.0 ? 0 : static_cast<std::size_t>(fl);
        const double v = fl < 0.0 ? word(0) : (1.0 - f) * word(k) + f * word(k + 1);
        const double env = 1.0 + spec.modulation_index * (2.0 * v / 255.0 - 1.0);
        const std::size_t n = lead + i;
        const double t = static_cast<double>(n) / spec.sample_rate;
        out[n] = static_cast<float>(spec.amplitude * env *
                                    std::sin(2.0 * std::numbers::pi * kAptSubcarrierHz * t));
    }
    return out;
}

std::vector<dsp::Complex32> apt_fm(const std::vector<float>& audio, const AptFmSpec& spec) {
    std::vector<dsp::Complex32> out(audio.size());
    double phase = 0.0;
    for (std::size_t n = 0; n < audio.size(); ++n) {
        const double t = static_cast<double>(n) / spec.sample_rate;
        const double f = spec.deviation_hz * audio[n] + spec.doppler_start_hz +
                         spec.doppler_rate_hz_per_s * t;
        phase += 2.0 * std::numbers::pi * f / spec.sample_rate;
        phase = std::remainder(phase, 2.0 * std::numbers::pi);
        out[n] = {static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
    }
    return out;
}

}  // namespace revenant::siggen
