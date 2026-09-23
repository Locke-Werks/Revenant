// POCSAG: the code against ITU-R M.584-2's own tables, and the decoder
// against the transmitter, from ideal audio and through an FM receiver.
//
// Tables 1 and 2 are the two codewords the Recommendation prints in full,
// and clause 1.3.4 says the idle codeword is a valid address codeword, so
// both should be code words of the clause 1.4 code. That is the one check in
// this file that a misreading of clause 1.4 shared by both ends of the round
// trip cannot pass. Table 1 is one. Table 2, as printed, is one bit away
// from one, and the first case below says which bit.
//
// Error rates are measured and reported in INFO and WARN lines with loose
// assertions, for the reason tests/decode/CMakeLists.txt gives.

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "core/decode/dv_phy.h"
#include "core/decode/pocsag.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/fsk_mod.h"

using namespace revenant;

namespace {

std::uint32_t from_bits(const char* bits) {
    std::uint32_t v = 0;
    for (const char* p = bits; *p != '\0'; ++p) {
        v = (v << 1U) | static_cast<std::uint32_t>(*p - '0');
    }
    return v;
}

std::vector<decode::PocsagPage> decode_audio(const decode::PocsagConfig& config,
                                             const std::vector<float>& audio, std::size_t block = 0,
                                             decode::PocsagStats* stats = nullptr) {
    auto decoder = decode::PocsagDecoder::create(config);
    REQUIRE(decoder.has_value());
    std::vector<decode::PocsagPage> pages;
    if (block == 0) {
        decoder->process(audio, pages);
    } else {
        for (std::size_t i = 0; i < audio.size(); i += block) {
            const std::size_t n = std::min(block, audio.size() - i);
            decoder->process(std::span<const float>(audio.data() + i, n), pages);
        }
    }
    decoder->flush(pages);
    if (stats != nullptr) {
        *stats = decoder->stats();
    }
    return pages;
}

struct Sent {
    std::uint32_t identity;
    std::uint8_t function;
    std::string text;
};

std::vector<siggen::PocsagPageSpec> specs_for(const std::vector<Sent>& sent) {
    std::vector<siggen::PocsagPageSpec> specs;
    for (const Sent& s : sent) {
        siggen::PocsagPageSpec spec;
        spec.identity = s.identity;
        spec.function = s.function;
        if (!s.text.empty()) {
            auto bits = (s.function == decode::kPocsagFunctionNumeric)
                            ? siggen::pocsag_numeric_bits(s.text)
                            : siggen::pocsag_alphanumeric_bits(s.text);
            REQUIRE(bits.has_value());
            spec.message_bits = *bits;
        }
        specs.push_back(spec);
    }
    return specs;
}

double channel_response(double hertz, const void*) {
    return std::abs(hertz) <= 7000.0 ? 1.0 : 0.0;
}

const std::vector<Sent> kPages = {
    {1234567, decode::kPocsagFunctionNumeric, "0123456789 U-[]"},
    {8, decode::kPocsagFunctionAlphanumeric,
     "A message long enough to run across a batch boundary, which clause 1.2 allows "
     "as long as the synchronization codeword is not displaced."},
    {2097151, 0b01, ""},
    {77, decode::kPocsagFunctionAlphanumeric, "Short one."},
};

void check_pages(const std::vector<decode::PocsagPage>& got, bool inverted) {
    std::string seen;
    for (const auto& g : got) {
        seen += std::to_string(g.identity) + " \"" + g.text + "\"; ";
    }
    INFO("decoded: " << seen);
    REQUIRE(got.size() == kPages.size());
    for (std::size_t i = 0; i < kPages.size(); ++i) {
        INFO("page " << i);
        CHECK(got[i].identity == kPages[i].identity);
        CHECK(got[i].function == kPages[i].function);
        CHECK(got[i].text == kPages[i].text);
        CHECK(got[i].uncorrectable_codewords == 0);
        CHECK(got[i].inverted == inverted);
        if (i > 0) {
            CHECK(got[i].position > got[i - 1].position);
        }
    }
}

}  // namespace

TEST_CASE("Tables 1 and 2 are code words of the clause 1.4 code", "[decode][pocsag]") {
    // Table 1 and Table 2, bits 1 to 32 as the Recommendation prints them.
    const std::uint32_t table_1 = from_bits("01111100110100100001010111011000");
    const std::uint32_t table_2 = from_bits("01111010110010011100000110010111");
    CHECK(table_1 == decode::kPocsagSync);
    CHECK(table_2 == decode::kPocsagIdleAsPrinted);

    // Table 1 is a code word. Re-encoding its 21 information bits reproduces
    // the whole word, so its check bits and parity are this generator's.
    {
        const auto c = decode::pocsag_correct(table_1);
        CHECK(c.valid);
        CHECK(c.corrected_bits == 0);
        CHECK(decode::pocsag_encode(table_1 >> 11U) == table_1);
    }

    // Table 2 as printed is not, although clause 1.3.4 says the idle
    // codeword is a valid address codeword. Flipping each bit in turn finds
    // exactly one code word one bit away, at bit No. 10, and that is the
    // idle codeword the decoder and transmitter use.
    CHECK(decode::pocsag_encode(table_2 >> 11U) != table_2);
    std::vector<int> repairs;
    for (unsigned i = 0; i < 32; ++i) {
        const std::uint32_t flipped = table_2 ^ (1U << i);
        if (decode::pocsag_encode(flipped >> 11U) == flipped) {
            repairs.push_back(32 - static_cast<int>(i));
            CHECK(flipped == decode::kPocsagIdle);
        }
    }
    CHECK(repairs == std::vector<int>{10});
    const auto c = decode::pocsag_correct(table_2);
    CHECK(c.valid);
    CHECK(c.word == decode::kPocsagIdle);

    // Clause 1.3.4: the idle codeword is an address codeword, flag 0.
    CHECK_FALSE(decode::pocsag_is_message(decode::kPocsagIdle));
}

TEST_CASE("the code corrects two errors and never miscorrects three", "[decode][pocsag]") {
    std::mt19937_64 engine(0xB0C5A6ULL);
    for (int trial = 0; trial < 12; ++trial) {
        const std::uint32_t word =
            decode::pocsag_encode(static_cast<std::uint32_t>(engine()) & 0x1FFFFFU);
        INFO("code word " << std::hex << word);
        for (unsigned i = 0; i < 32; ++i) {
            const auto one = decode::pocsag_correct(word ^ (1U << i));
            REQUIRE(one.valid);
            CHECK(one.word == word);
            CHECK(one.corrected_bits == 1);
            for (unsigned j = i + 1; j < 32; ++j) {
                const auto two = decode::pocsag_correct(word ^ (1U << i) ^ (1U << j));
                REQUIRE(two.valid);
                CHECK(two.word == word);
                CHECK(two.corrected_bits == 2);
                for (unsigned k = j + 1; k < 32; ++k) {
                    // Distance 6 with the parity bit: three errors sit at
                    // least three from every other code word, so the decoder
                    // must refuse rather than pick one.
                    const auto three =
                        decode::pocsag_correct(word ^ (1U << i) ^ (1U << j) ^ (1U << k));
                    if (three.valid) {
                        FAIL("three errors at " << i << ", " << j << ", " << k << " were accepted");
                    }
                }
            }
        }
    }
}

TEST_CASE("the numeric and alphanumeric formats round trip", "[decode][pocsag]") {
    auto numeric = siggen::pocsag_numeric_bits("0123456789 U-[]");
    REQUIRE(numeric.has_value());
    CHECK(numeric->size() % 20 == 0);
    CHECK(decode::pocsag_numeric(*numeric) == "0123456789 U-[]");
    // Table 3: "0" is 0000 and "U" is 1011, bit 1 first.
    auto u = siggen::pocsag_numeric_bits("U");
    REQUIRE(u.has_value());
    CHECK((*u)[0] == 1);
    CHECK((*u)[1] == 1);
    CHECK((*u)[2] == 0);
    CHECK((*u)[3] == 1);
    CHECK_FALSE(siggen::pocsag_numeric_bits("A").has_value());

    auto alpha = siggen::pocsag_alphanumeric_bits("Hello, pager {}~");
    REQUIRE(alpha.has_value());
    CHECK(alpha->size() % 20 == 0);
    CHECK(decode::pocsag_alphanumeric(*alpha) == "Hello, pager {}~");
}

TEST_CASE("POCSAG round trips from audio at three rates", "[decode][pocsag]") {
    const auto bits = siggen::pocsag_bits(specs_for(kPages));
    for (const double bit_rate : {decode::kPocsag512, decode::kPocsag1200, decode::kPocsag2400}) {
        for (const dsp::SampleRate rate : {dsp::SampleRate{48'000}, dsp::SampleRate{22'050}}) {
            for (const bool invert : {false, true}) {
                INFO(bit_rate << " bit/s at " << rate << " Hz, "
                              << (invert ? "inverted" : "upright"));
                siggen::PocsagModConfig mod;
                mod.rate = rate;
                mod.bit_rate = bit_rate;
                mod.invert = invert;
                auto audio = siggen::pocsag_render_audio(mod, bits);
                REQUIRE(audio.has_value());
                decode::PocsagConfig config;
                config.rate = rate;
                config.bit_rate = bit_rate;
                decode::PocsagStats stats;
                const auto got = decode_audio(config, *audio, 0, &stats);
                INFO(stats.batches << " batches, " << stats.codewords << " codewords");
                check_pages(got, invert);
                // The first address codeword follows the preamble and one
                // synchronization codeword; the page reports its first bit.
                const double samples_per_bit = static_cast<double>(rate) / bit_rate;
                const double first_slot = static_cast<double>(decode::kPocsagPreambleBits + 32 +
                                                              32 * 2 * (1234567U & 7U));
                CHECK(std::abs(static_cast<double>(got[0].position) -
                               first_slot * samples_per_bit) < samples_per_bit);
            }
        }
    }
}

TEST_CASE("POCSAG output does not depend on how the audio is blocked", "[decode][pocsag]") {
    const auto bits = siggen::pocsag_bits(specs_for(kPages));
    auto audio = siggen::pocsag_render_audio(siggen::PocsagModConfig{}, bits);
    REQUIRE(audio.has_value());
    const auto whole = decode_audio(decode::PocsagConfig{}, *audio);
    for (const std::size_t block : {std::size_t{1}, std::size_t{317}, std::size_t{10'000}}) {
        INFO("block of " << block);
        const auto split = decode_audio(decode::PocsagConfig{}, *audio, block);
        REQUIRE(split.size() == whole.size());
        for (std::size_t i = 0; i < whole.size(); ++i) {
            CHECK(split[i].position == whole[i].position);
            CHECK(split[i].message_bits == whole[i].message_bits);
        }
    }
}

TEST_CASE("POCSAG through an FM receiver against noise, measured", "[decode][pocsag]") {
    // The RF signal at plus and minus 4.5 kHz as complex baseband, noise
    // added there by the channel simulator, a tuning error, then the FM
    // discriminator core/decode/dv_phy.h already has, and the decoder reading
    // what it makes. 40 alphanumeric pages of 40 characters at 1200 bit/s.
    constexpr dsp::SampleRate kRate = 48'000;
    std::vector<Sent> sent;
    std::mt19937_64 engine(0x9A6E5ULL);
    for (int i = 0; i < 40; ++i) {
        std::string text;
        for (int k = 0; k < 40; ++k) {
            text.push_back(static_cast<char>(' ' + engine() % 94));
        }
        sent.push_back({static_cast<std::uint32_t>(engine() & 0x1FFFFFU),
                        decode::kPocsagFunctionAlphanumeric, text});
    }
    const auto bits = siggen::pocsag_bits(specs_for(sent));

    struct Point {
        double snr_2500_db;
        dsp::Hertz tuning_error_hz;
        double allowed_page_loss;
    };
    // Measured 2026-09-22 at 48 kHz, pages seed 0x9A6E5, noise seed 0x5CA7,
    // 40 pages of 15 codewords each:
    //
    //   SNR/2500 Hz  Eb/N0    raw BER    bits corrected  uncorrectable  pages lost
    //   20 dB        23.2 dB  0          0               0              0
    //   20 dB, tuned 1 kHz off  0        0               0              0
    //   12 dB        15.2 dB  0.00019    6               0              0
    //    8 dB        11.2 dB  0.023      584             0.033          0.40
    //    4 dB         7.2 dB  0.19       75              0.70           1.0
    //
    // At 8 dB the code is earning its place: 584 bits put right, and what it
    // cannot correct is a codeword in thirty. A page is fifteen codewords,
    // any of which loses it, which is why the page loss is what it is. At
    // 4 dB the discriminator is below its threshold and the loss is total.
    // The allowances sit above the measurements.
    //
    // Since 2026-09-23 an address codeword is refused past one corrected bit,
    // PocsagConfig::address_correction_budget, and 8 dB loses 0.475 of the
    // pages rather than the 0.40 above: three more addresses of the forty
    // had two bits wrong. Nothing else in the table moved.
    // tests/decode/test_pocsag_false_pages.cpp has what that buys.
    const Point points[] = {
        {20.0, 0, 0.0}, {20.0, 1000, 0.0}, {12.0, 0, 0.2}, {8.0, 0, 0.6}, {4.0, 0, 1.0}};

    for (const Point& p : points) {
        siggen::PocsagModConfig mod;
        mod.rate = kRate;
        auto rf = siggen::pocsag_render_baseband(mod, bits);
        REQUIRE(rf.has_value());
        if (p.tuning_error_hz != 0) {
            siggen::FrequencyConfig offset;
            offset.doppler_shift_hz = p.tuning_error_hz;
            REQUIRE(siggen::apply_frequency_offset(*rf, offset, kRate, 1).has_value());
        }
        REQUIRE(siggen::add_awgn(*rf, siggen::NoiseLevel::snr_in_2500_hz_db(p.snr_2500_db), kRate,
                                 0x5CA7ULL)
                    .has_value());
        // A receiver's channel filter before the discriminator, as any FM
        // receiver has: without it the discriminator sees the noise of the
        // whole 48 kHz and sits below its threshold at every level worth
        // measuring. 7 kHz either side passes the Carson bandwidth of
        // 4.5 kHz deviation at 1200 bit/s, 2 x (4.5 + 1.2) = 11.4 kHz.
        auto taps = decode::design_from_response(kRate, 127, channel_response, nullptr);
        REQUIRE(taps.has_value());
        std::vector<dsp::Complex32> filtered(rf->size());
        REQUIRE(decode::filter_complex(*rf, *taps, filtered).has_value());
        std::vector<float> audio(rf->size());
        REQUIRE(decode::fm_discriminate(filtered, audio, kRate).has_value());

        decode::PocsagConfig config;
        config.rate = kRate;
        decode::PocsagStats stats;
        const auto got = decode_audio(config, audio, 0, &stats);
        std::size_t good = 0;
        for (const Sent& s : sent) {
            for (const auto& g : got) {
                if (g.identity == s.identity && g.text == s.text) {
                    ++good;
                    break;
                }
            }
        }
        const double loss = 1.0 - static_cast<double>(good) / static_cast<double>(sent.size());

        // Raw bit error rate, from the same discriminator and clock the
        // decoder uses, against the bits sent, aligned on the preamble's end.
        decode::LevelDiscriminatorConfig level;
        level.rate = kRate;
        level.symbol_rate = decode::kPocsag1200;
        auto discriminator = decode::LevelDiscriminator::create(level);
        REQUIRE(discriminator.has_value());
        decode::BitClockConfig clock_config;
        clock_config.rate = kRate;
        clock_config.symbol_rate = decode::kPocsag1200;
        auto clock = decode::BitClock::create(clock_config);
        REQUIRE(clock.has_value());
        std::vector<float> soft;
        std::vector<decode::SoftBit> soft_bits;
        discriminator->process(audio, soft);
        clock->process(soft, soft_bits);
        const double samples_per_bit = static_cast<double>(kRate) / decode::kPocsag1200;
        std::size_t errors = 0;
        std::size_t compared = 0;
        for (const auto& b : soft_bits) {
            // Less the discriminator's delay and the channel filter's, 63
            // samples for 127 symmetric taps.
            const double at = (static_cast<double>(b.position) -
                               static_cast<double>(discriminator->group_delay()) - 63.0) /
                              samples_per_bit;
            const auto k = static_cast<std::size_t>(std::floor(at));
            if (k < decode::kPocsagPreambleBits || k >= bits.size()) {
                continue;
            }
            errors += ((b.value >= 0.0F ? 0U : 1U) != bits[k]) ? 1U : 0U;
            ++compared;
        }
        const double ber = static_cast<double>(errors) / static_cast<double>(compared);
        auto eb_n0 =
            siggen::reference_bandwidth_to_eb_over_n0_db(p.snr_2500_db, decode::kPocsag1200);
        REQUIRE(eb_n0.has_value());
        const double codeword_loss =
            stats.codewords == 0
                ? 1.0
                : static_cast<double>(stats.uncorrectable) / static_cast<double>(stats.codewords);

        INFO("SNR " << p.snr_2500_db << " dB in 2500 Hz (Eb/N0 " << *eb_n0 << " dB), tuned "
                    << p.tuning_error_hz << " Hz off: bit error rate " << ber << " over "
                    << compared << ", " << stats.corrected_bits
                    << " bits corrected, uncorrectable codewords " << codeword_loss
                    << ", pages lost " << loss);
        CHECK(compared > bits.size() / 2);
        CHECK(loss <= p.allowed_page_loss);
        WARN("POCSAG SNR " << p.snr_2500_db << " dB/2500 Hz (Eb/N0 " << *eb_n0 << " dB), offset "
                           << p.tuning_error_hz << " Hz: BER " << ber << ", corrected bits "
                           << stats.corrected_bits << ", uncorrectable codeword rate "
                           << codeword_loss << ", page loss " << loss);
    }
}
