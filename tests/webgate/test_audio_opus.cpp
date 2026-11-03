#include <catch2/catch_test_macros.hpp>

#include <opus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "tools/webgate/audio_opus.h"

namespace wg = revenant::webgate;

namespace {

std::vector<float> tone(std::size_t n, std::size_t start = 0) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = 0.5f * static_cast<float>(
                          std::sin(2.0 * std::numbers::pi * 1000.0 *
                                   static_cast<double>(start + i) / 48000.0));
    }
    return v;
}

struct DecoderDeleter {
    void operator()(::OpusDecoder* d) const { opus_decoder_destroy(d); }
};

// Decodes every packet to 960 samples each; fails the test if one does not.
std::vector<float> decode_all(const std::vector<std::vector<std::uint8_t>>& packets) {
    int err = 0;
    std::unique_ptr<::OpusDecoder, DecoderDeleter> dec(opus_decoder_create(48000, 1, &err));
    REQUIRE(err == OPUS_OK);
    std::vector<float> out;
    for (const auto& p : packets) {
        std::vector<float> frame(960);
        const int n = opus_decode_float(dec.get(), p.data(), static_cast<opus_int32>(p.size()),
                                        frame.data(), 960, 0);
        REQUIRE(n == 960);
        out.insert(out.end(), frame.begin(), frame.end());
    }
    return out;
}

double rms(const std::vector<float>& v, std::size_t from, std::size_t to) {
    double s = 0;
    for (std::size_t i = from; i < to; ++i) {
        s += static_cast<double>(v[i]) * v[i];
    }
    return std::sqrt(s / static_cast<double>(to - from));
}

}  // namespace

TEST_CASE("Opus 20 ms packets from odd chunk sizes", "[webgate][opus]") {
    auto enc = wg::LiveOpusEncoder::create(48000);
    REQUIRE(enc.has_value());

    const std::size_t sizes[] = {7, 4096, 961};
    std::size_t total = 0;
    std::size_t packets = 0;
    std::size_t k = 0;
    bool sizes_ok = true;
    while (total < 48000) {
        const std::size_t n = std::min(sizes[k++ % 3], 48000 - total);
        (*enc)->push(tone(n, total));
        total += n;
        for (const auto& p : (*enc)->take_packets()) {
            ++packets;
            sizes_ok = sizes_ok && !p.empty() && p.size() <= 1275;
        }
    }
    CHECK(packets >= 49);
    CHECK(packets <= 51);
    CHECK(sizes_ok);
    CHECK((*enc)->ok());
    CHECK((*enc)->error().empty());
    CHECK((*enc)->dropped_packets() == 0);
}

TEST_CASE("Opus decodes back to the tone", "[webgate][opus]") {
    auto enc = wg::LiveOpusEncoder::create(48000);
    REQUIRE(enc.has_value());

    const auto in = tone(48000);
    (*enc)->push(in);
    const auto packets = (*enc)->take_packets();
    REQUIRE(packets.size() >= 40);
    const auto out = decode_all(packets);

    // Skip two packets of encoder/decoder latency, then search lags: the codec
    // delay is not a whole number of frames, but must stay under two.
    const std::size_t skip = 2 * 960;
    const std::size_t max_lag = 2400;
    const std::size_t len = out.size() - skip - max_lag;
    // A 1 kHz tone repeats every 48 samples, so every multiple of 48 correlates
    // alike: the delay is the first lag that reaches the peak, not the argmax.
    std::vector<double> corr(max_lag);
    double best = -1.0;
    for (std::size_t lag = 0; lag < max_lag; ++lag) {
        double xy = 0, xx = 0, yy = 0;
        for (std::size_t i = 0; i < len; ++i) {
            const double x = in[skip + i];
            const double y = out[skip + i + lag];
            xy += x * y;
            xx += x * x;
            yy += y * y;
        }
        corr[lag] = xy / std::sqrt(xx * yy);
        best = std::max(best, corr[lag]);
    }
    std::size_t best_lag = 0;
    while (corr[best_lag] < best - 0.02) {
        ++best_lag;
    }
    CHECK(best > 0.9);
    CHECK(best_lag < 2 * 960);

    const double ratio = rms(out, skip, out.size()) / rms(in, skip, in.size());
    CHECK(ratio >= 0.7);
    CHECK(ratio <= 1.3);
}

TEST_CASE("Opus refuses 44100", "[webgate][opus]") {
    auto enc = wg::LiveOpusEncoder::create(44100);
    REQUIRE_FALSE(enc.has_value());
    CHECK(enc.error().message.find("48000") != std::string::npos);
}

TEST_CASE("Opus survives NaN and infinity in the input", "[webgate][opus]") {
    auto enc = wg::LiveOpusEncoder::create(48000);
    REQUIRE(enc.has_value());

    auto in = tone(48000);
    const float inf = std::numeric_limits<float>::infinity();
    for (std::size_t i = 20000; i < 20010; ++i) {
        in[i] = std::numeric_limits<float>::quiet_NaN();
    }
    in[20010] = inf;
    in[20011] = -inf;
    in[20012] = 50.0f;
    (*enc)->push(in);
    const auto packets = (*enc)->take_packets();
    REQUIRE(packets.size() >= 49);
    CHECK((*enc)->ok());
    CHECK((*enc)->dropped_packets() == 0);

    // decode_all requires 960 samples out of every packet, the damaged ones
    // included. The tail, well after the bad samples, must be the tone again
    // at about its own level (0.5 peak is 0.354 RMS).
    const auto out = decode_all(packets);
    for (const float s : out) {
        REQUIRE(std::isfinite(s));
    }
    const double tail = rms(out, out.size() - 10 * 960, out.size());
    CHECK(tail > 0.25);
    CHECK(tail < 0.45);
}

TEST_CASE("Opus caps an undrained queue at 250 packets", "[webgate][opus]") {
    auto enc = wg::LiveOpusEncoder::create(48000);
    REQUIRE(enc.has_value());

    (*enc)->push(tone(300 * 960));
    CHECK((*enc)->dropped_packets() == 50);
    CHECK((*enc)->take_packets().size() == wg::LiveOpusEncoder::kMaxQueuedPackets);
    CHECK((*enc)->take_packets().empty());
    // Overflow is loss, not an encoder fault.
    CHECK((*enc)->ok());
}

// ---- Web decoder fixture ----------------------------------------------------
//
// web/test/fixtures/tone1k.opus holds packets from LiveOpusEncoder for a 1 kHz
// tone, each as a little-endian u16 length then the packet. The phone's
// vendored WASM decoder is tested against it under node, which is how the web
// side proves it understands what this encoder sends. Opus output is not
// guaranteed bit-identical across builds, so the file is checked for what the
// web test relies on rather than compared byte for byte.

namespace {

const std::filesystem::path kWebFixture =
    std::filesystem::path(REVENANT_SOURCE_DIR) / "web" / "test" / "fixtures" / "tone1k.opus";

std::vector<std::vector<std::uint8_t>> read_fixture(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    REQUIRE(f.good());
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(f),
                                          std::istreambuf_iterator<char>()};
    std::vector<std::vector<std::uint8_t>> packets;
    std::size_t at = 0;
    while (at + 2 <= bytes.size()) {
        const std::size_t len = bytes[at] | (static_cast<std::size_t>(bytes[at + 1]) << 8);
        at += 2;
        REQUIRE(at + len <= bytes.size());
        packets.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                             bytes.begin() + static_cast<std::ptrdiff_t>(at + len));
        at += len;
    }
    CHECK(at == bytes.size());
    return packets;
}

}  // namespace

// Hidden: run by hand (test_webgate "[write-fixture]") to regenerate the file.
TEST_CASE("Opus writes the web decoder fixture", "[.][write-fixture]") {
    auto enc = wg::LiveOpusEncoder::create(48000);
    REQUIRE(enc.has_value());
    // 400 ms: twenty packets.
    (*enc)->push(tone(20 * 960));
    const auto packets = (*enc)->take_packets();
    REQUIRE(packets.size() == 20);
    std::ofstream f(kWebFixture, std::ios::binary | std::ios::trunc);
    REQUIRE(f.good());
    for (const auto& p : packets) {
        const char len[2] = {static_cast<char>(p.size() & 0xff),
                             static_cast<char>((p.size() >> 8) & 0xff)};
        f.write(len, 2);
        f.write(reinterpret_cast<const char*>(p.data()), static_cast<std::streamsize>(p.size()));
    }
    REQUIRE(f.good());
}

TEST_CASE("Opus web decoder fixture decodes with libopus", "[webgate][opus]") {
    const auto packets = read_fixture(kWebFixture);
    REQUIRE(packets.size() == 20);
    const auto out = decode_all(packets);
    // 0.5 amplitude sine is 0.354 RMS; skip the codec's start-up packets.
    const double r = rms(out, 4 * 960, out.size());
    CHECK(r > 0.25);
    CHECK(r < 0.45);
}
