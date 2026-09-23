// IMBE frames built from quantizer values, for the tests.
//
// TIA-102.BABA section 7.1 in the transmit direction plus the pitch relations
// of equations (46) to (48), shared by tests/decode/test_imbe.cpp, which
// checks the vocoder against them, and tests/decode/test_p25p1.cpp, which
// carries the frames they build across the air interface. One transcription
// of the section rather than two, so the two suites cannot disagree about
// what a frame is.
//
// None of this is an IMBE encoder. Nothing here estimates a pitch or
// quantizes a spectral amplitude; a test picks the quantizer values and this
// arranges their bits the way section 7.1 says.

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>
#include <vector>

#include "core/decode/imbe.h"

namespace revenant::decode::imbe_test {

// Equations (46) through (48), written out here rather than read off the
// decoder. A test that asks the code under test for its own expected value
// proves only that the code is self-consistent.
[[nodiscard]] inline double fundamental_for(std::uint32_t pitch) {
    return 4.0 * std::numbers::pi / (static_cast<double>(pitch) + 39.5);
}

[[nodiscard]] inline std::uint32_t harmonics_for(std::uint32_t pitch) {
    const double omega = fundamental_for(pitch);
    return static_cast<std::uint32_t>(
        std::floor(0.9254 * std::floor(std::numbers::pi / omega + 0.25)));
}

[[nodiscard]] inline std::uint32_t bands_for(std::uint32_t harmonics) {
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
[[nodiscard]] inline std::array<std::uint32_t, 8> prioritize(const Quantizers& q) {
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
[[nodiscard]] inline std::vector<std::uint8_t> bit_vector_frame(
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

[[nodiscard]] inline std::vector<std::uint8_t> channel_frame(
    const std::array<std::uint32_t, 8>& u) {
    std::vector<std::uint8_t> out(144, 0);
    const auto packed = imbe_pack_frame(u, out);
    REQUIRE(packed.has_value());
    return out;
}

// A frame with enough energy in it to hear. b2 picks the overall level from
// the Annex E quantizer; everything above it decodes to about half a step,
// which is as near to a flat envelope as the quantizers reach.
[[nodiscard]] inline Quantizers loud_frame(std::uint32_t pitch, bool voiced,
                                           std::uint32_t gain = 50) {
    Quantizers q;
    q.pitch = pitch;
    q.gain = gain;
    const std::uint32_t harmonics = harmonics_for(pitch);
    q.voicing = voiced ? (1U << bands_for(harmonics)) - 1U : 0U;
    return q;
}

}  // namespace revenant::decode::imbe_test
