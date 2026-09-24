#include "core/dsp/synth/dsc_mod.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <string>
#include <utility>

namespace revenant::siggen {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

// Table A1-2: two digits to a symbol, the first sent first. `digits` must be
// an even number of decimal digits.
void put_digits(std::vector<std::uint8_t>& out, std::string_view digits) {
    for (std::size_t i = 0; i + 1 < digits.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>((digits[i] - '0') * 10 + (digits[i + 1] - '0')));
    }
}

// Clause 5.2: nine digits and a tenth of 0.
Status put_identity(std::vector<std::uint8_t>& out, std::uint64_t identity) {
    if (identity > 999'999'999ULL) {
        return fail(std::format("{} has more than the nine digits clause 5.2 gives an identity",
                                identity));
    }
    put_digits(out, std::format("{:09}0", identity));
    return {};
}

// Clause 8.1.2, or ten nines for no position.
void put_position(std::vector<std::uint8_t>& out, const std::optional<decode::DscPosition>& p) {
    if (!p) {
        put_digits(out, "9999999999");
        return;
    }
    const int quadrant = (p->latitude < 0.0 ? 2 : 0) + (p->longitude < 0.0 ? 1 : 0);
    const auto degrees_minutes = [](double value) {
        const double a = std::abs(value);
        auto whole = static_cast<int>(std::floor(a));
        auto minutes = static_cast<int>(std::lround((a - whole) * 60.0));
        if (minutes == 60) {
            ++whole;
            minutes = 0;
        }
        return std::pair{whole, minutes};
    };
    const auto [lat_d, lat_m] = degrees_minutes(p->latitude);
    const auto [lon_d, lon_m] = degrees_minutes(p->longitude);
    put_digits(out, std::format("{}{:02}{:02}{:03}{:02}", quadrant, lat_d, lat_m, lon_d, lon_m));
}

// Clause 8.1.3, or 8888.
void put_utc(std::vector<std::uint8_t>& out, const std::optional<std::uint16_t>& hhmm) {
    put_digits(out, hhmm ? std::format("{:04}", *hhmm) : std::string("8888"));
}

Status put_distress(std::vector<std::uint8_t>& out, const decode::DscCall& call) {
    out.push_back(call.nature_of_distress.value_or(107));
    put_position(out, call.distress_position);
    put_utc(out, call.utc_hhmm);
    out.push_back(call.subsequent_communications.value_or(decode::kDscNoInformation));
    return {};
}

}  // namespace

Expected<std::array<std::uint8_t, 3>> dsc_frequency_element(const decode::DscFrequency& f) {
    std::vector<std::uint8_t> out;
    switch (f.kind) {
        case decode::DscFrequency::Kind::Frequency:
            if (f.value % 100 != 0 || f.value / 100 > 299'999) {
                return fail("Table A1-5 carries a frequency below 30 MHz in whole 100 Hz");
            }
            put_digits(out, std::format("{:06}", f.value / 100));
            break;
        case decode::DscFrequency::Kind::HfChannel:
            if (f.value > 99'999) {
                return fail("Table A1-5 carries an HF channel in five digits");
            }
            put_digits(out, std::format("3{:05}", f.value));
            break;
        case decode::DscFrequency::Kind::VhfChannel:
            if (f.value > 9'999) {
                return fail("Table A1-5 carries a VHF channel in four digits");
            }
            put_digits(out, std::format("90{:04}", f.value));
            break;
        case decode::DscFrequency::Kind::Other:
            out.assign(f.symbols.begin(), f.symbols.end());
            break;
    }
    return std::array<std::uint8_t, 3>{out[0], out[1], out[2]};
}

Expected<std::vector<std::uint8_t>> dsc_encode(const decode::DscCall& call) {
    std::vector<std::uint8_t> s;
    // Clause 4.1: the format specifier twice.
    s.push_back(call.format);
    s.push_back(call.format);

    const auto put_routine_or_relay = [&]() -> Status {
        if (call.category == decode::kDscCategoryDistress) {
            // Tables A1-4.2 to A1-4.4.
            s.push_back(call.telecommand1.value_or(decode::kDscTelecommandDistressRelay));
            if (call.distress_id) {
                if (auto st = put_identity(s, *call.distress_id); !st) {
                    return st;
                }
            } else {
                // Clause 8.4: an unknown station in distress.
                for (int i = 0; i < 5; ++i) {
                    s.push_back(decode::kDscNoInformation);
                }
            }
            return put_distress(s, call);
        }
        // Clause 8.3.1: the two telecommands, 126 for none.
        s.push_back(call.telecommand1.value_or(decode::kDscNoInformation));
        s.push_back(call.telecommand2.value_or(decode::kDscNoInformation));
        if (!call.frequencies.empty()) {
            for (const decode::DscFrequency& f : call.frequencies) {
                auto element = dsc_frequency_element(f);
                if (!element) {
                    return std::unexpected(element.error());
                }
                s.insert(s.end(), element->begin(), element->end());
            }
        } else {
            s.insert(s.end(), call.message_symbols.begin(), call.message_symbols.end());
        }
        return {};
    };

    Status built;
    switch (call.format) {
        case decode::kDscFormatDistress:
            // Table A1-4.1.
            built = put_identity(s, call.self_id);
            if (built) {
                built = put_distress(s, call);
            }
            break;
        case decode::kDscFormatAllShips:
            s.push_back(call.category.value_or(decode::kDscCategorySafety));
            built = put_identity(s, call.self_id);
            if (built) {
                built = put_routine_or_relay();
            }
            break;
        case decode::kDscFormatIndividual:
        case decode::kDscFormatGroup:
        case decode::kDscFormatGeographicArea:
        case decode::kDscFormatAutomatic:
            if (call.format == decode::kDscFormatGeographicArea) {
                const std::string area = call.area_digits.value_or("0000000000");
                if (area.size() != 10) {
                    return fail("clause 5.3's area is ten digits");
                }
                put_digits(s, area);
            } else {
                built = put_identity(s, call.address.value_or(0));
                if (!built) {
                    return std::unexpected(built.error());
                }
            }
            s.push_back(call.category.value_or(decode::kDscCategoryRoutine));
            built = put_identity(s, call.self_id);
            if (built) {
                built = put_routine_or_relay();
            }
            break;
        default:
            return fail(std::format("format specifier {} is not one clause 4.1 lists", call.format));
    }
    if (!built) {
        return std::unexpected(built.error());
    }
    // Clause 9.
    s.push_back(call.eos != 0 ? call.eos : decode::kDscEos);
    return s;
}

std::vector<std::uint8_t> dsc_air_symbols(std::span<const std::uint8_t> information) {
    // Clause 10.2: the error-check character over one format specifier and
    // the rest to the end of sequence.
    const std::uint8_t ecc = decode::dsc_ecc(information.subspan(1));

    // Clause 9 and Figure 1: the DX positions carry the information, the
    // ECC, and the end of sequence twice more; the RX positions the
    // information and the ECC.
    std::vector<std::uint8_t> dx(decode::kDscPhasingDxCount, decode::kDscPhasingDx);
    dx.insert(dx.end(), information.begin(), information.end());
    dx.push_back(ecc);
    dx.push_back(information.back());
    dx.push_back(information.back());

    std::vector<std::uint8_t> rx(decode::kDscPhasingRx.begin(), decode::kDscPhasingRx.end());
    rx.insert(rx.end(), information.begin(), information.end());
    rx.push_back(ecc);

    std::vector<std::uint8_t> slots;
    const std::size_t pairs = std::max(dx.size(), rx.size());
    for (std::size_t p = 0; p < pairs; ++p) {
        slots.push_back(p < dx.size() ? dx[p] : decode::kDscNoInformation);
        slots.push_back(p < rx.size() ? rx[p] : decode::kDscNoInformation);
    }
    return slots;
}

std::vector<std::uint8_t> dsc_bits(std::span<const std::uint8_t> information) {
    std::vector<std::uint8_t> bits;
    // Clause 3.4: "alternating B-Y or Y-B".
    for (std::size_t i = 0; i < decode::kDscVhfDotBits; ++i) {
        bits.push_back(static_cast<std::uint8_t>(i % 2));
    }
    for (const std::uint8_t symbol : dsc_air_symbols(information)) {
        const auto code = decode::dsc_encode_symbol(symbol);
        bits.insert(bits.end(), code.begin(), code.end());
    }
    return bits;
}

namespace {

// The subcarrier's continuous phase through every call and gap, and which
// tone each sample is on: -1 in a gap, where the carrier is unmodulated.
struct Subcarrier {
    std::vector<double> phase;
    std::vector<int> tone;
};

Expected<Subcarrier> subcarrier(const DscModConfig& config,
                                std::span<const std::vector<std::uint8_t>> calls_bits) {
    if (config.rate <= 0 || !(config.bit_rate > 0.0)) {
        return fail("a DSC transmitter needs a positive sample rate and bit rate");
    }
    const double samples_per_bit =
        static_cast<double>(config.rate) / (config.bit_rate * (1.0 + config.bit_rate_error));
    Subcarrier s;
    double phase = 0.0;
    const auto gap = [&](std::size_t bits) {
        const auto n = static_cast<std::size_t>(std::llround(static_cast<double>(bits) * samples_per_bit));
        for (std::size_t i = 0; i < n; ++i) {
            s.phase.push_back(phase);
            s.tone.push_back(-1);
        }
    };
    gap(config.lead_bits);
    for (std::size_t c = 0; c < calls_bits.size(); ++c) {
        const std::vector<std::uint8_t>& bits = calls_bits[c];
        const auto n = static_cast<std::size_t>(
            std::ceil(static_cast<double>(bits.size()) * samples_per_bit));
        for (std::size_t i = 0; i < n; ++i) {
            const auto k =
                std::min(bits.size() - 1, static_cast<std::size_t>(static_cast<double>(i) / samples_per_bit));
            // Clause 1.4: Y, 1, on the lower frequency.
            const bool y = bits[k] != 0U;
            const double hz = static_cast<double>(y ? config.y_hz : config.b_hz);
            s.phase.push_back(phase);
            s.tone.push_back(y ? 1 : 0);
            phase = std::fmod(phase + kTwoPi * hz / static_cast<double>(config.rate), kTwoPi);
        }
        gap(c + 1 == calls_bits.size() ? config.tail_bits : config.gap_bits);
    }
    return s;
}

}  // namespace

Expected<std::vector<float>> dsc_render_audio(const DscModConfig& config,
                                              std::span<const std::vector<std::uint8_t>> calls_bits) {
    auto s = subcarrier(config, calls_bits);
    if (!s) {
        return std::unexpected(s.error());
    }
    // With pre-emphasis the discriminator's output for each tone is in
    // proportion to its frequency, so the lower tone sits below the upper by
    // their ratio.
    const double y_level = config.preemphasis ? config.amplitude * static_cast<double>(config.y_hz) /
                                                    static_cast<double>(config.b_hz)
                                              : config.amplitude;
    std::vector<float> out(s->phase.size());
    for (std::size_t n = 0; n < out.size(); ++n) {
        if (s->tone[n] < 0) {
            out[n] = 0.0F;
            continue;
        }
        const double level = s->tone[n] == 1 ? y_level : config.amplitude;
        out[n] = static_cast<float>(level * std::sin(s->phase[n]));
    }
    return out;
}

Expected<std::vector<Complex32>> dsc_render_baseband(
    const DscModConfig& config, std::span<const std::vector<std::uint8_t>> calls_bits) {
    auto s = subcarrier(config, calls_bits);
    if (!s) {
        return std::unexpected(s.error());
    }
    // Clause 1.3.2: phase modulation by the subcarrier at the stated index.
    // Without pre-emphasis the same index is applied as frequency deviation
    // scaled to the upper tone, which is what plain FM of the subcarrier
    // would produce.
    std::vector<Complex32> out(s->phase.size());
    for (std::size_t n = 0; n < out.size(); ++n) {
        double theta = 0.0;
        if (s->tone[n] >= 0) {
            if (config.preemphasis) {
                theta = config.modulation_index * std::sin(s->phase[n]);
            } else {
                const double hz = static_cast<double>(s->tone[n] == 1 ? config.y_hz : config.b_hz);
                theta = config.modulation_index * static_cast<double>(config.b_hz) / hz *
                        std::sin(s->phase[n]);
            }
        }
        out[n] = Complex32(static_cast<float>(std::cos(theta)), static_cast<float>(std::sin(theta)));
    }
    return out;
}

}  // namespace revenant::siggen
