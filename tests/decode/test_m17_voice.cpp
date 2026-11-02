// M17 voice: Codec 2 frames through the 4FSK stream and back to audio.
//
// The test speech is synthetic, a glottal pulse train gliding in pitch through
// two formant resonators with a syllable envelope, encoded by the linked
// libcodec2 itself. What the receiver owes is the library's own decode of the
// bits that were sent, so audio rendered from the air is compared with that
// reference. Not sample for sample: libcodec2's synthesis draws on one random
// generator shared by every instance in the process (measured: correct bits
// still differ in the last bit or two), so a right payload shows as a
// segmental SNR near 50 dB and one wrong frame drags it far below 40. Its distance
// from the original waveform is the codec's business, not the receiver's: a
// parametric vocoder does not keep phase, so that figure is reported as
// segmental SNR for the record with only a loose bound.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "core/decode/codec2_voice.h"
#include "core/decode/m17.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/m17_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 48000;

// Two seconds of vowel-like sound at 8 kHz.
std::vector<float> test_speech(std::size_t samples) {
    std::vector<float> out(samples);
    const double fs = decode::kCodec2SampleRate;
    double phase = 0.0;
    // Two-pole resonators at roughly the first two formants of an open vowel.
    struct Resonator {
        double a1, a2, y1 = 0, y2 = 0;
        double step(double x) {
            const double y = x + a1 * y1 + a2 * y2;
            y2 = y1;
            y1 = y;
            return y;
        }
    };
    auto make = [fs](double f, double bw) {
        const double r = std::exp(-std::numbers::pi * bw / fs);
        return Resonator{2.0 * r * std::cos(2.0 * std::numbers::pi * f / fs), -r * r};
    };
    Resonator f1 = make(700.0, 90.0);
    Resonator f2 = make(1200.0, 110.0);
    for (std::size_t n = 0; n < samples; ++n) {
        const double t = static_cast<double>(n) / fs;
        const double f0 = 130.0 + 40.0 * std::sin(2.0 * std::numbers::pi * 0.7 * t);
        phase += f0 / fs;
        double pulse = 0.0;
        if (phase >= 1.0) {
            phase -= 1.0;
            pulse = 1.0;
        }
        const double envelope = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * 2.5 * t);
        out[n] = static_cast<float>(envelope * f2.step(f1.step(pulse)));
    }
    float peak = 0.0F;
    for (float x : out) {
        peak = std::max(peak, std::abs(x));
    }
    for (float& x : out) {
        x *= 0.5F / peak;
    }
    return out;
}

// Segmental SNR over 20 ms segments with energy, in dB.
double segmental_snr_db(std::span<const float> reference, std::span<const float> test) {
    const std::size_t seg = 160;
    const std::size_t n = std::min(reference.size(), test.size());
    double total = 0.0;
    int count = 0;
    for (std::size_t at = 0; at + seg <= n; at += seg) {
        double signal = 0.0;
        double noise = 0.0;
        for (std::size_t i = at; i < at + seg; ++i) {
            signal += double(reference[i]) * reference[i];
            const double e = double(reference[i]) - test[i];
            noise += e * e;
        }
        if (signal < 1e-6) {
            continue;
        }
        total += std::clamp(10.0 * std::log10(signal / std::max(noise, 1e-12)), -10.0, 60.0);
        ++count;
    }
    return count == 0 ? 0.0 : total / count;
}

double rms(std::span<const float> x) {
    double sum = 0.0;
    for (float v : x) {
        sum += double(v) * v;
    }
    return std::sqrt(sum / std::max<std::size_t>(x.size(), 1));
}

struct VoiceCase {
    decode::Codec2Mode mode;
    std::uint16_t data_type;  // M17 Table 3.4
    std::string name;
};

struct Encoded {
    siggen::M17StreamMessage message;
    std::vector<float> speech;
    std::vector<float> reference;  // the library's decode of what was sent
};

// Codec 2 frames packed per M17 Table 2.11, one 16-byte payload per 40 ms.
Encoded encode(const VoiceCase& c, std::size_t frames) {
    auto encoder = decode::Codec2Voice::create(c.mode);
    REQUIRE(encoder.has_value());
    auto reference = decode::Codec2Voice::create(c.mode);
    REQUIRE(reference.has_value());
    REQUIRE(encoder->bytes_per_frame() == 8);

    Encoded e;
    e.speech = test_speech(frames * 320);
    e.message.destination = decode::m17_encode_callsign("@ALL").value();
    e.message.source = decode::m17_encode_callsign("N0CALL").value();
    e.message.type = static_cast<std::uint16_t>(0x0001U | (c.data_type << 1U));
    const std::size_t per = 320 / encoder->samples_per_frame();
    std::size_t at = 0;
    for (std::size_t f = 0; f < frames; ++f) {
        std::array<std::uint8_t, 16> payload{};
        for (std::size_t k = 0; k < per; ++k) {
            std::span<std::uint8_t> bytes(payload.data() + 8 * k, 8);
            REQUIRE(encoder->encode(std::span<const float>(e.speech).subspan(at, encoder->samples_per_frame()), bytes)
                        .has_value());
            at += encoder->samples_per_frame();
            REQUIRE(reference->decode_all(bytes, e.reference).has_value());
        }
        if (per == 1) {
            // Table 2.11: the second half is arbitrary data at 1600.
            std::fill(payload.begin() + 8, payload.end(), std::uint8_t{0xA5});
        }
        e.message.payloads.push_back(payload);
    }
    return e;
}

std::vector<float> receive(std::span<const siggen::Complex32> samples, std::size_t block,
                           std::size_t& voiced_frames) {
    auto decoder = decode::M17::create(decode::M17Config{});
    REQUIRE(decoder.has_value());
    std::vector<decode::M17Frame> frames;
    for (std::size_t start = 0; start < samples.size(); start += block) {
        REQUIRE(decoder->process(samples.subspan(start, std::min(block, samples.size() - start)), frames)
                    .has_value());
    }
    std::vector<float> audio;
    voiced_frames = 0;
    for (const auto& f : frames) {
        if (!f.voice.empty()) {
            ++voiced_frames;
            audio.insert(audio.end(), f.voice.begin(), f.voice.end());
        }
    }
    return audio;
}

void pad(std::vector<siggen::Complex32>& samples) {
    samples.insert(samples.end(), static_cast<std::size_t>(kRate / 5), siggen::Complex32{1.0F, 0.0F});
}

}  // namespace

TEST_CASE("Codec 2 reports the frame shapes M17 Table 2.11 needs", "[decode][m17][codec2]") {
    for (auto mode : {decode::Codec2Mode::Mode3200, decode::Codec2Mode::Mode1600,
                      decode::Codec2Mode::Mode1300, decode::Codec2Mode::Mode700C}) {
        auto voice = decode::Codec2Voice::create(mode);
        REQUIRE(voice.has_value());
        INFO("bits " << voice->bits_per_frame() << " samples " << voice->samples_per_frame());
        CHECK(voice->samples_per_frame() % 160 == 0);
    }
    CHECK(decode::Codec2Voice::create(decode::Codec2Mode::Mode3200)->bits_per_frame() == 64);
    CHECK(decode::Codec2Voice::create(decode::Codec2Mode::Mode3200)->samples_per_frame() == 160);
    CHECK(decode::Codec2Voice::create(decode::Codec2Mode::Mode1600)->bits_per_frame() == 64);
    CHECK(decode::Codec2Voice::create(decode::Codec2Mode::Mode1600)->samples_per_frame() == 320);
}

TEST_CASE("M17 voice round trips to the library's own decode", "[decode][m17][codec2]") {
    const VoiceCase c = GENERATE(VoiceCase{decode::Codec2Mode::Mode3200, 0b10, "3200"},
                                 VoiceCase{decode::Codec2Mode::Mode1600, 0b11, "1600"});
    INFO("Codec 2 " << c.name);
    const std::size_t kFrames = 50;  // two seconds
    const Encoded e = encode(c, kFrames);

    auto samples = siggen::m17_render_stream(siggen::M17ModConfig{}, e.message);
    REQUIRE(samples.has_value());

    double noise_db = 0.0;
    SECTION("clean") {}
    SECTION("300 Hz off frequency") {
        siggen::FrequencyConfig offset;
        offset.doppler_shift_hz = 300;
        REQUIRE(siggen::apply_frequency_offset(*samples, offset, kRate, 7).has_value());
    }
    SECTION("AWGN at 12 dB in 9 kHz") {
        noise_db = 12.0;
    }
    pad(*samples);
    if (noise_db > 0.0) {
        REQUIRE(siggen::add_awgn(*samples, siggen::NoiseLevel::snr_in_reference_bandwidth_db(noise_db, 9000),
                                 kRate, 0x4D17'C0DEULL)
                    .has_value());
    }

    std::size_t voiced = 0;
    const auto audio = receive(*samples, 4000, voiced);
    CHECK(voiced == kFrames);
    REQUIRE(audio.size() == e.reference.size());

    const double exact = segmental_snr_db(e.reference, audio);
    const double codec = segmental_snr_db(e.speech, audio);
    INFO("segmental SNR against the reference decode " << exact << " dB, against the original "
                                                      << codec << " dB");
    // Every payload correct; see the note at the head of the file.
    CHECK(exact > 40.0);
    // The codec's own fidelity, loosely: a parametric coder does not keep
    // phase, so only gross failure is caught here.
    CHECK(codec > -10.0);
    CHECK(rms(audio) > 0.25 * rms(e.speech));
    CHECK(rms(audio) < 4.0 * rms(e.speech));
    WARN("M17 Codec 2 " << c.name << ": segmental SNR vs original " << codec << " dB, vs reference "
                        << exact << " dB");
}

TEST_CASE("M17 voice does not depend on how the input is blocked", "[decode][m17][codec2]") {
    const Encoded e = encode(VoiceCase{decode::Codec2Mode::Mode3200, 0b10, "3200"}, 20);
    auto samples = siggen::m17_render_stream(siggen::M17ModConfig{}, e.message);
    REQUIRE(samples.has_value());
    pad(*samples);
    REQUIRE(siggen::add_awgn(*samples, siggen::NoiseLevel::snr_in_reference_bandwidth_db(10.0, 9000), kRate,
                             0x4D17'B10DULL)
                .has_value());
    std::size_t a = 0;
    std::size_t b = 0;
    const auto whole = receive(*samples, samples->size(), a);
    const auto small = receive(*samples, 613, b);
    CHECK(a == b);
    REQUIRE(whole.size() == small.size());
    CHECK(segmental_snr_db(whole, small) > 40.0);
}

TEST_CASE("an encrypted M17 stream renders no voice", "[decode][m17][codec2]") {
    Encoded e = encode(VoiceCase{decode::Codec2Mode::Mode3200, 0b10, "3200"}, 10);
    // Table 3.5: encryption type 10, AES, in bits 4..3.
    e.message.type = static_cast<std::uint16_t>(e.message.type | (0b10U << 3U));
    auto samples = siggen::m17_render_stream(siggen::M17ModConfig{}, e.message);
    REQUIRE(samples.has_value());
    pad(*samples);
    std::size_t voiced = 0;
    const auto audio = receive(*samples, 4000, voiced);
    CHECK(voiced == 0);
    CHECK(audio.empty());
}
