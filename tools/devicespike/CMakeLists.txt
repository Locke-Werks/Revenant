# The M0.7 clean-room spike. Off by default: it is a one-off proof that the
# libusb path to the RTL-SDR works, not a component, and nothing links it.
#
# Build and run it with:
#   cmake --preset dev -DREVENANT_BUILD_DEVICE_SPIKE=ON
#   cmake --build --preset dev --target devicespike
#   ./build/dev/tools/devicespike/devicespike

find_package(PkgConfig QUIET)
find_package(libusb CONFIG QUIET)

if(NOT TARGET libusb::libusb)
    # The vcpkg port does not always export a config package, so fall back to
    # locating the library and header directly rather than failing the whole
    # configure over an optional tool.
    find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h PATH_SUFFIXES libusb-1.0)
    find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0)

    if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
        message(WARNING
            "libusb was not found, so the device spike will not be built. "
            "It is optional and nothing else depends on it.")
        return()
    endif()

    add_library(libusb_imported INTERFACE)
    target_include_directories(libusb_imported INTERFACE "${LIBUSB_INCLUDE_DIR}")
    target_link_libraries(libusb_imported INTERFACE "${LIBUSB_LIBRARY}")
    add_library(libusb::libusb ALIAS libusb_imported)
endif()

add_executable(devicespike main.cpp)
target_link_libraries(devicespike PRIVATE libusb::libusb)
target_include_directories(devicespike PRIVATE "${CMAKE_SOURCE_DIR}")
revenant_apply_warnings(devicespike)
revenant_embed_version(devicespike DESCRIPTION "Revenant device path spike")
