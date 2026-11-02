// ERMES: receiver audio or complex baseband to batches, addresses and
// messages.
//
// SPECIFICATION
//
// ETSI ETS 300 133-4, "Paging Systems (PS); European Radio Message System
// (ERMES); Part 4: Air interface specification", July 1992, from
// www.etsi.org/deliver/etsi_i_ets/300100_300199/30013304/01_60/, read
// 2026-11-02. Cited below as "133-4" with a clause. The character sets of
// Annex B come from ETS 300 133-2, July 1992, "Part 2: Service aspects", same
// site, read the same day, cited as "133-2". ETSI retains copyright in both;
// the constants here are the clause-cited facts the decoder needs, not
// reproductions of the documents' tables.
//
// The PDF of 133-4 sets its figures in a font whose glyphs are offset by 29
// code points from ASCII, so a plain text extraction loses every field width.
// The widths below were read from the figures after undoing that offset, and
// each one is checked against a total the prose states elsewhere: the 36-bit
// system information is two codewords, the 36-bit message header of 6.5 is
// 22 + 5 + 1 + 1 + 7, and the 35-bit RIC of clause 3 is 3 + 7 + 3 + 18 + 4.
//
// WHAT IS DECODED
//
//   - 4-PAM/FM at 3125 baud, 9.3: the Gray mapping of 9.3.1, from an FM
//     discriminator, either this file's or the engine's NFM audio;
//   - the batch of 5.4: preamble and synchronisation word, 6.2; system
//     information and supplementary system information, 6.3; the address
//     partition and its terminators, 6.4; the interleaved codeblocks of
//     clause 8;
//   - the shortened cyclic (30,18) code of clause 7, hard decision, two
//     errors corrected;
//   - message headers, 6.5.1, the additional information field and external
//     operator identity, 6.5.2, and message data, 6.5.3: numeric and
//     alphanumeric text through 133-2 Annex B, transparent data as bits,
//     long-message link counters, 11.4, and messages continued into the next
//     batch, 11.5.
//
// WHERE IT STOPS
//
// Messages are reported per message header. Joining the sub-messages of a
// long message, 11.4, and following a common temporary address through its
// three stages, 11.3, are a pager's bookkeeping across batches and are left
// to the caller: every field needed for both is reported.
//
// DEVIATIONS
//
//   - 133-2 Table B.2 is a national-variant 7-bit set. Its columns 2 to 7
//     agree with ASCII except at a handful of national positions, which are
//     reported as the ASCII character at the same code; columns 0 and 1 are
//     reported as LF and CR where the table puts them and as '?' otherwise.
//   - A transmitter may stop after any completed codeblock, 5.5 and 6.5.5. A
//     codeblock in which five or more of nine codewords are uncorrectable is
//     taken as the transmitter having stopped. That threshold is an
//     engineering choice, not from the document.
//
// CLEAN ROOM
//
// No ERMES implementation was read. Everything here is from the two
// documents and textbook receiver structure.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/decode/dv_phy.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::SampleIndex;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// 133-4 9.3.2.
inline constexpr double kErmesSymbolRate = 3125.0;

// 133-4 9.3.1: the outer carriers sit at +/-4687.5 Hz and the inner at
// +/-1562.5 Hz, so 1562.5 Hz per unit of a +/-1, +/-3 alphabet.
inline constexpr double kErmesDeviationPerUnitHz = 1562.5;

// 133-4 9.3.1, indexed by dibit value, first-transmitted bit most
// significant: 00 at -4687.5, 01 at -1562.5, 10 at +4687.5, 11 at +1562.5.
// The signs are in the figure on page 24, which a plain text extraction
// drops; the Gray property (adjacent levels differ in one bit) confirms them.
inline constexpr std::array<int, 4> kErmesDibitToLevel = {-3, -1, +3, +1};

// 133-4 clause 7: a codeword is 30 bits, 18 information and 12 check.
inline constexpr std::size_t kErmesCodewordBits = 30;
inline constexpr std::size_t kErmesInfoBits = 18;
inline constexpr std::size_t kErmesCodewordSymbols = 15;

// 133-4 clause 7: g(x) = x^12 + x^11 + x^9 + x^7 + x^6 + x^3 + x^2 + 1,
// octal 15315 (printed with an "8" subscript as 153158).
inline constexpr std::uint32_t kErmesGenerator = 015315;

// 133-4 6.2: preamble and synchronisation word SW1, 30 bits each.
inline constexpr std::uint32_t kErmesPreamble = 0b001000100010001000100010001000U;
inline constexpr std::uint32_t kErmesSyncWord = 0b100010100010000010100000101010U;

// 133-4 6.4: the address partition terminator.
inline constexpr std::uint32_t kErmesApt = 0b100100111000011000100011100100U;

// 133-4 6.5.4: the message delimiter.
inline constexpr std::uint32_t kErmesDelimiter = 0b110101011110011111101110111011U;

// 133-4 5.1: fifteen batches of 154 codewords, then one of 190, in a
// subsequence of 2500 codewords (5.5).
inline constexpr std::size_t kErmesBatchCodewords = 154;
inline constexpr std::size_t kErmesLastBatchCodewords = 190;

// 133-4 5.4: preamble, sync, two system information words and one of
// supplementary system information precede the address partition.
inline constexpr std::size_t kErmesHeaderCodewords = 5;

// 133-4 clause 8: codeblocks of nine codewords.
inline constexpr std::size_t kErmesCodeblockCodewords = 9;

// 133-2 Annex B, Table B.2 note 1: DC1 is the end of message character.
inline constexpr std::uint8_t kErmesEom = 0x11;

// 133-2 Annex B, Table B.1: 1011 is the space.
inline constexpr std::uint8_t kErmesNumericSpace = 0xB;

// ---------------------------------------------------------------------------
// The code
// ---------------------------------------------------------------------------

// 133-4 clause 7: 18 information bits to the 30-bit codeword, information in
// bits 29..12.
[[nodiscard]] std::uint32_t ermes_encode(std::uint32_t info);

struct ErmesCodeword {
    std::uint32_t info = 0;
    int corrected = 0;     // bits corrected, 0 to 2
    bool valid = false;    // false when more than two errors were detected
};

// True when a received word is within two bits of the address partition
// terminator or message delimiter, which are not codewords.
[[nodiscard]] bool ermes_is_structure_word(std::uint32_t word, std::uint32_t pattern);

// Hard decision, up to two errors corrected.
[[nodiscard]] ErmesCodeword ermes_decode(std::uint32_t word);

// 133-4 clause 8: nine codewords to the 270 bits in transmit order, and back.
[[nodiscard]] std::array<std::uint8_t, 270> ermes_interleave(std::span<const std::uint32_t, 9> words);
[[nodiscard]] std::array<std::uint32_t, 9> ermes_deinterleave(std::span<const std::uint8_t, 270> bits);

// 133-4 5.4: the index of the first message-partition codeword in a batch
// whose address partition carries `initial_addresses` addresses.
[[nodiscard]] std::size_t ermes_message_partition_start(std::size_t initial_addresses);

// 133-2 Annex B.
[[nodiscard]] char ermes_numeric_char(std::uint8_t nibble);
[[nodiscard]] char ermes_alpha_char(std::uint8_t code);

// ---------------------------------------------------------------------------
// Decoded structures
// ---------------------------------------------------------------------------

// 133-4 6.3.1.
struct ErmesSystemInfo {
    std::uint8_t country = 0;    // 7 bits
    std::uint8_t op = 0;         // 3 bits
    std::uint8_t paging_area = 0;  // 6 bits
    bool eti = false;
    bool bai = false;
    std::uint8_t fsi = 0;        // 5 bits
    std::uint8_t cycle = 0;      // 6 bits, 0 to 59
    std::uint8_t ssn = 0;        // 3 bits, 0 to 4
    std::uint8_t batch = 0;      // 4 bits, A = 0 to P = 15
};

// 133-4 6.3.2.
struct ErmesSupplementary {
    std::uint8_t ssit = 0;
    std::uint16_t ssif = 0;  // 14 bits

    // SSIT 0000, 6.3.2.1.
    std::uint8_t zone = 0;
    std::uint8_t hour = 0;
    std::uint8_t date = 0;

    // SSIT 0001, 6.3.2.2.
    std::uint8_t day_of_week = 0;
    std::uint8_t month = 0;
    std::uint16_t year = 0;  // 1990 + the 7-bit field
};

enum class ErmesCategory : std::uint8_t { Tone = 0, Numeric = 1, Alphanumeric = 2, Transparent = 3 };

struct ErmesMessage {
    ErmesSystemInfo system;          // of the batch the header was in
    SampleIndex first_sample = 0;    // of that batch's preamble

    // 6.5.1.
    std::uint32_t local_address = 0;  // 22 bits
    std::uint32_t initial_address = 0;  // its top 18
    std::uint8_t batch_bits = 0;        // its bottom 4
    std::uint8_t message_number = 0;
    bool external = false;
    bool aii = false;
    std::uint8_t vif = 0;

    // AII = 0, 6.5.1.1; for long messages, 6.5.1.2.1, taken from the AIN and AIF.
    ErmesCategory category = ErmesCategory::Tone;
    bool urgent = false;
    std::uint8_t alert = 0;

    // AII = 1, 6.5.1.2.
    std::uint8_t ait = 0;
    std::uint8_t ain = 0;
    std::optional<std::uint16_t> operator_identity;  // 13 bits, when external
    std::vector<std::uint8_t> aif;                   // the AIF's bits, one per byte
    std::optional<std::uint16_t> link_counter;       // AIT 001, 11.4

    // 6.5.3.
    std::string text;                 // numeric or alphanumeric
    std::vector<std::uint8_t> data;   // transparent data, one bit per byte
    std::size_t message_bits = 0;     // bits counted toward a link counter

    // True when the message continued from a previous batch, 11.5.
    bool continued = false;
    // True when it ran to the end of the stream without a delimiter.
    bool truncated = false;

    int corrected_bits = 0;
    int uncorrectable = 0;
};

struct ErmesBatch {
    SampleIndex first_sample = 0;
    bool inverted = false;
    double sync_score = 0.0;
    std::size_t codewords = 0;  // as received, up to 154 or 190

    ErmesSystemInfo system;
    std::optional<ErmesSupplementary> supplementary;

    // 6.4: the 18-bit initial addresses, highest first as sent.
    std::vector<std::uint32_t> initial_addresses;

    int corrected_bits = 0;
    int uncorrectable = 0;
};

struct ErmesOutput {
    std::vector<ErmesBatch> batches;
    std::vector<ErmesMessage> messages;
};

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

struct ErmesConfig {
    SampleRate rate = 48000;

    // Normalised correlation over the 30 symbols of preamble and sync word
    // that starts a batch. NOT A SPECIFIED VALUE: chosen so that two symbols
    // in thirty may be wrong. The two system information codewords that
    // follow must also decode, which is what stops noise starting a batch.
    double sync_threshold = 0.8;
};

struct ErmesStats {
    std::uint64_t batches = 0;
    std::uint64_t codewords = 0;
    std::uint64_t corrected_bits = 0;
    std::uint64_t uncorrectable = 0;
    std::uint64_t sync_rejected = 0;  // sync found, system information failed
};

class ErmesDecoder {
   public:
    [[nodiscard]] static Expected<ErmesDecoder> create(const ErmesConfig& config);

    // FM discriminator audio at config.rate. Any scale and either polarity:
    // levels are fitted to each batch's preamble and sync word.
    void process_audio(ConstRealSpan audio, ErmesOutput& out);

    // Complex baseband at config.rate, the carrier near zero hertz.
    void process(ConstComplexSpan samples, ErmesOutput& out);

    // End of stream: a message still open is emitted, marked truncated.
    void flush(ErmesOutput& out);

    [[nodiscard]] const ErmesStats& stats() const { return stats_; }

    void reset();

   private:
    ErmesDecoder() = default;

    enum class State : std::uint8_t { Search, Header, Address, Codeblocks };

    void run(ErmesOutput& out);
    [[nodiscard]] float sample_at(double position) const;
    [[nodiscard]] double symbol_position(std::size_t symbol) const;
    [[nodiscard]] bool have_symbols(std::size_t count) const;
    [[nodiscard]] std::uint32_t read_word(std::size_t symbol) const;
    [[nodiscard]] float read_level(std::size_t symbol) const;
    bool try_sync(double& best_score, bool& inverted, double& best_position);
    void begin_batch(ErmesOutput& out);
    void feed_message_word(const ErmesCodeword& cw, bool delimiter, ErmesOutput& out);
    void finish_message(ErmesOutput& out, bool truncated);
    void trim();

    ErmesConfig config_{};
    double sps_ = 0.0;
    FmDiscriminator discriminator_{};
    RealFir filter_{};
    std::size_t filter_delay_ = 0;

    std::vector<float> buffer_;
    SampleIndex buffer_start_ = 0;  // stream index of buffer_[0], filter delay removed
    SampleIndex search_from_ = 0;

    State state_ = State::Search;
    double batch_origin_ = 0.0;  // stream position of the preamble's first symbol
    bool inverted_ = false;
    double gain_ = 1.0;
    double offset_ = 0.0;
    std::size_t batch_length_ = 0;
    std::size_t word_index_ = 0;
    std::size_t message_start_ = 0;
    ErmesBatch batch_{};
    bool have_previous_batch_ = false;
    SampleIndex expected_next_ = 0;

    // Message parser state, which survives across batches, 11.5.
    enum class Parse : std::uint8_t { Idle, AfterDelimiter, Header2, Extra, Data };
    Parse parse_ = Parse::Idle;
    std::uint32_t header1_ = 0;
    std::vector<std::uint8_t> extra_bits_;
    std::size_t extra_words_ = 0;
    std::vector<std::uint8_t> data_bits_;
    std::optional<ErmesMessage> open_;

    ErmesStats stats_{};
};

// Parses the bits of a finished message into text or data, 6.5.3. Exposed
// for the tests.
void ermes_fill_payload(ErmesMessage& message, std::span<const std::uint8_t> bits);

}  // namespace revenant::decode
