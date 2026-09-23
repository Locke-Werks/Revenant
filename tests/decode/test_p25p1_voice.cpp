// P25 Phase 1 voice: the Logical Link Data Units, Link Control, encryption
// sync and IMBE frames, against the transmitter and against the vocoder.
//
// WHAT THE VOICE FRAMES ARE
//
// TIA-102.BABA prints no test vector for the decode direction and this project
// has no IMBE encoder, so the frames carried here are built the way
// tests/decode/test_imbe.cpp builds its own: quantizer values chosen by the
// test, arranged into bit vectors by section 7.1 and coded by sections 7.3 to
// 7.5 through imbe_pack_frame. That is the standard's own parameter encoding
// with the analysis half left out, and it is enough to say what this file
// needs to say: the 144 bits that go into an LDU come out of it unchanged, and
// the PCM that comes out of the air interface is the PCM the vocoder makes of
// the frames that went in, sample for sample.
//
// Error rates are measured and printed, and asserted loosely, for the reason
// test_p25p1.cpp gives at its head.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <print>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "core/decode/dv_codes.h"
#include "core/decode/imbe.h"
#include "core/decode/p25p1.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dv_mod.h"
#include "tests/decode/imbe_test_frames.h"

using namespace revenant;
using decode::P25Duid;

namespace {

constexpr dsp::SampleRate kRate = 48'000;
constexpr std::uint16_t kNac = 0x293;
constexpr std::uint16_t kTalkgroup = 0x02A7;
constexpr std::uint32_t kSource = 0x0012'D687;

using VoiceFrame = std::array<std::uint8_t, decode::kP25VoiceFrameBits>;

// `count` IMBE channel frames from seeded quantizer values: the pitch index
// walks the 0..207 range the encoder uses, voicing alternates in runs, and the
// spectral bits are drawn at random, so no two consecutive frames are alike.
std::vector<VoiceFrame> voice_frames(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<VoiceFrame> out;
    out.reserve(count);
    for (std::size_t f = 0; f < count; ++f) {
        decode::imbe_test::Quantizers q = decode::imbe_test::loud_frame(
            static_cast<std::uint32_t>((f * 23U + 11U) % 208U), (f / 3U) % 2U == 0U,
            static_cast<std::uint32_t>(30U + rng() % 30U));
        for (std::uint32_t& value : q.spectral) {
            value = static_cast<std::uint32_t>(rng() & 0xFFFFU);
        }
        q.sync = static_cast<std::uint32_t>(f & 1U);
        const std::vector<std::uint8_t> bits =
            decode::imbe_test::channel_frame(decode::imbe_test::prioritize(q));
        VoiceFrame frame{};
        std::copy(bits.begin(), bits.end(), frame.begin());
        out.push_back(frame);
    }
    return out;
}

// TIA-102.BAAA-A Figure 5-6, format $00, written out here from the figure
// rather than taken from the decoder: LCF, MFID, the emergency bit and 15
// reserved bits, TGID, source.
std::array<std::uint8_t, 9> group_link_control(std::uint8_t format, bool emergency,
                                                std::uint16_t talkgroup, std::uint32_t source) {
    return {
        format,
        0x00,
        static_cast<std::uint8_t>(emergency ? 0x80 : 0x00),
        0x00,
        static_cast<std::uint8_t>(talkgroup >> 8U),
        static_cast<std::uint8_t>(talkgroup & 0xFFU),
        static_cast<std::uint8_t>((source >> 16U) & 0xFFU),
        static_cast<std::uint8_t>((source >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(source & 0xFFU),
    };
}

siggen::P25VoiceMessage voice_message(std::size_t frames, bool encrypted, bool with_header) {
    siggen::P25VoiceMessage message;
    message.network_access_code = kNac;
    const std::uint8_t algid = encrypted ? std::uint8_t{0x84} : decode::kP25AlgidUnencrypted;
    if (with_header) {
        decode::P25Header header;
        header.algorithm_id = algid;
        header.key_id = encrypted ? std::uint16_t{0x1234} : std::uint16_t{0};
        header.talkgroup_id = kTalkgroup;
        if (encrypted) {
            for (std::size_t i = 0; i < header.message_indicator.size(); ++i) {
                header.message_indicator[i] = static_cast<std::uint8_t>(0x11 * (i + 1));
            }
        }
        message.header = header;
    }
    message.link_control = group_link_control(0x00, false, kTalkgroup, kSource);
    message.algorithm_id = algid;
    message.key_id = encrypted ? std::uint16_t{0x1234} : std::uint16_t{0};
    if (encrypted) {
        for (std::size_t i = 0; i < message.message_indicator.size(); ++i) {
            message.message_indicator[i] = static_cast<std::uint8_t>(0xA0 + i);
        }
    }
    message.voice = voice_frames(frames, 0x9E37'79B9'0025'0001ULL);
    for (std::size_t i = 0; i < 2 * frames / decode::kP25VoiceFramesPerLdu; ++i) {
        message.low_speed_data.push_back(static_cast<std::uint8_t>(0x41 + i));
    }
    return message;
}

// The vocoder run straight over the frames, with nothing in between.
std::vector<float> reference_pcm(std::span<const VoiceFrame> frames) {
    decode::ImbeDecoder decoder;
    std::vector<float> pcm;
    std::array<float, decode::ImbeDecoder::kPcmFrames> block{};
    for (const VoiceFrame& frame : frames) {
        REQUIRE(decoder.decode(frame, block).has_value());
        pcm.insert(pcm.end(), block.begin(), block.end());
    }
    return pcm;
}

// Pseudorandom dibits after the transmission, standing in for whatever the
// channel carries next. A receiver loses the tail of a capture to its filter
// delay and its timing window, and test_p25p1.cpp says why a test has to put
// something there.
std::vector<std::uint8_t> with_tail(std::vector<std::uint8_t> dibits) {
    std::uint64_t state = 0x7A11'0000'0000'0001ULL;
    for (int i = 0; i < 200; ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        dibits.push_back(static_cast<std::uint8_t>((state >> 33U) & 0x3U));
    }
    return dibits;
}

struct Received {
    std::vector<decode::P25Frame> frames;
    std::vector<float> pcm;
    decode::P25CallState call;
    // Call state as it stood right after the first frame was pushed.
    decode::P25CallState after_first;
};

Received receive(std::span<const std::uint8_t> dibits, double snr_db = 0.0) {
    // Pseudorandom dibits ahead of the transmission as well, so the first
    // data unit is past the receiver's acquisition transient, which
    // test_p25p1.cpp measures on its own. Every case here is about what the
    // data units carry.
    std::vector<std::uint8_t> stream;
    std::uint64_t state = 0x1EAD'0000'0000'0001ULL;
    for (int i = 0; i < 300; ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        stream.push_back(static_cast<std::uint8_t>((state >> 33U) & 0x3U));
    }
    stream.insert(stream.end(), dibits.begin(), dibits.end());

    siggen::P25ModConfig mod;
    mod.rate = kRate;
    auto samples = siggen::p25_render_dibits(mod, stream);
    REQUIRE(samples.has_value());
    if (snr_db > 0.0) {
        const auto level = siggen::NoiseLevel::snr_in_full_sample_rate_db(snr_db);
        auto report = siggen::add_awgn(*samples, level, kRate, 0x51'0025'7E57'0001ULL);
        REQUIRE(report.has_value());
    }

    decode::P25Config config;
    config.rate = kRate;
    auto decoder = decode::P25Phase1::create(config);
    REQUIRE(decoder.has_value());

    Received out;
    REQUIRE(decoder->process(*samples, out.frames).has_value());

    decode::P25Voice voice;
    for (std::size_t i = 0; i < out.frames.size(); ++i) {
        REQUIRE(voice.push(out.frames[i], out.pcm).has_value());
        if (i == 0) {
            out.after_first = voice.call();
        }
    }
    out.call = voice.call();
    return out;
}

// Clause 8.4 at the dibit level: the information dibits of one data unit.
std::vector<std::uint8_t> strip_status(std::span<const std::uint8_t> unit) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < unit.size(); ++i) {
        if (i >= decode::kP25FirstStatusSymbol &&
            (i - decode::kP25FirstStatusSymbol) % decode::kP25StatusSymbolInterval == 0) {
            continue;
        }
        out.push_back(unit[i]);
    }
    return out;
}

}  // namespace

TEST_CASE("Link Control, encryption sync and voice round trip through C4FM",
          "[decode][p25][voice]") {
    const siggen::P25VoiceMessage message = voice_message(36, false, true);
    auto dibits = siggen::p25_voice_message_dibits(message);
    INFO((dibits.has_value() ? std::string{} : dibits.error().message));
    REQUIRE(dibits.has_value());
    REQUIRE(dibits->size() == decode::kP25HduTotalSymbols + 4 * decode::kP25LduSymbols +
                                  decode::kP25SimpleTerminatorSymbols);

    const Received rx = receive(with_tail(*dibits));

    // Clause 5.1 and Figure 5-2: header, LDU1, LDU2, LDU1, LDU2, terminator.
    const P25Duid expected[] = {
        P25Duid::HeaderDataUnit,       P25Duid::LogicalLinkDataUnit1,
        P25Duid::LogicalLinkDataUnit2, P25Duid::LogicalLinkDataUnit1,
        P25Duid::LogicalLinkDataUnit2, P25Duid::TerminatorWithoutLinkControl,
    };
    REQUIRE(rx.frames.size() == std::size(expected));
    for (std::size_t i = 0; i < rx.frames.size(); ++i) {
        INFO("data unit " << i << " is " << decode::p25_duid_name(rx.frames[i].nid.duid));
        CHECK(rx.frames[i].nid.duid == static_cast<std::uint8_t>(expected[i]));
        CHECK(rx.frames[i].nid.network_access_code == kNac);
        CHECK_FALSE(rx.frames[i].code_word_failed);
    }

    REQUIRE(rx.frames[0].header.has_value());
    CHECK(rx.frames[0].header->talkgroup_id == kTalkgroup);

    std::size_t sent_frame = 0;
    std::size_t lsd = 0;
    for (std::size_t i = 1; i <= 4; ++i) {
        const decode::P25Frame& ldu = rx.frames[i];
        INFO("LDU " << i);
        if (ldu.nid.duid == static_cast<std::uint8_t>(P25Duid::LogicalLinkDataUnit1)) {
            REQUIRE(ldu.link_control.has_value());
            const decode::P25LinkControl& lc = *ldu.link_control;
            CHECK(lc.format == decode::kP25LcfGroupVoice);
            CHECK_FALSE(lc.encrypted);
            CHECK_FALSE(lc.emergency);
            CHECK(lc.talkgroup_id == kTalkgroup);
            CHECK(lc.source_id == kSource);
            CHECK_FALSE(lc.destination_id.has_value());
            CHECK(lc.code.rs_corrected == 0);
            CHECK(lc.code.erasures == 0);
            CHECK_FALSE(ldu.encryption_sync.has_value());
        } else {
            REQUIRE(ldu.encryption_sync.has_value());
            const decode::P25EncryptionSync& es = *ldu.encryption_sync;
            CHECK(es.algorithm_id == decode::kP25AlgidUnencrypted);
            CHECK_FALSE(es.encrypted);
            CHECK(es.key_id == 0);
            CHECK(es.message_indicator == std::array<std::uint8_t, 9>{});
            CHECK(es.code.rs_corrected == 0);
            CHECK_FALSE(ldu.link_control.has_value());
        }
        for (const auto& octet : ldu.low_speed_data) {
            REQUIRE(octet.has_value());
            CHECK(*octet == message.low_speed_data[lsd++]);
        }
        REQUIRE(ldu.voice.size() == decode::kP25VoiceFramesPerLdu);
        for (const VoiceFrame& frame : ldu.voice) {
            INFO("voice frame " << (sent_frame + 1));
            CHECK(frame == message.voice[sent_frame]);
            ++sent_frame;
        }
    }

    // The PCM out of the air interface is the vocoder's PCM for the frames
    // that went in, to the last bit, because every frame arrived intact and
    // the vocoder is deterministic.
    const std::vector<float> reference = reference_pcm(message.voice);
    REQUIRE(rx.pcm.size() == message.voice.size() * decode::ImbeDecoder::kPcmFrames);
    CHECK(rx.pcm == reference);

    CHECK(rx.call.talkgroup_id == kTalkgroup);
    CHECK(rx.call.source_id == kSource);
    CHECK(rx.call.encryption == decode::P25CallEncryption::Clear);
    CHECK(rx.call.frames_decoded == 36);
    CHECK(rx.call.frames_withheld == 0);
    CHECK(rx.call.frames_dropped == 0);
    CHECK(rx.call.frames_repeated == 0);
    CHECK(rx.call.frames_muted == 0);
    CHECK_FALSE(rx.call.active);
}

TEST_CASE("the header's Reed-Solomon parity is the standard's, not zero",
          "[decode][p25][rs]") {
    // The 36 hexbits under the header's Golay code must form a code word of
    // the clause 5.9 (36,20,17) code. Before the parity was computed they
    // were the 20 information hexbits and sixteen zeros, which is a code word
    // only when the information is all zero.
    siggen::P25HeaderMessage message;
    message.network_access_code = kNac;
    message.header.talkgroup_id = kTalkgroup;
    message.header.algorithm_id = decode::kP25AlgidUnencrypted;
    auto dibits = siggen::p25_header_message_dibits(message);
    REQUIRE(dibits.has_value());

    const std::vector<std::uint8_t> information =
        strip_status(std::span(*dibits).first(decode::kP25HduTotalSymbols));
    std::vector<std::uint8_t> hexbits;
    const std::size_t body = decode::kP25FrameSyncSymbols + decode::kP25NidSymbols;
    for (std::size_t w = 0; w < decode::kP25HduGolayWords; ++w) {
        std::uint32_t word = 0;
        for (std::size_t s = 0; s < decode::kP25HduSymbolsPerGolayWord; ++s) {
            word = (word << 2U) | information[body + w * decode::kP25HduSymbolsPerGolayWord + s];
        }
        const decode::Golay18Decode decoded = decode::p25_golay18_decode(word);
        CHECK(decoded.corrected_bits == 0);
        hexbits.push_back(decoded.information);
    }
    auto decoded = decode::p25_rs_decode(decode::kP25RsHeader, hexbits);
    REQUIRE(decoded.has_value());
    CHECK(decoded->decoded);
    CHECK(decoded->corrected == 0);
    CHECK(std::any_of(hexbits.begin() + 20, hexbits.end(),
                      [](std::uint8_t hexbit) { return hexbit != 0; }));
}

TEST_CASE("a receiver that joins mid-call has the talkgroup at the first LDU1",
          "[decode][p25][voice]") {
    // No header, and the capture starts halfway through the first LDU2, so
    // the first whole data unit the receiver sees is the second LDU1. Before
    // Link Control was decoded this receiver had the NAC and the DUID
    // sequence there and nothing else.
    const siggen::P25VoiceMessage message = voice_message(54, false, false);
    auto dibits = siggen::p25_voice_message_dibits(message);
    REQUIRE(dibits.has_value());

    const std::size_t cut = decode::kP25LduSymbols + 400;
    const std::vector<std::uint8_t> joined(dibits->begin() + static_cast<std::ptrdiff_t>(cut),
                                           dibits->end());
    const Received rx = receive(with_tail(joined));

    REQUIRE_FALSE(rx.frames.empty());
    const decode::P25Frame& first = rx.frames.front();
    CHECK(first.nid.duid == static_cast<std::uint8_t>(P25Duid::LogicalLinkDataUnit1));
    REQUIRE(first.link_control.has_value());
    CHECK(first.link_control->talkgroup_id == kTalkgroup);
    CHECK(first.link_control->source_id == kSource);

    // And the status surface has it after that one data unit, with the
    // encryption state still open until the LDU2 behind it.
    CHECK(rx.after_first.active);
    CHECK(rx.after_first.network_access_code == kNac);
    CHECK(rx.after_first.talkgroup_id == kTalkgroup);
    CHECK(rx.after_first.encryption == decode::P25CallEncryption::Unknown);

    // Nine frames were held for that LDU2, released when it said the call is
    // in clear, and nothing was lost: the PCM is the vocoder's for frames 19
    // through 54 from a cold start.
    const std::span<const VoiceFrame> heard(message.voice.data() + 18, 36);
    CHECK(rx.call.frames_decoded == 36);
    CHECK(rx.call.frames_dropped == 0);
    CHECK(rx.call.encryption == decode::P25CallEncryption::Clear);
    CHECK(rx.pcm == reference_pcm(heard));
}

TEST_CASE("an encrypted call reports its talkgroup and network and withholds its voice",
          "[decode][p25][voice]") {
    // docs/modes.md: identify what cannot be decoded, never attempt it. The
    // header and the encryption sync both carry ALGID $84, AES per
    // TIA-102.BAAC clause 2.8. Link Control stays in clear, as format $00
    // says it is.
    SECTION("from the header") {
        auto dibits = siggen::p25_voice_message_dibits(voice_message(36, true, true));
        REQUIRE(dibits.has_value());
        const Received rx = receive(with_tail(*dibits));

        REQUIRE_FALSE(rx.frames.empty());
        REQUIRE(rx.frames.front().header.has_value());
        CHECK(rx.frames.front().header->encrypted);
        CHECK(rx.pcm.empty());
        CHECK(rx.call.encryption == decode::P25CallEncryption::Encrypted);
        CHECK(rx.call.algorithm_id == std::optional<std::uint8_t>{0x84});
        CHECK(rx.call.key_id == std::optional<std::uint16_t>{0x1234});
        CHECK(rx.call.talkgroup_id == kTalkgroup);
        CHECK(rx.call.source_id == kSource);
        CHECK(rx.call.network_access_code == kNac);
        CHECK(rx.call.frames_decoded == 0);
        CHECK(rx.call.frames_withheld == 36);
    }

    SECTION("from the encryption sync, joined mid-call") {
        auto dibits = siggen::p25_voice_message_dibits(voice_message(36, true, false));
        REQUIRE(dibits.has_value());
        const Received rx = receive(with_tail(*dibits));

        // The first LDU1 is held until the LDU2 says what the call is, and
        // then withheld with the rest.
        CHECK(rx.after_first.encryption == decode::P25CallEncryption::Unknown);
        CHECK(rx.after_first.talkgroup_id == kTalkgroup);
        CHECK(rx.pcm.empty());
        CHECK(rx.call.encryption == decode::P25CallEncryption::Encrypted);
        CHECK(rx.call.talkgroup_id == kTalkgroup);
        CHECK(rx.call.frames_decoded == 0);
        CHECK(rx.call.frames_withheld == 36);

        bool saw_sync = false;
        for (const decode::P25Frame& frame : rx.frames) {
            if (frame.encryption_sync) {
                saw_sync = true;
                CHECK(frame.encryption_sync->encrypted);
                CHECK(frame.encryption_sync->algorithm_id == 0x84);
                CHECK(frame.encryption_sync->key_id == 0x1234);
                CHECK(frame.encryption_sync->message_indicator[0] == 0xA0);
                CHECK(frame.encryption_sync->message_indicator[8] == 0xA8);
            }
        }
        CHECK(saw_sync);
    }

    SECTION("from Link Control format $80") {
        // TIA-102.BAAC clause 2.2: $80 is format $00 encrypted, so the
        // talkgroup is not in clear and must not be reported from it.
        siggen::P25VoiceMessage message = voice_message(18, false, false);
        message.link_control = group_link_control(0x80, false, 0xBEEF, 0x00ABCDEF);
        auto dibits = siggen::p25_voice_message_dibits(message);
        REQUIRE(dibits.has_value());
        const Received rx = receive(with_tail(*dibits));

        CHECK(rx.pcm.empty());
        CHECK(rx.call.encryption == decode::P25CallEncryption::Encrypted);
        CHECK_FALSE(rx.call.talkgroup_id.has_value());
        for (const decode::P25Frame& frame : rx.frames) {
            if (frame.link_control) {
                CHECK(frame.link_control->encrypted);
                CHECK_FALSE(frame.link_control->talkgroup_id.has_value());
            }
        }
    }
}

TEST_CASE("an LDU's Reed-Solomon word survives the errors clause 5.9 promises",
          "[decode][p25][rs]") {
    // Driven from the transmitter's dibits with no modem in between, so each
    // error is placed exactly. The Link Control word is (24,12,13) under
    // (10,6,3) Hamming words: a single bit error per word is the Hamming
    // code's, a detected double error is an erasure, and a word replaced by
    // another valid Hamming word is an error only the Reed-Solomon code sees.
    const siggen::P25VoiceMessage message = voice_message(18, false, false);
    auto dibits = siggen::p25_voice_message_dibits(message);
    REQUIRE(dibits.has_value());
    const std::vector<std::uint8_t> ldu1 =
        strip_status(std::span(*dibits).first(decode::kP25LduSymbols));
    const std::vector<std::uint8_t> ldu2 = strip_status(
        std::span(*dibits).subspan(decode::kP25LduSymbols, decode::kP25LduSymbols));
    constexpr decode::P25LduLayout kLayout = decode::p25_ldu_layout();

    const auto word_at = [&](const std::vector<std::uint8_t>& unit, std::size_t w) {
        std::uint16_t word = 0;
        for (std::size_t s = 0; s < decode::kP25HammingWordSymbols; ++s) {
            word = static_cast<std::uint16_t>((word << 2U) | unit[kLayout.hamming[w] + s]);
        }
        return word;
    };
    const auto put_word = [&](std::vector<std::uint8_t>& unit, std::size_t w,
                              std::uint16_t word) {
        for (std::size_t s = 0; s < decode::kP25HammingWordSymbols; ++s) {
            unit[kLayout.hamming[w] + s] = static_cast<std::uint8_t>(
                (word >> (2U * (decode::kP25HammingWordSymbols - 1 - s))) & 0x3U);
        }
    };
    const auto decode_unit = [](const std::vector<std::uint8_t>& unit, P25Duid duid) {
        decode::P25Frame frame;
        REQUIRE(decode::p25_decode_ldu(unit, static_cast<std::uint8_t>(duid), frame).has_value());
        return frame;
    };

    SECTION("one bit in every Hamming word") {
        std::vector<std::uint8_t> unit = ldu1;
        for (std::size_t w = 0; w < decode::kP25LduHammingWords; ++w) {
            put_word(unit, w, static_cast<std::uint16_t>(word_at(unit, w) ^ (1U << (w % 10U))));
        }
        const decode::P25Frame frame = decode_unit(unit, P25Duid::LogicalLinkDataUnit1);
        REQUIRE(frame.link_control.has_value());
        CHECK(frame.link_control->talkgroup_id == kTalkgroup);
        CHECK(frame.link_control->code.inner_words_corrected == 24);
        CHECK(frame.link_control->code.rs_corrected == 0);
    }

    SECTION("twelve detected double errors are twelve erasures") {
        std::vector<std::uint8_t> unit = ldu1;
        std::size_t erased = 0;
        for (std::size_t w = 0; w < decode::kP25LduHammingWords && erased < 12; w += 2) {
            const std::uint16_t word = word_at(unit, w);
            bool placed = false;
            for (unsigned a = 0; a < 10U && !placed; ++a) {
                for (unsigned b = a + 1; b < 10U && !placed; ++b) {
                    const auto damaged = static_cast<std::uint16_t>(word ^ (1U << a) ^ (1U << b));
                    if (decode::p25_hamming10_decode(damaged).detected) {
                        put_word(unit, w, damaged);
                        placed = true;
                    }
                }
            }
            REQUIRE(placed);
            ++erased;
        }
        const decode::P25Frame frame = decode_unit(unit, P25Duid::LogicalLinkDataUnit1);
        REQUIRE(frame.link_control.has_value());
        CHECK(frame.link_control->talkgroup_id == kTalkgroup);
        CHECK(frame.link_control->source_id == kSource);
        CHECK(frame.link_control->code.erasures == 12);
    }

    SECTION("six undetectable hexbit errors in Link Control, seven too many") {
        std::vector<std::uint8_t> unit = ldu1;
        for (std::size_t w = 0; w < 6; ++w) {
            const auto hexbit = decode::p25_hamming10_decode(word_at(unit, w * 3)).information;
            put_word(unit, w * 3,
                     decode::p25_hamming10_encode(static_cast<std::uint8_t>(hexbit ^ 0x2AU)));
        }
        const decode::P25Frame six = decode_unit(unit, P25Duid::LogicalLinkDataUnit1);
        REQUIRE(six.link_control.has_value());
        CHECK(six.link_control->talkgroup_id == kTalkgroup);
        CHECK(six.link_control->code.rs_corrected == 6);

        const auto hexbit = decode::p25_hamming10_decode(word_at(unit, 23)).information;
        put_word(unit, 23, decode::p25_hamming10_encode(static_cast<std::uint8_t>(hexbit ^ 0x15U)));
        const decode::P25Frame seven = decode_unit(unit, P25Duid::LogicalLinkDataUnit1);
        CHECK(seven.code_word_failed);
        CHECK_FALSE(seven.link_control.has_value());
        // The voice is outside the Reed-Solomon word and still comes out.
        CHECK(seven.voice.size() == decode::kP25VoiceFramesPerLdu);
    }

    SECTION("four undetectable hexbit errors in the encryption sync") {
        std::vector<std::uint8_t> unit = ldu2;
        for (std::size_t w = 0; w < 4; ++w) {
            const auto hexbit = decode::p25_hamming10_decode(word_at(unit, w * 5)).information;
            put_word(unit, w * 5,
                     decode::p25_hamming10_encode(static_cast<std::uint8_t>(hexbit ^ 0x3FU)));
        }
        const decode::P25Frame frame = decode_unit(unit, P25Duid::LogicalLinkDataUnit2);
        REQUIRE(frame.encryption_sync.has_value());
        CHECK(frame.encryption_sync->algorithm_id == decode::kP25AlgidUnencrypted);
        CHECK(frame.encryption_sync->code.rs_corrected == 4);
    }
}

TEST_CASE("P25 voice through noise, measured", "[decode][p25][voice]") {
    // A two-superframe call through the channel at four signal to noise
    // ratios. What is counted: the channel bit errors in the voice frames
    // before any code sees them, what the vocoder repeated or muted, and how
    // many data units framed and how many Link Control and encryption sync
    // words decoded. Printed, and asserted only for their shape.
    //
    // Measured on 2026-09-22: every LDU framed and every LC and ES word
    // decoded at 30, 8 and 5 dB, with a voice channel bit error rate of 0,
    // 0.00019 and 0.0116 and no frame repeated or muted; at 3 dB one LDU of
    // four framed. The limit there is the frame sync search, whose 0.9
    // correlation threshold P25Config documents, and not any of the codes
    // this file tests, so the 3 dB point asserts nothing and is kept to show
    // where the knee is.
    const siggen::P25VoiceMessage message = voice_message(36, false, true);
    auto dibits = siggen::p25_voice_message_dibits(message);
    REQUIRE(dibits.has_value());
    const std::vector<std::uint8_t> stream = with_tail(*dibits);

    struct Point {
        double snr_db;
        bool expect_clean;
        std::size_t minimum_units;
    };
    for (const Point point : {Point{30.0, true, 4}, Point{8.0, false, 3}, Point{5.0, false, 2},
                              Point{3.0, false, 0}}) {
        INFO("signal to noise " << point.snr_db << " dB across the whole " << kRate
                                << " Hz sample rate");
        const Received rx = receive(stream, point.snr_db);

        // Place each LDU by its sync position, so a data unit the receiver
        // missed does not shift every later one. The transmission starts
        // after receive()'s 300 lead-in dibits, and the receiver's delay is a
        // few symbols, far inside the half-LDU rounding below.
        constexpr std::size_t kFirstLdu = 300 + decode::kP25HduTotalSymbols;
        bool header = false;
        std::size_t units = 0;
        std::size_t words_decoded = 0;
        std::size_t bit_errors = 0;
        std::size_t bits_compared = 0;
        for (const decode::P25Frame& frame : rx.frames) {
            header = header || frame.header.has_value();
            if (frame.voice.empty() || frame.first_symbol + decode::kP25LduSymbols / 2 < kFirstLdu) {
                continue;
            }
            const std::size_t index =
                (frame.first_symbol + decode::kP25LduSymbols / 2 - kFirstLdu) / decode::kP25LduSymbols;
            if (index >= 4) {
                continue;
            }
            ++units;
            words_decoded += (frame.link_control || frame.encryption_sync) ? 1U : 0U;
            for (std::size_t f = 0; f < frame.voice.size(); ++f) {
                const VoiceFrame& sent = message.voice[index * 9 + f];
                for (std::size_t b = 0; b < decode::kP25VoiceFrameBits; ++b) {
                    bit_errors += frame.voice[f][b] != sent[b] ? 1U : 0U;
                }
                bits_compared += decode::kP25VoiceFrameBits;
            }
        }
        const double ber = bits_compared == 0
                               ? 1.0
                               : static_cast<double>(bit_errors) / static_cast<double>(bits_compared);
        std::println(
            "test_p25p1_voice {:.0f} dB: header {}, {} of 4 LDUs framed, {} LC/ES words decoded, "
            "voice channel BER {:.5f} ({} of {} bits), {} frames to the vocoder, {} repeated, "
            "{} muted, {} withheld or dropped",
            point.snr_db, header ? "decoded" : "lost", units, words_decoded, ber, bit_errors,
            bits_compared, rx.call.frames_decoded, rx.call.frames_repeated, rx.call.frames_muted,
            rx.call.frames_withheld + rx.call.frames_dropped);

        if (point.expect_clean) {
            CHECK(header);
            CHECK(units == 4);
            CHECK(words_decoded == 4);
            CHECK(bit_errors == 0);
            CHECK(rx.pcm == reference_pcm(message.voice));
        } else {
            // Degraded and still framing.
            CHECK(units >= point.minimum_units);
        }
    }
}
