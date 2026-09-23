# Revenant's overlay of vcpkg's rtlsdr port, which vcpkg-configuration.json
# puts ahead of the registry. docs/rtlsdr-provenance.md, "The librtlsdr the
# engine links", says why it exists and what the trial measured.
#
# The rest of this file is the registry's port at the baseline, unchanged
# except for the source and the fourth patch. dependencies.diff is rebased onto
# the new source, one context line; library-linkage.diff and tools.diff apply
# as they are. cancel-waits-for-transfers.diff is Revenant's, and is a change
# to librtlsdr under librtlsdr's licence; its preamble says what it does.
#
# Pinned by commit rather than by tag: 797f814 is osmocom/rtl-sdr master as of
# 2026-09-23, which is also the v2.0.3 release, and a tag can be moved where a
# commit cannot.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO osmocom/rtl-sdr
    REF 797f8143266d983c56d8f35d2d442527529dd8a5
    SHA512 7a5885ea217dd848eb9a7eec97c1e55e3ca6e2a2163971237d95e55e2ab9ef957b8f7c7f91b35b5410fd68d8e54d3868ff48bd016a3d675ba9a41e85b144f2de
    HEAD_REF master
    PATCHES
        dependencies.diff
        library-linkage.diff
        tools.diff
        cancel-waits-for-transfers.diff
)

vcpkg_check_features(OUT_FEATURE_OPTIONS options
    FEATURES
        tools   BUILD_TOOLS
)

vcpkg_find_acquire_program(PKGCONFIG)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${options}
        "-DPKG_CONFIG_EXECUTABLE=${PKGCONFIG}"
        "-DCMAKE_REQUIRE_FIND_PACKAGE_PkgConfig=1"
        "-DCMAKE_DISABLE_FIND_PACKAGE_Git=1"
    OPTIONS_DEBUG
        -DBUILD_TOOLS=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/rtlsdr)
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

if(VCPKG_TARGET_IS_WINDOWS AND VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/librtlsdr.pc" " -lrtlsdr" " -lrtlsdr_static")
    if(NOT VCPKG_BUILD_TYPE)
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/librtlsdr.pc" " -lrtlsdr" " -lrtlsdr_static")
    endif()
endif()

if("tools" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES rtl_adsb rtl_biast rtl_eeprom rtl_fm rtl_power rtl_sdr rtl_tcp rtl_test  AUTO_CLEAN)
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL "${CURRENT_PORT_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
