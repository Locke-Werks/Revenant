// RTTY: start-stop two-tone FSK carrying International Telegraph Alphabet
// No. 2, from receiver audio to characters.
//
// SPECIFICATION
//
// The alphabet is ITU-T Recommendation S.1 (03/93), "International Telegraph
// Alphabet No. 2": clause 3 and Table 1/S.1 for the coding, clause 4.4 for
// the letter and figure shifts, clause 4.2 for the three figure-case
// positions left undefined, and Table 2/S.1 for how control characters are
// shown. The character structure is ITU-T Recommendation S.3 (11/88),
// "Transmission characteristics of the local end with its termination
// (ITA2)": clause 1.3 and Table 1/S.3 for the 7.5-unit character with its
// 1.5-unit stop element, and clause 1.4 for the shortest stop element a
// receiver must accept.
//
// Both were read from the ITU's own publication service, itu.int/rec, as the
// PDF of each Recommendation.
//
// WHAT NO DOCUMENT SAYS, AND WHERE IT COMES FROM INSTEAD
//
// 45.45 baud, 170 Hz shift, and a mark tone of 2125 Hz with space 170 Hz
// above it are amateur practice, not a standard. S.3 lists 50, 75, 100 and
// higher rates and no 45.45; S.1 defers the frequencies to Recommendation
// V.1 and the R series, which cover telegraph lines rather than a radio
// shift. They are the defaults because they are what is on the air, and
// every one is a parameter, as docs/modes.md says of this row. The
// "unshift on space" option is practice too and is off unless asked for.
//
// WHERE THIS STOPS
//
// At characters. No automatic frequency control: the tones are where the
// caller says they are, and a receiver tuned off by a good fraction of the
// bit rate loses margin. The round trip measures how much, rather than this
// comment guessing.
//
// CLEAN ROOM
//
// No RTTY implementation was read. Every constant names the clause or table
// it came from, or says it is practice.

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
// ITA2, from ITU-T S.1
// ---------------------------------------------------------------------------

// S.1 clause 3.1: 32 combinations of five units. Clause 3.2: condition A is
// start polarity and binary 0, condition Z is stop polarity and binary 1.
// Table 1 note: "In serial transmission, code element 1 is transmitted
// first."
//
// A combination is held here as a five-bit integer with code element 1 in
// bit 0, so the bits go to air in the order a right shift produces them.
inline constexpr std::size_t kIta2Units = 5;
inline constexpr std::size_t kIta2Combinations = 32;

// S.1 clause 4.4 and Table 1, combinations No. 29 and No. 30. Clause 4.5
// says neither moves the carriage.
inline constexpr std::uint8_t kIta2LetterShift = 0b11111;  // No. 29, ZZZZZ
inline constexpr std::uint8_t kIta2FigureShift = 0b11011;  // No. 30, ZZAZZ

// Table 1, combinations No. 31 and No. 32.
inline constexpr std::uint8_t kIta2Space = 0b00100;  // No. 31, AAZAA
inline constexpr std::uint8_t kIta2Null = 0b00000;   // No. 32, AAAAA

// The glyph a combination prints in each case. Zero means it prints nothing:
// the two shifts, and combination No. 32.
//
// Three substitutions, because S.1 draws characters that are not ASCII and
// defines three it does not assign:
//
//   - Table 1 prints the apostrophe as a typographic one and the hyphen as a
//     dash. Both come out as their ASCII forms here, since S.1 clause 2.3
//     says the Recommendation does not define a printing style.
//   - Figure-case No. 4 is "Who are you?" (clause 4.1) and No. 10 the audible
//     signal (clause 4.3). They come out as ASCII ENQ and BEL, which is this
//     project's mapping; Table 2 gives them as EQ and BL.
//   - Figure-case No. 6, 7 and 8 are "not defined" (clause 4.2), which
//     recommends printing an arbitrary sign such as a square to mark an
//     abnormal impression. They come out as U+25A1, a white square.
[[nodiscard]] char32_t ita2_letter(std::uint8_t combination);
[[nodiscard]] char32_t ita2_figure(std::uint8_t combination);

// The combination number S.1 Table 1 gives a code, 1 to 32, for reports and
// for the test that checks this file against the table.
[[nodiscard]] int ita2_combination_number(std::uint8_t combination);

// How a character is sent: the combination, and the case it needs, or no
// case at all for the five that print the same in both (clause 4.4 says the
// shift affects combinations 1 to 26 only).
struct Ita2Code {
    std::uint8_t combination = 0;
    enum class Case : std::uint8_t { Either, Letters, Figures } needs = Case::Either;
};

// Upper and lower case letters map to the same combination: S.1 clause 2.3
// leaves case to the terminal. Returns nothing for a character ITA2 cannot
// carry.
[[nodiscard]] std::optional<Ita2Code> ita2_encode(char32_t character);

// ---------------------------------------------------------------------------
// Character structure, from ITU-T S.3
// ---------------------------------------------------------------------------

// Table 1/S.3: at 50 and 75 baud the character is 7.5 units, one start unit,
// five code units and a 1.5-unit stop element. Clause 1.3 says the stop
// element should be at least 1.4 units and preferably 1.5.
inline constexpr double kRttyStopUnits = 1.5;

// Clause 1.4: a receiver must translate correctly signals whose stop elements
// are as short as 1.0 unit at 50 or 75 baud. The framer below therefore
// looks for the next start element one unit after the stop element begins,
// not one and a half.
inline constexpr double kRttyReceiverMinimumStopUnits = 1.0;

// How far either side of the detected edge the framer searches for the
// timing that best fits the character, in units. An engineering choice.
//
// The mark to space crossing alone places a character to within a few
// hundredths of a unit on a clean signal and to within a good fraction of one
// in noise, because the matched filter's output ramps over a whole unit and
// noise moves where the ramp crosses zero. A reading that far off centre sits
// on the side of a triangular eye. So the crossing is only a first estimate,
// and the framer then picks, within this distance of it, the timing at which
// the seven readings of the character stand furthest from zero: the start
// reading as space, the stop reading as mark, and the five code readings in
// whichever direction each went. Measured at 9.4 dB Eb/N0 over the same 300
// characters and the same noise, framing on the crossing alone gave a
// character error rate of 0.28 and the search gives 0.14.
//
// 0.4 stays inside the 1.0-unit stop element S.3 clause 1.4 requires a
// receiver to accept, so the search cannot slide a stop reading into the
// next character's start element.
inline constexpr double kRttyTimingSearchUnits = 0.4;

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

struct RttyConfig {
    SampleRate rate = 48000;

    // Practice, not a standard: see the header comment.
    double baud = 45.45;
    Hertz mark_hz = 2125;
    Hertz shift_hz = 170;

    // Polarity. True puts space above mark in the audio, which is what the
    // practice above means; false puts it below, which is the same signal
    // received on the other sideband.
    bool space_above_mark = true;

    // Practice: a space returns the decoder to letters case.
    bool unshift_on_space = false;
};

// One received character.
struct RttyCharacter {
    // Audio sample index at the leading edge of the start element.
    SampleIndex position = 0;

    // The five units, code element 1 in bit 0.
    std::uint8_t combination = 0;

    // What it prints, in the case in force when it arrived, or zero for a
    // shift or a null.
    char32_t glyph = 0;

    // The case in force after this character. A shift changes it.
    bool figures = false;

    // The smallest soft magnitude among the seven readings that framed and
    // decoded it, start and stop included. Near zero means at least one unit
    // was a guess. Reported so a caller can decide what to trust.
    float margin = 0.0F;
};

class RttyDecoder {
   public:
    [[nodiscard]] static Expected<RttyDecoder> create(const RttyConfig& config);

    // Consumes audio and appends every character whose stop element has
    // arrived. State carries across calls, so a caller may block the audio
    // any way it likes and gets the same characters.
    void process(ConstRealSpan audio, std::vector<RttyCharacter>& out);

    // Characters rejected because the stop element read as space. The
    // framer counts them and returns to hunting for a start element, which
    // is what lets it recover after a burst of noise.
    [[nodiscard]] std::uint64_t framing_errors() const { return framing_errors_; }

    // Start elements that were not space at their centre, so were a glitch
    // rather than a character.
    [[nodiscard]] std::uint64_t false_starts() const { return false_starts_; }

    void reset();

   private:
    RttyDecoder() = default;

    // Soft value at a fractional absolute index, interpolated. The index
    // must lie inside the history.
    [[nodiscard]] float soft_at(double index) const;

    // Frames one character from the edge found, once the history reaches
    // past its stop element. Returns false when more soft samples are needed.
    bool frame(std::vector<RttyCharacter>& out);

    RttyConfig config_{};
    ToneDiscriminator discriminator_{};
    double samples_per_unit_ = 0.0;

    // Soft values from history_start_ onward, absolute indices.
    std::vector<float> history_;
    std::uint64_t history_start_ = 0;

    // Next soft index to examine for a mark to space crossing, and the
    // crossing found, fractional, when have_edge_ is set.
    std::uint64_t scan_ = 1;
    bool have_edge_ = false;
    double edge_ = 0.0;

    bool figures_ = false;
    std::uint64_t framing_errors_ = 0;
    std::uint64_t false_starts_ = 0;
};

// UTF-8 of every glyph in a run of characters, skipping those that print
// nothing. Carriage return and line feed come out as themselves.
[[nodiscard]] std::string rtty_text(std::span<const RttyCharacter> characters);

}  // namespace revenant::decode
