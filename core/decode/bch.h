// Binary BCH codes over GF(2^m), m from 3 to 10: systematic encoder and
// hard-decision decoder, full length or shortened.
//
// THE CODE
//
// Lin and Costello, "Error Control Coding", chapter 6. A narrow-sense code of
// design distance d has as generator the least common multiple of the minimal
// polynomials of alpha^1 through alpha^(d-1), alpha being x in the field the
// given primitive polynomial defines; each minimal polynomial is the product
// of (x - alpha^j) over j's cyclotomic coset modulo 2^m - 1. A code whose
// standard prints its generator instead is built from that generator, and its
// correcting power is read off the longest run of consecutive roots alpha^1,
// alpha^2, ... the generator has, which is the BCH bound.
//
// THE DECODER
//
// Syndromes S_1 to S_2t by evaluating the received word at alpha^j; the
// Berlekamp-Massey algorithm (Massey, IEEE Trans. Inf. Theory, January 1969)
// for the error locator; a Chien search over the positions the word occupies.
// In a binary code an error's value is always one, so there is no Forney step.
// A word is refused when the locator's degree exceeds t, or the Chien search
// finds fewer roots inside the word than the locator's degree. That second
// test is what refuses most words beyond t, and also any correction that
// would land in the leading zeros a shortened code does not send.
//
// BIT ORDER
//
// One bit per byte. The first bit of a word is the coefficient of the highest
// power of x and the check bits are the lowest powers, so a systematic word is
// its information bits followed by its check bits, and a shortened word is the
// full word with its leading information bits fixed at zero and not sent.
//
// CLEAN ROOM
//
// Textbook only. No BCH implementation was read.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/error.h"

namespace revenant::decode {

struct BchParams {
    // 3 to 10.
    unsigned m = 0;
    // The field's primitive polynomial with its x^m term.
    unsigned field_polynomial = 0;
    // The design distance, 3 or more and odd for a narrow-sense code. Ignored
    // when `generator` is given.
    unsigned design_distance = 0;
    // An explicit generator, coefficients highest power first, monic. Empty
    // to derive it from the design distance.
    std::vector<std::uint8_t> generator;
};

class BchCodec {
public:
    // Fails for m outside 3 to 10, a field polynomial that is not primitive
    // of degree m, a design distance below 3 or leaving no information bits,
    // or an explicit generator that does not divide x^n - 1 or has no root
    // run that corrects at least one error.
    [[nodiscard]] static Expected<BchCodec> create(const BchParams& params);

    // Full length, 2^m - 1.
    [[nodiscard]] std::size_t n() const { return n_; }
    // Information bits at full length.
    [[nodiscard]] std::size_t k() const { return n_ - check_bits(); }
    [[nodiscard]] std::size_t check_bits() const { return generator_.size() - 1; }
    [[nodiscard]] std::size_t correctable() const { return t_; }
    // Highest power first, check_bits() + 1 coefficients.
    [[nodiscard]] const std::vector<std::uint8_t>& generator() const { return generator_; }

    // `information` may be 1 to k() bits; fewer is a shortened code. `check`
    // must hold exactly check_bits().
    [[nodiscard]] Status encode(std::span<const std::uint8_t> information,
                                std::span<std::uint8_t> check) const;

    // Corrects a word, information then check bits, in place. Its length is
    // check_bits() + 1 to n(). Returns the number of bits changed, or nothing
    // when the word is refused, in which case it is left as received.
    [[nodiscard]] std::optional<std::size_t> decode(std::span<std::uint8_t> word) const;

private:
    BchCodec() = default;

    [[nodiscard]] unsigned mul(unsigned a, unsigned b) const;
    [[nodiscard]] unsigned pow(unsigned exponent) const { return exp_[exponent % n_]; }

    std::size_t n_ = 0;
    std::size_t t_ = 0;
    std::vector<unsigned> exp_;
    std::vector<unsigned> log_;
    std::vector<std::uint8_t> generator_;
};

}  // namespace revenant::decode
