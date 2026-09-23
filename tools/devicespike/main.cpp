// A USB-layer diagnostic for a dongle that will not enumerate.
//
// This began as the M0.7 spike: proof that the chosen device path worked
// before M1 depended on it, written when Revenant was all rights reserved and
// strictly clean-room. The header below explains at length why it stops short
// of touching a tuner register, and that reasoning is now history rather than
// policy. Revenant is GPL-3.0-or-later and links librtlsdr, because the
// RTL2832U datasheet turned out to specify the USB transport and no raw IQ
// mode at all; docs/rtlsdr-provenance.md has the finding and
// docs/clean-room.md has the policy that replaced the absolute rule.
//
// The file survives the change because what it does is still useful and
// nothing else does it. When a dongle does not appear in revenant-cli --list,
// the question is whether the fault is above libusb or below it, and this
// answers that without librtlsdr in the way. It stays off by default.
//
// What this proves, and it is deliberately not more than this:
//
//   1. libusb is linkable and initialises.
//   2. The RTL-SDR on the desk enumerates at 0bda:2838.
//   3. The WinUSB binding that libwdi installed is one libusb can open.
//   4. The kernel driver can be detached and the interface claimed, which is
//      the step that actually fails when a device is bound to a vendor driver
//      instead.
//   5. Its descriptors, including the EEPROM-backed strings, read back.
//
// What this deliberately does NOT do is read tuner registers. The reason it
// was written that way, kept because the reasoning was sound even though the
// conclusion was overtaken:
//
// Revenant's licensing position rested on a clean-room claim: no copyleft
// source read, ported or consulted while implementing a corresponding
// component.
//
// The RTL2832U's vendor control protocol and the R820T2's register map are
// both obtainable from datasheets, and both also sit in a copyleft host
// library that anyone working here has probably seen. Writing register pokes
// from recollection would produce code that looks clean-room and is not, and a
// provenance claim that cannot be traced to a document is worth nothing
// precisely when it is challenged.
//
// So tuner access waited, on the expectation that the datasheets would carry
// it. They did not: see docs/rtlsdr-provenance.md, which is what settled the
// licence. Everything below is the USB standard, which needs no provenance:
// descriptors, configurations, interfaces and endpoints are specified in USB
// 2.0 and in libusb's own public API.
//
// Not part of the engine, not linked into anything, and off by default. Build
// it with -DREVENANT_BUILD_DEVICE_SPIKE=ON.

#include <libusb.h>

#include <cstdint>
#include <print>
#include <string>

#include "core/source/rtlsdr_lock.h"

namespace {

// Realtek's vendor id and the RTL2832U's product id, as they appear on the USB
// bus. These are not protocol knowledge: they are what the device reports in
// its own device descriptor, and Windows shows the same pair in Device Manager
// as USB\VID_0BDA&PID_2838.
constexpr std::uint16_t kRealtekVendorId = 0x0BDA;
constexpr std::uint16_t kRtl2832uProductId = 0x2838;

std::string describe_error(int code) {
    return std::string{libusb_error_name(code)} + ": " + libusb_strerror(code);
}

std::string read_string_descriptor(libusb_device_handle* handle, std::uint8_t index) {
    if (index == 0) {
        return "(none)";
    }
    unsigned char buffer[256] = {};
    const int length = libusb_get_string_descriptor_ascii(handle, index, buffer, sizeof(buffer));
    if (length < 0) {
        return "(unreadable: " + describe_error(length) + ")";
    }
    return std::string(reinterpret_cast<const char*>(buffer), static_cast<std::size_t>(length));
}

int report_interfaces(libusb_device* device) {
    libusb_config_descriptor* config = nullptr;
    const int result = libusb_get_active_config_descriptor(device, &config);
    if (result != LIBUSB_SUCCESS) {
        std::println(stderr, "could not read the active configuration: {}", describe_error(result));
        return result;
    }

    std::println("  configuration     {} interface(s)", config->bNumInterfaces);
    for (std::uint8_t i = 0; i < config->bNumInterfaces; ++i) {
        const libusb_interface& interface = config->interface[i];
        for (int alt = 0; alt < interface.num_altsetting; ++alt) {
            const libusb_interface_descriptor& descriptor = interface.altsetting[alt];
            std::println("  interface {}       class 0x{:02X}, {} endpoint(s)",
                         descriptor.bInterfaceNumber, descriptor.bInterfaceClass,
                         descriptor.bNumEndpoints);
            for (std::uint8_t e = 0; e < descriptor.bNumEndpoints; ++e) {
                const libusb_endpoint_descriptor& endpoint = descriptor.endpoint[e];
                const bool inbound = (endpoint.bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0;
                const auto transfer =
                    static_cast<int>(endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK);
                const char* kind = transfer == LIBUSB_TRANSFER_TYPE_BULK      ? "bulk"
                                   : transfer == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS ? "isochronous"
                                   : transfer == LIBUSB_TRANSFER_TYPE_INTERRUPT   ? "interrupt"
                                                                                  : "control";
                // The bulk IN endpoint is the one that will carry IQ at M1, so
                // its presence and its max packet size are the two facts worth
                // printing here.
                std::println("    endpoint 0x{:02X}  {} {}, max packet {}",
                             endpoint.bEndpointAddress, kind, inbound ? "IN" : "OUT",
                             endpoint.wMaxPacketSize);
            }
        }
    }

    libusb_free_config_descriptor(config);
    return LIBUSB_SUCCESS;
}

}  // namespace

int main() {
    libusb_context* context = nullptr;
    int result = libusb_init(&context);
    if (result != LIBUSB_SUCCESS) {
        std::println(stderr, "libusb_init failed: {}", describe_error(result));
        return 1;
    }

    std::println("libusb initialised");

    libusb_device** devices = nullptr;
    const ssize_t count = libusb_get_device_list(context, &devices);
    if (count < 0) {
        std::println(stderr, "libusb_get_device_list failed: {}",
                     describe_error(static_cast<int>(count)));
        libusb_exit(context);
        return 1;
    }

    libusb_device* target = nullptr;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[i], &descriptor) != LIBUSB_SUCCESS) {
            continue;
        }
        if (descriptor.idVendor == kRealtekVendorId &&
            descriptor.idProduct == kRtl2832uProductId) {
            target = devices[i];
            break;
        }
    }

    if (target == nullptr) {
        std::println(stderr,
                     "no device matching {:04x}:{:04x} is attached.\n"
                     "Plug in the RTL-SDR, and check it is bound to WinUSB rather than the "
                     "DVB-T driver.",
                     kRealtekVendorId, kRtl2832uProductId);
        libusb_free_device_list(devices, 1);
        libusb_exit(context);
        return 1;
    }

    std::println("found {:04x}:{:04x} on bus {} address {}", kRealtekVendorId, kRtl2832uProductId,
                 libusb_get_bus_number(target), libusb_get_device_address(target));

    // The same machine-wide lock the engine takes, held until the end of main,
    // so this never claims the interface out from under a stream or a test.
    // Taken after the descriptor scan because nothing above opens the device,
    // and a diagnostic that refused to list anything while a stream ran would
    // be refusing exactly when somebody wants to know what is attached.
    auto lock = revenant::source::lock_for_open(revenant::source::rtlsdr_lock_policy());
    if (!lock) {
        std::println(stderr, "{}", lock.error().message);
        libusb_free_device_list(devices, 1);
        libusb_exit(context);
        return 1;
    }

    libusb_device_handle* handle = nullptr;
    result = libusb_open(target, &handle);
    if (result != LIBUSB_SUCCESS) {
        std::println(stderr,
                     "libusb_open failed: {}\n"
                     "On Windows this is what a missing or wrong driver binding looks like. The "
                     "device needs WinUSB, which Zadig installs.",
                     describe_error(result));
        libusb_free_device_list(devices, 1);
        libusb_exit(context);
        return 1;
    }

    libusb_device_descriptor descriptor{};
    libusb_get_device_descriptor(target, &descriptor);

    std::println("  manufacturer      {}", read_string_descriptor(handle, descriptor.iManufacturer));
    std::println("  product           {}", read_string_descriptor(handle, descriptor.iProduct));
    std::println("  serial            {}", read_string_descriptor(handle, descriptor.iSerialNumber));
    std::println("  USB version       {:x}.{:02x}", descriptor.bcdUSB >> 8,
                 descriptor.bcdUSB & 0xFF);

    report_interfaces(target);

    // The claim is the real test. Enumeration and descriptor reads work
    // through the hub even when another driver owns the device; claiming the
    // interface is what fails if the binding is wrong, and it is what the M1
    // source backend will do on every open.
    libusb_set_auto_detach_kernel_driver(handle, 1);
    result = libusb_claim_interface(handle, 0);
    if (result != LIBUSB_SUCCESS) {
        std::println(stderr,
                     "libusb_claim_interface(0) failed: {}\n"
                     "Something else holds the device. Close any other SDR application.",
                     describe_error(result));
        libusb_close(handle);
        libusb_free_device_list(devices, 1);
        libusb_exit(context);
        return 1;
    }

    std::println("  interface 0       claimed and released cleanly");

    libusb_release_interface(handle, 0);
    libusb_close(handle);
    libusb_free_device_list(devices, 1);
    libusb_exit(context);

    std::println("");
    std::println("The libusb path to this device works, so a dongle that revenant-cli cannot");
    std::println("open has a fault above this layer rather than below it. Tuning and sample");
    std::println("delivery go through librtlsdr; the datasheets turned out not to carry the");
    std::println("IQ mode. See docs/rtlsdr-provenance.md and docs/clean-room.md.");
    return 0;
}
