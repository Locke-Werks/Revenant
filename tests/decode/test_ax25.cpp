// AX.25 over Bell 202: the frame against AX.25 2.2's own figures, the FCS
// against the TETRA X.25 check this tree already had, and the decoder against
// the transmitter.
//
// The figures and the TETRA cross-check are what can see a misreading shared
// by both ends of the round trip. The round trip then measures bit and frame
// error rates through add_real_awgn, reported in INFO and WARN lines with
// loose assertions, for the reason tests/decode/CMakeLists.txt gives.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "core/decode/ax25.h"
#include "core/decode/dv_codes.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/fsk_mod.h"
#include "core/dsp/synth/rds_mod.h"

using namespace revenant;

namespace {

decode::Ax25Address address(const char* call, std::uint8_t ssid, bool c) {
    decode::Ax25Address a;
    a.callsign = call;
    a.ssid = ssid;
    a.command_or_repeated = c;
    return a;
}

std::vector<std::uint8_t> bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

double mean_power(const std::vector<float>& x) {
    double sum = 0.0;
    for (const float v : x) {
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    return x.empty() ? 0.0 : sum / static_cast<double>(x.size());
}

std::vector<decode::Ax25Frame> decode_all(dsp::SampleRate rate, const std::vector<float>& audio,
                                          std::size_t block = 0,
                                          decode::Ax25Stats* stats = nullptr) {
    decode::Ax25Config config;
    config.rate = rate;
    auto decoder = decode::Ax25Decoder::create(config);
    REQUIRE(decoder.has_value());
    std::vector<decode::Ax25Frame> frames;
    if (block == 0) {
        decoder->process(audio, frames);
    } else {
        for (std::size_t i = 0; i < audio.size(); i += block) {
            const std::size_t n = std::min(block, audio.size() - i);
            decoder->process(std::span<const float>(audio.data() + i, n), frames);
        }
    }
    if (stats != nullptr) {
        *stats = decoder->stats();
    }
    return frames;
}

// A UI frame from N7LEM to APRS via WIDE1-1, carrying `info`.
std::vector<std::uint8_t> ui_frame(std::string_view info) {
    siggen::Ax25FrameSpec spec;
    spec.destination = address("APRS", 0, true);
    spec.source = address("N7LEM", 0, false);
    spec.repeaters.push_back(address("WIDE1", 1, false));
    spec.information = bytes(info);
    auto octets = siggen::ax25_frame_octets(spec);
    REQUIRE(octets.has_value());
    return *octets;
}

}  // namespace

TEST_CASE("the AX.25 FCS is the X.25 check TETRA's block code carries", "[decode][ax25]") {
    // EN 300 392-2 clause 8.2.3.3 and AX.25 2.2 clause 3.7 both name the
    // same ITU-T X.25 / ISO 3309 check. core/decode/dv_codes.cpp computes it
    // bit-serially, most significant bit of the register first, from the
    // TETRA text; core/decode/ax25.cpp computes it octet-wise with the
    // register reflected, from the Finnegan and Benson figure. Fed the same
    // bits in the order they go to air, the two must put the same sixteen
    // check bits on the air.
    std::mt19937_64 engine(0xFC5ULL);
    for (int trial = 0; trial < 50; ++trial) {
        std::vector<std::uint8_t> octets(1 + static_cast<std::size_t>(engine() % 300));
        for (auto& o : octets) {
            o = static_cast<std::uint8_t>(engine());
        }
        std::vector<std::uint8_t> bits;
        for (const std::uint8_t o : octets) {
            for (unsigned b = 0; b < 8; ++b) {
                bits.push_back(static_cast<std::uint8_t>((o >> b) & 1U));
            }
        }
        const std::vector<std::uint8_t> tetra = decode::tetra_block_code_parity(bits);
        const std::uint16_t fcs = decode::ax25_fcs(octets);
        for (unsigned i = 0; i < 16; ++i) {
            // Low octet first, each least significant bit first.
            CHECK(tetra[i] == ((fcs >> i) & 1U));
        }
    }
}

TEST_CASE("address encoding reproduces AX.25 2.2 Figures 3.4 and 3.7", "[decode][ax25]") {
    // Figure 3.4: an I frame from N7LEM to NJ7P, no repeater. A7 is 0xE0,
    // the C bit set on the destination; A14 is 0x61, C clear and the
    // extension bit set on the last octet.
    //
    // THE FIGURE MISPRINTS EVERY N. It gives A1 and A8, both "N", as
    // 10011000, 0x98, which is "L" (0x4C) shifted left; A10, which really is
    // "L", carries the same 0x98. "N" is 0x4E and shifted left is 0x9C,
    // which is what clause 3.12.2's rule, seven-bit ASCII in the left-most
    // seven bits, gives. Every other octet in Figures 3.4, 3.5, 3.7 and 3.8
    // agrees with that rule, so the rule is followed and A1 and A8 below
    // carry 0x9C where the figure prints 0x98.
    const std::uint8_t figure_3_4[] = {0x9C, 0x94, 0x6E, 0xA0, 0x40, 0x40, 0xE0,
                                       0x9C, 0x6E, 0x98, 0x8A, 0x9A, 0x40, 0x61,
                                       0x3E, 0xF0};
    siggen::Ax25FrameSpec spec;
    spec.destination = address("NJ7P", 0, true);
    spec.source = address("N7LEM", 0, false);
    spec.control = 0x3E;
    spec.pid = 0xF0;
    auto octets = siggen::ax25_frame_octets(spec);
    REQUIRE(octets.has_value());
    CHECK(*octets == std::vector<std::uint8_t>(std::begin(figure_3_4), std::end(figure_3_4)));

    auto frame = decode::ax25_parse(figure_3_4);
    REQUIRE(frame.has_value());
    CHECK(frame->destination.callsign == "NJ7P");
    CHECK(frame->destination.command_or_repeated);
    CHECK(frame->source.callsign == "N7LEM");
    CHECK_FALSE(frame->source.command_or_repeated);
    // "The P/F bit is set; the receive sequence number [N(R)] is 1; the send
    // sequence number [N(S)] is 7." Figure 4.1a positions.
    CHECK(frame->kind == decode::Ax25FrameKind::Information);
    CHECK(((frame->control >> 5U) & 7U) == 1U);
    CHECK((frame->control & decode::kAx25PollFinalBit) != 0U);
    CHECK(((frame->control >> 1U) & 7U) == 7U);
    REQUIRE(frame->pid.has_value());
    CHECK(*frame->pid == decode::kAx25PidNoLayer3);

    // Figure 3.7: repeater N7OO, SSID 1, H set, last subfield: HRRSSSS1 is
    // 0xE3.
    auto repeater = decode::ax25_encode_address(address("N7OO", 1, true), true);
    REQUIRE(repeater.has_value());
    // A15 is "N" and is misprinted 0x98 here too.
    const std::uint8_t figure_3_7[] = {0x9C, 0x6E, 0x9E, 0x9E, 0x40, 0x40, 0xE3};
    CHECK(std::equal(repeater->begin(), repeater->end(), std::begin(figure_3_7)));

    // Figure 3.8 prints the source SSID octet of a repeated frame as 0x61,
    // extension bit set. Clause 3.12.4 says the opposite in words: with a
    // repeater subfield present "the last octet of the source subfield has
    // its address extension bit set to 0". The text is followed here,
    // because with the bit set the repeater subfield could not be found,
    // and the figure is recorded as the document disagreeing with itself.
    spec.repeaters.push_back(address("N7OO", 1, true));
    octets = siggen::ax25_frame_octets(spec);
    REQUIRE(octets.has_value());
    CHECK((*octets)[13] == 0x60);
    CHECK((*octets)[20] == 0xE3);
}

TEST_CASE("AX.25 frames round trip through Bell 202 at two rates", "[decode][ax25]") {
    // Three frames back to back, one carrying octets that force bit stuffing
    // and one whose information holds the flag pattern itself.
    const std::vector<std::vector<std::uint8_t>> frames = {
        ui_frame("!4903.50N/07201.75W-Test 001234"),
        ui_frame(std::string("\x7E\x7E\xFF\xFF\xFE\x7F\x3F", 7)),
        ui_frame(">Net Control Center"),
    };
    for (const dsp::SampleRate rate : {dsp::SampleRate{48'000}, dsp::SampleRate{22'050}}) {
        INFO("rate " << rate);
        siggen::Ax25ModConfig mod;
        mod.rate = rate;
        auto audio = siggen::ax25_render(mod, frames);
        REQUIRE(audio.has_value());

        decode::Ax25Stats stats;
        const auto got = decode_all(rate, *audio, 0, &stats);
        INFO(stats.candidates << " candidates, " << stats.fcs_failures << " FCS failures");
        REQUIRE(got.size() == frames.size());
        for (std::size_t i = 0; i < frames.size(); ++i) {
            CHECK(got[i].octets == frames[i]);
            CHECK(got[i].is_ui);
            CHECK(got[i].destination.callsign == "APRS");
            CHECK(got[i].source.callsign == "N7LEM");
            REQUIRE(got[i].repeaters.size() == 1);
            CHECK(decode::ax25_address_text(got[i].repeaters[0]) == "WIDE1-1");
        }
        CHECK(got[0].first_sample < got[1].first_sample);

        // The first frame's first bit follows the leading flags. Its
        // reported sample lands within a bit of where the transmitter put it.
        const double samples_per_bit = static_cast<double>(rate) / decode::kBell202Baud;
        const double sent = 8.0 * static_cast<double>(mod.leading_flags) * samples_per_bit;
        CHECK(std::abs(static_cast<double>(got[0].first_sample) - sent) < samples_per_bit);
    }
}

TEST_CASE("AX.25 output does not depend on how the audio is blocked", "[decode][ax25]") {
    const std::vector<std::vector<std::uint8_t>> frames = {ui_frame(">block test one"),
                                                           ui_frame(">block test two")};
    auto audio = siggen::ax25_render(siggen::Ax25ModConfig{}, frames);
    REQUIRE(audio.has_value());
    const auto whole = decode_all(48'000, *audio);
    REQUIRE(whole.size() == 2);
    for (const std::size_t block : {std::size_t{1}, std::size_t{333}, std::size_t{8192}}) {
        INFO("block of " << block);
        const auto split = decode_all(48'000, *audio, block);
        REQUIRE(split.size() == whole.size());
        for (std::size_t i = 0; i < whole.size(); ++i) {
            CHECK(split[i].octets == whole[i].octets);
            CHECK(split[i].first_sample == whole[i].first_sample);
        }
    }
}

TEST_CASE("AX.25 decodes through clock error and emphasis tilt", "[decode][ax25]") {
    // Finnegan and Benson section 4: stations tilt the two tones by up to
    // 10 dB either way. And a sound card's clock is not the transmitter's.
    const std::vector<std::vector<std::uint8_t>> frames = {
        ui_frame("=4903.50N/07201.75W#PHG5132 a frame long enough to drift across"),
    };
    struct Case {
        double baud_error;
        double tilt_db;
    };
    const Case cases[] = {{0.005, 0.0}, {-0.005, 0.0}, {0.0, 6.0}, {0.0, -6.0}, {0.0, 10.0},
                          {0.0, -10.0}};
    for (const Case& c : cases) {
        INFO("clock error " << c.baud_error << ", space tone " << c.tilt_db << " dB");
        siggen::Ax25ModConfig mod;
        mod.baud_error = c.baud_error;
        mod.space_gain_db = c.tilt_db;
        auto audio = siggen::ax25_render(mod, frames);
        REQUIRE(audio.has_value());
        const auto got = decode_all(48'000, *audio);
        REQUIRE(got.size() == 1);
        CHECK(got[0].octets == frames[0]);
    }
}

TEST_CASE("AX.25 bit and frame error rates against noise, measured", "[decode][ax25]") {
    // Two measurements at each level. The bit error rate is taken on a long
    // random bit stream after NRZI decoding, aligned by searching for the
    // sent sequence, so it is the modem alone. The frame error rate is 60 UI
    // frames of 60 information octets each, with 16 flags between them so a
    // lost frame does not take its neighbour with it.
    struct Point {
        double snr_2500_db;
        double allowed_frame_error_rate;
    };
    // Measured 2026-09-22 at 48 kHz, frames seed 0xA25, noise seeds
    // 0xBADC0DE and 0x5EED:
    //
    //   SNR/2500 Hz   Eb/N0     bit error rate   frame error rate (60 frames)
    //   20 dB         23.2 dB   0                0
    //   10 dB         13.2 dB   0                0.05
    //    8 dB         11.2 dB   0.0024           0.57
    //    6 dB          9.2 dB   0.018            0.98
    //
    // The bit error rate is after NRZI, which turns one wrong tone decision
    // into two wrong bits, so it sits at twice the non-coherent FSK figure.
    // A frame here is 81 octets, about 650 bits, which is why the frame error
    // rate climbs so much faster. Allowances above the measurements.
    const Point points[] = {{20.0, 0.0}, {10.0, 0.2}, {8.0, 0.6}, {6.0, 1.0}};

    std::vector<std::vector<std::uint8_t>> frames;
    std::mt19937_64 engine(0xA25ULL);
    for (int i = 0; i < 60; ++i) {
        std::string info = ">";
        for (int k = 0; k < 59; ++k) {
            info.push_back(static_cast<char>(' ' + engine() % 94));
        }
        frames.push_back(ui_frame(info));
    }

    std::vector<std::uint8_t> random_bits(20'000);
    for (auto& b : random_bits) {
        b = static_cast<std::uint8_t>(engine() & 1U);
    }

    for (const Point& p : points) {
        auto eb_n0 = siggen::reference_bandwidth_to_eb_over_n0_db(p.snr_2500_db, 1200.0);
        REQUIRE(eb_n0.has_value());

        // Frames.
        siggen::Ax25ModConfig mod;
        mod.flags_between = 16;
        auto audio = siggen::ax25_render(mod, frames);
        REQUIRE(audio.has_value());
        const double power = mean_power(*audio);
        REQUIRE(siggen::add_real_awgn(*audio, power,
                                      siggen::NoiseLevel::snr_in_2500_hz_db(p.snr_2500_db),
                                      48'000, 0xBADC0DEULL)
                    .has_value());
        const auto got = decode_all(48'000, *audio);
        std::size_t good = 0;
        std::size_t next = 0;
        for (const auto& f : got) {
            for (std::size_t i = next; i < frames.size(); ++i) {
                if (f.octets == frames[i]) {
                    ++good;
                    next = i + 1;
                    break;
                }
            }
        }
        const double fer = 1.0 - static_cast<double>(good) / static_cast<double>(frames.size());

        // Raw bits, NRZI decoded by the same discriminator and clock the
        // decoder uses.
        auto raw = siggen::afsk_render_bits(mod, random_bits);
        REQUIRE(raw.has_value());
        REQUIRE(siggen::add_real_awgn(*raw, mean_power(*raw),
                                      siggen::NoiseLevel::snr_in_2500_hz_db(p.snr_2500_db),
                                      48'000, 0x5EEDULL)
                    .has_value());
        decode::ToneDiscriminatorConfig tones;
        tones.mark_hz = decode::kBell202MarkHz;
        tones.space_hz = decode::kBell202SpaceHz;
        tones.symbol_rate = decode::kBell202Baud;
        auto discriminator = decode::ToneDiscriminator::create(tones);
        REQUIRE(discriminator.has_value());
        decode::BitClockConfig clock_config;
        clock_config.symbol_rate = decode::kBell202Baud;
        auto clock = decode::BitClock::create(clock_config);
        REQUIRE(clock.has_value());
        std::vector<float> soft;
        std::vector<decode::SoftBit> soft_bits;
        discriminator->process(*raw, soft);
        clock->process(soft, soft_bits);
        std::vector<std::uint8_t> decoded;
        bool previous = true;
        for (const auto& b : soft_bits) {
            const bool level = b.value >= 0.0F;
            decoded.push_back(level == previous ? 1U : 0U);
            previous = level;
        }
        // Align on the first 64 sent bits after a settling allowance.
        constexpr std::size_t kSkip = 200;
        std::size_t best_shift = 0;
        std::size_t best_matches = 0;
        for (std::size_t shift = 0; shift < 64 && shift + kSkip + 64 < decoded.size(); ++shift) {
            std::size_t matches = 0;
            for (std::size_t i = 0; i < 64; ++i) {
                matches += (decoded[kSkip + shift + i] == random_bits[kSkip + i]) ? 1U : 0U;
            }
            if (matches > best_matches) {
                best_matches = matches;
                best_shift = shift;
            }
        }
        std::size_t errors = 0;
        std::size_t compared = 0;
        for (std::size_t i = kSkip; i + best_shift < decoded.size() && i < random_bits.size(); ++i) {
            errors += (decoded[i + best_shift] != random_bits[i]) ? 1U : 0U;
            ++compared;
        }
        const double ber = static_cast<double>(errors) / static_cast<double>(compared);

        INFO("SNR " << p.snr_2500_db << " dB in 2500 Hz, Eb/N0 " << *eb_n0 << " dB: bit error rate "
                    << ber << " over " << compared << " bits, frame error rate " << fer << " over "
                    << frames.size() << " frames");
        CHECK(compared > random_bits.size() / 2);
        CHECK(fer <= p.allowed_frame_error_rate);
        WARN("AX.25 SNR " << p.snr_2500_db << " dB/2500 Hz (Eb/N0 " << *eb_n0 << " dB): BER " << ber
                          << ", FER " << fer);
    }
}
