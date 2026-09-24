// CW: receiver audio to Morse text and the speed it was sent at.
//
// SPECIFICATION
//
// Recommendation ITU-R M.1677-1 (10/2009), "International Morse code",
// Annex 1 Part I. Clause 1.1 is the table of signals, clauses 1.1.1 to 1.1.3
// letters, figures and punctuation with the service signals, and clause 2 the
// timing: "2.1 A dash is equal to three dots. 2.2 The space between the
// signals forming the same letter is equal to one dot. 2.3 The space between
// two letters is equal to three dots. 2.4 The space between two words is
// equal to seven dots."
//
// SPEED AND FARNSWORTH ARE NOT IN THE RECOMMENDATION
//
// M.1677-1 states ratios and no speed. Words per minute is the "PARIS"
// convention, which this file takes from the ARRL Morse Transmission Timing
// Standard, Jon Bloom KE3Z, "A Standard for Morse Timing Using the Farnsworth
// Technique", QEX April 1990 pp 8-9, clause 2.1: "For purposes of specifying
// code speed, the 'PARIS' 50-unit standard is used", giving u = 1.2/c seconds
// for a unit at c words per minute. The same standard's clause 2.2 is the
// only published definition of Farnsworth timing this project found: the
// characters keep speed c, and extra delay spread over the character and word
// spaces brings the overall speed down to s, keeping "the 3/7 ratio of
// character space to word space". The decoder relies on exactly that
// property and nothing else about Farnsworth.
//
// WHAT THE DECODER DOES
//
// Envelope detection with a threshold that tracks the noise, then a timing
// decoder that estimates the dot length from the runs themselves. The unit is
// the smallest of the three lengths clause 2 uses, and every character with
// more than one element carries it twice over, in its dots or in the
// one-dot space between elements, which clause 2.2 fixes and Farnsworth
// leaves alone. So the unit is read off the shortest cluster of marks and
// spaces together, and marks are dots or dashes on either side of two units.
// Letter and word spaces are told apart from the spaces' own clusters, so a
// Farnsworth sender whose letter space is twenty units still reads as
// letters, and a pause between two overs is left out of them. Every constant
// that is a choice rather than a clause says so.
//
// Cw reads one tone near a pitch it is told, follows it as it drifts, and
// filters around it before the envelope so a neighbour a hundred hertz away
// does not key it. CwBand, at the end of this file, is what a receiver's
// audio is read with: it finds every keyed tone in the passband, wherever
// the tuning left it, and runs a Cw on each.
//
// CLEAN ROOM
//
// No CW decoder was read. The table is transcribed from the Recommendation's
// clause 1.1, and the timing from its clause 2 and the ARRL standard above.
//
// NOTHING HERE IS BIT EXACT AGAINST A GPU TWIN
//
// Host code with no kernel behind it. tests/decode/test_cw.cpp round trips it
// against core/dsp/synth/cw_mod.h, the keyer written from the same clauses.

#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/decode/tone_frontend.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// ---------------------------------------------------------------------------
// Specified constants
// ---------------------------------------------------------------------------

// ITU-R M.1677-1 Annex 1 Part I clause 2, in dots.
inline constexpr double kMorseDashDots = 3.0;          // clause 2.1
inline constexpr double kMorseElementSpaceDots = 1.0;  // clause 2.2
inline constexpr double kMorseLetterSpaceDots = 3.0;   // clause 2.3
inline constexpr double kMorseWordSpaceDots = 7.0;     // clause 2.4

// ARRL Morse Transmission Timing Standard clause 2.1: u = 1.2/c, from the
// fifty-unit word "PARIS".
inline constexpr double kParisUnitSecondsTimesWpm = 1.2;
inline constexpr double kParisWordUnits = 50.0;

// ARRL standard clause 2.2 and its Appendix A.2: of the fifty units of PARIS,
// 31 are elements and element spaces, and the other 19 are four letter
// spaces and one word space, 4*3 + 7.
inline constexpr double kParisCharacterUnits = 31.0;
inline constexpr double kParisSpacingUnits = 19.0;

// One row of the clause 1.1 table. `text` is what the decoder writes and the
// keyer reads; letters are upper case, and a service signal with no character
// of its own is written as its clause 1.1.3 name in angle brackets.
struct MorseSignal {
    std::string_view text;
    std::string_view code;  // '.' a dot, '-' a dash, in sending order
};

// ITU-R M.1677-1 Annex 1 Part I clause 1.1.
//
// Two signals in clause 1.1.3 share a code with a letter: the invitation to
// transmit is k's -.- and the multiplication sign is x's -..-, which clause
// 3.2.1 confirms by saying the multiplication sign "shall be transmitted" as
// X. Both are listed after the letters so a lookup by code finds the letter.
inline constexpr std::array<MorseSignal, 57> kMorseTable = {{
    // Clause 1.1.1, letters.
    {"A", ".-"},    {"B", "-..."},  {"C", "-.-."},  {"D", "-.."},   {"E", "."},
    {"F", "..-."},  {"G", "--."},   {"H", "...."},  {"I", ".."},    {"J", ".---"},
    {"K", "-.-"},   {"L", ".-.."},  {"M", "--"},    {"N", "-."},    {"O", "---"},
    {"P", ".--."},  {"Q", "--.-"},  {"R", ".-."},   {"S", "..."},   {"T", "-"},
    {"U", "..-"},   {"V", "...-"},  {"W", ".--"},   {"X", "-..-"},  {"Y", "-.--"},
    {"Z", "--.."},
    // Clause 1.1.1, the accented e, written as UTF-8 E with acute.
    {"\xC3\x89", "..-.."},
    // Clause 1.1.2, figures.
    {"1", ".----"}, {"2", "..---"}, {"3", "...--"}, {"4", "....-"}, {"5", "....."},
    {"6", "-...."}, {"7", "--..."}, {"8", "---.."}, {"9", "----."}, {"0", "-----"},
    // Clause 1.1.3, punctuation marks and miscellaneous signs, in the
    // Recommendation's order.
    {".", ".-.-.-"},
    {",", "--..--"},
    {":", "---..."},
    {"?", "..--.."},
    {"'", ".----."},
    {"-", "-....-"},
    {"/", "-..-."},
    {"(", "-.--."},
    {")", "-.--.-"},
    {"\"", ".-..-."},
    {"=", "-...-"},  // double hyphen
    {"<UNDERSTOOD>", "...-."},
    {"<ERROR>", "........"},
    {"+", ".-.-."},  // cross or addition sign
    {"<INVITATION TO TRANSMIT>", "-.-"},
    {"<WAIT>", ".-..."},
    {"<END OF WORK>", "...-.-"},
    {"<STARTING SIGNAL>", "-.-.-"},
    {"<MULTIPLICATION SIGN>", "-..-"},
    {"@", ".--.-."},
}};

// Code for a text token, case-insensitive for letters. Error for anything the
// table has no row for.
[[nodiscard]] Expected<std::string_view> morse_code_for(std::string_view token);

// The first row with this code, or an empty view.
[[nodiscard]] std::string_view morse_text_for(std::string_view code);

// Splits text into the tokens the table is keyed by: one character each,
// except a two-byte UTF-8 E-acute and a service signal in angle brackets.
// Spaces come back as " " tokens, which the keyer sends as word spaces.
[[nodiscard]] Expected<std::vector<std::string>> morse_tokens(std::string_view text);

// The Farnsworth spacing of the ARRL standard, clause 2.2: for characters at c
// words per minute and an overall speed s < c, the letter and word spaces in
// seconds. At s >= c both are the clause 2.3 and 2.4 standard spaces.
struct MorseSpacing {
    double unit_s = 0.0;
    double letter_space_s = 0.0;
    double word_space_s = 0.0;
};
[[nodiscard]] Expected<MorseSpacing> morse_spacing(double character_wpm, double overall_wpm);

// The inverse: the overall speed a letter space of `letter_space_s` implies at
// character speed c, from the same clause 2.2 equations.
[[nodiscard]] double morse_overall_wpm(double character_wpm, double letter_space_s);

// ---------------------------------------------------------------------------
// The timing decoder, marks and spaces in, characters out
// ---------------------------------------------------------------------------

struct MorseTimingConfig {
    // Speed range the unit estimate is clamped to. NOT A SPECIFIED RANGE:
    // five words per minute is the slowest speed the ARRL standard's own
    // example sends at, and fifty is past what hand keying reaches.
    double min_wpm = 5.0;
    double max_wpm = 50.0;
};

struct CwCharacter {
    // The row's text, " " for a word space, or empty when `recognised` is
    // false.
    std::string text;
    bool recognised = false;

    // The dots and dashes as received. Empty for a word space.
    std::string code;

    // Where the character's first mark started, in the caller's sample
    // numbering.
    SampleIndex first_sample = 0;

    // The character speed at the moment it was decoded.
    double wpm = 0.0;

    // The audio frequency of the tone it was read from, in hertz, and which
    // of CwBand's streams read it. Cw fills the pitch it is tracking and
    // stream 0; MorseTiming, which hears no tone, leaves both at zero.
    double pitch_hz = 0.0;
    std::uint32_t stream = 0;
};

class MorseTiming {
   public:
    MorseTiming() = default;
    explicit MorseTiming(const MorseTimingConfig& config) : config_(config) {}

    // A completed key-down run.
    void mark(double seconds, SampleIndex start, std::vector<CwCharacter>& out);

    // A completed key-up run, starting at `start`.
    void space(double seconds, SampleIndex start, std::vector<CwCharacter>& out);

    // The key has been up for `seconds` so far and still is. Lets a character
    // come out as soon as the gap is long enough to say so, rather than when
    // the next mark ends it. A word space waits for the gap to complete,
    // because the gap joins the letter and word clusters it is judged by.
    //
    // Not at the start of a transmission, which is held until its spacing
    // shows letter spaces and word spaces apart, or until a gap of twice the
    // longest space so far says the sender has stopped. See held_.
    void idle(double seconds, std::vector<CwCharacter>& out);

    // Emits whatever character is in progress.
    void flush(std::vector<CwCharacter>& out);

    [[nodiscard]] bool locked() const { return locked_; }
    [[nodiscard]] double unit_seconds() const { return unit_; }

    // Character speed, PARIS words per minute, from the mean of the dots
    // and the mean of the element spaces weighted equally. Not
    // 1.2 / unit_seconds() in noise, where the pooled unit reads long.
    [[nodiscard]] double wpm() const;

    // Overall speed implied by the letter spaces, the ARRL clause 2.2
    // inverse. Equal to wpm() for standard spacing and below it for
    // Farnsworth.
    [[nodiscard]] double overall_wpm() const;

    // How much the last kFitMarks marks look like clause 2.1's: split into
    // two clusters, zero unless the longer is two to five times the shorter,
    // and otherwise the fraction of marks near their own cluster's mean. And
    // how many marks that is over. Morse keyed by a machine or a hand scores
    // near one; the on and off of speech's syllables, read as marks, does
    // not. CwBand ends a stream on it.
    [[nodiscard]] double fit() const;
    [[nodiscard]] std::size_t fitted_marks() const { return fits_.size(); }

    void reset();

   private:
    struct Run {
        bool key_down = false;
        double seconds = 0.0;
        SampleIndex start = 0;
    };

    void estimate_unit();
    void remember_run(bool key_down, double seconds);
    void remember_long_space(double seconds);
    [[nodiscard]] bool spacing_known() const;
    void release(std::vector<CwCharacter>& out);
    void apply_mark(double seconds, SampleIndex start, std::vector<CwCharacter>& out);
    // `counted` is a space release() has already put in the clusters.
    void apply_space(double seconds, SampleIndex start, bool counted,
                     std::vector<CwCharacter>& out);
    void end_character(std::vector<CwCharacter>& out);
    [[nodiscard]] double letter_space_estimate() const;
    [[nodiscard]] double word_threshold() const;

    MorseTimingConfig config_{};
    bool locked_ = false;

    // The unit the runs are read against, and the one the speed is reported
    // from. They differ in noise; estimate_unit() says how and why.
    double unit_ = 0.0;
    double speed_unit_ = 0.0;

    // Recent runs of both kinds, which the unit is estimated from.
    std::deque<Run> recent_;

    // The start of a transmission, held back until the unit is known and the
    // long spaces show both clusters, so the first word space is judged
    // against letter spaces rather than alone. Clause 2.4's word space is
    // seven units only against clause 2.3's three; one space on its own
    // could be either, and a Farnsworth letter space is often longer than
    // seven. released_ is set once and stays set until reset().
    std::vector<Run> held_;
    bool released_ = false;

    // Spaces of two units or more, for the letter and word clusters.
    std::deque<double> long_spaces_;

    std::string code_;
    SampleIndex character_start_ = 0;
    bool character_ended_ = true;
    bool word_ended_ = true;
    bool any_text_ = false;

    // The last kFitMarks marks' lengths, for fit().
    std::deque<double> fits_;
};

// ---------------------------------------------------------------------------
// The audio decoder
// ---------------------------------------------------------------------------

struct CwConfig {
    SampleRate rate = 48000;

    // Where the operator put the tone in the audio passband.
    Hertz centre_hz = 700;

    // How far the tone may actually be from centre_hz, one-sided. NOT A
    // SPECIFIED VALUE: a CW filter is typically a few hundred hertz wide and
    // an operator tunes by ear to within a few tens, so 100 Hz covers the
    // tuning with room for a receiver whose sidetone pitch differs from its
    // offset.
    double capture_hz = 100.0;

    MorseTimingConfig timing{};

    // Seconds of audio the tone's frequency is estimated over before anything
    // is decoded. NOT A SPECIFIED VALUE; two seconds holds several elements
    // at five words per minute.
    double acquisition_seconds = 2.0;

    // Whether the tone is searched for within capture_hz of centre_hz before
    // anything is decoded. CwBand turns it off: it starts a decoder on a tone
    // it has already found, at the pitch it found it, and the search above
    // would cost the two seconds again.
    bool acquire = true;

    // Whether the frequency follows the tone after acquisition, a little
    // after every mark, by up to capture_hz from centre_hz either way. A
    // transmitter or receiver that drifts a few hertz a minute otherwise
    // walks out of the envelope filter, which integrates coherently: at 12
    // WPM its 80 ms boxcar loses a third of a tone 5 Hz away.
    bool track = true;

    // The tone's amplitude as the caller already measured it, in the units
    // the envelope reads, or zero for none. It seeds the mark level, so the
    // first marks are judged against the tone and not against whatever keyed
    // first; CwBand passes what its search saw.
    double expected_level = 0.0;
};

class Cw {
   public:
    [[nodiscard]] static Expected<Cw> create(const CwConfig& config);

    [[nodiscard]] Status process(ConstRealSpan audio, std::vector<CwCharacter>& out);

    // Emits the character in progress, for the end of a capture.
    void flush(std::vector<CwCharacter>& out);

    [[nodiscard]] bool acquired() const { return acquired_; }
    [[nodiscard]] double frequency_offset_hz() const { return offset_hz_; }

    // The tone's audio frequency, centre_hz plus the offset.
    [[nodiscard]] double pitch_hz() const {
        return static_cast<double>(config_.centre_hz) + offset_hz_;
    }

    [[nodiscard]] double wpm() const { return timing_.wpm(); }
    [[nodiscard]] double overall_wpm() const { return timing_.overall_wpm(); }
    [[nodiscard]] const MorseTiming& timing() const { return timing_; }

    // The tracked mark level above the noise's mean, in standard deviations
    // of the noise envelope: the quantity the squelch compares against its
    // threshold, so a caller can show it as a signal meter.
    [[nodiscard]] double level_deviations() const;

    // Seconds of audio since a mark last cleared the squelch's open bar, or
    // since the decoder acquired when none has. Zero before acquisition.
    [[nodiscard]] double seconds_since_mark() const;

    void reset();

   private:
    Cw() = default;

    void run(std::span<const Complex32> baseband, std::vector<CwCharacter>& out);
    void label(std::vector<CwCharacter>& out, std::size_t from) const;
    void seed_level();
    void set_boxcar(std::size_t length);
    [[nodiscard]] double mark_level() const;
    [[nodiscard]] double noise_sigma() const;
    [[nodiscard]] double noise_mean() const;
    [[nodiscard]] double noise_spread() const;
    [[nodiscard]] SampleIndex to_audio_sample(double decimated_index) const;

    CwConfig config_{};
    ToneFrontEnd front_{};
    MorseTiming timing_{};

    std::vector<Complex32> acquisition_;
    std::vector<Complex32> decimated_;
    bool acquired_ = false;
    double offset_hz_ = 0.0;
    SampleIndex index_ = 0;  // decimated index of the next sample to run

    // The envelope filter: a boxcar over the last `boxcar_` corrected samples.
    std::deque<Complex32> history_;
    std::size_t boxcar_ = 1;
    std::size_t history_limit_ = 1;

    // Level trackers and the debounced key state.
    double high_ = 0.0;
    double noise_quartile_ = 0.0;
    std::vector<double> warm_;
    bool heard_ = false;
    bool have_level_ = false;
    SampleIndex last_mark_ = 0;
    double mark_peak_ = 0.0;
    std::vector<double> mark_envelope_;
    bool raw_key_ = false;
    SampleIndex raw_since_ = 0;
    bool key_ = false;
    SampleIndex key_since_ = 0;

    // The mixer's phase, advanced sample by sample so that a change of
    // offset_hz_ turns the phasor from where it is rather than jumping it.
    double phase_ = 0.0;

    // A low-pass on the corrected samples, around the tone rather than
    // around centre_hz, so a neighbour a hundred hertz away is gone before
    // the boxcar. cw.cpp's kNeighbourSlackHz says why.
    std::vector<float> narrow_taps_;
    std::deque<Complex32> narrow_history_;

    // The tracker: the boxcar's sum over the mark in progress, each against
    // the one before, summed. Its angle is how far the tone turned per
    // sample, which is how far the offset is off.
    std::complex<double> turn_{0.0, 0.0};
    std::complex<double> previous_sum_{0.0, 0.0};
    SampleIndex acquired_at_ = 0;
};

// ---------------------------------------------------------------------------
// Every keyed tone in a receiver's audio
// ---------------------------------------------------------------------------
//
// A receiver in a CW segment hears more than one station, none of them where
// the operator's sidetone is: on usb or lsb the pitch is wherever the dial
// left each carrier, and on cw it is the sidetone plus however far off zero
// beat the click was. CwBand searches the audio for tones that are KEYED,
// starts a Cw on each one it finds at the pitch it found it, and labels every
// character with the pitch and a stream number.
//
// WHAT COUNTS AS KEYED
//
// A short-time spectrum of the audio, a frame every 8 ms with bins about
// 31 Hz wide at 48000 S/s. In each bin the noise is read from the lower fifth
// of the bin's power over the last kSearchWindowSeconds, scaled to a mean as
// for exponentially distributed power, and taken no lower than its
// neighbours', so a receiver filter that shapes the noise across the band
// does not matter. A bin is keyed when its mean power stands clear of that
// noise, when it went up and down in several runs of two frames or more, and
// when it was up for no more than most of the window. A steady carrier is up
// all the time; noise goes up in single frames and its mean does not stand
// clear. Two things that do key are then set aside: the splatter a strong
// station's edges throw into the bins beside it, which is only ever up while
// that station is, and speech, whose harmonics come up many at once across
// the band. None of these is a specified value; each constant in cw.cpp says
// what it was set from.
//
// STREAMS
//
// A keyed bin that is a local maximum and not within kStreamSpacingHz of a
// stream already running starts a Cw there, fed the last
// kSearchReplaySeconds of audio first, so the characters the search spent
// its window finding are decoded rather than lost. At most max_streams run
// at once; beyond that the strongest candidates win.
//
// A stream prints nothing until its marks show it is Morse: two clusters of
// mark lengths two to five times apart, as clause 2.1's dot and dash are
// three, and most marks near one of them (MorseTiming::fit). It then prints
// what it held and carries on. A stream ends, and hands over the character
// in progress, when its tone has keyed nothing for kStreamIdleSeconds; it is
// ended without printing more when its marks stop looking like Morse, or,
// before it has printed, when most of its characters are codes the table
// does not hold. A stream ended for either keeps its pitch from starting
// again for kStreamMuteSeconds.
//
// Streams are judged, started and ended only at whole multiples of
// kPieceSeconds of audio, so the output does not depend on how the caller
// blocked it.
//
// THE PITCH A STREAM IS LABELLED WITH is the Cw's own, which starts where the
// search's peak was, a parabola through three bins, and is then tracked, so
// the label moves with the tone.
//
// NOT HANDLED
//
// Two tones closer than about two bins, 50 Hz, are one peak to the search and
// one stream, which reads whichever is stronger. A stream's low-pass stops
// about 90 Hz from its tone, so a much stronger station nearer than that
// still keys it. Four or more stations keying at the same moment in one
// receiver's audio look like speech to the search and are not started.

struct CwBandConfig {
    SampleRate rate = 48000;

    // The audio frequencies searched for keyed tones. The upper edge is held
    // below the audio rate's Nyquist frequency. NOT SPECIFIED VALUES: usb's
    // default filter passes 300 to 2700 Hz, and a tone at the edge of a
    // filter is still a tone.
    double low_hz = 200.0;
    double high_hz = 2800.0;

    // Streams decoded at once. NOT A SPECIFIED VALUE: a 2.4 kHz sideband
    // filter parked in a busy CW segment holds half a dozen stations.
    std::size_t max_streams = 6;

    MorseTimingConfig timing{};
};

class CwBand {
   public:
    [[nodiscard]] static Expected<CwBand> create(const CwBandConfig& config);

    [[nodiscard]] Status process(ConstRealSpan audio, std::vector<CwCharacter>& out);

    // Ends every stream, handing over the characters in progress.
    void flush(std::vector<CwCharacter>& out);

    void reset();

    struct StreamInfo {
        std::uint32_t id = 0;
        double pitch_hz = 0.0;
        double wpm = 0.0;
        double overall_wpm = 0.0;
        double level_deviations = 0.0;
    };

    // The streams running now, oldest first.
    [[nodiscard]] std::vector<StreamInfo> streams() const;

    // Streams started since create() or reset(), including ended ones.
    [[nodiscard]] std::uint32_t streams_started() const { return next_id_ - 1; }

   private:
    CwBand() = default;

    struct Stream {
        std::uint32_t id = 0;
        SampleIndex origin = 0;  // CwBand's audio index of the Cw's sample 0
        std::deque<bool> recent;  // recognised, for the last few characters
        std::unique_ptr<Cw> decoder;

        // Characters held until the stream's marks show it is Morse, and
        // whether they have.
        std::vector<CwCharacter> waiting;
        bool confirmed = false;
    };

    struct Muted {
        double pitch_hz = 0.0;
        SampleIndex until = 0;
    };

    void step(ConstRealSpan audio, std::vector<CwCharacter>& out);
    void search(std::span<const Complex32> baseband);
    void evaluate(std::vector<CwCharacter>& out);
    [[nodiscard]] Status start_stream(double pitch_hz, double level, std::vector<CwCharacter>& out);
    void run_stream(Stream& stream, ConstRealSpan audio, std::vector<CwCharacter>& out);
    void deliver(Stream& stream, std::vector<CwCharacter>& fresh, std::vector<CwCharacter>& out);
    void finish(Stream& stream, std::vector<CwCharacter>& out);
    void end_streams(std::vector<CwCharacter>& out);

    CwBandConfig config_{};
    ToneFrontEnd front_{};

    // The short-time spectrum: bin k of an fft_size_ transform of the search
    // front end's output, bins first_bin_ to last_bin_ inclusive.
    std::size_t fft_size_ = 0;
    std::size_t hop_ = 0;
    std::size_t first_bin_ = 0;
    std::size_t last_bin_ = 0;
    std::vector<float> window_;
    std::vector<Complex32> twiddles_;
    std::vector<Complex32> pending_;  // search-rate samples not yet framed
    double bin_hz_ = 0.0;
    double centre_hz_ = 0.0;

    // Per bin, the last window_frames_ frame powers, a ring.
    std::size_t window_frames_ = 0;
    std::size_t evaluate_every_ = 0;
    std::vector<float> power_;
    std::size_t frames_ = 0;

    // Raw audio kept for a new stream's replay, starting at history_start_.
    std::vector<float> history_;
    SampleIndex history_start_ = 0;
    SampleIndex audio_index_ = 0;

    // Streams are judged, started and ended only at whole multiples of
    // piece_ audio samples from the start of the stream, so what comes out
    // does not depend on how the caller blocked the audio.
    std::size_t piece_ = 1;
    bool evaluation_due_ = false;
    std::vector<Complex32> baseband_;

    std::vector<Stream> streams_;
    std::vector<Muted> muted_;
    std::uint32_t next_id_ = 1;
};

}  // namespace revenant::decode
