// FS-1016 CELP. The specification, and every place it was silent, is in the
// header.

#include "core/decode/fs1016.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <type_traits>

namespace revenant::decode {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr std::size_t kSub = kFs1016SubframeSamples;
constexpr std::size_t kReg = kFs1016AdaptiveRegister;

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------

// 3.4. Index 10 is printed "98"; see header note 1.
constexpr std::array<double, 32> kStochasticGain = {
    -1330, -870, -660, -520, -418, -340, -278, -224, -178, -136, -98,
    -64,   -35,  -13,  -3,   -1,   1,    3,    13,   35,   64,   98,
    136,   178,  224,  278,  340,  418,  520,  660,  870,  1330};

// 3.6.
constexpr std::array<double, 32> kAdaptiveGain = {
    -0.993, -0.831, -0.693, -0.555, -0.414, -0.229, 0.000, 0.139,
    0.255,  0.368,  0.457,  0.531,  0.601,  0.653,  0.702, 0.745,
    0.780,  0.816,  0.850,  0.881,  0.915,  0.948,  0.983, 1.020,
    1.062,  1.117,  1.193,  1.289,  1.394,  1.540,  1.765, 1.991};

// 3.5.4, in ascending delay order: 20.00 to 25.67 in thirds, 26.00 to 33.75
// in quarters, 34.00 to 79.67 in thirds, 80 to 147 in whole samples.
constexpr std::array<std::uint8_t, 256> kAdaptiveHex = {
    // 20.00 .. 25.67
    0x42, 0x46, 0x47, 0x57, 0x56, 0x59, 0x58, 0xAE, 0xBE, 0xBA, 0xB8, 0xBC,
    0xAC, 0xA8, 0x94, 0x84, 0x8C, 0x9C,
    // 26.00 .. 33.75
    0x9E, 0x8E, 0x86, 0x96, 0x0A, 0x02, 0x0B, 0x03, 0x1B, 0x13, 0x09, 0x01,
    0x19, 0x11, 0xF3, 0xF7, 0xE7, 0xE3, 0xE5, 0xE1, 0xF1, 0xF5, 0x61, 0x65,
    0x75, 0x71, 0x6D, 0x7D, 0x69, 0x79, 0x7B, 0x7F,
    // 34.00 .. 79.67
    0x6B, 0xC1, 0xC0, 0xC3, 0xC2, 0xD2, 0xD3, 0xD1, 0xD0, 0x30, 0x32, 0x3A,
    0x31, 0x33, 0x3B, 0x3F, 0x37, 0x3E, 0x36, 0x34, 0x4A, 0x4B, 0x4E, 0x4F,
    0x5F, 0x5E, 0x5C, 0x5D, 0x54, 0x55, 0x50, 0x51, 0xAA, 0xA6, 0xA2, 0xB6,
    0xB2, 0xBB, 0xB0, 0xB9, 0xB4, 0xBD, 0xA4, 0xA0, 0xA9, 0xAD, 0x95, 0x85,
    0x9D, 0x8D, 0x89, 0x99, 0x88, 0x98, 0x90, 0x80, 0x9A, 0x8A, 0x82, 0x92,
    0x1A, 0x12, 0x00, 0x08, 0x06, 0x0E, 0x0F, 0x07, 0x17, 0x1F, 0x0D, 0x05,
    0x1D, 0x15, 0xFB, 0xFF, 0xEB, 0xEF, 0xED, 0xEA, 0xEE, 0xEC, 0xE6, 0xE2,
    0xE4, 0xE0, 0xF4, 0xF0, 0x60, 0x64, 0x74, 0x70, 0x73, 0x72, 0x6C, 0x7C,
    0x68, 0x78, 0x7A, 0x7E, 0x6A, 0x6E, 0x6F, 0xC4, 0xC5, 0xC9, 0xC8, 0xC7,
    0xCB, 0xC6, 0xCA, 0xD6, 0xDA, 0xDB, 0xD7, 0xD9, 0xD5, 0xD8, 0xD4, 0x20,
    0x28, 0x38, 0x22, 0x2A, 0x39, 0x29, 0x21, 0x23, 0x2B, 0x27, 0x2F, 0x25,
    0x2D, 0x3D, 0x35, 0x3C, 0x2E, 0x2C,
    // 80 .. 147
    0x26, 0x24, 0x49, 0x48, 0x4C, 0x4D, 0x44, 0x45, 0x40, 0x41, 0xA7, 0xA3,
    0xB7, 0xB3, 0xB1, 0xB5, 0xA5, 0xA1, 0x97, 0x87, 0x9F, 0x8F, 0x81, 0x91,
    0x9B, 0x8B, 0x83, 0x93, 0x18, 0x10, 0x04, 0x0C, 0x16, 0x1E, 0x14, 0x1C,
    0xF9, 0xFA, 0xFD, 0xE9, 0xFE, 0xE8, 0xFC, 0x43, 0xF2, 0xF6, 0xF8, 0x5B,
    0x5A, 0x63, 0x62, 0x77, 0x76, 0x52, 0x53, 0x66, 0x67, 0xCC, 0xCD, 0xAB,
    0xCF, 0xCE, 0xDE, 0xBF, 0xDF, 0xDD, 0xDC, 0xAF};

// 3.7.2, Hz. LSP 1 and 6 to 10 have eight levels (3 bits), 2 to 5 sixteen.
constexpr std::array<double, 8> kLsp1 = {100, 170, 225, 250, 280, 340, 420, 500};
constexpr std::array<double, 16> kLsp2 = {210, 235, 265, 295, 325, 360, 400, 440,
                                          480, 520, 560, 610, 670, 740, 810, 880};
constexpr std::array<double, 16> kLsp3 = {420,  460,  500,  540,  585,  640,  705,  775,
                                          850,  950,  1050, 1150, 1250, 1350, 1450, 1550};
constexpr std::array<double, 16> kLsp4 = {620,  660,  720,  795,  880,  970,  1080, 1170,
                                          1270, 1370, 1470, 1570, 1670, 1770, 1870, 1970};
constexpr std::array<double, 16> kLsp5 = {1000, 1050, 1130, 1210, 1285, 1350, 1430, 1510,
                                          1590, 1670, 1750, 1850, 1950, 2050, 2150, 2250};
constexpr std::array<double, 8> kLsp6 = {1470, 1570, 1690, 1830, 2000, 2200, 2400, 2600};
constexpr std::array<double, 8> kLsp7 = {1800, 1880, 1960, 2100, 2300, 2480, 2700, 2900};
constexpr std::array<double, 8> kLsp8 = {2225, 2400, 2525, 2650, 2800, 2950, 3150, 3350};
constexpr std::array<double, 8> kLsp9 = {2760, 2880, 3000, 3100, 3200, 3310, 3430, 3550};
constexpr std::array<double, 8> kLsp10 = {3190, 3270, 3350, 3420, 3490, 3590, 3710, 3830};

// 3.7.3: weight on the previous frame's LSPs, in eighths, per subframe.
constexpr std::array<double, 4> kPrevLspWeight = {7.0 / 8, 5.0 / 8, 3.0 / 8, 1.0 / 8};

// 3.8.2 bit assignment. Kinds: L = LSP j, D = PD(n), G = PG(n), I = CI(n),
// C = CG(n), H = HP, S = SP, Y = SY. Entry k is transmitted bit k + 1.
struct BitRef {
    char kind;
    std::uint8_t sub;
    std::uint8_t bit;
};
constexpr std::array<BitRef, 144> kBitTable = {{
    {'G', 4, 4}, {'D', 3, 4}, {'L', 1, 1}, {'C', 2, 4}, {'I', 3, 3}, {'I', 1, 8},     // 1-6
    {'D', 4, 0}, {'L', 8, 0}, {'G', 2, 3}, {'C', 3, 0}, {'D', 1, 5}, {'L', 3, 3},     // 7-12
    {'I', 2, 3}, {'I', 4, 4}, {'D', 2, 1}, {'L', 10, 0}, {'G', 1, 3}, {'C', 4, 0},    // 13-18
    {'L', 5, 2}, {'D', 3, 0}, {'H', 0, 0}, {'I', 1, 1}, {'I', 4, 8}, {'L', 2, 2},     // 19-24
    {'G', 3, 1}, {'D', 4, 5}, {'C', 1, 3}, {'I', 3, 5}, {'L', 7, 0}, {'I', 2, 1},     // 25-30
    {'D', 3, 7}, {'I', 1, 0}, {'G', 4, 0}, {'L', 4, 3}, {'C', 3, 1}, {'I', 1, 5},     // 31-36
    {'D', 2, 0}, {'I', 4, 1}, {'L', 9, 0}, {'I', 3, 8}, {'G', 1, 4}, {'C', 2, 2},     // 37-42
    {'D', 1, 3}, {'L', 6, 1}, {'I', 3, 4}, {'I', 2, 2}, {'C', 1, 4}, {'D', 2, 3},     // 43-48
    {'L', 1, 2}, {'G', 3, 2}, {'H', 0, 1}, {'D', 3, 1}, {'C', 4, 3}, {'L', 8, 1},     // 49-54
    {'G', 3, 0}, {'I', 2, 8}, {'D', 4, 1}, {'I', 4, 0}, {'L', 3, 2}, {'G', 2, 0},     // 55-60
    {'D', 1, 6}, {'C', 2, 0}, {'I', 3, 6}, {'L', 10, 1}, {'G', 1, 1}, {'I', 4, 7},    // 61-66
    {'D', 3, 3}, {'C', 1, 2}, {'L', 5, 3}, {'I', 1, 6}, {'L', 2, 0}, {'G', 3, 4},     // 67-72
    {'D', 1, 4}, {'C', 3, 2}, {'L', 7, 1}, {'I', 2, 7}, {'I', 3, 0}, {'D', 2, 5},     // 73-78
    {'L', 4, 1}, {'C', 1, 0}, {'G', 4, 3}, {'L', 9, 1}, {'D', 3, 6}, {'I', 1, 4},     // 79-84
    {'C', 2, 1}, {'L', 6, 2}, {'I', 4, 3}, {'G', 2, 2}, {'D', 4, 3}, {'L', 1, 0},     // 85-90
    {'C', 4, 2}, {'L', 8, 2}, {'I', 2, 4}, {'H', 0, 2}, {'D', 2, 2}, {'L', 3, 0},     // 91-96
    {'G', 1, 2}, {'C', 3, 4}, {'L', 10, 2}, {'I', 4, 5}, {'I', 2, 0}, {'D', 1, 2},    // 97-102
    {'L', 5, 1}, {'S', 0, 0}, {'G', 4, 2}, {'C', 2, 3}, {'L', 2, 1}, {'D', 4, 4},     // 103-108
    {'I', 1, 2}, {'G', 2, 1}, {'I', 3, 7}, {'L', 4, 0}, {'I', 2, 5}, {'D', 1, 7},     // 109-114
    {'G', 1, 0}, {'C', 4, 4}, {'L', 5, 0}, {'D', 4, 2}, {'I', 1, 3}, {'I', 3, 1},     // 115-120
    {'L', 7, 2}, {'I', 4, 2}, {'D', 1, 1}, {'G', 2, 4}, {'C', 3, 3}, {'L', 3, 1},     // 121-126
    {'I', 1, 7}, {'D', 3, 2}, {'I', 2, 6}, {'L', 9, 2}, {'G', 4, 1}, {'C', 1, 1},     // 127-132
    {'D', 2, 4}, {'H', 0, 3}, {'L', 6, 0}, {'G', 3, 3}, {'I', 4, 6}, {'D', 1, 0},     // 133-138
    {'L', 2, 3}, {'C', 4, 1}, {'I', 3, 2}, {'L', 4, 2}, {'D', 3, 5}, {'Y', 0, 0},     // 139-144
}};

// 3.9.1: the protected bits in the clause's order, and which parity bits
// cover each. HP-k covers protected bit i when bit k of kCover[i] is set. The
// values equal the 3.9.2 "parity result" column, which is the check that the
// two lists agree.
struct Protected {
    char kind;
    std::uint8_t sub;
    std::uint8_t bit;
};
constexpr std::array<Protected, 11> kProtected = {{
    {'D', 1, 5}, {'D', 1, 6}, {'D', 1, 7}, {'G', 1, 4}, {'G', 2, 4}, {'D', 3, 5},
    {'D', 3, 6}, {'D', 3, 7}, {'G', 3, 4}, {'G', 4, 4}, {'S', 0, 0}}};
constexpr std::array<std::uint8_t, 11> kCover = {3, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15};

unsigned get_field(const Fs1016Params& p, char kind, std::uint8_t sub) {
    const std::size_t n = sub == 0 ? 0 : sub - 1U;
    switch (kind) {
    case 'L': return p.lsp[n];
    case 'D': return p.pd[n];
    case 'G': return p.pg[n];
    case 'I': return p.ci[n];
    case 'C': return p.cg[n];
    case 'S': return p.sp;
    default: return 0;
    }
}

void set_bit(Fs1016Params& p, char kind, std::uint8_t sub, std::uint8_t bit, unsigned value) {
    const std::size_t n = sub == 0 ? 0 : sub - 1U;
    const unsigned mask = 1U << bit;
    auto apply = [&](auto& field) {
        using T = std::remove_reference_t<decltype(field)>;
        unsigned v = field;
        v = value ? (v | mask) : (v & ~mask);
        field = static_cast<T>(v);
    };
    switch (kind) {
    case 'L': apply(p.lsp[n]); break;
    case 'D': apply(p.pd[n]); break;
    case 'G': apply(p.pg[n]); break;
    case 'I': apply(p.ci[n]); break;
    case 'C': apply(p.cg[n]); break;
    case 'S': apply(p.sp); break;
    default: break;
    }
}

std::uint16_t gather_protected(const Fs1016Params& p) {
    std::uint16_t word = 0;
    for (std::size_t i = 0; i < kProtected.size(); ++i) {
        const Protected& b = kProtected[i];
        if ((get_field(p, b.kind, b.sub) >> b.bit) & 1U) {
            word = static_cast<std::uint16_t>(word | (1U << i));
        }
    }
    return word;
}

// ---------------------------------------------------------------------------
// 3.3.1: the code book program
// ---------------------------------------------------------------------------

// The function random() of 3.3.1: a five-tap additive generator with the
// printed seeds, taps j = 2 and k = 5, both counting down and wrapping.
struct BookRandom {
    std::array<int, 5> y = {-21161, -8478, 30892, -10216, 16950};
    int j = 2;
    int k = 5;
    int next() {
        int temp = (y[static_cast<std::size_t>(k - 1)] + y[static_cast<std::size_t>(j - 1)]) & 65535;
        if (temp > 32767) {
            temp -= 65536;
        }
        y[static_cast<std::size_t>(k - 1)] = temp;
        if (--k <= 0) {
            k = 5;
        }
        if (--j <= 0) {
            j = 5;
        }
        return temp;
    }
};

// Subroutine noise of 3.3.1, in single precision as the program declares
// its variables REAL. The inner loop calls random() four times and keeps
// only the last value, exactly as written.
void book_noise(BookRandom& rng, float& x1, float& x2) {
    std::array<float, 2> f{};
    float f1 = 0;
    float f2 = 0;
    float s = 0;
    do {
        for (float& fi : f) {
            for (int rep = 0; rep < 4; ++rep) {
                fi = static_cast<float>(rng.next() + 32768) / 65535.0F;
            }
        }
        f1 = 2.0F * f[0] - 1.0F;
        f2 = 2.0F * f[1] - 1.0F;
        s = f1 * f1 + f2 * f2;
    } while (s >= 1.0F);
    s = std::sqrt(-2.0F * std::log(s) / s);
    x1 = f1 * s;
    x2 = f2 * s;
}

std::array<std::int8_t, kFs1016StochasticBook> make_book() {
    // x(0:MAXCODE+1) with MAXCODE = 2*(512-1)+60 = 1082, THRESH = 1.2. The
    // program writes x(1) onward, so element e of the book is x(e + 1).
    constexpr int kMax = 1082;
    std::array<float, kMax + 2> x{};
    BookRandom rng;
    for (int k = 0; k <= kMax; k += 2) {
        book_noise(rng, x[static_cast<std::size_t>(k)], x[static_cast<std::size_t>(k + 1)]);
    }
    std::array<std::int8_t, kFs1016StochasticBook> book{};
    for (std::size_t e = 0; e < book.size(); ++e) {
        const float v = x[e + 1];
        book[e] = std::fabs(v) < 1.2F ? std::int8_t{0} : (v < 0 ? std::int8_t{-1} : std::int8_t{1});
    }
    return book;
}

// 3.3.2: code n is elements 2(511 - n) .. 2(511 - n) + 59.
const std::int8_t* stochastic_code(std::size_t n) {
    return fs1016_stochastic_book().data() + 2 * (511 - n);
}

// ---------------------------------------------------------------------------
// 3.5: adaptive codes
// ---------------------------------------------------------------------------

// The fraction of each delay as the 3.5.3 program's DATA statement spells it.
double delay_fraction(std::size_t index) {
    if (index < 18 || (index >= 50 && index < 188)) {
        const std::size_t base = index < 18 ? index : index - 50;
        switch (base % 3) {
        case 1: return 0.33333333;
        case 2: return 0.66666667;
        default: return 0.0;
        }
    }
    if (index < 50) {
        return 0.25 * static_cast<double>((index - 18) % 4);
    }
    return 0.0;
}

int delay_integer(std::size_t index) {
    if (index < 18) {
        return 20 + static_cast<int>(index / 3);
    }
    if (index < 50) {
        return 26 + static_cast<int>((index - 18) / 4);
    }
    if (index < 188) {
        return 34 + static_cast<int>((index - 50) / 3);
    }
    return 80 + static_cast<int>(index - 188);
}

// Weights for one fraction, w[j + 20] for j = -20..19.
using Weights = std::array<double, 40>;

const Weights& weights_for(double fraction) {
    static const std::array<Weights, 5> table = [] {
        std::array<Weights, 5> t{};
        const std::array<double, 5> f = {0.25, 0.33333333, 0.5, 0.66666667, 0.75};
        for (std::size_t i = 0; i < 5; ++i) {
            for (int j = -20; j < 20; ++j) {
                t[i][static_cast<std::size_t>(j + 20)] = fs1016_interp_weight(j, f[i]);
            }
        }
        return t;
    }();
    if (fraction < 0.3) {
        return table[0];
    }
    if (fraction < 0.4) {
        return table[1];
    }
    if (fraction < 0.6) {
        return table[2];
    }
    if (fraction < 0.7) {
        return table[3];
    }
    return table[4];
}

// One adaptive code from the register. reg[kReg + p] is element p for
// p = -147..-1. Positions at or after the start of the code read the code
// itself (3.5.1's repetition for integer delays under 60, 3.5.3's "previously
// interpolated values" for fractional ones; header note 2).
void adaptive_code(const std::array<double, kReg>& reg, std::size_t index,
                   std::array<double, kSub>& out) {
    const int d = delay_integer(index);
    const double fraction = delay_fraction(index);
    auto val = [&](int p) -> double {
        if (p < 0) {
            return reg[static_cast<std::size_t>(static_cast<int>(kReg) + p)];
        }
        return out[static_cast<std::size_t>(p)];
    };
    if (fraction == 0.0) {
        for (int k = 0; k < static_cast<int>(kSub); ++k) {
            out[static_cast<std::size_t>(k)] = val(k - d);
        }
        return;
    }
    const Weights& w = weights_for(fraction);
    for (int k = 0; k < static_cast<int>(kSub); ++k) {
        double sum = 0.0;
        for (int j = -20; j < 20; ++j) {
            sum += w[static_cast<std::size_t>(j + 20)] * val(k - d + j);
        }
        out[static_cast<std::size_t>(k)] = sum;
    }
}

const std::array<std::int16_t, 256>& hex_to_index() {
    static const std::array<std::int16_t, 256> inverse = [] {
        std::array<std::int16_t, 256> t{};
        t.fill(-1);
        for (std::size_t i = 0; i < kAdaptiveHex.size(); ++i) {
            t[kAdaptiveHex[i]] = static_cast<std::int16_t>(i);
        }
        return t;
    }();
    return inverse;
}

// 3.5.5: the lowest index a 6-bit code can reach from the previous
// subframe's index. 29.25 is index 31, 115.0 is index 223, 84.00 is 192.
std::size_t delta_base(std::size_t prev) {
    if (prev <= 31) {
        return 0;
    }
    if (prev >= 223) {
        return 192;
    }
    return prev - 31;
}

void push_register(std::array<double, kReg>& reg, const std::array<double, kSub>& exc) {
    std::copy(reg.begin() + kSub, reg.end(), reg.begin());
    std::copy(exc.begin(), exc.end(), reg.end() - kSub);
}

// ---------------------------------------------------------------------------
// Filters and LSPs
// ---------------------------------------------------------------------------

using Lpc = std::array<double, 11>;

// y = x / A(z), memory holds y[n-1], y[n-2], ...
template <std::size_t N>
void all_pole(const Lpc& a, const std::array<double, N>& in, std::array<double, N>& out,
              std::array<double, 10>& mem) {
    for (std::size_t n = 0; n < N; ++n) {
        double y = in[n];
        for (std::size_t i = 1; i <= 10; ++i) {
            y -= a[i] * mem[i - 1];
        }
        for (std::size_t i = 9; i > 0; --i) {
            mem[i] = mem[i - 1];
        }
        mem[0] = y;
        out[n] = y;
    }
}

// y = A(z) x, memory holds x[n-1], ...
template <std::size_t N>
void all_zero(const Lpc& a, const std::array<double, N>& in, std::array<double, N>& out,
              std::array<double, 10>& mem) {
    for (std::size_t n = 0; n < N; ++n) {
        double y = in[n];
        for (std::size_t i = 1; i <= 10; ++i) {
            y += a[i] * mem[i - 1];
        }
        for (std::size_t i = 9; i > 0; --i) {
            mem[i] = mem[i - 1];
        }
        mem[0] = in[n];
        out[n] = y;
    }
}

Lpc expand(const Lpc& a, double g) {
    Lpc out = a;
    double gi = 1.0;
    for (std::size_t i = 1; i <= 10; ++i) {
        gi *= g;
        out[i] = a[i] * gi;
    }
    return out;
}

std::array<double, 10> flat_lsp() {
    std::array<double, 10> lsp{};
    for (std::size_t i = 0; i < 10; ++i) {
        lsp[i] = static_cast<double>(i + 1) * 4000.0 / 11.0;
    }
    return lsp;
}

std::array<double, 10> dequantise_lsp(const Fs1016Params& p) {
    std::array<double, 10> lsp{};
    for (std::size_t i = 0; i < 10; ++i) {
        const auto table = fs1016_lsp_table(i + 1);
        lsp[i] = table[std::min<std::size_t>(p.lsp[i], table.size() - 1)];
    }
    std::sort(lsp.begin(), lsp.end());  // header note 5
    return lsp;
}

std::array<double, 10> interpolate_lsp(const std::array<double, 10>& prev,
                                       const std::array<double, 10>& cur, std::size_t sub) {
    std::array<double, 10> out{};
    for (std::size_t i = 0; i < 10; ++i) {
        out[i] = kPrevLspWeight[sub] * prev[i] + (1.0 - kPrevLspWeight[sub]) * cur[i];
    }
    return out;
}

std::size_t nearest(std::span<const double> table, double value) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < table.size(); ++i) {
        if (std::fabs(table[i] - value) < std::fabs(table[best] - value)) {
            best = i;
        }
    }
    return best;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public table access
// ---------------------------------------------------------------------------

const std::array<std::int8_t, kFs1016StochasticBook>& fs1016_stochastic_book() {
    static const auto book = make_book();
    return book;
}

double fs1016_adaptive_delay(std::size_t index) {
    return static_cast<double>(delay_integer(index)) + delay_fraction(index);
}

std::uint8_t fs1016_adaptive_hex(std::size_t index) { return kAdaptiveHex[index]; }

double fs1016_interp_weight(int j, double fraction) {
    // 3.5.3 program weights, N = 40: h(k) = 0.54 + 0.46 cos(pi k / 240) and
    // w(j) = h(nint(12 (j + f))) sin(pi (j + f)) / (pi (j + f)).
    const double x = static_cast<double>(j) + fraction;
    const double k = static_cast<double>(std::lround(12.0 * x));
    const double h = 0.54 + 0.46 * std::cos(kPi * k / 240.0);
    return h * std::sin(kPi * x) / (kPi * x);
}

double fs1016_stochastic_gain(std::size_t index) { return kStochasticGain[index & 31U]; }
double fs1016_adaptive_gain(std::size_t index) { return kAdaptiveGain[index & 31U]; }

std::span<const double> fs1016_lsp_table(std::size_t lsp) {
    switch (lsp) {
    case 1: return kLsp1;
    case 2: return kLsp2;
    case 3: return kLsp3;
    case 4: return kLsp4;
    case 5: return kLsp5;
    case 6: return kLsp6;
    case 7: return kLsp7;
    case 8: return kLsp8;
    case 9: return kLsp9;
    default: return kLsp10;
    }
}

std::uint8_t fs1016_hamming_parity(std::uint16_t protected_bits) {
    std::uint8_t parity = 0;
    for (std::size_t i = 0; i < kCover.size(); ++i) {
        if ((protected_bits >> i) & 1U) {
            parity ^= kCover[i];
        }
    }
    return parity;
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

Fs1016Bits fs1016_pack(const Fs1016Params& params, bool sync) {
    const std::uint8_t parity = fs1016_hamming_parity(gather_protected(params));
    Fs1016Bits bits{};
    for (std::size_t i = 0; i < kBitTable.size(); ++i) {
        const BitRef& b = kBitTable[i];
        unsigned v = 0;
        if (b.kind == 'H') {
            v = (parity >> b.bit) & 1U;
        } else if (b.kind == 'Y') {
            v = sync ? 1U : 0U;
        } else {
            v = (get_field(params, b.kind, b.sub) >> b.bit) & 1U;
        }
        bits[i] = static_cast<std::uint8_t>(v);
    }
    return bits;
}

Fs1016Unpacked fs1016_unpack(const Fs1016Bits& bits) {
    Fs1016Unpacked out;
    std::uint8_t received_parity = 0;
    for (std::size_t i = 0; i < kBitTable.size(); ++i) {
        const BitRef& b = kBitTable[i];
        const unsigned v = bits[i] & 1U;
        if (b.kind == 'H') {
            received_parity = static_cast<std::uint8_t>(received_parity | (v << b.bit));
        } else if (b.kind == 'Y') {
            out.sync = v != 0;
        } else {
            set_bit(out.params, b.kind, b.sub, b.bit, v);
        }
    }
    // 3.9.2: syndrome selects the protected bit to invert. 1, 2, 4 and 8 are
    // a parity bit in error and need nothing done to the data.
    out.syndrome = static_cast<std::uint8_t>(
        fs1016_hamming_parity(gather_protected(out.params)) ^ received_parity);
    for (std::size_t i = 0; i < kCover.size(); ++i) {
        if (kCover[i] == out.syndrome) {
            const Protected& b = kProtected[i];
            const unsigned cur = (get_field(out.params, b.kind, b.sub) >> b.bit) & 1U;
            set_bit(out.params, b.kind, b.sub, b.bit, cur ^ 1U);
            out.corrected = true;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// LPC <-> LSP (header note 3)
// ---------------------------------------------------------------------------

std::array<double, 10> fs1016_lpc_to_lsp(const Lpc& a) {
    // P(z) = A(z) + z^-11 A(1/z), Q(z) = A(z) - z^-11 A(1/z). On the unit
    // circle, after removing the linear phase, P is sum p_k cos((k - 5.5) w)
    // and Q is sum q_k sin((k - 5.5) w); their zeros in (0, pi) interlace.
    std::array<double, 12> p{};
    std::array<double, 12> q{};
    for (std::size_t k = 0; k < 12; ++k) {
        const double ak = k <= 10 ? a[k] : 0.0;
        const double ar = (11 - k) <= 10 ? a[11 - k] : 0.0;
        p[k] = ak + ar;
        q[k] = ak - ar;
    }
    auto fp = [&](double w) {
        double s = 0.0;
        for (std::size_t k = 0; k < 12; ++k) {
            s += p[k] * std::cos((static_cast<double>(k) - 5.5) * w);
        }
        return s;
    };
    auto fq = [&](double w) {
        double s = 0.0;
        for (std::size_t k = 0; k < 12; ++k) {
            s += q[k] * std::sin((static_cast<double>(k) - 5.5) * w);
        }
        return s;
    };
    std::vector<double> roots;
    constexpr int kGrid = 2048;
    for (int which = 0; which < 2; ++which) {
        auto f = [&](double w) { return which == 0 ? fp(w) : fq(w); };
        double w0 = 1e-4;
        double v0 = f(w0);
        for (int g = 1; g <= kGrid; ++g) {
            const double w1 = kPi * static_cast<double>(g) / kGrid - (g == kGrid ? 1e-4 : 0.0);
            const double v1 = f(w1);
            if ((v0 < 0) != (v1 < 0)) {
                double lo = w0;
                double hi = w1;
                double vlo = v0;
                for (int it = 0; it < 40; ++it) {
                    const double mid = 0.5 * (lo + hi);
                    const double vm = f(mid);
                    if ((vm < 0) == (vlo < 0)) {
                        lo = mid;
                        vlo = vm;
                    } else {
                        hi = mid;
                    }
                }
                roots.push_back(0.5 * (lo + hi));
            }
            w0 = w1;
            v0 = v1;
        }
    }
    std::array<double, 10> lsp = flat_lsp();
    if (roots.size() != 10) {
        return lsp;  // not minimum phase; the caller treats this as flat
    }
    std::sort(roots.begin(), roots.end());
    for (std::size_t i = 0; i < 10; ++i) {
        lsp[i] = roots[i] * 8000.0 / (2.0 * kPi);
    }
    return lsp;
}

Lpc fs1016_lsp_to_lpc(const std::array<double, 10>& lsp_hz) {
    // P has the odd-numbered LSPs (1, 3, ...) and a root at z = -1; Q the
    // even-numbered ones and a root at z = 1.
    auto build = [&](std::size_t first, double trivial) {
        std::array<double, 12> poly{};
        poly[0] = 1.0;
        poly[1] = trivial;
        std::size_t len = 2;
        for (std::size_t i = first; i < 10; i += 2) {
            const double c = -2.0 * std::cos(2.0 * kPi * lsp_hz[i] / 8000.0);
            std::array<double, 12> next{};
            for (std::size_t k = 0; k < len; ++k) {
                next[k] += poly[k];
                next[k + 1] += c * poly[k];
                next[k + 2] += poly[k];
            }
            poly = next;
            len += 2;
        }
        return poly;
    };
    const auto p = build(0, 1.0);
    const auto q = build(1, -1.0);
    Lpc a{};
    for (std::size_t k = 0; k <= 10; ++k) {
        a[k] = 0.5 * (p[k] + q[k]);
    }
    return a;
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

Fs1016Decoder::Fs1016Decoder(bool postfilter) : postfilter_(postfilter), prev_lsp_(flat_lsp()) {}

std::array<float, kFs1016FrameSamples> Fs1016Decoder::decode(const Fs1016Bits& bits) {
    const Fs1016Unpacked u = fs1016_unpack(bits);
    report_.sync = u.sync;
    report_.sync_as_expected = first_ || u.sync == expect_sync_;
    expect_sync_ = !u.sync;
    first_ = false;
    report_.syndrome = u.syndrome;
    report_.corrected = u.corrected;
    report_.corrections += u.corrected ? 1U : 0U;
    ++report_.frames;
    std::array<float, kFs1016FrameSamples> out{};
    synthesise(u.params, out);
    return out;
}

std::array<float, kFs1016FrameSamples> Fs1016Decoder::decode_params(const Fs1016Params& params) {
    ++report_.frames;
    std::array<float, kFs1016FrameSamples> out{};
    synthesise(params, out);
    return out;
}

std::string Fs1016Decoder::describe_last_frame() const {
    char line[160];
    std::snprintf(line, sizeof line, "FS-1016 frame %zu: sync %d%s, syndrome %u%s, %zu corrections so far",
                  report_.frames, report_.sync ? 1 : 0,
                  report_.sync_as_expected ? "" : " (out of step)",
                  static_cast<unsigned>(report_.syndrome),
                  report_.corrected ? " (corrected)" : "", report_.corrections);
    return line;
}

void Fs1016Decoder::synthesise(const Fs1016Params& params,
                               std::array<float, kFs1016FrameSamples>& out) {
    const auto cur_lsp = dequantise_lsp(params);
    for (std::size_t sub = 0; sub < kFs1016Subframes; ++sub) {
        const Lpc a = fs1016_lsp_to_lpc(interpolate_lsp(prev_lsp_, cur_lsp, sub));

        std::size_t pd_index = 0;
        if (sub % 2 == 0) {
            const std::int16_t idx = hex_to_index()[params.pd[sub]];
            pd_index = idx < 0 ? prev_pd_index_ : static_cast<std::size_t>(idx);
        } else {
            pd_index = std::min<std::size_t>(delta_base(prev_pd_index_) + (params.pd[sub] & 63U), 255);
        }
        prev_pd_index_ = pd_index;

        std::array<double, kSub> adapt{};
        adaptive_code(adaptive_, pd_index, adapt);
        const std::int8_t* code = stochastic_code(params.ci[sub] & 511U);
        const double pg = fs1016_adaptive_gain(params.pg[sub]);
        const double cg = fs1016_stochastic_gain(params.cg[sub]);
        std::array<double, kSub> exc{};
        for (std::size_t i = 0; i < kSub; ++i) {
            exc[i] = pg * adapt[i] + cg * static_cast<double>(code[i]);
        }
        push_register(adaptive_, exc);

        std::array<double, kSub> speech{};
        all_pole(a, exc, speech, synth_mem_);

        if (postfilter_) {
            // Header note 6: A(z/0.5)/A(z/0.8) with a mild first-order tilt,
            // scaled to keep the subframe's energy.
            std::array<double, kSub> num{};
            std::array<double, kSub> pf{};
            all_zero(expand(a, 0.5), speech, num, pf_zero_mem_);
            all_pole(expand(a, 0.8), num, pf, pf_pole_mem_);
            double ein = 0.0;
            double eout = 0.0;
            for (std::size_t i = 0; i < kSub; ++i) {
                const double t = pf[i] - 0.3 * pf_tilt_mem_;
                pf_tilt_mem_ = pf[i];
                pf[i] = t;
                ein += speech[i] * speech[i];
                eout += t * t;
            }
            const double g = eout > 0 ? std::sqrt(ein / eout) : 1.0;
            for (std::size_t i = 0; i < kSub; ++i) {
                speech[i] = pf[i] * g;
            }
        }
        for (std::size_t i = 0; i < kSub; ++i) {
            // 3.2.3: the synthesis is in 16-bit PCM units.
            out[sub * kSub + i] = static_cast<float>(std::clamp(speech[i] / 32768.0, -1.0, 1.0));
        }
    }
    prev_lsp_ = cur_lsp;
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

namespace {

// 3.2.5: the weighting filter's bandwidth expansion.
constexpr double kGamma = 0.8;
// 3.7.2: 15 Hz bandwidth expansion.
constexpr double kLspExpand = 0.994;

Lpc analyse_lpc(std::span<const double> window_samples) {
    const std::size_t n = window_samples.size();
    std::vector<double> w(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double h = 0.54 - 0.46 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                                static_cast<double>(n - 1));
        w[i] = window_samples[i] * h;
    }
    std::array<double, 11> r{};
    for (std::size_t lag = 0; lag <= 10; ++lag) {
        for (std::size_t i = lag; i < n; ++i) {
            r[lag] += w[i] * w[i - lag];
        }
    }
    Lpc a{};
    a[0] = 1.0;
    if (r[0] <= 1e-3) {
        return a;
    }
    r[0] *= 1.0001;  // a white-noise floor so silence stays well conditioned
    double err = r[0];
    for (std::size_t i = 1; i <= 10; ++i) {
        double acc = r[i];
        for (std::size_t j = 1; j < i; ++j) {
            acc += a[j] * r[i - j];
        }
        const double k = -acc / err;
        Lpc next = a;
        for (std::size_t j = 1; j < i; ++j) {
            next[j] = a[j] + k * a[i - j];
        }
        next[i] = k;
        a = next;
        err *= (1.0 - k * k);
        if (err <= 0) {
            break;
        }
    }
    return expand(a, kLspExpand);
}

// 3.7.2: nearest table value per LSP, never below the previous LSP's value.
std::array<std::uint8_t, 10> quantise_lsp(const std::array<double, 10>& lsp) {
    std::array<std::uint8_t, 10> idx{};
    double floor_hz = 0.0;
    for (std::size_t i = 0; i < 10; ++i) {
        const auto table = fs1016_lsp_table(i + 1);
        std::size_t best = table.size();
        for (std::size_t k = 0; k < table.size(); ++k) {
            if (table[k] < floor_hz) {
                continue;
            }
            if (best == table.size() || std::fabs(table[k] - lsp[i]) < std::fabs(table[best] - lsp[i])) {
                best = k;
            }
        }
        if (best == table.size()) {
            best = table.size() - 1;
        }
        idx[i] = static_cast<std::uint8_t>(best);
        floor_hz = table[best];
    }
    return idx;
}

}  // namespace

Fs1016Encoder::Fs1016Encoder() : prev_lsp_(flat_lsp()) {}

Fs1016Bits Fs1016Encoder::encode(std::span<const float> frame) {
    std::copy(speech_.begin() + kFs1016FrameSamples, speech_.end(), speech_.begin());
    for (std::size_t i = 0; i < kFs1016FrameSamples; ++i) {
        const double v = i < frame.size() ? static_cast<double>(frame[i]) : 0.0;
        speech_[kFs1016FrameSamples + i] = v * 32768.0;  // 3.2.3
    }

    // 3.7.2: window over subframes 3 and 4 of the frame being coded and 1
    // and 2 of the next.
    const Lpc a_frame = analyse_lpc(std::span<const double>(speech_).subspan(120, 240));
    params_ = Fs1016Params{};
    params_.lsp = quantise_lsp(fs1016_lpc_to_lsp(a_frame));
    const auto cur_lsp = dequantise_lsp(params_);

    for (std::size_t sub = 0; sub < kFs1016Subframes; ++sub) {
        const Lpc a = fs1016_lsp_to_lpc(interpolate_lsp(prev_lsp_, cur_lsp, sub));
        const Lpc aw = expand(a, kGamma);

        std::array<double, kSub> s{};
        std::copy_n(speech_.begin() + static_cast<std::ptrdiff_t>(sub * kSub), kSub, s.begin());

        // Weighted speech: A(z) then 1/A(z/g), with running memories.
        std::array<double, kSub> res{};
        std::array<double, kSub> sw{};
        all_zero(a, s, res, fir_mem_);
        all_pole(aw, res, sw, wiir_mem_);

        // Zero-input response of the weighted synthesis filter 1/A(z/g).
        std::array<double, kSub> zero{};
        std::array<double, kSub> zir{};
        std::array<double, 10> mem = h_mem_;
        all_pole(aw, zero, zir, mem);

        std::array<double, kSub> target{};
        for (std::size_t i = 0; i < kSub; ++i) {
            target[i] = sw[i] - zir[i];
        }

        // Impulse response of 1/A(z/g), 60 samples.
        std::array<double, kSub> imp{};
        std::array<double, kSub> h{};
        imp[0] = 1.0;
        std::array<double, 10> hmem{};
        all_pole(aw, imp, h, hmem);
        auto filter = [&](const auto& code, std::array<double, kSub>& y) {
            for (std::size_t n = 0; n < kSub; ++n) {
                double acc = 0.0;
                for (std::size_t k = 0; k <= n; ++k) {
                    acc += static_cast<double>(code[k]) * h[n - k];
                }
                y[n] = acc;
            }
        };
        auto dot = [](const std::array<double, kSub>& x, const std::array<double, kSub>& y) {
            double s2 = 0.0;
            for (std::size_t i = 0; i < kSub; ++i) {
                s2 += x[i] * y[i];
            }
            return s2;
        };

        // Adaptive search (3.5), on the quantised gain's error.
        std::size_t lo = 0;
        std::size_t hi = 255;
        if (sub % 2 == 1) {
            lo = delta_base(prev_pd_index_);
            hi = lo + 63;
        }
        double best_err = std::numeric_limits<double>::max();
        std::size_t best_pd = lo;
        std::size_t best_pg = 6;  // gain 0.000
        std::array<double, kSub> best_ya{};
        std::array<double, kSub> best_ca{};
        const double tt = dot(target, target);
        for (std::size_t idx = lo; idx <= hi; ++idx) {
            std::array<double, kSub> c{};
            adaptive_code(adaptive_, idx, c);
            std::array<double, kSub> y{};
            filter(c, y);
            const double yy = dot(y, y);
            if (yy <= 0) {
                continue;
            }
            const double ty = dot(target, y);
            const std::size_t gi = nearest(kAdaptiveGain, ty / yy);
            const double g = kAdaptiveGain[gi];
            const double e = tt - 2.0 * g * ty + g * g * yy;
            if (e < best_err) {
                best_err = e;
                best_pd = idx;
                best_pg = gi;
                best_ya = y;
                best_ca = c;
            }
        }
        if (best_err == std::numeric_limits<double>::max()) {
            adaptive_code(adaptive_, best_pd, best_ca);
            best_ya.fill(0.0);
        }
        const double pg = kAdaptiveGain[best_pg];
        std::array<double, kSub> target2{};
        for (std::size_t i = 0; i < kSub; ++i) {
            target2[i] = target[i] - pg * best_ya[i];
        }

        // Stochastic search (3.3, 3.4) over all 512 codes.
        const double tt2 = dot(target2, target2);
        best_err = std::numeric_limits<double>::max();
        std::size_t best_ci = 0;
        std::size_t best_cg = 16;
        for (std::size_t n = 0; n < kFs1016StochasticCodes; ++n) {
            const std::int8_t* code = stochastic_code(n);
            std::array<double, kSub> c{};
            for (std::size_t i = 0; i < kSub; ++i) {
                c[i] = code[i];
            }
            std::array<double, kSub> y{};
            filter(c, y);
            const double yy = dot(y, y);
            if (yy <= 0) {
                continue;
            }
            const double ty = dot(target2, y);
            const std::size_t gi = nearest(kStochasticGain, ty / yy);
            const double g = kStochasticGain[gi];
            const double e = tt2 - 2.0 * g * ty + g * g * yy;
            if (e < best_err) {
                best_err = e;
                best_ci = n;
                best_cg = gi;
            }
        }

        // Transmitted values.
        params_.pd[sub] = sub % 2 == 0 ? kAdaptiveHex[best_pd]
                                       : static_cast<std::uint8_t>(best_pd - lo);
        params_.pg[sub] = static_cast<std::uint8_t>(best_pg);
        params_.ci[sub] = static_cast<std::uint16_t>(best_ci);
        params_.cg[sub] = static_cast<std::uint8_t>(best_cg);
        prev_pd_index_ = best_pd;

        // The excitation the decoder will build, to keep both sides in step.
        const std::int8_t* code = stochastic_code(best_ci);
        const double cg = kStochasticGain[best_cg];
        std::array<double, kSub> exc{};
        for (std::size_t i = 0; i < kSub; ++i) {
            exc[i] = pg * best_ca[i] + cg * static_cast<double>(code[i]);
        }
        push_register(adaptive_, exc);
        std::array<double, kSub> discard{};
        all_pole(aw, exc, discard, h_mem_);
    }
    prev_lsp_ = cur_lsp;

    const Fs1016Bits bits = fs1016_pack(params_, sync_);
    sync_ = !sync_;  // 3.8.3: zero first, then alternate
    return bits;
}

}  // namespace revenant::decode
