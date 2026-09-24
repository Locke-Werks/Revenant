#include "core/decode/cw.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <numbers>

namespace revenant::decode {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kSecondsPerMinute = 60.0;

// Runs the unit is estimated over. NOT A SPECIFIED VALUE. Sixteen is three or
// four characters, so a change of speed is followed within about that many
// characters, and still enough runs that the shortest fifth of them is the
// one-unit cluster in any text with more than one element per character.
constexpr std::size_t kRecentRuns = 16;

// Runs needed before a unit is trusted at all. Two characters' worth at the
// least, since a single character's runs can all be one length.
constexpr std::size_t kRunsToLock = 6;

// Letter-or-word spaces kept for the two clusters. Also how many a
// transmission may start with, all one length, before the decoder stops
// waiting for the second cluster and prints them as letter spaces.
constexpr std::size_t kLongSpaces = 12;

// A long space more than this many times the median of the long spaces kept
// is a pause, the gap between one over and the next, and is left out of the
// two clusters; it still ends a word. NOT A SPECIFIED VALUE. Clause 2 puts a
// word space at 7/3 of a letter space and the ARRL standard keeps that ratio
// under Farnsworth, so a word space is never three medians when letter spaces
// are the majority, as they are in any text with words of more than one
// character. Measured on 2026-09-23 on the KF4FIC 20 m 1603 UT recording,
// the station at -10950 Hz, machine sent at 20 WPM: letter spaces 175 to
// 188 ms, word spaces 360 to 369 ms, and pauses of 3491 and 4547 ms between
// its calls. Before this, one pause in the twelve kept put the cut between
// the two clusters at 1.8 seconds, every word space fell under it, and 129
// "TEST"s were printed without the space either side of one.
constexpr double kPauseToMedian = 3.0;

// What counts as marks clause 2.1 would recognise, over the last 24: two
// clusters whose means are two to five times apart, clause 2.1's three with
// room for a sender's weighting and for noise, which shortens dots more than
// dashes, and each mark within 45 per cent of its cluster's mean. NOT
// SPECIFIED VALUES. A hand sender whose every element is stretched by up to
// 20 per cent keeps nearly all of its marks inside that.
constexpr double kFitRatioLow = 2.0;
constexpr double kFitRatioHigh = 5.0;
constexpr double kFitSpread = 0.45;
constexpr std::size_t kFitMarks = 24;

// How long a gap has to run before the decoder stops holding the start of a
// transmission back for its spacing and prints what it has: this many times
// clause 2.4's seven units, and this many times the longest space already
// held. NOT A SPECIFIED VALUE. Either alone would release on an ordinary
// space: seven units is a Farnsworth letter space at 18 words per minute
// characters and 12 overall, and the longest space held can be a letter space
// the gap in progress is the word space after. The gap itself is judged when
// it ends, as every space is, so releasing early costs nothing but the wait.
constexpr double kReleaseGapFactor = 2.0;

// A mark is a dot below this many units and a dash above, and a space is
// inside a character below it. Clause 2 puts the two populations at one and
// three units, so two is the midpoint either way.
constexpr double kDotDashSplitUnits = 2.0;

// Two clusters of long spaces are letter and word spaces when the larger mean
// is at least this multiple of the smaller. Clause 2 gives 7/3 = 2.33 and the
// ARRL standard keeps that ratio under Farnsworth; 1.6 leaves room for a hand
// sender's spacing to wander a long way before it is misread.
constexpr double kWordToLetterRatio = 1.6;

// Envelope filter length as a fraction of the unit. A boxcar no longer than
// an element passes a keyed carrier's edges as straight ramps, so a threshold
// at the middle of the ramp measures every element's length exactly whatever
// the filter's length; 0.8 of a unit leaves room for a sender's short dots.
constexpr double kBoxcarUnits = 0.8;

// Runs shorter than this fraction of a unit are noise and are absorbed into
// the run around them.
constexpr double kGlitchUnits = 0.3;

// Hysteresis about the midpoint between the two tracked levels. Symmetric,
// so the rising edge and the falling edge of a ramp are delayed alike and a
// mark's measured length does not change.
constexpr double kThresholdOn = 0.55;
constexpr double kThresholdOff = 0.45;

// Level tracking. Each mark moves the mark level this fraction of the way to
// its own peak, so the level follows a fade within a few marks. The hold is
// how long the level is kept once the key goes up before it starts to fall
// back towards the noise, and the fall takes as long again. The noise time
// is how quickly the noise quartile moves, and the warm time how much audio
// has to pass before any mark may start. None of these is a specified value.
constexpr double kMarkLevelLearning = 0.3;
constexpr double kLevelHoldSeconds = 3.0;
constexpr double kNoiseSeconds = 1.0;
constexpr double kWarmSeconds = 0.25;

// The noise envelope is Rayleigh distributed: white Gaussian audio, mixed to
// complex baseband and summed by a boxcar, is complex Gaussian, and its
// magnitude is Rayleigh with some scale sigma. Textbook properties of that
// distribution turn the tracked lower quartile into the mean and the spread
// the thresholds need: the quartile is sigma*sqrt(-2 ln 0.75) = 0.7585 sigma,
// the mean sigma*sqrt(pi/2) and the standard deviation
// sigma*sqrt((4 - pi)/2).
constexpr double kNoiseQuantile = 0.25;

// A mark may only start this many standard deviations of the noise envelope
// above its mean. Open is the bar for the first mark out of silence, and
// hold the bar while marks keep arriving; the squelch falls back to open
// after kSquelchHoldSeconds with no mark that cleared the open bar. On
// Rayleigh noise the open bar is 4.86 sigma, which one sample exceeds with
// probability exp(-4.86^2/2) = 7.6e-6, and hold is 2.56 sigma, with 0.037.
//
// Each half was measured on its own at 20 WPM on 2026-09-22, with an earlier
// mark level tracker than the one below. A single bar at 4.5 deviations read
// -10 dB in 2500 Hz at a character error rate of 0.61 against 0.13 with the
// hold, because a weak signal's marks dip under a high bar. A hold renewed by
// any mark decoded 130 characters from a minute of noise after a
// transmission ended. An open bar of 4.5 decoded four characters from that
// minute, and 5.5 decodes none.
constexpr double kSquelchOpenDeviations = 5.5;
constexpr double kSquelchHoldDeviations = 2.0;
constexpr double kSquelchHoldSeconds = 3.0;

// An envelope below this never starts a mark, whatever the noise statistics
// say. Audio with no noise in it at all, which only a synthetic source
// produces, has a noise floor made of float rounding, around 1e-8 of full
// scale; the squelch scaled from that floor keys on the rounding. 1e-5 of
// full scale is 100 dB down, below any receiver's own noise.
constexpr double kDigitalSilence = 1e-5;

// The same squared-carrier acquisition threshold core/decode/psk31.cpp
// measures, for the carrier itself here: noise's best line stood below four.
constexpr double kAcquisitionLineRatio = 6.0;

// How much of the frequency error one mark measures is taken out after it.
// NOT A SPECIFIED VALUE. The error is how far the boxcar's sum turned from
// one sample to the next over the mark, and a dot at 40 WPM holds the boxcar
// whole for only a few samples at 500 S/s, so one dot's reading is noisy at
// a weak signal's SNR; a third of it each time settles within about ten
// marks and averages the noise over as many. Only a mark that cleared the
// squelch's open bar moves it.
constexpr double kTrackGain = 0.3;

// The low-pass after the mixer, which follows the tone. It passes a dot's
// main lobe at the fastest speed decoded, one over 50 WPM's 24 ms unit, 42 Hz
// either side, plus kNeighbourSlackHz for a tone the tracker has not yet
// settled on, and stops kNeighbourTransitionHz above that. NOT SPECIFIED
// VALUES. Without it the front end's own filter, built around centre_hz and
// wide enough for capture_hz of tuning, still passed a station 160 Hz away,
// and the boxcar is a sinc whose sidelobes are 27 dB down there: measured on
// 2026-09-23 with stations at 540 and 700 Hz, both at +10 dB, the stream at
// 540 read "CQ T ST DE EV O9E4GL AIOT" for "CQ TEST DE K9BGL K9BGL TEST".
constexpr double kNeighbourSlackHz = 15.0;
constexpr double kNeighbourTransitionHz = 35.0;

double unit_for_wpm(double wpm) {
    return kParisUnitSecondsTimesWpm / wpm;
}

char upper(char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

// Splits sorted values into two clusters at the cut that maximises the
// between-class variance. Returns false when there is no cut.
bool split_two(std::vector<double> values, double& low_mean, double& high_mean) {
    if (values.size() < 2) {
        return false;
    }
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    double total = 0.0;
    for (const double v : values) {
        total += v;
    }
    double best_score = -1.0;
    double prefix = 0.0;
    for (std::size_t k = 1; k < n; ++k) {
        prefix += values[k - 1];
        const double a = prefix / static_cast<double>(k);
        const double b = (total - prefix) / static_cast<double>(n - k);
        const double score = static_cast<double>(k) * static_cast<double>(n - k) * (b - a) * (b - a);
        if (score > best_score) {
            best_score = score;
            low_mean = a;
            high_mean = b;
        }
    }
    return true;
}

double median_of(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

// The long spaces with the pauses taken out, kPauseToMedian.
std::vector<double> without_pauses(std::vector<double> values) {
    if (values.size() < 3) {
        return values;
    }
    const double limit = kPauseToMedian * median_of(values);
    std::erase_if(values, [limit](double v) { return v > limit; });
    return values;
}

}  // namespace

Expected<std::string_view> morse_code_for(std::string_view token) {
    std::string key(token);
    if (key.size() == 1) {
        key[0] = upper(key[0]);
    } else if (key.size() == 2 && key == "\xC3\xA9") {
        key = "\xC3\x89";  // lower-case e-acute keys the same row
    } else {
        for (char& c : key) {
            c = upper(c);
        }
    }
    for (const MorseSignal& signal : kMorseTable) {
        if (signal.text == key) {
            return signal.code;
        }
    }
    return fail(std::format(
        "ITU-R M.1677-1 clause 1.1 has no signal for '{}'; clause 3 says a sign with no "
        "Morse signal is spelled out, which is the sender's job and not the keyer's",
        token));
}

std::string_view morse_text_for(std::string_view code) {
    for (const MorseSignal& signal : kMorseTable) {
        if (signal.code == code) {
            return signal.text;
        }
    }
    return {};
}

Expected<std::vector<std::string>> morse_tokens(std::string_view text) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '<') {
            const std::size_t close = text.find('>', i);
            if (close == std::string_view::npos) {
                return fail(std::format("unclosed service signal at offset {} of '{}'", i, text));
            }
            tokens.emplace_back(text.substr(i, close - i + 1));
            i = close + 1;
            continue;
        }
        if (static_cast<unsigned char>(c) == 0xC3 && i + 1 < text.size()) {
            tokens.emplace_back(text.substr(i, 2));
            i += 2;
            continue;
        }
        tokens.emplace_back(1, c);
        ++i;
    }
    for (const std::string& token : tokens) {
        if (token == " ") {
            continue;
        }
        if (auto code = morse_code_for(token); !code) {
            return std::unexpected(code.error());
        }
    }
    return tokens;
}

Expected<MorseSpacing> morse_spacing(double character_wpm, double overall_wpm) {
    if (!(character_wpm > 0.0) || !(overall_wpm > 0.0)) {
        return fail(std::format("morse_spacing needs positive speeds; got {} and {}",
                                character_wpm, overall_wpm));
    }
    MorseSpacing spacing;
    spacing.unit_s = unit_for_wpm(character_wpm);
    if (overall_wpm >= character_wpm) {
        spacing.letter_space_s = kMorseLetterSpaceDots * spacing.unit_s;
        spacing.word_space_s = kMorseWordSpaceDots * spacing.unit_s;
        return spacing;
    }
    // ARRL standard clause 2.2 and Appendix A.2:
    //   t_a = (60c - 37.2s) / (sc),  t_c = 3 t_a / 19,  t_w = 7 t_a / 19
    // where 37.2 is the 31 character units of PARIS at 1.2 seconds each.
    const double c = character_wpm;
    const double s = overall_wpm;
    const double added = (kSecondsPerMinute * c -
                          kParisCharacterUnits * kParisUnitSecondsTimesWpm * s) /
                         (s * c);
    spacing.letter_space_s = kMorseLetterSpaceDots * added / kParisSpacingUnits;
    spacing.word_space_s = kMorseWordSpaceDots * added / kParisSpacingUnits;
    return spacing;
}

double morse_overall_wpm(double character_wpm, double letter_space_s) {
    if (!(character_wpm > 0.0) || !(letter_space_s > 0.0)) {
        return 0.0;
    }
    // Clause 2.2 solved for s: t_a = 19 t_c / 3, s = 60c / (t_a c + 37.2).
    const double added = kParisSpacingUnits * letter_space_s / kMorseLetterSpaceDots;
    const double s = kSecondsPerMinute * character_wpm /
                     (added * character_wpm + kParisCharacterUnits * kParisUnitSecondsTimesWpm);
    return std::min(s, character_wpm);
}

// ---------------------------------------------------------------------------
// MorseTiming
// ---------------------------------------------------------------------------

void MorseTiming::reset() {
    locked_ = false;
    unit_ = 0.0;
    speed_unit_ = 0.0;
    recent_.clear();
    held_.clear();
    released_ = false;
    long_spaces_.clear();
    code_.clear();
    character_start_ = 0;
    character_ended_ = true;
    word_ended_ = true;
    any_text_ = false;
    fits_.clear();
}

double MorseTiming::wpm() const {
    return (locked_ && speed_unit_ > 0.0) ? kParisUnitSecondsTimesWpm / speed_unit_ : 0.0;
}

double MorseTiming::overall_wpm() const {
    return morse_overall_wpm(wpm(), letter_space_estimate());
}

void MorseTiming::estimate_unit() {
    if (recent_.size() < kRunsToLock) {
        return;
    }
    std::vector<double> sorted;
    sorted.reserve(recent_.size());
    for (const Run& run : recent_) {
        sorted.push_back(run.seconds);
    }
    std::sort(sorted.begin(), sorted.end());
    const double fifth = sorted[sorted.size() / 5];

    // The one-unit cluster: everything within a factor of two of the
    // twentieth percentile. Dots and element spaces are both in it.
    double sum = 0.0;
    std::size_t count = 0;
    bool longer = false;
    for (const double v : sorted) {
        if (v <= 2.0 * fifth) {
            sum += v;
            ++count;
        }
    }
    const double mean = sum / static_cast<double>(count);

    // The speed is read from the same cluster with its dots and its spaces
    // averaged apart and the two means weighted equally, because clause 2
    // makes both one unit and noise does not leave them so. The threshold
    // sits above the middle of a noisy carrier's edges, so a dot comes out
    // short and an element space long by about the same amount, 50.5 and
    // 69.1 ms for 60 at -8 dB in 2500 Hz, and random text keys more element
    // spaces than dots, so the pooled mean reads long. Over 40 transmissions
    // of 186 characters at -8 dB, measured on 2026-09-23, the pooled mean
    // read 19.36 WPM for 20 and 32.75 for 35, and the equal weighting 20.13
    // and 34.27. A cluster with only one kind in it, the element spaces of a
    // run of dashes, is that kind's mean.
    //
    // The decoder itself keeps the pooled mean. Decoding against the equal
    // weighting was tried and cost characters: the bench sweep's 20 WPM
    // curve got worse at 6 of its 17 points, 0.0592 against 0.0554 at -8 dB,
    // though 35 WPM improved, 0.0504 against 0.0577 at -8 dB over the 40
    // transmissions above. Why decoding prefers the pooled mean at 20 WPM
    // has not been established; the sweep is the case that decides, so the
    // decode path keeps what it measured best on.
    double dot_sum = 0.0;
    double space_sum = 0.0;
    std::size_t dots = 0;
    std::size_t spaces = 0;
    for (const Run& run : recent_) {
        if (run.seconds <= 2.0 * fifth) {
            (run.key_down ? dot_sum : space_sum) += run.seconds;
            ++(run.key_down ? dots : spaces);
        }
    }
    double speed_mean = mean;
    if (dots > 0 && spaces > 0) {
        speed_mean = 0.5 * (dot_sum / static_cast<double>(dots) +
                            space_sum / static_cast<double>(spaces));
    }
    for (const double v : sorted) {
        if (v >= 2.5 * mean) {
            longer = true;
        }
    }
    // Before the first lock there has to be a second cluster, or a run of one
    // length, all dashes or all dots with letter spaces between, could be
    // taken for units whatever length it is.
    if (!locked_ && !longer) {
        return;
    }

    // After it, a new unit has to be confirmed by a dash: a mark at least
    // twice it, which clause 2.1 says a dash is at three. The twentieth
    // percentile is a unit only while a fifth of the recent runs are one unit
    // long, and T, M and O between them can leave fewer: "TMT OT" keys three
    // one-unit runs in fifteen, the percentile lands on a dash, the cluster
    // holds dashes and letter spaces, and the first version took the unit for
    // somewhere between one and three and printed dashes after it as dots.
    // No mark in that window is twice such a unit, where a real change of
    // speed brings its own dashes with it. A window with no dash at all keeps
    // the unit it had, which costs nothing: its dots are read against the
    // unit that was right.
    if (locked_) {
        const bool confirmed = std::ranges::any_of(recent_, [&](const Run& run) {
            return run.key_down && run.seconds >= kDotDashSplitUnits * mean;
        });
        if (!confirmed) {
            return;
        }
    }
    const double shortest = unit_for_wpm(config_.max_wpm);
    const double longest = unit_for_wpm(config_.min_wpm);
    unit_ = std::clamp(mean, shortest, longest);
    speed_unit_ = std::clamp(speed_mean, shortest, longest);
    locked_ = true;
}

double MorseTiming::letter_space_estimate() const {
    if (long_spaces_.empty()) {
        return kMorseLetterSpaceDots * unit_;
    }
    double low = 0.0;
    double high = 0.0;
    const std::vector<double> values =
        without_pauses(std::vector<double>(long_spaces_.begin(), long_spaces_.end()));
    if (split_two(values, low, high) && high >= kWordToLetterRatio * low) {
        return low;
    }
    double sum = 0.0;
    for (const double v : values) {
        sum += v;
    }
    return sum / static_cast<double>(values.size());
}

double MorseTiming::word_threshold() const {
    // Clause 2.3 and 2.4 put letters at three units and words at seven; the
    // ARRL standard stretches both by the same factor under Farnsworth. So
    // the cut is at five of whatever the letter space's three turned out to
    // be, or midway between the two clusters when both have been seen.
    const double midpoint_over_letter =
        0.5 * (kMorseLetterSpaceDots + kMorseWordSpaceDots) / kMorseLetterSpaceDots;
    if (long_spaces_.empty()) {
        return midpoint_over_letter * kMorseLetterSpaceDots * unit_;
    }
    double low = 0.0;
    double high = 0.0;
    const std::vector<double> values =
        without_pauses(std::vector<double>(long_spaces_.begin(), long_spaces_.end()));
    if (split_two(values, low, high) && high >= kWordToLetterRatio * low) {
        return 0.5 * (low + high);
    }
    return midpoint_over_letter * letter_space_estimate();
}

void MorseTiming::end_character(std::vector<CwCharacter>& out) {
    if (character_ended_) {
        return;
    }
    CwCharacter character;
    character.code = code_;
    const std::string_view text = morse_text_for(code_);
    character.recognised = !text.empty();
    character.text = std::string(text);
    character.first_sample = character_start_;
    character.wpm = wpm();
    out.push_back(std::move(character));
    character_ended_ = true;
    any_text_ = true;
}

void MorseTiming::apply_mark(double seconds, SampleIndex start, std::vector<CwCharacter>&) {
    if (character_ended_) {
        code_.clear();
        character_start_ = start;
        character_ended_ = false;
    }
    code_.push_back(seconds < kDotDashSplitUnits * unit_ ? '.' : '-');
    word_ended_ = false;
}

void MorseTiming::apply_space(double seconds, SampleIndex start, bool counted,
                              std::vector<CwCharacter>& out) {
    if (seconds < kDotDashSplitUnits * unit_) {
        return;
    }
    if (!counted) {
        remember_long_space(seconds);
    }
    end_character(out);
    if (!word_ended_ && any_text_ && seconds >= word_threshold()) {
        CwCharacter space;
        space.text = " ";
        space.recognised = true;
        space.first_sample = start;
        space.wpm = wpm();
        out.push_back(std::move(space));
        word_ended_ = true;
    }
}

void MorseTiming::remember_long_space(double seconds) {
    // A pause is not kept at all, so the twelve kept are twelve spaces of
    // the text. The check reads the ones already kept, and once there are
    // three of them; before that without_pauses sorts it out when the
    // clusters are read.
    if (long_spaces_.size() >= 3 &&
        seconds > kPauseToMedian *
                      median_of(std::vector<double>(long_spaces_.begin(), long_spaces_.end()))) {
        return;
    }
    long_spaces_.push_back(seconds);
    if (long_spaces_.size() > kLongSpaces) {
        long_spaces_.pop_front();
    }
}

bool MorseTiming::spacing_known() const {
    // The held long spaces, by the unit as it now stands.
    std::vector<double> spaces;
    for (const Run& run : held_) {
        if (!run.key_down && run.seconds >= kDotDashSplitUnits * unit_) {
            spaces.push_back(run.seconds);
        }
    }
    if (spaces.size() >= kLongSpaces) {
        return true;
    }
    double low = 0.0;
    double high = 0.0;
    return split_two(without_pauses(std::move(spaces)), low, high) &&
           high >= kWordToLetterRatio * low;
}

void MorseTiming::release(std::vector<CwCharacter>& out) {
    released_ = true;
    const std::vector<Run> waiting = std::move(held_);
    held_.clear();

    // Every held long space joins the clusters before any of them is judged,
    // so the first is read against the ones after it rather than alone.
    for (const Run& run : waiting) {
        if (!run.key_down && run.seconds >= kDotDashSplitUnits * unit_) {
            remember_long_space(run.seconds);
        }
    }
    for (const Run& run : waiting) {
        if (run.key_down) {
            apply_mark(run.seconds, run.start, out);
        } else {
            apply_space(run.seconds, run.start, true, out);
        }
    }
}

void MorseTiming::remember_run(bool key_down, double seconds) {
    recent_.push_back(Run{key_down, seconds, 0});
    if (recent_.size() > kRecentRuns) {
        recent_.pop_front();
    }
}

double MorseTiming::fit() const {
    // The kept marks split into their own two clusters, which clause 2.1
    // puts three to one apart, and each mark against the nearer. Not
    // against the unit: the unit is read from dots and element spaces
    // together and a hand sender moves it, where the two clusters of marks
    // are there whatever the spaces do.
    double dots = 0.0;
    double dashes = 0.0;
    if (fits_.size() < 2 ||
        !split_two(std::vector<double>(fits_.begin(), fits_.end()), dots, dashes) ||
        !(dots > 0.0)) {
        return 0.0;
    }
    const double ratio = dashes / dots;
    if (ratio < kFitRatioLow || ratio > kFitRatioHigh) {
        return 0.0;
    }
    std::size_t good = 0;
    for (const double seconds : fits_) {
        const double nearer =
            std::abs(std::log(seconds / dots)) < std::abs(std::log(seconds / dashes)) ? dots
                                                                                       : dashes;
        good += std::abs(seconds / nearer - 1.0) <= kFitSpread ? 1U : 0U;
    }
    return static_cast<double>(good) / static_cast<double>(fits_.size());
}

void MorseTiming::mark(double seconds, SampleIndex start, std::vector<CwCharacter>& out) {
    remember_run(true, seconds);
    estimate_unit();
    fits_.push_back(seconds);
    if (fits_.size() > kFitMarks) {
        fits_.pop_front();
    }
    if (!released_) {
        held_.push_back(Run{true, seconds, start});
        if (locked_ && spacing_known()) {
            release(out);
        }
        return;
    }
    apply_mark(seconds, start, out);
}

void MorseTiming::space(double seconds, SampleIndex start, std::vector<CwCharacter>& out) {
    if (recent_.empty() && held_.empty()) {
        // Silence before the first mark is not a space between anything.
        return;
    }
    if (!locked_ && seconds > kMorseWordSpaceDots * unit_for_wpm(config_.min_wpm)) {
        // Marks that never made a unit, followed by a gap longer than a word
        // space at the slowest speed read, were not the start of a
        // transmission. They were clicks: measured on 2026-09-23, a station
        // 170 Hz away at +24 dB in 2500 Hz keyed three marks of 16 to 20 ms
        // into a stream through its edges, two seconds before the stream's
        // own station began, and the unit locked on them at 24 ms, so the
        // station's first dots at 48 ms read as dashes and "K9BGL" printed
        // as "EE E O T# B G L".
        recent_.clear();
        held_.clear();
        return;
    }
    remember_run(false, seconds);
    if (!released_) {
        held_.push_back(Run{false, seconds, start});
        if (locked_ && spacing_known()) {
            release(out);
        }
        return;
    }
    apply_space(seconds, start, false, out);
}

void MorseTiming::idle(double seconds, std::vector<CwCharacter>& out) {
    if (!locked_) {
        return;
    }
    if (!released_) {
        // A transmission that stopped before its spacing showed two clusters:
        // one word, or one-character words, then a pause. Printed once the
        // pause is plainly more than a space inside the transmission, with
        // whatever clusters the held spaces make.
        double longest = kMorseWordSpaceDots * unit_;
        for (const Run& run : held_) {
            if (!run.key_down) {
                longest = std::max(longest, run.seconds);
            }
        }
        if (seconds < kReleaseGapFactor * longest) {
            return;
        }
        release(out);
    }
    if (seconds >= kDotDashSplitUnits * unit_) {
        end_character(out);
    }
    // The word space itself is emitted when the space completes, where it
    // also joins the clusters; emitting it here as well would put it out
    // before the gap that justifies it had been counted. A caller at the end
    // of a capture calls flush, which needs no word space after it.
}

void MorseTiming::flush(std::vector<CwCharacter>& out) {
    // Never locked means the runs never showed two lengths, and a unit
    // guessed from one length would decode them as whichever element the
    // guess made them. Nothing is reported rather than that.
    if (!locked_) {
        return;
    }
    if (!released_) {
        release(out);
    }
    end_character(out);
}

// ---------------------------------------------------------------------------
// Cw
// ---------------------------------------------------------------------------

Expected<Cw> Cw::create(const CwConfig& config) {
    if (config.rate <= 0) {
        return fail(std::format("Cw needs a positive audio rate; got {}", config.rate));
    }
    if (!(config.timing.min_wpm > 0.0) || !(config.timing.max_wpm > config.timing.min_wpm)) {
        return fail(std::format("Cw needs 0 < min_wpm < max_wpm; got {} and {}",
                                config.timing.min_wpm, config.timing.max_wpm));
    }
    if (!(config.acquisition_seconds > 0.0) || !(config.capture_hz > 0.0)) {
        return fail("Cw needs a positive acquisition time and capture range");
    }

    Cw decoder;
    decoder.config_ = config;
    decoder.timing_ = MorseTiming(config.timing);

    // A keyed carrier's sidebands reach roughly one over the element length
    // either side; two over the shortest dot keeps the edges of the fastest
    // keying the timing decoder accepts.
    const double shortest = unit_for_wpm(config.timing.max_wpm);
    ToneFrontEndConfig front;
    front.rate = config.rate;
    front.centre_hz = config.centre_hz;
    front.passband_hz = config.capture_hz + 2.0 / shortest;
    front.minimum_output_rate = 500;
    auto made = ToneFrontEnd::create(front);
    if (!made) {
        return std::unexpected(with_context(made.error(), "creating the CW front end"));
    }
    decoder.front_ = std::move(*made);

    const double rate = static_cast<double>(decoder.front_.output_rate());
    const double longest = unit_for_wpm(config.timing.min_wpm);
    decoder.history_limit_ =
        static_cast<std::size_t>(std::ceil(kBoxcarUnits * longest * rate)) + 1;
    decoder.set_boxcar(static_cast<std::size_t>(std::lround(kBoxcarUnits * shortest * rate)));
    decoder.acquired_ = !config.acquire;
    decoder.seed_level();

    const double pass = 1.0 / shortest + kNeighbourSlackHz;
    auto narrow_taps = static_cast<std::size_t>(std::ceil(3.3 * rate / kNeighbourTransitionHz));
    narrow_taps |= 1U;
    auto narrow = design_lowpass(decoder.front_.output_rate(),
                                 std::min(pass + 0.5 * kNeighbourTransitionHz, 0.45 * rate),
                                 narrow_taps);
    if (!narrow) {
        return std::unexpected(with_context(narrow.error(), "designing the CW tone filter"));
    }
    decoder.narrow_taps_ = std::move(*narrow);
    return decoder;
}

void Cw::reset() {
    front_.reset();
    timing_.reset();
    acquisition_.clear();
    acquired_ = !config_.acquire;
    acquired_at_ = 0;
    offset_hz_ = 0.0;
    phase_ = 0.0;
    narrow_history_.clear();
    turn_ = {0.0, 0.0};
    previous_sum_ = {0.0, 0.0};
    index_ = 0;
    history_.clear();
    const double rate = static_cast<double>(front_.output_rate());
    set_boxcar(static_cast<std::size_t>(
        std::lround(kBoxcarUnits * unit_for_wpm(config_.timing.max_wpm) * rate)));
    high_ = 0.0;
    noise_quartile_ = 0.0;
    warm_.clear();
    heard_ = false;
    have_level_ = false;
    last_mark_ = 0;
    mark_peak_ = 0.0;
    raw_key_ = false;
    raw_since_ = 0;
    key_ = false;
    key_since_ = 0;
    seed_level();
}

void Cw::seed_level() {
    if (config_.expected_level > 0.0) {
        high_ = config_.expected_level;
        have_level_ = true;
    }
}

double Cw::mark_level() const {
    // The mark just ended ran from its rising half-amplitude crossing to its
    // falling one, and the boxcar was wholly inside it for all but half a
    // boxcar at each end. Those samples, and only those, see the carrier's
    // full level; their mean is an unbiased reading of it, where the peak of
    // the whole mark is biased upwards by the noise it rode on, and that
    // bias pushed the thresholds up until marks came out short and spaces
    // long. The recorded run also carries the debounce's worth of samples
    // past the falling crossing, which the upper bound here drops.
    const std::size_t half = boxcar_ / 2;
    const std::size_t samples = mark_envelope_.size();
    const auto debounce = static_cast<std::size_t>(index_ + 1 - raw_since_);
    const std::size_t end = samples > debounce + half ? samples - debounce - half : 0;
    if (end <= half) {
        return mark_peak_;
    }
    double sum = 0.0;
    for (std::size_t i = half; i < end; ++i) {
        sum += mark_envelope_[i];
    }
    return sum / static_cast<double>(end - half);
}

double Cw::noise_sigma() const {
    return noise_quartile_ / std::sqrt(-2.0 * std::log(1.0 - kNoiseQuantile));
}

double Cw::noise_mean() const {
    return noise_sigma() * std::sqrt(0.5 * kPi);
}

double Cw::noise_spread() const {
    return noise_sigma() * std::sqrt(0.5 * (4.0 - kPi));
}

double Cw::level_deviations() const {
    const double spread = noise_spread();
    return spread > 0.0 ? (high_ - noise_mean()) / spread : 0.0;
}

void Cw::set_boxcar(std::size_t length) {
    boxcar_ = std::clamp<std::size_t>(length, 1, history_limit_);
}

SampleIndex Cw::to_audio_sample(double decimated_index) const {
    const double centred = decimated_index - 0.5 * static_cast<double>(boxcar_ - 1) -
                           0.5 * static_cast<double>(narrow_taps_.size() - 1);
    const double input = centred * static_cast<double>(front_.decimation()) -
                         static_cast<double>(front_.group_delay());
    return input > 0.0 ? static_cast<SampleIndex>(std::llround(input)) : SampleIndex{0};
}

void Cw::label(std::vector<CwCharacter>& out, std::size_t from) const {
    for (std::size_t i = from; i < out.size(); ++i) {
        out[i].pitch_hz = pitch_hz();
    }
}

void Cw::flush(std::vector<CwCharacter>& out) {
    const std::size_t from = out.size();
    timing_.flush(out);
    label(out, from);
}

double Cw::seconds_since_mark() const {
    if (!acquired_) {
        return 0.0;
    }
    const SampleIndex since = heard_ ? last_mark_ : acquired_at_;
    return index_ > since
               ? static_cast<double>(index_ - since) / static_cast<double>(front_.output_rate())
               : 0.0;
}

Status Cw::process(ConstRealSpan audio, std::vector<CwCharacter>& out) {
    const std::size_t from = out.size();
    decimated_.clear();
    front_.process(audio, decimated_);
    if (acquired_) {
        run(decimated_, out);
        label(out, from);
        return {};
    }

    acquisition_.insert(acquisition_.end(), decimated_.begin(), decimated_.end());
    const auto needed = static_cast<std::size_t>(
        std::ceil(config_.acquisition_seconds * static_cast<double>(front_.output_rate())));
    while (!acquired_ && acquisition_.size() >= needed) {
        auto estimate = estimate_tone_offset(
            std::span<const Complex32>(acquisition_.data(), needed), front_.output_rate(), 1,
            config_.capture_hz);
        if (!estimate) {
            return std::unexpected(with_context(estimate.error(), "finding the CW tone"));
        }
        if (estimate->line_to_mean < kAcquisitionLineRatio) {
            const std::size_t slide = needed / 4;
            acquisition_.erase(acquisition_.begin(),
                               acquisition_.begin() + static_cast<std::ptrdiff_t>(slide));
            index_ += slide;
            continue;
        }
        offset_hz_ = estimate->offset_hz;
        acquired_ = true;
        acquired_at_ = index_;
        key_since_ = index_;
        raw_since_ = index_;
    }
    if (acquired_) {
        std::vector<Complex32> held;
        held.swap(acquisition_);
        run(held, out);
    }
    label(out, from);
    return {};
}

void Cw::run(std::span<const Complex32> baseband, std::vector<CwCharacter>& out) {
    const double rate = static_cast<double>(front_.output_rate());
    const double hold = 1.0 / (kLevelHoldSeconds * rate);
    const double noise = 1.0 / (kNoiseSeconds * rate);

    // Output samples until the front end's whole low-pass has seen input,
    // and the boxcar after it. Counted from the start of the stream, which is
    // where index_ counts from, so a stream acquired late has long passed it.
    const auto settle = static_cast<SampleIndex>(
        (2 * front_.group_delay() + front_.decimation() - 1) / front_.decimation() +
        narrow_taps_.size() + boxcar_);

    for (const Complex32 sample : baseband) {
        const std::complex<double> mixed =
            std::complex<double>{sample.real(), sample.imag()} * std::polar(1.0, -phase_);
        phase_ = std::fmod(phase_ + 2.0 * kPi * offset_hz_ / rate, 2.0 * kPi);
        narrow_history_.push_back(
            Complex32{static_cast<float>(mixed.real()), static_cast<float>(mixed.imag())});
        if (narrow_history_.size() > narrow_taps_.size()) {
            narrow_history_.pop_front();
        }
        std::complex<double> value{0.0, 0.0};
        for (std::size_t i = 0; i < narrow_history_.size(); ++i) {
            const Complex32 x = narrow_history_[narrow_history_.size() - 1 - i];
            value += static_cast<double>(narrow_taps_[i]) * std::complex<double>{x.real(), x.imag()};
        }
        history_.push_back(
            Complex32{static_cast<float>(value.real()), static_cast<float>(value.imag())});
        if (history_.size() > history_limit_) {
            history_.pop_front();
        }

        // The envelope: magnitude of a boxcar over the last boxcar_ samples,
        // normalised so a steady carrier of amplitude A reads A.
        std::complex<double> sum{0.0, 0.0};
        const std::size_t have = std::min(boxcar_, history_.size());
        for (std::size_t i = 0; i < have; ++i) {
            const Complex32 x = history_[history_.size() - 1 - i];
            sum += std::complex<double>{x.real(), x.imag()};
        }
        const double envelope = std::abs(sum) / static_cast<double>(boxcar_);

        // The tracker reads how far the boxcar's sum turned since the sample
        // before, while the boxcar lies wholly inside a mark. The sum of a
        // tone off by f turns by 2 pi f / rate a sample, as the tone does,
        // with the boxcar's gain over the noise. The first version summed
        // each corrected sample against the one before, which has none, and
        // measured on 2026-09-23 the frequency it tracked wandered enough
        // that `bench sweep --mode cw` crossed 0.01 at -5.61 dB, against
        // -6.86 with tracking off and -6.85 with this.
        if (key_ && index_ >= key_since_ + boxcar_) {
            turn_ += sum * std::conj(previous_sum_);
        }
        previous_sum_ = sum;

        // Each level learns only from samples whose whole boxcar sits inside
        // one run, so the ramps at a mark's edges teach neither. Without that
        // the one-unit spaces inside a long character, a figure's five
        // elements say, feed half-height ramp samples to the noise tracker,
        // which climbs until the squelch shuts and the rest of the
        // transmission is lost. That is what the first version did.
        if (key_) {
            // The mark level learns once per mark, from the mark's peak, in
            // the transition below. A sample-by-sample average was tried
            // first and cannot work: a dot only fills the boxcar for a fifth
            // of a unit, and no fixed delay after the mark starts lands
            // inside that fifth for every speed and every mark. Until the
            // first mark ends there is no level, so it rises with the first
            // mark's envelope.
            if (!have_level_) {
                high_ = std::max(high_, envelope);
            }
        } else {
            // The noise floor as a running lower quartile, which a keyed
            // carrier that is up less than three quarters of the time cannot
            // drag upwards. Before the first mark the carrier's own samples
            // are in here too; the quartile of that mixture sits higher than
            // the noise's, which makes the squelch start out conservative
            // rather than open.
            //
            // The first kWarmSeconds are sorted for a starting quartile, since
            // a tracker started from one sample takes seconds to climb out of
            // wherever that sample happened to be, and decodes noise the
            // while.
            //
            // Nothing is learned before the front end's low-pass and the
            // boxcar have filled. Until then the envelope is the filter's
            // start-up transient, rising from zero, and at 48000 S/s that is
            // 25 of the 125 warm samples. The first version sorted them in
            // with the noise: the starting quartile came out 0.0090 where
            // the noise's settles at 0.0180, so the open bar stood about 1.8
            // noise deviations above the real mean rather than 5.5, and 19 of
            // 200 transmissions at +10 dB began with characters keyed by the
            // lead-in's noise. With the fill left out it was 1 of 200.
            const auto warm_samples = static_cast<std::size_t>(kWarmSeconds * rate);
            if (index_ < settle) {
                // Still filling.
            } else if (warm_.size() < warm_samples) {
                warm_.push_back(envelope);
                if (warm_.size() == warm_samples) {
                    std::vector<double> sorted = warm_;
                    std::sort(sorted.begin(), sorted.end());
                    noise_quartile_ = sorted[static_cast<std::size_t>(
                        kNoiseQuantile * static_cast<double>(sorted.size()))];
                }
            } else {
                const double below = envelope < noise_quartile_ ? 1.0 : 0.0;
                noise_quartile_ += noise_quartile_ * noise * 4.0 * (kNoiseQuantile - below);
            }
            // The mark level is held through ordinary gaps and only starts to
            // fall once the key has been up for a hold time. Decaying it in
            // every gap, as the first version did, let a run of dots drag it
            // to seven tenths of the real level, because a dot teaches the
            // tracker for a fifth of a unit and the gaps around it forget for
            // two; the thresholds then sat at a third of the carrier instead
            // of half, and every mark came out a fifth of a unit long.
            const double key_up = static_cast<double>(index_ + 1 - key_since_) / rate;
            if (key_up > kLevelHoldSeconds) {
                high_ -= (high_ - noise_mean()) * hold;
            }
        }

        // A mark starts at the midpoint between the noise and the mark level,
        // and never below the squelch, which is kSquelchOpenDeviations spreads
        // of the noise envelope above its mean while nothing has been heard
        // and kSquelchHoldDeviations while a signal is keying.
        const double since_mark = static_cast<double>(index_ - last_mark_) / rate;
        const bool keying = heard_ && since_mark < kSquelchHoldSeconds;
        const double squelch = keying ? kSquelchHoldDeviations : kSquelchOpenDeviations;
        const double mean = noise_mean();
        const double spread = noise_spread();
        const double span = std::max(high_ - mean, 0.0);
        const double on = std::max(mean + std::max(kThresholdOn * span, squelch * spread),
                                   kDigitalSilence);
        const double off = mean + std::max(kThresholdOff * span, 0.5 * squelch * spread);
        const bool warm = warm_.size() >= static_cast<std::size_t>(kWarmSeconds * rate);
        const bool raw = raw_key_ ? (envelope >= off) : (warm && envelope > on);
        if (raw != raw_key_) {
            raw_key_ = raw;
            raw_since_ = index_;
        }

        const double unit = timing_.locked() ? timing_.unit_seconds()
                                             : unit_for_wpm(config_.timing.max_wpm);
        const auto glitch = std::max<SampleIndex>(
            2, static_cast<SampleIndex>(std::lround(kGlitchUnits * unit * rate)));

        if (key_) {
            mark_peak_ = std::max(mark_peak_, envelope);
            mark_envelope_.push_back(envelope);
        }
        if (raw_key_ != key_ && index_ + 1 - raw_since_ >= glitch) {
            const double seconds = static_cast<double>(raw_since_ - key_since_) / rate;
            if (key_) {
                // Only a mark that would have cleared the open bar keeps the
                // squelch at the hold bar. Otherwise noise that slipped under
                // the hold bar would renew it for ever, which is what the
                // first version of the hold did: 130 characters from a minute
                // of noise after a transmission ended.
                if (mark_peak_ >= mean + kSquelchOpenDeviations * spread) {
                    heard_ = true;
                    last_mark_ = raw_since_;
                    if (config_.track && std::abs(turn_) > 0.0) {
                        const double residual = std::arg(turn_) * rate / (2.0 * kPi);
                        offset_hz_ = std::clamp(offset_hz_ + kTrackGain * residual,
                                                -config_.capture_hz, config_.capture_hz);
                    }
                }
                high_ = have_level_ ? high_ + (mark_level() - high_) * kMarkLevelLearning
                                    : mark_level();
                have_level_ = true;
                timing_.mark(seconds, to_audio_sample(static_cast<double>(key_since_)), out);
                if (timing_.locked()) {
                    // Shrinks at once and grows a tenth at a time. Growing
                    // freely ran away on noise at 35 WPM: a unit read long
                    // widened the boxcar, the wider boxcar filled the
                    // one-unit spaces, the filled spaces took the short runs
                    // out of the unit estimate, and the estimate settled at
                    // 13 WPM with the text lost.
                    const auto wanted = static_cast<std::size_t>(
                        std::lround(kBoxcarUnits * timing_.unit_seconds() * rate));
                    const std::size_t ceiling = boxcar_ + std::max<std::size_t>(1, boxcar_ / 10);
                    set_boxcar(std::min(wanted, ceiling));
                }
            } else {
                timing_.space(seconds, to_audio_sample(static_cast<double>(key_since_)), out);
            }
            key_ = raw_key_;
            key_since_ = raw_since_;
            mark_peak_ = envelope;
            mark_envelope_.clear();
            turn_ = {0.0, 0.0};
        }
        if (!key_) {
            timing_.idle(static_cast<double>(index_ + 1 - key_since_) / rate, out);
        }
        ++index_;
    }
}

// ---------------------------------------------------------------------------
// CwBand
// ---------------------------------------------------------------------------

namespace {

// The search's spectrum. Bins no wider than this, and a frame every quarter
// of a transform. NOT SPECIFIED VALUES: a bin has to be narrower than the
// gap between two stations worth telling apart and a frame shorter than a
// letter space at the fastest speed decoded, 50 WPM's 72 ms. At 48000 S/s
// audio the search runs at 4000 S/s, so 128 points, 31.25 Hz and a 32 ms
// frame every 8 ms.
constexpr double kSearchBinHz = 32.0;
constexpr std::size_t kSearchHopDivisor = 4;

// Hertz kept past each end of the search range by the search's front end.
constexpr double kSearchMarginHz = 50.0;

// How much spectrum a bin is judged over, how often, and how much audio a
// new stream is given to read before the present. NOT SPECIFIED VALUES. At
// 12 WPM two and a half seconds is 25 units, which holds a two-letter call
// and the spaces either side of it; the replay is the window and half a
// second more, so a stream's own noise estimate has something before the
// first mark it was started for.
constexpr double kSearchWindowSeconds = 2.5;
constexpr double kSearchEvaluateSeconds = 0.25;
constexpr double kSearchReplaySeconds = 3.0;

// A bin's noise is the lower fifth of its own power over the window, which
// for exponentially distributed noise power is -ln(0.8) = 0.223 of the mean,
// and no less than the median of that over the bins this many either side.
// NOT A SPECIFIED VALUE for the neighbourhood: 125 Hz either side at 31 Hz a
// bin, narrower than any receiver filter's shape changes over.
constexpr double kSearchNoiseQuantile = 0.2;
constexpr std::size_t kNoiseNeighbourBins = 4;

// A bin is keyed when three things hold over the window. Its mean power is at
// least kKeyedExcess times its noise, which noise alone does not reach: the
// mean of several hundred frames of it sits within a tenth of the noise. It
// went up past kKeyedUpFactor times its noise in at least kKeyedMinimumRuns
// runs of two frames or more, a single frame being what noise makes. And it
// was up for no more than kKeyedMaximumDuty of the window, which a steady
// carrier is not, since its lower fifth is the carrier and its mean never
// clears its own noise estimate. NOT SPECIFIED VALUES. Four runs is two
// characters at the least; Morse keys its carrier for under half the time,
// PARIS for 22 units of 50, which the window's smearing of the one-unit
// spaces raises but not to 85 per cent.
//
// WHAT THE FIRST VERSION DID: counted runs past eight times the noise and
// had no excess test. Eight was needed to keep noise from counting runs, and
// it missed a keyed tone at -10 dB in 2500 Hz, 5.5 dB over the noise in a
// 47 Hz Hann bin while up: measured on 2026-09-23, the bench sweep read 0.50
// at -10 dB against the single-tone decoder's 0.28 on the same trials.
constexpr double kKeyedExcess = 1.5;
constexpr double kKeyedUpFactor = 3.0;
constexpr std::size_t kKeyedMinimumRuns = 4;
constexpr std::size_t kKeyedMinimumRunFrames = 2;
constexpr double kKeyedMaximumDuty = 0.85;

// A frame is on in a bin, for telling splatter and speech from a station, at
// eight times the bin's mean noise power, 9 dB, which noise alone exceeds in
// one frame of e^-8 = 3.4e-4. NOT A SPECIFIED VALUE.
constexpr double kKeyedOnFactor = 8.0;

// A candidate closer than this to a stream's pitch, or to a muted one, is
// that stream. Two bins, NOT A SPECIFIED VALUE.
constexpr double kStreamSpacingHz = 50.0;

// A keyed bin is a stronger tone's splatter, and not a station, when a
// stronger keyed bin within kSplatterReachHz was on within
// kSplatterSlackFrames of at least kSplatterShared of the frames it was on.
// NOT SPECIFIED VALUES. Measured on 2026-09-23 with a tone at +10 dB in
// 2500 Hz and 12 to 40 WPM, the frames holding its edges put streams 56 to
// 88 Hz either side of it that printed its keying as T and E. Two stations in
// a conversation take turns, so they share few frames; two keying at once
// share about as many as their duty cycles make likely, well under 80 per
// cent.
constexpr double kSplatterReachHz = 250.0;
constexpr std::size_t kSplatterSlackFrames = 2;
constexpr double kSplatterShared = 0.8;

// A candidate is speech when, over the frames it was on, the groups of
// adjacent bins on at the same time at least kVoiceApartHz from it averaged
// kVoiceGroups or more. NOT SPECIFIED VALUES. Measured on 2026-09-23 over the
// cases in tests/decode/test_cw_band.cpp: every candidate a minute of speech
// on a usb receiver put up averaged 4.5 to 13.3 other groups, and every
// candidate in the CW cases 0.01 to 1.7, the most where two stations key at
// once. So four or more stations keying at the same moment in one receiver's
// audio would read as speech; one to three do not.
constexpr double kVoiceApartHz = 100.0;
constexpr double kVoiceGroups = 3.0;

// Each stream's Cw may follow its tone this far from where it started, and
// its front end is built this much wider than the fastest keying's
// sidebands. The search's pitch is good to a few hertz; the rest is drift.
constexpr double kStreamCaptureHz = 25.0;

// A stream with nothing keyed for this long is over. NOT A SPECIFIED VALUE:
// longer than any word space down to 5 WPM, 1.7 s, and than the pause
// between two overs in the recordings docs/sensitivity.md reads, 4.5 s.
constexpr double kStreamIdleSeconds = 10.0;

// A stream that has not yet printed is judged on its last eight characters,
// and ended when more than half of them were codes the table does not hold.
// A stream ended for that, or as not Morse by the fit below, leaves its pitch
// alone for twenty seconds. NOT SPECIFIED VALUES.
constexpr std::size_t kStreamJudgeCharacters = 8;
constexpr double kStreamMuteSeconds = 20.0;

// A stream prints nothing until MorseTiming::fit() reads at least kConfirmFit
// over kConfirmMarks marks, and then prints what it held. It is ended as not
// Morse once it has kFitJudgeMarks marks and its fit is under kConfirmFit
// before it printed, or under kKeepFit after. A transmission that ends before
// kConfirmMarks is printed if it keyed kShortMarks or more and fit. NOT
// SPECIFIED VALUES. Measured on 2026-09-23 over the cases in
// tests/decode/test_cw_band.cpp: machine sent CW at 12 to 40 WPM fit 1.00 over
// its last 24 marks and a hand sender with 20 per cent jitter at least 0.96,
// while of the 28 streams a minute of speech on a usb receiver started, 25
// were ended as not Morse at fits of 0 to 0.79. That minute printed 747
// characters in 6 streams before this and 57 in 3 after; confirming on 16
// marks rather than 12 printed the same 57. Four marks is "TU".
constexpr std::size_t kConfirmMarks = 12;
constexpr std::size_t kFitJudgeMarks = 24;
constexpr std::size_t kShortMarks = 4;
constexpr double kConfirmFit = 0.8;
constexpr double kKeepFit = 0.5;

// A new stream's mark level starts at this fraction of the amplitude its
// search bin saw while on. NOT A SPECIFIED VALUE. The bin reads the tone low
// by up to 1.4 dB when it falls between two bins, and lower still over frames
// only part filled, so the seed sits under the tone and the first marks raise
// it. Without a seed the level came from the first marks, and measured on
// 2026-09-23 the first marks a stream saw could be the clicks of a station
// 160 Hz away keying at the same time, 28 dB under its own tone: the
// thresholds sat at the clicks and the stream's first characters were dots,
// "EHSEI<END OF WORK>9BGL" for "K9BGL".
constexpr double kSeedLevelFraction = 0.6;

// Streams are started and ended on a grid of this many seconds of audio,
// counted from the first sample, and never between. NOT A SPECIFIED VALUE;
// short against the quarter second between judgements.
constexpr double kPieceSeconds = 0.01;

// In-place radix-2 transform, size a power of two, twiddles[k] =
// exp(-j 2 pi k / size) for k below size / 2.
void transform(std::vector<Complex32>& data, const std::vector<Complex32>& twiddles) {
    const std::size_t n = data.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const std::size_t step = n / length;
        for (std::size_t start = 0; start < n; start += length) {
            for (std::size_t k = 0; k < length / 2; ++k) {
                const Complex32 w = twiddles[k * step];
                const Complex32 a = data[start + k];
                const Complex32 b = data[start + k + length / 2] * w;
                data[start + k] = a + b;
                data[start + k + length / 2] = a - b;
            }
        }
    }
}

}  // namespace

Expected<CwBand> CwBand::create(const CwBandConfig& config) {
    if (config.rate <= 0) {
        return fail(std::format("CwBand needs a positive audio rate; got {}", config.rate));
    }
    if (config.max_streams == 0) {
        return fail("CwBand needs room for at least one stream");
    }
    const double nyquist = 0.5 * static_cast<double>(config.rate);
    const double low = std::max(config.low_hz, kStreamCaptureHz + 2.0 * kSearchMarginHz);
    const double high = std::min(config.high_hz, nyquist - 2.0 * kSearchMarginHz);
    if (!(high > low + 4.0 * kSearchBinHz)) {
        return fail(std::format(
            "CwBand cannot search {} to {} Hz of audio at {} S/s: the range has to lie inside "
            "the audio band with {} Hz to spare at each end and be at least {} Hz wide",
            config.low_hz, config.high_hz, config.rate, 2.0 * kSearchMarginHz,
            4.0 * kSearchBinHz));
    }

    CwBand band;
    band.config_ = config;
    band.config_.low_hz = low;
    band.config_.high_hz = high;

    ToneFrontEndConfig front;
    front.rate = config.rate;
    front.centre_hz = static_cast<Hertz>(std::lround(0.5 * (low + high)));
    front.passband_hz = 0.5 * (high - low) + kSearchMarginHz;
    front.minimum_output_rate =
        static_cast<SampleRate>(std::ceil(2.5 * front.passband_hz));
    auto made = ToneFrontEnd::create(front);
    if (!made) {
        return std::unexpected(with_context(made.error(), "creating the CW search's front end"));
    }
    band.front_ = std::move(*made);
    band.centre_hz_ = static_cast<double>(front.centre_hz);

    const double out_rate = static_cast<double>(band.front_.output_rate());
    band.fft_size_ = 16;
    while (out_rate / static_cast<double>(band.fft_size_) > kSearchBinHz) {
        band.fft_size_ *= 2;
    }
    band.hop_ = band.fft_size_ / kSearchHopDivisor;
    band.bin_hz_ = out_rate / static_cast<double>(band.fft_size_);

    band.window_.resize(band.fft_size_);
    double sum = 0.0;
    for (std::size_t i = 0; i < band.fft_size_; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                              static_cast<double>(band.fft_size_));
        band.window_[i] = static_cast<float>(w);
        sum += w;
    }
    // A tone of amplitude A reads A squared in its bin.
    for (float& w : band.window_) {
        w = static_cast<float>(static_cast<double>(w) / sum);
    }
    band.twiddles_.resize(band.fft_size_ / 2);
    for (std::size_t k = 0; k < band.twiddles_.size(); ++k) {
        const double angle = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(band.fft_size_);
        band.twiddles_[k] = Complex32{static_cast<float>(std::cos(angle)),
                                      static_cast<float>(std::sin(angle))};
    }

    // Search bins are numbered by frequency from the lowest in range, at
    // offsets from the front end's centre that are whole bins.
    const auto lowest = static_cast<std::ptrdiff_t>(std::ceil((low - band.centre_hz_) / band.bin_hz_));
    const auto highest =
        static_cast<std::ptrdiff_t>(std::floor((high - band.centre_hz_) / band.bin_hz_));
    const auto half = static_cast<std::ptrdiff_t>(band.fft_size_ / 2);
    band.first_bin_ = static_cast<std::size_t>(std::max(lowest, -half + 1) + half);
    band.last_bin_ = static_cast<std::size_t>(std::min(highest, half - 1) + half);

    const double frame_seconds = static_cast<double>(band.hop_) / out_rate;
    band.window_frames_ = static_cast<std::size_t>(std::lround(kSearchWindowSeconds / frame_seconds));
    band.evaluate_every_ =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(kSearchEvaluateSeconds / frame_seconds)));
    band.power_.assign((band.last_bin_ - band.first_bin_ + 1) * band.window_frames_, 0.0F);
    band.piece_ = std::max<std::size_t>(
        1, static_cast<std::size_t>(kPieceSeconds * static_cast<double>(config.rate)));
    return band;
}

void CwBand::reset() {
    front_.reset();
    pending_.clear();
    std::fill(power_.begin(), power_.end(), 0.0F);
    frames_ = 0;
    history_.clear();
    history_start_ = 0;
    audio_index_ = 0;
    streams_.clear();
    muted_.clear();
    next_id_ = 1;
    evaluation_due_ = false;
}

std::vector<CwBand::StreamInfo> CwBand::streams() const {
    std::vector<StreamInfo> out;
    out.reserve(streams_.size());
    for (const Stream& stream : streams_) {
        StreamInfo info;
        info.id = stream.id;
        info.pitch_hz = stream.decoder->pitch_hz();
        info.wpm = stream.decoder->wpm();
        info.overall_wpm = stream.decoder->overall_wpm();
        info.level_deviations = stream.decoder->level_deviations();
        out.push_back(info);
    }
    return out;
}

void CwBand::run_stream(Stream& stream, ConstRealSpan audio, std::vector<CwCharacter>& out) {
    std::vector<CwCharacter> fresh;
    // A Cw refuses nothing once it is built; its process() fails only in
    // acquisition, which a stream's Cw does not do.
    static_cast<void>(stream.decoder->process(audio, fresh));
    deliver(stream, fresh, out);
}

void CwBand::deliver(Stream& stream, std::vector<CwCharacter>& fresh,
                     std::vector<CwCharacter>& out) {
    for (CwCharacter& c : fresh) {
        c.first_sample += stream.origin;
        c.stream = stream.id;
        if (c.text != " ") {
            stream.recent.push_back(c.recognised);
            if (stream.recent.size() > kStreamJudgeCharacters) {
                stream.recent.pop_front();
            }
        }
        (stream.confirmed ? out : stream.waiting).push_back(std::move(c));
    }
    fresh.clear();
    const MorseTiming& timing = stream.decoder->timing();
    if (!stream.confirmed && timing.fitted_marks() >= kConfirmMarks &&
        timing.fit() >= kConfirmFit) {
        stream.confirmed = true;
        out.insert(out.end(), std::make_move_iterator(stream.waiting.begin()),
                   std::make_move_iterator(stream.waiting.end()));
        stream.waiting.clear();
    }
}

Status CwBand::start_stream(double pitch_hz, double level, std::vector<CwCharacter>& out) {
    CwConfig config;
    config.expected_level = kSeedLevelFraction * std::sqrt(level);
    config.rate = config_.rate;
    config.centre_hz = static_cast<Hertz>(std::lround(pitch_hz));
    config.capture_hz = kStreamCaptureHz;
    config.timing = config_.timing;
    config.acquire = false;
    config.track = true;
    auto made = Cw::create(config);
    if (!made) {
        return std::unexpected(with_context(made.error(), "starting a CW stream"));
    }
    Stream stream;
    stream.id = next_id_++;
    stream.decoder = std::make_unique<Cw>(std::move(*made));

    const auto replay = static_cast<SampleIndex>(kSearchReplaySeconds * static_cast<double>(config_.rate));
    const SampleIndex from = std::max(history_start_, audio_index_ > replay ? audio_index_ - replay : 0);
    stream.origin = from;
    const auto offset = static_cast<std::size_t>(from - history_start_);
    run_stream(stream, ConstRealSpan(history_.data() + offset, history_.size() - offset), out);
    streams_.push_back(std::move(stream));
    return {};
}

void CwBand::search(std::span<const Complex32> baseband) {
    pending_.insert(pending_.end(), baseband.begin(), baseband.end());
    const std::size_t bins = last_bin_ - first_bin_ + 1;
    std::vector<Complex32> frame(fft_size_);
    std::size_t used = 0;
    while (pending_.size() - used >= fft_size_) {
        for (std::size_t i = 0; i < fft_size_; ++i) {
            frame[i] = pending_[used + i] * window_[i];
        }
        transform(frame, twiddles_);
        const std::size_t slot = frames_ % window_frames_;
        for (std::size_t j = 0; j < bins; ++j) {
            // Search bin j is transform bin first_bin_ + j counted from -N/2.
            const std::size_t k = (first_bin_ + j + fft_size_ / 2) % fft_size_;
            power_[j * window_frames_ + slot] = std::norm(frame[k]);
        }
        ++frames_;
        used += hop_;
        if (frames_ >= window_frames_ && frames_ % evaluate_every_ == 0) {
            evaluation_due_ = true;
        }
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(used));
}

void CwBand::evaluate(std::vector<CwCharacter>& out) {
    const std::size_t bins = last_bin_ - first_bin_ + 1;
    const std::size_t w = window_frames_;
    const std::size_t oldest = frames_ % w;
    std::vector<double> level(bins, 0.0);
    std::vector<double> mean(bins, 0.0);
    std::vector<double> snr(bins, 0.0);
    std::vector<std::uint8_t> on(bins * w, 0);
    std::vector<float> sorted(w);
    const double silence = kDigitalSilence * kDigitalSilence;

    // Each bin's own noise, from its lower fifth.
    std::vector<double> own(bins, 0.0);
    for (std::size_t j = 0; j < bins; ++j) {
        const float* row = power_.data() + j * w;
        std::copy(row, row + w, sorted.begin());
        const auto at = sorted.begin() + static_cast<std::ptrdiff_t>(kSearchNoiseQuantile * static_cast<double>(w));
        std::nth_element(sorted.begin(), at, sorted.end());
        own[j] = static_cast<double>(*at) / -std::log(1.0 - kSearchNoiseQuantile);
    }

    for (std::size_t j = 0; j < bins; ++j) {
        const float* row = power_.data() + j * w;
        // The lower fifth of a few hundred frames that overlap is read to
        // about a fifth either way, which let a bin whose fifth came out low
        // pass the excess test on noise alone: measured on 2026-09-23, one
        // bench transmission in twenty at +3 dB grew a stream somewhere in
        // the band that printed a line of Es. So the noise is the larger of
        // the bin's own and the median of its neighbourhood's: the median is
        // read to a third as far, and the larger keeps a bin at the edge of a
        // receiver's filter, whose neighbours outside it are nearly silent,
        // on its own reading.
        std::vector<double> near;
        for (std::size_t k = j > kNoiseNeighbourBins ? j - kNoiseNeighbourBins : 0;
             k <= std::min(bins - 1, j + kNoiseNeighbourBins); ++k) {
            near.push_back(own[k]);
        }
        const double noise = std::max(own[j], median_of(std::move(near)));
        const double on_level = std::max(kKeyedOnFactor * noise, silence);
        const double up_level = std::max(kKeyedUpFactor * noise, silence);
        std::size_t runs = 0;
        std::size_t up_frames = 0;
        std::size_t run = 0;
        double up_sum = 0.0;
        double sum = 0.0;
        for (std::size_t i = 0; i < w; ++i) {
            const double p = row[(oldest + i) % w];
            sum += p;
            if (p > on_level) {
                on[j * w + i] = 1;
            }
            if (p > up_level) {
                ++run;
                ++up_frames;
                up_sum += p;
            } else {
                if (run >= kKeyedMinimumRunFrames) {
                    ++runs;
                }
                run = 0;
            }
        }
        if (run >= kKeyedMinimumRunFrames) {
            ++runs;
        }
        mean[j] = sum / static_cast<double>(w);
        const double duty = static_cast<double>(up_frames) / static_cast<double>(w);
        const double excess = noise > 0.0 ? mean[j] / noise : 0.0;
        if (excess >= kKeyedExcess && runs >= kKeyedMinimumRuns && duty <= kKeyedMaximumDuty) {
            level[j] = up_sum / static_cast<double>(up_frames);
            snr[j] = noise > 0.0 ? level[j] / noise : level[j] / silence;
        }
    }

    const double now = static_cast<double>(audio_index_);
    std::erase_if(muted_, [&](const Muted& m) { return static_cast<double>(m.until) <= now; });

    struct Candidate {
        double pitch = 0.0;
        double snr = 0.0;
        double level = 0.0;
    };
    std::vector<Candidate> candidates;
    for (std::size_t j = 0; j < bins; ++j) {
        if (level[j] <= 0.0) {
            continue;
        }
        const bool above_left = j == 0 || mean[j] >= mean[j - 1];
        const bool above_right = j + 1 == bins || mean[j] > mean[j + 1];
        if (!above_left || !above_right) {
            continue;
        }
        // Splatter from a stronger tone nearby: every time that tone keys,
        // the frames holding its edge spread it across the bins either side,
        // so a bin two or three away goes on and off with it and looks keyed.
        // It is only ever on while the stronger bin is.
        const auto reach = static_cast<std::size_t>(std::ceil(kSplatterReachHz / bin_hz_));
        bool splatter = false;
        for (std::size_t k = j > reach ? j - reach : 0; k <= std::min(bins - 1, j + reach) && !splatter;
             ++k) {
            if (k == j || mean[k] <= mean[j]) {
                continue;
            }
            std::size_t mine = 0;
            std::size_t shared = 0;
            for (std::size_t i = 0; i < w; ++i) {
                if (on[j * w + i] == 0) {
                    continue;
                }
                ++mine;
                const std::size_t lo = i > kSplatterSlackFrames ? i - kSplatterSlackFrames : 0;
                const std::size_t hi = std::min(w - 1, i + kSplatterSlackFrames);
                for (std::size_t m = lo; m <= hi; ++m) {
                    if (on[k * w + m] != 0) {
                        ++shared;
                        break;
                    }
                }
            }
            splatter = mine > 0 && static_cast<double>(shared) >=
                                       kSplatterShared * static_cast<double>(mine);
        }
        if (splatter) {
            continue;
        }
        // Speech: a voiced syllable is a row of harmonics a pitch apart
        // across the whole band, so while this bin is on, many other tones
        // well away from it are on too. A CW station is one tone.
        {
            const auto apart = static_cast<std::size_t>(std::ceil(kVoiceApartHz / bin_hz_));
            std::size_t frames_on = 0;
            std::size_t groups = 0;
            for (std::size_t i = 0; i < w; ++i) {
                if (on[j * w + i] == 0) {
                    continue;
                }
                ++frames_on;
                bool in_group = false;
                for (std::size_t k = 0; k < bins; ++k) {
                    const bool counted = (k + apart <= j || k >= j + apart) && on[k * w + i] != 0;
                    if (counted && !in_group) {
                        ++groups;
                    }
                    in_group = counted;
                }
            }
            if (frames_on > 0 &&
                static_cast<double>(groups) >= kVoiceGroups * static_cast<double>(frames_on)) {
                continue;
            }
        }
        // A parabola through the log of the three bins' mean power, which
        // for a Hann window is close to the tone's own shape.
        double delta = 0.0;
        if (j > 0 && j + 1 < bins && mean[j - 1] > 0.0 && mean[j + 1] > 0.0) {
            const double a = std::log(mean[j - 1]);
            const double b = std::log(mean[j]);
            const double c = std::log(mean[j + 1]);
            const double denominator = a - 2.0 * b + c;
            if (denominator < 0.0) {
                delta = std::clamp(0.5 * (a - c) / denominator, -0.5, 0.5);
            }
        }
        const double offset =
            (static_cast<double>(first_bin_ + j) - static_cast<double>(fft_size_ / 2) + delta) *
            bin_hz_;
        const double pitch = centre_hz_ + offset;
        if (pitch < config_.low_hz || pitch > config_.high_hz) {
            continue;
        }
        const auto near = [&](double other) { return std::abs(other - pitch) < kStreamSpacingHz; };
        if (std::ranges::any_of(streams_, [&](const Stream& s) { return near(s.decoder->pitch_hz()); }) ||
            std::ranges::any_of(muted_, [&](const Muted& m) { return near(m.pitch_hz); })) {
            continue;
        }
        candidates.push_back(Candidate{pitch, snr[j], level[j]});
    }
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) { return a.snr > b.snr; });
    for (const Candidate& candidate : candidates) {
        if (streams_.size() >= config_.max_streams) {
            break;
        }
        // A candidate this close to one started a moment ago in this same
        // pass is the same tone.
        if (std::ranges::any_of(streams_, [&](const Stream& s) {
                return std::abs(s.decoder->pitch_hz() - candidate.pitch) < kStreamSpacingHz;
            })) {
            continue;
        }
        // A pitch whose front end would not fit the audio band is skipped.
        static_cast<void>(start_stream(candidate.pitch, candidate.level, out));
    }
}

void CwBand::end_streams(std::vector<CwCharacter>& out) {
    const double rate = static_cast<double>(config_.rate);
    for (std::size_t i = 0; i < streams_.size();) {
        Stream& stream = streams_[i];
        std::size_t unrecognised = 0;
        for (const bool ok : stream.recent) {
            unrecognised += ok ? 0U : 1U;
        }
        const MorseTiming& timing = stream.decoder->timing();
        const bool judged = timing.fitted_marks() >= kFitJudgeMarks;
        const bool not_morse =
            judged && timing.fit() < (stream.confirmed ? kKeepFit : kConfirmFit);
        // A stream that has printed is not ended for a run of codes the
        // table does not hold: that is a fade or a burst of noise on a
        // station already shown to be Morse, and it comes back.
        const bool garbage =
            not_morse || (!stream.confirmed && stream.recent.size() >= kStreamJudgeCharacters &&
                          2 * unrecognised > stream.recent.size());
        const bool idle = stream.decoder->seconds_since_mark() > kStreamIdleSeconds;
        if (!garbage && !idle) {
            ++i;
            continue;
        }
        if (garbage) {
            muted_.push_back(Muted{stream.decoder->pitch_hz(),
                                   audio_index_ + static_cast<SampleIndex>(kStreamMuteSeconds * rate)});
        } else {
            finish(stream, out);
        }
        streams_.erase(streams_.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void CwBand::finish(Stream& stream, std::vector<CwCharacter>& out) {
    std::vector<CwCharacter> fresh;
    stream.decoder->flush(fresh);
    deliver(stream, fresh, out);
    // A short transmission that ended before it keyed kConfirmMarks marks:
    // printed if it keyed kShortMarks and its marks fit as a longer one's
    // must.
    const MorseTiming& timing = stream.decoder->timing();
    if (!stream.confirmed && timing.fitted_marks() >= kShortMarks &&
        timing.fit() >= kConfirmFit) {
        out.insert(out.end(), std::make_move_iterator(stream.waiting.begin()),
                   std::make_move_iterator(stream.waiting.end()));
    }
    stream.waiting.clear();
}

Status CwBand::process(ConstRealSpan audio, std::vector<CwCharacter>& out) {
    std::size_t done = 0;
    while (done < audio.size()) {
        const auto into = static_cast<std::size_t>(audio_index_ % piece_);
        const std::size_t take = std::min(piece_ - into, audio.size() - done);
        step(audio.subspan(done, take), out);
        done += take;
    }
    return {};
}

void CwBand::step(ConstRealSpan audio, std::vector<CwCharacter>& out) {
    history_.insert(history_.end(), audio.begin(), audio.end());
    audio_index_ += audio.size();

    for (Stream& stream : streams_) {
        run_stream(stream, audio, out);
    }

    baseband_.clear();
    front_.process(audio, baseband_);
    search(baseband_);

    if (audio_index_ % piece_ != 0) {
        return;
    }
    end_streams(out);
    if (evaluation_due_) {
        evaluation_due_ = false;
        evaluate(out);
    }
    const auto keep = static_cast<std::size_t>(kSearchReplaySeconds * static_cast<double>(config_.rate));
    if (history_.size() > keep) {
        const std::size_t drop = history_.size() - keep;
        history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(drop));
        history_start_ += drop;
    }
}

void CwBand::flush(std::vector<CwCharacter>& out) {
    for (Stream& stream : streams_) {
        finish(stream, out);
    }
}

}  // namespace revenant::decode
