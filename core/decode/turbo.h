// Parallel concatenated ("turbo") codes built from two copies of the eight
// state recursive systematic convolutional code G(D) = [1, n0/d, n1/d], with
// n0 = 1 + D + D^3, n1 = 1 + D + D^2 + D^3 and d = 1 + D^2 + D^3, an
// interleaver, a puncturing pattern for the data clocks and a puncturing and
// repetition pattern for the six termination clocks: an encoder, and an
// iterative max-log-MAP decoder.
//
// SPECIFICATION
//
// The code family is the one Recommendation ITU-R M.2092-2 (02/2026), Annex 2
// clause A2-1.2.4, defines for VDES, which it says follows ETSI EN 302 583:
// A2-1.2.4.1 and Figure 7 (two RSC encoders in parallel, the second fed the
// permuted input, each producing X, Y0, Y1), A2-1.2.4.2 and Figure 8 (the
// constituent transfer function above; k clocks of data, then six
// termination clocks with the switch at (b), the first three terminating and
// emitting RSC 1 only and the last three RSC 2 only, X first), A2-1.2.4.3 (the
// CCSDS 131.0-B-2 interleaver, given there as formulas over k = k1 k2 and
// eight primes) and A2-1.2.4.4 with Tables 5 and 6 (puncturing read left to
// right within a clock in the order X, Y0, Y1, X', Y'0, Y'1, then clock by
// clock; a tail entry of 2 or 3 sends that many copies).
//
// Which k1, k2, primes and patterns a mode uses is the mode's business, cited
// in that mode's own file. This file holds none of them.
//
// THE DECODER
//
// Textbook, from no listing. Each constituent code is decoded by the BCJR
// algorithm (L. R. Bahl, J. Cocke, F. Jelinek and J. Raviv, IEEE Trans. Inf.
// Theory, March 1974) in the max-log approximation (P. Robertson, E.
// Villebrun and P. Hoeher, ICC 1995), the two exchanging extrinsic
// information through the interleaver (C. Berrou, A. Glavieux and P.
// Thitimajshima, ICC 1993). The extrinsic values are scaled by 0.7 before
// they are passed on, the standard correction for the overconfidence of the
// max-log approximation (J. Vogt and A. Finger, Electronics Letters, November
// 2000). Both trellises start in state zero and, terminated, end in it.
//
// Repeated tail symbols are combined by adding their LLRs. Max-log-MAP is
// invariant to a common scale on its inputs, so LLRs need only be
// proportional to the true ones.
//
// CONVENTIONS
//
// A log-likelihood ratio is log(P(bit = 0) / P(bit = 1)), as in
// core/decode/ldpc.h. Bits in and out are one per byte, 0 or 1.
//
// CLEAN ROOM
//
// No turbo code implementation was read.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "core/error.h"

namespace revenant::decode {

// One clock's entries in the order X, Y0, Y1, X', Y'0, Y'1: 0 deletes the
// symbol, 1 passes it, 2 or 3 pass that many copies.
using TurboClockPattern = std::array<std::uint8_t, 6>;

struct TurboCodeSpec {
    // The interleaver: the s-th bit into the second encoder is input bit
    // permutation[s]. Its size is the information length k.
    std::vector<std::uint32_t> permutation;
    // Applied cyclically over the k data clocks.
    std::vector<TurboClockPattern> data_pattern;
    // The six termination clocks.
    std::array<TurboClockPattern, 6> tail_pattern{};
};

// The CCSDS 131.0-B-2 interleaver as M.2092-2 clause A2-1.2.4.3 writes it,
// zero-based: for s = 1..k, m = (s-1) mod 2, i = floor((s-1)/(2 k2)),
// j = floor((s-1)/2) - i k2, t = (19 i + 1) mod (k1/2), q = t mod 8 + 1,
// c = (p_q j + 21 m) mod k2, pi(s) = 2 (t + c k1/2 + 1) - m. Fails on an odd
// or zero k1, a zero k2, or a result that is not a permutation.
[[nodiscard]] Expected<std::vector<std::uint32_t>> ccsds_turbo_permutation(
    std::size_t k1, std::size_t k2, std::span<const std::uint32_t, 8> primes);

class TurboCode {
public:
    // Fails on an empty permutation or one that is not a permutation, an
    // empty data pattern, or a pattern entry above 3.
    [[nodiscard]] static Expected<TurboCode> create(TurboCodeSpec spec);

    [[nodiscard]] std::size_t information_bits() const { return spec_.permutation.size(); }
    // Coded bits, data and tail.
    [[nodiscard]] std::size_t encoded_bits() const { return data_bits_ + tail_bits_; }
    [[nodiscard]] std::size_t tail_bits() const { return tail_bits_; }

    // `information` holds information_bits() bits; returns encoded_bits().
    [[nodiscard]] std::vector<std::uint8_t> encode(std::span<const std::uint8_t> information) const;

    struct Result {
        std::vector<std::uint8_t> bits;
        std::size_t iterations = 0;
        // Whether `accept` returned true; always false when none was given.
        bool accepted = false;
    };

    // `llr` holds encoded_bits() values in transmission order. Runs up to
    // `max_iterations` full iterations (both decoders), stopping early when
    // `accept`, given the current hard decision, returns true.
    [[nodiscard]] Result decode(
        std::span<const float> llr, std::size_t max_iterations,
        const std::function<bool(std::span<const std::uint8_t>)>& accept = {}) const;

private:
    TurboCode() = default;

    TurboCodeSpec spec_;
    std::size_t data_bits_ = 0;
    std::size_t tail_bits_ = 0;
};

}  // namespace revenant::decode
