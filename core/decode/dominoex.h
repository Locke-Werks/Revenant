// DominoEX: receiver audio to text, primary and secondary channels.
//
// SPECIFICATION
//
// Three documents by the mode's designer, hosted at
// https://www.qsl.net/zl1bpu/MFSK/ and fetched 2026-11-02:
//
//   - "DominoEX Technical Information", M. Greenman ZL1BPU, (c) 1997-2009
//     (Technical.htm). Cited as "Technical" with its section heading.
//   - "DominoEX Varicode Table", Version 2.0 of 28 July 2009, (c) M.
//     Greenman 2005-2009 (DominoEX Varicode Table.pdf). Cited as "Table",
//     its notes by number.
//   - "DominoEX - an IFK Mode for HF" (DEX.htm), the overview, for the table
//     of modes: baud rate, 18 tones, and tone spacing of one or two times
//     the baud rate.
//
// The ARRL's Domino page (arrl.org/domino) was fetched and adds nothing used
// here.
//
// WHAT THE DOCUMENTS FIX
//
//   - 18 tones, phase continuous, Technical "Modulation".
//   - IFK+: each nibble is added to the previous tone number plus 2, modulo
//     18, Technical "Coding" and "Offset IFK" ("a fixed offset (+2) is added
//     each time"). The receiver subtracts the previous tone and 2, adding 18
//     when negative; the page's "16 added" is that 18 less the 2.
//   - Speeds are a sample rate over 1024 samples per symbol at 1T spacing,
//     or over 2048 at 2T for 8 baud and below, Technical "Low Symbol Rate".
//     DEX.htm's table gives the six released: 4, 5, 8 at 2T and 11, 16, 22 at
//     1T. Faster submodes (44, 88) are in no document read and are not
//     offered.
//   - Nibble varicode, Table and Technical "Character Sync": an initial
//     nibble is 0 to 7, a continuation 8 to 15, one to three nibbles a
//     character, transmitted in the order printed.
//   - Idle is the secondary alphabet, never a dedicated idle code, Table
//     note 9; repeated secondary SPACE (3,8,8) when there is nothing else.
//
// WHERE THE TABLE DISAGREES WITH ITSELF
//
// Read from the PDF's text, the table gives ':' (58) and '<' (60) the same
// codes, 0,8,10 primary and 5,8,10 secondary, and gives 'J' (74) and '4'
// (52) the same secondary code, 4,15,8. Note 11 says corrected values are
// printed in red, which a text extraction cannot see. The receiver prints
// the lower character code for each pair, ':' and '4', and the transmitter
// sends '<' and secondary 'J' with the same codes, so they round trip as
// ':' and '4'. Unresolved until the PDF's colours or the author settle it.
// ASCII 0 to 31 have no secondary code in the table.
//
// WHAT THIS FILE CHOSE
//
// core/decode/mfsk_demod.h's tone bank, four tones wider than the signal on
// each side, and nibbles by non-coherent differential detection over pairs
// of symbols (dominoex.cpp says how); no FEC (Technical "No FEC Coding": "No FEC is used
// by default"). The DominoEX FEC option is THOR's machinery and not offered
// here; core/decode/thor.h is THOR.
//
// CLEAN ROOM
//
// No DominoEX or THOR implementation was read, fldigi and ZL2AFP's program
// included.

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <span>
#include <utility>
#include <vector>

#include "core/decode/mfsk_demod.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::SampleIndex;

// Technical "Offset IFK" and "Modulation".
inline constexpr int kDominoTones = 18;
inline constexpr int kDominoIfkOffset = 2;
// Receiver bank margin each side, an engineering choice (dominoex.cpp).
inline constexpr int kDominoGuardTones = 4;
inline constexpr int kDominoBank = kDominoTones + 2 * kDominoGuardTones;

enum class DominoSpeed : std::uint8_t { D4, D5, D8, D11, D16, D22 };

struct DominoGeometry {
    double symbol_rate = 0.0;
    double tone_spacing_hz = 0.0;
};

// DEX.htm's table and Technical "Low Symbol Rate": 8000, 11025, 16000 Hz
// over 2048 samples at 2T; 11025, 16000, 22050 Hz over 1024 at 1T.
[[nodiscard]] DominoGeometry domino_geometry(DominoSpeed speed);

// Table, regularised to three nibbles as its note 7 suggests; a nibble of 0
// after the first ends the code. A secondary code of {0, 0, 0} is absent.
struct DominoVaricodeEntry {
    std::array<std::uint8_t, 3> primary;
    std::array<std::uint8_t, 3> secondary;
};
extern const std::array<DominoVaricodeEntry, 256> kDominoVaricode;

// Nibbles of a character, primary or secondary alphabet. Empty for a
// character with no secondary code.
[[nodiscard]] std::vector<std::uint8_t> domino_nibbles(std::uint8_t code, bool secondary);

struct DominoDecodedCharacter {
    std::uint8_t code = 0;
    bool secondary = false;
    bool valid = false;
};
// Looks up a complete nibble sequence.
[[nodiscard]] DominoDecodedCharacter domino_lookup(std::span<const std::uint8_t> nibbles);

struct DominoConfig {
    SampleRate rate = 8000;
    DominoSpeed speed = DominoSpeed::D11;
    // The signal is searched for between these edges; IFK makes the result
    // indifferent to a whole-tone error.
    double search_low_hz = 300.0;
    double search_high_hz = 2700.0;
    bool lower_sideband = false;
};

struct DominoCharacter {
    std::uint8_t code = 0;
    // Secondary (idle, beacon) alphabet rather than the keyboard's.
    bool secondary = false;
    // Audio sample index of the character's first symbol.
    SampleIndex position = 0;
};

class DominoDecoder {
public:
    [[nodiscard]] static Expected<DominoDecoder> create(const DominoConfig& config);

    // Chunking does not change the output.
    void process(ConstRealSpan audio, std::vector<DominoCharacter>& out);
    // A character is complete when the next initial nibble arrives; this
    // completes the last one.
    void flush(std::vector<DominoCharacter>& out);
    void reset();

    [[nodiscard]] bool acquired() const { return demod_.acquired(); }
    [[nodiscard]] std::uint64_t invalid() const { return invalid_; }
    [[nodiscard]] double tone0_hz() const { return demod_.tone0_hz(); }

private:
    explicit DominoDecoder(MfskDemodulator demod) : demod_(std::move(demod)) {}
    void on_nibble(int nibble, SampleIndex position, std::vector<DominoCharacter>& out);
    void finish(std::vector<DominoCharacter>& out);

    DominoConfig config_{};
    MfskDemodulator demod_;
    std::vector<MfskSymbol> symbols_;
    using Bank = std::array<double, kDominoBank>;
    void decide(const Bank& cur, SampleIndex position, std::vector<DominoCharacter>& out);
    Bank previous_{};
    bool have_previous_ = false;
    double peak_ = 0.0;
    std::vector<std::uint8_t> nibbles_;
    SampleIndex start_ = 0;
    std::uint64_t invalid_ = 0;
};

}  // namespace revenant::decode
