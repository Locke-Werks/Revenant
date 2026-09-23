// Varicode, the PSK31 alphabet.
//
// SPECIFICATION
//
// Peter Martinez G3PLX, "PSK31: A New Radio-Teletype Mode", QEX July/August
// 1999 pp 3-9, reprinted with the RSGB's permission from RadCom December 1998
// and January 1999 with the author's February 1999 updates. ARRL hosts it as
// arrl.org/files/file/Technology/tis/info/pdf/x9907003.pdf. Table 1 on page 4
// is the alphabet, and the section "PSK31 Alphabet" on pages 4 and 5 is the
// rule it was built from. That magazine table is the primary publication this
// file cites.
//
// THE ARRL WEB PAGE IS NOT THE SAME TABLE, AND IT IS THE ONE THAT IS WRONG
//
// arrl.org/psk31-spec carries the same alphabet retyped as an HTML list, and
// on 2026-09-22 it disagrees with the QEX table in three places, each of which
// breaks the code's own rules:
//
//   - it gives Y and Z the same code, 101111011. QEX gives Z 1010101101.
//   - it gives p 1111111, the code QEX and the page itself give I. QEX gives
//     p 111111.
//   - its column between _ and a lists two entries, printed "." and "/", where
//     ASCII has one character, the grave accent.
//
// Two characters sharing a code cannot be decoded, which is how the page
// fails its own "self-synchronizing" claim. tests/decode/test_psk31.cpp
// checks the table here for exactly those properties, so a retyping error of
// that kind fails a test rather than a contact.
//
// ONE GLYPH IN THE QEX TABLE IS MISPRINTED AND ITS POSITION IS NOT
//
// The row between & and ( in the second column is ASCII 39, the apostrophe,
// and the typesetting renders its glyph as a grave accent. Its position in the
// table's ASCII order is what fixes which character it is, and the real grave
// accent, ASCII 96, has its own row between _ and a.
//
// WHAT THE CODE IS, IN THE ARTICLE'S OWN WORDS
//
// Page 5: key-down is 1 and key-up is 0, the shortest code is a single 1,
// codes never contain two consecutive zeros, and "the letter-gap can also be
// shortened to two bits". So a character is a run of bits starting and ending
// with a 1 with no 00 inside it, and 00 between characters is what makes the
// stream self-synchronising. Table 1's caption: "A minimum of two zeros is
// inserted between characters."
//
// The extended alphabet on page 9 (codes 128 to 255, longer than ten bits) is
// not carried. The article says early decoders ignore anything longer than ten
// bits as corruption and that it "would not be a good idea" to rely on it, and
// this decoder does what those decoders do and says so in its output.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// QEX July/August 1999 Table 1, indexed by ASCII value 0 through 127, codes
// written as the table writes them, left bit first on the air.
inline constexpr std::array<std::string_view, 128> kVaricode = {
    // 0 NUL through 31 US, the table's first column.
    "1010101011", "1011011011", "1011101101", "1101110111",  // NUL SOH STX ETX
    "1011101011", "1101011111", "1011101111", "1011111101",  // EOT ENQ ACK BEL
    "1011111111", "11101111",   "11101",      "1101101111",  // BS HT LF VT
    "1011011101", "11111",      "1101110101", "1110101011",  // FF CR SO SI
    "1011110111", "1011110101", "1110101101", "1110101111",  // DLE DC1 DC2 DC3
    "1101011011", "1101101011", "1101101101", "1101010111",  // DC4 NAK SYN ETB
    "1101111011", "1101111101", "1110110111", "1101010101",  // CAN EM SUB ESC
    "1101011101", "1110111011", "1011111011", "1101111111",  // FS GS RS US
    // 32 SP through 63 ?.
    "1",          "111111111",  "101011111",  "111110101",   // SP ! " #
    "111011011",  "1011010101", "1010111011", "101111111",   // $ % & '
    "11111011",   "11110111",   "101101111",  "111011111",   // ( ) * +
    "1110101",    "110101",     "1010111",    "110101111",   // , - . /
    "10110111",   "10111101",   "11101101",   "11111111",    // 0 1 2 3
    "101110111",  "101011011",  "101101011",  "110101101",   // 4 5 6 7
    "110101011",  "110110111",  "11110101",   "110111101",   // 8 9 : ;
    "111101101",  "1010101",    "111010111",  "1010101111",  // < = > ?
    // 64 @ through 95 _.
    "1010111101", "1111101",    "11101011",   "10101101",    // @ A B C
    "10110101",   "1110111",    "11011011",   "11111101",    // D E F G
    "101010101",  "1111111",    "111111101",  "101111101",   // H I J K
    "11010111",   "10111011",   "11011101",   "10101011",    // L M N O
    "11010101",   "111011101",  "10101111",   "1101111",     // P Q R S
    "1101101",    "101010111",  "110110101",  "101011101",   // T U V W
    "101110101",  "101111011",  "1010101101", "111110111",   // X Y Z [
    "111101111",  "111111011",  "1010111111", "101101101",   // \ ] ^ _
    // 96 ` through 127 DEL.
    "1011011111", "1011",       "1011111",    "101111",      // ` a b c
    "101101",     "11",         "111101",     "1011011",     // d e f g
    "101011",     "1101",       "111101011",  "10111111",    // h i j k
    "11011",      "111011",     "1111",       "111",         // l m n o
    "111111",     "110111111",  "10101",      "10111",       // p q r s
    "101",        "110111",     "1111011",    "1101011",     // t u v w
    "11011111",   "1011101",    "111010101",  "1010110111",  // x y z {
    "110111011",  "1010110101", "1011010111", "1110110101",  // | } ~ DEL
};

// Table 1 stops at ten bits, and page 5 says so: "We can do the 128-character
// ASCII set with 10 bits."
inline constexpr std::size_t kVaricodeMaxBits = 10;

// The code for one ASCII character, left bit first, or an error for a value
// above 127, which the base alphabet does not cover.
[[nodiscard]] Expected<std::string_view> varicode_for(std::uint8_t ascii);

// Encodes text to the bit stream that goes on the air: each character's code
// followed by the two-zero gap Table 1's caption requires. One bit per byte.
[[nodiscard]] Expected<std::vector<std::uint8_t>> varicode_encode(std::string_view text);

// One character recovered from the bit stream.
struct VaricodeCharacter {
    // The ASCII value, when `recognised`. When not, the code was well formed
    // but is in no row of Table 1, which is what a longer-than-ten-bit
    // extended-alphabet code or a bit error that happens to keep the 00 rule
    // produces, and this is 0.
    std::uint8_t ascii = 0;
    bool recognised = false;

    // The bit count of the code, so a caller measuring throughput does not
    // have to look it up again.
    std::size_t bits = 0;

    // Index of the character's first bit in the caller's bit numbering.
    std::uint64_t first_bit = 0;
};

// The inverse of the table, by lookup on the code read as a binary number with
// a leading one, so "1011" is 0b1011. Returns false for a code in no row.
[[nodiscard]] bool varicode_lookup(std::uint32_t code, std::size_t bits, std::uint8_t& ascii);

// Streaming decoder: feed decided bits in order, get characters out as each
// closing 00 arrives.
class VaricodeDecoder {
   public:
    // `bit_index` is the caller's index of this bit, carried into the output so
    // a character can be placed in time.
    void push(std::uint8_t bit, std::uint64_t bit_index, std::vector<VaricodeCharacter>& out);

    void reset();

   private:
    std::uint32_t code_ = 0;
    std::size_t bits_ = 0;

    // Starts at zero, not two. Whatever arrives before the first 00 is the
    // tail of a character whose start was never seen, or the receiver
    // settling, and page 5's self-synchronisation is exactly the rule that a
    // character is only known to start after a gap. So nothing is reported
    // until one has been seen.
    std::size_t zeros_ = 0;
    bool after_gap_ = false;
    std::uint64_t first_bit_ = 0;
    bool overflowed_ = false;
};

}  // namespace revenant::decode
