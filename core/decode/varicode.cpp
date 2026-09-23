#include "core/decode/varicode.h"

#include <format>

namespace revenant::decode {
namespace {

// Longest run this decoder will accumulate before it stops trying to read it
// as a character. Anything past Table 1's ten bits is already unrecognised;
// this bound only keeps a stream of ones, which is a steady carrier and the
// PSK31 postamble, from overflowing the word.
constexpr std::size_t kLongestRun = 31;

}  // namespace

Expected<std::string_view> varicode_for(std::uint8_t ascii) {
    if (ascii >= kVaricode.size()) {
        return fail(std::format(
            "Varicode's base alphabet is QEX July/August 1999 Table 1, ASCII 0 to 127; "
            "{} is outside it",
            ascii));
    }
    return kVaricode[ascii];
}

Expected<std::vector<std::uint8_t>> varicode_encode(std::string_view text) {
    std::vector<std::uint8_t> bits;
    bits.reserve(text.size() * 9);
    for (const char c : text) {
        auto code = varicode_for(static_cast<std::uint8_t>(c));
        if (!code) {
            return std::unexpected(code.error());
        }
        for (const char symbol : *code) {
            bits.push_back(static_cast<std::uint8_t>(symbol == '1' ? 1 : 0));
        }
        // Table 1 caption: "A minimum of two zeros is inserted between
        // characters."
        bits.push_back(0);
        bits.push_back(0);
    }
    return bits;
}

bool varicode_lookup(std::uint32_t code, std::size_t bits, std::uint8_t& ascii) {
    if (bits == 0 || bits > kVaricodeMaxBits) {
        return false;
    }
    for (std::size_t i = 0; i < kVaricode.size(); ++i) {
        const std::string_view entry = kVaricode[i];
        if (entry.size() != bits) {
            continue;
        }
        std::uint32_t value = 0;
        for (const char symbol : entry) {
            value = (value << 1U) | (symbol == '1' ? 1U : 0U);
        }
        if (value == code) {
            ascii = static_cast<std::uint8_t>(i);
            return true;
        }
    }
    return false;
}

void VaricodeDecoder::reset() {
    code_ = 0;
    bits_ = 0;
    zeros_ = 0;
    after_gap_ = false;
    first_bit_ = 0;
    overflowed_ = false;
}

void VaricodeDecoder::push(std::uint8_t bit, std::uint64_t bit_index,
                           std::vector<VaricodeCharacter>& out) {
    if (bit != 0) {
        if (zeros_ >= 2) {
            // The first 1 after a gap starts a character.
            code_ = 0;
            bits_ = 0;
            first_bit_ = bit_index;
            overflowed_ = false;
            after_gap_ = true;
        } else if (zeros_ == 1 && bits_ > 0) {
            // A single zero is inside a character: page 5 allows one and only
            // forbids two.
            if (bits_ < kLongestRun) {
                code_ <<= 1U;
                ++bits_;
            } else {
                overflowed_ = true;
            }
        }
        if (bits_ < kLongestRun) {
            code_ = (code_ << 1U) | 1U;
            ++bits_;
        } else {
            overflowed_ = true;
        }
        zeros_ = 0;
        return;
    }

    ++zeros_;
    if (zeros_ == 2 && bits_ > 0 && !after_gap_) {
        // The run before the first gap: see zeros_ in the header.
        code_ = 0;
        bits_ = 0;
        return;
    }
    if (zeros_ == 2 && bits_ > 0) {
        VaricodeCharacter character;
        character.bits = bits_;
        character.first_bit = first_bit_;
        std::uint8_t ascii = 0;
        if (!overflowed_ && varicode_lookup(code_, bits_, ascii)) {
            character.ascii = ascii;
            character.recognised = true;
        }
        out.push_back(character);
        code_ = 0;
        bits_ = 0;
    }
}

}  // namespace revenant::decode
