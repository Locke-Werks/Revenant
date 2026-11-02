// Fano sequential decoding of a rate 1/2 convolutional code with a long
// constraint length, where a Viterbi trellis of 2^31 states is out of reach.
//
// Source: textbook theory only. Fano 1963; Lin and Costello, "Error Control
// Coding", chapter 13, for the Fano metric and the threshold-walking search.
// The WSPR generator pair a caller would pass is published in G4JNT's "WSPR
// Coding Process"; nothing here is specific to it.
//
// The search follows one path through the code tree at a time, moving forward
// while the path's accumulated Fano metric stays above a running threshold,
// backing up to try the other branch when it does not, and loosening the
// threshold by `delta` when there is nothing left to try above it. Its work is
// unbounded on a bad block, so it stops after a cycle budget and reports
// failure rather than returning a guess.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/decode/dv_codes.h"
#include "core/error.h"

namespace revenant::decode {

struct FanoConfig {
    // Two generators, D^0 in the least significant bit as in dv_codes.h, for
    // a code of constraint length up to 32.
    std::uint32_t generator0 = 0;
    std::uint32_t generator1 = 0;

    // Trailing input bits known to be zero (the flush). The search does not
    // branch on them.
    std::size_t tail_bits = 0;

    // Threshold step, in metric units of 1/kFanoMetricScale bit.
    int delta = 2 * 16;

    // Forward and backward moves allowed per decoded bit before giving up.
    std::size_t max_cycles_per_bit = 10000;
};

inline constexpr int kFanoMetricScale = 16;

struct FanoResult {
    bool decoded = false;
    std::vector<std::uint8_t> bits;  // every input bit, tail included
    std::size_t cycles = 0;
    int metric = 0;  // final path metric, in 1/kFanoMetricScale bit
};

// `llr` holds two log-likelihood ratios per input bit, ln P(0)/P(1), so
// positive leans to zero as in SoftSpan. Branch metrics come from a table of
// the Fano metric log2(p(r|b)/p(r)) - R over quantised LLRs.
//
// A malformed call is an error. A block the search could not finish inside its
// budget is a FanoResult with decoded false, not an error.
[[nodiscard]] Expected<FanoResult> fano_decode(const FanoConfig& config, SoftSpan llr);

}  // namespace revenant::decode
