// Cospas-Sarsat 406 MHz first-generation distress beacons: complex baseband
// to decoded messages.
//
// SPECIFICATION
//
// C/S T.001, "Specification for Cospas-Sarsat 406 MHz Distress Beacons",
// Issue 4 - Revision 7, March 2021, the International Cospas-Sarsat
// Programme. Fetched 2026-11-02 from a copy the Thai MCC publishes at
// sar.mot.go.th/document/THMCC/ (the Programme's own site did not answer a
// direct link); the cover, revision table and page footers identify it.
// Cited as "T.001" with a section, table or figure. Later revisions (to
// Rev. 13 by one secondary listing) were not held; what is decoded here is
// the long-standing first-generation core, which those revisions extend
// rather than change.
//
// WHAT IS DECODED
//
//   - the burst, 2.2.5 to 2.2.8: 160 ms of unmodulated carrier, then 15
//     ones of bit sync, the 9-bit frame sync 000101111 (or 011010000 in
//     self-test), the format flag, and 112 or 144 bits in all, at 400 bit/s;
//   - the modulation, 2.3.5 and 2.3.6 with figure 2.5: biphase-L, a one
//     sent as +1.1 rad for the first half bit and -1.1 rad for the second;
//   - BCH-1, the (82,61) code shortened from BCH(127,106), and BCH-2, the
//     (38,26) code shortened from BCH(63,51), section 3.2 and Annex B, both
//     corrected through core/decode/bch.h;
//   - PDF-1's format flag, protocol flag and country code, A1.2, and the
//     protocol code, A1.3 and Table A2;
//   - the user protocols of A2: maritime (MMSI or call sign), radio call
//     sign, aviation, serial (all six beacon types), test, orbitography and
//     national, with the modified-Baudot characters of Table A3;
//   - the location protocols of A3: user-location (A3.3.4), standard
//     (A3.3.5), national (A3.3.6), RLS (A3.3.7) and ELT(DT) (A3.3.8), coarse
//     position in PDF-1 plus the PDF-2 offset, with the default values of
//     A3.2 read as "no position";
//   - the 15 Hex ID of 3.3: bits 26 to 85 with any PDF-1 position replaced
//     by its default.
//
// THE RECEIVER
//
// T.001 specifies the transmitter only; everything below is this file's
// design. A burst is found by its carrier as a spectral line. The carrier's
// frequency, its rate of change and its phase are read from the first 120 ms
// after the rise, which T.001 2.2.6 guarantees is unmodulated. The frame is found by correlating the 24 sync
// bits' biphase waveform against the quadrature component. Bits are then
// integrated half by half, with a carrier loop on each bit's mean (biphase
// with symmetric deviation, 2.3.6, leaves cos(1.1) of the carrier in every
// bit) and a timing loop on the mid-bit transition every biphase bit has.
// The timing loop is there because 2.2.8 allows the bit rate to be 1% off,
// which is 1.4 bits over a long message.
//
// CLEAN ROOM
//
// No beacon decoder was read: not dec406, not gr-satellites, not any
// other. The Table A3 code is reproduced because a decoder cannot work
// without it; it is a character code, cited at the table.

#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/decode/bch.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// T.001 2.2.6 to 2.2.8.
inline constexpr double kCsBitRate = 400.0;
inline constexpr double kCsPhaseDeviation = 1.1;  // radians, 2.3.6
inline constexpr double kCsCarrierSeconds = 0.160;
inline constexpr std::size_t kCsShortBits = 112;
inline constexpr std::size_t kCsLongBits = 144;
inline constexpr std::uint32_t kCsFrameSync = 0b000101111;     // 2.2.8.2
inline constexpr std::uint32_t kCsFrameSyncTest = 0b011010000;  // self-test

enum class CsProtocol : std::uint8_t {
    // A2, user protocols (Table A2-A), short or long.
    Maritime,
    RadioCallSign,
    Aviation,
    Serial,
    Test,
    Orbitography,
    NationalUser,
    SecondGeneration,  // code 101, not a first-generation protocol
    // A3, location protocols (Table A2-B).
    StandardMmsi,
    StandardAircraftAddress,
    StandardEltSerial,
    StandardEltOperator,
    StandardEpirbSerial,
    StandardPlbSerial,
    ShipSecurity,
    NationalElt,
    NationalEpirb,
    NationalPlb,
    StandardTest,
    NationalTest,
    Rls,
    EltDt,
    Spare,
};

[[nodiscard]] const char* cs_protocol_name(CsProtocol protocol);

struct CsPosition {
    double latitude = 0.0;   // degrees, north positive
    double longitude = 0.0;  // degrees, east positive
    // True when PDF-2 refined the PDF-1 coarse position, or for
    // user-location protocols carried the position itself.
    bool refined = false;
    // A3.3.2.1: provided by an internal navigation device.
    std::optional<bool> internal_source;
};

struct CsMessage {
    std::vector<std::uint8_t> bits;  // 1-based in T.001; bits[0] is bit 1
    bool long_message = false;
    bool self_test = false;
    bool user_protocol = false;  // protocol flag, A1.2.2
    unsigned country_code = 0;   // A1.2.3
    CsProtocol protocol = CsProtocol::Spare;
    std::string hex_id;          // 3.3, 15 characters
    // BCH bits corrected, or nothing when the field was refused.
    std::optional<std::size_t> bch1;
    std::optional<std::size_t> bch2;  // long messages only
    // A human-readable identity: MMSI, call sign, registration, serial and
    // type approval number, 24-bit address, as the protocol carries.
    std::string identity;
    std::optional<std::uint32_t> aircraft_address;
    std::optional<std::uint32_t> mmsi;  // all nine digits where recoverable
    // A2 bits 84-85 or A3.3.2.2: 0 none, 1 121.5 MHz, 2 SART, 3 other.
    std::optional<unsigned> homing;
    std::optional<CsPosition> position;
    // Short messages: the six non-protected bits, A2.9.
    std::optional<unsigned> non_protected;
};

// Decodes bits 1 to 112 or 1 to 144, one per byte. Fails when the frame
// sync is neither pattern, or BCH-1 is refused.
[[nodiscard]] Expected<CsMessage> cs_parse(std::span<const std::uint8_t> bits);

// Fills BCH-1 (and BCH-2 for a long message) of a message whose bits 1 to
// 85 (and 107 to 132) are set. For the transmitter and the tests.
void cs_fill_bch(std::span<std::uint8_t> bits);

// Table A3, for the transmitter and the tests: the six-bit code of a
// character, or nothing.
[[nodiscard]] std::optional<unsigned> cs_baudot_encode(char c);
[[nodiscard]] char cs_baudot_decode(unsigned code);

struct CsDecoderConfig {
    dsp::SampleRate rate = 0;
    // A burst is found by its 160 ms carrier: a block of up to 20 ms, a
    // power of two long, whose strongest frequency bin exceeds the mean bin
    // by this factor. 16 puts a false trigger on noise at about one block
    // in 35000 for 256-point blocks, and a chosen, not cited, figure.
    double trigger_ratio = 16.0;
};

struct CsBurst {
    CsMessage message;
    std::uint64_t sample_position = 0;  // start of the carrier
    double frequency_offset_hz = 0.0;
    double bit_rate = 0.0;  // measured
};

class CsDecoder {
public:
    [[nodiscard]] static Expected<CsDecoder> create(const CsDecoderConfig& config);

    // Complex baseband with the beacon within a few kilohertz of zero.
    // Chunks may split anywhere.
    void push(dsp::ConstComplexSpan in, std::vector<CsBurst>& out);
    void reset();
    [[nodiscard]] std::size_t bursts_failed() const { return failed_; }

private:
    CsDecoder() = default;
    void process_capture(std::vector<CsBurst>& out);

    CsDecoderConfig config_;
    std::size_t window_ = 0;        // trigger block, a power of two
    std::size_t capture_len_ = 0;   // the longest burst plus margin
    std::vector<dsp::Complex32> history_;  // the block being filled
    std::uint64_t samples_ = 0;
    bool capturing_ = false;
    std::vector<dsp::Complex32> capture_;
    std::uint64_t capture_start_ = 0;
    std::uint64_t holdoff_until_ = 0;
    std::size_t failed_ = 0;
};

// ===========================================================================
// Second-generation beacons (SGB)
// ===========================================================================
//
// SPECIFICATION
//
// C/S T.018, "Specification for Second-Generation Cospas-Sarsat 406-MHz
// Distress Beacons", Issue 1 - Revision 7, March 2021, fetched 2026-11-02
// from the Programme's own site,
// https://www.cospas-sarsat.int/images/stories/SystemDocs/Current/T018-MAR-26-2021.pdf.
// Its pages are watermarked "superseded by a later version"; the later
// revisions were not held. Cited as "T.018" with a section, table or
// appendix. Used:
//
//   - 2.2.2 to 2.2.7: a 1 s burst; 166.7 ms of preamble (6400 chips per arm,
//     bits all zero) then 250 bits at 300 bit/s, odd bits (1, 3, ...) on I
//     and even bits on Q, 256 chips a bit per arm, a one sent as the
//     inverted PRN (Table 2.4), chip logic 1 at signal level -1 (Table 2.3);
//   - 2.2.3, Figure 2-2 and Table 2.2: the PRN, the m-sequence of
//     X^23 + X^18 + 1, truncated to 38 400 chips per arm, from the four
//     tabulated register presets (normal and self-test, I and Q), checked
//     against the table's first 64 chips;
//   - 2.3.3: OQPSK, I leading Q by half a chip;
//   - 3.1 and Table 3.1: 154 main field bits, a 48-bit rotating field, 48
//     BCH bits; TAC, serial number, country code, homing and RLS flags, test
//     protocol flag, the encoded GNSS location with its "no location"
//     defaults, the vessel ID types 0 to 7 with the Table 3.2 modified
//     Baudot code (the same code as T.001 Table A3), the beacon type;
//   - 3.3 and 3.4: the rotating field identifier, bits 155 to 158, of which
//     15 is the cancellation message; the other rotating fields are passed
//     through as bits, not decoded;
//   - 3.5 and Appendix B: BCH(250,202), shortened from BCH(255,207), t = 6,
//     its printed generator, checked against Appendix B's worked example;
//   - 3.6 and Table 3.10: the 23 Hex ID.
//
// THE RECEIVER
//
// T.018 specifies the transmitter; the receiver is this file's design.
// Rectangular chips are assumed (2.3.2 allows filtered rectangular among
// others), so the matched filter is a one-chip moving sum. A burst is found
// by differential correlation of the matched filter's output at half-chip
// spacing against the preamble: each sample times the conjugate of the one a
// chip earlier, correlated with the same product of the known preamble
// waveform for each of the two modes. A carrier offset turns every product
// by the same angle, so it does not reduce the correlation. Timing is then
// refined to a sample, the carrier offset read from the peak of a
// zero-padded FFT of the despread preamble, and the burst despread bit by
// bit with a second order phase loop (decision directed after the preamble)
// and an early-late chip timing loop, which follows the 2.3.1.2 chip rate
// tolerance and Doppler on the chip clock. Hard decisions go to the BCH
// decoder.
//
// The differential detector loses against coherent detection at low SNR,
// so acquisition, not decoding, sets this receiver's sensitivity:
// tests/decode/test_cospas_sarsat.cpp measured every burst at C/N0 38 dB-Hz,
// 8 in 10 at 36 and none at 34, with a 1 kHz offset (2026-11-02).
//
// CLEAN ROOM
//
// No second-generation beacon decoder or encoder was read.

inline constexpr double kSgbChipRate = 38400.0;            // 2.2.3 f), 2.3.1.2
inline constexpr std::size_t kSgbChipsPerArm = 38400;       // 2.2.3 e)
inline constexpr std::size_t kSgbPreambleChips = 6400;      // 2.2.4
inline constexpr std::size_t kSgbChipsPerBit = 256;         // 2.2.3 b)
inline constexpr std::size_t kSgbMessageBits = 250;         // 2.2.6
inline constexpr std::size_t kSgbInformationBits = 202;     // 3.1
inline constexpr std::size_t kSgbMainFieldBits = 154;       // 3.1

enum class SgbMode : std::uint8_t { Normal, SelfTest };

// T.018 Table 2.2 and Figure 2-2: the 38 400 chips of one arm, logic 0 or 1,
// first chip first.
[[nodiscard]] std::vector<std::uint8_t> sgb_prn(SgbMode mode, bool q_arm);

// 3.5 and Appendix B: fills bits 203 to 250 from bits 1 to 202. One bit per
// byte; bits[0] is bit 1.
void sgb_fill_bch(std::span<std::uint8_t> bits);

struct SgbMessage {
    std::vector<std::uint8_t> bits;  // 250, bits[0] is bit 1, as corrected
    bool self_test = false;          // sent with the self-test PRN
    std::size_t bch_corrected = 0;
    // Table 3.1.
    unsigned tac = 0;
    unsigned serial = 0;
    unsigned country_code = 0;
    bool homing = false;
    bool rls = false;
    bool test_protocol = false;
    // Nothing when bits 44 to 90 hold the "no location" defaults.
    std::optional<double> latitude;   // degrees, north positive
    std::optional<double> longitude;  // degrees, east positive
    unsigned vessel_id_type = 0;      // bits 91-93
    std::string identity;             // as the vessel ID type carries it
    std::optional<std::uint32_t> mmsi;
    std::optional<std::uint32_t> aircraft_address;
    unsigned beacon_type = 0;  // bits 138-140
    // 3.3: the rotating field identifier and its 48 bits.
    unsigned rotating_field = 0;
    bool cancellation = false;  // rotating field 15
    std::string hex_id;         // 3.6, 23 characters
};

[[nodiscard]] const char* sgb_beacon_type_name(unsigned type);

// Decodes 250 bits. Fails on another length or when the BCH decoder refuses
// the word.
[[nodiscard]] Expected<SgbMessage> sgb_parse(std::span<const std::uint8_t> bits,
                                             SgbMode mode = SgbMode::Normal);

struct SgbDecoderConfig {
    // A whole, even number of samples per chip: 76 800, 153 600, 307 200 S/s.
    dsp::SampleRate rate = 153600;
    // Normalised differential preamble correlation, 0 to 1, above which a
    // burst is taken to start. Noise reads about 1/sqrt(12800), 0.009; the
    // value is a choice: for a Rayleigh-distributed noise metric it is 5.6
    // standard deviations, and a false start costs one failed BCH decode.
    double threshold = 0.05;
};

struct SgbBurst {
    SgbMessage message;
    std::uint64_t sample_position = 0;  // the first preamble chip
    double frequency_offset_hz = 0.0;   // from the preamble
    double chip_rate = 0.0;             // mean over the burst, as tracked
    double sync_metric = 0.0;
};

class SgbDecoder {
public:
    [[nodiscard]] static Expected<SgbDecoder> create(const SgbDecoderConfig& config);

    // Complex baseband with the beacon within a few kilohertz of zero.
    // Chunks may split anywhere: the same stream gives the same bursts.
    void push(dsp::ConstComplexSpan in, std::vector<SgbBurst>& out);
    void reset();
    [[nodiscard]] std::size_t bursts_failed() const { return failed_; }

private:
    SgbDecoder() = default;
    void correlate_block(std::vector<SgbBurst>& out);
    void finish_ready(std::vector<SgbBurst>& out);
    void decode_burst(std::int64_t start, SgbMode mode, double metric,
                      std::vector<SgbBurst>& out);
    [[nodiscard]] std::complex<double> filtered(double sample) const;

    SgbDecoderConfig config_;
    std::size_t sps_ = 0;  // samples per chip
    // Differential templates of the two modes, length kTemplate, and the
    // FFTs of their zero-padded conjugates.
    std::array<std::vector<std::complex<double>>, 2> template_;
    std::array<std::vector<std::complex<double>>, 2> template_fft_;
    std::array<std::vector<std::uint8_t>, 2> prn_i_;
    std::array<std::vector<std::uint8_t>, 2> prn_q_;
    double template_energy_ = 0.0;

    // Raw input from raw_base_, and its running sum for the matched filter.
    std::vector<std::complex<double>> prefix_;  // prefix_[i]: sum of raw before raw_base_ + i
    std::int64_t raw_base_ = 0;
    std::int64_t total_ = 0;
    std::complex<double> running_{};

    // Differential products on the half-chip grid, from grid index d_base_.
    std::vector<std::complex<double>> diff_;
    std::int64_t d_base_ = 0;
    std::int64_t next_block_ = 0;  // grid index of the next correlation block

    // A correlation peak waiting to be confirmed as the largest nearby.
    bool have_candidate_ = false;
    std::int64_t candidate_ = 0;
    double candidate_metric_ = 0.0;
    SgbMode candidate_mode_ = SgbMode::Normal;
    std::int64_t holdoff_until_ = 0;  // grid index

    struct Pending {
        std::int64_t start = 0;  // sample of the first preamble chip
        SgbMode mode = SgbMode::Normal;
        double metric = 0.0;
    };
    std::vector<Pending> pending_;
    std::size_t failed_ = 0;
};

}  // namespace revenant::decode
