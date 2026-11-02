// LRIT/HRIT ("xRIT") files from Space Packets: the transport layer that
// reassembles them and the header records that describe them, shared by the
// GK-2A LRIT and GOES HRIT decoders.
//
// SPECIFICATION
//
// CGMS LRIT/HRIT Global Specification, issue 2.8, 30 October 2013,
// CGMS/DOC/12/0017, fetched 2026-11-02 from www.cgms-info.org/documents/
// cgms-lrit-hrit-global-specification-(v2-8-of-30-oct-2013).pdf. Cited as
// "CGMS".
//
//   - 6.2.2 and figure 6-1: the transport file is a 16-bit file counter, a
//     64-bit TP_SDU length in bits, the TP_SDU and 0 to 7 fill bits; it is
//     cut into blocks of 8190 octets, each followed by a CRC-16 of
//     g(x) = x^16 + x^12 + x^5 + 1 preset to all ones, and each block is the
//     user data of one source packet. 6.3.2: reception concatenates the user
//     data less its last two octets under the sequence flags, per APID.
//   - 7.1 figure 7-1: sequence flags 3 one whole file, 1 first, 0
//     continuation, 2 last.
//   - 4.2.1 tables 4-1 to 4-3 and figure 4-2: the header records, each a
//     type octet, a 16-bit record length counting the whole record, and
//     data; the primary header (type 0, length 16) giving the file type, the
//     total header length in octets and the data field length in bits.
//   - 4.2.2: image structure (type 1: bits per pixel, columns, lines,
//     compression flag), annotation (4), time stamp (5), ancillary text (6),
//     key header (7). 4.2.3.1: an image's pixels are MSB first, line by line.
//   - 5.6.2: a file carrying a key header is encrypted. Nothing here
//     decrypts; such a file is delivered with `encrypted` set.
//
// The CRC is the same polynomial and preset as the CCSDS FECF, so
// core/decode/ccsds.h's ccsds_fecf computes it.
//
// CLEAN ROOM
//
// Written from CGMS. No xRIT implementation was read: not goestools, xrit-rx,
// SatDump, or any other.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/decode/ccsds.h"

namespace revenant::decode {

// CGMS 6.2.2.
inline constexpr std::size_t kXritBlockOctets = 8190;
inline constexpr std::size_t kXritTransportHeaderOctets = 10;

struct XritHeaderRecord {
    std::uint8_t type = 0;
    // The record after its three-octet type and length.
    std::vector<std::uint8_t> data;
};

struct XritImageStructure {
    std::uint8_t bits_per_pixel = 0;
    std::uint16_t columns = 0;
    std::uint16_t lines = 0;
    // 0 none, 1 lossless, 2 lossy (CGMS table 4-4, 5.3).
    std::uint8_t compression = 0;
};

struct XritFile {
    std::uint8_t virtual_channel = 0;
    std::uint16_t apid = 0;
    std::uint16_t file_counter = 0;
    // Primary header, CGMS table 4-1.
    std::uint8_t file_type = 0;
    std::uint32_t total_header_octets = 0;
    std::uint64_t data_field_bits = 0;
    std::vector<XritHeaderRecord> records;
    std::vector<std::uint8_t> data;
    // Every segment's CRC matched.
    bool crc_ok = true;

    [[nodiscard]] const XritHeaderRecord* find(std::uint8_t type) const;
    [[nodiscard]] std::optional<XritImageStructure> image_structure() const;
    // Header type 4, without trailing NULs.
    [[nodiscard]] std::string annotation() const;
    // A key header (type 7) is present and, where its data is a key number,
    // the number is not zero.
    [[nodiscard]] bool encrypted() const;
};

// Parses an S_PDU: header records, then the data field. Nothing when the
// primary header is missing or its lengths overrun the bytes.
[[nodiscard]] std::optional<XritFile> parse_xrit_file(std::span<const std::uint8_t> s_pdu);

// Reassembles transport files per APID and parses them.
class XritTransport {
public:
    void push(std::uint8_t virtual_channel, const SpacePacket& packet, std::vector<XritFile>& out);
    void reset();

    [[nodiscard]] std::size_t files() const { return files_; }
    [[nodiscard]] std::size_t crc_failures() const { return crc_failures_; }
    // Files abandoned on a sequence gap or a malformed transport header.
    [[nodiscard]] std::size_t dropped() const { return dropped_; }

private:
    struct Partial {
        std::vector<std::uint8_t> bytes;
        std::uint16_t next_count = 0;
        bool crc_ok = true;
        std::uint8_t vc = 0;
    };
    void complete(std::uint16_t apid, Partial& p, std::vector<XritFile>& out);
    std::map<std::uint16_t, Partial> open_;
    std::size_t files_ = 0;
    std::size_t crc_failures_ = 0;
    std::size_t dropped_ = 0;
};

}  // namespace revenant::decode
