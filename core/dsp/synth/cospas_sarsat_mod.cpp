#include "core/dsp/synth/cospas_sarsat_mod.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

#include "core/decode/cospas_sarsat.h"

namespace revenant::siggen {

namespace {

void put(std::vector<std::uint8_t>& bits, std::size_t first, std::size_t last, std::uint32_t value) {
    for (std::size_t i = first; i <= last; ++i) {
        bits[i - 1] = static_cast<std::uint8_t>((value >> (last - i)) & 1U);
    }
}

std::vector<std::uint8_t> header(bool long_message, bool self_test) {
    std::vector<std::uint8_t> bits(long_message ? decode::kCsLongBits : decode::kCsShortBits, 0);
    put(bits, 1, 15, 0x7FFF);
    put(bits, 16, 24, self_test ? decode::kCsFrameSyncTest : decode::kCsFrameSync);
    bits[24] = long_message ? 1 : 0;
    return bits;
}

// A3.3.1: round to 4 s, offset from the coarse value, sign then minutes
// then 4-second steps; zero offset is the default pattern.
void put_offset(std::vector<std::uint8_t>& bits, std::size_t sign_bit, unsigned minute_bits, double actual,
                double coarse) {
    const double rounded = std::round(std::abs(actual) * 900.0) / 900.0;
    const double off = rounded - coarse;
    const auto units = static_cast<std::uint32_t>(std::lround(std::abs(off) * 900.0));
    if (units == 0) {
        put(bits, sign_bit, sign_bit, 1);
        put(bits, sign_bit + 1, sign_bit + minute_bits, 0);
        put(bits, sign_bit + minute_bits + 1, sign_bit + minute_bits + 4, 15);
        return;
    }
    put(bits, sign_bit, sign_bit, off >= 0.0 ? 1U : 0U);
    put(bits, sign_bit + 1, sign_bit + minute_bits, units / 15);
    put(bits, sign_bit + minute_bits + 1, sign_bit + minute_bits + 4, units % 15);
}

}  // namespace

Expected<std::vector<dsp::Complex32>> cs_render_burst(const CsBurstConfig& c, std::span<const std::uint8_t> bits) {
    if (c.rate <= 0 || c.bit_rate <= 0.0) {
        return fail("cs_render_burst: rate and bit rate must be positive");
    }
    const double fs = static_cast<double>(c.rate);
    const double carrier = decode::kCsCarrierSeconds * fs;
    const double tb = fs / c.bit_rate;
    const double ramp = std::max(1.0, c.rise_seconds * fs);
    const auto lead = static_cast<std::size_t>(c.lead_seconds * fs);
    const auto body = static_cast<std::size_t>(carrier + tb * static_cast<double>(bits.size()));
    const auto total = lead + body + static_cast<std::size_t>(c.tail_seconds * fs);
    std::vector<dsp::Complex32> out(total);
    // Half-bit phase levels, figure 2.5: a one is + then -.
    const auto level = [&](double t) {
        if (t < carrier) {
            return 0.0;
        }
        const double b = (t - carrier) / tb;
        const auto k = static_cast<std::size_t>(b);
        if (k >= bits.size()) {
            return 0.0;
        }
        const bool first = (b - static_cast<double>(k)) < 0.5;
        return ((bits[k] != 0U) == first ? 1.0 : -1.0) * decode::kCsPhaseDeviation;
    };
    for (std::size_t i = 0; i < body; ++i) {
        // A linear ramp between levels: the average of the step over the
        // rise time, which is a ramp of that length.
        double phase = 0.0;
        const int taps = 8;
        for (int j = 0; j < taps; ++j) {
            phase += level(static_cast<double>(i) - ramp / 2.0 + ramp * (j + 0.5) / taps);
        }
        phase /= taps;
        out[lead + i] = std::polar(static_cast<float>(c.amplitude), static_cast<float>(phase + c.initial_phase));
    }
    return out;
}

std::vector<std::uint8_t> cs_standard_location(unsigned country, unsigned protocol, std::uint32_t identity,
                                               double latitude, double longitude, bool internal_source,
                                               bool homing, bool self_test) {
    auto bits = header(true, self_test);
    put(bits, 26, 26, 0);
    put(bits, 27, 36, country);
    put(bits, 37, 40, protocol);
    put(bits, 41, 64, identity);
    // A3.3.5.3: nearest quarter degree.
    const double clat = std::round(std::abs(latitude) * 4.0) / 4.0;
    const double clon = std::round(std::abs(longitude) * 4.0) / 4.0;
    put(bits, 65, 65, latitude < 0.0 ? 1U : 0U);
    put(bits, 66, 74, static_cast<std::uint32_t>(clat * 4.0));
    put(bits, 75, 75, longitude < 0.0 ? 1U : 0U);
    put(bits, 76, 85, static_cast<std::uint32_t>(clon * 4.0));
    // A3.3.5.4.
    put(bits, 107, 110, 0b1101);
    put(bits, 111, 111, internal_source ? 1U : 0U);
    put(bits, 112, 112, homing ? 1U : 0U);
    put_offset(bits, 113, 5, latitude, clat);
    put_offset(bits, 123, 5, longitude, clon);
    decode::cs_fill_bch(bits);
    return bits;
}

std::vector<std::uint8_t> cs_serial_user_location(unsigned country, unsigned beacon_type, std::uint32_t serial,
                                                  double latitude, double longitude) {
    auto bits = header(true, false);
    put(bits, 26, 26, 1);
    put(bits, 27, 36, country);
    put(bits, 37, 39, 0b011);
    put(bits, 40, 42, beacon_type);
    put(bits, 43, 43, 0);
    put(bits, 44, 63, serial);
    put(bits, 84, 85, 1);  // 121.5 MHz
    // A3.3.4.3: degrees and 4-minute steps, rounded.
    const auto quantise = [](double v) { return std::lround(std::abs(v) * 15.0); };  // 4-minute units
    const auto la = quantise(latitude);
    const auto lo = quantise(longitude);
    put(bits, 107, 107, 1);
    put(bits, 108, 108, latitude < 0.0 ? 1U : 0U);
    put(bits, 109, 115, static_cast<std::uint32_t>(la / 15));
    put(bits, 116, 119, static_cast<std::uint32_t>(la % 15));
    put(bits, 120, 120, longitude < 0.0 ? 1U : 0U);
    put(bits, 121, 128, static_cast<std::uint32_t>(lo / 15));
    put(bits, 129, 132, static_cast<std::uint32_t>(lo % 15));
    decode::cs_fill_bch(bits);
    return bits;
}

Expected<std::vector<dsp::Complex32>> sgb_render_burst(const SgbBurstConfig& config,
                                                       std::span<const std::uint8_t> bits) {
    if (bits.size() != decode::kSgbMessageBits) {
        return fail("SGB burst needs 250 message bits");
    }
    if (config.rate <= 0 || !(config.chip_rate > 0.0)) {
        return fail("SGB burst needs a positive sample rate and chip rate");
    }
    const auto mode = config.self_test ? decode::SgbMode::SelfTest : decode::SgbMode::Normal;
    const auto pi = decode::sgb_prn(mode, false);
    const auto pq = decode::sgb_prn(mode, true);
    // 2.2.7: a chip's value is its PRN chip xor its arm's bit; the preamble's
    // bits are zero; odd message bits on I, even on Q.
    auto arm = [&](const std::vector<std::uint8_t>& prn, std::size_t chip, bool q) -> double {
        std::uint8_t v = prn[chip];
        if (chip >= decode::kSgbPreambleChips) {
            const std::size_t k = (chip - decode::kSgbPreambleChips) / decode::kSgbChipsPerBit;
            v = static_cast<std::uint8_t>(v ^ bits[2 * k + (q ? 1U : 0U)]);
        }
        return 1.0 - 2.0 * v;  // Table 2.3
    };
    const double fs = static_cast<double>(config.rate);
    const auto lead = static_cast<std::size_t>(config.lead_seconds * fs);
    const auto tail = static_cast<std::size_t>(config.tail_seconds * fs);
    const double burst_seconds = static_cast<double>(decode::kSgbChipsPerArm) / config.chip_rate;
    const auto len = static_cast<std::size_t>(std::ceil(burst_seconds * fs));
    std::vector<dsp::Complex32> out(lead + len + tail, dsp::Complex32{});
    const double a = config.amplitude / std::numbers::sqrt2;
    for (std::size_t n = 0; n < len; ++n) {
        const double chips = static_cast<double>(n) * config.chip_rate / fs;
        const auto ci = static_cast<std::size_t>(chips);
        // 2.3.3: Q half a chip behind I.
        const double cq_f = chips - 0.5;
        double i = 0.0;
        double q = 0.0;
        if (ci < decode::kSgbChipsPerArm) {
            i = arm(pi, ci, false);
        }
        if (cq_f >= 0.0) {
            const auto cq = static_cast<std::size_t>(cq_f);
            if (cq < decode::kSgbChipsPerArm) {
                q = arm(pq, cq, true);
            }
        }
        const std::complex<double> v =
            std::complex<double>(i, q) * a * std::polar(1.0, config.initial_phase);
        out[lead + n] = {static_cast<float>(v.real()), static_cast<float>(v.imag())};
    }
    return out;
}

std::vector<std::uint8_t> sgb_message(const SgbMessageSpec& s) {
    std::vector<std::uint8_t> b(decode::kSgbMessageBits, 0);
    // T.018 Table 3.1.
    put(b, 1, 16, s.tac);
    put(b, 17, 30, s.serial);
    put(b, 31, 40, s.country);
    put(b, 41, 41, s.homing ? 1U : 0U);
    put(b, 42, 42, s.rls ? 1U : 0U);
    put(b, 43, 43, s.test_protocol ? 1U : 0U);
    if (s.latitude && s.longitude) {
        // 15 bits of a degree, rounded, per the note under Table 3.1.
        auto enc = [](double v, unsigned& deg, unsigned& frac) {
            auto units = static_cast<std::uint32_t>(std::llround(std::abs(v) * 32768.0));
            deg = units >> 15U;
            frac = units & 0x7FFFU;
        };
        unsigned d = 0;
        unsigned f = 0;
        enc(*s.latitude, d, f);
        put(b, 44, 44, *s.latitude < 0.0 ? 1U : 0U);
        put(b, 45, 51, d);
        put(b, 52, 66, f);
        enc(*s.longitude, d, f);
        put(b, 67, 67, *s.longitude < 0.0 ? 1U : 0U);
        put(b, 68, 75, d);
        put(b, 76, 90, f);
    } else {
        put(b, 44, 66, (1U << 22U) | (0b1111111U << 15U) | 0b000001111100000U);
        put(b, 67, 90, (1U << 23U) | (0b11111111U << 15U) | 0b111110000011111U);
    }
    put(b, 91, 93, s.vessel_id_type);
    put(b, 94, 115, static_cast<std::uint32_t>(s.vessel_id >> 22U));
    put(b, 116, 137, static_cast<std::uint32_t>(s.vessel_id & 0x3FFFFFU));
    put(b, 138, 140, s.beacon_type);
    put(b, 141, 154, s.rotating_field == 15 ? 0U : 0x3FFFU);
    // 3.3: the rotating field identifier, then its content.
    put(b, 155, 158, s.rotating_field);
    put(b, 159, 180, static_cast<std::uint32_t>(s.rotating_bits >> 22U));
    put(b, 181, 202, static_cast<std::uint32_t>(s.rotating_bits & 0x3FFFFFU));
    decode::sgb_fill_bch(b);
    return b;
}

}  // namespace revenant::siggen
