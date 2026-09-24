// DSC on VHF: the ten-bit code against ITU-R M.493-15's own Table A1-1, the
// call formats against Tables A1-4.1 to A1-4.9 read by hand, and the decoder
// against the transmitter in core/dsp/synth/dsc_mod.h, from ideal audio and
// through an FM receiver.
//
// Error rates are measured and reported in INFO and WARN lines with loose
// assertions, for the reason tests/decode/CMakeLists.txt gives.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "core/decode/dsc.h"
#include "core/decode/dv_phy.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dsc_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 48'000;

// The fingerprint rule tests/decode/test_dmr.cpp states, for ITU-R's
// copyright on M.493.
std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t hash = 0xCBF2'9CE4'8422'2325ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x0000'0100'0000'01B3ULL;
    }
    return hash;
}

double nfm_channel(double hertz, const void*) {
    return std::abs(hertz) <= 8000.0 ? 1.0 : 0.0;
}

std::vector<decode::DscCall> decode_audio(const std::vector<float>& audio, std::size_t block = 0,
                                          decode::DscStats* stats = nullptr) {
    auto decoder = decode::DscDecoder::create(decode::DscConfig{});
    REQUIRE(decoder.has_value());
    std::vector<decode::DscCall> out;
    if (block == 0) {
        decoder->process(audio, out);
    } else {
        for (std::size_t i = 0; i < audio.size(); i += block) {
            const std::size_t n = std::min(block, audio.size() - i);
            decoder->process(std::span<const float>(audio.data() + i, n), out);
        }
    }
    if (stats != nullptr) {
        *stats = decoder->stats();
    }
    return out;
}

decode::DscFrequency vhf_channel(std::uint64_t channel) {
    decode::DscFrequency f;
    f.kind = decode::DscFrequency::Kind::VhfChannel;
    f.value = channel;
    return f;
}

// One call of each kind the decoder parses, from Tables A1-4.1 to A1-4.9.
std::vector<decode::DscCall> calls() {
    std::vector<decode::DscCall> all;
    {
        // Table A1-4.1, a VHF distress alert: flooding, a position, a time,
        // radiotelephony to follow.
        decode::DscCall c;
        c.format = decode::kDscFormatDistress;
        c.self_id = 366123456;
        c.nature_of_distress = 101;
        c.distress_position = decode::DscPosition{37.0 + 48.0 / 60.0, -(122.0 + 25.0 / 60.0)};
        c.utc_hhmm = 1745;
        c.subsequent_communications = 100;
        c.eos = decode::kDscEos;
        all.push_back(c);
    }
    {
        // Table A1-4.1 with neither position nor time, clause 8.1.2's ten
        // nines and clause 8.1.3's 8888, and a man overboard.
        decode::DscCall c;
        c.format = decode::kDscFormatDistress;
        c.self_id = 972000123;
        c.nature_of_distress = 110;
        c.subsequent_communications = decode::kDscNoInformation;
        c.eos = decode::kDscEos;
        all.push_back(c);
    }
    {
        // Table A1-4.2, a distress acknowledgement to all ships.
        decode::DscCall c;
        c.format = decode::kDscFormatAllShips;
        c.category = decode::kDscCategoryDistress;
        c.self_id = 3669991;
        c.telecommand1 = decode::kDscTelecommandDistressAcknowledgement;
        c.distress_id = 366123456;
        c.nature_of_distress = 101;
        c.distress_position = decode::DscPosition{-(33.0 + 51.0 / 60.0), 151.0 + 12.0 / 60.0};
        c.utc_hhmm = 1746;
        c.subsequent_communications = 100;
        c.eos = decode::kDscEos;
        all.push_back(c);
    }
    {
        // Table A1-4.5, an all ships safety call proposing channel 16.
        decode::DscCall c;
        c.format = decode::kDscFormatAllShips;
        c.category = decode::kDscCategorySafety;
        c.self_id = 235009876;
        c.telecommand1 = 100;
        c.telecommand2 = decode::kDscNoInformation;
        c.frequencies = {vhf_channel(16)};
        c.eos = decode::kDscEos;
        all.push_back(c);
    }
    {
        // Table A1-4.9, a routine individual call on channel 72 wanting an
        // acknowledgement.
        decode::DscCall c;
        c.format = decode::kDscFormatIndividual;
        c.address = 211456780;
        c.category = decode::kDscCategoryRoutine;
        c.self_id = 211987650;
        c.telecommand1 = 100;
        c.telecommand2 = decode::kDscNoInformation;
        c.frequencies = {vhf_channel(72)};
        c.eos = decode::kDscEosAcknowledgeRq;
        all.push_back(c);
    }
    {
        // Table A1-4.8, a routine group call; clause 5.2 group identities
        // lead with a 0.
        decode::DscCall c;
        c.format = decode::kDscFormatGroup;
        c.address = 23512345;  // 023512345
        c.category = decode::kDscCategoryRoutine;
        c.self_id = 2351000;
        c.telecommand1 = 100;
        c.telecommand2 = decode::kDscNoInformation;
        c.frequencies = {vhf_channel(1078)};
        c.eos = decode::kDscEos;
        all.push_back(c);
    }
    {
        // Table A1-4.3, a distress alert relay to an individual station,
        // the station in distress unknown per clause 8.4.
        decode::DscCall c;
        c.format = decode::kDscFormatIndividual;
        c.address = 2320001;
        c.category = decode::kDscCategoryDistress;
        c.self_id = 244123000;
        c.telecommand1 = decode::kDscTelecommandDistressRelay;
        c.nature_of_distress = 102;
        c.distress_position = decode::DscPosition{51.0 + 1.0 / 60.0, 1.0 + 30.0 / 60.0};
        c.utc_hhmm = 905;
        c.subsequent_communications = 100;
        c.eos = decode::kDscEosAcknowledgeRq;
        all.push_back(c);
    }
    return all;
}

void check_same(const decode::DscCall& got, const decode::DscCall& sent) {
    INFO("format " << int(sent.format) << " from " << sent.self_id);
    CHECK(got.format == sent.format);
    CHECK(got.self_id == sent.self_id);
    CHECK(got.address == sent.address);
    CHECK(got.category == sent.category);
    CHECK(got.telecommand1 == sent.telecommand1);
    CHECK(got.telecommand2 == sent.telecommand2);
    CHECK(got.distress_id == sent.distress_id);
    CHECK(got.nature_of_distress == sent.nature_of_distress);
    CHECK(got.utc_hhmm == sent.utc_hhmm);
    CHECK(got.subsequent_communications == sent.subsequent_communications);
    CHECK(got.eos == sent.eos);
    CHECK(got.distress_position.has_value() == sent.distress_position.has_value());
    if (got.distress_position && sent.distress_position) {
        // Clause 8.1.2 carries whole minutes.
        CHECK(std::abs(got.distress_position->latitude - sent.distress_position->latitude) < 1e-9);
        CHECK(std::abs(got.distress_position->longitude - sent.distress_position->longitude) <
              1e-9);
    }
    REQUIRE(got.frequencies.size() == sent.frequencies.size());
    for (std::size_t i = 0; i < got.frequencies.size(); ++i) {
        CHECK(got.frequencies[i].kind == sent.frequencies[i].kind);
        CHECK(got.frequencies[i].value == sent.frequencies[i].value);
    }
}

std::vector<std::vector<std::uint8_t>> bits_of(const std::vector<decode::DscCall>& sent) {
    std::vector<std::vector<std::uint8_t>> out;
    for (const decode::DscCall& c : sent) {
        auto information = siggen::dsc_encode(c);
        REQUIRE(information.has_value());
        out.push_back(siggen::dsc_bits(*information));
    }
    return out;
}

// Sent calls found in order among the received ones.
std::size_t count_received(const std::vector<decode::DscCall>& got,
                           const std::vector<decode::DscCall>& sent) {
    std::size_t good = 0;
    std::size_t k = 0;
    for (const decode::DscCall& s : sent) {
        auto information = siggen::dsc_encode(s);
        REQUIRE(information.has_value());
        if (k < got.size() && got[k].symbols == *information) {
            ++good;
            ++k;
        }
    }
    return good;
}

}  // namespace

TEST_CASE("Table A1-1 is the ten-bit rule the code applies", "[decode][dsc]") {
    // Read out of the Recommendation's own Table A1-1 by script on
    // 2026-09-23, each symbol with its ten emitted bits as 0 for B and 1 for
    // Y, bit 1 first, and hashed; M.493-16's table hashes the same.
    std::string regenerated;
    for (unsigned v = 0; v < 128; ++v) {
        const auto bits = decode::dsc_encode_symbol(static_cast<std::uint8_t>(v));
        regenerated += std::format("{} ", v);
        for (const std::uint8_t b : bits) {
            regenerated.push_back(b != 0U ? '1' : '0');
        }
        regenerated.push_back('\n');
        CHECK(decode::dsc_decode_symbol(bits) == v);
    }
    CHECK(fnv1a(regenerated) == 0x0C6D'F3C5'B17B'3721ULL);
}

TEST_CASE("every single bit error in a character is detected", "[decode][dsc]") {
    // Clause 1.1.1's check bits count the B elements, so flipping one
    // information bit moves the count by one and flipping one check bit
    // changes the count it states.
    for (unsigned v = 0; v < 128; ++v) {
        const auto bits = decode::dsc_encode_symbol(static_cast<std::uint8_t>(v));
        for (std::size_t i = 0; i < decode::kDscCharacterBits; ++i) {
            auto damaged = bits;
            damaged[i] ^= 1U;
            CHECK_FALSE(decode::dsc_decode_symbol(damaged).has_value());
        }
    }
}

TEST_CASE("the formats read as Tables A1-4.1 to A1-4.9 lay them out", "[decode][dsc]") {
    // A distress alert written out by hand from Table A1-4.1 and clauses
    // 5.2 and 8.1: format specifier twice, the self-ID's ten digits in five
    // characters, fire, position 1 (NW) 51 deg 30 min N 000 deg 07 min W,
    // 12:34 UTC, radiotelephony, end of sequence.
    const std::vector<std::uint8_t> alert = {112, 112, 23, 51, 23, 45, 60, 100, 15,
                                             13,  0,   0,  7,  12, 34, 100, 127};
    auto call = decode::dsc_parse(alert);
    REQUIRE(call.has_value());
    CHECK(call->format == decode::kDscFormatDistress);
    CHECK(call->self_id == 235123456);
    CHECK(call->nature_of_distress == 100);
    REQUIRE(call->distress_position.has_value());
    CHECK(std::abs(call->distress_position->latitude - 51.5) < 1e-9);
    CHECK(std::abs(call->distress_position->longitude - -(7.0 / 60.0)) < 1e-9);
    CHECK(call->utc_hhmm == 1234);
    CHECK(call->subsequent_communications == 100);
    CHECK(call->eos == 127);

    // Clause 10.2's ECC over one format specifier and the rest, as the
    // transmitter computes it, then by hand: the XOR of the same symbols.
    unsigned parity = 0;
    for (std::size_t i = 1; i < alert.size(); ++i) {
        parity ^= alert[i];
    }
    CHECK(decode::dsc_ecc(std::span<const std::uint8_t>(alert).subspan(1)) == parity);

    // Table A1-5: 90 00 72 is VHF channel 72, and HM 0 an MF frequency in
    // 100 Hz, 21 82 00 being 2182.0 kHz.
    const std::vector<std::uint8_t> individual = {120, 120, 21, 14, 56, 78, 0,   100, 21,
                                                  19,  87,  65, 0,  100, 126, 90, 0,  72, 117};
    call = decode::dsc_parse(individual);
    REQUIRE(call.has_value());
    CHECK(call->address == 211456780);
    CHECK(call->category == decode::kDscCategoryRoutine);
    CHECK(call->self_id == 211987650);
    REQUIRE(call->frequencies.size() == 1);
    CHECK(call->frequencies[0].kind == decode::DscFrequency::Kind::VhfChannel);
    CHECK(call->frequencies[0].value == 72);
    CHECK(call->eos == decode::kDscEosAcknowledgeRq);
    const decode::DscFrequency mf = {decode::DscFrequency::Kind::Frequency, 2'182'000, {}};
    auto element = siggen::dsc_frequency_element(mf);
    REQUIRE(element.has_value());
    CHECK(*element == std::array<std::uint8_t, 3>{2, 18, 20});

    // A format clause 4.1 does not list is refused, Table A1-3 note 1.
    const std::vector<std::uint8_t> unknown = {121, 121, 1, 2, 3, 4, 5, 100, 127};
    CHECK_FALSE(decode::dsc_parse(unknown).has_value());
}

TEST_CASE("the characters go on the air as Figure 1 b) shows", "[decode][dsc]") {
    const auto information = siggen::dsc_encode(calls()[3]);
    REQUIRE(information.has_value());
    const auto slots = siggen::dsc_air_symbols(*information);
    // Six DX phasing characters and RX 111 down to 104, alternating.
    for (std::size_t j = 0; j < 6; ++j) {
        CHECK(slots[2 * j] == decode::kDscPhasingDx);
    }
    for (std::size_t j = 0; j < 8; ++j) {
        CHECK(slots[2 * j + 1] == 111 - j);
    }
    // Each information character in DX, then four other characters, then
    // again in RX (clause 1.2.1).
    for (std::size_t i = 0; i < information->size(); ++i) {
        CHECK(slots[12 + 2 * i] == (*information)[i]);
        CHECK(slots[12 + 2 * i + 5] == (*information)[i]);
    }
    // After the end of sequence: the ECC in DX and RX, then the end of
    // sequence twice more in DX (clause 9).
    const std::size_t n = information->size();
    const std::uint8_t ecc = slots[12 + 2 * n];
    CHECK(slots[12 + 2 * n + 5] == ecc);
    CHECK(slots[12 + 2 * (n + 1)] == information->back());
    CHECK(slots[12 + 2 * (n + 2)] == information->back());
}

TEST_CASE("DSC round trips from ideal audio", "[decode][dsc]") {
    const auto sent = calls();
    for (const bool preemphasis : {true, false}) {
        INFO((preemphasis ? "with" : "without") << " pre-emphasis");
        siggen::DscModConfig mod;
        mod.preemphasis = preemphasis;
        auto audio = siggen::dsc_render_audio(mod, bits_of(sent));
        REQUIRE(audio.has_value());
        decode::DscStats stats;
        const auto got = decode_audio(*audio, 0, &stats);
        REQUIRE(got.size() == sent.size());
        for (std::size_t i = 0; i < sent.size(); ++i) {
            check_same(got[i], sent[i]);
            CHECK(got[i].from_rx == 0);
            if (i > 0) {
                CHECK(got[i].first_sample > got[i - 1].last_sample);
            }
        }
    }
}

TEST_CASE("time diversity recovers a character whose DX copy is lost", "[decode][dsc]") {
    const auto sent = calls();
    auto all_bits = bits_of(sent);
    // Damage one bit of the DX copy of the self-ID's first character and of
    // the end of sequence in the first call, and of the RX copy of the
    // nature of distress: each still has one good copy.
    const auto information = siggen::dsc_encode(sent[0]);
    REQUIRE(information.has_value());
    const std::size_t first_char_bit = decode::kDscVhfDotBits;
    const auto slot_bit = [&](std::size_t slot) { return first_char_bit + slot * 10; };
    all_bits[0][slot_bit(12 + 2 * 2) + 3] ^= 1U;                      // self-ID, DX
    all_bits[0][slot_bit(12 + 2 * (information->size() - 1)) + 9] ^= 1U;  // EOS, DX
    all_bits[0][slot_bit(12 + 2 * 7 + 5) + 0] ^= 1U;                  // nature, RX
    auto audio = siggen::dsc_render_audio(siggen::DscModConfig{}, all_bits);
    REQUIRE(audio.has_value());
    const auto got = decode_audio(*audio);
    REQUIRE(got.size() == sent.size());
    check_same(got[0], sent[0]);
    CHECK(got[0].from_rx == 2);

    // Both copies of one character damaged loses the call, and the next is
    // still found.
    all_bits = bits_of(sent);
    all_bits[0][slot_bit(12 + 2 * 3) + 1] ^= 1U;
    all_bits[0][slot_bit(12 + 2 * 3 + 5) + 1] ^= 1U;
    audio = siggen::dsc_render_audio(siggen::DscModConfig{}, all_bits);
    REQUIRE(audio.has_value());
    decode::DscStats stats;
    const auto partial = decode_audio(*audio, 0, &stats);
    CHECK(partial.size() == sent.size() - 1);
    CHECK(stats.lost == 1);
    REQUIRE_FALSE(partial.empty());
    check_same(partial[0], sent[1]);
}

TEST_CASE("DSC output does not depend on how the audio is blocked", "[decode][dsc]") {
    auto audio = siggen::dsc_render_audio(siggen::DscModConfig{}, bits_of(calls()));
    REQUIRE(audio.has_value());
    const auto whole = decode_audio(*audio);
    REQUIRE(whole.size() == calls().size());
    for (const std::size_t block : {std::size_t{1}, std::size_t{317}, std::size_t{10'000}}) {
        INFO("block of " << block);
        const auto split = decode_audio(*audio, block);
        REQUIRE(split.size() == whole.size());
        for (std::size_t i = 0; i < whole.size(); ++i) {
            CHECK(split[i].symbols == whole[i].symbols);
            CHECK(split[i].first_sample == whole[i].first_sample);
            CHECK(split[i].last_sample == whole[i].last_sample);
        }
    }
}

TEST_CASE("DSC through an FM receiver at a high and a low SNR, measured", "[decode][dsc]") {
    // Clause 1.3.2's phase modulation at index 2.0 as complex baseband, noise
    // there, an nfm receiver's 8 kHz either side, the discriminator, and the
    // decoder. Seven calls, ten times over.
    std::vector<decode::DscCall> sent;
    for (int r = 0; r < 10; ++r) {
        for (decode::DscCall c : calls()) {
            c.self_id = (c.self_id + static_cast<std::uint64_t>(r) * 11U) % 1'000'000'000U;
            sent.push_back(c);
        }
    }
    const auto bits = bits_of(sent);

    struct Point {
        double snr_2500_db;
        double allowed_loss;
    };
    // Measured 2026-09-23 at 48000 S/s, noise seed 0xD5C, 70 calls a point:
    // none lost at 20 dB in 2500 Hz, 27 of 70 at 14 dB with 117 characters
    // taken from their RX copies. On the same audio 16 dB lost none and
    // 12 dB 68 of 70, and at 10 dB the dot pattern and phasing were never
    // found at all: the discriminator is below its threshold in the 16 kHz
    // channel. The allowances sit above the measurements, which the WARN
    // line prints; bench sweep --mode dsc has the curve.
    const Point points[] = {{20.0, 0.0}, {14.0, 0.6}};
    for (const Point& p : points) {
        auto rf = siggen::dsc_render_baseband(siggen::DscModConfig{}, bits);
        REQUIRE(rf.has_value());
        REQUIRE(siggen::add_awgn(*rf, siggen::NoiseLevel::snr_in_2500_hz_db(p.snr_2500_db), kRate,
                                 0xD5CULL)
                    .has_value());
        auto taps = decode::design_from_response(kRate, 127, nfm_channel, nullptr);
        REQUIRE(taps.has_value());
        std::vector<dsp::Complex32> filtered(rf->size());
        REQUIRE(decode::filter_complex(*rf, *taps, filtered).has_value());
        std::vector<float> audio(rf->size());
        REQUIRE(decode::fm_discriminate(filtered, audio, kRate).has_value());

        decode::DscStats stats;
        const auto got = decode_audio(audio, 0, &stats);
        const std::size_t good = count_received(got, sent);
        const double loss = 1.0 - static_cast<double>(good) / static_cast<double>(sent.size());
        int from_rx = 0;
        for (const auto& g : got) {
            from_rx += g.from_rx;
        }
        INFO("SNR " << p.snr_2500_db << " dB in 2500 Hz: calls lost " << loss << ", phasings "
                    << stats.phasings << ", lost " << stats.lost << ", ECC failures "
                    << stats.ecc_failures << ", characters from RX " << from_rx);
        CHECK(loss <= p.allowed_loss);
        CHECK(got.size() == good);
        WARN("DSC SNR " << p.snr_2500_db << " dB/2500 Hz: call error rate " << loss << " of "
                        << sent.size() << "; " << from_rx << " characters taken from RX, "
                        << stats.ecc_failures << " ECC failures");
    }
}

TEST_CASE("noise alone decodes no DSC call, measured", "[decode][dsc]") {
    auto decoder = decode::DscDecoder::create(decode::DscConfig{});
    REQUIRE(decoder.has_value());
    std::vector<decode::DscCall> got;
    constexpr std::size_t kSeconds = 120;
    std::mt19937_64 engine(0xD5C0ULL);
    std::normal_distribution<float> gauss(0.0F, 1.0F);
    std::vector<float> audio(kRate);
    for (std::size_t s = 0; s < kSeconds; ++s) {
        for (float& v : audio) {
            v = gauss(engine);
        }
        decoder->process(audio, got);
    }
    WARN("DSC on noise: " << decoder->stats().phasings << " phasings found in " << kSeconds
                          << " s, " << got.size() << " calls reported");
    CHECK(got.empty());
}
