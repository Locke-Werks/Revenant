#include "core/decode/pocsag.h"

#include <bit>
#include <iterator>

namespace revenant::decode {

namespace {

// Remainder of a polynomial of degree at most 30 on division by the clause
// 1.4 generator, which is the 10 check bits.
constexpr std::uint32_t remainder(std::uint32_t v) {
    for (int i = 30; i >= 10; --i) {
        if (((v >> static_cast<unsigned>(i)) & 1U) != 0U) {
            v ^= kPocsagGenerator << static_cast<unsigned>(i - 10);
        }
    }
    return v & 0x3FFU;
}

// Syndrome of a single error at each of the 31 BCH positions, position 0
// being the x^0 coefficient. Syndromes are linear, so a double error's is the
// exclusive or of two of these.
constexpr std::array<std::uint32_t, 31> make_single_syndromes() {
    std::array<std::uint32_t, 31> s{};
    for (unsigned i = 0; i < 31; ++i) {
        s[i] = remainder(1U << i);
    }
    return s;
}

constexpr std::array<std::uint32_t, 31> kSingleSyndromes = make_single_syndromes();

bool even_parity(std::uint32_t word) {
    return (std::popcount(word) & 1) == 0;
}

// Table 3, indexed by the 4-bit combination with bit 1 as the least
// significant, which is how the table's "Bit No.: 4 3 2 1" heading reads.
std::string numeric_character(unsigned v) {
    static constexpr char kDigits[] = "0123456789";
    if (v < 10) {
        return std::string(1, kDigits[v]);
    }
    switch (v) {
        case 0xA:
            // "Spare": no glyph. U+FFFD, the replacement character.
            return std::string{static_cast<char>(0xEF), static_cast<char>(0xBF),
                               static_cast<char>(0xBD)};
        case 0xB: return "U";
        case 0xC: return " ";
        case 0xD: return "-";
        case 0xE: return "]";
        default: return "[";
    }
}

}  // namespace

std::uint32_t pocsag_encode(std::uint32_t information) {
    const std::uint32_t shifted = (information & 0x1FFFFFU) << 10U;
    const std::uint32_t block = shifted | remainder(shifted);
    const std::uint32_t word = block << 1U;
    return word | (even_parity(word) ? 0U : 1U);
}

PocsagCorrection pocsag_correct(std::uint32_t received) {
    PocsagCorrection result;
    const std::uint32_t block = received >> 1U;
    const std::uint32_t syndrome = remainder(block);
    const bool parity_ok = even_parity(received);

    if (syndrome == 0U) {
        result.valid = true;
        result.word = parity_ok ? received : (received ^ 1U);
        result.corrected_bits = parity_ok ? 0 : 1;
        return result;
    }

    const auto finish = [&](std::uint32_t error, int in_block) {
        std::uint32_t word = received ^ (error << 1U);
        int corrected = in_block;
        if (!even_parity(word)) {
            if (in_block == 2) {
                // Two in the block and the parity still odd is a third
                // error: detected, not corrected.
                return;
            }
            word ^= 1U;
            ++corrected;
        }
        result.valid = true;
        result.word = word;
        result.corrected_bits = corrected;
    };

    for (unsigned i = 0; i < 31; ++i) {
        if (kSingleSyndromes[i] == syndrome) {
            finish(1U << i, 1);
            return result;
        }
    }
    for (unsigned i = 0; i < 31; ++i) {
        for (unsigned j = i + 1; j < 31; ++j) {
            if ((kSingleSyndromes[i] ^ kSingleSyndromes[j]) == syndrome) {
                finish((1U << i) | (1U << j), 2);
                return result;
            }
        }
    }
    return result;
}

std::string pocsag_numeric(std::span<const std::uint8_t> bits) {
    std::string text;
    for (std::size_t i = 0; i + 4 <= bits.size(); i += 4) {
        // Clause 2.1: "The bits of each character are transmitted in
        // numerical order starting with bit No. 1."
        const unsigned v = (bits[i] & 1U) | ((bits[i + 1] & 1U) << 1U) |
                           ((bits[i + 2] & 1U) << 2U) | ((bits[i + 3] & 1U) << 3U);
        text += numeric_character(v);
    }
    // Clause 2.1: the unwanted part of the last codeword is filled with
    // spaces. A message that really ends in a space cannot be told from fill,
    // so trailing spaces go.
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
    return text;
}

std::string pocsag_alphanumeric(std::span<const std::uint8_t> bits) {
    std::string text;
    for (std::size_t i = 0; i + 7 <= bits.size(); i += 7) {
        unsigned v = 0;
        for (unsigned b = 0; b < 7; ++b) {
            v |= (bits[i + b] & 1U) << b;
        }
        if (v != 0) {
            text.push_back(static_cast<char>(v));
        }
    }
    // Clause 2.2 names End of Text and End of Transmission as fill.
    while (!text.empty() && (text.back() == 0x03 || text.back() == 0x04)) {
        text.pop_back();
    }
    return text;
}

// ---------------------------------------------------------------------------
// PocsagDecoder
// ---------------------------------------------------------------------------

Expected<PocsagDecoder> PocsagDecoder::create(const PocsagConfig& config) {
    if (config.sync_tolerance < 0 || config.sync_tolerance > 4) {
        return fail("a POCSAG sync tolerance above four bits would find sync in noise");
    }
    if (config.address_correction_budget < 0 || config.address_correction_budget > 2) {
        return fail(
            "the M.584-2 clause 1.4 code corrects at most two bits, so an address correction "
            "budget must be 0, 1 or 2");
    }
    LevelDiscriminatorConfig level;
    level.rate = config.rate;
    level.symbol_rate = config.bit_rate;
    auto discriminator = LevelDiscriminator::create(level);
    if (!discriminator) {
        return std::unexpected(with_context(discriminator.error(), "POCSAG"));
    }
    BitClockConfig clock;
    clock.rate = config.rate;
    clock.symbol_rate = config.bit_rate;
    auto bit_clock = BitClock::create(clock);
    if (!bit_clock) {
        return std::unexpected(with_context(bit_clock.error(), "POCSAG"));
    }

    PocsagDecoder d;
    d.config_ = config;
    d.discriminator_ = std::move(*discriminator);
    d.clock_ = std::move(*bit_clock);
    d.reset();
    return d;
}

void PocsagDecoder::reset() {
    discriminator_.reset();
    clock_.reset();
    shift_ = 0;
    synced_ = false;
    inverted_ = false;
    bit_in_word_ = 0;
    word_in_batch_ = 0;
    word_sample_ = 0;
    expecting_sync_ = false;
    open_.reset();
    consecutive_bad_ = 0;
    stats_ = {};
    bit_count_ = 0;
    confirmed_ = false;
    held_.clear();
}

void PocsagDecoder::process(ConstRealSpan audio, std::vector<PocsagPage>& out) {
    soft_.clear();
    bits_.clear();
    discriminator_.process(audio, soft_);
    clock_.process(soft_, bits_);
    const auto delay = static_cast<SampleIndex>(discriminator_.group_delay());
    for (const SoftBit& b : bits_) {
        // M.539-3 clause 4.3: a positive shift is binary 0.
        const std::uint8_t bit = (b.value >= 0.0F) ? 0U : 1U;
        on_bit(bit, b.position > delay ? b.position - delay : 0, out);
    }
}

void PocsagDecoder::flush(std::vector<PocsagPage>& out) {
    finish(out);
    lose_sync();
    bit_in_word_ = 0;
    expecting_sync_ = false;
}

void PocsagDecoder::lose_sync() {
    if (synced_ && !confirmed_) {
        ++stats_.batches_unconfirmed;
    }
    synced_ = false;
    stats_.pages_unconfirmed += held_.size();
    held_.clear();
    confirmed_ = false;
}

void PocsagDecoder::confirm(std::vector<PocsagPage>& out) {
    out.insert(out.end(), std::make_move_iterator(held_.begin()),
               std::make_move_iterator(held_.end()));
    held_.clear();
    confirmed_ = true;
}

void PocsagDecoder::on_bit(std::uint8_t raw, SampleIndex sample, std::vector<PocsagPage>& out) {
    shift_ = (shift_ << 1U) | raw;
    ++bit_count_;
    const auto tolerance = config_.sync_tolerance;
    const auto codeword = static_cast<std::uint32_t>(shift_);

    if (!synced_) {
        if (bit_count_ < kPocsagCodewordBits) {
            return;
        }
        if (std::popcount(codeword ^ kPocsagSync) <= tolerance) {
            synced_ = true;
            inverted_ = false;
        } else if (std::popcount(codeword ^ ~kPocsagSync) <= tolerance) {
            synced_ = true;
            inverted_ = true;
        } else {
            return;
        }
        // Clause 1.1: a transmission starts with at least 576 bits of
        // reversals, 101010, to help the receiver acquire batch
        // synchronization. The 32 before this codeword being reversals is
        // that preamble, and the batch is taken at once. A reversal
        // complemented is a reversal, so the polarity does not enter into
        // it; either phase is accepted, since the clause does not say which
        // bit the preamble ends on.
        const auto before = static_cast<std::uint32_t>(shift_ >> 32U);
        const bool preamble = bit_count_ >= 2 * kPocsagCodewordBits &&
                              (std::popcount(before ^ 0xAAAA'AAAAU) <= tolerance ||
                               std::popcount(before ^ 0x5555'5555U) <= tolerance);
        confirmed_ = preamble;
        held_.clear();
        ++stats_.batches;
        bit_in_word_ = 0;
        word_in_batch_ = 0;
        expecting_sync_ = false;
        return;
    }

    if (bit_in_word_ == 0) {
        word_sample_ = sample;
    }
    ++bit_in_word_;
    if (bit_in_word_ < kPocsagCodewordBits) {
        return;
    }
    bit_in_word_ = 0;
    const std::uint32_t word = inverted_ ? ~codeword : codeword;

    if (expecting_sync_) {
        // Clause 1.2: every batch begins with the synchronization codeword.
        // Once locked the flywheel allows twice the acquisition tolerance,
        // since the codeword is where it must be rather than being searched
        // for.
        if (std::popcount(word ^ kPocsagSync) <= 2 * tolerance) {
            ++stats_.batches;
            expecting_sync_ = false;
            word_in_batch_ = 0;
            if (!confirmed_) {
                // Clause 2.3: a valid batch has followed, so the provisional
                // one was real and its pages go out.
                confirm(out);
            }
            return;
        }
        // Clause 1.2 lets a transmission end at the end of any batch; this is
        // that, or a lost lock. Either way the page in progress is done, and
        // the search starts again from the bits already in the register. A
        // provisional batch ends here unconfirmed, and its pages with it.
        finish(out);
        lose_sync();
        return;
    }

    on_codeword(word, word_sample_, out);
    ++word_in_batch_;
    if (word_in_batch_ == kPocsagCodewordsPerBatch) {
        expecting_sync_ = true;
    }
}

void PocsagDecoder::on_codeword(std::uint32_t received, SampleIndex sample,
                                std::vector<PocsagPage>& out) {
    ++stats_.codewords;
    const PocsagCorrection c = pocsag_correct(received);
    if (!c.valid) {
        ++stats_.uncorrectable;
        if (!open_) {
            return;
        }
        ++open_->uncorrectable_codewords;
        for (unsigned k = 2; k <= 21; ++k) {
            open_->message_bits.push_back(static_cast<std::uint8_t>((received >> (32U - k)) & 1U));
        }
        // Clause 2.5.1: stop when two successive codewords are
        // indecipherable.
        if (++consecutive_bad_ >= 2) {
            finish(out);
        }
        return;
    }
    stats_.corrected_bits += static_cast<std::uint64_t>(c.corrected_bits);
    const std::uint32_t word = c.word;

    if (word == kPocsagIdle) {
        // Clause 1.2: a message ends at the next idle codeword.
        finish(out);
        return;
    }
    if (pocsag_is_message(word)) {
        if (!open_) {
            return;
        }
        consecutive_bad_ = 0;
        open_->corrected_bits += c.corrected_bits;
        // Clause 1.3.3: message bits are bits 2 to 21.
        for (unsigned k = 2; k <= 21; ++k) {
            open_->message_bits.push_back(static_cast<std::uint8_t>((word >> (32U - k)) & 1U));
        }
        return;
    }

    // Clause 1.2: a message also ends at the next address codeword.
    finish(out);

    // Clause 1.3.2 and 1.4: an address codeword is the BCH(31,21) block and
    // an even parity bit, distance 6 together. Corrected past the budget,
    // it is as likely another code word's errors as this one's; see
    // PocsagConfig::address_correction_budget. It still ends the message
    // before it, since whatever it was, it was not that message's text.
    if (c.corrected_bits > config_.address_correction_budget) {
        ++stats_.addresses_refused;
        return;
    }

    PocsagPage page;
    // Clause 1.3.2: bits 2 to 19 are the 18 most significant bits of the
    // identity, and clause 1.2 puts the 3 least significant in the frame.
    const auto frame = static_cast<std::uint32_t>(word_in_batch_ / 2);
    page.identity = (((word >> 13U) & 0x3FFFFU) << 3U) | frame;
    // Bits 20 and 21.
    page.function = static_cast<std::uint8_t>((word >> 11U) & 0x3U);
    page.position = sample;
    page.corrected_bits = c.corrected_bits;
    page.inverted = inverted_;
    open_ = std::move(page);
    consecutive_bad_ = 0;
}

void PocsagDecoder::finish(std::vector<PocsagPage>& out) {
    if (!open_) {
        return;
    }
    PocsagPage page = std::move(*open_);
    open_.reset();
    consecutive_bad_ = 0;
    if (page.function == kPocsagFunctionNumeric) {
        page.format = PocsagPage::Format::Numeric;
        page.text = pocsag_numeric(page.message_bits);
    } else if (page.function == kPocsagFunctionAlphanumeric) {
        page.format = PocsagPage::Format::Alphanumeric;
        page.text = pocsag_alphanumeric(page.message_bits);
    }
    if (confirmed_) {
        out.push_back(std::move(page));
    } else {
        held_.push_back(std::move(page));
    }
}

}  // namespace revenant::decode
