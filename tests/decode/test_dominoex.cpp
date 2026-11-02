// DominoEX: the varicode table's own rules and printed entries, IFK+, then
// the receiver against the transmitter. Documents: core/decode/dominoex.h.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "core/decode/dominoex.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dominoex_mod.h"
#include "core/dsp/synth/rds_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 8000;

const std::string kText =
    "cq cq de zl1bpu the quick brown fox jumps over the lazy dog 0123456789 "
    "DominoEX Test, with Capitals & symbols: (ok?) = yes/no; \"quoted\" @ 42% "
    "nvis on eighty metres. ";

double mean_power(const std::vector<float>& x) {
    double sum = 0.0;
    for (const float v : x) {
        sum += static_cast<double>(v) * v;
    }
    return x.empty() ? 0.0 : sum / static_cast<double>(x.size());
}

// Edit distance from a to the best-matching stretch of b: characters b has
// before and after that stretch are free. DominoEX has no preamble, so noise
// before and after the transmission decodes to characters; those are not
// errors in the text and are not counted here.
std::size_t edit_distance(const std::string& a, const std::string& b) {
    std::vector<std::size_t> prev(b.size() + 1, 0);
    std::vector<std::size_t> cur(b.size() + 1);
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1,
                               prev[j - 1] + (a[i - 1] == b[j - 1] ? 0U : 1U)});
        }
        std::swap(prev, cur);
    }
    return *std::min_element(prev.begin(), prev.end());
}

struct Decoded {
    std::string primary;
    std::string secondary;
};

Decoded decode_text(const decode::DominoConfig& cfg, const std::vector<float>& audio,
                    std::size_t chunk = 0) {
    auto dec = decode::DominoDecoder::create(cfg);
    REQUIRE(dec.has_value());
    std::vector<decode::DominoCharacter> out;
    if (chunk == 0) {
        dec->process(audio, out);
    } else {
        for (std::size_t at = 0; at < audio.size(); at += chunk) {
            dec->process(std::span<const float>(audio.data() + at, std::min(chunk, audio.size() - at)),
                         out);
        }
    }
    dec->flush(out);
    Decoded d;
    for (const auto& c : out) {
        (c.secondary ? d.secondary : d.primary).push_back(static_cast<char>(c.code));
    }
    return d;
}

double cer(const std::string& got) {
    return static_cast<double>(edit_distance(kText, got)) / static_cast<double>(kText.size());
}

}  // namespace

TEST_CASE("DominoEX varicode follows the table's notes", "[dominoex]") {
    // Printed entries: SPACE 0 / 3,8,8; e 1 / 3,8,9; a 4 / 3,8,12; E 3,8;
    // NUL 1,15,9; DEL 2,10,15 / 7,10,15; BLANK 255 1,15,8 / 6,15,8.
    using V = std::vector<std::uint8_t>;
    CHECK(decode::domino_nibbles(' ', false) == V{0});
    CHECK(decode::domino_nibbles(' ', true) == V{3, 8, 8});
    CHECK(decode::domino_nibbles('e', false) == V{1});
    CHECK(decode::domino_nibbles('e', true) == V{3, 8, 9});
    CHECK(decode::domino_nibbles('a', false) == V{4});
    CHECK(decode::domino_nibbles('a', true) == V{3, 8, 12});
    CHECK(decode::domino_nibbles('E', false) == V{3, 8});
    CHECK(decode::domino_nibbles(0, false) == V{1, 15, 9});
    CHECK(decode::domino_nibbles(0, true).empty());
    CHECK(decode::domino_nibbles(127, false) == V{2, 10, 15});
    CHECK(decode::domino_nibbles(127, true) == V{7, 10, 15});
    CHECK(decode::domino_nibbles(255, false) == V{1, 15, 8});
    CHECK(decode::domino_nibbles(255, true) == V{6, 15, 8});

    // Notes 5 and 6: initial nibble below 8, continuations 8 and above.
    // Eight single-nibble characters, Technical "Character-based System".
    int singles = 0;
    std::set<std::vector<std::uint8_t>> seen;
    int duplicates = 0;
    for (int c = 0; c < 256; ++c) {
        for (const bool sec : {false, true}) {
            const auto n = decode::domino_nibbles(static_cast<std::uint8_t>(c), sec);
            if (n.empty()) {
                continue;
            }
            CHECK(n[0] < 8);
            for (std::size_t i = 1; i < n.size(); ++i) {
                CHECK(n[i] >= 8);
            }
            singles += n.size() == 1 ? 1 : 0;
            duplicates += seen.insert(n).second ? 0 : 1;
            const auto back = decode::domino_lookup(n);
            CHECK(back.valid);
            CHECK(back.secondary == sec);
        }
    }
    CHECK(singles == 8);
    // The three collisions the header records: ':' and '<' on both
    // alphabets, 'J' and '4' on the secondary.
    CHECK(duplicates == 3);
    CHECK(decode::domino_lookup(decode::domino_nibbles('<', false)).code == ':');
}

TEST_CASE("DominoEX IFK+ never repeats a tone", "[dominoex]") {
    std::vector<int> nibbles;
    for (int i = 0; i < 400; ++i) {
        nibbles.push_back((i * 7 + i / 3) % 16);
    }
    const auto tones = decode::ifk_encode(nibbles, decode::kDominoTones, decode::kDominoIfkOffset);
    for (std::size_t i = 1; i < tones.size(); ++i) {
        CHECK(tones[i] != tones[i - 1]);
    }
    CHECK(decode::ifk_decode(tones, decode::kDominoTones, decode::kDominoIfkOffset) == nibbles);
}

TEST_CASE("DominoEX round trips at every speed, offset and on LSB", "[dominoex]") {
    for (const auto speed : {decode::DominoSpeed::D4, decode::DominoSpeed::D5, decode::DominoSpeed::D8,
                             decode::DominoSpeed::D11, decode::DominoSpeed::D16,
                             decode::DominoSpeed::D22}) {
        for (const bool lsb : {false, true}) {
            siggen::DominoModConfig mod;
            mod.speed = speed;
            mod.base_hz = 1111.1;
            mod.lower_sideband = lsb;
            mod.lead_in_samples = 2345.6;
            mod.tail_samples = 3000;
            mod.idle_after = "de zl1bpu ";
            auto audio = siggen::domino_render(mod, kText);
            REQUIRE(audio.has_value());
            decode::DominoConfig cfg;
            cfg.speed = speed;
            cfg.lower_sideband = lsb;
            const Decoded d = decode_text(cfg, *audio);
            INFO("speed " << static_cast<int>(speed) << " lsb " << lsb << " got " << d.primary);
            // The silence before the signal can leave one character of
            // garbage: there is no preamble to wait for.
            CHECK(d.primary.ends_with(kText));
            CHECK(d.primary.size() <= kText.size() + 1);
            CHECK(d.secondary.ends_with("de zl1bpu "));
        }
    }
}

TEST_CASE("DominoEX output does not depend on chunking", "[dominoex]") {
    siggen::DominoModConfig mod;
    mod.lead_in_samples = 999;
    auto audio = siggen::domino_render(mod, kText);
    REQUIRE(audio.has_value());
    REQUIRE(siggen::add_real_awgn(*audio, mean_power(*audio),
                                  siggen::NoiseLevel::snr_in_2500_hz_db(-8.0), kRate, 3)
                .has_value());
    decode::DominoConfig cfg;
    const Decoded whole = decode_text(cfg, *audio);
    CHECK(decode_text(cfg, *audio, 1).primary == whole.primary);
    CHECK(decode_text(cfg, *audio, 517).primary == whole.primary);
}

TEST_CASE("DominoEX through noise, with drift", "[dominoex][noise]") {
    struct Point {
        decode::DominoSpeed speed;
        double snr;
        double limit;
    };
    // Limits of 1 are measurement points.
    const Point points[] = {
        {decode::DominoSpeed::D11, 0.0, 0.01},  {decode::DominoSpeed::D11, -6.0, 0.02},
        {decode::DominoSpeed::D11, -9.0, 1.0},  {decode::DominoSpeed::D11, -12.0, 1.0},
        {decode::DominoSpeed::D4, -10.0, 0.02}, {decode::DominoSpeed::D4, -14.0, 1.0},
        {decode::DominoSpeed::D4, -17.0, 1.0},  {decode::DominoSpeed::D22, -6.0, 1.0},
        {decode::DominoSpeed::D22, -9.0, 1.0},
    };
    for (const Point& p : points) {
        const int runs = 3;
        double total = 0.0;
        for (int r = 0; r < runs; ++r) {
            siggen::DominoModConfig mod;
            mod.speed = p.speed;
            mod.base_hz = 1200.0 + 2.7 * r;
            mod.drift_hz_per_s = p.speed == decode::DominoSpeed::D4 ? 0.05 : 0.5;
            mod.lead_in_samples = 4000.0 + 11.0 * r;
            mod.tail_samples = 4000;
            auto audio = siggen::domino_render(mod, kText);
            REQUIRE(audio.has_value());
            REQUIRE(siggen::add_real_awgn(*audio, mean_power(*audio),
                                          siggen::NoiseLevel::snr_in_2500_hz_db(p.snr), kRate,
                                          200 + static_cast<std::uint64_t>(r))
                        .has_value());
            decode::DominoConfig cfg;
            cfg.speed = p.speed;
            total += cer(decode_text(cfg, *audio).primary);
        }
        const double mean = total / runs;
        std::printf("DominoEX speed %d at %.1f dB in 2500 Hz: CER %.4f over %zu characters x %d\n",
                    static_cast<int>(p.speed), p.snr, mean, kText.size(), runs);
        CHECK(mean <= p.limit);
    }
}
