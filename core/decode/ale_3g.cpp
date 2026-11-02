#include "core/decode/ale_3g.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>

namespace revenant::decode {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

// Table C-V, repeated as table C-X.
constexpr std::array<std::uint8_t, 256> kTlc = {
    2, 6, 1, 6, 1, 6, 3, 0, 6, 0, 1, 1, 5, 0, 0, 6, 2, 6, 2, 1, 6, 2, 3, 2, 7, 6, 4, 3, 0, 2, 3, 5,
    2, 7, 5, 1, 5, 1, 7, 6, 1, 7, 1, 5, 4, 4, 0, 7, 2, 2, 6, 2, 2, 2, 6, 3, 3, 3, 7, 7, 3, 2, 4, 5,
    0, 7, 4, 7, 7, 7, 2, 3, 1, 6, 7, 6, 5, 7, 0, 5, 1, 0, 7, 6, 2, 4, 0, 2, 7, 5, 5, 4, 1, 5, 1, 5,
    6, 7, 3, 0, 2, 7, 6, 6, 4, 0, 7, 4, 3, 2, 2, 6, 6, 7, 4, 7, 2, 0, 2, 7, 2, 1, 5, 4, 6, 2, 3, 2,
    1, 6, 0, 7, 1, 1, 2, 6, 2, 2, 0, 2, 2, 3, 6, 7, 1, 7, 1, 7, 1, 5, 7, 7, 2, 2, 2, 0, 4, 3, 4, 2,
    0, 6, 7, 6, 0, 5, 0, 7, 1, 7, 4, 1, 2, 3, 4, 6, 7, 2, 2, 0, 6, 4, 4, 6, 6, 4, 2, 2, 6, 5, 3, 4,
    2, 3, 5, 7, 7, 1, 0, 0, 0, 3, 1, 2, 0, 1, 6, 2, 7, 4, 4, 3, 2, 5, 4, 5, 6, 4, 2, 5, 6, 2, 2, 4,
    7, 0, 6, 2, 3, 7, 2, 5, 4, 2, 4, 1, 5, 5, 3, 6, 1, 1, 3, 2, 7, 5, 7, 0, 7, 3, 5, 0, 0, 1, 2, 0,
};

// Table C-VI.
constexpr std::array<std::uint8_t, 384> kBw0Preamble = {
    7, 7, 7, 7, 5, 4, 3, 1, 1, 2, 0, 2, 7, 2, 2, 0, 1, 3, 4, 7, 5, 3, 7, 7, 4, 3, 1, 0, 1, 1, 5, 2,
    1, 6, 0, 0, 4, 7, 6, 2, 2, 3, 6, 0, 5, 1, 7, 6, 1, 6, 1, 7, 6, 6, 6, 1, 7, 3, 0, 4, 7, 1, 2, 2,
    3, 3, 6, 7, 7, 1, 7, 3, 1, 5, 0, 3, 3, 4, 5, 2, 5, 2, 5, 3, 1, 7, 2, 1, 5, 7, 6, 1, 2, 5, 3, 5,
    3, 6, 2, 0, 7, 5, 6, 6, 0, 1, 4, 2, 5, 4, 1, 1, 7, 0, 0, 6, 6, 7, 5, 6, 3, 7, 4, 0, 2, 6, 3, 6,
    4, 5, 1, 0, 0, 4, 5, 5, 4, 7, 1, 5, 1, 5, 6, 7, 3, 3, 5, 2, 2, 2, 7, 2, 3, 3, 0, 4, 1, 4, 1, 3,
    6, 0, 7, 2, 6, 1, 5, 0, 1, 4, 1, 1, 7, 0, 7, 4, 0, 2, 4, 5, 3, 0, 0, 3, 1, 2, 6, 4, 6, 5, 2, 6,
    0, 0, 7, 3, 5, 3, 4, 0, 6, 2, 7, 2, 3, 3, 7, 6, 7, 1, 0, 0, 6, 7, 3, 1, 5, 5, 0, 2, 3, 4, 2, 7,
    7, 4, 5, 2, 1, 6, 1, 0, 4, 7, 1, 6, 1, 2, 4, 0, 3, 6, 5, 4, 5, 4, 4, 6, 1, 2, 5, 1, 3, 6, 2, 7,
    2, 6, 7, 4, 7, 3, 0, 1, 5, 0, 5, 3, 4, 5, 0, 7, 3, 2, 7, 0, 3, 2, 7, 0, 6, 1, 6, 7, 7, 1, 4, 2,
    6, 7, 7, 4, 2, 7, 2, 7, 3, 7, 6, 3, 2, 6, 5, 6, 6, 3, 6, 6, 4, 1, 0, 6, 2, 6, 4, 1, 5, 5, 4, 3,
    3, 4, 6, 3, 5, 2, 4, 1, 1, 7, 5, 3, 7, 1, 6, 5, 4, 6, 6, 2, 3, 4, 2, 3, 3, 7, 4, 1, 4, 4, 5, 4,
    6, 1, 3, 4, 6, 1, 7, 4, 1, 3, 5, 2, 6, 5, 5, 4, 2, 1, 5, 1, 6, 1, 2, 7, 1, 4, 4, 2, 3, 4, 7, 3,
};

// Table C-XI.
constexpr std::array<std::uint8_t, 576> kBw1Preamble = {
    4, 4, 7, 3, 7, 7, 1, 0, 4, 7, 1, 6, 6, 5, 5, 0, 6, 4, 0, 6, 0, 1, 6, 2, 2, 7, 3, 5, 1, 5, 4, 5,
    5, 6, 6, 0, 2, 0, 4, 0, 7, 0, 2, 6, 3, 7, 1, 5, 2, 3, 2, 3, 7, 1, 1, 7, 0, 0, 0, 7, 4, 5, 2, 3,
    2, 3, 7, 3, 1, 0, 3, 4, 2, 5, 6, 6, 6, 5, 2, 3, 2, 7, 6, 7, 6, 6, 1, 0, 1, 2, 6, 5, 6, 5, 1, 4,
    3, 5, 2, 6, 5, 6, 5, 2, 5, 2, 0, 0, 2, 6, 7, 0, 4, 2, 2, 5, 0, 2, 5, 1, 5, 2, 1, 2, 3, 4, 1, 7,
    4, 1, 0, 5, 1, 1, 4, 1, 6, 2, 6, 5, 2, 2, 3, 1, 7, 1, 0, 6, 0, 4, 6, 2, 3, 3, 6, 1, 5, 0, 4, 2,
    5, 1, 4, 6, 7, 2, 1, 5, 4, 1, 4, 7, 7, 1, 5, 4, 1, 7, 0, 2, 6, 2, 4, 7, 1, 1, 3, 0, 6, 4, 2, 1,
    7, 3, 2, 5, 4, 0, 4, 4, 5, 4, 6, 7, 7, 2, 6, 7, 1, 1, 4, 6, 0, 5, 1, 6, 6, 1, 2, 3, 2, 5, 3, 4,
    5, 2, 0, 4, 1, 4, 6, 5, 2, 6, 3, 2, 2, 3, 0, 7, 7, 0, 2, 2, 1, 6, 6, 6, 0, 5, 1, 3, 4, 5, 1, 6,
    7, 2, 2, 2, 1, 3, 7, 5, 7, 0, 6, 6, 5, 7, 2, 4, 0, 3, 0, 6, 1, 4, 3, 4, 0, 1, 5, 4, 5, 1, 5, 7,
    6, 5, 6, 4, 7, 7, 0, 1, 4, 3, 5, 6, 1, 5, 7, 1, 5, 3, 1, 0, 5, 5, 0, 4, 2, 2, 2, 5, 2, 4, 5, 3,
    6, 2, 6, 3, 5, 0, 4, 0, 0, 7, 3, 5, 1, 4, 5, 5, 2, 5, 2, 6, 6, 3, 7, 6, 0, 2, 7, 1, 4, 3, 5, 2,
    6, 1, 2, 0, 6, 5, 1, 7, 1, 0, 6, 3, 0, 4, 7, 6, 0, 5, 0, 4, 1, 5, 7, 0, 4, 6, 6, 1, 7, 0, 5, 1,
    6, 0, 6, 4, 6, 6, 1, 4, 6, 3, 3, 2, 1, 4, 4, 1, 4, 6, 7, 2, 6, 2, 4, 6, 1, 0, 5, 0, 4, 0, 5, 4,
    4, 2, 5, 2, 7, 2, 4, 4, 7, 3, 6, 4, 7, 5, 6, 5, 6, 5, 5, 3, 2, 3, 4, 7, 5, 7, 2, 7, 1, 5, 5, 3,
    0, 3, 5, 4, 2, 1, 3, 7, 1, 5, 4, 4, 3, 7, 5, 5, 5, 4, 7, 0, 7, 7, 1, 0, 5, 4, 7, 0, 4, 6, 7, 1,
    0, 0, 3, 4, 5, 4, 7, 0, 3, 3, 2, 2, 2, 0, 3, 2, 7, 0, 0, 3, 0, 5, 3, 7, 1, 4, 2, 3, 5, 3, 5, 7,
    1, 3, 3, 1, 0, 1, 1, 6, 5, 1, 5, 1, 5, 0, 7, 0, 2, 5, 7, 6, 7, 7, 3, 1, 0, 3, 1, 4, 2, 3, 5, 1,
    4, 0, 2, 1, 7, 1, 1, 7, 4, 5, 0, 1, 0, 0, 3, 6, 6, 6, 6, 3, 7, 3, 2, 6, 0, 3, 7, 5, 1, 0, 1, 6,
};

// Table C-IX, repeated as table C-XIII.
constexpr std::array<std::uint8_t, 256> kPn = {
    0, 2, 4, 3, 3, 6, 4, 5, 7, 6, 7, 0, 5, 5, 4, 3, 5, 4, 3, 7, 0, 7, 6, 2, 6, 2, 4, 6, 7, 2, 4, 7,
    5, 5, 7, 0, 7, 3, 3, 3, 7, 3, 3, 1, 4, 2, 3, 7, 0, 2, 7, 7, 3, 5, 1, 0, 1, 4, 0, 5, 0, 0, 0, 0,
    6, 5, 0, 1, 2, 7, 6, 5, 5, 2, 7, 3, 3, 3, 2, 1, 2, 5, 6, 1, 3, 4, 2, 1, 0, 1, 2, 3, 6, 4, 7, 5,
    2, 2, 6, 2, 7, 6, 5, 2, 4, 6, 5, 4, 7, 2, 5, 1, 0, 0, 7, 7, 3, 5, 4, 2, 1, 4, 2, 7, 0, 3, 4, 0,
    0, 0, 7, 7, 3, 5, 4, 2, 1, 4, 2, 7, 0, 3, 4, 0, 1, 0, 5, 2, 6, 0, 3, 5, 1, 0, 5, 1, 5, 2, 5, 6,
    3, 2, 3, 7, 1, 2, 2, 0, 7, 1, 3, 6, 4, 2, 6, 2, 7, 4, 3, 7, 6, 7, 2, 3, 1, 7, 4, 1, 5, 1, 5, 4,
    7, 1, 1, 2, 3, 6, 7, 7, 6, 6, 1, 2, 2, 4, 1, 7, 7, 5, 5, 4, 7, 7, 5, 0, 7, 3, 7, 5, 7, 7, 5, 0,
    6, 6, 6, 1, 3, 4, 4, 4, 0, 3, 3, 2, 1, 4, 5, 4, 5, 3, 1, 1, 1, 2, 5, 1, 7, 1, 5, 7, 2, 0, 0, 6,
};

struct Interleaver {
    int rows, cols, irs, ics, dirs, dics, irf, icf, dirf, dicf;
};
// Tables C-VII and C-XII.
constexpr Interleaver kBw0Interleaver{4, 13, 0, 1, 1, 0, 1, 0, 0, 1};
constexpr Interleaver kBw1Interleaver{16, 9, 0, 1, 1, 0, 1, 0, 0, 1};

// Figures C-6 and C-8 as tap masks over the coder window: bit 0 is the cell
// "1" (the oldest bit), bit j is cell xj, the highest the newest bit.
struct Code {
    int k;
    std::array<std::uint32_t, 3> taps;
    int outputs;
};
constexpr std::uint32_t taps_of(std::initializer_list<int> cells) {
    std::uint32_t m = 0;
    for (const int c : cells) {
        m |= 1U << static_cast<unsigned>(c);
    }
    return m;
}
constexpr Code kBw0Code{7, {taps_of({6, 4, 3, 1, 0}), taps_of({6, 5, 4, 3, 0}), 0}, 2};
constexpr Code kBw1Code{9, {taps_of({8, 7, 6, 3, 0}), taps_of({8, 7, 5, 4, 1, 0}), taps_of({8, 6, 5, 3, 2, 1, 0})}, 3};

const Code& code_of(Ale3gWaveform w) { return w == Ale3gWaveform::Bw0 ? kBw0Code : kBw1Code; }
const Interleaver& interleaver_of(Ale3gWaveform w) {
    return w == Ale3gWaveform::Bw0 ? kBw0Interleaver : kBw1Interleaver;
}

// Table C-VIII: element j of the Walsh sequence for coded bits n (b3 the
// first fetched) is 4 when the parity of n & j is odd.
int walsh(int n, int j) { return (std::popcount(static_cast<unsigned>(n & j)) & 1) != 0 ? 4 : 0; }

std::vector<std::uint8_t> payload_bits(Ale3gWaveform w, std::uint64_t payload) {
    const int n = ale3g_payload_bits(w);
    std::vector<std::uint8_t> b(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        b[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((payload >> (n - 1 - i)) & 1U);
    }
    return b;
}

std::uint64_t field(std::uint64_t payload, int bits, int from, int len) {
    return (payload >> (bits - from - len)) & ((std::uint64_t{1} << len) - 1);
}

// Tail-biting Viterbi decoding over three passes of the circular trellis,
// keeping the middle pass. Soft values positive for 1.
std::vector<std::uint8_t> viterbi(const Code& c, std::span<const float> soft, int n_bits) {
    const int mem = c.k - 1;
    const int states = 1 << mem;
    const int passes = 3;
    const int steps = passes * n_bits;
    std::vector<double> metric(static_cast<std::size_t>(states), 0.0);
    std::vector<double> next(static_cast<std::size_t>(states));
    std::vector<std::uint8_t> from(static_cast<std::size_t>(steps * states));
    for (int i = 0; i < steps; ++i) {
        const int bit_index = i % n_bits;
        std::fill(next.begin(), next.end(), -std::numeric_limits<double>::infinity());
        for (int s = 0; s < states; ++s) {
            for (int b = 0; b < 2; ++b) {
                const std::uint32_t window = static_cast<std::uint32_t>(s) | (static_cast<std::uint32_t>(b) << mem);
                double m = metric[static_cast<std::size_t>(s)];
                for (int o = 0; o < c.outputs; ++o) {
                    const int bit = std::popcount(window & c.taps[static_cast<std::size_t>(o)]) & 1;
                    const float l = soft[static_cast<std::size_t>(bit_index * c.outputs + o)];
                    m += bit != 0 ? l : -l;
                }
                const auto ns = static_cast<std::size_t>(window >> 1U);
                if (m > next[ns]) {
                    next[ns] = m;
                    // The bit leaving the window identifies the predecessor.
                    from[static_cast<std::size_t>(i * states) + ns] = static_cast<std::uint8_t>(s & 1);
                }
            }
        }
        metric.swap(next);
    }
    auto s = static_cast<int>(std::max_element(metric.begin(), metric.end()) - metric.begin());
    std::vector<std::uint8_t> decided(static_cast<std::size_t>(steps));
    for (int i = steps - 1; i >= 0; --i) {
        // The newest bit of the state after step i is the bit decided there.
        decided[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((s >> (mem - 1)) & 1);
        const int old = from[static_cast<std::size_t>(i * states + s)];
        s = ((s << 1) & (states - 1)) | old;
    }
    return {decided.begin() + n_bits, decided.begin() + 2 * n_bits};
}

}  // namespace

std::span<const std::uint8_t> ale3g_tlc() { return kTlc; }
std::span<const std::uint8_t> ale3g_preamble(Ale3gWaveform w) {
    if (w == Ale3gWaveform::Bw0) {
        return kBw0Preamble;
    }
    return kBw1Preamble;
}
std::span<const std::uint8_t> ale3g_pn() { return kPn; }

std::uint32_t ale3g_crc(std::span<const std::uint8_t> bits, int k, std::uint32_t poly) {
    // Figure C-4: each bit added at the C_{K-1} end; the sum feeds back into
    // every cell whose coefficient is one.
    const std::uint32_t mask = (1U << static_cast<unsigned>(k)) - 1U;
    std::uint32_t c = 0;
    for (const std::uint8_t b : bits) {
        const std::uint32_t fb = ((c >> static_cast<unsigned>(k - 1)) & 1U) ^ (b & 1U);
        c = (c << 1U) & mask;
        if (fb != 0) {
            c ^= poly;
        }
    }
    return c;
}

int ale3g_link_id(std::uint32_t caller, std::uint32_t responder) {
    const std::uint32_t t1 = caller * 0x13C6EFU;
    const std::uint32_t t2 = responder * 0x13C6EFU;
    return static_cast<int>(((t1 >> 4U) + (t2 >> 15U)) & 0x3FU);
}

std::vector<int> ale3g_interleave_order(Ale3gWaveform w) {
    const auto& p = interleaver_of(w);
    const int n = p.rows * p.cols;
    std::vector<int> matrix(static_cast<std::size_t>(n), 0);
    int rs = 0;
    int cs = 0;
    for (int s = 0; s < n;) {
        matrix[static_cast<std::size_t>(rs * p.cols + cs)] = s;
        ++s;
        cs = (cs + p.ics + ((s % p.rows) == 0 ? p.dics : 0)) % p.cols;
        rs = (rs + p.irs + ((s % p.cols) == 0 ? p.dirs : 0)) % p.rows;
    }
    std::vector<int> order;
    order.reserve(static_cast<std::size_t>(n));
    int rf = 0;
    int cf = 0;
    for (int f = 0; f < n;) {
        order.push_back(matrix[static_cast<std::size_t>(rf * p.cols + cf)]);
        ++f;
        cf = (cf + p.icf + ((f % p.rows) == 0 ? p.dicf : 0)) % p.cols;
        rf = (rf + p.irf + ((f % p.cols) == 0 ? p.dirf : 0)) % p.rows;
    }
    return order;
}

std::vector<std::uint8_t> ale3g_encode(Ale3gWaveform w, std::span<const std::uint8_t> payload) {
    const auto& c = code_of(w);
    const int mem = c.k - 1;
    const int n = static_cast<int>(payload.size());
    std::vector<std::uint8_t> out;
    out.reserve(payload.size() * static_cast<std::size_t>(c.outputs));
    for (int i = 0; i < n; ++i) {
        // Window bit j holds payload bit i - mem + j, cyclically.
        std::uint32_t window = 0;
        for (int j = 0; j <= mem; ++j) {
            const int src = ((i - mem + j) % n + n) % n;
            window |= static_cast<std::uint32_t>(payload[static_cast<std::size_t>(src)] & 1U) << static_cast<unsigned>(j);
        }
        for (int o = 0; o < c.outputs; ++o) {
            out.push_back(static_cast<std::uint8_t>(std::popcount(window & c.taps[static_cast<std::size_t>(o)]) & 1));
        }
    }
    return out;
}

std::vector<std::uint8_t> ale3g_data(Ale3gWaveform w, std::uint64_t payload) {
    const auto coded = ale3g_encode(w, payload_bits(w, payload));
    const auto order = ale3g_interleave_order(w);
    std::vector<std::uint8_t> out;
    out.reserve(ale3g_data_symbols(w));
    for (std::size_t g = 0; g + 3 < order.size(); g += 4) {
        int n = 0;
        for (std::size_t b = 0; b < 4; ++b) {
            n = (n << 1) | coded[static_cast<std::size_t>(order[g + b])];
        }
        for (int rep = 0; rep < 4; ++rep) {
            for (int j = 0; j < 16; ++j) {
                out.push_back(static_cast<std::uint8_t>(walsh(n, j)));
            }
        }
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>((out[i] + kPn[i % kPn.size()]) % 8U);
    }
    return out;
}

std::vector<std::uint8_t> ale3g_burst(Ale3gWaveform w, std::uint64_t payload) {
    std::vector<std::uint8_t> out(kTlc.begin(), kTlc.end());
    const auto pre = ale3g_preamble(w);
    out.insert(out.end(), pre.begin(), pre.end());
    const auto data = ale3g_data(w, payload);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

const char* ale3g_pdu_name(Ale3gPduType t) {
    switch (t) {
    case Ale3gPduType::Call: return "LE_Call";
    case Ale3gPduType::Handshake: return "LE_Handshake";
    case Ale3gPduType::Notification: return "LE_Notification";
    case Ale3gPduType::TimeOffset: return "LE_Time_Offset";
    case Ale3gPduType::GroupTime: return "LE_Group_Time_Broadcast";
    case Ale3gPduType::Broadcast: return "LE_Broadcast";
    case Ale3gPduType::ScanningCall: return "LE_Scanning_Call";
    case Ale3gPduType::Reserved: return "reserved";
    case Ale3gPduType::TmRequest: return "TM_REQ";
    case Ale3gPduType::TmConfirm: return "TM_CONF";
    case Ale3gPduType::TmTerminate: return "TM_TERM";
    case Ale3gPduType::TmReserved: return "TM reserved type";
    case Ale3gPduType::OtherBw1: return "BW1 non-TM";
    }
    return "?";
}

namespace {

bool crc_ok(std::uint64_t payload, int bits, int k, std::uint32_t poly) {
    std::vector<std::uint8_t> b(static_cast<std::size_t>(bits - k));
    for (int i = 0; i < bits - k; ++i) {
        b[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((payload >> (bits - 1 - i)) & 1U);
    }
    return ale3g_crc(b, k, poly) == (payload & ((std::uint64_t{1} << k) - 1));
}

std::uint64_t with_crc(std::uint64_t body, int bits, int k, std::uint32_t poly) {
    // body holds the bits - k leading bits.
    std::vector<std::uint8_t> b(static_cast<std::size_t>(bits - k));
    for (int i = 0; i < bits - k; ++i) {
        b[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((body >> (bits - k - 1 - i)) & 1U);
    }
    return (body << k) | ale3g_crc(b, k, poly);
}

}  // namespace

Ale3gPdu ale3g_parse(Ale3gWaveform w, std::uint64_t payload) {
    Ale3gPdu p;
    const auto f = [&](int from, int len) { return static_cast<int>(field(payload, ale3g_payload_bits(w), from, len)); };
    if (w == Ale3gWaveform::Bw1) {
        // Table C-XXXVI.
        p.protocol = f(0, 3);
        if (p.protocol != 1) {
            p.type = Ale3gPduType::OtherBw1;
            return p;
        }
        p.priority = f(3, 2);
        p.dest_address = f(5, 11);
        p.source_address = f(16, 11);
        const int type = f(27, 3);
        p.argument = f(30, 6);
        p.type = type == 0 ? Ale3gPduType::TmRequest
               : type == 1 ? Ale3gPduType::TmConfirm
               : type == 2 ? Ale3gPduType::TmTerminate
                           : Ale3gPduType::TmReserved;
        p.crc_ok = crc_ok(payload, 48, 12, kAle3gCrc12);
        return p;
    }
    // Figure C-17.
    const int lead = f(0, 2);
    if (lead == 2) {
        const int six = f(2, 6);
        if (six == 0x3F) {
            p.type = Ale3gPduType::Notification;
            p.caller_status = f(8, 3);
        } else {
            p.type = Ale3gPduType::Call;
            p.called_member = six;
            p.call_type = f(8, 3);
        }
        p.caller_member = f(11, 6);
        p.caller_group = f(17, 5);
        p.crc_ok = crc_ok(payload, 26, 4, kAle3gCrc4);
        return p;
    }
    if (lead == 0) {
        p.type = Ale3gPduType::Handshake;
        p.link_id = f(2, 6);
        p.command = f(8, 3);
        p.argument = f(11, 7);
        p.crc_ok = crc_ok(payload, 26, 8, kAle3gCrc8);
        return p;
    }
    if (lead == 3) {
        p.type = Ale3gPduType::Reserved;
        return p;
    }
    p.crc_ok = crc_ok(payload, 26, 8, kAle3gCrc8);
    switch (f(2, 3)) {
    case 4:
        p.type = Ale3gPduType::TimeOffset;
        p.time_quality = f(5, 3);
        p.offset = f(8, 1) != 0 ? -f(9, 9) : f(9, 9);
        break;
    case 5:
        p.type = f(5, 1) == 0 ? Ale3gPduType::GroupTime : Ale3gPduType::Reserved;
        p.server_group = f(6, 5);
        p.dwell = f(11, 4);
        p.slot = f(15, 3);
        break;
    case 6:
        p.type = Ale3gPduType::Broadcast;
        p.countdown = f(5, 3);
        p.call_type = f(8, 3);
        p.channel = f(11, 7);
        break;
    case 7:
        p.type = f(5, 2) == 3 ? Ale3gPduType::ScanningCall : Ale3gPduType::Reserved;
        p.called_address = f(7, 11);
        break;
    default:
        p.type = Ale3gPduType::Reserved;
        break;
    }
    return p;
}

std::uint64_t ale3g_call(int called_member, int call_type, int caller_member, int caller_group) {
    const std::uint64_t body = (std::uint64_t{2} << 20U) | (static_cast<std::uint64_t>(called_member & 0x3F) << 14U) |
                               (static_cast<std::uint64_t>(call_type & 7) << 11U) |
                               (static_cast<std::uint64_t>(caller_member & 0x3F) << 5U) |
                               static_cast<std::uint64_t>(caller_group & 0x1F);
    return with_crc(body, 26, 4, kAle3gCrc4);
}

std::uint64_t ale3g_handshake(int link_id, int command, int argument) {
    const std::uint64_t body = (static_cast<std::uint64_t>(link_id & 0x3F) << 10U) |
                               (static_cast<std::uint64_t>(command & 7) << 7U) |
                               static_cast<std::uint64_t>(argument & 0x7F);
    return with_crc(body, 26, 8, kAle3gCrc8);
}

std::uint64_t ale3g_scanning_call(int called_address) {
    const std::uint64_t body = (std::uint64_t{0b0111111} << 11U) | static_cast<std::uint64_t>(called_address & 0x7FF);
    return with_crc(body, 26, 8, kAle3gCrc8);
}

std::uint64_t ale3g_tm(int priority, int dest, int source, int type, int argument) {
    const std::uint64_t body = (std::uint64_t{1} << 33U) | (static_cast<std::uint64_t>(priority & 3) << 31U) |
                               (static_cast<std::uint64_t>(dest & 0x7FF) << 20U) |
                               (static_cast<std::uint64_t>(source & 0x7FF) << 9U) |
                               (static_cast<std::uint64_t>(type & 7) << 6U) |
                               static_cast<std::uint64_t>(argument & 0x3F);
    return with_crc(body, 48, 12, kAle3gCrc12);
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

Expected<Ale3gDecoder> Ale3gDecoder::create(const Ale3gConfig& config) {
    if (config.rate < 7200 || config.rate > 192000) {
        return fail("3G ALE decoder: sample rate must be 7.2 to 192 kHz");
    }
    const double sps = static_cast<double>(config.rate) / kAle3gSymbolRate;
    const auto taps = design_rrc(config.rate, kAle3gSymbolRate, config.roll_off,
                                 2 * static_cast<std::size_t>(std::lround(5.0 * sps)) + 1);
    if (!taps) {
        return std::unexpected(taps.error());
    }
    return Ale3gDecoder(config, *taps);
}

Ale3gDecoder::Ale3gDecoder(const Ale3gConfig& config, std::vector<float> taps)
    : config_(config), taps_(std::move(taps)) {
    fs_ = static_cast<double>(config.rate);
    sps_ = fs_ / kAle3gSymbolRate;
}

void Ale3gDecoder::reset() {
    raw_.clear();
    in_count_ = 0;
    y_.clear();
    base_ = 0;
    next_ = 0;
}

Ale3gDecoder::Cplx Ale3gDecoder::sample(double t) const {
    // y_[i] is the filter output centred on input sample base_ + i.
    const double x = t - static_cast<double>(base_);
    const auto i = static_cast<std::int64_t>(std::floor(x));
    const double a = x - static_cast<double>(i);
    if (i < 0 || i + 1 >= static_cast<std::int64_t>(y_.size())) {
        return {};
    }
    return (1.0 - a) * y_[static_cast<std::size_t>(i)] + a * y_[static_cast<std::size_t>(i + 1)];
}

double Ale3gDecoder::metric(Ale3gWaveform w, double t, double* freq) const {
    // Preamble correlation in coherent segments of eight symbols, added
    // without phase, normalised by the segment energies: 1 for a clean
    // preamble, 1/8 on average for noise.
    const auto pre = ale3g_preamble(w);
    constexpr int kSeg = 8;
    double num = 0.0;
    double den = 0.0;
    Cplx prev{};
    Cplx turn{};
    double e_first = 0.0;
    double e_last = 0.0;
    for (std::size_t s = 0; s + kSeg <= pre.size(); s += kSeg) {
        Cplx acc{};
        double e = 0.0;
        const auto tally = [&](double v) { (s < pre.size() / 2 ? e_first : e_last) += v; };
        for (int j = 0; j < kSeg; ++j) {
            const std::size_t k = s + static_cast<std::size_t>(j);
            const Cplx y = sample(t + static_cast<double>(k) * sps_);
            acc += y * std::polar(1.0, -std::numbers::pi / 4.0 * pre[k]);
            e += std::norm(y);
        }
        tally(e);
        num += std::norm(acc);
        den += kSeg * e;
        if (s > 0) {
            turn += acc * std::conj(prev);
        }
        prev = acc;
    }
    if (freq != nullptr) {
        *freq = std::arg(turn) * kAle3gSymbolRate / (kTwoPi * kSeg);
    }
    // A window only partly over signal (a burst edge after silence) can
    // match by chance in its few live segments; require the power to be
    // spread over both halves of the preamble.
    if (den <= 0.0 || e_first < 0.25 * e_last || e_last < 0.25 * e_first) {
        return 0.0;
    }
    return num / den;
}

void Ale3gDecoder::decode(Ale3gWaveform w, double t, double m, double freq, std::vector<Ale3gBurst>& out) {
    const std::size_t npre = ale3g_preamble_symbols(w);
    const auto pre = ale3g_preamble(w);
    // Phase reference from the whole preamble, offset removed.
    Cplx ref{};
    for (std::size_t k = 0; k < npre; ++k) {
        const double tk = t + static_cast<double>(k) * sps_;
        ref += sample(tk) * std::polar(1.0, -std::numbers::pi / 4.0 * pre[k] - kTwoPi * freq * (tk - t) / fs_);
    }
    const Cplx rot = std::polar(1.0, -std::arg(ref));
    const std::size_t nd = ale3g_data_symbols(w);
    std::vector<Cplx> d(nd);
    for (std::size_t k = 0; k < nd; ++k) {
        const double tk = t + static_cast<double>(npre + k) * sps_;
        d[k] = sample(tk) * rot * std::polar(1.0, -kTwoPi * freq * (tk - t) / fs_ - std::numbers::pi / 4.0 * kPn[k % kPn.size()]);
    }
    // Walsh correlation: four repeats of sixteen, coherent over the 64.
    const std::size_t groups = nd / 64;
    std::vector<float> fetched;
    fetched.reserve(groups * 4);
    for (std::size_t g = 0; g < groups; ++g) {
        std::array<double, 16> mw{};
        for (int n = 0; n < 16; ++n) {
            Cplx acc{};
            for (std::size_t r = 0; r < 4; ++r) {
                for (int j = 0; j < 16; ++j) {
                    const Cplx v = d[g * 64 + r * 16 + static_cast<std::size_t>(j)];
                    acc += walsh(n, j) != 0 ? -v : v;
                }
            }
            mw[static_cast<std::size_t>(n)] = std::abs(acc);
        }
        double scale = 0.0;
        for (const double v : mw) {
            scale = std::max(scale, v);
        }
        scale = scale > 0.0 ? 4.0 / scale : 1.0;
        for (int b = 3; b >= 0; --b) {
            double one = 0.0;
            double zero = 0.0;
            for (int n = 0; n < 16; ++n) {
                if (((n >> b) & 1) != 0) {
                    one = std::max(one, mw[static_cast<std::size_t>(n)]);
                } else {
                    zero = std::max(zero, mw[static_cast<std::size_t>(n)]);
                }
            }
            fetched.push_back(static_cast<float>((one - zero) * scale));
        }
    }
    const auto order = ale3g_interleave_order(w);
    std::vector<float> coded(order.size(), 0.0F);
    for (std::size_t f = 0; f < order.size() && f < fetched.size(); ++f) {
        coded[static_cast<std::size_t>(order[f])] = fetched[f];
    }
    const int nbits = ale3g_payload_bits(w);
    const auto bits = viterbi(code_of(w), coded, nbits);
    std::uint64_t payload = 0;
    for (const auto b : bits) {
        payload = (payload << 1U) | b;
    }
    Ale3gBurst burst;
    burst.position = static_cast<std::int64_t>(std::llround(t));
    burst.waveform = w;
    burst.metric = m;
    burst.offset_hz = freq;
    burst.payload = payload;
    burst.pdu = ale3g_parse(w, payload);
    out.push_back(burst);
}

bool Ale3gDecoder::step(std::vector<Ale3gBurst>& out) {
    const double look = 2.0 * std::ceil(sps_);
    const double longest = static_cast<double>(ale3g_preamble_symbols(Ale3gWaveform::Bw1)) * sps_;
    const std::int64_t have = base_ + static_cast<std::int64_t>(y_.size());
    if (static_cast<double>(next_) + look + longest + 2.0 > static_cast<double>(have)) {
        return false;
    }
    const auto t = static_cast<double>(next_);
    const double m0 = metric(Ale3gWaveform::Bw0, t, nullptr);
    const double m1 = metric(Ale3gWaveform::Bw1, t, nullptr);
    if (std::max(m0, m1) < config_.detect_threshold) {
        ++next_;
        return true;
    }
    // The peak within two symbols.
    double best = -1.0;
    double best_t = t;
    auto best_w = Ale3gWaveform::Bw0;
    for (int dt = 0; dt <= static_cast<int>(look); ++dt) {
        for (const auto w : {Ale3gWaveform::Bw0, Ale3gWaveform::Bw1}) {
            const double v = metric(w, t + dt, nullptr);
            if (v > best) {
                best = v;
                best_t = t + dt;
                best_w = w;
            }
        }
    }
    const double end = best_t + static_cast<double>(ale3g_preamble_symbols(best_w) + ale3g_data_symbols(best_w)) * sps_;
    if (end + 2.0 > static_cast<double>(have)) {
        return false;
    }
    double freq = 0.0;
    (void)metric(best_w, best_t, &freq);
    decode(best_w, best_t, best, freq, out);
    next_ = static_cast<std::int64_t>(std::ceil(end));
    return true;
}

void Ale3gDecoder::trim() {
    const std::int64_t keep = next_ - 4;
    if (keep - base_ > 65536) {
        const auto drop = static_cast<std::size_t>(keep - base_);
        y_.erase(y_.begin(), y_.begin() + static_cast<std::ptrdiff_t>(drop));
        base_ = keep;
    }
}

void Ale3gDecoder::process(dsp::ConstRealSpan audio, std::vector<Ale3gBurst>& out) {
    const std::size_t len = taps_.size();
    const auto delay = static_cast<std::int64_t>(len / 2);
    for (const float x : audio) {
        const std::int64_t n = in_count_++;
        // Mixed to baseband with the phase of the sample's absolute index.
        const double ph = std::fmod(static_cast<double>(n) * kAle3gCarrierHz / fs_, 1.0);
        raw_.push_back(static_cast<double>(x) * std::polar(1.0, -kTwoPi * ph));
        if (raw_.size() > len) {
            raw_.erase(raw_.begin());
        }
        if (raw_.size() == len) {
            // The filter output centred on input sample n - delay.
            Cplx acc{};
            for (std::size_t i = 0; i < len; ++i) {
                acc += raw_[i] * static_cast<double>(taps_[len - 1 - i]);
            }
            if (y_.empty()) {
                base_ = n - delay;
                next_ = std::max(next_, base_);
            }
            y_.push_back(acc);
        }
    }
    while (step(out)) {
        trim();
    }
}

void Ale3gDecoder::flush(std::vector<Ale3gBurst>& out) {
    const auto pad = static_cast<std::size_t>(
        std::ceil(static_cast<double>(kAle3gTlcSymbols + 576 + 2304) * sps_) + static_cast<double>(taps_.size()));
    const std::vector<float> zeros(pad, 0.0F);
    process(zeros, out);
    reset();
}

}  // namespace revenant::decode
