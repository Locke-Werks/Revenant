// FX.25: the Reed-Solomon codec on its own, the correlation tags against
// Table 1, and the decoder against the transmitter in core/dsp/synth.
//
// The document prints no worked codeblock, so nothing here can see a
// misreading both ends share; core/decode/fx25.h lists the three Reed-Solomon
// parameters the document leaves open, and those are the ones a shared
// misreading would be in. What the round trips do establish is the part the
// document does fix: the tags, the octet order, the padding, and that a frame
// the ordinary AX.25 deframer loses to bit errors comes back through the
// code.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "core/decode/ax25.h"
#include "core/decode/fx25.h"
#include "core/decode/reed_solomon.h"
#include "core/dsp/synth/fsk_mod.h"

using namespace revenant;

namespace {

decode::Ax25Address address(const char* call, std::uint8_t ssid, bool c) {
    decode::Ax25Address a;
    a.callsign = call;
    a.ssid = ssid;
    a.command_or_repeated = c;
    return a;
}

std::vector<std::uint8_t> ui_frame(std::string_view info) {
    siggen::Ax25FrameSpec spec;
    spec.destination = address("APRS", 0, true);
    spec.source = address("N7LEM", 0, false);
    spec.repeaters.push_back(address("WIDE1", 1, false));
    spec.information.assign(info.begin(), info.end());
    auto octets = siggen::ax25_frame_octets(spec);
    REQUIRE(octets.has_value());
    return *octets;
}

std::vector<decode::Ax25Frame> decode_all(const std::vector<float>& audio, std::size_t block = 0,
                                          decode::Ax25Stats* stats = nullptr) {
    auto decoder = decode::Ax25Decoder::create(decode::Ax25Config{});
    REQUIRE(decoder.has_value());
    std::vector<decode::Ax25Frame> frames;
    if (block == 0) {
        decoder->process(audio, frames);
    } else {
        for (std::size_t i = 0; i < audio.size(); i += block) {
            const std::size_t n = std::min(block, audio.size() - i);
            decoder->process(std::span<const float>(audio.data() + i, n), frames);
        }
    }
    if (stats != nullptr) {
        *stats = decoder->stats();
    }
    return frames;
}

decode::Rs8Codec codec(std::size_t check) {
    auto c = decode::Rs8Codec::create(decode::fx25_rs_params(check));
    REQUIRE(c.has_value());
    return std::move(*c);
}

// Distinct positions in [0, n), `count` of them.
std::vector<std::size_t> positions(std::mt19937_64& engine, std::size_t n, std::size_t count) {
    std::vector<std::size_t> all(n);
    for (std::size_t i = 0; i < n; ++i) {
        all[i] = i;
    }
    std::shuffle(all.begin(), all.end(), engine);
    all.resize(count);
    return all;
}

}  // namespace

TEST_CASE("the FX.25 Reed-Solomon generators have their stated roots", "[decode][fx25]") {
    for (const std::size_t check : decode::kFx25CheckSymbolCounts) {
        INFO(check << " check symbols");
        const decode::Rs8Codec c = codec(check);
        const auto& g = c.generator();
        REQUIRE(g.size() == check + 1);
        CHECK(g.front() == 1);
        const auto evaluate = [&](unsigned power) {
            const std::uint8_t x = c.alpha_power(power);
            std::uint8_t acc = 0;
            for (const std::uint8_t coefficient : g) {
                acc = static_cast<std::uint8_t>(c.multiply(acc, x) ^ coefficient);
            }
            return acc;
        };
        for (unsigned j = 0; j < check; ++j) {
            CHECK(evaluate(decode::kFx25FirstRoot + j) == 0);
        }
        // And nowhere else in the run either side.
        CHECK(evaluate(decode::kFx25FirstRoot + static_cast<unsigned>(check)) != 0);
        CHECK(evaluate(decode::kFx25FirstRoot + 254) != 0);
    }
    // A polynomial without x primitive is refused: x^8 + 1 is not even
    // irreducible.
    CHECK_FALSE(decode::Rs8Codec::create({0x101, 1, 16}).has_value());
    CHECK_FALSE(decode::Rs8Codec::create({decode::kFx25FieldPolynomial, 1, 15}).has_value());
}

TEST_CASE("Reed-Solomon corrects up to t symbol errors exactly at every Table 1 length",
          "[decode][fx25]") {
    std::mt19937_64 engine(0xF25ULL);
    for (const decode::Fx25Mode& mode : decode::kFx25Modes) {
        INFO("Tag_" << int(mode.index) << ": RS(" << mode.n << ", " << mode.k << ")");
        const decode::Rs8Codec c = codec(mode.check());
        const std::size_t t = c.correctable();
        for (std::size_t errors = 0; errors <= t; ++errors) {
            for (int trial = 0; trial < 4; ++trial) {
                std::vector<std::uint8_t> word(mode.n);
                for (std::size_t i = 0; i < mode.k; ++i) {
                    word[i] = static_cast<std::uint8_t>(engine());
                }
                c.encode(std::span<const std::uint8_t>(word).first(mode.k),
                         std::span<std::uint8_t>(word).subspan(mode.k));
                const std::vector<std::uint8_t> sent = word;
                for (const std::size_t p : positions(engine, mode.n, errors)) {
                    word[p] = static_cast<std::uint8_t>(word[p] ^ (1U + engine() % 255U));
                }
                const auto corrected = c.decode(word);
                INFO(errors << " errors, trial " << trial);
                REQUIRE(corrected.has_value());
                CHECK(*corrected == errors);
                CHECK(word == sent);
            }
        }
    }
}

TEST_CASE("Reed-Solomon refuses words with more than t symbol errors", "[decode][fx25]") {
    std::mt19937_64 engine(0xBADF25ULL);
    for (const decode::Fx25Mode& mode : decode::kFx25Modes) {
        INFO("Tag_" << int(mode.index) << ": RS(" << mode.n << ", " << mode.k << ")");
        const decode::Rs8Codec c = codec(mode.check());
        const std::size_t t = c.correctable();
        for (const std::size_t errors : {t + 1, t + 2, t + 5, 2 * t}) {
            for (int trial = 0; trial < 8; ++trial) {
                std::vector<std::uint8_t> word(mode.n);
                for (std::size_t i = 0; i < mode.k; ++i) {
                    word[i] = static_cast<std::uint8_t>(engine());
                }
                c.encode(std::span<const std::uint8_t>(word).first(mode.k),
                         std::span<std::uint8_t>(word).subspan(mode.k));
                for (const std::size_t p : positions(engine, mode.n, errors)) {
                    word[p] = static_cast<std::uint8_t>(word[p] ^ (1U + engine() % 255U));
                }
                const std::vector<std::uint8_t> received = word;
                INFO(errors << " errors, trial " << trial);
                CHECK_FALSE(c.decode(word).has_value());
                // A refused word is handed back as it came.
                CHECK(word == received);
            }
        }
    }
}

TEST_CASE("FX.25 correlation tags follow Table 1 and are far enough apart", "[decode][fx25]") {
    // "Correlation Tag Details": "Transmission order of the bytes for Tag_01
    // would be 0x3E 0x2F 0x53 0x8A 0xDF 0xB7 0x4D 0xB7."
    const decode::Fx25Mode* tag01 = decode::fx25_mode(0x01);
    REQUIRE(tag01 != nullptr);
    const std::uint8_t sent[] = {0x3E, 0x2F, 0x53, 0x8A, 0xDF, 0xB7, 0x4D, 0xB7};
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(static_cast<std::uint8_t>(tag01->tag >> (8 * i)) == sent[i]);
    }

    // "FEC Algorithms", Table 1: every assigned code is one of three, and k
    // and n are what the row names.
    for (const decode::Fx25Mode& m : decode::kFx25Modes) {
        CHECK(std::find(decode::kFx25CheckSymbolCounts.begin(),
                        decode::kFx25CheckSymbolCounts.end(),
                        m.check()) != decode::kFx25CheckSymbolCounts.end());
        CHECK(m.n <= decode::kRs8FullLength);
    }

    // The two figures kFx25TagTolerance is argued from. Table 1 with its
    // reserved and undefined rows: at least 24 bits between any two.
    std::vector<std::uint64_t> table = {0x566ED2717946107EULL, 0x0293D578626B67E6ULL,
                                        0xE3B0B0D6917E58A6ULL, 0x720267AF1BE1F846ULL,
                                        0x93210201E8F4C706ULL, 0x41C246CB5DE62A7EULL};
    for (const decode::Fx25Mode& m : decode::kFx25Modes) {
        table.push_back(m.tag);
    }
    int closest = 64;
    for (std::size_t i = 0; i < table.size(); ++i) {
        for (std::size_t j = i + 1; j < table.size(); ++j) {
            closest = std::min(closest, std::popcount(table[i] ^ table[j]));
        }
    }
    CHECK(closest >= 24);
    CHECK(static_cast<int>(2 * decode::kFx25TagTolerance) < closest);

    // Flags then each assigned tag, every window short of the tag: at least
    // 17 bits from any assigned tag.
    int nearest_early = 64;
    for (const decode::Fx25Mode& m : decode::kFx25Modes) {
        for (unsigned shift = 1; shift < 64; ++shift) {
            const std::uint64_t window = (0x7E7E7E7E7E7E7E7EULL >> shift) | (m.tag << (64 - shift));
            for (const decode::Fx25Mode& other : decode::kFx25Modes) {
                nearest_early = std::min(nearest_early, std::popcount(window ^ other.tag));
            }
        }
    }
    CHECK(nearest_early >= 17);
    CHECK(static_cast<int>(decode::kFx25TagTolerance) < nearest_early);

    // Matching inside the tolerance and not past it.
    std::mt19937_64 engine(0x7A6ULL);
    for (const decode::Fx25Mode& m : decode::kFx25Modes) {
        for (const unsigned flips : {0U, 1U, 5U, decode::kFx25TagTolerance}) {
            std::uint64_t window = m.tag;
            for (const std::size_t p : positions(engine, 64, flips)) {
                window ^= std::uint64_t{1} << p;
            }
            const auto match = decode::fx25_match_tag(window);
            REQUIRE(match.has_value());
            CHECK(match->mode == &m);
            CHECK(match->errors == flips);
        }
        std::uint64_t window = m.tag;
        for (const std::size_t p : positions(engine, 64, decode::kFx25TagTolerance + 1)) {
            window ^= std::uint64_t{1} << p;
        }
        CHECK_FALSE(decode::fx25_match_tag(window).has_value());
    }
    CHECK_FALSE(decode::fx25_match_tag(0x7E7E7E7E7E7E7E7EULL).has_value());
}

TEST_CASE("an FX.25 codeblock is the AX.25 packet, 0x7E padding and check octets",
          "[decode][fx25]") {
    const std::vector<std::uint8_t> frame = ui_frame(">FX.25 codeblock layout");
    const decode::Fx25Mode* mode = nullptr;
    auto word = siggen::fx25_codeblock(frame, siggen::Fx25Choice{}, mode);
    REQUIRE(word.has_value());
    REQUIRE(mode != nullptr);
    // 21 address octets, control, PID, 23 information octets and the FCS
    // are 48 octets, more after stuffing and the two flags: RS(80,64) is the
    // shortest 16-check code that holds them.
    CHECK(mode->index == 0x03);
    REQUIRE(word->size() == mode->n);
    // "AX.25 Packet Requirements": an opening flag. "Pad Requirements":
    // 0x7E to the end of the information octets.
    CHECK((*word)[0] == decode::kAx25Flag);
    CHECK((*word)[mode->k - 1] == decode::kAx25Flag);
    CHECK((*word)[mode->k - 2] == decode::kAx25Flag);

    const decode::Rs8Codec c = codec(mode->check());
    std::vector<std::uint8_t> copy = *word;
    const auto corrected = c.decode(copy);
    REQUIRE(corrected.has_value());
    CHECK(*corrected == 0);

    // The packet's bits are the ordinary deframer's input: reading them back
    // gives the frame and its FCS.
    decode::HdlcDeframer deframer;
    std::vector<decode::HdlcFrame> out;
    for (std::size_t i = 0; i < 8 * mode->k; ++i) {
        deframer.push(static_cast<std::uint8_t>(((*word)[i / 8] >> (i % 8)) & 1U), i, out);
    }
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].octets.size() == frame.size() + 2);
    CHECK(std::equal(frame.begin(), frame.end(), out[0].octets.begin()));

    // A packet too long for the code asked for is refused.
    siggen::Fx25Choice small;
    small.tag = 0x04;
    CHECK_FALSE(siggen::fx25_codeblock(frame, small, mode).has_value());
}

TEST_CASE("FX.25 frames round trip through Bell 202 under every Table 1 code", "[decode][fx25]") {
    // RS(48,32) and RS(64,32) hold 32 octets of packet, so the short frame
    // has no repeater and two octets of information.
    siggen::Ax25FrameSpec short_spec;
    short_spec.destination = address("APRS", 0, true);
    short_spec.source = address("N7LEM", 0, false);
    short_spec.information = {'>', 'x'};
    auto short_frame = siggen::ax25_frame_octets(short_spec);
    REQUIRE(short_frame.has_value());
    const std::vector<std::vector<std::uint8_t>> all = {
        ui_frame("!4903.50N/07201.75W-FX.25 round trip"),
        ui_frame(std::string("\x7E\x7E\xFF\xFF\xFE\x7F\x3F", 7)),
        *short_frame,
    };
    for (const decode::Fx25Mode& mode : decode::kFx25Modes) {
        INFO("Tag_" << int(mode.index));
        siggen::Fx25Choice choice;
        choice.tag = mode.index;
        // The frames this code can carry.
        std::vector<std::vector<std::uint8_t>> frames;
        for (const auto& f : all) {
            const decode::Fx25Mode* used = nullptr;
            if (siggen::fx25_codeblock(f, choice, used).has_value()) {
                frames.push_back(f);
            }
        }
        REQUIRE_FALSE(frames.empty());
        auto audio = siggen::fx25_render(siggen::Ax25ModConfig{}, frames, choice);
        REQUIRE(audio.has_value());
        decode::Ax25Stats stats;
        const auto got = decode_all(*audio, 0, &stats);
        CHECK(stats.fx25_blocks == frames.size());
        CHECK(stats.fx25_uncorrectable == 0);
        // One frame each: the copy the ordinary deframer also found is
        // replaced, not repeated.
        REQUIRE(got.size() == frames.size());
        for (std::size_t i = 0; i < frames.size(); ++i) {
            CHECK(got[i].octets == frames[i]);
            CHECK(got[i].fx25);
            CHECK(got[i].fx25_tag == mode.index);
            CHECK(got[i].fx25_corrected == 0);
        }
    }
}

TEST_CASE("FX.25 recovers a frame that bit errors cost plain AX.25", "[decode][fx25]") {
    const std::vector<std::vector<std::uint8_t>> frames = {
        ui_frame("=4903.50N/07201.75W#a frame long enough to take several hits")};
    siggen::Ax25ModConfig mod;
    siggen::Fx25Choice choice;
    choice.check_symbols = 16;
    auto clean = siggen::fx25_bits(frames, choice, mod);
    REQUIRE(clean.has_value());
    const decode::Fx25Mode* mode = nullptr;
    REQUIRE(siggen::fx25_codeblock(frames[0], choice, mode).has_value());
    const std::size_t codeblock_start = 8 * mod.leading_flags + decode::kFx25TagBits;

    // Bits flipped before NRZI, which the receiver's NRZI decoding turns
    // back into exactly those bit errors. Each lands in a different octet of
    // the AX.25 packet, so each costs the code one symbol.
    const auto corrupt = [&](std::size_t symbols) {
        std::vector<std::uint8_t> bits = *clean;
        for (std::size_t s = 0; s < symbols; ++s) {
            const std::size_t octet = 3 + 4 * s;
            bits[codeblock_start + 8 * octet + (s % 8)] ^= 1U;
        }
        return bits;
    };
    const auto with_errors = [&](std::size_t symbols) {
        auto audio = siggen::afsk_render_bits(mod, corrupt(symbols));
        REQUIRE(audio.has_value());
        return *audio;
    };
    // What plain AX.25 makes of the same bits, deframed directly so the
    // modem plays no part: no frame whose FCS checks.
    const auto plain_frames = [&](std::size_t symbols) {
        decode::HdlcDeframer deframer;
        std::vector<decode::HdlcFrame> out;
        const std::vector<std::uint8_t> bits = corrupt(symbols);
        for (std::size_t i = 0; i < bits.size(); ++i) {
            deframer.push(bits[i], i, out);
        }
        std::size_t good = 0;
        for (const auto& f : out) {
            const std::size_t n = f.octets.size();
            const auto fcs = static_cast<std::uint16_t>(
                f.octets[n - 2] | (static_cast<unsigned>(f.octets[n - 1]) << 8U));
            good += decode::ax25_fcs(std::span(f.octets).first(n - 2)) == fcs ? 1U : 0U;
        }
        return good;
    };
    REQUIRE(plain_frames(0) == 1);

    // Within the code's reach: plain AX.25 loses the frame, and the
    // codeblock hands it back corrected.
    for (const std::size_t symbols : {std::size_t{1}, std::size_t{4}, mode->check() / 2}) {
        INFO(symbols << " octets hit");
        CHECK(plain_frames(symbols) == 0);
        decode::Ax25Stats stats;
        const auto got = decode_all(with_errors(symbols), 0, &stats);
        CHECK(stats.fx25_blocks == 1);
        CHECK(stats.fx25_uncorrectable == 0);
        REQUIRE(got.size() == 1);
        CHECK(got[0].octets == frames[0]);
        CHECK(got[0].fx25);
        CHECK(got[0].fx25_corrected == symbols);
    }

    // One past it: refused, and nothing reported.
    {
        decode::Ax25Stats stats;
        const auto got = decode_all(with_errors(mode->check() / 2 + 1), 0, &stats);
        CHECK(got.empty());
        CHECK(stats.fx25_blocks == 1);
        CHECK(stats.fx25_uncorrectable == 1);
    }

    // And errors in the tag itself, up to the tolerance, still find it.
    {
        std::vector<std::uint8_t> bits = *clean;
        for (std::size_t b = 0; b < decode::kFx25TagBits; b += 9) {
            bits[8 * mod.leading_flags + b] ^= 1U;
        }
        auto audio = siggen::afsk_render_bits(mod, bits);
        REQUIRE(audio.has_value());
        const auto got = decode_all(*audio);
        REQUIRE(got.size() == 1);
        CHECK(got[0].fx25);
        CHECK(got[0].octets == frames[0]);
    }
}

TEST_CASE("plain AX.25 decodes unchanged beside FX.25", "[decode][fx25]") {
    const std::vector<std::vector<std::uint8_t>> plain = {ui_frame(">plain one"),
                                                          ui_frame(">plain two")};
    const std::vector<std::vector<std::uint8_t>> wrapped = {ui_frame(">wrapped")};

    // Plain frames alone: no tag found, nothing marked.
    auto audio = siggen::ax25_render(siggen::Ax25ModConfig{}, plain);
    REQUIRE(audio.has_value());
    decode::Ax25Stats stats;
    auto got = decode_all(*audio, 0, &stats);
    CHECK(stats.fx25_blocks == 0);
    REQUIRE(got.size() == 2);
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK(got[i].octets == plain[i]);
        CHECK_FALSE(got[i].fx25);
    }

    // A plain transmission, then an FX.25 one, then a plain one, in a
    // single stream: all three, in order, only the middle one marked.
    siggen::Ax25ModConfig mod;
    std::vector<std::uint8_t> bits = siggen::hdlc_bits(std::span(plain).first(1), mod);
    auto fx = siggen::fx25_bits(wrapped, siggen::Fx25Choice{}, mod);
    REQUIRE(fx.has_value());
    bits.insert(bits.end(), fx->begin(), fx->end());
    const std::vector<std::uint8_t> tail = siggen::hdlc_bits(std::span(plain).subspan(1), mod);
    bits.insert(bits.end(), tail.begin(), tail.end());
    auto mixed = siggen::afsk_render_bits(mod, bits);
    REQUIRE(mixed.has_value());
    for (const std::size_t block : {std::size_t{0}, std::size_t{1}, std::size_t{333}}) {
        INFO("block of " << block);
        got = decode_all(*mixed, block);
        REQUIRE(got.size() == 3);
        CHECK(got[0].octets == plain[0]);
        CHECK_FALSE(got[0].fx25);
        CHECK(got[1].octets == wrapped[0]);
        CHECK(got[1].fx25);
        CHECK(got[2].octets == plain[1]);
        CHECK_FALSE(got[2].fx25);
        CHECK(got[0].first_sample < got[1].first_sample);
        CHECK(got[1].first_sample < got[2].first_sample);
    }
}
