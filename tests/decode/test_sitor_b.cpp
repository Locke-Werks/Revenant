// SITOR-B: the 7-unit code against ITU-R M.625-4's tables and against ITU-T
// S.1, and the decoder against the transmitter, time diversity included.
//
// The code checks are what a round trip cannot see. M.625-4 Table 1 prints
// the ITA2 combination beside each 7-unit signal, and ITU-T S.1 Table 1 is
// the same alphabet from a different document, so the two are held against
// each other through core/decode/rtty.h's table.
//
// Error rates are measured and reported in WARN lines with loose assertions,
// for the reason tests/decode/CMakeLists.txt gives.

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/decode/rtty.h"
#include "core/decode/sitor_b.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/fsk_mod.h"
#include "core/dsp/synth/rds_mod.h"

using namespace revenant;

namespace {

double mean_power(const std::vector<float>& x) {
    double sum = 0.0;
    for (const float v : x) {
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    return x.empty() ? 0.0 : sum / static_cast<double>(x.size());
}

std::string utf8(std::u32string_view text) {
    std::string out;
    for (const char32_t c : text) {
        out.push_back(static_cast<char>(c));
    }
    return out;
}

// Printing characters only, mutilated ones as '#'.
std::string text_of(const std::vector<decode::SitorCharacter>& chars) {
    std::string out;
    for (const auto& c : chars) {
        if (c.mutilated) {
            out.push_back('#');
        } else if (c.glyph != 0) {
            out.push_back(static_cast<char>(c.glyph));
        }
    }
    return out;
}

std::vector<decode::SitorCharacter> decode_all(const decode::SitorConfig& config,
                                               const std::vector<float>& audio,
                                               decode::SitorStats* stats = nullptr) {
    auto decoder = decode::SitorBDecoder::create(config);
    REQUIRE(decoder.has_value());
    std::vector<decode::SitorCharacter> out;
    decoder->process(audio, out);
    if (stats != nullptr) {
        *stats = decoder->stats();
    }
    return out;
}

// M.625-4 Table 1, letters case, as the table prints the 7-unit column.
struct Printed {
    char32_t letter;
    const char* signal;
};
constexpr Printed kLetters[26] = {
    {U'A', "BBBYYYB"}, {U'B', "YBYYBBB"}, {U'C', "BYBBBYY"}, {U'D', "BBYYBYB"}, {U'E', "YBBYBYB"},
    {U'F', "BBYBBYY"}, {U'G', "BYBYBBY"}, {U'H', "BYYBYBB"}, {U'I', "BYBBYYB"}, {U'J', "BBBYBYY"},
    {U'K', "YBBBBYY"}, {U'L', "BYBYYBB"}, {U'M', "BYYBBBY"}, {U'N', "BYYBBYB"}, {U'O', "BYYYBBB"},
    {U'P', "BYBBYBY"}, {U'Q', "YBBBYBY"}, {U'R', "BYBYBYB"}, {U'S', "BBYBYYB"}, {U'T', "YYBYBBB"},
    {U'U', "YBBBYYB"}, {U'V', "YYBBBBY"}, {U'W', "BBBYYBY"}, {U'X', "YBYBBBY"}, {U'Y', "BBYBYBY"},
    {U'Z', "BBYYYBB"},
};

const std::u32string kText = U"CQ CQ DE SITOR THE QUICK BROWN FOX 0123456789 -?:().,'=/+\r\n";

}  // namespace

TEST_CASE("M.625-4 Table 1 carries ITU-T S.1's alphabet", "[decode][sitor]") {
    // Each printed 7-unit signal decodes to the combination whose S.1 letter
    // is the letter M.625-4 prints beside it.
    for (const Printed& p : kLetters) {
        INFO("letter " << static_cast<char>(p.letter) << ", signal " << p.signal);
        const auto s = decode::sitor_classify(decode::sitor_signal(p.signal));
        REQUIRE(s.kind == decode::SitorSignal::Kind::Traffic);
        CHECK(decode::ita2_letter(s.combination) == p.letter);
    }
    // And every one of the 32 combinations maps to a signal and back.
    for (std::uint8_t c = 0; c < 32; ++c) {
        const std::uint8_t signal = decode::sitor_encode(c);
        CHECK(std::popcount(signal) == 3);
        const auto s = decode::sitor_classify(signal);
        REQUIRE(s.kind == decode::SitorSignal::Kind::Traffic);
        CHECK(s.combination == c);
    }
}

TEST_CASE("the constant ratio code uses all 35 three-of-seven patterns", "[decode][sitor]") {
    // Clause 1.1 and Tables 1 and 2: 32 traffic signals and the three mode B
    // service signals are 35, which is exactly C(7,3). So every pattern of
    // weight three means something and every other weight is mutilated,
    // which is why a single bit error is always detected.
    std::set<std::uint8_t> used;
    for (std::uint8_t c = 0; c < 32; ++c) {
        used.insert(decode::sitor_encode(c));
    }
    used.insert(decode::kSitorPhasing1);
    used.insert(decode::kSitorPhasing2);
    used.insert(decode::kSitorIdleBeta);
    CHECK(used.size() == 35);
    int weight_three = 0;
    for (unsigned v = 0; v < 128; ++v) {
        const auto s = decode::sitor_classify(static_cast<std::uint8_t>(v));
        if (std::popcount(v) == 3) {
            ++weight_three;
            CHECK(s.kind != decode::SitorSignal::Kind::Mutilated);
        } else {
            CHECK(s.kind == decode::SitorSignal::Kind::Mutilated);
        }
    }
    CHECK(weight_three == 35);
    CHECK(decode::sitor_classify(decode::sitor_signal("BBBBYYY")).kind ==
          decode::SitorSignal::Kind::Phasing1OrAlpha);
    CHECK(decode::sitor_classify(decode::sitor_signal("YBBYYBB")).kind ==
          decode::SitorSignal::Kind::Phasing2);
    CHECK(decode::sitor_classify(decode::sitor_signal("BBYYBBY")).kind ==
          decode::SitorSignal::Kind::IdleBeta);
}

TEST_CASE("SITOR-B round trips at two rates and on both sidebands", "[decode][sitor]") {
    auto codes = siggen::ita2_encode_text(kText);
    REQUIRE(codes.has_value());
    for (const dsp::SampleRate rate : {dsp::SampleRate{48'000}, dsp::SampleRate{11'025}}) {
        for (const bool upper : {true, false}) {
            INFO("rate " << rate << ", " << (upper ? "upper" : "lower") << " sideband sent");
            siggen::SitorModConfig mod;
            mod.rate = rate;
            mod.upper_sideband = upper;
            auto audio = siggen::sitor_b_render(mod, *codes);
            REQUIRE(audio.has_value());
            // The decoder assumes upper sideband; the lower sideband case
            // is found by the complemented phasing signals.
            decode::SitorConfig config;
            config.rate = rate;
            decode::SitorStats stats;
            const auto got = decode_all(config, *audio, &stats);
            // Clause 4.6.1 puts CR LF first, which is also what starts
            // printing under clause 4.6.4.
            CHECK(text_of(got) == "\r\n" + utf8(kText));
            CHECK(stats.phasings == 1);
            CHECK(stats.both_mutilated == 0);
            CHECK(stats.ends_of_transmission == 1);
        }
    }
}

TEST_CASE("time diversity recovers what one copy loses", "[decode][sitor]") {
    // Damage signals on the air directly: flip one unit of the DX copy of
    // some characters, of the RX copy of others, and of both copies of one.
    auto codes = siggen::ita2_encode_text(U"ABCDEFGHIJKLMNOP");
    REQUIRE(codes.has_value());
    siggen::SitorModConfig mod;
    auto slots = siggen::sitor_b_signals(mod, *codes);
    // DX slot of traffic character i (counting the leading CR LF as 0 and 1)
    // is 2 * (phasing_pairs + i); its RX copy is five slots later.
    const auto dx_slot = [&](std::size_t i) { return 2 * (mod.phasing_pairs + i); };
    // Character 2 is the letter shift, so "A" is 3 and "C" is 5.
    slots[dx_slot(5)] ^= 0x01U;      // "C": DX damaged
    slots[dx_slot(7) + 5] ^= 0x10U;  // "E": RX damaged
    slots[dx_slot(9)] ^= 0x02U;      // "G": both damaged
    slots[dx_slot(9) + 5] ^= 0x04U;
    auto audio = siggen::sitor_b_render_signals(mod, slots);
    REQUIRE(audio.has_value());

    decode::SitorStats stats;
    const auto got = decode_all(decode::SitorConfig{}, *audio, &stats);
    // Clause 4.6.5: a character lost in both copies prints as the error
    // character.
    CHECK(text_of(got) == "\r\nABCDEF#HIJKLMNOP");
    CHECK(stats.dx_mutilated == 2);
    CHECK(stats.rx_mutilated == 2);
    CHECK(stats.both_mutilated == 1);
    std::size_t from_rx = 0;
    for (const auto& c : got) {
        from_rx += c.from_rx ? 1U : 0U;
    }
    CHECK(from_rx == 1);
}

TEST_CASE("flush decides the characters whose RX copy never came from their DX copy",
          "[decode][sitor]") {
    // The stream stops on the RX slot after the DX copy of the last
    // character, which carries the RX copy of "F", so the RX copies of "G"
    // and "H" are never sent. Clause 4.3 with one copy: take it if it
    // checks. "G"'s is damaged, so it is mutilated rather than guessed.
    auto codes = siggen::ita2_encode_text(U"ABCDEFGH");
    REQUIRE(codes.has_value());
    siggen::SitorModConfig mod;
    auto slots = siggen::sitor_b_signals(mod, *codes);
    // CR, LF and the letter shift come first, so "H" is character 10.
    const auto dx_slot = [&](std::size_t i) { return 2 * (mod.phasing_pairs + i); };
    slots.resize(dx_slot(10) + 2);
    slots[dx_slot(9)] ^= 0x01U;  // "G": its only copy damaged
    auto audio = siggen::sitor_b_render_signals(mod, slots);
    REQUIRE(audio.has_value());
    // Half a signal of silence, so the discriminator's delay does not cut
    // into the last whole signal.
    audio->insert(audio->end(), 1680, 0.0F);

    auto decoder = decode::SitorBDecoder::create(decode::SitorConfig{});
    REQUIRE(decoder.has_value());
    std::vector<decode::SitorCharacter> got;
    decoder->process(*audio, got);
    INFO("before flush: \"" << text_of(got) << "\"");
    CHECK(text_of(got) == "\r\nABCDEF");
    for (const auto& c : got) {
        CHECK_FALSE(c.single_copy);
    }

    const std::size_t decided = got.size();
    const std::uint64_t dx_lost = decoder->stats().dx_mutilated;
    decoder->flush(got);
    INFO("after flush: \"" << text_of(got) << "\"");
    CHECK(text_of(got) == "\r\nABCDEF#H");
    REQUIRE(got.size() == decided + 2);
    for (std::size_t i = decided; i < got.size(); ++i) {
        CHECK(got[i].single_copy);
        CHECK_FALSE(got[i].from_rx);
        CHECK(got[i].position > got[i - 1].position);
    }
    CHECK(got[decided].mutilated);
    CHECK_FALSE(got[decided + 1].mutilated);
    CHECK(decoder->stats().dx_mutilated == dx_lost + 1);
    CHECK_FALSE(decoder->phased());

    // Stand-by afterwards, so nothing is handed over twice.
    decoder->flush(got);
    CHECK(got.size() == decided + 2);
}

TEST_CASE("flush after a transmission that ended on its idle signals appends nothing",
          "[decode][sitor]") {
    auto codes = siggen::ita2_encode_text(kText);
    REQUIRE(codes.has_value());
    auto audio = siggen::sitor_b_render(siggen::SitorModConfig{}, *codes);
    REQUIRE(audio.has_value());
    auto decoder = decode::SitorBDecoder::create(decode::SitorConfig{});
    REQUIRE(decoder.has_value());
    std::vector<decode::SitorCharacter> got;
    decoder->process(*audio, got);
    const std::size_t decided = got.size();
    decoder->flush(got);
    CHECK(got.size() == decided);
    CHECK(text_of(got) == "\r\n" + utf8(kText));
}

TEST_CASE("a noise burst never prints a wrong character", "[decode][sitor]") {
    // Clause 4.2 spaces the copies 280 ms apart, so a burst shorter than
    // that can take at most one copy of any character, and the other copy
    // carries it. 150 ms of loud noise over the middle of the traffic.
    //
    // What that does not promise is that nothing is lost. The code detects
    // a change of weight and nothing else, so noise that swaps one Y for one
    // B leaves a valid signal that is the wrong one. Clause 4.3 then sees two
    // valid copies that differ and calls both mutilated rather than guess.
    // Measured with this seed: two characters hit in the DX copy, one
    // recovered from its RX copy and one lost that way. What this case holds
    // is that a character printed is always the character sent.
    auto codes = siggen::ita2_encode_text(kText);
    REQUIRE(codes.has_value());
    auto audio = siggen::sitor_b_render(siggen::SitorModConfig{}, *codes);
    REQUIRE(audio.has_value());
    const std::size_t start = audio->size() / 2;
    std::mt19937_64 engine(0xB0257ULL);
    std::normal_distribution<float> noise(0.0F, 3.0F);
    for (std::size_t i = start; i < start + 7'200; ++i) {
        (*audio)[i] = noise(engine);
    }
    decode::SitorStats stats;
    const auto got = decode_all(decode::SitorConfig{}, *audio, &stats);
    const std::string received = text_of(got);
    const std::string sent = "\r\n" + utf8(kText);
    INFO("received \"" << received << "\"; DX lost " << stats.dx_mutilated << ", RX lost "
                       << stats.rx_mutilated << ", both " << stats.both_mutilated);
    REQUIRE(received.size() == sent.size());
    std::size_t lost = 0;
    for (std::size_t i = 0; i < sent.size(); ++i) {
        if (received[i] == '#') {
            ++lost;
        } else {
            CHECK(received[i] == sent[i]);
        }
    }
    CHECK(lost <= 2);
    CHECK(stats.dx_mutilated + stats.rx_mutilated + stats.both_mutilated > 0);
}

TEST_CASE("SITOR-B character error rate against noise, measured", "[decode][sitor]") {
    // Random text through the transmitter, add_real_awgn and the decoder.
    // Two figures at each level: the fraction of DX copies mutilated, which
    // is what a receiver without time diversity would lose, and the fraction
    // of characters lost in both copies, which is what this one loses.
    std::mt19937_64 engine(0x5170EULL);
    constexpr char32_t kPool[] = U"ABCDEFGHIJKLMNOPQRSTUVWXYZ    0123456789.,";
    std::u32string text;
    for (int i = 0; i < 400; ++i) {
        text.push_back(kPool[engine() % (std::size(kPool) - 1)]);
    }
    auto codes = siggen::ita2_encode_text(text);
    REQUIRE(codes.has_value());

    struct Point {
        double snr_2500_db;
        double allowed_loss;
    };
    // Measured 2026-09-22 at 48 kHz, text seed 0x5170E, noise seed 0xD1CE,
    // 562 characters:
    //
    //   SNR/2500 Hz  Eb/N0    DX copies lost  lost in both copies
    //   10 dB        24.0 dB  0               0
    //   -2 dB        12.0 dB  0.0053          0
    //   -5 dB         9.0 dB  0.089           0.012
    //
    // At -5 dB time diversity turns one character in eleven lost into one
    // in eighty. The allowances sit above the measurements.
    const Point points[] = {{10.0, 0.0}, {-2.0, 0.01}, {-5.0, 0.1}};
    for (const Point& p : points) {
        auto audio = siggen::sitor_b_render(siggen::SitorModConfig{}, *codes);
        REQUIRE(audio.has_value());
        REQUIRE(siggen::add_real_awgn(*audio, mean_power(*audio),
                                      siggen::NoiseLevel::snr_in_2500_hz_db(p.snr_2500_db), 48'000,
                                      0xD1CEULL)
                    .has_value());
        auto eb_n0 =
            siggen::reference_bandwidth_to_eb_over_n0_db(p.snr_2500_db, decode::kSitorBaud);
        REQUIRE(eb_n0.has_value());
        decode::SitorStats stats;
        const auto got = decode_all(decode::SitorConfig{}, *audio, &stats);
        const double decided = static_cast<double>(stats.characters);
        const double dx_loss =
            decided > 0 ? static_cast<double>(stats.dx_mutilated) / decided : 1.0;
        const double both_loss =
            decided > 0 ? static_cast<double>(stats.both_mutilated) / decided : 1.0;
        INFO("SNR " << p.snr_2500_db << " dB in 2500 Hz, Eb/N0 " << *eb_n0 << " dB: " << decided
                    << " characters decided of " << codes->size() + 2 << ", DX copies lost "
                    << dx_loss << ", lost in both " << both_loss << ", phasings " << stats.phasings
                    << ", losses of phase " << stats.losses_of_phase);
        CHECK(both_loss <= p.allowed_loss);
        WARN("SITOR-B SNR " << p.snr_2500_db << " dB/2500 Hz (Eb/N0 " << *eb_n0
                            << " dB): " << decided << " decided, DX lost " << dx_loss
                            << ", both lost " << both_loss);
    }
}
