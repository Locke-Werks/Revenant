// BeiDou B1I D1 transmitter side. See beidou_mod.h.

#include "core/dsp/synth/beidou_mod.h"

#include <cmath>

namespace revenant::siggen {

namespace {

std::uint64_t enc(double value, int lsb_exp, unsigned bits) {
    const auto v = static_cast<std::int64_t>(std::llround(std::ldexp(value, -lsb_exp)));
    return static_cast<std::uint64_t>(v) & ((std::uint64_t{1} << bits) - 1U);
}

// Split a value over an MSB field and an LSB field.
void split(B1iInfo& b, unsigned hi_first, unsigned hi_n, unsigned lo_first, unsigned lo_n,
           std::uint64_t v) {
    b1i_put(b, hi_first, hi_n, v >> lo_n);
    b1i_put(b, lo_first, lo_n, v & ((std::uint64_t{1} << lo_n) - 1U));
}

}  // namespace

void b1i_put(B1iInfo& bits, unsigned first, unsigned count, std::uint64_t value) {
    for (unsigned i = 0; i < count; ++i) {
        bits[first + i] = static_cast<std::uint8_t>((value >> (count - 1U - i)) & 1U);
    }
}

B1iInfo b1i_header(unsigned fra_id, std::uint32_t sow) {
    B1iInfo b{};
    b1i_put(b, 1, 11, 0b11100010010);
    b1i_put(b, 16, 3, fra_id);
    split(b, 19, 8, 31, 12, sow);
    return b;
}

void b1i_put_clock_iono(B1iInfo& b, const decode::gnss::B1iClockIono& c) {
    b1i_put(b, 43, 1, c.sat_h1 ? 1 : 0);
    b1i_put(b, 44, 5, c.aodc);
    b1i_put(b, 49, 4, c.urai);
    b1i_put(b, 61, 13, c.week);
    split(b, 74, 9, 91, 8, enc(c.toc_s, 3, 17));
    b1i_put(b, 99, 10, static_cast<std::uint64_t>(std::llround(c.tgd1_s / 0.1e-9)) & 0x3FFU);
    split(b, 109, 4, 121, 6, static_cast<std::uint64_t>(std::llround(c.tgd2_s / 0.1e-9)) & 0x3FFU);
    b1i_put(b, 127, 8, enc(c.alpha[0], -30, 8));
    b1i_put(b, 135, 8, enc(c.alpha[1], -27, 8));
    b1i_put(b, 151, 8, enc(c.alpha[2], -24, 8));
    b1i_put(b, 159, 8, enc(c.alpha[3], -24, 8));
    split(b, 167, 6, 181, 2, enc(c.beta[0], 11, 8));
    b1i_put(b, 183, 8, enc(c.beta[1], 14, 8));
    b1i_put(b, 191, 8, enc(c.beta[2], 16, 8));
    split(b, 199, 4, 211, 4, enc(c.beta[3], 16, 8));
    b1i_put(b, 215, 11, enc(c.a2, -66, 11));
    split(b, 226, 7, 241, 17, enc(c.a0, -33, 24));
    split(b, 258, 5, 271, 17, enc(c.a1, -50, 22));
    b1i_put(b, 288, 5, c.aode);
}

void b1i_put_ephemeris(B1iInfo& s2, B1iInfo& s3, const decode::gnss::B1iEphemeris& e) {
    split(s2, 43, 10, 61, 6, enc(e.delta_n_sc_s, -43, 16));
    split(s2, 67, 16, 91, 2, enc(e.cuc_rad, -31, 18));
    split(s2, 93, 20, 121, 12, enc(e.m0_sc, -31, 32));
    split(s2, 133, 10, 151, 22, enc(e.e, -33, 32));
    b1i_put(s2, 181, 18, enc(e.cus_rad, -31, 18));
    split(s2, 199, 4, 211, 14, enc(e.crc_m, -6, 18));
    split(s2, 225, 8, 241, 10, enc(e.crs_m, -6, 18));
    split(s2, 251, 12, 271, 20, enc(e.sqrt_a, -19, 32));
    const std::uint64_t toe = enc(e.toe_s, 3, 17);
    b1i_put(s2, 291, 2, toe >> 15U);
    b1i_put(s3, 43, 10, (toe >> 5U) & 0x3FFU);
    b1i_put(s3, 61, 5, toe & 0x1FU);
    split(s3, 66, 17, 91, 15, enc(e.i0_sc, -31, 32));
    split(s3, 106, 7, 121, 11, enc(e.cic_rad, -31, 18));
    split(s3, 132, 11, 151, 13, enc(e.omega_dot_sc_s, -43, 24));
    split(s3, 164, 9, 181, 9, enc(e.cis_rad, -31, 18));
    split(s3, 190, 13, 211, 1, enc(e.idot_sc_s, -43, 14));
    split(s3, 212, 21, 241, 11, enc(e.omega0_sc, -31, 32));
    split(s3, 252, 11, 271, 21, enc(e.omega_sc, -31, 32));
}

std::vector<std::uint8_t> b1i_encode(const B1iInfo& b) {
    std::vector<std::uint8_t> out;
    out.reserve(300);
    for (unsigned i = 1; i <= 15; ++i) {
        out.push_back(b[i]);
    }
    std::uint16_t info = 0;
    for (unsigned i = 16; i <= 26; ++i) {
        info = static_cast<std::uint16_t>((info << 1U) | b[i]);
    }
    const std::uint16_t cw = decode::gnss::b1i_bch_encode(info);
    for (int i = 14; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((cw >> static_cast<unsigned>(i)) & 1U));
    }
    for (unsigned w = 1; w < 10; ++w) {
        std::uint16_t ia = 0;
        std::uint16_t ib = 0;
        for (unsigned i = 0; i < 11; ++i) {
            ia = static_cast<std::uint16_t>((ia << 1U) | b[30 * w + 1 + i]);
            ib = static_cast<std::uint16_t>((ib << 1U) | b[30 * w + 12 + i]);
        }
        const std::uint16_t ca = decode::gnss::b1i_bch_encode(ia);
        const std::uint16_t cb = decode::gnss::b1i_bch_encode(ib);
        for (int i = 14; i >= 0; --i) {
            out.push_back(static_cast<std::uint8_t>((ca >> static_cast<unsigned>(i)) & 1U));
            out.push_back(static_cast<std::uint8_t>((cb >> static_cast<unsigned>(i)) & 1U));
        }
    }
    return out;
}

}  // namespace revenant::siggen
