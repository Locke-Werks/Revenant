// Lists the attached RTL-SDRs by serial, and rewrites one dongle's serial.
//
// Two dongles from the same batch usually both say 00000001, and an index is
// a position in libusb's enumeration that moves when anything is replugged.
// A distinct serial is the only stable name a dongle can carry, and setting
// one is a write to its EEPROM, so this does the minimum: it changes the
// serial descriptor and nothing else, and refuses any image it does not
// fully understand. core/source/rtl_eeprom.h holds the layout rules and is
// what the tests exercise.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include <rtl-sdr.h>

#include "core/source/rtl_eeprom.h"
#include "core/source/rtlsdr_lock.h"

namespace {

namespace eeprom = revenant::source::rtl_eeprom;

constexpr std::string_view kUsage = R"(revenant-rtl-serial: list RTL-SDRs, or set one's serial number

usage:
  revenant-rtl-serial list
  revenant-rtl-serial set <index> <new-serial> [--dry-run]

list prints one line per dongle: index, serial and product, tab-separated.

set rewrites the serial in the dongle's EEPROM. The new serial is 1 to 32
ASCII letters, digits, '-' or '_'. --dry-run shows the bytes that would
change and writes nothing. Unplug and replug the dongle afterwards.

exit codes:
  0  success
  1  the device could not be opened, read or written, or its EEPROM was refused
  2  bad usage
)";

int usage_error(std::string_view message)
{
    std::println(stderr, "{}", message);
    std::print(stderr, "{}", kUsage);
    return 2;
}

int list()
{
    const std::uint32_t count = rtlsdr_get_device_count();
    if (count == 0) {
        std::println(stderr, "no RTL-SDR is attached");
        return 0;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        char manufacturer[256]{};
        char product[256]{};
        char serial[256]{};
        if (rtlsdr_get_device_usb_strings(i, manufacturer, product, serial) != 0) {
            std::println("{}\t?\t{}", i, rtlsdr_get_device_name(i));
            continue;
        }
        std::println("{}\t{}\t{}", i, serial, product);
    }
    return 0;
}

void hex_dump(const eeprom::Image& before, const eeprom::Image& after)
{
    std::size_t first = eeprom::kSize;
    std::size_t last = 0;
    for (std::size_t i = 0; i < eeprom::kSize; ++i) {
        if (before[i] != after[i]) {
            first = std::min(first, i);
            last = i;
        }
    }
    if (first == eeprom::kSize) {
        std::println("no bytes change");
        return;
    }
    // Whole 16-byte rows, so the offsets line up with any other EEPROM dump.
    for (std::size_t row = first & ~std::size_t{15}; row <= last; row += 16) {
        std::string was = std::format("  {:02X}  was", row);
        std::string now = std::format("  {:02X}  now", row);
        for (std::size_t i = row; i < row + 16; ++i) {
            was += std::format(" {:02X}", before[i]);
            now += std::format("{}{:02X}", before[i] == after[i] ? ' ' : '*', after[i]);
        }
        std::println("{}", was);
        std::println("{}", now);
    }
}

struct Closer {
    rtlsdr_dev_t* dev = nullptr;
    ~Closer()
    {
        if (dev != nullptr) {
            rtlsdr_close(dev);
        }
    }
};

int set(std::uint32_t index, std::string_view serial, bool dry_run)
{
    if (!eeprom::valid_serial(serial)) {
        return usage_error("the new serial must be 1 to 32 ASCII letters, digits, '-' or '_'");
    }

    // For the whole operation, read through read-back, so an engine cannot
    // open the dongle half way through a write.
    auto lock = revenant::source::lock_for_open(revenant::source::rtlsdr_lock_policy());
    if (!lock) {
        std::println(stderr, "{}", lock.error().message);
        return 1;
    }

    Closer device;
    if (const int rc = rtlsdr_open(&device.dev, index); rc != 0 || device.dev == nullptr) {
        device.dev = nullptr;
        std::println(stderr,
                     "could not open the RTL-SDR at index {} (rtlsdr_open returned {}). Another "
                     "program is probably using it; close it and try again.",
                     index, rc);
        return 1;
    }

    eeprom::Image original{};
    if (const int rc = rtlsdr_read_eeprom(device.dev, original.data(), 0, eeprom::kSize); rc < 0) {
        std::println(stderr, "could not read the EEPROM (rtlsdr_read_eeprom returned {}).", rc);
        return 1;
    }

    const auto parsed = eeprom::parse(original);
    if (!parsed) {
        std::println(stderr, "refusing: {}", parsed.error());
        return 1;
    }
    const auto rebuilt = eeprom::with_serial(original, serial);
    if (!rebuilt) {
        std::println(stderr, "refusing: {}", rebuilt.error());
        return 1;
    }

    std::println("device {}: {:04x}:{:04x} {} {}", index, parsed->vid, parsed->pid,
                 parsed->manufacturer.text, parsed->product.text);
    std::println("serial before: {}", parsed->serial.text);
    std::println("serial after:  {}", serial);

    if (dry_run) {
        hex_dump(original, *rebuilt);
        std::println("dry run: nothing was written");
        return 0;
    }

    // rtlsdr_write_eeprom skips bytes that already match, so rewriting the
    // whole image costs only the bytes that change.
    eeprom::Image to_write = *rebuilt;
    if (const int rc = rtlsdr_write_eeprom(device.dev, to_write.data(), 0, eeprom::kSize);
        rc < 0) {
        std::println(stderr,
                     "WRITE FAILED (rtlsdr_write_eeprom returned {}). The EEPROM may be partly "
                     "written. Run this again before unplugging the dongle.",
                     rc);
        return 1;
    }

    eeprom::Image readback{};
    if (const int rc = rtlsdr_read_eeprom(device.dev, readback.data(), 0, eeprom::kSize); rc < 0) {
        std::println(stderr,
                     "THE WRITE COULD NOT BE VERIFIED: reading it back failed (rtlsdr_read_eeprom "
                     "returned {}). Do not unplug the dongle; run this again.",
                     rc);
        return 1;
    }
    if (readback != *rebuilt) {
        std::println(stderr, "VERIFY FAILED: the EEPROM does not hold what was written.");
        hex_dump(*rebuilt, readback);
        std::println(stderr, "Do not unplug the dongle; run this again.");
        return 1;
    }

    std::println("written and verified. Unplug the dongle and plug it back in for the new "
                 "serial to show.");
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    std::vector<std::string_view> args;
    bool dry_run = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            std::print("{}", kUsage);
            return 0;
        }
        if (arg == "--dry-run") {
            dry_run = true;
        } else {
            args.push_back(arg);
        }
    }

    if (args.empty()) {
        return usage_error("no command given");
    }
    if (args[0] == "list") {
        if (args.size() != 1 || dry_run) {
            return usage_error("list takes no arguments");
        }
        return list();
    }
    if (args[0] == "set") {
        if (args.size() != 3) {
            return usage_error("set takes an index and a new serial");
        }
        std::uint32_t index = 0;
        const auto text = args[1];
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), index);
        if (ec != std::errc{} || end != text.data() + text.size()) {
            return usage_error("the index must be a non-negative integer");
        }
        return set(index, args[2], dry_run);
    }
    return usage_error(std::format("unknown command: {}", args[0]));
}
