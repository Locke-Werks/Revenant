# Speech to text: core/transcribe/utterance.h names the pieces and the order
# audio passes through them.
#
# A LIBRARY OF ITS OWN, NOT MORE FILES IN revenant_core, so whisper.cpp and
# ggml reach only what links this: the engine host and the tests that need a
# recogniser. Every other test binary and tool links revenant_core and stays
# as it was, without a 1.6 GB model's runtime it never calls.
#
# whisper.cpp and ggml come from vcpkg, ggml through Revenant's overlay in
# vcpkg-overlays/ggml, which pins the CPU baseline and builds the Vulkan
# backend against the LunarG SDK; its portfile says why on both counts. Both
# are static libraries against the static CRT like everything else in this
# tree, and the only DLL they import is vulkan-1.dll, which the engine already
# imports.

find_package(whisper CONFIG REQUIRED)
find_package(ggml CONFIG REQUIRED)

# An edit to the overlay re-runs the configure, and with it vcpkg's install.
# vcpkg's toolchain watches vcpkg.json and vcpkg-configuration.json and not
# the overlay's own files, so on 2026-10-03, when the overlay moved ggml to
# the commit whisper.cpp 1.8.3 was synced to, a build tree configured before
# the move kept linking the old ggml and aborted in its first transcription on
# the mask assertion the portfile describes, until somebody configured by hand.
file(GLOB revenant_ggml_overlay CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/vcpkg-overlays/ggml/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${revenant_ggml_overlay})

# EACH CONFIGURATION LINKS ITS OWN BUILD OF THE TWO LIBRARIES.
#
# Neither package exports a per-configuration target. Both configs make an
# UNKNOWN IMPORTED target from a single find_library call, so whichever of
# vcpkg_installed/<triplet>/lib and .../debug/lib the search reaches first is
# the one every configuration links. vcpkg's toolchain puts debug/lib first
# when CMAKE_BUILD_TYPE is not set, which is every multi-config generator, so
# the vs preset linked the Debug ggml into its RelWithDebInfo and Release
# builds: /MTd objects into a /MT program, which the linker refuses with
# LNK2038 on 2026-10-03. The ci preset is single-config RelWithDebInfo and
# found the release libraries, which is why only one of the two broke.
#
# So the location is stated per configuration here, from the same installed
# tree the packages were found in. MAP_IMPORTED_CONFIG is not the answer: it
# picks among locations a target has, and these targets have one.
set(revenant_vcpkg_tree "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
foreach(pair IN ITEMS
        "whisper|whisper"
        "ggml::ggml|ggml"
        "ggml::ggml-base|ggml-base"
        "ggml::ggml-cpu|ggml-cpu"
        "ggml::ggml-vulkan|ggml-vulkan")
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 imported)
    list(GET pair 1 library)
    if(NOT TARGET ${imported})
        message(FATAL_ERROR
            "${imported} is not defined after find_package; the ggml overlay or "
            "the whisper-cpp port no longer exports what core/transcribe links.")
    endif()
    set(release_lib "${revenant_vcpkg_tree}/lib/${library}.lib")
    set(debug_lib "${revenant_vcpkg_tree}/debug/lib/${library}.lib")
    if(NOT EXISTS "${release_lib}" OR NOT EXISTS "${debug_lib}")
        message(FATAL_ERROR
            "${library}.lib is not in both ${revenant_vcpkg_tree}/lib and its "
            "debug/lib; vcpkg built only one configuration of it.")
    endif()
    set_target_properties(${imported} PROPERTIES
        IMPORTED_CONFIGURATIONS "DEBUG;RELEASE;RELWITHDEBINFO;MINSIZEREL"
        IMPORTED_LOCATION "${release_lib}"
        IMPORTED_LOCATION_DEBUG "${debug_lib}"
        IMPORTED_LOCATION_RELEASE "${release_lib}"
        IMPORTED_LOCATION_RELWITHDEBINFO "${release_lib}"
        IMPORTED_LOCATION_MINSIZEREL "${release_lib}"
    )
endforeach()

add_library(revenant_transcribe STATIC
    model_store.cpp
    transcriber.cpp
    whisper_prepare.cpp
    whisper_runner.cpp
)

target_include_directories(revenant_transcribe PUBLIC "${CMAKE_SOURCE_DIR}")

target_link_libraries(revenant_transcribe
    PUBLIC
        revenant_core
        Threads::Threads
    PRIVATE
        whisper
        ggml::ggml
)

# WinHTTP for the download, CNG for the hash, the shell for the known folder
# the model lives under and COM's allocator that hands that path back.
if(WIN32)
    target_link_libraries(revenant_transcribe PRIVATE winhttp bcrypt shell32 ole32)
endif()

# The download's User-Agent, from the one place the version is written.
target_compile_definitions(revenant_transcribe PRIVATE
    REVENANT_TRANSCRIBE_VERSION="${REVENANT_VERSION}"
)

revenant_apply_warnings(revenant_transcribe)
