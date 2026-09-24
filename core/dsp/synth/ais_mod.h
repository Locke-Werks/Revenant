// An AIS transmitter, written from the same clauses of ITU-R M.1371-5 as
// core/decode/ais.h.
//
// WHY THIS EXISTS
//
// The reason core/dsp/synth/fsk_mod.h gives: the decoder has no GPU kernel and
// so no bit-exact twin, and a round trip through a transmitter written from
// the same clauses stands in its place. A round trip that fails means one end
// misread the document. One that passes proves the two ends agree, not that
// either agrees with a ship; the message tables are the part of the document
// both ends read the same way, so the tests in tests/decode/test_ais.cpp that
// check fields against values worked out by hand from the tables are the ones
// that can see a misreading shared by both.
//
// WHAT IT PRODUCES
//
// Complex baseband: GMSK bursts with the carrier off between them, as the
// slots of a TDMA channel sound to a receiver. The bursts follow Annex 2
// clause 3.2.2.9's Table 12: 8 bits of ramp, 24 of training sequence, the
// start flag, the data and FCS bit stuffed, the end flag, and the buffer with
// the carrier ramped down and then off, per clause 3.2.2.10's "no modulation
// of the RF after the termination of transmission".

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/decode/ais.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Complex32;
using dsp::SampleRate;

// The message tables of M.1371-5 Annex 8 in reverse: the data portion of a
// Message 1, 2, 3, 4, 5, 11, 18, 19, 21 or 24 from the fields
// decode::ais_parse fills. Fields left empty go out as each table's "not
// available" or default value. Fails on a message ID it does not encode, and
// on text a field cannot carry: a character outside Table 47, or more of them
// than the field holds. Message 21's name may run to 34 characters, the 20 of
// its name field and the 14 of Table 73's extension.
[[nodiscard]] Expected<std::vector<std::uint8_t>> ais_encode(const decode::AisMessage& message);

// Annex 8 Table 47 in reverse: the six-bit value of a character, or nothing
// for one the table does not hold.
[[nodiscard]] std::optional<unsigned> ais_sixbit_value(char c);

// One packet's bits before NRZI, per Annex 2 Table 12: 8 bits of ramp, which
// carry no data and are sent as ones so NRZI holds one level through them,
// the 24-bit training sequence of alternating zeros and ones starting with a
// zero (clause 3.2.2.3), the start flag, the data and its FCS low octet first
// with each octet least significant bit first and bit stuffed (clauses
// 3.2.2.1, 3.2.2.6 and 3.3.7), and the end flag. The buffer after it is not
// bits: the carrier is off.
[[nodiscard]] std::vector<std::uint8_t> ais_packet_bits(std::span<const std::uint8_t> data);

struct AisModConfig {
    SampleRate rate = 48000;
    double amplitude = 1.0;

    // Clause 2.3.1.2: BT 0.4 at most. Clause 2.3.2: index 0.5.
    double bandwidth_time = decode::kAisTransmitBt;
    double deviation_hz = decode::kAisDeviationHz;

    // Fractional error of the transmitter's bit clock; clause 2.4 allows
    // 50 ppm.
    double bit_rate_error = 0.0;

    // The carrier's error from the channel centre; clause 2.3.3 allows
    // 500 Hz.
    double carrier_offset_hz = 0.0;

    // Carrier off before the first packet, between packets and after the
    // last, in bits.
    std::size_t lead_bits = 64;
    std::size_t gap_bits = 64;
    std::size_t tail_bits = 64;
};

// Renders packets' bits, each from ais_packet_bits, as GMSK bursts: NRZI per
// clause 2.6, a change of level for a zero, then each level shaped by the
// Gaussian filter of the configured BT and frequency modulating the carrier
// by plus or minus the deviation. The amplitude rises over the ramp bits and
// falls over 8 bits after the end flag, Table 6's TE to TF, raised-cosine
// both ways.
[[nodiscard]] Expected<std::vector<Complex32>> ais_render_packets(
    const AisModConfig& config, std::span<const std::vector<std::uint8_t>> packet_bits);

// Encodes each message, frames it and renders the lot.
[[nodiscard]] Expected<std::vector<Complex32>> ais_render(
    const AisModConfig& config, std::span<const decode::AisMessage> messages);

}  // namespace revenant::siggen
