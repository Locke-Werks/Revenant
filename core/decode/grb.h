// GOES-R Rebroadcast (GRB): complex baseband of one polarisation to CCSDS
// AOS frames and Space Packets.
//
// SPECIFICATION
//
// GOES-R Series Product Definition and Users' Guide, Volume 4: GOES
// Rebroadcast (GRB), 416-R-PUG-GRB-0348 Vol 4 Revision 2.3 (cover page DCN
// 7035538 Revision J.1, 01 June 2020), free from NOAA OSPO, fetched
// 2026-11-02 from www.ospo.noaa.gov/resources/documents/PUG/. Cited as "PUG".
// The DVB-S2 layer is ETSI EN 302 307-1 V1.4.1, implemented in
// core/decode/dvbs2.h; the AOS and packet layers are CCSDS 732.0 and 133.0
// through core/decode/ccsds.h.
//
//   - PUG 4.3: single continuous generic stream, CCM, normal 64 800-bit
//     FECFRAMEs; QPSK 9/10 at 8.665938 Msym/s (the default since GOES-16's
//     checkout) or 8PSK 2/3 at 7.825768 Msym/s; roll-off 0.25.
//   - PUG 4.3.1 and table 4.3.1.1: MODCOD 0x0B or 0x0D, TYPE 0 (normal, no
//     pilots). This receiver reads TYPE off each header and handles pilots
//     anyway, since it costs nothing.
//   - PUG 4.3.2 and table 4.3.2.1: BBHEADER MATYPE generic continuous,
//     single stream, CCM, roll-off 0.25; UPL 0; DFL up to 58 112 bits;
//     zero padding after a short data field.
//   - PUG 4.4.1 figure 4.4.1: CADUs of 2048 octets, the marker 0x1ACFFC1D
//     and a 2044-octet AOS Transfer Frame: six-octet primary header, a
//     2036-octet data field (M_PDU header and packet zone) and a two-octet
//     Frame Error Control Field. CADUs run on through the data fields of
//     successive BBFRAMEs with no alignment to them.
//   - PUG table 4.4.2.1: virtual channel 5 is RHCP, 6 LHCP, 63 idle.
//
// DEVIATIONS AND OPEN POINTS
//
//   - The PUG does not say the CADU is randomised or Reed-Solomon coded and
//     its figure gives no room for check symbols, so neither is applied. The
//     FECF is checked with ccsds_fecf_ok.
//   - The PUG text gives the Transfer Frame Version Number as 0b00, while
//     calling the frame a CCSDS Version 2 frame, which 732.0 4.1.2.2.2
//     numbers 0b01. A frame with either is accepted here; the transmitter in
//     core/dsp/synth/grb_mod.h writes 0b01.
//   - The physical layer scrambling code is not stated. EN 302 307 5.5.4
//     says n = 0 is the default for broadcasting, so that is the default
//     here, and it is configurable.
//   - The demodulator's carrier phase ambiguity is resolved by the
//     PLHEADER correlation. Spectral inversion is not searched for.
//   - Measured on synthetic signals at 20 Msps and Es/N0 10 dB: QPSK
//     acquires a 250 kHz carrier offset, 8PSK acquires 200 kHz but at
//     250 kHz the eighth-power estimate came out at -250 kHz and nothing
//     locked. A geostationary downlink has next to no Doppler, so the
//     offset is the receiver's own; keep it inside 200 kHz for 8PSK.
//
// THE SIGNAL CANNOT COME FROM A DONGLE
//
// At 8.67 Msym/s and roll-off 0.25 one polarisation occupies about 10.8 MHz,
// beyond what an RTL-SDR samples. This is written and tested against
// synthetic baseband only; a capture at 20 Msps or more from a wideband
// receiver is what it would need.
//
// CLEAN ROOM
//
// Written from the PUG, EN 302 307 and the CCSDS books. No GRB or DVB-S2
// receiver was read: not SatDump, goesrecv, leandvb, gr-dvbs2rx or any other.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "core/decode/ccsds.h"
#include "core/decode/dvbs2.h"
#include "core/decode/psk_demod.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// PUG 4.3.
inline constexpr double kGrbQpskSymbolRate = 8.665938e6;
inline constexpr double kGrb8pskSymbolRate = 7.825768e6;
inline constexpr double kGrbRollOff = 0.25;
// PUG figure 4.4.1.
inline constexpr std::size_t kGrbCaduOctets = 2048;
inline constexpr std::size_t kGrbFrameOctets = 2044;
inline constexpr std::uint32_t kGrbMarker = 0x1ACFFC1D;
// PUG table 4.4.2.1.
inline constexpr std::uint8_t kGrbVcRhcp = 5;
inline constexpr std::uint8_t kGrbVcLhcp = 6;
// PUG table 4.3.2.1: generic continuous, single stream, CCM, no ISSY, no
// NPD, roll-off 0.25.
inline constexpr std::uint8_t kGrbMatype1 = 0b01110001;

enum class GrbModulation : std::uint8_t { Qpsk910, Psk8Rate23 };

struct GrbConfig {
    double sample_rate = 20.0e6;
    GrbModulation modulation = GrbModulation::Qpsk910;
    // Zero means the PUG rate for the modulation.
    double symbol_rate = 0.0;
    double roll_off = kGrbRollOff;
    std::uint32_t gold_code = 0;
    std::size_t ldpc_iterations = 50;
    // Passed to the demodulator.
    double carrier_bandwidth = 0.002;
    double timing_bandwidth = 0.002;
    std::size_t acquisition_symbols = 2048;
};

struct GrbFrame {
    AosFrameHeader header;
    // The 2044-octet Transfer Frame, FECF included.
    std::vector<std::uint8_t> frame;
    bool continuous = false;
};

struct GrbEvents {
    std::function<void(const GrbFrame&)> on_frame;
    // Idle packets (APID 0x7FF) are not delivered.
    std::function<void(std::uint8_t virtual_channel, const SpacePacket&)> on_packet;
};

struct GrbStats {
    std::size_t plframes = 0;         // headers decoded, any MODCOD
    std::size_t dummy_plframes = 0;
    std::size_t skipped_plframes = 0;  // a MODCOD or TYPE not implemented
    std::size_t fec_failures = 0;      // BCH refused after LDPC
    std::size_t ldpc_iterations = 0;   // summed
    std::size_t bch_corrected = 0;
    std::size_t bbheader_failures = 0;  // CRC-8 or DFL out of range
    std::size_t relocks = 0;
    std::size_t frames = 0;  // CADUs whose FECF matched
    std::size_t fecf_failures = 0;
    std::size_t idle_frames = 0;
    std::size_t packets = 0;
    std::size_t packets_dropped = 0;
};

class GrbDecoder {
public:
    [[nodiscard]] static Expected<GrbDecoder> create(const GrbConfig& config, GrbEvents events);

    void push(std::span<const dsp::Complex32> samples);
    // Demodulated symbols at unit amplitude, one per channel symbol, carrier
    // phase locked up to an ambiguity. push() ends up here.
    void push_symbols(std::span<const dsp::Complex32> symbols);
    // Decodes a PLFRAME still waiting for its end, if whole.
    void finish();
    void reset();

    [[nodiscard]] const GrbStats& stats() const { return stats_; }
    [[nodiscard]] bool demod_locked() const { return demod_.locked(); }
    [[nodiscard]] double frequency_offset_hz() const { return demod_.frequency_offset_hz(); }
    // True while PLHEADERs keep arriving where the last frame said.
    [[nodiscard]] bool frame_locked() const { return locked_; }

private:
    GrbDecoder() = default;
    void scan();
    void decode_plframe(std::size_t at, const Dvbs2Header& header);
    void push_bits(std::span<const std::uint8_t> bits);
    void deliver(std::span<const std::uint8_t> frame);
    void lose_stream();
    Dvbs2Fec* fec_for(std::uint8_t modcod);

    GrbConfig config_;
    GrbEvents events_;
    PskDemod demod_;
    std::optional<Dvbs2Fec> fec_qpsk_;
    std::optional<Dvbs2Fec> fec_8psk_;
    std::vector<std::uint8_t> scrambling_;
    GrbStats stats_;

    // Symbols not yet consumed; sym_[0] is absolute symbol base_.
    std::vector<dsp::Complex32> sym_;
    std::size_t next_ = 0;  // where the next header is expected, or where to search from
    bool locked_ = false;

    // CADU deframer over the concatenated data fields.
    std::vector<std::uint8_t> stream_;  // bits not yet consumed
    std::size_t stream_pos_ = 0;
    bool cadu_sync_ = false;
    std::array<std::optional<std::uint32_t>, 64> vc_count_{};
    std::array<SpacePacketExtractor, 64> extractors_{};

    std::vector<dsp::Complex32> data_;
    std::vector<float> llr_;
    std::vector<float> llr_deint_;
    std::vector<std::uint8_t> bbframe_;
    std::vector<SpacePacket> packets_;
};

}  // namespace revenant::decode
