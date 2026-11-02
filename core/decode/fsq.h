// FSQ: receiver audio to text, the character layer only.
//
// SPECIFICATION
//
// Two documents by the mode's designer, M. Greenman ZL1BPU, at
// https://www.qsl.net/zl1bpu/MFSK/ and fetched 2026-11-02:
//
//   - "FSQ" (FSQweb.htm), the mode description. Cited as "FSQ".
//   - "WSQ Varicode V3.0", (c) Murray Greenman Dec 2013 (WSQ Varicode
//     V3.png), the alphabet image the FSQ page links as "the FSQ Alphabet
//     and Varicode". Cited as "Varicode". It is an image; the table below
//     was transcribed from it by eye and checked against FSQ's counts: 29
//     one-symbol characters, 75 two-symbol, 104 in all.
//
// WHAT THE DOCUMENTS FIX
//
//   - 33 tones spaced 8.7890625 Hz, "exactly 1.5 x the baud rate at the
//     highest speed", FSQ. So the top speed is 5.859375 Bd, 2048 samples at
//     12000 Hz, which is also the page's FFT bin of 2.9296875 Hz times two.
//   - IFK+: the character number is added to the previous tone number "and
//     ONE is added as well", wrapping by 33, FSQ. 32 differences, 32 codes.
//   - The alphabet, Varicode: a symbol of 0 to 28 is a character on its own
//     or the first of two; a following 29, 30 or 31 selects the two-symbol
//     character. So a one-symbol character is known complete when the next
//     symbol is not 29 to 31.
//
// WHAT THEY DO NOT FIX, AND WHAT THIS FILE DOES
//
// The three slower speeds are given only as "nominally 6, 4.5, 3 and 2
// baud", and the page's receiver needs no speed because it decodes without
// symbol timing. This receiver times symbols, so it takes the symbol rate as
// a parameter, defaulting to the one rate the page fixes exactly; the slower
// rates are not guessed at. FSQ's sentence mode and FSQCALL's directed
// messages sit above the characters and are not decoded here; their framing
// is not in either document read. The image mode is not decoded.
//
// Varicode prints '×' at 158 and '£' at 156, which is a DOS code page's
// placement; the codes are carried as printed.
//
// Detection is core/decode/dominoex.h's: a bank four tones wider than the
// signal each side and non-coherent differential detection over pairs of
// symbols, with 33 tones and an offset of one.
//
// CLEAN ROOM
//
// No FSQ or WSQ implementation was read: not fldigi, not the author's
// FSQCALL source, which the FSQ page links and which was not opened.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "core/decode/mfsk_demod.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::SampleIndex;

// FSQ: 33 tones, IFK+ offset one, 8.7890625 Hz spacing, 5.859375 Bd top.
inline constexpr int kFsqTones = 33;
inline constexpr int kFsqIfkOffset = 1;
inline constexpr double kFsqToneSpacingHz = 8.7890625;
inline constexpr double kFsqTopSymbolRate = 12000.0 / 2048.0;
// Varicode: a second symbol of 29 to 31 makes a two-symbol character.
inline constexpr int kFsqFirstShift = 29;
// Receiver bank margin each side, an engineering choice as in dominoex.h.
inline constexpr int kFsqGuardTones = 4;
inline constexpr int kFsqBank = kFsqTones + 2 * kFsqGuardTones;

// Varicode's 104 entries: the character code, the first symbol, and the
// second (-1 for a one-symbol character). CRLF is listed once, as 13.
struct FsqVaricodeEntry {
    std::uint8_t code;
    std::int8_t first;
    std::int8_t second;
};
extern const std::array<FsqVaricodeEntry, 104> kFsqVaricode;

// Symbols for a character; empty if the alphabet has none. Line feed sends
// CRLF's symbol as well.
[[nodiscard]] std::vector<int> fsq_symbols(std::uint8_t code);
// The character for a complete code, or -1.
[[nodiscard]] int fsq_lookup(int first, int second);

struct FsqConfig {
    SampleRate rate = 8000;
    double symbol_rate = kFsqTopSymbolRate;
    double search_low_hz = 300.0;
    double search_high_hz = 2700.0;
    bool lower_sideband = false;
};

struct FsqCharacter {
    // 13 for CRLF, 0 for IDLE, otherwise as Varicode prints.
    std::uint8_t code = 0;
    SampleIndex position = 0;
};

class FsqDecoder {
public:
    [[nodiscard]] static Expected<FsqDecoder> create(const FsqConfig& config);
    // Chunking does not change the output. IDLE is not reported.
    void process(ConstRealSpan audio, std::vector<FsqCharacter>& out);
    void flush(std::vector<FsqCharacter>& out);
    void reset();

    [[nodiscard]] std::uint64_t invalid() const { return invalid_; }

private:
    explicit FsqDecoder(MfskDemodulator demod) : demod_(std::move(demod)) {}
    using Bank = std::array<double, kFsqBank>;
    void decide(const Bank& cur, SampleIndex position, std::vector<FsqCharacter>& out);
    void on_symbol(int value, SampleIndex position, std::vector<FsqCharacter>& out);
    void emit(int code, SampleIndex position, std::vector<FsqCharacter>& out);

    FsqConfig config_{};
    MfskDemodulator demod_;
    std::vector<MfskSymbol> symbols_;
    Bank previous_{};
    bool have_previous_ = false;
    double peak_ = 0.0;
    int pending_ = -1;
    SampleIndex pending_position_ = 0;
    std::uint64_t invalid_ = 0;
};

}  // namespace revenant::decode
