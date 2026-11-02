#include "core/dsp/synth/atv_mod.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace revenant::siggen {

std::vector<float> atv_composite(decode::AtvStandard standard, dsp::SampleRate rate,
                                 std::span<const AtvPicture> pictures, double lead_lines) {
    const decode::AtvTiming t = decode::atv_timing(standard);
    const double h = t.line_us;
    const double half = h / 2.0;
    const double field = t.lines_per_field * h;
    // The first field-sync edge sits on the line grid, so the first field is
    // a first field (BT.470 figure 2, notes 2 and 3).
    const double ov0 = (lead_lines + t.pre_equalising_lines) * h;
    const double fields = 2.0 * static_cast<double>(pictures.size());
    const double total_us = ov0 + fields * field;
    const auto samples = static_cast<std::size_t>(total_us * static_cast<double>(rate) / 1e6);
    const double active = h - t.front_porch_us - t.active_start_us;

    std::vector<float> out(samples, 0.0F);
    for (std::size_t n = 0; n < samples; ++n) {
        const double us = static_cast<double>(n) * 1e6 / static_cast<double>(rate);
        // Field k's vertical interval runs from OV_k - l H.
        const double from_start = us - (ov0 - t.pre_equalising_lines * h);
        const auto k = static_cast<long long>(std::floor(from_start / field));
        double level = 0.0;
        bool handled = false;
        if (k >= 0 && static_cast<double>(k) < fields) {
            const double ov = ov0 + static_cast<double>(k) * field;
            const double u = us - ov;
            const double in_half = std::fmod(u + t.pre_equalising_lines * h, half);
            if (u < 0.0) {
                level = in_half < t.equalising_us ? t.sync_level : 0.0;
                handled = true;
            } else if (u < t.field_sync_lines * h) {
                level = in_half < t.field_sync_us ? t.sync_level : 0.0;
                handled = true;
            } else if (u < (t.field_sync_lines + t.post_equalising_lines) * h) {
                level = in_half < t.equalising_us ? t.sync_level : 0.0;
                handled = true;
            }
            if (!handled) {
                const double in_line = std::fmod(us - ov0, h);
                const double line_start = us - in_line;
                const bool second = (k % 2) == 1;
                const double first = decode::atv_first_line_offset(standard, second);
                const double row_f = (line_start - ov) / h - first;
                const auto row = static_cast<long long>(std::lround(row_f));
                if (in_line < t.sync_us) {
                    level = t.sync_level;
                } else if (row >= 0 && row < static_cast<long long>(t.rows_per_field) &&
                           in_line >= t.active_start_us && in_line < t.active_start_us + active) {
                    const AtvPicture& pic = pictures[static_cast<std::size_t>(k / 2)];
                    const auto y = static_cast<std::size_t>(2 * row + (second ? 1 : 0));
                    const auto x = std::min<std::size_t>(
                        pic.width - 1,
                        static_cast<std::size_t>((in_line - t.active_start_us) / active * pic.width));
                    const double g = y < pic.height ? pic.gray[y * pic.width + x] / 255.0 : 0.0;
                    level = t.black_level + (100.0 - t.black_level) * g;
                }
                handled = true;
            }
        }
        if (!handled) {
            level = std::fmod(us - ov0 + 1000.0 * h, h) < t.sync_us ? t.sync_level : 0.0;
        }
        out[n] = static_cast<float>(level);
    }
    return out;
}

std::vector<Complex32> atv_fm_modulate(std::span<const float> composite, decode::AtvStandard standard,
                                       dsp::SampleRate rate, double deviation_hz) {
    const decode::AtvTiming t = decode::atv_timing(standard);
    std::vector<Complex32> out(composite.size());
    double phase = 0.0;
    for (std::size_t n = 0; n < composite.size(); ++n) {
        const double x = (composite[n] - t.sync_level) / (100.0 - t.sync_level);
        const double f = deviation_hz * (2.0 * x - 1.0);
        phase = std::fmod(phase + 2.0 * std::numbers::pi * f / static_cast<double>(rate),
                          2.0 * std::numbers::pi);
        out[n] = Complex32(static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase)));
    }
    return out;
}

std::vector<Complex32> atv_am_modulate(std::span<const float> composite, decode::AtvStandard standard) {
    const decode::AtvTiming t = decode::atv_timing(standard);
    std::vector<Complex32> out(composite.size());
    for (std::size_t n = 0; n < composite.size(); ++n) {
        const double x = (composite[n] - t.sync_level) / (100.0 - t.sync_level);
        out[n] = Complex32(static_cast<float>(1.0 - 0.875 * x), 0.0F);
    }
    return out;
}

}  // namespace revenant::siggen
