// GLONASS L1OF transmitter side. See glonass_mod.h.

#include "core/dsp/synth/glonass_mod.h"

#include <cmath>

namespace revenant::siggen {

using decode::gnss::GlonassStringBits;

void glonass_put(GlonassStringBits& bits, unsigned hi, unsigned lo, std::uint32_t value) {
    const unsigned n = hi - lo + 1;
    for (unsigned i = 0; i < n; ++i) {
        bits[hi - i] = static_cast<std::uint8_t>((value >> (n - 1U - i)) & 1U);
    }
}

std::uint32_t glonass_signmag(double value, double lsb, unsigned bits) {
    const auto mag = static_cast<std::uint32_t>(std::llround(std::abs(value) / lsb));
    return (value < 0.0 ? (1U << (bits - 1U)) : 0U) | (mag & ((1U << (bits - 1U)) - 1U));
}

GlonassStringBits glonass_empty_string(unsigned number) {
    GlonassStringBits b{};
    glonass_put(b, 84, 81, number);
    return b;
}

std::vector<GlonassStringBits> glonass_immediate_strings(const decode::gnss::GlonassEphemeris& e) {
    std::vector<GlonassStringBits> s(4);
    for (unsigned i = 0; i < 4; ++i) {
        s[i] = glonass_empty_string(i + 1);
    }
    glonass_put(s[0], 78, 77, e.p1);
    glonass_put(s[0], 76, 72, e.tk_h);
    glonass_put(s[0], 71, 66, e.tk_min);
    glonass_put(s[0], 65, 65, e.tk_30s);
    for (unsigned a = 0; a < 3; ++a) {
        glonass_put(s[a], 64, 41, glonass_signmag(e.vel_km_s[a], std::ldexp(1.0, -20), 24));
        glonass_put(s[a], 40, 36, glonass_signmag(e.acc_km_s2[a], std::ldexp(1.0, -30), 5));
        glonass_put(s[a], 35, 9, glonass_signmag(e.pos_km[a], std::ldexp(1.0, -11), 27));
    }
    glonass_put(s[1], 80, 78, e.bn);
    glonass_put(s[1], 77, 77, e.p2 ? 1 : 0);
    glonass_put(s[1], 76, 70, e.tb_min / 15U);
    glonass_put(s[2], 80, 80, e.p3 ? 1 : 0);
    glonass_put(s[2], 79, 69, glonass_signmag(e.gamma_n, std::ldexp(1.0, -40), 11));
    glonass_put(s[2], 67, 66, e.p);
    glonass_put(s[2], 65, 65, e.ln3 ? 1 : 0);
    glonass_put(s[3], 80, 59, glonass_signmag(e.tau_n_s, std::ldexp(1.0, -30), 22));
    glonass_put(s[3], 58, 54, glonass_signmag(e.delta_tau_n_s, std::ldexp(1.0, -30), 5));
    glonass_put(s[3], 53, 49, e.en_days);
    glonass_put(s[3], 34, 34, e.p4 ? 1 : 0);
    glonass_put(s[3], 33, 30, e.ft);
    glonass_put(s[3], 26, 16, e.nt_days);
    glonass_put(s[3], 15, 11, e.slot);
    glonass_put(s[3], 10, 9, e.m_type);
    return s;
}

std::vector<std::uint8_t> glonass_string_symbols(GlonassStringBits bits) {
    bits[85] = 0;
    decode::gnss::glonass_fill_check(bits);
    std::vector<std::uint8_t> out;
    out.reserve(200);
    std::uint8_t r = 0;
    for (unsigned j = 0; j < 85; ++j) {
        r = static_cast<std::uint8_t>(bits[85 - j] ^ (j == 0 ? 0U : r));
        out.push_back(r);                                      // meander 0
        out.push_back(static_cast<std::uint8_t>(r ^ 1U));      // meander 1
    }
    for (std::uint8_t c : decode::gnss::glonass_time_mark()) {
        out.push_back(c);
    }
    return out;
}

}  // namespace revenant::siggen
