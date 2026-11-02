// LPC-10 per FED-STD-1015. See lpc10.h for the document, the clauses used and
// every place the document leaves a choice to the implementer.

#include "core/decode/lpc10.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <numbers>

namespace revenant::decode {

namespace {

// ---------------------------------------------------------------------------
// 3.4.1: voiced pitch periods and their codes, read off the encoding table.
// ---------------------------------------------------------------------------
constexpr std::array<Lpc10PitchCode, 60> kPitchEncode{{
    {156, 76}, {152, 101}, {148, 100}, {144, 108}, {140, 104}, {136, 106},
    {132, 98}, {128, 114}, {124, 112}, {120, 113}, {116, 97},  {112, 99},
    {108, 67}, {104, 71},  {100, 70},  {96, 78},   {92, 74},   {88, 75},
    {84, 73},  {80, 77},   {78, 69},   {76, 85},   {74, 81},   {72, 83},
    {70, 82},  {68, 86},   {66, 84},   {64, 92},   {62, 88},   {60, 90},
    {58, 26},  {56, 58},   {54, 56},   {52, 60},   {50, 52},   {48, 54},
    {46, 50},  {44, 51},   {42, 49},   {40, 53},   {39, 37},   {38, 45},
    {37, 41},  {36, 43},   {35, 42},   {34, 46},   {33, 38},   {32, 39},
    {31, 7},   {30, 15},   {29, 14},   {28, 30},   {27, 22},   {26, 23},
    {25, 21},  {24, 29},   {23, 25},   {22, 27},   {21, 11},   {20, 19},
}};

// ---------------------------------------------------------------------------
// 3.4.2: the decoding table, code 0..127, transcribed separately from the
// encoding table so the two can be checked against each other.
// ---------------------------------------------------------------------------
constexpr int U = kLpc10PitchU;
constexpr int T = kLpc10PitchT;
constexpr int I = kLpc10PitchI;
constexpr std::array<int, 128> kPitchDecode{
    U,   U,   U,   I,   U,   I,   I,   31,  U,   I,   I,   21,  I,   I,   29,  30,   // 0-15
    U,   I,   I,   20,  I,   25,  27,  26,  I,   23,  58,  22,  I,   24,  28,  I,    // 16-31
    U,   I,   I,   I,   I,   39,  33,  32,  I,   37,  35,  36,  I,   38,  34,  I,    // 32-47
    I,   42,  46,  44,  50,  40,  48,  I,   54,  I,   56,  I,   52,  I,   I,   T,    // 48-63
    U,   I,   I,   108, I,   78,  100, 104, I,   84,  92,  88,  156, 80,  96,  I,    // 64-79
    I,   74,  70,  72,  66,  76,  68,  I,   62,  I,   60,  I,   64,  I,   I,   T,    // 80-95
    I,   116, 132, 112, 148, 152, I,   I,   140, I,   136, I,   144, I,   I,   T,    // 96-111
    124, 120, 128, I,   I,   I,   I,   T,   I,   I,   I,   T,   I,   T,   T,   T,    // 112-127
};

// ---------------------------------------------------------------------------
// 3.5: RMS. Lower edge of each code's input range, and the decoded value.
// ---------------------------------------------------------------------------
constexpr std::array<std::int16_t, 32> kRmsFrom{
    0,  1,  2,  3,  4,  5,  6,  7,   8,   9,   11,  13,  15,  18,  22,  26,
    31, 36, 43, 52, 61, 73, 87, 104, 124, 148, 177, 211, 252, 301, 360, 429};
constexpr std::array<std::int16_t, 32> kRmsDecode{
    0,  1,  2,  3,  4,  5,  6,  7,   8,   9,   11,  13,  16,  19,  23,  27,
    32, 39, 46, 55, 66, 79, 94, 113, 135, 164, 192, 230, 275, 328, 392, 468};

// ---------------------------------------------------------------------------
// 3.6.1 to 3.6.9: for each coefficient the lowest index, then the "From"
// column and the "Decode" column in index order.
// ---------------------------------------------------------------------------
struct RcTable {
    int bits;
    int min_index;
    std::span<const double> from;
    std::span<const double> decode;
};

// 3.6.1, RC1 and RC2, index -15..15 (code 10000 is not listed; note 6).
constexpr std::array<double, 31> kRc12From{
    -.9999, -.9843, -.9687, -.9530, -.9374, -.9062, -.8749, -.8280, -.7655, -.6874, -.6093,
    -.5312, -.4218, -.3124, -.2031, -.0937, .0938,  .2032,  .3125,  .4219,  .5313,  .6094,
    .6875,  .7656,  .8281,  .8750,  .9063,  .9375,  .9531,  .9688,  .9844};
constexpr std::array<double, 31> kRc12Decode{
    -.9844, -.9688, -.9531, -.9375, -.9218, -.8906, -.8438, -.7812, -.7187, -.6406, -.5625,
    -.4688, -.3593, -.2500, -.1406, .0313,  .1406,  .2500,  .3593,  .4688,  .5625,  .6406,
    .7187,  .7812,  .8438,  .8906,  .9218,  .9375,  .9531,  .9688,  .9844};

// 3.6.2, RC3, index -16..15.
constexpr std::array<double, 32> kRc3From{
    -.9999, -.5890, -.5455, -.5018, -.4582, -.4147, -.3711, -.3275, -.2839, -.2403, -.1966,
    -.1531, -.1095, -.0659, -.0223, .0213,  .0649,  .1140,  .1576,  .2012,  .2448,  .2884,
    .3319,  .3756,  .4192,  .4627,  .5063,  .5500,  .5935,  .6371,  .6807,  .7243};
constexpr std::array<double, 32> kRc3Decode{
    -.6033, -.5598, -.5164, -.4729, -.4295, -.3860, -.3426, -.2991, -.2557, -.2122, -.1687,
    -.1253, -.0818, -.0384, .0051,  .0485,  .0920,  .1355,  .1789,  .2224,  .2658,  .3093,
    .3527,  .3962,  .4396,  .4831,  .5266,  .5700,  .6135,  .6569,  .7004,  .7438};

// 3.6.3, RC4, index -16..15.
constexpr std::array<double, 32> kRc4From{
    -.9999, -.7626, -.7236, -.6845, -.6454, -.6064, -.5673, -.5282, -.4892, -.4501, -.4111,
    -.3720, -.3329, -.2939, -.2548, -.2157, -.1767, -.1328, -.0937, -.0547, -.0156, .0235,
    .0625,  .1016,  .1406,  .1797,  .2188,  .2578,  .2969,  .3360,  .3750,  .4141};
constexpr std::array<double, 32> kRc4Decode{
    -.7774, -.7383, -.6993, -.6602, -.6211, -.5821, -.5430, -.5040, -.4649, -.4258, -.3868,
    -.3477, -.3086, -.2696, -.2305, -.1914, -.1524, -.1133, -.0743, -.0352, .0039,  .0429,
    .0820,  .1211,  .1601,  .1992,  .2382,  .2773,  .3164,  .3554,  .3945,  .4336};

// 3.6.4 to 3.6.7, RC5..RC8, index -8..7.
constexpr std::array<double, 16> kRc5From{-.9999, -.6046, -.5323, -.4599, -.3876, -.3152,
                                          -.2429, -.1706, -.0983, -.0213, .0510,  .1233,
                                          .1956,  .2680,  .3404,  .4127};
constexpr std::array<double, 16> kRc5Decode{-.6358, -.5635, -.4912, -.4190, -.3467, -.2744,
                                            -.2022, -.1299, -.0577, .0146,  .0869,  .1591,
                                            .2314,  .3037,  .3759,  .4482};
constexpr std::array<double, 16> kRc6From{-.9999, -.7010, -.6327, -.5644, -.4961, -.4278,
                                          -.3595, -.2912, -.2229, -.1504, -.0821, -.0138,
                                          .0545,  .1228,  .1911,  .2594};
constexpr std::array<double, 16> kRc6Decode{-.7315, -.6631, -.5948, -.5264, -.4581, -.3897,
                                            -.3213, -.2530, -.1846, -.1162, -.0479, .0205,
                                            .0888,  .1572,  .2256,  .2939};
constexpr std::array<double, 16> kRc7From{-.9999, -.5473, -.4808, -.4144, -.3479, -.2815,
                                          -.2151, -.1487, -.0823, -.0116, .0548,  .1212,
                                          .1876,  .2541,  .3205,  .3870};
constexpr std::array<double, 16> kRc7Decode{-.5762, -.5098, -.4434, -.3770, -.3106, -.2442,
                                            -.1778, -.1114, -.0450, .0214,  .0878,  .1542,
                                            .2206,  .2870,  .3534,  .4198};
constexpr std::array<double, 16> kRc8From{-.9999, -.6241, -.5567, -.4894, -.4220, -.3547,
                                          -.2873, -.2200, -.1526, -.0810, -.0137, .0537,
                                          .1210,  .1884,  .2557,  .3231};
constexpr std::array<double, 16> kRc8Decode{-.6539, -.5865, -.5191, -.4517, -.3843, -.3169,
                                            -.2496, -.1822, -.1148, -.0474, .0200,  .0874,
                                            .1548,  .2222,  .2895,  .3569};

// 3.6.8, RC9, index -4..3. 3.6.9, RC10, index -2..1.
constexpr std::array<double, 8> kRc9From{-.9999, -.3076, -.1907, -.0737,
                                         .0433,  .1639,  .2808,  .3978};
constexpr std::array<double, 8> kRc9Decode{-.3634, -.2462, -.1290, -.0118,
                                           .1054,  .2226,  .3398,  .4570};
constexpr std::array<double, 4> kRc10From{-.9999, -.3116, -.1202, .0744};
constexpr std::array<double, 4> kRc10Decode{-.4043, -.2129, -.0215, .1699};

const std::array<RcTable, kLpc10Order>& rc_tables() {
    static const std::array<RcTable, kLpc10Order> tables{{
        {5, -15, kRc12From, kRc12Decode},
        {5, -15, kRc12From, kRc12Decode},
        {5, -16, kRc3From, kRc3Decode},
        {5, -16, kRc4From, kRc4Decode},
        {4, -8, kRc5From, kRc5Decode},
        {4, -8, kRc6From, kRc6Decode},
        {4, -8, kRc7From, kRc7Decode},
        {4, -8, kRc8From, kRc8Decode},
        {3, -4, kRc9From, kRc9Decode},
        {2, -2, kRc10From, kRc10Decode},
    }};
    return tables;
}

// 3.7: data nibble -> error control nibble, read from the hexadecimal table.
constexpr std::array<std::uint8_t, 16> kEc{0x0, 0x7, 0xB, 0xC, 0xD, 0xA, 0x6, 0x1,
                                           0xE, 0x9, 0x5, 0x2, 0x3, 0x4, 0x8, 0xF};

// 3.8.1: the 40 recommended excitation values.
constexpr std::array<std::int16_t, 40> kExcitation{
    249,  -262, 363,  -362, 100, 367, 79,  78,  10,  -277, -82, 376, 288, -65,
    -20,  138,  -62,  -315, -247, -78, -82, -123, -39, 65,  64,  19,  16,  32,
    19,   -15,  -29,  -21,  -18,  -27, -31, -22,  -12, -10, -10, -4};

// 3.2.3 and 3.8.4.
constexpr double kPreemphasis = 0.9375;
constexpr double kDeemphasis = 0.75;

// ---------------------------------------------------------------------------
// 3.3.3: the bit assignment. Field ids: P = 0, R = 1, RC(k) = 1 + k,
// error control on R = 12, error control on RC(k) = 12 + k (k = 1..4), the
// sync bit and the unused bit last. "Bit 0 = least significant bit of data,
// bit 5 = least significant bit of error control", so error control bit
// R-5 is bit 0 of the error control word.
// ---------------------------------------------------------------------------
enum : std::uint8_t {
    kP = 0,
    kR = 1,
    kEcR = 12,
    kSync = 20,
    kUnused = 21,
};
constexpr std::uint8_t rc(int k) { return static_cast<std::uint8_t>(1 + k); }
constexpr std::uint8_t ec(int k) { return static_cast<std::uint8_t>(12 + k); }

struct Slot {
    std::uint8_t field;
    std::uint8_t bit;
};

constexpr std::array<Slot, kLpc10FrameBits> kVoicedMap{{
    {rc(1), 0}, {rc(2), 0}, {rc(3), 0}, {kP, 0},     {kR, 0},     {rc(1), 1},  // 1-6
    {rc(2), 1}, {rc(3), 1}, {kP, 1},    {kR, 1},     {rc(1), 2},  {rc(4), 0},  // 7-12
    {rc(3), 2}, {kR, 2},    {kP, 2},    {rc(4), 1},  {rc(1), 3},  {rc(2), 2},  // 13-18
    {rc(3), 3}, {rc(4), 2}, {kR, 3},    {rc(1), 4},  {rc(2), 3},  {rc(3), 4},  // 19-24
    {rc(4), 3}, {kR, 4},    {kP, 3},    {rc(2), 4},  {rc(7), 0},  {rc(8), 0},  // 25-30
    {kP, 4},    {rc(4), 4}, {rc(5), 0}, {rc(6), 0},  {rc(7), 1},  {rc(10), 0}, // 31-36
    {rc(8), 1}, {rc(5), 1}, {rc(6), 1}, {rc(7), 2},  {rc(9), 0},  {kP, 5},     // 37-42
    {rc(5), 2}, {rc(6), 2}, {rc(10), 1}, {rc(8), 2}, {kP, 6},     {rc(9), 1},  // 43-48
    {rc(5), 3}, {rc(6), 3}, {rc(7), 3}, {rc(9), 2},  {rc(8), 3},  {kSync, 0},  // 49-54
}};

constexpr std::array<Slot, kLpc10FrameBits> kNonvoicedMap{{
    {rc(1), 0}, {rc(2), 0}, {rc(3), 0}, {kP, 0},     {kR, 0},     {rc(1), 1},  // 1-6
    {rc(2), 1}, {rc(3), 1}, {kP, 1},    {kR, 1},     {rc(1), 2},  {rc(4), 0},  // 7-12
    {rc(3), 2}, {kR, 2},    {kP, 2},    {rc(4), 1},  {rc(1), 3},  {rc(2), 2},  // 13-18
    {rc(3), 3}, {rc(4), 2}, {kR, 3},    {rc(1), 4},  {rc(2), 3},  {rc(3), 4},  // 19-24
    {rc(4), 3}, {kR, 4},    {kP, 3},    {rc(2), 4},  {ec(3), 0},  {kEcR, 0},   // 25-30
    {kP, 4},    {rc(4), 4}, {ec(1), 0}, {ec(2), 0},  {ec(3), 1},  {ec(4), 0},  // 31-36
    {kEcR, 1},  {ec(1), 1}, {ec(2), 1}, {ec(3), 2},  {ec(4), 1},  {kP, 5},     // 37-42
    {ec(1), 2}, {ec(2), 2}, {kUnused, 0}, {kEcR, 2}, {kP, 6},     {ec(4), 2},  // 43-48
    {ec(1), 3}, {ec(2), 3}, {ec(3), 3}, {ec(4), 3},  {kEcR, 3},   {kSync, 0},  // 49-54
}};

constexpr std::size_t kFieldCount = 22;

// Reflection coefficients to direct-form predictor coefficients, the step-up
// recursion matching the Levinson-Durbin sign convention RC1 = R1/R0 (3.6.1):
// x[n] = e[n] + sum a[j] x[n-j].
std::array<double, kLpc10Order> step_up(const std::array<double, kLpc10Order>& k) {
    std::array<double, kLpc10Order> a{};
    std::array<double, kLpc10Order> prev{};
    for (std::size_t i = 0; i < kLpc10Order; ++i) {
        prev = a;
        a[i] = k[i];
        for (std::size_t j = 0; j < i; ++j) {
            a[j] = prev[j] - k[i] * prev[i - 1 - j];
        }
    }
    return a;
}

int sign_extend(std::uint32_t raw, int bits) {
    const std::uint32_t sign = 1U << static_cast<unsigned>(bits - 1);
    return static_cast<int>(raw ^ sign) - static_cast<int>(sign);
}

}  // namespace

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------

std::span<const Lpc10PitchCode> lpc10_pitch_encode_table() noexcept { return kPitchEncode; }
std::span<const int> lpc10_pitch_decode_table() noexcept { return kPitchDecode; }

int lpc10_rms_decode(int code) noexcept {
    return kRmsDecode[static_cast<std::size_t>(std::clamp(code, 0, 31))];
}

int lpc10_rms_encode(double rms) noexcept {
    const double v = std::clamp(std::round(rms), 0.0, 511.0);
    int code = 0;
    for (int i = 0; i < 32; ++i) {
        if (v >= kRmsFrom[static_cast<std::size_t>(i)]) code = i;
    }
    return code;
}

std::span<const std::int16_t> lpc10_rms_from_table() noexcept { return kRmsFrom; }

int lpc10_rc_bits(int k) noexcept { return rc_tables()[static_cast<std::size_t>(k - 1)].bits; }
int lpc10_rc_min_index(int k) noexcept {
    return rc_tables()[static_cast<std::size_t>(k - 1)].min_index;
}
int lpc10_rc_max_index(int k) noexcept {
    const auto& t = rc_tables()[static_cast<std::size_t>(k - 1)];
    return t.min_index + static_cast<int>(t.decode.size()) - 1;
}

double lpc10_rc_decode(int k, int index) noexcept {
    const auto& t = rc_tables()[static_cast<std::size_t>(k - 1)];
    const int i = std::clamp(index - t.min_index, 0, static_cast<int>(t.decode.size()) - 1);
    return t.decode[static_cast<std::size_t>(i)];
}

double lpc10_rc_from(int k, int index) noexcept {
    const auto& t = rc_tables()[static_cast<std::size_t>(k - 1)];
    const int i = std::clamp(index - t.min_index, 0, static_cast<int>(t.from.size()) - 1);
    return t.from[static_cast<std::size_t>(i)];
}

int lpc10_rc_encode(int k, double value) noexcept {
    const auto& t = rc_tables()[static_cast<std::size_t>(k - 1)];
    // The tables print four decimals; the "From" edge is inclusive.
    const double v = std::round(value * 10000.0) / 10000.0;
    int index = 0;
    for (std::size_t i = 0; i < t.from.size(); ++i) {
        if (v >= t.from[i] - 1e-9) index = static_cast<int>(i);
    }
    return t.min_index + index;
}

std::uint8_t lpc10_ec_encode(std::uint8_t nibble) noexcept { return kEc[nibble & 0xFU]; }

std::span<const std::int16_t> lpc10_excitation() noexcept { return kExcitation; }

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

Status lpc10_pack(const Lpc10Frame& frame, std::uint8_t sync, std::span<std::uint8_t> bits) {
    if (bits.size() < kLpc10FrameBits) {
        return fail(std::format("LPC-10 pack needs {} bits of room, got {}", kLpc10FrameBits,
                                bits.size()));
    }
    std::array<std::uint32_t, kFieldCount> value{};

    // 3.4.1: seven ZEROs unvoiced, seven ONEs transition, else the table.
    if (frame.voicing == Lpc10Voicing::Unvoiced) {
        value[kP] = 0;
    } else if (frame.voicing == Lpc10Voicing::Transition) {
        value[kP] = 0x7F;
    } else {
        const auto it = std::ranges::find(kPitchEncode, frame.pitch_period, &Lpc10PitchCode::period);
        if (it == kPitchEncode.end()) {
            return fail(std::format("LPC-10 pitch period {} is not one of the 60 in "
                                    "FED-STD-1015 3.4.1",
                                    frame.pitch_period));
        }
        value[kP] = it->code;
    }
    value[kR] = static_cast<std::uint32_t>(std::clamp(frame.rms_code, 0, 31));
    for (int k = 1; k <= static_cast<int>(kLpc10Order); ++k) {
        const int idx = std::clamp(frame.rc[static_cast<std::size_t>(k - 1)],
                                   lpc10_rc_min_index(k), lpc10_rc_max_index(k));
        value[rc(k)] = static_cast<std::uint32_t>(idx) & ((1U << lpc10_rc_bits(k)) - 1U);
    }

    const bool voiced = frame.voicing == Lpc10Voicing::Voiced;
    if (!voiced) {
        // 3.7: the four most significant bits of R and RC(1)..RC(4).
        value[kEcR] = kEc[(value[kR] >> 1U) & 0xFU];
        for (int k = 1; k <= 4; ++k) {
            value[ec(k)] = kEc[(value[rc(k)] >> 1U) & 0xFU];
        }
    }
    value[kSync] = sync & 1U;

    const auto& map = voiced ? kVoicedMap : kNonvoicedMap;
    for (std::size_t i = 0; i < kLpc10FrameBits; ++i) {
        bits[i] = static_cast<std::uint8_t>((value[map[i].field] >> map[i].bit) & 1U);
    }
    return {};
}

Expected<Lpc10Unpacked> lpc10_unpack(std::span<const std::uint8_t> bits) {
    if (bits.size() < kLpc10FrameBits) {
        return fail(std::format("LPC-10 unpack needs {} bits, got {}", kLpc10FrameBits,
                                bits.size()));
    }
    // The pitch field sits in the same seven positions in both maps, so read
    // it first to learn which map the rest of the frame follows.
    std::uint32_t pitch_code = 0;
    for (std::size_t i = 0; i < kLpc10FrameBits; ++i) {
        if (kVoicedMap[i].field == kP) {
            pitch_code |= static_cast<std::uint32_t>(bits[i] & 1U) << kVoicedMap[i].bit;
        }
    }
    Lpc10Unpacked out;
    const int decoded = kPitchDecode[pitch_code];
    const bool voiced = decoded > 0;
    if (decoded == kLpc10PitchU) out.frame.voicing = Lpc10Voicing::Unvoiced;
    if (decoded == kLpc10PitchT) out.frame.voicing = Lpc10Voicing::Transition;
    if (decoded == kLpc10PitchI) {
        out.pitch_invalid = true;
        out.frame.voicing = Lpc10Voicing::Unvoiced;
    }
    if (voiced) {
        out.frame.voicing = Lpc10Voicing::Voiced;
        out.frame.pitch_period = decoded;
    }

    std::array<std::uint32_t, kFieldCount> value{};
    const auto& map = voiced ? kVoicedMap : kNonvoicedMap;
    for (std::size_t i = 0; i < kLpc10FrameBits; ++i) {
        value[map[i].field] |= static_cast<std::uint32_t>(bits[i] & 1U) << map[i].bit;
    }

    if (!voiced && !out.pitch_invalid) {
        // 3.7: correct the four most significant bits of each protected field
        // from the nearest of the sixteen 8-bit codewords. The code's minimum
        // distance is 4, so one error is corrected and two are detected.
        auto correct = [&](std::uint8_t data_field, std::uint8_t ec_field) {
            const std::uint32_t data = (value[data_field] >> 1U) & 0xFU;
            const std::uint32_t check = value[ec_field] & 0xFU;
            int best = 99;
            int ties = 0;
            std::uint32_t best_data = data;
            for (std::uint32_t cand = 0; cand < 16; ++cand) {
                const int d = std::popcount(cand ^ data) + std::popcount(kEc[cand] ^ check);
                if (d < best) {
                    best = d;
                    best_data = cand;
                    ties = 1;
                } else if (d == best) {
                    ++ties;
                }
            }
            if (best == 0) return;
            if (best == 1 && ties == 1) {
                ++out.ec_corrected;
                value[data_field] = (value[data_field] & 1U) | (best_data << 1U);
            } else {
                ++out.ec_uncorrectable;
            }
        };
        correct(kR, kEcR);
        for (int k = 1; k <= 4; ++k) correct(rc(k), ec(k));
    }

    out.frame.rms_code = static_cast<int>(value[kR]);
    const int last = voiced ? static_cast<int>(kLpc10Order) : 4;
    for (int k = 1; k <= last; ++k) {
        out.frame.rc[static_cast<std::size_t>(k - 1)] =
            sign_extend(value[rc(k)], lpc10_rc_bits(k));
    }
    out.sync = static_cast<std::uint8_t>(value[kSync] & 1U);
    return out;
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

Lpc10Decoder::Lpc10Decoder() { reset(); }

void Lpc10Decoder::reset() {
    previous_ = Decoded{};
    last_voiced_pitch_ = 60.0;
    have_previous_ = false;
    last_good_ = Lpc10Frame{};
    have_last_good_ = false;
    pending_.clear();
    excitation_tail_.clear();
    filter_memory_.fill(0.0);
    deemphasis_ = 0.0;
    noise_ = 0x1234567U;
    report_ = Lpc10FrameReport{};
}

Status Lpc10Decoder::decode(std::span<const std::uint8_t> bits, std::span<float> out) {
    if (bits.size() != kLpc10FrameBits) {
        return fail(std::format("LPC-10 decode wants a {}-bit frame, got {} bits; frame the "
                                "stream with Lpc10FrameSync first",
                                kLpc10FrameBits, bits.size()));
    }
    if (out.size() < kLpc10FrameSamples) {
        return fail(std::format("LPC-10 decode writes {} samples, the output holds {}",
                                kLpc10FrameSamples, out.size()));
    }
    auto unpacked = lpc10_unpack(bits);
    if (!unpacked) return std::unexpected(unpacked.error());

    // Note 5 in the header: an invalid pitch code or an uncorrectable error
    // control word repeats the previous frame.
    Lpc10Frame frame = unpacked->frame;
    const bool repeat = unpacked->pitch_invalid || unpacked->ec_uncorrectable > 0;
    if (repeat) {
        frame = have_last_good_ ? last_good_ : Lpc10Frame{};
    } else {
        last_good_ = frame;
        have_last_good_ = true;
    }
    if (auto s = synthesize(frame, out); !s) return s;
    report_.repeated = repeat;
    report_.ec_corrected = unpacked->ec_corrected;
    report_.ec_uncorrectable = unpacked->ec_uncorrectable;
    report_.sync = unpacked->sync;
    return {};
}

Status Lpc10Decoder::synthesize(const Lpc10Frame& frame, std::span<float> out) {
    if (out.size() < kLpc10FrameSamples) {
        return fail(std::format("LPC-10 synthesis writes {} samples, the output holds {}",
                                kLpc10FrameSamples, out.size()));
    }
    Decoded now;
    now.voicing = frame.voicing;
    now.rms = lpc10_rms_decode(frame.rms_code);
    const int order = frame.voicing == Lpc10Voicing::Voiced ? static_cast<int>(kLpc10Order) : 4;
    for (int k = 1; k <= order; ++k) {
        now.rc[static_cast<std::size_t>(k - 1)] =
            lpc10_rc_decode(k, frame.rc[static_cast<std::size_t>(k - 1)]);
    }
    if (frame.voicing == Lpc10Voicing::Voiced) {
        now.pitch = std::clamp(frame.pitch_period, 20, 156);
        last_voiced_pitch_ = now.pitch;
    } else {
        now.pitch = last_voiced_pitch_;
    }
    if (!have_previous_) {
        previous_ = now;
        have_previous_ = true;
    }
    run(now, out);
    previous_ = now;

    report_ = Lpc10FrameReport{};
    report_.voicing = frame.voicing;
    report_.pitch_period = frame.voicing == Lpc10Voicing::Voiced ? frame.pitch_period : 0;
    report_.rms = static_cast<int>(now.rms);
    return {};
}

void Lpc10Decoder::run(const Decoded& now, std::span<float> out) {
    from_ = &previous_;
    to_ = &now;
    const bool previous_voiced = previous_.voicing == Lpc10Voicing::Voiced;
    while (pending_.size() < kLpc10FrameSamples) {
        const double frac =
            static_cast<double>(pending_.size()) / static_cast<double>(kLpc10FrameSamples);
        bool voiced = now.voicing == Lpc10Voicing::Voiced;
        if (now.voicing == Lpc10Voicing::Transition) {
            // Note 4: the voiced half comes first after a voiced frame.
            const bool first_half = frac < 0.5;
            voiced = previous_voiced ? first_half : !first_half;
        }
        int length = 45;
        if (voiced) {
            double pitch = now.pitch;
            if (previous_voiced && now.voicing == Lpc10Voicing::Voiced) {
                pitch = previous_.pitch + (now.pitch - previous_.pitch) * frac;
            }
            length = static_cast<int>(std::lround(pitch));
        }
        emit_epoch(length, voiced, frac);
    }
    for (std::size_t i = 0; i < kLpc10FrameSamples; ++i) out[i] = static_cast<float>(pending_[i]);
    pending_.erase(pending_.begin(),
                   pending_.begin() + static_cast<std::ptrdiff_t>(kLpc10FrameSamples));
}

void Lpc10Decoder::emit_epoch(int length, bool voiced, double frac) {
    // Note 2: parameters interpolated linearly to the epoch's start.
    std::array<double, kLpc10Order> k{};
    for (std::size_t i = 0; i < kLpc10Order; ++i) {
        k[i] = from_->rc[i] + (to_->rc[i] - from_->rc[i]) * frac;
    }
    if (!voiced) {
        // 3.8.2: unvoiced synthesis is 4th order.
        for (std::size_t i = 4; i < kLpc10Order; ++i) k[i] = 0.0;
    }
    const double rms = from_->rms + (to_->rms - from_->rms) * frac;
    double power_gain = 1.0;  // prod(1 - k^2), the inverse of the filter's power gain
    for (double ki : k) power_gain *= std::max(1e-6, 1.0 - ki * ki);
    const auto a = step_up(k);

    const auto n = static_cast<std::size_t>(length);
    std::vector<double> e(n, 0.0);
    // The overlapped part of the previous epoch's excitation (3.8.1).
    const std::size_t carry = std::min(n, excitation_tail_.size());
    for (std::size_t i = 0; i < carry; ++i) e[i] += excitation_tail_[i];
    excitation_tail_.erase(excitation_tail_.begin(),
                           excitation_tail_.begin() + static_cast<std::ptrdiff_t>(carry));

    if (voiced) {
        // 3.8.1: all 40 values every epoch; a period shorter than 40 pushes the
        // remainder, already at this epoch's level, into the next epoch.
        double energy = 0.0;
        for (std::int16_t v : kExcitation) energy += static_cast<double>(v) * v;
        const double scale = rms * std::sqrt(power_gain * static_cast<double>(n) / energy);
        for (std::size_t i = 0; i < kExcitation.size(); ++i) {
            const double v = scale * kExcitation[i];
            if (i < n) {
                e[i] += v;
            } else {
                const std::size_t j = i - n;
                if (excitation_tail_.size() <= j) excitation_tail_.resize(j + 1, 0.0);
                excitation_tail_[j] += v;
            }
        }
    } else {
        // 3.8.2: pseudorandom excitation, uniform with unit variance.
        const double scale = rms * std::sqrt(power_gain);
        for (std::size_t i = 0; i < n; ++i) {
            noise_ = noise_ * 1664525U + 1013904223U;
            const double u = static_cast<double>(noise_ >> 8U) / 16777216.0;
            e[i] += scale * (u - 0.5) * std::sqrt(12.0);
        }
    }

    for (std::size_t i = 0; i < n; ++i) {
        double y = e[i];
        for (std::size_t j = 0; j < kLpc10Order; ++j) y += a[j] * filter_memory_[j];
        for (std::size_t j = kLpc10Order - 1; j > 0; --j) filter_memory_[j] = filter_memory_[j - 1];
        filter_memory_[0] = y;
        // 3.8.4: de-emphasis 1 / (1 - 0.75 z^-1).
        deemphasis_ = y + kDeemphasis * deemphasis_;
        pending_.push_back(deemphasis_ / kLpc10FullScale);
    }
}

std::string Lpc10Decoder::describe_last_frame() const {
    const char* v = report_.voicing == Lpc10Voicing::Voiced       ? "voiced"
                    : report_.voicing == Lpc10Voicing::Transition ? "transition"
                                                                  : "unvoiced";
    return std::format("LPC-10 {} pitch {} rms {}{} ec corrected {} uncorrectable {} sync {}", v,
                       report_.pitch_period, report_.rms, report_.repeated ? " REPEATED" : "",
                       report_.ec_corrected, report_.ec_uncorrectable, report_.sync);
}

// ---------------------------------------------------------------------------
// Encoder: this project's own analysis (header note 1).
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kLpcWindow = 240;
constexpr std::size_t kPitchHistory = 180 + 156 + 4;
constexpr int kMinLag = 20;
constexpr int kMaxLag = 156;

// A 2nd-order Butterworth lowpass at 1 kHz, run twice, for the pitch copy.
// Coefficients from the bilinear transform with prewarping.
struct Biquad {
    double b0, b1, b2, a1, a2;
};
Biquad lowpass_1k() {
    const double w = std::tan(std::numbers::pi * 1000.0 / 8000.0);
    const double q = std::numbers::sqrt2;
    const double norm = 1.0 / (1.0 + q * w + w * w);
    return {w * w * norm, 2.0 * w * w * norm, w * w * norm, 2.0 * (w * w - 1.0) * norm,
            (1.0 - q * w + w * w) * norm};
}

double normalised_correlation(const std::vector<double>& x, std::size_t start, std::size_t len,
                              int lag) {
    double xy = 0.0;
    double xx = 0.0;
    double yy = 0.0;
    for (std::size_t n = start; n < start + len; ++n) {
        const double a = x[n];
        const double b = x[n - static_cast<std::size_t>(lag)];
        xy += a * b;
        xx += a * a;
        yy += b * b;
    }
    if (xx <= 0.0 || yy <= 0.0) return 0.0;
    return xy / std::sqrt(xx * yy);
}

}  // namespace

Lpc10Encoder::Lpc10Encoder() { reset(); }

void Lpc10Encoder::reset() {
    history_.assign(kLpcWindow, 0.0);
    lowpass_.assign(kPitchHistory, 0.0);
    preemphasis_state_ = 0.0;
    lowpass_state_.fill(0.0);
    sync_ = 0;
    last_rc_.fill(0.0);
}

Expected<Lpc10Frame> Lpc10Encoder::analyse(std::span<const float> pcm) {
    if (pcm.size() != kLpc10FrameSamples) {
        return fail(std::format("LPC-10 encode takes {} samples, got {}", kLpc10FrameSamples,
                                pcm.size()));
    }
    static const Biquad lp = lowpass_1k();

    // 3.2.3 preemphasis, on the 12-bit scale of 3.2.2.
    std::vector<double> pre(kLpc10FrameSamples);
    std::vector<double> low(kLpc10FrameSamples);
    for (std::size_t i = 0; i < kLpc10FrameSamples; ++i) {
        const double x = static_cast<double>(pcm[i]) * kLpc10FullScale;
        pre[i] = x - kPreemphasis * preemphasis_state_;
        preemphasis_state_ = x;
        // Two cascaded biquads, transposed direct form II.
        double v = x;
        for (int stage = 0; stage < 2; ++stage) {
            double& s1 = lowpass_state_[static_cast<std::size_t>(stage * 2)];
            double& s2 = lowpass_state_[static_cast<std::size_t>(stage * 2 + 1)];
            const double y = lp.b0 * v + s1;
            s1 = lp.b1 * v - lp.a1 * y + s2;
            s2 = lp.b2 * v - lp.a2 * y;
            v = y;
        }
        low[i] = v;
    }
    history_.erase(history_.begin(),
                   history_.begin() + static_cast<std::ptrdiff_t>(kLpc10FrameSamples));
    history_.insert(history_.end(), pre.begin(), pre.end());
    lowpass_.erase(lowpass_.begin(),
                   lowpass_.begin() + static_cast<std::ptrdiff_t>(kLpc10FrameSamples));
    lowpass_.insert(lowpass_.end(), low.begin(), low.end());

    Lpc10Frame frame;

    // RMS of this frame's preemphasized speech (3.5).
    double energy = 0.0;
    for (double v : pre) energy += v * v;
    const double rms = std::sqrt(energy / static_cast<double>(kLpc10FrameSamples));
    frame.rms_code = lpc10_rms_encode(rms);

    // Reflection coefficients: Hamming-windowed autocorrelation and
    // Levinson-Durbin, with the RC1 = R1/R0 sign of 3.6.1.
    std::array<double, kLpc10Order + 1> r{};
    std::vector<double> w(kLpcWindow);
    for (std::size_t n = 0; n < kLpcWindow; ++n) {
        const double h = 0.54 - 0.46 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) /
                                                static_cast<double>(kLpcWindow - 1));
        w[n] = history_[n] * h;
    }
    for (std::size_t lag = 0; lag <= kLpc10Order; ++lag) {
        for (std::size_t n = lag; n < kLpcWindow; ++n) r[lag] += w[n] * w[n - lag];
    }
    r[0] = r[0] * (1.0 + 1e-6) + 1e-3;  // white-noise floor keeps silence stable
    std::array<double, kLpc10Order> a{};
    std::array<double, kLpc10Order> k{};
    double err = r[0];
    for (std::size_t i = 0; i < kLpc10Order; ++i) {
        double acc = r[i + 1];
        for (std::size_t j = 0; j < i; ++j) acc -= a[j] * r[i - j];
        const double ki = std::clamp(acc / err, -0.9999, 0.9999);
        k[i] = ki;
        const auto prev = a;
        a[i] = ki;
        for (std::size_t j = 0; j < i; ++j) a[j] = prev[j] - ki * prev[i - 1 - j];
        err *= (1.0 - ki * ki);
    }
    last_rc_ = k;

    // Pitch and voicing, on the lowpassed copy. A half frame is voiced when
    // its normalised autocorrelation peak passes 0.5; the pitch is the
    // shortest lag whose correlation is within 10% of the best, which keeps
    // the tracker off multiples of the period.
    const std::size_t frame_start = kPitchHistory - kLpc10FrameSamples;
    auto best_lag = [&](std::size_t start, std::size_t len, double& peak) {
        std::array<double, kMaxLag + 1> c{};
        peak = 0.0;
        for (int lag = kMinLag; lag <= kMaxLag; ++lag) {
            c[static_cast<std::size_t>(lag)] = normalised_correlation(lowpass_, start, len, lag);
            peak = std::max(peak, c[static_cast<std::size_t>(lag)]);
        }
        for (int lag = kMinLag + 1; lag < kMaxLag; ++lag) {
            const double v = c[static_cast<std::size_t>(lag)];
            if (v >= 0.9 * peak && v >= c[static_cast<std::size_t>(lag - 1)] &&
                v >= c[static_cast<std::size_t>(lag + 1)]) {
                return lag;
            }
        }
        return kMinLag;
    };
    double peak_full = 0.0;
    double peak_a = 0.0;
    double peak_b = 0.0;
    const int lag = best_lag(frame_start, kLpc10FrameSamples, peak_full);
    (void)best_lag(frame_start, kLpc10FrameSamples / 2, peak_a);
    (void)best_lag(frame_start + kLpc10FrameSamples / 2, kLpc10FrameSamples / 2, peak_b);
    const bool loud = rms > 2.0;
    const bool va = loud && peak_a > 0.5;
    const bool vb = loud && peak_b > 0.5;
    if (va && vb) {
        frame.voicing = Lpc10Voicing::Voiced;
    } else if (va || vb) {
        frame.voicing = Lpc10Voicing::Transition;
    } else {
        frame.voicing = Lpc10Voicing::Unvoiced;
    }
    if (frame.voicing == Lpc10Voicing::Voiced) {
        int best = kPitchEncode[0].period;
        for (const auto& p : kPitchEncode) {
            if (std::abs(p.period - lag) < std::abs(best - lag)) best = p.period;
        }
        frame.pitch_period = best;
    }

    const int order = frame.voicing == Lpc10Voicing::Voiced ? static_cast<int>(kLpc10Order) : 4;
    for (int i = 1; i <= order; ++i) {
        frame.rc[static_cast<std::size_t>(i - 1)] =
            lpc10_rc_encode(i, k[static_cast<std::size_t>(i - 1)]);
    }
    return frame;
}

Status Lpc10Encoder::encode(std::span<const float> pcm, std::span<std::uint8_t> bits) {
    auto frame = analyse(pcm);
    if (!frame) return std::unexpected(frame.error());
    if (auto s = lpc10_pack(*frame, sync_, bits); !s) return s;
    sync_ ^= 1U;  // 3.3.4
    return {};
}

// ---------------------------------------------------------------------------
// Frame synchronisation
// ---------------------------------------------------------------------------

void Lpc10FrameSync::reset() {
    window_.clear();
    run_.fill(0);
    seen_ = 0;
    locked_ = false;
    phase_ = 0;
    misses_ = 0;
}

void Lpc10FrameSync::push(std::span<const std::uint8_t> bits,
                          std::vector<std::array<std::uint8_t, kLpc10FrameBits>>& frames) {
    for (std::uint8_t b : bits) {
        window_.push_back(static_cast<std::uint8_t>(b & 1U));
        if (window_.size() > kLpc10FrameBits + 1) window_.pop_front();
        ++seen_;
        if (window_.size() < kLpc10FrameBits + 1) continue;

        // Treat the newest bit as a candidate sync bit and the one 54 earlier
        // as the previous frame's.
        const auto phase = static_cast<std::size_t>(seen_ % kLpc10FrameBits);
        const bool alternated = window_.back() != window_.front();
        int& run = run_[phase];
        run = alternated ? run + 1 : 0;

        if (locked_ && phase == phase_) {
            std::array<std::uint8_t, kLpc10FrameBits> frame{};
            std::copy(window_.begin() + 1, window_.end(), frame.begin());
            frames.push_back(frame);
            misses_ = alternated ? 0 : misses_ + 1;
            if (misses_ >= kLoseFrames) {
                locked_ = false;
                misses_ = 0;
            }
        } else if (!locked_ && run >= kLockFrames) {
            bool ahead = true;
            for (std::size_t p = 0; p < kLpc10FrameBits; ++p) {
                if (p != phase && run_[p] >= run) ahead = false;
            }
            if (ahead) {
                locked_ = true;
                phase_ = phase;
                misses_ = 0;
                std::array<std::uint8_t, kLpc10FrameBits> frame{};
                std::copy(window_.begin() + 1, window_.end(), frame.begin());
                frames.push_back(frame);
            }
        }
    }
}

}  // namespace revenant::decode
