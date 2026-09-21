// P25 Phase 1 IMBE vocoder, decode side.
//
// TIA-102.BABA prints no test vector and no golden buffer for the decode
// direction. Its one worked example, section 10, runs spectral amplitudes
// forward into quantizer values, which is the encoder. So there is nothing
// here to assert against a buffer the document supplies, and every case below
// asserts against something the document states instead: a figure drawn in
// full, a property claimed in prose, a table that has to add up, or an
// arithmetic identity.
//
// Each case says, at the top, what wrong implementation it rejects. A case
// that passes against any plausible implementation verifies nothing and reads
// as though it does, which is the failure this file is written to avoid.
//
// Two of the cases are measurements rather than assertions of a known answer.
// They print what they measured and assert a floor well under it, because the
// question they ask, "does this actually detect a corrupted frame", is one the
// standard asserts and does not prove.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <print>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/decode/imbe.h"

namespace {

using revenant::decode::ImbeDecoder;
using revenant::decode::ImbeFrameCause;
using revenant::decode::ImbeFrameState;
using revenant::decode::imbe_frame_map;
using revenant::decode::imbe_pack_frame;
using revenant::decode::imbe_pitch_refinement_window;
using revenant::decode::imbe_priority_scan;
using revenant::decode::imbe_synthesis_window;

constexpr double kPi = std::numbers::pi;

// Equations (46) through (48), written out here rather than read off the
// decoder. A test that asks the code under test for its own expected value
// proves only that the code is self-consistent.
[[nodiscard]] double fundamental_for(std::uint32_t pitch) {
    return 4.0 * kPi / (static_cast<double>(pitch) + 39.5);
}

[[nodiscard]] std::uint32_t harmonics_for(std::uint32_t pitch) {
    const double omega = fundamental_for(pitch);
    return static_cast<std::uint32_t>(
        std::floor(0.9254 * std::floor(kPi / omega + 0.25)));
}

[[nodiscard]] std::uint32_t bands_for(std::uint32_t harmonics) {
    return harmonics <= 36 ? (harmonics + 2) / 3 : 12;
}

// The quantizer values of section 6, before any of the bit manipulation of
// section 7.
struct Quantizers {
    std::uint32_t pitch = 0;                   // b0, eight bits
    std::uint32_t voicing = 0;                 // b1, K bits
    std::uint32_t gain = 0;                    // b2, six bits
    std::array<std::uint32_t, 58> spectral{};  // b3 through b(L+1)
    std::uint32_t sync = 0;                    // b(L+2), one bit
};

// Section 7.1 in the transmit direction, written from the prose of that
// section and from Figure 22. This is the inverse of what the decoder does
// and it is written independently of it: only the scan order is shared, and
// the scan order is what Figure 22 draws.
[[nodiscard]] std::array<std::uint32_t, 8> prioritize(const Quantizers& q) {
    const std::uint32_t harmonics = harmonics_for(q.pitch);
    const std::uint32_t bands = bands_for(harmonics);

    std::array<std::uint16_t, 70> scan{};
    const auto scanned = imbe_priority_scan(harmonics, scan);
    REQUIRE(scanned.has_value());
    const std::size_t cells = *scanned;

    auto scan_source = [&](std::size_t index) {
        const std::uint16_t cell = scan[index];
        return (q.spectral[cell / 16U] >> (cell % 16U)) & 1U;
    };
    auto bit_of = [](std::uint32_t word, int bit) {
        return (word >> bit) & 1U;
    };

    std::array<std::uint32_t, 8> u{};
    // u0 bits 11..6 from the six MSBs of b0, bits 5..3 from the three MSBs of
    // b2, bits 2..0 from the first three cells of the scan.
    u[0] |= ((q.pitch >> 2) & 0x3FU) << 6;
    u[0] |= ((q.gain >> 3) & 0x7U) << 3;
    std::size_t at = 0;
    for (int bit = 2; bit >= 0; --bit) {
        u[0] |= scan_source(at++) << bit;
    }
    for (std::size_t vector = 1; vector <= 3; ++vector) {
        for (int bit = 11; bit >= 0; --bit) {
            u[vector] |= scan_source(at++) << bit;
        }
    }
    // Then all of b1 MSB first, bit 2 and bit 1 of b2, and the rest of the
    // scan, into u4 bit 10 down to u7 bit 4.
    std::vector<std::pair<std::size_t, int>> sink;
    for (std::size_t vector = 4; vector <= 6; ++vector) {
        for (int bit = 10; bit >= 0; --bit) {
            sink.emplace_back(vector, bit);
        }
    }
    for (int bit = 6; bit >= 4; --bit) {
        sink.emplace_back(7, bit);
    }
    for (std::size_t t = 0; t < sink.size(); ++t) {
        std::uint32_t value = 0;
        if (t < bands) {
            value = bit_of(q.voicing, static_cast<int>(bands - 1 - t));
        } else if (t == bands) {
            value = bit_of(q.gain, 2);
        } else if (t == bands + 1) {
            value = bit_of(q.gain, 1);
        } else {
            value = scan_source(at++);
        }
        u[sink[t].first] |= value << sink[t].second;
    }
    REQUIRE(at == cells);
    u[7] |= bit_of(q.gain, 0) << 3;
    u[7] |= bit_of(q.pitch, 1) << 2;
    u[7] |= bit_of(q.pitch, 0) << 1;
    u[7] |= (q.sync & 1U);
    return u;
}

// Section 7.1: the bit vectors end to end, MSB of u0 first.
[[nodiscard]] std::vector<std::uint8_t> bit_vector_frame(
    const std::array<std::uint32_t, 8>& u) {
    constexpr std::array<int, 8> kWidths = {12, 12, 12, 12, 11, 11, 11, 7};
    std::vector<std::uint8_t> out;
    out.reserve(88);
    for (std::size_t vector = 0; vector < 8; ++vector) {
        for (int bit = kWidths[vector] - 1; bit >= 0; --bit) {
            out.push_back(static_cast<std::uint8_t>((u[vector] >> bit) & 1U));
        }
    }
    return out;
}

[[nodiscard]] std::vector<std::uint8_t> channel_frame(
    const std::array<std::uint32_t, 8>& u) {
    std::vector<std::uint8_t> out(144, 0);
    const auto packed = imbe_pack_frame(u, out);
    REQUIRE(packed.has_value());
    return out;
}

// Annex H, used the other way: pull a modulated code vector back out of a
// frame, and push an arbitrary one in.
[[nodiscard]] std::array<std::uint32_t, 8> split(
    std::span<const std::uint8_t> frame) {
    std::array<std::uint32_t, 8> c{};
    const auto map = imbe_frame_map();
    for (std::size_t i = 0; i < map.size(); ++i) {
        c[map[i] / 32U] |= static_cast<std::uint32_t>(frame[i])
                           << (map[i] % 32U);
    }
    return c;
}

[[nodiscard]] std::vector<std::uint8_t> join(
    const std::array<std::uint32_t, 8>& c) {
    std::vector<std::uint8_t> frame(144, 0);
    const auto map = imbe_frame_map();
    for (std::size_t i = 0; i < map.size(); ++i) {
        frame[i] =
            static_cast<std::uint8_t>((c[map[i] / 32U] >> (map[i] % 32U)) & 1U);
    }
    return frame;
}

// A frame with enough energy in it to hear. b2 picks the overall level from
// the Annex E quantizer; everything above it decodes to about half a step,
// which is as near to a flat envelope as the quantizers reach.
[[nodiscard]] Quantizers loud_frame(std::uint32_t pitch, bool voiced,
                                    std::uint32_t gain = 50) {
    Quantizers q;
    q.pitch = pitch;
    q.gain = gain;
    const std::uint32_t harmonics = harmonics_for(pitch);
    q.voicing = voiced ? (1U << bands_for(harmonics)) - 1U : 0U;
    return q;
}

[[nodiscard]] double rms(std::span<const float> samples) {
    double total = 0.0;
    for (const float sample : samples) {
        total += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return std::sqrt(total / static_cast<double>(samples.size()));
}

// Normalized autocorrelation at one lag. A sum of harmonics of omega_0 is
// periodic; filtered noise is not.
[[nodiscard]] double periodicity(std::span<const float> samples, int lag) {
    double numerator = 0.0;
    double left = 0.0;
    double right = 0.0;
    const auto count = static_cast<int>(samples.size()) - lag;
    for (int n = 0; n < count; ++n) {
        const double a = samples[static_cast<std::size_t>(n)];
        const double b = samples[static_cast<std::size_t>(n + lag)];
        numerator += a * b;
        left += a * a;
        right += b * b;
    }
    const double scale = std::sqrt(left * right);
    return scale > 0.0 ? numerator / scale : 0.0;
}

// Run a decoder over the same frame repeatedly and return the last block.
[[nodiscard]] std::vector<float> steady_state(std::span<const std::uint8_t> bits,
                                              int frames, int keep) {
    ImbeDecoder decoder;
    std::vector<float> out;
    for (int f = 0; f < frames; ++f) {
        std::array<float, 160> block{};
        const auto status = decoder.decode(bits, block);
        REQUIRE(status.has_value());
        if (f >= frames - keep) {
            out.insert(out.end(), block.begin(), block.end());
        }
    }
    return out;
}

}  // namespace

// -------------------------------------------------------------------------
// Section 7.3, the error control codes.

// REJECTS: a [23,12] generator matrix with any single bit mistranscribed from
// the one printed in section 7.3. The binary Golay code has minimum distance
// seven, which is what lets it correct three errors; flip one entry of gG and
// the code it generates is some other [23,12] code with a smaller distance.
TEST_CASE("the section 7.3 Golay generator gives a [23,12,7] code",
          "[imbe][fec]") {
    std::vector<std::uint32_t> codewords;
    codewords.reserve(4096);
    for (std::uint32_t message = 0; message < 4096; ++message) {
        // m0 is all zeros, equation (86), so c0 is nu0 untouched.
        const std::array<std::uint32_t, 8> u = {message, 0, 0, 0, 0, 0, 0, 0};
        codewords.push_back(split(channel_frame(u))[0]);
    }
    REQUIRE(codewords[0] == 0);
    // Systematic form: the twelve data bits survive as the top twelve.
    for (std::uint32_t message = 0; message < 4096; ++message) {
        REQUIRE((codewords[message] >> 11) == message);
    }
    int smallest = 23;
    for (std::uint32_t message = 1; message < 4096; ++message) {
        smallest = std::min(smallest, std::popcount(codewords[message]));
    }
    REQUIRE(smallest == 7);
}

// REJECTS: a Golay decoder that corrects fewer than three errors, and one that
// claims to detect four. The [23,12,7] code is perfect: the 2048 syndromes are
// in bijection with the 2048 error patterns of weight three or less, which
// leaves no syndrome over to mean "too corrupt to fix". Every weight four
// pattern therefore lands inside some other codeword's sphere and is corrected
// to the wrong message, silently. The frame is thrown away by the machinery of
// sections 7.4 and 7.7 instead, which is what the next cases measure.
TEST_CASE("the Golay code corrects three errors and cannot detect four",
          "[imbe][fec]") {
    std::vector<std::uint32_t> codewords;
    codewords.reserve(4096);
    for (std::uint32_t message = 0; message < 4096; ++message) {
        const std::array<std::uint32_t, 8> u = {message, 0, 0, 0, 0, 0, 0, 0};
        codewords.push_back(split(channel_frame(u))[0]);
    }

    auto nearest = [&codewords](std::uint32_t received) {
        std::uint32_t best = 0;
        int distance = 24;
        for (const std::uint32_t word : codewords) {
            const int d = std::popcount(received ^ word);
            if (d < distance) {
                distance = d;
                best = word;
            }
        }
        return std::pair{best, distance};
    };

    std::vector<std::uint32_t> correctable = {0};
    for (int a = 0; a < 23; ++a) {
        correctable.push_back(1U << a);
        for (int b = a + 1; b < 23; ++b) {
            correctable.push_back((1U << a) | (1U << b));
            for (int c = b + 1; c < 23; ++c) {
                correctable.push_back((1U << a) | (1U << b) | (1U << c));
            }
        }
    }
    // 1 + 23 + C(23,2) + C(23,3), which is 2048, one per syndrome.
    REQUIRE(correctable.size() == 2048);
    for (const std::uint32_t pattern : correctable) {
        const auto [word, distance] = nearest(pattern);
        REQUIRE(word == 0);
        REQUIRE(distance == std::popcount(pattern));
    }

    std::size_t four = 0;
    for (int a = 0; a < 23; ++a) {
        for (int b = a + 1; b < 23; ++b) {
            for (int c = b + 1; c < 23; ++c) {
                for (int d = c + 1; d < 23; ++d) {
                    const std::uint32_t pattern =
                        (1U << a) | (1U << b) | (1U << c) | (1U << d);
                    const auto [word, distance] = nearest(pattern);
                    REQUIRE(word != 0);
                    REQUIRE(distance <= 3);
                    ++four;
                }
            }
        }
    }
    REQUIRE(four == 8855);  // C(23, 4)
}

// REJECTS: a [15,11] generator with a mistranscribed row, and a Hamming
// decoder that does not correct its one error. The same perfectness argument
// applies: sixteen syndromes, sixteen patterns of weight one or less, nothing
// left over for detection.
TEST_CASE("the section 7.3 Hamming generator gives a [15,11,3] code",
          "[imbe][fec]") {
    // m4 depends on u0 alone, equations (84) and (90), so holding u0 fixed and
    // sweeping u4 isolates the code vector.
    constexpr std::uint32_t kSeed = 0;
    auto code_vector = [](std::uint32_t message) {
        const std::array<std::uint32_t, 8> u = {kSeed, 0, 0, 0, message,
                                                0,     0, 0};
        return split(channel_frame(u))[4];
    };
    const std::uint32_t modulation = code_vector(0);

    std::vector<std::uint32_t> codewords;
    codewords.reserve(2048);
    for (std::uint32_t message = 0; message < 2048; ++message) {
        codewords.push_back(code_vector(message) ^ modulation);
    }
    for (std::uint32_t message = 0; message < 2048; ++message) {
        REQUIRE((codewords[message] >> 4) == message);
    }
    int smallest = 15;
    for (std::uint32_t message = 1; message < 2048; ++message) {
        smallest = std::min(smallest, std::popcount(codewords[message]));
    }
    REQUIRE(smallest == 3);

    auto nearest_distance = [&codewords](std::uint32_t received) {
        int distance = 16;
        for (const std::uint32_t word : codewords) {
            distance = std::min(distance, std::popcount(received ^ word));
        }
        return distance;
    };
    for (int a = 0; a < 15; ++a) {
        for (int b = a + 1; b < 15; ++b) {
            const std::uint32_t pattern = (1U << a) | (1U << b);
            REQUIRE(nearest_distance(pattern) == 1);
        }
    }
}

// REJECTS: a decoder whose Golay stage reports the wrong number of corrected
// bits, which is the number the frame repeat rule of section 7.7 and the
// smoothing thresholds of equation (112) are all computed from. Driven through
// the whole decoder rather than through a unit, because eps_0 reaching the
// report is the part that matters.
TEST_CASE("every correctable error in c0 is corrected and counted",
          "[imbe][fec]") {
    constexpr std::uint32_t kSeed = 20250921;
    std::mt19937_64 rng(kSeed);
    std::println("test_imbe correctable-error sweep seed {}", kSeed);

    std::vector<std::uint32_t> messages = {0, 4095};
    for (int i = 0; i < 4; ++i) {
        messages.push_back(static_cast<std::uint32_t>(rng() & 0xFFFU));
    }

    for (const std::uint32_t message : messages) {
        const std::array<std::uint32_t, 8> u = {message, 0, 0, 0, 0, 0, 0, 0};
        const auto clean = split(channel_frame(u));
        const std::uint32_t expected_pitch = (message >> 6) << 2;

        for (int a = 0; a < 23; ++a) {
            for (int b = a + 1; b < 23; ++b) {
                const std::uint32_t pattern = (1U << a) | (1U << b);
                auto damaged = clean;
                damaged[0] ^= pattern;
                const auto frame = join(damaged);
                ImbeDecoder decoder;
                std::array<float, 160> out{};
                REQUIRE(decoder.decode(frame, out).has_value());
                REQUIRE(decoder.last_frame().corrected[0] == 2);
                REQUIRE(decoder.last_frame().pitch_index == expected_pitch);
            }
        }
    }
}

// -------------------------------------------------------------------------
// Section 7.5 and Annex H, the interleave.

// REJECTS: an Annex H transcription that drops, duplicates or moves a bit.
// Section 7.5 states the separation property outright: "the minimum separation
// between any two bits of the same error correction code is 3 symbols". A
// table that is a bijection but scrambled would still round trip through
// imbe_pack_frame and decode(), and this is what catches it.
TEST_CASE("Annex H covers every code vector bit once, three symbols apart",
          "[imbe][interleave]") {
    const auto map = imbe_frame_map();
    REQUIRE(map.size() == 144);

    std::vector<std::pair<int, int>> seen;
    for (const std::uint16_t slot : map) {
        seen.emplace_back(slot / 32, slot % 32);
    }
    std::vector<std::pair<int, int>> sorted = seen;
    std::ranges::sort(sorted);
    REQUIRE(std::ranges::adjacent_find(sorted) == sorted.end());

    std::vector<std::pair<int, int>> expected;
    for (int vector = 0; vector < 4; ++vector) {
        for (int bit = 0; bit < 23; ++bit) {
            expected.emplace_back(vector, bit);
        }
    }
    for (int vector = 4; vector < 7; ++vector) {
        for (int bit = 0; bit < 15; ++bit) {
            expected.emplace_back(vector, bit);
        }
    }
    for (int bit = 0; bit < 7; ++bit) {
        expected.emplace_back(7, bit);
    }
    std::ranges::sort(expected);
    REQUIRE(sorted == expected);

    // Two bits of one code vector never land closer than three symbols, where
    // a symbol is a dibit and frame position i sits in symbol i / 2.
    int closest = 72;
    for (std::size_t i = 0; i < map.size(); ++i) {
        for (std::size_t j = i + 1; j < map.size(); ++j) {
            if (map[i] / 32 != map[j] / 32) {
                continue;
            }
            const int separation =
                std::abs(static_cast<int>(i / 2) - static_cast<int>(j / 2));
            closest = std::min(closest, separation);
        }
    }
    REQUIRE(closest == 3);
}

// -------------------------------------------------------------------------
// Section 7.1 and Figure 22, the priority scan.

// REJECTS: a scan that walks columns instead of rows, or that starts at the
// LSB, or that includes a cell an allocation does not have. Figure 22 draws
// the whole grid for L = 16 and names the destination of every shaded cell,
// and the two ends of that figure are asserted exactly: the first cell is bit
// 5 of b3 going to bit 2 of u0, and the last is bit 0 of b17 going to bit 4
// of u7.
TEST_CASE("the priority scan reproduces Figure 22 for L = 16",
          "[imbe][priority]") {
    std::array<std::uint16_t, 70> scan{};
    const auto scanned = imbe_priority_scan(16, scan);
    REQUIRE(scanned.has_value());

    // The rows of Figure 22, top to bottom. Row 5 is the top row that has any
    // shaded cell; rows 10 through 6 are empty at this L.
    const std::vector<std::pair<int, std::vector<int>>> figure = {
        {5, {3, 4, 5, 8, 9}},
        {4, {3, 4, 5, 6, 7, 8, 9, 10}},
        {3, {3, 4, 5, 6, 7, 8, 9, 10, 11, 12}},
        {2, {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}},
        {1, {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17}},
        {0, {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17}},
    };
    std::vector<std::uint16_t> expected;
    for (const auto& [bit, columns] : figure) {
        for (const int column : columns) {
            expected.push_back(static_cast<std::uint16_t>(column * 16 + bit));
        }
    }
    REQUIRE(*scanned == expected.size());
    const std::vector<std::uint16_t> produced(
        scan.begin(), scan.begin() + static_cast<std::ptrdiff_t>(*scanned));
    REQUIRE(produced == expected);

    // Figure 22's own labels at the two ends of the scan.
    REQUIRE(scan[0] == 3 * 16 + 5);   // b3 bit 5, labelled u0,2
    REQUIRE(scan[66] == 17 * 16 + 0);  // b17 bit 0, labelled u7,4
}

// REJECTS: a mistranscribed entry anywhere in Annex F or Annex G, and a wrong
// Annex J row. Table 1 fixes the budget at 88 bits: eight for b0, K for b1,
// six for b2 and one for synchronization, leaving 73 - K for b3 through
// b(L+1). Annex J's six block lengths have to sum to L for the same bits to
// describe the same envelope. Both hold for every L the pitch quantizer can
// produce, and imbe_priority_scan refuses rather than returning a short scan
// if either fails.
TEST_CASE("the annex bit allocation adds up for every L", "[imbe][tables]") {
    for (std::uint32_t harmonics = 9; harmonics <= 56; ++harmonics) {
        std::array<std::uint16_t, 70> scan{};
        const auto scanned = imbe_priority_scan(harmonics, scan);
        REQUIRE(scanned.has_value());
        REQUIRE(*scanned == 73 - bands_for(harmonics));
    }
    std::array<std::uint16_t, 70> spare{};
    REQUIRE_FALSE(imbe_priority_scan(8, spare).has_value());
    REQUIRE_FALSE(imbe_priority_scan(57, spare).has_value());
}

// -------------------------------------------------------------------------
// Section 6.1, the pitch quantizer.

// REJECTS: an implementation that reads equation (47) as a single floor rather
// than the nested pair it prints, and one that takes b0's two least
// significant bits from anywhere but u7. Also a bit allocation that only
// happens to work at the L values a voice sample visits: this walks all 208.
TEST_CASE("equations (46) to (48) hold for every valid pitch index",
          "[imbe][pitch]") {
    std::uint32_t seen_low = 57;
    std::uint32_t seen_high = 8;
    for (std::uint32_t pitch = 0; pitch <= 207; ++pitch) {
        const Quantizers q = loud_frame(pitch, true);
        const auto bits = bit_vector_frame(prioritize(q));
        ImbeDecoder decoder;
        std::array<float, 160> out{};
        REQUIRE(decoder.decode(bits, out).has_value());

        const auto& report = decoder.last_frame();
        REQUIRE(report.state == ImbeFrameState::Decoded);
        REQUIRE(report.pitch_index == pitch);
        REQUIRE(report.harmonics == harmonics_for(pitch));
        REQUIRE(report.voiced_bands == bands_for(report.harmonics));
        const double hertz = fundamental_for(pitch) * 8000.0 / (2.0 * kPi);
        REQUIRE(std::abs(report.fundamental_hz - hertz) < 1e-6);
        seen_low = std::min(seen_low, report.harmonics);
        seen_high = std::max(seen_high, report.harmonics);
    }
    // Section 6.1 restricts omega_0, and Annex J tabulates exactly this range.
    REQUIRE(seen_low == 9);
    REQUIRE(seen_high == 56);
}

// -------------------------------------------------------------------------
// The two fixed windows.

// REJECTS: a synthesis window that is not the trapezoid Annex I tabulates, in
// particular one that ramps over the wrong span. The four values quoted are
// read off Annex I; the ramp is 0.02 per sample and reaches zero at 105.
TEST_CASE("Annex I is the trapezoid the annex prints", "[imbe][windows]") {
    REQUIRE(imbe_synthesis_window(0) == 1.0);
    REQUIRE(imbe_synthesis_window(55) == 1.0);
    REQUIRE(std::abs(imbe_synthesis_window(-104) - 0.020000) < 5e-7);
    REQUIRE(std::abs(imbe_synthesis_window(-80) - 0.500000) < 5e-7);
    REQUIRE(std::abs(imbe_synthesis_window(56) - 0.980000) < 5e-7);
    REQUIRE(std::abs(imbe_synthesis_window(80) - 0.500000) < 5e-7);
    REQUIRE(imbe_synthesis_window(105) == 0.0);
    REQUIRE(imbe_synthesis_window(106) == 0.0);
    for (int n = -105; n <= 105; ++n) {
        REQUIRE(imbe_synthesis_window(n) == imbe_synthesis_window(-n));
    }
    // Equation (126) divides by this, so it must never be zero over the frame.
    // And the window sums to exactly one against itself shifted by a frame,
    // which is what makes the two terms of equation (133) add up to an
    // unwindowed sinusoid when neither the amplitude nor the fundamental has
    // moved. A ramp over the wrong span breaks that and puts a 50 Hz warble on
    // every sustained vowel, which is audible and hard to trace.
    for (int n = 0; n < 160; ++n) {
        const double now = imbe_synthesis_window(n);
        const double before = imbe_synthesis_window(n - 160);
        REQUIRE(now * now + before * before > 0.0);
        REQUIRE(std::abs(now + before - 1.0) < 1e-12);
    }
}

// REJECTS: a shifted or truncated transcription of Annex C. The annex prints
// n = -110 to 110; it is even, it peaks at 1.0, and it falls away
// monotonically. The three values quoted are read off the annex.
TEST_CASE("Annex C is the even window the annex prints", "[imbe][windows]") {
    REQUIRE(imbe_pitch_refinement_window(0) == 1.0);
    REQUIRE(std::abs(imbe_pitch_refinement_window(16) - 0.943474) < 5e-7);
    REQUIRE(std::abs(imbe_pitch_refinement_window(-55) - 0.482955) < 5e-7);
    REQUIRE(std::abs(imbe_pitch_refinement_window(110) - 0.014873) < 5e-7);
    REQUIRE(imbe_pitch_refinement_window(111) == 0.0);
    for (int n = 1; n <= 110; ++n) {
        REQUIRE(imbe_pitch_refinement_window(n) ==
                imbe_pitch_refinement_window(-n));
        REQUIRE(imbe_pitch_refinement_window(n) <
                imbe_pitch_refinement_window(n - 1));
    }
    // Equation (121) is a function of these two windows and nothing else, so
    // this is a single number that moves if either transcription moves. It is
    // a regression pin, not an independent check: the standard prints no value
    // for gamma_w.
    const double gamma = ImbeDecoder::unvoiced_scaling_coefficient();
    REQUIRE(gamma > 0.0);
    std::println("test_imbe gamma_w = {:.9f}", gamma);
    REQUIRE(std::abs(gamma - 146.643270844) < 1e-6);
}

// -------------------------------------------------------------------------
// Determinism.

// REJECTS: any hidden state the standard does not have. A vocoder that seeds
// its noise from a clock, or that leaves a static buffer between instances, or
// that depends on what the caller did with the output span, fails here. The
// second half also rejects an implementation that accumulates across a call
// boundary: the same frames written into one long buffer and into separate
// 160 sample buffers must give the same samples.
TEST_CASE("the same frames give the same samples, bit for bit",
          "[imbe][determinism]") {
    const Quantizers q = loud_frame(64, true);
    const auto bits = channel_frame(prioritize(q));

    std::vector<float> first;
    std::vector<float> second;
    for (int run = 0; run < 2; ++run) {
        ImbeDecoder decoder;
        std::vector<float>& into = run == 0 ? first : second;
        for (int f = 0; f < 6; ++f) {
            std::array<float, 160> block{};
            REQUIRE(decoder.decode(bits, block).has_value());
            into.insert(into.end(), block.begin(), block.end());
        }
    }
    REQUIRE(first == second);

    ImbeDecoder blocked;
    std::vector<float> wide(6 * 160 + 37, 0.0F);
    for (int f = 0; f < 6; ++f) {
        // A span that is longer than one frame and starts at an offset the
        // first run never used.
        const auto slice =
            std::span<float>(wide).subspan(static_cast<std::size_t>(f) * 160,
                                           wide.size() -
                                               static_cast<std::size_t>(f) *
                                                   160);
        REQUIRE(blocked.decode(bits, slice).has_value());
    }
    for (std::size_t n = 0; n < first.size(); ++n) {
        REQUIRE(wide[n] == first[n]);
    }

    // reset() puts the decoder back where the constructor left it, Annex A.
    ImbeDecoder reused;
    std::vector<float> third;
    for (int f = 0; f < 3; ++f) {
        std::array<float, 160> block{};
        REQUIRE(reused.decode(bits, block).has_value());
    }
    reused.reset();
    for (int f = 0; f < 6; ++f) {
        std::array<float, 160> block{};
        REQUIRE(reused.decode(bits, block).has_value());
        third.insert(third.end(), block.begin(), block.end());
    }
    REQUIRE(third == first);
}

// REJECTS: a deinterleave or demodulation that is not the exact inverse of
// sections 7.3 to 7.5. The 88 bit path hands the decoder the bit vectors
// directly; the 144 bit path makes it recover them through the Golay codes,
// the pseudo-random modulation and Annex H. With no channel errors the two
// must produce identical samples.
TEST_CASE("the channel frame and the bit vectors decode alike",
          "[imbe][fec]") {
    for (const std::uint32_t pitch : {0U, 37U, 104U, 207U}) {
        for (const bool voiced : {true, false}) {
            const Quantizers q = loud_frame(pitch, voiced);
            const auto u = prioritize(q);
            const auto wide = channel_frame(u);
            const auto narrow = bit_vector_frame(u);

            ImbeDecoder a;
            ImbeDecoder b;
            for (int f = 0; f < 3; ++f) {
                std::array<float, 160> left{};
                std::array<float, 160> right{};
                REQUIRE(a.decode(wide, left).has_value());
                REQUIRE(b.decode(narrow, right).has_value());
                REQUIRE(left == right);
            }
            REQUIRE(a.last_frame().errors_total == 0);
            REQUIRE(a.last_frame().state == ImbeFrameState::Decoded);
        }
    }
}

// -------------------------------------------------------------------------
// What the three kinds of frame sound like.

// REJECTS: a decoder that ignores the V/UV bits of equation (51), which is the
// single most likely way to get a plausible looking waveform out of a broken
// vocoder. An all voiced frame is a sum of harmonics of omega_0 and repeats
// with the pitch period; an all unvoiced frame is shaped noise and does not.
// The period is 2 pi / omega_0 = (b0 + 39.5) / 2 samples, never an integer,
// but four periods always are: 2 b0 + 79.
TEST_CASE("a voiced frame is periodic and an unvoiced frame is not",
          "[imbe][synthesis]") {
    constexpr std::uint32_t kPitch = 0;
    const int lag = static_cast<int>(2 * kPitch + 79);

    const auto voiced = steady_state(
        bit_vector_frame(prioritize(loud_frame(kPitch, true))), 8, 2);
    const auto unvoiced = steady_state(
        bit_vector_frame(prioritize(loud_frame(kPitch, false))), 8, 2);

    const double voiced_periodicity = periodicity(voiced, lag);
    const double unvoiced_periodicity = periodicity(unvoiced, lag);
    std::println(
        "test_imbe periodicity at lag {}: voiced {:.4f}, unvoiced {:.4f}", lag,
        voiced_periodicity, unvoiced_periodicity);

    // Without this the unvoiced case would pass against a decoder whose
    // unvoiced path emits nothing at all: silence is not periodic either. The
    // two frames carry the same gain, so they have to come out at comparable
    // levels, and equations (109) and (110) are what make that true.
    const double voiced_level = rms(voiced);
    const double unvoiced_level = rms(unvoiced);
    std::println("test_imbe level: voiced {:.3e}, unvoiced {:.3e}",
                 voiced_level, unvoiced_level);
    REQUIRE(unvoiced_level > 0.1 * voiced_level);
    REQUIRE(unvoiced_level < 10.0 * voiced_level);
    REQUIRE(voiced_periodicity > 0.85);
    REQUIRE(unvoiced_periodicity < 0.5);
    REQUIRE(voiced_periodicity > unvoiced_periodicity + 0.4);

    // A constant output would correlate perfectly at every lag, so periodicity
    // alone does not separate a periodic waveform from a stuck DC level. Two
    // more measurements do. At roughly half a period the correlation falls
    // away, and the mean of a waveform that swings about zero is far below its
    // RMS, where for a constant the two are equal.
    const double half = periodicity(voiced, lag / 8);
    double mean = 0.0;
    for (const float sample : voiced) {
        mean += static_cast<double>(sample);
    }
    mean /= static_cast<double>(voiced.size());
    const double level = rms(voiced);
    std::println(
        "test_imbe voiced at lag {}: {:.4f}, mean {:.3e} against rms {:.3e}",
        lag / 8, half, mean, level);
    REQUIRE(half < 0.6);
    REQUIRE(level > 0.0);
    REQUIRE(std::abs(mean) < 0.2 * level);
}

// REJECTS: a decoder that loses the gain vector of section 6.4.1, or that
// normalizes the output level away. Annex E spans 11.5 in log2, from -2.842205
// to 8.695827, so the quietest frame the quantizer can carry is three orders
// of magnitude below the loudest; an implementation that dropped b2 would
// produce the same level for both. b2 = 0 is also the nearest thing the format
// has to a silence frame, and it has to come out inaudible rather than
// missing: the assertion is that it is quiet, not that it is zero.
TEST_CASE("the gain quantizer sets the level, silence included",
          "[imbe][synthesis]") {
    const auto quiet = steady_state(
        bit_vector_frame(prioritize(loud_frame(40, true, 0))), 6, 1);
    const auto loud = steady_state(
        bit_vector_frame(prioritize(loud_frame(40, true, 63))), 6, 1);
    const double quiet_level = rms(quiet);
    const double loud_level = rms(loud);
    std::println("test_imbe level: b2 = 0 gives {:.3e}, b2 = 63 gives {:.3e}",
                 quiet_level, loud_level);
    REQUIRE(quiet_level > 0.0);
    REQUIRE(quiet_level < 1.0 / 512.0);  // under 64 counts of a 16 bit scale
    REQUIRE(loud_level > 1.0 / 32.0);
    REQUIRE(loud_level > quiet_level * 100.0);
}

// REJECTS: a decoder that corrects errors and does not count them. Equation
// (95) sums the seven counts and equation (96) is a first order filter over
// that sum with coefficients .95 and .000365, and both feed the frame repeat
// rule of section 7.7 and the adaptive smoothing thresholds of section 9. The
// arithmetic is exact and is asserted exactly.
TEST_CASE("equations (95) and (96) count what the codes corrected",
          "[imbe][fec]") {
    const auto clean = channel_frame(prioritize(loud_frame(48, true)));

    ImbeDecoder decoder;
    std::array<float, 160> block{};
    REQUIRE(decoder.decode(clean, block).has_value());
    REQUIRE(decoder.last_frame().errors_total == 0);
    REQUIRE(decoder.last_frame().error_rate == 0.0);

    // One bit of c1, which is a [23,12] Golay code vector and is demodulated
    // correctly because c0 is untouched.
    const auto map = imbe_frame_map();
    std::size_t at = map.size();
    for (std::size_t i = 0; i < map.size(); ++i) {
        if (map[i] / 32 == 1 && map[i] % 32 == 5) {
            at = i;
        }
    }
    REQUIRE(at < map.size());
    auto damaged = clean;
    damaged[at] ^= 1U;

    REQUIRE(decoder.decode(damaged, block).has_value());
    const auto& report = decoder.last_frame();
    REQUIRE(report.corrected[0] == 0);
    REQUIRE(report.corrected[1] == 1);
    REQUIRE(report.errors_total == 1);
    REQUIRE(std::abs(report.error_rate - 0.000365) < 1e-15);
    REQUIRE(report.state == ImbeFrameState::Decoded);

    REQUIRE(decoder.decode(clean, block).has_value());
    REQUIRE(decoder.last_frame().errors_total == 0);
    REQUIRE(std::abs(decoder.last_frame().error_rate - 0.95 * 0.000365) <
            1e-15);
}

// -------------------------------------------------------------------------
// Sections 7.7 and 7.8, and Annex K Flow Chart 9.

// REJECTS: a decoder that emits silence, or noise, on a frame it cannot use.
// Section 7.7 says to repeat the previous frame's model parameters, and
// section 7.8 says to mute only in the cases it names. The three reserved
// pitch ranges of Flow Chart 9 do different things and this asserts each one,
// including that a repeated frame still carries the previous frame's L and
// fundamental and still makes a comparable amount of noise.
TEST_CASE("a reserved pitch index repeats or mutes as Flow Chart 9 says",
          "[imbe][repeat]") {
    const Quantizers good = loud_frame(48, true);
    const auto good_bits = bit_vector_frame(prioritize(good));

    auto reserved = [](std::uint32_t pitch) {
        // A pitch index above 207 has no L, so the rest of the frame cannot be
        // laid out; only the eight bits of b0 and the sync bit are meaningful.
        std::array<std::uint32_t, 8> u{};
        u[0] |= ((pitch >> 2) & 0x3FU) << 6;
        u[7] |= ((pitch >> 1) & 1U) << 2;
        u[7] |= (pitch & 1U) << 1;
        return bit_vector_frame(u);
    };

    struct Case {
        std::uint32_t pitch;
        ImbeFrameState state;
        ImbeFrameCause cause;
    };
    const std::array<Case, 4> cases = {{
        {208, ImbeFrameState::Repeated, ImbeFrameCause::ReservedPitchIndex},
        {215, ImbeFrameState::Repeated, ImbeFrameCause::ReservedPitchIndex},
        {216, ImbeFrameState::Muted, ImbeFrameCause::MuteRequestPitchIndex},
        {220, ImbeFrameState::Repeated, ImbeFrameCause::ReservedPitchIndex},
    }};

    for (const Case& probe : cases) {
        ImbeDecoder decoder;
        std::array<float, 160> block{};
        for (int f = 0; f < 4; ++f) {
            REQUIRE(decoder.decode(good_bits, block).has_value());
        }
        const double speech = rms(block);
        const std::uint32_t harmonics = decoder.last_frame().harmonics;
        const double hertz = decoder.last_frame().fundamental_hz;

        REQUIRE(decoder.decode(reserved(probe.pitch), block).has_value());
        const auto& report = decoder.last_frame();
        REQUIRE(report.state == probe.state);
        REQUIRE(report.cause == probe.cause);
        REQUIRE(report.pitch_index == probe.pitch);
        // Equations (99) to (104): the parameters are the previous frame's.
        REQUIRE(report.harmonics == harmonics);
        REQUIRE(std::abs(report.fundamental_hz - hertz) < 1e-9);

        if (probe.state == ImbeFrameState::Repeated) {
            // Still speech, not a gap. A repeat that fell silent would be
            // indistinguishable from a dead channel.
            REQUIRE(rms(block) > speech * 0.2);
        } else {
            // Section 7.8: comfort noise over [-5, 5] on the 16 bit scale.
            for (const float sample : block) {
                REQUIRE(std::abs(sample) <= 5.0F / ImbeDecoder::kFullScale);
            }
            REQUIRE(rms(block) > 0.0);
        }
        // Either way the report says so rather than leaving the caller to
        // guess from the samples.
        REQUIRE(decoder.describe_last_frame().find(
                    probe.state == ImbeFrameState::Muted ? "MUTED"
                                                         : "REPEATED") !=
                std::string::npos);
    }
}

// REJECTS: a decoder that repeats the same frame forever. Flow Chart 9 mutes
// on the fourth consecutive invalid frame, which is what stops a lost signal
// turning into a stuck vowel.
TEST_CASE("the fourth consecutive invalid frame mutes", "[imbe][repeat]") {
    const auto good = bit_vector_frame(prioritize(loud_frame(48, true)));
    std::array<std::uint32_t, 8> bad{};
    bad[0] = 0x3FU << 6;  // b0 = 252, reserved
    bad[7] = 0;
    const auto bad_bits = bit_vector_frame(bad);

    ImbeDecoder decoder;
    std::array<float, 160> block{};
    REQUIRE(decoder.decode(good, block).has_value());
    REQUIRE(decoder.last_frame().state == ImbeFrameState::Decoded);

    for (int f = 1; f <= 3; ++f) {
        REQUIRE(decoder.decode(bad_bits, block).has_value());
        REQUIRE(decoder.last_frame().state == ImbeFrameState::Repeated);
        REQUIRE(decoder.last_frame().consecutive_invalid ==
                static_cast<std::uint32_t>(f));
    }
    REQUIRE(decoder.decode(bad_bits, block).has_value());
    REQUIRE(decoder.last_frame().state == ImbeFrameState::Muted);
    REQUIRE(decoder.last_frame().cause ==
            ImbeFrameCause::FourthConsecutiveInvalid);
    for (const float sample : block) {
        REQUIRE(std::abs(sample) <= 5.0F / ImbeDecoder::kFullScale);
    }

    // A good frame clears the count and speech comes back.
    REQUIRE(decoder.decode(good, block).has_value());
    REQUIRE(decoder.last_frame().state == ImbeFrameState::Decoded);
    REQUIRE(decoder.last_frame().consecutive_invalid == 0);
    REQUIRE(rms(block) > 0.0);
}

// A MEASUREMENT, not an assertion of a known answer. Section 7.4 claims that
// errors beyond the Golay code's three in c0 make it mis-decode u0, which
// randomizes the modulation vectors and drives the other six codes to correct
// near their limit, and that counting those corrections detects the frame.
// Nothing in the standard proves it and the Golay code cannot refuse on its
// own, so the rate is measured here and printed.
//
// REJECTS: a decoder that accepts a corrupted frame and synthesizes from the
// parameters it happens to recover. That is the failure mode this whole
// mechanism exists to prevent, and the one an implementation gets for free by
// leaving sections 7.4, 7.6 and 7.7 out.
TEST_CASE("a frame with an uncorrectable c0 is refused, measured",
          "[imbe][repeat]") {
    constexpr std::uint32_t kSeed = 0x5150C0DEU;
    std::mt19937_64 rng(kSeed);
    std::println("test_imbe uncorrectable-c0 sweep seed {}", kSeed);

    const Quantizers good = loud_frame(48, true);
    const auto clean = split(channel_frame(prioritize(good)));
    const auto good_bits = channel_frame(prioritize(good));

    constexpr int kTrials = 400;
    int detected = 0;
    std::uniform_int_distribution<int> where(0, 22);
    for (int trial = 0; trial < kTrials; ++trial) {
        std::uint32_t pattern = 0;
        while (std::popcount(pattern) < 5) {
            pattern |= 1U << where(rng);
        }
        auto damaged = clean;
        damaged[0] ^= pattern;

        ImbeDecoder decoder;
        std::array<float, 160> block{};
        // Two clean frames first, so the decoder has parameters to repeat and
        // an error rate at the floor.
        REQUIRE(decoder.decode(good_bits, block).has_value());
        REQUIRE(decoder.decode(good_bits, block).has_value());
        REQUIRE(decoder.last_frame().state == ImbeFrameState::Decoded);

        REQUIRE(decoder.decode(join(damaged), block).has_value());
        if (decoder.last_frame().state != ImbeFrameState::Decoded) {
            ++detected;
        }
    }
    const double rate = static_cast<double>(detected) /
                        static_cast<double>(kTrials);
    std::println(
        "test_imbe five-bit c0 corruption: {} of {} frames refused ({:.1f}%)",
        detected, kTrials, rate * 100.0);
    // Measured at 1.000 on this seed. The floor is set well below that rather
    // than at it, because the detector is statistical: equation (98) can miss
    // when the randomized code vectors happen to need few corrections.
    REQUIRE(rate > 0.90);
}

// -------------------------------------------------------------------------
// Refusals.

// REJECTS: a decoder that returns a bare failure, or that half decodes a
// malformed call and leaves the caller to work out what went wrong. Every
// refusal names the number it got and the number it wanted.
TEST_CASE("a refusal names the number, the cause and the fix",
          "[imbe][errors]") {
    ImbeDecoder decoder;
    std::array<float, 160> out{};

    const std::vector<std::uint8_t> short_frame(143, 0);
    const auto wrong_size = decoder.decode(short_frame, out);
    REQUIRE_FALSE(wrong_size.has_value());
    REQUIRE(wrong_size.error().message.find("143") != std::string::npos);
    REQUIRE(wrong_size.error().message.find("144") != std::string::npos);
    REQUIRE(wrong_size.error().message.find("88") != std::string::npos);

    std::vector<std::uint8_t> packed(144, 0);
    packed[37] = 2;
    const auto not_a_bit = decoder.decode(packed, out);
    REQUIRE_FALSE(not_a_bit.has_value());
    REQUIRE(not_a_bit.error().message.find("37") != std::string::npos);
    REQUIRE(not_a_bit.error().message.find("unpack") != std::string::npos);

    std::array<float, 159> cramped{};
    const std::vector<std::uint8_t> frame(144, 0);
    const auto too_small = decoder.decode(frame, cramped);
    REQUIRE_FALSE(too_small.has_value());
    REQUIRE(too_small.error().message.find("159") != std::string::npos);
    REQUIRE(too_small.error().message.find("160") != std::string::npos);

    // A refused call must not move the decoder on. The frame that follows has
    // to decode exactly as it would have without the three bad calls.
    const auto bits = bit_vector_frame(prioritize(loud_frame(48, true)));
    ImbeDecoder clean;
    std::array<float, 160> expected{};
    REQUIRE(clean.decode(bits, expected).has_value());
    std::array<float, 160> after{};
    REQUIRE(decoder.decode(bits, after).has_value());
    REQUIRE(after == expected);
}
