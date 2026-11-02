// ERMES transmitter for the round trip in tests/decode/test_ermes.cpp.
//
// SPECIFICATION
//
// ETSI ETS 300 133-4, July 1992, the same clauses core/decode/ermes.h cites:
// the batch of 5.4, the formats of clause 6, the code of clause 7, the
// interleaving of clause 8 and 4-PAM/FM of 9.3.
//
// One deviation. 9.3.3 specifies the premodulation filter as amplitude and
// group delay masks around a 10th-order Bessel low pass with its 3 dB point
// at 3.9 kHz. This file uses a linear-phase FIR with a Gaussian magnitude
// that has the same 3 dB point, which a Bessel response approximates and
// which sits inside both masks; the group delay is flat, as the mask allows.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/decode/ermes.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Complex32;
using dsp::SampleRate;

struct ErmesModConfig {
    SampleRate rate = 48000;
    double amplitude = 1.0;
};

struct ErmesMessageSpec {
    std::uint32_t local_address = 0;  // 22 bits
    std::uint8_t message_number = 1;
    decode::ErmesCategory category = decode::ErmesCategory::Alphanumeric;
    bool urgent = false;
    std::uint8_t alert = 0;

    // Set for an external receiver, 6.5.2: the 13-bit home operator identity.
    std::optional<std::uint16_t> operator_identity;

    // Set to send as a long sub-message, AII = 1 and AIT = 001, 11.4.
    std::optional<std::uint16_t> link_counter;

    std::string text;                // numeric or alphanumeric
    std::vector<std::uint8_t> data;  // transparent, one bit per byte
};

struct ErmesBatchSpec {
    decode::ErmesSystemInfo system;
    std::uint8_t ssit = 0;
    std::uint16_t ssif = 0;
    std::vector<std::uint32_t> initial_addresses;  // sent in the order given
    std::vector<ErmesMessageSpec> messages;

    // Fill the batch to its full 154 or 190 codewords with delimiter
    // codeblocks, 6.5.6, or stop after the last needed codeblock, 6.5.5.
    bool fill = true;
};

// 6.5: the information words of one message, header to the last data word,
// without the delimiters around it.
[[nodiscard]] Expected<std::vector<std::uint32_t>> ermes_message_words(const ErmesMessageSpec& m);

// The batch's codewords in logical (not interleaved) order, 30 bits each,
// preamble and sync first.
[[nodiscard]] Expected<std::vector<std::uint32_t>> ermes_batch_codewords(const ErmesBatchSpec& b);

// The batch as 4-PAM levels in transmit order, codeblocks interleaved.
[[nodiscard]] Expected<std::vector<int>> ermes_batch_levels(const ErmesBatchSpec& b);

[[nodiscard]] Expected<std::vector<Complex32>> ermes_render_levels(const ErmesModConfig& config,
                                                                   std::span<const int> levels);

[[nodiscard]] Expected<std::vector<Complex32>> ermes_render_batches(
    const ErmesModConfig& config, std::span<const ErmesBatchSpec> batches);

}  // namespace revenant::siggen
