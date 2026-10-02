#include "core/decode/il2p.h"

#include <bit>

#include "core/decode/ax25.h"

namespace revenant::decode {

namespace {

// AX.25 2.2 Figure 4.4, the U-frame control octets with P/F clear, in the
// order of the IL2P opcodes 0 to 7 of "Translated AX.25 U-Frame Control
// Subfield Map": SABM, DISC, DM, UA, FRMR, UI, XID, TEST.
constexpr std::array<std::uint8_t, 8> kUControl = {0x2F, 0x43, 0x0F, 0x63, 0x87, 0x03, 0xAF, 0xE3};

// Figure 4.4's SABME, which the U-frame map says is "Not supported, send as
// Transparent".
constexpr std::uint8_t kSabme = 0x6F;

// AX.25 2.2 Figure 3.5: bit 7 of an SSID octet is the C bit, bits 6 and 5
// the reserved bits, bits 4 to 1 the SSID and bit 0 the extension bit.
constexpr std::uint8_t kSsidC = 0x80;
constexpr std::uint8_t kSsidReserved = 0x60;

// "IL2P Type 1 Header": "take the ASCII code for a printable character and
// subtract 0x20".
constexpr unsigned kSixbitOffset = 0x20;

std::vector<std::uint8_t> to_bits(std::span<const std::uint8_t> octets) {
    std::vector<std::uint8_t> bits;
    bits.reserve(8 * octets.size());
    for (const std::uint8_t octet : octets) {
        for (int b = 7; b >= 0; --b) {
            bits.push_back(static_cast<std::uint8_t>((octet >> b) & 1U));
        }
    }
    return bits;
}

std::vector<std::uint8_t> to_octets(std::span<const std::uint8_t> bits) {
    std::vector<std::uint8_t> octets(bits.size() / 8, 0);
    for (std::size_t i = 0; i < 8 * octets.size(); ++i) {
        octets[i / 8] = static_cast<std::uint8_t>((octets[i / 8] << 1U) | bits[i]);
    }
    return octets;
}

// The data octets of a Reed-Solomon word whose check octets follow.
void append_encoded(std::vector<std::uint8_t>& out, const Rs8Codec& codec,
                    std::span<const std::uint8_t> scrambled) {
    out.insert(out.end(), scrambled.begin(), scrambled.end());
    std::vector<std::uint8_t> check(codec.params().check_symbols, 0);
    codec.encode(scrambled, check);
    out.insert(out.end(), check.begin(), check.end());
}

}  // namespace

std::optional<std::uint8_t> il2p_ax25_pid(std::uint8_t il2p_pid) {
    // "IL2P AX.25 PID Code Mapping".
    switch (il2p_pid & 0x0FU) {
        case 0x2: return std::uint8_t{0x20};  // ENGINEERING CHOICE, see il2p.h
        case 0x3: return std::uint8_t{0x01};
        case 0x4: return std::uint8_t{0x06};
        case 0x5: return std::uint8_t{0x07};
        case 0x6: return std::uint8_t{0x08};
        case 0xB: return std::uint8_t{0xCC};
        case 0xC: return std::uint8_t{0xCD};
        case 0xD: return std::uint8_t{0xCE};
        case 0xE: return std::uint8_t{0xCF};
        case 0xF: return std::uint8_t{0xF0};
        default: return std::nullopt;
    }
}

std::optional<std::uint8_t> il2p_pid_for(std::uint8_t ax25_pid) {
    for (std::uint8_t p = 0x3; p <= 0xF; ++p) {
        if (const auto mapped = il2p_ax25_pid(p); mapped && *mapped == ax25_pid) {
            return p;
        }
    }
    return std::nullopt;
}

std::vector<std::uint8_t> il2p_scramble(std::span<const std::uint8_t> block) {
    // Stages 0 to 8 from the input end. Five zeros, then four ones.
    std::array<std::uint8_t, 9> c = {0, 0, 0, 0, 0, 1, 1, 1, 1};
    std::vector<std::uint8_t> in = to_bits(block);
    // "Extracting All Data from LFSR Memory": five more clocks flush it.
    constexpr std::size_t kDelay = 5;
    in.insert(in.end(), kDelay, 0);
    std::vector<std::uint8_t> out;
    out.reserve(in.size());
    for (const std::uint8_t d : in) {
        const std::uint8_t feedback = c[8];
        const auto tap = static_cast<std::uint8_t>(c[4] ^ feedback);
        out.push_back(tap);
        c = {static_cast<std::uint8_t>(d ^ feedback), c[0], c[1], c[2], c[3], tap, c[5], c[6], c[7]};
    }
    return to_octets(std::span<const std::uint8_t>(out).subspan(kDelay));
}

std::vector<std::uint8_t> il2p_descramble(std::span<const std::uint8_t> block) {
    // Five ones, then four zeros.
    std::array<std::uint8_t, 9> c = {1, 1, 1, 1, 1, 0, 0, 0, 0};
    const std::vector<std::uint8_t> in = to_bits(block);
    std::vector<std::uint8_t> out;
    out.reserve(in.size());
    for (const std::uint8_t d : in) {
        out.push_back(static_cast<std::uint8_t>(c[8] ^ d));
        c = {d, c[0], c[1], c[2], c[3], static_cast<std::uint8_t>(c[4] ^ d), c[5], c[6], c[7]};
    }
    return to_octets(out);
}

std::array<std::uint8_t, kIl2pHeaderOctets> il2p_pack_header(const Il2pHeader& h) {
    std::array<std::uint8_t, kIl2pHeaderOctets> o{};
    const auto sixbit = [](const std::string& call, std::size_t i) {
        const char c = i < call.size() ? call[i] : ' ';
        return static_cast<std::uint8_t>((static_cast<unsigned>(c) - kSixbitOffset) & 0x3FU);
    };
    if (h.type == 1) {
        for (std::size_t i = 0; i < 6; ++i) {
            o[i] = sixbit(h.destination, i);
            o[6 + i] = sixbit(h.source, i);
        }
        o[0] = static_cast<std::uint8_t>(o[0] | (h.ui ? 0x40U : 0U));
        for (std::size_t i = 0; i < 4; ++i) {
            o[1 + i] = static_cast<std::uint8_t>(o[1 + i] | (((h.pid >> (3 - i)) & 1U) << 6U));
        }
        for (std::size_t i = 0; i < 7; ++i) {
            o[5 + i] = static_cast<std::uint8_t>(o[5 + i] | (((h.control >> (6 - i)) & 1U) << 6U));
        }
        o[12] = static_cast<std::uint8_t>(((h.destination_ssid & 0x0FU) << 4U) |
                                          (h.source_ssid & 0x0FU));
    }
    o[0] = static_cast<std::uint8_t>(o[0] | (h.reserved ? 0x80U : 0U));
    o[1] = static_cast<std::uint8_t>(o[1] | ((h.type & 1U) << 7U));
    for (std::size_t i = 0; i < 10; ++i) {
        o[2 + i] = static_cast<std::uint8_t>(o[2 + i] | (((h.payload_count >> (9 - i)) & 1U) << 7U));
    }
    return o;
}

Il2pHeader il2p_unpack_header(std::span<const std::uint8_t, kIl2pHeaderOctets> o) {
    Il2pHeader h;
    h.reserved = (o[0] & 0x80U) != 0U;
    h.type = static_cast<std::uint8_t>((o[1] >> 7U) & 1U);
    for (std::size_t i = 0; i < 10; ++i) {
        h.payload_count = static_cast<std::uint16_t>((h.payload_count << 1U) | ((o[2 + i] >> 7U) & 1U));
    }
    if (h.type != 1) {
        return h;
    }
    for (std::size_t i = 0; i < 6; ++i) {
        h.destination.push_back(static_cast<char>((o[i] & 0x3FU) + kSixbitOffset));
        h.source.push_back(static_cast<char>((o[6 + i] & 0x3FU) + kSixbitOffset));
    }
    h.ui = (o[0] & 0x40U) != 0U;
    for (std::size_t i = 0; i < 4; ++i) {
        h.pid = static_cast<std::uint8_t>((h.pid << 1U) | ((o[1 + i] >> 6U) & 1U));
    }
    for (std::size_t i = 0; i < 7; ++i) {
        h.control = static_cast<std::uint8_t>((h.control << 1U) | ((o[5 + i] >> 6U) & 1U));
    }
    h.destination_ssid = static_cast<std::uint8_t>(o[12] >> 4U);
    h.source_ssid = static_cast<std::uint8_t>(o[12] & 0x0FU);
    return h;
}

Il2pBlockLayout il2p_block_layout(std::size_t payload_count) {
    Il2pBlockLayout l;
    if (payload_count == 0) {
        return l;
    }
    l.block_count = (payload_count + kIl2pMaximumBlockData - 1) / kIl2pMaximumBlockData;
    l.small_size = payload_count / l.block_count;
    l.large_size = l.small_size + 1;
    l.large_count = payload_count - l.block_count * l.small_size;
    l.small_count = l.block_count - l.large_count;
    return l;
}

Expected<Il2pTranslation> il2p_translate(std::span<const std::uint8_t> ax25) {
    if (ax25.empty() || ax25.size() > kIl2pMaximumPayload) {
        return fail("an IL2P payload is 1 to 1023 octets");
    }
    Il2pTranslation t;
    const auto transparent = [&]() {
        // "IL2P Type 0 Header": the whole frame as payload, every other
        // header field zero (transmit procedure, Type 0 step 6).
        t.header = Il2pHeader{};
        t.header.payload_count = static_cast<std::uint16_t>(ax25.size());
        t.payload.assign(ax25.begin(), ax25.end());
        return t;
    };

    // "Type 1 Header Control and Addressing Subfields": no repeaters, no
    // modulo-128 sequence numbers, no character outside DEC SIXBIT. Beyond
    // those, anything the header has no room for goes Type 0 too, so the
    // receiver rebuilds the frame octet for octet: reserved SSID bits other
    // than the ones AX.25 2.2 clause 3.12.2 defaults to, and C bits that do
    // not make a command or a response.
    constexpr std::size_t kAddresses = 2 * kAx25AddressOctets;
    if (ax25.size() < kAddresses + 1) {
        return transparent();
    }
    const std::uint8_t dest_ssid = ax25[6];
    const std::uint8_t src_ssid = ax25[13];
    if ((dest_ssid & 1U) != 0U || (src_ssid & 1U) == 0U) {
        return transparent();
    }
    Il2pHeader h;
    h.type = 1;
    for (std::size_t i = 0; i < 6; ++i) {
        for (const std::size_t at : {i, kAx25AddressOctets + i}) {
            const std::uint8_t octet = ax25[at];
            const unsigned c = octet >> 1U;
            if ((octet & 1U) != 0U || c < kSixbitOffset || c > kSixbitOffset + 0x3FU) {
                return transparent();
            }
            (at < kAx25AddressOctets ? h.destination : h.source).push_back(static_cast<char>(c));
        }
    }
    if ((dest_ssid & kSsidReserved) != kSsidReserved || (src_ssid & kSsidReserved) != kSsidReserved) {
        return transparent();
    }
    const bool dest_c = (dest_ssid & kSsidC) != 0U;
    const bool src_c = (src_ssid & kSsidC) != 0U;
    if (dest_c == src_c) {
        return transparent();
    }
    // AX.25 2.2 clause 6.1.2: a command has the destination's C bit set and
    // the source's clear.
    const unsigned command = dest_c ? 1U : 0U;
    h.destination_ssid = static_cast<std::uint8_t>((dest_ssid >> 1U) & 0x0FU);
    h.source_ssid = static_cast<std::uint8_t>((src_ssid >> 1U) & 0x0FU);

    const std::uint8_t control = ax25[kAddresses];
    const unsigned pf = (control >> 4U) & 1U;
    std::size_t info = kAddresses + 1;
    if ((control & 0x01U) == 0U) {
        // "Translated AX.25 I-Frame Control Subfield": I frames are
        // commands, so the C bit is not carried and a response cannot be.
        if (command == 0U || ax25.size() <= info) {
            return transparent();
        }
        const auto pid = il2p_pid_for(ax25[info]);
        if (!pid) {
            return transparent();
        }
        h.pid = *pid;
        h.control = static_cast<std::uint8_t>((pf << 6U) | (((control >> 5U) & 7U) << 3U) |
                                              ((control >> 1U) & 7U));
        ++info;
    } else if ((control & 0x03U) == 0x01U) {
        // "Translated AX.25 S-Frame Control Subfield Map", P/F at bit 6 by
        // the engineering choice in il2p.h.
        h.pid = 0x0;
        h.control = static_cast<std::uint8_t>((pf << 6U) | (((control >> 5U) & 7U) << 3U) |
                                              (command << 2U) | ((control >> 2U) & 3U));
    } else {
        // "Translated AX.25 U-Frame Control Subfield Map".
        const auto base = static_cast<std::uint8_t>(control & ~kAx25PollFinalBit);
        if (base == kSabme) {
            return transparent();
        }
        std::optional<unsigned> opcode;
        for (unsigned op = 0; op < kUControl.size(); ++op) {
            if (kUControl[op] == base) {
                opcode = op;
            }
        }
        if (!opcode) {
            return transparent();
        }
        h.control = static_cast<std::uint8_t>((pf << 6U) | (*opcode << 3U) | (command << 2U));
        if (base == kAx25ControlUi) {
            // "UI Subfield": set for UI frames, whose PID the PID subfield
            // then carries.
            if (ax25.size() <= info) {
                return transparent();
            }
            const auto pid = il2p_pid_for(ax25[info]);
            if (!pid) {
                return transparent();
            }
            h.ui = true;
            h.pid = *pid;
            ++info;
        } else {
            h.pid = 0x1;
        }
    }
    t.header = h;
    t.header.payload_count = static_cast<std::uint16_t>(ax25.size() - info);
    t.payload.assign(ax25.begin() + static_cast<std::ptrdiff_t>(info), ax25.end());
    return t;
}

Expected<std::vector<std::uint8_t>> il2p_to_ax25(const Il2pHeader& h,
                                                std::span<const std::uint8_t> payload) {
    if (h.type != 1) {
        return std::vector<std::uint8_t>(payload.begin(), payload.end());
    }
    const unsigned ctl = h.control;
    const unsigned pf = (ctl >> 6U) & 1U;
    unsigned command = 1;
    std::uint8_t control = 0;
    std::optional<std::uint8_t> pid;
    if (h.pid == 0x0) {
        command = (ctl >> 2U) & 1U;
        control = static_cast<std::uint8_t>((((ctl >> 3U) & 7U) << 5U) | (pf << 4U) |
                                            ((ctl & 3U) << 2U) | 0x01U);
    } else if (h.pid == 0x1 || h.ui) {
        command = (ctl >> 2U) & 1U;
        control = static_cast<std::uint8_t>(kUControl[(ctl >> 3U) & 7U] | (pf << 4U));
        if (h.ui) {
            pid = il2p_ax25_pid(h.pid);
            if (!pid) {
                return fail("an IL2P UI header names a PID the mapping table does not define");
            }
        }
    } else {
        control = static_cast<std::uint8_t>((((ctl >> 3U) & 7U) << 5U) | (pf << 4U) |
                                            ((ctl & 7U) << 1U));
        pid = il2p_ax25_pid(h.pid);
        if (!pid) {
            return fail("an IL2P header names a PID the mapping table leaves to the future");
        }
    }

    std::vector<std::uint8_t> out;
    out.reserve(2 * kAx25AddressOctets + 2 + payload.size());
    const auto call = [&out](const std::string& text) {
        for (std::size_t i = 0; i < 6; ++i) {
            const char c = i < text.size() ? text[i] : ' ';
            out.push_back(static_cast<std::uint8_t>(static_cast<unsigned>(c) << 1U));
        }
    };
    call(h.destination);
    out.push_back(static_cast<std::uint8_t>((command != 0U ? kSsidC : 0U) | kSsidReserved |
                                            ((h.destination_ssid & 0x0FU) << 1U)));
    call(h.source);
    out.push_back(static_cast<std::uint8_t>((command != 0U ? 0U : kSsidC) | kSsidReserved |
                                            ((h.source_ssid & 0x0FU) << 1U) | 0x01U));
    out.push_back(control);
    if (pid) {
        out.push_back(*pid);
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Rs8Params il2p_rs_params(std::size_t check_symbols) {
    return Rs8Params{kIl2pFieldPolynomial, kIl2pFirstRoot, check_symbols};
}

std::array<std::uint8_t, kIl2pCrcOctets> il2p_encode_crc(std::span<const std::uint8_t> ax25) {
    // "Optional Trailing CRC": "calculated in the same manner as for an AX.25
    // frame", which the examples confirm is the FCS value itself, and
    // "Encoded CRC Format": CRC3, the high nibble, first.
    const std::uint16_t crc = ax25_fcs(ax25);
    std::array<std::uint8_t, kIl2pCrcOctets> out{};
    for (std::size_t i = 0; i < kIl2pCrcOctets; ++i) {
        out[i] = kIl2pHammingEncode[(crc >> (12U - 4U * i)) & 0x0FU];
    }
    return out;
}

Il2pCrc il2p_check_crc(std::span<const std::uint8_t, kIl2pCrcOctets> trailer,
                       std::span<const std::uint8_t> ax25) {
    unsigned crc = 0;
    bool exact = true;
    for (const std::uint8_t octet : trailer) {
        const std::uint8_t nibble = kIl2pHammingDecode[octet & 0x7FU];
        crc = (crc << 4U) | nibble;
        exact = exact && kIl2pHammingEncode[nibble] == octet;
    }
    if (crc == ax25_fcs(ax25)) {
        return Il2pCrc::Verified;
    }
    return exact ? Il2pCrc::Mismatch : Il2pCrc::Absent;
}

// ---------------------------------------------------------------------------
// Il2pCodec
// ---------------------------------------------------------------------------

Expected<Il2pCodec> Il2pCodec::create() {
    Il2pCodec codec;
    for (const std::size_t check : {kIl2pHeaderParity, kIl2pPayloadParity}) {
        auto c = Rs8Codec::create(il2p_rs_params(check));
        if (!c) {
            return std::unexpected(with_context(c.error(), "IL2P"));
        }
        codec.codecs_.push_back(std::move(*c));
    }
    return codec;
}

std::optional<Il2pCodec::HeaderResult> Il2pCodec::decode_header(
    std::span<const std::uint8_t> octets) const {
    if (octets.size() < kIl2pHeaderOctets + kIl2pHeaderParity) {
        return std::nullopt;
    }
    // Receive procedure steps 3 to 5: RS decode, then unscramble.
    std::vector<std::uint8_t> word(octets.begin(),
                                   octets.begin() + kIl2pHeaderOctets + kIl2pHeaderParity);
    const auto corrected = header_codec().decode(word);
    if (!corrected) {
        return std::nullopt;
    }
    const std::vector<std::uint8_t> plain =
        il2p_descramble(std::span<const std::uint8_t>(word).first(kIl2pHeaderOctets));
    HeaderResult r;
    r.header = il2p_unpack_header(std::span<const std::uint8_t, kIl2pHeaderOctets>(plain.data(),
                                                                                 kIl2pHeaderOctets));
    r.corrected = *corrected;
    return r;
}

std::optional<Il2pCodec::PayloadResult> Il2pCodec::decode_payload(
    std::size_t payload_count, std::span<const std::uint8_t> octets) const {
    const Il2pBlockLayout layout = il2p_block_layout(payload_count);
    if (octets.size() < layout.air_octets(payload_count)) {
        return std::nullopt;
    }
    PayloadResult r;
    std::size_t at = 0;
    for (std::size_t i = 0; i < layout.block_count; ++i) {
        const std::size_t size = layout.size_of(i);
        std::vector<std::uint8_t> word(octets.begin() + static_cast<std::ptrdiff_t>(at),
                                       octets.begin() +
                                           static_cast<std::ptrdiff_t>(at + size + kIl2pPayloadParity));
        at += size + kIl2pPayloadParity;
        // Receive procedure step 10: "RS decode and then unscramble each
        // payload block", the register reset for each.
        const auto corrected = payload_codec().decode(word);
        if (!corrected) {
            return std::nullopt;
        }
        r.corrected += *corrected;
        const std::vector<std::uint8_t> plain =
            il2p_descramble(std::span<const std::uint8_t>(word).first(size));
        r.payload.insert(r.payload.end(), plain.begin(), plain.end());
    }
    return r;
}

void Il2pCodec::finish(Il2pPacket& packet) const {
    if (!packet.payload_ok) {
        packet.error = "a payload block was refused by the Reed-Solomon decoder";
        return;
    }
    auto ax25 = il2p_to_ax25(packet.header, packet.payload);
    if (!ax25) {
        packet.error = ax25.error().message;
        return;
    }
    packet.ax25 = std::move(*ax25);
}

std::optional<Il2pPacket> Il2pCodec::decode(std::span<const std::uint8_t> octets) const {
    const auto header = decode_header(octets);
    if (!header) {
        return std::nullopt;
    }
    Il2pPacket packet;
    packet.header = header->header;
    packet.header_corrected = header->corrected;
    const std::size_t start = kIl2pHeaderOctets + kIl2pHeaderParity;
    const std::size_t air = il2p_block_layout(packet.header.payload_count)
                                .air_octets(packet.header.payload_count);
    if (octets.size() < start + air) {
        return std::nullopt;
    }
    const auto payload = decode_payload(packet.header.payload_count, octets.subspan(start, air));
    packet.payload_ok = payload.has_value();
    if (payload) {
        packet.payload = payload->payload;
        packet.payload_corrected = payload->corrected;
    }
    finish(packet);
    if (!packet.ax25.empty() && octets.size() >= start + air + kIl2pCrcOctets) {
        packet.crc = il2p_check_crc(
            std::span<const std::uint8_t, kIl2pCrcOctets>(octets.data() + start + air, kIl2pCrcOctets),
            packet.ax25);
    }
    return packet;
}

Expected<std::vector<std::uint8_t>> Il2pCodec::encode(std::span<const std::uint8_t> ax25,
                                                      bool crc) const {
    auto translated = il2p_translate(ax25);
    if (!translated) {
        return std::unexpected(translated.error());
    }
    std::vector<std::uint8_t> out;
    // Transmit procedure steps 4 to 7: compose, scramble, RS encode.
    const auto header = il2p_pack_header(translated->header);
    append_encoded(out, header_codec(), il2p_scramble(header));
    // Steps 8 to 10: "Scramble then RS encode each payload block (large
    // blocks closest to header)".
    const Il2pBlockLayout layout = il2p_block_layout(translated->payload.size());
    std::size_t at = 0;
    for (std::size_t i = 0; i < layout.block_count; ++i) {
        const std::size_t size = layout.size_of(i);
        append_encoded(out, payload_codec(),
                       il2p_scramble(std::span<const std::uint8_t>(translated->payload).subspan(at, size)));
        at += size;
    }
    if (crc) {
        const auto trailer = il2p_encode_crc(ax25);
        out.insert(out.end(), trailer.begin(), trailer.end());
    }
    return out;
}

// ---------------------------------------------------------------------------
// Il2pReceiver
// ---------------------------------------------------------------------------

Expected<Il2pReceiver> Il2pReceiver::create() {
    auto codec = Il2pCodec::create();
    if (!codec) {
        return std::unexpected(codec.error());
    }
    Il2pReceiver r;
    r.codec_ = std::move(*codec);
    return r;
}

void Il2pReceiver::reset() {
    state_ = State::Hunting;
    window_ = 0;
    window_fill_ = 0;
    sync_errors_ = 0;
    octets_.clear();
    partial_ = 0;
    partial_bits_ = 0;
    wanted_ = 0;
    packet_ = {};
    headers_ = 0;
}

std::optional<unsigned> Il2pReceiver::sync_match() const {
    if (window_fill_ < kIl2pSyncBits) {
        return std::nullopt;
    }
    const auto errors = static_cast<unsigned>(std::popcount((window_ ^ kIl2pSyncWord) & 0xFFFFFFU));
    if (errors > kIl2pSyncTolerance) {
        return std::nullopt;
    }
    return errors;
}

void Il2pReceiver::begin_packet(unsigned sync_errors) {
    state_ = State::Header;
    sync_errors_ = sync_errors;
    octets_.clear();
    partial_ = 0;
    partial_bits_ = 0;
    wanted_ = kIl2pHeaderOctets + kIl2pHeaderParity;
    window_ = 0;
    window_fill_ = 0;
}

void Il2pReceiver::complete(std::vector<Il2pPacket>& out, bool with_trailer) {
    if (with_trailer && !packet_.ax25.empty()) {
        const std::size_t at = octets_.size() - kIl2pCrcOctets;
        packet_.crc = il2p_check_crc(
            std::span<const std::uint8_t, kIl2pCrcOctets>(octets_.data() + at, kIl2pCrcOctets),
            packet_.ax25);
        if (packet_.crc == Il2pCrc::Verified) {
            packet_.last_sample = last_sample_;
        }
    }
    out.push_back(std::move(packet_));
    packet_ = {};
    state_ = State::Hunting;
}

void Il2pReceiver::push(std::uint8_t bit, SampleIndex sample, std::vector<Il2pPacket>& out) {
    bit &= 1U;
    if (state_ == State::Hunting || state_ == State::Trailer) {
        window_ = ((window_ << 1U) | bit) & 0xFFFFFFU;
        if (window_fill_ < kIl2pSyncBits) {
            ++window_fill_;
        }
        if (const auto errors = sync_match()) {
            if (state_ == State::Trailer) {
                // A packet straight after this one: no trailer was sent.
                complete(out, false);
            }
            begin_packet(*errors);
            return;
        }
        if (state_ == State::Hunting) {
            return;
        }
    }

    // "Packet Structure": most significant bit first.
    if (octets_.empty() && partial_bits_ == 0 && state_ == State::Header) {
        first_sample_ = sample;
    }
    partial_ = static_cast<std::uint8_t>((partial_ << 1U) | bit);
    last_sample_ = sample;
    if (++partial_bits_ < 8) {
        return;
    }
    octets_.push_back(partial_);
    partial_ = 0;
    partial_bits_ = 0;
    if (octets_.size() < wanted_) {
        return;
    }

    switch (state_) {
        case State::Header: {
            const auto header = codec_.decode_header(octets_);
            if (!header) {
                // Receive procedure: a false match "rejected by the receiver
                // after the header fails RS decoding".
                state_ = State::Hunting;
                return;
            }
            ++headers_;
            packet_ = {};
            packet_.header = header->header;
            packet_.header_corrected = header->corrected;
            packet_.sync_errors = sync_errors_;
            packet_.first_sample = first_sample_;
            const std::size_t count = packet_.header.payload_count;
            wanted_ += il2p_block_layout(count).air_octets(count);
            if (count == 0) {
                packet_.payload_ok = true;
                packet_.last_sample = last_sample_;
                codec_.finish(packet_);
                state_ = State::Trailer;
                wanted_ += kIl2pCrcOctets;
                return;
            }
            state_ = State::Payload;
            return;
        }
        case State::Payload: {
            const std::size_t start = kIl2pHeaderOctets + kIl2pHeaderParity;
            const auto payload = codec_.decode_payload(
                packet_.header.payload_count, std::span<const std::uint8_t>(octets_).subspan(start));
            packet_.last_sample = last_sample_;
            packet_.payload_ok = payload.has_value();
            if (payload) {
                packet_.payload = payload->payload;
                packet_.payload_corrected = payload->corrected;
            }
            codec_.finish(packet_);
            if (packet_.ax25.empty()) {
                // Nothing for a trailer to check.
                complete(out, false);
                return;
            }
            state_ = State::Trailer;
            wanted_ += kIl2pCrcOctets;
            return;
        }
        case State::Trailer:
            complete(out, true);
            return;
        case State::Hunting:
            return;
    }
}

}  // namespace revenant::decode
