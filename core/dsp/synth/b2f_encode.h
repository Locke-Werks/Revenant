// Builds the B2F side of a Winlink exchange for tests: message files, FC
// proposals, FS answers and FBB binary items. Same documents as
// core/decode/b2f.h. Compression is not done here, for the reason that
// header gives; the payload is passed in already in its transmitted form.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace revenant::dsp::synth::b2f {

struct OutMessage {
    std::string mid;
    std::string date;
    std::string type = "Private";
    std::string from;
    std::vector<std::string> to;
    std::vector<std::string> cc;
    std::string subject;
    std::string mbo;
    std::string body;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files;
};

// B2F Message Structure: header, blank line, body, CR LF, attachments each
// followed by CR LF.
std::vector<std::uint8_t> message_file(const OutMessage& m);

// "FC EM mid usize csize\r"
std::string fc_line(std::string_view type, std::string_view mid, std::uint32_t usize,
                    std::uint32_t csize);

// The Version 1 six-byte prefix, CRC16 supplied by the caller.
std::vector<std::uint8_t> with_prefix(std::uint16_t crc16, std::uint32_t uncompressed_size,
                                      std::span<const std::uint8_t> payload);

// SOH header, STX blocks of up to 256 bytes, EOT and checksum.
std::vector<std::uint8_t> item(std::string_view title, std::uint32_t offset,
                               std::span<const std::uint8_t> data);

}  // namespace revenant::dsp::synth::b2f
