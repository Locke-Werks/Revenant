// POCSAG, Radio Paging Code No. 1: receiver audio to pages.
//
// SPECIFICATION
//
// The code and format are Recommendation ITU-R M.584-2 (11/97), "Codes and
// formats for radio paging", Annex 1, read from the ITU's own publication
// service. Clauses used: 1.1 (preamble), 1.2 (batches and frames), 1.3 and
// Figure 2 (codeword types), Table 1 (the synchronization codeword), 1.3.2
// (address codewords), 1.3.3 (message codewords), Table 2 (the idle
// codeword), 1.4 (the BCH(31,21) code and the parity bit), 2.1 and Table 3
// (numeric messages), 2.2 (alphanumeric messages) and 2.5.1 (end of
// message).
//
// M.584-2 is silent on the air interface. The bit rates and the polarity come
// from Recommendation ITU-R M.539-3 (09/94), "Technical and operational
// characteristics of international radio-paging systems", clause 4.3: 512 or
// 1200 bit/s, direct FSK, "a positive frequency shift representing binary 0, a
// negative frequency shift for binary 1", e.g. plus and minus 4.5 kHz in a
// 25 kHz channel. M.539 was withdrawn in 2007 and is cited as the last
// document to state these, read from the ITU's archive of withdrawn texts.
// 2400 bit/s appears in neither document and is practice; the rate is a
// parameter, so the decoder does not need a document for it.
//
// WHAT THIS TAKES AS INPUT
//
// The audio an FM receiver makes of the channel, where the RF shift has
// become an audio level. Clause 4.3's polarity puts binary 0 on positive
// audio. A receiver whose discriminator is inverted, or that sits on the
// other side of a mixer, delivers the opposite, so the decoder looks for the
// synchronization codeword in both polarities and reports which it found.
//
// WHERE THIS STOPS
//
// At pages: the 21-bit identity, the function bits, and the message as M.584
// defines it for function bits 00 (numeric) and 11 (alphanumeric). Function
// bits 01 and 10 have no message format in M.584; their message codewords are
// handed out raw, and pocsag_numeric and pocsag_alphanumeric are public so a
// caller who knows a network's practice can apply either. One rate per
// decoder: a caller wanting all three runs three.
//
// CLEAN ROOM
//
// No POCSAG implementation was read.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/decode/fsk.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// ---------------------------------------------------------------------------
// Specified constants
// ---------------------------------------------------------------------------

// M.539-3 clause 4.3.
inline constexpr double kPocsag512 = 512.0;
inline constexpr double kPocsag1200 = 1200.0;
// Practice, not in M.584-2 or M.539-3.
inline constexpr double kPocsag2400 = 2400.0;

// M.584-2 clause 1.1: reversals for at least 576 bits, a batch plus a
// codeword.
inline constexpr std::size_t kPocsagPreambleBits = 576;

// Clause 1.2 and the Figure 1 note: a batch is the synchronization codeword
// and 8 frames of 2 codewords, 17 codewords in all.
inline constexpr std::size_t kPocsagFrames = 8;
inline constexpr std::size_t kPocsagCodewordsPerBatch = 16;
inline constexpr std::size_t kPocsagCodewordBits = 32;

// Table 1, bits 1 to 32 with bit 1 as the most significant: clause 1.3 sends
// the most significant bit first.
inline constexpr std::uint32_t kPocsagSync = 0x7CD215D8U;

// Table 2, WITH ONE BIT CORRECTED. As printed, Table 2 reads 0x7AC9C197, and
// that is not a code word of the clause 1.4 code: its syndrome is not zero.
// Clause 1.3.4 says in words that the idle codeword "is a valid address
// codeword", so the table contradicts its own clause. Exactly one code word
// lies within one bit of the printed pattern, found by flipping each bit in
// turn and checking it against clause 1.4, and it differs in bit 10:
// 0x7A89C197. That is what this carries. A transmitter that sent the
// misprint would still be read as idle, because the decoder corrects the one
// bit before comparing. tests/decode/test_pocsag.cpp repeats the search.
inline constexpr std::uint32_t kPocsagIdle = 0x7A89C197U;
inline constexpr std::uint32_t kPocsagIdleAsPrinted = 0x7AC9C197U;

// Clause 1.4: x^10 + x^9 + x^8 + x^6 + x^5 + x^3 + 1.
inline constexpr std::uint32_t kPocsagGenerator = 0b111'0110'1001U;

// Clause 2.1 and 2.2: function bits 00 introduce a numeric message and 11 an
// alphanumeric one.
inline constexpr std::uint8_t kPocsagFunctionNumeric = 0b00;
inline constexpr std::uint8_t kPocsagFunctionAlphanumeric = 0b11;

// ---------------------------------------------------------------------------
// The code
// ---------------------------------------------------------------------------

// Clause 1.4: 21 information bits, the 10 check bits of the BCH(31,21) code,
// and a 32nd bit for even parity over the whole word. `information` holds
// bits 1 to 21 in its low 21 bits, bit 1 most significant.
[[nodiscard]] std::uint32_t pocsag_encode(std::uint32_t information);

struct PocsagCorrection {
    std::uint32_t word = 0;
    // Bits changed, 0 to 2. The BCH code has distance 5 and the parity bit
    // makes it 6, so two errors are corrected and three are detected rather
    // than miscorrected.
    int corrected_bits = 0;
    bool valid = false;
};

[[nodiscard]] PocsagCorrection pocsag_correct(std::uint32_t received);

// Clause 1.3.2 and 1.3.3.
[[nodiscard]] inline bool pocsag_is_message(std::uint32_t word) {
    return (word & 0x80000000U) != 0U;
}

// Clause 2.1: 4-bit characters, bit 1 first, five to a codeword. `bits` is
// the message bits of consecutive message codewords in transmission order,
// one per byte. Table 3's two bracket glyphs are "]" for 1110 and "[" for
// 1111; its "Spare" combination, 1010, has no glyph and comes out as U+FFFD.
[[nodiscard]] std::string pocsag_numeric(std::span<const std::uint8_t> bits);

// Clause 2.2: 7-bit International Alphabet No. 5, bit 1 first, packed across
// codeword boundaries. Null characters, and trailing End of Text and End of
// Transmission, which the clause names as fill, are dropped; a partial
// character at the end is fill and is dropped too.
[[nodiscard]] std::string pocsag_alphanumeric(std::span<const std::uint8_t> bits);

// ---------------------------------------------------------------------------
// What comes out
// ---------------------------------------------------------------------------

struct PocsagPage {
    // Clause 1.3.2 with clause 1.2: the 18 address bits followed by the 3
    // bits of the frame the address codeword arrived in.
    std::uint32_t identity = 0;
    std::uint8_t function = 0;

    enum class Format : std::uint8_t { Numeric, Alphanumeric, Unspecified };
    Format format = Format::Unspecified;

    // The message, decoded per the function bits, empty for Unspecified.
    std::string text;

    // The message bits of every message codeword, 20 each, in transmission
    // order, one per byte. An uncorrectable codeword contributes its bits
    // as received.
    std::vector<std::uint8_t> message_bits;

    // Audio sample index of the address codeword's first bit.
    SampleIndex position = 0;

    // Bits the BCH decode changed across the address and message codewords,
    // and how many message codewords it could not correct.
    int corrected_bits = 0;
    int uncorrectable_codewords = 0;

    // The synchronization codeword was found complemented.
    bool inverted = false;
};

struct PocsagConfig {
    SampleRate rate = 48000;
    double bit_rate = kPocsag1200;

    // Bit errors allowed in a received synchronization codeword. An
    // engineering choice: two is what the codeword itself is corrected to,
    // and the chance that 32 bits of reversal preamble or data sit within
    // two bits of the sync pattern is what keeps it from being higher. The
    // same tolerance applies to the 32 preamble bits clause 1.1 puts before
    // the first synchronization codeword; see PocsagDecoder.
    int sync_tolerance = 2;

    // Bits an address codeword may have corrected, the parity bit included,
    // and still start a page. An engineering choice made from clause 1.4:
    // the BCH(31,21) code with the even parity bit has distance 6, so a
    // decoder that corrects t bits can also detect 5 - t. At two, the most
    // it can correct, a word with four errors can land within two bits of a
    // different code word and is read as that address; at one, every word
    // with two to four errors is refused. Measured in
    // tests/decode/test_pocsag_false_pages.cpp: at 6 dB in 2500 Hz, most of
    // the addresses nobody sent had two bits corrected, many of them an idle
    // codeword, which clause 1.3.4 says is never a pager's, read one code word
    // over. Message codewords are corrected to two regardless: a wrong
    // character is a wrong character, not a page for somebody else.
    int address_correction_budget = 1;
};

struct PocsagStats {
    std::uint64_t batches = 0;
    std::uint64_t codewords = 0;
    std::uint64_t corrected_bits = 0;
    std::uint64_t uncorrectable = 0;

    // Address codewords the BCH decode corrected by more than
    // PocsagConfig::address_correction_budget, and so refused.
    std::uint64_t addresses_refused = 0;

    // Batches synchronised without a preamble in front of them whose next
    // synchronization codeword never came, and the pages found in them,
    // which were dropped rather than reported; see PocsagDecoder.
    std::uint64_t batches_unconfirmed = 0;
    std::uint64_t pages_unconfirmed = 0;
};

// WHAT IT TAKES TO BE SYNCHRONISED
//
// A synchronization codeword found by search, within sync_tolerance bits, is
// taken at once when the 32 bits before it are the clause 1.1 preamble of
// reversals within the same tolerance: that is how a transmission starts, and
// clause 1.1 says the preamble is there to help a pager acquire batch
// synchronization. Found without one, the batch is provisional. Clause 2.3
// asks that a receiver which lost synchronization, or started after the
// preamble, "achieve synchronization on receipt of a number of valid
// batches", so the pages in a provisional batch are held until the next
// synchronization codeword arrives where clause 1.2 puts it, 17 codewords on,
// and dropped if it does not. One is the number taken here.
//
// That rule is what stops noise alone paging anyone. Noise sits within two
// bits of the synchronization codeword about once an hour at 1200 bit/s, and
// the batch that follows is sixteen random words, a quarter of which the code
// accepts: tests/decode/test_pocsag_false_pages.cpp measured 8.1 pages an
// hour across the three rates before it, and none after.
class PocsagDecoder {
public:
    [[nodiscard]] static Expected<PocsagDecoder> create(const PocsagConfig& config);

    // Consumes audio and appends every page whose end has been seen: the
    // next address or idle codeword, two indecipherable message codewords in
    // a row (clause 2.5.1), or the loss of batch synchronization. A page
    // found in a provisional batch is appended once the batch is confirmed.
    void process(ConstRealSpan audio, std::vector<PocsagPage>& out);

    // The stream has ended. Emits a page still open, because the codeword
    // that would have ended it is not coming: clause 1.2 has a transmission
    // end on an idle codeword, and when that codeword is the last thing in a
    // capture the receiver's own filter and clock delay leave it a bit short
    // of complete. Pages still held in a provisional batch are dropped, since
    // nothing now can confirm it. The next process() call starts a fresh
    // search for sync.
    void flush(std::vector<PocsagPage>& out);

    [[nodiscard]] const PocsagStats& stats() const { return stats_; }

    void reset();

private:
    PocsagDecoder() = default;

    void on_bit(std::uint8_t bit, SampleIndex sample, std::vector<PocsagPage>& out);
    void on_codeword(std::uint32_t word, SampleIndex sample, std::vector<PocsagPage>& out);
    void finish(std::vector<PocsagPage>& out);

    // Synchronisation is lost or the stream ended: held pages are dropped.
    void lose_sync();

    // The provisional batch is confirmed: held pages go out.
    void confirm(std::vector<PocsagPage>& out);

    PocsagConfig config_{};
    LevelDiscriminator discriminator_{};
    BitClock clock_{};
    std::vector<float> soft_;
    std::vector<SoftBit> bits_;

    // The last 64 bits, most recent in bit 0: the low 32 are the codeword
    // being assembled or searched for, the high 32 what came before it. And
    // how many bits have been seen, so the sync search does not fire on a
    // register not yet full.
    std::uint64_t shift_ = 0;
    std::uint64_t bit_count_ = 0;

    // False while the batch in hand was found without a preamble and its
    // next synchronization codeword has not yet arrived; the pages finished
    // meanwhile wait in held_.
    bool confirmed_ = false;
    std::vector<PocsagPage> held_;

    bool synced_ = false;
    bool inverted_ = false;
    std::size_t bit_in_word_ = 0;
    std::size_t word_in_batch_ = 0;
    SampleIndex word_sample_ = 0;
    bool expecting_sync_ = false;

    std::optional<PocsagPage> open_;
    int consecutive_bad_ = 0;
    PocsagStats stats_{};
};

}  // namespace revenant::decode
