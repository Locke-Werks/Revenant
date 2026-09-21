#include "core/decode/dv_codes.h"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <limits>

namespace revenant::decode {
namespace {

// The NID's 47 parity bits: the remainder of the 16 information bits shifted
// up by 47 places, divided by the generator. TIA-102.BAAA-A clause 8.5.2.
std::uint64_t p25_nid_parity47(std::uint16_t information) {
    std::uint64_t reg = static_cast<std::uint64_t>(information) << 47U;
    for (int shift = 62; shift >= 47; --shift) {
        if ((reg >> static_cast<unsigned>(shift)) & 1ULL) {
            reg ^= kP25NidGenerator << static_cast<unsigned>(shift - 47);
        }
    }
    return reg & ((1ULL << 47U) - 1ULL);
}

std::uint64_t p25_nid_codeword(std::uint16_t information) {
    const std::uint64_t word63 =
        (static_cast<std::uint64_t>(information) << 47U) | p25_nid_parity47(information);
    return (word63 << 1U) | p25_nid_trailing_parity(information);
}

}  // namespace

// ---------------------------------------------------------------------------
// P25 Phase 1: the Network Identifier BCH code
// ---------------------------------------------------------------------------

std::uint8_t p25_nid_trailing_parity(std::uint16_t nid_information) {
    return static_cast<std::uint8_t>(((nid_information >> 1U) ^ nid_information) & 1U);
}

Status p25_nid_encode(std::uint16_t nac, std::uint8_t duid, BitSpan out) {
    if (out.size() != 64) {
        return fail(std::format("the P25 NID code word is 64 bits; got a span of {}", out.size()));
    }
    if (nac > 0x0FFFU) {
        return fail(std::format("the P25 Network Access Code is 12 bits; got {:#x}", nac));
    }
    if (duid > 0x0FU) {
        return fail(std::format("the P25 Data Unit ID is 4 bits; got {:#x}", duid));
    }

    // Table 8-3: NAC most significant bit first, then the DUID.
    const auto information = static_cast<std::uint16_t>((nac << 4U) | duid);
    const std::uint64_t word = p25_nid_codeword(information);
    for (std::size_t i = 0; i < 64; ++i) {
        out[i] = static_cast<std::uint8_t>((word >> (63U - i)) & 1ULL);
    }
    return {};
}

Expected<P25NidDecode> p25_nid_decode(ConstBitSpan word) {
    if (word.size() != 64) {
        return fail(
            std::format("the P25 NID code word is 64 bits; got a span of {}", word.size()));
    }

    std::uint64_t received = 0;
    for (const std::uint8_t bit : word) {
        received = (received << 1U) | static_cast<std::uint64_t>(bit & 1U);
    }

    std::uint32_t best_distance = std::numeric_limits<std::uint32_t>::max();
    std::uint16_t best_information = 0;
    for (std::uint32_t candidate = 0; candidate < 0x1'0000U; ++candidate) {
        const auto information = static_cast<std::uint16_t>(candidate);
        const std::uint64_t distance =
            static_cast<std::uint64_t>(std::popcount(received ^ p25_nid_codeword(information)));
        if (distance < best_distance) {
            best_distance = static_cast<std::uint32_t>(distance);
            best_information = information;
            if (best_distance == 0) {
                break;
            }
        }
    }

    P25NidDecode out;
    out.nac = static_cast<std::uint16_t>((best_information >> 4U) & 0x0FFFU);
    out.duid = static_cast<std::uint8_t>(best_information & 0x0FU);
    out.corrected_bits = best_distance;
    return out;
}

// ---------------------------------------------------------------------------
// P25 Phase 1: the shortened Golay code
// ---------------------------------------------------------------------------

std::uint32_t p25_golay18_encode(std::uint8_t information) {
    std::uint32_t word = 0;
    for (unsigned row = 0; row < 6U; ++row) {
        // Row 1 of Table 5-3 is the most significant information bit.
        if ((information >> (5U - row)) & 1U) {
            word ^= kP25Golay18Rows[row];
        }
    }
    return word & 0x3FFFFU;
}

Golay18Decode p25_golay18_decode(std::uint32_t word) {
    const std::uint32_t received = word & 0x3FFFFU;
    Golay18Decode out;
    std::uint32_t best_distance = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t candidate = 0; candidate < 64U; ++candidate) {
        const auto information = static_cast<std::uint8_t>(candidate);
        const auto distance =
            static_cast<std::uint32_t>(std::popcount(received ^ p25_golay18_encode(information)));
        if (distance < best_distance) {
            best_distance = distance;
            out.information = information;
        }
    }
    out.corrected_bits = best_distance;
    return out;
}

// ---------------------------------------------------------------------------
// Convolutional codes
// ---------------------------------------------------------------------------

Expected<std::vector<std::uint8_t>> convolutional_encode(const ConvolutionalCode& code,
                                                         ConstBitSpan input) {
    if (code.generators.empty()) {
        return fail("convolutional_encode was given no generator polynomials");
    }
    if (code.memory == 0 || code.memory > 16) {
        return fail(std::format("convolutional_encode needs a memory in 1..16; got {}",
                                code.memory));
    }

    std::vector<std::uint8_t> out;
    out.reserve(input.size() * code.generators.size());
    std::uint32_t reg = 0;
    for (const std::uint8_t bit : input) {
        // The register holds the current bit in position 0 and the history
        // above it, so a generator's bit j multiplies the input delayed by j,
        // which is how both standards write their polynomials in D.
        reg = ((reg << 1U) | static_cast<std::uint32_t>(bit & 1U)) &
              ((1U << (code.memory + 1U)) - 1U);
        for (const std::uint32_t generator : code.generators) {
            out.push_back(static_cast<std::uint8_t>(std::popcount(reg & generator) & 1U));
        }
    }
    return out;
}

Expected<std::vector<std::uint8_t>> viterbi_decode(const ConvolutionalCode& code, SoftSpan soft,
                                                   bool terminated) {
    if (code.generators.empty()) {
        return fail("viterbi_decode was given no generator polynomials");
    }
    if (code.memory == 0 || code.memory > 8) {
        return fail(std::format(
            "viterbi_decode needs a memory in 1..8; got {}. Above eight the trellis is "
            "wider than anything in this tree needs and the allocation below would be a "
            "surprise rather than a decode",
            code.memory));
    }
    const std::size_t outputs = code.generators.size();
    if (soft.size() % outputs != 0) {
        return fail(std::format(
            "viterbi_decode got {} soft bits, which is not a whole number of {}-bit "
            "encoder outputs",
            soft.size(), outputs));
    }

    const std::size_t steps = soft.size() / outputs;
    const std::size_t states = std::size_t{1} << code.memory;

    // Branch outputs, precomputed: for each state and each input bit, the bits
    // the encoder emits and the state it moves to.
    std::vector<std::uint32_t> branch_bits(states * 2);
    std::vector<std::uint32_t> next_state(states * 2);
    for (std::size_t state = 0; state < states; ++state) {
        for (std::uint32_t bit = 0; bit < 2U; ++bit) {
            const std::uint32_t reg =
                ((static_cast<std::uint32_t>(state) << 1U) | bit) & ((1U << (code.memory + 1U)) - 1U);
            std::uint32_t emitted = 0;
            for (std::size_t g = 0; g < outputs; ++g) {
                emitted |= static_cast<std::uint32_t>(std::popcount(reg & code.generators[g]) & 1U)
                           << g;
            }
            branch_bits[state * 2 + bit] = emitted;
            next_state[state * 2 + bit] =
                reg & ((1U << code.memory) - 1U);
        }
    }

    constexpr float kUnreachable = 1e18F;
    std::vector<float> metric(states, kUnreachable);
    std::vector<float> next_metric(states, kUnreachable);
    metric[0] = 0.0F;

    // Traceback needs both the decoded bit and where it came from. The source
    // state is not recoverable from the destination and the bit alone: the
    // shift throws away the predecessor's top bit, which is exactly the bit
    // the trellis branched on.
    std::vector<std::uint8_t> decided(steps * states, 0);
    std::vector<std::uint16_t> source(steps * states, 0);

    for (std::size_t step = 0; step < steps; ++step) {
        std::fill(next_metric.begin(), next_metric.end(), kUnreachable);
        for (std::size_t state = 0; state < states; ++state) {
            if (metric[state] >= kUnreachable) {
                continue;
            }
            for (std::uint32_t bit = 0; bit < 2U; ++bit) {
                const std::uint32_t emitted = branch_bits[state * 2 + bit];
                // Correlation metric against the soft values, in the sign
                // convention of SoftSpan: an emitted 0 wants a positive value,
                // so an agreeing branch lowers the cost.
                float cost = 0.0F;
                for (std::size_t g = 0; g < outputs; ++g) {
                    const float value = soft[step * outputs + g];
                    cost += ((emitted >> g) & 1U) ? value : -value;
                }
                const std::size_t target = next_state[state * 2 + bit];
                const float candidate = metric[state] + cost;
                if (candidate < next_metric[target]) {
                    next_metric[target] = candidate;
                    decided[step * states + target] = static_cast<std::uint8_t>(bit);
                    source[step * states + target] = static_cast<std::uint16_t>(state);
                }
            }
        }
        metric.swap(next_metric);
    }

    std::size_t state = 0;
    if (!terminated) {
        state = static_cast<std::size_t>(
            std::distance(metric.begin(), std::min_element(metric.begin(), metric.end())));
    }

    std::vector<std::uint8_t> out(steps, 0);
    for (std::size_t i = steps; i-- > 0;) {
        out[i] = decided[i * states + state];
        state = source[i * states + state];
    }
    return out;
}

// ---------------------------------------------------------------------------
// D-STAR
// ---------------------------------------------------------------------------

std::vector<std::uint16_t> dstar_interleave_map() {
    constexpr std::size_t kRows = 24;
    constexpr std::size_t kColumns = 28;

    std::vector<std::uint16_t> map;
    map.reserve(kDStarHeaderEncodedBits);
    for (std::size_t row = 0; row < kRows; ++row) {
        for (std::size_t column = 0; column < kColumns; ++column) {
            const std::size_t index = column * kRows + row;
            if (index < kDStarHeaderEncodedBits) {
                map.push_back(static_cast<std::uint16_t>(index));
            }
        }
    }
    return map;
}

void dstar_scramble(BitSpan bits) {
    // Ap1.1: S(x) = x^7 + x^4 + 1, all-ones initial state. Taps at 7 and 4
    // means the new bit is the exclusive or of the bits seven and four places
    // back, which is the standard Fibonacci form of that polynomial.
    std::uint8_t reg = 0x7FU;
    for (std::uint8_t& bit : bits) {
        const std::uint8_t feedback =
            static_cast<std::uint8_t>(((reg >> 6U) ^ (reg >> 3U)) & 1U);
        bit = static_cast<std::uint8_t>((bit ^ feedback) & 1U);
        reg = static_cast<std::uint8_t>(((reg << 1U) | feedback) & 0x7FU);
    }
}

std::uint16_t dstar_header_fcs(std::span<const std::uint8_t> bytes) {
    // Clause 4.1.1 k names CRC-CCITT and G(x) = x^16 + x^12 + x^5 + 1.
    // Ap1.4 says every code's bits go out least significant first, so the
    // register is run in the reflected form, which is the same polynomial
    // read the other way round: 0x8408.
    std::uint16_t crc = 0xFFFFU;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 1U) ? static_cast<std::uint16_t>((crc >> 1U) ^ 0x8408U)
                             : static_cast<std::uint16_t>(crc >> 1U);
        }
    }
    return static_cast<std::uint16_t>(~crc);
}

// ---------------------------------------------------------------------------
// TETRA
// ---------------------------------------------------------------------------

std::vector<std::uint32_t> tetra_rate_two_thirds_map(std::size_t punctured_bits) {
    // Clause 8.2.3.1.3: t = 3, P(1) = 1, P(2) = 2, P(3) = 5, i = j.
    static constexpr std::uint32_t kCoefficients[3] = {1U, 2U, 5U};
    std::vector<std::uint32_t> map;
    map.reserve(punctured_bits);
    for (std::size_t j = 1; j <= punctured_bits; ++j) {
        const std::size_t group = (j - 1) / 3;
        const std::size_t within = (j - 1) % 3;
        // The clause is one-based in V; this tree indexes from zero.
        map.push_back(static_cast<std::uint32_t>(8 * group + kCoefficients[within] - 1));
    }
    return map;
}

Expected<std::vector<std::uint32_t>> tetra_block_interleave_map(std::size_t k, std::size_t a) {
    if (k == 0) {
        return fail("tetra_block_interleave_map needs a positive block length");
    }
    if (a == 0) {
        return fail("tetra_block_interleave_map needs a positive interleave step");
    }

    // Clause 8.2.4.1: b4(k) = b3(i) with k = 1 + ((a * i) mod K), one-based.
    std::vector<std::uint32_t> map(k, 0);
    std::vector<std::uint8_t> filled(k, 0);
    for (std::size_t i = 1; i <= k; ++i) {
        const std::size_t position = 1 + ((a * i) % k);
        if (position > k || filled[position - 1] != 0) {
            return fail(std::format(
                "a ({}) and K ({}) are not coprime, so the clause 8.2.4.1 interleaver is "
                "not a permutation and position {} was written twice",
                a, k, position));
        }
        filled[position - 1] = 1;
        map[position - 1] = static_cast<std::uint32_t>(i - 1);
    }
    return map;
}

Expected<std::vector<std::uint8_t>> tetra_scrambling_sequence(ConstBitSpan extended_colour_code,
                                                              std::size_t length) {
    if (extended_colour_code.size() != 30) {
        return fail(std::format(
            "the TETRA extended colour code is 30 bits per clause 8.2.5.2; got {}",
            extended_colour_code.size()));
    }

    // Clause 8.2.5.2, equation 8.40. c_i = 1 for these i and zero elsewhere.
    static constexpr int kTaps[] = {1, 2, 4, 5, 7, 8, 10, 11, 12, 16, 22, 23, 26, 32};

    // History indexed so that history[j] is p(k - 1 - j) as k advances, which
    // keeps the recursion below reading the way the equation does.
    std::array<std::uint8_t, 32> history{};
    // Initialisation, equation 8.42: p(k) = e(1-k) for k = -29..0, and
    // p(k) = 1 for k = -31 and -30. At k = 1 the recursion reads p(0) through
    // p(-31), so history[j] = p(-j).
    for (std::size_t j = 0; j < 30; ++j) {
        history[j] = static_cast<std::uint8_t>(extended_colour_code[j] & 1U);
    }
    history[30] = 1;
    history[31] = 1;

    std::vector<std::uint8_t> out;
    out.reserve(length);
    for (std::size_t n = 0; n < length; ++n) {
        std::uint8_t bit = 0;
        for (const int tap : kTaps) {
            bit ^= history[static_cast<std::size_t>(tap - 1)];
        }
        bit &= 1U;
        out.push_back(bit);
        for (std::size_t j = 31; j > 0; --j) {
            history[j] = history[j - 1];
        }
        history[0] = bit;
    }
    return out;
}

std::vector<std::uint8_t> tetra_block_code_parity(ConstBitSpan information) {
    // Clause 8.2.3.3 is the ITU-T X.25 frame check sequence it cites: preset
    // the register to all ones, feed the information bits most significant
    // first, and complement the result.
    std::uint16_t crc = 0xFFFFU;
    for (const std::uint8_t bit : information) {
        const std::uint16_t top = static_cast<std::uint16_t>((crc >> 15U) & 1U);
        crc = static_cast<std::uint16_t>(crc << 1U);
        if ((top ^ (bit & 1U)) != 0U) {
            crc ^= 0x1021U;
        }
    }
    crc = static_cast<std::uint16_t>(~crc);

    // b2(k) = f(K1 + 16 - k) for k = K1+1..K1+16, so the first check bit sent
    // is f(15), which is the most significant coefficient.
    std::vector<std::uint8_t> out(16, 0);
    for (std::size_t i = 0; i < 16; ++i) {
        out[i] = static_cast<std::uint8_t>((crc >> (15U - i)) & 1U);
    }
    return out;
}

bool tetra_block_code_verify(ConstBitSpan word) {
    if (word.size() < 17) {
        return false;
    }
    const std::size_t information_bits = word.size() - 16;
    const std::vector<std::uint8_t> expected =
        tetra_block_code_parity(word.subspan(0, information_bits));
    for (std::size_t i = 0; i < 16; ++i) {
        if ((word[information_bits + i] & 1U) != (expected[i] & 1U)) {
            return false;
        }
    }
    return true;
}

}  // namespace revenant::decode
