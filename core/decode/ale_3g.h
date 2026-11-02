// Third generation automatic link establishment (3G ALE): receiver audio to
// the burst waveforms BW0 and BW1, and their payloads to 3G-ALE and traffic
// management PDUs.
//
// SPECIFICATION
//
// MIL-STD-188-141B, 1 March 1999, Appendix C, distribution statement A
// (approved for public release, unlimited), read from everyspec.com. A US
// Government work; the tables below are transcribed from it. MIL-STD-188-141D
// Appendix C (22 December 2017) replaces that text with a reference to
// STANAG 4538, which it calls essentially identical; 141B is the last public
// edition that carries it. Clauses used:
//
//   C.4.10    order of transmission: fields left to right, MSB first
//   C.4.12    the CRC procedure, figure C-4
//   C.5.1.2   the block interleaver's stuff and fetch algorithm
//   C.5.1.3   BW0: figure C-5 timing; table C-V TLC/AGC guard sequence,
//             table C-VI preamble, figure C-6 rate 1/2 K = 7 tail-biting
//             code, table C-VII interleaver, table C-VIII Walsh symbols,
//             table C-IX PN sequence
//   C.5.1.4   BW1: figure C-7 timing; tables C-X, C-XI, figure C-8 rate 1/3
//             K = 9 tail-biting code, tables C-XII, C-XIII
//   C.5.1.8   table C-XX, 8-PSK at 2400 symbols/s on 1800 Hz
//   C.5.2.2   figure C-17, the 3G-ALE PDUs; tables C-XXII to C-XXV; the
//             LinkID computation and its five worked examples; C.5.2.2.6
//             the 4- and 8-bit CRC polynomials
//   C.5.3.4   table C-XXXVI, the TM PDU carried on BW1, and its 12-bit CRC
//
// Readings the document leaves open, each an assumption:
//
//   - The convolutional encoders of figures C-6 and C-8 list cells x6..x1
//     (x8..x1) and a final tap "1". This reads "1" as the cell that receives
//     the bit shifted out of x1, so the seven (nine) taps span the newest
//     payload bit (x6 or x8) back to the oldest ("1"). For BW0 the two
//     generators are then the familiar 133 and 171 octal pair.
//   - The transmit low pass filter of figure C-16 is not specified. The
//     transmitter in core/dsp/synth/ale_3g_mod.h uses a root raised cosine
//     of roll-off 0.35 and the receiver the same, a matched pair; a real
//     transmitter's filter will be something else, which costs a little SNR
//     and nothing more because the data is Walsh coded at 16 symbols a bit.
//
// WHERE THIS STOPS
//
// BW0 and BW1 are detected and decoded. BW2, BW3 and BW4 (the HDL and LDL
// traffic waveforms) are not. BW5 is not in MIL-STD-188-141B; it is a later
// STANAG 4538 addition and is not implemented. A BW1 payload whose protocol
// field is not the TM protocol's 001 (an HDL_ACK, C.5.4.4) is reported raw.
// Synchronous mode slot and dwell timing (C.5.2.3) is a property of a network
// and is not modelled: each burst is reported where it is found.
//
// CLEAN ROOM
//
// No 3G ALE or STANAG 4538 implementation was read.

#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/decode/dv_phy.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// ---------------------------------------------------------------------------
// Specified constants
// ---------------------------------------------------------------------------

// C.5.1.8: 8-PSK, 2400 symbols/s, 1800 Hz carrier.
inline constexpr double kAle3gSymbolRate = 2400.0;
inline constexpr double kAle3gCarrierHz = 1800.0;
inline constexpr std::size_t kAle3gTlcSymbols = 256;

enum class Ale3gWaveform : std::uint8_t { Bw0, Bw1 };

// Figures C-5 and C-7.
[[nodiscard]] constexpr std::size_t ale3g_preamble_symbols(Ale3gWaveform w) {
    return w == Ale3gWaveform::Bw0 ? 384 : 576;
}
[[nodiscard]] constexpr std::size_t ale3g_data_symbols(Ale3gWaveform w) {
    return w == Ale3gWaveform::Bw0 ? 832 : 2304;
}
[[nodiscard]] constexpr int ale3g_payload_bits(Ale3gWaveform w) { return w == Ale3gWaveform::Bw0 ? 26 : 48; }

// Tables C-V (C-X), C-VI, C-XI and C-IX (C-XIII).
[[nodiscard]] std::span<const std::uint8_t> ale3g_tlc();
[[nodiscard]] std::span<const std::uint8_t> ale3g_preamble(Ale3gWaveform w);
[[nodiscard]] std::span<const std::uint8_t> ale3g_pn();

// C.4.12: a K-bit CRC of the bits given (each 0 or 1, first transmitted
// first) under g(x), whose coefficients g_{K-1}..g_0 are `poly` (g_K is
// implied). The result's MSB is C_{K-1}, the first CRC bit sent.
[[nodiscard]] std::uint32_t ale3g_crc(std::span<const std::uint8_t> bits, int k, std::uint32_t poly);
// C.5.2.2.6 and table C-XXXVI.
inline constexpr std::uint32_t kAle3gCrc4 = 0xB;    // x^4 + x^3 + x + 1
inline constexpr std::uint32_t kAle3gCrc8 = 0x9B;   // x^8 + x^7 + x^4 + x^3 + x + 1
inline constexpr std::uint32_t kAle3gCrc12 = 0xBCF; // x^12+x^11+x^9+x^8+x^7+x^6+x^3+x^2+x+1

// C.5.2.2.2: the link ID from two 11-bit addresses.
[[nodiscard]] int ale3g_link_id(std::uint32_t caller, std::uint32_t responder);

// ---------------------------------------------------------------------------
// Burst coding
// ---------------------------------------------------------------------------

// The payload, first bit transmitted in the most significant of the low
// `ale3g_payload_bits` bits, to the data symbols (PN spread, before 8-PSK).
[[nodiscard]] std::vector<std::uint8_t> ale3g_data(Ale3gWaveform w, std::uint64_t payload);

// The whole burst: TLC, preamble and data symbols.
[[nodiscard]] std::vector<std::uint8_t> ale3g_burst(Ale3gWaveform w, std::uint64_t payload);

// C.5.1.2, for testing: the fetch order as stuff indices.
[[nodiscard]] std::vector<int> ale3g_interleave_order(Ale3gWaveform w);

// Tail-biting convolutional code of figures C-6 and C-8: payload bits in,
// coded bits out in transmit order.
[[nodiscard]] std::vector<std::uint8_t> ale3g_encode(Ale3gWaveform w, std::span<const std::uint8_t> payload);

// ---------------------------------------------------------------------------
// PDUs
// ---------------------------------------------------------------------------

enum class Ale3gPduType : std::uint8_t {
    Call,          // LE_Call
    Handshake,     // LE_Handshake
    Notification,  // LE_Notification
    TimeOffset,    // LE_Time_Offset
    GroupTime,     // LE_Group_Time_Broadcast
    Broadcast,     // LE_Broadcast
    ScanningCall,  // LE_Scanning_Call
    Reserved,      // 11 leading bits, or 0110 1 in a group time PDU
    TmRequest,     // TM PDUs on BW1, by Type
    TmConfirm,
    TmTerminate,
    TmReserved,
    OtherBw1,      // a BW1 payload of another protocol
};

[[nodiscard]] const char* ale3g_pdu_name(Ale3gPduType t);

struct Ale3gPdu {
    Ale3gPduType type = Ale3gPduType::Reserved;
    bool crc_ok = false;
    // Fields, by figure C-17 and table C-XXXVI; those a PDU lacks stay -1.
    int called_member = -1;
    int call_type = -1;
    int caller_member = -1;
    int caller_group = -1;
    int link_id = -1;
    int command = -1;
    int argument = -1;
    int caller_status = -1;
    int time_quality = -1;
    int offset = -1;        // signed, from the sign and magnitude fields
    int server_group = -1;
    int dwell = -1;
    int slot = -1;
    int countdown = -1;
    int channel = -1;
    int called_address = -1;  // 11 bits
    int protocol = -1;        // BW1
    int priority = -1;
    int dest_address = -1;
    int source_address = -1;
};

[[nodiscard]] Ale3gPdu ale3g_parse(Ale3gWaveform w, std::uint64_t payload);

// Builders for the transmitter and tests: the payload with its CRC.
[[nodiscard]] std::uint64_t ale3g_call(int called_member, int call_type, int caller_member, int caller_group);
[[nodiscard]] std::uint64_t ale3g_handshake(int link_id, int command, int argument);
[[nodiscard]] std::uint64_t ale3g_scanning_call(int called_address);
[[nodiscard]] std::uint64_t ale3g_tm(int priority, int dest, int source, int type, int argument);

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

struct Ale3gConfig {
    dsp::SampleRate rate = 8000;
    // Engineering choices: the widest carrier offset the preamble search
    // tolerates is set by its 8-symbol coherent segments, about +/- 75 Hz;
    // the threshold is the preamble's share of received power. Noise alone
    // averages 1/8.
    double detect_threshold = 0.22;
    // Root raised cosine roll-off of the matched filter.
    double roll_off = 0.35;
};

struct Ale3gBurst {
    std::int64_t position = 0;   // sample at the first preamble symbol
    Ale3gWaveform waveform = Ale3gWaveform::Bw0;
    double metric = 0.0;         // preamble match, 0 to 1
    double offset_hz = 0.0;
    std::uint64_t payload = 0;
    Ale3gPdu pdu;
};

class Ale3gDecoder {
public:
    [[nodiscard]] static Expected<Ale3gDecoder> create(const Ale3gConfig& config);

    // Appends each burst once decoded. The output depends only on the
    // concatenated input, never on how it was split into calls.
    void process(dsp::ConstRealSpan audio, std::vector<Ale3gBurst>& out);
    void flush(std::vector<Ale3gBurst>& out);
    void reset();

private:
    using Cplx = std::complex<double>;
    explicit Ale3gDecoder(const Ale3gConfig& config, std::vector<float> taps);

    [[nodiscard]] Cplx sample(double t) const;
    [[nodiscard]] double metric(Ale3gWaveform w, double t, double* freq) const;
    bool step(std::vector<Ale3gBurst>& out);
    void decode(Ale3gWaveform w, double t, double metric, double freq, std::vector<Ale3gBurst>& out);
    void trim();

    Ale3gConfig config_;
    double fs_ = 0.0;
    double sps_ = 0.0;
    std::vector<float> taps_;
    std::vector<Cplx> raw_;       // the last taps_.size() inputs, mixed to baseband
    std::int64_t in_count_ = 0;
    std::vector<Cplx> y_;         // filtered baseband, absolute base_ + i
    std::int64_t base_ = 0;
    std::int64_t next_ = 0;       // next search position
};

}  // namespace revenant::decode
