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

// ---------------------------------------------------------------------------
// The audio decoder against the keyer
// ---------------------------------------------------------------------------

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
    CHECK(std::abs(got.wpm - 20.0) < 0.6);
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
    CHECK(std::abs(got.wpm - 18.0) < 0.6);
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

TEST_CASE("CW character error rate against noise, measured", "[decode][cw]") {
    struct Point {
        double wpm;
        double snr_db;
        double allowed_character_error_rate;
    };
    // Measured on 2026-09-22 over the 186 characters below, signal to noise
    // in 2500 Hz of audio, with this seed:
    //
    //   12 WPM: no errors to -6 dB; CER 0.016 at -8, 0.10 at -10, 0.45 at -12
    //   20 WPM: no errors to -6 dB; CER 0.011 at -8, 0.086 at -10, 0.43 at -12
    //   35 WPM: CER 0.005 at -4 dB, 0.022 at -6, 0.075 at -8, 0.34 at -10
    //
    // Speed reads slow in noise, 18.3 WPM for 20 at -10 dB, and 19.5 at 0 dB.
    // Why is not established; the clean round trips read within 2 per cent.
    const Point points[] = {
        {20.0, 10.0, 0.0},
        {20.0, -10.0, 0.20},
        {35.0, 10.0, 0.0},
        {35.0, -8.0, 0.20},
    };
    const std::string text =
        "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789 " + kSample +
        " NOW IS THE TIME FOR ALL GOOD MEN TO COME TO THE AID OF THE PARTY";
    for (const Point& point : points) {
        siggen::CwModConfig mod;
        mod.tone_hz = 720;
        mod.wpm = point.wpm;
        const std::vector<float> audio = render(mod, text, point.snr_db, true, 0x5A1ULL);
        decode::CwConfig config;
        const AudioResult got = decode_audio(config, audio, 8192);
        const double cer = static_cast<double>(edit_distance(text, got.text)) /
                           static_cast<double>(text.size());
        INFO(point.wpm << " WPM at " << point.snr_db << " dB in 2500 Hz: CER " << cer
                       << ", measured " << got.wpm << " WPM, decoded '" << got.text << "'");
        CHECK(cer <= point.allowed_character_error_rate);
        WARN("CW " << point.wpm << " WPM at " << point.snr_db << " dB in 2500 Hz: CER " << cer
                   << ", measured " << got.wpm << " WPM");
    }
}
