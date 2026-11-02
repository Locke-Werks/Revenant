#include "core/decode/mil_39tone.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace revenant::decode {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

// Table B-II, part 1 and part 2.
constexpr std::array<Tone39PreambleTone, 4> kPart1 = {{
    {3, 3.0, 0.0}, {15, 3.0, 103.7}, {27, 3.0, 103.7}, {39, 3.0, 0.0}}};
constexpr std::array<Tone39PreambleTone, 3> kPart2 = {{{9, 4.0, 0.0}, {21, 4.0, 90.0}, {33, 4.0, 0.0}}};

// Table B-II, part 3: Doppler at amplitude 2, then data tones 1 to 39.
constexpr std::array<Tone39PreambleTone, 40> kPart3 = {{
    {0, 2.0, 0.0},     {1, 1.0, 0.0},     {2, 1.0, 5.6},     {3, 1.0, 19.7},    {4, 1.0, 42.2},
    {5, 1.0, 73.1},    {6, 1.0, 115.3},   {7, 1.0, 165.9},   {8, 1.0, 225.0},   {9, 1.0, 295.3},
    {10, 1.0, 14.1},   {11, 1.0, 101.3},  {12, 1.0, 199.7},  {13, 1.0, 303.8},  {14, 1.0, 59.1},
    {15, 1.0, 185.6},  {16, 1.0, 317.8},  {17, 1.0, 101.3},  {18, 1.0, 253.1},  {19, 1.0, 56.3},
    {20, 1.0, 225.0},  {21, 1.0, 45.0},   {22, 1.0, 236.3},  {23, 1.0, 73.1},   {24, 1.0, 281.3},
    {25, 1.0, 137.8},  {26, 1.0, 5.6},    {27, 1.0, 239.1},  {28, 1.0, 123.8},  {29, 1.0, 19.7},
    {30, 1.0, 281.3},  {31, 1.0, 194.1},  {32, 1.0, 115.3},  {33, 1.0, 45.0},   {34, 1.0, 345.9},
    {35, 1.0, 295.3},  {36, 1.0, 253.1},  {37, 1.0, 222.2},  {38, 1.0, 199.7},  {39, 1.0, 185.6}}};

// Table B-III.
constexpr std::array<Tone39Framing, 28> kFraming = {{
    {75, 1, 567, 15876, 252},    {75, 4, 234, 26208, 416},    {75, 12, 75, 25200, 400},
    {75, 36, 16, 16128, 256},    {150, 1, 576, 16128, 256},   {150, 9, 100, 25200, 400},
    {150, 25, 36, 25200, 400},   {150, 81, 8, 18144, 288},    {300, 1, 567, 15876, 252},
    {300, 17, 54, 25704, 408},   {300, 47, 18, 23688, 376},   {300, 153, 4, 17136, 272},
    {600, 1, 567, 15876, 252},   {600, 33, 30, 27720, 440},   {600, 99, 10, 27720, 440},
    {600, 297, 2, 16632, 264},   {1200, 1, 567, 15876, 252},  {1200, 63, 14, 24696, 392},
    {1200, 189, 6, 31752, 504},  {1200, 567, 1, 15876, 252},  {2400, 1, 144, 8064, 256},
    {2400, 9, 16, 8064, 256},    {2400, 18, 12, 12096, 384},  {2400, 27, 9, 13608, 432},
    {2400, 36, 7, 14112, 448},   {2400, 72, 3, 12096, 384},   {2400, 144, 1, 8064, 256},
    {2400, 288, 1, 16128, 512}}};

constexpr std::array<int, 6> kRates = {75, 150, 300, 600, 1200, 2400};

// GF(16) from x^4 + x + 1 (B.5.2).
struct Gf16 {
    std::array<std::uint8_t, 30> exp{};
    std::array<int, 16> log{};
    Gf16() {
        int v = 1;
        for (int i = 0; i < 15; ++i) {
            exp[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v);
            exp[static_cast<std::size_t>(i + 15)] = static_cast<std::uint8_t>(v);
            log[static_cast<std::size_t>(v)] = i;
            v <<= 1;
            if ((v & 0x10) != 0) {
                v ^= 0x13;
            }
        }
    }
    [[nodiscard]] std::uint8_t mul(std::uint8_t a, std::uint8_t b) const {
        if (a == 0 || b == 0) {
            return 0;
        }
        return exp[static_cast<std::size_t>(log[a] + log[b])];
    }
};

const Gf16& gf() {
    static const Gf16 g;
    return g;
}

std::array<std::uint8_t, 5> generator() {
    const auto& f = gf();
    return {1, f.exp[13], f.exp[6], f.exp[3], f.exp[10]};
}

// The remainder of a word, highest degree first, modulo g(x); highest
// degree of the remainder first.
std::array<std::uint8_t, 4> remainder(std::span<const std::uint8_t> word) {
    const auto& f = gf();
    const auto g = generator();
    std::array<std::uint8_t, 4> r{};
    for (const std::uint8_t s : word) {
        const std::uint8_t top = r[0];
        r = {r[1], r[2], r[3], static_cast<std::uint8_t>(s & 0xF)};
        for (std::size_t i = 0; i < 4; ++i) {
            r[i] ^= f.mul(top, g[i + 1]);
        }
    }
    return r;
}

std::uint16_t key(const std::array<std::uint8_t, 4>& r) {
    return static_cast<std::uint16_t>((r[0] << 12U) | (r[1] << 8U) | (r[2] << 4U) | r[3]);
}

// Syndrome table for every pattern of up to two symbol errors in a word of
// n symbols: position * 16 + value for each, packed, or -1.
struct SyndromeTable {
    std::vector<std::int32_t> entry = std::vector<std::int32_t>(65536, -1);
    explicit SyndromeTable(int n) {
        std::vector<std::uint8_t> e(static_cast<std::size_t>(n), 0);
        for (int p = 0; p < n; ++p) {
            for (int v = 1; v < 16; ++v) {
                e[static_cast<std::size_t>(p)] = static_cast<std::uint8_t>(v);
                entry[key(remainder(e))] = p * 16 + v;
                for (int q = p + 1; q < n; ++q) {
                    for (int u = 1; u < 16; ++u) {
                        e[static_cast<std::size_t>(q)] = static_cast<std::uint8_t>(u);
                        entry[key(remainder(e))] = ((p * 16 + v) << 8) | (q * 16 + u) | 0x10000;
                    }
                    e[static_cast<std::size_t>(q)] = 0;
                }
            }
            e[static_cast<std::size_t>(p)] = 0;
        }
    }
};

const SyndromeTable& table(int n) {
    static const SyndromeTable t7(7);
    static const SyndromeTable t14(14);
    return n == 7 ? t7 : t14;
}

double wrap(double a) {
    return std::remainder(a, kTwoPi);
}

}  // namespace

std::span<const Tone39PreambleTone> tone39_preamble_part1() { return kPart1; }
std::span<const Tone39PreambleTone> tone39_preamble_part2() { return kPart2; }
std::span<const Tone39PreambleTone> tone39_preamble_part3() { return kPart3; }

int tone39_phase_step(int earlier, int later) {
    // Table B-V: (later, earlier) MARK SPACE +45, SPACE SPACE +135,
    // SPACE MARK +225, MARK MARK +315.
    if (later != 0) {
        return earlier != 0 ? 7 : 1;
    }
    return earlier != 0 ? 5 : 3;
}

std::span<const Tone39Framing> tone39_framing_table() { return kFraming; }

const Tone39Framing* tone39_framing(int rate, int degree) {
    for (const auto& f : kFraming) {
        if (f.rate == rate && f.degree == degree) {
            return &f;
        }
    }
    return nullptr;
}

bool tone39_valid(const Tone39Mode& mode) { return tone39_framing(mode.rate, mode.degree) != nullptr; }

std::vector<std::uint8_t> tone39_framing_sequence(int length) {
    // Figure B-5: nine stages, the generated bit is stage 9 xor the taps
    // after x^4, x^6 and x^7 (stages 5, 3 and 2 counted from the input),
    // and is shifted in at stage 1. c[0] is the bit generated from state
    // 111111111; the sequence is the `length` bits ending with it.
    std::array<std::uint8_t, 9> s{};
    s.fill(1);
    std::array<std::uint8_t, 511> c{};
    for (auto& b : c) {
        b = static_cast<std::uint8_t>(s[8] ^ s[4] ^ s[2] ^ s[1]);
        for (std::size_t i = 8; i > 0; --i) {
            s[i] = s[i - 1];
        }
        s[0] = b;
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(std::max(length, 0)));
    for (int i = 0; i < length; ++i) {
        const int j = ((i - (length - 1)) % 511 + 511) % 511;
        out[static_cast<std::size_t>(i)] = c[static_cast<std::size_t>(j)];
    }
    return out;
}

std::array<std::uint8_t, 5> tone39_rs_generator() { return generator(); }

std::vector<std::uint8_t> tone39_rs_encode(std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> word(data.begin(), data.end());
    word.resize(data.size() + 4, 0);
    const auto r = remainder(word);
    std::copy(r.begin(), r.end(), word.end() - 4);
    return word;
}

int tone39_rs_decode(std::span<std::uint8_t> word) {
    const int n = static_cast<int>(word.size());
    if (n != 7 && n != 14) {
        return -1;
    }
    const auto r = remainder(word);
    const std::uint16_t k = key(r);
    if (k == 0) {
        return 0;
    }
    const std::int32_t e = table(n).entry[k];
    if (e < 0) {
        return -1;
    }
    const auto apply = [&](int pv) {
        word[static_cast<std::size_t>(pv / 16)] ^= static_cast<std::uint8_t>(pv % 16);
    };
    if ((e & 0x10000) != 0) {
        apply((e >> 8) & 0xFF);
        apply(e & 0xFF);
        return 2;
    }
    apply(e & 0xFF);
    return 1;
}

int tone39_superblock_data_bits(const Tone39Mode& mode) {
    return mode.degree * tone39_rs_k(mode.rate) * 4;
}

int tone39_superblock_bits(const Tone39Mode& mode) { return mode.degree * tone39_rs_n(mode.rate) * 4; }

namespace {

// Figures B-2 and B-3: data bits fill codeword symbols a pair of codewords
// at a time, eight bits to a symbol row (four to each codeword), a lone
// last codeword four bits to a row. Returns, for each data bit in order,
// codeword * 64 + symbol * 4 + bit (bit 0 the first, most significant).
std::vector<int> load_order(const Tone39Mode& mode) {
    const int k = tone39_rs_k(mode.rate);
    std::vector<int> order;
    order.reserve(static_cast<std::size_t>(tone39_superblock_data_bits(mode)));
    int c = 0;
    for (; c + 1 < mode.degree; c += 2) {
        for (int r = 0; r < k; ++r) {
            for (int w = 0; w < 2; ++w) {
                for (int b = 0; b < 4; ++b) {
                    order.push_back((c + w) * 64 + r * 4 + b);
                }
            }
        }
    }
    if (c < mode.degree) {
        for (int r = 0; r < k; ++r) {
            for (int b = 0; b < 4; ++b) {
                order.push_back(c * 64 + r * 4 + b);
            }
        }
    }
    return order;
}

}  // namespace

std::vector<std::uint8_t> tone39_encode_superblock(const Tone39Mode& mode,
                                                   std::span<const std::uint8_t> data) {
    const int n = tone39_rs_n(mode.rate);
    const int k = tone39_rs_k(mode.rate);
    std::vector<std::vector<std::uint8_t>> cw(static_cast<std::size_t>(mode.degree),
                                              std::vector<std::uint8_t>(static_cast<std::size_t>(k), 0));
    const auto order = load_order(mode);
    for (std::size_t i = 0; i < order.size() && i < data.size(); ++i) {
        const int o = order[i];
        if (data[i] != 0) {
            cw[static_cast<std::size_t>(o / 64)][static_cast<std::size_t>((o % 64) / 4)] |=
                static_cast<std::uint8_t>(8U >> static_cast<unsigned>(o % 4));
        }
    }
    for (auto& w : cw) {
        w = tone39_rs_encode(w);
    }
    // Read out a symbol row at a time across the codewords, first bit first.
    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(tone39_superblock_bits(mode)));
    for (int r = 0; r < n; ++r) {
        for (const auto& w : cw) {
            for (int b = 0; b < 4; ++b) {
                out.push_back(static_cast<std::uint8_t>((w[static_cast<std::size_t>(r)] >> (3 - b)) & 1U));
            }
        }
    }
    return out;
}

Tone39BlockDecode tone39_decode_superblock(const Tone39Mode& mode, std::span<const float> soft) {
    const int n = tone39_rs_n(mode.rate);
    Tone39BlockDecode out;
    std::vector<std::vector<std::uint8_t>> cw(static_cast<std::size_t>(mode.degree),
                                              std::vector<std::uint8_t>(static_cast<std::size_t>(n), 0));
    std::size_t i = 0;
    for (int r = 0; r < n; ++r) {
        for (auto& w : cw) {
            for (int b = 0; b < 4; ++b, ++i) {
                if (i < soft.size() && soft[i] > 0.0F) {
                    w[static_cast<std::size_t>(r)] |= static_cast<std::uint8_t>(8U >> static_cast<unsigned>(b));
                }
            }
        }
    }
    for (auto& w : cw) {
        const int e = tone39_rs_decode(w);
        ++out.codewords;
        if (e < 0) {
            ++out.failed;
        } else {
            out.corrected += e;
        }
    }
    const auto order = load_order(mode);
    out.data.reserve(order.size());
    for (const int o : order) {
        const auto sym = cw[static_cast<std::size_t>(o / 64)][static_cast<std::size_t>((o % 64) / 4)];
        out.data.push_back(static_cast<std::uint8_t>((sym >> (3 - (o % 4))) & 1U));
    }
    return out;
}

std::vector<std::uint8_t> tone39_stream(const Tone39Mode& mode, std::span<const std::uint8_t> data) {
    const Tone39Framing* f = tone39_framing(mode.rate, mode.degree);
    if (f == nullptr) {
        return {};
    }
    const auto seq = tone39_framing_sequence(f->length);
    const std::size_t per = static_cast<std::size_t>(tone39_superblock_data_bits(mode));
    const std::size_t blocks = std::max<std::size_t>(1, (data.size() + per - 1) / per);
    std::vector<std::uint8_t> out;
    for (std::size_t b = 0; b < blocks; ++b) {
        if (b % static_cast<std::size_t>(f->superblocks) == 0) {
            out.insert(out.end(), seq.begin(), seq.end());
        }
        std::vector<std::uint8_t> chunk(per, 0);
        for (std::size_t i = 0; i < per && b * per + i < data.size(); ++i) {
            chunk[i] = data[b * per + i];
        }
        const auto coded = tone39_encode_superblock(mode, chunk);
        out.insert(out.end(), coded.begin(), coded.end());
    }
    return out;
}

int tone39_bits_per_element(int rate) { return rate == 2400 ? 78 : 64 * rate / 1200; }

std::vector<std::array<std::uint8_t, 78>> tone39_elements(const Tone39Mode& mode,
                                                         std::span<const std::uint8_t> stream) {
    const int per = tone39_bits_per_element(mode.rate);
    const std::size_t words = (stream.size() + static_cast<std::size_t>(per) - 1) / static_cast<std::size_t>(per);
    const auto bit = [&](std::size_t word, int i) -> std::uint8_t {
        const std::size_t at = word * static_cast<std::size_t>(per) + static_cast<std::size_t>(i);
        return at < stream.size() ? stream[at] : 0;
    };
    std::vector<std::array<std::uint8_t, 78>> out;
    if (mode.rate == 2400) {
        for (std::size_t n = 0; n < words; ++n) {
            auto& e = out.emplace_back();
            for (int i = 0; i < 78; ++i) {
                e[static_cast<std::size_t>(i)] = bit(n, i);
            }
        }
        return out;
    }
    const int d = 1200 / mode.rate;
    const bool time = mode.diversity == Tone39Diversity::TimeFrequency && d > 1;
    const int lag = 16 / d;  // B.5.6.1: word i - k (16 / d) in group k
    const std::size_t elements = words + (time ? static_cast<std::size_t>((d - 1) * lag) : 0);
    for (std::size_t n = 0; n < elements; ++n) {
        auto& e = out.emplace_back();
        for (int g = 0; g < d; ++g) {
            const long long w = static_cast<long long>(n) - (time ? static_cast<long long>(g) * lag : 0);
            for (int i = 0; i < per; ++i) {
                e[static_cast<std::size_t>(g * per + i)] =
                    (w >= 0 && static_cast<std::size_t>(w) < words) ? bit(static_cast<std::size_t>(w), i) : 0;
            }
        }
        // B.5.5: tones 33 to 39 repeat tones 1 to 7.
        for (int i = 0; i < 14; ++i) {
            e[static_cast<std::size_t>(64 + i)] = e[static_cast<std::size_t>(i)];
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

namespace {

// Combines the element soft values into the stream for one rate and
// diversity hypothesis.
std::vector<float> combine(int rate, Tone39Diversity diversity, const std::vector<std::array<float, 78>>& soft) {
    std::vector<float> u;
    if (rate == 2400) {
        for (const auto& e : soft) {
            u.insert(u.end(), e.begin(), e.end());
        }
        return u;
    }
    const int d = 1200 / rate;
    const int per = 64 / d;
    const bool time = diversity == Tone39Diversity::TimeFrequency && d > 1;
    const int lag = 16 / d;
    const std::size_t n_el = soft.size();
    u.assign(n_el * static_cast<std::size_t>(per), 0.0F);
    for (std::size_t n = 0; n < n_el; ++n) {
        std::array<float, 64> e{};
        for (int i = 0; i < 64; ++i) {
            e[static_cast<std::size_t>(i)] = soft[n][static_cast<std::size_t>(i)];
        }
        for (int i = 0; i < 14; ++i) {
            e[static_cast<std::size_t>(i)] += soft[n][static_cast<std::size_t>(64 + i)];
        }
        for (int g = 0; g < d; ++g) {
            const long long w = static_cast<long long>(n) - (time ? static_cast<long long>(g) * lag : 0);
            if (w < 0) {
                continue;
            }
            for (int i = 0; i < per; ++i) {
                u[static_cast<std::size_t>(w) * static_cast<std::size_t>(per) + static_cast<std::size_t>(i)] +=
                    e[static_cast<std::size_t>(g * per + i)];
            }
        }
    }
    if (time) {
        // The last words never had their first copy; they are the tail
        // elements' padding.
        const std::size_t tail = static_cast<std::size_t>((d - 1) * lag) * static_cast<std::size_t>(per);
        u.resize(u.size() > tail ? u.size() - tail : 0);
    }
    return u;
}

double framing_match(std::span<const float> u, int length) {
    if (u.size() < static_cast<std::size_t>(length)) {
        return 0.0;
    }
    const auto seq = tone39_framing_sequence(length);
    double num = 0.0;
    double den = 0.0;
    for (int i = 0; i < length; ++i) {
        const double s = u[static_cast<std::size_t>(i)];
        num += seq[static_cast<std::size_t>(i)] != 0 ? s : -s;
        den += std::abs(s);
    }
    return den > 0.0 ? num / den : 0.0;
}

Tone39Transmission decode_stream(const Tone39Mode& mode, std::span<const float> u) {
    Tone39Transmission t;
    t.mode = mode;
    const Tone39Framing* f = tone39_framing(mode.rate, mode.degree);
    const auto sb = static_cast<std::size_t>(tone39_superblock_bits(mode));
    std::size_t at = 0;
    while (true) {
        at += static_cast<std::size_t>(f->length);
        bool more = true;
        for (int b = 0; b < f->superblocks; ++b) {
            if (at + sb > u.size()) {
                more = false;
                break;
            }
            const auto d = tone39_decode_superblock(mode, u.subspan(at, sb));
            t.data.insert(t.data.end(), d.data.begin(), d.data.end());
            t.codewords += d.codewords;
            t.corrected += d.corrected;
            t.failed += d.failed;
            ++t.superblocks;
            at += sb;
        }
        if (!more) {
            break;
        }
    }
    return t;
}

}  // namespace

Expected<Tone39Decoder> Tone39Decoder::create(const Tone39Config& config) {
    if (config.rate < 6400 || config.rate > 192000) {
        return fail("39-tone decoder: sample rate must be 6.4 to 192 kHz");
    }
    if (config.data_rate != 0 && std::find(kRates.begin(), kRates.end(), config.data_rate) == kRates.end()) {
        return fail("39-tone decoder: data rate must be 75 to 2400 bit/s or 0");
    }
    if (config.data_rate != 0 && config.degree != 0 && tone39_framing(config.data_rate, config.degree) == nullptr) {
        return fail("39-tone decoder: interleaving degree not in table B-I");
    }
    return Tone39Decoder(config);
}

Tone39Decoder::Tone39Decoder(const Tone39Config& config) : config_(config) {
    fs_ = static_cast<double>(config.rate);
    element_ = fs_ * kTone39ElementSeconds;
    window_ = static_cast<int>(std::lround(fs_ / kTone39SpacingHz));
    hop_ = static_cast<int>(std::lround(element_ * 2.0));
}

void Tone39Decoder::reset() {
    buf_.clear();
    base_ = 0;
    end_ = 0;
    state_ = State::Search;
    next_hop_ = 0;
    soft_.clear();
    pending_.clear();
}

float Tone39Decoder::at(std::int64_t i) const {
    return buf_[static_cast<std::size_t>(i - base_)];
}

Tone39Decoder::Cplx Tone39Decoder::correlate(double hz, std::int64_t start) const {
    const Cplx step = std::polar(1.0, -kTwoPi * hz / fs_);
    Cplx ph{1.0, 0.0};
    Cplx acc{};
    for (int i = 0; i < window_; ++i) {
        acc += static_cast<double>(at(start + i)) * ph;
        ph *= step;
    }
    return acc;
}

bool Tone39Decoder::search() {
    // Part 1 (B.5.4.1): four unmodulated tones. Fraction of power in them
    // over eight elements, searched over carrier offsets on a 1 Hz grid.
    const int n = static_cast<int>(std::lround(element_ * 8.0));
    if (next_hop_ + 2 * hop_ + n > end_) {
        return false;
    }
    if (next_hop_ < base_) {
        next_hop_ = base_ + ((hop_ - (base_ % hop_)) % hop_);
    }
    const int span = static_cast<int>(std::ceil(config_.max_offset_hz));
    const auto measure = [&](std::int64_t p, double& best_off) {
        double energy = 0.0;
        for (int i = 0; i < n; ++i) {
            energy += static_cast<double>(at(p + i)) * at(p + i);
        }
        if (energy <= 0.0) {
            return 0.0;
        }
        std::vector<double> power(static_cast<std::size_t>(2 * span + 1), 0.0);
        for (int o = -span; o <= span; ++o) {
            double sum = 0.0;
            for (const auto& t : kPart1) {
                const Cplx step = std::polar(1.0, -kTwoPi * (tone39_frequency(t.tone) + o) / fs_);
                Cplx ph{1.0, 0.0};
                Cplx acc{};
                for (int i = 0; i < n; ++i) {
                    acc += static_cast<double>(at(p + i)) * ph;
                    ph *= step;
                }
                sum += std::norm(acc);
            }
            power[static_cast<std::size_t>(o + span)] = sum;
        }
        const auto it = std::max_element(power.begin(), power.end());
        const auto k = static_cast<int>(it - power.begin());
        double frac = 0.0;
        if (k > 0 && k < 2 * span) {
            const double a = power[static_cast<std::size_t>(k - 1)];
            const double b = *it;
            const double c = power[static_cast<std::size_t>(k + 1)];
            const double den = a - 2.0 * b + c;
            if (den < 0.0) {
                frac = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
            }
        }
        best_off = (k - span) + frac;
        return *it / (0.5 * n * energy);
    };
    double off = 0.0;
    const double r = measure(next_hop_, off);
    if (r < config_.detect_threshold) {
        next_hop_ += hop_;
        return true;
    }
    // Take the best of this hop and the next two, all inside part 1.
    double best = r;
    double best_off = off;
    for (int h = 1; h <= 2; ++h) {
        double o2 = 0.0;
        const double r2 = measure(next_hop_ + h * hop_, o2);
        if (r2 > best) {
            best = r2;
            best_off = o2;
        }
    }
    detect_ = next_hop_;
    offset_ = best_off;
    state_ = State::Timing;
    return true;
}

bool Tone39Decoder::timing() {
    const auto e = element_;
    const std::int64_t need = detect_ + static_cast<std::int64_t>(std::ceil(125.0 * e)) + window_;
    if (need > end_) {
        return false;
    }
    const auto fail_back = [&] {
        state_ = State::Search;
        next_hop_ = detect_ + 3 * hop_;
        return true;
    };
    const auto power = [&](std::span<const Tone39PreambleTone> tones, std::int64_t s) {
        double p = 0.0;
        for (const auto& t : tones) {
            p += std::norm(correlate(tone39_frequency(t.tone) + offset_, s));
        }
        return p;
    };
    // Coarse: the first element step where part 2's tones outweigh part 1's.
    std::int64_t s2 = -1;
    for (int m = 0; m < 66; ++m) {
        const auto s = detect_ + static_cast<std::int64_t>(std::llround(m * e));
        double energy = 0.0;
        for (int i = 0; i < window_; ++i) {
            energy += static_cast<double>(at(s + i)) * at(s + i);
        }
        // Part 2's tones must hold a real share of the window's power, not
        // just beat part 1's in noise ahead of the preamble.
        const double p2 = power(kPart2, s);
        if (p2 > power(kPart1, s) && p2 > config_.detect_threshold * 0.5 * window_ * energy) {
            s2 = s;
            break;
        }
    }
    if (s2 < 0) {
        return fail_back();
    }
    // Fine: part 2 tones change phase by the same amount every element, so
    // the sum of element-to-element products is coherent while the window
    // sits inside an element and falls as it straddles a boundary.
    const int period = static_cast<int>(std::lround(e));
    std::vector<double> f(static_cast<std::size_t>(period), 0.0);
    for (int u = 0; u < period; ++u) {
        double sum = 0.0;
        for (const auto& t : kPart2) {
            const double hz = tone39_frequency(t.tone) + offset_;
            Cplx acc{};
            Cplx prev = correlate(hz, s2 + u);
            for (int m = 1; m <= 4; ++m) {
                const Cplx cur = correlate(hz, s2 + u + static_cast<std::int64_t>(std::llround(m * e)));
                acc += prev * std::conj(cur);
                prev = cur;
            }
            sum += std::abs(acc);
        }
        f[static_cast<std::size_t>(u)] = sum;
    }
    const auto best = static_cast<int>(std::max_element(f.begin(), f.end()) - f.begin());
    const double thr = 0.9 * f[static_cast<std::size_t>(best)];
    int lo = best;
    int hi = best;
    while (hi - lo < period - 1 && f[static_cast<std::size_t>(((lo - 1) % period + period) % period)] > thr) {
        --lo;
    }
    while (hi - lo < period - 1 && f[static_cast<std::size_t>((hi + 1) % period)] > thr) {
        ++hi;
    }
    const double centre = static_cast<double>(s2) + 0.5 * (lo + hi);
    // Part 3: the first element where the other tones appear.
    std::vector<Tone39PreambleTone> others;
    for (const auto& t : kPart3) {
        if (t.tone != 9 && t.tone != 21 && t.tone != 33) {
            others.push_back(t);
        }
    }
    for (int m = 0; m < 30; ++m) {
        const auto s = static_cast<std::int64_t>(std::llround(centre + m * e));
        double p_other = 0.0;
        for (const auto& t : others) {
            const double hz = t.tone == 0 ? kTone39DopplerHz : tone39_frequency(t.tone);
            p_other += std::norm(correlate(hz + offset_, s));
        }
        if (p_other > power(kPart2, s)) {
            // Refine the timing on the elements that follow part 3. Every
            // phase change is an odd multiple of 45 degrees (zero in an
            // extended part 3), so the fourth power of the element-to-element
            // products adds coherently across the tones while the window
            // lies inside an element, and not when it straddles an onset.
            // Energy alone would not do: what a straddling window loses at
            // one tone leaks into its neighbours.
            const double coarse = centre + m * e;
            std::vector<double> g(static_cast<std::size_t>(period), 0.0);
            std::array<Cplx, kTone39DataTones> prev{};
            for (int v = 0; v < period; ++v) {
                double sum = 0.0;
                for (int n = 0; n <= 24; ++n) {
                    const auto at_n = static_cast<std::int64_t>(std::llround(coarse + v + n * e));
                    Cplx q{};
                    for (int k = 1; k <= kTone39DataTones; ++k) {
                        const Cplx cur = correlate(tone39_frequency(k) + offset_, at_n);
                        const Cplx d = cur * std::conj(prev[static_cast<std::size_t>(k - 1)]);
                        prev[static_cast<std::size_t>(k - 1)] = cur;
                        const double a = std::abs(d);
                        if (n > 0 && a > 0.0) {
                            q += d * d * d * d / (a * a * a);
                        }
                    }
                    sum += std::abs(q);
                }
                g[static_cast<std::size_t>(v)] = sum;
            }
            {
                // Smooth over +/- 3 samples, circularly, against noise.
                std::vector<double> sm(g.size(), 0.0);
                for (int v = 0; v < period; ++v) {
                    for (int j = -3; j <= 3; ++j) {
                        sm[static_cast<std::size_t>(v)] +=
                            g[static_cast<std::size_t>((((v + j) % period) + period) % period)];
                    }
                }
                g = std::move(sm);
            }
            // Part 2's estimate is good to a fraction of the guard time; at
            // low signal to noise the fourth power metric has false peaks
            // further out, so the peak is taken within a sixth of an element.
            const int reach = period / 6;
            int gb = 0;
            for (int v = -reach; v <= reach; ++v) {
                const auto i = static_cast<std::size_t>((v + period) % period);
                if (g[i] > g[static_cast<std::size_t>(gb)]) {
                    gb = static_cast<int>(i);
                }
            }
            const double gt = 0.95 * g[static_cast<std::size_t>(gb)];
            const auto gat = [&](int i) { return g[static_cast<std::size_t>(((i % period) + period) % period)]; };
            int glo = gb;
            int ghi = gb;
            while (ghi - glo < period - 1 && gat(glo - 1) > gt) {
                --glo;
            }
            while (ghi - glo < period - 1 && gat(ghi + 1) > gt) {
                ++ghi;
            }
            double v_centre = 0.5 * (glo + ghi);
            // The representative nearest the coarse estimate, so part 3 stays
            // the element it was found to be.
            v_centre = std::remainder(v_centre, static_cast<double>(period));
            t0_ = coarse + v_centre;
            state_ = State::Data;
            element_index_ = 0;
            extended_ = false;
            in_preamble_ = true;
            rho_ = 0.0;
            doppler_power_ = 0.0;
            weak_ = 0;
            soft_.clear();
            return true;
        }
    }
    return fail_back();
}

bool Tone39Decoder::data_element() {
    const double exact = t0_ + element_index_ * element_;
    const auto s = static_cast<std::int64_t>(std::llround(exact));
    if (s + window_ > end_) {
        return false;
    }
    std::array<Cplx, 40> x{};
    for (int k = 0; k < 40; ++k) {
        const double hz = (k == 0 ? kTone39DopplerHz : tone39_frequency(k)) + offset_;
        // Refer the phase to the exact element position, not the rounded one.
        x[static_cast<std::size_t>(k)] =
            correlate(hz, s) * std::polar(1.0, -kTwoPi * hz * (static_cast<double>(s) - exact) / fs_);
    }
    const int index = element_index_++;
    if (index == 0) {
        prev_ = x;
        doppler_power_ = std::norm(x[0]);
        return true;
    }
    std::array<Cplx, 40> d{};
    for (std::size_t k = 0; k < 40; ++k) {
        d[k] = x[k] * std::conj(prev_[k]);
    }
    prev_ = x;
    // Rotation per element common to every tone: the Doppler tone's advance
    // beyond its nominal 393.75 Hz.
    const double rho_meas = wrap(std::arg(d[0]) - kTwoPi * kTone39DopplerHz * element_ / fs_);
    const double pd = std::norm(x[0]);
    if (in_preamble_) {
        double num = 0.0;
        double den = 0.0;
        for (std::size_t k = 1; k < 40; ++k) {
            num += std::real(d[k] * std::polar(1.0, -rho_meas));
            den += std::abs(d[k]);
        }
        if (den > 0.0 && num / den > 0.6 && index <= 12) {
            // B.5.4.2: extended part 3 repeats the reference phases.
            extended_ = true;
            rho_ = rho_meas;
            doppler_power_ = 0.8 * doppler_power_ + 0.2 * pd;
            return true;
        }
        in_preamble_ = false;
        rho_ = rho_meas;
    }
    // Refine with the data tones: every phase change is an odd multiple of
    // 45 degrees, so the fourth power of the corrected change is -1.
    Cplx q{};
    double amp = 0.0;
    const Cplx coarse = std::polar(1.0, -rho_);
    for (std::size_t k = 1; k < 40; ++k) {
        const double a = std::abs(d[k]);
        if (a <= 0.0) {
            continue;
        }
        const Cplx u = d[k] * coarse / a;
        q += a * u * u * u * u;
        amp += a;
    }
    if (amp > 0.0) {
        // The offset is fixed and the estimate from one element is noisy at
        // low signal to noise, so follow it with a first order loop.
        rho_ = wrap(rho_ + 0.3 * std::arg(-q) / 4.0);
    }
    std::array<float, 78> soft{};
    const double scale = amp > 0.0 ? 39.0 / amp : 1.0;
    for (std::size_t k = 1; k < 40; ++k) {
        const Cplx u = d[k] * std::polar(scale, -rho_);
        // Table B-V: earlier bit MARK at 225 and 315 degrees, later bit MARK
        // at 45 and 315.
        soft[2 * (k - 1)] = static_cast<float>(-std::imag(u));
        soft[2 * (k - 1) + 1] = static_cast<float>(std::real(u));
    }
    soft_.push_back(soft);
    // End of transmission: the Doppler tone gone for three elements.
    if (pd < 0.1 * doppler_power_) {
        if (++weak_ >= 3) {
            soft_.resize(soft_.size() > 3 ? soft_.size() - 3 : 0);
            finish(pending_);
            state_ = State::Search;
            next_hop_ = s;
            return true;
        }
    } else {
        weak_ = 0;
        doppler_power_ = 0.95 * doppler_power_ + 0.05 * pd;
    }
    if (soft_.size() > (1U << 20U)) {
        finish(pending_);
        state_ = State::Search;
        next_hop_ = s;
    }
    return true;
}

void Tone39Decoder::finish(std::vector<Tone39Transmission>& out) {
    if (soft_.empty()) {
        return;
    }
    struct Hypo {
        int rate;
        Tone39Diversity div;
        int length;
        double match;
    };
    Hypo best{0, Tone39Diversity::TimeFrequency, 0, -2.0};
    std::vector<float> best_u;
    for (const int rate : kRates) {
        if (config_.data_rate != 0 && rate != config_.data_rate) {
            continue;
        }
        std::vector<Tone39Diversity> divs = {config_.diversity};
        if (config_.auto_diversity && rate < 1200) {
            divs = {Tone39Diversity::TimeFrequency, Tone39Diversity::Frequency};
        }
        for (const auto div : divs) {
            const auto u = combine(rate, div, soft_);
            for (const auto& f : kFraming) {
                if (f.rate != rate || (config_.degree != 0 && f.degree != config_.degree)) {
                    continue;
                }
                const double m = framing_match(u, f.length);
                if (m > best.match + 1e-9) {
                    best = {rate, div, f.length, m};
                    best_u = u;
                }
            }
        }
    }
    if (best.rate == 0) {
        soft_.clear();
        return;
    }
    Tone39Transmission chosen;
    bool have = false;
    for (const auto& f : kFraming) {
        if (f.rate != best.rate || f.length != best.length ||
            (config_.degree != 0 && f.degree != config_.degree)) {
            continue;
        }
        auto t = decode_stream({best.rate, f.degree, best.div}, best_u);
        const auto score = [](const Tone39Transmission& a) {
            return a.codewords == 0 ? 1.0 : static_cast<double>(a.failed) / a.codewords;
        };
        if (!have || score(t) < score(chosen)) {
            chosen = std::move(t);
            have = true;
        }
    }
    chosen.position = static_cast<std::int64_t>(std::llround(t0_ - 0.5 * (element_ - window_)));
    chosen.offset_hz = offset_;
    chosen.extended_preamble = extended_;
    chosen.elements = static_cast<int>(soft_.size());
    chosen.framing_match = best.match;
    out.push_back(std::move(chosen));
    soft_.clear();
}

void Tone39Decoder::trim() {
    std::int64_t keep = end_;
    switch (state_) {
    case State::Search: keep = next_hop_; break;
    case State::Timing: keep = detect_; break;
    case State::Data:
        keep = static_cast<std::int64_t>(std::floor(t0_ + element_index_ * element_)) - 2;
        break;
    }
    keep = std::min(keep, end_);
    if (keep - base_ > 65536) {
        const auto drop = static_cast<std::size_t>(keep - base_);
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(drop));
        base_ = keep;
    }
}

bool Tone39Decoder::step() {
    switch (state_) {
    case State::Search: return search();
    case State::Timing: return timing();
    case State::Data: return data_element();
    }
    return false;
}

void Tone39Decoder::process(dsp::ConstRealSpan audio, std::vector<Tone39Transmission>& out) {
    buf_.insert(buf_.end(), audio.begin(), audio.end());
    end_ += static_cast<std::int64_t>(audio.size());
    while (step()) {
        trim();
    }
    for (auto& t : pending_) {
        out.push_back(std::move(t));
    }
    pending_.clear();
}

void Tone39Decoder::flush(std::vector<Tone39Transmission>& out) {
    // Silence long enough for a preamble found near the end to be resolved
    // and a transmission in progress to end on its own.
    const auto pad = static_cast<std::size_t>(std::ceil(135.0 * element_)) + static_cast<std::size_t>(4 * hop_);
    buf_.insert(buf_.end(), pad, 0.0F);
    end_ += static_cast<std::int64_t>(pad);
    while (step()) {
    }
    if (state_ == State::Data) {
        finish(pending_);
    }
    for (auto& t : pending_) {
        out.push_back(std::move(t));
    }
    pending_.clear();
    const auto e = end_;
    reset();
    base_ = e;
    end_ = e;
    next_hop_ = e;
}

}  // namespace revenant::decode
