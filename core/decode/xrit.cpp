#include "core/decode/xrit.h"

namespace revenant::decode {

namespace {

std::uint64_t be(std::span<const std::uint8_t> b, std::size_t off, std::size_t n) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        v = (v << 8U) | b[off + i];
    }
    return v;
}

}  // namespace

const XritHeaderRecord* XritFile::find(std::uint8_t type) const {
    for (const auto& r : records) {
        if (r.type == type) {
            return &r;
        }
    }
    return nullptr;
}

std::optional<XritImageStructure> XritFile::image_structure() const {
    const auto* r = find(1);
    if (r == nullptr || r->data.size() < 6) {
        return std::nullopt;
    }
    XritImageStructure s;
    s.bits_per_pixel = r->data[0];
    s.columns = static_cast<std::uint16_t>(be(r->data, 1, 2));
    s.lines = static_cast<std::uint16_t>(be(r->data, 3, 2));
    s.compression = r->data[5];
    return s;
}

std::string XritFile::annotation() const {
    const auto* r = find(4);
    if (r == nullptr) {
        return {};
    }
    std::string s(r->data.begin(), r->data.end());
    while (!s.empty() && s.back() == '\0') {
        s.pop_back();
    }
    return s;
}

bool XritFile::encrypted() const {
    const auto* r = find(7);
    if (r == nullptr) {
        return false;
    }
    // CGMS 4.2.2.7 leaves the content mission specific; a record that is
    // all zero (GK-2A table 4.9: key number 0, "encryption is not applied")
    // is read as clear.
    for (const auto b : r->data) {
        if (b != 0U) {
            return true;
        }
    }
    return false;
}

std::optional<XritFile> parse_xrit_file(std::span<const std::uint8_t> b) {
    // CGMS table 4-1: type 0, length 16.
    if (b.size() < 16 || b[0] != 0 || be(b, 1, 2) != 16) {
        return std::nullopt;
    }
    XritFile f;
    f.file_type = b[3];
    f.total_header_octets = static_cast<std::uint32_t>(be(b, 4, 4));
    f.data_field_bits = be(b, 8, 8);
    if (f.total_header_octets < 16 || f.total_header_octets > b.size()) {
        return std::nullopt;
    }
    std::size_t off = 0;
    while (off + 3 <= f.total_header_octets) {
        const std::size_t len = static_cast<std::size_t>(be(b, off + 1, 2));
        if (len < 3 || off + len > f.total_header_octets) {
            return std::nullopt;
        }
        XritHeaderRecord r;
        r.type = b[off];
        r.data.assign(b.begin() + static_cast<std::ptrdiff_t>(off + 3), b.begin() + static_cast<std::ptrdiff_t>(off + len));
        f.records.push_back(std::move(r));
        off += len;
    }
    const std::size_t data_octets = static_cast<std::size_t>((f.data_field_bits + 7) / 8);
    if (f.total_header_octets + data_octets > b.size()) {
        return std::nullopt;
    }
    f.data.assign(b.begin() + f.total_header_octets,
                  b.begin() + static_cast<std::ptrdiff_t>(f.total_header_octets + data_octets));
    return f;
}

void XritTransport::push(std::uint8_t vc, const SpacePacket& packet, std::vector<XritFile>& out) {
    const auto& h = packet.header;
    const std::span<const std::uint8_t> field = std::span(packet.octets).subspan(kSpacePacketHeaderOctets);
    if (field.size() < 3) {
        return;
    }
    const auto user = field.first(field.size() - 2);
    const auto crc = static_cast<std::uint16_t>(be(field, field.size() - 2, 2));
    const bool ok = ccsds_fecf(user) == crc;
    if (!ok) {
        ++crc_failures_;
    }
    const auto it = open_.find(h.apid);
    switch (h.sequence_flags) {
        case 3:
        case 1: {
            if (it != open_.end()) {
                ++dropped_;  // a new file began before the last ended
                open_.erase(it);
            }
            Partial p;
            p.bytes.assign(user.begin(), user.end());
            p.next_count = static_cast<std::uint16_t>((h.sequence_count + 1U) & 0x3FFFU);
            p.crc_ok = ok;
            p.vc = vc;
            if (h.sequence_flags == 3) {
                complete(h.apid, p, out);
            } else {
                open_[h.apid] = std::move(p);
            }
            return;
        }
        default: {
            if (it == open_.end()) {
                return;  // joined mid-file
            }
            Partial& p = it->second;
            if (h.sequence_count != p.next_count) {
                ++dropped_;
                open_.erase(it);
                return;
            }
            p.bytes.insert(p.bytes.end(), user.begin(), user.end());
            p.next_count = static_cast<std::uint16_t>((h.sequence_count + 1U) & 0x3FFFU);
            p.crc_ok = p.crc_ok && ok;
            if (h.sequence_flags == 2) {
                Partial done = std::move(p);
                open_.erase(it);
                complete(h.apid, done, out);
            }
            return;
        }
    }
}

void XritTransport::complete(std::uint16_t apid, Partial& p, std::vector<XritFile>& out) {
    if (p.bytes.size() < kXritTransportHeaderOctets) {
        ++dropped_;
        return;
    }
    const auto counter = static_cast<std::uint16_t>(be(p.bytes, 0, 2));
    const std::uint64_t bits = be(p.bytes, 2, 8);
    const std::size_t octets = static_cast<std::size_t>((bits + 7) / 8);
    if (kXritTransportHeaderOctets + octets > p.bytes.size()) {
        ++dropped_;
        return;
    }
    auto f = parse_xrit_file(std::span(p.bytes).subspan(kXritTransportHeaderOctets, octets));
    if (!f) {
        ++dropped_;
        return;
    }
    f->virtual_channel = p.vc;
    f->apid = apid;
    f->file_counter = counter;
    f->crc_ok = p.crc_ok;
    ++files_;
    out.push_back(std::move(*f));
}

void XritTransport::reset() {
    open_.clear();
    files_ = 0;
    crc_failures_ = 0;
    dropped_ = 0;
}

}  // namespace revenant::decode
