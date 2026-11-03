#include <catch2/catch_test_macros.hpp>

#include <opus.h>

#include <algorithm>
#include <cmath>
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

}  // namespace

TEST_CASE("Opus 20 ms packets from odd chunk sizes", "[webgate][opus]") {
    auto enc = wg::OpusEncoder::create(48000);
    REQUIRE(enc.has_value());

    const std::size_t sizes[] = {7, 4096, 961};
    std::size_t total = 0;
    std::size_t packets = 0;
    std::size_t k = 0;
    while (total < 48000) {
        const std::size_t n = std::min(sizes[k++ % 3], 48000 - total);
        (*enc)->push(tone(n, total));
        total += n;
        packets += (*enc)->take_packets().size();
    }
    CHECK(packets >= 49);
    CHECK(packets <= 51);
}

TEST_CASE("Opus decodes back to the tone", "[webgate][opus]") {
    auto enc = wg::OpusEncoder::create(48000);
    REQUIRE(enc.has_value());

    const auto in = tone(48000);
    (*enc)->push(in);
    const auto packets = (*enc)->take_packets();
    REQUIRE(packets.size() >= 40);

    int err = 0;
    ::OpusDecoder* dec = opus_decoder_create(48000, 1, &err);
    REQUIRE(err == OPUS_OK);

    std::vector<float> out;
    for (const auto& p : packets) {
        std::vector<float> frame(960);
        const int n = opus_decode_float(dec, p.data(), static_cast<opus_int32>(p.size()),
                                        frame.data(), 960, 0);
        REQUIRE(n == 960);
        out.insert(out.end(), frame.begin(), frame.end());
    }
    opus_decoder_destroy(dec);

    // Skip two packets of encoder/decoder latency, then search a small lag
    // window: the codec delay is not a whole number of frames.
    const std::size_t skip = 2 * 960;
    const std::size_t len = out.size() - skip - 400;
    double best = -1.0;
    for (std::size_t lag = 0; lag < 400; ++lag) {
        double xy = 0, xx = 0, yy = 0;
        for (std::size_t i = 0; i < len; ++i) {
            const double x = in[skip + i];
            const double y = out[skip + i + lag];
            xy += x * y;
            xx += x * x;
            yy += y * y;
        }
        best = std::max(best, xy / std::sqrt(xx * yy));
    }
    CHECK(best > 0.9);
}

TEST_CASE("Opus refuses 44100", "[webgate][opus]") {
    auto enc = wg::OpusEncoder::create(44100);
    REQUIRE_FALSE(enc.has_value());
    CHECK(enc.error().message.find("48000") != std::string::npos);
}
