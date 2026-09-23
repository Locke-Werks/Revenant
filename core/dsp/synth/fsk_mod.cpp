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

// ---------------------------------------------------------------------------
// POCSAG
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kPocsagMessageBits = 20;

void pad_to_codeword(std::vector<std::uint8_t>& bits, std::span<const std::uint8_t> filler) {
    std::size_t f = 0;
    while (bits.size() % kPocsagMessageBits != 0) {
        bits.push_back(filler[f % filler.size()]);
        ++f;
    }
}

}  // namespace

Expected<std::vector<std::uint8_t>> pocsag_numeric_bits(std::string_view text) {
    std::vector<std::uint8_t> bits;
    const auto push = [&bits](unsigned v) {
        for (unsigned b = 0; b < 4; ++b) {
            bits.push_back(static_cast<std::uint8_t>((v >> b) & 1U));
        }
    };
    for (const char c : text) {
        // M.584-2 Table 3.
        unsigned v = 0;
        if (c >= '0' && c <= '9') {
            v = static_cast<unsigned>(c - '0');
        } else if (c == 'U') {
            v = 0xB;
        } else if (c == ' ') {
            v = 0xC;
        } else if (c == '-') {
            v = 0xD;
        } else if (c == ']') {
            v = 0xE;
        } else if (c == '[') {
            v = 0xF;
        } else {
            return fail("a character in the text is not in M.584-2 Table 3");
        }
        push(v);
    }
    // Clause 2.1: fill with spaces, 1100 sent bit 1 first.
    const std::uint8_t space[] = {0, 0, 1, 1};
    pad_to_codeword(bits, space);
    return bits;
}

Expected<std::vector<std::uint8_t>> pocsag_alphanumeric_bits(std::string_view text) {
    std::vector<std::uint8_t> bits;
    for (const char c : text) {
        const auto v = static_cast<unsigned char>(c);
        if (v > 0x7F) {
            return fail("International Alphabet No. 5 is seven bits");
        }
        for (unsigned b = 0; b < 7; ++b) {
            bits.push_back(static_cast<std::uint8_t>((v >> b) & 1U));
        }
    }
    const std::uint8_t null[] = {0};
    pad_to_codeword(bits, null);
    return bits;
}

std::vector<std::uint32_t> pocsag_codewords(std::span<const PocsagPageSpec> pages) {
    std::vector<std::uint32_t> slots;
    const auto frame_of_next = [&slots] { return (slots.size() % decode::kPocsagCodewordsPerBatch) / 2; };

    for (std::size_t p = 0; p < pages.size(); ++p) {
        const PocsagPageSpec& page = pages[p];
        if (p > 0) {
            // Clause 1.2: at least one address or idle codeword between the
            // end of one message and the next message's address.
            slots.push_back(decode::kPocsagIdle);
        }
        const std::size_t frame = page.identity & 0x7U;
        while (frame_of_next() != frame) {
            slots.push_back(decode::kPocsagIdle);
        }
        // Clause 1.3.2: flag 0, 18 address bits, 2 function bits.
        const std::uint32_t address = (((page.identity >> 3U) & 0x3FFFFU) << 2U) |
                                      (page.function & 0x3U);
        slots.push_back(decode::pocsag_encode(address));
        // Clause 1.3.3: flag 1, 20 message bits.
        for (std::size_t i = 0; i + kPocsagMessageBits <= page.message_bits.size();
             i += kPocsagMessageBits) {
            std::uint32_t payload = 0;
            for (std::size_t k = 0; k < kPocsagMessageBits; ++k) {
                payload = (payload << 1U) | (page.message_bits[i + k] & 1U);
            }
            slots.push_back(decode::pocsag_encode((1U << 20U) | payload));
        }
    }
    // Clause 1.2: the last codeword should be idle, and the batch completes.
    slots.push_back(decode::kPocsagIdle);
    while (slots.size() % decode::kPocsagCodewordsPerBatch != 0) {
        slots.push_back(decode::kPocsagIdle);
    }

    std::vector<std::uint32_t> words;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (i % decode::kPocsagCodewordsPerBatch == 0) {
            words.push_back(decode::kPocsagSync);
        }
        words.push_back(slots[i]);
    }
    return words;
}

std::vector<std::uint8_t> pocsag_bits(std::span<const PocsagPageSpec> pages) {
    std::vector<std::uint8_t> bits;
    // Clause 1.1: "101010... repeated for a period of at least 576 bits".
    for (std::size_t i = 0; i < decode::kPocsagPreambleBits; ++i) {
        bits.push_back(static_cast<std::uint8_t>((i % 2 == 0) ? 1U : 0U));
    }
    for (const std::uint32_t word : pocsag_codewords(pages)) {
        for (unsigned b = 0; b < decode::kPocsagCodewordBits; ++b) {
            bits.push_back(static_cast<std::uint8_t>((word >> (31U - b)) & 1U));
        }
    }
    return bits;
}

namespace {

Status check_pocsag(const PocsagModConfig& config) {
    if (config.rate <= 0 || !(config.bit_rate > 0.0) ||
        config.bit_rate * 2.0 > static_cast<double>(config.rate)) {
        return fail("POCSAG needs a bit rate between zero and half the sample rate");
    }
    return {};
}

// M.539-3 clause 4.3: binary 0 is the positive shift.
double pocsag_level(std::uint8_t bit, bool invert) {
    const double level = (bit == 0U) ? 1.0 : -1.0;
    return invert ? -level : level;
}

}  // namespace

Expected<std::vector<float>> pocsag_render_audio(const PocsagModConfig& config,
                                                 std::span<const std::uint8_t> bits) {
    if (auto ok = check_pocsag(config); !ok) {
        return std::unexpected(ok.error());
    }
    const double samples_per_bit =
        static_cast<double>(config.rate) / (config.bit_rate * (1.0 + config.bit_rate_error));
    const auto count =
        static_cast<std::size_t>(std::ceil(static_cast<double>(bits.size()) * samples_per_bit));
    std::vector<float> out(count);
    for (std::size_t n = 0; n < count; ++n) {
        const auto k = std::min(bits.size() - 1,
                                static_cast<std::size_t>(static_cast<double>(n) / samples_per_bit));
        out[n] = static_cast<float>(config.amplitude * pocsag_level(bits[k], config.invert));
    }
    return out;
}

// ---------------------------------------------------------------------------
// SITOR-B and NAVTEX
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> sitor_b_signals(const SitorModConfig& config,
                                          std::span<const std::uint8_t> combinations) {
    // M.625-4 clause 4.4.2: phasing signal 2 in the DX position.
    std::vector<std::uint8_t> dx(config.phasing_pairs, decode::kSitorPhasing2);
    const std::size_t first_traffic = dx.size();
    if (config.line_end_first) {
        // Clause 4.6.1: carriage return (No. 27) and line feed (No. 28).
        dx.push_back(decode::sitor_encode(0b01000));
        dx.push_back(decode::sitor_encode(0b00010));
    }
    for (const std::uint8_t c : combinations) {
        dx.push_back(decode::sitor_encode(c));
    }
    // Clause 4.6.7.1: idle signal alpha to finish, plus two more so the RX
    // copies of the last two traffic signals are on the air before the end.
    for (std::size_t i = 0; i < config.closing_alphas + 2; ++i) {
        dx.push_back(decode::kSitorPhasing1);
    }

    std::vector<std::uint8_t> slots;
    slots.reserve(2 * dx.size());
    for (std::size_t k = 0; k < dx.size(); ++k) {
        slots.push_back(dx[k]);
        // Clause 4.2: the RX slot after DX slot k repeats DX slot k - 2, five
        // slots back. During phasing, and before the first repeat is due, it
        // carries phasing signal 1 (clause 4.4.2).
        const bool repeat = k >= 2 && k - 2 >= first_traffic;
        slots.push_back(repeat ? dx[k - 2] : decode::kSitorPhasing1);
    }
    return slots;
}

Expected<std::vector<float>> sitor_b_render_signals(const SitorModConfig& config,
                                                    std::span<const std::uint8_t> signals) {
    if (config.rate <= 0 || config.shift_hz <= 0 || config.centre_hz * 2 >= config.rate) {
        return fail("SITOR needs a positive rate and shift and a centre below half the rate");
    }
    // Table 1 note 2: Y is the lower emitted frequency.
    const double half = static_cast<double>(config.shift_hz) / 2.0;
    const double centre = static_cast<double>(config.centre_hz) + config.tone_offset_hz;
    const double y_hz = config.upper_sideband ? centre - half : centre + half;
    const double b_hz = config.upper_sideband ? centre + half : centre - half;

    std::vector<Segment> segments;
    segments.reserve(signals.size() * decode::kSitorSignalUnits);
    for (const std::uint8_t s : signals) {
        // Table 1 note 3: bit position 1 first, Y = 1.
        for (std::size_t u = 0; u < decode::kSitorSignalUnits; ++u) {
            segments.push_back({((s >> u) & 1U) != 0U, 1.0});
        }
    }
    return render_segments(segments, config.rate, decode::kSitorBaud, y_hz, b_hz, config.amplitude);
}

Expected<std::vector<float>> sitor_b_render(const SitorModConfig& config,
                                            std::span<const std::uint8_t> combinations) {
    const std::vector<std::uint8_t> signals = sitor_b_signals(config, combinations);
    return sitor_b_render_signals(config, signals);
}

std::u32string navtex_text(char area, char subject, int serial, std::u32string_view message) {
    std::u32string text = U"ZCZC ";
    text.push_back(static_cast<char32_t>(area));
    text.push_back(static_cast<char32_t>(subject));
    text.push_back(static_cast<char32_t>(U'0' + (serial / 10) % 10));
    text.push_back(static_cast<char32_t>(U'0' + serial % 10));
    text += U"\r\n";
    text += message;
    text += U"NNNN\r\n\n";
    return text;
}

Expected<std::vector<dsp::Complex32>> pocsag_render_baseband(const PocsagModConfig& config,
                                                             std::span<const std::uint8_t> bits) {
    if (auto ok = check_pocsag(config); !ok) {
        return std::unexpected(ok.error());
    }
    const double samples_per_bit =
        static_cast<double>(config.rate) / (config.bit_rate * (1.0 + config.bit_rate_error));
    const auto count =
        static_cast<std::size_t>(std::ceil(static_cast<double>(bits.size()) * samples_per_bit));
    std::vector<dsp::Complex32> out(count);
    double phase = 0.0;
    for (std::size_t n = 0; n < count; ++n) {
        const auto k = std::min(bits.size() - 1,
                                static_cast<std::size_t>(static_cast<double>(n) / samples_per_bit));
        out[n] = dsp::Complex32(static_cast<float>(config.amplitude * std::cos(phase)),
                                static_cast<float>(config.amplitude * std::sin(phase)));
        phase = std::fmod(phase + kTwoPi * config.deviation_hz * pocsag_level(bits[k], config.invert) /
                                      static_cast<double>(config.rate),
                          kTwoPi);
    }
    return out;
}

}  // namespace revenant::siggen
