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

// Letter-or-word spaces kept for the two clusters.
constexpr std::size_t kLongSpaces = 12;

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
    recent_.clear();
    held_.clear();
    long_spaces_.clear();
    code_.clear();
    character_start_ = 0;
    character_ended_ = true;
    word_ended_ = true;
    any_text_ = false;
}

double MorseTiming::wpm() const {
    return (locked_ && unit_ > 0.0) ? kParisUnitSecondsTimesWpm / unit_ : 0.0;
}

double MorseTiming::overall_wpm() const {
    return morse_overall_wpm(wpm(), letter_space_estimate());
}

void MorseTiming::estimate_unit() {
    if (recent_.size() < kRunsToLock) {
        return;
    }
    std::vector<double> sorted(recent_.begin(), recent_.end());
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
    const double shortest = unit_for_wpm(config_.max_wpm);
    const double longest = unit_for_wpm(config_.min_wpm);
    unit_ = std::clamp(mean, shortest, longest);
    locked_ = true;
}

double MorseTiming::letter_space_estimate() const {
    if (long_spaces_.empty()) {
        return kMorseLetterSpaceDots * unit_;
    }
    double low = 0.0;
    double high = 0.0;
    std::vector<double> values(long_spaces_.begin(), long_spaces_.end());
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
    std::vector<double> values(long_spaces_.begin(), long_spaces_.end());
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

void MorseTiming::apply_space(double seconds, SampleIndex start,
                              std::vector<CwCharacter>& out) {
    if (seconds < kDotDashSplitUnits * unit_) {
        return;
    }
    long_spaces_.push_back(seconds);
    if (long_spaces_.size() > kLongSpaces) {
        long_spaces_.pop_front();
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

void MorseTiming::mark(double seconds, SampleIndex start, std::vector<CwCharacter>& out) {
    recent_.push_back(seconds);
    if (recent_.size() > kRecentRuns) {
        recent_.pop_front();
    }
    estimate_unit();
    if (!locked_) {
        held_.push_back(Run{true, seconds, start});
        return;
    }
    if (!held_.empty()) {
        // The unit has just become known: decode what was waiting for it.
        const std::vector<Run> waiting = std::move(held_);
        held_.clear();
        for (const Run& run : waiting) {
            if (run.key_down) {
                apply_mark(run.seconds, run.start, out);
            } else {
                apply_space(run.seconds, run.start, out);
            }
        }
    }
    apply_mark(seconds, start, out);
}

void MorseTiming::space(double seconds, SampleIndex start, std::vector<CwCharacter>& out) {
    if (recent_.empty() && held_.empty()) {
        // Silence before the first mark is not a space between anything.
        return;
    }
    recent_.push_back(seconds);
    if (recent_.size() > kRecentRuns) {
        recent_.pop_front();
    }
    if (!locked_) {
        held_.push_back(Run{false, seconds, start});
        return;
    }
    apply_space(seconds, start, out);
}

void MorseTiming::idle(double seconds, std::vector<CwCharacter>& out) {
    if (!locked_ || !held_.empty()) {
        return;
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
    return decoder;
}

void Cw::reset() {
    front_.reset();
    timing_.reset();
    acquisition_.clear();
    acquired_ = false;
    offset_hz_ = 0.0;
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
    const double centred = decimated_index - 0.5 * static_cast<double>(boxcar_ - 1);
    const double input = centred * static_cast<double>(front_.decimation()) -
                         static_cast<double>(front_.group_delay());
    return input > 0.0 ? static_cast<SampleIndex>(std::llround(input)) : SampleIndex{0};
}

Status Cw::process(ConstRealSpan audio, std::vector<CwCharacter>& out) {
    decimated_.clear();
    front_.process(audio, decimated_);
    if (acquired_) {
        run(decimated_, out);
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
        key_since_ = index_;
        raw_since_ = index_;
    }
    if (acquired_) {
        std::vector<Complex32> held;
        held.swap(acquisition_);
        run(held, out);
    }
    return {};
}

void Cw::run(std::span<const Complex32> baseband, std::vector<CwCharacter>& out) {
    const double rate = static_cast<double>(front_.output_rate());
    const double hold = 1.0 / (kLevelHoldSeconds * rate);
    const double noise = 1.0 / (kNoiseSeconds * rate);

    for (const Complex32 sample : baseband) {
        const double cycles = std::fmod(offset_hz_ * static_cast<double>(index_) / rate, 1.0);
        const std::complex<double> value =
            std::complex<double>{sample.real(), sample.imag()} * std::polar(1.0, -2.0 * kPi * cycles);
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
            const auto warm_samples = static_cast<std::size_t>(kWarmSeconds * rate);
            if (warm_.size() < warm_samples) {
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
        }
        if (!key_) {
            timing_.idle(static_cast<double>(index_ + 1 - key_since_) / rate, out);
        }
        ++index_;
    }
}

}  // namespace revenant::decode
