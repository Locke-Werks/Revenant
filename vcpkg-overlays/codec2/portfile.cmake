# Revenant's codec2 port. The registry has none at the baseline as of
# 2026-11-02, so this overlay is the recipe.
#
# Only the vocoder is compiled, by the CMakeLists.txt beside this file in
# place of upstream's: Revenant links Codec 2 for speech and nothing else.
# LGPL-2.1, no "or later", read from the notice at the head of src/codec2.h.
# The linked-copyleft table in docs/clean-room.md carries the row.
#
# Pinned to the 1.2.0 tag, with the archive hash below holding it to the
# bytes fetched on 2026-11-02.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO drowe67/codec2
    REF 1.2.0
    SHA512 a44e6eb3ec60b81e4dfefaa733c4f8a40c5e2e90454e964b14fbce896edd0877e32d917ce878a619d7e7e544c670231d7d0c9cd35659be6989fd18d474443489
    HEAD_REF main
    PATCHES
        msvc-no-vla.diff
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH share/codec2)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

# The grant is the notice at the head of src/codec2.h, LGPL-2.1 without "or
# later"; COPYING is the LGPL-2.1 text it refers to.
file(READ "${SOURCE_PATH}/src/codec2.h" header)
string(FIND "${header}" "#ifndef" notice_end)
string(SUBSTRING "${header}" 0 ${notice_end} notice)
file(READ "${SOURCE_PATH}/COPYING" licence)
file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/copyright" "${notice}\n${licence}")
