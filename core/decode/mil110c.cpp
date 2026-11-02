#include "core/decode/mil110c.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <utility>

#include "core/decode/mil110.h"

namespace revenant::decode {

namespace {

using C = dsp::Complex32;
using mil110_rx::kSps;

constexpr double kPi = std::numbers::pi;
constexpr double kBbRate = kSps * kMil110SymbolRate;
constexpr std::size_t kFrameSymbols = kMil110cDataSymbols + kMil110cProbeSymbols;  // 287
constexpr std::size_t kGroupSymbols =
    kMil110cFramesPerGroup * kFrameSymbols + kMil110cReinserted;  // 20736

// Table C-I: symbol n is exp(j n pi / 4).
constexpr std::array<std::complex<float>, 8> kPsk8 = {{
    {1.000000F, 0.000000F},
    {0.707107F, 0.707107F},
    {0.000000F, 1.000000F},
    {-0.707107F, 0.707107F},
    {-1.000000F, 0.000000F},
    {-0.707107F, -0.707107F},
    {0.000000F, -1.000000F},
    {0.707107F, -0.707107F},
}};

// Table C-V.
constexpr std::array<std::complex<float>, 16> kQam16 = {{
    {0.866025F, 0.500000F},
    {0.500000F, 0.866025F},
    {1.000000F, 0.000000F},
    {0.258819F, 0.258819F},
    {-0.500000F, 0.866025F},
    {0.000000F, 1.000000F},
    {-0.866025F, 0.500000F},
    {-0.258819F, 0.258819F},
    {0.500000F, -0.866025F},
    {0.000000F, -1.000000F},
    {0.866025F, -0.500000F},
    {0.258819F, -0.258819F},
    {-0.866025F, -0.500000F},
    {-0.500000F, -0.866025F},
    {-1.000000F, 0.000000F},
    {-0.258819F, -0.258819F},
}};

// Table C-VI.
constexpr std::array<std::complex<float>, 32> kQam32 = {{
    {0.866380F, 0.499386F},   {0.984849F, 0.173415F},   {0.499386F, 0.866380F},
    {0.173415F, 0.984849F},   {0.520246F, 0.520246F},   {0.520246F, 0.173415F},
    {0.173415F, 0.520246F},   {0.173415F, 0.173415F},   {-0.866380F, 0.499386F},
    {-0.984849F, 0.173415F},  {-0.499386F, 0.866380F},  {-0.173415F, 0.984849F},
    {-0.520246F, 0.520246F},  {-0.520246F, 0.173415F},  {-0.173415F, 0.520246F},
    {-0.173415F, 0.173415F},  {0.866380F, -0.499386F},  {0.984849F, -0.173415F},
    {0.499386F, -0.866380F},  {0.173415F, -0.984849F},  {0.520246F, -0.520246F},
    {0.520246F, -0.173415F},  {0.173415F, -0.520246F},  {0.173415F, -0.173415F},
    {-0.866380F, -0.499386F}, {-0.984849F, -0.173415F}, {-0.499386F, -0.866380F},
    {-0.173415F, -0.984849F}, {-0.520246F, -0.520246F}, {-0.520246F, -0.173415F},
    {-0.173415F, -0.520246F}, {-0.173415F, -0.173415F},
}};

// Table C-VII.
constexpr std::array<std::complex<float>, 64> kQam64 = {{
    {1.000000F, 0.000000F},   {0.822878F, 0.568218F},   {0.821137F, 0.152996F},
    {0.932897F, 0.360142F},   {0.000000F, -1.000000F},  {0.822878F, -0.568218F},
    {0.821137F, -0.152996F},  {0.932897F, -0.360142F},  {0.568218F, 0.822878F},
    {0.588429F, 0.588429F},   {0.588429F, 0.117686F},   {0.588429F, 0.353057F},
    {0.568218F, -0.822878F},  {0.588429F, -0.588429F},  {0.588429F, -0.117686F},
    {0.588429F, -0.353057F},  {0.152996F, 0.821137F},   {0.117686F, 0.588429F},
    {0.117686F, 0.117686F},   {0.117686F, 0.353057F},   {0.152996F, -0.821137F},
    {0.117686F, -0.588429F},  {0.117686F, -0.117686F},  {0.117686F, -0.353057F},
    {0.360142F, 0.932897F},   {0.353057F, 0.588429F},   {0.353057F, 0.117686F},
    {0.353057F, 0.353057F},   {0.360142F, -0.932897F},  {0.353057F, -0.588429F},
    {0.353057F, -0.117686F},  {0.353057F, -0.353057F},  {0.000000F, 1.000000F},
    {-0.822878F, 0.568218F},  {-0.821137F, 0.152996F},  {-0.932897F, 0.360142F},
    {-1.000000F, 0.000000F},  {-0.822878F, -0.568218F}, {-0.821137F, -0.152996F},
    {-0.932897F, -0.360142F}, {-0.568218F, 0.822878F},  {-0.588429F, 0.588429F},
    {-0.588429F, 0.117686F},  {-0.588429F, 0.353057F},  {-0.568218F, -0.822878F},
    {-0.588429F, -0.588429F}, {-0.588429F, -0.117686F}, {-0.588429F, -0.353057F},
    {-0.152996F, 0.821137F},  {-0.117686F, 0.588429F},  {-0.117686F, 0.117686F},
    {-0.117686F, 0.353057F},  {-0.152996F, -0.821137F}, {-0.117686F, -0.588429F},
    {-0.117686F, -0.117686F}, {-0.117686F, -0.353057F}, {-0.360142F, 0.932897F},
    {-0.353057F, 0.588429F},  {-0.353057F, 0.117686F},  {-0.353057F, 0.353057F},
    {-0.360142F, -0.932897F}, {-0.353057F, -0.588429F}, {-0.353057F, -0.117686F},
    {-0.353057F, -0.353057F},
}};

// Tables C-XIV, C-XV and C-XVI, rows 3200 to 9600, columns 1 to 72 frames.
// C-XIV is three quarters of C-XV throughout, so only C-XV is kept.
constexpr std::array<std::array<std::size_t, 6>, 5> kInterleaverBits = {{
    {512, 1536, 4608, 9216, 18432, 36864},
    {768, 2304, 6912, 13824, 27648, 55296},
    {1024, 3072, 9216, 18432, 36864, 73728},
    {1280, 3840, 11520, 23040, 46080, 92160},
    {1536, 4608, 13824, 27648, 55296, 110592},
}};
constexpr std::array<std::array<std::size_t, 6>, 5> kIncrement = {{
    {97, 229, 805, 1393, 3281, 6985},
    {145, 361, 1045, 2089, 5137, 10273},
    {189, 481, 1393, 3281, 6985, 11141},
    {201, 601, 1741, 3481, 8561, 14441},
    {229, 805, 2089, 5137, 10273, 17329},
}};

// Table C-XIII: S6 S7 S8 for the four sets, a 1 being "-".
constexpr std::array<std::uint8_t, 4> kSetCodes = {0b001, 0b010, 0b011, 0b100};

// C.5.3.2.3: of each six encoder bits, these are sent.
constexpr std::array<bool, 6> kPuncture = {true, true, true, false, false, true};

// Wrap-around Viterbi overlap at each end, an engineering choice: about
// seven constraint lengths, past the five that survivors usually need.
constexpr std::size_t kWrap = 48;

int parity(unsigned v) { return std::popcount(v) & 1; }

}  // namespace

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

int mil110c_bit_rate(Mil110cRate rate) {
    switch (rate) {
        case Mil110cRate::Bps3200: return 3200;
        case Mil110cRate::Bps4800: return 4800;
        case Mil110cRate::Bps6400: return 6400;
        case Mil110cRate::Bps8000: return 8000;
        case Mil110cRate::Bps9600: return 9600;
        case Mil110cRate::Bps12800: return 12800;
    }
    return 0;
}

std::size_t mil110c_frames(Mil110cInterleave interleave) {
    static constexpr std::array<std::size_t, 6> kFrames = {1, 3, 9, 18, 36, 72};
    return kFrames[static_cast<std::size_t>(interleave)];
}

int mil110c_bits_per_symbol(Mil110cRate rate) {
    static constexpr std::array<int, 6> kBits = {2, 3, 4, 5, 6, 6};
    return kBits[static_cast<std::size_t>(rate)];
}

std::optional<std::array<std::uint8_t, 2>> mil110c_codes(Mil110cMode mode) {
    // Table C-X, reading 1: 001 for 3200 bit/s and 1 frame upward.
    if (mode.rate == Mil110cRate::Bps12800 && mode.interleave != Mil110cInterleave::UltraShort) {
        return std::nullopt;
    }
    return std::array<std::uint8_t, 2>{static_cast<std::uint8_t>(static_cast<int>(mode.rate) + 1),
                                       static_cast<std::uint8_t>(
                                           static_cast<int>(mode.interleave) + 1)};
}

std::optional<std::array<std::uint8_t, 3>> mil110c_designators(Mil110cMode mode) {
    const auto codes = mil110c_codes(mode);
    if (!codes) {
        return std::nullopt;
    }
    // C.5.2.1.1: dibit i is rate bit i (most significant first) then
    // interleaver bit i, through table C-III.
    std::array<std::uint8_t, 3> d{};
    for (int i = 0; i < 3; ++i) {
        const unsigned r = ((*codes)[0] >> (2 - i)) & 1U;
        const unsigned l = ((*codes)[1] >> (2 - i)) & 1U;
        d[static_cast<std::size_t>(i)] = kMil110cDibit[(r << 1) | l];
    }
    return d;
}

std::optional<Mil110cMode> mil110c_mode_from_designators(std::array<std::uint8_t, 3> d) {
    for (int r = 0; r < 6; ++r) {
        for (int l = 0; l < 6; ++l) {
            const Mil110cMode m{static_cast<Mil110cRate>(r), static_cast<Mil110cInterleave>(l)};
            const auto got = mil110c_designators(m);
            if (got && *got == d) {
                return m;
            }
        }
    }
    return std::nullopt;
}

Expected<Mil110cLayout> mil110c_layout(Mil110cMode mode) {
    if (!mil110c_codes(mode)) {
        return fail("MIL-STD-188-110 Appendix C: 12800 bit/s is defined only without interleaving");
    }
    Mil110cLayout l;
    l.bits_per_symbol = mil110c_bits_per_symbol(mode.rate);
    l.frames = mil110c_frames(mode.interleave);
    if (mode.rate == Mil110cRate::Bps12800) {
        // C.4 and the table C-IX note: uncoded, no interleaving.
        l.coded = false;
        l.input_bits = kMil110cDataSymbols * 6;
        return l;
    }
    const auto r = static_cast<std::size_t>(mode.rate);
    const auto c = static_cast<std::size_t>(mode.interleave);
    l.interleaver = kInterleaverBits[r][c];
    l.increment = kIncrement[r][c];
    l.input_bits = l.interleaver * 3 / 4;
    return l;
}

// ---------------------------------------------------------------------------
// Building blocks
// ---------------------------------------------------------------------------

std::span<const std::complex<float>> mil110c_constellation(Mil110cRate rate) {
    switch (rate) {
        case Mil110cRate::Bps3200:
        case Mil110cRate::Bps4800: return kPsk8;
        case Mil110cRate::Bps6400: return kQam16;
        case Mil110cRate::Bps8000: return kQam32;
        case Mil110cRate::Bps9600:
        case Mil110cRate::Bps12800: return kQam64;
    }
    return kPsk8;
}

std::array<std::uint8_t, kMil110cDataSymbols> mil110c_scrambler(int bits) {
    // Reading 2: bit k of s is the cell labelled x^k in figure C-5, the
    // rightmost cell bit 0.
    std::array<std::uint8_t, kMil110cDataSymbols> out{};
    unsigned s = kMil110cScramblerSeed;
    const unsigned mask = (1U << bits) - 1U;
    for (auto& v : out) {
        v = static_cast<std::uint8_t>(s & mask);
        for (int i = 0; i < bits; ++i) {
            const unsigned fb = (s ^ (s >> 4)) & 1U;
            s = (s >> 1) | (fb << 8);
        }
    }
    return out;
}

std::array<std::uint8_t, 103> mil110c_reinserted(std::array<std::uint8_t, 3> d) {
    std::array<std::uint8_t, 103> t{};
    std::size_t n = 0;
    for (const auto v : kMil110cMiniProbe) {
        t[n++] = v;
    }
    t[n++] = 2;  // the "+" probe's period-16 sequence, one further
    for (std::size_t i = 0; i < 3; ++i) {
        for (const auto b : kMil110cBarker) {
            t[n++] = static_cast<std::uint8_t>((d[i] + b) % 8);
        }
    }
    t[n++] = 6;  // the "-" probe's sequence, one earlier
    for (const auto v : kMil110cMiniProbe) {
        t[n++] = static_cast<std::uint8_t>((v + 4) % 8);
    }
    return t;
}

std::array<bool, kMil110cFramesPerGroup> mil110c_probe_signs(Mil110cMode mode) {
    std::array<bool, kMil110cFramesPerGroup> s{};
    const auto codes = mil110c_codes(mode).value_or(std::array<std::uint8_t, 2>{0, 0});
    const unsigned six = (static_cast<unsigned>(codes[0]) << 3) | codes[1];
    for (std::size_t k = 0; k < s.size(); ++k) {
        const std::size_t i = k % 18;
        if (i < 7) {
            s[k] = true;
        } else if (i == 7 || i == 17) {
            s[k] = false;
        } else if (i < 14) {
            s[k] = ((six >> (13 - i)) & 1U) != 0U;
        } else {
            s[k] = ((kSetCodes[k / 18] >> (16 - i)) & 1U) != 0U;
        }
    }
    return s;
}

std::vector<std::uint8_t> mil110c_encode(ConstBitSpan input) {
    const std::size_t k = input.size();
    std::vector<std::uint8_t> out;
    if (k < 7) {
        return out;
    }
    out.reserve(k * 4 / 3 + 4);
    unsigned r = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        r = ((r << 1) | (input[i] & 1U)) & 0x7FU;
    }
    std::size_t m = 0;
    auto emit = [&](unsigned bit) {
        if (kPuncture[m % 6]) {
            out.push_back(static_cast<std::uint8_t>(bit));
        }
        ++m;
    };
    for (std::size_t i = 0; i < k; ++i) {
        r = ((r << 1) | (input[(i + 6) % k] & 1U)) & 0x7FU;
        emit(static_cast<unsigned>(parity(r & kMil110G1)));
        emit(static_cast<unsigned>(parity(r & kMil110G2)));
    }
    return out;
}

std::vector<std::uint8_t> mil110c_decode(std::span<const float> soft) {
    const std::size_t k = soft.size() * 3 / 4;
    std::vector<std::uint8_t> u(k, 0);
    if (k < kWrap || soft.size() * 3 != k * 4) {
        return u;
    }
    // Depuncture: step j of the circular encoder has newest input
    // u[(j + 6) % k] and soft pair ms[2j], ms[2j + 1].
    std::vector<float> ms(2 * k, 0.0F);
    std::size_t p = 0;
    for (std::size_t m = 0; m < 2 * k; ++m) {
        if (kPuncture[m % 6]) {
            ms[m] = soft[p++];
        }
    }
    // Wrap-around Viterbi: run over the block with kWrap steps of its tail
    // before it and kWrap of its head after, every starting state equal.
    const std::size_t steps = k + 2 * kWrap;
    std::vector<std::uint64_t> dec(steps);
    std::array<float, 64> pm{};
    std::array<float, 64> next{};
    for (std::size_t t = 0; t < steps; ++t) {
        const std::size_t j = (t + k - kWrap) % k;
        const float a = ms[2 * j];
        const float b = ms[2 * j + 1];
        std::uint64_t d = 0;
        float best = -1e30F;
        for (unsigned ns = 0; ns < 64; ++ns) {
            float m0 = 0.0F;
            float m1 = 0.0F;
            for (unsigned hi = 0; hi < 2; ++hi) {
                const unsigned r = ns | (hi << 6);
                const float bm = (parity(r & kMil110G1) != 0 ? -a : a) +
                                 (parity(r & kMil110G2) != 0 ? -b : b);
                const unsigned ps = (ns >> 1) | (hi << 5);
                (hi == 0 ? m0 : m1) = pm[ps] + bm;
            }
            if (m1 > m0) {
                next[ns] = m1;
                d |= std::uint64_t{1} << ns;
            } else {
                next[ns] = m0;
            }
            best = std::max(best, next[ns]);
        }
        for (unsigned s = 0; s < 64; ++s) {
            pm[s] = next[s] - best;
        }
        dec[t] = d;
    }
    unsigned s = static_cast<unsigned>(std::max_element(pm.begin(), pm.end()) - pm.begin());
    std::vector<std::uint8_t> v(k, 0);
    for (std::size_t t = steps; t-- > 0;) {
        if (t >= kWrap && t < kWrap + k) {
            v[t - kWrap] = static_cast<std::uint8_t>(s & 1U);
        }
        const unsigned hi = static_cast<unsigned>((dec[t] >> s) & 1U);
        s = (s >> 1) | (hi << 5);
    }
    for (std::size_t i = 0; i < k; ++i) {
        u[(i + 6) % k] = v[i];
    }
    return u;
}

Expected<std::vector<std::complex<float>>> mil110c_transmit_symbols(Mil110cMode mode,
                                                                    ConstBitSpan data,
                                                                    int agc_blocks) {
    auto layout_or = mil110c_layout(mode);
    if (!layout_or) {
        return std::unexpected(layout_or.error());
    }
    if (agc_blocks < 0 || agc_blocks > 7) {
        return fail("MIL-STD-188-110 Appendix C: the AGC block count runs from 0 to 7");
    }
    const Mil110cLayout l = *layout_or;
    const auto d = *mil110c_designators(mode);
    const auto tail = mil110c_reinserted(d);
    const auto signs = mil110c_probe_signs(mode);
    const auto points = mil110c_constellation(mode.rate);
    const auto bps = static_cast<std::size_t>(l.bits_per_symbol);
    const auto scr = mil110c_scrambler(bps <= 3 ? 3 : l.bits_per_symbol);
    std::vector<std::complex<float>> out;

    // C.5.2.1.1: AGC blocks, the conjugate of the first 184 symbols.
    for (int b = 0; b < agc_blocks; ++b) {
        for (const auto v : kMil110cSyncPreamble) {
            out.push_back(std::conj(kPsk8[v]));
        }
    }
    for (const auto v : kMil110cSyncPreamble) {
        out.push_back(kPsk8[v]);
    }
    for (const auto v : tail) {
        out.push_back(kPsk8[v]);
    }

    // C.5.4.3: data, EOM, zeros to the end of the input block.
    std::vector<std::uint8_t> in(data.begin(), data.end());
    for (int i = 31; i >= 0; --i) {
        in.push_back(static_cast<std::uint8_t>((kMil110Eom >> i) & 1U));
    }
    while (in.size() % l.input_bits != 0) {
        in.push_back(0);
    }

    std::size_t frame = 0;  // from the start of the data
    const std::size_t blocks = in.size() / l.input_bits;
    std::vector<std::uint8_t> fetched;
    for (std::size_t blk = 0; blk < blocks; ++blk) {
        const ConstBitSpan block(in.data() + blk * l.input_bits, l.input_bits);
        if (l.coded) {
            const auto code = mil110c_encode(block);
            fetched.assign(l.interleaver, 0);
            for (std::size_t n = 0; n < code.size(); ++n) {
                fetched[(n * l.increment) % l.interleaver] = code[n];
            }
        } else {
            fetched.assign(block.begin(), block.end());
        }
        const std::size_t frames = fetched.size() / (kMil110cDataSymbols * bps);
        for (std::size_t f = 0; f < frames; ++f, ++frame) {
            for (std::size_t s = 0; s < kMil110cDataSymbols; ++s) {
                unsigned v = 0;
                for (std::size_t b = 0; b < bps; ++b) {
                    v = (v << 1) | fetched[(f * kMil110cDataSymbols + s) * bps + b];
                }
                if (bps == 2) {
                    out.push_back(kPsk8[(kMil110cDibit[v] + scr[s]) % 8]);
                } else if (bps == 3) {
                    out.push_back(kPsk8[(kMil110cTribit[v] + scr[s]) % 8]);
                } else {
                    out.push_back(points[v ^ scr[s]]);
                }
            }
            const std::size_t in_group = frame % kMil110cFramesPerGroup;
            const unsigned add = signs[in_group] ? 4U : 0U;
            for (const auto v : kMil110cMiniProbe) {
                out.push_back(kPsk8[(v + add) % 8]);
            }
            const bool last = blk + 1 == blocks && f + 1 == frames;
            if (in_group + 1 == kMil110cFramesPerGroup && !last) {
                for (std::size_t i = kMil110cProbeSymbols; i < tail.size(); ++i) {
                    out.push_back(kPsk8[tail[i]]);
                }
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The receiver
// ---------------------------------------------------------------------------

namespace {

constexpr double kMuProbe = 0.02;
constexpr double kMuDecided = 0.004;
constexpr double kLostMse = 0.6;
constexpr int kLostFrames = 6;
constexpr double kFitMse = 0.65;
constexpr std::int64_t kTriggerWindow = 800;

}  // namespace

Expected<Mil110cDecoder> Mil110cDecoder::create(const Mil110cConfig& config) {
    if (config.rate < 6000) {
        return fail("MIL-STD-188-110 Appendix C: sample rate below 6000 S/s");
    }
    Mil110cDecoder d;
    d.config_ = config;
    if (auto st = d.fe_.init(config.rate, config.carrier_hz, kMil110SymbolRate); !st) {
        return std::unexpected(st.error());
    }
    d.sync_ref_.resize(kMil110cSyncSymbols);
    for (std::size_t m = 0; m < kMil110cSyncSymbols; ++m) {
        d.sync_ref_[m] = std::conj(kPsk8[kMil110cSyncPreamble[m]]);
    }
    d.reset();
    return d;
}

void Mil110cDecoder::reset() {
    fe_.reset();
    state_ = State::Search;
    search_pos_ = 0;
    trigger_ = -1;
    message_ = {};
}

void Mil110cDecoder::process(dsp::ConstRealSpan audio, std::vector<Mil110cMessage>& messages) {
    fe_.push(audio);
    run(messages);
}

double Mil110cDecoder::detect_metric(std::int64_t pos) const {
    std::complex<double> acc{};
    double energy = 0.0;
    C prev{};
    for (std::size_t m = 0; m < kMil110cSyncSymbols; ++m) {
        const C r = sample(pos, m);
        const C z = r * sync_ref_[m];
        if (m != 0) {
            acc += std::complex<double>(z * std::conj(prev));
        }
        prev = z;
        energy += std::norm(r);
    }
    return energy > 0.0 ? std::abs(acc) / energy : 0.0;
}

bool Mil110cDecoder::acquire(std::int64_t pos) {
    // Carrier offset from the differential product, refined by the phase
    // drift between eight blocks of 23 symbols.
    std::vector<std::complex<double>> z(kMil110cSyncSymbols);
    std::complex<double> dsum{};
    for (std::size_t m = 0; m < z.size(); ++m) {
        z[m] = std::complex<double>(sample(pos, m) * sync_ref_[m]);
        if (m != 0) {
            dsum += z[m] * std::conj(z[m - 1]);
        }
    }
    double w_sym = std::arg(dsum);
    std::array<std::complex<double>, 8> blocks{};
    for (std::size_t m = 0; m < z.size(); ++m) {
        blocks[m / 23] += z[m] * std::polar(1.0, -w_sym * static_cast<double>(m));
    }
    std::complex<double> drift{};
    for (std::size_t b = 1; b < blocks.size(); ++b) {
        drift += blocks[b] * std::conj(blocks[b - 1]);
    }
    w_sym += std::arg(drift) / 23.0;
    std::complex<double> h{};
    for (std::size_t m = 0; m < z.size(); ++m) {
        h += z[m] * std::polar(1.0, -w_sym * static_cast<double>(m));
    }
    h /= static_cast<double>(z.size());
    if (std::abs(h) <= 0.0) {
        return false;
    }

    // D0, D1, D2: the phase of each Barker group against the preamble's.
    std::array<std::uint8_t, 3> d{};
    for (std::size_t i = 0; i < 3; ++i) {
        std::complex<double> acc{};
        for (std::size_t k = 0; k < kMil110cBarker.size(); ++k) {
            const std::size_t m = kMil110cSyncSymbols + 32 + 13 * i + k;
            acc += std::complex<double>(sample(pos, m) * std::conj(kPsk8[kMil110cBarker[k]])) *
                   std::polar(1.0, -w_sym * static_cast<double>(m));
        }
        const double a = std::arg(acc * std::conj(h));
        const auto q = static_cast<int>(std::lround(a / (kPi / 2.0)));
        d[i] = static_cast<std::uint8_t>((((q % 4) + 4) % 4) * 2);
    }
    const auto mode = mil110c_mode_from_designators(d);
    if (!mode) {
        return false;
    }
    auto layout = mil110c_layout(*mode);
    if (!layout) {
        return false;
    }

    origin_ = pos;
    omega_ = w_sym / kSps;
    mode_ = *mode;
    designators_ = d;
    layout_ = *layout;
    tail_ = mil110c_reinserted(d);
    signs_ = mil110c_probe_signs(mode_);
    scrambler_ = mil110c_scrambler(layout_.bits_per_symbol <= 3 ? 3 : layout_.bits_per_symbol);
    const auto pts = mil110c_constellation(mode_.rate);
    points_.assign(pts.begin(), pts.end());

    // The equaliser from a least-squares solve over the whole preamble.
    std::vector<C> known;
    known.reserve(kMil110cPreambleSymbols);
    for (const auto v : kMil110cSyncPreamble) {
        known.push_back(kPsk8[v]);
    }
    for (const auto v : tail_) {
        known.push_back(kPsk8[v]);
    }
    eq_.start(config_.ff_symbols, config_.fb_taps, C(1.0F / std::complex<float>(h)));
    // A false peak fits with an error near one; a real preamble at 3 dB
    // fits near 0.5. An engineering choice between the two.
    const auto fit = eq_.solve(fe_, origin_, omega_, 0, known);
    if (!fit || *fit > kFitMse) {
        return false;
    }
    eq_.mark_centre();
    eq_.enable_timing(true);
    symbol_ = static_cast<std::int64_t>(kMil110cPreambleSymbols);

    const std::size_t block = layout_.coded ? layout_.interleaver : layout_.input_bits;
    block_.assign(block, 0.0F);
    block_fill_ = 0;
    shift_ = 0;
    shift_count_ = 0;
    mse_sum_ = 0.0;
    mse_n_ = 0;
    frame_err_ = 0.0;
    frame_known_ = 0;
    bad_frames_ = 0;

    message_ = {};
    message_.mode = mode_;
    message_.designators = designators_;
    message_.offset_hz = omega_ * kBbRate / (2.0 * kPi);
    message_.position = fe_.to_input(static_cast<double>(origin_) +
                                     kSps * static_cast<double>(kMil110cPreambleSymbols));
    return true;
}

void Mil110cDecoder::run(std::vector<Mil110cMessage>& messages) {
    const auto sync_span = kSps * static_cast<std::int64_t>(kMil110cSyncSymbols);
    const auto reach = 4 * static_cast<std::int64_t>(config_.ff_symbols) + 8;
    for (;;) {
        if (state_ == State::Search) {
            if (trigger_ < 0) {
                if (!fe_.available(search_pos_ + sync_span)) {
                    break;
                }
                if (detect_metric(search_pos_) >= config_.detect_threshold) {
                    trigger_ = search_pos_;
                } else {
                    ++search_pos_;
                }
                continue;
            }
            if (!fe_.available(trigger_ + kTriggerWindow +
                               kSps * static_cast<std::int64_t>(kMil110cPreambleSymbols) +
                               reach)) {
                break;
            }
            std::int64_t best = trigger_;
            double best_m = -1.0;
            for (std::int64_t p = trigger_; p < trigger_ + kTriggerWindow; ++p) {
                const double m = detect_metric(p);
                if (m > best_m) {
                    best_m = m;
                    best = p;
                }
            }
            trigger_ = -1;
            if (acquire(best)) {
                state_ = State::Data;
            } else {
                search_pos_ = best + 1;
            }
            continue;
        }
        if (!fe_.available(eq_.reach(origin_, symbol_))) {
            break;
        }
        on_symbol(messages);
    }
    const std::int64_t keep =
        (state_ == State::Search ? search_pos_ : eq_.earliest(origin_, symbol_)) - 64;
    fe_.discard_before(keep);
}

void Mil110cDecoder::on_symbol(std::vector<Mil110cMessage>& messages) {
    const auto nd = static_cast<std::size_t>(symbol_) - kMil110cPreambleSymbols;
    const std::size_t q = nd % kGroupSymbols;
    const C y = eq_.output(fe_, origin_, omega_, symbol_, snap_);

    std::optional<std::uint8_t> known;
    bool frame_end = false;
    if (q >= kMil110cFramesPerGroup * kFrameSymbols) {
        known = tail_[kMil110cProbeSymbols + q - kMil110cFramesPerGroup * kFrameSymbols];
    } else {
        const std::size_t fg = q / kFrameSymbols;
        const std::size_t pos = q % kFrameSymbols;
        if (pos >= kMil110cDataSymbols) {
            const unsigned add = signs_[fg] ? 4U : 0U;
            known = static_cast<std::uint8_t>(
                (kMil110cMiniProbe[pos - kMil110cDataSymbols] + add) % 8);
            frame_end = pos + 1 == kFrameSymbols;
        } else {
            // A data symbol: max-log soft bits over the rate's alphabet, the
            // first bit fetched most significant.
            const auto bps = static_cast<std::size_t>(layout_.bits_per_symbol);
            const std::size_t count = std::size_t{1} << bps;
            const unsigned scr = scrambler_[pos];
            std::array<float, 64> dist{};
            std::array<std::size_t, 64> index{};
            std::size_t nearest = 0;
            for (std::size_t v = 0; v < count; ++v) {
                if (bps == 2) {
                    index[v] = (kMil110cDibit[v] + scr) % 8;
                } else if (bps == 3) {
                    index[v] = (kMil110cTribit[v] + scr) % 8;
                } else {
                    index[v] = v ^ scr;
                }
                dist[v] = std::norm(y - points_[index[v]]);
                if (dist[v] < dist[nearest]) {
                    nearest = v;
                }
            }
            const auto scale = static_cast<float>(1.0 / std::max(eq_.sigma2(), 1e-4));
            const C d = points_[index[nearest]];
            eq_.adapt(snap_, y, d, kMuDecided, true);
            eq_.loop(y, d);
            eq_.push_feedback(d);
            eq_.advance();
            ++symbol_;
            for (std::size_t b = 0; b < bps; ++b) {
                float d0 = 1e9F;
                float d1 = 1e9F;
                const std::size_t mask = std::size_t{1} << (bps - 1 - b);
                for (std::size_t v = 0; v < count; ++v) {
                    if ((v & mask) != 0U) {
                        d1 = std::min(d1, dist[v]);
                    } else {
                        d0 = std::min(d0, dist[v]);
                    }
                }
                block_[block_fill_++] = (d1 - d0) * scale;
            }
            if (block_fill_ == block_.size()) {
                block_fill_ = 0;
                on_block(messages);
            }
            return;
        }
    }

    const C d = kPsk8[*known];
    const double e2 = std::norm(d - y);
    frame_err_ += e2;
    ++frame_known_;
    eq_.track_error(e2, 0.02);
    eq_.adapt(snap_, y, d, kMuProbe, true);
    eq_.loop(y, d);
    eq_.push_feedback(d);
    eq_.advance();
    ++symbol_;
    if (frame_end) {
        eq_.recentre();
        const double mse = frame_err_ / static_cast<double>(frame_known_);
        frame_err_ = 0.0;
        frame_known_ = 0;
        mse_sum_ += mse;
        ++mse_n_;
        bad_frames_ = mse > kLostMse ? bad_frames_ + 1 : 0;
        if (bad_frames_ >= kLostFrames) {
            finish(false, messages);
        }
    }
}

void Mil110cDecoder::on_block(std::vector<Mil110cMessage>& messages) {
    std::vector<std::uint8_t> bits;
    if (layout_.coded) {
        std::vector<float> code(block_.size());
        for (std::size_t n = 0; n < code.size(); ++n) {
            code[n] = block_[(n * layout_.increment) % block_.size()];
        }
        bits = mil110c_decode(code);
    } else {
        bits.resize(block_.size());
        for (std::size_t i = 0; i < bits.size(); ++i) {
            bits[i] = block_[i] < 0.0F ? 1 : 0;
        }
    }
    for (const auto b : bits) {
        shift_ = (shift_ << 1) | b;
        message_.bits.push_back(b);
        if (++shift_count_ >= 32 && shift_ == kMil110Eom) {
            message_.bits.resize(message_.bits.size() - 32);
            finish(true, messages);
            return;
        }
    }
}

void Mil110cDecoder::finish(bool eom, std::vector<Mil110cMessage>& messages) {
    message_.eom = eom;
    message_.mse = mse_n_ != 0 ? mse_sum_ / static_cast<double>(mse_n_) : 0.0;
    message_.snr_db = message_.mse > 0.0 ? -10.0 * std::log10(message_.mse) : 99.0;
    message_.clock_ppm = eq_.clock_ppm();
    message_.bytes.assign((message_.bits.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < message_.bits.size(); ++i) {
        message_.bytes[i / 8] =
            static_cast<std::uint8_t>(message_.bytes[i / 8] | (message_.bits[i] << (i % 8)));
    }
    messages.push_back(std::move(message_));
    message_ = {};
    state_ = State::Search;
    search_pos_ = static_cast<std::int64_t>(eq_.centre(origin_, symbol_));
    trigger_ = -1;
}

void Mil110cDecoder::flush(std::vector<Mil110cMessage>& messages) {
    if (state_ == State::Data) {
        finish(false, messages);
    }
}

}  // namespace revenant::decode
