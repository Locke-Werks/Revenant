#include "core/engine/passband_notch.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>

#include "core/dsp/notch_map.h"
#include "core/dsp/spectrum_levels_reference.h"

namespace revenant::engine {

namespace {

// The audio frequency a passband offset demodulates to, signed so a bin on
// the wrong side of the carrier comes out non-positive and is left alone.
// The inverse of dsp::notch_offsets, in double so a bin centre is not
// rounded to a whole hertz first.
[[nodiscard]] std::optional<double> audio_of_offset(Demod mode, double offset_hz,
                                                    double cw_pitch_hz) {
    switch (mode) {
        case Demod::Usb: return offset_hz;
        case Demod::Lsb: return -offset_hz;
        case Demod::Am:
        case Demod::Sam:
        case Demod::Dsb: return std::abs(offset_hz);
        case Demod::Cw: return offset_hz + cw_pitch_hz;
        case Demod::Nfm:
        case Demod::Wfm:
        case Demod::P25p1:
        case Demod::Dstar:
        case Demod::Tetra:
        case Demod::Dmr:
        case Demod::Raw: return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

PassbandNotchMarks map_passband_notches(const PassbandNotchRequest& request) {
    PassbandNotchMarks out;
    const auto pitch = static_cast<double>(request.cw_pitch);
    for (std::size_t i = 0; i < request.notches.size(); ++i) {
        const dsp::NotchSpec& n = request.notches[i];
        if (!n.enabled) {
            continue;
        }
        const auto audio = dsp::notch_audio_hz(request.demod, n.hz, request.cw_pitch);
        const dsp::NotchOffsets offsets =
            dsp::notch_offsets(request.demod, audio.value_or(0.0), pitch);
        if (offsets.audio_only) {
            out.audio_only = true;
            out.marks[out.count++] = PassbandNotchMark{
                static_cast<double>(n.hz), static_cast<double>(n.width_hz), n.depth_db,
                static_cast<std::uint8_t>(i)};
            continue;
        }
        for (std::uint8_t k = 0; k < offsets.count; ++k) {
            out.marks[out.count++] =
                PassbandNotchMark{offsets.hz[k], static_cast<double>(n.width_hz), n.depth_db,
                                  static_cast<std::uint8_t>(i)};
        }
    }
    return out;
}

std::size_t shape_passband(std::span<float> power_db, double bin_width_hz, double bin_zero_hz,
                           const PassbandNotchRequest& request,
                           const PassbandNotchMarks& marks,
                           std::span<std::uint8_t> excluded) {
    for (std::size_t b = 0; b < excluded.size(); ++b) {
        excluded[b] = 0;
    }
    if (marks.audio_only || marks.count == 0 || request.audio_rate <= 0 ||
        excluded.size() < power_db.size()) {
        return 0;
    }

    const auto pitch = static_cast<double>(request.cw_pitch);
    const double nyquist = 0.5 * static_cast<double>(request.audio_rate);

    // One section per enabled notch, designed once and evaluated per bin.
    std::array<std::optional<dsp::NotchDesign>, dsp::kMaxNotches> designs{};
    std::array<double, dsp::kMaxNotches> centres{};
    for (std::size_t i = 0; i < request.notches.size(); ++i) {
        const dsp::NotchSpec& n = request.notches[i];
        if (!n.enabled) {
            continue;
        }
        const auto audio = dsp::notch_audio_hz(request.demod, n.hz, request.cw_pitch);
        if (!audio) {
            continue;
        }
        auto design = dsp::design_notch(*audio, n.depth_db, static_cast<double>(n.width_hz),
                                        request.audio_rate);
        if (design) {
            designs[i] = *design;
            centres[i] = *audio;
        }
    }

    for (std::size_t b = 0; b < power_db.size(); ++b) {
        const double offset = bin_zero_hz + bin_width_hz * static_cast<double>(b);
        const auto audio = audio_of_offset(request.demod, offset, pitch);
        if (audio && *audio > 0.0 && *audio < nyquist) {
            // Each section at the point of this bin nearest its centre. A
            // notch is often narrower than a bin, and evaluated at the bin
            // centre alone a carrier sitting on the notch would show a few dB
            // of the depth it actually loses, or the full depth, depending on
            // where the bin edges happen to fall.
            const double half_bin = 0.5 * bin_width_hz;
            double shaped = 0.0;
            for (std::size_t i = 0; i < designs.size(); ++i) {
                if (designs[i]) {
                    const double at = std::clamp(centres[i], *audio - half_bin, *audio + half_bin);
                    shaped += dsp::notch_section_db(*designs[i], at, request.audio_rate);
                }
            }
            power_db[b] += static_cast<float>(shaped);
        }
    }

    std::size_t count = 0;
    for (const PassbandNotchMark& mark : marks.view()) {
        const double half = 0.5 * mark.width_hz;
        const double lo = (mark.offset_hz - half - bin_zero_hz) / bin_width_hz;
        const double hi = (mark.offset_hz + half - bin_zero_hz) / bin_width_hz;
        const auto first = static_cast<std::ptrdiff_t>(std::ceil(lo));
        const auto last = static_cast<std::ptrdiff_t>(std::floor(hi));
        for (std::ptrdiff_t b = std::max<std::ptrdiff_t>(first, 0);
             b <= last && b < static_cast<std::ptrdiff_t>(power_db.size()); ++b) {
            auto& flag = excluded[static_cast<std::size_t>(b)];
            if (flag == 0) {
                flag = 1;
                ++count;
            }
        }
    }
    return count;
}

bool measure_outside(std::span<const float> power_db, std::span<const std::uint8_t> excluded,
                     std::span<float> scratch, std::array<float, 2>& levels) {
    std::size_t kept = 0;
    for (std::size_t b = 0; b < power_db.size() && kept < scratch.size(); ++b) {
        if (b >= excluded.size() || excluded[b] == 0) {
            scratch[kept++] = power_db[b];
        }
    }
    if (kept == 0) {
        return false;
    }
    dsp::SpectrumLevelsParams params;
    params.bins = static_cast<std::uint32_t>(kept);
    return dsp::reference_spectrum_levels(params, scratch.first(kept), levels).has_value();
}

}  // namespace revenant::engine
