#include "core/decode/dmr_codes.h"

#include <algorithm>
#include <bit>
#include <vector>

namespace revenant::decode {
namespace {

// The degree of the generator, which is the number of cyclic parity bits.
unsigned degree(std::uint32_t polynomial) {
    return static_cast<unsigned>(std::bit_width(polynomial)) - 1U;
}

// x^r * m(x) mod g(x), over GF(2).
std::uint32_t remainder(std::uint32_t message, unsigned r, std::uint32_t generator) {
    std::uint64_t value = static_cast<std::uint64_t>(message) << r;
    const int width = std::bit_width(generator);
    while (std::bit_width(value) >= width) {
        value ^= static_cast<std::uint64_t>(generator) << (std::bit_width(value) - width);
    }
    return static_cast<std::uint32_t>(value);
}

// Every code word of a code, indexed by its information.
std::vector<std::uint32_t> build_codebook(const DmrBlockCode& code) {
    std::vector<std::uint32_t> out(std::size_t{1} << code.k);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = dmr_block_encode(code, static_cast<std::uint32_t>(i));
    }
    return out;
}

// The table for one of the seven codes, built once. A table per code because
// the decoders search it on every word; each function-local static is
// initialised once and read-only after, which keeps the decoders pure
// functions of their arguments.
const std::vector<std::uint32_t>& codebook(const DmrBlockCode& code) {
    static const std::vector<std::uint32_t> golay20 = build_codebook(kDmrGolay20);
    static const std::vector<std::uint32_t> qr16 = build_codebook(kDmrQr16);
    static const std::vector<std::uint32_t> hamming17 = build_codebook(kDmrHamming17);
    static const std::vector<std::uint32_t> hamming13 = build_codebook(kDmrHamming13);
    static const std::vector<std::uint32_t> hamming15 = build_codebook(kDmrHamming15);
    static const std::vector<std::uint32_t> hamming16 = build_codebook(kDmrHamming16);
    static const std::vector<std::uint32_t> hamming7 = build_codebook(kDmrHamming7);
    static const std::vector<std::uint32_t> none;

    const auto same = [&](const DmrBlockCode& other) {
        return code.n == other.n && code.k == other.k && code.generator == other.generator &&
               code.extended == other.extended;
    };
    if (same(kDmrGolay20)) return golay20;
    if (same(kDmrQr16)) return qr16;
    if (same(kDmrHamming17)) return hamming17;
    if (same(kDmrHamming13)) return hamming13;
    if (same(kDmrHamming15)) return hamming15;
    if (same(kDmrHamming16)) return hamming16;
    if (same(kDmrHamming7)) return hamming7;
    return none;
}

// GF(2^8) tables from formula B.14, built once.
struct Gf256 {
    std::array<std::uint8_t, 255> exp{};
    std::array<int, 256> log{};
};

const Gf256& gf256() {
    static const Gf256 field = [] {
        Gf256 out;
        out.log.fill(-1);
        std::uint32_t value = 1;
        for (unsigned e = 0; e < 255; ++e) {
            out.exp[e] = static_cast<std::uint8_t>(value);
            out.log[value] = static_cast<int>(e);
            value <<= 1U;
            if ((value & 0x100U) != 0) {
                value ^= kDmrGf256Polynomial;
            }
        }
        return out;
    }();
    return field;
}

std::uint8_t gf_div(std::uint8_t a, std::uint8_t b) {
    if (a == 0 || b == 0) {
        return 0;
    }
    const Gf256& f = gf256();
    return f.exp[static_cast<unsigned>(f.log[a] - f.log[b] + 255) % 255U];
}

// Reads `count` bits one per byte, first most significant.
std::uint32_t pack(std::span<const std::uint8_t> bits) {
    std::uint32_t out = 0;
    for (const std::uint8_t bit : bits) {
        out = (out << 1U) | (bit & 1U);
    }
    return out;
}

void write_bits(std::uint32_t value, std::span<std::uint8_t> bits) {
    const std::size_t n = bits.size();
    for (std::size_t i = 0; i < n; ++i) {
        bits[i] = static_cast<std::uint8_t>((value >> (n - 1 - i)) & 1U);
    }
}

// Figure B.1's shape: 13 rows of 15, the first nine carrying data.
constexpr std::size_t kBptcRows = 13;
constexpr std::size_t kBptcColumns = 15;
constexpr std::size_t kBptcDataRows = 9;
constexpr std::size_t kBptcDataColumns = 11;

// Formula B.1. The matrix bits are numbered from 1 left to right and top to
// bottom behind R(3) at 0.
constexpr std::size_t bptc_position(std::size_t matrix_index) {
    return (matrix_index * 181U) % kDmrBptcBits;
}

}  // namespace

// ---------------------------------------------------------------------------
// Block codes
// ---------------------------------------------------------------------------

std::uint32_t dmr_block_encode(const DmrBlockCode& code, std::uint32_t information) {
    const std::uint32_t message = information & ((1U << code.k) - 1U);
    const unsigned r = degree(code.generator);
    std::uint32_t word = (message << r) | remainder(message, r, code.generator);
    if (code.extended) {
        word = (word << 1U) | (static_cast<std::uint32_t>(std::popcount(word)) & 1U);
    }
    return word;
}

DmrBlockDecode dmr_block_decode(const DmrBlockCode& code, std::uint32_t word) {
    const std::vector<std::uint32_t>& book = codebook(code);
    const std::uint32_t mask = (code.n >= 32) ? 0xFFFF'FFFFU : ((1U << code.n) - 1U);
    word &= mask;
    DmrBlockDecode best;
    best.distance = code.n + 1;
    for (std::uint32_t i = 0; i < book.size(); ++i) {
        const auto distance = static_cast<std::uint32_t>(std::popcount(book[i] ^ word));
        if (distance < best.distance) {
            best.distance = distance;
            best.information = i;
            best.codeword = book[i];
            if (distance == 0) {
                break;
            }
        }
    }
    best.detected = best.distance > (code.distance - 1) / 2;
    return best;
}

// ---------------------------------------------------------------------------
// Reed-Solomon (12,9)
// ---------------------------------------------------------------------------

std::uint8_t dmr_gf256_exp(unsigned e) { return gf256().exp[e % 255U]; }

int dmr_gf256_log(std::uint8_t value) { return gf256().log[value]; }

std::uint8_t dmr_gf256_mul(std::uint8_t a, std::uint8_t b) {
    if (a == 0 || b == 0) {
        return 0;
    }
    const Gf256& f = gf256();
    return f.exp[static_cast<unsigned>(f.log[a] + f.log[b]) % 255U];
}

std::array<std::uint8_t, 4> dmr_rs_generator() {
    // Multiply out (x + alpha)(x + alpha^2)(x + alpha^3); index is degree.
    std::array<std::uint8_t, 4> g{1, 0, 0, 0};
    for (unsigned root = 1; root <= 3; ++root) {
        const std::uint8_t a = dmr_gf256_exp(root);
        std::array<std::uint8_t, 4> next{};
        for (std::size_t d = 0; d < 4; ++d) {
            // x * g(x) + a * g(x)
            if (d > 0) {
                next[d] ^= g[d - 1];
            }
            next[d] ^= dmr_gf256_mul(a, g[d]);
        }
        g = next;
    }
    return g;
}

std::array<std::uint8_t, 3> dmr_rs_parity(std::span<const std::uint8_t, 9> message) {
    // Systematic division of m(x) * x^3 by g(x), highest degree first: the
    // shift register form of formula B.11's c = m * G.
    const std::array<std::uint8_t, 4> g = dmr_rs_generator();
    std::array<std::uint8_t, 3> r{};  // r[0] is the x^2 coefficient
    for (const std::uint8_t m : message) {
        const std::uint8_t feedback = static_cast<std::uint8_t>(m ^ r[0]);
        r[0] = static_cast<std::uint8_t>(r[1] ^ dmr_gf256_mul(feedback, g[2]));
        r[1] = static_cast<std::uint8_t>(r[2] ^ dmr_gf256_mul(feedback, g[1]));
        r[2] = dmr_gf256_mul(feedback, g[0]);
    }
    return r;
}

DmrRsDecode dmr_rs_decode(std::span<const std::uint8_t, 12> received) {
    DmrRsDecode out;
    std::copy(received.begin(), received.end(), out.codeword.begin());

    // S_j = c(alpha^j), with received[0] the coefficient of x^11.
    std::array<std::uint8_t, 3> s{};
    for (unsigned j = 1; j <= 3; ++j) {
        std::uint8_t sum = 0;
        const std::uint8_t a = dmr_gf256_exp(j);
        for (const std::uint8_t c : received) {
            sum = static_cast<std::uint8_t>(dmr_gf256_mul(sum, a) ^ c);
        }
        s[j - 1] = sum;
    }
    if (s[0] == 0 && s[1] == 0 && s[2] == 0) {
        out.decoded = true;
        return out;
    }
    // One error of value Y at degree e gives S_j = Y * alpha^(j*e), so
    // alpha^e = S2/S1 = S3/S2. Anything else is more than one error.
    if (s[0] == 0 || s[1] == 0) {
        return out;
    }
    const std::uint8_t locator = gf_div(s[1], s[0]);
    if (gf_div(s[2], s[1]) != locator) {
        return out;
    }
    const int e = dmr_gf256_log(locator);
    if (e < 0 || e > 11) {
        // A root in the 243 positions the shortening deleted: nobody sent it.
        return out;
    }
    const std::uint8_t value = gf_div(s[0], locator);
    out.codeword[static_cast<std::size_t>(11 - e)] ^= value;
    out.corrected = 1;
    out.decoded = true;
    return out;
}

// ---------------------------------------------------------------------------
// CRCs and the checksum
// ---------------------------------------------------------------------------

std::uint8_t dmr_crc8(std::span<const std::uint8_t> bits) {
    // G8 without its x^8 term.
    constexpr std::uint32_t kPoly = 0x07U;
    std::uint32_t crc = 0;
    for (const std::uint8_t bit : bits) {
        const std::uint32_t top = ((crc >> 7U) & 1U) ^ (bit & 1U);
        crc = (crc << 1U) & 0xFFU;
        if (top != 0) {
            crc ^= kPoly;
        }
    }
    return static_cast<std::uint8_t>(crc);
}

std::uint16_t dmr_crc_ccitt(std::span<const std::uint8_t> octets) {
    // GH without its x^16 term.
    constexpr std::uint32_t kPoly = 0x1021U;
    std::uint32_t crc = 0;
    for (const std::uint8_t octet : octets) {
        for (int bit = 7; bit >= 0; --bit) {
            const std::uint32_t top = ((crc >> 15U) & 1U) ^ ((octet >> static_cast<unsigned>(bit)) & 1U);
            crc = (crc << 1U) & 0xFFFFU;
            if (top != 0) {
                crc ^= kPoly;
            }
        }
    }
    // Formula B.20 adds IH(x), a one in every one of the sixteen places.
    return static_cast<std::uint16_t>(crc ^ 0xFFFFU);
}

std::uint8_t dmr_checksum5(std::span<const std::uint8_t, 9> lc) {
    unsigned sum = 0;
    for (const std::uint8_t octet : lc) {
        sum += octet;
    }
    return static_cast<std::uint8_t>(sum % 31U);
}

// ---------------------------------------------------------------------------
// BPTC (196,96)
// ---------------------------------------------------------------------------

std::array<std::uint8_t, kDmrBptcBits> dmr_bptc196_encode(
    std::span<const std::uint8_t, kDmrBptcInformationBits> information) {
    std::array<std::array<std::uint8_t, kBptcColumns>, kBptcRows> m{};

    // Figure B.1: R(2), R(1), R(0) and then I(95) onwards, eleven to a row.
    std::size_t next = 0;
    for (std::size_t r = 0; r < kBptcDataRows; ++r) {
        for (std::size_t c = (r == 0 ? 3 : 0); c < kBptcDataColumns; ++c) {
            m[r][c] = static_cast<std::uint8_t>(information[next++] & 1U);
        }
    }
    for (std::size_t r = 0; r < kBptcDataRows; ++r) {
        const std::uint32_t word =
            dmr_block_encode(kDmrHamming15, pack(std::span<const std::uint8_t>(m[r].data(), kBptcDataColumns)));
        write_bits(word, m[r]);
    }
    for (std::size_t c = 0; c < kBptcColumns; ++c) {
        std::uint32_t column = 0;
        for (std::size_t r = 0; r < kBptcDataRows; ++r) {
            column = (column << 1U) | m[r][c];
        }
        const std::uint32_t word = dmr_block_encode(kDmrHamming13, column);
        for (std::size_t r = 0; r < kBptcRows; ++r) {
            m[r][c] = static_cast<std::uint8_t>((word >> (kBptcRows - 1 - r)) & 1U);
        }
    }

    std::array<std::uint8_t, kDmrBptcBits> out{};
    for (std::size_t r = 0; r < kBptcRows; ++r) {
        for (std::size_t c = 0; c < kBptcColumns; ++c) {
            out[bptc_position(1 + r * kBptcColumns + c)] = m[r][c];
        }
    }
    return out;
}

DmrBptcDecode dmr_bptc196_decode(std::span<const std::uint8_t, kDmrBptcBits> received) {
    std::array<std::array<std::uint8_t, kBptcColumns>, kBptcRows> m{};
    for (std::size_t r = 0; r < kBptcRows; ++r) {
        for (std::size_t c = 0; c < kBptcColumns; ++c) {
            m[r][c] = static_cast<std::uint8_t>(received[bptc_position(1 + r * kBptcColumns + c)] & 1U);
        }
    }

    DmrBptcDecode out;
    bool clean = false;
    // Four passes is an engineering choice and not a measured optimum: each
    // pass of rows then columns can only clear errors the other direction
    // left isolated, so a pass that changes nothing ends it, and four bounds
    // the work on a burst that is noise.
    for (int pass = 0; pass < 4 && !clean; ++pass) {
        clean = true;
        std::uint32_t changed = 0;
        for (std::size_t r = 0; r < kBptcDataRows; ++r) {
            const std::uint32_t word = pack(m[r]);
            const DmrBlockDecode d = dmr_block_decode(kDmrHamming15, word);
            if (d.distance == 0) {
                continue;
            }
            clean = false;
            if (!d.detected) {
                write_bits(d.codeword, m[r]);
                changed += d.distance;
            }
        }
        for (std::size_t c = 0; c < kBptcColumns; ++c) {
            std::uint32_t word = 0;
            for (std::size_t r = 0; r < kBptcRows; ++r) {
                word = (word << 1U) | m[r][c];
            }
            const DmrBlockDecode d = dmr_block_decode(kDmrHamming13, word);
            if (d.distance == 0) {
                continue;
            }
            clean = false;
            if (!d.detected) {
                for (std::size_t r = 0; r < kBptcRows; ++r) {
                    m[r][c] = static_cast<std::uint8_t>((d.codeword >> (kBptcRows - 1 - r)) & 1U);
                }
                changed += d.distance;
            }
        }
        out.corrected += changed;
        if (changed == 0) {
            break;
        }
    }

    // Clean means every row and every column is a code word now, whether or
    // not the loop above got there on its last pass.
    clean = true;
    for (std::size_t r = 0; r < kBptcDataRows && clean; ++r) {
        clean = dmr_block_decode(kDmrHamming15, pack(m[r])).distance == 0;
    }
    for (std::size_t c = 0; c < kBptcColumns && clean; ++c) {
        std::uint32_t word = 0;
        for (std::size_t r = 0; r < kBptcRows; ++r) {
            word = (word << 1U) | m[r][c];
        }
        clean = dmr_block_decode(kDmrHamming13, word).distance == 0;
    }
    out.clean = clean;

    std::size_t next = 0;
    for (std::size_t r = 0; r < kBptcDataRows; ++r) {
        for (std::size_t c = (r == 0 ? 3 : 0); c < kBptcDataColumns; ++c) {
            out.information[next++] = m[r][c];
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Embedded signalling
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kEmbeddedRows = 8;
constexpr std::size_t kEmbeddedColumns = 16;

// Figure B.3's data: LC(71) down to LC(0) eleven to a row for two rows and
// ten to a row after, with CS(4) to CS(0) in column 10 of rows 2 to 6.
using EmbeddedMatrix = std::array<std::array<std::uint8_t, kEmbeddedColumns>, kEmbeddedRows>;

}  // namespace

std::array<std::uint8_t, kDmrEmbeddedBits> dmr_embedded_encode(std::span<const std::uint8_t, 9> lc) {
    EmbeddedMatrix m{};
    std::array<std::uint8_t, 72> bits{};
    for (std::size_t i = 0; i < 72; ++i) {
        bits[i] = static_cast<std::uint8_t>((lc[i / 8] >> (7U - i % 8)) & 1U);
    }
    const std::uint8_t cs = dmr_checksum5(lc);

    std::size_t next = 0;
    for (std::size_t r = 0; r < 7; ++r) {
        const std::size_t data = (r < 2) ? 11 : 10;
        for (std::size_t c = 0; c < data; ++c) {
            m[r][c] = bits[next++];
        }
        if (r >= 2) {
            m[r][10] = static_cast<std::uint8_t>((cs >> (6U - r)) & 1U);
        }
        const std::uint32_t word = dmr_block_encode(kDmrHamming16, pack(std::span<const std::uint8_t>(m[r].data(), 11)));
        write_bits(word, m[r]);
    }
    // B.2.1: each column's parity check makes its count of ones even.
    for (std::size_t c = 0; c < kEmbeddedColumns; ++c) {
        std::uint8_t parity = 0;
        for (std::size_t r = 0; r < 7; ++r) {
            parity ^= m[r][c];
        }
        m[7][c] = parity;
    }

    std::array<std::uint8_t, kDmrEmbeddedBits> out{};
    for (std::size_t c = 0; c < kEmbeddedColumns; ++c) {
        for (std::size_t r = 0; r < kEmbeddedRows; ++r) {
            out[c * kEmbeddedRows + r] = m[r][c];
        }
    }
    return out;
}

DmrEmbeddedDecode dmr_embedded_decode(std::span<const std::uint8_t, kDmrEmbeddedBits> received) {
    EmbeddedMatrix m{};
    for (std::size_t c = 0; c < kEmbeddedColumns; ++c) {
        for (std::size_t r = 0; r < kEmbeddedRows; ++r) {
            m[r][c] = static_cast<std::uint8_t>(received[c * kEmbeddedRows + r] & 1U);
        }
    }

    DmrEmbeddedDecode out;
    bool rows_clean = true;
    for (std::size_t r = 0; r < 7; ++r) {
        const DmrBlockDecode d = dmr_block_decode(kDmrHamming16, pack(m[r]));
        if (d.detected) {
            rows_clean = false;
            continue;
        }
        if (d.distance != 0) {
            write_bits(d.codeword, m[r]);
            out.corrected += d.distance;
        }
    }
    bool columns_clean = true;
    for (std::size_t c = 0; c < kEmbeddedColumns; ++c) {
        std::uint8_t parity = 0;
        for (std::size_t r = 0; r < kEmbeddedRows; ++r) {
            parity ^= m[r][c];
        }
        columns_clean = columns_clean && parity == 0;
    }
    out.clean = rows_clean && columns_clean;

    std::array<std::uint8_t, 72> bits{};
    std::size_t next = 0;
    std::uint8_t cs = 0;
    for (std::size_t r = 0; r < 7; ++r) {
        const std::size_t data = (r < 2) ? 11 : 10;
        for (std::size_t c = 0; c < data; ++c) {
            bits[next++] = m[r][c];
        }
        if (r >= 2) {
            cs = static_cast<std::uint8_t>((cs << 1U) | m[r][10]);
        }
    }
    for (std::size_t i = 0; i < 72; ++i) {
        out.lc[i / 8] = static_cast<std::uint8_t>((out.lc[i / 8] << 1U) | bits[i]);
    }
    out.checksum_valid = cs == dmr_checksum5(out.lc);
    return out;
}

// ---------------------------------------------------------------------------
// The CACH
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kShortLcRows = 4;
constexpr std::size_t kShortLcColumns = 17;

}  // namespace

std::array<std::uint8_t, kDmrShortLcBits> dmr_short_lc_encode(std::span<const std::uint8_t, 28> lc) {
    // Figure B.6: LC(27) to LC(0), then CR(7) to CR(0), twelve to a row.
    std::array<std::uint8_t, 36> info{};
    std::copy(lc.begin(), lc.end(), info.begin());
    const std::uint8_t crc = dmr_crc8(lc);
    for (std::size_t i = 0; i < 8; ++i) {
        info[28 + i] = static_cast<std::uint8_t>((crc >> (7U - i)) & 1U);
    }

    std::array<std::array<std::uint8_t, kShortLcColumns>, kShortLcRows> m{};
    for (std::size_t r = 0; r < 3; ++r) {
        const std::uint32_t word =
            dmr_block_encode(kDmrHamming17, pack(std::span<const std::uint8_t>(info.data() + r * 12, 12)));
        write_bits(word, m[r]);
    }
    for (std::size_t c = 0; c < kShortLcColumns; ++c) {
        m[3][c] = static_cast<std::uint8_t>(m[0][c] ^ m[1][c] ^ m[2][c]);
    }

    std::array<std::uint8_t, kDmrShortLcBits> out{};
    for (std::size_t c = 0; c < kShortLcColumns; ++c) {
        for (std::size_t r = 0; r < kShortLcRows; ++r) {
            out[c * kShortLcRows + r] = m[r][c];
        }
    }
    return out;
}

DmrShortLcDecode dmr_short_lc_decode(std::span<const std::uint8_t, kDmrShortLcBits> received) {
    std::array<std::array<std::uint8_t, kShortLcColumns>, kShortLcRows> m{};
    for (std::size_t c = 0; c < kShortLcColumns; ++c) {
        for (std::size_t r = 0; r < kShortLcRows; ++r) {
            m[r][c] = static_cast<std::uint8_t>(received[c * kShortLcRows + r] & 1U);
        }
    }

    DmrShortLcDecode out;
    bool rows_clean = true;
    for (std::size_t r = 0; r < 3; ++r) {
        const DmrBlockDecode d = dmr_block_decode(kDmrHamming17, pack(m[r]));
        if (d.detected) {
            rows_clean = false;
            continue;
        }
        if (d.distance != 0) {
            write_bits(d.codeword, m[r]);
            out.corrected += d.distance;
        }
    }
    bool columns_clean = true;
    for (std::size_t c = 0; c < kShortLcColumns; ++c) {
        columns_clean = columns_clean && ((m[0][c] ^ m[1][c] ^ m[2][c] ^ m[3][c]) == 0);
    }
    out.clean = rows_clean && columns_clean;

    std::array<std::uint8_t, 36> info{};
    for (std::size_t r = 0; r < 3; ++r) {
        std::copy_n(m[r].begin(), 12, info.begin() + static_cast<std::ptrdiff_t>(r * 12));
    }
    std::copy_n(info.begin(), 28, out.lc.begin());
    std::uint8_t crc = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        crc = static_cast<std::uint8_t>((crc << 1U) | info[28 + i]);
    }
    out.crc_valid = crc == dmr_crc8(out.lc);
    return out;
}

std::array<std::uint8_t, kDmrCachBits> dmr_cach_interleave(const DmrCachBits& bits) {
    std::array<std::uint8_t, kDmrCachBits> out{};
    std::size_t tact = 0;
    std::size_t payload = 0;
    for (std::size_t i = 0; i < kDmrCachBits; ++i) {
        if (tact < kDmrCachTactPositions.size() && kDmrCachTactPositions[tact] == i) {
            out[i] = static_cast<std::uint8_t>((bits.tact >> (6U - tact)) & 1U);
            ++tact;
        } else {
            out[i] = static_cast<std::uint8_t>(bits.payload[payload++] & 1U);
        }
    }
    return out;
}

DmrCachBits dmr_cach_deinterleave(std::span<const std::uint8_t, kDmrCachBits> received) {
    DmrCachBits out;
    std::size_t tact = 0;
    std::size_t payload = 0;
    for (std::size_t i = 0; i < kDmrCachBits; ++i) {
        if (tact < kDmrCachTactPositions.size() && kDmrCachTactPositions[tact] == i) {
            out.tact = (out.tact << 1U) | (received[i] & 1U);
            ++tact;
        } else {
            out.payload[payload++] = static_cast<std::uint8_t>(received[i] & 1U);
        }
    }
    return out;
}

}  // namespace revenant::decode
