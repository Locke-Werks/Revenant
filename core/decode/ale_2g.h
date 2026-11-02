// Second generation automatic link establishment (2G ALE): receiver audio to
// ALE words, and words to calls and soundings.
//
// SPECIFICATION
//
// MIL-STD-188-141D, 22 December 2017, Appendix A, distribution statement A
// (approved for public release, unlimited), read from everyspec.com. A US
// Government work; the tables below are transcribed from it. Clauses used:
//
//   A.5.1.2   eight tones, 750 to 2500 Hz, and the three bits each carries
//   A.5.1.3   125 symbols/s, 49 bits per word, 392 ms per triple
//   A.5.2.2.2 Golay (24,12) code, generator matrix of figure A-6, the
//             encoding example of figure A-8, syndrome decoding
//   A.5.2.2.3 figure A-10: the A and B halves, G13..G24 inverted, the
//             A1 B1 A2 B2 ... A24 B24 S49 transmit order
//   A.5.2.2.4 and tables A-VI, A-VII: 2/3 majority vote over three words,
//             and the count of unanimous votes
//   A.5.2.3   figure A-12 (word layout) and table A-VIII (preambles)
//   A.5.2.4.2 the Basic 38 ASCII subset; A.5.2.4.3 "@" stuffing, with its
//             "TO MIA", "DATA MI@" example
//   A.5.2.6.3 the synchronisation criteria a word must meet
//
// One reading is not stated in the document and is an assumption: which of
// a tone's three bits is sent first. A.5.1.2 prints each tone's bits with
// the LSB on the right, and figure A-12 note 2 says the MSB of a word is sent
// first; this takes the leftmost bit of a tone's triple as the earliest. A
// receiver that read it the other way would decode nothing at all, so a
// recording settles it at once.
//
// WHERE THIS STOPS
//
// At the words and the addresses in them. CMD orderwire words are reported
// raw. Linking protection (MIL-STD-188-141D Appendix B) scrambles the word
// stream under a key; a protected transmission fails the Golay and
// character checks here and reads as nothing, which is what it should read
// as. The AQC-ALE packed address word (A.5.8) is not interpreted.
//
// CLEAN ROOM
//
// No ALE implementation was read.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/decode/mfsk_demod.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// ---------------------------------------------------------------------------
// Specified constants
// ---------------------------------------------------------------------------

// A.5.1.2: tone 0 is 750 Hz, tones 250 Hz apart. A.5.1.3: 125 symbols/s.
inline constexpr double kAleToneBaseHz = 750.0;
inline constexpr double kAleToneSpacingHz = 250.0;
inline constexpr double kAleSymbolRate = 125.0;
inline constexpr int kAleTones = 8;

// A.5.1.3 and A.5.2.2.3: 48 interleaved bits plus the stuff bit S49.
inline constexpr std::size_t kAleWordBits = 49;
// A.5.2.2.4: every word sent three times; 147 bits is 49 symbols, 392 ms.
inline constexpr std::size_t kAleTripleBits = 3 * kAleWordBits;

// A.5.1.2: the three bits each tone carries, tone 0 (750 Hz) first.
inline constexpr std::array<std::uint8_t, 8> kAleToneBits = {0b000, 0b001, 0b011, 0b010,
                                                            0b110, 0b111, 0b101, 0b100};

// Table A-VIII. The value is W1 W2 W3, P3 the MSB.
enum class AlePreamble : std::uint8_t {
    Data = 0b000,
    Thru = 0b001,
    To = 0b010,
    Twas = 0b011,
    From = 0b100,
    Tis = 0b101,
    Cmd = 0b110,
    Rep = 0b111,
};

[[nodiscard]] const char* ale_preamble_name(AlePreamble p);

// ---------------------------------------------------------------------------
// Code
// ---------------------------------------------------------------------------

// A.5.2.2.2, figure A-6. Twelve data bits in, W1 (or W13) in bit 11; the
// twelve check bits out, G1 in bit 11.
[[nodiscard]] std::uint16_t ale_golay_parity(std::uint16_t data);

struct AleGolayDecode {
    std::uint16_t data = 0;
    // Bits corrected, or -1 when the syndrome is not that of any pattern of
    // up to max_errors bits (the uncorrectable flag of A.5.2.2.2.2).
    int errors = -1;
};

// A.5.2.6.3 lets the receiver choose a correction mode of 3/4, 2/5, 1/6 or
// 0/7; max_errors is the first figure.
[[nodiscard]] AleGolayDecode ale_golay_decode(std::uint16_t data, std::uint16_t parity,
                                              int max_errors = 3);

// Figure A-12: preamble in W1..W3, three 7-bit characters in W4..W24.
[[nodiscard]] std::uint32_t ale_word(AlePreamble preamble, const char chars[3]);
[[nodiscard]] AlePreamble ale_word_preamble(std::uint32_t word);
[[nodiscard]] std::array<char, 3> ale_word_chars(std::uint32_t word);

// Figure A-10: the 49 bits of one transmitted word, in transmit order.
[[nodiscard]] std::array<std::uint8_t, kAleWordBits> ale_word_bits(std::uint32_t word);

// A.5.2.4.2: A-Z, 0-9, "@" and "?".
[[nodiscard]] bool ale_basic38(char c);

// A.5.2.3.2.1 and A.5.2.4.3: a whole address as its routing word followed by
// alternating DATA and REP, the last word stuffed with "@". first must be a
// routing preamble (TO, TIS, TWAS, THRU or FROM).
[[nodiscard]] std::vector<std::uint32_t> ale_address_words(AlePreamble first,
                                                           const std::string& address);

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

struct Ale2gConfig {
    dsp::SampleRate rate = 8000;

    // A.5.2.6.3 leaves the unanimous vote threshold to the manufacturer.
    // Engineering choice: 30 of 48. Noise alone averages 12; a word whose
    // copies each carry 10% bit errors averages about 35.
    int unanimous_threshold = 30;
    int golay_max_errors = 3;

    // Frequency correction from the tone bank's AFC. The 250 Hz spacing
    // bounds what can be followed without a preamble at +/- 125 Hz.
    bool afc = true;
};

struct AleWord {
    // Sample index of the start of the symbol holding the word's first bit
    // in the third copy.
    std::int64_t position = 0;
    std::uint32_t word = 0;
    AlePreamble preamble = AlePreamble::Data;
    std::array<char, 3> chars{};
    int unanimous = 0;
    int corrected = 0;  // bit errors the two Golay decodes corrected
};

// A frame, assembled from words, once the transmission ends or the next one
// begins. A.5.2.3.2: TO names who is called; TIS or TWAS names who is
// calling or sounding, and with no TO before it the frame is a sound (A.5.3).
struct AleFrame {
    enum class Kind : std::uint8_t { Call, Sound, Unterminated };
    Kind kind = Kind::Unterminated;
    std::int64_t position = 0;
    std::vector<std::string> to;    // TO addresses, and THRU in a group call
    std::string from;               // the TIS or TWAS address
    bool accepting = false;         // TIS (true) or TWAS (false), A.5.2.3.2.2-3
    std::vector<std::uint32_t> commands;  // CMD words and what extends them
    std::vector<AleWord> words;
};

class Ale2gDecoder {
public:
    [[nodiscard]] static Expected<Ale2gDecoder> create(const Ale2gConfig& config);

    // Appends words and completed frames. The output depends only on the
    // concatenated input, never on how it was split into calls.
    void process(dsp::ConstRealSpan audio, std::vector<AleWord>& words,
                 std::vector<AleFrame>& frames);

    // Ends the frame in progress, as the end of a recording does.
    void flush(std::vector<AleFrame>& frames);

    void reset();

    [[nodiscard]] bool locked() const { return locked_; }

private:
    explicit Ale2gDecoder(const Ale2gConfig& config, MfskDemodulator demod);

    struct Candidate {
        bool ok = false;
        std::uint32_t word = 0;
        int unanimous = 0;
        int corrected = 0;
    };

    // Reads the majority word ending at bit index end (exclusive of S49
    // being the last bit, table A-VII).
    [[nodiscard]] Candidate read_at(std::int64_t end) const;
    [[nodiscard]] bool routing_ok(const Candidate& c) const;
    void on_bit(std::vector<AleWord>& words, std::vector<AleFrame>& frames);
    void accept(const Candidate& c, std::int64_t end, std::vector<AleWord>& words,
                std::vector<AleFrame>& frames);
    void end_frame(std::vector<AleFrame>& frames);

    Ale2gConfig config_;
    MfskDemodulator demod_;
    std::vector<MfskSymbol> symbols_;

    // Bit history, absolute index bit_base_ + i, with the sample position of
    // the symbol each came in.
    std::vector<std::uint8_t> bits_;
    std::vector<std::int64_t> bit_pos_;
    std::int64_t bit_base_ = 0;
    std::int64_t bit_count_ = 0;

    // Word phase (A.5.2.6.1): once locked, a word is read every 147 bits.
    bool locked_ = false;
    std::int64_t next_read_ = 0;
    int misses_ = 0;

    // Acquisition: a candidate is held for two more word periods, so that
    // the copy alignment with the most unanimous votes wins (a word with two
    // fresh copies and one stale one also decodes, one word early).
    bool pending_ = false;
    Candidate pending_best_;
    std::int64_t pending_end_ = 0;
    std::int64_t pending_first_ = 0;

    AleFrame frame_;
    bool frame_open_ = false;
    std::uint32_t last_word_ = 0xFFFFFFFFU;
    enum class Field : std::uint8_t { None, To, Thru, Conclusion, From, Cmd };
    Field field_ = Field::None;
};

}  // namespace revenant::decode
