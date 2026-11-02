// EnOcean ERP1: complex baseband to subtelegrams.
//
// SPECIFICATION
//
// EnOcean Radio Protocol 1 (ERP1), EnOcean GmbH, specification V1.2,
// November 9, 2020, from
// https://www.enocean.com/wp-content/uploads/Knowledge-Base/EnOceanRadioProtocol1.pdf,
// downloaded 2026-11-02. Free from the manufacturer. ISO/IEC 14543-3-10,
// which standardises the same protocol, is purchasable and was not read.
//
// Clauses used: 2 and Figure 1 (subtelegram structure, 21-byte maximum),
// 3 and its parameter table (868.3 MHz, ASK, 125 kbit/s, 8/12 coding,
// length by EOF), 1.6 (5 % data rate tolerance), 1.7 and its coding table
// (inverted air polarity, 8-to-12 coding, Sync/EOF pair), 1.8 (preamble),
// 1.9 (start of frame), 4.2 and Table 1 (RX maturity time), 4.3 and
// Table 3 (hash selection by STATUS bit 7), 4.3.2 (checksum), 4.3.3 (CRC),
// 5.2.3 and Table 4 (repeater level bits), 5.3.2 and Figure 3
// (addressed telegram encapsulation, RORG 0xA6).
//
// The clause numbers inside chapter 3 of the PDF are printed as 1.1 to 1.12;
// they are cited here as printed.
//
// DEVIATIONS
//
// Start of frame variant 1 (clause 1.9) is a code violation of 1.5 bit
// periods high, 1.5 low and 1 high, and is not recognised: only variant 2,
// the four bits 1001, which the clause requires of new transmitters. A
// receiver that must hear older devices needs the variant 1 detector added.
//
// docs/modes.md describes the coding as Manchester. ERP1 clause 1.7 says 8
// to 12 coding with inverse and sync bits, and this follows the document.
//
// Clause 2 says STATUS is absent from a switch telegram but gives no rule
// for recognising one from the air. The hash is checked as Table 3 says,
// with STATUS taken as the byte before HASH; a subtelegram without STATUS
// passes only when its last data byte happens to select the right hash.
//
// WHAT THIS TAKES AS INPUT
//
// Complex baseband centred on the carrier at any rate from about 4 samples
// per bit (500 kS/s) upward. ASK is detected on the envelope, so a
// frequency offset well inside the sample-rate bandwidth needs no
// correction.
//
// WHERE THIS STOPS
//
// At subtelegrams: RORG, data, sender id, STATUS, the repeater count, and the
// destination of an addressed telegram. Equipment Profiles (what the data
// bytes mean for a given RORG and device) are a separate document and are not
// applied. Each subtelegram is reported; a repeat of one already reported
// within the RX maturity time is flagged rather than dropped.
//
// CLEAN ROOM
//
// No EnOcean implementation was read.

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

// ERP1 chapter 3 parameter table: data rate 125 kbit/s (124.875 to 125.125).
inline constexpr double kEnoceanBitRate = 125000.0;
// ERP1 chapter 3 parameter table: carrier 868.3 MHz.
inline constexpr double kEnoceanCarrierHz = 868.3e6;
// ERP1 clause 2: the maximal valid total length is 21 bytes.
inline constexpr std::size_t kEnoceanMaxBytes = 21;
// Figure 1: RORG, at least one data byte, 4 TXID, STATUS, HASH.
inline constexpr std::size_t kEnoceanMinBytes = 8;
// Clause 1.8: 8 preamble bits 10101010, then clause 1.9 variant 2 SOF 1001.
// Logical bits, first transmitted in the most significant position.
inline constexpr std::uint32_t kEnoceanPreambleSof = 0b1010'1010'1001U;
inline constexpr unsigned kEnoceanPreambleSofBits = 12;
// Clause 1.7: 12 encoded bits per byte.
inline constexpr unsigned kEnoceanEncodedBits = 12;
// Clause 1.7: Sync/EOF pair "01" means another byte follows, "10" ends.
inline constexpr unsigned kEnoceanSyncMore = 0b01U;
inline constexpr unsigned kEnoceanSyncEof = 0b10U;
// Clause 5.3.2: RORG of an addressed (encapsulated) telegram.
inline constexpr std::uint8_t kEnoceanRorgAddressed = 0xA6;
// Clause 4.2, Table 1: RX maturity time, 100 ms.
inline constexpr double kEnoceanRxMaturitySeconds = 0.100;

// Clause 4.3.2: the byte sum of every byte except HASH, overflow ignored.
[[nodiscard]] std::uint8_t enocean_checksum(std::span<const std::uint8_t> bytes);
// Clause 4.3.3: remainder modulo x^8 + x^2 + x + 1, starting from RORG.
[[nodiscard]] std::uint8_t enocean_crc8(std::span<const std::uint8_t> bytes);
// Clause 1.7: one byte to its 12 encoded bits, first transmitted in bit 11.
// `last` selects the EOF pair rather than the sync pair.
[[nodiscard]] std::uint16_t enocean_encode_byte(std::uint8_t byte, bool last);

struct EnoceanTelegram {
    // Sample index of the first data bit after the start of frame.
    dsp::SampleIndex sample = 0;
    // RORG through HASH as received.
    std::vector<std::uint8_t> bytes;
    std::uint8_t rorg = 0;
    // For an addressed telegram, the encapsulated RORG; otherwise rorg.
    std::uint8_t inner_rorg = 0;
    std::vector<std::uint8_t> data;
    std::uint32_t sender_id = 0;
    std::optional<std::uint32_t> destination_id;
    std::uint8_t status = 0;
    // Table 4: 0 original, 1 or 2 hops, 15 "shall not be repeated".
    unsigned repeater_level = 0;
    // Table 3: true when STATUS bit 7 selected the CRC, false the checksum.
    bool crc8 = false;
    // Same bytes as a subtelegram reported within the RX maturity time.
    bool repeat = false;
    // High (logical 0) envelope level, linear, for a signal strength readout.
    float level = 0.0F;
};

struct EnoceanConfig {
    dsp::SampleRate sample_rate = 1'000'000;
    double bit_rate = kEnoceanBitRate;
};

class EnoceanDecoder {
   public:
    EnoceanDecoder() = default;
    [[nodiscard]] static Expected<EnoceanDecoder> create(const EnoceanConfig& config);

    void process(dsp::ConstComplexSpan samples, std::vector<EnoceanTelegram>& out);
    void reset();

   private:
    void on_bit(unsigned bit, std::vector<EnoceanTelegram>& out);
    void finish(std::vector<EnoceanTelegram>& out);

    EnoceanConfig config_{};
    double samples_per_bit_ = 8.0;
    std::size_t box_length_ = 8;

    // Envelope smoothing.
    std::vector<float> box_;
    std::size_t box_pos_ = 0;
    double box_sum_ = 0.0;
    double high_ = 0.0;
    double low_ = 0.0;
    double high_decay_ = 0.0;
    double low_rise_ = 0.0;

    // Bit clock.
    double phase_ = 0.0;
    bool level_high_ = false;
    dsp::SampleIndex sample_ = 0;

    // Framing.
    bool in_frame_ = false;
    std::uint32_t shift_ = 0;
    unsigned group_bits_ = 0;
    std::uint16_t group_ = 0;
    std::vector<std::uint8_t> bytes_;
    dsp::SampleIndex frame_sample_ = 0;
    float frame_level_ = 0.0F;

    // Repeat flagging.
    std::vector<std::uint8_t> last_bytes_;
    dsp::SampleIndex last_sample_ = 0;
    bool have_last_ = false;
};

}  // namespace revenant::decode
