// VDES application specific messages, ASM-TER: complex baseband to ASM
// transmission packets.
//
// SPECIFICATION
//
// Recommendation ITU-R M.2092-2 (02/2026), "Technical characteristics for a
// VHF data exchange system in the VHF maritime mobile band", read from the
// ITU's own publication service on 2026-11-02,
// https://www.itu.int/dms_pubrec/itu-r/rec/m/R-REC-M.2092-2-202602-I!!PDF-E.pdf.
// Clauses used, all of Annex 2 unless another annex is named: A2-1.2.2 and
// Figure 4 (the slot and the burst: ramp up, 27 syncword symbols, 16 Link ID
// symbols, data symbols, ramp down), A2-1.2.3.2 and Table 1 (the ASM-TER
// syncword, 1 + Barker 13 + inverted Barker 13), A2-1.2.3.3 (syncword bit 1
// sent as pi/4-QPSK 11 and bit 0 as 00), A2-1.2.3.4 with Tables 2 and 3 (the
// Link ID: six bits, a (32,6) first order Reed-Muller code, scrambled by a
// fixed word), A2-1.2.3.5 (payload with CRC, most significant bit first),
// A2-1.2.5 (CRC-32, generator 04C11DB7, register preset to ones, appended most
// significant bit first and checked by a zero remainder), A2-1.2.6 and Figure 9
// (the bit scrambler, 1 + x^-14 + x^-15, preset 100101010000000, re-initialised
// per packet), A2-1.2.9 and Figure 11 (the alternating pi/4-QPSK mapping), Table
// 7 (ASM Link IDs 1 to 7: 9.6 ksym/s, root raised cosine roll-off 0.35, symbols
// per burst, channel bits and FEC output bits), and Annex 3 clauses A3-7.1,
// Table 24 (the slot transmission structure) and A3-7.2 to A3-7.5, Tables 25 to
// 28 (the message header common to every ASM message).
//
// THE AIS PHYSICAL LAYER IS NOT THIS ONE
//
// docs/modes.md says the ASM channels reuse the AIS physical layer. M.2092-2
// does not: AIS is GMSK at 9 600 bit/s (M.1371), and ASM-TER is pi/4-QPSK at
// 9 600 symbol/s with a Barker syncword, Table 7. So nothing of
// core/decode/ais.h is used here. What the two share is the M.1371 frame of
// 2 250 slots a minute, clause A2-1.2.2, which a receiver need not know.
//
// WHERE THIS STOPS
//
// At Link IDs 1, 2 and 3, the one, two and three slot formats Table 7 gives an
// FEC rate of 1. Link IDs 5, 6 and 7 carry the same payload through clause
// A2-1.2.4's rate 3/4 turbo code, which is not implemented; those bursts are
// reported with their Link ID and no payload. Link ID 4 is the satellite
// format, which uses a different syncword. VDE-TER (Table 8) is not decoded.
//
// From a decoded packet, the header of Tables 26 to 28 is read for every
// message ID: message ID, retransmit flag, repeat indicator, session ID, source
// ID and data count. For Messages 0 and 2, which carry no communication state,
// the application data of `data count` bits is cut out, and for Message 2 its
// first 16 bits are the ASM Identifier of clause A3-6.2. Every other message is
// reported with its header and the payload raw.
//
// WHAT THE DOCUMENT LEAVES OPEN, AND WHAT THIS DOES
//
// Table 7 gives each uncoded format 10 more channel bits than payload and CRC
// (394 against 384 for one slot), "padding + FEC tail 10 + 0". The
// Recommendation does not say whether those bits are scrambled. They come
// after the CRC, so a receiver reads the same packet either way; the
// transmitter in core/dsp/synth/vdes_mod.h sends them as zeros through the
// scrambler.
//
// HOW IT RECEIVES
//
// Textbook coherent reception, not a clause. A root raised cosine matched
// filter at the transmit roll-off (Proakis, Digital Communications, 5th ed.,
// section 9.2). The burst is found by differential correlation against the
// syncword: the product of each symbol with the conjugate of the one before
// it, correlated with the same product of the known symbols, which a carrier
// offset rotates as a whole and so does not reduce; this is the detection the
// table's own note on the satellite syncword calls differential. Its angle is
// the carrier offset. Timing is the correlation peak, refined by a parabola
// through the three samples at it. Carrier phase and the residual offset come
// from a straight line fitted to the phase of the 43 known symbols of the
// syncword and the decoded Link ID, then a first order decision-directed loop
// follows the phase through the data.
//
// CLEAN ROOM
//
// No VDES implementation was read.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/decode/dv_phy.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// ---------------------------------------------------------------------------
// Specified constants, M.2092-2
// ---------------------------------------------------------------------------

// Annex 3 clause A3-7.2 and RR Appendix 18: ASM 1 and ASM 2.
inline constexpr dsp::Hertz kVdesAsm1Hz = 161'950'000;
inline constexpr dsp::Hertz kVdesAsm2Hz = 162'000'000;

// Table 7: 9.6 ksym/s, roll-off 0.35.
inline constexpr double kVdesAsmSymbolRate = 9600.0;
inline constexpr double kVdesAsmRollOff = 0.35;

// Table 7 and Figure 4: 4 ramp symbols each end, 27 syncword symbols, 16 Link
// ID symbols.
inline constexpr std::size_t kVdesRampSymbols = 4;
inline constexpr std::size_t kVdesSyncSymbols = 27;
inline constexpr std::size_t kVdesLinkIdSymbols = 16;

// Table 1, ASM-TER and VDE-TER: 1, Barker 13, inverted Barker 13.
inline constexpr std::array<std::uint8_t, kVdesSyncSymbols> kVdesTerSyncword{
    1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0};

// Table 2, the Reed-Muller generator matrix, row i multiplying Link ID bit
// D_i, D0 the most significant. Each row's 32 bits, first transmitted in the
// most significant position.
inline constexpr std::array<std::uint32_t, 6> kVdesLinkIdGenerator{
    0b10000010111010011110100110010110U, 0b01000001110101011101010101010101U,
    0b00100011101100110011001100110011U, 0b00010011000011111000111100001111U,
    0b00001000011111110000000011111111U, 0b00000100000000000111111111111111U};

// Clause A2-1.2.3.4: the scrambling word.
inline constexpr std::uint32_t kVdesLinkIdScramble = 0b11000010111000101000111001001111U;

// Clause A2-1.2.6 and Figure 9: the bit scrambler's preset, cells 1 to 15.
inline constexpr std::array<std::uint8_t, 15> kVdesScramblerPreset{1, 0, 0, 1, 0, 1, 0, 1,
                                                                   0, 0, 0, 0, 0, 0, 0};

// One ASM format of Table 7.
struct VdesAsmFormat {
    unsigned link_id = 0;
    unsigned slots = 0;
    // Symbols per burst, ramps included.
    std::size_t burst_symbols = 0;
    // Data symbols after the Link ID.
    std::size_t data_symbols = 0;
    // FEC output bits: payload and CRC.
    std::size_t packet_bits = 0;
    // FEC rate 1 in Table 7.
    bool uncoded = false;
};

// Table 7's ASM-TER formats, Link IDs 1, 2, 3, 5, 6 and 7, or nothing.
[[nodiscard]] std::optional<VdesAsmFormat> vdes_asm_format(unsigned link_id);

// The 32-bit Link ID code word of Table 3, most significant bit first.
[[nodiscard]] std::uint32_t vdes_link_id_codeword(unsigned link_id);

// The Figure 9 scrambler over `bits` (one bit per byte), in place. Its own
// inverse.
void vdes_scramble(std::span<std::uint8_t> bits);

// Clause A2-1.2.5: CRC-32 over octets, most significant bit first, preset to
// ones, no final inversion.
[[nodiscard]] std::uint32_t vdes_crc32(std::span<const std::uint8_t> octets);

// Clause A2-1.2.9 and Figure 11: the unit point for a symbol's two bits, MSB
// first, at symbol position n counted from the first syncword symbol. Even n
// use the points at 45, 135, 225 and 315 degrees, odd n those at 0, 90, 180
// and 270. 11 is at the first of each set, then 01, 00 and 10 counter-clockwise.
[[nodiscard]] dsp::Complex32 vdes_pi4qpsk_point(std::size_t n, unsigned msb, unsigned lsb);

// ---------------------------------------------------------------------------
// Messages, Annex 3 clause A3-7
// ---------------------------------------------------------------------------

// Tables 26 to 28: the header, 56 bits.
inline constexpr std::size_t kVdesAsmHeaderBits = 56;

struct VdesAsmMessage {
    std::uint8_t message_id = 0;
    bool retransmit = false;
    std::uint8_t repeat = 0;
    std::uint8_t session_id = 0;
    std::uint32_t source_id = 0;
    std::uint16_t data_count = 0;

    // Messages 0 and 2: `data_count` bits of application data, MSB first,
    // packed from the top of the first octet.
    std::optional<std::vector<std::uint8_t>> application_data;
    // Message 2: the first 16 bits of the application data.
    std::optional<std::uint16_t> asm_identifier;
};

// Reads the header of a packet's payload (CRC removed). Never fails on a
// payload of at least 7 octets.
[[nodiscard]] std::optional<VdesAsmMessage> vdes_asm_parse(std::span<const std::uint8_t> payload);

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

struct VdesAsmConfig {
    // Complex baseband centred on the ASM channel. A whole number of samples
    // per symbol, four or more: 38 400, 48 000 and 96 000 S/s all are.
    dsp::SampleRate rate = 48000;

    // Normalised differential syncword correlation, 0 to 1, above which a
    // burst is taken to start. Noise reads about 1/sqrt(26), 0.2; random data
    // symbols reached 0.556 in tests/decode/test_vdes.cpp, and a burst at
    // 8 dB Es/N0 still reads well above this.
    double sync_threshold = 0.65;

    // Matched filter length in symbols.
    std::size_t filter_span_symbols = 8;
};

struct VdesAsmBurst {
    unsigned link_id = 0;
    // The Link ID's soft correlation with its code word, normalised: 1 is a
    // perfect match.
    double link_id_quality = 0.0;
    std::optional<VdesAsmFormat> format;

    // False when the format is not decoded (turbo coded) or the CRC failed.
    bool crc_ok = false;
    // Payload without the CRC, when crc_ok.
    std::vector<std::uint8_t> payload;
    std::optional<VdesAsmMessage> message;

    // Input sample index of the first syncword symbol, less the filter delay.
    dsp::SampleIndex sync_sample = 0;
    double sync_metric = 0.0;
    double cfo_hz = 0.0;
    // Clause A2-1.2.8: the SINR from the error vectors of the decided symbols,
    // and CQI = 40 + 4 SINR clamped to 0..255.
    double sinr_db = 0.0;
    std::uint8_t cqi = 0;
};

struct VdesAsmStats {
    std::uint64_t syncs = 0;
    std::uint64_t unknown_link_id = 0;
    std::uint64_t not_decoded = 0;
    std::uint64_t crc_failures = 0;
    std::uint64_t packets = 0;
};

class VdesAsmDecoder {
public:
    [[nodiscard]] static Expected<VdesAsmDecoder> create(const VdesAsmConfig& config);

    // Appends every burst that has completed. The same stream gives the same
    // bursts however it is split.
    void process(dsp::ConstComplexSpan samples, std::vector<VdesAsmBurst>& out);

    [[nodiscard]] const VdesAsmStats& stats() const { return stats_; }

    void reset();

private:
    VdesAsmDecoder() = default;

    // The syncword and Link ID of a detected burst: timing, carrier offset,
    // the 43 known symbols after the offset is removed, and the Link ID.
    struct Front {
        double t0 = 0.0;
        double w = 0.0;
        std::array<dsp::Complex32, kVdesSyncSymbols + kVdesLinkIdSymbols> z{};
        double amplitude = 0.0;
        unsigned link_id = 0;
        double link_id_quality = 0.0;
    };

    [[nodiscard]] dsp::Complex32 at(double index) const;
    [[nodiscard]] double metric_at(std::int64_t t, dsp::Complex32* corr) const;
    [[nodiscard]] Front front(std::int64_t t, double metric, dsp::Complex32 corr) const;
    [[nodiscard]] dsp::Complex32 sample(const Front& f, std::size_t n) const;
    void decode_at(const Front& f, double metric, std::vector<VdesAsmBurst>& out);

    VdesAsmConfig config_{};
    std::size_t sps_ = 0;
    ComplexFir filter_{};
    std::size_t filter_delay_ = 0;

    std::array<dsp::Complex32, kVdesSyncSymbols> sync_points_{};
    std::array<dsp::Complex32, kVdesSyncSymbols - 1> sync_diff_{};

    std::vector<dsp::Complex32> buffer_;
    std::int64_t buffer_start_ = 0;
    std::int64_t total_ = 0;

    std::int64_t next_metric_ = 0;
    // A peak waiting for sps samples of lower metric after it.
    bool have_candidate_ = false;
    std::int64_t candidate_ = 0;
    double candidate_metric_ = 0.0;
    dsp::Complex32 candidate_corr_{};
    std::int64_t quiet_until_ = 0;

    // A detected burst waiting for its samples.
    struct Pending {
        std::int64_t t = 0;
        double metric = 0.0;
        dsp::Complex32 corr{};
    };
    std::vector<Pending> pending_;

    std::vector<dsp::Complex32> scratch_;
    VdesAsmStats stats_{};
};

}  // namespace revenant::decode
