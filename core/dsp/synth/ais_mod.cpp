#include "core/dsp/synth/ais_mod.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <string>
#include <string_view>

namespace revenant::siggen {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

// Annex 2 Table 12 and Table 6: 8 bits of ramp before the training sequence,
// and TE to TF, 8 bits, for the fall after the end flag.
constexpr std::size_t kRampBits = 8;

// Fields most significant bit first, in table order, per Annex 2 clause
// 3.3.7.
class BitWriter {
public:
    void put(std::uint64_t value, unsigned width) {
        for (unsigned i = width; i > 0; --i) {
            bits_.push_back(static_cast<std::uint8_t>((value >> (i - 1U)) & 1U));
        }
    }
    void put_signed(std::int64_t value, unsigned width) {
        put(static_cast<std::uint64_t>(value) & ((std::uint64_t{1} << width) - 1U), width);
    }
    [[nodiscard]] Status put_text(std::string_view text, std::size_t characters) {
        if (text.size() > characters) {
            return fail(std::format("\"{}\" is {} characters and the AIS field holds {}", text,
                                    text.size(), characters));
        }
        for (std::size_t i = 0; i < characters; ++i) {
            // Clause 3.3.7: unused characters are "@", at the end.
            const char c = i < text.size() ? text[i] : '@';
            const auto v = ais_sixbit_value(c);
            if (!v) {
                return fail(std::format("'{}' is not in M.1371-5 Annex 8 Table 47", c));
            }
            put(*v, 6);
        }
        return {};
    }
    [[nodiscard]] std::size_t size() const { return bits_.size(); }

    // Clause 3.3.7: "Unused bits in the last byte should be set to zero".
    [[nodiscard]] std::vector<std::uint8_t> octets() const {
        std::vector<std::uint8_t> out((bits_.size() + 7) / 8, 0);
        for (std::size_t i = 0; i < bits_.size(); ++i) {
            out[i / 8] = static_cast<std::uint8_t>(out[i / 8] | (bits_[i] << (7U - i % 8)));
        }
        return out;
    }

private:
    std::vector<std::uint8_t> bits_;
};

void put_header(BitWriter& w, const decode::AisMessage& m) {
    w.put(m.message_id, 6);
    w.put(m.repeat, 2);
    w.put(m.mmsi, 30);
}

// Table 48's position fields, or their "not available" values.
void put_position(BitWriter& w, const std::optional<decode::AisPosition>& p) {
    if (p) {
        w.put_signed(std::llround(p->longitude * decode::kAisPositionUnitsPerDegree), 28);
        w.put_signed(std::llround(p->latitude * decode::kAisPositionUnitsPerDegree), 27);
    } else {
        w.put_signed(decode::kAisLongitudeNotAvailable, 28);
        w.put_signed(decode::kAisLatitudeNotAvailable, 27);
    }
}

void put_motion(BitWriter& w, const decode::AisMessage& m) {
    w.put(m.sog_tenths.value_or(decode::kAisSogNotAvailable), 10);
    w.put(m.position_accuracy ? 1U : 0U, 1);
    put_position(w, m.position);
    w.put(m.cog_tenths.value_or(decode::kAisCogNotAvailable), 12);
    w.put(m.heading.value_or(decode::kAisHeadingNotAvailable), 9);
    // Table 48: 60 is "time stamp is not available", the default.
    w.put(m.timestamp.value_or(60), 6);
}

void put_dimensions(BitWriter& w, const std::optional<decode::AisDimensions>& d) {
    const decode::AisDimensions v = d.value_or(decode::AisDimensions{});
    w.put(v.to_bow, 9);
    w.put(v.to_stern, 9);
    w.put(v.to_port, 6);
    w.put(v.to_starboard, 6);
}

// Table 49 names a communication state and does not fill one in for a
// transmitter that has none; a station with no SOTDMA state to report is
// not one this project simulates, so a message with none sends zeros.
std::uint32_t state_of(const decode::AisMessage& m) {
    return m.communication_state.value_or(0U) & 0x7FFFFU;
}

}  // namespace

std::optional<unsigned> ais_sixbit_value(char c) {
    // Table 47: "@" to "_" are 0 to 31, space to "?" are 32 to 63.
    const auto u = static_cast<unsigned char>(c);
    if (u >= 64U && u <= 95U) {
        return u - 64U;
    }
    if (u >= 32U && u <= 63U) {
        return u;
    }
    return std::nullopt;
}

Expected<std::vector<std::uint8_t>> ais_encode(const decode::AisMessage& m) {
    BitWriter w;
    put_header(w, m);
    switch (m.message_id) {
        case 1:
        case 2:
        case 3: {
            // Table 48.
            w.put(m.navigational_status.value_or(15), 4);
            w.put_signed(m.rate_of_turn.value_or(-128), 8);
            put_motion(w, m);
            w.put(m.special_manoeuvre.value_or(0), 2);
            w.put(0, 3);
            w.put(m.raim ? 1U : 0U, 1);
            w.put(state_of(m), 19);
            break;
        }
        case 4:
        case 11: {
            // Table 51, with each field's "not available" value as default.
            const decode::AisMessage::Utc utc =
                m.utc.value_or(decode::AisMessage::Utc{0, 0, 0, 24, 60, 60});
            w.put(utc.year, 14);
            w.put(utc.month, 4);
            w.put(utc.day, 5);
            w.put(utc.hour, 5);
            w.put(utc.minute, 6);
            w.put(utc.second, 6);
            w.put(m.position_accuracy ? 1U : 0U, 1);
            put_position(w, m.position);
            w.put(m.epfd.value_or(0), 4);
            w.put(0, 1);
            w.put(0, 9);
            w.put(m.raim ? 1U : 0U, 1);
            w.put(state_of(m), 19);
            break;
        }
        case 5: {
            // Table 52.
            w.put(m.ais_version.value_or(0), 2);
            w.put(m.imo_number.value_or(0), 30);
            if (auto s = w.put_text(m.callsign.value_or(""), 7); !s) {
                return std::unexpected(s.error());
            }
            if (auto s = w.put_text(m.name.value_or(""), 20); !s) {
                return std::unexpected(s.error());
            }
            w.put(m.ship_type.value_or(0), 8);
            put_dimensions(w, m.dimensions);
            w.put(m.epfd.value_or(0), 4);
            const decode::AisMessage::Eta eta =
                m.eta.value_or(decode::AisMessage::Eta{0, 0, 24, 60});
            w.put(eta.month, 4);
            w.put(eta.day, 5);
            w.put(eta.hour, 5);
            w.put(eta.minute, 6);
            w.put(m.draught_tenths.value_or(0), 8);
            if (auto s = w.put_text(m.destination.value_or(""), 20); !s) {
                return std::unexpected(s.error());
            }
            w.put(m.dte_not_available.value_or(true) ? 1U : 0U, 1);
            w.put(0, 1);
            break;
        }
        case 18: {
            // Table 70.
            w.put(0, 8);
            put_motion(w, m);
            w.put(0, 2);
            const decode::AisMessage::ClassBFlags flags =
                m.class_b.value_or(decode::AisMessage::ClassBFlags{});
            w.put(flags.carrier_sense ? 1U : 0U, 1);
            w.put(flags.display ? 1U : 0U, 1);
            w.put(flags.dsc ? 1U : 0U, 1);
            w.put(flags.whole_band ? 1U : 0U, 1);
            w.put(flags.message_22 ? 1U : 0U, 1);
            w.put(m.assigned_mode.value_or(false) ? 1U : 0U, 1);
            w.put(m.raim ? 1U : 0U, 1);
            // The selector: Table 70 has a Class B "CS" unit always send 1.
            w.put(flags.carrier_sense ? 1U : 0U, 1);
            w.put(state_of(m), 19);
            break;
        }
        case 19: {
            // Table 71.
            w.put(0, 8);
            put_motion(w, m);
            w.put(0, 4);
            if (auto s = w.put_text(m.name.value_or(""), 20); !s) {
                return std::unexpected(s.error());
            }
            w.put(m.ship_type.value_or(0), 8);
            put_dimensions(w, m.dimensions);
            w.put(m.epfd.value_or(0), 4);
            w.put(m.raim ? 1U : 0U, 1);
            w.put(m.dte_not_available.value_or(true) ? 1U : 0U, 1);
            w.put(m.assigned_mode.value_or(false) ? 1U : 0U, 1);
            w.put(0, 4);
            break;
        }
        case 21: {
            // Table 73.
            const std::string name = m.name.value_or("");
            if (name.size() > 34) {
                return fail(std::format("an AtoN name of {} characters is more than Table 73's 20 "
                                        "and 14 of extension",
                                        name.size()));
            }
            w.put(m.aton_type.value_or(0), 5);
            if (auto s = w.put_text(std::string_view(name).substr(0, std::min<std::size_t>(20, name.size())), 20);
                !s) {
                return std::unexpected(s.error());
            }
            w.put(m.position_accuracy ? 1U : 0U, 1);
            put_position(w, m.position);
            put_dimensions(w, m.dimensions);
            w.put(m.epfd.value_or(0), 4);
            w.put(m.timestamp.value_or(60), 6);
            w.put(m.off_position.value_or(false) ? 1U : 0U, 1);
            w.put(m.aton_status.value_or(0), 8);
            w.put(m.raim ? 1U : 0U, 1);
            w.put(m.virtual_aton.value_or(false) ? 1U : 0U, 1);
            w.put(m.assigned_mode.value_or(false) ? 1U : 0U, 1);
            w.put(0, 1);
            if (name.size() > 20) {
                // The extension: only the characters needed, no "@", then
                // spare bits to the octet boundary, which octets() supplies.
                const std::string_view extension = std::string_view(name).substr(20);
                if (auto s = w.put_text(extension, extension.size()); !s) {
                    return std::unexpected(s.error());
                }
            }
            break;
        }
        case 24: {
            // Tables 78 and 79.
            const std::uint8_t part = m.part_number.value_or(0);
            w.put(part, 2);
            if (part == 0) {
                if (auto s = w.put_text(m.name.value_or(""), 20); !s) {
                    return std::unexpected(s.error());
                }
            } else if (part == 1) {
                w.put(m.ship_type.value_or(0), 8);
                if (auto s = w.put_text(m.vendor.value_or(""), 3); !s) {
                    return std::unexpected(s.error());
                }
                w.put(m.unit_model.value_or(0), 4);
                w.put(m.unit_serial.value_or(0), 20);
                if (auto s = w.put_text(m.callsign.value_or(""), 7); !s) {
                    return std::unexpected(s.error());
                }
                put_dimensions(w, m.dimensions);
                w.put(m.epfd.value_or(0), 4);
                w.put(0, 2);
            } else {
                return fail("Message 24 defines parts 0 and 1 only");
            }
            break;
        }
        default:
            return fail(std::format("Message {} is not one this transmitter encodes", m.message_id));
    }
    return w.octets();
}

std::vector<std::uint8_t> ais_packet_bits(std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> bits;
    bits.reserve(kRampBits + decode::kAisTrainingBits + 16 + data.size() * 10 + 24);
    // Table 12's ramp. Ones hold the NRZI level.
    for (std::size_t i = 0; i < kRampBits; ++i) {
        bits.push_back(1);
    }
    // Clause 3.2.2.3: "010101010...", not stuffed.
    for (std::size_t i = 0; i < decode::kAisTrainingBits; ++i) {
        bits.push_back(static_cast<std::uint8_t>(i % 2));
    }
    const auto put_flag = [&bits] {
        for (unsigned b = 0; b < 8; ++b) {
            bits.push_back(static_cast<std::uint8_t>((decode::kAisFlag >> b) & 1U));
        }
    };
    put_flag();

    const std::uint16_t fcs = decode::ais_fcs(data);
    std::vector<std::uint8_t> octets(data.begin(), data.end());
    octets.push_back(static_cast<std::uint8_t>(fcs & 0xFFU));
    octets.push_back(static_cast<std::uint8_t>(fcs >> 8U));
    int ones = 0;
    for (const std::uint8_t octet : octets) {
        for (unsigned b = 0; b < 8; ++b) {
            // Clause 3.3.7: least significant bit first.
            const auto bit = static_cast<std::uint8_t>((octet >> b) & 1U);
            bits.push_back(bit);
            ones = (bit != 0U) ? ones + 1 : 0;
            if (ones == decode::kAisStuffAfterOnes) {
                // Clause 3.2.2.1.
                bits.push_back(0);
                ones = 0;
            }
        }
    }
    put_flag();
    return bits;
}

Expected<std::vector<Complex32>> ais_render_packets(
    const AisModConfig& config, std::span<const std::vector<std::uint8_t>> packet_bits) {
    if (config.rate <= 0) {
        return fail("an AIS transmitter needs a positive sample rate");
    }
    if (!(config.bandwidth_time > 0.0)) {
        return fail("an AIS transmitter needs a positive bandwidth-time product");
    }
    const double bit_rate = decode::kAisBitRate * (1.0 + config.bit_rate_error);
    const double samples_per_bit = static_cast<double>(config.rate) / bit_rate;
    if (samples_per_bit < 2.0) {
        return fail("an AIS transmitter needs at least two samples a bit");
    }

    // The frequency pulse of GMSK: a one-bit rectangle through a Gaussian
    // filter whose 3 dB bandwidth is BT times the bit rate. In bit periods,
    // the Gaussian's standard deviation is sqrt(ln 2) / (2 pi BT), and the
    // rectangle convolved with it is the difference of two error functions.
    // Textbook GMSK, not a clause: clause 2.3.1 names GMSK and its BT, and
    // clause 2.3.2 the index.
    const double sigma = std::sqrt(std::log(2.0)) / (kTwoPi * config.bandwidth_time);
    const double scale = 1.0 / (sigma * std::numbers::sqrt2);
    const auto pulse = [scale](double t_bits) {
        return 0.5 * (std::erf((t_bits + 0.5) * scale) - std::erf((t_bits - 0.5) * scale));
    };
    // Past three standard deviations and half a bit the pulse is below 1e-3
    // of its peak.
    const int reach = static_cast<int>(std::ceil(0.5 + 3.5 * sigma)) + 1;

    std::vector<Complex32> out;
    const auto silence = [&](std::size_t bits) {
        out.resize(out.size() +
                       static_cast<std::size_t>(std::llround(static_cast<double>(bits) *
                                                             samples_per_bit)),
                   Complex32(0.0F, 0.0F));
    };

    silence(config.lead_bits);
    double phase = 0.0;
    double carrier_phase = 0.0;
    const std::size_t burst_count = packet_bits.size();
    for (std::size_t p = 0; p < burst_count; ++p) {
        const std::vector<std::uint8_t>& bits = packet_bits[p];
        // Clause 2.6: a change of level for a zero.
        std::vector<double> levels(bits.size());
        bool level = true;
        for (std::size_t k = 0; k < bits.size(); ++k) {
            if (bits[k] == 0U) {
                level = !level;
            }
            levels[k] = level ? 1.0 : -1.0;
        }
        // The level holds through the fall after the end flag.
        const std::size_t total_bits = bits.size() + kRampBits;
        const auto count = static_cast<std::size_t>(
            std::llround(static_cast<double>(total_bits) * samples_per_bit));
        const auto level_at = [&](long long k) {
            if (k < 0) {
                return levels.front();
            }
            if (static_cast<std::size_t>(k) >= levels.size()) {
                return levels.back();
            }
            return levels[static_cast<std::size_t>(k)];
        };
        for (std::size_t n = 0; n < count; ++n) {
            // Bit k occupies [k, k+1) bit periods; its pulse is centred on
            // k + 0.5.
            const double t = static_cast<double>(n) / samples_per_bit;
            const auto centre = static_cast<long long>(std::floor(t));
            double frequency = 0.0;
            for (long long k = centre - reach; k <= centre + reach; ++k) {
                frequency += level_at(k) * pulse(t - (static_cast<double>(k) + 0.5));
            }
            frequency *= config.deviation_hz;

            // Raised-cosine rise over the ramp bits and fall over the last
            // eight.
            double envelope = 1.0;
            const double fall_start = static_cast<double>(bits.size());
            if (t < static_cast<double>(kRampBits)) {
                envelope = 0.5 - 0.5 * std::cos(std::numbers::pi * t / kRampBits);
            } else if (t >= fall_start) {
                const double into = std::min((t - fall_start) / kRampBits, 1.0);
                envelope = 0.5 + 0.5 * std::cos(std::numbers::pi * into);
            }
            const double total = phase + carrier_phase;
            out.emplace_back(static_cast<float>(config.amplitude * envelope * std::cos(total)),
                             static_cast<float>(config.amplitude * envelope * std::sin(total)));
            phase = std::fmod(phase + kTwoPi * frequency / static_cast<double>(config.rate), kTwoPi);
            carrier_phase = std::fmod(
                carrier_phase + kTwoPi * config.carrier_offset_hz / static_cast<double>(config.rate),
                kTwoPi);
        }
        silence(p + 1 == burst_count ? config.tail_bits : config.gap_bits);
    }
    return out;
}

Expected<std::vector<Complex32>> ais_render(const AisModConfig& config,
                                            std::span<const decode::AisMessage> messages) {
    std::vector<std::vector<std::uint8_t>> packets;
    packets.reserve(messages.size());
    for (const decode::AisMessage& m : messages) {
        auto data = ais_encode(m);
        if (!data) {
            return std::unexpected(with_context(data.error(), "encoding an AIS message"));
        }
        packets.push_back(ais_packet_bits(*data));
    }
    return ais_render_packets(config, packets);
}

}  // namespace revenant::siggen
