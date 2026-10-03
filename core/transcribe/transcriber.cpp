#include "core/transcribe/transcriber.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <new>
#include <string_view>
#include <system_error>
#include <utility>

#include "core/engine/load_clock.h"
#include "core/thread_role.h"

namespace revenant::transcribe {

namespace {

// ---------------------------------------------------------------------------
// LEVEL
//
// Analogue receivers hand over audio at whatever level the signal arrived:
// AM and SSB track the carrier, FM tracks the deviation the transmitter used,
// and the engine's AGC is tuned for a listener rather than a recogniser.
// Whisper is not level invariant. Its front end, log_mel_spectrogram in
// openai/whisper's whisper/audio.py and log_mel_spectrogram in whisper.cpp,
// takes log10 of the mel power, clamps to 8 decades below the window's
// maximum, and then applies a FIXED (x + 4) / 4. The clamp is relative and
// the offset is not, so the same words 30 dB quieter reach the encoder as a
// different input from the one it was trained on.
//
// So each utterance is brought to one RMS before it is recognised, limited by
// its peak and by a gain cap.

// -20 dBFS RMS. Ordinary speech levels in broadcast and podcast material sit
// a few dB either side of this (EBU R 128 targets -23 LUFS), and that is the
// material Whisper's training set is mostly made of.
constexpr float kTargetRms = 0.1F;

// -0.9 dBFS. A gain that would put any sample above this is reduced until it
// does not, so normalising never clips, which would add harmonics the
// recogniser hears as consonants.
constexpr float kPeakCeiling = 0.9F;

// +30 dB. An utterance is cut by a squelch or a level detector, so it should
// hold a signal; one 50 dB down is still brought to the target, and near
// silence is lifted by 30 dB and no further, so a squelch that opened on
// nothing hands Whisper quiet noise rather than noise at speech level, which
// is the input it is best known for hallucinating on.
constexpr float kMaxGain = 31.622776F;

// Removes the mean first. An envelope detector's output carries the carrier
// as DC, and DC counted into the RMS would set the gain from the carrier
// instead of the speech riding on it.
void normalise_level(std::vector<float>& pcm)
{
    if (pcm.empty()) {
        return;
    }
    double sum = 0.0;
    for (const float sample : pcm) {
        sum += sample;
    }
    const auto mean = static_cast<float>(sum / static_cast<double>(pcm.size()));

    double energy = 0.0;
    float peak = 0.0F;
    for (float& sample : pcm) {
        sample -= mean;
        energy += static_cast<double>(sample) * sample;
        peak = std::max(peak, std::abs(sample));
    }
    const auto rms = static_cast<float>(std::sqrt(energy / static_cast<double>(pcm.size())));

    float gain = rms > 0.0F ? kTargetRms / rms : kMaxGain;
    gain = std::min(gain, kMaxGain);
    if (peak * gain > kPeakCeiling) {
        gain = kPeakCeiling / peak;
    }
    for (float& sample : pcm) {
        sample *= gain;
    }
}

// ---------------------------------------------------------------------------
// TEXT HELPERS

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool is_ascii_alnum(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::vector<std::string_view> split_words(std::string_view text)
{
    std::vector<std::string_view> words;
    std::size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && is_space(text[at])) {
            ++at;
        }
        const std::size_t begin = at;
        while (at < text.size() && !is_space(text[at])) {
            ++at;
        }
        if (at > begin) {
            words.push_back(text.substr(begin, at - begin));
        }
    }
    return words;
}

// Lower case, every ASCII character that is not a letter or a digit turned to
// a space, runs of spaces collapsed. The same normalisation Baranski et al.
// list their hallucinations in ("i m sorry", "subtitles by the amara org
// community"), so the list below can be copied from their table as printed.
// Bytes above 0x7F, which is every non-ASCII letter in UTF-8, pass unchanged.
std::string match_key(std::string_view text)
{
    std::string key;
    key.reserve(text.size());
    bool pending_space = false;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (is_ascii_alnum(c) || byte >= 0x80U) {
            if (pending_space && !key.empty()) {
                key.push_back(' ');
            }
            pending_space = false;
            key.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
        } else {
            pending_space = true;
        }
    }
    return key;
}

// The text with what Whisper writes for sounds rather than words taken out:
// spans in square brackets ("[BLANK_AUDIO]", "[MUSIC]"), in parentheses
// ("(music)", "(laughs)"), between asterisks ("*sigh*"), and the music notes
// U+266A and U+266B, which Whisper writes in pairs over a song. Unclosed
// spans run to the end, which is how a truncated "[BLANK_AUD" reads.
std::string strip_sound_marks(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    char closer = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (closer != 0) {
            if (c == closer) {
                closer = 0;
            }
            continue;
        }
        if (c == '[') {
            closer = ']';
            continue;
        }
        if (c == '(') {
            closer = ')';
            continue;
        }
        if (c == '*') {
            closer = '*';
            continue;
        }
        // U+266A is E2 99 AA in UTF-8 and U+266B is E2 99 AB.
        if (static_cast<unsigned char>(c) == 0xE2U && i + 2 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) == 0x99U &&
            (static_cast<unsigned char>(text[i + 2]) == 0xAAU ||
             static_cast<unsigned char>(text[i + 2]) == 0xABU)) {
            i += 2;
            continue;
        }
        out.push_back(c);
    }
    return out;
}

bool has_words(std::string_view text)
{
    return !match_key(text).empty();
}

// ---------------------------------------------------------------------------
// THE RULES. Every rule that decides whether text is speech is in this block,
// and judge() below applies them in the order they are listed.
//
// RULE 1, WHISPER'S OWN. A segment is skipped when no_speech_prob is above
// TranscriberOptions::reject_no_speech AND avg_logprob is below
// reject_logprob. This is transcribe.py in openai/whisper: should_skip is set
// by no_speech_prob > no_speech_threshold and cleared again when avg_logprob >
// logprob_threshold, so both have to agree before a window is dropped. The
// defaults, 0.6 and -1.0, are its defaults.
//
// RULE 2, LOOPS ARE CUT TO ONE. Four or more consecutive copies of the same
// run of one to eight words become a single copy. A decoder that has started
// repeating itself conditions each token on the repetition before it and
// rarely stops on its own; Baranski et al. (below) deloop before anything
// else for that reason, and whisper's own transcribe.py treats a decode whose
// gzip compression ratio exceeds 2.4 as failed for the same symptom. Four
// rather than two or three, because people on a radio do say "copy copy" and
// "mayday mayday mayday"; a phrase said four times running is far more often
// the decoder than the speaker. Applied to each segment and again to the
// joined text, because a loop Whisper emits as one segment per copy is only
// visible once the segments are put back together. A loop cut to one copy
// then meets rules 3 and 4 like any other text, which is what rejects "Thank
// you." said forty times.
constexpr std::size_t kLoopMaxWords = 8;
constexpr std::size_t kLoopMinCopies = 4;
//
// RULE 3, NO WORDS. A segment that is empty, whitespace, punctuation, or only
// sound marks (strip_sound_marks above) carries nothing to read and is
// dropped. Whisper writes "[BLANK_AUDIO]" and "(music)" exactly when it has
// decided there was no speech, so these are its own verdict in text form.
// Sound marks inside text that has words are left alone: "[inaudible]" in
// the middle of a sentence is information.
//
// RULE 4, KNOWN HALLUCINATIONS, WHEN NOT CONFIDENT. Whisper turns non-speech
// audio into a small set of stock phrases, learned from the subtitle files of
// online video it was trained on. The list is the head of Table III of
// Baranski, Jasinski, Bartolewska, Kacprzak, Witkowski and Kowalczyk,
// "Investigation of Whisper ASR Hallucinations Induced by Non-Speech Audio",
// ICASSP 2025, arXiv:2501.11378, measured on whisper large-v3, the model
// whose encoder kDefaultModel carries. By their count the top ten are over
// half of every hallucination they recorded; "thank you" alone is 24.76% and
// "thanks for watching" 10.32%. The two entries printed after the table's
// ellipsis, a subtitle credit and a channel greeting, are included because
// they cannot be radio traffic.
//
// Matched against the WHOLE text of a segment, or of the utterance, after
// match_key(): a listed phrase inside a longer sentence is speech. Some of
// these are words people say on the air ("okay", "thank you", "bye"), so a
// match is rejected only when the recogniser was not confident in it, and
// confident is a stricter bar than rule 1's because a stock phrase is the
// one output for which a merely middling score is suspicious:
// no_speech_prob at most kListedKeepNoSpeech AND avg_logprob at least
// kListedKeepLogprob. Unlike rule 1 either signal alone is enough to reject.
// These two numbers are a judgement, not a measurement from the paper, which
// removed its list unconditionally; tune them against recordings.
constexpr float kListedKeepNoSpeech = 0.2F;
constexpr float kListedKeepLogprob = -0.5F;

struct ListedPhrase {
    std::string_view key;
    // A prefix match, for the credit lines that run on with a name.
    bool prefix = false;
};

constexpr ListedPhrase kListedPhrases[] = {
    // Baranski et al., Table III, in its order, as printed.
    {"thank you"},
    {"thanks for watching"},
    {"so"},
    {"thank you for watching"},
    {"the"},
    {"you"},
    {"oh"},
    {"okay"},
    {"i m sorry"},
    {"oh my god"},
    {"bye"},
    {"i m not sure what i m doing here"},
    {"uh"},
    {"meow"},
    {"subtitles by the amara org community"},
    {"subtitles by steamteamextra"},
    {"hello everyone welcome to my channel"},
    // The table carries two subtitle credits with different names, so the
    // form is the hallucination and the name is incidental. A prefix covers
    // the names it did not happen to record.
    {"subtitles by", true},
};

bool is_listed_phrase(std::string_view text)
{
    const std::string key = match_key(text);
    if (key.empty()) {
        return false;
    }
    for (const ListedPhrase& phrase : kListedPhrases) {
        if (phrase.prefix ? key.starts_with(phrase.key) : key == phrase.key) {
            return true;
        }
    }
    return false;
}

bool confident_enough_for_a_listed_phrase(float no_speech_prob, float avg_logprob)
{
    return no_speech_prob <= kListedKeepNoSpeech && avg_logprob >= kListedKeepLogprob;
}

// Rule 2. Words are compared by match_key, so "Thank you." and "thank you,"
// are the same word pair; the first copy is kept as it was written.
std::string collapse_loops(std::string_view text)
{
    const std::vector<std::string_view> words = split_words(text);
    std::vector<std::string> keys;
    keys.reserve(words.size());
    for (const std::string_view word : words) {
        std::string key = match_key(word);
        // A token with no letters, a lone "-", compares as itself.
        keys.push_back(key.empty() ? std::string(word) : std::move(key));
    }

    std::string out;
    out.reserve(text.size());
    const auto append = [&out](std::string_view word) {
        if (!out.empty()) {
            out.push_back(' ');
        }
        out.append(word);
    };

    std::size_t at = 0;
    while (at < words.size()) {
        std::size_t best_period = 0;
        std::size_t best_span = 0;
        for (std::size_t period = 1; period <= kLoopMaxWords && at + period <= words.size(); ++period) {
            std::size_t copies = 1;
            while (at + (copies + 1) * period <= words.size()) {
                bool same = true;
                for (std::size_t k = 0; k < period; ++k) {
                    if (keys[at + k] != keys[at + copies * period + k]) {
                        same = false;
                        break;
                    }
                }
                if (!same) {
                    break;
                }
                ++copies;
            }
            if (copies >= kLoopMinCopies && copies * period > best_span) {
                best_period = period;
                best_span = copies * period;
            }
        }
        if (best_period != 0) {
            for (std::size_t k = 0; k < best_period; ++k) {
                append(words[at + k]);
            }
            at += best_span;
        } else {
            append(words[at]);
            ++at;
        }
    }
    return out;
}

struct Verdict {
    bool speech = false;
    std::string text;
    float confidence = 0.0F;
    float no_speech_prob = 0.0F;
};

Verdict judge(const std::vector<RecognisedSegment>& segments, const TranscriberOptions& options)
{
    std::string joined;
    // Weighted by the length of each kept segment's text, a stand-in for its
    // token count, which RecognisedSegment does not carry.
    double logprob_weighted = 0.0;
    double weight = 0.0;
    float no_speech = 0.0F;

    for (const RecognisedSegment& segment : segments) {
        // Rule 1.
        if (segment.no_speech_prob > options.reject_no_speech &&
            segment.avg_logprob < options.reject_logprob) {
            continue;
        }
        // Rule 2.
        const std::string text = collapse_loops(trim(segment.text));
        // Rule 3.
        if (!has_words(strip_sound_marks(text))) {
            continue;
        }
        // Rule 4, on the segment by itself.
        if (is_listed_phrase(text) &&
            !confident_enough_for_a_listed_phrase(segment.no_speech_prob, segment.avg_logprob)) {
            continue;
        }
        if (!joined.empty()) {
            joined.push_back(' ');
        }
        joined.append(text);
        const auto length = static_cast<double>(text.size());
        logprob_weighted += length * segment.avg_logprob;
        weight += length;
        // The most doubtful of the kept segments. Whisper estimates it once
        // per 30 s window, so every segment of one utterance normally carries
        // the same value anyway.
        no_speech = std::max(no_speech, segment.no_speech_prob);
    }

    Verdict verdict;
    if (joined.empty()) {
        return verdict;
    }
    const auto avg_logprob = static_cast<float>(logprob_weighted / weight);

    // Rule 2 again across segment boundaries, then rule 4 on the whole.
    std::string text = collapse_loops(joined);
    if (is_listed_phrase(text) && !confident_enough_for_a_listed_phrase(no_speech, avg_logprob)) {
        return verdict;
    }

    verdict.speech = true;
    verdict.text = std::move(text);
    // exp of the mean log-probability: the GEOMETRIC mean of the chosen
    // tokens' probabilities, which is all avg_logprob can give back. It sits at
    // or below the arithmetic mean Transcript::confidence describes, and it is
    // the figure whisper's own threshold is written in.
    verdict.confidence = std::clamp(std::exp(avg_logprob), 0.0F, 1.0F);
    verdict.no_speech_prob = no_speech;
    return verdict;
}

// An exception escaping the worker would be std::terminate, which takes the
// radio down with the least important thread in it. Prepare and recognise are
// someone else's code over ggml and a GPU driver, so the boundary is here.
template <class Call>
auto guarded(Call&& call, std::string_view what) -> decltype(call())
{
    try {
        return call();
    } catch (const std::exception& error) {
        return fail(std::format("{} threw: {}", what, error.what()));
    } catch (...) {
        return fail(std::format("{} threw something that is not a std::exception", what));
    }
}

}  // namespace

Expected<std::unique_ptr<Transcriber>> Transcriber::create(TranscriberOptions options, Prepare prepare,
                                                           Sink sink)
{
    if (options.max_queued == 0) {
        return fail("a transcriber needs room for at least one utterance");
    }
    if (!prepare) {
        return fail("a transcriber needs a way to prepare its recogniser");
    }
    if (!sink) {
        return fail("a transcriber needs somewhere to send its transcripts");
    }
    if (!std::isfinite(options.reject_no_speech) || !std::isfinite(options.reject_logprob)) {
        return fail("a transcriber's rejection thresholds must be finite numbers");
    }

    std::unique_ptr<Transcriber> transcriber(
        new (std::nothrow) Transcriber(std::move(options), std::move(prepare), std::move(sink)));
    if (transcriber == nullptr) {
        return fail("could not allocate a transcriber");
    }
    try {
        transcriber->worker_ = std::thread([raw = transcriber.get()] { raw->run(); });
    } catch (const std::system_error& error) {
        return fail(std::format("could not start the transcriber's thread: {}", error.what()),
                    error.code().value());
    }
    return transcriber;
}

Transcriber::Transcriber(TranscriberOptions options, Prepare prepare, Sink sink)
    : options_(std::move(options)), prepare_(std::move(prepare)), sink_(std::move(sink))
{
    counters_.model_name = options_.model_name;
}

Transcriber::~Transcriber()
{
    {
        const std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cancel_.store(true, std::memory_order_relaxed);
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Transcriber::set_enabled(bool on)
{
    std::deque<Utterance> emptied;
    {
        const std::lock_guard lock(mutex_);
        if (counters_.enabled == on) {
            return;
        }
        counters_.enabled = on;
        if (!on) {
            counters_.dropped += queue_.size();
            emptied.swap(queue_);
        } else if (!preparing_ && !prepare_pending_ &&
                   (counters_.state == ModelState::Absent || counters_.state == ModelState::Failed)) {
            prepare_pending_ = true;
        }
    }
    wake_.notify_all();
    // `emptied` frees its audio here, outside the lock.
}

bool Transcriber::ready() const
{
    const std::lock_guard lock(mutex_);
    return counters_.enabled && counters_.state == ModelState::Ready;
}

void Transcriber::submit(Utterance utterance)
{
    {
        const std::lock_guard lock(mutex_);
        if (!counters_.enabled || counters_.state != ModelState::Ready) {
            ++counters_.dropped;
            return;
        }
        if (queue_.size() >= options_.max_queued) {
            queue_.pop_front();
            ++counters_.dropped;
        }
        queue_.push_back(std::move(utterance));
    }
    wake_.notify_one();
}

TranscriberStatus Transcriber::status() const
{
    const std::lock_guard lock(mutex_);
    TranscriberStatus snapshot = counters_;
    snapshot.queued = static_cast<std::uint32_t>(std::min<std::size_t>(queue_.size(), UINT32_MAX));
    return snapshot;
}

void Transcriber::run()
{
    // SigId, below normal, and not lower. Transcription is the least urgent
    // work in the process, but core/thread_role.h has no class under SigId,
    // and THREAD_PRIORITY_LOWEST set here privately would be a fourth level
    // that file does not know about. Below normal already yields to every
    // decode lane and to the display; sharing a level with identification
    // means the two take turns, and both shed work of their own when behind.
    describe_this_thread(L"revenant transcriber", ThreadClass::SigId);

    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] {
            return stop_ || prepare_pending_ ||
                   (counters_.enabled && counters_.state == ModelState::Ready && !queue_.empty());
        });
        if (stop_) {
            return;
        }
        if (prepare_pending_) {
            run_prepare(lock);
            continue;
        }
        Utterance utterance = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        process(std::move(utterance));
        lock.lock();
    }
}

void Transcriber::run_prepare(std::unique_lock<std::mutex>& lock)
{
    prepare_pending_ = false;
    preparing_ = true;
    // The first thing any prepare does is find out what is on disk.
    counters_.state = ModelState::Verifying;
    counters_.detail.clear();
    counters_.backend.clear();
    counters_.bytes_done = 0;
    counters_.bytes_total = 0;
    lock.unlock();

    const PrepareReport report = [this](ModelState state, std::uint64_t done, std::uint64_t total) {
        if (state != ModelState::Downloading && state != ModelState::Verifying &&
            state != ModelState::Loading) {
            return;
        }
        const std::lock_guard report_lock(mutex_);
        counters_.state = state;
        counters_.bytes_done = done;
        counters_.bytes_total = total;
    };
    Expected<std::unique_ptr<Recogniser>> prepared =
        guarded([&] { return prepare_(report, cancel_); }, "preparing the recogniser");

    std::string backend;
    if (prepared && *prepared != nullptr) {
        backend = (*prepared)->backend();
    }

    lock.lock();
    preparing_ = false;
    if (stop_) {
        return;
    }
    if (!prepared) {
        counters_.state = ModelState::Failed;
        counters_.detail = prepared.error().message;
        return;
    }
    if (*prepared == nullptr) {
        counters_.state = ModelState::Failed;
        counters_.detail = "preparing the recogniser returned none";
        return;
    }
    recogniser_ = std::move(*prepared);
    failures_in_a_row_ = 0;
    counters_.backend = std::move(backend);
    counters_.state = ModelState::Ready;
}

void Transcriber::process(Utterance utterance)
{
    std::vector<float> pcm = std::move(utterance.pcm);
    utterance.pcm.clear();
    normalise_level(pcm);

    Expected<std::vector<RecognisedSegment>> segments =
        pcm.empty() ? Expected<std::vector<RecognisedSegment>>{}
                    : guarded([&] { return recogniser_->recognise(pcm); }, "recognising");
    pcm = {};

    if (!segments) {
        std::unique_ptr<Recogniser> broken;
        std::deque<Utterance> abandoned;
        {
            const std::lock_guard lock(mutex_);
            ++counters_.errors;
            ++failures_in_a_row_;
            // Quiet on purpose: one bad utterance is not news. The message
            // waits in detail for whoever asks.
            counters_.detail = segments.error().message;
            if (failures_in_a_row_ >= kFailuresInARow) {
                counters_.state = ModelState::Failed;
                counters_.detail = std::format("the recogniser failed {} utterances in a row, the last with: {}",
                                               failures_in_a_row_, segments.error().message);
                counters_.dropped += queue_.size();
                abandoned.swap(queue_);
                // A recogniser that fails every time is most likely holding a
                // lost device, and only a fresh prepare gets a new one, so it
                // is released here and the next switch-on reloads.
                broken = std::move(recogniser_);
            }
        }
        return;
    }

    const Verdict verdict = judge(*segments, options_);
    const std::uint64_t now = engine::load_clock_ns();
    const double latency_ms = utterance.closed_ns != 0 && now >= utterance.closed_ns
                                  ? static_cast<double>(now - utterance.closed_ns) / 1.0e6
                                  : 0.0;

    Transcript transcript;
    {
        const std::lock_guard lock(mutex_);
        failures_in_a_row_ = 0;
        if (counters_.state == ModelState::Ready) {
            counters_.detail.clear();
        }
        // Finishes nothing more once destruction has begun.
        if (stop_) {
            return;
        }
        if (!verdict.speech) {
            ++counters_.rejected;
            return;
        }
        // The switch went off while this was being recognised; off means no
        // more text, the same as the utterances it emptied out of the queue.
        if (!counters_.enabled) {
            ++counters_.dropped;
            return;
        }
        ++counters_.transcribed;
        counters_.last_latency_ms = latency_ms;
        transcript.sequence = sequence_++;
    }
    transcript.heard = std::move(utterance);
    transcript.text = verdict.text;
    transcript.confidence = verdict.confidence;
    transcript.no_speech_prob = verdict.no_speech_prob;
    transcript.latency_ms = latency_ms;

    // The sink is the server's, and the same reasoning as guarded() applies.
    try {
        sink_(std::move(transcript));
    } catch (...) {
        const std::lock_guard lock(mutex_);
        counters_.detail = "the transcript sink threw";
    }
}

}  // namespace revenant::transcribe
