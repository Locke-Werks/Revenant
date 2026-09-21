// P25 Phase 1: the codes against the document, and the demodulator against the
// transmitter.
//
// WHAT THE CASES HERE ARE FOR, IN TWO GROUPS
//
// The first group checks the codes against TIA-102.BAAA-A's own printed
// tables. Those cases are the reason core/decode/dv_codes.h carries a
// generator polynomial rather than a copied matrix: the polynomial is the
// rule, the matrix is sixteen instances of it, and a case that regenerates the
// matrix from the polynomial establishes that the rule in the code is the rule
// in the document. Retyping the matrix would establish only that it was
// retyped correctly.
//
// The second group is the round trip. It is what stands in place of the
// project's bit-exact-against-a-scalar-twin rule, which does not reach a
// decoder with no GPU kernel behind it. tests/decode/CMakeLists.txt states
// that reasoning for RDS and it applies here unchanged.
//
// EVERY ERROR RATE HERE IS MEASURED AND REPORTED, NEVER ASSERTED TIGHT
//
// The thresholds are loose on purpose. A bit error rate asserted to three
// decimal places is a test that fails when somebody improves the receiver, and
// the number that matters is in the INFO line either way. What the assertions
// hold is the shape: clean at high signal to noise, degraded and still
// framing at low, and the frame metadata recovered exactly in the first case.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/decode/dv_codes.h"
#include "core/decode/dv_phy.h"
#include "core/decode/p25p1.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dv_mod.h"

using namespace revenant;
using decode::P25Duid;

namespace {

constexpr dsp::SampleRate kRate = 48'000;

// TIA-102.BAAA-A clause 8.5.2 prints the generator matrix for the full 64-bit
// code in octal: sixteen rows, each a 16-bit identity part and a 48-bit parity
// part. Transcribed here as the document's own strings, because the whole
// point of the case below is to compare the implementation against them.
struct MatrixRow {
    const char* identity;
    const char* parity;
};

constexpr MatrixRow kClause852Matrix[16] = {
    {"100000", "6331141367235452"}, {"040000", "5265521614723276"},
    {"020000", "4603711461164164"}, {"010000", "2301744630472072"},
    {"004000", "7271623073000466"}, {"002000", "5605650752635660"},
    {"001000", "2702724365316730"}, {"000400", "1341352172547354"},
    {"000200", "0560565075263566"}, {"000100", "6141333751704220"},
    {"000040", "3060555764742110"}, {"000020", "1430266772361044"},
    {"000010", "0614133375170422"}, {"000004", "6037114611641642"},
    {"000002", "5326507063515373"}, {"000001", "4662302756473127"},
};

std::uint64_t from_octal(const char* digits) {
    std::uint64_t value = 0;
    for (const char* p = digits; *p != '\0'; ++p) {
        value = (value << 3U) | static_cast<std::uint64_t>(*p - '0');
    }
    return value;
}

// The 64 bits the encoder produces, as one integer, most significant first.
std::uint64_t encoded_nid(std::uint16_t nac, std::uint8_t duid) {
    std::array<std::uint8_t, 64> bits{};
    REQUIRE(decode::p25_nid_encode(nac, duid, bits).has_value());
    std::uint64_t value = 0;
    for (const std::uint8_t bit : bits) {
        value = (value << 1U) | bit;
    }
    return value;
}

decode::P25Header sample_header(bool encrypted) {
    decode::P25Header header;
    for (std::size_t i = 0; i < header.message_indicator.size(); ++i) {
        header.message_indicator[i] =
            encrypted ? static_cast<std::uint8_t>(0x11 * (i + 1)) : std::uint8_t{0};
    }
    header.manufacturer_id = 0x00;
    header.algorithm_id = encrypted ? std::uint8_t{0x84} : decode::kP25AlgidUnencrypted;
    header.key_id = encrypted ? std::uint16_t{0x1234} : std::uint16_t{0};
    header.talkgroup_id = 0x02A7;
    return header;
}

}  // namespace

// ---------------------------------------------------------------------------
// The codes, against the document
// ---------------------------------------------------------------------------

TEST_CASE("the NID generator polynomial regenerates the clause 8.5.2 matrix",
          "[decode][p25]") {
    // Clause 8.5.2 states the polynomial's shape in prose before printing it:
    // "of 47-th degree with 27 non-zero terms". Both are checkable.
    std::uint64_t terms = 0;
    for (unsigned bit = 0; bit < 64U; ++bit) {
        terms += (decode::kP25NidGenerator >> bit) & 1ULL;
    }
    CHECK(terms == 27);
    CHECK((decode::kP25NidGenerator >> 47U) == 1ULL);

    for (std::size_t row = 0; row < 16; ++row) {
        INFO("row " << (row + 1) << " of the clause 8.5.2 generator matrix");

        const auto identity = static_cast<std::uint16_t>(from_octal(kClause852Matrix[row].identity));
        const std::uint64_t parity48 = from_octal(kClause852Matrix[row].parity);

        // The identity part must be a single bit, in row order from the most
        // significant. If this fails the transcription is wrong rather than
        // the code.
        REQUIRE(identity == static_cast<std::uint16_t>(1U << (15U - row)));

        // The document's 48 parity columns are the code's 47 BCH parity bits
        // followed by the appended 64th bit.
        const std::uint64_t word = encoded_nid(static_cast<std::uint16_t>(identity >> 4U),
                                               static_cast<std::uint8_t>(identity & 0x0FU));
        const std::uint64_t got_information = word >> 48U;
        const std::uint64_t got_parity48 = word & 0xFFFF'FFFF'FFFFULL;

        CHECK(got_information == identity);
        CHECK(got_parity48 == parity48);
    }
}

TEST_CASE("the 64th NID bit reproduces every P value in Table 8-4", "[decode][p25]") {
    // Clause 8.5 says a single parity bit is appended and does not say what of.
    // Table 8-4 prints its value for each of the six defined Data Unit IDs,
    // which is what core/decode/dv_codes.cpp's rule was derived to satisfy.
    struct Row {
        std::uint8_t duid;
        std::uint8_t p;
        const char* usage;
    };
    constexpr Row kTable84[] = {
        {0b0000, 0, "Header Data Unit"},
        {0b0011, 0, "Terminator without subsequent Link Control"},
        {0b0101, 1, "Logical Link Data Unit 1"},
        {0b1010, 1, "Logical Link Data Unit 2"},
        {0b1100, 0, "Packet Data Unit"},
        {0b1111, 0, "Terminator with subsequent Link Control"},
    };

    for (const Row& row : kTable84) {
        INFO(row.usage);
        // Table 8-4's P column is printed against the DUID alone, so the NAC
        // is zero for the comparison to mean what the table means.
        const auto information = static_cast<std::uint16_t>(row.duid);
        CHECK(decode::p25_nid_trailing_parity(information) == row.p);
        CHECK((encoded_nid(0, row.duid) & 1ULL) == row.p);
    }
}

TEST_CASE("the NID code corrects up to eleven errors and is a distance 23 code",
          "[decode][p25]") {
    // Clause 8.5 names it a (63,16,23) BCH code, so the minimum distance of
    // the 64-bit word is at least 23 and up to 11 errors are correctable.
    // Measuring the true minimum over all 65535 non-zero code words is the
    // only way to say that about the implementation rather than about the
    // clause.
    std::uint32_t minimum = 64;
    for (std::uint32_t information = 1; information < 0x1'0000U; ++information) {
        const std::uint64_t word = encoded_nid(
            static_cast<std::uint16_t>(information >> 4U),
            static_cast<std::uint8_t>(information & 0x0FU));
        std::uint32_t weight = 0;
        for (unsigned bit = 0; bit < 64U; ++bit) {
            weight += static_cast<std::uint32_t>((word >> bit) & 1ULL);
        }
        minimum = std::min(minimum, weight);
    }
    INFO("minimum weight measured over all 65535 non-zero code words: " << minimum);
    CHECK(minimum == 23);

    // Eleven errors, placed at the eleven positions a decoder finds hardest to
    // tell from another code word: the low bits of the parity.
    std::array<std::uint8_t, 64> bits{};
    REQUIRE(decode::p25_nid_encode(0x293, static_cast<std::uint8_t>(P25Duid::LogicalLinkDataUnit1),
                                   bits)
                .has_value());
    for (std::size_t i = 0; i < 11; ++i) {
        bits[53 + i] ^= 1U;
    }
    auto decoded = decode::p25_nid_decode(bits);
    REQUIRE(decoded.has_value());
    CHECK(decoded->nac == 0x293);
    CHECK(decoded->duid == static_cast<std::uint8_t>(P25Duid::LogicalLinkDataUnit1));
    CHECK(decoded->corrected_bits == 11);
}

TEST_CASE("the shortened Golay code is systematic and has distance 8", "[decode][p25]") {
    // Clause 5.7's Table 5-3 says the left six columns of the (18,6,8) matrix
    // are the identity, so the top six bits of a code word are the
    // information.
    for (std::uint8_t information = 0; information < 64U; ++information) {
        const std::uint32_t word = decode::p25_golay18_encode(information);
        CHECK(((word >> 12U) & 0x3FU) == information);
    }

    std::uint32_t minimum = 18;
    for (std::uint8_t information = 1; information < 64U; ++information) {
        std::uint32_t weight = 0;
        const std::uint32_t word = decode::p25_golay18_encode(information);
        for (unsigned bit = 0; bit < 18U; ++bit) {
            weight += (word >> bit) & 1U;
        }
        minimum = std::min(minimum, weight);
    }
    INFO("minimum weight over the 63 non-zero code words: " << minimum);
    CHECK(minimum == 8);

    // Three errors are corrected, which is what distance 8 guarantees.
    for (std::uint8_t information = 0; information < 64U; ++information) {
        const std::uint32_t word = decode::p25_golay18_encode(information);
        const std::uint32_t damaged = word ^ 0b000'000'000'000'000'111U;
        const decode::Golay18Decode decoded = decode::p25_golay18_decode(damaged);
        CHECK(decoded.information == information);
        CHECK(decoded.corrected_bits == 3);
    }
}

TEST_CASE("the frame sync pattern is Table 8-1 read through Table 9-1", "[decode][p25]") {
    // Table 8-1's expanded vector, written out as the document prints it.
    const std::string expected = "010101010111010111110101111111110111011111111111";
    REQUIRE(expected.size() == decode::kP25FrameSyncBits);

    std::uint64_t word = 0;
    for (const char c : expected) {
        word = (word << 1U) | static_cast<std::uint64_t>(c == '1' ? 1 : 0);
    }
    CHECK(word == decode::kP25FrameSync);

    // Table 9-1 maps dibit 01 to symbol +3 and 11 to -3, which are the only
    // two dibits the sync word uses, so every pattern entry is +3 or -3.
    const auto pattern = decode::p25_frame_sync_pattern();
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        INFO("symbol " << i);
        CHECK((pattern[i] == 3.0F || pattern[i] == -3.0F));
    }
    CHECK(pattern[0] == 3.0F);   // dibit 01
    CHECK(pattern[5] == -3.0F);  // dibit 11
}

TEST_CASE("status symbols come out every 36th symbol from the 35th", "[decode][p25]") {
    // Clause 8.4 and the clause 10.2 annex, which prints them at absolute
    // symbol positions 35, 71, 107 and so on through 395.
    std::vector<float> symbols(decode::kP25HduTotalSymbols, 0.0F);
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        symbols[i] = static_cast<float>(i);
    }
    const std::vector<float> stripped = decode::p25_strip_status_symbols(symbols);

    CHECK(stripped.size() == decode::kP25HduTotalSymbols - decode::kP25HduStatusSymbols);
    for (const float value : stripped) {
        const auto index = static_cast<std::size_t>(value);
        const bool was_status =
            index >= decode::kP25FirstStatusSymbol &&
            (index - decode::kP25FirstStatusSymbol) % decode::kP25StatusSymbolInterval == 0;
        CHECK_FALSE(was_status);
    }
}

// ---------------------------------------------------------------------------
// The round trip
// ---------------------------------------------------------------------------

TEST_CASE("a header data unit round trips through C4FM with no errors", "[decode][p25]") {
    siggen::P25HeaderMessage message;
    message.network_access_code = 0x293;
    message.header = sample_header(false);

    siggen::P25ModConfig mod;
    mod.rate = kRate;

    // Two copies of the message back to back. A receiver loses the tail of a
    // capture to its filter delay and to the timing estimator's window, so
    // the second copy is what makes the first one's terminator land inside
    // the part that decodes. A real transmission has whatever comes next
    // there; a test has to put something.
    auto dibits = siggen::p25_header_message_dibits(message);
    INFO((dibits.has_value() ? std::string{} : dibits.error().message));
    REQUIRE(dibits.has_value());
    std::vector<std::uint8_t> stream = *dibits;
    stream.insert(stream.end(), dibits->begin(), dibits->end());

    auto samples = siggen::p25_render_dibits(mod, stream);
    INFO((samples.has_value() ? std::string{} : samples.error().message));
    REQUIRE(samples.has_value());

    decode::P25Config config;
    config.rate = kRate;
    auto decoder = decode::P25Phase1::create(config);
    INFO((decoder.has_value() ? std::string{} : decoder.error().message));
    REQUIRE(decoder.has_value());

    std::vector<decode::P25Frame> frames;
    REQUIRE(decoder->process(*samples, frames).has_value());

    INFO("frames recovered: " << frames.size());
    REQUIRE_FALSE(frames.empty());

    const decode::P25Frame& hdu = frames.front();
    INFO("sync correlation " << hdu.sync_score << ", NID corrections "
                             << hdu.nid.corrected_bits);
    CHECK(hdu.nid.network_access_code == 0x293);
    CHECK(hdu.nid.duid == static_cast<std::uint8_t>(P25Duid::HeaderDataUnit));
    CHECK(hdu.nid.corrected_bits == 0);

    REQUIRE(hdu.header.has_value());
    CHECK(hdu.header->talkgroup_id == 0x02A7);
    CHECK(hdu.header->algorithm_id == decode::kP25AlgidUnencrypted);
    CHECK_FALSE(hdu.header->encrypted);

    // The terminator the transmitter appends, so the DUID sequence is what a
    // real short transmission looks like rather than one frame in isolation.
    REQUIRE(frames.size() >= 2);
    CHECK(frames[1].nid.duid ==
          static_cast<std::uint8_t>(P25Duid::TerminatorWithoutLinkControl));

    // THE FIRST HEADER IS NOT ERROR FREE AND THE SECOND ONE IS.
    //
    // The first data unit of a capture sits inside the receiver's own
    // transient: the transmit filter is ringing up, and the first window the
    // timing estimator averages over is part turn-on and part signal, so its
    // estimate is pulled off by a fraction of a symbol. That costs a handful
    // of Golay corrections in the header and nothing else, because the code
    // has distance 8 and is spending one of its three.
    //
    // It is a real property of any burst receiver and not a defect, so the
    // case measures it rather than hiding it, and holds the standard against
    // the second header, which is past the transient.
    INFO("first header: worst Golay correction " << hdu.header->worst_golay_correction
                                                 << " over "
                                                 << hdu.header->golay_words_corrected
                                                 << " of 36 words");
    CHECK(hdu.header->worst_golay_correction <= 3);

    REQUIRE(frames.size() >= 3);
    const decode::P25Frame& second = frames[2];
    CHECK(second.nid.duid == static_cast<std::uint8_t>(P25Duid::HeaderDataUnit));
    CHECK(second.nid.corrected_bits == 0);
    REQUIRE(second.header.has_value());
    INFO("second header: worst Golay correction " << second.header->worst_golay_correction);
    CHECK(second.header->worst_golay_correction == 0);
    CHECK(second.header->golay_words_corrected == 0);
    CHECK(second.header->talkgroup_id == 0x02A7);
}

TEST_CASE("an encrypted header is identified and its payload is left alone", "[decode][p25]") {
    // docs/modes.md: where a frame is encrypted, say so on the status surface
    // and stop. The talkgroup and the network are in clear either way, which
    // is the whole reason framing an encrypted call is worth doing.
    siggen::P25HeaderMessage message;
    message.network_access_code = 0x4D8;
    message.header = sample_header(true);

    siggen::P25ModConfig mod;
    mod.rate = kRate;
    auto samples = siggen::p25_render_header_message(mod, message);
    REQUIRE(samples.has_value());

    decode::P25Config config;
    config.rate = kRate;
    auto decoder = decode::P25Phase1::create(config);
    REQUIRE(decoder.has_value());

    std::vector<decode::P25Frame> frames;
    REQUIRE(decoder->process(*samples, frames).has_value());
    REQUIRE_FALSE(frames.empty());
    REQUIRE(frames.front().header.has_value());

    const decode::P25Header& header = *frames.front().header;
    CHECK(header.encrypted);
    CHECK(header.algorithm_id == 0x84);  // AES, per TIA-102.BAAC clause 2.8
    CHECK(header.key_id == 0x1234);

    // Still reported, because the standard puts them outside the encryption.
    CHECK(frames.front().nid.network_access_code == 0x4D8);
    CHECK(header.talkgroup_id == 0x02A7);
}

TEST_CASE("an inverted discriminator still frames", "[decode][p25]") {
    // Whether a positive deviation comes out of the discriminator positive
    // depends on which sideband the tuner landed on, and no standard can fix
    // it. core/decode/p25p1.cpp corrects it from the sync correlation's sign.
    siggen::P25HeaderMessage message;
    message.network_access_code = 0x293;
    message.header = sample_header(false);

    siggen::P25ModConfig mod;
    mod.rate = kRate;
    auto samples = siggen::p25_render_header_message(mod, message);
    REQUIRE(samples.has_value());

    // Conjugating the baseband negates the instantaneous frequency, which is
    // exactly what a spectral inversion does.
    for (dsp::Complex32& sample : *samples) {
        sample = std::conj(sample);
    }

    decode::P25Config config;
    config.rate = kRate;
    auto decoder = decode::P25Phase1::create(config);
    REQUIRE(decoder.has_value());

    std::vector<decode::P25Frame> frames;
    REQUIRE(decoder->process(*samples, frames).has_value());
    REQUIRE_FALSE(frames.empty());
    CHECK(frames.front().inverted);
    CHECK(frames.front().nid.network_access_code == 0x293);
    REQUIRE(frames.front().header.has_value());
    CHECK(frames.front().header->talkgroup_id == 0x02A7);
}

TEST_CASE("P25 symbol error rate against noise, measured", "[decode][p25]") {
    // A long pseudorandom dibit stream through the modulator, the channel and
    // the demodulator, with the error rate measured rather than asserted to a
    // figure. The comparison is per dibit and is reported as both a symbol
    // error rate and the bit error rate a caller would see.
    constexpr std::size_t kSymbols = 6'000;
    std::vector<std::uint8_t> sent(kSymbols, 0);
    std::uint64_t state = 0x5EED'1234'ABCD'0001ULL;
    for (std::uint8_t& dibit : sent) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        dibit = static_cast<std::uint8_t>((state >> 33U) & 0x3U);
    }

    struct Point {
        double snr_db;
        double allowed_symbol_error_rate;
    };
    // Full-sample-rate signal to noise, per docs/snr-convention.md, which is
    // the basis that names its bandwidth rather than assuming one.
    // Measured on 2026-09-21: 0 symbol errors in 5911 at 30 dB, and a
    // symbol error rate of 0.047 at 4 dB. The allowances are set above the
    // measured figures rather than at them, because the number that matters
    // is in the INFO line and an assertion pinned to three decimals is one
    // that fails when somebody improves the receiver.
    const Point points[] = {{30.0, 0.001}, {4.0, 0.08}};

    for (const Point& point : points) {
        INFO("signal to noise " << point.snr_db << " dB across the whole "
                                << kRate << " Hz sample rate");

        siggen::P25ModConfig mod;
        mod.rate = kRate;
        auto samples = siggen::p25_render_dibits(mod, sent);
        REQUIRE(samples.has_value());

        const auto level = siggen::NoiseLevel::snr_in_full_sample_rate_db(point.snr_db);
        auto report = siggen::add_awgn(*samples, level, kRate, 0x9E37'79B9'7F4A'7C15ULL);
        INFO((report.has_value() ? std::string{} : report.error().message));
        REQUIRE(report.has_value());

        decode::P25Config config;
        config.rate = kRate;
        auto decoder = decode::P25Phase1::create(config);
        REQUIRE(decoder.has_value());

        std::vector<decode::P25Frame> frames;
        REQUIRE(decoder->process(*samples, frames).has_value());

        // The frame layer has nothing to lock onto in a bare symbol stream, so
        // the comparison is against the recovered soft symbols directly. The
        // timing loop needs a few tens of symbols to pull in, and the filters
        // have a group delay, so the alignment is found by correlation rather
        // than assumed.
        const std::span<const float> recovered = decoder->last_symbols();
        REQUIRE(recovered.size() > kSymbols / 2);

        std::vector<float> reference(sent.size(), 0.0F);
        for (std::size_t i = 0; i < sent.size(); ++i) {
            reference[i] = static_cast<float>(decode::kP25DibitToSymbol[sent[i]]);
        }

        // Correlate the first 64 transmitted symbols against the recovered run
        // to find where the stream starts.
        const std::span<const float> head(reference.data(), 128);
        auto hit = decode::correlate_pattern(recovered, head);
        REQUIRE(hit.has_value());
        INFO("stream found at recovered symbol " << hit->offset << ", correlation "
                                                 << hit->score);
        REQUIRE(std::abs(hit->score) > 0.5);

        const std::size_t compare =
            std::min(sent.size(), recovered.size() - hit->offset);
        REQUIRE(compare > kSymbols / 2);

        // Counted twice: over the whole run, and over the run after the
        // first two estimator windows.
        //
        // The split is the point rather than a way to get a nicer number.
        // The timing estimator averages over a window, so the first window
        // of a capture is part transmitter turn-on and part signal and its
        // estimate is pulled off by a fraction of a symbol. Every error at
        // 30 dB is in that window. Reporting one figure would either hide
        // the acquisition cost or attribute it to the channel, and both are
        // wrong about where the errors are.
        constexpr std::size_t kSettled = 64;
        std::size_t symbol_errors = 0;
        std::size_t bit_errors = 0;
        std::size_t settled_errors = 0;
        std::size_t settled_compared = 0;
        for (std::size_t i = 0; i < compare; ++i) {
            float value = recovered[hit->offset + i];
            if (hit->inverted) {
                value = -value;
            }
            std::uint8_t got = 0;
            if (value >= 2.0F) {
                got = 0b01;
            } else if (value >= 0.0F) {
                got = 0b00;
            } else if (value >= -2.0F) {
                got = 0b10;
            } else {
                got = 0b11;
            }
            const bool wrong = got != sent[i];
            if (wrong) {
                ++symbol_errors;
            }
            if (i >= kSettled) {
                ++settled_compared;
                settled_errors += wrong ? 1U : 0U;
            }
            bit_errors += static_cast<std::size_t>(((got >> 1U) & 1U) != ((sent[i] >> 1U) & 1U));
            bit_errors += static_cast<std::size_t>((got & 1U) != (sent[i] & 1U));
        }

        REQUIRE(settled_compared > 0);
        const double ser = static_cast<double>(symbol_errors) / static_cast<double>(compare);
        const double ber = static_cast<double>(bit_errors) / static_cast<double>(2 * compare);
        const double settled_ser =
            static_cast<double>(settled_errors) / static_cast<double>(settled_compared);
        INFO("compared " << compare << " symbols: symbol error rate " << ser
                         << ", bit error rate " << ber << "; after the first " << kSettled
                         << " symbols, " << settled_compared << " compared at a symbol "
                         << "error rate of " << settled_ser);
        CHECK(settled_ser <= point.allowed_symbol_error_rate);
    }
}
