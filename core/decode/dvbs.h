// DVB-S: complex baseband to MPEG-2 transport stream packets, plus the PAT,
// PMT and SDT read out of them.
//
// SPECIFICATION
//
// ETSI EN 300 421 V1.1.2 (1997-08), "Digital Video Broadcasting (DVB);
// Framing structure, channel coding and modulation for 11/12 GHz satellite
// services", free from etsi.org. Cited below as "EN 300 421" with a clause,
// table or figure. ETSI keeps the rights in the text, so clause numbers are
// cited and no table is reproduced.
//
// The transport stream layer is ITU-T Rec. H.222.0 (05/2006) | ISO/IEC
// 13818-1, free from itu.int, cited as "H.222.0": the packet header of
// 2.4.3.2, the pointer_field of 2.4.4.1, the PAT of 2.4.4.3, the PMT of
// 2.4.4.8 and the CRC_32 of Annex A. The Service Description Table is ETSI EN
// 300 468 V1.17.1 (2022-10), free from etsi.org, cited as "EN 300 468": the
// section of 5.2.3, table 5, carried on PID 0x0011 with table_id 0x42, and
// the service_descriptor of 6.2.33, table 88.
//
// WHAT IS DECODED
//
//   - QPSK with root raised cosine shaping at roll-off 0.35, EN 300 421 4.5,
//     through core/decode/psk_demod.h. Figure 5 maps bit pair (I, Q) = (0, 0)
//     to the first quadrant, (1, 0) to the second, (1, 1) to the third and
//     (0, 1) to the fourth: a zero bit is a positive rail.
//   - The K=7 rate 1/2 mother code, G1 = 171 and G2 = 133 octal, and the
//     punctured rates 2/3, 3/4, 5/6 and 7/8 of table 2, through
//     core/decode/viterbi_stream.h. Table 2's I and Q columns put the
//     surviving bits on the rails in encoder output order, X before Y within
//     a step, I before Q within a symbol.
//   - The Forney convolutional interleaver, I = 12, M = 17, of 4.4.2 and
//     figure 4, with the sync byte always on branch 0.
//   - RS(204,188, T=8), shortened from RS(255,239), 4.4.2: field polynomial
//     x^8 + x^4 + x^3 + x^2 + 1, generator roots lambda^0 to lambda^15 with
//     lambda = 0x02, through core/decode/reed_solomon.h.
//   - Energy dispersal, 4.4.1 and figure 2: the PRBS 1 + X^14 + X^15 loaded
//     with 100101010000000 at every inverted sync byte 0xB8, which opens a
//     group of eight packets, running but not applied over the seven
//     following sync bytes.
//   - The transport stream header, and the PAT, PMT and SDT sections with
//     their CRC_32 checked.
//
// ACQUISITION
//
// EN 300 421 4.4.3 NOTE says a receiver tries each code rate and puncturing
// configuration until lock, and resolves the demodulator's phase ambiguity on
// the sync byte. That is what this does. Over a window of symbols it tries,
// for each rate allowed, a quarter turn or none, the spectrum as received or
// conjugated, and every alignment of the puncturing period against the
// soft-bit stream; for each it runs the Viterbi decoder and looks for a byte
// equal to 0x47 or 0xB8 at five consecutive 204-byte spacings. A half turn
// needs no hypothesis of its own: both generators have odd weight, so it
// inverts every decoded bit, and 0x47 and 0xB8 are each other's complement.
// The inverted stream shows one 0x47 to seven 0xB8, and is flipped back.
//
// DEVIATIONS
//
// The transport_error_indicator of a packet whose RS word was refused is set,
// as H.222.0 2.4.3.3 permits an entity outside the transport layer to do. A
// service name is returned as the raw bytes of the descriptor, including any
// leading character table selector, because EN 300 468 Annex A was not read.
// MPEG-2 video, audio and every other elementary stream are identified by
// stream_type and not decoded.
//
// CLEAN ROOM
//
// No DVB implementation was read: not dvbs2rx, not GNU Radio's gr-dtv, not
// leandvb. Written from the three documents above.

#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/decode/psk_demod.h"
#include "core/decode/reed_solomon.h"
#include "core/decode/viterbi_stream.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

inline constexpr std::size_t kTsPacketBytes = 188;
inline constexpr std::size_t kDvbsCodedPacketBytes = 204;
inline constexpr std::uint8_t kTsSync = 0x47;
inline constexpr std::uint8_t kTsSyncInverted = 0xB8;
// EN 300 421 4.4.2: I = 12 branches, M = 17 cells per unit of branch index.
inline constexpr std::size_t kDvbsInterleaveDepth = 12;
inline constexpr std::size_t kDvbsInterleaveCell = 17;
// End to end delay of interleaver plus de-interleaver: (I - 1) * M * I bytes,
// which is exactly 11 coded packets.
inline constexpr std::size_t kDvbsInterleaveDelayBytes =
    (kDvbsInterleaveDepth - 1) * kDvbsInterleaveCell * kDvbsInterleaveDepth;

enum class DvbsCodeRate : std::uint8_t { R1_2, R2_3, R3_4, R5_6, R7_8 };

// EN 300 421 table 2, flattened into encoder output order X1 Y1 X2 Y2 ...;
// 1 means transmitted. The convention of StreamingViterbiConfig::puncture.
[[nodiscard]] std::vector<std::uint8_t> dvbs_puncture_pattern(DvbsCodeRate rate);
// Information bits per coded bit, as a fraction: 1/2, 2/3 and so on.
[[nodiscard]] double dvbs_code_rate_value(DvbsCodeRate rate);
[[nodiscard]] const char* dvbs_code_rate_name(DvbsCodeRate rate);

// EN 300 421 table 2: G1 = 171, G2 = 133 octal, D^0 in the least significant
// bit, which is the CCSDS pair without CCSDS's inversion of G2.
inline constexpr std::uint32_t kDvbsMemory = 6;
inline constexpr std::array<std::uint32_t, 2> kDvbsGenerators{kCcsdsK7G1, kCcsdsK7G2};

// The QPSK symbol index of psk_point(Qpsk, k) carrying bit pair (I, Q), per
// figure 5, and its inverse: index for 2*i + q.
inline constexpr std::array<std::uint8_t, 4> kDvbsQpskIndex{0, 3, 1, 2};
// bit_map for PskDemodConfig: label (i << 1 | q) of point k.
inline constexpr std::array<std::uint8_t, 8> kDvbsQpskBitMap{0b00, 0b10, 0b11, 0b01, 0, 0, 0, 0};

// The energy dispersal PRBS of EN 300 421 figure 2.
class DvbsPrbs {
public:
    DvbsPrbs() { reset(); }
    // Loads 100101010000000 into registers 1 to 15.
    void reset();
    [[nodiscard]] std::uint8_t next_bit();
    [[nodiscard]] std::uint8_t next_byte();  // MSB first

private:
    std::uint16_t reg_ = 0;  // bit i - 1 is register i
};

// Randomises, or de-randomises, one group of eight 188-byte packets in place,
// 4.4.1. On transmit the first sync byte becomes 0xB8; on receive every sync
// byte becomes 0x47. Bytes 1 to 187 of each packet are XORed with the PRBS.
void dvbs_energy_dispersal(std::span<std::uint8_t> eight_packets, bool transmit);

// The convolutional interleaver of 4.4.2 one byte at a time. The interleaver
// delays branch j by j * M bytes; the de-interleaver by (I - 1 - j) * M. The
// branch is the byte count modulo I, so the caller starts the count on a sync
// byte.
class DvbsInterleaver {
public:
    explicit DvbsInterleaver(bool deinterleave);
    [[nodiscard]] std::uint8_t push(std::uint8_t byte);
    void reset();

private:
    bool deinterleave_;
    std::size_t branch_ = 0;
    std::array<std::vector<std::uint8_t>, kDvbsInterleaveDepth> fifo_;
    std::array<std::size_t, kDvbsInterleaveDepth> head_{};
};

// RS(204,188) of 4.4.2.
[[nodiscard]] const Rs8Codec& dvbs_rs();

struct DvbsConfig {
    SampleRate rate = 0;
    double symbol_rate = 0.0;
    // Empty tries every rate in table 2.
    std::optional<DvbsCodeRate> code_rate;
    double roll_off = 0.35;
    double max_offset_hz = 0.0;
    double carrier_bandwidth = 0.01;
    double timing_bandwidth = 0.005;
    // Viterbi survivor window; punctured high rates want more than 5K.
    std::size_t traceback = 128;
    // Consecutive missing sync bytes that drop lock.
    unsigned sync_loss = 8;
};

struct DvbsPacket {
    // Sync byte restored to 0x47. transport_error_indicator set when the RS
    // word was refused.
    std::array<std::uint8_t, kTsPacketBytes> bytes{};
    bool rs_ok = false;
    std::uint8_t rs_corrected = 0;
};

struct DvbsStats {
    std::uint64_t packets = 0;
    std::uint64_t rs_failed = 0;
    std::uint64_t rs_corrected_bytes = 0;
    std::uint64_t sync_losses = 0;
};

class DvbsDecoder {
public:
    DvbsDecoder() = default;
    [[nodiscard]] static Expected<DvbsDecoder> create(const DvbsConfig& config);

    // Consumes complex baseband and returns every packet it completes. The
    // span is valid until the next call. The output does not depend on how
    // the stream is split into calls.
    std::span<const DvbsPacket> process(dsp::ConstComplexSpan in);
    void reset();

    [[nodiscard]] bool locked() const { return locked_; }
    [[nodiscard]] std::optional<DvbsCodeRate> code_rate() const;
    [[nodiscard]] bool spectrum_inverted() const { return hyp_.conjugate; }
    [[nodiscard]] double frequency_offset_hz() const { return demod_.frequency_offset_hz(); }
    [[nodiscard]] const DvbsStats& stats() const { return stats_; }

private:
    struct Hypothesis {
        DvbsCodeRate rate = DvbsCodeRate::R1_2;
        bool quarter = false;
        bool conjugate = false;
        std::size_t skip = 0;  // soft values dropped before the decoder
        std::size_t sync_bit = 0;
        bool invert = false;
    };

    void on_symbol(Complex32 symbol);
    void try_acquire();
    [[nodiscard]] std::optional<Hypothesis> test(DvbsCodeRate rate, bool quarter,
                                                 bool conjugate, std::size_t skip) const;
    void feed_locked(Complex32 symbol);
    void on_bits(std::span<const std::uint8_t> bits);
    void on_byte(std::uint8_t byte);
    void on_coded_packet();
    void lose_lock();

    DvbsConfig config_;
    PskDemod demod_;
    std::size_t acquisition_symbols_ = 0;

    std::vector<Complex32> window_;
    bool locked_ = false;
    Hypothesis hyp_;
    std::optional<StreamingViterbi> viterbi_;
    std::size_t soft_skipped_ = 0;
    std::uint64_t bit_count_ = 0;
    std::uint8_t byte_acc_ = 0;
    unsigned byte_bits_ = 0;
    std::uint64_t byte_count_ = 0;
    unsigned sync_misses_ = 0;

    std::optional<DvbsInterleaver> deinterleaver_;
    std::uint64_t deinterleaved_ = 0;
    std::array<std::uint8_t, kDvbsCodedPacketBytes> coded_{};
    std::size_t coded_fill_ = 0;
    DvbsPrbs prbs_;
    int group_index_ = -1;  // packet within the energy dispersal group

    std::vector<std::uint8_t> bits_scratch_;
    std::vector<DvbsPacket> out_;
    DvbsStats stats_;
};

// ---------------------------------------------------------------------------
// Transport stream, H.222.0 and EN 300 468
// ---------------------------------------------------------------------------

struct TsHeader {
    bool transport_error = false;
    bool payload_unit_start = false;
    std::uint16_t pid = 0;
    std::uint8_t scrambling = 0;
    std::uint8_t adaptation_field_control = 0;
    std::uint8_t continuity_counter = 0;
    // Offset of the payload within the packet, or 188 when there is none.
    std::size_t payload_offset = kTsPacketBytes;
};

// Fails on a wrong sync byte or an adaptation field that overruns the packet.
[[nodiscard]] std::optional<TsHeader> ts_parse_header(std::span<const std::uint8_t> packet);

// H.222.0 Annex A: CRC_32 of a PSI section, polynomial 0x04C11DB7, register
// preset to all ones, no reflection, no final inversion.
[[nodiscard]] std::uint32_t ts_crc32(std::span<const std::uint8_t> bytes);

struct TsProgram {
    std::uint16_t program_number = 0;
    std::uint16_t pid = 0;  // program_map_PID, or network_PID for program 0
};
struct TsPat {
    std::uint16_t transport_stream_id = 0;
    std::uint8_t version = 0;
    std::vector<TsProgram> programs;
};
struct TsElementaryStream {
    std::uint8_t stream_type = 0;
    std::uint16_t pid = 0;
};
struct TsPmt {
    std::uint16_t program_number = 0;
    std::uint16_t pcr_pid = 0;
    std::uint8_t version = 0;
    std::vector<TsElementaryStream> streams;
};
struct TsService {
    std::uint16_t service_id = 0;
    std::uint8_t service_type = 0;
    std::uint8_t running_status = 0;
    bool free_ca_mode = false;
    std::string provider;
    std::string name;
};
struct TsSdt {
    std::uint16_t transport_stream_id = 0;
    std::uint16_t original_network_id = 0;
    std::uint8_t version = 0;
    std::vector<TsService> services;
};

// Assembles PSI sections on PID 0, on the PMT PIDs the PAT names, and on PID
// 0x0011, and keeps the latest current tables. Packets flagged with a
// transport error are skipped.
class TsPsiParser {
public:
    void push(std::span<const std::uint8_t> packet);
    void reset();

    [[nodiscard]] const std::optional<TsPat>& pat() const { return pat_; }
    [[nodiscard]] const std::map<std::uint16_t, TsPmt>& pmts() const { return pmts_; }
    [[nodiscard]] const std::optional<TsSdt>& sdt() const { return sdt_; }
    [[nodiscard]] std::uint64_t crc_errors() const { return crc_errors_; }
    [[nodiscard]] std::uint64_t sections() const { return sections_; }
    // Packets seen per PID.
    [[nodiscard]] const std::map<std::uint16_t, std::uint64_t>& pid_counts() const {
        return pid_counts_;
    }

private:
    struct Assembly {
        std::vector<std::uint8_t> data;
        bool active = false;
    };
    void feed(Assembly& a, std::span<const std::uint8_t> bytes);
    void on_section(std::span<const std::uint8_t> section);
    [[nodiscard]] bool wanted(std::uint16_t pid) const;

    std::map<std::uint16_t, Assembly> assembly_;
    std::optional<TsPat> pat_;
    std::map<std::uint16_t, TsPmt> pmts_;
    std::optional<TsSdt> sdt_;
    std::uint64_t crc_errors_ = 0;
    std::uint64_t sections_ = 0;
    std::map<std::uint16_t, std::uint64_t> pid_counts_;
};

}  // namespace revenant::decode
