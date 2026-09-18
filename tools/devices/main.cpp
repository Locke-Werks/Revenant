// Lists the Vulkan devices this machine exposes, with the index each one
// answers to.
//
// REVENANT_GPU_INDEX selects a device throughout the project: the test suite
// uses it, the CI matrix binds a leg to it, and the engine will read it too.
// An index is useless without a way to find out what it points at, and
// "run the suite and read the banner" is a poor answer when the question is
// which device to aim the suite at in the first place.
//
// Deliberately trivial, and deliberately not part of the engine. It opens no
// device and creates no logical device: enumeration alone is enough and it
// keeps this runnable on a machine where creation would fail, which is exactly
// when somebody is trying to find out why.

#include <cstdlib>
#include <print>
#include <string_view>

#include "core/gpu/context.h"

namespace {

constexpr std::string_view kUsage = R"(revenant-devices: list the Vulkan devices on this machine

usage:
  revenant-devices [--verbose]

Each line begins with the index REVENANT_GPU_INDEX accepts.

exit codes:
  0  at least one device was found
  1  no device, or Vulkan could not be reached
  2  bad usage
)";

}  // namespace

int main(int argc, char** argv) {
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            std::print("{}", kUsage);
            return 0;
        } else {
            std::println(stderr, "unknown argument: {}", arg);
            std::print(stderr, "{}", kUsage);
            return 2;
        }
    }

    const auto devices = revenant::gpu::enumerate_devices();
    if (!devices) {
        std::println(stderr, "could not enumerate Vulkan devices: {}", devices.error().message);
        std::println(stderr,
                     "A driver may be missing, or the Vulkan loader may not be installed.");
        return 1;
    }

    if (devices->empty()) {
        std::println(stderr, "no Vulkan devices are present");
        return 1;
    }

    for (const auto& device : *devices) {
        std::println("{}", device.describe());
        if (verbose) {
            std::println("      vendor 0x{:04X}  device 0x{:04X}  driver {}", device.vendor_id,
                         device.device_id, device.driver_version);
            std::println("      max workgroup x {}  max invocations {}  shared memory {} KiB",
                         device.max_workgroup_size_x, device.max_workgroup_invocations,
                         device.max_workgroup_shared_memory / 1024);
            // Printed because it differs by a factor of 512 between the two
            // devices here, and because it is what bounds how much capture the
            // device ring can hold.
            std::println("      max allocation {} MiB  max storage buffer {} MiB",
                         device.max_memory_allocation_size / (1024 * 1024),
                         device.max_storage_buffer_range / (1024 * 1024));
        }
    }

    if (const char* selected = std::getenv("REVENANT_GPU_INDEX");
        selected != nullptr && *selected != '\0') {
        std::println("");
        std::println("REVENANT_GPU_INDEX is set to {}", selected);
    }

    return 0;
}
