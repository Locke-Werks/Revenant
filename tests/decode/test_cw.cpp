// CW: the Morse table and timing against ITU-R M.1677-1 and the ARRL timing
// standard, the timing decoder against exact key runs, and the audio decoder
// against the keyer in core/dsp/synth/cw_mod.h.
//
// Error rates are measured and printed and the assertions around them are
// loose, as everywhere in this directory.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/decode/cw.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/cw_mod.h"
#include "core/dsp/synth/psk31_mod.h"

using namespace revenant;

namespace {

std::string join(const std::vector<decode::CwCharacter>& characters) {
    std::string text;
    for (const auto& character : characters) {
        text += character.recognised ? character.text : std::string("#");
    }
    return text;
}

// Feeds exact key runs to the timing decoder, as if from a perfect envelope
// detector. Sample indices count milliseconds.
std::string decode_runs(const std::vector<siggen::CwKeyRun>& runs, decode::MorseTiming& timing) {
    std::vector<decode::CwCharacter> out;
    double t = 0.0;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        const auto& run = runs[i];
        const auto start = static_cast<dsp::SampleIndex>(std::llround(t * 1000.0));
        if (run.key_down) {
            timing.mark(run.seconds, start, out);
        } else if (i + 1 == runs.size()) {
            // The tail is a silence nothing ends, as it is on air.
            timing.idle(run.seconds, out);
        } else {
            timing.space(run.seconds, start, out);
        }
        t += run.seconds;
    }
    timing.flush(out);
    return join(out);
}

std::size_t edit_distance(const std::string& a, const std::string& b) {
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) {
        row[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1,
                               diagonal + (a[i - 1] == b[j - 1] ? 0U : 1U)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

struct AudioResult {
    std::string text;

    // Every character with the dots and dashes it was read from, for the
    // failure message.
    std::string coded;
    double wpm = 0.0;
    double overall_wpm = 0.0;
    double offset_hz = 0.0;
    bool acquired = false;
};

AudioResult decode_audio(const decode::CwConfig& config, std::span<const float> audio,
                         std::size_t block) {
    auto decoder = decode::Cw::create(config);
    INFO((decoder.has_value() ? std::string{} : decoder.error().message));
    REQUIRE(decoder.has_value());
    std::vector<decode::CwCharacter> out;
    for (std::size_t start = 0; start < audio.size(); start += block) {
        const std::size_t length = std::min(block, audio.size() - start);
        REQUIRE(decoder->process(audio.subspan(start, length), out).has_value());
    }
    decoder->flush(out);
    AudioResult result;
    result.text = join(out);
    for (const auto& character : out) {
        result.coded += character.text + "[" + character.code + "]";
    }
    result.wpm = decoder->wpm();
    result.overall_wpm = decoder->overall_wpm();
    result.offset_hz = decoder->frequency_offset_hz();
    result.acquired = decoder->acquired();
    return result;
}

std::vector<float> render(const siggen::CwModConfig& mod, const std::string& text,
                          double snr_in_2500_hz_db, bool noisy, std::uint64_t seed) {
    auto analytic = siggen::cw_render_analytic(mod, text);
    INFO((analytic.has_value() ? std::string{} : analytic.error().message));
    REQUIRE(analytic.has_value());
    if (!noisy) {
        return siggen::analytic_to_audio(*analytic);
    }
    auto audio = siggen::analytic_to_noisy_audio(*analytic, snr_in_2500_hz_db,
                                                 siggen::kWsjtxReferenceBandwidthHz, mod.rate,
                                                 seed);
    REQUIRE(audio.has_value());
    return *audio;
}

const std::string kSample = "CQ CQ DE G3PLX = RST 599 5NN QTH KENDAL, NAME PETER? <END OF WORK>";

}  // namespace

// ---------------------------------------------------------------------------
// The table and the timing, against the documents
// ---------------------------------------------------------------------------

TEST_CASE("the Morse table is ITU-R M.1677-1 clause 1.1", "[decode][cw]") {
    std::map<std::string_view, std::vector<std::string_view>> by_code;
    for (const auto& signal : decode::kMorseTable) {
        INFO("row " << signal.text);
        REQUIRE(!signal.code.empty());
        CHECK(signal.code.find_first_not_of(".-") == std::string_view::npos);
        by_code[signal.code].push_back(signal.text);
    }
    // Exactly the two sharings the Recommendation makes: the invitation to
    // transmit is K and the multiplication sign is X (clause 3.2.1).
    std::size_t shared = 0;
    for (const auto& [code, texts] : by_code) {
        if (texts.size() > 1) {
            ++shared;
            INFO("code " << code);
            CHECK((texts.front() == "K" || texts.front() == "X"));
        }
    }
    CHECK(shared == 2);
    CHECK(decode::morse_text_for("-.-") == "K");
    CHECK(decode::morse_text_for("-..-") == "X");

    // Spot checks against the printed table.
    CHECK(decode::morse_code_for("a").value() == ".-");
    CHECK(decode::morse_code_for("Q").value() == "--.-");
    CHECK(decode::morse_code_for("\xC3\xA9").value() == "..-..");
    CHECK(decode::morse_code_for("0").value() == "-----");
    CHECK(decode::morse_code_for("@").value() == ".--.-.");
    CHECK(decode::morse_code_for("<ERROR>").value() == "........");
    CHECK(decode::morse_code_for("<END OF WORK>").value() == "...-.-");
    CHECK(!decode::morse_code_for("%").has_value());
}

TEST_CASE("PARIS is fifty units under clause 2", "[decode][cw]") {
    // The ARRL standard's speed definition rests on the word PARIS being 50
    // units, and that number is not in either document as such: it follows
    // from M.1677-1 clause 2 applied to the clause 1.1 codes. Checked by
    // keying the word and adding up the runs.
    siggen::CwModConfig mod;
    mod.wpm = 20.0;
    mod.lead_in_seconds = 0.0;
    mod.tail_seconds = 0.0;
    auto runs = siggen::cw_key_runs(mod, "PARIS");
    REQUIRE(runs.has_value());
    double total = 0.0;
    for (const auto& run : *runs) {
        total += run.seconds;
    }
    const double unit = decode::kParisUnitSecondsTimesWpm / mod.wpm;
    CHECK(std::abs(total / unit - 43.0) < 1e-9);
    CHECK(43.0 + decode::kMorseWordSpaceDots == decode::kParisWordUnits);
    CHECK(decode::kParisCharacterUnits + decode::kParisSpacingUnits == decode::kParisWordUnits);
}

TEST_CASE("Farnsworth spacing keeps the ARRL standard's word time and 3/7 ratio",
          "[decode][cw]") {
    // ARRL standard clause 2.2's own example: 5 WPM overall with 18 WPM
    // characters. A 50-unit word takes 50 * 1.2 / 5 = 12 s at the overall
    // speed (Appendix A.2, t_50).
    auto spacing = decode::morse_spacing(18.0, 5.0);
    REQUIRE(spacing.has_value());
    const double word = 31.0 * spacing->unit_s + 4.0 * spacing->letter_space_s +
                        spacing->word_space_s;
    CHECK(std::abs(word - 12.0) < 1e-9);
    CHECK(std::abs(spacing->word_space_s / spacing->letter_space_s - 7.0 / 3.0) < 1e-12);
    CHECK(std::abs(decode::morse_overall_wpm(18.0, spacing->letter_space_s) - 5.0) < 1e-9);

    // At or above the character speed it is clause 2's standard spacing.
    auto standard = decode::morse_spacing(20.0, 20.0);
    REQUIRE(standard.has_value());
    CHECK(std::abs(standard->letter_space_s - 3.0 * 0.06) < 1e-12);
    CHECK(std::abs(standard->word_space_s - 7.0 * 0.06) < 1e-12);
    CHECK(std::abs(decode::morse_overall_wpm(20.0, standard->letter_space_s) - 20.0) < 1e-9);
}

// ---------------------------------------------------------------------------
// The timing decoder against exact key runs
// ---------------------------------------------------------------------------

TEST_CASE("the timing decoder reads exact key runs across the speed range",
          "[decode][cw]") {
    for (const double wpm : {5.0, 12.0, 20.0, 35.0, 50.0}) {
        siggen::CwModConfig mod;
        mod.wpm = wpm;
        auto runs = siggen::cw_key_runs(mod, kSample);
        REQUIRE(runs.has_value());
        decode::MorseTiming timing;
        const std::string got = decode_runs(*runs, timing);
        INFO(wpm << " WPM decoded '" << got << "', measured " << timing.wpm());
        CHECK(got == kSample);
        CHECK(std::abs(timing.wpm() - wpm) < 0.01 * wpm);
    }
}

TEST_CASE("the timing decoder reads Farnsworth spacing as letters and words",
          "[decode][cw]") {
    for (const double overall : {5.0, 10.0, 15.0}) {
        siggen::CwModConfig mod;
        mod.wpm = 18.0;
        mod.overall_wpm = overall;
        auto runs = siggen::cw_key_runs(mod, kSample);
        REQUIRE(runs.has_value());
        decode::MorseTiming timing;
        const std::string got = decode_runs(*runs, timing);
        INFO("18/" << overall << " decoded '" << got << "', measured " << timing.wpm() << " / "
                   << timing.overall_wpm());
        CHECK(got == kSample);
        CHECK(std::abs(timing.wpm() - 18.0) < 0.2);
        CHECK(std::abs(timing.overall_wpm() - overall) < 0.05 * overall);
    }
}

TEST_CASE("the timing decoder follows a hand sender and a change of speed",
          "[decode][cw]") {
    // Ten per cent jitter on every element and space.
    siggen::CwModConfig hand;
    hand.wpm = 18.0;
    hand.jitter = 0.10;
    hand.seed = 0x4A11D;
    auto runs = siggen::cw_key_runs(hand, kSample);
    REQUIRE(runs.has_value());
    decode::MorseTiming timing;
    const std::string got = decode_runs(*runs, timing);
    INFO("hand sent: '" << got << "'");
    CHECK(got == kSample);

    // Fifteen then thirty five words per minute, in one stream.
    siggen::CwModConfig slow;
    slow.wpm = 15.0;
    slow.tail_seconds = 7.0 * decode::kParisUnitSecondsTimesWpm / 15.0;
    siggen::CwModConfig fast;
    fast.wpm = 35.0;
    fast.lead_in_seconds = 0.0;
    auto first = siggen::cw_key_runs(slow, "SLOW START HERE");
    auto second = siggen::cw_key_runs(fast, "THEN MUCH FASTER TEXT FOLLOWS");
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    std::vector<siggen::CwKeyRun> both = *first;
    both.back().seconds += second->front().seconds;
    both.insert(both.end(), second->begin() + 1, second->end());
    decode::MorseTiming changing;
    const std::string mixed = decode_runs(both, changing);
    const std::string expected = "SLOW START HERE THEN MUCH FASTER TEXT FOLLOWS";
    const std::size_t errors = edit_distance(mixed, expected);
    INFO("speed change decoded '" << mixed << "', " << errors << " edits, ending at "
                                  << changing.wpm() << " WPM");
    // The unit follows within a few characters of the change; those few are
    // the price and are counted, not hidden.
    CHECK(errors <= 4);
    CHECK(std::abs(changing.wpm() - 35.0) < 1.0);
}

TEST_CASE("the timing decoder keeps the space after a one-character first word",
          "[decode][cw]") {
    // The first long space a transmission sends has nothing to be compared
    // with. When the first word is one character that space is a word space,
    // clause 2.4's seven units, and the first version took the lone sample for
    // the letter-space cluster, set the word cut at five thirds of it, and
    // printed the first two words as one: "K PARIS" as "KPARIS". The sweep
    // saw it at +10 dB, where the noise has no part in it: of 200
    // transmissions of text like the sweep's, 27 lost that space and nothing
    // else.
    for (const std::string text : {"K PARIS", "A CQ CQ DE G3PLX", "F Q M PARIS DE G3PLX",
                                   "0 7W0 L72"}) {
        for (const double wpm : {12.0, 20.0, 35.0}) {
            siggen::CwModConfig mod;
            mod.wpm = wpm;
            auto runs = siggen::cw_key_runs(mod, text);
            REQUIRE(runs.has_value());
            decode::MorseTiming timing;
            const std::string got = decode_runs(*runs, timing);
            INFO(wpm << " WPM sent '" << text << "' decoded '" << got << "'");
            CHECK(got == text);
        }
    }
}

TEST_CASE("the timing decoder reads a run of dashes as dashes", "[decode][cw]") {
    // T, M and O carry no one-unit run between them but the element spaces
    // inside M and O, so a few of them together leave the recent runs almost
    // all three units long: dashes and letter spaces. The unit estimate took
    // its one-unit cluster as everything within twice the twentieth
    // percentile, which in that window is a dash, so the cluster held dashes
    // and the unit came out too long; dashes after it were shorter than two
    // of those and printed as dots, "PARIS TTTTTTTTTT PARIS" as
    // "PARIS TTTTTTH HARIS". Clause 2.1 fixes a dash at three dots whatever
    // the window holds.
    for (const std::string text : {"PARIS TTTTTTTTTT PARIS", "PARIS TMT OT MOTTO TOM PARIS",
                                   "CQ 0 00 000 TOM 0000 DE G3PLX"}) {
        for (const double wpm : {12.0, 20.0, 35.0}) {
            siggen::CwModConfig mod;
            mod.wpm = wpm;
            auto runs = siggen::cw_key_runs(mod, text);
            REQUIRE(runs.has_value());
            decode::MorseTiming timing;
            const std::string got = decode_runs(*runs, timing);
            INFO(wpm << " WPM sent '" << text << "' decoded '" << got << "', measured "
                     << timing.wpm());
            CHECK(got == text);
            CHECK(std::abs(timing.wpm() - wpm) < 0.01 * wpm);
        }
    }
}

TEST_CASE("the timing decoder keeps word spaces across the pauses between overs",
          "[decode][cw]") {
    // A contest station calls, pauses for an answer, and calls again. The
    // pauses joined the letter and word clusters, the cut between them landed
    // between the word spaces and the pauses, and every word space read as a
    // letter space: on the KF4FIC 20 m 1603 UT recording, 129 "TEST"s from
    // one station and none of them a word. The pauses here are that
    // station's, 3.5 and 4.5 s, at its 20 WPM.
    const std::string call = "CQ TEST DE W7E W7E TEST";
    siggen::CwModConfig mod;
    mod.wpm = 20.0;
    auto one = siggen::cw_key_runs(mod, call);
    REQUIRE(one.has_value());
    // The call's own key runs, without the keyer's lead-in and tail.
    std::vector<siggen::CwKeyRun> body = *one;
    while (!body.empty() && !body.front().key_down) {
        body.erase(body.begin());
    }
    while (!body.empty() && !body.back().key_down) {
        body.pop_back();
    }
    std::vector<siggen::CwKeyRun> runs = {{false, 0.5}};
    for (const double pause : {3.5, 4.5, 3.5}) {
        runs.insert(runs.end(), body.begin(), body.end());
        runs.push_back(siggen::CwKeyRun{false, pause});
    }
    runs.insert(runs.end(), body.begin(), body.end());
    runs.push_back(siggen::CwKeyRun{false, 1.0});
    decode::MorseTiming timing;
    const std::string got = decode_runs(runs, timing);
    const std::string want = call + " " + call + " " + call + " " + call;
    INFO("decoded '" << got << "'");
    CHECK(got == want);
}

TEST_CASE("the timing decoder forgets clicks that never made a unit", "[decode][cw]") {
    // Three marks of 16 to 20 ms, a station 170 Hz away keying into this
    // stream through its edges, then two seconds before the stream's own
    // station starts at 25 WPM. The unit used to lock on the clicks, at the
    // 50 WPM floor, and the station's first dots read as dashes: "K9BGL" as
    // "EE E O T# B G L".
    const std::string text = "K9BGL DE W7E R TU 5NN CN88";
    std::vector<siggen::CwKeyRun> runs = {
        {false, 0.45}, {true, 0.016}, {false, 0.078}, {true, 0.016},
        {false, 0.126}, {true, 0.020}, {false, 1.98}};
    siggen::CwModConfig mod;
    mod.wpm = 25.0;
    auto station = siggen::cw_key_runs(mod, text);
    REQUIRE(station.has_value());
    auto first_mark = std::ranges::find_if(*station, [](const auto& run) { return run.key_down; });
    runs.insert(runs.end(), first_mark, station->end());
    decode::MorseTiming timing;
    const std::string got = decode_runs(runs, timing);
    INFO("decoded '" << got << "'");
    CHECK(got == text);
}

// ---------------------------------------------------------------------------
// The audio decoder against the keyer
// ---------------------------------------------------------------------------

TEST_CASE("CW keys nothing from the noise before a transmission", "[decode][cw]") {
    // Half a second of noise, the keyer's own lead-in, before the first
    // element, at +10 dB in 2500 Hz where nothing else goes wrong. The squelch
    // opens 5.5 noise deviations above the noise's mean, so the lead-in
    // should key nothing. It keyed characters in 19 of 200 transmissions of
    // text like the sweep's at +10 dB, and in 4 of these 32, because the
    // noise statistics started from a quarter of a second that included the
    // front end's own start-up.
    siggen::CwModConfig mod;
    mod.tone_hz = 720;
    mod.wpm = 20.0;
    const std::string text = "CQ DE G3PLX";
    std::size_t wrong = 0;
    std::string first_wrong;
    for (std::uint64_t seed = 1; seed <= 32; ++seed) {
        const std::vector<float> audio = render(mod, text, 10.0, true, 0xC0DE'0000ULL + seed);
        const AudioResult got = decode_audio(decode::CwConfig{}, audio, 4800);
        if (got.text != text) {
            ++wrong;
            if (first_wrong.empty()) {
                first_wrong = "seed " + std::to_string(seed) + " decoded '" + got.text +
                              "' as " + got.coded;
            }
        }
    }
    INFO(wrong << " of 32 wrong; first " << first_wrong);
    CHECK(wrong == 0);
}

TEST_CASE("CW round trips at 48 kHz with the tone off centre", "[decode][cw]") {
    siggen::CwModConfig mod;
    mod.rate = 48000;
    mod.tone_hz = 750;
    mod.wpm = 20.0;
    const std::vector<float> audio = render(mod, kSample, 0.0, false, 0);

    decode::CwConfig config;
    config.rate = 48000;
    config.centre_hz = 700;
    const AudioResult got = decode_audio(config, audio, 4800);
    INFO("decoded '" << got.text << "' at " << got.wpm << " WPM, offset " << got.offset_hz);
    INFO("as " << got.coded);
    CHECK(got.text == kSample);
    CHECK(std::abs(got.wpm - 20.0) < 0.05);
    CHECK(std::abs(got.offset_hz - 50.0) < 0.5);
}

TEST_CASE("CW round trips at 11025 Hz with Farnsworth spacing", "[decode][cw]") {
    siggen::CwModConfig mod;
    mod.rate = 11025;
    mod.tone_hz = 590;
    mod.wpm = 18.0;
    mod.overall_wpm = 8.0;
    const std::vector<float> audio = render(mod, kSample, 0.0, false, 0);

    decode::CwConfig config;
    config.rate = 11025;
    config.centre_hz = 620;
    const AudioResult got = decode_audio(config, audio, 1000);
    INFO("decoded '" << got.text << "' at " << got.wpm << " / " << got.overall_wpm
                     << " WPM, offset " << got.offset_hz);
    CHECK(got.text == kSample);
    CHECK(std::abs(got.wpm - 18.0) < 0.05);
    CHECK(std::abs(got.overall_wpm - 8.0) < 0.6);
}

TEST_CASE("the CW decoder does not depend on how its input is blocked", "[decode][cw]") {
    siggen::CwModConfig mod;
    mod.tone_hz = 700;
    mod.wpm = 25.0;
    const std::vector<float> audio = render(mod, kSample, 5.0, true, 0xB10CULL);
    decode::CwConfig config;
    const AudioResult whole = decode_audio(config, audio, audio.size());
    const AudioResult small = decode_audio(config, audio, 777);
    CHECK(whole.text == small.text);
    CHECK(whole.wpm == small.wpm);
}

TEST_CASE("the CW decoder reports nothing from noise alone", "[decode][cw]") {
    constexpr dsp::SampleRate kRate = 48000;
    std::vector<siggen::Complex32> noise(static_cast<std::size_t>(30 * kRate));
    REQUIRE(siggen::add_awgn_at_power(noise, 1.0, 0xC0FF'0015ULL).has_value());
    const std::vector<float> audio = siggen::analytic_to_audio(noise);
    decode::CwConfig config;
    config.rate = kRate;
    const AudioResult got = decode_audio(config, audio, 9600);
    INFO("acquired " << got.acquired << ", text '" << got.text << "'");
    CHECK(got.text.empty());
}

TEST_CASE("the CW decoder falls silent in the noise after a transmission", "[decode][cw]") {
    // The harder case: the decoder has acquired, the squelch has been held
    // open by a signal, and then there is a minute of nothing but noise. The
    // hold bar in core/decode/cw.cpp keys on noise when nothing renews it
    // properly, which is how this case was found.
    siggen::CwModConfig mod;
    mod.tone_hz = 720;
    mod.wpm = 20.0;
    mod.tail_seconds = 60.0;
    auto analytic = siggen::cw_render_analytic(mod, kSample);
    REQUIRE(analytic.has_value());
    std::vector<siggen::Complex32> noisy = *analytic;
    REQUIRE(siggen::add_awgn_at_power(noisy, 0.05, 0x77ULL).has_value());
    const std::vector<float> audio = siggen::analytic_to_audio(noisy);
    const AudioResult got = decode_audio(decode::CwConfig{}, audio, 4800);
    INFO("decoded '" << got.text << "'");
    CHECK(got.text == kSample);
}

namespace {

const std::string kNoiseText =
    "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789 " + kSample +
    " NOW IS THE TIME FOR ALL GOOD MEN TO COME TO THE AID OF THE PARTY";

}  // namespace

TEST_CASE("CW character error rate against noise, measured", "[decode][cw]") {
    struct Point {
        double wpm;
        double snr_db;
        double allowed_character_error_rate;
        double allowed_wpm_error;
    };
    // Measured on 2026-09-22 over the 186 characters below, signal to noise
    // in 2500 Hz of audio, with this seed:
    //
    //   12 WPM: no errors to -6 dB; CER 0.016 at -8, 0.10 at -10, 0.45 at -12
    //   20 WPM: no errors to -6 dB; CER 0.011 at -8, 0.086 at -10, 0.43 at -12
    //   35 WPM: CER 0.005 at -4 dB, 0.022 at -6, 0.075 at -8, 0.34 at -10
    //
    // On 2026-09-23 this case printed CER 0.10 for 20 WPM at -10 dB and
    // 0.054 for 35 at -8 dB, and it prints the same with the speed read as
    // the next case describes, because the runs are still read against the
    // pooled unit. The speeds it printed moved: 18.25 to 18.69 WPM for 20 at
    // -10 dB and 33.3 to 34.62 for 35 at -8 dB, with 19.80 and 34.82 at
    // +10 dB. One transmission's reading wanders, by 0.82 WPM at 20 and
    // -10 dB and by 2.0 at 35 and -8 dB as a standard deviation over 40, so
    // the speed bounds here are loose where the noise is and the next case
    // holds the average. The pooled reading fails the two noisy bounds.
    //
    // Since "Decode every keyed tone in a receiver's audio, wherever it is"
    // put a low-pass around the tone ahead of the boxcar and tracks the
    // tone's frequency from the boxcar's own sum, it prints CER 0.043 for
    // 20 WPM at -10 dB, read at 18.63 WPM, and 0.022 for 35 at -8 dB, read at
    // 35.57.
    const Point points[] = {
        {20.0, 10.0, 0.0, 0.3},
        {20.0, -10.0, 0.20, 1.5},
        {35.0, 10.0, 0.0, 0.5},
        {35.0, -8.0, 0.20, 1.0},
    };
    for (const Point& point : points) {
        siggen::CwModConfig mod;
        mod.tone_hz = 720;
        mod.wpm = point.wpm;
        const std::vector<float> audio = render(mod, kNoiseText, point.snr_db, true, 0x5A1ULL);
        decode::CwConfig config;
        const AudioResult got = decode_audio(config, audio, 8192);
        const double cer = static_cast<double>(edit_distance(kNoiseText, got.text)) /
                           static_cast<double>(kNoiseText.size());
        INFO(point.wpm << " WPM at " << point.snr_db << " dB in 2500 Hz: CER " << cer
                       << ", measured " << got.wpm << " WPM, decoded '" << got.text << "'");
        CHECK(cer <= point.allowed_character_error_rate);
        CHECK(std::abs(got.wpm - point.wpm) < point.allowed_wpm_error);
        WARN("CW " << point.wpm << " WPM at " << point.snr_db << " dB in 2500 Hz: CER " << cer
                   << ", measured " << got.wpm << " WPM");
    }
}

TEST_CASE("CW reads its speed in noise without leaning slow", "[decode][cw]") {
    // In noise the mark threshold sits above the middle of the carrier's
    // edges, so dots come out short and element spaces long by about the
    // same amount: measured on 2026-09-23 over 40 transmissions of 60
    // characters at 20 WPM, 60 ms a unit, 50.5 and 69.1 ms at -8 dB, 55.6
    // and 64.4 at 0 dB, 58.6 and 61.4 at +10 dB. The one-unit cluster pooled
    // the two, random text keys more element spaces than dots, and the
    // pooled unit read long: 61.5, 60.8 and 60.3 ms. The speed now weights
    // the dots' mean and the spaces' equally, as clause 2 makes both one
    // unit; core/decode/cw.cpp's estimate_unit() has why decoding does not.
    //
    // Over 40 transmissions of the text below at -8 dB, the pooled speed
    // averaged 11.66 WPM for 12 and 19.36 for 20, 2.9 and 3.2 per cent slow,
    // and the equal weighting 12.02 and 20.13. These eight printed 12.06 and
    // 20.13 on 2026-09-23. The bound is 1.5 per cent, which the pooled
    // reading fails by about twice.
    constexpr int kTransmissions = 8;
    for (const double wpm : {12.0, 20.0}) {
        double sum = 0.0;
        for (int i = 0; i < kTransmissions; ++i) {
            siggen::CwModConfig mod;
            mod.tone_hz = 720;
            mod.wpm = wpm;
            const std::vector<float> audio =
                render(mod, kNoiseText, -8.0, true, 0x9000ULL + static_cast<std::uint64_t>(i));
            sum += decode_audio(decode::CwConfig{}, audio, 8192).wpm;
        }
        const double mean = sum / kTransmissions;
        INFO(wpm << " WPM at -8 dB read " << mean << " on average over " << kTransmissions);
        CHECK(std::abs(mean - wpm) < 0.015 * wpm);
        WARN("CW speed " << wpm << " WPM at -8 dB: mean " << mean);
    }
}

