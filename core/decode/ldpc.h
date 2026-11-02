// Generic binary LDPC codes: a min-sum belief propagation decoder over a
// parity-check matrix the caller supplies, and a systematic encoder over a
// generator matrix the caller supplies.
//
// No mode's matrix lives here. A mode that uses an LDPC code builds its
// matrix from its own specification, citing that document, and hands it in.
//
// THE ALGORITHM
//
// Textbook, from no listing. The code is the null space of a sparse parity
// check matrix H (R. G. Gallager, "Low-Density Parity-Check Codes", MIT
// Press, 1963). Decoding is flooding-schedule message passing on the Tanner
// graph with the min-sum approximation to the check-node update (M. P. C.
// Fossorier, M. Mihaljevic and H. Imai, IEEE Trans. Commun., May 1999), and
// its two standard corrections for the overestimate min-sum makes: scaling
// the check message by a factor below one (normalised min-sum) or subtracting
// a constant from its magnitude and clamping at zero (offset min-sum), both
// from J. Chen and M. P. C. Fossorier, IEEE Trans. Commun., August 2002.
//
// After every iteration, and once before the first, the hard decision is
// checked against every parity check. A zero syndrome stops the decoder: the
// hard decision is then a code word, though not necessarily the one sent.
//
// CONVENTIONS
//
// A log-likelihood ratio is log(P(bit = 0) / P(bit = 1)), so a positive value
// favours zero. BPSK mapping bit b to 1 - 2b gives LLR = 2y / sigma^2.
// Bits in and out are one per byte, 0 or 1.
//
// CLEAN ROOM
//
// No LDPC implementation was read.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/error.h"

namespace revenant::decode {

enum class LdpcCheckRule {
    NormalisedMinSum,
    OffsetMinSum,
};

struct LdpcConfig {
    // Code length: the number of variable nodes, columns of H.
    std::size_t variables = 0;
    // One entry per parity check, row of H: the variables it constrains.
    // Each row needs at least two distinct indices below `variables`.
    std::vector<std::vector<std::uint32_t>> checks;
    std::size_t max_iterations = 50;
    LdpcCheckRule rule = LdpcCheckRule::NormalisedMinSum;
    // Normalised min-sum multiplies check messages by this, in (0, 1].
    float scale = 0.75F;
    // Offset min-sum subtracts this from check message magnitudes, >= 0.
    float offset = 0.5F;
};

struct LdpcResult {
    // Iterations run. Zero when the channel's hard decision already had a
    // zero syndrome.
    std::size_t iterations = 0;
    // Whether the output satisfies every parity check.
    bool converged = false;
};

class LdpcDecoder {
public:
    // Fails on an empty code, a check naming a variable out of range or the
    // same variable twice, a check of fewer than two variables, zero
    // iterations, or a correction factor out of range.
    [[nodiscard]] static Expected<LdpcDecoder> create(LdpcConfig config);

    [[nodiscard]] std::size_t variables() const { return config_.variables; }
    [[nodiscard]] std::size_t checks() const { return config_.checks.size(); }
    [[nodiscard]] const LdpcConfig& config() const { return config_; }

    // `llr` and `bits` each hold variables() entries. Not const: the
    // message buffers are reused between calls, so one decoder serves one
    // thread at a time.
    LdpcResult decode(std::span<const float> llr, std::span<std::uint8_t> bits);

    // Whether `bits` satisfies every parity check.
    [[nodiscard]] bool syndrome_zero(std::span<const std::uint8_t> bits) const;

private:
    LdpcDecoder() = default;

    LdpcConfig config_;
    // Edges in check-major order: check c owns [check_start_[c],
    // check_start_[c + 1]) and edge_variable_ names each edge's variable.
    std::vector<std::size_t> check_start_;
    std::vector<std::uint32_t> edge_variable_;
    // Messages, one of each per edge.
    std::vector<float> to_check_;
    std::vector<float> to_variable_;
    std::vector<float> total_;
};

class LdpcEncoder {
public:
    // `generator` holds k rows of n bits, one per byte, whose first k
    // columns are the identity, so a code word is the information bits
    // followed by n - k parity bits. Fails on no rows, rows of unequal or
    // too short a length, a non-binary entry, or a non-identity prefix.
    [[nodiscard]] static Expected<LdpcEncoder> create(
        const std::vector<std::vector<std::uint8_t>>& generator);

    [[nodiscard]] std::size_t information_bits() const { return k_; }
    [[nodiscard]] std::size_t length() const { return n_; }

    // `information` holds k bits and `codeword` n bits.
    void encode(std::span<const std::uint8_t> information,
                std::span<std::uint8_t> codeword) const;

private:
    LdpcEncoder() = default;

    std::size_t k_ = 0;
    std::size_t n_ = 0;
    std::size_t words_per_row_ = 0;
    // Parity columns of each row, packed 64 to a word.
    std::vector<std::uint64_t> parity_;
};

}  // namespace revenant::decode
