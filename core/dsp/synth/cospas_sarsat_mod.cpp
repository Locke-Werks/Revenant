#include "core/dsp/synth/cospas_sarsat_mod.h"

#include <algorithm>
#include <cmath>
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

}  // namespace revenant::siggen
