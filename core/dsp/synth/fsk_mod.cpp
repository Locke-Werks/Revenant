#include "core/dsp/synth/fsk_mod.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace revenant::siggen {
namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

// A run of one tone lasting a whole or fractional number of units.
struct Segment {
    bool mark = true;
    double units = 0.0;
};

// Continuous-phase two-tone audio from a list of segments. Each sample takes
// the tone of the segment its own instant falls in, and the phase carries
// through every change of tone, which is what a keyed oscillator does.
std::vector<float> render_segments(std::span<const Segment> segments, SampleRate rate,
                                   double baud, double mark_hz, double space_hz,
                                   double amplitude, double space_gain = 1.0) {
    double total_units = 0.0;
    for (const Segment& s : segments) {
        total_units += s.units;
    }
    const double samples_per_unit = static_cast<double>(rate) / baud;
    const auto count = static_cast<std::size_t>(std::ceil(total_units * samples_per_unit));

    std::vector<float> out;
    out.reserve(count);
    std::size_t segment = 0;
    double segment_end = segments.empty() ? 0.0 : segments[0].units;
    double phase = 0.0;
    for (std::size_t n = 0; n < count; ++n) {
        const double u = static_cast<double>(n) / samples_per_unit;
        while (segment + 1 < segments.size() && u >= segment_end) {
            ++segment;
            segment_end += segments[segment].units;
        }
        const double f = segments[segment].mark ? mark_hz : space_hz;
        const double gain = segments[segment].mark ? 1.0 : space_gain;
        out.push_back(static_cast<float>(amplitude * gain * std::cos(phase)));
        phase = std::fmod(phase + kTwoPi * f / static_cast<double>(rate), kTwoPi);
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// RTTY
// ---------------------------------------------------------------------------

Expected<std::vector<std::uint8_t>> ita2_encode_text(std::u32string_view text) {
    using Case = decode::Ita2Code::Case;
    std::vector<std::uint8_t> out;
    out.push_back(decode::kIta2LetterShift);
    bool figures = false;
    for (const char32_t c : text) {
        const auto code = decode::ita2_encode(c);
        if (!code) {
            return fail("a character in the text has no ITA2 combination");
        }
        if (code->needs == Case::Figures && !figures) {
            out.push_back(decode::kIta2FigureShift);
            figures = true;
        } else if (code->needs == Case::Letters && figures) {
            out.push_back(decode::kIta2LetterShift);
            figures = false;
        }
        out.push_back(code->combination);
    }
    return out;
}

Expected<std::vector<float>> rtty_render(const RttyModConfig& config,
                                         std::span<const std::uint8_t> combinations) {
    if (config.rate <= 0 || !(config.baud > 0.0)) {
        return fail("RTTY needs a positive sample rate and baud");
    }
    const double mark = static_cast<double>(config.mark_hz) + config.tone_offset_hz;
    const double space = config.space_above_mark ? mark + static_cast<double>(config.shift_hz)
                                                 : mark - static_cast<double>(config.shift_hz);
    if (!(space > 0.0) || !(mark > 0.0) || std::max(mark, space) * 2.0 >= static_cast<double>(config.rate)) {
        return fail("RTTY tones must lie between zero and half the sample rate");
    }
    if (!(config.stop_units > 0.0)) {
        return fail("RTTY needs a stop element");
    }

    std::vector<Segment> segments;
    segments.push_back({true, config.lead_units});
    for (const std::uint8_t combination : combinations) {
        // S.1 clause 3.2: start polarity is condition A, which is space.
        segments.push_back({false, 1.0});
        // Table 1 note: code element 1 first. Condition Z, binary 1, is mark.
        for (std::size_t unit = 0; unit < decode::kIta2Units; ++unit) {
            segments.push_back({((combination >> unit) & 1U) != 0U, 1.0});
        }
        segments.push_back({true, config.stop_units});
    }
    segments.push_back({true, config.tail_units});

    return render_segments(segments, config.rate, config.baud, mark, space, config.amplitude);
}

// ---------------------------------------------------------------------------
// AX.25
// ---------------------------------------------------------------------------

Expected<std::vector<std::uint8_t>> ax25_frame_octets(const Ax25FrameSpec& frame) {
    if (frame.repeaters.size() > decode::kAx25MaximumRepeaters) {
        return fail("more AX.25 repeaters than the decoder accepts");
    }
    std::vector<std::uint8_t> out;
    std::vector<const decode::Ax25Address*> order = {&frame.destination, &frame.source};
    for (const auto& r : frame.repeaters) {
        order.push_back(&r);
    }
    for (std::size_t i = 0; i < order.size(); ++i) {
        auto encoded = decode::ax25_encode_address(*order[i], i + 1 == order.size());
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        out.insert(out.end(), encoded->begin(), encoded->end());
    }
    out.push_back(frame.control);
    if (frame.pid) {
        out.push_back(*frame.pid);
    }
    out.insert(out.end(), frame.information.begin(), frame.information.end());
    return out;
}

std::vector<std::uint8_t> hdlc_bits(std::span<const std::vector<std::uint8_t>> frames,
                                    const Ax25ModConfig& config) {
    std::vector<std::uint8_t> bits;
    const auto flags = [&bits](std::size_t count) {
        for (std::size_t f = 0; f < count; ++f) {
            for (unsigned b = 0; b < 8; ++b) {
                bits.push_back(static_cast<std::uint8_t>((decode::kAx25Flag >> b) & 1U));
            }
        }
    };

    flags(config.leading_flags);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        std::vector<std::uint8_t> octets = frames[i];
        // Clause 3.8 by way of Finnegan and Benson section 3.2: the FCS
        // register holds the check bit-reversed, so low octet first, each
        // least significant bit first, sends bit 15 first.
        const std::uint16_t fcs = decode::ax25_fcs(octets);
        octets.push_back(static_cast<std::uint8_t>(fcs & 0xFFU));
        octets.push_back(static_cast<std::uint8_t>(fcs >> 8U));

        int ones = 0;
        for (const std::uint8_t octet : octets) {
            for (unsigned b = 0; b < 8; ++b) {
                const auto bit = static_cast<std::uint8_t>((octet >> b) & 1U);
                bits.push_back(bit);
                ones = bit != 0U ? ones + 1 : 0;
                if (ones == decode::kAx25StuffAfterOnes) {
                    bits.push_back(0);
                    ones = 0;
                }
            }
        }
        flags(i + 1 == frames.size() ? config.trailing_flags : config.flags_between);
    }
    return bits;
}

Expected<std::vector<float>> afsk_render_bits(const Ax25ModConfig& config,
                                              std::span<const std::uint8_t> bits) {
    if (config.rate <= 0 || !(config.baud > 0.0)) {
        return fail("AFSK needs a positive sample rate and baud");
    }
    if (config.mark_hz <= 0 || config.space_hz <= 0 || config.mark_hz * 2 >= config.rate ||
        config.space_hz * 2 >= config.rate) {
        return fail("AFSK tones must lie between zero and half the sample rate");
    }

    std::vector<Segment> segments;
    segments.reserve(bits.size());
    bool mark = true;
    for (const std::uint8_t bit : bits) {
        if (bit == 0U) {
            mark = !mark;
        }
        segments.push_back({mark, 1.0});
    }
    return render_segments(segments, config.rate, config.baud * (1.0 + config.baud_error),
                           static_cast<double>(config.mark_hz),
                           static_cast<double>(config.space_hz), config.amplitude,
                           std::pow(10.0, config.space_gain_db / 20.0));
}

Expected<std::vector<float>> ax25_render(const Ax25ModConfig& config,
                                         std::span<const std::vector<std::uint8_t>> frames) {
    const std::vector<std::uint8_t> bits = hdlc_bits(frames, config);
    return afsk_render_bits(config, bits);
}

}  // namespace revenant::siggen
