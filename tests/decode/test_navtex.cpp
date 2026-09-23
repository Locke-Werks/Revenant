// NAVTEX: messages framed as ITU-R M.540-2 Annex II Figure 1 draws them,
// through SITOR-B audio and back.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "core/decode/navtex.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/fsk_mod.h"
#include "core/dsp/synth/rds_mod.h"

using namespace revenant;

namespace {

std::string utf8(std::u32string_view text) {
    std::string out;
    for (const char32_t c : text) {
        out.push_back(static_cast<char>(c));
    }
    return out;
}

// Figure 1: at least 10 s of phasing before the first "ZCZC", which is 72
// pairs at 140 ms, and no line end before it.
siggen::SitorModConfig navtex_mod() {
    siggen::SitorModConfig mod;
    mod.phasing_pairs = 72;
    mod.line_end_first = false;
    return mod;
}

std::vector<decode::NavtexMessage> decode_all(const std::vector<float>& audio) {
    auto decoder = decode::NavtexDecoder::create(decode::NavtexConfig{});
    REQUIRE(decoder.has_value());
    std::vector<decode::NavtexMessage> out;
    decoder->process(audio, out);
    decoder->flush(out);
    return out;
}

const std::u32string kBody =
    U"GALE WARNING 042\r\nSOUTHWEST 8 TO 9, INCREASING STORM 10 LATER.\r\n";

}  // namespace

TEST_CASE("a NAVTEX message round trips with its preamble", "[decode][navtex]") {
    auto codes = siggen::ita2_encode_text(siggen::navtex_text('E', 'A', 7, kBody));
    REQUIRE(codes.has_value());
    auto audio = siggen::sitor_b_render(navtex_mod(), *codes);
    REQUIRE(audio.has_value());

    const auto got = decode_all(*audio);
    REQUIRE(got.size() == 1);
    CHECK(got[0].area == 'E');
    CHECK(got[0].subject == 'A');
    CHECK(got[0].serial == 7);
    CHECK(got[0].preamble_clean);
    CHECK(got[0].complete);
    CHECK(got[0].mutilated_characters == 0);
    // The text between the preamble's line end and "NNNN", with the line
    // end before "NNNN" trimmed.
    CHECK(got[0].text == "GALE WARNING 042\r\nSOUTHWEST 8 TO 9, INCREASING STORM 10 LATER.");
    // The first "Z" is traffic character 1, after the letter shift, so its
    // DX copy is in slot 2 x (72 + 1): 146 signals of 7 units of 480
    // samples.
    const double expected = 146.0 * 7.0 * 480.0;
    CHECK(std::abs(static_cast<double>(got[0].position) - expected) < 480.0);
}

TEST_CASE("NAVTEX frames back-to-back messages and serial 00", "[decode][navtex]") {
    std::u32string text = siggen::navtex_text('K', 'B', 0, U"FIRST.\r\n");
    text += siggen::navtex_text('K', 'D', 12, U"SECOND.\r\n");
    auto codes = siggen::ita2_encode_text(text);
    REQUIRE(codes.has_value());
    auto audio = siggen::sitor_b_render(navtex_mod(), *codes);
    REQUIRE(audio.has_value());
    const auto got = decode_all(*audio);
    REQUIRE(got.size() == 2);
    // Clause 6: serial 00 is printed whatever a receiver has filtered.
    CHECK(got[0].serial == 0);
    CHECK(got[0].subject == 'B');
    CHECK(got[0].text == "FIRST.");
    CHECK(got[1].serial == 12);
    CHECK(got[1].subject == 'D');
    CHECK(got[1].text == "SECOND.");
    CHECK(got[0].complete);
    CHECK(got[1].complete);
}

TEST_CASE("a preamble lost in both copies is flagged, as clause 3 requires", "[decode][navtex]") {
    auto codes = siggen::ita2_encode_text(siggen::navtex_text('E', 'A', 7, kBody));
    REQUIRE(codes.has_value());
    auto mod = navtex_mod();
    auto slots = siggen::sitor_b_signals(mod, *codes);
    // Traffic character 0 is the letter shift, 1 to 4 are "ZCZC", 5 is the
    // space and 6 is B1. Damage both copies of B1.
    const std::size_t dx = 2 * (mod.phasing_pairs + 6);
    slots[dx] ^= 0x01U;
    slots[dx + 5] ^= 0x01U;
    auto audio = siggen::sitor_b_render_signals(mod, slots);
    REQUIRE(audio.has_value());
    const auto got = decode_all(*audio);
    REQUIRE(got.size() == 1);
    CHECK_FALSE(got[0].preamble_clean);
    CHECK(got[0].area == '?');
    CHECK(got[0].subject == 'A');
    CHECK(got[0].complete);
}

TEST_CASE("a lost NNNN leaves the message incomplete, not merged", "[decode][navtex]") {
    std::u32string text = siggen::navtex_text('E', 'A', 1, U"ONE.\r\n");
    text += siggen::navtex_text('E', 'A', 2, U"TWO.\r\n");
    auto codes = siggen::ita2_encode_text(text);
    REQUIRE(codes.has_value());
    auto mod = navtex_mod();
    auto slots = siggen::sitor_b_signals(mod, *codes);
    // Find the first message's "NNNN" in the traffic and lose both copies
    // of its second "N".
    const std::uint8_t n = decode::sitor_encode(decode::ita2_encode(U'N')->combination);
    std::size_t hit = 0;
    for (std::size_t i = 0; i < codes->size(); ++i) {
        if (decode::sitor_encode((*codes)[i]) == n && i + 1 < codes->size() &&
            decode::sitor_encode((*codes)[i + 1]) == n) {
            hit = i + 1;
            break;
        }
    }
    REQUIRE(hit != 0);
    const std::size_t dx = 2 * (mod.phasing_pairs + hit);
    slots[dx] ^= 0x01U;
    slots[dx + 5] ^= 0x01U;
    auto audio = siggen::sitor_b_render_signals(mod, slots);
    REQUIRE(audio.has_value());
    const auto got = decode_all(*audio);
    REQUIRE(got.size() == 2);
    CHECK_FALSE(got[0].complete);
    CHECK(got[0].serial == 1);
    CHECK(got[1].complete);
    CHECK(got[1].serial == 2);
    CHECK(got[1].text == "TWO.");
}

TEST_CASE("NAVTEX messages against noise, measured", "[decode][navtex]") {
    // Ten messages of about 120 characters, each its own transmission with
    // its own phasing, through add_real_awgn. A message counts as received
    // when its preamble is clean and its text arrives exactly.
    std::mt19937_64 engine(0x7A7E4ULL);
    constexpr char32_t kPool[] = U"ABCDEFGHIJKLMNOPQRSTUVWXYZ     0123456789.,";
    struct Point {
        double snr_2500_db;
        double allowed_loss;
    };
    // Measured 2026-09-22 at 48 kHz, text seed 0x7A7E4, ten messages of 120
    // characters each: 10 exact at 10 dB in 2500 Hz, 10 exact at -2 dB, and
    // 2 exact at -5 dB (9.0 dB Eb/N0) with all ten preambles still clean,
    // because a preamble is 7 characters and a message is 120. The
    // allowances sit above the measurements.
    const Point points[] = {{10.0, 0.0}, {-2.0, 0.2}, {-5.0, 1.0}};
    for (const Point& p : points) {
        std::size_t received = 0;
        std::size_t clean_preambles = 0;
        constexpr int kMessages = 10;
        for (int m = 0; m < kMessages; ++m) {
            std::u32string body;
            for (int k = 0; k < 120; ++k) {
                body.push_back(kPool[engine() % (std::size(kPool) - 1)]);
            }
            auto codes = siggen::ita2_encode_text(siggen::navtex_text('E', 'A', m + 1, body + U"\r\n"));
            REQUIRE(codes.has_value());
            auto audio = siggen::sitor_b_render(navtex_mod(), *codes);
            REQUIRE(audio.has_value());
            double power = 0.0;
            for (const float v : *audio) {
                power += static_cast<double>(v) * static_cast<double>(v);
            }
            power /= static_cast<double>(audio->size());
            REQUIRE(siggen::add_real_awgn(*audio, power,
                                          siggen::NoiseLevel::snr_in_2500_hz_db(p.snr_2500_db),
                                          48'000, 0xA11 + static_cast<std::uint64_t>(m))
                        .has_value());
            const auto got = decode_all(*audio);
            for (const auto& g : got) {
                if (g.serial == m + 1 && g.preamble_clean) {
                    ++clean_preambles;
                    if (g.text == utf8(body)) {
                        ++received;
                    }
                    break;
                }
            }
        }
        const double loss = 1.0 - static_cast<double>(received) / kMessages;
        auto eb_n0 = siggen::reference_bandwidth_to_eb_over_n0_db(p.snr_2500_db, decode::kSitorBaud);
        REQUIRE(eb_n0.has_value());
        INFO("SNR " << p.snr_2500_db << " dB in 2500 Hz, Eb/N0 " << *eb_n0 << " dB: " << received
                    << " of " << kMessages << " messages exact, " << clean_preambles
                    << " clean preambles");
        CHECK(loss <= p.allowed_loss);
        WARN("NAVTEX SNR " << p.snr_2500_db << " dB/2500 Hz (Eb/N0 " << *eb_n0 << " dB): "
                           << received << "/" << kMessages << " exact, " << clean_preambles
                           << " clean preambles");
    }
}
