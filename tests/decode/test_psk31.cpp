// PSK31, PSK63 and QPSK31: the alphabet and the convolutional code against
// the article, and the receiver against the transmitter.
//
// The two groups tests/decode/CMakeLists.txt describes. The first checks
// core/decode/varicode.h and the QPSK code against what QEX July/August 1999
// states, including the article's own worked example. The second is the
// round trip through core/dsp/synth/psk31_mod.h, at two audio rates and with
// the tone off the receiver's centre by a tuning error, and the error rates
// against noise.
//
// Every error rate below is measured and printed in an INFO line and the
// assertion around it is loose, for the reason the CMakeLists gives: the
// number a reader wants is the one in the log.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <format>
#include <numbers>
#include <set>
#include <string>
#include <vector>

#include "core/decode/dv_codes.h"
#include "core/decode/psk31.h"
#include "core/decode/tone_frontend.h"
#include "core/decode/varicode.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/psk31_mod.h"

using namespace revenant;

namespace {

// Every printable character once, so the round trips exercise the whole of
// Table 1 that a keyboard can reach.
std::string printable_text() {
    std::string text = "cq cq de g3plx ";
    for (int c = 32; c < 127; ++c) {
        text.push_back(static_cast<char>(c));
    }
    text += " the quick brown fox jumps over the lazy dog 0123456789\r\n";
    return text;
}

std::string pseudorandom_text(std::size_t length, std::uint64_t seed) {
    // Lower case letters and spaces, the text PSK31 carries most, so the
    // character error rate reflects the code lengths that dominate on air.
    static constexpr std::string_view kPool = "etaoin shrdlu cmfwyp vbgkqjxz ";
    std::string text;
    std::uint64_t state = seed;
    for (std::size_t i = 0; i < length; ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        text.push_back(kPool[(state >> 33U) % kPool.size()]);
    }
    return text;
}

struct Decoded {
    std::string text;
    std::vector<std::uint8_t> bits;
    double offset_hz = 0.0;
    bool acquired = false;
    double strength = 0.0;
    double rejected = 0.0;
    double rejected_pair = 0.0;
    std::size_t unrecognised = 0;
};

Decoded decode_audio(const decode::Psk31Config& config, std::span<const float> audio,
                     std::size_t block) {
    Decoded result;
    auto decoder = decode::Psk31::create(config);
    INFO((decoder.has_value() ? std::string{} : decoder.error().message));
    REQUIRE(decoder.has_value());
    std::vector<decode::Psk31Character> characters;
    for (std::size_t start = 0; start < audio.size(); start += block) {
        const std::size_t length = std::min(block, audio.size() - start);
        REQUIRE(decoder->process(audio.subspan(start, length), characters).has_value());
        const auto bits = decoder->last_bits();
        result.bits.insert(result.bits.end(), bits.begin(), bits.end());
    }
    for (const auto& character : characters) {
        if (character.recognised) {
            result.text.push_back(static_cast<char>(character.ascii));
        } else {
            ++result.unrecognised;
        }
    }
    result.offset_hz = decoder->frequency_offset_hz();
    result.acquired = decoder->acquired();
    result.strength = decoder->acquisition_strength();
    result.rejected = decoder->strongest_rejected();
    result.rejected_pair = decoder->strongest_rejected_pair();
    return result;
}

// Levenshtein distance, so a dropped or split character counts once rather
// than shifting every character after it into a mismatch.
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

std::vector<float> render(const siggen::Psk31ModConfig& mod, const std::string& text,
                          double snr_in_2500_hz_db, std::uint64_t seed, bool noisy) {
    auto bits = siggen::psk31_message_bits(mod, text);
    REQUIRE(bits.has_value());
    auto analytic = siggen::psk31_render_analytic(mod, *bits);
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

// Bit error rate of `got` against the text bits of `sent`, the bits between
// the preamble and the postamble. Aligned on the best correlation of 256 bits
// from the middle of the text, so that bits lost at the start to a late
// acquisition shorten the comparison rather than defeat it.
double measure_bit_error_rate(const std::vector<std::uint8_t>& sent, std::size_t preamble,
                              std::size_t postamble, const std::vector<std::uint8_t>& got,
                              std::size_t& compared) {
    compared = 0;
    constexpr std::size_t kPattern = 256;
    if (got.size() < kPattern || sent.size() < preamble + postamble + 2 * kPattern) {
        return 1.0;
    }
    const std::size_t text_end = sent.size() - postamble;
    const std::size_t middle = (preamble + text_end) / 2 - kPattern / 2;

    std::vector<float> recovered(got.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        recovered[i] = got[i] ? 1.0F : -1.0F;
    }
    std::vector<float> pattern(kPattern);
    for (std::size_t i = 0; i < kPattern; ++i) {
        pattern[i] = sent[middle + i] ? 1.0F : -1.0F;
    }
    auto hit = decode::correlate_pattern(recovered, pattern);
    if (!hit || hit->score < 0.3) {
        return 1.0;
    }
    // got[j] lines up with sent[j + shift].
    const auto shift = static_cast<std::ptrdiff_t>(middle) - static_cast<std::ptrdiff_t>(hit->offset);
    std::size_t errors = 0;
    for (std::size_t s = preamble; s < text_end; ++s) {
        const std::ptrdiff_t j = static_cast<std::ptrdiff_t>(s) - shift;
        if (j < 0 || static_cast<std::size_t>(j) >= got.size()) {
            continue;
        }
        errors += static_cast<std::size_t>(got[static_cast<std::size_t>(j)] != sent[s]);
        ++compared;
    }
    return compared == 0 ? 1.0 : static_cast<double>(errors) / static_cast<double>(compared);
}

}  // namespace

// ---------------------------------------------------------------------------
// The alphabet and the code, against the article
// ---------------------------------------------------------------------------

TEST_CASE("Varicode obeys the rules QEX page 5 builds it from", "[decode][psk31]") {
    // Every code starts and ends with a 1, never holds 00, is at most ten
    // bits, and no two characters share one. The arrl.org/psk31-spec copy of
    // this table fails the last rule twice, which is what this case exists to
    // catch in a retyping.
    std::set<std::string_view> seen;
    for (std::size_t i = 0; i < decode::kVaricode.size(); ++i) {
        const std::string_view code = decode::kVaricode[i];
        INFO("ASCII " << i << " code " << code);
        REQUIRE(!code.empty());
        CHECK(code.front() == '1');
        CHECK(code.back() == '1');
        CHECK(code.find("00") == std::string_view::npos);
        CHECK(code.size() <= decode::kVaricodeMaxBits);
        CHECK(code.find_first_not_of("01") == std::string_view::npos);
        CHECK(seen.insert(code).second);
    }

    // Spot checks against Table 1 as printed, including the two rows the
    // ARRL web copy has wrong.
    CHECK(decode::kVaricode[' '] == "1");
    CHECK(decode::kVaricode['e'] == "11");
    CHECK(decode::kVaricode['t'] == "101");
    CHECK(decode::kVaricode['Z'] == "1010101101");
    CHECK(decode::kVaricode['p'] == "111111");
    CHECK(decode::kVaricode['\''] == "101111111");
    CHECK(decode::kVaricode['`'] == "1011011111");
    CHECK(decode::kVaricode[127] == "1110110101");
}

TEST_CASE("Varicode spends the short codes on lower case, as page 5 says",
          "[decode][psk31]") {
    // Page 5: "Varicode the shortest code is allocated to the word space",
    // and Table 1's caption: "the lower case letters have the shortest
    // patterns". Checked as a property rather than retyped: every lower case
    // letter is at least as short as its upper case partner.
    CHECK(decode::kVaricode[' '].size() == 1);
    for (char c = 'a'; c <= 'z'; ++c) {
        INFO("letter " << c);
        CHECK(decode::kVaricode[static_cast<std::size_t>(c)].size() <=
              decode::kVaricode[static_cast<std::size_t>(c - 'a' + 'A')].size());
    }
}

TEST_CASE("every Varicode character survives the encoder and the decoder",
          "[decode][psk31]") {
    std::string all;
    for (int c = 0; c < 128; ++c) {
        all.push_back(static_cast<char>(c));
    }
    auto bits = decode::varicode_encode(all);
    REQUIRE(bits.has_value());

    decode::VaricodeDecoder decoder;
    std::vector<decode::VaricodeCharacter> out;
    std::uint64_t index = 0;
    // Idle before, as a transmission has.
    for (int i = 0; i < 8; ++i) {
        decoder.push(0, index++, out);
    }
    for (const std::uint8_t bit : *bits) {
        decoder.push(bit, index++, out);
    }
    REQUIRE(out.size() == all.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        INFO("character " << i);
        CHECK(out[i].recognised);
        CHECK(out[i].ascii == static_cast<std::uint8_t>(all[i]));
        CHECK(out[i].bits == decode::kVaricode[i].size());
    }
    CHECK(!decode::varicode_for(128).has_value());
}

TEST_CASE("a code longer than ten bits is reported, not guessed", "[decode][psk31]") {
    // QEX page 9: early decoders ignore a code with no 00 ten bits after the
    // last one. The extended alphabet's code 128 is 1110111101, ten bits and
    // not in Table 1, and 255 is 101101011011, twelve.
    decode::VaricodeDecoder decoder;
    std::vector<decode::VaricodeCharacter> out;
    std::uint64_t index = 0;
    for (const char c : std::string_view{"00" "101101011011" "00" "1110111101" "00"}) {
        decoder.push(static_cast<std::uint8_t>(c - '0'), index++, out);
    }
    REQUIRE(out.size() == 2);
    CHECK(!out[0].recognised);
    CHECK(!out[1].recognised);
}

TEST_CASE("the QPSK31 generators reproduce every entry of the page 9 table",
          "[decode][psk31]") {
    for (std::uint32_t run = 0; run < 32; ++run) {
        INFO("run of five " << run);
        CHECK(decode::qpsk31_shift_from_generators(run) == decode::kQpsk31PhaseTable[run]);
    }

    // The same thing through dv_codes' encoder, which is what the Viterbi
    // decoder's trellis is built from: its two outputs for each register
    // state are the signs that pick the shift.
    decode::ConvolutionalCode code;
    code.memory = decode::kQpsk31Memory;
    code.generators = decode::kQpsk31Generators;
    std::vector<std::uint8_t> input;
    std::uint64_t state = 0x9E37'79B9'7F4A'7C15ULL;
    for (int i = 0; i < 200; ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        input.push_back(static_cast<std::uint8_t>((state >> 40U) & 1U));
    }
    auto encoded = decode::convolutional_encode(code, input);
    REQUIRE(encoded.has_value());
    const std::vector<std::uint8_t> shifts =
        siggen::psk31_symbol_shifts(decode::Psk31Mode::Qpsk31, input);
    for (std::size_t i = 0; i < input.size(); ++i) {
        const std::uint8_t in_phase = (*encoded)[2 * i];
        const std::uint8_t quadrature = (*encoded)[2 * i + 1];
        std::uint8_t expected = decode::kPskShiftReverse;
        if (in_phase != 0U) {
            expected = quadrature != 0U ? decode::kPskShiftNone : decode::kPskShiftRetard;
        } else if (quadrature != 0U) {
            expected = decode::kPskShiftAdvance;
        }
        INFO("bit " << i);
        CHECK(shifts[i] == expected);
    }
}

TEST_CASE("the space character encodes to the article's worked example", "[decode][psk31]") {
    // QEX page 9: "the 'space' symbol, a single 1 preceded and followed by
    // zeros, would be represented by successive run-of-five groups 00000,
    // 00001, 00010, 00100, 01000, 10000, 00000, which results in the
    // transmitter sending the QPSK pattern 2,1,3,3,0,1,2."
    const std::vector<std::uint8_t> bits = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0};
    const std::vector<std::uint8_t> shifts =
        siggen::psk31_symbol_shifts(decode::Psk31Mode::Qpsk31, bits);
    const std::vector<std::uint8_t> tail(shifts.end() - 7, shifts.end());
    CHECK(tail == std::vector<std::uint8_t>{2, 1, 3, 3, 0, 1, 2});

    // And the page's closing note: continuous zeros give continuous
    // reversals, the same as BPSK.
    const std::vector<std::uint8_t> idle(12, 0);
    for (const std::uint8_t shift : siggen::psk31_symbol_shifts(decode::Psk31Mode::Qpsk31, idle)) {
        CHECK(shift == decode::kPskShiftReverse);
    }
}

// ---------------------------------------------------------------------------
// The test channel itself
// ---------------------------------------------------------------------------

TEST_CASE("noisy PSK31 audio carries the signal to noise ratio it was asked for",
          "[decode][psk31]") {
    // core/dsp/synth/psk31_mod.h says the complex simulator's noise, taken
    // through the real part, lands 3.01 dB worse than asked, and that
    // analytic_to_noisy_audio compensates. Both halves measured here, by
    // differencing noisy audio against clean and scaling the white noise's
    // power to 2500 Hz of the one-sided audio spectrum.
    siggen::Psk31ModConfig mod;
    auto bits = siggen::psk31_message_bits(mod, "measure the channel, not the decoder");
    REQUIRE(bits.has_value());
    auto analytic = siggen::psk31_render_analytic(mod, *bits);
    REQUIRE(analytic.has_value());
    const std::vector<float> clean = siggen::analytic_to_audio(*analytic);

    const auto realised_db = [&](const std::vector<float>& noisy) {
        double signal = 0.0;
        double noise = 0.0;
        for (std::size_t i = 0; i < clean.size(); ++i) {
            signal += static_cast<double>(clean[i]) * clean[i];
            const double n = static_cast<double>(noisy[i]) - clean[i];
            noise += n * n;
        }
        const double in_band =
            noise * 2500.0 / (0.5 * static_cast<double>(mod.rate));
        return 10.0 * std::log10(signal / in_band);
    };

    constexpr double kAsked = -6.0;
    auto compensated = siggen::analytic_to_noisy_audio(
        *analytic, kAsked, siggen::kWsjtxReferenceBandwidthHz, mod.rate, 0xC0FFEEULL);
    REQUIRE(compensated.has_value());
    const double got = realised_db(*compensated);

    std::vector<siggen::Complex32> naive(analytic->begin(), analytic->end());
    REQUIRE(siggen::add_awgn(naive, siggen::NoiseLevel::snr_in_2500_hz_db(kAsked), mod.rate,
                             0xC0FFEEULL)
                .has_value());
    const double uncompensated = realised_db(siggen::analytic_to_audio(naive));

    INFO("asked " << kAsked << " dB, compensated route delivered " << got
                  << " dB, the complex simulator's noise through the real part " << uncompensated
                  << " dB");
    CHECK(std::abs(got - kAsked) < 0.1);
    CHECK(std::abs(uncompensated - (kAsked - 3.0103)) < 0.1);
}

// ---------------------------------------------------------------------------
// The receiver against the transmitter
// ---------------------------------------------------------------------------

TEST_CASE("the PSK31 receiver does not acquire on noise alone", "[decode][psk31]") {
    // Twenty seconds of white audio. The acquisition threshold in
    // core/decode/psk31.cpp is what keeps this from decoding garbage at a
    // frequency picked out of the noise.
    constexpr dsp::SampleRate kRate = 48000;
    std::vector<siggen::Complex32> noise(static_cast<std::size_t>(20 * kRate));
    REQUIRE(siggen::add_awgn_at_power(noise, 1.0, 0x0015E0ULL).has_value());
    const std::vector<float> audio = siggen::analytic_to_audio(noise);

    for (const decode::Psk31Mode mode :
         {decode::Psk31Mode::Bpsk31, decode::Psk31Mode::Qpsk31, decode::Psk31Mode::Bpsk63}) {
        decode::Psk31Config config;
        config.rate = kRate;
        config.mode = mode;
        const Decoded got = decode_audio(config, audio, 9600);
        INFO("mode " << static_cast<int>(mode) << " acquired " << got.acquired << " strength "
                     << got.strength << ", strongest line in noise " << got.rejected
                     << ", strongest pair " << got.rejected_pair << ", text '" << got.text << "'");
        WARN("PSK noise-only mode " << static_cast<int>(mode) << ": strongest line "
                                    << got.rejected << ", strongest pair " << got.rejected_pair);
        CHECK(!got.acquired);
        CHECK(got.text.empty());
    }
}

TEST_CASE("the idle's pair of tones is found and a lone carrier is not taken for one",
          "[decode][psk31]") {
    // 64 symbols at 16 samples a symbol, 500 S/s, as PSK31's acquisition
    // window: a carrier 8 Hz up reversed every symbol, which is two tones at
    // 8 +/- 15.625 Hz, and the same carrier unreversed.
    constexpr double kRate = 500.0;
    std::vector<dsp::Complex32> idle(1024);
    std::vector<dsp::Complex32> carrier(1024);
    for (std::size_t n = 0; n < idle.size(); ++n) {
        const double t = static_cast<double>(n) / kRate;
        const std::complex<double> tone = std::polar(1.0, 2.0 * std::numbers::pi * 8.0 * t);
        const std::complex<double> reversed = tone * std::cos(std::numbers::pi * 31.25 * t);
        idle[n] = dsp::Complex32(static_cast<float>(reversed.real()), static_cast<float>(reversed.imag()));
        carrier[n] = dsp::Complex32(static_cast<float>(tone.real()), static_cast<float>(tone.imag()));
    }
    auto pair = decode::estimate_tone_pair(idle, 500, 15.625, 40.0);
    auto lone = decode::estimate_tone_pair(carrier, 500, 15.625, 40.0);
    REQUIRE(pair.has_value());
    REQUIRE(lone.has_value());
    INFO("idle: " << pair->line_to_mean << " at " << pair->offset_hz << " Hz; lone carrier: "
                  << lone->line_to_mean);
    CHECK(std::abs(pair->offset_hz - 8.0) < 0.05);
    CHECK(pair->line_to_mean > 10.0);
    CHECK(lone->line_to_mean < 2.0);
    CHECK_FALSE(decode::estimate_tone_pair(idle, 500, 15.625, 240.0).has_value());
}

TEST_CASE("QPSK31 acquires on its idle preamble where the squared line falls short",
          "[decode][psk31]") {
    // At -12 dB in 2500 Hz the squared line of the 64 symbol idle often falls
    // short of its threshold, and QPSK data after it carries no line the
    // square or the fourth power can find at that level. Until the idle's two
    // tones were read directly, such a transmission was acquired on its
    // postamble and printed almost nothing: 11 of the bench sweep's first 16
    // at this level. Each seed here is one the squared line turned down.
    const std::string text = "the quick brown fox jumps over the lazy dog";
    for (std::uint64_t seed = 0x53; seed < 0x5B; ++seed) {
        siggen::Psk31ModConfig mod;
        mod.rate = 48000;
        mod.tone_hz = 1508;
        mod.mode = decode::Psk31Mode::Qpsk31;
        const std::vector<float> audio = render(mod, text, -12.0, seed, true);

        decode::Psk31Config config;
        config.rate = 48000;
        config.centre_hz = 1500;
        config.mode = decode::Psk31Mode::Qpsk31;
        auto decoder = decode::Psk31::create(config);
        REQUIRE(decoder.has_value());
        std::vector<decode::Psk31Character> characters;
        // The preamble and a tenth of a second more: acquisition has to
        // happen in it.
        const std::size_t preamble_samples = 64 * 48000 * 100 / 3125 + 4800;
        const std::span<const float> all(audio);
        REQUIRE(decoder->process(all.first(preamble_samples), characters).has_value());
        const bool on_preamble = decoder->acquired();
        REQUIRE(decoder->process(all.subspan(preamble_samples), characters).has_value());
        std::string got;
        for (const auto& character : characters) {
            if (character.recognised) {
                got.push_back(static_cast<char>(character.ascii));
            }
        }
        const double cer = static_cast<double>(edit_distance(text, got)) / static_cast<double>(text.size());
        INFO("seed " << seed << ": squared line turned down at " << decoder->strongest_rejected()
                     << ", acquired on the preamble " << on_preamble << " at strength "
                     << decoder->acquisition_strength() << ", offset " << decoder->frequency_offset_hz()
                     << " Hz against 8, text '" << got << "'");
        CHECK(decoder->strongest_rejected() > 0.0);
        CHECK(on_preamble);
        CHECK(cer <= 0.3);
        // A quarter of the 3.9 Hz either way a QPSK31 decision tolerates.
        CHECK(std::abs(decoder->frequency_offset_hz() - 8.0) < 1.0);
    }
}

TEST_CASE("BPSK31 round trips at 48 kHz with the tone off centre", "[decode][psk31]") {
    const std::string text = printable_text();
    siggen::Psk31ModConfig mod;
    mod.rate = 48000;
    mod.tone_hz = 1012;  // a 12 Hz tuning error against the receiver below
    const std::vector<float> audio = render(mod, text, 0.0, 0, false);

    decode::Psk31Config config;
    config.rate = 48000;
    config.centre_hz = 1000;
    const Decoded got = decode_audio(config, audio, 4096);
    INFO("decoded: " << got.text);
    INFO("measured offset " << got.offset_hz << " Hz against 12");
    CHECK(got.acquired);
    CHECK(got.text == text);
    CHECK(got.unrecognised == 0);
    CHECK(std::abs(got.offset_hz - 12.0) < 0.2);
}

TEST_CASE("BPSK31 round trips at 11025 Hz with the tone below centre", "[decode][psk31]") {
    const std::string text = printable_text();
    siggen::Psk31ModConfig mod;
    mod.rate = 11025;
    mod.tone_hz = 1473;  // 27 Hz below the receiver's 1500
    const std::vector<float> audio = render(mod, text, 0.0, 0, false);

    decode::Psk31Config config;
    config.rate = 11025;
    config.centre_hz = 1500;
    const Decoded got = decode_audio(config, audio, 1000);
    INFO("decoded: " << got.text);
    INFO("measured offset " << got.offset_hz << " Hz against -27");
    CHECK(got.text == text);
    CHECK(std::abs(got.offset_hz + 27.0) < 0.2);
}

TEST_CASE("PSK63 round trips at 48 kHz with the tone off centre", "[decode][psk31]") {
    const std::string text = printable_text();
    siggen::Psk31ModConfig mod;
    mod.rate = 48000;
    mod.tone_hz = 1491;
    mod.mode = decode::Psk31Mode::Bpsk63;
    mod.preamble_symbols = 128;
    mod.postamble_symbols = 128;
    const std::vector<float> audio = render(mod, text, 0.0, 0, false);

    decode::Psk31Config config;
    config.rate = 48000;
    config.centre_hz = 1500;
    config.mode = decode::Psk31Mode::Bpsk63;
    config.acquisition_symbols = 128;
    const Decoded got = decode_audio(config, audio, 4800);
    INFO("decoded: " << got.text);
    INFO("measured offset " << got.offset_hz << " Hz against -9");
    CHECK(got.text == text);
    CHECK(std::abs(got.offset_hz + 9.0) < 0.3);
}

TEST_CASE("QPSK31 round trips at 48 kHz with the tone off centre", "[decode][psk31]") {
    const std::string text = printable_text();
    siggen::Psk31ModConfig mod;
    mod.rate = 48000;
    mod.tone_hz = 1507;
    mod.mode = decode::Psk31Mode::Qpsk31;
    const std::vector<float> audio = render(mod, text, 0.0, 0, false);

    decode::Psk31Config config;
    config.rate = 48000;
    config.centre_hz = 1500;
    config.mode = decode::Psk31Mode::Qpsk31;
    const Decoded got = decode_audio(config, audio, 4096);
    INFO("decoded: " << got.text);
    INFO("measured offset " << got.offset_hz << " Hz against 7");
    CHECK(got.text == text);
    CHECK(std::abs(got.offset_hz - 7.0) < 0.2);
}

TEST_CASE("the PSK31 receiver does not depend on how its input is blocked",
          "[decode][psk31]") {
    const std::string text = "block invariance matters for a replay";
    siggen::Psk31ModConfig mod;
    mod.tone_hz = 1003;
    mod.mode = decode::Psk31Mode::Qpsk31;
    const std::vector<float> audio = render(mod, text, 10.0, 0x5EED'0001ULL, true);

    decode::Psk31Config config;
    config.mode = decode::Psk31Mode::Qpsk31;
    const Decoded whole = decode_audio(config, audio, audio.size());
    const Decoded small = decode_audio(config, audio, 997);
    CHECK(whole.text == small.text);
    CHECK(whole.bits == small.bits);
    CHECK(whole.text == text);
}

TEST_CASE("flush hands over what QPSK31's Viterbi decoder held when the stream stopped",
          "[decode][psk31]") {
    // A transmission cut off a few symbols after its last character. The
    // Viterbi decoder holds decision_delay_bits and up to kViterbiCommitBits
    // more undecided, so without flush the tail of the text goes with it.
    // Clean audio, so every bit flush decides is the bit sent.
    const std::string text = "the end of a stream";
    siggen::Psk31ModConfig mod;
    mod.tone_hz = 1004;
    mod.mode = decode::Psk31Mode::Qpsk31;
    mod.postamble_symbols = 40;
    const std::vector<float> audio = render(mod, text, 0.0, 0, false);

    decode::Psk31Config config;
    config.mode = decode::Psk31Mode::Qpsk31;
    auto decoder = decode::Psk31::create(config);
    REQUIRE(decoder.has_value());
    std::vector<decode::Psk31Character> characters;
    REQUIRE(decoder->process(audio, characters).has_value());
    std::string before;
    for (const auto& c : characters) {
        before.push_back(static_cast<char>(c.ascii));
    }
    INFO("before flush: '" << before << "'");
    REQUIRE(before.size() < text.size());
    CHECK(text.starts_with(before));

    const std::size_t held = characters.size();
    decoder->flush(characters);
    std::string after;
    for (const auto& c : characters) {
        after.push_back(static_cast<char>(c.ascii));
    }
    INFO("after flush: '" << after << "', " << decoder->last_bits().size() << " bits committed");
    CHECK(after == text);
    CHECK(!decoder->last_bits().empty());
    for (std::size_t i = held; i < characters.size(); ++i) {
        CHECK(characters[i].recognised);
        CHECK(characters[i].first_sample > characters[held - 1].first_sample);
    }

    // Nothing is held twice.
    decoder->flush(characters);
    CHECK(characters.size() == text.size());
    CHECK(decoder->last_bits().empty());
}

TEST_CASE("flush on a binary PSK stream appends nothing", "[decode][psk31]") {
    const std::string text = "binary holds no bits";
    siggen::Psk31ModConfig mod;
    mod.tone_hz = 1004;
    mod.postamble_symbols = 40;
    const std::vector<float> audio = render(mod, text, 0.0, 0, false);

    auto decoder = decode::Psk31::create(decode::Psk31Config{});
    REQUIRE(decoder.has_value());
    std::vector<decode::Psk31Character> characters;
    REQUIRE(decoder->process(audio, characters).has_value());
    const std::size_t before = characters.size();
    decoder->flush(characters);
    CHECK(characters.size() == before);
    CHECK(decoder->last_bits().empty());
}

TEST_CASE("the PSK31 matched filter's output is pinned bit for bit", "[decode][psk31]") {
    // The matched filter used to erase the front of its history once per
    // sample and now trims it once per call. That changed the bookkeeping and
    // not one sum, so every decision and the fine AFC's average come out the
    // same, and the figures below were taken from the build before the change.
    // The AFC's residual is a leaky average of every differential phasor, so
    // frequency_offset_hz moves in its last bit if any filter output does.
    // Noisy QPSK31 in 997-sample blocks and whole, and BPSK63 in blocks of 1000.
    struct Pin {
        decode::Psk31Mode mode;
        std::size_t block;
        std::uint64_t bits_hash;
        std::uint64_t offset_bits;
    };
    const auto hash_of = [](const Decoded& got) {
        std::uint64_t h = 0xCBF2'9CE4'8422'2325ULL;
        for (const std::uint8_t bit : got.bits) {
            h = (h ^ bit) * 0x0100'0000'01B3ULL;
        }
        return h;
    };
    const std::string text = pseudorandom_text(120, 0x0B17'1DE7ULL);
    const Pin pins[] = {
        {decode::Psk31Mode::Qpsk31, 997, 0x0933'1AAE'5026'3DB4ULL, 0x4018'402D'B068'B309ULL},
        {decode::Psk31Mode::Qpsk31, 0, 0x0933'1AAE'5026'3DB4ULL, 0x4018'402D'B068'B309ULL},
        {decode::Psk31Mode::Bpsk63, 1000, 0x44E1'E1A2'0136'021AULL, 0x4018'260A'C01F'CDB5ULL},
    };
    for (const Pin& pin : pins) {
        siggen::Psk31ModConfig mod;
        mod.tone_hz = 1006;
        mod.mode = pin.mode;
        const std::vector<float> audio = render(mod, text, -4.0, 0x0B17'5EEDULL, true);
        decode::Psk31Config config;
        config.mode = pin.mode;
        if (pin.mode == decode::Psk31Mode::Bpsk63) {
            config.acquisition_symbols = 128;
        }
        const Decoded got = decode_audio(config, audio, pin.block == 0 ? audio.size() : pin.block);
        const std::uint64_t hash = hash_of(got);
        const auto offset = std::bit_cast<std::uint64_t>(got.offset_hz);
        WARN(std::format("PSK pin mode {} block {}: {} bits, hash {:#018x}, offset {} Hz bits "
                         "{:#018x}",
                         static_cast<int>(pin.mode), pin.block, got.bits.size(), hash,
                         got.offset_hz, offset));
        CHECK(hash == pin.bits_hash);
        CHECK(offset == pin.offset_bits);
    }
}

TEST_CASE("PSK31 error rates against noise, measured", "[decode][psk31]") {
    // Signal to noise in the 2500 Hz reference bandwidth of
    // docs/snr-convention.md. For PSK31 Eb/N0 is that figure plus
    // 10*log10(2500/31.25) = 19.03 dB, so -12 dB here is an Eb/N0 of 7 dB.
    struct Point {
        decode::Psk31Mode mode;
        double snr_db;
        double allowed_character_error_rate;
    };
    // Measured on 2026-09-22 over 600 characters, 4453 text bits, with this
    // seed:
    //
    //   BPSK31  +10 dB: BER 0,       CER 0
    //   BPSK31  -10 dB: BER 2.5e-3,  CER 0.023
    //   QPSK31  +10 dB: BER 0,       CER 0
    //   QPSK31  -12 dB: BER 2.8e-2,  CER 0.090
    //   PSK63   +10 dB: BER 0,       CER 0
    //   PSK63    -7 dB: BER 4.0e-3,  CER 0.038
    //
    // Differential BPSK theory at -10 dB, an Eb/N0 of 9.03 dB, is a bit error
    // rate of 0.5*exp(-Eb/N0) = 1.7e-4, so this receiver sits about 2 dB from
    // it. Most of that is the matched filter: the pulse is two symbols long,
    // not a Nyquist pulse, and after matching it leaves a sixth of each
    // neighbour on every symbol.
    //
    // The allowances are loose on purpose.
    const Point points[] = {
        {decode::Psk31Mode::Bpsk31, 10.0, 0.0},
        {decode::Psk31Mode::Bpsk31, -10.0, 0.08},
        {decode::Psk31Mode::Qpsk31, 10.0, 0.0},
        {decode::Psk31Mode::Qpsk31, -12.0, 0.20},
        {decode::Psk31Mode::Bpsk63, 10.0, 0.0},
        {decode::Psk31Mode::Bpsk63, -7.0, 0.12},
    };

    const std::string text = pseudorandom_text(600, 0xFACE'0FF5'1234'5678ULL);
    for (const Point& point : points) {
        siggen::Psk31ModConfig mod;
        mod.rate = 48000;
        mod.tone_hz = 1508;
        mod.mode = point.mode;
        auto sent = siggen::psk31_message_bits(mod, text);
        REQUIRE(sent.has_value());
        const std::vector<float> audio = render(mod, text, point.snr_db, 0xA11CE ^ 0x42ULL, true);

        decode::Psk31Config config;
        config.rate = 48000;
        config.centre_hz = 1500;
        config.mode = point.mode;
        const Decoded got = decode_audio(config, audio, 8192);

        std::size_t compared = 0;
        const double ber = measure_bit_error_rate(*sent, mod.preamble_symbols,
                                                  mod.postamble_symbols, got.bits, compared);
        const double cer = static_cast<double>(edit_distance(text, got.text)) /
                           static_cast<double>(text.size());
        INFO("mode " << static_cast<int>(point.mode) << " at " << point.snr_db
                     << " dB in 2500 Hz: bit error rate " << ber << " over " << compared
                     << " bits, character error rate " << cer << ", offset " << got.offset_hz
                     << " Hz against 8");
        CHECK(got.acquired);
        CHECK(cer <= point.allowed_character_error_rate);
        // Printed on success as well, because the figure is the result.
        WARN("PSK mode " << static_cast<int>(point.mode) << " at " << point.snr_db
                         << " dB in 2500 Hz: BER " << ber << " over " << compared
                         << " bits, CER " << cer);
    }
}
