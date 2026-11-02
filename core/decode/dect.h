// DECT: complex baseband to physical packet P32 A-fields.
//
// SPECIFICATION
//
// ETSI EN 300 175-2 V2.9.1 (2022-03), "Digital Enhanced Cordless
// Telecommunications (DECT); Common Interface (CI); Part 2: Physical Layer
// (PHL)", from
// https://www.etsi.org/deliver/etsi_en/300100_300199/30017502/02.09.01_60/en_30017502v020901p.pdf
// and ETSI EN 300 175-3 V2.9.1 (2022-03), "Part 3: Medium Access Control
// (MAC) layer", from
// https://www.etsi.org/deliver/etsi_en/300100_300199/30017503/02.09.01_60/en_30017503v020901p.pdf,
// both downloaded 2026-11-02. Free from ETSI.
//
// Part 2 clauses used: 4.1 (1,728 MHz channel spacing), 4.2.1 (1 152
// ksymbol/s), 4.4.2 (packet P32, 420 or 424 symbols), 4.6 (S-field, RFP and
// PP patterns), 4.7.2 (P32 D-field, 388 symbols), 4.8 (Z-field), 5.4.1 (GFSK,
// BT 0,5), 5.4.2 (a "1" is +f, nominal deviation 288 kHz).
//
// Part 3 clauses used: 6.2.1.1.2 and Table 6.4 (P32 D-field: A 64, B 320,
// X 4), 6.2.5.2 (R-CRC, g(x) = 202 611 octal, last check bit inverted), 7.0
// (MSB first), 7.1.2 and Table 7.1 (TA), 7.1.3 to 7.1.5 (Q1, BA, Q2),
// Table 7.2 (BA 111 is no B-field), 7.2.1 (tail a8 to a47), 7.2.2 (NT, RFPI,
// LSB in a47), 7.2.3.1 and Table 7.3 (QT header), 7.2.3.2 and Figure 7.6
// (static system information).
//
// DEVIATIONS
//
// The RFPI is reported as its 40 bits and the E bit. Splitting it into PARI
// and RPN depends on the access rights class, which is EN 300 175-6, not read
// for this work.
//
// Only 2-level modulation and packet P32 are decoded. P00, P00j and P80 and
// the higher-level modulations are not.
//
// WHAT THIS TAKES AS INPUT
//
// Complex baseband at any rate of at least 2 samples per symbol (2,304 MS/s;
// 2,4 MS/s from a wideband tap works), with the DECT carrier at
// `frequency_offset_hz` from the centre. Residual offset up to about 100 kHz
// is removed per burst from the S-field.
//
// WHERE THIS STOPS
//
// At the A-field: header, tail, R-CRC, and the NT and QT tail messages. The
// B-field is located (its first sample and whether BA says one is present)
// and not decoded: no voice, no descrambling, no encryption.
//
// CLEAN ROOM
//
// No DECT implementation was read.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// ---------------------------------------------------------------------------
// Specified constants
// ---------------------------------------------------------------------------

// EN 300 175-2 clause 4.2.1: 1 152 ksymbol/s.
inline constexpr double kDectSymbolRate = 1'152'000.0;
// EN 300 175-2 clause 4.1: carriers 1,728 MHz apart.
inline constexpr double kDectChannelSpacingHz = 1'728'000.0;
// EN 300 175-2 clause 5.4.2: nominal peak deviation 288 kHz.
inline constexpr double kDectDeviationHz = 288'000.0;
// EN 300 175-2 clause 5.4.1: Gaussian BT 0,5.
inline constexpr double kDectBt = 0.5;
// EN 300 175-2 clause 4.6: s0 to s31 for an RFP, s0 in the most significant
// bit. A PP sends the inverse.
inline constexpr std::uint32_t kDectSFieldRfp = 0b1010'1010'1010'1010'1110'1001'1000'1010U;
inline constexpr std::uint32_t kDectSFieldPp = ~kDectSFieldRfp;
inline constexpr std::size_t kDectSFieldBits = 32;
// EN 300 175-2 clause 4.7.2: P32 D-field, 388 symbols.
inline constexpr std::size_t kDectDFieldBits = 388;
// EN 300 175-2 clause 4.8: 4 Z-field symbols repeating the last 4 of D.
inline constexpr std::size_t kDectZFieldBits = 4;
// EN 300 175-3 Table 6.4: A-field 64, B-field 320, X-field 4.
inline constexpr std::size_t kDectAFieldBits = 64;
inline constexpr std::size_t kDectBFieldBits = 320;
inline constexpr std::size_t kDectXFieldBits = 4;
// EN 300 175-3 clause 6.2.5.2: g(x) = x^16 + x^10 + x^8 + x^7 + x^3 + 1,
// 202 611 octal; the low 16 bits for an MSB-first register.
inline constexpr std::uint32_t kDectRCrcGenerator = 0x10589U;

// EN 300 175-3 clause 6.2.5.2: the 16 check bits for 48 data bits, a8 to a47
// with a0 first; the data sits in the low 48 bits, a0 most significant.
[[nodiscard]] std::uint16_t dect_rcrc(std::uint64_t data48);

// EN 300 175-3 Table 7.1.
enum class DectTail : std::uint8_t {
    Ct0 = 0,             // 000 CT data packet 0
    Ct1 = 1,             // 001 CT data packet 1
    NtConnectionless = 2, // 010 NT on a connectionless or dummy bearer
    Nt = 3,              // 011 identities information
    Qt = 4,              // 100 system information and multiframe marker
    Combined = 5,        // 101 combined coding of a3 to a7
    Mt = 6,              // 110 MAC layer control
    PtOrFirstPp = 7,     // 111 paging tail (RFP) or first PP transmission
};

enum class DectPolarity : std::uint8_t { Rfp, Pp };

// EN 300 175-3 clause 7.2.3.2, Figure 7.6.
struct DectStaticSystemInfo {
    bool normal_reverse = false;  // a11
    unsigned slot_number = 0;     // a12 to a15, Table 7.5
    unsigned start_position = 0;  // a16 to a17, Table 7.6
    bool escape = false;          // a18
    unsigned transceivers = 0;    // a19 to a20
    bool extended_carriers = false;  // a21, Mc
    unsigned rf_carriers = 0;     // a22 to a31
    unsigned carrier_number = 0;  // a34 to a39, CN
    bool extended_info = false;   // a40
    unsigned primary_scan_carrier = 0;  // a42 to a47, PSCN
};

struct DectQt {
    unsigned qh = 0;              // a8 to a11, Table 7.3
    std::uint64_t info = 0;       // a12 to a47
    std::optional<DectStaticSystemInfo> static_info;  // QH 0 or 1
};

struct DectBurst {
    // Input sample index of the centre of s0.
    dsp::SampleIndex sample = 0;
    DectPolarity polarity = DectPolarity::Rfp;
    float correlation = 0.0F;
    // Carrier frequency estimated from the S-field, relative to the input
    // centre.
    double frequency_hz = 0.0;

    std::uint64_t a_field = 0;  // a0 in bit 63
    DectTail tail_id = DectTail::Ct0;
    bool q1 = false;
    unsigned ba = 0;
    bool q2 = false;
    std::uint64_t tail = 0;  // a8 in bit 39
    std::uint16_t rcrc = 0;
    bool crc_ok = false;

    // NT: the 40-bit RFPI, LSB in a47, and its E bit a8.
    std::optional<std::uint64_t> rfpi;
    bool rfpi_e = false;
    std::optional<DectQt> qt;

    // Table 7.2: BA 111 means no B-field.
    bool b_field_present = false;
    // Input sample index of the centre of b0, and its length in symbols.
    dsp::SampleIndex b_field_sample = 0;
    std::size_t b_field_bits = kDectBFieldBits;
};

struct DectConfig {
    dsp::SampleRate sample_rate = 4'608'000;
    double frequency_offset_hz = 0.0;
    // |normalised S-field correlation| needed to take a burst.
    double threshold = 0.8;
    // Report bursts whose A-field fails the R-CRC.
    bool emit_crc_failures = false;
};

class DectDecoder {
   public:
    DectDecoder() = default;
    [[nodiscard]] static Expected<DectDecoder> create(const DectConfig& config);

    void process(dsp::ConstComplexSpan samples, std::vector<DectBurst>& out);
    void reset();

   private:
    [[nodiscard]] double at(double index) const;
    [[nodiscard]] double correlate(std::size_t t0, double& mean) const;
    bool decode(std::size_t t0, double rho, double mean, std::vector<DectBurst>& out);
    void trim();

    DectConfig config_{};
    double sps_ = 4.0;
    std::vector<float> taps_;
    std::vector<dsp::Complex32> history_;
    std::size_t history_pos_ = 0;
    double nco_phase_ = 0.0;
    double nco_step_ = 0.0;
    dsp::Complex32 previous_{1.0F, 0.0F};
    double disc_scale_ = 1.0;

    std::vector<float> freq_;  // discriminator output, normalised to deviation
    std::size_t base_ = 0;     // absolute index of freq_[0]
    std::size_t produced_ = 0; // absolute index of the next output
    std::size_t next_ = 0;     // next t0 to evaluate

    bool hunting_ = false;
    std::size_t window_end_ = 0;
    std::size_t best_t0_ = 0;
    double best_rho_ = 0.0;
    double best_mean_ = 0.0;
    bool pending_ = false;
};

}  // namespace revenant::decode
