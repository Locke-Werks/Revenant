// RTTY: the alphabet against ITU-T S.1, and the decoder against the
// transmitter.
//
// The first group checks the ITA2 table in core/decode/rtty.cpp against
// Table 1/S.1 as transcribed here a second time, column by column. That is
// the only check in this file that can see a misreading of the table, because
// the transmitter and the decoder share the table and a round trip through
// both is blind to anything they share.
//
// The second group is the round trip, at two sample rates and three sets of
// practice parameters, plus the two framing properties ITU-T S.3 states: a
// 1.5-unit stop element sent, and a stop element as short as 1.0 unit
// accepted. Error rates are measured and reported in INFO lines, with loose
// assertions around them, for the reason tests/decode/CMakeLists.txt gives.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/decode/rtty.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/fsk_mod.h"
#include "core/dsp/synth/rds_mod.h"

using namespace revenant;

namespace {

// Table 1/S.1, read down its columns in combination-number order. Units are
// the coding column, code element 1 first.
constexpr const char* kTable1Units[32] = {
    "ZZAAA", "ZAAZZ", "AZZZA", "ZAAZA", "ZAAAA", "ZAZZA", "AZAZZ", "AAZAZ",
    "AZZAA", "ZZAZA", "ZZZZA", "AZAAZ", "AAZZZ", "AAZZA", "AAAZZ", "AZZAZ",
    "ZZZAZ", "AZAZA", "ZAZAA", "AAAAZ", "ZZZAA", "AZZZZ", "ZZAAZ", "ZAZZZ",
    "ZAZAZ", "ZAAAZ", "AAAZA", "AZAAA", "ZZZZZ", "ZZAZZ", "AAZAA", "AAAAA",
};
constexpr char32_t kTable1Letters[] = U"ABCDEFGHIJKLMNOPQRSTUVWXYZ";
// The figure column for combinations 1 to 26, with the four the decoder
// substitutes marked by a zero and checked separately.
constexpr char32_t kTable1Figures[] = U"-?:\0" U"3\0\0\0" U"8\0()" U".,90" U"14'5" U"7=2/" U"6+";

std::uint8_t combination_of(const char* units) {
    std::uint8_t value = 0;
    for (int i = 0; i < 5; ++i) {
        if (units[i] == 'Z') {
            value = static_cast<std::uint8_t>(value | (1U << i));
        }
    }
    return value;
}

std::string utf8(std::u32string_view text) {
    std::string out;
    for (const char32_t c : text) {
        out.push_back(static_cast<char>(c));
    }
    return out;
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
                               diagonal + ((a[i - 1] == b[j - 1]) ? 0U : 1U)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

double mean_power(const std::vector<float>& x) {
    double sum = 0.0;
    for (const float v : x) {
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    return x.empty() ? 0.0 : sum / static_cast<double>(x.size());
}

// Printable ITA2 text of a given length, letters and figures mixed so the
// shifts are exercised, from a stated seed.
std::u32string random_text(std::size_t length, std::uint64_t seed) {
    constexpr char32_t kPool[] = U"ABCDEFGHIJKLMNOPQRSTUVWXYZ    0123456789-?:().,'=/+";
    std::mt19937_64 engine(seed);
    std::uniform_int_distribution<std::size_t> pick(0, std::size(kPool) - 2);
    std::u32string text;
    for (std::size_t i = 0; i < length; ++i) {
        text.push_back(kPool[pick(engine)]);
    }
    return text;
}

struct Decoded {
    std::vector<decode::RttyCharacter> characters;
    std::uint64_t framing_errors = 0;
};

Decoded decode_all(const decode::RttyConfig& config, const std::vector<float>& audio,
                   std::size_t block = 0) {
    auto decoder = decode::RttyDecoder::create(config);
    REQUIRE(decoder.has_value());
    Decoded d;
    if (block == 0) {
        decoder->process(audio, d.characters);
    } else {
        for (std::size_t i = 0; i < audio.size(); i += block) {
            const std::size_t n = std::min(block, audio.size() - i);
            decoder->process(std::span<const float>(audio.data() + i, n), d.characters);
        }
    }
    d.framing_errors = decoder->framing_errors();
    return d;
}

const std::u32string kPangram =
    U"RYRYRY THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789 -?:().,'=/+\r\n";

}  // namespace

TEST_CASE("the ITA2 table is Table 1/S.1", "[decode][rtty]") {
    std::set<std::uint8_t> seen;
    for (int number = 1; number <= 32; ++number) {
        const std::uint8_t combination = combination_of(kTable1Units[number - 1]);
        INFO("combination No. " << number << ", units " << kTable1Units[number - 1]);
        CHECK(decode::ita2_combination_number(combination) == number);
        seen.insert(combination);
        if (number <= 26) {
            CHECK(decode::ita2_letter(combination) == kTable1Letters[number - 1]);
            if (kTable1Figures[number - 1] != 0) {
                CHECK(decode::ita2_figure(combination) == kTable1Figures[number - 1]);
            }
        }
    }
    // Clause 3.1: 32 combinations, so every one of the 32 five-unit patterns
    // is used exactly once.
    CHECK(seen.size() == 32);

    // The four figure-case positions S.1 does not give a printing character:
    // clause 4.1, clause 4.2 three times, clause 4.3.
    CHECK(decode::ita2_figure(combination_of("ZAAZA")) == U'\x05');
    CHECK(decode::ita2_figure(combination_of("ZAZZA")) == char32_t{0x25A1});
    CHECK(decode::ita2_figure(combination_of("AZAZZ")) == char32_t{0x25A1});
    CHECK(decode::ita2_figure(combination_of("AAZAZ")) == char32_t{0x25A1});
    CHECK(decode::ita2_figure(combination_of("ZZAZA")) == U'\x07');

    CHECK(combination_of("ZZZZZ") == decode::kIta2LetterShift);
    CHECK(combination_of("ZZAZZ") == decode::kIta2FigureShift);
    CHECK(combination_of("AAZAA") == decode::kIta2Space);
}

TEST_CASE("every ITA2 character encodes and decodes to itself", "[decode][rtty]") {
    for (const char32_t c : kPangram) {
        const auto code = decode::ita2_encode(c);
        REQUIRE(code.has_value());
        using Case = decode::Ita2Code::Case;
        const char32_t back = (code->needs == Case::Figures) ? decode::ita2_figure(code->combination)
                                                             : decode::ita2_letter(code->combination);
        CHECK(back == c);
    }
    CHECK_FALSE(decode::ita2_encode(U'@').has_value());
    CHECK(decode::ita2_encode(U'q')->combination == decode::ita2_encode(U'Q')->combination);
}

TEST_CASE("RTTY round trips clean at two rates and three parameter sets", "[decode][rtty]") {
    struct Case {
        dsp::SampleRate rate;
        double baud;
        dsp::Hertz mark;
        dsp::Hertz shift;
        bool space_above;
    };
    // 45.45/170 is amateur practice, 50/450 and 75/850 are the other shifts
    // in use at the S.3 rates, and the last case is the reversed sideband.
    const Case cases[] = {
        {48'000, 45.45, 2125, 170, true},
        {8'000, 45.45, 2125, 170, true},
        {48'000, 50.0, 1275, 450, true},
        {11'025, 75.0, 2975, 850, false},
    };
    for (const Case& c : cases) {
        INFO("rate " << c.rate << ", " << c.baud << " baud, mark " << c.mark << " Hz, shift "
                     << c.shift << " Hz, space " << (c.space_above ? "above" : "below"));
        siggen::RttyModConfig mod;
        mod.rate = c.rate;
        mod.baud = c.baud;
        mod.mark_hz = c.mark;
        mod.shift_hz = c.shift;
        mod.space_above_mark = c.space_above;
        auto codes = siggen::ita2_encode_text(kPangram);
        REQUIRE(codes.has_value());
        auto audio = siggen::rtty_render(mod, *codes);
        REQUIRE(audio.has_value());

        decode::RttyConfig config;
        config.rate = c.rate;
        config.baud = c.baud;
        config.mark_hz = c.mark;
        config.shift_hz = c.shift;
        config.space_above_mark = c.space_above;
        const Decoded d = decode_all(config, *audio);
        CHECK(decode::rtty_text(d.characters) == utf8(kPangram));
        CHECK(d.characters.size() == codes->size());
        CHECK(d.framing_errors == 0);

        // The first start element's leading edge is after lead_units of idle
        // mark. Its reported position should land there to within a few
        // percent of a unit, which is what the edge interpolation buys.
        REQUIRE_FALSE(d.characters.empty());
        const double expected = mod.lead_units * static_cast<double>(c.rate) / c.baud;
        const double error = std::abs(static_cast<double>(d.characters.front().position) - expected);
        INFO("first start edge reported at " << d.characters.front().position << ", sent at "
                                             << expected);
        CHECK(error < 0.05 * static_cast<double>(c.rate) / c.baud);
    }
}

TEST_CASE("RTTY output does not depend on how the audio is blocked", "[decode][rtty]") {
    siggen::RttyModConfig mod;
    auto codes = siggen::ita2_encode_text(kPangram);
    REQUIRE(codes.has_value());
    auto audio = siggen::rtty_render(mod, *codes);
    REQUIRE(audio.has_value());

    decode::RttyConfig config;
    const Decoded whole = decode_all(config, *audio);
    for (const std::size_t block : {std::size_t{1}, std::size_t{97}, std::size_t{4096}}) {
        INFO("block of " << block << " samples");
        const Decoded split = decode_all(config, *audio, block);
        REQUIRE(split.characters.size() == whole.characters.size());
        for (std::size_t i = 0; i < whole.characters.size(); ++i) {
            CHECK(split.characters[i].position == whole.characters[i].position);
            CHECK(split.characters[i].combination == whole.characters[i].combination);
        }
    }
}

TEST_CASE("a 1.0 unit stop element is accepted, as S.3 clause 1.4 requires", "[decode][rtty]") {
    siggen::RttyModConfig mod;
    mod.stop_units = decode::kRttyReceiverMinimumStopUnits;
    auto codes = siggen::ita2_encode_text(kPangram);
    REQUIRE(codes.has_value());
    auto audio = siggen::rtty_render(mod, *codes);
    REQUIRE(audio.has_value());
    const Decoded d = decode_all(decode::RttyConfig{}, *audio);
    CHECK(decode::rtty_text(d.characters) == utf8(kPangram));
}

TEST_CASE("unshift on space returns to letters only when asked", "[decode][rtty]") {
    // FIGS 1 SPACE then a letter combination with no shift in between. With
    // the option on it prints the letter; off, the figure.
    const std::vector<std::uint8_t> codes = {
        decode::kIta2LetterShift, decode::kIta2FigureShift, combination_of("ZZZAZ"),
        decode::kIta2Space,       combination_of("ZZAAA"),
    };
    siggen::RttyModConfig mod;
    auto audio = siggen::rtty_render(mod, codes);
    REQUIRE(audio.has_value());

    decode::RttyConfig config;
    CHECK(decode::rtty_text(decode_all(config, *audio).characters) == "1 -");
    config.unshift_on_space = true;
    CHECK(decode::rtty_text(decode_all(config, *audio).characters) == "1 A");
}

TEST_CASE("RTTY recovers framing after a burst of noise", "[decode][rtty]") {
    // Half a second of loud noise laid over the middle of a message. The
    // framer must lose what the burst covers and nothing after it: the text
    // following the burst is compared exactly.
    const std::u32string text = random_text(120, 0xA11CE);
    siggen::RttyModConfig mod;
    auto codes = siggen::ita2_encode_text(text);
    REQUIRE(codes.has_value());
    auto audio = siggen::rtty_render(mod, *codes);
    REQUIRE(audio.has_value());

    const std::size_t burst_start = audio->size() / 2;
    const std::size_t burst_length = 24'000;
    std::mt19937_64 engine(0xB0257ULL);
    std::normal_distribution<float> noise(0.0F, 2.0F);
    for (std::size_t i = burst_start; i < burst_start + burst_length; ++i) {
        (*audio)[i] = noise(engine);
    }

    const Decoded d = decode_all(decode::RttyConfig{}, *audio);
    // Characters whose start edge falls at least three characters after the
    // burst ends, which is how long a start-stop receiver may take to fall
    // back into step on continuous text.
    const double samples_per_char = (7.5) * 48'000.0 / 45.45;
    const auto resume = static_cast<std::uint64_t>(
        static_cast<double>(burst_start + burst_length) + 3.0 * samples_per_char);
    std::string after;
    for (const auto& c : d.characters) {
        if (c.position >= resume && c.glyph != 0) {
            after.push_back(static_cast<char>(c.glyph));
        }
    }
    const std::string sent = utf8(text);
    INFO("decoded after the burst: \"" << after << "\"; framing errors " << d.framing_errors);
    REQUIRE(after.size() >= 20);
    // The tail of what was sent, exactly.
    CHECK(sent.substr(sent.size() - after.size()) == after);
}

TEST_CASE("RTTY character error rate against noise, measured", "[decode][rtty]") {
    // A random text through the transmitter, add_real_awgn and the decoder,
    // scored by edit distance so a dropped or invented character counts once
    // rather than shifting every comparison after it. Stated in 2500 Hz, per
    // docs/snr-convention.md, and as Eb/N0 at 45.45 bit/s for comparison
    // with the non-coherent FSK curve, which at 9.4 dB is a bit error rate
    // of 0.0065.
    const std::u32string text = random_text(300, 0x5EED0045ULL);
    auto codes = siggen::ita2_encode_text(text);
    REQUIRE(codes.has_value());

    struct Point {
        double snr_2500_db;
        double tone_offset_hz;
        double allowed_character_error_rate;
    };
    // Measured 2026-09-22, seed 0xC0FFEE, 300 characters at 48 kHz:
    //
    //   SNR/2500 Hz   Eb/N0     unit error rate   character error rate
    //   +10 dB        27.4 dB   0                 0
    //    -5 dB        12.4 dB   0                 0.0033
    //    -8 dB         9.4 dB   0.0069            0.14
    //   +10 dB, 10 Hz mistuned  0                 0
    //
    // The unit error rate at 9.4 dB sits on the non-coherent FSK curve,
    // 0.0064, so the discriminator is doing its job. The character error
    // rate is about twenty times it because a character is seven readings,
    // any one of which loses it, and a lost shift garbles every character
    // until the next one. The allowances sit above the measurements.
    const Point points[] = {{10.0, 0.0, 0.001}, {-5.0, 0.0, 0.03}, {-8.0, 0.0, 0.3}, {10.0, 10.0, 0.01}};
    for (const Point& p : points) {
        siggen::RttyModConfig mod;
        mod.tone_offset_hz = p.tone_offset_hz;
        auto audio = siggen::rtty_render(mod, *codes);
        REQUIRE(audio.has_value());
        const double power = mean_power(*audio);
        const auto level = siggen::NoiseLevel::snr_in_2500_hz_db(p.snr_2500_db);
        auto report = siggen::add_real_awgn(*audio, power, level, 48'000, 0xC0FFEEULL);
        REQUIRE(report.has_value());
        auto eb_n0 = siggen::reference_bandwidth_to_eb_over_n0_db(p.snr_2500_db, 45.45);
        REQUIRE(eb_n0.has_value());

        const Decoded d = decode_all(decode::RttyConfig{}, *audio);
        const std::string got = decode::rtty_text(d.characters);
        const std::string sent = utf8(text);
        const double cer = static_cast<double>(edit_distance(sent, got)) /
                           static_cast<double>(sent.size());

        // The unit error rate of the discriminator alone, read at the
        // instants the transmitter put the unit centres, so the framer's
        // timing is not in it. The gap between this and the character error
        // rate is what framing and lost shifts cost.
        decode::ToneDiscriminatorConfig tones;
        tones.mark_hz = 2125;
        tones.space_hz = 2295;
        tones.symbol_rate = 45.45;
        auto discriminator = decode::ToneDiscriminator::create(tones);
        REQUIRE(discriminator.has_value());
        std::vector<float> soft;
        discriminator->process(*audio, soft);
        const double spu = 48'000.0 / 45.45;
        std::size_t unit_errors = 0;
        std::size_t units = 0;
        for (std::size_t k = 0; k < codes->size(); ++k) {
            const double edge = mod.lead_units * spu + static_cast<double>(k) * 7.5 * spu +
                                static_cast<double>(discriminator->group_delay());
            for (std::size_t u = 0; u < 5; ++u) {
                const auto at = static_cast<std::size_t>(
                    std::llround(edge + (static_cast<double>(u) + 1.5) * spu));
                const bool mark = soft[at] >= 0.0F;
                const bool sent_mark = (((*codes)[k] >> u) & 1U) != 0U;
                unit_errors += (mark != sent_mark) ? 1U : 0U;
                ++units;
            }
        }
        const double unit_error_rate = static_cast<double>(unit_errors) / static_cast<double>(units);

        INFO("SNR " << p.snr_2500_db << " dB in 2500 Hz, Eb/N0 " << *eb_n0 << " dB, mistuned "
                    << p.tone_offset_hz << " Hz: character error rate " << cer << " over "
                    << sent.size() << " characters, " << d.framing_errors
                    << " framing errors; unit error rate at the transmitted timing "
                    << unit_error_rate << " over " << units);
        CHECK(cer <= p.allowed_character_error_rate);
        // Printed on success as well, so the figure is in the log.
        WARN("RTTY SNR " << p.snr_2500_db << " dB/2500 Hz (Eb/N0 " << *eb_n0 << " dB), offset "
                         << p.tone_offset_hz << " Hz: CER " << cer << ", unit error rate "
                         << unit_error_rate);
    }
}
