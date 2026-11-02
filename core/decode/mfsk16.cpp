#include "core/decode/mfsk16.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace revenant::decode {

// Varicode, "The Table": Table[n] for n = 0 to 255, in character order rather
// than the page's frequency order.
const std::array<std::string_view, 256> kMfskVaricode = {
    "11101011100", "11101100000", "11101101000", "11101101100",  // 0..3
    "11101110000", "11101110100", "11101111000", "11101111100",  // 4..7
    "10101000", "11110000000", "11110100000", "11110101000",  // 8..11
    "11110101100", "10101100", "11110110000", "11110110100",  // 12..15
    "11110111000", "11110111100", "11111000000", "11111010000",  // 16..19
    "11111010100", "11111011000", "11111011100", "11111100000",  // 20..23
    "11111101000", "11111101100", "11111110000", "11111110100",  // 24..27
    "11111111000", "11111111100", "100000000000", "101000000000",  // 28..31
    "100", "111000000", "111111100", "1011011000",  // 32..35
    "1010101000", "1010100000", "1000000000", "110111100",  // 36..39
    "111110100", "111110000", "1010110100", "111100000",  // 40..43
    "10100000", "111011000", "111010100", "111101000",  // 44..47
    "11100000", "11110000", "101000000", "101010100",  // 48..51
    "101110100", "101100000", "101101100", "110100000",  // 52..55
    "110000000", "110101100", "111101100", "111111000",  // 56..59
    "1011000000", "111011100", "1010111100", "111010000",  // 60..63
    "1010000000", "10111100", "100000000", "11010100",  // 64..67
    "11011100", "10111000", "11111000", "101010000",  // 68..71
    "101011000", "11000000", "110110100", "101111100",  // 72..75
    "11110100", "11101000", "11111100", "11010000",  // 76..79
    "11101100", "110110000", "11011000", "10110100",  // 80..83
    "10110000", "101011100", "110101000", "101101000",  // 84..87
    "101110000", "101111000", "110111000", "1011101000",  // 88..91
    "1011010000", "1011101100", "1011010100", "1010110000",  // 92..95
    "1010101100", "10100", "1100000", "111000",  // 96..99
    "110100", "1000", "1010000", "1011000",  // 100..103
    "110000", "11000", "10000000", "1110000",  // 104..107
    "101100", "1000000", "11100", "10000",  // 108..111
    "1010100", "1111000", "100000", "101000",  // 112..115
    "1100", "111100", "1101100", "1101000",  // 116..119
    "1110100", "1011100", "1111100", "1011011100",  // 120..123
    "1010111000", "1011100000", "1011110000", "101010000000",  // 124..127
    "101010100000", "101010101000", "101010101100", "101010110000",  // 128..131
    "101010110100", "101010111000", "101010111100", "101011000000",  // 132..135
    "101011010000", "101011010100", "101011011000", "101011011100",  // 136..139
    "101011100000", "101011101000", "101011101100", "101011110000",  // 140..143
    "101011110100", "101011111000", "101011111100", "101100000000",  // 144..147
    "101101000000", "101101010000", "101101010100", "101101011000",  // 148..151
    "101101011100", "101101100000", "101101101000", "101101101100",  // 152..155
    "101101110000", "101101110100", "101101111000", "101101111100",  // 156..159
    "1011110100", "1011111000", "1011111100", "1100000000",  // 160..163
    "1101000000", "1101010000", "1101010100", "1101011000",  // 164..167
    "1101011100", "1101100000", "1101101000", "1101101100",  // 168..171
    "1101110000", "1101110100", "1101111000", "1101111100",  // 172..175
    "1110000000", "1110100000", "1110101000", "1110101100",  // 176..179
    "1110110000", "1110110100", "1110111000", "1110111100",  // 180..183
    "1111000000", "1111010000", "1111010100", "1111011000",  // 184..187
    "1111011100", "1111100000", "1111101000", "1111101100",  // 188..191
    "1111110000", "1111110100", "1111111000", "1111111100",  // 192..195
    "10000000000", "10100000000", "10101000000", "10101010000",  // 196..199
    "10101010100", "10101011000", "10101011100", "10101100000",  // 200..203
    "10101101000", "10101101100", "10101110000", "10101110100",  // 204..207
    "10101111000", "10101111100", "10110000000", "10110100000",  // 208..211
    "10110101000", "10110101100", "10110110000", "10110110100",  // 212..215
    "10110111000", "10110111100", "10111000000", "10111010000",  // 216..219
    "10111010100", "10111011000", "10111011100", "10111100000",  // 220..223
    "10111101000", "10111101100", "10111110000", "10111110100",  // 224..227
    "10111111000", "10111111100", "11000000000", "11010000000",  // 228..231
    "11010100000", "11010101000", "11010101100", "11010110000",  // 232..235
    "11010110100", "11010111000", "11010111100", "11011000000",  // 236..239
    "11011010000", "11011010100", "11011011000", "11011011100",  // 240..243
    "11011100000", "11011101000", "11011101100", "11011110000",  // 244..247
    "11011110100", "11011111000", "11011111100", "11100000000",  // 248..251
    "11101000000", "11101010000", "11101010100", "11101011000",  // 252..255
};

namespace {

// Indexed by (1 << length) | code, so codes of different lengths that share
// a value stay apart. -1 for no character.
const std::vector<std::int16_t>& varicode_lookup() {
    static const std::vector<std::int16_t> table = [] {
        std::vector<std::int16_t> t(std::size_t{1} << (kMfskVaricodeMaxBits + 1), -1);
        for (std::size_t c = 0; c < kMfskVaricode.size(); ++c) {
            std::uint32_t code = 0;
            for (const char b : kMfskVaricode[c]) {
                code = (code << 1U) | (b == '1' ? 1U : 0U);
            }
            const std::size_t length = kMfskVaricode[c].size();
            t[(std::size_t{1} << length) | code] = static_cast<std::int16_t>(c);
        }
        return t;
    }();
    return table;
}

// Pairs held back before printing, so the pairing check has seen enough of
// the new data to have chosen. An engineering choice: 64 data bits is about
// two seconds of MFSK16.
constexpr std::uint64_t kHoldBits = 64;

// Spec E.8 allows a squelch on decoder metrics. This one: a character prints
// only if, over the 24 data bits before it was decided, which hold its own,
// re-encoding disagrees with fewer than this share of the received coded
// bits. Noise decodes to about one half; the signal at the weakest level the
// tests decode stays near a tenth. An engineering choice.
constexpr std::uint64_t kSquelchBefore = 24;
constexpr double kSquelchShare = 0.3;

// Exponential average of the re-encoding disagreement, per decided bit, and
// how far the other pairing must be ahead before the receiver changes to it.
// Engineering choices.
constexpr double kDisagreementAverage = 0.02;
constexpr double kSwitchMargin = 0.08;

}  // namespace

Mfsk16Geometry mfsk16_geometry(Mfsk16Mode mode) {
    if (mode == Mfsk16Mode::Mfsk8) {
        return {32, 5, 7.8125};
    }
    return {16, 4, 15.625};
}

int mfsk_gray(int tone) { return tone ^ (tone >> 1); }

int mfsk_gray_inverse(int weight) {
    int tone = 0;
    for (; weight != 0; weight >>= 1) {
        tone ^= weight;
    }
    return tone;
}

void mfsk_varicode_append(std::string_view latin1, std::vector<std::uint8_t>& bits) {
    for (const char ch : latin1) {
        for (const char b : kMfskVaricode[static_cast<unsigned char>(ch)]) {
            bits.push_back(b == '1' ? 1 : 0);
        }
    }
}

int MfskVaricodeDecoder::push(std::uint8_t bit) {
    int result = -1;
    if (bit != 0) {
        if (zeros_ >= 2 && length_ > 0 && !overflow_) {
            result = varicode_lookup()[(std::size_t{1} << length_) | code_];
        }
        if (zeros_ >= 2) {
            code_ = 0;
            length_ = 0;
            overflow_ = false;
        }
        zeros_ = 0;
    } else {
        ++zeros_;
    }
    if (length_ < kMfskVaricodeMaxBits) {
        code_ = (code_ << 1U) | (bit != 0 ? 1U : 0U);
        ++length_;
    } else {
        overflow_ = true;
    }
    return result;
}

void MfskVaricodeDecoder::reset() { *this = MfskVaricodeDecoder{}; }

template <typename T>
MfskDiagonalDelay<T>::MfskDiagonalDelay(int bits, bool receive) : bits_(bits), lines_(bits) {
    for (int i = 0; i < bits; ++i) {
        const int delay = kMfskInterleaverStages * (receive ? bits - 1 - i : i);
        lines_[i].assign(static_cast<std::size_t>(delay), T{});
    }
}

template <typename T>
void MfskDiagonalDelay<T>::push(std::span<const T> in, std::span<T> out) {
    for (int i = 0; i < bits_; ++i) {
        auto& line = lines_[i];
        line.push_back(in[i]);
        out[i] = line.front();
        line.pop_front();
    }
}

template class MfskDiagonalDelay<float>;
template class MfskDiagonalDelay<std::uint8_t>;

Expected<Mfsk16Decoder> Mfsk16Decoder::create(const Mfsk16Config& config) {
    const Mfsk16Geometry g = mfsk16_geometry(config.mode);
    MfskDemodConfig dc;
    dc.rate = config.rate;
    dc.tones = g.tones;
    dc.tone_spacing_hz = g.symbol_rate;
    dc.symbol_rate = g.symbol_rate;
    dc.base_hz = config.base_hz;
    dc.search = config.search;
    dc.search_low_hz = config.search_low_hz;
    dc.search_high_hz = config.search_high_hz;
    // An engineering choice. Spec C.12's eight symbols of the lowest tone
    // cannot place the tone set on their own, since one tone fits anywhere in
    // it; the search needs data symbols reaching across the set, and a
    // shorter window placed MFSK16 a whole number of tones low when the
    // window held mostly silence and preamble.
    dc.acquisition_symbols = config.mode == Mfsk16Mode::Mfsk8 ? 48 : 64;
    auto demod = MfskDemodulator::create(dc);
    if (!demod) {
        return std::unexpected(demod.error());
    }
    Mfsk16Decoder d(std::move(*demod));
    d.config_ = config;
    d.geometry_ = g;
    for (int i = 0; i < 2; ++i) {
        StreamingViterbiConfig vc;
        vc.memory = kMfskFecMemory;
        vc.generators = {kMfskFecGenerators[0], kMfskFecGenerators[1]};
        vc.traceback = config.traceback;
        auto v = StreamingViterbi::create(vc);
        if (!v) {
            return std::unexpected(v.error());
        }
        d.lanes_.push_back(Lane{std::move(*v)});
    }
    d.reset();
    return d;
}

void Mfsk16Decoder::reset() {
    demod_.reset();
    deinterleave_ = MfskDiagonalDelay<float>(geometry_.bits_per_symbol, true);
    for (std::size_t i = 0; i < lanes_.size(); ++i) {
        Lane& lane = lanes_[i];
        lane.viterbi.reset();
        lane.varicode.reset();
        lane.pending.clear();
        lane.pending_sample.clear();
        lane.held.clear();
        lane.have_half = false;
        lane.skip_one = (i == 1);
        lane.encoder = 0;
        lane.disagreement = 0.0;
        lane.decided = 0;
        lane.wrong.fill(0.5F);
    }
    chosen_ = 0;
    last_printed_ = 0;
    printed_any_ = false;
}

void Mfsk16Decoder::process(ConstRealSpan audio, std::vector<Mfsk16Character>& out) {
    symbols_.clear();
    demod_.process(audio, symbols_);
    for (const MfskSymbol& s : symbols_) {
        on_symbol(s, out);
    }
}

void Mfsk16Decoder::on_symbol(const MfskSymbol& symbol, std::vector<Mfsk16Character>& out) {
    const int n = geometry_.bits_per_symbol;
    const int m = geometry_.tones;
    double total = 0.0;
    for (const float e : symbol.energies) {
        total += e;
    }
    soft_.assign(static_cast<std::size_t>(n), 0.0F);
    if (total > 0.0) {
        for (int j = 0; j < n; ++j) {
            float z0 = 0.0F;
            float z1 = 0.0F;
            for (int k = 0; k < m; ++k) {
                const int tone = config_.lower_sideband ? m - 1 - k : k;
                const float e = symbol.energies[static_cast<std::size_t>(k)];
                if (((mfsk_gray(tone) >> (n - 1 - j)) & 1) != 0) {
                    z1 = std::max(z1, e);
                } else {
                    z0 = std::max(z0, e);
                }
            }
            soft_[static_cast<std::size_t>(j)] = static_cast<float>((z0 - z1) / total);
        }
    }
    aligned_.assign(static_cast<std::size_t>(n), 0.0F);
    deinterleave_.push(soft_, aligned_);

    const auto position = static_cast<SampleIndex>(symbol.start);
    for (std::size_t li = 0; li < lanes_.size(); ++li) {
        Lane& lane = lanes_[li];
        for (const float v : aligned_) {
            if (lane.skip_one) {
                lane.skip_one = false;
                continue;
            }
            if (!lane.have_half) {
                lane.half = v;
                lane.have_half = true;
                continue;
            }
            lane.have_half = false;
            const float pair[2] = {lane.half, v};
            lane.pending.emplace_back(lane.half, v);
            lane.pending_sample.push_back(position);
            bits_.clear();
            lane.viterbi.push(pair, bits_);
            on_bits(lane, bits_);
        }
    }
    choose_and_print(out, false);
}

void Mfsk16Decoder::on_bits(Lane& lane, const std::vector<std::uint8_t>& bits) {
    for (const std::uint8_t bit : bits) {
        if (lane.pending.empty()) {
            break;
        }
        const auto [s0, s1] = lane.pending.front();
        const SampleIndex position = lane.pending_sample.front();
        lane.pending.pop_front();
        lane.pending_sample.pop_front();

        // Re-encode and compare with what was received: the wrong pairing
        // decodes to a sequence whose re-encoding disagrees with about half
        // the received bits, the right one with only the channel's errors.
        lane.encoder = ((lane.encoder << 1U) | bit) & 0x7FU;
        double wrong = 0.0;
        int counted = 0;
        const float soft[2] = {s0, s1};
        for (int g = 0; g < 2; ++g) {
            if (soft[g] == 0.0F) {
                continue;
            }
            const auto expected =
                static_cast<std::uint8_t>(std::popcount(lane.encoder & kMfskFecGenerators[g]) & 1);
            const std::uint8_t received = soft[g] < 0.0F ? 1 : 0;
            wrong += expected != received ? 1.0 : 0.0;
            ++counted;
        }
        const double share = counted > 0 ? wrong / counted : 0.5;
        lane.disagreement += kDisagreementAverage * (share - lane.disagreement);
        ++lane.decided;
        lane.wrong[lane.decided % lane.wrong.size()] = static_cast<float>(share);

        const int code = lane.varicode.push(bit);
        if (code >= 0) {
            lane.held.push_back(Held{static_cast<std::uint8_t>(code), position, lane.decided});
        }
    }
}

void Mfsk16Decoder::choose_and_print(std::vector<Mfsk16Character>& out, bool all) {
    const int other = 1 - chosen_;
    if (lanes_[static_cast<std::size_t>(other)].disagreement + kSwitchMargin <
        lanes_[static_cast<std::size_t>(chosen_)].disagreement) {
        chosen_ = other;
    }
    for (std::size_t li = 0; li < lanes_.size(); ++li) {
        Lane& lane = lanes_[li];
        while (!lane.held.empty() && (all || lane.decided - lane.held.front().decided >= kHoldBits)) {
            const Held h = lane.held.front();
            lane.held.pop_front();
            if (static_cast<int>(li) != chosen_) {
                continue;
            }
            if (printed_any_ && h.position <= last_printed_) {
                continue;
            }
            const std::uint64_t from = h.decided > kSquelchBefore ? h.decided - kSquelchBefore : 1;
            const std::uint64_t to = h.decided;
            double sum = 0.0;
            for (std::uint64_t i = from; i <= to; ++i) {
                sum += lane.wrong[i % lane.wrong.size()];
            }
            if (sum / static_cast<double>(to - from + 1) >= kSquelchShare) {
                continue;
            }
            printed_any_ = true;
            last_printed_ = h.position;
            out.push_back(Mfsk16Character{h.code, h.position});
        }
    }
}

void Mfsk16Decoder::flush(std::vector<Mfsk16Character>& out) {
    for (Lane& lane : lanes_) {
        bits_.clear();
        lane.viterbi.flush(false, bits_);
        on_bits(lane, bits_);
        lane.pending.clear();
        lane.pending_sample.clear();
    }
    choose_and_print(out, true);
}

}  // namespace revenant::decode
