# Stamps a Windows executable with the application icon and a VERSIONINFO
# block, both derived from cmake/RevenantVersion.cmake so the version exists in
# exactly one place.
#
# Every Locke Werks binary is identifiable in a taskbar and in Alt-Tab without
# reading its label, and reports a real version to anyone who opens its
# properties. Both are part of building the program rather than a release-time
# afterthought: a version resource added at release is a version resource that
# disagrees with the build it came from.

function(revenant_embed_version target)
    if(NOT WIN32)
        return()
    endif()

    cmake_parse_arguments(ARG "" "DESCRIPTION;FILENAME" "" ${ARGN})

    if(NOT ARG_DESCRIPTION)
        set(ARG_DESCRIPTION "${REVENANT_PRODUCT}")
    endif()
    if(NOT ARG_FILENAME)
        set(ARG_FILENAME "${target}.exe")
    endif()

    # The resource compiler treats a backslash in a string as an escape, so the
    # icon path is handed over with forward slashes rather than composed by
    # hand at each call site.
    file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets/revenant.ico" REVENANT_ICON_PATH)

    if(NOT EXISTS "${REVENANT_ICON_PATH}")
        message(FATAL_ERROR
            "assets/revenant.ico is missing. Generate it with: python scripts/make_icon.py")
    endif()

    set(REVENANT_RC_DESCRIPTION "${ARG_DESCRIPTION}")
    set(REVENANT_RC_INTERNAL_NAME "${target}")
    set(REVENANT_RC_ORIGINAL_FILENAME "${ARG_FILENAME}")

    set(generated "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
    configure_file("${CMAKE_SOURCE_DIR}/res/revenant.rc.in" "${generated}" @ONLY)

    target_sources(${target} PRIVATE "${generated}")
endfunction()
