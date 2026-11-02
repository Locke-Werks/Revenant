// SITOR-A monitor: Table 2's mode A column as printed, and the monitor
// against core/dsp/synth/sitor_a_mod.h's two-station link, repetitions
// included.
//
// Error rates are measured and reported in WARN lines with loose assertions,
// for the reason tests/decode/CMakeLists.txt gives.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/decode/sitor_a.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/fsk_mod.h"
#include "core/dsp/synth/rds_mod.h"
#include "core/dsp/synth/sitor_a_mod.h"

using namespace revenant;

namespace {

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

decode::SitorAOutput monitor(const decode::SitorAConfig& config, const std::vector<float>& audio,
                             std::size_t block = 0, decode::SitorAStats* stats = nullptr) {
    auto m = decode::SitorAMonitor::create(config);
    REQUIRE(m.has_value());
    decode::SitorAOutput out;
    const std::size_t step = block == 0 ? audio.size() : block;
    for (std::size_t i = 0; i < audio.size(); i += step) {
        m->process(std::span<const float>(audio.data() + i, std::min(step, audio.size() - i)), out);
    }
    if (stats != nullptr) {
        *stats = m->stats();
    }
    return out;
}

const std::u32string kText = U"CQ CQ DE AMTOR RYRYRY THE QUICK BROWN FOX 0123456789 -?:().,'=/+\r\n";

std::vector<std::array<std::uint8_t, 3>> blocks_of(std::u32string_view text) {
    auto codes = siggen::ita2_encode_text(text);
    REQUIRE(codes.has_value());
    return siggen::sitor_a_blocks(*codes);
}

}  // namespace

TEST_CASE("M.625-4 Table 2's mode A column as printed", "[decode][sitor_a]") {
    CHECK(decode::kSitorACs1 == decode::sitor_signal("BYBYYBB"));
    CHECK(decode::kSitorACs2 == decode::sitor_signal("YBYBYBB"));
    CHECK(decode::kSitorACs3 == decode::sitor_signal("BYYBBYB"));
    CHECK(decode::kSitorACs4 == decode::sitor_signal("BYBYBBY"));
    CHECK(decode::kSitorACs5 == decode::sitor_signal("BYYBYBB"));
    CHECK(decode::kSitorAIdleBeta == decode::sitor_signal("BBYYBBY"));
    CHECK(decode::kSitorAIdleAlpha == decode::sitor_signal("BBBBYYY"));
    CHECK(decode::kSitorARq == decode::sitor_signal("YBBYYBB"));
    // The three idle and repetition signals are the mode B ones sitor_b.h
    // already holds, and every control signal keeps the constant ratio.
    CHECK(decode::kSitorAIdleAlpha == decode::kSitorPhasing1);
    CHECK(decode::kSitorARq == decode::kSitorPhasing2);
    CHECK(decode::kSitorAIdleBeta == decode::kSitorIdleBeta);
    for (const auto s : {decode::kSitorACs1, decode::kSitorACs2, decode::kSitorACs3,
                         decode::kSitorACs4, decode::kSitorACs5}) {
        CHECK(std::popcount(s) == 3);
    }
}

TEST_CASE("SITOR-A monitor prints the link's text once, on both sidebands and two rates",
          "[decode][sitor_a]") {
    const auto blocks = blocks_of(kText);
    for (const bool usb : {true, false}) {
        for (const dsp::SampleRate rate : {48000, 11025}) {
            INFO("rate " << rate << (usb ? " USB" : " LSB"));
            siggen::SitorAModConfig mod;
            mod.rate = rate;
            mod.upper_sideband = usb;
            auto link = siggen::sitor_a_render(mod, blocks);
            REQUIRE(link.has_value());
            decode::SitorAConfig config;
            config.rate = rate;
            config.upper_sideband = usb;
            decode::SitorAStats stats;
            const auto out = monitor(config, link->audio, 0, &stats);
            CHECK(text_of(out.characters) == "CQ CQ DE AMTOR RYRYRY THE QUICK BROWN FOX 0123456789 -?:().,'=/+\r\n");
            CHECK(stats.acquisitions == 1);
            REQUIRE_FALSE(out.blocks.empty());
            CHECK(out.blocks.back().kind == decode::SitorABlock::Kind::IdleAlpha);
        }
    }
}

TEST_CASE("a block the IRS asks for again is printed once", "[decode][sitor_a]") {
    const auto blocks = blocks_of(kText);
    siggen::SitorAModConfig mod;
    mod.lost_at_irs = {2, 3, 9};
    auto link = siggen::sitor_a_render(mod, blocks);
    REQUIRE(link.has_value());
    CHECK(link->blocks_sent.size() == blocks.size() + 1 + 3);
    decode::SitorAStats stats;
    const auto out = monitor(decode::SitorAConfig{}, link->audio, 0, &stats);
    CHECK(text_of(out.characters) == "CQ CQ DE AMTOR RYRYRY THE QUICK BROWN FOX 0123456789 -?:().,'=/+\r\n");
    CHECK(stats.repeats_suppressed == 3);
    std::size_t by_cs = 0;
    for (const auto& b : out.blocks) {
        by_cs += b.accepted_by == decode::SitorABlock::Accepted::ByControlSignals ? 1U : 0U;
    }
    CHECK(by_cs == blocks.size());
}

TEST_CASE("SITOR-A monitor output does not depend on how the audio is blocked",
          "[decode][sitor_a]") {
    siggen::SitorAModConfig mod;
    mod.lost_at_irs = {4};
    auto link = siggen::sitor_a_render(mod, blocks_of(kText));
    REQUIRE(link.has_value());
    const auto whole = monitor(decode::SitorAConfig{}, link->audio);
    for (const std::size_t block : {std::size_t{1}, std::size_t{511}, std::size_t{48000}}) {
        INFO("block of " << block);
        const auto split = monitor(decode::SitorAConfig{}, link->audio, block);
        REQUIRE(split.blocks.size() == whole.blocks.size());
        REQUIRE(split.characters.size() == whole.characters.size());
        for (std::size_t i = 0; i < whole.blocks.size(); ++i) {
            CHECK(split.blocks[i].position == whole.blocks[i].position);
            CHECK(split.blocks[i].signals == whole.blocks[i].signals);
        }
    }
}

TEST_CASE("SITOR-A monitor character loss against noise, measured", "[decode][sitor_a]") {
    std::mt19937_64 engine(0x5A1ULL);
    constexpr char32_t kPool[] = U"ABCDEFGHIJKLMNOPQRSTUVWXYZ    ";
    std::u32string text;
    for (int i = 0; i < 300; ++i) {
        text.push_back(kPool[engine() % (std::size(kPool) - 1)]);
    }
    const auto blocks = blocks_of(text);
    const double mod_iss_amplitude = siggen::SitorAModConfig{}.iss_amplitude;
    struct Point {
        double snr_2500_db;
        double allowed_loss;
    };
    // Measured 2026-11-02 at 48 kHz, noise seed 0x5A1E, 301 characters, SNR
    // against the ISS burst's power:
    //
    //   SNR/2500 Hz   characters lost   blocks mutilated
    //   40, 10, 0 dB  0                 0
    //   -4 dB         0.020             1 (acquired one block late)
    //   -6 dB         0.26              8
    //   -8 dB         1                 never acquired
    //
    // The allowances sit above the measurements.
    const Point points[] = {{40.0, 0.0}, {10.0, 0.0}, {0.0, 0.0}, {-4.0, 0.05}, {-6.0, 0.4}, {-8.0, 1.0}};
    for (const Point& p : points) {
        auto link = siggen::sitor_a_render(siggen::SitorAModConfig{}, blocks);
        REQUIRE(link.has_value());
        // The reference is the ISS burst's power, not the mean over the pauses.
        const double iss_power = 0.5 * mod_iss_amplitude * mod_iss_amplitude;
        REQUIRE(siggen::add_real_awgn(link->audio, iss_power,
                                      siggen::NoiseLevel::snr_in_2500_hz_db(p.snr_2500_db), 48'000,
                                      0x5A1EULL)
                    .has_value());
        decode::SitorAStats stats;
        const auto out = monitor(decode::SitorAConfig{}, link->audio, 0, &stats);
        std::size_t lost = 0;
        for (const auto& c : out.characters) {
            lost += c.mutilated ? 1U : 0U;
        }
        // The text and the letter shift ita2_encode_text puts first.
        const double expected = static_cast<double>(text.size() + 1);
        const double missing = std::max(0.0, expected - static_cast<double>(out.characters.size()));
        const double loss = (static_cast<double>(lost) + missing) / expected;
        CHECK(loss <= p.allowed_loss);
        CHECK(stats.acquisitions <= 2);
        WARN("SITOR-A monitor SNR " << p.snr_2500_db << " dB/2500 Hz: " << out.characters.size()
                                    << " characters of " << text.size() + 1 << ", " << lost
                                    << " mutilated, loss " << loss << "; acquisitions "
                                    << stats.acquisitions << ", losses " << stats.losses
                                    << ", mutilated blocks " << stats.mutilated_blocks);
    }
}

TEST_CASE("noise alone acquires no SITOR-A link, measured", "[decode][sitor_a]") {
    auto m = decode::SitorAMonitor::create(decode::SitorAConfig{});
    REQUIRE(m.has_value());
    decode::SitorAOutput out;
    constexpr std::size_t kSeconds = 120;
    std::mt19937_64 engine(0x5A10ULL);
    std::normal_distribution<float> gauss(0.0F, 0.3F);
    std::vector<float> audio(48000);
    for (std::size_t s = 0; s < kSeconds; ++s) {
        for (float& v : audio) {
            v = gauss(engine);
        }
        m->process(audio, out);
    }
    WARN("SITOR-A on noise: " << m->stats().acquisitions << " acquisitions in " << kSeconds
                              << " s, " << out.characters.size() << " characters printed");
    CHECK(m->stats().acquisitions <= 1);
}
