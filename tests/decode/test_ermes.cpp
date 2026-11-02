// ERMES: the code and formats against ETS 300 133-4's own values, and the
// decoder against the transmitter in core/dsp/synth/ermes_mod.h.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "core/decode/dv_phy.h"
#include "core/decode/ermes.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/ermes_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 48000;

decode::ErmesSystemInfo system_for(std::uint8_t batch) {
    decode::ErmesSystemInfo s;
    s.country = 0x5A;
    s.op = 5;
    s.paging_area = 37;
    s.eti = true;
    s.bai = false;
    s.fsi = 30;
    s.cycle = 42;
    s.ssn = 3;
    s.batch = batch;
    return s;
}

std::vector<siggen::ErmesBatchSpec> sample_batches() {
    std::vector<siggen::ErmesBatchSpec> batches;

    siggen::ErmesBatchSpec a;
    a.system = system_for(0);
    a.ssit = 0;
    a.ssif = static_cast<std::uint16_t>((4U << 11U) | (13U << 6U) | (2U << 1U));
    a.initial_addresses = {0x3FFF0, 0x12345, 0x00010};
    siggen::ErmesMessageSpec alpha;
    alpha.local_address = (0x12345U << 4U) | 0;
    alpha.message_number = 1;
    alpha.category = decode::ErmesCategory::Alphanumeric;
    alpha.alert = 3;
    alpha.text = "ERMES clause 6.5.3 round trip, 169.425 MHz";
    a.messages.push_back(alpha);
    siggen::ErmesMessageSpec numeric;
    numeric.local_address = (0x3FFF0U << 4U) | 0;
    numeric.message_number = 7;
    numeric.category = decode::ErmesCategory::Numeric;
    numeric.urgent = true;
    numeric.text = "0123-456.789 U/%";
    a.messages.push_back(numeric);
    siggen::ErmesMessageSpec tone;
    tone.local_address = (0x00010U << 4U);
    tone.category = decode::ErmesCategory::Tone;
    tone.alert = 6;
    a.messages.push_back(tone);
    batches.push_back(a);

    siggen::ErmesBatchSpec b;
    b.system = system_for(1);
    b.ssit = 1;
    b.ssif = static_cast<std::uint16_t>((3U << 11U) | (11U << 7U) | 36U);
    b.initial_addresses = {0x2AAAA};
    siggen::ErmesMessageSpec data;
    data.local_address = (0x2AAAAU << 4U) | 1;
    data.message_number = 2;
    data.category = decode::ErmesCategory::Transparent;
    data.operator_identity = 0x1ABC;
    for (int i = 0; i < 61; ++i) {
        data.data.push_back(static_cast<std::uint8_t>((i * 7 + 3) % 5 < 2));
    }
    b.messages.push_back(data);
    siggen::ErmesMessageSpec longm;
    longm.local_address = (0x2AAAAU << 4U) | 1;
    longm.message_number = 3;
    longm.category = decode::ErmesCategory::Alphanumeric;
    longm.urgent = true;
    longm.alert = 4;
    longm.link_counter = 10000;
    longm.text = "first part";
    b.messages.push_back(longm);
    batches.push_back(b);
    return batches;
}

decode::ErmesOutput decode_audio(std::span<const float> audio, std::size_t block) {
    auto decoder = decode::ErmesDecoder::create({});
    REQUIRE(decoder.has_value());
    decode::ErmesOutput out;
    for (std::size_t i = 0; i < audio.size(); i += block) {
        decoder->process_audio(audio.subspan(i, std::min(block, audio.size() - i)), out);
    }
    decoder->flush(out);
    return out;
}

decode::ErmesOutput decode_iq(std::span<const dsp::Complex32> iq, std::size_t block) {
    auto decoder = decode::ErmesDecoder::create({});
    REQUIRE(decoder.has_value());
    decode::ErmesOutput out;
    for (std::size_t i = 0; i < iq.size(); i += block) {
        decoder->process(iq.subspan(i, std::min(block, iq.size() - i)), out);
    }
    decoder->flush(out);
    return out;
}

std::vector<dsp::Complex32> render(const std::vector<siggen::ErmesBatchSpec>& batches) {
    auto iq = siggen::ermes_render_batches({}, batches);
    REQUIRE(iq.has_value());
    // A little carrier before and after, as a real capture would have.
    std::vector<dsp::Complex32> out(kRate / 20, dsp::Complex32{1.0F, 0.0F});
    out.insert(out.end(), iq->begin(), iq->end());
    out.insert(out.end(), kRate / 20, dsp::Complex32{1.0F, 0.0F});
    return out;
}

void check_messages(const decode::ErmesOutput& out) {
    REQUIRE(out.batches.size() == 2);
    CHECK(out.batches[0].system.country == 0x5A);
    CHECK(out.batches[0].system.op == 5);
    CHECK(out.batches[0].system.paging_area == 37);
    CHECK(out.batches[0].system.eti);
    CHECK(out.batches[0].system.fsi == 30);
    CHECK(out.batches[0].system.cycle == 42);
    CHECK(out.batches[0].system.ssn == 3);
    CHECK(out.batches[1].system.batch == 1);
    REQUIRE(out.batches[0].supplementary.has_value());
    CHECK(out.batches[0].supplementary->zone == 4);
    CHECK(out.batches[0].supplementary->hour == 13);
    CHECK(out.batches[0].supplementary->date == 2);
    REQUIRE(out.batches[1].supplementary.has_value());
    CHECK(out.batches[1].supplementary->day_of_week == 3);
    CHECK(out.batches[1].supplementary->month == 11);
    CHECK(out.batches[1].supplementary->year == 2026);
    CHECK(out.batches[0].initial_addresses == std::vector<std::uint32_t>{0x3FFF0, 0x12345, 0x00010});
    CHECK(out.batches[0].codewords == 154);

    REQUIRE(out.messages.size() == 5);
    const auto& m0 = out.messages[0];
    CHECK(m0.initial_address == 0x12345);
    CHECK(m0.message_number == 1);
    CHECK(m0.category == decode::ErmesCategory::Alphanumeric);
    CHECK(m0.alert == 3);
    CHECK(m0.text == "ERMES clause 6.5.3 round trip, 169.425 MHz");
    const auto& m1 = out.messages[1];
    CHECK(m1.category == decode::ErmesCategory::Numeric);
    CHECK(m1.urgent);
    CHECK(m1.text == "0123-456.789 U/%");
    const auto& m2 = out.messages[2];
    CHECK(m2.category == decode::ErmesCategory::Tone);
    CHECK(m2.alert == 6);
    const auto& m3 = out.messages[3];
    CHECK(m3.category == decode::ErmesCategory::Transparent);
    CHECK(m3.external);
    CHECK(m3.operator_identity == std::optional<std::uint16_t>{0x1ABC});
    CHECK(m3.batch_bits == 1);
    CHECK(m3.data.size() == 61);
    const auto& m4 = out.messages[4];
    CHECK(m4.aii);
    CHECK(m4.ait == 1);
    CHECK(m4.urgent);
    CHECK(m4.alert == 4);
    CHECK(m4.link_counter == std::optional<std::uint16_t>{10000});
    CHECK(m4.text == "first part");
    CHECK(m4.message_bits == 70);
}

}  // namespace

TEST_CASE("ERMES clause 7 code against the document's examples", "[ermes]") {
    // Clause 7: m = 1 gives g(x) itself, and all ones gives check bits
    // 110 001 001 011.
    CHECK(decode::ermes_encode(1) == ((1U << 12U) | 0b101011001101U));
    CHECK(decode::ermes_encode(0x3FFFF) == ((0x3FFFFU << 12U) | 0b110001001011U));

    std::mt19937 rng(7);
    for (int trial = 0; trial < 2000; ++trial) {
        const std::uint32_t info = rng() & 0x3FFFFU;
        const std::uint32_t cw = decode::ermes_encode(info);
        const unsigned a = rng() % 30;
        unsigned b = rng() % 30;
        while (b == a) {
            b = rng() % 30;
        }
        const auto one = decode::ermes_decode(cw ^ (1U << a));
        CHECK((one.valid && one.info == info && one.corrected == 1));
        const auto two = decode::ermes_decode(cw ^ (1U << a) ^ (1U << b));
        CHECK((two.valid && two.info == info && two.corrected == 2));
    }
    // Distance 6: three errors are detected, never miscorrected.
    int detected = 0;
    for (int trial = 0; trial < 500; ++trial) {
        const std::uint32_t info = rng() & 0x3FFFFU;
        std::uint32_t e = 0;
        while (std::popcount(e) < 3) {
            e |= 1U << (rng() % 30);
        }
        detected += decode::ermes_decode(decode::ermes_encode(info) ^ e).valid ? 0 : 1;
    }
    CHECK(detected == 500);
}

TEST_CASE("ERMES terminator and delimiter sit outside the code", "[ermes]") {
    for (std::uint32_t pattern : {decode::kErmesApt, decode::kErmesDelimiter}) {
        CHECK_FALSE(decode::ermes_decode(pattern).valid);
        for (unsigned i = 0; i < 30; ++i) {
            CHECK_FALSE(decode::ermes_decode(pattern ^ (1U << i)).valid);
        }
    }
    CHECK(std::popcount(decode::kErmesApt ^ decode::kErmesDelimiter) >= 6);
}

TEST_CASE("ERMES 5.4 partition boundaries match the document's typical values", "[ermes]") {
    auto k_first = [](std::size_t i) { return (154 - decode::ermes_message_partition_start(i)) / 9; };
    auto k_last = [](std::size_t i) { return (190 - decode::ermes_message_partition_start(i)) / 9; };
    CHECK(decode::ermes_message_partition_start(0) == 10);  // J = 5
    CHECK(k_first(0) == 16);
    CHECK(decode::ermes_message_partition_start(4) == 10);  // J = 1
    CHECK(k_first(4) == 16);
    CHECK(decode::ermes_message_partition_start(5) == 19);  // J = 9
    CHECK(k_first(5) == 15);
    CHECK(k_first(139) == 1);
    CHECK(k_last(0) == 20);
    CHECK(k_last(22) == 18);
    CHECK(k_last(139) == 5);
}

TEST_CASE("ERMES 11.4 link counter example", "[ermes]") {
    // 11.4: the first sub-message of a 10000-bit alphanumeric message has
    // AIF = 10 00 10 01 11 00 01 00 00.
    siggen::ErmesMessageSpec m;
    m.category = decode::ErmesCategory::Alphanumeric;
    m.urgent = true;
    m.alert = 4;
    m.link_counter = 10000;
    auto words = siggen::ermes_message_words(m);
    REQUIRE(words.has_value());
    REQUIRE(words->size() >= 3);
    CHECK((*words)[2] == 0b100010011100010000U);
    // AIN = 1100 for urgent alert type 4, 6.5.1.2.1.
    CHECK(((*words)[1] & 0x7FU) == 0b0011100U);
}

TEST_CASE("ERMES interleaver round trip", "[ermes]") {
    std::array<std::uint32_t, 9> words{};
    for (std::size_t i = 0; i < 9; ++i) {
        words[i] = decode::ermes_encode(static_cast<std::uint32_t>(i * 12345 + 7));
    }
    const auto bits = decode::ermes_interleave(words);
    // Clause 8: the first nine bits sent are the MSBs of codewords 1 to 9.
    for (std::size_t i = 0; i < 9; ++i) {
        CHECK(bits[i] == ((words[i] >> 29U) & 1U));
    }
    CHECK(decode::ermes_deinterleave(bits) == words);
}

TEST_CASE("ERMES round trip, clean, from baseband and from scaled audio", "[ermes]") {
    const auto iq = render(sample_batches());
    const auto out = decode_iq(iq, 4096);
    check_messages(out);
    CHECK(out.batches[0].corrected_bits == 0);

    // Engine NFM audio: arbitrary scale and inverted polarity.
    std::vector<float> audio(iq.size());
    REQUIRE(decode::fm_discriminate(iq, audio, kRate).has_value());
    for (float& a : audio) {
        a *= -0.37F / 5000.0F;
    }
    const auto from_audio = decode_audio(audio, 1000);
    check_messages(from_audio);
    CHECK(from_audio.batches[0].inverted);
}

TEST_CASE("ERMES chunk independence", "[ermes]") {
    const auto iq = render(sample_batches());
    const auto whole = decode_iq(iq, iq.size());
    check_messages(whole);
    for (std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{1023}}) {
        const auto split = decode_iq(iq, block);
        REQUIRE(split.messages.size() == whole.messages.size());
        for (std::size_t i = 0; i < whole.messages.size(); ++i) {
            CHECK(split.messages[i].text == whole.messages[i].text);
            CHECK(split.messages[i].data == whole.messages[i].data);
            CHECK(split.messages[i].local_address == whole.messages[i].local_address);
        }
        REQUIRE(split.batches.size() == whole.batches.size());
        for (std::size_t i = 0; i < whole.batches.size(); ++i) {
            CHECK(split.batches[i].first_sample == whole.batches[i].first_sample);
        }
    }
}

TEST_CASE("ERMES round trip through AWGN and a two-path channel", "[ermes]") {
    const auto clean = render(sample_batches());
    for (double snr : {20.0, 14.0, 11.0, 8.0, 6.0, 4.0}) {
        siggen::ChannelConfig cfg;
        cfg.noise = siggen::NoiseLevel::snr_in_reference_bandwidth_db(snr, 25000);
        auto ch = siggen::apply_channel(clean, cfg, kRate, 0xE4E5ULL + static_cast<std::uint64_t>(snr));
        REQUIRE(ch.has_value());
        const auto out = decode_iq(ch->samples, 4096);
        int corrected = 0;
        for (const auto& b : out.batches) {
            corrected += b.corrected_bits;
        }
        INFO("SNR " << snr << " dB in 25 kHz: " << out.messages.size()
                    << " messages, corrected bits " << corrected);
        WARN("ERMES AWGN " << snr << " dB in 25 kHz: messages " << out.messages.size()
                           << "/5, corrected bits " << corrected);
        if (snr >= 14.0) {
            check_messages(out);
        }
    }

    siggen::ChannelConfig cfg;
    cfg.multipath.taps = {{0.0, 0.0, 0.0}, {12e-6, -6.0, 1.3}};
    cfg.noise = siggen::NoiseLevel::snr_in_reference_bandwidth_db(25.0, 25000);
    auto ch = siggen::apply_channel(clean, cfg, kRate, 0x2FA7ULL);
    REQUIRE(ch.has_value());
    const auto out = decode_iq(ch->samples, 4096);
    int corrected = 0;
    for (const auto& b : out.batches) {
        corrected += b.corrected_bits;
    }
    WARN("ERMES two-path 12 us at -6 dB, 25 dB: corrected bits " << corrected);
    check_messages(out);
}

TEST_CASE("ERMES noise alone starts nothing", "[ermes]") {
    std::vector<dsp::Complex32> noise(kRate * 10, dsp::Complex32{0.0F, 0.0F});
    REQUIRE(siggen::add_awgn_at_power(noise, 1.0, 99).has_value());
    const auto out = decode_iq(noise, 4096);
    CHECK(out.batches.empty());
    CHECK(out.messages.empty());
}
