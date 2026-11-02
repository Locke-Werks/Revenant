// NOAA APT: the line format against KLM section 4.2's figures, and the decoder
// against the transmitter in core/dsp/synth/apt_mod.h, through audio and
// through FM with noise and Doppler, in any blocking.
//
// Error figures are measured and printed; the assertions around them are
// loose, as everywhere in this directory.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <vector>

#include "core/decode/apt.h"
#include "core/dsp/synth/apt_mod.h"
#include "core/dsp/synth/channel.h"

using namespace revenant;

namespace {

siggen::AptImage test_image(std::size_t rows) {
    siggen::AptImage img;
    img.rows = rows;
    img.channel_a.resize(rows * decode::kAptChannelWords);
    img.channel_b.resize(rows * decode::kAptChannelWords);
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < decode::kAptChannelWords; ++c) {
            const double a = 128.0 + 100.0 * std::sin(static_cast<double>(c) / 40.0) *
                                         std::cos(static_cast<double>(r) / 20.0);
            img.channel_a[r * decode::kAptChannelWords + c] =
                static_cast<std::uint8_t>(std::lround(a));
            img.channel_b[r * decode::kAptChannelWords + c] =
                static_cast<std::uint8_t>(c * 255 / (decode::kAptChannelWords - 1));
        }
    }
    return img;
}

struct Capture {
    std::vector<decode::AptLine> lines;
    std::vector<decode::AptTelemetry> telemetry;
    std::vector<decode::ImageRow> rows;
    int opens = 0;
    int closes = 0;
};

Capture decode_all(const std::vector<float>& audio, double fs, std::size_t chunk) {
    Capture cap;
    decode::AptEvents ev;
    ev.image.on_open = [&](const decode::ImageOpen&) { ++cap.opens; };
    ev.image.on_row = [&](const decode::ImageRow& r) { cap.rows.push_back(r); };
    ev.image.on_close = [&](const decode::ImageClose&) { ++cap.closes; };
    ev.on_line = [&](const decode::AptLine& l) { cap.lines.push_back(l); };
    ev.on_telemetry = [&](const decode::AptTelemetry& t) { cap.telemetry.push_back(t); };
    auto dec = decode::AptDecoder::create(decode::AptConfig{fs}, ev);
    REQUIRE(dec.has_value());
    for (std::size_t i = 0; i < audio.size(); i += chunk) {
        const std::size_t n = std::min(chunk, audio.size() - i);
        dec->push(std::span<const float>(audio.data() + i, n));
    }
    dec->finish();
    return cap;
}

// Mean absolute error over both video channels of the lines decoded.
double video_error(const Capture& cap, const std::vector<siggen::AptLineWords>& sent) {
    double err = 0.0;
    std::size_t n = 0;
    for (std::size_t r = 0; r < cap.lines.size() && r < sent.size(); ++r) {
        for (std::size_t base : {decode::kAptChannelAVideo, decode::kAptChannelBVideo}) {
            for (std::size_t k = base + 2; k < base + decode::kAptChannelWords - 2; ++k) {
                err += std::abs(static_cast<int>(cap.lines[r].words[k]) -
                                static_cast<int>(sent[r][k]));
                ++n;
            }
        }
    }
    return n == 0 ? 1e9 : err / static_cast<double>(n);
}

std::vector<float> discriminate(const std::vector<dsp::Complex32>& x, double fs, double dev) {
    std::vector<float> out(x.size(), 0.0F);
    for (std::size_t n = 1; n < x.size(); ++n) {
        const auto d = x[n] * std::conj(x[n - 1]);
        out[n] = static_cast<float>(std::arg(d) * fs / (2.0 * std::numbers::pi * dev));
    }
    return out;
}

std::vector<float> decimate(const std::vector<float>& x, std::size_t factor) {
    std::vector<float> out(x.size() / factor);
    for (std::size_t i = 0; i < out.size(); ++i) {
        float s = 0.0F;
        for (std::size_t k = 0; k < factor; ++k) {
            s += x[i * factor + k];
        }
        out[i] = s / static_cast<float>(factor);
    }
    return out;
}

}  // namespace

TEST_CASE("APT line format matches KLM figures 4.2.3-2 and 4.2.3-3", "[apt]") {
    // Figure 4.2.3-2: 39 + 47 + 909 + 45 per channel, two channels, 2080.
    STATIC_REQUIRE(2 * (decode::kAptSyncWords + decode::kAptSpaceWords +
                        decode::kAptChannelWords + decode::kAptTelemetryWords) ==
                   decode::kAptLineWords);
    STATIC_REQUIRE(decode::kAptChannelBTelemetry + decode::kAptTelemetryWords ==
                   decode::kAptLineWords);

    // Figure 4.2.3-3: Sync A is 7 cycles of 1040 Hz at 4160 words/s, 2 high
    // and 2 low; Sync B 7 pulses at 832 pps, 3 high and 2 low.
    const auto a = decode::apt_sync_a();
    const auto b = decode::apt_sync_b();
    CHECK(std::count(a.begin(), a.end(), decode::kAptSyncHigh) == 14);
    CHECK(std::count(b.begin(), b.end(), decode::kAptSyncHigh) == 21);
    CHECK(a[3] == decode::kAptSyncLow);
    CHECK(a[4] == decode::kAptSyncHigh);
    CHECK(a[6] == decode::kAptSyncLow);
    CHECK(a[29] == decode::kAptSyncHigh);
    CHECK(a[30] == decode::kAptSyncLow);
    CHECK(b[4] == decode::kAptSyncHigh);
    CHECK(b[6] == decode::kAptSyncHigh);
    CHECK(b[7] == decode::kAptSyncLow);
    CHECK(b[38] == decode::kAptSyncLow);
    CHECK(b[36] == decode::kAptSyncHigh);
    CHECK(4160.0 / 4.0 == 1040.0);
    CHECK(4160.0 / 5.0 == 832.0);

    // 4.2.3: wedge 3 is 3A, wedge 6 is 3B.
    CHECK(decode::apt_channel_name(3) == "3A");
    CHECK(decode::apt_channel_name(6) == "3B");
    CHECK(decode::apt_channel_name(4) == "4");
    CHECK(decode::apt_channel_name(7).empty());
}

TEST_CASE("APT round trip on clean audio", "[apt]") {
    auto img = test_image(150);
    img.channel_a_wedge = 6;
    img.channel_b_wedge = 4;
    const auto sent = siggen::apt_format(img);
    for (double fs : {11025.0, 20800.0}) {
        siggen::AptAudioSpec spec;
        spec.sample_rate = fs;
        const auto audio = siggen::apt_audio(sent, spec);
        const auto cap = decode_all(audio, fs, 4096);
        const double err = video_error(cap, sent);
        INFO("fs " << fs << " lines " << cap.lines.size() << " video mean abs error " << err);
        CHECK(cap.opens == 1);
        CHECK(cap.closes == 1);
        REQUIRE(cap.lines.size() >= sent.size() - 1);
        CHECK(std::abs(cap.lines[0].start_sample - spec.lead_seconds * fs) < 1.0);
        CHECK(err < 3.0);
        CHECK(cap.rows.size() == cap.lines.size());
        CHECK(cap.rows[0].width == decode::kAptLineWords);
        REQUIRE(cap.telemetry.size() == 1);
        CHECK(cap.telemetry[0].first_line == 0);
        CHECK(cap.telemetry[0].channel[0] == "3B");
        CHECK(cap.telemetry[0].channel[1] == "4");
        CHECK(std::abs(cap.telemetry[0].wedges[1][14] - 200.0F) < 4.0F);
        CHECK(std::abs(cap.telemetry[0].wedges[0][14] - 60.0F) < 4.0F);
    }
}

TEST_CASE("APT tracks a word clock 300 ppm slow", "[apt]") {
    const auto sent = siggen::apt_format(test_image(60));
    siggen::AptAudioSpec spec;
    spec.clock_ppm = 300.0;
    const auto cap = decode_all(siggen::apt_audio(sent, spec), spec.sample_rate, 4096);
    const double err = video_error(cap, sent);
    INFO("lines " << cap.lines.size() << " error " << err << " period "
                  << cap.lines.back().period_samples);
    REQUIRE(cap.lines.size() >= sent.size() - 1);
    CHECK(err < 4.0);
    CHECK(std::abs(cap.lines.back().period_samples - 5512.5 * 1.0003) < 0.5);
}

TEST_CASE("APT decodes the same in any blocking", "[apt]") {
    const auto sent = siggen::apt_format(test_image(140));
    siggen::AptAudioSpec spec;
    const auto audio = siggen::apt_audio(sent, spec);
    const auto whole = decode_all(audio, spec.sample_rate, audio.size());
    REQUIRE(whole.lines.size() >= sent.size() - 1);
    for (std::size_t chunk : {std::size_t{1}, std::size_t{37}, std::size_t{4096}}) {
        const auto part = decode_all(audio, spec.sample_rate, chunk);
        INFO("chunk " << chunk);
        REQUIRE(part.lines.size() == whole.lines.size());
        for (std::size_t i = 0; i < whole.lines.size(); ++i) {
            REQUIRE(part.lines[i].words == whole.lines[i].words);
            REQUIRE(part.lines[i].start_sample == whole.lines[i].start_sample);
        }
        REQUIRE(part.telemetry.size() == whole.telemetry.size());
    }
}

TEST_CASE("APT through FM with noise and Doppler", "[apt]") {
    const auto sent = siggen::apt_format(test_image(140));
    constexpr double kAudio = 11025.0;
    constexpr std::size_t kOver = 4;
    siggen::AptAudioSpec aspec;
    aspec.sample_rate = kAudio * kOver;
    const auto audio = siggen::apt_audio(sent, aspec);
    siggen::AptFmSpec fspec;
    fspec.sample_rate = aspec.sample_rate;
    // A 137 MHz pass sweeps about +3 kHz to -3 kHz.
    fspec.doppler_start_hz = 3000.0;
    fspec.doppler_rate_hz_per_s = -6000.0 / 75.0;
    const auto clean = siggen::apt_fm(audio, fspec);

    for (double snr : {20.0, 14.0, 11.0}) {
        auto rf = clean;
        REQUIRE(siggen::add_awgn(rf, siggen::NoiseLevel::snr_in_reference_bandwidth_db(snr, 40000),
                                 static_cast<dsp::SampleRate>(fspec.sample_rate),
                                 0xA9'7000ULL + static_cast<std::uint64_t>(snr))
                    .has_value());
        const auto demod = decimate(discriminate(rf, fspec.sample_rate, fspec.deviation_hz), kOver);
        const auto cap = decode_all(demod, kAudio, 8192);
        const double err = video_error(cap, sent);
        std::size_t free = 0;
        for (const auto& l : cap.lines) {
            free += l.free_running ? 1 : 0;
        }
        INFO("SNR " << snr << " dB in 40 kHz: lines " << cap.lines.size() << " free running "
                    << free << " video mean abs error " << err << " telemetry frames "
                    << cap.telemetry.size());
        CHECK(cap.lines.size() >= sent.size() - 1);
        if (snr >= 14.0) {
            CHECK(err < 12.0);
            CHECK(cap.telemetry.size() == 1);
        } else {
            CHECK(err < 40.0);
        }
        // Print every case's figures.
        CHECK(true);
        WARN("SNR " << snr << " dB/40 kHz: video MAE " << err << ", free-running " << free);
    }
}
