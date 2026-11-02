// GPS LNAV transmitter side. See gps_lnav_mod.h.

#include "core/dsp/synth/gps_lnav_mod.h"

#include <cmath>

namespace revenant::siggen {

namespace {

std::uint64_t enc(double value, int lsb_exp, unsigned bits) {
    const auto v = static_cast<std::int64_t>(std::llround(std::ldexp(value, -lsb_exp)));
    return static_cast<std::uint64_t>(v) & ((std::uint64_t{1} << bits) - 1U);
}

}  // namespace

void lnav_put(LnavWords& words, unsigned first, unsigned count, std::uint64_t value) {
    for (unsigned i = 0; i < count; ++i) {
        const unsigned n = first + i - 1;
        const unsigned w = n / 30;
        const unsigned b = n % 30;
        const std::uint32_t bit = static_cast<std::uint32_t>((value >> (count - 1U - i)) & 1U);
        const std::uint32_t mask = 1U << (23U - b);
        words[w] = bit != 0 ? (words[w] | mask) : (words[w] & ~mask);
    }
}

LnavWords lnav_header(unsigned id, std::uint32_t tow_count_next) {
    LnavWords w{};
    lnav_put(w, 1, 8, 0b10001011);
    lnav_put(w, 31, 17, tow_count_next);
    lnav_put(w, 50, 3, id);
    return w;
}

void lnav_put_clock(LnavWords& w, const decode::gnss::LnavClock& c) {
    lnav_put(w, 61, 10, c.week);
    lnav_put(w, 71, 2, c.l2_codes);
    lnav_put(w, 73, 4, c.ura_index);
    lnav_put(w, 77, 6, c.health);
    lnav_put(w, 83, 2, c.iodc >> 8U);
    lnav_put(w, 211, 8, c.iodc & 0xFFU);
    lnav_put(w, 91, 1, c.l2_p_data ? 1 : 0);
    lnav_put(w, 197, 8, enc(c.tgd_s, -31, 8));
    lnav_put(w, 219, 16, enc(c.toc_s, 4, 16));
    lnav_put(w, 241, 8, enc(c.af2, -55, 8));
    lnav_put(w, 249, 16, enc(c.af1, -43, 16));
    lnav_put(w, 271, 22, enc(c.af0, -31, 22));
}

void lnav_put_ephemeris(LnavWords& s2, LnavWords& s3, const decode::gnss::LnavEphemeris& e) {
    lnav_put(s2, 61, 8, e.iode);
    lnav_put(s2, 69, 16, enc(e.crs_m, -5, 16));
    lnav_put(s2, 91, 16, enc(e.delta_n_sc_s, -43, 16));
    const std::uint64_t m0 = enc(e.m0_sc, -31, 32);
    lnav_put(s2, 107, 8, m0 >> 24U);
    lnav_put(s2, 121, 24, m0);
    lnav_put(s2, 151, 16, enc(e.cuc_rad, -29, 16));
    const std::uint64_t ecc = enc(e.e, -33, 32);
    lnav_put(s2, 167, 8, ecc >> 24U);
    lnav_put(s2, 181, 24, ecc);
    lnav_put(s2, 211, 16, enc(e.cus_rad, -29, 16));
    const std::uint64_t sa = enc(e.sqrt_a, -19, 32);
    lnav_put(s2, 227, 8, sa >> 24U);
    lnav_put(s2, 241, 24, sa);
    lnav_put(s2, 271, 16, enc(e.toe_s, 4, 16));
    lnav_put(s2, 287, 1, e.fit_interval ? 1 : 0);
    lnav_put(s2, 288, 5, e.aodo);

    lnav_put(s3, 61, 16, enc(e.cic_rad, -29, 16));
    const std::uint64_t om0 = enc(e.omega0_sc, -31, 32);
    lnav_put(s3, 77, 8, om0 >> 24U);
    lnav_put(s3, 91, 24, om0);
    lnav_put(s3, 121, 16, enc(e.cis_rad, -29, 16));
    const std::uint64_t i0 = enc(e.i0_sc, -31, 32);
    lnav_put(s3, 137, 8, i0 >> 24U);
    lnav_put(s3, 151, 24, i0);
    lnav_put(s3, 181, 16, enc(e.crc_m, -5, 16));
    const std::uint64_t om = enc(e.omega_sc, -31, 32);
    lnav_put(s3, 197, 8, om >> 24U);
    lnav_put(s3, 211, 24, om);
    lnav_put(s3, 241, 24, enc(e.omega_dot_sc_s, -43, 24));
    lnav_put(s3, 271, 8, e.iode);
    lnav_put(s3, 279, 14, enc(e.idot_sc_s, -43, 14));
}

void lnav_put_almanac(LnavWords& w, const decode::gnss::LnavAlmanac& a) {
    lnav_put(w, 61, 2, 1);  // data ID 01, 20.3.3.5.1.1
    lnav_put(w, 63, 6, a.sv_id);
    lnav_put(w, 69, 16, enc(a.e, -21, 16));
    lnav_put(w, 91, 8, enc(a.toa_s, 12, 8));
    lnav_put(w, 99, 16, enc(a.delta_i_sc, -19, 16));
    lnav_put(w, 121, 16, enc(a.omega_dot_sc_s, -38, 16));
    lnav_put(w, 137, 8, a.health);
    lnav_put(w, 151, 24, enc(a.sqrt_a, -11, 24));
    lnav_put(w, 181, 24, enc(a.omega0_sc, -23, 24));
    lnav_put(w, 211, 24, enc(a.omega_sc, -23, 24));
    lnav_put(w, 241, 24, enc(a.m0_sc, -23, 24));
    const std::uint64_t af0 = enc(a.af0_s, -20, 11);
    lnav_put(w, 271, 8, af0 >> 3U);
    lnav_put(w, 290, 3, af0 & 7U);
    lnav_put(w, 279, 11, enc(a.af1, -38, 11));
}

void lnav_put_utc_iono(LnavWords& w, const decode::gnss::LnavUtcIono& x) {
    lnav_put(w, 61, 2, 1);
    lnav_put(w, 63, 6, 56);
    lnav_put(w, 69, 8, enc(x.alpha[0], -30, 8));
    lnav_put(w, 77, 8, enc(x.alpha[1], -27, 8));
    lnav_put(w, 91, 8, enc(x.alpha[2], -24, 8));
    lnav_put(w, 99, 8, enc(x.alpha[3], -24, 8));
    lnav_put(w, 107, 8, enc(x.beta[0], 11, 8));
    lnav_put(w, 121, 8, enc(x.beta[1], 14, 8));
    lnav_put(w, 129, 8, enc(x.beta[2], 16, 8));
    lnav_put(w, 137, 8, enc(x.beta[3], 16, 8));
    lnav_put(w, 151, 24, enc(x.a1, -50, 24));
    const std::uint64_t a0 = enc(x.a0_s, -30, 32);
    lnav_put(w, 181, 24, a0 >> 8U);
    lnav_put(w, 211, 8, a0 & 0xFFU);
    lnav_put(w, 219, 8, enc(x.tot_s, 12, 8));
    lnav_put(w, 227, 8, x.wnt);
    lnav_put(w, 241, 8, static_cast<std::uint64_t>(x.dt_ls) & 0xFFU);
    lnav_put(w, 249, 8, x.wn_lsf);
    lnav_put(w, 257, 8, x.dn);
    lnav_put(w, 271, 8, static_cast<std::uint64_t>(x.dt_lsf) & 0xFFU);
}

std::vector<std::uint8_t> lnav_encode(const LnavWords& words, bool& d29, bool& d30) {
    std::vector<std::uint8_t> out;
    out.reserve(300);
    for (std::size_t w = 0; w < 10; ++w) {
        std::uint32_t d = words[w] & 0xFFFFFFU;
        std::uint32_t parity = decode::gnss::lnav_parity(d, d29, d30);
        if (w == 1 || w == 9) {
            // Solve bits 23 and 24 for D29 = D30 = 0.
            for (std::uint32_t t = 0; t < 4; ++t) {
                const std::uint32_t cand = (d & ~3U) | t;
                const std::uint32_t p = decode::gnss::lnav_parity(cand, d29, d30);
                if ((p & 3U) == 0) {
                    d = cand;
                    parity = p;
                    break;
                }
            }
        }
        const std::uint32_t tx = d30 ? (d ^ 0xFFFFFFU) : d;
        const std::uint32_t word = (tx << 6U) | parity;
        for (int b = 29; b >= 0; --b) {
            out.push_back(static_cast<std::uint8_t>((word >> static_cast<unsigned>(b)) & 1U));
        }
        d29 = ((word >> 1U) & 1U) != 0;
        d30 = (word & 1U) != 0;
    }
    return out;
}

}  // namespace revenant::siggen
