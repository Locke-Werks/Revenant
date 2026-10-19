# Revenant's hackrf port. The registry has none at the baseline or upstream as
# of 2026-10-18, so this overlay is the recipe. docs/hackrf-provenance.md says
# what is built and under which licence.
#
# Only host/libhackrf/src/hackrf.c and hackrf.h are compiled, both
# BSD-3-Clause, by the CMakeLists.txt beside this file in place of upstream's.
# No firmware, no tools, and nothing from the GPL-2.0 build files.
#
# Pinned by commit rather than by tag: 1cfe7df is the v2026.01.3 release,
# greatscottgadgets/hackrf's latest on 2026-10-18, and a tag can be moved
# where a commit cannot.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO greatscottgadgets/hackrf
    REF 1cfe7dfe98d333450217d50e3f3a1ad0702e000f
    SHA512 b981eae0790f81c43bdedb178a1e2ed40e04152febfc6f1c7a0c5ea539fd3fcba059dfcc5ebec847899318d2d61f247f2c9687ac28dd63e5b0e5384dc4f6ea30
    HEAD_REF master
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" "${CMAKE_CURRENT_LIST_DIR}/hackrf-config.cmake"
     DESTINATION "${SOURCE_PATH}")

# hackrf.h defines ADD_EXPORTS unconditionally on Windows, so every build and
# every consumer sees dllexport. Static, that would put libhackrf's whole API
# in the export table of the executable that links it.
if(VCPKG_TARGET_IS_WINDOWS AND VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    vcpkg_replace_string("${SOURCE_PATH}/host/libhackrf/src/hackrf.h"
        "#define ADDAPI __declspec(dllexport)" "#define ADDAPI")
    vcpkg_replace_string("${SOURCE_PATH}/host/libhackrf/src/hackrf.h"
        "#define ADDAPI __declspec(dllimport)" "#define ADDAPI")
endif()

vcpkg_find_acquire_program(PKGCONFIG)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        "-DPKG_CONFIG_EXECUTABLE=${PKGCONFIG}"
        "-DHACKRF_RELEASE=v2026.01.3"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH share/hackrf)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

# The BSD-3-Clause notice is the head of hackrf.h; the repository's COPYING is
# the GPL-2.0 that covers the firmware and build files this port does not use.
file(READ "${SOURCE_PATH}/host/libhackrf/src/hackrf.h" header)
string(FIND "${header}" "*/" notice_end)
string(SUBSTRING "${header}" 0 ${notice_end} notice)
file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/copyright" "${notice}*/\n")
