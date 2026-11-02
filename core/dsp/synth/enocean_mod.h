// EnOcean ERP1 synthetic transmitter, written from the same document as
// core/decode/enocean.h: EnOcean Radio Protocol 1 V1.2 (November 9, 2020).
// See that header for the clauses. Start of frame variant 2 only.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

struct EnoceanTxConfig {
    dsp::SampleRate sample_rate = 1'000'000;
    // ERP1 chapter 3: 125 kbit/s; clause 1.6 lets a receiver see 5 % either way.
    double bit_rate = 125000.0;
    // Clause 1.5: modulation depth 20 to 36 dB.
    double depth_db = 26.0;
    double frequency_offset_hz = 0.0;
    double amplitude = 1.0;
    // Clause 1.11: an emission no stronger than a "1" before the preamble.
    std::size_t pre_preamble_bits = 8;
};

// Figure 1 layout with HASH appended: Table 3 picks the CRC when STATUS bit 7
// is set, the checksum otherwise.
[[nodiscard]] std::vector<std::uint8_t> enocean_subtelegram(std::uint8_t rorg,
                                                            std::span<const std::uint8_t> data,
                                                            std::uint32_t sender_id,
                                                            std::uint8_t status);

// Logical bits of one frame, preamble through EOF, first transmitted first.
[[nodiscard]] std::vector<std::uint8_t> enocean_frame_bits(std::span<const std::uint8_t> subtelegram);

// One subtelegram as complex baseband: pre-preamble emission, then the frame.
[[nodiscard]] Expected<std::vector<dsp::Complex32>> enocean_modulate(
    std::span<const std::uint8_t> subtelegram, const EnoceanTxConfig& config);

}  // namespace revenant::siggen
