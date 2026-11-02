#include "core/decode/ale_2g.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace revenant::decode {

namespace {

// MIL-STD-188-141D figure A-6, the P half of G = [I12 : P]. Row 1 is the
// check contribution of data bit W1 (or W13), and within a row the leftmost
// printed bit is G1 (or G13), held here in bit 11.
constexpr std::array<std::uint16_t, 12> kGolayP = {
    0b101011100011, 0b111110010010, 0b110100101011, 0b110001110110,
    0b110011011001, 0b011001101101, 0b001100110111, 0b101101111000,
    0b010110111100, 0b001011011110, 0b101110001101, 0b010111000111,
};

// Syndrome to error pattern, for every pattern of up to three bits over the
// 24-bit code word (data in bits 23..12, checks in 11..0). The code's minimum
// distance of 8 makes every such syndrome distinct (A.5.2.2.2.2).
struct SyndromeTable {
    std::array<std::uint32_t, 4096> pattern{};
    std::array<std::int8_t, 4096> weight{};

    SyndromeTable() {
        weight.fill(-1);
        auto syndrome = [](std::uint32_t e) {
            return static_cast<std::uint16_t>(
                ale_golay_parity(static_cast<std::uint16_t>(e >> 12U)) ^ (e & 0xFFFU));
        };
        weight[0] = 0;
        for (int a = 0; a < 24; ++a) {
            const std::uint32_t ea = 1U << static_cast<unsigned>(a);
            const auto sa = syndrome(ea);
            weight[sa] = 1;
            pattern[sa] = ea;
            for (int b = a + 1; b < 24; ++b) {
                const std::uint32_t eb = ea | (1U << static_cast<unsigned>(b));
                const auto sb = syndrome(eb);
                weight[sb] = 2;
                pattern[sb] = eb;
                for (int c = b + 1; c < 24; ++c) {
                    const std::uint32_t ec = eb | (1U << static_cast<unsigned>(c));
                    const auto sc = syndrome(ec);
                    weight[sc] = 3;
                    pattern[sc] = ec;
                }
            }
        }
    }
};

const SyndromeTable& syndrome_table() {
    static const SyndromeTable table;
    return table;
}

bool is_routing(AlePreamble p) {
    return p == AlePreamble::To || p == AlePreamble::Tis || p == AlePreamble::Twas ||
           p == AlePreamble::Thru || p == AlePreamble::From;
}

std::string chars_of(std::uint32_t word) {
    const auto c = ale_word_chars(word);
    return std::string(c.begin(), c.end());
}

// A.5.2.4.3: "@" stuffs the empty trailing positions of the last word. The
// special addresses of A.5.2.4.6 onwards begin with "@" and keep theirs.
std::string unstuff(std::string address) {
    if (!address.empty() && address.front() != '@') {
        while (!address.empty() && address.back() == '@') {
            address.pop_back();
        }
    }
    return address;
}

}  // namespace

const char* ale_preamble_name(AlePreamble p) {
    switch (p) {
        case AlePreamble::Data: return "DATA";
        case AlePreamble::Thru: return "THRU";
        case AlePreamble::To: return "TO";
        case AlePreamble::Twas: return "TWAS";
        case AlePreamble::From: return "FROM";
        case AlePreamble::Tis: return "TIS";
        case AlePreamble::Cmd: return "CMD";
        case AlePreamble::Rep: return "REP";
    }
    return "?";
}

std::uint16_t ale_golay_parity(std::uint16_t data) {
    std::uint16_t parity = 0;
    for (unsigned row = 0; row < 12; ++row) {
        if (((data >> (11U - row)) & 1U) != 0U) {
            parity ^= kGolayP[row];
        }
    }
    return parity;
}

AleGolayDecode ale_golay_decode(std::uint16_t data, std::uint16_t parity, int max_errors) {
    data &= 0xFFFU;
    parity &= 0xFFFU;
    const auto s = static_cast<std::uint16_t>(ale_golay_parity(data) ^ parity);
    const auto& table = syndrome_table();
    const int w = table.weight[s];
    AleGolayDecode out;
    if (w < 0 || w > max_errors) {
        out.data = data;
        out.errors = -1;
        return out;
    }
    out.data = static_cast<std::uint16_t>(data ^ (table.pattern[s] >> 12U));
    out.errors = w;
    return out;
}

std::uint32_t ale_word(AlePreamble preamble, const char chars[3]) {
    std::uint32_t w = static_cast<std::uint32_t>(preamble) & 7U;
    for (int i = 0; i < 3; ++i) {
        w = (w << 7U) | (static_cast<std::uint32_t>(static_cast<unsigned char>(chars[i])) & 0x7FU);
    }
    return w;
}

AlePreamble ale_word_preamble(std::uint32_t word) {
    return static_cast<AlePreamble>((word >> 21U) & 7U);
}

std::array<char, 3> ale_word_chars(std::uint32_t word) {
    return {static_cast<char>((word >> 14U) & 0x7FU), static_cast<char>((word >> 7U) & 0x7FU),
            static_cast<char>(word & 0x7FU)};
}

std::array<std::uint8_t, kAleWordBits> ale_word_bits(std::uint32_t word) {
    const auto wa = static_cast<std::uint16_t>((word >> 12U) & 0xFFFU);
    const auto wb = static_cast<std::uint16_t>(word & 0xFFFU);
    // Figure A-10: coder A is W1..W12 then G1..G12; coder B is W13..W24 then
    // G13..G24 inverted.
    const std::uint32_t a = (static_cast<std::uint32_t>(wa) << 12U) | ale_golay_parity(wa);
    const std::uint32_t b = ((static_cast<std::uint32_t>(wb) << 12U) | ale_golay_parity(wb)) ^ 0xFFFU;
    std::array<std::uint8_t, kAleWordBits> out{};
    for (unsigned i = 0; i < 24; ++i) {
        out[2 * i] = static_cast<std::uint8_t>((a >> (23U - i)) & 1U);
        out[2 * i + 1] = static_cast<std::uint8_t>((b >> (23U - i)) & 1U);
    }
    out[48] = 0;  // S49
    return out;
}

bool ale_basic38(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '@' || c == '?';
}

std::vector<std::uint32_t> ale_address_words(AlePreamble first, const std::string& address) {
    std::string padded = address;
    while (padded.empty() || padded.size() % 3 != 0) {
        padded.push_back('@');
    }
    std::vector<std::uint32_t> words;
    for (std::size_t i = 0; i < padded.size(); i += 3) {
        AlePreamble p = first;
        if (i > 0) {
            // A.5.2.3.2.1: TO, DATA, REP, DATA, REP.
            p = ((i / 3) % 2 == 1) ? AlePreamble::Data : AlePreamble::Rep;
        }
        words.push_back(ale_word(p, padded.data() + i));
    }
    return words;
}

// ---------------------------------------------------------------------------

Expected<Ale2gDecoder> Ale2gDecoder::create(const Ale2gConfig& config) {
    if (config.rate < 2000) {
        return std::unexpected(Error("2G ALE decoder: sample rate must be at least 2000 Hz"));
    }
    if (config.unanimous_threshold < 0 || config.unanimous_threshold > 48) {
        return std::unexpected(Error("2G ALE decoder: unanimous vote threshold is 0 to 48"));
    }
    if (config.golay_max_errors < 0 || config.golay_max_errors > 3) {
        return std::unexpected(Error("2G ALE decoder: Golay correction is 0 to 3 errors"));
    }
    MfskDemodConfig mc;
    mc.rate = config.rate;
    mc.tones = kAleTones;
    mc.tone_spacing_hz = kAleToneSpacingHz;
    mc.symbol_rate = kAleSymbolRate;
    mc.base_hz = kAleToneBaseHz;
    mc.search = false;
    mc.afc = config.afc;
    auto demod = MfskDemodulator::create(mc);
    if (!demod) {
        return std::unexpected(demod.error());
    }
    return Ale2gDecoder(config, std::move(*demod));
}

Ale2gDecoder::Ale2gDecoder(const Ale2gConfig& config, MfskDemodulator demod)
    : config_(config), demod_(std::move(demod)) {}

void Ale2gDecoder::reset() {
    demod_.reset();
    symbols_.clear();
    bits_.clear();
    bit_pos_.clear();
    bit_base_ = 0;
    bit_count_ = 0;
    locked_ = false;
    pending_ = false;
    misses_ = 0;
    frame_ = AleFrame{};
    frame_open_ = false;
    last_word_ = 0xFFFFFFFFU;
    field_ = Field::None;
}

Ale2gDecoder::Candidate Ale2gDecoder::read_at(std::int64_t end) const {
    Candidate c;
    // Table A-VII: the newest bit is S49 and is ignored; M(n-1) is B24 and
    // M(n-48) is A1. Each majority bit votes over three copies 49 bits apart.
    const std::int64_t oldest = end - 48 - 2 * static_cast<std::int64_t>(kAleWordBits);
    if (oldest < bit_base_) {
        return c;
    }
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    int unanimous = 0;
    for (std::int64_t j = 0; j < 48; ++j) {
        const std::int64_t idx = end - 48 + j - bit_base_;
        const int r0 = bits_[static_cast<std::size_t>(idx)];
        const int r1 = bits_[static_cast<std::size_t>(idx - 49)];
        const int r2 = bits_[static_cast<std::size_t>(idx - 98)];
        const int sum = r0 + r1 + r2;
        // Table A-VI.
        if (sum == 0 || sum == 3) {
            ++unanimous;
        }
        const std::uint32_t m = sum >= 2 ? 1U : 0U;
        if ((j & 1) == 0) {
            a = (a << 1U) | m;
        } else {
            b = (b << 1U) | m;
        }
    }
    b ^= 0xFFFU;  // G13..G24 back to normal before decoding (figure A-10)
    const auto da = ale_golay_decode(static_cast<std::uint16_t>(a >> 12U),
                                     static_cast<std::uint16_t>(a & 0xFFFU),
                                     config_.golay_max_errors);
    const auto db = ale_golay_decode(static_cast<std::uint16_t>(b >> 12U),
                                     static_cast<std::uint16_t>(b & 0xFFFU),
                                     config_.golay_max_errors);
    if (da.errors < 0 || db.errors < 0) {
        return c;
    }
    c.word = (static_cast<std::uint32_t>(da.data) << 12U) | db.data;
    c.unanimous = unanimous;
    c.corrected = da.errors + db.errors;
    c.ok = unanimous >= config_.unanimous_threshold;
    return c;
}

bool Ale2gDecoder::routing_ok(const Candidate& c) const {
    if (!c.ok || !is_routing(ale_word_preamble(c.word))) {
        return false;
    }
    const auto chars = ale_word_chars(c.word);
    return std::all_of(chars.begin(), chars.end(), ale_basic38);
}

void Ale2gDecoder::process(dsp::ConstRealSpan audio, std::vector<AleWord>& words,
                           std::vector<AleFrame>& frames) {
    symbols_.clear();
    demod_.process(audio, symbols_);
    for (const auto& s : symbols_) {
        const std::uint8_t tri = kAleToneBits[static_cast<std::size_t>(s.tone & 7)];
        for (int k = 2; k >= 0; --k) {
            bits_.push_back(static_cast<std::uint8_t>((tri >> k) & 1U));
            bit_pos_.push_back(s.start);
            ++bit_count_;
            on_bit(words, frames);
        }
        // Keep a little over two triples of history.
        constexpr std::size_t kKeep = 2 * kAleTripleBits;
        if (bits_.size() > 4 * kKeep) {
            const std::size_t drop = bits_.size() - kKeep;
            bits_.erase(bits_.begin(), bits_.begin() + static_cast<std::ptrdiff_t>(drop));
            bit_pos_.erase(bit_pos_.begin(), bit_pos_.begin() + static_cast<std::ptrdiff_t>(drop));
            bit_base_ += static_cast<std::int64_t>(drop);
        }
    }
}

void Ale2gDecoder::on_bit(std::vector<AleWord>& words, std::vector<AleFrame>& frames) {
    const std::int64_t end = bit_count_ - 1;
    if (locked_) {
        // Read one word period late, so the alignments a word either side of
        // the expected one can be compared. A scan call's repeated word
        // reads the same at every alignment and cannot fix the triple phase
        // (A.5.2.6.1); the first different word after it does, and so does
        // a slipped clock.
        constexpr auto kWord = static_cast<std::int64_t>(kAleWordBits);
        if (end != next_read_ + kWord) {
            return;
        }
        const std::int64_t expected = next_read_;
        Candidate best;
        std::int64_t best_end = expected;
        for (const std::int64_t at : {expected, expected - kWord, expected + kWord}) {
            const Candidate c = read_at(at);
            const bool ok = c.ok && (!is_routing(ale_word_preamble(c.word)) || routing_ok(c));
            if (ok && (!best.ok || c.unanimous > best.unanimous)) {
                best = c;
                best_end = at;
            }
        }
        next_read_ = best_end + static_cast<std::int64_t>(kAleTripleBits);
        if (best.ok) {
            misses_ = 0;
            accept(best, best_end, words, frames);
            return;
        }
        if (++misses_ >= 2) {
            locked_ = false;
            end_frame(frames);
        }
        return;
    }

    const Candidate c = read_at(end);
    if (pending_) {
        if (routing_ok(c) && c.word == pending_best_.word && c.unanimous > pending_best_.unanimous) {
            pending_best_ = c;
            pending_end_ = end;
        }
        if (end >= pending_first_ + 2 * static_cast<std::int64_t>(kAleWordBits)) {
            pending_ = false;
            locked_ = true;
            misses_ = 0;
            next_read_ = pending_end_ + static_cast<std::int64_t>(kAleTripleBits);
            accept(pending_best_, pending_end_, words, frames);
        }
        return;
    }
    if (routing_ok(c)) {
        pending_ = true;
        pending_best_ = c;
        pending_end_ = end;
        pending_first_ = end;
    }
}

void Ale2gDecoder::accept(const Candidate& c, std::int64_t end, std::vector<AleWord>& words,
                          std::vector<AleFrame>& frames) {
    AleWord w;
    const std::int64_t first = end - 48 - bit_base_;
    w.position = bit_pos_[static_cast<std::size_t>(std::max<std::int64_t>(first, 0))];
    w.word = c.word;
    w.preamble = ale_word_preamble(c.word);
    w.chars = ale_word_chars(c.word);
    w.unanimous = c.unanimous;
    w.corrected = c.corrected;
    words.push_back(w);

    // A scan call repeats its first word for as long as the scan takes
    // (A.5.2.2.4); the frame records it once.
    if (frame_open_ && c.word == last_word_) {
        return;
    }
    const AlePreamble prev = frame_open_ ? ale_word_preamble(last_word_) : AlePreamble::Data;
    last_word_ = c.word;

    // A TO after a conclusion starts the next frame.
    if (w.preamble == AlePreamble::To && field_ == Field::Conclusion) {
        end_frame(frames);
    }
    if (!frame_open_) {
        frame_ = AleFrame{};
        frame_.position = w.position;
        frame_open_ = true;
        field_ = Field::None;
    }
    frame_.words.push_back(w);
    const std::string text = chars_of(c.word);

    switch (w.preamble) {
        case AlePreamble::To:
        case AlePreamble::Thru:
            frame_.to.push_back(text);
            field_ = w.preamble == AlePreamble::To ? Field::To : Field::Thru;
            break;
        case AlePreamble::Tis:
        case AlePreamble::Twas:
            frame_.from = text;
            frame_.accepting = w.preamble == AlePreamble::Tis;
            field_ = Field::Conclusion;
            break;
        case AlePreamble::From:
            frame_.from = text;
            field_ = Field::From;
            break;
        case AlePreamble::Cmd:
            frame_.commands.push_back(c.word);
            field_ = Field::Cmd;
            break;
        case AlePreamble::Data:
        case AlePreamble::Rep:
            if (field_ == Field::Cmd) {
                frame_.commands.push_back(c.word);
            } else if (w.preamble == AlePreamble::Rep && prev != AlePreamble::Data &&
                       (field_ == Field::To || field_ == Field::Thru)) {
                // A.5.2.3.4.2: REP straight after a routing word repeats its
                // function for a different address (a group call's next
                // station).
                frame_.to.push_back(text);
            } else if ((field_ == Field::To || field_ == Field::Thru) && !frame_.to.empty()) {
                frame_.to.back() += text;
            } else if (field_ == Field::Conclusion || field_ == Field::From) {
                frame_.from += text;
            }
            break;
    }
}

void Ale2gDecoder::end_frame(std::vector<AleFrame>& frames) {
    if (!frame_open_) {
        return;
    }
    for (auto& a : frame_.to) {
        a = unstuff(a);
    }
    frame_.from = unstuff(frame_.from);
    if (field_ == Field::Conclusion) {
        frame_.kind = frame_.to.empty() ? AleFrame::Kind::Sound : AleFrame::Kind::Call;
    } else {
        frame_.kind = AleFrame::Kind::Unterminated;
    }
    frames.push_back(std::move(frame_));
    frame_ = AleFrame{};
    frame_open_ = false;
    last_word_ = 0xFFFFFFFFU;
    field_ = Field::None;
}

void Ale2gDecoder::flush(std::vector<AleFrame>& frames) {
    end_frame(frames);
}

}  // namespace revenant::decode
