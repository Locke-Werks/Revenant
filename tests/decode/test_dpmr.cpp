// dPMR: the codes against ETSI TS 102 490's own statements of them, and the
// decoder against the transmitter in core/dsp/synth/dpmr_mod.h.
//
// The documents print no test vector above the sync words, so the coding
// cases check the rules the clauses state (the Table 7.1 rows, the 7.5 output
// order, the 6.1.5 dibit rule against a printed channel code) rather than a
// worked frame. Error rates are measured and printed and the assertions
// around them are loose, as everywhere in this directory.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "core/decode/dpmr.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dpmr_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 48000;

siggen::DpmrMessage sample_message(std::size_t frames, std::uint64_t seed) {
    siggen::DpmrMessage message;
    message.called_id = 0x123456;
    message.own_id = 0xABCDEF;
    message.mode = 1;
    message.slow_data = 0x2A5A5;
    message.end.end_type = 1;
    message.end.status = 19;
    message.payload.resize(frames);
    std::uint64_t state = seed;
    for (auto& frame : message.payload) {
        for (auto& block : frame) {
            for (std::uint8_t& byte : block) {
                state = state * 6364136223846793005ULL + 1442695040888963407ULL;
                byte = static_cast<std::uint8_t>(state >> 45U);
            }
        }
    }
    return message;
}

std::vector<siggen::Complex32> render(const siggen::DpmrMessage& message) {
    auto samples = siggen::dpmr_render(siggen::DpmrModConfig{}, message);
    REQUIRE(samples.has_value());
    // Silence either side, so the filters and timing windows settle and
    // finish as they would on air.
    std::vector<siggen::Complex32> out(static_cast<std::size_t>(kRate / 10),
                                       siggen::Complex32{1.0F, 0.0F});
    out.insert(out.end(), samples->begin(), samples->end());
    out.insert(out.end(), static_cast<std::size_t>(kRate / 5), siggen::Complex32{1.0F, 0.0F});
    return out;
}

std::vector<decode::DpmrFrame> decode_all(std::span<const siggen::Complex32> samples,
                                          std::size_t block) {
    auto decoder = decode::Dpmr::create(decode::DpmrConfig{});
    REQUIRE(decoder.has_value());
    std::vector<decode::DpmrFrame> frames;
    for (std::size_t i = 0; i < samples.size(); i += block) {
        const std::size_t n = std::min(block, samples.size() - i);
        REQUIRE(decoder->process(samples.subspan(i, n), frames).has_value());
    }
    return frames;
}

struct Tally {
    bool header = false;
    bool end = false;
    std::size_t payloads = 0;
    std::size_t cch_good = 0;
    std::size_t tch_bit_errors = 0;
    std::size_t ids = 0;
};

Tally tally(const std::vector<decode::DpmrFrame>& frames, const siggen::DpmrMessage& message) {
    Tally t;
    std::size_t index = 0;
    for (const auto& frame : frames) {
        if (frame.kind == decode::DpmrFrameKind::Header && frame.header &&
            frame.header->called_id == message.called_id &&
            frame.header->own_id == message.own_id) {
            t.header = true;
        }
        if (frame.kind == decode::DpmrFrameKind::End && frame.end && frame.end->crc_valid &&
            frame.end->status == message.end.status) {
            t.end = true;
        }
        if (frame.kind != decode::DpmrFrameKind::Payload) {
            continue;
        }
        ++t.payloads;
        if (frame.cch && frame.cch->crc_valid) {
            ++t.cch_good;
            index = frame.cch->frame_number + (index / 4) * 4;
        }
        if (frame.called_id == message.called_id && frame.own_id == message.own_id) {
            ++t.ids;
        }
        if (index < message.payload.size()) {
            for (std::size_t b = 0; b < 4; ++b) {
                for (std::size_t i = 0; i < 9; ++i) {
                    t.tch_bit_errors += static_cast<std::size_t>(std::popcount(
                        static_cast<unsigned>(frame.tch[b][i] ^ message.payload[index][b][i])));
                }
            }
        }
        ++index;
    }
    return t;
}

}  // namespace

TEST_CASE("the dPMR Hamming code is TS 102 490 Table 7.1", "[decode][dpmr]") {
    // Each row of the generator matrix is the code word of one data bit.
    const std::array<std::uint16_t, 8> rows = {
        0b100000001110, 0b010000000111, 0b001000001010, 0b000100000101,
        0b000010001011, 0b000001001100, 0b000000100110, 0b000000010011,
    };
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(decode::dpmr_hamming_encode(static_cast<std::uint8_t>(0x80U >> i)) == rows[i]);
    }
    // Every single error in every word is corrected, and the code is linear.
    for (unsigned data = 0; data < 256; ++data) {
        const std::uint16_t word = decode::dpmr_hamming_encode(static_cast<std::uint8_t>(data));
        CHECK(decode::dpmr_hamming_decode(word).data == data);
        for (unsigned bit = 0; bit < 12; ++bit) {
            const auto d = decode::dpmr_hamming_decode(static_cast<std::uint16_t>(word ^ (1U << bit)));
            CHECK(d.data == data);
            CHECK(d.corrected);
        }
    }
}

TEST_CASE("dPMR sync words, channel codes and interleavers follow clauses 6 and 7",
          "[decode][dpmr]") {
    // 490 6.1: every sync symbol is an outer level.
    for (const float s : decode::dpmr_word_symbols(decode::kDpmrFs1, 48)) {
        CHECK((s == 3.0F || s == -3.0F));
    }
    // 658 6.1 note: FS4 is the symbol-wise complement of FS1.
    const auto fs1 = decode::dpmr_word_symbols(decode::kDpmrFs1, 48);
    const auto fs4 = decode::dpmr_word_symbols(decode::kDpmrFs4, 48);
    for (std::size_t i = 0; i < fs1.size(); ++i) {
        CHECK(fs1[i] == -fs4[i]);
    }
    // 490 Table 6.1, group A, 446.003125 MHz: 577577 hex. 7.1: 0 is 01, 1 is 11.
    const auto code = decode::dpmr_channel_code_value(0x577577U);
    REQUIRE(code.has_value());
    CHECK(*code == 0x145);
    CHECK(decode::dpmr_channel_code_bits(*code) == 0x577577U);
    CHECK_FALSE(decode::dpmr_channel_code_value(0x577574U).has_value());

    // 490 7.5: input [1..72] leaves the transmit interleaver as
    // [1,13,25,37,49,61,2,...]; the 12 x 10 as [1,13,...,109,2,...].
    std::vector<std::uint8_t> index(120);
    for (std::size_t i = 0; i < index.size(); ++i) {
        index[i] = static_cast<std::uint8_t>(i + 1);
    }
    const auto tch = decode::dpmr_interleave(std::span(index).first(72), 6);
    CHECK(std::vector<std::uint8_t>(tch.begin(), tch.begin() + 7) ==
          std::vector<std::uint8_t>{1, 13, 25, 37, 49, 61, 2});
    CHECK(tch.back() == 72);
    const auto hi = decode::dpmr_interleave(index, 10);
    CHECK(hi[9] == 109);
    CHECK(hi[10] == 2);
    CHECK(decode::dpmr_deinterleave(hi, 10) == index);
}

TEST_CASE("the dPMR scrambler is a 511-bit sequence and self-inverse", "[decode][dpmr]") {
    std::vector<std::uint8_t> zeros(1022, 0);
    decode::dpmr_scramble(zeros);
    // 490 7.4: preset all ones, so the first nine outputs are the preset.
    for (std::size_t i = 0; i < 9; ++i) {
        CHECK(zeros[i] == 1);
    }
    for (std::size_t i = 0; i < 511; ++i) {
        CHECK(zeros[i] == zeros[i + 511]);
    }
    std::vector<std::uint8_t> bits(72);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        bits[i] = static_cast<std::uint8_t>((i * 7) % 3 == 0);
    }
    auto copy = bits;
    decode::dpmr_scramble(copy);
    CHECK(copy != bits);
    decode::dpmr_scramble(copy);
    CHECK(copy == bits);
}

TEST_CASE("dPMR CCH, HI and END fields survive their coding chains", "[decode][dpmr]") {
    decode::DpmrCch cch;
    cch.frame_number = 2;
    cch.id_part = 0xABC;
    cch.mode = 1;
    cch.format = 1;
    cch.slow_data = 0x3FFFF;
    auto bits = decode::dpmr_encode_cch(cch);
    REQUIRE(bits.size() == 72);
    // One error in each of the six Hamming words, after de-interleaving.
    for (std::size_t w = 0; w < 6; ++w) {
        bits[w] ^= 1U;  // row 0 of the 12 x 6 holds bit 0 of each word
    }
    auto back = decode::dpmr_decode_cch(bits);
    CHECK(back.crc_valid);
    CHECK(back.corrected_words == 6);
    CHECK(back.frame_number == 2);
    CHECK(back.id_part == 0xABC);
    CHECK(back.slow_data == 0x3FFFF);

    decode::DpmrHeaderInfo hi;
    hi.header_type = 0b0010;
    hi.called_id = decode::dpmr_isf_address(255);
    hi.own_id = 0x00F00D;
    hi.call_info = 0x5A5;
    const auto hb = decode::dpmr_encode_hi(hi);
    REQUIRE(hb.size() == 120);
    const auto hback = decode::dpmr_decode_hi(hb);
    CHECK(hback.crc_valid);
    CHECK(hback.called_id == 0xFFFFFF);
    CHECK(hback.own_id == 0x00F00D);
    CHECK(hback.call_info == 0x5A5);

    decode::DpmrEnd end;
    end.end_type = 1;
    end.arq = 1;
    end.wait = 4;
    end.status = 31;
    auto eb = decode::dpmr_encode_end(end);
    REQUIRE(eb.size() == 72);
    // Ruin the first copy; the second carries it.
    for (std::size_t i = 0; i < 36; i += 2) {
        eb[i] ^= 1U;
    }
    const auto eback = decode::dpmr_decode_end(eb);
    CHECK(eback.crc_valid);
    CHECK(eback.status == 31);
    CHECK(eback.wait == 4);
}

TEST_CASE("a dPMR transmission round trips through 4FSK", "[decode][dpmr]") {
    const auto message = sample_message(8, 0xD9A1);
    auto samples = render(message);
    bool invert = false;
    SECTION("as sent") {}
    SECTION("with the discriminator inverted") {
        invert = true;
        for (auto& s : samples) {
            s = std::conj(s);
        }
    }
    SECTION("200 Hz off frequency") {
        double phase = 0.0;
        for (auto& s : samples) {
            s *= siggen::Complex32{static_cast<float>(std::cos(phase)),
                                   static_cast<float>(std::sin(phase))};
            phase += 2.0 * 3.14159265358979 * 200.0 / kRate;
        }
    }
    const auto frames = decode_all(samples, samples.size());
    const Tally t = tally(frames, message);
    CHECK(t.header);
    CHECK(t.end);
    CHECK(t.payloads == 8);
    CHECK(t.cch_good == 8);
    CHECK(t.ids == 2);
    CHECK(t.tch_bit_errors == 0);
    for (const auto& frame : frames) {
        CHECK(frame.inverted == invert);
        if (frame.kind == decode::DpmrFrameKind::Payload) {
            CHECK(frame.voice);
        }
        if (frame.channel_code) {
            CHECK(*frame.channel_code == 0x145);
        }
    }
}

TEST_CASE("a dPMR receiver joins a transmission late on FS2", "[decode][dpmr]") {
    const auto message = sample_message(12, 0x1A7E);
    auto samples = render(message);
    // Drop the header and the first frame: 100 ms of lead, 36 + 156 header
    // symbols and one 192-symbol frame, plus a little.
    const std::size_t sps = 20;
    const std::size_t cut = static_cast<std::size_t>(kRate / 10) + (36 + 156 + 192 + 30) * sps;
    std::vector<siggen::Complex32> late(samples.begin() + static_cast<std::ptrdiff_t>(cut),
                                        samples.end());
    const auto frames = decode_all(late, late.size());
    const Tally t = tally(frames, message);
    CHECK_FALSE(t.header);
    CHECK(t.end);
    // Frame 2 has no FS2, so the first frame joined is frame 3 of the first
    // superframe: ten of the twelve.
    CHECK(t.payloads == 10);
    CHECK(t.ids == 2);
}

TEST_CASE("the dPMR receiver does not depend on how its input is blocked", "[decode][dpmr]") {
    const auto message = sample_message(8, 0xB10C);
    auto samples = render(message);
    REQUIRE(siggen::add_awgn(samples, siggen::NoiseLevel::snr_in_reference_bandwidth_db(10.0, 4000),
                             kRate, 0xD9'B10CULL)
                .has_value());
    const auto whole = decode_all(samples, samples.size());
    const auto small = decode_all(samples, 777);
    REQUIRE(whole.size() == small.size());
    for (std::size_t i = 0; i < whole.size(); ++i) {
        CHECK(whole[i].kind == small[i].kind);
        CHECK(whole[i].first_sample == small[i].first_sample);
        CHECK(whole[i].tch == small[i].tch);
    }
}

TEST_CASE("dPMR error rates against noise, measured", "[decode][dpmr]") {
    const auto message = sample_message(16, 0x5EED);
    for (const double snr : {20.0, 12.0, 9.0, 6.0}) {
        auto samples = render(message);
        REQUIRE(siggen::add_awgn(samples, siggen::NoiseLevel::snr_in_reference_bandwidth_db(snr, 4000),
                                 kRate, 0x5EED'0490ULL)
                    .has_value());
        const auto frames = decode_all(samples, 4096);
        const Tally t = tally(frames, message);
        std::printf("dPMR %4.1f dB in 4 kHz: header %d end %d payload %zu/16 cch %zu tch BER %.4f\n",
                    snr, t.header ? 1 : 0, t.end ? 1 : 0, t.payloads, t.cch_good,
                    static_cast<double>(t.tch_bit_errors) / (16.0 * 288.0));
        if (snr >= 12.0) {
            CHECK(t.header);
            CHECK(t.cch_good == 16);
        }
    }
}

TEST_CASE("dPMR does not start on noise", "[decode][dpmr]") {
    // Five minutes of complex noise, no signal.
    std::vector<siggen::Complex32> noise(static_cast<std::size_t>(kRate) * 300,
                                         siggen::Complex32{0.0F, 0.0F});
    REQUIRE(siggen::add_awgn_at_power(noise, 1.0, 0x4015E).has_value());
    const auto frames = decode_all(noise, 48000);
    std::size_t starts = 0;
    for (const auto& frame : frames) {
        if (frame.kind != decode::DpmrFrameKind::Payload) {
            ++starts;
        }
    }
    std::printf("dPMR on 300 s of noise: %zu frames, %zu headers or ends\n", frames.size(),
                starts);
    CHECK(frames.empty());
}
