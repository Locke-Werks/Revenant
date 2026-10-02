// IL2P: the document's own example packets in both directions, the pieces
// they are built from, and the AX.25 decoder against the transmitter in
// core/dsp/synth.
//
// The example packets are what can see a misreading both ends share, which
// a round trip cannot: they were produced by another implementation, and
// core/decode/il2p.h lists the readings they settled. The round trips then
// cover what the examples do not reach: payloads past one block, errors, the
// sync tolerance, and IL2P beside plain AX.25 and FX.25 in one stream.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "core/decode/ax25.h"
#include "core/decode/il2p.h"
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

std::vector<std::uint8_t> frame_octets(const siggen::Ax25FrameSpec& spec) {
    auto octets = siggen::ax25_frame_octets(spec);
    REQUIRE(octets.has_value());
    return *octets;
}

// A UI frame with no path, which IL2P carries in a Type 1 header.
std::vector<std::uint8_t> ui_frame(std::string_view info) {
    siggen::Ax25FrameSpec spec;
    spec.destination = address("APRS", 0, true);
    spec.source = address("N7LEM", 7, false);
    spec.information.assign(info.begin(), info.end());
    return frame_octets(spec);
}

// The same with a path, which only Type 0 can carry.
std::vector<std::uint8_t> ui_frame_with_path(std::string_view info) {
    siggen::Ax25FrameSpec spec;
    spec.destination = address("APRS", 0, true);
    spec.source = address("N7LEM", 0, false);
    spec.repeaters.push_back(address("WIDE1", 1, false));
    spec.information.assign(info.begin(), info.end());
    return frame_octets(spec);
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

std::vector<float> render_levels(const std::vector<std::uint8_t>& levels) {
    auto audio = siggen::afsk_render_levels(siggen::Ax25ModConfig{}, levels);
    REQUIRE(audio.has_value());
    return *audio;
}

decode::Il2pCodec codec() {
    auto c = decode::Il2pCodec::create();
    REQUIRE(c.has_value());
    return std::move(*c);
}

std::vector<std::uint8_t> hex(std::string_view text) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < text.size() + 1;) {
        while (i < text.size() && text[i] == ' ') {
            ++i;
        }
        if (i >= text.size()) {
            break;
        }
        out.push_back(static_cast<std::uint8_t>(std::stoul(std::string(text.substr(i, 2)), nullptr, 16)));
        i += 2;
    }
    return out;
}

// "Example Encoded Packets": AX.25 data without flags or FCS, and the IL2P
// data after the sync word with the trailing CRC. The S frame's printed
// fields say N(R) 5 and P/F 1, while its AX.25 control octet 0x81 and its
// IL2P control subfield 0x24 both say N(R) 4 and P/F 0; the two encodings
// agree with each other, so the octets are what is tested.
struct Example {
    const char* name;
    std::vector<std::uint8_t> ax25;
    std::vector<std::uint8_t> il2p;
};

std::vector<Example> examples() {
    return {
        {"AX.25 S-Frame", hex("96 82 64 88 8A AE E4 96 96 68 90 8A 94 6F 81"),
         hex("26 57 4D 57 F1 D2 A8 F0 6A F2 7B AD 23 BD C0 7F 00 1D 2B")},
        {"AX.25 U-Frame", hex("86 A2 40 40 40 40 60 96 96 68 90 8A 94 FF 03 F0"),
         hex("6A EA 9C C2 01 11 FC 14 1F DA 6E F2 53 91 BD 47 6C 54 54")},
        {"AX.25 I-Frame",
         hex("96 82 64 88 8A AE E4 96 96 68 90 8A 94 65 B8 CF 30 31 32 33 34 35 36 37 38"),
         hex("26 13 6D 02 8C FE FB E8 AA 94 2D 6A 34 43 35 3C 69 9F 0C 75 5A 38 A1 7F A5 DA "
             "D8 F6 EA 57 37 3D B1 2A B0 DE 44 A8 20 D0 1D 5A 2B 38")},
    };
}

// The line bit at which octet `octet` after the sync word of the first
// packet starts.
std::size_t octet_bit(const siggen::Il2pChoice& choice, std::size_t octet) {
    return choice.preamble_bits + decode::kIl2pSyncBits + 8 * octet;
}

}  // namespace

TEST_CASE("IL2P decodes the document's example packets", "[decode][il2p]") {
    const decode::Il2pCodec c = codec();
    for (const Example& e : examples()) {
        INFO(e.name);
        const auto packet = c.decode(e.il2p);
        REQUIRE(packet.has_value());
        CHECK(packet->header.type == 1);
        CHECK(packet->header_corrected == 0);
        CHECK(packet->payload_ok);
        CHECK(packet->ax25 == e.ax25);
        CHECK(packet->crc == decode::Il2pCrc::Verified);
        CHECK(packet->error.empty());
    }

    // The I frame's printed fields: KA2DEW-2 to KK4HEJ-2, PID TheNET, nine
    // payload octets.
    const auto i_frame = c.decode(examples()[2].il2p);
    REQUIRE(i_frame.has_value());
    CHECK(i_frame->header.destination == "KA2DEW");
    CHECK(i_frame->header.source == "KK4HEJ");
    CHECK(i_frame->header.destination_ssid == 2);
    CHECK(i_frame->header.source_ssid == 2);
    CHECK(i_frame->header.pid == 0xE);
    CHECK(i_frame->header.payload_count == 9);
    CHECK_FALSE(i_frame->header.ui);
    // The U frame is UI with PID 0xF, "No L3".
    const auto u_frame = c.decode(examples()[1].il2p);
    REQUIRE(u_frame.has_value());
    CHECK(u_frame->header.ui);
    CHECK(u_frame->header.pid == 0xF);
    CHECK(u_frame->header.destination == "CQ    ");
    CHECK(u_frame->header.source_ssid == 15);

    // And the frames each parse as AX.25, the U frame as a response.
    auto parsed = decode::ax25_parse(u_frame->ax25);
    REQUIRE(parsed.has_value());
    CHECK(parsed->is_ui);
    CHECK(parsed->source.callsign == "KK4HEJ");
    CHECK_FALSE(parsed->destination.command_or_repeated);
    CHECK(parsed->source.command_or_repeated);
}

TEST_CASE("IL2P encodes the document's example frames to its example packets",
          "[decode][il2p]") {
    const decode::Il2pCodec c = codec();
    for (const Example& e : examples()) {
        INFO(e.name);
        auto encoded = c.encode(e.ax25, true);
        REQUIRE(encoded.has_value());
        CHECK(*encoded == e.il2p);
        // Without the trailer, the same less its four octets.
        auto bare = c.encode(e.ax25, false);
        REQUIRE(bare.has_value());
        CHECK(*bare == std::vector<std::uint8_t>(e.il2p.begin(), e.il2p.end() - 4));
    }
}

TEST_CASE("IL2P scrambling, the Hamming tables and the block layout", "[decode][il2p]") {
    std::mt19937_64 engine(0x11A2ULL);
    for (const std::size_t n : {1U, 2U, 13U, 239U}) {
        std::vector<std::uint8_t> data(n);
        for (auto& o : data) {
            o = static_cast<std::uint8_t>(engine());
        }
        const auto scrambled = decode::il2p_scramble(data);
        REQUIRE(scrambled.size() == n);
        CHECK(decode::il2p_descramble(scrambled) == data);
    }

    // "Hamming Decode Table": every codeword and every single-bit error of
    // it decodes to its nibble, the examples given in the text included.
    for (std::uint8_t d = 0; d < 16; ++d) {
        const std::uint8_t word = decode::kIl2pHammingEncode[d];
        CHECK(decode::kIl2pHammingDecode[word] == d);
        for (unsigned b = 0; b < 7; ++b) {
            CHECK(decode::kIl2pHammingDecode[word ^ (1U << b)] == d);
        }
    }
    CHECK(decode::kIl2pHammingEncode[0x7] == 0x47);
    CHECK(decode::kIl2pHammingDecode[0x6] == 0xE);

    // "Payload Block Size Computations".
    CHECK(decode::il2p_block_layout(0).block_count == 0);
    for (const std::size_t count : {1U, 9U, 238U, 239U, 240U, 478U, 479U, 600U, 1023U}) {
        INFO(count << " payload octets");
        const auto l = decode::il2p_block_layout(count);
        CHECK(l.block_count == (count + 238) / 239);
        std::size_t total = 0;
        std::size_t previous = 1000;
        for (std::size_t i = 0; i < l.block_count; ++i) {
            CHECK(l.size_of(i) <= decode::kIl2pMaximumBlockData);
            CHECK(l.size_of(i) <= previous);
            previous = l.size_of(i);
            total += l.size_of(i);
        }
        CHECK(total == count);
        CHECK(l.small_count + l.large_count == l.block_count);
    }

    // Header packing is its own inverse.
    decode::Il2pHeader h;
    h.type = 1;
    h.payload_count = 0x2A5;
    h.destination = "AB1CDE";
    h.source = "Z9    ";
    h.destination_ssid = 11;
    h.source_ssid = 4;
    h.ui = true;
    h.pid = 0xB;
    h.control = 0x55;
    const auto packed = decode::il2p_pack_header(h);
    const auto back = decode::il2p_unpack_header(packed);
    CHECK(back.type == 1);
    CHECK(back.payload_count == 0x2A5);
    CHECK(back.destination == h.destination);
    CHECK(back.source == h.source);
    CHECK(back.destination_ssid == 11);
    CHECK(back.source_ssid == 4);
    CHECK(back.ui);
    CHECK(back.pid == 0xB);
    CHECK(back.control == 0x55);
    CHECK_FALSE(back.reserved);
}

TEST_CASE("IL2P translates what Type 1 can carry and sends the rest Type 0", "[decode][il2p]") {
    const auto spec = [](std::uint8_t control, std::optional<std::uint8_t> pid, bool command) {
        siggen::Ax25FrameSpec s;
        s.destination = address("N0CALL", 3, command);
        s.source = address("KK4HEJ", 12, !command);
        s.control = control;
        s.pid = pid;
        return s;
    };
    struct Case {
        const char* name;
        siggen::Ax25FrameSpec spec;
        std::uint8_t type;
    };
    std::vector<Case> cases = {
        {"I, N(R) 7, P, N(S) 3", spec(0xF6, 0xF0, true), 1},
        {"I, PID IP", spec(0x00, 0xCC, true), 1},
        {"I as a response", spec(0x00, 0xF0, false), 0},
        {"I, PID with no mapping", spec(0x00, 0xC3, true), 0},
        {"I, layer 3 PID 0x2's form", spec(0x00, 0x20, true), 0},
        {"RR response, F", spec(0xB1, std::nullopt, false), 1},
        {"SREJ command", spec(0x4D, std::nullopt, true), 1},
        {"SABM", spec(0x3F, std::nullopt, true), 1},
        {"SABME", spec(0x7F, std::nullopt, true), 0},
        {"DISC", spec(0x53, std::nullopt, true), 1},
        {"DM", spec(0x1F, std::nullopt, false), 1},
        {"UA", spec(0x73, std::nullopt, false), 1},
        {"FRMR", spec(0x87, std::nullopt, false), 1},
        {"UI, P", spec(0x13, 0xF0, true), 1},
        {"XID", spec(0xAF, std::nullopt, true), 1},
        {"TEST", spec(0xE3, std::nullopt, false), 1},
    };
    Case with_path{"UI with a path", spec(0x03, 0xF0, true), 0};
    with_path.spec.repeaters.push_back(address("WIDE2", 2, false));
    cases.push_back(with_path);
    Case info{"UI with information", spec(0x03, 0xF0, true), 1};
    info.spec.information = {'h', 'i', 0x00, 0xFF};
    cases.push_back(info);

    const decode::Il2pCodec c = codec();
    for (const Case& k : cases) {
        INFO(k.name);
        const std::vector<std::uint8_t> ax25 = frame_octets(k.spec);
        auto t = decode::il2p_translate(ax25);
        REQUIRE(t.has_value());
        CHECK(t->header.type == k.type);
        auto back = decode::il2p_to_ax25(t->header, t->payload);
        REQUIRE(back.has_value());
        CHECK(*back == ax25);
        // And through the whole packet.
        auto packet = c.encode(ax25, true);
        REQUIRE(packet.has_value());
        const auto decoded = c.decode(*packet);
        REQUIRE(decoded.has_value());
        CHECK(decoded->ax25 == ax25);
        CHECK(decoded->crc == decode::Il2pCrc::Verified);
    }

    // 0x2 rebuilds as 0x20, and a "Future" PID does not rebuild at all.
    decode::Il2pHeader h;
    h.type = 1;
    h.destination = "A     ";
    h.source = "B     ";
    h.pid = 0x2;
    auto layer3 = decode::il2p_to_ax25(h, {});
    REQUIRE(layer3.has_value());
    CHECK(layer3->back() == 0x20);
    h.pid = 0x8;
    CHECK_FALSE(decode::il2p_to_ax25(h, {}).has_value());

    CHECK_FALSE(decode::il2p_translate(std::vector<std::uint8_t>(1024, 0x40)).has_value());
}

TEST_CASE("IL2P frames round trip through Bell 202 with no NRZI", "[decode][il2p]") {
    siggen::Ax25FrameSpec i_spec;
    i_spec.destination = address("N0CALL", 1, true);
    i_spec.source = address("N0CALL", 2, false);
    i_spec.control = 0x00;
    i_spec.pid = 0xF0;
    i_spec.information.assign(600, 0);
    for (std::size_t i = 0; i < i_spec.information.size(); ++i) {
        i_spec.information[i] = static_cast<std::uint8_t>(i * 7);
    }
    siggen::Ax25FrameSpec rr;
    rr.destination = address("N0CALL", 1, false);
    rr.source = address("N0CALL", 2, true);
    rr.control = 0xA1;
    rr.pid = std::nullopt;
    const std::vector<std::vector<std::uint8_t>> frames = {
        ui_frame("!4903.50N/07201.75W-IL2P Type 1"),
        ui_frame_with_path(">IL2P Type 0, a path"),
        frame_octets(i_spec),
        frame_octets(rr),
    };
    const std::vector<std::uint8_t> types = {1, 0, 1, 1};

    for (const bool crc : {true, false}) {
        siggen::Il2pChoice choice;
        choice.trailing_crc = crc;
        auto audio = siggen::il2p_render(siggen::Ax25ModConfig{}, frames, choice);
        REQUIRE(audio.has_value());
        for (const std::size_t block : {std::size_t{0}, std::size_t{1}, std::size_t{333}}) {
            INFO("trailing CRC " << crc << ", block of " << block);
            decode::Ax25Stats stats;
            const auto got = decode_all(*audio, block, &stats);
            CHECK(stats.il2p_packets == frames.size());
            CHECK(stats.il2p_uncorrectable == 0);
            CHECK(stats.il2p_untranslatable == 0);
            CHECK(stats.fx25_blocks == 0);
            REQUIRE(got.size() == frames.size());
            for (std::size_t i = 0; i < frames.size(); ++i) {
                CHECK(got[i].octets == frames[i]);
                CHECK(got[i].il2p);
                CHECK_FALSE(got[i].fx25);
                CHECK(got[i].il2p_header_type == types[i]);
                CHECK(got[i].il2p_corrected == 0);
                CHECK(got[i].il2p_crc == crc);
                if (i > 0) {
                    CHECK(got[i - 1].first_sample < got[i].first_sample);
                }
            }
        }
    }
}

TEST_CASE("IL2P corrects symbol errors that cost plain AX.25 the frame", "[decode][il2p]") {
    const std::vector<std::vector<std::uint8_t>> frames = {
        ui_frame("=4903.50N/07201.75W#a frame long enough to take several hits")};
    const siggen::Il2pChoice choice;
    auto clean = siggen::il2p_levels(frames, choice);
    REQUIRE(clean.has_value());
    const std::size_t header_octets = decode::kIl2pHeaderOctets + decode::kIl2pHeaderParity;

    // Line bits flipped, one in each of `payload` payload octets and
    // `header` header octets; with no NRZI each flip is one bit error.
    const auto corrupt = [&](std::size_t header, std::size_t payload) {
        std::vector<std::uint8_t> bits = *clean;
        for (std::size_t s = 0; s < header; ++s) {
            bits[octet_bit(choice, 2 + 5 * s) + (s % 8)] ^= 1U;
        }
        for (std::size_t s = 0; s < payload; ++s) {
            bits[octet_bit(choice, header_octets + 1 + 3 * s) + ((s + 3) % 8)] ^= 1U;
        }
        return render_levels(bits);
    };

    // Plain AX.25 loses the same frame to a single bit error.
    {
        siggen::Ax25ModConfig mod;
        std::vector<std::uint8_t> bits = siggen::hdlc_bits(frames, mod);
        bits[8 * mod.leading_flags + 8 * 20 + 3] ^= 1U;
        auto audio = siggen::afsk_render_bits(mod, bits);
        REQUIRE(audio.has_value());
        CHECK(decode_all(*audio).empty());
    }

    for (const std::size_t payload : {std::size_t{1}, std::size_t{4},
                                      decode::kIl2pPayloadParity / 2}) {
        for (const std::size_t header : {std::size_t{0}, std::size_t{1}}) {
            INFO(header << " header and " << payload << " payload octets hit");
            decode::Ax25Stats stats;
            const auto got = decode_all(corrupt(header, payload), 0, &stats);
            REQUIRE(got.size() == 1);
            CHECK(got[0].octets == frames[0]);
            CHECK(got[0].il2p);
            CHECK(got[0].il2p_corrected == header + payload);
            CHECK(got[0].il2p_crc);
        }
    }

    // One past the payload code's reach: refused, and nothing reported.
    {
        decode::Ax25Stats stats;
        const auto got = decode_all(corrupt(0, decode::kIl2pPayloadParity / 2 + 1), 0, &stats);
        CHECK(got.empty());
        CHECK(stats.il2p_packets == 1);
        CHECK(stats.il2p_uncorrectable == 1);
    }
    // One past the header code's: the header is refused and the packet with it.
    {
        decode::Ax25Stats stats;
        const auto got = decode_all(corrupt(2, 0), 0, &stats);
        CHECK(got.empty());
        CHECK(stats.il2p_packets == 0);
    }

    // "Sync Word": one bit wrong is still a match, two are not.
    for (const std::size_t flips : {std::size_t{1}, std::size_t{2}}) {
        INFO(flips << " sync bits flipped");
        std::vector<std::uint8_t> bits = *clean;
        for (std::size_t f = 0; f < flips; ++f) {
            bits[choice.preamble_bits + 5 + 9 * f] ^= 1U;
        }
        const auto got = decode_all(render_levels(bits));
        CHECK(got.size() == (flips == 1 ? 1U : 0U));
    }
}

TEST_CASE("IL2P refuses a trailing CRC that names another frame", "[decode][il2p]") {
    const decode::Il2pCodec c = codec();
    const std::vector<std::uint8_t> frame = ui_frame(">checked");
    auto packet = c.encode(frame, true);
    REQUIRE(packet.has_value());
    // Four exact codewords for a different CRC.
    const auto other = decode::il2p_encode_crc(ui_frame(">another"));
    std::copy(other.begin(), other.end(), packet->end() - 4);
    const auto decoded = c.decode(*packet);
    REQUIRE(decoded.has_value());
    CHECK(decoded->crc == decode::Il2pCrc::Mismatch);

    // One bit wrong in each trailer octet: Hamming still recovers the CRC.
    auto noisy = c.encode(frame, true);
    REQUIRE(noisy.has_value());
    for (std::size_t i = 0; i < 4; ++i) {
        (*noisy)[noisy->size() - 4 + i] ^= static_cast<std::uint8_t>(1U << (i + 1));
    }
    const auto recovered = c.decode(*noisy);
    REQUIRE(recovered.has_value());
    CHECK(recovered->crc == decode::Il2pCrc::Verified);

    // And through the receiver: a disagreeing CRC costs the frame.
    siggen::Il2pChoice choice;
    auto levels = siggen::il2p_levels(std::vector<std::vector<std::uint8_t>>{frame}, choice);
    REQUIRE(levels.has_value());
    const std::size_t trailer = octet_bit(choice, packet->size() - 4);
    for (std::size_t i = 0; i < 32; ++i) {
        (*levels)[trailer + i] = static_cast<std::uint8_t>((other[i / 8] >> (7 - i % 8)) & 1U);
    }
    decode::Ax25Stats stats;
    CHECK(decode_all(render_levels(*levels), 0, &stats).empty());
    CHECK(stats.il2p_uncorrectable == 1);
}

TEST_CASE("plain AX.25, FX.25 and IL2P decode together in one stream", "[decode][il2p]") {
    const std::vector<std::vector<std::uint8_t>> plain = {ui_frame_with_path(">plain one"),
                                                          ui_frame_with_path(">plain two")};
    const std::vector<std::vector<std::uint8_t>> wrapped = {ui_frame_with_path(">wrapped")};
    const std::vector<std::vector<std::uint8_t>> il2p = {ui_frame(">il2p one"),
                                                         ui_frame(">il2p two")};

    // Plain AX.25 alone finds no IL2P packet.
    {
        auto audio = siggen::ax25_render(siggen::Ax25ModConfig{}, plain);
        REQUIRE(audio.has_value());
        decode::Ax25Stats stats;
        const auto got = decode_all(*audio, 0, &stats);
        REQUIRE(got.size() == 2);
        CHECK(stats.il2p_packets == 0);
        CHECK_FALSE(got[0].il2p);
        CHECK_FALSE(got[1].il2p);
    }

    // Plain, FX.25, two IL2P back to back, plain: NRZI for the HDLC parts,
    // tone levels as they are for IL2P, one continuous-phase transmission.
    siggen::Ax25ModConfig mod;
    std::vector<std::uint8_t> levels;
    const auto append_nrzi = [&](const std::vector<std::uint8_t>& bits) {
        const auto l = siggen::nrzi_levels(bits);
        levels.insert(levels.end(), l.begin(), l.end());
    };
    append_nrzi(siggen::hdlc_bits(std::span(plain).first(1), mod));
    auto fx = siggen::fx25_bits(wrapped, siggen::Fx25Choice{}, mod);
    REQUIRE(fx.has_value());
    append_nrzi(*fx);
    siggen::Il2pChoice choice;
    choice.preamble_bits = 64;
    auto il = siggen::il2p_levels(il2p, choice);
    REQUIRE(il.has_value());
    levels.insert(levels.end(), il->begin(), il->end());
    append_nrzi(siggen::hdlc_bits(std::span(plain).subspan(1), mod));
    const std::vector<float> audio = render_levels(levels);

    for (const std::size_t block : {std::size_t{0}, std::size_t{1}, std::size_t{333}}) {
        INFO("block of " << block);
        decode::Ax25Stats stats;
        const auto got = decode_all(audio, block, &stats);
        REQUIRE(got.size() == 5);
        CHECK(got[0].octets == plain[0]);
        CHECK(got[1].octets == wrapped[0]);
        CHECK(got[1].fx25);
        CHECK(got[2].octets == il2p[0]);
        CHECK(got[2].il2p);
        CHECK(got[3].octets == il2p[1]);
        CHECK(got[3].il2p);
        CHECK(got[4].octets == plain[1]);
        for (const std::size_t i : {std::size_t{0}, std::size_t{4}}) {
            CHECK_FALSE(got[i].il2p);
            CHECK_FALSE(got[i].fx25);
        }
        for (std::size_t i = 1; i < got.size(); ++i) {
            CHECK(got[i - 1].first_sample < got[i].first_sample);
        }
        CHECK(stats.il2p_packets == 2);
        CHECK(stats.fx25_blocks == 1);
    }
}
