// The phone's rate converter: digital voice at 8000 S/s, and the other rates
// a receiver can run at, reaching both encoders at 48000.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <opus.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "tools/webgate/audio_opus.h"
#include "tools/webgate/audio_rate.h"
#include "tools/webgate/hls_playlist.h"

namespace wg = revenant::webgate;

namespace {

constexpr double kToneHz = 1000.0;

std::vector<float> tone(std::uint32_t rate, std::size_t n, std::size_t start = 0) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = 0.5F * static_cast<float>(std::sin(2.0 * std::numbers::pi * kToneHz *
                                                  static_cast<double>(start + i) / rate));
    }
    return v;
}

// Feeds `in` in a repeating pattern of awkward chunk sizes, as the engine's
// chunks never line up with the filter or with 20 ms.
std::vector<float> convert_chunked(wg::PhoneRateConverter& conv, std::uint32_t rate,
                                   const std::vector<float>& in) {
    const std::size_t sizes[] = {160, 1, 333, 97, 1440};
    std::vector<float> out;
    std::size_t at = 0;
    std::size_t k = 0;
    while (at < in.size()) {
        const std::size_t n = std::min(sizes[k++ % 5], in.size() - at);
        const auto got = conv.convert(rate, std::span<const float>(in).subspan(at, n));
        out.insert(out.end(), got.begin(), got.end());
        at += n;
    }
    return out;
}

struct DecoderDeleter {
    void operator()(::OpusDecoder* d) const { opus_decoder_destroy(d); }
};

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

// Zero crossings per second over [from, to), which a pitch error moves by the
// ratio of the rates: 8000 played as 48000 would read six times too high.
double frequency_of(const std::vector<float>& v, std::size_t from, std::size_t to) {
    std::size_t crossings = 0;
    for (std::size_t i = from + 1; i < to; ++i) {
        if ((v[i - 1] < 0.0F) != (v[i] < 0.0F)) {
            ++crossings;
        }
    }
    return static_cast<double>(crossings) / 2.0 / (static_cast<double>(to - from) / 48000.0);
}

}  // namespace

TEST_CASE("rate converter: a tone comes out at 48000, on pitch, seamless across chunks",
          "[webgate][audio-rate]") {
    const std::uint32_t rate = GENERATE(8000U, 16000U, 24000U, 44100U);
    INFO("input rate " << rate);
    const auto in = tone(rate, rate * 2);

    wg::PhoneRateConverter chunked;
    const auto out = convert_chunked(chunked, rate, in);
    CHECK(chunked.resampling());

    // The same stream converted in one call: the filter is fed the same
    // samples in the same order whatever the blocking, so any difference
    // would be a chunk boundary showing.
    wg::PhoneRateConverter whole;
    const auto once = whole.convert(rate, in);
    REQUIRE(out.size() == once.size());
    CHECK(std::equal(out.begin(), out.end(), once.begin()));

    // Zero delay, so output n is the tone at n / 48000 s. Past the first half
    // filter, which reads the silence before the stream, it matches the ideal
    // to within the filter's ripple; a click anywhere would not.
    const auto expect = tone(48000, out.size());
    // Everything owed for two seconds bar the last half filter, which waits
    // for input that has not come.
    CHECK(out.size() >= 2 * 48000 - 400);
    CHECK(out.size() <= 2 * 48000);
    float worst = 0.0F;
    for (std::size_t n = 400; n < out.size(); ++n) {
        worst = std::max(worst, std::abs(out[n] - expect[n]));
    }
    CHECK(worst < 2e-3F);
    CHECK(std::abs(frequency_of(out, 400, out.size()) - kToneHz) < 2.0);
}

TEST_CASE("rate converter: 48000 passes through bit for bit, with no filter",
          "[webgate][audio-rate]") {
    wg::PhoneRateConverter conv;
    const auto in = tone(48000, 4801);
    const auto out = conv.convert(48000, in);
    CHECK_FALSE(conv.resampling());
    // The caller's own samples, not a copy of them.
    CHECK(out.data() == in.data());
    CHECK(out.size() == in.size());
}

TEST_CASE("rate converter: 8000 to 48000 uses exact phases", "[webgate][audio-rate]") {
    revenant::transcribe::Resampler r(8000, 48000);
    CHECK(r.exact_phases());
    CHECK(r.phases() == 6);
    // Half the filter is what the stream waits for: under 5 ms at 8000.
    CHECK(static_cast<double>(r.taps()) / 2.0 / 8000.0 < 0.005);
}

TEST_CASE("rate converter: a rate change flushes the old stream and starts clean",
          "[webgate][audio-rate]") {
    wg::PhoneRateConverter conv;
    const auto dv = tone(8000, 8000);
    const auto first = convert_chunked(conv, 8000, dv);
    REQUIRE(conv.input_rate() == 8000);

    SECTION("to 48000") {
        const auto analog = tone(48000, 4800);
        const auto got = conv.convert(48000, analog);
        CHECK(conv.input_rate() == 48000);
        CHECK_FALSE(conv.resampling());
        // Every sample the second of 8000 owed, then the new chunk intact.
        REQUIRE(first.size() + got.size() == 48000 + analog.size());
        CHECK(std::equal(analog.begin(), analog.end(), got.end() - 4800));
        // And the next chunk at 48000 is back to passing straight through.
        const auto next = conv.convert(48000, analog);
        CHECK(next.data() == analog.data());
    }

    SECTION("to 16000") {
        const auto voice = tone(16000, 16000);
        const auto got = conv.convert(16000, voice);
        CHECK(conv.input_rate() == 16000);
        CHECK(conv.resampling());
        const std::size_t flushed = 48000 - first.size();
        // The new stream, after the old one's tail, is the same as a fresh
        // converter's: nothing of the 8000 history leaks into it.
        wg::PhoneRateConverter fresh;
        const auto clean = fresh.convert(16000, voice);
        REQUIRE(got.size() == flushed + clean.size());
        CHECK(std::equal(clean.begin(), clean.end(), got.begin() + static_cast<std::ptrdiff_t>(flushed)));
    }
}

TEST_CASE("rate converter: 8000 voice through Opus decodes on pitch", "[webgate][audio-rate][opus]") {
    auto enc = wg::LiveOpusEncoder::create(wg::kPhoneAudioRate);
    REQUIRE(enc.has_value());
    wg::PhoneRateConverter conv;
    const auto in = tone(8000, 8000);
    const auto at48 = convert_chunked(conv, 8000, in);
    (*enc)->push(at48);
    const auto packets = (*enc)->take_packets();
    // One second of voice is 50 packets bar the half filter still held back.
    CHECK(packets.size() >= 49);
    CHECK((*enc)->ok());
    const auto out = decode_all(packets);
    CHECK(std::abs(frequency_of(out, 2 * 960, out.size()) - kToneHz) < 10.0);
}

TEST_CASE("rate converter: HLS takes 8000 voice", "[webgate][audio-rate][hls]") {
    wg::HlsSegmenter segmenter(wg::kPhoneAudioRate);
    REQUIRE(segmenter.ok());
    wg::PhoneRateConverter conv;
    // Three seconds, the middle one silence, as a DV channel is between calls.
    std::vector<float> in = tone(8000, 8000);
    in.resize(16000, 0.0F);
    const auto tail = tone(8000, 8000, 16000);
    in.insert(in.end(), tail.begin(), tail.end());
    const auto at48 = convert_chunked(conv, 8000, in);
    segmenter.push(at48);
    CHECK(segmenter.ok());
    const auto playlist = segmenter.playlist();
    CHECK(playlist.find("#EXTINF") != std::string::npos);
    CHECK(segmenter.segment(0).has_value());
    CHECK(segmenter.segment(1).has_value());
}
