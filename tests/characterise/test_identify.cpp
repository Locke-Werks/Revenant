// Protocol identification on one probe's dwell of each protocol, and on noise.
//
// WHAT THIS FILE IS THE RECORD OF
//
// core/identify/identify.h claims a protocol only on a verified sync, counted
// from the decoder core/decode already runs. Each case below renders two
// seconds of one protocol from core/dsp/synth's transmitter for it, at the rate
// the probe pool would pick for its width, centred at DC as a probe hands it
// over, and puts it in noise. Then the same rows run on noise alone, which is
// the case the rule exists for: a family and a width make a row plausible, and
// nothing on noise may verify.
//
// The audio-domain transmitters make real audio at their own tone, so the
// helpers here move it to DC the way a probe centred on the signal would see
// it, and FM-modulate the two that ride an FM carrier.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <print>
#include <string>
#include <vector>

#include "core/characterise/catalogue.h"
#include "core/decode/ax25.h"
#include "core/decode/pocsag.h"
#include "core/decode/rtty.h"
#include "core/decode/tetra.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/cw_mod.h"
#include "core/dsp/synth/dv_mod.h"
#include "core/dsp/synth/fsk_mod.h"
#include "core/dsp/synth/m17_mod.h"
#include "core/dsp/synth/psk31_mod.h"
#include "core/identify/identify.h"
#include "tests/characterise/signal_lab.h"

using namespace revenant;
using characterise::ModulationFamily;
using identify::Protocol;

namespace {

constexpr std::uint64_t kSeed = 20260923;

// The SNR every protocol is rendered at, in the 2500 Hz reference bandwidth.
// Chosen to sit well above every decoder's measured crossing in
// docs/sensitivity.md, because this file tests the counting and the
// plausibility table, not the decoders' thresholds.
constexpr double kSnrDb = 20.0;

// A narrow detection's dwell, core/engine/probe.h's kProbeIdentifyDwellSeconds:
// the four HF modes are rendered at the length a probe collects for them.
constexpr std::size_t kNarrowSeconds = 5;

[[nodiscard]] std::vector<dsp::Complex32> repeat_to(std::vector<dsp::Complex32> once,
                                                    std::size_t count) {
    REQUIRE_FALSE(once.empty());
    std::vector<dsp::Complex32> out;
    out.reserve(count);
    while (out.size() < count) {
        const std::size_t take = std::min(once.size(), count - out.size());
        out.insert(out.end(), once.begin(), once.begin() + static_cast<std::ptrdiff_t>(take));
    }
    return out;
}

[[nodiscard]] std::vector<dsp::Complex32> in_noise(std::vector<dsp::Complex32> clean,
                                                   dsp::SampleRate rate, double snr_db,
                                                   std::uint64_t seed) {
    auto added = siggen::add_awgn(dsp::ComplexSpan(clean),
                                  siggen::NoiseLevel::snr_in_2500_hz_db(snr_db), rate, seed);
    REQUIRE(added.has_value());
    return clean;
}

// A tone at `centre_hz` in an analytic signal, moved to DC.
[[nodiscard]] std::vector<dsp::Complex32> analytic_to_dc(std::vector<dsp::Complex32> analytic,
                                                         dsp::SampleRate rate, double centre_hz) {
    const double step = -2.0 * std::numbers::pi * centre_hz / static_cast<double>(rate);
    for (std::size_t n = 0; n < analytic.size(); ++n) {
        const double phase = std::fmod(step * static_cast<double>(n), 2.0 * std::numbers::pi);
        const std::complex<double> moved =
            std::complex<double>(analytic[n].real(), analytic[n].imag()) * std::polar(1.0, phase);
        analytic[n] = dsp::Complex32(static_cast<float>(moved.real()),
                                     static_cast<float>(moved.imag()));
    }
    return analytic;
}

// Real audio with its signal near `centre_hz`, moved to DC, and the image the
// real part leaves at twice the centre taken off with a windowed sinc.
[[nodiscard]] std::vector<dsp::Complex32> audio_to_dc(const std::vector<float>& audio,
                                                      dsp::SampleRate rate, double centre_hz,
                                                      double cutoff_hz) {
    std::vector<dsp::Complex32> mixed(audio.size());
    const double step = -2.0 * std::numbers::pi * centre_hz / static_cast<double>(rate);
    for (std::size_t n = 0; n < audio.size(); ++n) {
        const double phase = std::fmod(step * static_cast<double>(n), 2.0 * std::numbers::pi);
        mixed[n] = dsp::Complex32(static_cast<float>(2.0 * audio[n] * std::cos(phase)),
                                  static_cast<float>(2.0 * audio[n] * std::sin(phase)));
    }
    constexpr int kHalf = 128;
    const double normalised = cutoff_hz / static_cast<double>(rate);
    std::vector<double> taps(2 * kHalf + 1);
    double sum = 0.0;
    for (int k = -kHalf; k <= kHalf; ++k) {
        const double x = 2.0 * normalised * static_cast<double>(k);
        const double sinc = k == 0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
        const double window = 0.54 + 0.46 * std::cos(std::numbers::pi * k / kHalf);
        taps[static_cast<std::size_t>(k + kHalf)] = sinc * window;
        sum += sinc * window;
    }
    std::vector<dsp::Complex32> out(mixed.size());
    for (std::size_t n = 0; n < mixed.size(); ++n) {
        std::complex<double> acc(0.0, 0.0);
        for (int k = -kHalf; k <= kHalf; ++k) {
            const auto j = static_cast<std::ptrdiff_t>(n) + k;
            if (j < 0 || j >= static_cast<std::ptrdiff_t>(mixed.size())) {
                continue;
            }
            const dsp::Complex32 x = mixed[static_cast<std::size_t>(j)];
            acc += taps[static_cast<std::size_t>(k + kHalf)] / sum *
                   std::complex<double>(x.real(), x.imag());
        }
        out[n] = dsp::Complex32(static_cast<float>(acc.real()), static_cast<float>(acc.imag()));
    }
    return out;
}

// Up by four and down by three through one windowed sinc at the lower
// Nyquist, for the TETRA stream: 72000 to 96000 S/s.
[[nodiscard]] std::vector<dsp::Complex32> resample_four_thirds(
    const std::vector<dsp::Complex32>& in) {
    constexpr int kUp = 4;
    constexpr int kDown = 3;
    constexpr int kHalf = 16 * kUp;
    // Cutoff at the input's Nyquist, as a fraction of the upsampled rate.
    constexpr double kCutoff = 0.5 / kUp;
    std::vector<double> taps(2 * kHalf + 1);
    for (int k = -kHalf; k <= kHalf; ++k) {
        const double x = 2.0 * kCutoff * static_cast<double>(k);
        const double sinc = k == 0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
        const double window = 0.54 + 0.46 * std::cos(std::numbers::pi * k / kHalf);
        taps[static_cast<std::size_t>(k + kHalf)] = 2.0 * kCutoff * kUp * sinc * window;
    }
    const std::size_t out_count = in.size() * kUp / kDown;
    std::vector<dsp::Complex32> out(out_count);
    for (std::size_t m = 0; m < out_count; ++m) {
        // Output m sits at upsampled index m * kDown; sum the input samples
        // whose upsampled positions fall under the filter.
        const auto centre = static_cast<std::ptrdiff_t>(m * kDown);
        std::complex<double> acc(0.0, 0.0);
        for (std::ptrdiff_t n = (centre - kHalf + kUp - 1) / kUp; n * kUp <= centre + kHalf; ++n) {
            if (n < 0 || n >= static_cast<std::ptrdiff_t>(in.size())) {
                continue;
            }
            const std::ptrdiff_t k = centre - n * kUp;
            if (k < -kHalf || k > kHalf) {
                continue;
            }
            const dsp::Complex32 x = in[static_cast<std::size_t>(n)];
            acc += taps[static_cast<std::size_t>(k + kHalf)] *
                   std::complex<double>(x.real(), x.imag());
        }
        out[m] = dsp::Complex32(static_cast<float>(acc.real()), static_cast<float>(acc.imag()));
    }
    return out;
}

// Audio frequency-modulated onto a carrier at DC, `deviation_hz` per unit.
[[nodiscard]] std::vector<dsp::Complex32> fm_modulate(const std::vector<float>& audio,
                                                      dsp::SampleRate rate, double deviation_hz) {
    std::vector<dsp::Complex32> out(audio.size());
    double phase = 0.0;
    const double scale = 2.0 * std::numbers::pi * deviation_hz / static_cast<double>(rate);
    for (std::size_t n = 0; n < audio.size(); ++n) {
        phase = std::fmod(phase + scale * static_cast<double>(audio[n]), 2.0 * std::numbers::pi);
        out[n] = dsp::Complex32(static_cast<float>(std::cos(phase)),
                                static_cast<float>(std::sin(phase)));
    }
    return out;
}

[[nodiscard]] identify::Identification run(const std::vector<dsp::Complex32>& samples,
                                           dsp::SampleRate rate, double occupied_hz,
                                           ModulationFamily family = ModulationFamily::Unknown) {
    identify::IdentifyHints hints;
    hints.family = family;
    hints.occupied_hz = occupied_hz;
    identify::IdentifyConfig config;
    config.rate = rate;
    auto result = identify::identify(dsp::ConstComplexSpan(samples), hints, config);
    REQUIRE(result.has_value());
    return *result;
}

void print_attempts(const char* label, const identify::Identification& result) {
    std::string line;
    for (const identify::Attempt& attempt : result.attempts) {
        if (attempt.result == identify::AttemptResult::NotPlausible) {
            continue;
        }
        line += std::format(" {}={}/{}{}", identify::protocol_name(attempt.protocol),
                            attempt.verified, attempt.required,
                            attempt.result == identify::AttemptResult::Unavailable ? "(n/a)" : "");
    }
    std::println("  {:<8} -> {:<8} conf {:.3f}  tried:{}", label,
                 identify::protocol_name(result.protocol), result.confidence, line);
}

void check_identified(const char* label, const identify::Identification& result,
                      Protocol wanted) {
    print_attempts(label, result);
    INFO(label);
    CHECK(result.protocol == wanted);
    CHECK(result.confidence >= 0.9);
}

}  // namespace

TEST_CASE("each protocol is identified from one dwell of it", "[characterise][identify]") {
    std::println("test_identify: seed {}, {:.0f} dB in 2500 Hz", kSeed, kSnrDb);

    SECTION("P25 Phase 1") {
        constexpr dsp::SampleRate kRate = 48000;
        siggen::P25ModConfig mod;
        mod.rate = kRate;
        siggen::P25HeaderMessage message;
        auto once = siggen::p25_render_header_message(mod, message);
        REQUIRE(once.has_value());
        auto samples = in_noise(repeat_to(*once, 2 * kRate), kRate, kSnrDb, kSeed + 1);
        check_identified("p25", run(samples, kRate, 8100.0), Protocol::P25Phase1);
    }

    SECTION("D-STAR") {
        constexpr dsp::SampleRate kRate = 48000;
        siggen::DStarModConfig mod;
        mod.rate = kRate;
        siggen::DStarMessage message;
        message.header.own_callsign = "N0CALL";
        message.header.companion = "CQCQCQ";
        message.header.destination_repeater = "DIRECT";
        message.header.departure_repeater = "DIRECT";
        message.header.own_suffix = "TEST";
        std::uint32_t state = 0x1234567U;
        for (std::size_t i = 0; i < 84; ++i) {
            std::array<std::uint8_t, decode::kDStarVoiceBits> voice{};
            for (std::uint8_t& bit : voice) {
                state = state * 1103515245U + 12345U;
                bit = static_cast<std::uint8_t>((state >> 16) & 1U);
            }
            message.voice_frames.push_back(voice);
        }
        auto once = siggen::dstar_render(mod, message);
        REQUIRE(once.has_value());
        once->resize(std::min<std::size_t>(once->size(), 2 * kRate));
        auto samples = in_noise(repeat_to(*once, 2 * kRate), kRate, kSnrDb, kSeed + 2);
        check_identified("dstar", run(samples, kRate, 5000.0), Protocol::DStar);
    }

    SECTION("TETRA") {
        constexpr dsp::SampleRate kRate = 96000;
        siggen::TetraModConfig mod;
        mod.rate = kRate;
        decode::TetraSyncPdu pdu;
        pdu.colour_code = 5;
        pdu.mobile_country_code = 234;
        pdu.mobile_network_code = 14;
        pdu.frame_number = 3;
        pdu.multiframe_number = 7;
        // One continuous differential stream, as tests/decode/test_tetra.cpp
        // renders it: clause 5.4's phase reference runs across bursts, so
        // bursts rendered one at a time and joined would each restart it.
        std::vector<std::uint8_t> bits;
        for (std::uint64_t burst = 0; bits.size() < 2 * decode::kTetraBurstBits * 150; ++burst) {
            auto one = siggen::tetra_sync_burst_bits(pdu, kSeed + burst);
            REQUIRE(one.has_value());
            bits.insert(bits.end(), one->begin(), one->end());
        }
        // The transmitter wants a whole number of samples per symbol and the
        // probe bucket a 25 kHz signal lands in is 96000 S/s, which is 5.33,
        // so the stream is rendered at 72000 and resampled by 4/3.
        mod.rate = 72000;
        auto native = siggen::tetra_render_bits(mod, bits);
        REQUIRE(native.has_value());
        auto stream = resample_four_thirds(*native);
        stream.resize(2 * kRate);
        auto samples = in_noise(std::move(stream), kRate, kSnrDb, kSeed + 3);
        check_identified("tetra", run(samples, kRate, 23000.0), Protocol::Tetra);
    }

    SECTION("M17") {
        constexpr dsp::SampleRate kRate = 48000;
        siggen::M17ModConfig mod;
        mod.rate = kRate;
        siggen::M17StreamMessage message;
        message.destination = 0xFFFFFFFFFFFFULL;
        message.source = 0x00000061D0D1ULL;
        for (std::size_t i = 0; i < 40; ++i) {
            std::array<std::uint8_t, decode::kM17StreamPayloadBytes> payload{};
            payload[0] = static_cast<std::uint8_t>(i);
            message.payloads.push_back(payload);
        }
        auto once = siggen::m17_render_stream(mod, message);
        REQUIRE(once.has_value());
        once->resize(std::min<std::size_t>(once->size(), 2 * kRate));
        auto samples = in_noise(repeat_to(*once, 2 * kRate), kRate, kSnrDb, kSeed + 4);
        check_identified("m17", run(samples, kRate, 9000.0), Protocol::M17);
    }

    SECTION("POCSAG") {
        constexpr dsp::SampleRate kRate = 48000;
        siggen::PocsagModConfig mod;
        mod.rate = kRate;
        mod.bit_rate = decode::kPocsag1200;
        std::vector<siggen::PocsagPageSpec> pages(3);
        for (std::size_t i = 0; i < pages.size(); ++i) {
            pages[i].identity = 1234567U + static_cast<std::uint32_t>(i);
            auto bits = siggen::pocsag_alphanumeric_bits("IDENTIFY TEST PAGE");
            REQUIRE(bits.has_value());
            pages[i].message_bits = *bits;
        }
        const std::vector<std::uint8_t> bits = siggen::pocsag_bits(pages);
        auto once = siggen::pocsag_render_baseband(mod, bits);
        REQUIRE(once.has_value());
        once->resize(std::min<std::size_t>(once->size(), 2 * kRate));
        auto samples = in_noise(repeat_to(*once, 2 * kRate), kRate, kSnrDb, kSeed + 5);
        check_identified("pocsag", run(samples, kRate, 12000.0), Protocol::Pocsag);
    }

    SECTION("AX.25") {
        constexpr dsp::SampleRate kRate = 48000;
        siggen::Ax25ModConfig mod;
        mod.rate = kRate;
        siggen::Ax25FrameSpec frame;
        frame.destination = decode::Ax25Address{"APRS", 0};
        frame.source = decode::Ax25Address{"N0CALL", 7};
        const std::string text = "!4903.50N/07201.75W-identify";
        frame.information.assign(text.begin(), text.end());
        auto octets = siggen::ax25_frame_octets(frame);
        REQUIRE(octets.has_value());
        const std::vector<std::vector<std::uint8_t>> frames = {*octets, *octets, *octets};
        auto audio = siggen::ax25_render(mod, frames);
        REQUIRE(audio.has_value());
        auto baseband = fm_modulate(*audio, kRate, 6000.0);
        auto samples = in_noise(repeat_to(std::move(baseband), 2 * kRate), kRate, kSnrDb, kSeed + 6);
        check_identified("ax25", run(samples, kRate, 10000.0), Protocol::Ax25);
    }

    SECTION("RTTY") {
        constexpr dsp::SampleRate kRate = 12000;
        siggen::RttyModConfig mod;
        mod.rate = kRate;
        auto combinations = siggen::ita2_encode_text(U"RYRYRY THE QUICK BROWN FOX 73 DE N0CALL ");
        REQUIRE(combinations.has_value());
        auto audio = siggen::rtty_render(mod, *combinations);
        REQUIRE(audio.has_value());
        auto baseband = audio_to_dc(*audio, kRate, 2210.0, 600.0);
        baseband.resize(std::min<std::size_t>(baseband.size(), kNarrowSeconds * kRate));
        auto samples = in_noise(repeat_to(std::move(baseband), kNarrowSeconds * kRate), kRate, kSnrDb, kSeed + 7);
        check_identified("rtty", run(samples, kRate, 250.0), Protocol::Rtty);
    }

    SECTION("SITOR-B") {
        constexpr dsp::SampleRate kRate = 12000;
        siggen::SitorModConfig mod;
        mod.rate = kRate;
        // Four pairs rather than the transmitter's sixteen, which alone take
        // 2.24 s: the decoder phases on any run of them, and a probe that
        // catches a transmission's start catches its first few.
        mod.phasing_pairs = 4;
        auto combinations = siggen::ita2_encode_text(U"THE QUICK BROWN FOX JUMPS 1234567890 ");
        REQUIRE(combinations.has_value());
        auto audio = siggen::sitor_b_render(mod, *combinations);
        REQUIRE(audio.has_value());
        auto baseband = audio_to_dc(*audio, kRate, 1700.0, 600.0);
        baseband.resize(std::min<std::size_t>(baseband.size(), kNarrowSeconds * kRate));
        auto samples = in_noise(repeat_to(std::move(baseband), kNarrowSeconds * kRate), kRate, kSnrDb, kSeed + 8);
        check_identified("sitor-b", run(samples, kRate, 300.0), Protocol::SitorB);
    }

    SECTION("PSK31") {
        constexpr dsp::SampleRate kRate = 12000;
        siggen::Psk31ModConfig mod;
        mod.rate = kRate;
        mod.tone_hz = 1000;
        mod.preamble_symbols = 16;
        auto bits = siggen::psk31_message_bits(mod, "cq cq de n0call n0call pse k ");
        REQUIRE(bits.has_value());
        auto analytic = siggen::psk31_render_analytic(mod, *bits);
        REQUIRE(analytic.has_value());
        auto baseband = analytic_to_dc(std::move(*analytic), kRate, 1000.0);
        baseband.resize(std::min<std::size_t>(baseband.size(), kNarrowSeconds * kRate));
        auto samples = in_noise(repeat_to(std::move(baseband), kNarrowSeconds * kRate), kRate, kSnrDb, kSeed + 9);
        check_identified("psk31", run(samples, kRate, 60.0), Protocol::Psk31);
    }

    SECTION("CW") {
        constexpr dsp::SampleRate kRate = 12000;
        siggen::CwModConfig mod;
        mod.rate = kRate;
        mod.tone_hz = 700;
        mod.wpm = 25.0;
        mod.lead_in_seconds = 0.1;
        mod.tail_seconds = 0.1;
        auto analytic = siggen::cw_render_analytic(mod, "CQ TEST DE N0CALL K");
        REQUIRE(analytic.has_value());
        auto baseband = analytic_to_dc(std::move(*analytic), kRate, 700.0);
        baseband.resize(std::min<std::size_t>(baseband.size(), kNarrowSeconds * kRate));
        auto samples = in_noise(repeat_to(std::move(baseband), kNarrowSeconds * kRate), kRate, kSnrDb, kSeed + 10);
        check_identified("cw", run(samples, kRate, 80.0), Protocol::Cw);
    }
}

// REJECTS: a row that verifies on noise. Every row is made plausible in turn
// by the width it asks for and an Unknown family, which rules nothing out, and
// nothing may come back. This is the case the whole rule exists for.
TEST_CASE("noise verifies nothing at any plausible width", "[characterise][identify]") {
    struct Case {
        const char* label;
        dsp::SampleRate rate;
        double occupied;
    };
    const Case cases[] = {
        {"vhf 4fsk", 48000, 8100.0}, {"vhf gmsk", 48000, 5000.0}, {"vhf fm", 48000, 12000.0},
        {"tetra", 96000, 23000.0},   {"hf fsk", 12000, 250.0},    {"hf psk", 12000, 60.0},
        {"hf cw", 12000, 80.0},
    };
    for (std::uint64_t trial = 0; trial < 3; ++trial) {
        for (const Case& c : cases) {
            // The dwell a probe collects at this width.
            const std::size_t seconds = c.occupied <= 600.0 ? kNarrowSeconds : 2;
            const auto noise = characterise_test::gaussian_noise(
                seconds * static_cast<std::size_t>(c.rate), 1.0, kSeed + 1000 + 17 * trial);
            const auto result = run(noise, c.rate, c.occupied);
            print_attempts(c.label, result);
            INFO(c.label << " trial " << trial);
            CHECK(result.protocol == Protocol::None);
        }
    }
}

// REJECTS: CW claimed on a carrier that is on and never keyed, which is what
// the tier-two "carrier" answer mostly is on HF. A keyed carrier has elements
// and spaces to time; a steady one has neither.
TEST_CASE("a steady carrier is not CW", "[characterise][identify]") {
    constexpr dsp::SampleRate kRate = 12000;
    const auto tone = characterise_test::pure_tone(kNarrowSeconds * kRate, kRate, 0, 1.0);
    const auto samples = in_noise(tone, kRate, kSnrDb, kSeed + 20);
    const auto result = run(samples, kRate, 10.0, ModulationFamily::Unmodulated);
    print_attempts("carrier", result);
    CHECK(result.protocol == Protocol::None);
}

// The DMR row is present, plausible on a 4FSK-wide signal, and unavailable
// until core/decode/dmr.h lands; with IdentifyConfig::dmr off it is gone.
TEST_CASE("the DMR row waits for its decoder and can be switched off", "[characterise][identify]") {
    constexpr dsp::SampleRate kRate = 48000;
    const auto noise = characterise_test::gaussian_noise(2 * kRate, 1.0, kSeed + 30);
    identify::IdentifyHints hints;
    hints.occupied_hz = 8100.0;
    identify::IdentifyConfig config;
    config.rate = kRate;

    auto on = identify::identify(dsp::ConstComplexSpan(noise), hints, config);
    REQUIRE(on.has_value());
    const auto dmr = std::find_if(on->attempts.begin(), on->attempts.end(),
                                  [](const identify::Attempt& a) { return a.protocol == Protocol::Dmr; });
    REQUIRE(dmr != on->attempts.end());
    CHECK(dmr->result == identify::AttemptResult::Unavailable);

    config.dmr = false;
    auto off = identify::identify(dsp::ConstComplexSpan(noise), hints, config);
    REQUIRE(off.has_value());
    CHECK(std::none_of(off->attempts.begin(), off->attempts.end(),
                       [](const identify::Attempt& a) { return a.protocol == Protocol::Dmr; }));
}

// The plausibility table on its own: a family rules out only what the physics
// does, and Unknown rules out nothing inside a row's width.
TEST_CASE("the plausibility table follows the width and the family", "[characterise][identify]") {
    identify::IdentifyHints narrow;
    narrow.occupied_hz = 60.0;
    CHECK(identify::plausible(Protocol::Psk31, narrow, 12000));
    CHECK_FALSE(identify::plausible(Protocol::P25Phase1, narrow, 12000));

    identify::IdentifyHints carrier = narrow;
    carrier.family = ModulationFamily::Unmodulated;
    CHECK(identify::plausible(Protocol::Cw, carrier, 12000));
    CHECK_FALSE(identify::plausible(Protocol::Psk31, carrier, 12000));

    identify::IdentifyHints fourfsk;
    fourfsk.occupied_hz = 8100.0;
    fourfsk.family = ModulationFamily::Fsk;
    CHECK(identify::plausible(Protocol::P25Phase1, fourfsk, 48000));
    CHECK(identify::plausible(Protocol::M17, fourfsk, 48000));
    CHECK_FALSE(identify::plausible(Protocol::P25Phase1, fourfsk, 6000));
    CHECK_FALSE(identify::plausible(Protocol::Tetra, fourfsk, 96000));

    identify::IdentifyHints broadcast;
    broadcast.occupied_hz = 180000.0;
    CHECK(identify::plausible(Protocol::Rds, broadcast, 192000));
}

// THE RTTY ROW'S BAR, measured. RTTY carries no code, so the row counts framed
// characters, and a 100 baud SITOR-B signal with the same 170 Hz shift frames
// characters at 45.45 baud too. What tells them apart is how cleanly each
// character was read: rtty.h's margin is the smallest of its seven soft
// readings, which the discriminator bounds to one.
TEST_CASE("identify survey: the RTTY row's margin on RTTY, SITOR-B and noise",
          "[.identify-survey]") {
    constexpr dsp::SampleRate kRate = 12000;
    const auto rtty_row = [](const std::vector<dsp::Complex32>& baseband) {
        std::vector<float> audio(baseband.size());
        const double step = 2.0 * std::numbers::pi * 2210.0 / static_cast<double>(kRate);
        for (std::size_t n = 0; n < baseband.size(); ++n) {
            const double phase = std::fmod(step * static_cast<double>(n), 2.0 * std::numbers::pi);
            audio[n] = static_cast<float>(
                (std::complex<double>(baseband[n].real(), baseband[n].imag()) *
                 std::polar(1.0, phase))
                    .real());
        }
        std::string line;
        for (const bool space_above : {true, false}) {
            decode::RttyConfig config;
            config.rate = kRate;
            config.space_above_mark = space_above;
            auto decoder = decode::RttyDecoder::create(config);
            REQUIRE(decoder.has_value());
            std::vector<decode::RttyCharacter> characters;
            decoder->process(dsp::ConstRealSpan(audio), characters);
            double sum = 0.0;
            std::size_t over_half = 0;
            for (const decode::RttyCharacter& c : characters) {
                sum += static_cast<double>(c.margin);
                over_half += c.margin >= 0.5F ? 1 : 0;
            }
            line += std::format("  [{} chars, {} framing errors, mean margin {:.3f}, {} over 0.5]",
                                characters.size(), decoder->framing_errors(),
                                characters.empty() ? 0.0 : sum / static_cast<double>(characters.size()),
                                over_half);
        }
        return line;
    };

    for (const double level : {20.0, 10.0, 5.0}) {
        siggen::RttyModConfig mod;
        mod.rate = kRate;
        auto combinations = siggen::ita2_encode_text(U"RYRYRY THE QUICK BROWN FOX 73 DE N0CALL ");
        REQUIRE(combinations.has_value());
        auto audio = siggen::rtty_render(mod, *combinations);
        REQUIRE(audio.has_value());
        auto baseband = audio_to_dc(*audio, kRate, 2210.0, 600.0);
        auto samples = in_noise(repeat_to(std::move(baseband), kNarrowSeconds * kRate), kRate,
                                level, kSeed + 40);
        std::println("rtty    {:>4.0f} dB{}", level, rtty_row(samples));
    }
    for (const double level : {20.0, 10.0}) {
        siggen::SitorModConfig mod;
        mod.rate = kRate;
        auto combinations = siggen::ita2_encode_text(U"THE QUICK BROWN FOX JUMPS 1234567890 ");
        REQUIRE(combinations.has_value());
        auto audio = siggen::sitor_b_render(mod, *combinations);
        REQUIRE(audio.has_value());
        auto baseband = audio_to_dc(*audio, kRate, 1700.0, 600.0);
        auto samples = in_noise(repeat_to(std::move(baseband), kNarrowSeconds * kRate), kRate,
                                level, kSeed + 41);
        std::println("sitor-b {:>4.0f} dB{}", level, rtty_row(samples));
    }
    for (std::uint64_t trial = 0; trial < 3; ++trial) {
        const auto noise =
            characterise_test::gaussian_noise(kNarrowSeconds * kRate, 1.0, kSeed + 50 + trial);
        std::println("noise   trial {}{}", trial, rtty_row(noise));
    }
}
