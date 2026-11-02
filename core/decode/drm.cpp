// DRM30 structure, coding and parsing. See drm.h for the specification; the
// receiver is in drm_rx.cpp.

#include "core/decode/drm.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>

#include "core/decode/dv_codes.h"

namespace revenant::decode {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

struct Range {
    int kmin;
    int kmax;
};

// ES table 49; {0, -1} marks a dash.
constexpr std::array<std::array<Range, 6>, 4> kCarriers = {{
    {{{2, 102}, {2, 114}, {-102, 102}, {-114, 114}, {-98, 314}, {-110, 350}}},
    {{{1, 91}, {1, 103}, {-91, 91}, {-103, 103}, {-87, 279}, {-99, 311}}},
    {{{0, -1}, {0, -1}, {0, -1}, {-69, 69}, {0, -1}, {-67, 213}}},
    {{{0, -1}, {0, -1}, {0, -1}, {-44, 44}, {0, -1}, {-43, 135}}},
}};

// ES table 59: gain references boosted to amplitude 2.
constexpr std::array<std::array<std::array<int, 4>, 6>, 4> kBoost = {{
    {{{2, 6, 98, 102}, {2, 6, 110, 114}, {-102, -98, 98, 102}, {-114, -110, 110, 114},
      {-98, -94, 310, 314}, {-110, -106, 346, 350}}},
    {{{1, 3, 89, 91}, {1, 3, 101, 103}, {-91, -89, 89, 91}, {-103, -101, 101, 103},
      {-87, -85, 277, 279}, {-99, -97, 309, 311}}},
    {{{}, {}, {}, {-69, -67, 67, 69}, {}, {-67, -65, 211, 213}}},
    {{{}, {}, {}, {-44, -43, 43, 44}, {}, {-43, -42, 134, 135}}},
}};

struct PhasePair {
    int k;
    int phase;
};

// ES table 52 (and the starred rows of tables 53 to 56).
constexpr std::array<std::array<PhasePair, 3>, 4> kFrequencyRefs = {{
    {{{18, 205}, {54, 836}, {72, 215}}},
    {{{16, 331}, {48, 651}, {64, 555}}},
    {{{11, 214}, {33, 392}, {44, 242}}},
    {{{7, 788}, {21, 1014}, {28, 332}}},
}};

// ES tables 53 to 56.
constexpr PhasePair kTimeA[] = {{17, 973}, {18, 205}, {19, 717}, {21, 264}, {28, 357}, {29, 357},
                                {32, 952}, {33, 440}, {39, 856}, {40, 88},  {41, 88},  {53, 68},
                                {54, 836}, {55, 836}, {56, 836}, {60, 1008}, {61, 1008}, {63, 752},
                                {71, 215}, {72, 215}, {73, 727}};
constexpr PhasePair kTimeB[] = {{14, 304}, {16, 331}, {18, 108}, {20, 620}, {24, 192},
                                {26, 704}, {32, 44},  {36, 432}, {42, 588}, {44, 844},
                                {48, 651}, {49, 651}, {50, 651}, {54, 460}, {56, 460},
                                {62, 944}, {64, 555}, {66, 940}, {68, 428}};
constexpr PhasePair kTimeC[] = {{8, 722},  {10, 466}, {11, 214}, {12, 214}, {14, 479},
                                {16, 516}, {18, 260}, {22, 577}, {24, 662}, {28, 3},
                                {30, 771}, {32, 392}, {33, 392}, {36, 37},  {38, 37},
                                {42, 474}, {44, 242}, {45, 242}, {46, 754}};
constexpr PhasePair kTimeD[] = {{5, 636},  {6, 124},  {7, 788},  {8, 788},  {9, 200},  {11, 688},
                                {12, 152}, {14, 920}, {15, 920}, {17, 644}, {18, 388}, {20, 652},
                                {21, 1014}, {23, 176}, {24, 176}, {26, 752}, {27, 496}, {28, 332},
                                {29, 432}, {30, 964}, {32, 452}};

std::span<const PhasePair> time_refs(DrmMode mode) {
    switch (mode) {
        case DrmMode::A: return kTimeA;
        case DrmMode::B: return kTimeB;
        case DrmMode::C: return kTimeC;
        case DrmMode::D: return kTimeD;
    }
    return {};
}

// ES 8.4.4.3.2 to 8.4.4.3.5: W1024[n][m], Z256[n][m] and Q1024.
constexpr int kWA[5][3] = {{228, 341, 455}, {455, 569, 683}, {683, 796, 910}, {910, 0, 114}, {114, 228, 341}};
constexpr int kZA[5][3] = {{0, 81, 248}, {18, 106, 106}, {122, 116, 31}, {129, 129, 39}, {33, 32, 111}};
constexpr int kWB[3][5] = {{512, 0, 512, 0, 512}, {0, 512, 0, 512, 0}, {512, 0, 512, 0, 512}};
constexpr int kZB[3][5] = {{0, 57, 164, 64, 12}, {168, 255, 161, 106, 118}, {25, 232, 132, 233, 38}};
constexpr int kWC[2][10] = {{465, 372, 279, 186, 93, 0, 931, 838, 745, 652},
                            {931, 838, 745, 652, 559, 465, 372, 279, 186, 93}};
constexpr int kZC[2][10] = {{0, 76, 29, 76, 9, 190, 161, 248, 33, 108},
                            {179, 178, 83, 253, 127, 105, 101, 198, 250, 145}};
constexpr int kWD[3][8] = {{366, 439, 512, 585, 658, 731, 805, 878},
                           {731, 805, 878, 951, 0, 73, 146, 219},
                           {73, 146, 219, 293, 366, 439, 512, 585}};
constexpr int kZD[3][8] = {{0, 240, 17, 60, 220, 38, 151, 101},
                           {110, 7, 78, 82, 175, 150, 106, 25},
                           {165, 7, 252, 124, 253, 177, 197, 142}};

int gain_phase(DrmMode mode, int n, int m, int p, int s) {
    int w = 0, z = 0, q = 0;
    switch (mode) {
        case DrmMode::A: w = kWA[n][m]; z = kZA[n][m]; q = 36; break;
        case DrmMode::B: w = kWB[n][m]; z = kZB[n][m]; q = 12; break;
        case DrmMode::C: w = kWC[n][m]; z = kZC[n][m]; q = 12; break;
        case DrmMode::D: w = kWD[n][m]; z = kZD[n][m]; q = 14; break;
    }
    // ES 8.4.4.3.1.
    const long long v = 4LL * z + static_cast<long long>(p) * w +
                        static_cast<long long>(p) * p * (1 + s) * q;
    return static_cast<int>(((v % 1024) + 1024) % 1024);
}

// ES tables 62 to 65, as {symbol, carriers}.
struct FacRow {
    int symbol;
    std::array<int, 8> k;
};
constexpr FacRow kFacA[] = {
    {2, {26, 46, 66, 86}},          {3, {10, 30, 50, 70, 90}},
    {4, {14, 22, 34, 62, 74, 94}},  {5, {26, 38, 58, 66, 78}},
    {6, {22, 30, 42, 62, 70, 82}},  {7, {26, 34, 46, 66, 74, 86}},
    {8, {10, 30, 38, 50, 58, 70, 78, 90}}, {9, {14, 22, 34, 42, 62, 74, 82, 94}},
    {10, {26, 38, 46, 66, 86}},     {11, {10, 30, 50, 70, 90}},
    {12, {14, 34, 74, 94}},         {13, {38, 58, 78}}};
constexpr FacRow kFacB[] = {
    {2, {13, 25, 43, 55, 67}},      {3, {15, 27, 45, 57, 69}},     {4, {17, 29, 47, 59, 71}},
    {5, {19, 31, 49, 61, 73}},      {6, {9, 21, 33, 51, 63, 75}},  {7, {11, 23, 35, 53, 65, 77}},
    {8, {13, 25, 37, 55, 67, 79}},  {9, {15, 27, 39, 57, 69, 81}}, {10, {17, 29, 41, 59, 71, 83}},
    {11, {19, 31, 43, 61, 73}},     {12, {21, 33, 45, 63, 75}},    {13, {23, 35, 47, 65, 77}}};
constexpr FacRow kFacC[] = {
    {3, {9, 21, 45, 57}},     {4, {23, 35, 47}},         {5, {13, 25, 37, 49}},
    {6, {15, 27, 39, 51}},    {7, {5, 17, 29, 41, 53}},  {8, {7, 19, 31, 43, 55}},
    {9, {9, 21, 45, 57}},     {10, {23, 35, 47}},        {11, {13, 25, 37, 49}},
    {12, {15, 27, 39, 51}},   {13, {5, 17, 29, 41, 53}}, {14, {7, 19, 31, 43, 55}},
    {15, {9, 21, 45, 57}},    {16, {23, 35, 47}},        {17, {13, 25, 37, 49}},
    {18, {15, 27, 39, 51}}};
constexpr FacRow kFacD[] = {
    {3, {9, 18, 27}},     {4, {10, 19}},         {5, {11, 20, 29}},     {6, {12, 30}},
    {7, {13, 22, 31}},    {8, {5, 14, 23, 32}},  {9, {6, 15, 24, 33}},  {10, {16, 25, 34}},
    {11, {8, 17, 26, 35}}, {12, {9, 18, 27, 36}}, {13, {10, 19, 37}},   {14, {11, 20, 29}},
    {15, {12, 30}},       {16, {13, 22, 31}},    {17, {5, 14, 23, 32}}, {18, {6, 15, 24, 33}},
    {19, {16, 25, 34}},   {20, {8, 17, 26, 35}}, {21, {9, 18, 27, 36}}, {22, {10, 19, 37}}};

std::span<const FacRow> fac_rows(DrmMode mode) {
    switch (mode) {
        case DrmMode::A: return kFacA;
        case DrmMode::B: return kFacB;
        case DrmMode::C: return kFacC;
        case DrmMode::D: return kFacD;
    }
    return {};
}

bool is_fac(DrmMode mode, int s, int k) {
    for (const FacRow& row : fac_rows(mode)) {
        if (row.symbol == s) {
            for (int c : row.k) {
                if (c == k && c != 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

Complex32 polar(double amplitude, int phase1024) {
    const double a = kTwoPi * phase1024 / 1024.0;
    return {static_cast<float>(amplitude * std::cos(a)), static_cast<float>(amplitude * std::sin(a))};
}

int positive_mod(int a, int m) { return ((a % m) + m) % m; }

void push_bits(std::vector<std::uint8_t>& out, std::uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (n - 1 - i)) & 1U));
    }
}

std::uint64_t read_bits(std::span<const std::uint8_t> bits, std::size_t& at, unsigned n) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) {
        v = (v << 1U) | (at < bits.size() ? (bits[at] & 1U) : 0U);
        ++at;
    }
    return v;
}

std::vector<std::uint8_t> bytes_to_bits(std::span<const std::uint8_t> bytes) {
    std::vector<std::uint8_t> bits;
    bits.reserve(bytes.size() * 8);
    for (std::uint8_t b : bytes) {
        push_bits(bits, b, 8);
    }
    return bits;
}

// ES 7.3.2: generators 133, 171, 145, 133, 171, 145 octal, written with the
// coefficient of D^0 in the least significant bit as dv_codes.h wants.
constexpr std::uint32_t kGenerators[6] = {0x6D, 0x4F, 0x53, 0x6D, 0x4F, 0x53};
const ConvolutionalCode kMotherCode{6, kGenerators};

// ES 7.3.3: interleaver constants per level.
std::optional<unsigned> level_t(DrmQam qam, std::size_t p) {
    switch (qam) {
        case DrmQam::Qam4: return 21U;
        case DrmQam::Qam16: return p == 0 ? 13U : 21U;
        case DrmQam::Qam64:
            if (p == 0) {
                return std::nullopt;
            }
            return p == 1 ? 13U : 21U;
    }
    return std::nullopt;
}

}  // namespace

// ---------------------------------------------------------------------------
// Structure
// ---------------------------------------------------------------------------

DrmModeParams drm_mode_params(DrmMode mode) {
    // ES table 47 at 48 kS/s: Tu of 24, 21 1/3, 14 2/3 and 9 1/3 ms and Tg/Tu
    // of 1/9, 1/4, 4/11 and 11/14; ES table 60 for x, y and k0.
    switch (mode) {
        case DrmMode::A: return {1152, 128, 15, 4, 5, 2};
        case DrmMode::B: return {1024, 256, 15, 2, 3, 1};
        case DrmMode::C: return {704, 256, 20, 2, 2, 1};
        case DrmMode::D: return {448, 352, 24, 1, 3, 1};
    }
    return {};
}

char drm_mode_letter(DrmMode mode) { return static_cast<char>('A' + static_cast<int>(mode)); }

bool drm_occupancy_valid(DrmMode mode, int occupancy) {
    if (occupancy < 0 || occupancy > 5) {
        return false;
    }
    const Range r = kCarriers[static_cast<std::size_t>(mode)][static_cast<std::size_t>(occupancy)];
    return r.kmax >= r.kmin;
}

Expected<DrmStructure> drm_structure(DrmMode mode, int occupancy) {
    DrmStructure st;
    st.mode = mode;
    st.occupancy = occupancy;
    const DrmModeParams prm = drm_mode_params(mode);
    st.ns = prm.ns;
    const auto mi = static_cast<std::size_t>(mode);
    if (occupancy < 0) {
        st.kmin = std::numeric_limits<int>::max();
        st.kmax = std::numeric_limits<int>::min();
        for (int so = 0; so < 6; ++so) {
            if (drm_occupancy_valid(mode, so)) {
                st.kmin = std::min(st.kmin, kCarriers[mi][static_cast<std::size_t>(so)].kmin);
                st.kmax = std::max(st.kmax, kCarriers[mi][static_cast<std::size_t>(so)].kmax);
            }
        }
    } else {
        if (!drm_occupancy_valid(mode, occupancy)) {
            return fail(std::format("DRM mode {} has no spectrum occupancy {} (ES table 49)",
                                    drm_mode_letter(mode), occupancy));
        }
        st.kmin = kCarriers[mi][static_cast<std::size_t>(occupancy)].kmin;
        st.kmax = kCarriers[mi][static_cast<std::size_t>(occupancy)].kmax;
    }
    const std::size_t sdc_symbols = (mode == DrmMode::A || mode == DrmMode::B) ? 2 : 3;  // ES 8.5.3.1
    const std::size_t total = 3 * st.ns * st.carriers();
    st.type.assign(total, DrmCell::Unused);
    st.pilot.assign(total, Complex32{});

    const double root2 = std::numbers::sqrt2;
    for (std::size_t f = 0; f < 3; ++f) {
        for (std::size_t s = 0; s < st.ns; ++s) {
            const int si = static_cast<int>(s);
            for (int k = st.kmin; k <= st.kmax; ++k) {
                const std::size_t idx = st.index(f, s, k);
                // ES table 50.
                if (k == 0 || (mode == DrmMode::A && (k == -1 || k == 1))) {
                    continue;
                }
                // ES 8.4.2: frequency references in every symbol.
                bool done = false;
                for (const PhasePair& fr : kFrequencyRefs[mi]) {
                    if (fr.k == k) {
                        int phase = fr.phase;
                        if (mode == DrmMode::D && k != 28 && (s % 2) == 1) {
                            phase = (phase + 512) % 1024;
                        }
                        st.type[idx] = DrmCell::Frequency;
                        st.pilot[idx] = polar(root2, phase);
                        done = true;
                    }
                }
                if (done) {
                    continue;
                }
                // ES 8.4.3: time references in symbol 0.
                if (s == 0) {
                    for (const PhasePair& tr : time_refs(mode)) {
                        if (tr.k == k) {
                            st.type[idx] = DrmCell::Time;
                            st.pilot[idx] = polar(root2, tr.phase);
                            done = true;
                        }
                    }
                    if (done) {
                        continue;
                    }
                }
                // ES 8.4.4: gain references.
                const int n = si % prm.y;
                if (positive_mod(k - prm.k0 - prm.x * n, prm.x * prm.y) == 0) {
                    const int m = si / prm.y;
                    const int p = (k - prm.k0 - n * prm.x) / (prm.x * prm.y);
                    double amp = root2;
                    if (occupancy >= 0) {
                        for (int b : kBoost[mi][static_cast<std::size_t>(occupancy)]) {
                            if (b == k && b != 0) {
                                amp = 2.0;
                            }
                        }
                    }
                    st.type[idx] = DrmCell::Gain;
                    st.pilot[idx] = polar(amp, gain_phase(mode, n, m, p, si));
                    continue;
                }
                if (f == 0 && s < sdc_symbols) {
                    st.type[idx] = DrmCell::Sdc;
                    st.sdc.push_back(static_cast<std::uint32_t>(idx));
                    continue;
                }
                if (is_fac(mode, si, k)) {
                    st.type[idx] = DrmCell::Fac;
                    st.fac[f].push_back(static_cast<std::uint32_t>(idx));
                    continue;
                }
                st.type[idx] = DrmCell::Msc;
                st.msc.push_back(static_cast<std::uint32_t>(idx));
            }
        }
    }
    return st;
}

// ---------------------------------------------------------------------------
// FFT
// ---------------------------------------------------------------------------

DrmFft::DrmFft(std::size_t n) : n_(n) {
    std::size_t rest = n;
    for (std::size_t p = 2; p * p <= rest; ++p) {
        while (rest % p == 0) {
            factors_.push_back(p);
            rest /= p;
        }
    }
    if (rest > 1) {
        factors_.push_back(rest);
    }
    factors_.push_back(1);
    twiddle_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double a = -kTwoPi * static_cast<double>(i) / static_cast<double>(n);
        twiddle_[i] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
    }
}

void DrmFft::recurse(const Complex32* in, Complex32* out, std::size_t n, std::size_t stride,
                     const std::size_t* factors, bool inverse) const {
    if (n == 1) {
        out[0] = in[0];
        return;
    }
    const std::size_t p = factors[0];
    const std::size_t m = n / p;
    for (std::size_t q = 0; q < p; ++q) {
        recurse(in + q * stride, out + q * m, m, stride * p, factors + 1, inverse);
    }
    const std::size_t step = n_ / n;
    std::vector<Complex32> t(p);
    for (std::size_t k = 0; k < m; ++k) {
        for (std::size_t q = 0; q < p; ++q) {
            Complex32 w = twiddle_[(q * k * step) % n_];
            if (inverse) {
                w = std::conj(w);
            }
            t[q] = out[q * m + k] * w;
        }
        for (std::size_t u = 0; u < p; ++u) {
            Complex32 acc{};
            for (std::size_t q = 0; q < p; ++q) {
                Complex32 w = twiddle_[(q * u * m * step) % n_];
                if (inverse) {
                    w = std::conj(w);
                }
                acc += t[q] * w;
            }
            out[k + u * m] = acc;
        }
    }
}

void DrmFft::transform(std::vector<Complex32>& data, bool inverse) const {
    std::vector<Complex32> out(n_);
    recurse(data.data(), out.data(), n_, 1, factors_.data(), inverse);
    data.swap(out);
}

// ---------------------------------------------------------------------------
// Coding
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> drm_prbs(std::size_t length) {
    // ES 7.2.2, figure 19: nine stages from all ones, the output fed back.
    std::uint32_t reg = 0x1FF;
    std::vector<std::uint8_t> out(length);
    for (std::size_t i = 0; i < length; ++i) {
        const std::uint32_t bit = ((reg >> 8U) ^ (reg >> 4U)) & 1U;
        out[i] = static_cast<std::uint8_t>(bit);
        reg = ((reg << 1U) | bit) & 0x1FFU;
    }
    return out;
}

std::uint32_t drm_crc(std::span<const std::uint8_t> bits, unsigned width, std::uint32_t polynomial) {
    const std::uint32_t mask = (width >= 32) ? 0xFFFFFFFFU : ((1U << width) - 1U);
    std::uint32_t reg = mask;
    for (std::uint8_t b : bits) {
        const std::uint32_t fb = ((reg >> (width - 1)) ^ b) & 1U;
        reg = (reg << 1U) & mask;
        if (fb) {
            reg ^= polynomial;
        }
    }
    return (~reg) & mask;
}

std::vector<std::uint32_t> drm_interleaver(std::size_t xin, unsigned t) {
    std::vector<std::uint32_t> perm(xin);
    if (xin == 0) {
        return perm;
    }
    std::size_t s = 1;
    while (s < xin) {
        s <<= 1U;
    }
    const std::size_t q = s / 4 - 1;
    perm[0] = 0;
    for (std::size_t i = 1; i < xin; ++i) {
        std::size_t v = (t * perm[i - 1] + q) % s;
        while (v >= xin) {
            v = (t * v + q) % s;
        }
        perm[i] = static_cast<std::uint32_t>(v);
    }
    return perm;
}

Expected<DrmPuncture> drm_puncture(DrmRate rate) {
    // ES table 27, the B0..B5 rows of each pattern.
    auto make = [](std::initializer_list<const char*> rows) {
        DrmPuncture p;
        std::size_t r = 0;
        for (const char* row : rows) {
            for (const char* c = row; *c; ++c) {
                p.rows[r].push_back(static_cast<std::uint8_t>(*c - '0'));
            }
            ++r;
        }
        p.period = p.rows[0].size();
        for (; r < 6; ++r) {
            p.rows[r].assign(p.period, 0);
        }
        return p;
    };
    const int rx = rate.rx;
    const int ry = rate.ry;
    if (rx == 1 && ry == 6) return make({"1", "1", "1", "1", "1", "1"});
    if (rx == 1 && ry == 4) return make({"1", "1", "1", "1"});
    if (rx == 3 && ry == 10) return make({"111", "111", "111", "100"});
    if (rx == 1 && ry == 3) return make({"1", "1", "1"});
    if (rx == 4 && ry == 11) return make({"1111", "1111", "1110"});
    if (rx == 2 && ry == 5) return make({"11", "11", "10"});
    if (rx == 1 && ry == 2) return make({"1", "1"});
    if (rx == 4 && ry == 7) return make({"1111", "1010", "0100"});
    if (rx == 3 && ry == 5) return make({"111", "101"});
    if (rx == 2 && ry == 3) return make({"11", "10"});
    if (rx == 8 && ry == 11) return make({"11111111", "10010010"});
    if (rx == 3 && ry == 4) return make({"111", "100"});
    if (rx == 4 && ry == 5) return make({"1111", "1000"});
    if (rx == 7 && ry == 8) return make({"1111111", "1000000"});
    if (rx == 8 && ry == 9) return make({"11111111", "10000000"});
    return fail(std::format("DRM has no code rate {}/{} (ES table 27)", rx, ry));
}

DrmPuncture drm_tail_puncture(int rp) {
    // ES table 28: B0 and B1 always, then B2 and B3 per rp.
    static constexpr const char* b2[12] = {"000000", "100000", "100100", "110100", "110110", "111110",
                                           "111111", "111111", "111111", "111111", "111111", "111111"};
    static constexpr const char* b3[12] = {"000000", "000000", "000000", "000000", "000000", "000000",
                                           "000000", "100000", "100100", "110100", "110101", "111101"};
    DrmPuncture p;
    p.period = 6;
    rp = std::clamp(rp, 0, 11);
    for (std::size_t i = 0; i < 6; ++i) {
        p.rows[0].push_back(1);
        p.rows[1].push_back(1);
        p.rows[2].push_back(static_cast<std::uint8_t>(b2[rp][i] - '0'));
        p.rows[3].push_back(static_cast<std::uint8_t>(b3[rp][i] - '0'));
        p.rows[4].push_back(0);
        p.rows[5].push_back(0);
    }
    return p;
}

Expected<DrmCodingPlan> drm_plan(DrmQam qam, std::size_t n1, std::size_t n2,
                                 std::span<const DrmRate> rate_a, std::span<const DrmRate> rate_b) {
    DrmCodingPlan plan;
    plan.qam = qam;
    plan.n1 = n1;
    plan.n2 = n2;
    const std::size_t levels = plan.levels();
    if (rate_b.size() < levels || (n1 > 0 && rate_a.size() < levels)) {
        return fail("drm_plan needs a code rate per level");
    }
    if (n2 < 20) {
        return fail("DRM needs at least 20 cells in the lower protected part (ES 7.2.1.1)");
    }
    for (std::size_t p = 0; p < levels; ++p) {
        plan.rate_b[p] = rate_b[p];
        if (n1 > 0) {
            plan.rate_a[p] = rate_a[p];
            const std::size_t num = 2 * n1 * static_cast<std::size_t>(rate_a[p].rx);
            if (num % static_cast<std::size_t>(rate_a[p].ry) != 0) {
                return fail("drm_plan: N1 is not a multiple of the part A rate denominators");
            }
            plan.m1[p] = num / static_cast<std::size_t>(rate_a[p].ry);
        }
        // ES 7.3.1.1 and 7.3.2.
        const std::size_t avail = 2 * n2 - 12;
        const auto ry = static_cast<std::size_t>(rate_b[p].ry);
        plan.m2[p] = static_cast<std::size_t>(rate_b[p].rx) * (avail / ry);
        plan.rp[p] = static_cast<int>(avail - ry * (avail / ry));
        plan.l1 += plan.m1[p];
        plan.l2 += plan.m2[p];
    }
    return plan;
}

DrmCodingPlan drm_fac_plan() {
    // ES 7.2.1.2 and table 39: 72 bits on 65 4-QAM cells at rate 3/5, the
    // whole mother codeword punctured by table 27 (ES 7.3.2).
    DrmCodingPlan plan;
    plan.qam = DrmQam::Qam4;
    plan.n2 = 65;
    plan.rate_b[0] = {3, 5};
    plan.m2[0] = 72;
    plan.l2 = 72;
    plan.fac_tail = true;
    return plan;
}

Expected<DrmCodingPlan> drm_sdc_plan(std::size_t n_sdc, int sdc_mode) {
    // ES tables 36 and 37.
    if (sdc_mode == 0) {
        const std::array<DrmRate, 2> r = {DrmRate{1, 3}, DrmRate{2, 3}};
        return drm_plan(DrmQam::Qam16, 0, n_sdc, {}, r);
    }
    const std::array<DrmRate, 1> r = {DrmRate{1, 2}};
    return drm_plan(DrmQam::Qam4, 0, n_sdc, {}, r);
}

Expected<DrmMscRates> drm_msc_rates(DrmQam qam, int level) {
    // ES tables 30 and 32.
    if (qam == DrmQam::Qam16) {
        switch (level) {
            case 0: return DrmMscRates{{DrmRate{1, 3}, DrmRate{2, 3}, DrmRate{}}, 3};
            case 1: return DrmMscRates{{DrmRate{1, 2}, DrmRate{3, 4}, DrmRate{}}, 4};
            default: break;
        }
    } else if (qam == DrmQam::Qam64) {
        switch (level) {
            case 0: return DrmMscRates{{DrmRate{1, 4}, DrmRate{1, 2}, DrmRate{3, 4}}, 4};
            case 1: return DrmMscRates{{DrmRate{1, 3}, DrmRate{2, 3}, DrmRate{4, 5}}, 15};
            case 2: return DrmMscRates{{DrmRate{1, 2}, DrmRate{3, 4}, DrmRate{7, 8}}, 8};
            case 3: return DrmMscRates{{DrmRate{2, 3}, DrmRate{4, 5}, DrmRate{8, 9}}, 45};
            default: break;
        }
    }
    return fail(std::format("DRM MSC has no protection level {} for this constellation", level));
}

std::size_t drm_msc_n1(DrmQam qam, int level, std::size_t bytes) {
    if (bytes == 0) {
        return 0;
    }
    auto r = drm_msc_rates(qam, level);
    if (!r) {
        return 0;
    }
    // ES 7.2.1.1: N1 = ceil(8X / (2 RYlcm sum Rp)) RYlcm, with 2 RYlcm sum Rp
    // an integer because RYlcm is a multiple of every RYp.
    std::size_t denom = 0;
    for (std::size_t p = 0; p < static_cast<std::size_t>(qam); ++p) {
        denom += 2 * static_cast<std::size_t>(r->rates[p].rx) *
                 static_cast<std::size_t>(r->ry_lcm / r->rates[p].ry);
    }
    const std::size_t lcm = static_cast<std::size_t>(r->ry_lcm);
    return (8 * bytes + denom - 1) / denom * lcm;
}

Expected<DrmCodingPlan> drm_msc_plan(DrmQam qam, int pa, int pb, std::size_t part_a_bytes,
                                     std::size_t n_mux) {
    auto rb = drm_msc_rates(qam, pb);
    if (!rb) {
        return std::unexpected(rb.error());
    }
    std::size_t n1 = 0;
    std::array<DrmRate, 3> ra_rates{};
    if (part_a_bytes > 0) {
        auto ra = drm_msc_rates(qam, pa);
        if (!ra) {
            return std::unexpected(ra.error());
        }
        ra_rates = ra->rates;
        n1 = drm_msc_n1(qam, pa, part_a_bytes);
    }
    if (n1 + 20 > n_mux) {
        return fail("DRM part A leaves fewer than 20 cells for part B (ES 7.2.1.1)");
    }
    return drm_plan(qam, n1, n_mux - n1, ra_rates, rb->rates);
}

namespace {

// Per level, the number of mother-code steps and which part each falls in.
std::vector<std::uint8_t> puncture_mask(const DrmCodingPlan& plan, std::size_t p) {
    const std::size_t m = plan.m1[p] + plan.m2[p];
    std::vector<std::uint8_t> mask((m + 6) * 6, 0);
    DrmPuncture pa;
    if (plan.m1[p] > 0) {
        pa = drm_puncture(plan.rate_a[p]).value();
    }
    const DrmPuncture pb = drm_puncture(plan.rate_b[p]).value();
    const DrmPuncture pt = plan.fac_tail ? pb : drm_tail_puncture(plan.rp[p]);
    for (std::size_t step = 0; step < m + 6; ++step) {
        const DrmPuncture* pat = nullptr;
        std::size_t col = 0;
        if (step < plan.m1[p]) {
            pat = &pa;
            col = step % pa.period;
        } else if (step < m || plan.fac_tail) {
            pat = &pb;
            col = (step - plan.m1[p]) % pb.period;
        } else {
            pat = &pt;
            col = (step - m) % pt.period;
        }
        for (std::size_t r = 0; r < 6; ++r) {
            mask[step * 6 + r] = pat->rows[r][col];
        }
    }
    return mask;
}

struct LevelLayout {
    std::vector<std::uint8_t> mask;
    std::vector<std::uint32_t> perm_a;
    std::vector<std::uint32_t> perm_b;
    bool interleaved = false;
};

LevelLayout layout(const DrmCodingPlan& plan, std::size_t p) {
    LevelLayout l;
    l.mask = puncture_mask(plan, p);
    const auto t = level_t(plan.qam, p);
    if (t) {
        l.interleaved = true;
        l.perm_a = drm_interleaver(2 * plan.n1, *t);
        l.perm_b = drm_interleaver(2 * plan.n2, *t);
    }
    return l;
}

// Level p's input bits, in u order, ES 7.3.1.1.
std::vector<std::uint8_t> level_input(const DrmCodingPlan& plan, std::size_t p,
                                      std::span<const std::uint8_t> u) {
    std::vector<std::uint8_t> a;
    std::size_t off = 0;
    for (std::size_t q = 0; q < p; ++q) {
        off += plan.m1[q];
    }
    a.insert(a.end(), u.begin() + static_cast<std::ptrdiff_t>(off),
             u.begin() + static_cast<std::ptrdiff_t>(off + plan.m1[p]));
    off = plan.l1;
    for (std::size_t q = 0; q < p; ++q) {
        off += plan.m2[q];
    }
    a.insert(a.end(), u.begin() + static_cast<std::ptrdiff_t>(off),
             u.begin() + static_cast<std::ptrdiff_t>(off + plan.m2[p]));
    return a;
}

// Encodes one level to its 2(N1+N2) transmitted bits, interleaved.
Expected<std::vector<std::uint8_t>> encode_level(const DrmCodingPlan& plan, const LevelLayout& l,
                                                 std::size_t p, std::vector<std::uint8_t> a) {
    a.insert(a.end(), 6, 0);
    auto mother = convolutional_encode(kMotherCode, a);
    if (!mother) {
        return std::unexpected(mother.error());
    }
    std::vector<std::uint8_t> v;
    v.reserve(2 * (plan.n1 + plan.n2));
    for (std::size_t i = 0; i < mother->size(); ++i) {
        if (l.mask[i]) {
            v.push_back((*mother)[i]);
        }
    }
    if (v.size() != 2 * (plan.n1 + plan.n2)) {
        return fail(std::format("DRM level {} punctured to {} bits, expected {}", p, v.size(),
                                2 * (plan.n1 + plan.n2)));
    }
    if (!l.interleaved) {
        return v;
    }
    std::vector<std::uint8_t> y(v.size());
    const std::size_t na = 2 * plan.n1;
    for (std::size_t i = 0; i < na; ++i) {
        y[i] = v[l.perm_a[i]];
    }
    for (std::size_t i = 0; i < 2 * plan.n2; ++i) {
        y[na + i] = v[na + l.perm_b[i]];
    }
    return y;
}

float axis_value(DrmQam qam, unsigned bits) {
    // ES figures 26, 29 and 30: v = (2^P - 1) - 2 (y0 + 2 y1 + 4 y2).
    const unsigned top = (1U << static_cast<unsigned>(qam)) - 1U;
    return static_cast<float>(static_cast<int>(top) - 2 * static_cast<int>(bits));
}

float qam_scale(DrmQam qam) {
    switch (qam) {
        case DrmQam::Qam4: return static_cast<float>(1.0 / std::sqrt(2.0));
        case DrmQam::Qam16: return static_cast<float>(1.0 / std::sqrt(10.0));
        case DrmQam::Qam64: return static_cast<float>(1.0 / std::sqrt(42.0));
    }
    return 1.0F;
}

}  // namespace

Complex32 drm_qam_point(DrmQam qam, std::span<const std::uint8_t> i_bits,
                        std::span<const std::uint8_t> q_bits) {
    unsigned bi = 0;
    unsigned bq = 0;
    for (std::size_t p = 0; p < static_cast<std::size_t>(qam); ++p) {
        bi |= static_cast<unsigned>(i_bits[p] & 1U) << p;
        bq |= static_cast<unsigned>(q_bits[p] & 1U) << p;
    }
    const float a = qam_scale(qam);
    return {a * axis_value(qam, bi), a * axis_value(qam, bq)};
}

Expected<std::vector<Complex32>> drm_mlc_encode(const DrmCodingPlan& plan,
                                                std::span<const std::uint8_t> bits) {
    if (bits.size() != plan.l1 + plan.l2) {
        return fail(std::format("drm_mlc_encode wants {} bits, got {}", plan.l1 + plan.l2, bits.size()));
    }
    const std::size_t levels = plan.levels();
    std::vector<std::vector<std::uint8_t>> y(levels);
    for (std::size_t p = 0; p < levels; ++p) {
        auto yl = encode_level(plan, layout(plan, p), p, level_input(plan, p, bits));
        if (!yl) {
            return std::unexpected(yl.error());
        }
        y[p] = std::move(*yl);
    }
    const std::size_t cells = plan.n1 + plan.n2;
    std::vector<Complex32> out(cells);
    std::array<std::uint8_t, 3> ib{};
    std::array<std::uint8_t, 3> qb{};
    for (std::size_t n = 0; n < cells; ++n) {
        for (std::size_t p = 0; p < levels; ++p) {
            ib[p] = y[p][2 * n];
            qb[p] = y[p][2 * n + 1];
        }
        out[n] = drm_qam_point(plan.qam, ib, qb);
    }
    return out;
}

Expected<std::vector<std::uint8_t>> drm_mlc_decode(const DrmCodingPlan& plan,
                                                   std::span<const Complex32> cells,
                                                   std::span<const float> weights) {
    const std::size_t ncell = plan.n1 + plan.n2;
    if (cells.size() != ncell || weights.size() != ncell) {
        return fail(std::format("drm_mlc_decode wants {} cells, got {}", ncell, cells.size()));
    }
    const std::size_t levels = plan.levels();
    const float a = qam_scale(plan.qam);
    const unsigned points = 1U << levels;
    // Known bits of the levels decoded so far, per cell and axis.
    std::vector<std::uint8_t> known(2 * ncell, 0);
    std::vector<std::uint8_t> u(plan.l1 + plan.l2, 0);
    std::vector<float> soft(2 * ncell);

    for (std::size_t p = 0; p < levels; ++p) {
        // Multistage decoding: max-log soft value of bit p, given the bits of
        // the lower levels already decided and the higher levels free.
        const unsigned low_mask = (1U << p) - 1U;
        for (std::size_t n = 0; n < ncell; ++n) {
            for (std::size_t axis = 0; axis < 2; ++axis) {
                const float v = (axis == 0 ? cells[n].real() : cells[n].imag()) / a;
                float best0 = std::numeric_limits<float>::max();
                float best1 = std::numeric_limits<float>::max();
                for (unsigned b = 0; b < points; ++b) {
                    if ((b & low_mask) != (known[2 * n + axis] & low_mask)) {
                        continue;
                    }
                    const float d = v - axis_value(plan.qam, b);
                    const float d2 = d * d;
                    if ((b >> p) & 1U) {
                        best1 = std::min(best1, d2);
                    } else {
                        best0 = std::min(best0, d2);
                    }
                }
                soft[2 * n + axis] = weights[n] * (best1 - best0);
            }
        }
        const LevelLayout l = layout(plan, p);
        // Deinterleave.
        std::vector<float> v(2 * ncell);
        if (l.interleaved) {
            const std::size_t na = 2 * plan.n1;
            for (std::size_t i = 0; i < na; ++i) {
                v[l.perm_a[i]] = soft[i];
            }
            for (std::size_t i = 0; i < 2 * plan.n2; ++i) {
                v[na + l.perm_b[i]] = soft[na + i];
            }
        } else {
            v = soft;
        }
        // Depuncture.
        std::vector<float> mother(l.mask.size(), 0.0F);
        std::size_t j = 0;
        for (std::size_t i = 0; i < l.mask.size(); ++i) {
            if (l.mask[i]) {
                mother[i] = v[j++];
            }
        }
        auto decoded = viterbi_decode(kMotherCode, mother, true);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        std::vector<std::uint8_t> a_bits(decoded->begin(),
                                         decoded->begin() + static_cast<std::ptrdiff_t>(plan.m1[p] + plan.m2[p]));
        // Write back into u order.
        std::size_t off = 0;
        for (std::size_t q = 0; q < p; ++q) {
            off += plan.m1[q];
        }
        std::copy_n(a_bits.begin(), plan.m1[p], u.begin() + static_cast<std::ptrdiff_t>(off));
        off = plan.l1;
        for (std::size_t q = 0; q < p; ++q) {
            off += plan.m2[q];
        }
        std::copy_n(a_bits.begin() + static_cast<std::ptrdiff_t>(plan.m1[p]), plan.m2[p],
                    u.begin() + static_cast<std::ptrdiff_t>(off));
        if (p + 1 < levels) {
            auto y = encode_level(plan, l, p, a_bits);
            if (!y) {
                return std::unexpected(y.error());
            }
            for (std::size_t i = 0; i < 2 * ncell; ++i) {
                known[i] |= static_cast<std::uint8_t>(((*y)[i] & 1U) << p);
            }
        }
    }
    return u;
}

// ---------------------------------------------------------------------------
// FAC
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> drm_fac_bits(const DrmFacChannel& c, const DrmFacService& s) {
    std::vector<std::uint8_t> b;
    // ES 6.3.3.
    push_bits(b, c.enhancement, 1);
    push_bits(b, c.identity, 2);
    push_bits(b, c.rm, 1);
    push_bits(b, c.occupancy, 3);
    push_bits(b, c.short_interleave, 1);
    push_bits(b, c.msc_mode, 2);
    push_bits(b, c.sdc_mode, 1);
    push_bits(b, c.services, 4);
    push_bits(b, c.reconfiguration, 3);
    push_bits(b, c.toggle, 1);
    push_bits(b, c.rfu, 1);
    // ES 6.3.4.
    push_bits(b, s.service_id, 24);
    push_bits(b, s.short_id, 2);
    push_bits(b, s.audio_ca, 1);
    push_bits(b, s.language, 4);
    push_bits(b, s.data, 1);
    push_bits(b, s.descriptor, 5);
    push_bits(b, s.data_ca, 1);
    push_bits(b, s.rfa, 6);
    // ES 6.3.5.
    push_bits(b, drm_crc(b, 8, kDrmCrc8Poly), 8);
    return b;
}

DrmFac drm_parse_fac(std::span<const std::uint8_t> bits) {
    DrmFac f;
    if (bits.size() < 72) {
        return f;
    }
    std::size_t at = 0;
    auto& c = f.channel;
    c.enhancement = read_bits(bits, at, 1) != 0;
    c.identity = static_cast<std::uint8_t>(read_bits(bits, at, 2));
    c.rm = read_bits(bits, at, 1) != 0;
    c.occupancy = static_cast<std::uint8_t>(read_bits(bits, at, 3));
    c.short_interleave = read_bits(bits, at, 1) != 0;
    c.msc_mode = static_cast<std::uint8_t>(read_bits(bits, at, 2));
    c.sdc_mode = static_cast<std::uint8_t>(read_bits(bits, at, 1));
    c.services = static_cast<std::uint8_t>(read_bits(bits, at, 4));
    c.reconfiguration = static_cast<std::uint8_t>(read_bits(bits, at, 3));
    c.toggle = read_bits(bits, at, 1) != 0;
    c.rfu = read_bits(bits, at, 1) != 0;
    auto& s = f.service;
    s.service_id = static_cast<std::uint32_t>(read_bits(bits, at, 24));
    s.short_id = static_cast<std::uint8_t>(read_bits(bits, at, 2));
    s.audio_ca = read_bits(bits, at, 1) != 0;
    s.language = static_cast<std::uint8_t>(read_bits(bits, at, 4));
    s.data = read_bits(bits, at, 1) != 0;
    s.descriptor = static_cast<std::uint8_t>(read_bits(bits, at, 5));
    s.data_ca = read_bits(bits, at, 1) != 0;
    s.rfa = static_cast<std::uint8_t>(read_bits(bits, at, 6));
    const auto crc = static_cast<std::uint32_t>(read_bits(bits, at, 8));
    f.crc_ok = crc == drm_crc(bits.first(64), 8, kDrmCrc8Poly);
    return f;
}

// ---------------------------------------------------------------------------
// SDC
// ---------------------------------------------------------------------------

std::size_t drm_sdc_data_bytes(std::size_t l_sdc) {
    // ES 6.4.2: 4 bits of AFS index and 16 of CRC around whole bytes.
    return l_sdc < 20 ? 0 : (l_sdc - 20) / 8;
}

Expected<std::vector<std::uint8_t>> drm_sdc_bits(std::uint8_t afs, std::span<const std::uint8_t> data,
                                                 std::size_t l_sdc) {
    const std::size_t n = drm_sdc_data_bytes(l_sdc);
    if (data.size() > n) {
        return fail(std::format("SDC data field holds {} bytes, given {}", n, data.size()));
    }
    std::vector<std::uint8_t> field(data.begin(), data.end());
    field.resize(n, 0x00);
    std::vector<std::uint8_t> crc_in;
    push_bits(crc_in, afs & 0xFU, 8);
    for (std::uint8_t byte : field) {
        push_bits(crc_in, byte, 8);
    }
    std::vector<std::uint8_t> bits;
    push_bits(bits, afs & 0xFU, 4);
    for (std::uint8_t byte : field) {
        push_bits(bits, byte, 8);
    }
    push_bits(bits, drm_crc(crc_in, 16, kDrmCrc16Poly), 16);
    bits.resize(l_sdc, 0);
    return bits;
}

std::vector<std::uint8_t> drm_sdc_entity(std::uint8_t type, bool version, std::uint8_t nibble,
                                         std::span<const std::uint8_t> body) {
    std::vector<std::uint8_t> bits;
    push_bits(bits, body.size() & 0x7FU, 7);
    push_bits(bits, version ? 1U : 0U, 1);
    push_bits(bits, type & 0xFU, 4);
    push_bits(bits, nibble & 0xFU, 4);
    for (std::uint8_t b : body) {
        push_bits(bits, b, 8);
    }
    std::vector<std::uint8_t> bytes(bits.size() / 8, 0);
    for (std::size_t i = 0; i < bytes.size() * 8; ++i) {
        bytes[i / 8] = static_cast<std::uint8_t>(bytes[i / 8] | (bits[i] << (7 - i % 8)));
    }
    return bytes;
}

DrmSdc drm_parse_sdc(std::span<const std::uint8_t> bits) {
    DrmSdc sdc;
    if (bits.size() < 20) {
        return sdc;
    }
    const std::size_t n = drm_sdc_data_bytes(bits.size());
    std::size_t at = 0;
    sdc.afs_index = static_cast<std::uint8_t>(read_bits(bits, at, 4));
    std::vector<std::uint8_t> crc_in;
    push_bits(crc_in, sdc.afs_index, 8);
    crc_in.insert(crc_in.end(), bits.begin() + 4, bits.begin() + 4 + static_cast<std::ptrdiff_t>(8 * n));
    std::size_t crc_at = 4 + 8 * n;
    const auto crc = static_cast<std::uint32_t>(read_bits(bits, crc_at, 16));
    sdc.crc_ok = crc == drm_crc(crc_in, 16, kDrmCrc16Poly);
    if (!sdc.crc_ok) {
        return sdc;
    }
    const std::size_t end = 4 + 8 * n;
    while (at + 16 <= end) {
        std::size_t peek = at;
        if (read_bits(bits, peek, 16) == 0) {
            break;  // ES 6.4.3.0: padding bytes are 0x00
        }
        DrmSdcEntity e;
        const auto length = static_cast<std::size_t>(read_bits(bits, at, 7));
        e.version = read_bits(bits, at, 1) != 0;
        e.type = static_cast<std::uint8_t>(read_bits(bits, at, 4));
        e.nibble = static_cast<std::uint8_t>(read_bits(bits, at, 4));
        if (at + 8 * length > end) {
            break;
        }
        for (std::size_t i = 0; i < length; ++i) {
            e.body.push_back(static_cast<std::uint8_t>(read_bits(bits, at, 8)));
        }
        sdc.entities.push_back(std::move(e));
    }
    for (const DrmSdcEntity& e : sdc.entities) {
        switch (e.type) {
            case 0:  // ES 6.4.3.1
                if (!e.version) {
                    sdc.protection_a = static_cast<std::uint8_t>(e.nibble >> 2U);
                    sdc.protection_b = static_cast<std::uint8_t>(e.nibble & 3U);
                    sdc.streams.clear();
                    for (std::size_t i = 0; i + 3 <= e.body.size(); i += 3) {
                        DrmStreamDescription d;
                        d.part_a = static_cast<std::uint16_t>((e.body[i] << 4U) | (e.body[i + 1] >> 4U));
                        d.part_b = static_cast<std::uint16_t>(((e.body[i + 1] & 0xFU) << 8U) | e.body[i + 2]);
                        sdc.streams.push_back(d);
                    }
                }
                break;
            case 1: {  // ES 6.4.3.2 and 6.7.3.1
                std::string label(e.body.begin(), e.body.end());
                if (!label.empty() && static_cast<unsigned char>(label[0]) < 0x10) {
                    label.erase(0, 1);
                }
                sdc.labels.emplace_back(static_cast<std::uint8_t>(e.nibble >> 2U), label);
                break;
            }
            case 5: {  // ES 6.4.3.6
                if (e.body.empty()) {
                    break;
                }
                DrmApplication app;
                app.short_id = static_cast<std::uint8_t>(e.nibble >> 2U);
                app.stream_id = static_cast<std::uint8_t>(e.nibble & 3U);
                app.packet_mode = (e.body[0] & 0x80U) != 0;
                app.domain = static_cast<std::uint8_t>(e.body[0] & 7U);
                std::size_t data_at = 1;
                if (app.packet_mode && e.body.size() >= 2) {
                    app.data_units = (e.body[0] & 0x40U) != 0;
                    app.packet_id = static_cast<std::uint8_t>((e.body[0] >> 4U) & 3U);
                    app.packet_length = e.body[1];
                    data_at = 2;
                }
                app.application_data.assign(e.body.begin() + static_cast<std::ptrdiff_t>(data_at), e.body.end());
                sdc.applications.push_back(std::move(app));
                break;
            }
            case 8: {  // ES 6.4.3.9
                std::vector<std::uint8_t> b;
                push_bits(b, e.nibble, 4);
                for (std::uint8_t byte : e.body) {
                    push_bits(b, byte, 8);
                }
                if (b.size() >= 28) {
                    std::size_t p = 0;
                    sdc.mjd = static_cast<std::uint32_t>(read_bits(b, p, 17));
                    sdc.utc_hours = static_cast<std::uint8_t>(read_bits(b, p, 5));
                    sdc.utc_minutes = static_cast<std::uint8_t>(read_bits(b, p, 6));
                    if (b.size() >= 36) {
                        p += 2;
                        const bool negative = read_bits(b, p, 1) != 0;
                        const int value = static_cast<int>(read_bits(b, p, 5));
                        sdc.local_offset_half_hours = negative ? -value : value;
                    }
                }
                break;
            }
            case 9: {  // ES 6.4.3.10
                if (e.body.size() < 2) {
                    break;
                }
                DrmAudioInfo ai;
                ai.short_id = static_cast<std::uint8_t>(e.nibble >> 2U);
                ai.stream_id = static_cast<std::uint8_t>(e.nibble & 3U);
                ai.coding = static_cast<std::uint8_t>(e.body[0] >> 6U);
                ai.text = (e.body[1] & 0x80U) != 0;
                if (!e.version) {
                    sdc.audio.push_back(ai);
                }
                break;
            }
            default:
                break;
        }
    }
    return sdc;
}

// ---------------------------------------------------------------------------
// Text messages and packets
// ---------------------------------------------------------------------------

std::vector<std::array<std::uint8_t, 4>> drm_text_pieces(const std::string& text, bool toggle) {
    std::vector<std::array<std::uint8_t, 4>> pieces;
    const std::size_t segments = std::max<std::size_t>(1, (text.size() + 15) / 16);
    for (std::size_t seg = 0; seg < segments && seg < 8; ++seg) {
        const std::size_t from = seg * 16;
        const std::size_t len = std::min<std::size_t>(16, text.size() - std::min(text.size(), from));
        const bool first = seg == 0;
        const bool last = seg + 1 == segments;
        // ES 6.5: header, body, CRC-16 over both.
        std::vector<std::uint8_t> bits;
        push_bits(bits, toggle, 1);
        push_bits(bits, first, 1);
        push_bits(bits, last, 1);
        push_bits(bits, 0, 1);
        push_bits(bits, len == 0 ? 0 : len - 1, 4);
        push_bits(bits, first ? 0xFU : seg, 4);
        push_bits(bits, 0, 4);
        for (std::size_t i = 0; i < len; ++i) {
            push_bits(bits, static_cast<std::uint8_t>(text[from + i]), 8);
        }
        push_bits(bits, drm_crc(bits, 16, kDrmCrc16Poly), 16);
        std::vector<std::uint8_t> bytes(bits.size() / 8, 0);
        for (std::size_t i = 0; i < bits.size(); ++i) {
            bytes[i / 8] = static_cast<std::uint8_t>(bytes[i / 8] | (bits[i] << (7 - i % 8)));
        }
        pieces.push_back({0xFF, 0xFF, 0xFF, 0xFF});
        for (std::size_t i = 0; i < bytes.size(); i += 4) {
            std::array<std::uint8_t, 4> piece{};
            for (std::size_t j = 0; j < 4 && i + j < bytes.size(); ++j) {
                piece[j] = bytes[i + j];
            }
            pieces.push_back(piece);
        }
    }
    return pieces;
}

std::optional<DrmTextMessage> DrmTextAssembler::push(std::uint8_t stream,
                                                     std::span<const std::uint8_t, 4> piece) {
    if (piece[0] == 0xFF && piece[1] == 0xFF && piece[2] == 0xFF && piece[3] == 0xFF) {
        segment_.clear();
        in_segment_ = true;
        return std::nullopt;
    }
    if (!in_segment_) {
        return std::nullopt;
    }
    segment_.insert(segment_.end(), piece.begin(), piece.end());
    if (segment_.size() < 2) {
        return std::nullopt;
    }
    const bool command = (segment_[0] & 0x10U) != 0;
    const std::size_t body = command ? 0 : static_cast<std::size_t>((segment_[0] & 0xFU) + 1);
    const std::size_t total = 2 + body + 2;
    if (segment_.size() < total) {
        return std::nullopt;
    }
    in_segment_ = false;
    std::vector<std::uint8_t> bits;
    for (std::size_t i = 0; i < 2 + body; ++i) {
        push_bits(bits, segment_[i], 8);
    }
    const std::uint32_t crc = (static_cast<std::uint32_t>(segment_[2 + body]) << 8U) | segment_[3 + body];
    if (crc != drm_crc(bits, 16, kDrmCrc16Poly) || command) {
        return std::nullopt;
    }
    const bool toggle = (segment_[0] & 0x80U) != 0;
    const bool first = (segment_[0] & 0x40U) != 0;
    const bool last = (segment_[0] & 0x20U) != 0;
    if (first) {
        message_.clear();
        expected_ = 0;
    } else {
        const int segnum = (segment_[1] >> 4U) & 7;
        if (segnum != expected_) {
            message_.clear();
            expected_ = -1;
            return std::nullopt;
        }
    }
    if (expected_ < 0) {
        return std::nullopt;
    }
    message_.append(segment_.begin() + 2, segment_.begin() + 2 + static_cast<std::ptrdiff_t>(body));
    ++expected_;
    if (last) {
        DrmTextMessage m;
        m.stream = stream;
        m.text = message_;
        m.toggle = toggle;
        expected_ = -1;
        return m;
    }
    return std::nullopt;
}

std::vector<std::uint8_t> drm_packet(bool first, bool last, std::uint8_t packet_id,
                                     std::uint8_t continuity, std::span<const std::uint8_t> useful,
                                     std::size_t length) {
    const bool padded = useful.size() < length;
    std::vector<std::uint8_t> p;
    p.push_back(static_cast<std::uint8_t>((first ? 0x80U : 0U) | (last ? 0x40U : 0U) |
                                          ((packet_id & 3U) << 4U) | (padded ? 0x08U : 0U) |
                                          (continuity & 7U)));
    if (padded) {
        p.push_back(static_cast<std::uint8_t>(useful.size()));
    }
    p.insert(p.end(), useful.begin(), useful.begin() + static_cast<std::ptrdiff_t>(std::min(useful.size(), length)));
    p.resize(1 + length, 0x00);
    const std::uint32_t crc = drm_crc(bytes_to_bits(p), 16, kDrmCrc16Poly);
    p.push_back(static_cast<std::uint8_t>(crc >> 8U));
    p.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    return p;
}

}  // namespace revenant::decode
