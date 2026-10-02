// Reed-Solomon codes over GF(2^8), encoder and errors-only decoder, for the
// FX.25 codeblocks of core/decode/fx25.h.
//
// WHY THIS IS NOT THE P25 CODE IN core/decode/dv_codes.h
//
// dv_codes.h decodes P25's Reed-Solomon codes over GF(2^6), whose symbols are
// six-bit hexbits and whose field, generator and erasure handling are fixed by
// TIA-102.BAAA-B. FX.25 needs octet symbols and three different numbers of
// check symbols, so the arithmetic is a different field and the code here is
// parameterised by the field polynomial, the first consecutive root and the
// number of check symbols rather than written for one code.
//
// THE ALGORITHM
//
// Textbook, from no listing. A received word is evaluated at the generator's
// roots to give the syndromes; the Berlekamp-Massey algorithm (J. L. Massey,
// "Shift-register synthesis and BCH decoding", IEEE Trans. Inf. Theory,
// January 1969) finds the error locator; a Chien search evaluates the locator
// at every position the word actually occupies; Forney's formula (G. D.
// Forney, "On decoding BCH codes", IEEE Trans. Inf. Theory, October 1965)
// gives each error's value from the error evaluator. A word is refused when
// the locator's degree exceeds half the check symbols, when the Chien search
// finds fewer roots than that degree inside the word, or when the corrected
// word does not have zero syndromes. The second test is what refuses most
// words with more errors than the code corrects, and it is also what refuses
// a correction that would land in the zero symbols a shortened code never
// sends.
//
// SYMBOL ORDER
//
// The first symbol of a word is the coefficient of the highest power of x,
// and the check symbols are the lowest powers, so a systematic word is its
// information symbols followed by its check symbols. A shortened code is the
// full length-255 code with leading information symbols fixed at zero and not
// sent, which with this order leaves every sent symbol's power unchanged.
//
// CLEAN ROOM
//
// No Reed-Solomon implementation was read.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/error.h"

namespace revenant::decode {

// A GF(2^8) code's full length: the multiplicative group's order.
inline constexpr std::size_t kRs8FullLength = 255;

struct Rs8Params {
    // The field's primitive polynomial with its x^8 term, 0x100 to 0x1FF.
    unsigned field_polynomial = 0;
    // The generator's roots are alpha^first_root through
    // alpha^(first_root + check_symbols - 1), alpha being x.
    unsigned first_root = 0;
    // Twice the number of symbol errors the code corrects.
    std::size_t check_symbols = 0;
};

class Rs8Codec {
public:
    // Fails when the polynomial is not of degree 8 or does not have x as a
    // primitive element, or when the number of check symbols is not between
    // 2 and 254 and even.
    [[nodiscard]] static Expected<Rs8Codec> create(const Rs8Params& params);

    [[nodiscard]] const Rs8Params& params() const { return params_; }
    [[nodiscard]] std::size_t correctable() const { return params_.check_symbols / 2; }

    // Check symbols for `information`, which may be any length from 1 to
    // 255 less the check symbols; shorter is a shortened code. `check` must
    // hold exactly check_symbols octets.
    void encode(std::span<const std::uint8_t> information, std::span<std::uint8_t> check) const;

    // Corrects a received word, information then check symbols, in place.
    // Returns the number of symbols changed, zero for a word that was already
    // a code word, or nothing when the word was refused, in which case it is
    // left as it was received.
    [[nodiscard]] std::optional<std::size_t> decode(std::span<std::uint8_t> word) const;

    // The generator polynomial, highest power first, monic: check_symbols + 1
    // coefficients. Exposed for the tests.
    [[nodiscard]] const std::vector<std::uint8_t>& generator() const { return generator_; }

    // Field arithmetic, exposed for the tests.
    [[nodiscard]] std::uint8_t alpha_power(unsigned exponent) const;
    [[nodiscard]] std::uint8_t multiply(std::uint8_t a, std::uint8_t b) const;

private:
    Rs8Codec() = default;

    [[nodiscard]] std::uint8_t divide(std::uint8_t a, std::uint8_t b) const;

    Rs8Params params_{};
    // exp_ is doubled so a sum of two logarithms indexes it without a modulo.
    std::array<std::uint8_t, 2 * kRs8FullLength> exp_{};
    std::array<std::uint16_t, 256> log_{};
    std::vector<std::uint8_t> generator_;
};

}  // namespace revenant::decode
