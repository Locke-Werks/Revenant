#include "core/dsp/synth/b2f_encode.h"

#include <algorithm>

namespace revenant::dsp::synth::b2f {

namespace {

void put(std::vector<std::uint8_t>& out, std::string_view s) {
    out.insert(out.end(), s.begin(), s.end());
}

void field(std::vector<std::uint8_t>& out, std::string_view key, std::string_view value) {
    put(out, key);
    put(out, ": ");
    put(out, value);
    put(out, "\r\n");
}

}  // namespace

std::vector<std::uint8_t> message_file(const OutMessage& m) {
    std::vector<std::uint8_t> out;
    field(out, "Mid", m.mid);
    field(out, "Date", m.date);
    field(out, "Type", m.type);
    field(out, "From", m.from);
    for (const auto& t : m.to) {
        field(out, "To", t);
    }
    for (const auto& c : m.cc) {
        field(out, "Cc", c);
    }
    field(out, "Subject", m.subject);
    field(out, "Mbo", m.mbo);
    field(out, "Body", std::to_string(m.body.size()));
    for (const auto& [name, data] : m.files) {
        field(out, "File", std::to_string(data.size()) + " " + name);
    }
    put(out, "\r\n");
    put(out, m.body);
    put(out, "\r\n");
    for (const auto& [name, data] : m.files) {
        out.insert(out.end(), data.begin(), data.end());
        put(out, "\r\n");
    }
    return out;
}

std::string fc_line(std::string_view type, std::string_view mid, std::uint32_t usize,
                    std::uint32_t csize) {
    return "FC " + std::string(type) + " " + std::string(mid) + " " + std::to_string(usize) + " " +
           std::to_string(csize) + "\r";
}

std::vector<std::uint8_t> with_prefix(std::uint16_t crc16, std::uint32_t uncompressed_size,
                                      std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> out = {
        static_cast<std::uint8_t>(crc16 & 0xFF),
        static_cast<std::uint8_t>(crc16 >> 8),
        static_cast<std::uint8_t>(uncompressed_size & 0xFF),
        static_cast<std::uint8_t>((uncompressed_size >> 8) & 0xFF),
        static_cast<std::uint8_t>((uncompressed_size >> 16) & 0xFF),
        static_cast<std::uint8_t>(uncompressed_size >> 24),
    };
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> item(std::string_view title, std::uint32_t offset,
                               std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> out;
    const std::string off = std::to_string(offset);
    out.push_back(0x01);
    out.push_back(static_cast<std::uint8_t>(title.size() + off.size() + 2));
    put(out, title);
    out.push_back(0x00);
    put(out, off);
    out.push_back(0x00);
    std::uint8_t sum = 0;
    for (std::size_t i = 0; i < data.size(); i += 256) {
        const std::size_t n = std::min<std::size_t>(256, data.size() - i);
        out.push_back(0x02);
        out.push_back(static_cast<std::uint8_t>(n & 0xFF));
        for (std::size_t k = 0; k < n; ++k) {
            out.push_back(data[i + k]);
            sum = static_cast<std::uint8_t>(sum + data[i + k]);
        }
    }
    out.push_back(0x04);
    out.push_back(static_cast<std::uint8_t>(-sum));
    return out;
}

}  // namespace revenant::dsp::synth::b2f
