// Z-Wave (ITU-T G.9959): complex baseband to MAC frames.
//
// SPECIFICATION
//
// Recommendation ITU-T G.9959 (01/2015), "Short range narrow-band digital
// radiocommunication transceivers - PHY, MAC, SAR and LLC layer
// specifications", approved 2015-01-13, from
// https://www.itu.int/rec/T-REC-G.9959-201501-I/en, downloaded 2026-11-02.
// Cited below as "G.9959" with a clause, table or figure. The figures are
// images in the PDF and were read from a rendering of the page.
//
// WHAT IS DECODED
//
//   - the three data rates of Table 7-2 and the modulations of Table 7-4: R1,
//     9.6 kbit/s Manchester FSK; R2, 40 kbit/s NRZ FSK; R3, 100 kbit/s GFSK;
//   - the preamble of 7.1.3.2 and the SOF of Table 7-11, found by one
//     correlation over the last preamble byte and the SOF;
//   - the MPDU of 8.1.3: HomeID, source NodeID, the frame control field of
//     Figures 8-11 and 8-12, length, destination NodeID or the multicast
//     destination fields of Figure 8-14, payload; and the FCS of 8.1.3.8
//     (R1, R2) or the CRC-16 of 8.1.3.9 (R3).
//
// One decoder runs one data rate. A wideband consumer that wants all three
// on a channel runs three, which is what a G.9959 receiver in channel
// configuration 2 does with its two channels anyway (Table 7-3).
//
// DEVIATIONS
//
//   - The MPDU layout is a configuration choice, not inferred. Figures 8-5
//     and 8-6 differ by a sequence number byte, and which applies depends on
//     the channel configuration of the network, which nothing in the frame
//     says. The default is configurations 1 and 2 for every rate.
//   - The R1 end of frame delimiter, 7.1.3.5, is neither required nor
//     checked: the length field already ends the frame.
//   - A frame whose FCS fails is dropped unless keep_bad_frames is set.
//
// CLEAN ROOM
//
// Written from the Recommendation alone. No Z-Wave implementation, rtl_433
// included, was read.
//
// NOTHING HERE IS BIT EXACT AGAINST A GPU TWIN
//
// Host code with no kernel behind it. The round trip in
// tests/decode/test_zwave.cpp against core/dsp/synth/zwave_mod.h stands in its
// place.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::SampleIndex;

// ---------------------------------------------------------------------------
// Physical layer constants
// ---------------------------------------------------------------------------

enum class ZWaveRate : std::uint8_t { R1, R2, R3 };

// G.9959 Table 7-2: bit rates.
inline constexpr double kZWaveBitRateR1 = 9600.0;
inline constexpr double kZWaveBitRateR2 = 40000.0;
inline constexpr double kZWaveBitRateR3 = 100000.0;

// G.9959 Table 7-4: frequency separation, 40, 40 and 58 kHz. The deviation
// either side of centre is half of it, Table 7-5.
inline constexpr double kZWaveSeparationR1Hz = 40000.0;
inline constexpr double kZWaveSeparationR2Hz = 40000.0;
inline constexpr double kZWaveSeparationR3Hz = 58000.0;

// G.9959 Table 7-4: R1 alone is offset 20 kHz from the channel centre.
inline constexpr double kZWaveFrequencyOffsetR1Hz = 20000.0;

// G.9959 7.1.2.4 and Table 7-4: GFSK at R3 with BT = 0.6.
inline constexpr double kZWaveGaussianBt = 0.6;

// G.9959 7.1.3.2: preamble bytes are 0x55, "01010101".
inline constexpr std::uint8_t kZWavePreambleByte = 0x55;

// G.9959 Table 7-11: the SOF, bits 7..0 = 1111 0000.
inline constexpr std::uint8_t kZWaveSof = 0xF0;

// G.9959 Table 7-10: minimum preamble, 10 bytes for R1 and R2 singlecast in
// configurations 1 and 2, 24 bytes for R3 singlecast in configuration 3.
inline constexpr std::size_t kZWavePreambleBytesR1R2 = 10;
inline constexpr std::size_t kZWavePreambleBytesR3 = 24;

// G.9959 Table 7-27: maximum PSDU, 64 bytes at R1 and R2, 170 at R3.
inline constexpr std::size_t kZWaveMaxFrameR1R2 = 64;
inline constexpr std::size_t kZWaveMaxFrameR3 = 170;

// G.9959 8.1.3.8: the FCS starts at 0xFF and XORs every byte from HomeID to
// the end of the payload.
inline constexpr std::uint8_t kZWaveChecksumInitial = 0xFF;

// G.9959 8.1.3.9: CRC-CCITT, x^16 + x^12 + x^5 + 1, initialised to 0x1D0F,
// no bits appended. Nothing is said of reflection or a final XOR, so there is
// neither; bytes go in most significant bit first, as 8.1.3 transmits them.
inline constexpr std::uint16_t kZWaveCrcPolynomial = 0x1021;
inline constexpr std::uint16_t kZWaveCrcInitial = 0x1D0F;

// G.9959 Table 8-14: header types.
inline constexpr std::uint8_t kZWaveHeaderSinglecast = 0x1;
inline constexpr std::uint8_t kZWaveHeaderMulticast = 0x2;
inline constexpr std::uint8_t kZWaveHeaderAck = 0x3;
inline constexpr std::uint8_t kZWaveHeaderRouted = 0x8;

// G.9959 Table 8-15: broadcast destination.
inline constexpr std::uint8_t kZWaveBroadcastNode = 0xFF;

// G.9959 Figure 8-14 and 8.1.3.6.1: up to 29 mask bytes.
inline constexpr std::size_t kZWaveMaxMaskBytes = 29;

enum class ZWaveLayout : std::uint8_t {
    Config12,  // Figure 8-5, 8-7, 8-11
    Config3,   // Figure 8-6, 8-8, 8-12
};

[[nodiscard]] double zwave_bit_rate(ZWaveRate rate);
// Symbols per second on air: Table 7-2, 19.2 kbaud at R1 for Manchester.
[[nodiscard]] double zwave_symbol_rate(ZWaveRate rate);
[[nodiscard]] double zwave_separation_hz(ZWaveRate rate);
// Offset of the tone pair's centre from the channel centre, Table 7-4.
[[nodiscard]] double zwave_frequency_offset_hz(ZWaveRate rate);
[[nodiscard]] std::size_t zwave_max_frame_bytes(ZWaveRate rate);
// R1 and R2 carry a one-byte FCS, R3 a two-byte CRC (8.1.3.8, 8.1.3.9).
[[nodiscard]] std::size_t zwave_fcs_bytes(ZWaveRate rate);

// ---------------------------------------------------------------------------
// The checks
// ---------------------------------------------------------------------------

// G.9959 8.1.3.8, GenerateCheckSum.
[[nodiscard]] std::uint8_t zwave_checksum(std::span<const std::uint8_t> bytes);

// G.9959 8.1.3.9.
[[nodiscard]] std::uint16_t zwave_crc16(std::span<const std::uint8_t> bytes);

// ---------------------------------------------------------------------------
// The MAC frame
// ---------------------------------------------------------------------------

struct ZWaveFrameControl {
    std::uint16_t raw = 0;          // byte 5 high, byte 6 low
    bool routed = false;            // Figure 8-11 byte 5 bit 7 (config 1, 2)
    bool ack_request = false;       // byte 5 bit 6 (1, 2) or bit 7 (3)
    bool low_power = false;         // byte 5 bit 5 (1, 2) or bit 6 (3)
    bool speed_modified = false;    // byte 5 bit 4 (config 1, 2)
    std::uint8_t header_type = 0;   // byte 5 bits 3..0, Table 8-14
    std::uint8_t beaming = 0;       // byte 6 bits 6..5 (1, 2) or 6..4 (3)
    std::uint8_t sequence = 0;      // byte 6 bits 3..0 (config 1, 2)
};

[[nodiscard]] ZWaveFrameControl zwave_parse_frame_control(std::uint16_t raw, ZWaveLayout layout);

// G.9959 8.1.3.6.1: the NodeIDs a multicast mask addresses, by
// Dst NodeID = bit address offset + mask byte number * 8 + mask bit number + 1,
// mask bit 0 being the least significant (the 0xC5 example).
[[nodiscard]] std::vector<std::uint8_t> zwave_multicast_nodes(std::uint8_t address_offset,
                                                              std::span<const std::uint8_t> mask);

struct ZWaveMpdu {
    std::uint32_t home_id = 0;
    std::uint8_t source = 0;
    ZWaveFrameControl frame_control;
    std::uint8_t length = 0;            // the whole MPDU, 8.1.3.4
    std::uint8_t sequence_number = 0;   // configuration 3 only, 8.1.3.5
    std::uint8_t destination = 0;       // singlecast, ACK, routed
    // Multicast, Figure 8-14.
    std::uint8_t multicast_offset = 0;  // bits 7..5
    std::vector<std::uint8_t> multicast_mask;
    std::vector<std::uint8_t> multicast_nodes;
    std::vector<std::uint8_t> payload;
    std::uint16_t fcs = 0;
    bool fcs_valid = false;
};

// Parses a complete MPDU, FCS included. Fails when the bytes are too short
// for the header, or the length field disagrees with them.
[[nodiscard]] Expected<ZWaveMpdu> zwave_parse_mpdu(std::span<const std::uint8_t> bytes,
                                                   ZWaveRate rate, ZWaveLayout layout);

// What a transmitter puts on air: everything in the MPDU but length and FCS,
// which zwave_build_mpdu fills.
struct ZWaveMpduFields {
    std::uint32_t home_id = 0;
    std::uint8_t source = 0;
    std::uint16_t frame_control = 0;
    std::uint8_t sequence_number = 0;  // configuration 3 only
    // Destination NodeID, or for a multicast the control byte then the mask.
    std::vector<std::uint8_t> destination{0x01};
    std::vector<std::uint8_t> payload;
};

[[nodiscard]] Expected<std::vector<std::uint8_t>> zwave_build_mpdu(const ZWaveMpduFields& fields,
                                                                   ZWaveRate rate,
                                                                   ZWaveLayout layout);

// ---------------------------------------------------------------------------
// The receiver
// ---------------------------------------------------------------------------

struct ZWaveConfig {
    dsp::SampleRate sample_rate = 2'400'000;
    ZWaveRate rate = ZWaveRate::R2;
    ZWaveLayout layout = ZWaveLayout::Config12;
    // Where the channel centre sits in the input, relative to its centre.
    double channel_offset_hz = 0.0;
    // Normalised correlation the preamble tail and SOF must reach.
    float sync_threshold = 0.75F;
    bool keep_bad_frames = false;
};

struct ZWaveFrame {
    ZWaveRate rate = ZWaveRate::R2;
    // Input sample index of the end of the SOF.
    SampleIndex sof_end_sample = 0;
    float sync_correlation = 0.0F;
    // Measured carrier error against channel_offset_hz.
    double frequency_error_hz = 0.0;
    std::vector<std::uint8_t> bytes;  // the MPDU as received
    ZWaveMpdu mpdu;
};

class ZWave {
   public:
    [[nodiscard]] static Expected<ZWave> create(const ZWaveConfig& config);

    // Any chunk size; the output for a stream does not depend on how it was
    // split.
    [[nodiscard]] Status process(dsp::ConstComplexSpan samples, std::vector<ZWaveFrame>& out);

    void reset();

    [[nodiscard]] const ZWaveConfig& config() const { return config_; }

   private:
    ZWave() = default;

    void run(std::vector<ZWaveFrame>& out);
    [[nodiscard]] float sample_at(double position) const;
    [[nodiscard]] int bit_at(std::size_t bit_index, double origin, float dc) const;

    ZWaveConfig config_;
    std::size_t decimation_ = 1;
    double output_rate_ = 0.0;
    double samples_per_symbol_ = 0.0;  // after decimation, chips for R1
    std::size_t average_length_ = 1;
    std::vector<float> taps_;
    std::vector<float> pattern_;
    double mix_step_ = 0.0;            // radians per input sample
    double discriminator_scale_ = 0.0; // radians to hertz

    // Input-rate state.
    SampleIndex input_index_ = 0;
    double mix_phase_ = 0.0;
    std::vector<std::complex<float>> history_;  // last taps_.size() mixed samples
    std::size_t history_pos_ = 0;
    std::size_t decimation_phase_ = 0;

    // Decimated-rate state, indexed by absolute decimated index.
    std::complex<float> previous_{1.0F, 0.0F};
    std::vector<float> raw_;       // discriminator output, from base_
    std::vector<float> averaged_;  // moving average, same indexing
    SampleIndex base_ = 0;
    double running_sum_ = 0.0;
    SampleIndex search_ = 0;
};

}  // namespace revenant::decode
