// P25 Phase 1 trunking control channel: the TSDU, its trellis coded blocks,
// and the TSBK messages, against the documents and against the transmitter.
//
// The same two groups as tests/decode/test_p25p1.cpp. The first holds the
// codes to what TIA-102.BAAA-A prints: Table 7-4's rows against the rule
// core/decode/dv_codes.cpp generates the interleave from, and the clause 6.2
// CRC against a second reading of the clause done here by long division. The
// second is the round trip through core/dsp/synth/dv_mod.cpp, clean and with
// errors the trellis code has to correct, and each parsed message with
// values built here from the AABC-B figures and required back exactly.
//
// The message octets below are written out field by field from the figures
// rather than produced by an encoder in core, so a parse that misreads a
// figure is checked against a second reading of it, the way
// tests/decode/test_p25p1_voice.cpp builds Link Control from Figure 5-6.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "core/decode/dv_codes.h"
#include "core/decode/p25_tsbk.h"
#include "core/decode/p25p1.h"
#include "core/dsp/synth/dv_mod.h"

using namespace revenant;
using decode::P25Duid;

namespace {

constexpr dsp::SampleRate kRate = 48'000;

using Fields = std::array<std::uint8_t, 10>;

// AABC-B Figure 4.2.1-1, GRP_V_CH_GRANT.
Fields group_voice_grant(std::uint8_t options, std::uint16_t channel, std::uint16_t group,
                         std::uint32_t source) {
    return Fields{0x00,
                  0x00,
                  options,
                  static_cast<std::uint8_t>(channel >> 8U),
                  static_cast<std::uint8_t>(channel),
                  static_cast<std::uint8_t>(group >> 8U),
                  static_cast<std::uint8_t>(group),
                  static_cast<std::uint8_t>(source >> 16U),
                  static_cast<std::uint8_t>(source >> 8U),
                  static_cast<std::uint8_t>(source)};
}

// AABC-B Figure 6.2.9-1, IDEN_UP, field by field into a 64-bit string.
Fields identifier_update(std::uint8_t iden, std::uint16_t bw, std::uint16_t offset,
                         std::uint16_t spacing, std::uint32_t base) {
    std::uint64_t bits = iden & 0xFU;
    bits = (bits << 9U) | (bw & 0x1FFU);
    bits = (bits << 9U) | (offset & 0x1FFU);
    bits = (bits << 10U) | (spacing & 0x3FFU);
    bits = (bits << 32U) | base;
    Fields f{0x3D, 0x00};
    for (std::size_t i = 0; i < 8; ++i) {
        f[2 + i] = static_cast<std::uint8_t>(bits >> (56U - 8U * i));
    }
    return f;
}

// AABC-B Figure 6.2.29-1, IDEN_UP_VU.
Fields identifier_update_vu(std::uint8_t iden, std::uint8_t bw_vu, std::uint16_t offset_vu,
                            std::uint16_t spacing, std::uint32_t base) {
    std::uint64_t bits = iden & 0xFU;
    bits = (bits << 4U) | (bw_vu & 0xFU);
    bits = (bits << 14U) | (offset_vu & 0x3FFFU);
    bits = (bits << 10U) | (spacing & 0x3FFU);
    bits = (bits << 32U) | base;
    Fields f{0x34, 0x00};
    for (std::size_t i = 0; i < 8; ++i) {
        f[2 + i] = static_cast<std::uint8_t>(bits >> (56U - 8U * i));
    }
    return f;
}

// AABC-B Figure 6.2.15-1, RFSS_STS_BCST.
Fields rfss_status(std::uint8_t lra, bool active, std::uint16_t system, std::uint8_t rfss,
                   std::uint8_t site, std::uint16_t channel, std::uint8_t service_class) {
    return Fields{0x3A,
                  0x00,
                  lra,
                  static_cast<std::uint8_t>((active ? 0x10U : 0x00U) | ((system >> 8U) & 0x0FU)),
                  static_cast<std::uint8_t>(system),
                  rfss,
                  site,
                  static_cast<std::uint8_t>(channel >> 8U),
                  static_cast<std::uint8_t>(channel),
                  service_class};
}

// AABC-B Figure 6.2.11-1, NET_STS_BCST.
Fields network_status(std::uint8_t lra, std::uint32_t wacn, std::uint16_t system,
                      std::uint16_t channel, std::uint8_t service_class) {
    return Fields{0x3B,
                  0x00,
                  lra,
                  static_cast<std::uint8_t>(wacn >> 12U),
                  static_cast<std::uint8_t>(wacn >> 4U),
                  static_cast<std::uint8_t>(((wacn & 0xFU) << 4U) | ((system >> 8U) & 0xFU)),
                  static_cast<std::uint8_t>(system),
                  static_cast<std::uint8_t>(channel >> 8U),
                  static_cast<std::uint8_t>(channel),
                  service_class};
}

decode::P25Tsbk tsbk_of(const Fields& fields) {
    return decode::p25_tsbk_from_octets(decode::p25_tsbk_with_crc(fields));
}

std::vector<decode::P25Frame> decode_dibits(const std::vector<std::uint8_t>& stream) {
    siggen::P25ModConfig mod;
    mod.rate = kRate;
    auto samples = siggen::p25_render_dibits(mod, stream);
    INFO((samples.has_value() ? std::string{} : samples.error().message));
    REQUIRE(samples.has_value());

    decode::P25Config config;
    config.rate = kRate;
    auto decoder = decode::P25Phase1::create(config);
    REQUIRE(decoder.has_value());
    std::vector<decode::P25Frame> frames;
    REQUIRE(decoder->process(*samples, frames).has_value());
    return frames;
}

}  // namespace

// ---------------------------------------------------------------------------
// The codes, against the document
// ---------------------------------------------------------------------------

TEST_CASE("the data interleave reproduces every row Table 7-4 prints", "[decode][p25][tsbk]") {
    // TIA-102.BAAA-A Table 7-4 as the archive copy's text layer carries it,
    // (output index, input index), four column pairs.
    constexpr std::array<std::array<int, 2>, 50> kPrinted = {{
        {0, 0},   {1, 1},   {2, 8},   {3, 9},   {4, 16},  {5, 17},  {18, 72},
        {19, 73}, {20, 80}, {21, 81}, {22, 88}, {23, 89}, {24, 96}, {25, 97},
        {26, 2},  {27, 3},  {28, 10}, {29, 11}, {30, 18}, {31, 19}, {44, 74},
        {45, 75}, {46, 82}, {47, 83}, {48, 90}, {49, 91}, {50, 4},  {51, 5},
        {52, 12}, {53, 13}, {54, 20}, {55, 21}, {68, 76}, {69, 77}, {70, 84},
        {71, 85}, {72, 92}, {73, 93}, {74, 6},  {75, 7},  {76, 14}, {77, 15},
        {78, 22}, {79, 23}, {92, 78}, {93, 79}, {94, 86}, {95, 87}, {96, 94},
        {97, 95},
    }};
    const auto map = decode::p25_data_interleave_map();
    for (const auto& row : kPrinted) {
        INFO("output " << row[0]);
        CHECK(static_cast<int>(map[static_cast<std::size_t>(row[0])]) == row[1]);
    }

    // And it is a permutation, so nothing is sent twice or not at all.
    std::array<int, decode::kP25TrellisBlockDibits> seen{};
    for (const std::uint8_t input : map) {
        ++seen[input];
    }
    for (const int count : seen) {
        CHECK(count == 1);
    }
}

TEST_CASE("the trellis encoder follows Tables 7-2 and 7-3 from state zero",
          "[decode][p25][tsbk]") {
    // All-zero octets: every input dibit is 0, the machine stays in state 0
    // and sends Table 7-2's point 0 throughout, which Table 7-3 gives as +1
    // then -1, dibits %00 and %10 by Table 9-1.
    const std::array<std::uint8_t, 12> zeros{};
    const auto sent = decode::p25_trellis12_encode(zeros);
    const auto map = decode::p25_data_interleave_map();
    for (std::size_t o = 0; o < sent.size(); ++o) {
        const std::size_t input = map[o];
        CHECK(sent[o] == (input % 2 == 0 ? 0b00 : 0b10));
    }

    // One octet $40: the first input dibit is %01, so the first point is
    // Table 7-2 row 0, column 1, point 15, which Table 7-3 sends as -3 then
    // +1, and the second is row 1, column 0, point 4, -3 then -1.
    std::array<std::uint8_t, 12> one{};
    one[0] = 0x40;
    const auto coded = decode::p25_trellis12_encode(one);
    std::array<std::uint8_t, decode::kP25TrellisBlockDibits> deinterleaved{};
    for (std::size_t o = 0; o < coded.size(); ++o) {
        deinterleaved[map[o]] = coded[o];
    }
    CHECK(deinterleaved[0] == 0b11);
    CHECK(deinterleaved[1] == 0b00);
    CHECK(deinterleaved[2] == 0b11);
    CHECK(deinterleaved[3] == 0b10);
}

TEST_CASE("the header CRC is clause 6.2's division, inverted", "[decode][p25][tsbk]") {
    // A second reading of clause 6.2, written as the long division it
    // describes: x^16 M(x) as a bit string, reduced by G(x), then I(x)
    // added.
    const auto by_division = [](std::span<const std::uint8_t> octets) {
        std::vector<std::uint8_t> bits;
        for (const std::uint8_t octet : octets) {
            for (int b = 7; b >= 0; --b) {
                bits.push_back(static_cast<std::uint8_t>((octet >> b) & 1));
            }
        }
        bits.insert(bits.end(), 16, 0);
        // x^16 + x^12 + x^5 + 1, highest degree first.
        std::array<std::uint8_t, 17> g{};
        g[0] = g[4] = g[11] = g[16] = 1;
        for (std::size_t i = 0; i + 16 < bits.size(); ++i) {
            if (bits[i] != 0) {
                for (std::size_t j = 0; j < g.size(); ++j) {
                    bits[i + j] ^= g[j];
                }
            }
        }
        std::uint16_t remainder = 0;
        for (std::size_t i = bits.size() - 16; i < bits.size(); ++i) {
            remainder = static_cast<std::uint16_t>((remainder << 1U) | bits[i]);
        }
        return static_cast<std::uint16_t>(remainder ^ 0xFFFFU);
    };

    std::mt19937 engine(0x7AB0);
    for (int trial = 0; trial < 200; ++trial) {
        std::array<std::uint8_t, 10> octets{};
        for (std::uint8_t& octet : octets) {
            octet = static_cast<std::uint8_t>(engine());
        }
        CHECK(decode::p25_header_crc(octets) == by_division(octets));
    }
    // All zero: the remainder is zero and only I(x) is left.
    const std::array<std::uint8_t, 10> zeros{};
    CHECK(decode::p25_header_crc(zeros) == 0xFFFF);
}

TEST_CASE("a TSBK with a damaged CRC is rejected and not parsed", "[decode][p25][tsbk]") {
    const Fields fields = rfss_status(0x12, true, 0x3A5, 0x01, 0x07, 0x1123, 0x70);
    auto octets = decode::p25_tsbk_with_crc(fields);
    REQUIRE(decode::p25_tsbk_from_octets(octets).crc_ok);

    // Any single bit, in the information or in the CRC itself.
    for (std::size_t bit = 0; bit < 96; ++bit) {
        auto damaged = octets;
        damaged[bit / 8] = static_cast<std::uint8_t>(damaged[bit / 8] ^ (0x80U >> (bit % 8)));
        const decode::P25Tsbk tsbk = decode::p25_tsbk_from_octets(damaged);
        INFO("bit " << bit);
        CHECK_FALSE(tsbk.crc_ok);
        CHECK(std::holds_alternative<std::monostate>(decode::p25_parse_tsbk(tsbk)));
    }
}

TEST_CASE("the trellis code's free distance is five bits", "[decode][p25][tsbk]") {
    // Not a number the clause states. Measured here from Tables 7-2 and 7-3
    // read through Table 9-1, as the least Hamming distance between two
    // paths that part and meet again, because it is what the case below
    // relies on: five bits means any two bit errors are corrected wherever
    // they fall, and errors further apart than an error event are corrected
    // independently. Clustered errors beyond two are not guaranteed, and an
    // earlier version of the case below that put four errors a quarter of a
    // block apart on the air found pairs the interleaver had brought within
    // one event of each other.
    const auto nibble = [](std::uint8_t point) {
        const auto dibit = [](std::int8_t level) {
            return decode::kP25SymbolToDibit[static_cast<std::size_t>((level + 3) / 2)];
        };
        return static_cast<std::uint8_t>(
            (dibit(decode::kP25TrellisConstellation[point][0]) << 2U) |
            dibit(decode::kP25TrellisConstellation[point][1]));
    };
    const auto weight = [](unsigned x) { return static_cast<int>(std::popcount(x)); };

    // Dijkstra over pairs of states, from every split to the first merge.
    int best = 1'000;
    for (std::uint8_t start = 0; start < 4; ++start) {
        std::array<std::array<int, 4>, 4> distance{};
        for (auto& row : distance) {
            row.fill(1'000);
        }
        std::array<std::array<bool, 4>, 4> done{};
        for (std::uint8_t a = 0; a < 4; ++a) {
            for (std::uint8_t b = 0; b < 4; ++b) {
                if (a != b) {
                    distance[a][b] =
                        weight(nibble(decode::kP25Trellis12Transitions[start][a]) ^
                               nibble(decode::kP25Trellis12Transitions[start][b]));
                }
            }
        }
        for (int round = 0; round < 16; ++round) {
            int s1 = -1;
            int s2 = -1;
            for (int a = 0; a < 4; ++a) {
                for (int b = 0; b < 4; ++b) {
                    if (!done[a][b] && distance[a][b] < 1'000 &&
                        (s1 < 0 || distance[a][b] < distance[s1][s2])) {
                        s1 = a;
                        s2 = b;
                    }
                }
            }
            if (s1 < 0) {
                break;
            }
            done[s1][s2] = true;
            if (s1 == s2) {
                best = std::min(best, distance[s1][s2]);
                continue;
            }
            for (std::uint8_t a = 0; a < 4; ++a) {
                for (std::uint8_t b = 0; b < 4; ++b) {
                    const int step = weight(nibble(decode::kP25Trellis12Transitions[s1][a]) ^
                                            nibble(decode::kP25Trellis12Transitions[s2][b]));
                    distance[a][b] = std::min(distance[a][b], distance[s1][s2] + step);
                }
            }
        }
    }
    CHECK(best == 5);
}

TEST_CASE("the trellis decoder corrects any two bit errors", "[decode][p25][tsbk]") {
    std::mt19937 engine(0x25);
    std::uniform_int_distribution<std::size_t> position(0, decode::kP25TrellisBlockDibits - 1);
    for (int trial = 0; trial < 500; ++trial) {
        Fields fields{};
        for (std::uint8_t& octet : fields) {
            octet = static_cast<std::uint8_t>(engine());
        }
        const auto octets = decode::p25_tsbk_with_crc(fields);
        auto sent = decode::p25_trellis12_encode(octets);

        const decode::P25Tsbk clean = decode::p25_decode_tsbk(sent);
        CHECK(clean.octets == octets);
        CHECK(clean.crc_ok);
        CHECK(clean.corrected_bits == 0);

        // Two bits anywhere, the same dibit or not.
        std::size_t first = position(engine);
        std::size_t second = position(engine);
        const auto bit = [&] { return static_cast<std::uint8_t>(1U << (engine() & 1U)); };
        const std::uint8_t first_bit = bit();
        std::uint8_t second_bit = bit();
        if (first == second && first_bit == second_bit) {
            second_bit = static_cast<std::uint8_t>(second_bit ^ 0b11U);
        }
        sent[first] = static_cast<std::uint8_t>(sent[first] ^ first_bit);
        sent[second] = static_cast<std::uint8_t>(sent[second] ^ second_bit);

        const decode::P25Tsbk repaired = decode::p25_decode_tsbk(sent);
        INFO("trial " << trial << ", dibits " << first << " and " << second);
        CHECK(repaired.octets == octets);
        CHECK(repaired.crc_ok);
        CHECK(repaired.corrected_bits == 2);
    }
}

// ---------------------------------------------------------------------------
// The messages
// ---------------------------------------------------------------------------

TEST_CASE("a group voice channel grant parses to the values sent", "[decode][p25][tsbk]") {
    // Emergency and protected set, priority 4; channel identifier 2,
    // number 0x345.
    const decode::P25Tsbk tsbk =
        tsbk_of(group_voice_grant(0xC4, 0x2345, 0x1F2E, 0xABCDEF));
    REQUIRE(tsbk.crc_ok);
    CHECK(tsbk.opcode == decode::kP25OpGroupVoiceGrant);
    const auto message = decode::p25_parse_tsbk(tsbk);
    const auto* grant = std::get_if<decode::P25GroupVoiceGrant>(&message);
    REQUIRE(grant != nullptr);
    CHECK(grant->options.emergency());
    CHECK(grant->options.protected_mode());
    CHECK_FALSE(grant->options.duplex());
    CHECK(grant->options.priority() == 4);
    CHECK(grant->channel.identifier == 2);
    CHECK(grant->channel.number == 0x345);
    CHECK(grant->group_address == 0x1F2E);
    CHECK(grant->source_address == 0xABCDEF);
}

TEST_CASE("a group voice channel grant update parses both pairs", "[decode][p25][tsbk]") {
    const Fields fields{0x02, 0x00, 0x10, 0x64, 0x00, 0x2A, 0xF3, 0xFF, 0xFF, 0xFE};
    const auto message = decode::p25_parse_tsbk(tsbk_of(fields));
    const auto* update = std::get_if<decode::P25GroupVoiceGrantUpdate>(&message);
    REQUIRE(update != nullptr);
    CHECK(update->channel[0] == decode::P25Channel{1, 0x064});
    CHECK(update->group_address[0] == 0x002A);
    CHECK(update->channel[1] == decode::P25Channel{0xF, 0x3FF});
    CHECK(update->group_address[1] == 0xFFFE);
}

TEST_CASE("an explicit group voice grant update parses both channels", "[decode][p25][tsbk]") {
    const Fields fields{0x03, 0x00, 0x00, 0x00, 0x31, 0x23, 0x32, 0x45, 0x12, 0x34};
    const auto message = decode::p25_parse_tsbk(tsbk_of(fields));
    const auto* update = std::get_if<decode::P25GroupVoiceGrantUpdateExplicit>(&message);
    REQUIRE(update != nullptr);
    CHECK(update->options.raw == 0);
    CHECK(update->transmit == decode::P25Channel{3, 0x123});
    CHECK(update->receive == decode::P25Channel{3, 0x245});
    CHECK(update->group_address == 0x1234);
}

TEST_CASE("an identifier update turns a channel number into a frequency",
          "[decode][p25][tsbk]") {
    // 851.00625 MHz base, 12.5 kHz bandwidth (100 eighths of a kHz), 6.25
    // kHz spacing (50), and a 45 MHz offset below (sign 0, 180 x 250 kHz).
    // 851.00625 MHz is outside both of clause 6.2.9's VHF and UHF ranges, so
    // opcode $3D keeps Figure 6.2.9-1's layout.
    const decode::P25Tsbk tsbk = tsbk_of(identifier_update(1, 100, 180, 50, 170'201'250));
    const auto message = decode::p25_parse_tsbk(tsbk);
    const auto* iden = std::get_if<decode::P25IdentifierUpdate>(&message);
    REQUIRE(iden != nullptr);
    CHECK_FALSE(iden->vhf_uhf);
    CHECK(iden->identifier == 1);
    CHECK(iden->bandwidth_raw == 100);
    CHECK(iden->transmit_offset_raw == 180);
    CHECK(iden->channel_spacing_raw == 50);
    CHECK(iden->base_frequency_raw == 170'201'250U);
    CHECK(iden->base_frequency_hz == 851'006'250U);
    CHECK(iden->channel_spacing_hz == 6'250U);
    REQUIRE(iden->bandwidth_hz.has_value());
    CHECK(*iden->bandwidth_hz == 12'500U);
    REQUIRE(iden->transmit_offset_hz.has_value());
    CHECK(*iden->transmit_offset_hz == -45'000'000);

    // Clause 2.3.9.2: base + number x spacing.
    CHECK(decode::p25_channel_frequency_hz(*iden, decode::P25Channel{1, 100}) ==
          std::optional<std::uint64_t>{851'631'250U});
    // A channel under another identifier is not this one's to resolve.
    CHECK_FALSE(decode::p25_channel_frequency_hz(*iden, decode::P25Channel{2, 100}).has_value());

    // Clause 2.3.35's $80, read as the offset value: no standard offset.
    const auto none = decode::p25_parse_tsbk(tsbk_of(identifier_update(1, 100, 0x180, 50,
                                                                       170'201'250)));
    REQUIRE(std::holds_alternative<decode::P25IdentifierUpdate>(none));
    CHECK_FALSE(std::get<decode::P25IdentifierUpdate>(none).transmit_offset_hz.has_value());
}

TEST_CASE("the VHF/UHF identifier update parses under both opcodes", "[decode][p25][tsbk]") {
    // 451.000 MHz base, 12.5 kHz spacing (100), BW VU %0101 (12.5 kHz), and
    // the subscriber transmitting 5 MHz above: sign 1, 400 channel spacings.
    const std::uint16_t offset = 0x2000U | 400U;
    const Fields vu = identifier_update_vu(4, 0b0101, offset, 100, 90'200'000);
    Fields same_opcode = vu;
    same_opcode[0] = 0x3D;  // clause 6.2.9: selected by the base frequency

    for (const Fields& fields : {vu, same_opcode}) {
        const auto message = decode::p25_parse_tsbk(tsbk_of(fields));
        const auto* iden = std::get_if<decode::P25IdentifierUpdate>(&message);
        REQUIRE(iden != nullptr);
        CHECK(iden->vhf_uhf);
        CHECK(iden->identifier == 4);
        CHECK(iden->bandwidth_raw == 0b0101);
        CHECK(iden->bandwidth_hz == std::optional<std::uint32_t>{12'500U});
        CHECK(iden->transmit_offset_raw == offset);
        CHECK(iden->transmit_offset_hz == std::optional<std::int64_t>{5'000'000});
        CHECK(iden->base_frequency_hz == 451'000'000U);
        CHECK(decode::p25_channel_frequency_hz(*iden, decode::P25Channel{4, 7}) ==
              std::optional<std::uint64_t>{451'087'500U});
    }
}

TEST_CASE("the RFSS, network and adjacent status broadcasts parse", "[decode][p25][tsbk]") {
    {
        const auto message = decode::p25_parse_tsbk(
            tsbk_of(rfss_status(0x12, true, 0x3A5, 0x01, 0x07, 0x1123, 0x70)));
        const auto* rfss = std::get_if<decode::P25RfssStatus>(&message);
        REQUIRE(rfss != nullptr);
        CHECK(rfss->lra == 0x12);
        CHECK(rfss->network_active);
        CHECK(rfss->system_id == 0x3A5);
        CHECK(rfss->rfss_id == 0x01);
        CHECK(rfss->site_id == 0x07);
        CHECK(rfss->channel == decode::P25Channel{1, 0x123});
        CHECK(rfss->system_service_class == 0x70);
    }
    {
        const auto message = decode::p25_parse_tsbk(
            tsbk_of(network_status(0x00, 0xBEE00, 0x6C1, 0x1001, 0xF0)));
        const auto* net = std::get_if<decode::P25NetworkStatus>(&message);
        REQUIRE(net != nullptr);
        CHECK(net->wacn_id == 0xBEE00U);
        CHECK(net->system_id == 0x6C1);
        CHECK(net->channel == decode::P25Channel{1, 0x001});
        CHECK(net->system_service_class == 0xF0);
    }
    {
        // Figure 6.2.2-1 is Figure 6.2.15-1 with C, F and V beside A.
        Fields fields = rfss_status(0x05, true, 0x123, 0x02, 0x09, 0x2050, 0x10);
        fields[0] = 0x3C;
        fields[3] = static_cast<std::uint8_t>(fields[3] | 0xA0U);  // C and V
        const auto message = decode::p25_parse_tsbk(tsbk_of(fields));
        const auto* adjacent = std::get_if<decode::P25AdjacentStatus>(&message);
        REQUIRE(adjacent != nullptr);
        CHECK(adjacent->conventional);
        CHECK_FALSE(adjacent->failure);
        CHECK(adjacent->valid);
        CHECK(adjacent->network_active);
        CHECK(adjacent->system_id == 0x123);
        CHECK(adjacent->rfss_id == 0x02);
        CHECK(adjacent->site_id == 0x09);
        CHECK(adjacent->channel == decode::P25Channel{2, 0x050});
        CHECK(adjacent->system_service_class == 0x10);
    }
}

TEST_CASE("protected, manufacturer and unparsed TSBKs keep their numbers",
          "[decode][p25][tsbk]") {
    // AABB-B clause 5.2: P set means the opcode and arguments are encrypted.
    Fields secret = group_voice_grant(0, 0x1001, 0x0101, 0x000001);
    secret[0] = 0x40;
    const decode::P25Tsbk protected_block = tsbk_of(secret);
    CHECK(protected_block.crc_ok);
    CHECK(protected_block.protected_block);
    CHECK(std::holds_alternative<std::monostate>(decode::p25_parse_tsbk(protected_block)));

    Fields vendor = group_voice_grant(0, 0x1001, 0x0101, 0x000001);
    vendor[1] = 0x90;
    const decode::P25Tsbk vendor_block = tsbk_of(vendor);
    CHECK(vendor_block.manufacturer_id == 0x90);
    CHECK(std::holds_alternative<std::monostate>(decode::p25_parse_tsbk(vendor_block)));

    // SYS_SRV_BCST, Table 6.2-1's %111000, is not parsed here and comes out
    // by number and by name.
    const Fields system_service{0x38, 0x00, 0, 0, 0, 0, 0, 0, 0, 0};
    const decode::P25Tsbk unparsed = tsbk_of(system_service);
    CHECK(unparsed.opcode == 0x38);
    CHECK(std::holds_alternative<std::monostate>(decode::p25_parse_tsbk(unparsed)));
    CHECK(decode::p25_osp_alias(0x38) == "SYS_SRV_BCST");
    CHECK(decode::p25_osp_alias(0x01).empty());
}

// ---------------------------------------------------------------------------
// The round trip
// ---------------------------------------------------------------------------

TEST_CASE("TSDUs of one, two and three blocks round trip through C4FM", "[decode][p25][tsbk]") {
    const Fields grant = group_voice_grant(0x00, 0x1064, 0x0FA0, 0x1234AB);
    const Fields iden = identifier_update(1, 100, 180, 50, 170'201'250);
    const Fields rfss = rfss_status(0x12, true, 0x3A5, 0x01, 0x07, 0x1123, 0x70);
    const Fields net = network_status(0x00, 0xBEE00, 0x3A5, 0x1123, 0x70);

    siggen::P25TsduMessage three{0x293, {rfss, net, iden}};
    siggen::P25TsduMessage one{0x293, {grant}};
    siggen::P25TsduMessage two{0x293, {iden, grant}};

    // AABB-B Figure 5-1 and the micro-slot arithmetic: 5, 8 and 10 slots.
    CHECK(decode::p25_tsdu_symbols(1) == 180);
    CHECK(decode::p25_tsdu_symbols(2) == 288);
    CHECK(decode::p25_tsdu_symbols(3) == 360);

    std::vector<std::uint8_t> stream;
    for (const auto* message : {&three, &one, &two, &three}) {
        auto dibits = siggen::p25_tsdu_dibits(*message);
        INFO((dibits.has_value() ? std::string{} : dibits.error().message));
        REQUIRE(dibits.has_value());
        CHECK(dibits->size() == decode::p25_tsdu_symbols(message->blocks.size()));
        stream.insert(stream.end(), dibits->begin(), dibits->end());
    }

    const std::vector<decode::P25Frame> frames = decode_dibits(stream);
    // The last TSDU is the tail the receiver's filter delay eats; the first
    // three have to come out.
    REQUIRE(frames.size() >= 3);
    const std::array<const siggen::P25TsduMessage*, 3> expected{&three, &one, &two};
    for (std::size_t f = 0; f < 3; ++f) {
        const decode::P25Frame& frame = frames[f];
        INFO("TSDU " << f);
        CHECK(frame.nid.duid == static_cast<std::uint8_t>(P25Duid::TrunkingSignalingDataUnit));
        CHECK(frame.nid.network_access_code == 0x293);
        CHECK(decode::p25_duid_name(frame.nid.duid) == "tsdu");
        REQUIRE(frame.tsbks.size() == expected[f]->blocks.size());
        for (std::size_t b = 0; b < frame.tsbks.size(); ++b) {
            const decode::P25Tsbk& tsbk = frame.tsbks[b];
            INFO("block " << b << ", trellis corrected " << tsbk.corrected_bits);
            CHECK(tsbk.crc_ok);
            CHECK(tsbk.last_block == (b + 1 == frame.tsbks.size()));
            Fields sent = expected[f]->blocks[b];
            sent[0] = static_cast<std::uint8_t>(tsbk.last_block ? (sent[0] | 0x80U) : sent[0]);
            CHECK(std::equal(sent.begin(), sent.end(), tsbk.octets.begin()));
        }
    }

    // And what the blocks say, from the three-block TSDU.
    const auto& site = frames[0].tsbks;
    const auto rfss_message = decode::p25_parse_tsbk(site[0]);
    REQUIRE(std::holds_alternative<decode::P25RfssStatus>(rfss_message));
    CHECK(std::get<decode::P25RfssStatus>(rfss_message).site_id == 0x07);
    const auto net_message = decode::p25_parse_tsbk(site[1]);
    REQUIRE(std::holds_alternative<decode::P25NetworkStatus>(net_message));
    CHECK(std::get<decode::P25NetworkStatus>(net_message).wacn_id == 0xBEE00U);
    const auto grant_message = decode::p25_parse_tsbk(frames[1].tsbks[0]);
    REQUIRE(std::holds_alternative<decode::P25GroupVoiceGrant>(grant_message));
    CHECK(std::get<decode::P25GroupVoiceGrant>(grant_message).group_address == 0x0FA0);
}

TEST_CASE("TSBK bit errors on the air are corrected by the trellis code",
          "[decode][p25][tsbk]") {
    const Fields grant = group_voice_grant(0x00, 0x1064, 0x0FA0, 0x1234AB);
    const Fields rfss = rfss_status(0x12, true, 0x3A5, 0x01, 0x07, 0x1123, 0x70);
    siggen::P25TsduMessage message{0x293, {rfss, grant}};

    auto clean = siggen::p25_tsdu_dibits(message);
    REQUIRE(clean.has_value());
    std::vector<std::uint8_t> damaged = *clean;

    // Three dibits wrong in each block, well apart, and none in the sync
    // word or the NID, which have codes of their own. A dibit at a status
    // symbol position would be thrown away, so none of these is one. Symbol
    // 60 is past the sync word and the NID with their status symbol, the
    // first block runs to about symbol 157 and the second to 259.
    std::size_t flipped = 0;
    for (const std::size_t symbol : {70U, 100U, 140U, 170U, 210U, 250U}) {
        if (symbol >= decode::kP25FirstStatusSymbol &&
            (symbol - decode::kP25FirstStatusSymbol) % decode::kP25StatusSymbolInterval == 0) {
            continue;
        }
        damaged[symbol] = static_cast<std::uint8_t>(damaged[symbol] ^ 0b01U);
        ++flipped;
    }
    REQUIRE(flipped == 6);

    std::vector<std::uint8_t> stream = damaged;
    stream.insert(stream.end(), clean->begin(), clean->end());
    const std::vector<decode::P25Frame> frames = decode_dibits(stream);
    REQUIRE_FALSE(frames.empty());
    REQUIRE(frames[0].tsbks.size() == 2);
    std::uint32_t corrected = 0;
    for (const decode::P25Tsbk& tsbk : frames[0].tsbks) {
        CHECK(tsbk.crc_ok);
        corrected += tsbk.corrected_bits;
    }
    INFO("trellis corrected " << corrected << " bits over both blocks");
    CHECK(corrected >= 6);
    const auto parsed = decode::p25_parse_tsbk(frames[0].tsbks[1]);
    REQUIRE(std::holds_alternative<decode::P25GroupVoiceGrant>(parsed));
    CHECK(std::get<decode::P25GroupVoiceGrant>(parsed).source_address == 0x1234AB);
}

TEST_CASE("a TSDU split across calls decodes the same as whole", "[decode][p25][tsbk]") {
    const Fields iden = identifier_update(1, 100, 180, 50, 170'201'250);
    const Fields rfss = rfss_status(0x12, true, 0x3A5, 0x01, 0x07, 0x1123, 0x70);
    siggen::P25TsduMessage message{0x293, {rfss, iden, rfss}};
    auto dibits = siggen::p25_tsdu_dibits(message);
    REQUIRE(dibits.has_value());
    std::vector<std::uint8_t> stream = *dibits;
    stream.insert(stream.end(), dibits->begin(), dibits->end());
    stream.insert(stream.end(), dibits->begin(), dibits->end());

    siggen::P25ModConfig mod;
    mod.rate = kRate;
    auto samples = siggen::p25_render_dibits(mod, stream);
    REQUIRE(samples.has_value());

    decode::P25Config config;
    config.rate = kRate;
    auto whole = decode::P25Phase1::create(config);
    auto split = decode::P25Phase1::create(config);
    REQUIRE(whole.has_value());
    REQUIRE(split.has_value());

    std::vector<decode::P25Frame> expected;
    REQUIRE(whole->process(*samples, expected).has_value());
    std::vector<decode::P25Frame> got;
    // 997 samples a call, which lands block boundaries everywhere.
    for (std::size_t at = 0; at < samples->size(); at += 997) {
        const std::size_t n = std::min<std::size_t>(997, samples->size() - at);
        REQUIRE(split->process(std::span(*samples).subspan(at, n), got).has_value());
    }
    REQUIRE(expected.size() >= 2);
    REQUIRE(got.size() == expected.size());
    for (std::size_t f = 0; f < got.size(); ++f) {
        CHECK(got[f].first_symbol == expected[f].first_symbol);
        REQUIRE(got[f].tsbks.size() == expected[f].tsbks.size());
        for (std::size_t b = 0; b < got[f].tsbks.size(); ++b) {
            CHECK(got[f].tsbks[b].octets == expected[f].tsbks[b].octets);
        }
    }
    CHECK(expected[0].tsbks.size() == 3);
}
