// The RTL-SDR's 256-byte configuration EEPROM, parsed and rebuilt with a new
// serial number.
//
// Kept free of librtlsdr so the layout rules can be tested without a dongle:
// tools/rtl-serial does the reading and writing, this does the arithmetic, and
// the arithmetic is where a mistake bricks the USB descriptors.
//
// The layout is librtlsdr's, the one rtl_eeprom writes: 0x28 0x32 as a
// signature, VID and PID little-endian, three config bytes, then from offset 9
// the manufacturer, product and serial as USB string descriptors, each
// [length][0x03][UTF-16LE], length counting its own two header bytes.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace revenant::source::rtl_eeprom {

inline constexpr std::size_t kSize = 256;
inline constexpr std::size_t kStringsOffset = 9;
inline constexpr std::size_t kMaxSerial = 32;

using Image = std::array<std::uint8_t, kSize>;

struct Descriptor {
    std::size_t offset = 0;  // of the length byte
    std::size_t length = 0;  // header included
    std::string text;        // ASCII where the descriptor is; '?' elsewhere
};

struct Parsed {
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    Descriptor manufacturer;
    Descriptor product;
    Descriptor serial;

    // One past the serial descriptor: where the strings block ends.
    [[nodiscard]] std::size_t strings_end() const { return serial.offset + serial.length; }
};

// What a serial may contain. Restricted to what every tool that matches on a
// serial, rtl_tcp's -d among them, passes through a shell unquoted.
[[nodiscard]] inline bool valid_serial(std::string_view serial)
{
    if (serial.empty() || serial.size() > kMaxSerial) {
        return false;
    }
    for (const char c : serial) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

namespace detail {

[[nodiscard]] inline std::expected<Descriptor, std::string> parse_descriptor(
    const Image& image, std::size_t offset, std::string_view what)
{
    if (offset + 2 > kSize) {
        return std::unexpected(std::string(what) + " descriptor starts past the end of the EEPROM");
    }
    const std::size_t length = image[offset];
    if (length < 2 || length % 2 != 0) {
        return std::unexpected(std::string(what) + " descriptor has length " + std::to_string(length) +
                               ", which is not an even number of at least 2");
    }
    if (image[offset + 1] != 0x03) {
        return std::unexpected(std::string(what) + " descriptor is not type 0x03 (string)");
    }
    if (offset + length > kSize) {
        return std::unexpected(std::string(what) + " descriptor runs past the end of the EEPROM");
    }
    Descriptor d{offset, length, {}};
    for (std::size_t i = offset + 2; i < offset + length; i += 2) {
        const unsigned unit = image[i] | (image[i + 1] << 8);
        d.text.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
    }
    return d;
}

}  // namespace detail

// Refuses anything that is not exactly the layout above. A refusal here is
// the whole safety story: nothing is rebuilt from an image this rejects.
[[nodiscard]] inline std::expected<Parsed, std::string> parse(const Image& image)
{
    if (image[0] != 0x28 || image[1] != 0x32) {
        return std::unexpected("the EEPROM does not begin with 0x28 0x32, so it is not the layout "
                               "librtlsdr writes; refusing to touch it");
    }
    Parsed p;
    p.vid = static_cast<std::uint16_t>(image[2] | (image[3] << 8));
    p.pid = static_cast<std::uint16_t>(image[4] | (image[5] << 8));

    auto manufacturer = detail::parse_descriptor(image, kStringsOffset, "manufacturer");
    if (!manufacturer) {
        return std::unexpected(manufacturer.error());
    }
    p.manufacturer = std::move(*manufacturer);

    auto product = detail::parse_descriptor(image, p.manufacturer.offset + p.manufacturer.length,
                                            "product");
    if (!product) {
        return std::unexpected(product.error());
    }
    p.product = std::move(*product);

    auto serial = detail::parse_descriptor(image, p.product.offset + p.product.length, "serial");
    if (!serial) {
        return std::unexpected(serial.error());
    }
    p.serial = std::move(*serial);
    return p;
}

// The original image with the serial descriptor replaced. The first nine
// bytes and the other two descriptors are copied byte for byte. If the new
// block is shorter, the bytes it no longer covers up to the old end become
// 0xFF, so no tail of the old serial survives to be misread as a descriptor.
// Bytes beyond the old end are left as they were.
[[nodiscard]] inline std::expected<Image, std::string> with_serial(const Image& image,
                                                                   std::string_view serial)
{
    if (!valid_serial(serial)) {
        return std::unexpected("the new serial must be 1 to 32 ASCII letters, digits, '-' or '_'");
    }
    auto parsed = parse(image);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const std::size_t serial_offset = parsed->serial.offset;
    const std::size_t new_length = 2 + 2 * serial.size();
    const std::size_t new_end = serial_offset + new_length;
    if (new_end > kSize) {
        return std::unexpected("the three string descriptors with the new serial take " +
                               std::to_string(new_end) + " bytes, more than the EEPROM's 256");
    }

    Image out = image;
    out[serial_offset] = static_cast<std::uint8_t>(new_length);
    out[serial_offset + 1] = 0x03;
    for (std::size_t i = 0; i < serial.size(); ++i) {
        out[serial_offset + 2 + 2 * i] = static_cast<std::uint8_t>(serial[i]);
        out[serial_offset + 3 + 2 * i] = 0x00;
    }
    for (std::size_t i = new_end; i < parsed->strings_end(); ++i) {
        out[i] = 0xFF;
    }
    return out;
}

}  // namespace revenant::source::rtl_eeprom
