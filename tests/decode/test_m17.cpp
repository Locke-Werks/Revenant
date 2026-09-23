// M17: the codes against the specification's own tables and test vectors,
// and the decoder against the transmitter in core/dsp/synth/m17_mod.h.
//
// The two groups tests/decode/CMakeLists.txt describes. Where the document
// prints a table or a worked example, the case checks the implementation
// regenerates it from the rule rather than holding a copy of it.
//
// Error rates are measured and printed and the assertions around them are
// loose, as everywhere in this directory.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "core/decode/m17.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/m17_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 48000;

std::vector<std::array<std::uint8_t, 16>> payloads(std::size_t frames, std::uint64_t seed) {
    std::vector<std::array<std::uint8_t, 16>> out(frames);
    std::uint64_t state = seed;
    for (auto& frame : out) {
        for (std::uint8_t& byte : frame) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            byte = static_cast<std::uint8_t>(state >> 45U);
        }
    }
    return out;
}

siggen::M17StreamMessage sample_message(std::size_t frames) {
    siggen::M17StreamMessage message;
    message.destination = decode::m17_encode_callsign("M17-M17 C").value();
    message.source = decode::m17_encode_callsign("SP5WWP").value();
    // Table 3.7: stream, voice, no encryption, channel access number 5.
    message.type = static_cast<std::uint16_t>(0x0005U | (5U << 7U));
    for (std::size_t i = 0; i < message.meta.size(); ++i) {
        message.meta[i] = static_cast<std::uint8_t>(0xA0 + i);
    }
    message.payloads = payloads(frames, 0x4D17'0001ULL);
    return message;
}

// Silence after the end marker, so the receiver's filters and timing windows
// can finish with it, as the noise after a real transmission would let them.
void pad(std::vector<siggen::Complex32>& samples) {
    samples.insert(samples.end(), static_cast<std::size_t>(kRate / 5),
                   siggen::Complex32{1.0F, 0.0F});
}

std::vector<decode::M17Frame> decode_all(std::span<const siggen::Complex32> samples,
                                         std::size_t block) {
    auto decoder = decode::M17::create(decode::M17Config{});
    INFO((decoder.has_value() ? std::string{} : decoder.error().message));
    REQUIRE(decoder.has_value());
    std::vector<decode::M17Frame> frames;
    for (std::size_t start = 0; start < samples.size(); start += block) {
        const std::size_t length = std::min(block, samples.size() - start);
        REQUIRE(decoder->process(samples.subspan(start, length), frames).has_value());
    }
    return frames;
}

}  // namespace

// ---------------------------------------------------------------------------
// The codes, against the document
// ---------------------------------------------------------------------------

TEST_CASE("the M17 CRC reproduces Table 2.6", "[decode][m17]") {
    CHECK(decode::m17_crc({}) == 0xFFFF);
    const std::string a = "A";
    CHECK(decode::m17_crc(std::span(reinterpret_cast<const std::uint8_t*>(a.data()), a.size())) ==
          0x206E);
    const std::string digits = "123456789";
    CHECK(decode::m17_crc(std::span(reinterpret_cast<const std::uint8_t*>(digits.data()),
                                    digits.size())) == 0x772B);
    std::array<std::uint8_t, 256> all{};
    for (std::size_t i = 0; i < all.size(); ++i) {
        all[i] = static_cast<std::uint8_t>(i);
    }
    CHECK(decode::m17_crc(all) == 0x1C31);

    // 2.6: "a CRC computed over the entire 30-byte LSF frame, including a
    // valid CRC field, will always equal zero."
    const std::array<std::uint8_t, 14> meta{};
    const auto lsf = decode::m17_lsf_bytes(1234, 5678, 0x0005, meta);
    CHECK(decode::m17_crc(lsf) == 0);
}

TEST_CASE("M17 addresses follow Appendix A", "[decode][m17]") {
    // A.2's worked example.
    CHECK(decode::m17_encode_callsign("AB1CD").value() == 0x9FDD51ULL);
    // "the calculated address of 'ABC' is the same as 'ABC ', or 'ABC  '".
    CHECK(decode::m17_encode_callsign("ABC").value() ==
          decode::m17_encode_callsign("ABC  ").value());
    CHECK(decode::m17_encode_callsign("abc").value() ==
          decode::m17_encode_callsign("ABC").value());
    CHECK(!decode::m17_encode_callsign("TOOLONGCALL").has_value());

    const auto decoded = decode::m17_decode_address(0x9FDD51ULL);
    CHECK(decoded.kind == decode::M17AddressKind::Standard);
    CHECK(decoded.callsign == "AB1CD");
    CHECK(decode::m17_decode_address(decode::m17_encode_callsign("M17-M17 C").value()).callsign ==
          "M17-M17 C");
    CHECK(decode::m17_decode_address(decode::m17_encode_callsign("W1/X.Y").value()).callsign ==
          "W1/X.Y");

    // Table A.2's ranges, including "........." as the last standard address.
    CHECK(decode::m17_encode_callsign(".........").value() == 0xEE6B27FFFFFFULL);
    CHECK(decode::m17_decode_address(0).kind == decode::M17AddressKind::Reserved);
    CHECK(decode::m17_decode_address(0xEE6B28000000ULL).kind == decode::M17AddressKind::Extended);
    CHECK(decode::m17_decode_address(0xFFFFFFFFFFFFULL).kind == decode::M17AddressKind::Broadcast);
}

TEST_CASE("M17 symbols follow Table 1.1 and the sync bursts Table 2.3", "[decode][m17]") {
    // 1.2's worked example: 0xB4 is sent as -1, -3, +3, +1.
    const auto b4 = decode::m17_word_symbols(0xB4B4);
    CHECK(b4[0] == -1.0F);
    CHECK(b4[1] == -3.0F);
    CHECK(b4[2] == 3.0F);
    CHECK(b4[3] == 1.0F);

    using Row = std::array<float, 8>;
    CHECK(decode::m17_word_symbols(decode::kM17SyncLsf) == Row{3, 3, 3, 3, -3, -3, 3, -3});
    CHECK(decode::m17_word_symbols(decode::kM17SyncBert) == Row{-3, 3, -3, -3, 3, 3, 3, 3});
    CHECK(decode::m17_word_symbols(decode::kM17SyncStream) == Row{-3, -3, -3, -3, 3, 3, -3, 3});
    CHECK(decode::m17_word_symbols(decode::kM17SyncPacket) == Row{3, -3, 3, 3, -3, -3, -3, -3});
    // 1.4.5: 0x555D is +3, +3, +3, +3, +3, +3, -3, +3.
    CHECK(decode::m17_word_symbols(decode::kM17EotWord) == Row{3, 3, 3, 3, 3, 3, -3, 3});
}

TEST_CASE("the M17 Golay code regenerates Appendix D's printed matrix", "[decode][m17]") {
    // (D.1), the P half of G = [I12 | P], one row per data bit from the most
    // significant: eleven check bits then the parity bit.
    constexpr std::array<std::uint32_t, 12> kPrinted = {
        0xC75, 0x63B, 0xF68, 0x7B4, 0x3DA, 0xD99, 0x6CD, 0x367, 0xDC6, 0xA97, 0x93E, 0x8EB,
    };
    for (std::size_t row = 0; row < 12; ++row) {
        const auto data = static_cast<std::uint16_t>(0x800U >> row);
        const std::uint32_t word = decode::m17_golay_encode(data);
        INFO("row " << row + 1);
        CHECK((word >> 12U) == data);
        CHECK((word & 0xFFFU) == kPrinted[row]);
    }

    // The extended Golay code's minimum distance is 8, which is what the
    // three-error correction below rests on.
    std::uint32_t lightest = 24;
    for (std::uint16_t data = 1; data < 4096; ++data) {
        lightest = std::min<std::uint32_t>(
            lightest, static_cast<std::uint32_t>(std::popcount(decode::m17_golay_encode(data))));
    }
    CHECK(lightest == 8);

    // Three errors in any positions are corrected.
    const std::uint32_t word = decode::m17_golay_encode(0xA5C);
    std::array<float, 24> soft{};
    for (std::size_t i = 0; i < 24; ++i) {
        const bool one = ((word >> (23 - i)) & 1U) != 0U;
        soft[i] = one ? -1.0F : 1.0F;
    }
    soft[0] = -soft[0];
    soft[11] = -soft[11];
    soft[23] = -soft[23];
    const auto decoded = decode::m17_golay_decode(soft);
    CHECK(decoded.data == 0xA5C);
    CHECK(decoded.corrected_bits == 3);
}

TEST_CASE("the M17 interleaver and puncturers are Appendices E and F", "[decode][m17]") {
    // Appendix F's table, from its first rows.
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 13> printed = {{
        {0, 0}, {1, 137}, {2, 90}, {3, 227}, {4, 180}, {5, 317}, {6, 270},
        {92, 92}, {93, 229}, {184, 184}, {185, 321}, {277, 45}, {278, 366},
    }};
    for (const auto& [input, output] : printed) {
        INFO("input " << input);
        CHECK(decode::m17_interleave(input) == output);
    }
    std::set<std::uint32_t> seen;
    for (std::uint32_t i = 0; i < 368; ++i) {
        CHECK(decode::m17_interleave(decode::m17_interleave(i)) == i);
        seen.insert(decode::m17_interleave(i));
    }
    CHECK(seen.size() == 368);

    // Appendix E's linearised P1 and its count.
    const auto p1 = decode::m17_puncture_p1();
    const std::array<std::uint8_t, 12> head = {1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1};
    CHECK(std::equal(head.begin(), head.end(), p1.begin()));
    CHECK(std::count(p1.begin(), p1.end(), std::uint8_t{1}) == 46);
    CHECK(std::count(decode::kM17PunctureP2.begin(), decode::kM17PunctureP2.end(),
                     std::uint8_t{1}) == 11);
}

TEST_CASE("the M17 LSF and stream frame survive their FEC chains", "[decode][m17]") {
    const std::array<std::uint8_t, 14> meta = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
    const auto lsf = decode::m17_lsf_bytes(decode::m17_encode_callsign("ECHO").value(),
                                           decode::m17_encode_callsign("N0CALL").value(), 0x0005,
                                           meta);
    auto bits = decode::m17_encode_lsf(lsf);
    REQUIRE(bits.has_value());
    REQUIRE(bits->size() == 368);

    // Twelve errors spread by the interleaver are within what the code
    // corrects.
    std::vector<float> soft(368);
    for (std::size_t i = 0; i < 368; ++i) {
        soft[i] = (*bits)[i] ? -1.0F : 1.0F;
    }
    for (std::size_t i = 0; i < 12; ++i) {
        soft[i * 29 + 3] = -soft[i * 29 + 3];
    }
    auto decoded = decode::m17_decode_lsf(soft);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == lsf);
    const auto parsed = decode::m17_parse_lsf(*decoded);
    CHECK(parsed.crc_valid);
    CHECK(parsed.destination.callsign == "ECHO");
    CHECK(parsed.source.callsign == "N0CALL");
    CHECK(parsed.type.stream);
    CHECK(parsed.type.data_type == 2);

    const std::array<std::uint8_t, 5> chunk = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
    const auto payload = payloads(1, 7).front();
    auto frame_bits = decode::m17_encode_stream_frame(chunk, 4, 0x8123, payload);
    REQUIRE(frame_bits.has_value());
    std::vector<float> frame_soft(368);
    for (std::size_t i = 0; i < 368; ++i) {
        frame_soft[i] = (*frame_bits)[i] ? -1.0F : 1.0F;
    }
    for (std::size_t i = 0; i < 8; ++i) {
        frame_soft[i * 41 + 5] = -frame_soft[i * 41 + 5];
    }
    // Any single bit wrong with full confidence is corrected, wherever it
    // lands.
    std::size_t single_failures = 0;
    for (std::size_t i = 0; i < 368; ++i) {
        std::vector<float> one(368);
        for (std::size_t j = 0; j < 368; ++j) {
            one[j] = (*frame_bits)[j] ? -1.0F : 1.0F;
        }
        one[i] = -3.0F * one[i];
        auto got = decode::m17_decode_stream_frame(one);
        REQUIRE(got.has_value());
        if (got->payload != payload || got->frame_number != 0x0123) {
            ++single_failures;
            WARN("single error at " << i << " not corrected");
        }
    }
    CHECK(single_failures == 0);

    auto frame = decode::m17_decode_stream_frame(frame_soft);
    REQUIRE(frame.has_value());
    CHECK(frame->lich_chunk == chunk);
    CHECK(frame->lich_count == 4);
    CHECK(frame->frame_number == 0x0123);
    CHECK(frame->last);
    CHECK(frame->payload == payload);
}

// ---------------------------------------------------------------------------
// The receiver against the transmitter
// ---------------------------------------------------------------------------

TEST_CASE("an M17 stream round trips through 4FSK", "[decode][m17]") {
    const auto message = sample_message(20);
    siggen::M17ModConfig mod;
    auto samples = siggen::m17_render_stream(mod, message);
    REQUIRE(samples.has_value());

    SECTION("as sent") {}
    SECTION("250 Hz off frequency") {
        siggen::FrequencyConfig offset;
        offset.doppler_shift_hz = 250;
        REQUIRE(siggen::apply_frequency_offset(*samples, offset, kRate, 1).has_value());
    }
    SECTION("with the discriminator inverted") {
        for (auto& sample : *samples) {
            sample = std::conj(sample);
        }
    }
    pad(*samples);

    const auto frames = decode_all(*samples, 4000);
    REQUIRE(frames.size() == 22);
    REQUIRE(frames.front().kind == decode::M17FrameKind::LinkSetup);
    REQUIRE(frames.front().lsf.has_value());
    const auto& lsf = *frames.front().lsf;
    CHECK(lsf.crc_valid);
    CHECK(lsf.destination.callsign == "M17-M17 C");
    CHECK(lsf.source.callsign == "SP5WWP");
    CHECK(lsf.type.stream);
    CHECK(lsf.type.channel_access_number == 5);
    CHECK(lsf.meta == message.meta);

    for (std::size_t i = 0; i < 20; ++i) {
        const auto& frame = frames[1 + i];
        INFO("stream frame " << i);
        REQUIRE(frame.kind == decode::M17FrameKind::Stream);
        REQUIRE(frame.stream.has_value());
        CHECK(frame.stream->frame_number == i);
        CHECK(frame.stream->last == (i == 19));
        CHECK(frame.stream->lich_count == i % 6);
        CHECK(frame.stream->payload == message.payloads[i]);
    }
    CHECK(frames.back().kind == decode::M17FrameKind::EndOfTransmission);
}

TEST_CASE("an M17 stream round trips at 24 kHz", "[decode][m17]") {
    // Five samples a symbol, half the 1.3 recommendation, which is as low as
    // the transmitter's integer upsampling and the timing interpolator
    // comfortably go.
    constexpr dsp::SampleRate kLowRate = 24000;
    const auto message = sample_message(12);
    siggen::M17ModConfig mod;
    mod.rate = kLowRate;
    auto samples = siggen::m17_render_stream(mod, message);
    REQUIRE(samples.has_value());
    samples->insert(samples->end(), static_cast<std::size_t>(kLowRate / 5),
                    siggen::Complex32{1.0F, 0.0F});

    decode::M17Config config;
    config.rate = kLowRate;
    auto decoder = decode::M17::create(config);
    REQUIRE(decoder.has_value());
    std::vector<decode::M17Frame> frames;
    REQUIRE(decoder->process(*samples, frames).has_value());
    REQUIRE(frames.size() == 14);
    REQUIRE(frames.front().lsf.has_value());
    CHECK(frames.front().lsf->crc_valid);
    for (std::size_t i = 0; i < 12; ++i) {
        INFO("stream frame " << i);
        REQUIRE(frames[1 + i].stream.has_value());
        CHECK(frames[1 + i].stream->payload == message.payloads[i]);
    }
}

TEST_CASE("an M17 receiver that joins late rebuilds the LSF from the LICH", "[decode][m17]") {
    const auto message = sample_message(20);
    siggen::M17ModConfig mod;
    auto samples = siggen::m17_render_stream(mod, message);
    REQUIRE(samples.has_value());
    pad(*samples);

    // Start listening part way into the third stream frame: the preamble, the
    // LSF and two stream frames are gone.
    const std::size_t samples_per_frame = 192 * 10;
    const std::size_t start = (192 + 192) * 10 + 2 * samples_per_frame + 700;
    const std::span<const siggen::Complex32> late(samples->data() + start, samples->size() - start);
    const auto frames = decode_all(late, 3000);

    REQUIRE(!frames.empty());
    std::size_t first_number = 0;
    bool rebuilt = false;
    for (const auto& frame : frames) {
        if (frame.kind == decode::M17FrameKind::Stream && frame.stream && first_number == 0) {
            first_number = frame.stream->frame_number;
        }
        if (frame.lsf && frame.lsf_from_lich) {
            rebuilt = true;
            CHECK(frame.lsf->crc_valid);
            CHECK(frame.lsf->source.callsign == "SP5WWP");
            CHECK(frame.lsf->destination.callsign == "M17-M17 C");
            // Six consecutive chunks, so no sooner than the sixth frame heard.
            INFO("rebuilt at frame " << frame.stream->frame_number << ", first heard "
                                     << first_number);
            CHECK(frame.stream->frame_number >= first_number + 5);
        }
    }
    CHECK(first_number == 3);
    CHECK(rebuilt);
}

TEST_CASE("the M17 receiver does not depend on how its input is blocked", "[decode][m17]") {
    const auto message = sample_message(8);
    auto samples = siggen::m17_render_stream(siggen::M17ModConfig{}, message);
    REQUIRE(samples.has_value());
    pad(*samples);
    REQUIRE(siggen::add_awgn(*samples, siggen::NoiseLevel::snr_in_reference_bandwidth_db(8.0, 9000),
                             kRate, 0x4D17'B10CULL)
                .has_value());
    const auto whole = decode_all(*samples, samples->size());
    const auto small = decode_all(*samples, 777);
    REQUIRE(whole.size() == small.size());
    for (std::size_t i = 0; i < whole.size(); ++i) {
        CHECK(whole[i].kind == small[i].kind);
        CHECK(whole[i].first_sample == small[i].first_sample);
        CHECK(whole[i].stream.has_value() == small[i].stream.has_value());
        if (whole[i].stream && small[i].stream) {
            CHECK(whole[i].stream->payload == small[i].stream->payload);
        }
    }
}

TEST_CASE("M17 error rates against noise, measured", "[decode][m17]") {
    // Signal to noise in the 9 kHz channel bandwidth M17 1.1 names. Measured
    // on 2026-09-22 with these seeds, 9392 raw symbols and 100 stream frames
    // per point:
    //
    //   20 dB: SER 0,       stream FER 0,    LSF decoded
    //   16 dB: SER 1.1e-4,  stream FER 0,    LSF decoded
    //   14 dB: SER 1.7e-3,  stream FER 0,    LSF decoded
    //   12 dB: SER 1.0e-2,  stream FER 0.01, LSF lost
    //   10 dB: SER 6.2e-2,  stream FER 0.18, LSF lost
    //    8 dB: SER 0.21,    stream FER 0.99
    //
    // One LSF per point, so "lost" is one sample and says only that the LSF's
    // P1 puncturing, rate 0.66 against the stream's 0.53, gives out first.
    // The allowances are loose on purpose.
    struct Point {
        double snr_db;
        double allowed_frame_error_rate;
    };
    const Point points[] = {{20.0, 0.0}, {10.0, 0.4}};

    // Raw symbols first, no framing, for the symbol error rate the codes
    // start from.
    std::vector<int> symbols(9600);
    std::uint64_t state = 0x5E'4D17ULL;
    for (int& symbol : symbols) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        symbol = decode::kM17DibitToSymbol[(state >> 50U) & 3U];
    }
    const auto message = sample_message(100);

    for (const Point& point : points) {
        const auto level = siggen::NoiseLevel::snr_in_reference_bandwidth_db(point.snr_db, 9000);

        auto raw = siggen::m17_render_symbols(siggen::M17ModConfig{}, symbols);
        REQUIRE(raw.has_value());
        REQUIRE(siggen::add_awgn(*raw, level, kRate, 0x5EED'0017ULL).has_value());
        auto decoder = decode::M17::create(decode::M17Config{});
        REQUIRE(decoder.has_value());
        std::vector<decode::M17Frame> ignored;
        std::vector<float> got;
        for (std::size_t start = 0; start < raw->size(); start += 4800) {
            const std::size_t length = std::min<std::size_t>(4800, raw->size() - start);
            REQUIRE(decoder->process(std::span(*raw).subspan(start, length), ignored).has_value());
            got.insert(got.end(), decoder->last_symbols().begin(), decoder->last_symbols().end());
        }
        std::vector<float> head(256);
        for (std::size_t i = 0; i < head.size(); ++i) {
            head[i] = static_cast<float>(symbols[200 + i]);
        }
        auto hit = decode::correlate_pattern(got, head);
        REQUIRE(hit.has_value());
        std::size_t errors = 0;
        std::size_t compared = 0;
        for (std::size_t i = 0; 200 + i < symbols.size() && hit->offset + i < got.size(); ++i) {
            const float v = got[hit->offset + i];
            const int sliced = v >= 2.0F ? 3 : v >= 0.0F ? 1 : v >= -2.0F ? -1 : -3;
            errors += static_cast<std::size_t>(sliced != symbols[200 + i]);
            ++compared;
        }
        const double ser = static_cast<double>(errors) / static_cast<double>(compared);

        auto samples = siggen::m17_render_stream(siggen::M17ModConfig{}, message);
        REQUIRE(samples.has_value());
        pad(*samples);
        REQUIRE(siggen::add_awgn(*samples, level, kRate, 0x5EED'0018ULL).has_value());
        const auto frames = decode_all(*samples, 8000);
        std::size_t good = 0;
        bool lsf_ok = false;
        for (const auto& frame : frames) {
            if (frame.kind == decode::M17FrameKind::LinkSetup && frame.lsf) {
                lsf_ok = frame.lsf->crc_valid;
            }
            if (frame.stream && frame.stream->frame_number < message.payloads.size() &&
                frame.stream->payload == message.payloads[frame.stream->frame_number]) {
                ++good;
            }
        }
        const double fer = 1.0 - static_cast<double>(good) /
                                     static_cast<double>(message.payloads.size());
        INFO(point.snr_db << " dB in 9 kHz: symbol error rate " << ser << " over " << compared
                          << ", stream frame error rate " << fer << ", LSF CRC valid " << lsf_ok);
        CHECK(compared > 9000);
        CHECK(fer <= point.allowed_frame_error_rate);
        WARN("M17 at " << point.snr_db << " dB in 9 kHz: SER " << ser << " over " << compared
                       << " symbols, stream FER " << fer << ", LSF " << (lsf_ok ? "ok" : "lost"));
    }
}
