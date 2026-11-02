#include "core/decode/dvbs2.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "core/decode/dvbs.h"
#include "core/decode/dvbs2_tables.h"

namespace revenant::decode {

namespace {

using dsp::Complex32;

// Table 6a, g1 to g12, as bit masks of x^0 .. x^16.
constexpr std::array<std::uint32_t, 12> kBchPolys = {
    (1U << 0) | (1U << 2) | (1U << 3) | (1U << 5) | (1U << 16),
    (1U << 0) | (1U << 1) | (1U << 4) | (1U << 5) | (1U << 6) | (1U << 8) | (1U << 16),
    (1U << 0) | (1U << 2) | (1U << 3) | (1U << 4) | (1U << 5) | (1U << 7) | (1U << 8) | (1U << 9) | (1U << 10) |
        (1U << 11) | (1U << 16),
    (1U << 0) | (1U << 2) | (1U << 4) | (1U << 6) | (1U << 9) | (1U << 11) | (1U << 12) | (1U << 14) | (1U << 16),
    (1U << 0) | (1U << 1) | (1U << 2) | (1U << 3) | (1U << 5) | (1U << 8) | (1U << 9) | (1U << 10) | (1U << 11) |
        (1U << 12) | (1U << 16),
    (1U << 0) | (1U << 2) | (1U << 4) | (1U << 5) | (1U << 7) | (1U << 8) | (1U << 9) | (1U << 10) | (1U << 12) |
        (1U << 13) | (1U << 14) | (1U << 15) | (1U << 16),
    (1U << 0) | (1U << 2) | (1U << 5) | (1U << 6) | (1U << 8) | (1U << 9) | (1U << 10) | (1U << 11) | (1U << 13) |
        (1U << 15) | (1U << 16),
    (1U << 0) | (1U << 1) | (1U << 2) | (1U << 5) | (1U << 6) | (1U << 8) | (1U << 9) | (1U << 12) | (1U << 13) |
        (1U << 14) | (1U << 16),
    (1U << 0) | (1U << 5) | (1U << 7) | (1U << 9) | (1U << 10) | (1U << 11) | (1U << 16),
    (1U << 0) | (1U << 1) | (1U << 2) | (1U << 5) | (1U << 7) | (1U << 8) | (1U << 10) | (1U << 12) | (1U << 13) |
        (1U << 14) | (1U << 16),
    (1U << 0) | (1U << 2) | (1U << 3) | (1U << 5) | (1U << 9) | (1U << 11) | (1U << 12) | (1U << 13) | (1U << 16),
    (1U << 0) | (1U << 1) | (1U << 5) | (1U << 6) | (1U << 7) | (1U << 9) | (1U << 11) | (1U << 12) | (1U << 16),
};

// 5.5.2.4 figure 13b, rows 1 to 6, the leftmost column the most significant
// of 32 bits.
constexpr std::array<std::uint32_t, 6> kPlsRows = {
    0x55555555U, 0x33333333U, 0x0F0F0F0FU, 0x00FF00FFU, 0x0000FFFFU, 0xFFFFFFFFU,
};
// 5.5.2.4: the PLS scrambling sequence, the first bit the most significant.
constexpr std::uint64_t kPlsScramble = 0b0111000110011101100000111100100101010011010000100010110111111010ULL;

// Figure 9: label (2 bits) to grid index; figure 10: label (3 bits) to grid
// index, angle k * pi/4.
constexpr std::array<std::uint8_t, 4> kQpskGrid = {1, 7, 3, 5};
constexpr std::array<std::uint8_t, 8> kPsk8Grid = {1, 0, 4, 5, 2, 7, 3, 6};

Complex32 grid_point(unsigned k) { return psk_point(PskModulation::Psk8, k & 7U); }

std::array<std::uint8_t, 64> pls_bits(std::uint8_t modcod, std::uint8_t type) {
    const unsigned b = (static_cast<unsigned>(modcod & 0x1FU) << 2U) | (type & 3U);
    std::uint32_t y = 0;
    for (unsigned r = 0; r < 6; ++r) {
        if (((b >> (6U - r)) & 1U) != 0U) {
            y ^= kPlsRows[r];
        }
    }
    const unsigned b7 = b & 1U;
    std::array<std::uint8_t, 64> out{};
    for (unsigned i = 0; i < 32; ++i) {
        const unsigned yi = (y >> (31U - i)) & 1U;
        out[2 * i] = static_cast<std::uint8_t>(yi);
        out[2 * i + 1] = static_cast<std::uint8_t>(yi ^ b7);
    }
    for (unsigned i = 0; i < 64; ++i) {
        out[i] = static_cast<std::uint8_t>(out[i] ^ ((kPlsScramble >> (63U - i)) & 1U));
    }
    return out;
}

// 5.5.2: pi/2-BPSK. Zero-based even positions are the one-based odd ones.
std::uint8_t header_index(std::size_t i, unsigned bit) {
    if (i % 2 == 0) {
        return bit == 0U ? 1 : 5;
    }
    return bit == 0U ? 3 : 7;
}

struct HeaderTables {
    std::array<Complex32, kDvbs2SofSymbols> sof{};
    // 128 code words, (modcod << 2) | type, conjugated PLS symbols.
    std::vector<std::array<Complex32, 64>> pls;
};

const HeaderTables& header_tables() {
    static const HeaderTables t = [] {
        HeaderTables h;
        for (std::size_t i = 0; i < kDvbs2SofSymbols; ++i) {
            const unsigned bit = (kDvbs2Sof >> (25U - i)) & 1U;
            h.sof[i] = std::conj(grid_point(header_index(i, bit)));
        }
        h.pls.resize(128);
        for (unsigned w = 0; w < 128; ++w) {
            const auto bits = pls_bits(static_cast<std::uint8_t>(w >> 2U), static_cast<std::uint8_t>(w & 3U));
            for (std::size_t i = 0; i < 64; ++i) {
                h.pls[w][i] = std::conj(grid_point(header_index(kDvbs2SofSymbols + i, bits[i])));
            }
        }
        return h;
    }();
    return t;
}

float mean_magnitude(std::span<const Complex32> s) {
    double sum = 0.0;
    for (const auto& z : s) {
        sum += std::abs(z);
    }
    return s.empty() ? 0.0F : static_cast<float>(sum / static_cast<double>(s.size()));
}

}  // namespace

std::optional<Dvbs2Code> dvbs2_code(std::uint8_t modcod) {
    // Tables 5a, 6a and 7a.
    switch (modcod) {
        case kDvbs2ModcodQpsk910:
            return Dvbs2Code{modcod, PskModulation::Qpsk, 58192, 58320, 8, 18};
        case kDvbs2Modcod8psk23:
            return Dvbs2Code{modcod, PskModulation::Psk8, 43040, 43200, 10, 60};
        default:
            return std::nullopt;
    }
}

unsigned dvbs2_modcod_bits(std::uint8_t modcod) {
    // Table 12.
    if (modcod >= 1 && modcod <= 11) {
        return 2;
    }
    if (modcod >= 12 && modcod <= 17) {
        return 3;
    }
    if (modcod >= 18 && modcod <= 23) {
        return 4;
    }
    if (modcod >= 24 && modcod <= 28) {
        return 5;
    }
    return 0;
}

std::size_t dvbs2_plframe_symbols(std::uint8_t modcod, std::uint8_t type) {
    if (modcod == kDvbs2ModcodDummy) {
        return kDvbs2SlotSymbols * 37;  // 5.5.1
    }
    const unsigned bits = dvbs2_modcod_bits(modcod);
    if (bits == 0) {
        return 0;
    }
    const std::size_t n = (type & 2U) != 0U ? 16200 : 64800;
    const std::size_t slots = n / bits / kDvbs2SlotSymbols;
    std::size_t len = kDvbs2SlotSymbols * (slots + 1);
    if ((type & 1U) != 0U) {
        len += kDvbs2PilotSymbols * ((slots - 1) / 16);
    }
    return len;
}

std::uint8_t dvbs2_grid_index(PskModulation modulation, unsigned label) {
    return modulation == PskModulation::Psk8 ? kPsk8Grid[label & 7U] : kQpskGrid[label & 3U];
}

std::array<std::uint8_t, kDvbs2SlotSymbols> dvbs2_plheader(std::uint8_t modcod, std::uint8_t type) {
    std::array<std::uint8_t, kDvbs2SlotSymbols> out{};
    for (std::size_t i = 0; i < kDvbs2SofSymbols; ++i) {
        out[i] = header_index(i, (kDvbs2Sof >> (25U - i)) & 1U);
    }
    const auto bits = pls_bits(modcod, type);
    for (std::size_t i = 0; i < 64; ++i) {
        out[kDvbs2SofSymbols + i] = header_index(kDvbs2SofSymbols + i, bits[i]);
    }
    return out;
}

Dvbs2Header dvbs2_sof(std::span<const Complex32> s) {
    Dvbs2Header h;
    if (s.size() < kDvbs2SofSymbols) {
        return h;
    }
    const auto& t = header_tables();
    Complex32 c{};
    for (std::size_t i = 0; i < kDvbs2SofSymbols; ++i) {
        c += s[i] * t.sof[i];
    }
    const float mag = mean_magnitude(s.first(kDvbs2SofSymbols));
    const float a = std::abs(c);
    h.metric = mag > 0.0F ? a / (static_cast<float>(kDvbs2SofSymbols) * mag) : 0.0F;
    h.phase = a > 0.0F ? c / a : Complex32{1.0F, 0.0F};
    return h;
}

Dvbs2Header dvbs2_decode_plheader(std::span<const Complex32> s) {
    Dvbs2Header best;
    if (s.size() < kDvbs2SlotSymbols) {
        return best;
    }
    const auto& t = header_tables();
    Complex32 sof{};
    for (std::size_t i = 0; i < kDvbs2SofSymbols; ++i) {
        sof += s[i] * t.sof[i];
    }
    const float mag = mean_magnitude(s.first(kDvbs2SlotSymbols));
    float best_abs = -1.0F;
    unsigned best_word = 0;
    Complex32 best_c{};
    for (unsigned w = 0; w < 128; ++w) {
        Complex32 c = sof;
        for (std::size_t i = 0; i < 64; ++i) {
            c += s[kDvbs2SofSymbols + i] * t.pls[w][i];
        }
        const float a = std::abs(c);
        if (a > best_abs) {
            best_abs = a;
            best_word = w;
            best_c = c;
        }
    }
    best.modcod = static_cast<std::uint8_t>(best_word >> 2U);
    best.type = static_cast<std::uint8_t>(best_word & 3U);
    best.metric = mag > 0.0F ? best_abs / (static_cast<float>(kDvbs2SlotSymbols) * mag) : 0.0F;
    best.phase = best_abs > 0.0F ? best_c / best_abs : Complex32{1.0F, 0.0F};
    return best;
}

std::vector<std::uint8_t> dvbs2_pl_scrambling(std::uint32_t gold_code, std::size_t length) {
    // 5.5.4: two degree-18 m-sequences over their whole period.
    constexpr std::size_t kPeriod = (std::size_t{1} << 18) - 1;
    std::vector<std::uint8_t> x(kPeriod + 18, 0);
    std::vector<std::uint8_t> y(kPeriod + 18, 0);
    x[0] = 1;
    for (std::size_t i = 0; i < 18; ++i) {
        y[i] = 1;
    }
    for (std::size_t i = 0; i + 18 < x.size(); ++i) {
        x[i + 18] = static_cast<std::uint8_t>(x[i + 7] ^ x[i]);
        y[i + 18] = static_cast<std::uint8_t>(y[i + 10] ^ y[i + 7] ^ y[i + 5] ^ y[i]);
    }
    const auto z = [&](std::size_t i) {
        return static_cast<unsigned>(x[(i + gold_code) % kPeriod] ^ y[i % kPeriod]);
    };
    std::vector<std::uint8_t> r(length);
    for (std::size_t i = 0; i < length; ++i) {
        r[i] = static_cast<std::uint8_t>(2U * z((i + 131072) % kPeriod) + z(i));
    }
    return r;
}

std::uint8_t dvbs2_crc8(std::span<const std::uint8_t> octets) {
    // Figure 2, MSB first, register zeroed.
    std::uint8_t crc = 0;
    for (const std::uint8_t o : octets) {
        for (int b = 7; b >= 0; --b) {
            const unsigned in = (static_cast<unsigned>(o) >> static_cast<unsigned>(b)) & 1U;
            const unsigned fb = in ^ (static_cast<unsigned>(crc) >> 7U);
            crc = static_cast<std::uint8_t>(crc << 1U);
            if (fb != 0U) {
                crc ^= 0xD5U;  // X^7 + X^6 + X^4 + X^2 + 1, the X^8 term implied
            }
        }
    }
    return crc;
}

void dvbs2_bb_scramble(std::span<std::uint8_t> bits) {
    DvbsPrbs prbs;
    for (auto& b : bits) {
        b = static_cast<std::uint8_t>(b ^ prbs.next_bit());
    }
}

void dvbs2_write_bbheader(const Dvbs2BbHeader& h, std::span<std::uint8_t> bits) {
    std::array<std::uint8_t, 10> o{};
    o[0] = h.matype1;
    o[1] = h.matype2;
    o[2] = static_cast<std::uint8_t>(h.upl >> 8U);
    o[3] = static_cast<std::uint8_t>(h.upl & 0xFFU);
    o[4] = static_cast<std::uint8_t>(h.dfl >> 8U);
    o[5] = static_cast<std::uint8_t>(h.dfl & 0xFFU);
    o[6] = h.sync;
    o[7] = static_cast<std::uint8_t>(h.syncd >> 8U);
    o[8] = static_cast<std::uint8_t>(h.syncd & 0xFFU);
    o[9] = dvbs2_crc8(std::span(o).first(9));
    for (std::size_t i = 0; i < kDvbs2BbHeaderBits; ++i) {
        bits[i] = static_cast<std::uint8_t>((o[i / 8] >> (7U - i % 8)) & 1U);
    }
}

std::optional<Dvbs2BbHeader> dvbs2_parse_bbheader(std::span<const std::uint8_t> bits) {
    if (bits.size() < kDvbs2BbHeaderBits) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 10> o{};
    for (std::size_t i = 0; i < kDvbs2BbHeaderBits; ++i) {
        o[i / 8] = static_cast<std::uint8_t>(o[i / 8] | ((bits[i] & 1U) << (7U - i % 8)));
    }
    if (dvbs2_crc8(std::span(o).first(9)) != o[9]) {
        return std::nullopt;
    }
    Dvbs2BbHeader h;
    h.matype1 = o[0];
    h.matype2 = o[1];
    h.upl = static_cast<std::uint16_t>((o[2] << 8U) | o[3]);
    h.dfl = static_cast<std::uint16_t>((o[4] << 8U) | o[5]);
    h.sync = o[6];
    h.syncd = static_cast<std::uint16_t>((o[7] << 8U) | o[8]);
    return h;
}

void dvbs2_interleave_8psk(std::span<const std::uint8_t> in, std::span<std::uint8_t> out) {
    // Table 8: 21 600 rows, 3 columns; written column by column, read row by row.
    constexpr std::size_t kRows = kDvbs2NormalFrameBits / 3;
    for (std::size_t r = 0; r < kRows; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            out[3 * r + c] = in[c * kRows + r];
        }
    }
}

void dvbs2_deinterleave_8psk(std::span<const float> in, std::span<float> out) {
    constexpr std::size_t kRows = kDvbs2NormalFrameBits / 3;
    for (std::size_t r = 0; r < kRows; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            out[c * kRows + r] = in[3 * r + c];
        }
    }
}

Expected<Dvbs2Fec> Dvbs2Fec::create(std::uint8_t modcod, std::size_t max_iterations) {
    const auto code = dvbs2_code(modcod);
    if (!code) {
        return fail("only DVB-S2 normal frames at QPSK 9/10 and 8PSK 2/3 are implemented");
    }
    Dvbs2Fec f;
    f.code_ = *code;
    f.table_ = modcod == kDvbs2ModcodQpsk910 ? dvbs2_tables::table_b11() : dvbs2_tables::table_b6();

    // 5.3.1: g = g1 * ... * gt over GF(2), then highest power first.
    std::vector<std::uint8_t> g{1};  // lowest power first while multiplying
    for (unsigned i = 0; i < code->t; ++i) {
        std::vector<std::uint8_t> next(g.size() + 16, 0);
        for (std::size_t a = 0; a < g.size(); ++a) {
            if (g[a] == 0U) {
                continue;
            }
            for (unsigned b = 0; b <= 16; ++b) {
                if (((kBchPolys[i] >> b) & 1U) != 0U) {
                    next[a + b] ^= 1U;
                }
            }
        }
        g = std::move(next);
    }
    BchParams bp;
    bp.m = 16;
    bp.field_polynomial = kBchPolys[0];
    bp.generator.assign(g.rbegin(), g.rend());
    auto bch = BchCodec::create(bp);
    if (!bch) {
        return std::unexpected(bch.error());
    }
    if (bch->check_bits() != code->nbch - code->kbch || bch->correctable() < code->t) {
        return fail("the DVB-S2 BCH generator came out with the wrong degree or strength");
    }
    f.bch_ = std::move(*bch);

    // 5.3.2.1: parity check j holds every information bit accumulated at
    // address j, p_j, and p_(j-1).
    const std::size_t k = code->nbch;
    const std::size_t m = kDvbs2NormalFrameBits - k;
    LdpcConfig lc;
    lc.variables = kDvbs2NormalFrameBits;
    lc.max_iterations = max_iterations;
    lc.checks.resize(m);
    std::size_t group = 0;
    for (std::size_t pos = 0; pos < f.table_.size(); ++group) {
        const std::size_t count = f.table_[pos];
        const auto row = f.table_.subspan(pos + 1, count);
        pos += count + 1;
        for (std::size_t j = 0; j < 360; ++j) {
            const auto info = static_cast<std::uint32_t>(group * 360 + j);
            for (const std::uint16_t x : row) {
                lc.checks[(x + j * code->q) % m].push_back(info);
            }
        }
    }
    if (group * 360 != k) {
        return fail("the DVB-S2 LDPC table does not cover the information bits");
    }
    for (std::size_t j = 0; j < m; ++j) {
        lc.checks[j].push_back(static_cast<std::uint32_t>(k + j));
        if (j > 0) {
            lc.checks[j].push_back(static_cast<std::uint32_t>(k + j - 1));
        }
    }
    // Check 0 has p_0 and its information bits; each table row has at least
    // three entries, so no check is left with fewer than two variables.
    auto ldpc = LdpcDecoder::create(std::move(lc));
    if (!ldpc) {
        return std::unexpected(ldpc.error());
    }
    f.ldpc_ = std::move(*ldpc);
    f.bits_.resize(kDvbs2NormalFrameBits);
    return f;
}

void Dvbs2Fec::encode(std::span<const std::uint8_t> bbframe, std::span<std::uint8_t> out) const {
    const std::size_t kbch = code_.kbch;
    const std::size_t k = code_.nbch;
    const std::size_t m = kDvbs2NormalFrameBits - k;
    std::copy_n(bbframe.begin(), kbch, out.begin());
    (void)bch_->encode(bbframe.first(kbch), out.subspan(kbch, k - kbch));
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(k), out.end(), std::uint8_t{0});
    auto parity = out.subspan(k, m);
    std::size_t group = 0;
    for (std::size_t pos = 0; pos < table_.size(); ++group) {
        const std::size_t count = table_[pos];
        const auto row = table_.subspan(pos + 1, count);
        pos += count + 1;
        for (std::size_t j = 0; j < 360; ++j) {
            if (out[group * 360 + j] == 0U) {
                continue;
            }
            for (const std::uint16_t x : row) {
                parity[(x + j * code_.q) % m] ^= 1U;
            }
        }
    }
    for (std::size_t i = 1; i < m; ++i) {
        parity[i] ^= parity[i - 1];
    }
}

Dvbs2DecodeResult Dvbs2Fec::decode(std::span<const float> llr, std::span<std::uint8_t> bbframe) {
    Dvbs2DecodeResult r;
    const auto lr = ldpc_->decode(llr.first(kDvbs2NormalFrameBits), bits_);
    r.ldpc_iterations = lr.iterations;
    r.ldpc_converged = lr.converged;
    const auto fixed = bch_->decode(std::span(bits_).first(code_.nbch));
    if (!fixed) {
        return r;
    }
    r.bch_corrected = *fixed;
    r.ok = true;
    std::copy_n(bits_.begin(), code_.kbch, bbframe.begin());
    return r;
}

void dvbs2_demap(PskModulation modulation, std::span<const Complex32> symbols, float n0, std::span<float> llr) {
    const float inv = 1.0F / std::max(n0, 1e-6F);
    if (modulation != PskModulation::Psk8) {
        // Figure 9: a zero bit is a positive rail, MSB on I.
        const float s = 2.0F * std::numbers::sqrt2_v<float> * inv;
        for (std::size_t i = 0; i < symbols.size(); ++i) {
            llr[2 * i] = s * symbols[i].real();
            llr[2 * i + 1] = s * symbols[i].imag();
        }
        return;
    }
    std::array<Complex32, 8> points{};
    for (unsigned label = 0; label < 8; ++label) {
        points[label] = grid_point(kPsk8Grid[label]);
    }
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        std::array<float, 8> d{};
        for (unsigned label = 0; label < 8; ++label) {
            d[label] = std::norm(symbols[i] - points[label]);
        }
        for (unsigned b = 0; b < 3; ++b) {
            float d0 = std::numeric_limits<float>::max();
            float d1 = d0;
            for (unsigned label = 0; label < 8; ++label) {
                if (((label >> (2U - b)) & 1U) != 0U) {
                    d1 = std::min(d1, d[label]);
                } else {
                    d0 = std::min(d0, d[label]);
                }
            }
            llr[3 * i + b] = (d1 - d0) * inv;
        }
    }
}

}  // namespace revenant::decode
