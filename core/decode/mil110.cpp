#include "core/decode/mil110.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

#include "core/decode/dv_phy.h"

namespace revenant::decode {

namespace {

using C = dsp::Complex32;

constexpr double kPi = std::numbers::pi;

// Two samples a symbol after the front end.
constexpr double kBbRate = 2.0 * kMil110SymbolRate;
constexpr std::size_t kHeadSymbols = kMil110SegmentHead.size() * 32;  // 288

C tribit_point(unsigned t) {
    const double a = kPi / 4.0 * static_cast<double>(t % 8U);
    return {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
}

// The preamble's tribit, before scrambling, for channel symbol c at k.
std::uint8_t sync_tribit(std::uint8_t c, std::size_t k) {
    return static_cast<std::uint8_t>(
        (kMil110SyncPatterns[c][k % 8] + kMil110SyncScrambler[k % 32]) % 8);
}

// Channel symbols of the segment carrying `count` (5.3.2.3.7.2.1).
std::array<std::uint8_t, 15> segment_symbols(std::array<std::uint8_t, 2> d, int count) {
    std::array<std::uint8_t, 15> s{};
    for (std::size_t i = 0; i < kMil110SegmentHead.size(); ++i) {
        s[i] = kMil110SegmentHead[i];
    }
    s[9] = d[0];
    s[10] = d[1];
    const auto c = mil110_count_symbols(count);
    s[11] = c[0];
    s[12] = c[1];
    s[13] = c[2];
    s[14] = 0;
    return s;
}

// The points a symbol of `bits` channel bits may take before scrambling
// (5.3.2.3.7.1.1, figure 5), and the interleaver bits each carries, first
// bit most significant: the inverse of tables VIII and IX.
struct Alphabet {
    std::size_t size = 0;
    std::array<std::uint8_t, 8> tribit{};
    std::array<std::uint8_t, 8> label{};
};

Alphabet alphabet(int bits) {
    Alphabet a;
    if (bits == 3) {
        a.size = 8;
        for (std::uint8_t in = 0; in < 8; ++in) {
            a.tribit[in] = kMil110Mgd3[in];
            a.label[in] = in;
        }
    } else if (bits == 2) {
        a.size = 4;
        for (std::uint8_t in = 0; in < 4; ++in) {
            a.tribit[in] = static_cast<std::uint8_t>(2 * kMil110Mgd2[in]);
            a.label[in] = in;
        }
    } else {
        a.size = 2;
        a.tribit[0] = 0;
        a.tribit[1] = 4;
        a.label[0] = 0;
        a.label[1] = 1;
    }
    return a;
}

}  // namespace

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

int mil110_bit_rate(Mil110Rate rate) {
    switch (rate) {
        case Mil110Rate::Bps75: return 75;
        case Mil110Rate::Bps150: return 150;
        case Mil110Rate::Bps300: return 300;
        case Mil110Rate::Bps600: return 600;
        case Mil110Rate::Bps1200: return 1200;
        case Mil110Rate::Bps2400:
        case Mil110Rate::Bps2400Voice: return 2400;
        case Mil110Rate::Bps4800: return 4800;
    }
    return 0;
}

namespace {

struct DesignatorRow {
    Mil110Rate rate;
    std::uint8_t short_d1, short_d2, long_d1, long_d2;  // 0 = reserved
};

// Table XI.
constexpr std::array<DesignatorRow, 8> kDesignators = {{
    {Mil110Rate::Bps4800, 7, 6, 0, 0},
    {Mil110Rate::Bps2400Voice, 7, 7, 0, 0},
    {Mil110Rate::Bps2400, 6, 4, 4, 4},
    {Mil110Rate::Bps1200, 6, 5, 4, 5},
    {Mil110Rate::Bps600, 6, 6, 4, 6},
    {Mil110Rate::Bps300, 6, 7, 4, 7},
    {Mil110Rate::Bps150, 7, 4, 5, 4},
    {Mil110Rate::Bps75, 7, 5, 5, 5},
}};

}  // namespace

std::optional<std::array<std::uint8_t, 2>> mil110_designators(Mil110Mode mode) {
    for (const auto& row : kDesignators) {
        if (row.rate != mode.rate) {
            continue;
        }
        if (mode.interleave == Mil110Interleave::Long) {
            if (row.long_d1 == 0) {
                return std::nullopt;
            }
            return std::array<std::uint8_t, 2>{row.long_d1, row.long_d2};
        }
        return std::array<std::uint8_t, 2>{row.short_d1, row.short_d2};
    }
    return std::nullopt;
}

std::optional<Mil110Mode> mil110_mode_from_designators(std::uint8_t d1, std::uint8_t d2,
                                                       bool short_is_zero) {
    for (const auto& row : kDesignators) {
        if (row.short_d1 == d1 && row.short_d2 == d2) {
            return Mil110Mode{row.rate,
                              short_is_zero ? Mil110Interleave::Zero : Mil110Interleave::Short};
        }
        if (row.long_d1 != 0 && row.long_d1 == d1 && row.long_d2 == d2) {
            return Mil110Mode{row.rate, Mil110Interleave::Long};
        }
    }
    return std::nullopt;
}

Expected<Mil110Layout> mil110_layout(Mil110Mode mode) {
    if (!mil110_designators(mode)) {
        return fail("MIL-STD-188-110: reserved rate and interleave combination");
    }
    Mil110Layout l;
    const bool lng = mode.interleave == Mil110Interleave::Long;
    const bool none = mode.interleave == Mil110Interleave::Zero;
    l.preamble_segments = lng ? 24 : 3;          // 5.3.2.2.1
    l.block_symbols = lng ? 11520 : 1440;        // 5.3.2.3.7.1.2
    l.rows = 40;                                 // table VI
    switch (mode.rate) {
        case Mil110Rate::Bps4800:
            l.bits_per_symbol = 3;
            l.unknown = 32;
            l.known = 16;
            l.coded = false;
            l.rows = 0;  // 5.3.2.3.4 note: no interleaver
            break;
        case Mil110Rate::Bps2400:
        case Mil110Rate::Bps2400Voice:
            l.bits_per_symbol = 3;
            l.unknown = 32;
            l.known = 16;
            l.cols = lng ? 576 : 72;
            break;
        case Mil110Rate::Bps1200:
            l.bits_per_symbol = 2;
            l.unknown = 20;
            l.known = 20;
            l.cols = lng ? 288 : 36;
            break;
        case Mil110Rate::Bps600:
        case Mil110Rate::Bps300:
        case Mil110Rate::Bps150:
            l.bits_per_symbol = 1;
            l.unknown = 20;
            l.known = 20;
            l.cols = lng ? 144 : 18;
            l.repeat = mode.rate == Mil110Rate::Bps600 ? 1 : (mode.rate == Mil110Rate::Bps300 ? 2 : 4);
            break;
        case Mil110Rate::Bps75:
            // 5.3.2.3.7.1.1: two bits per set of 32 tribits, no probes.
            l.bits_per_symbol = 2;
            l.unknown = 32;
            l.known = 0;
            l.rows = lng ? 20 : 10;
            l.cols = lng ? 36 : 9;
            l.row_step = 7;
            l.col_step = 7;
            break;
    }
    if (none) {
        l.rows = 0;
        l.cols = 0;
    }
    return l;
}

// ---------------------------------------------------------------------------
// Building blocks
// ---------------------------------------------------------------------------

std::array<std::uint8_t, kMil110DataScramblerPeriod> mil110_data_scrambler() {
    // Figure 6, cells numbered from the left: the leftmost cell's output
    // feeds the rightmost cell and is added into the cells left of the
    // three adders. Bit 11 is the leftmost cell, so the shift is to the left
    // and the adders land on bits 1, 4 and 6. The output is the three
    // rightmost cells, MSB the third from the right.
    std::array<std::uint8_t, kMil110DataScramblerPeriod> out{};
    std::uint32_t s = kMil110DataScramblerSeed;
    for (auto& v : out) {
        for (int i = 0; i < 8; ++i) {
            const std::uint32_t fb = (s >> 11) & 1U;
            s = (s << 1) & 0xFFFU;
            if (fb != 0U) {
                s ^= 0x053U;
            }
        }
        v = static_cast<std::uint8_t>(s & 7U);
    }
    return out;
}

std::array<std::uint8_t, 3> mil110_count_symbols(int count) {
    const auto c = static_cast<unsigned>(count) & 63U;
    return {static_cast<std::uint8_t>(4U + ((c >> 4) & 3U)),
            static_cast<std::uint8_t>(4U + ((c >> 2) & 3U)),
            static_cast<std::uint8_t>(4U + (c & 3U))};
}

Mil110Cell mil110_load_cell(const Mil110Layout& layout, std::size_t k) {
    return {(k % layout.rows) * layout.row_step % layout.rows, k / layout.rows};
}

Mil110Cell mil110_fetch_cell(const Mil110Layout& layout, std::size_t j) {
    const std::size_t sweep = j / layout.rows;
    const std::size_t row = j % layout.rows;
    const std::size_t back = (row * layout.col_step) % layout.cols;
    return {row, (sweep % layout.cols + layout.cols - back) % layout.cols};
}

std::vector<std::uint8_t> mil110_probe(const Mil110Layout& layout,
                                       std::array<std::uint8_t, 2> designators,
                                       std::size_t frame) {
    std::vector<std::uint8_t> p(layout.known, 0);
    if (layout.known == 0) {
        return p;
    }
    const std::size_t per_block = layout.block_symbols / (layout.unknown + layout.known);
    if ((frame + 1) % per_block == 0) {
        for (std::size_t q = 0; q < 16 && q < p.size(); ++q) {
            p[q] = kMil110SyncPatterns[designators[q / 8]][q % 8];
        }
    }
    return p;
}

Expected<std::vector<std::uint8_t>> mil110_transmit_symbols(Mil110Mode mode, ConstBitSpan data) {
    auto layout_or = mil110_layout(mode);
    if (!layout_or) {
        return std::unexpected(layout_or.error());
    }
    const Mil110Layout l = *layout_or;
    const auto d = *mil110_designators(mode);
    std::vector<std::uint8_t> out;

    // Preamble, counting down to zero.
    for (std::size_t seg = 0; seg < l.preamble_segments; ++seg) {
        const int count = static_cast<int>(l.preamble_segments - 1 - seg);
        for (const auto c : segment_symbols(d, count)) {
            for (std::size_t k = 0; k < 32; ++k) {
                out.push_back(sync_tribit(c, k));
            }
        }
    }

    // User bits, EOM, flush.
    std::vector<std::uint8_t> in(data.begin(), data.end());
    for (int i = 31; i >= 0; --i) {
        in.push_back(static_cast<std::uint8_t>((kMil110Eom >> i) & 1U));
    }
    in.insert(in.end(), kMil110FlushBits, 0);

    const std::size_t interleaved = l.rows * l.cols;
    const std::size_t frame_bits = l.unknown == 32 && l.known == 0
                                       ? 2
                                       : l.unknown * static_cast<std::size_t>(l.bits_per_symbol);
    const std::size_t unit = interleaved != 0 ? interleaved : frame_bits;

    std::vector<std::uint8_t> coded;
    if (l.coded) {
        const std::size_t per_in = 2 * static_cast<std::size_t>(l.repeat);
        while ((in.size() * per_in) % unit != 0) {
            in.push_back(0);
        }
        static constexpr std::array<std::uint32_t, 2> kGen = {kMil110G1, kMil110G2};
        auto enc = convolutional_encode(ConvolutionalCode{6, kGen}, in);
        if (!enc) {
            return std::unexpected(enc.error());
        }
        coded.reserve(enc->size() * static_cast<std::size_t>(l.repeat));
        for (std::size_t i = 0; i + 1 < enc->size(); i += 2) {
            for (int r = 0; r < l.repeat; ++r) {
                coded.push_back((*enc)[i]);
                coded.push_back((*enc)[i + 1]);
            }
        }
    } else {
        while (in.size() % unit != 0) {
            in.push_back(0);
        }
        coded = std::move(in);
    }

    // Interleave block by block.
    std::vector<std::uint8_t> fetched(coded.size());
    if (interleaved != 0) {
        std::vector<std::uint8_t> m(interleaved);
        for (std::size_t b = 0; b < coded.size(); b += interleaved) {
            for (std::size_t k = 0; k < interleaved; ++k) {
                const auto c = mil110_load_cell(l, k);
                m[c.row * l.cols + c.col] = coded[b + k];
            }
            for (std::size_t j = 0; j < interleaved; ++j) {
                const auto c = mil110_fetch_cell(l, j);
                fetched[b + j] = m[c.row * l.cols + c.col];
            }
        }
    } else {
        fetched = coded;
    }

    // Symbol formation, framing and scrambling.
    const auto scr = mil110_data_scrambler();
    const auto alpha = alphabet(l.bits_per_symbol);
    const auto bps = static_cast<std::size_t>(l.bits_per_symbol);
    std::size_t n = 0;
    auto emit = [&](std::uint8_t t) {
        out.push_back(static_cast<std::uint8_t>((t + scr[n % kMil110DataScramblerPeriod]) % 8));
        ++n;
    };
    auto symbol_at = [&](std::size_t i) {
        unsigned v = 0;
        for (std::size_t b = 0; b < bps; ++b) {
            v = (v << 1) | fetched[i * bps + b];
        }
        return v;
    };
    const std::size_t symbols = fetched.size() / bps;
    if (l.known == 0) {
        // 75 bit/s: table X, the exceptional set closing each block.
        const std::size_t per_block = mode.interleave == Mil110Interleave::Long ? 360 : 45;
        for (std::size_t s = 0; s < symbols; ++s) {
            const unsigned v = kMil110Mgd2[symbol_at(s)];
            const bool exceptional = (s + 1) % per_block == 0;
            const auto& pat = kMil110SyncPatterns[exceptional ? 4 + v : v];
            for (std::size_t k = 0; k < 32; ++k) {
                emit(pat[k % 8]);
            }
        }
    } else {
        std::size_t frame = 0;
        for (std::size_t s = 0; s < symbols; s += l.unknown, ++frame) {
            for (std::size_t i = 0; i < l.unknown; ++i) {
                emit(alpha.tribit[symbol_at(s + i)]);
            }
            for (const auto t : mil110_probe(l, d, frame)) {
                emit(t);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The receiver
// ---------------------------------------------------------------------------

namespace {

// Engineering choices for the adaptation, per symbol.
constexpr double kMuTrain = 0.04;
constexpr double kMuProbe = 0.02;
constexpr double kMuDecided = 0.004;
constexpr double kLoopP = 0.02;
constexpr double kLoopI = 2e-4;
// A frame whose probes average this squared error reads as no signal, and
// this many in a row end the message.
constexpr double kLostMse = 0.6;
constexpr int kLostFrames = 25;
constexpr int kLostSets = 75;
constexpr std::size_t kTriggerWindow = 400;

}  // namespace

Expected<Mil110Decoder> Mil110Decoder::create(const Mil110Config& config) {
    if (config.rate < 6000) {
        return fail("MIL-STD-188-110: sample rate below 6000 S/s");
    }
    Mil110Decoder d;
    d.config_ = config;
    const auto sps = static_cast<double>(config.rate) / kMil110SymbolRate;
    const auto taps = static_cast<std::size_t>(std::ceil(8.0 * sps)) | 1U;
    auto rrc = design_rrc(config.rate, kMil110SymbolRate, 0.35, taps);
    if (!rrc) {
        return std::unexpected(rrc.error());
    }
    d.rrc_ = std::move(*rrc);
    d.ratio_ = static_cast<double>(config.rate) / kBbRate;
    d.scrambler_ = mil110_data_scrambler();
    d.head_ref_.resize(kHeadSymbols);
    for (std::size_t m = 0; m < kHeadSymbols; ++m) {
        d.head_ref_[m] = std::conj(tribit_point(sync_tribit(kMil110SegmentHead[m / 32], m % 32)));
    }
    d.reset();
    return d;
}

void Mil110Decoder::reset() {
    fir_hist_.assign(rrc_.size(), C{});
    fir_pos_ = 0;
    in_count_ = 0;
    interp_ = {};
    filt_count_ = 0;
    out_count_ = 0;
    bb_.clear();
    bb_base_ = 0;
    state_ = State::Search;
    search_pos_ = 0;
    trigger_ = -1;
    message_ = {};
}

void Mil110Decoder::process(dsp::ConstRealSpan audio, std::vector<Mil110Message>& messages) {
    const double w = 2.0 * kPi * config_.carrier_hz / static_cast<double>(config_.rate);
    for (const float a : audio) {
        // The phase from the count modulo the rate, so it stays exact.
        const double ph = w * static_cast<double>(in_count_ % config_.rate);
        ++in_count_;
        fir_hist_[fir_pos_] = C{static_cast<float>(a * std::cos(ph)),
                                static_cast<float>(-a * std::sin(ph))};
        C y{};
        std::size_t idx = fir_pos_;
        for (const float t : rrc_) {
            y += t * fir_hist_[idx];
            idx = idx == 0 ? fir_hist_.size() - 1 : idx - 1;
        }
        fir_pos_ = (fir_pos_ + 1) % fir_hist_.size();

        // Cubic Lagrange interpolation to 4800 S/s over the last four
        // filtered samples, filt_count_ - 3 .. filt_count_.
        interp_ = {interp_[1], interp_[2], interp_[3], y};
        const std::int64_t newest = filt_count_++;
        for (;;) {
            const double t = static_cast<double>(out_count_) * ratio_;
            const auto base = static_cast<std::int64_t>(std::floor(t));
            if (base + 2 > newest) {
                break;
            }
            const auto mu = static_cast<float>(t - static_cast<double>(base));
            const float cm1 = -mu * (mu - 1.0F) * (mu - 2.0F) / 6.0F;
            const float c0 = (mu + 1.0F) * (mu - 1.0F) * (mu - 2.0F) / 2.0F;
            const float c1 = -(mu + 1.0F) * mu * (mu - 2.0F) / 2.0F;
            const float c2 = (mu + 1.0F) * mu * (mu - 1.0F) / 6.0F;
            const auto off = static_cast<std::size_t>(newest - (base + 2));
            if (off != 0) {
                break;  // ratio below one; not reachable above 4800 S/s
            }
            bb_.push_back(cm1 * interp_[0] + c0 * interp_[1] + c1 * interp_[2] + c2 * interp_[3]);
            ++out_count_;
        }
    }
    run(messages);
}

bool Mil110Decoder::available(std::int64_t index) const {
    return index < bb_base_ + static_cast<std::int64_t>(bb_.size());
}

Mil110Decoder::C Mil110Decoder::bb(std::int64_t index) const {
    if (index < bb_base_) {
        return {};
    }
    return bb_[static_cast<std::size_t>(index - bb_base_)];
}

Mil110Decoder::C Mil110Decoder::derot(std::int64_t index) const {
    const double ph = -omega_ * static_cast<double>(index - origin_);
    return bb(index) * C{static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph))};
}

double Mil110Decoder::detect_metric(std::int64_t pos) const {
    std::complex<double> acc{};
    double energy = 0.0;
    C prev{};
    for (std::size_t m = 0; m < kHeadSymbols; ++m) {
        const C r = bb(pos + 2 * static_cast<std::int64_t>(m));
        const C z = r * head_ref_[m];
        if (m != 0) {
            acc += std::complex<double>(z * std::conj(prev));
        }
        prev = z;
        energy += std::norm(r);
    }
    return energy > 0.0 ? std::abs(acc) / energy : 0.0;
}

bool Mil110Decoder::acquire(std::int64_t pos) {
    // Coarse frequency from the differential product, then a refinement
    // from the phase drift between 32-symbol blocks.
    std::vector<std::complex<double>> z(kHeadSymbols);
    std::complex<double> dsum{};
    for (std::size_t m = 0; m < kHeadSymbols; ++m) {
        z[m] = std::complex<double>(bb(pos + 2 * static_cast<std::int64_t>(m)) * head_ref_[m]);
        if (m != 0) {
            dsum += z[m] * std::conj(z[m - 1]);
        }
    }
    double w_sym = std::arg(dsum);
    std::array<std::complex<double>, 9> blocks{};
    for (std::size_t m = 0; m < kHeadSymbols; ++m) {
        blocks[m / 32] += z[m] * std::polar(1.0, -w_sym * static_cast<double>(m));
    }
    std::complex<double> drift{};
    for (std::size_t b = 1; b < blocks.size(); ++b) {
        drift += blocks[b] * std::conj(blocks[b - 1]);
    }
    w_sym += std::arg(drift) / 32.0;
    std::complex<double> h{};
    for (std::size_t m = 0; m < kHeadSymbols; ++m) {
        h += z[m] * std::polar(1.0, -w_sym * static_cast<double>(m));
    }
    h /= static_cast<double>(kHeadSymbols);
    if (std::abs(h) <= 0.0) {
        return false;
    }
    origin_ = pos;
    omega_ = w_sym / 2.0;

    // The rest of the segment: D1, D2, C1..C3 against the eight patterns.
    const auto hn = std::conj(h) / std::abs(h);
    std::array<std::uint8_t, 6> got{};
    for (std::size_t j = 9; j < 15; ++j) {
        double best = -1e30;
        for (std::uint8_t v = 0; v < 8; ++v) {
            std::complex<double> acc{};
            for (std::size_t k = 0; k < 32; ++k) {
                const auto i = pos + 2 * static_cast<std::int64_t>(32 * j + k);
                acc += std::complex<double>(derot(i) * std::conj(tribit_point(sync_tribit(v, k))));
            }
            const double score = (acc * hn).real();
            if (score > best) {
                best = score;
                got[j - 9] = v;
            }
        }
    }
    const auto mode = mil110_mode_from_designators(got[0], got[1], config_.short_is_zero);
    if (!mode || got[2] < 4 || got[3] < 4 || got[4] < 4 || got[5] != 0) {
        return false;
    }
    const int count = ((got[2] - 4) << 4) | ((got[3] - 4) << 2) | (got[4] - 4);
    auto layout = mil110_layout(*mode);
    if (!layout || count >= static_cast<int>(layout->preamble_segments)) {
        return false;
    }

    mode_ = *mode;
    designators_ = {got[0], got[1]};
    layout_ = *layout;
    count0_ = count;
    data_start_ = static_cast<std::int64_t>(count + 1) * static_cast<std::int64_t>(kMil110SegmentSymbols);

    // Equaliser: the measured gain on the centre tap.
    const std::size_t ff = 4 * config_.ff_symbols + 1;
    ff_.assign(ff, C{});
    ff_[2 * config_.ff_symbols] = C(1.0F / std::complex<float>(h));
    fb_.assign(mode_.rate == Mil110Rate::Bps75 ? 0 : config_.fb_taps, C{});
    past_.assign(fb_.size(), C{});
    x_.assign(ff, C{});
    theta_ = 0.0;
    nu_ = 0.0;
    sigma2_ = 0.1;
    symbol_ = 0;

    // Data path.
    block_fill_ = 0;
    const std::size_t n = layout_.rows * layout_.cols;
    block_.assign(n, 0.0F);
    deinterleave_.assign(n, 0);
    if (n != 0) {
        std::vector<std::size_t> fetch_at(n);
        for (std::size_t j = 0; j < n; ++j) {
            const auto c = mil110_fetch_cell(layout_, j);
            fetch_at[c.row * layout_.cols + c.col] = j;
        }
        for (std::size_t k = 0; k < n; ++k) {
            const auto c = mil110_load_cell(layout_, k);
            deinterleave_[k] = fetch_at[c.row * layout_.cols + c.col];
        }
    }
    derep_.clear();
    viterbi_.reset();
    if (layout_.coded) {
        StreamingViterbiConfig vc;
        vc.memory = 6;
        vc.generators = {kMil110G1, kMil110G2};
        auto v = StreamingViterbi::create(vc);
        if (!v) {
            return false;
        }
        viterbi_.emplace(std::move(*v));
    }
    shift_ = 0;
    shift_count_ = 0;
    mse_sum_ = 0.0;
    mse_n_ = 0;
    frame_err_ = 0.0;
    bad_frames_ = 0;
    set_.clear();
    set_index_ = 0;

    message_ = {};
    message_.mode = mode_;
    message_.designators = designators_;
    message_.offset_hz = omega_ * kBbRate / (2.0 * kPi);
    // bb index to input sample: the resampler's grid plus the filter delay.
    message_.position = static_cast<std::int64_t>(
        std::llround(static_cast<double>(origin_ + 2 * data_start_) * ratio_)) -
                        static_cast<std::int64_t>(rrc_.size() / 2);
    return true;
}

Mil110Decoder::C Mil110Decoder::equalise(std::int64_t n, std::vector<C>& x) const {
    const auto centre = origin_ + 2 * n;
    const auto first = centre - 2 * static_cast<std::int64_t>(config_.ff_symbols);
    C y{};
    for (std::size_t k = 0; k < ff_.size(); ++k) {
        x[k] = derot(first + static_cast<std::int64_t>(k));
        y += ff_[k] * x[k];
    }
    y *= C{static_cast<float>(std::cos(theta_)), static_cast<float>(-std::sin(theta_))};
    for (std::size_t i = 0; i < fb_.size(); ++i) {
        y -= fb_[i] * past_[i];
    }
    return y;
}

void Mil110Decoder::adapt(const std::vector<C>& x, C y, C d, double theta, double mu) {
    const C e = d - y;
    double p = 1e-6;
    for (const auto& v : x) {
        p += std::norm(v);
    }
    for (const auto& v : past_) {
        p += std::norm(v);
    }
    const auto g = static_cast<float>(mu / p);
    const C er = e * C{static_cast<float>(std::cos(theta)), static_cast<float>(std::sin(theta))};
    for (std::size_t k = 0; k < ff_.size(); ++k) {
        ff_[k] += g * er * std::conj(x[k]);
    }
    for (std::size_t i = 0; i < fb_.size(); ++i) {
        fb_[i] -= g * e * std::conj(past_[i]);
    }
}

void Mil110Decoder::loop(C y, C d) {
    const double err = std::arg(std::complex<double>(y * std::conj(d)));
    theta_ += kLoopP * err;
    nu_ += kLoopI * err;
}

void Mil110Decoder::push_feedback(C d) {
    if (past_.empty()) {
        return;
    }
    std::rotate(past_.rbegin(), past_.rbegin() + 1, past_.rend());
    past_[0] = d;
}

void Mil110Decoder::run(std::vector<Mil110Message>& messages) {
    const auto head_span = 2 * static_cast<std::int64_t>(kHeadSymbols);
    const auto reach = 2 * static_cast<std::int64_t>(config_.ff_symbols) + 2;
    for (;;) {
        if (state_ == State::Search) {
            if (trigger_ < 0) {
                if (!available(search_pos_ + head_span)) {
                    break;
                }
                if (detect_metric(search_pos_) >= config_.detect_threshold) {
                    trigger_ = search_pos_;
                } else {
                    ++search_pos_;
                }
                continue;
            }
            // The best peak in the window, then the whole segment after it.
            const auto window = static_cast<std::int64_t>(kTriggerWindow);
            if (!available(trigger_ + window + 2 * static_cast<std::int64_t>(kMil110SegmentSymbols) + reach)) {
                break;
            }
            std::int64_t best = trigger_;
            double best_m = -1.0;
            for (std::int64_t p = trigger_; p < trigger_ + window; ++p) {
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

        if (!available(origin_ + 2 * symbol_ + reach)) {
            break;
        }
        if (symbol_ < data_start_) {
            // Training on the rest of the preamble.
            const auto seg = static_cast<std::size_t>(symbol_) / kMil110SegmentSymbols;
            const auto within = static_cast<std::size_t>(symbol_) % kMil110SegmentSymbols;
            const auto chans = segment_symbols(designators_, count0_ - static_cast<int>(seg));
            const C d = tribit_point(sync_tribit(chans[within / 32], within % 32));
            const C y = equalise(symbol_, x_);
            adapt(x_, y, d, theta_, kMuTrain);
            loop(y, d);
            sigma2_ = 0.98 * sigma2_ + 0.02 * std::norm(d - y);
            push_feedback(d);
            theta_ += nu_;
            ++symbol_;
            continue;
        }
        if (mode_.rate == Mil110Rate::Bps75) {
            on_set_symbol(messages);
        } else {
            on_data_symbol(messages);
        }
    }

    // Keep what the search and the equaliser can still reach.
    const std::int64_t keep =
        (state_ == State::Search ? search_pos_ : origin_ + 2 * symbol_) - 2 * reach - 16;
    if (keep > bb_base_ + 65536) {
        const auto drop = static_cast<std::size_t>(keep - bb_base_);
        bb_.erase(bb_.begin(), bb_.begin() + static_cast<std::ptrdiff_t>(drop));
        bb_base_ = keep;
    }
}

void Mil110Decoder::on_data_symbol(std::vector<Mil110Message>& messages) {
    const auto nd = static_cast<std::size_t>(symbol_ - data_start_);
    const std::size_t frame_len = layout_.unknown + layout_.known;
    const std::size_t frame = nd / frame_len;
    const std::size_t pos = nd % frame_len;
    const unsigned scr = scrambler_[nd % kMil110DataScramblerPeriod];
    const double theta = theta_;
    const C y = equalise(symbol_, x_);

    if (pos < layout_.unknown) {
        // Descramble, then max-log soft bits over the rate's alphabet.
        const C z = y * std::conj(tribit_point(scr));
        const auto a = alphabet(layout_.bits_per_symbol);
        std::array<float, 8> dist{};
        std::size_t nearest = 0;
        for (std::size_t j = 0; j < a.size; ++j) {
            dist[j] = std::norm(z - tribit_point(a.tribit[j]));
            if (dist[j] < dist[nearest]) {
                nearest = j;
            }
        }
        const auto bits = static_cast<std::size_t>(layout_.bits_per_symbol);
        std::array<float, 3> soft{};
        const auto scale = static_cast<float>(1.0 / std::max(sigma2_, 1e-3));
        for (std::size_t b = 0; b < bits; ++b) {
            float d0 = 1e9F;
            float d1 = 1e9F;
            const unsigned mask = 1U << (bits - 1 - b);
            for (std::size_t j = 0; j < a.size; ++j) {
                if ((a.label[j] & mask) != 0U) {
                    d1 = std::min(d1, dist[j]);
                } else {
                    d0 = std::min(d0, dist[j]);
                }
            }
            soft[b] = (d1 - d0) * scale;
        }
        const C d = tribit_point(a.tribit[nearest] + scr);
        adapt(x_, y, d, theta, kMuDecided);
        loop(y, d);
        push_feedback(d);
        theta_ += nu_;
        ++symbol_;
        on_soft(std::span<const float>(soft.data(), bits), messages);
        return;
    }

    if (pos == layout_.unknown) {
        probe_ = mil110_probe(layout_, designators_, frame);
        frame_err_ = 0.0;
    }
    const C d = tribit_point(probe_[pos - layout_.unknown] + scr);
    const double e2 = std::norm(d - y);
    frame_err_ += e2;
    sigma2_ = 0.98 * sigma2_ + 0.02 * e2;
    adapt(x_, y, d, theta, kMuProbe);
    loop(y, d);
    push_feedback(d);
    theta_ += nu_;
    ++symbol_;
    if (pos + 1 == frame_len) {
        const double mse = frame_err_ / static_cast<double>(layout_.known);
        mse_sum_ += mse;
        ++mse_n_;
        bad_frames_ = mse > kLostMse ? bad_frames_ + 1 : 0;
        if (bad_frames_ >= kLostFrames) {
            finish(false, messages);
        }
    }
}

void Mil110Decoder::on_set_symbol(std::vector<Mil110Message>& messages) {
    // 75 bit/s: gather a set of 32, decide it whole, then train on it.
    SetSymbol s;
    s.theta = theta_;
    s.x.resize(ff_.size());
    s.y = equalise(symbol_, s.x);
    set_.push_back(std::move(s));
    theta_ += nu_;
    ++symbol_;
    if (set_.size() < 32) {
        return;
    }
    const auto nd0 = static_cast<std::size_t>(symbol_ - data_start_) - 32;
    const std::size_t per_block = mode_.interleave == Mil110Interleave::Long ? 360 : 45;
    const bool exceptional = (set_index_ + 1) % per_block == 0;
    std::array<std::complex<double>, 4> corr{};
    for (std::size_t k = 0; k < 32; ++k) {
        const unsigned scr = scrambler_[(nd0 + k) % kMil110DataScramblerPeriod];
        const auto z = std::complex<double>(set_[k].y * std::conj(tribit_point(scr)));
        for (unsigned v = 0; v < 4; ++v) {
            const auto t = kMil110SyncPatterns[exceptional ? 4 + v : v][k % 8];
            corr[v] += t == 0 ? z : -z;
        }
    }
    unsigned best = 0;
    for (unsigned v = 1; v < 4; ++v) {
        if (corr[v].real() > corr[best].real()) {
            best = v;
        }
    }
    // Soft bits over the four values; the interleaver dibit is the inverse
    // of table IX.
    const auto scale = static_cast<float>(1.0 / (32.0 * std::max(sigma2_, 1e-3)));
    std::array<float, 2> soft{};
    for (unsigned b = 0; b < 2; ++b) {
        double m0 = -1e30;
        double m1 = -1e30;
        for (unsigned in = 0; in < 4; ++in) {
            const double m = corr[kMil110Mgd2[in]].real();
            if (((in >> (1 - b)) & 1U) != 0U) {
                m1 = std::max(m1, m);
            } else {
                m0 = std::max(m0, m);
            }
        }
        soft[b] = static_cast<float>((m0 - m1)) * scale;
    }
    // Train on the decided set, and turn the phase loop once for it.
    double err = 0.0;
    for (std::size_t k = 0; k < 32; ++k) {
        const unsigned scr = scrambler_[(nd0 + k) % kMil110DataScramblerPeriod];
        const C d = tribit_point(kMil110SyncPatterns[exceptional ? 4 + best : best][k % 8] + scr);
        err += std::norm(d - set_[k].y);
        adapt(set_[k].x, set_[k].y, d, set_[k].theta, kMuProbe);
    }
    const double ph = std::arg(corr[best]);
    theta_ += 8.0 * kLoopP * ph;
    nu_ += 8.0 * kLoopI * ph;
    err /= 32.0;
    sigma2_ = 0.9 * sigma2_ + 0.1 * err;
    mse_sum_ += err;
    ++mse_n_;
    set_.clear();
    ++set_index_;
    bad_frames_ = err > kLostMse ? bad_frames_ + 1 : 0;
    on_soft(soft, messages);
    if (state_ == State::Data && bad_frames_ >= kLostSets) {
        finish(false, messages);
    }
}

void Mil110Decoder::on_soft(std::span<const float> soft, std::vector<Mil110Message>& messages) {
    auto decode = [&](std::span<const float> v) {
        if (!layout_.coded) {
            std::vector<std::uint8_t> bits(v.size());
            for (std::size_t i = 0; i < v.size(); ++i) {
                bits[i] = v[i] < 0.0F ? 1 : 0;
            }
            on_bits(bits, messages);
            return;
        }
        // Repetition: the pairs come round `repeat` times; add them.
        const auto group = 2 * static_cast<std::size_t>(layout_.repeat);
        std::vector<std::uint8_t> bits;
        for (const float s : v) {
            derep_.push_back(s);
            if (derep_.size() < group) {
                continue;
            }
            std::array<float, 2> pair{};
            for (std::size_t i = 0; i < group; ++i) {
                pair[i % 2] += derep_[i];
            }
            derep_.clear();
            viterbi_->push(pair, bits);
        }
        on_bits(bits, messages);
    };

    if (layout_.rows == 0) {
        decode(soft);
        return;
    }
    for (const float s : soft) {
        if (state_ != State::Data) {
            return;
        }
        block_[block_fill_++] = s;
        if (block_fill_ == block_.size()) {
            block_fill_ = 0;
            std::vector<float> loaded(block_.size());
            for (std::size_t k = 0; k < loaded.size(); ++k) {
                loaded[k] = block_[deinterleave_[k]];
            }
            decode(loaded);
        }
    }
}

void Mil110Decoder::on_bits(std::span<const std::uint8_t> bits,
                            std::vector<Mil110Message>& messages) {
    for (const auto b : bits) {
        if (state_ != State::Data) {
            return;
        }
        shift_ = (shift_ << 1) | b;
        message_.bits.push_back(b);
        if (++shift_count_ >= 32 && shift_ == kMil110Eom) {
            message_.bits.resize(message_.bits.size() - 32);
            finish(true, messages);
            return;
        }
    }
}

void Mil110Decoder::finish(bool eom, std::vector<Mil110Message>& messages) {
    message_.eom = eom;
    message_.mse = mse_n_ != 0 ? mse_sum_ / static_cast<double>(mse_n_) : 0.0;
    message_.snr_db = message_.mse > 0.0 ? -10.0 * std::log10(message_.mse) : 99.0;
    message_.bytes.assign((message_.bits.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < message_.bits.size(); ++i) {
        message_.bytes[i / 8] =
            static_cast<std::uint8_t>(message_.bytes[i / 8] | (message_.bits[i] << (i % 8)));
    }
    messages.push_back(std::move(message_));
    message_ = {};
    state_ = State::Search;
    search_pos_ = origin_ + 2 * symbol_;
    trigger_ = -1;
    set_.clear();
    set_index_ = 0;
}

void Mil110Decoder::flush(std::vector<Mil110Message>& messages) {
    if (state_ != State::Data) {
        return;
    }
    if (viterbi_) {
        std::vector<std::uint8_t> bits;
        viterbi_->flush(false, bits);
        on_bits(bits, messages);
    }
    if (state_ == State::Data) {
        finish(false, messages);
    }
}

}  // namespace revenant::decode
