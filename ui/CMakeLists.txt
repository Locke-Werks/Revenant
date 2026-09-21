# The Qt/QML client, which is a separate CMake project on purpose.
#
# WHY THIS IS NOT add_subdirectory(ui) FROM THE ROOT
#
# revenant_core is compiled /MT and revenant-cli links no CRT DLL, which is
# what lets it ship as one signed file. Qt 6.8.3 at C:/Qt/6.8.3/msvc2022_64 is
# built against the dynamic CRT: dumpbin /DEPENDENTS on Qt6Core.dll lists
# MSVCP140.dll, VCRUNTIME140.dll and the api-ms-win-crt-* forwarders, checked
# rather than assumed. One CMake project cannot hold both runtimes, and a
# std::string handed across that boundary in-process is allocated on one heap
# and freed on another. So this configures on its own, against the dynamic
# vcpkg triplet, and talks to the engine over a socket.
#
# The consequence for this file: it links no part of the engine. It compiles
# the schema and core/rpc/client.cpp from the main tree by path, and those
# depend on capnp, core/rpc/types.h and core/error.h and nothing else. See
# core/rpc/CMakeLists.txt, which states the same rule from the other side.
#
# TO CONFIGURE AND BUILD, from any shell, no vcvars needed:
#
#   cd ui
#   cmake --preset vs
#   cmake --build --preset vs
#
# The vs preset uses the Visual Studio generator, which finds its own
# toolchain. CMakePresets.json also carries a ninja preset for a shell that is
# already an x64 Native Tools prompt. Both pin VCPKG_TARGET_TRIPLET to
# x64-windows and CMAKE_MSVC_RUNTIME_LIBRARY to the dynamic CRT; configuring
# without those produces a link that half matches Qt and fails in ways that
# read as missing symbols rather than as a runtime mismatch.

cmake_minimum_required(VERSION 3.28)

project(revenant_ui
    DESCRIPTION "The Qt/QML client for the Revenant engine"
    LANGUAGES CXX
)

# Where the engine tree is. A cache variable rather than a hard-coded
# relative path so this can be configured from a checkout laid out
# differently, and resolved because CAPNP_GENERATE_CPP matches the source
# prefix against the schema path as a string: an unresolved ".." in here
# fails that comparison with a message about output paths that says nothing
# about the cause. get_filename_component rather than cmake_path, because
# cmake_path's NORMALIZE leaves a trailing slash on a path ending in "..".
set(REVENANT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.."
    CACHE PATH "The Revenant engine source tree this client is built against")
get_filename_component(REVENANT_SOURCE_DIR "${REVENANT_SOURCE_DIR}" ABSOLUTE)

if(NOT EXISTS "${REVENANT_SOURCE_DIR}/core/rpc/revenant.capnp")
    message(FATAL_ERROR
        "REVENANT_SOURCE_DIR is '${REVENANT_SOURCE_DIR}' and there is no "
        "core/rpc/revenant.capnp under it. Point it at the engine tree.")
endif()

option(REVENANT_WERROR "Treat warnings as errors" OFF)

# The house warning set, taken from the engine tree rather than copied, so the
# two cannot drift.
list(APPEND CMAKE_MODULE_PATH "${REVENANT_SOURCE_DIR}/cmake")
include(CompilerFlags)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Multimedia is here for QAudioSink and nothing else. It brings QMediaDevices
# and QAudioDevice with it, which is the device picker, and it brings a
# backend plugin that windeployqt copies below.
#
# WHICH BACKEND PLUGIN ACTUALLY MATTERS HERE. Qt 6.8.3 ships two,
# windowsmediaplugin and ffmpegmediaplugin. Raw PCM through QAudioSink is
# served by the windows platform backend, so nothing on this path depends on
# FFmpeg or on the five shared libraries beside it. Both plugins are present
# at C:/Qt/6.8.3/msvc2022_64 and windeployqt copies what it finds; an install
# that carries only the windows one still plays audio, which is worth knowing
# before the Forge installer's payload is decided.
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Multimedia Quick QuickControls2)
find_package(CapnProto CONFIG REQUIRED)

# ui/tests, which is a separate executable that links no Qt at all. Optional,
# so a checkout with no Catch2 in the dynamic triplet still configures and
# builds the client; see the block above the target for what it covers and
# what it deliberately does not.
option(REVENANT_UI_TESTS "Build the ui unit tests" ON)

qt_standard_project_setup(REQUIRES 6.8)

# ---------------------------------------------------------------------------
# The schema, compiled here rather than imported
# ---------------------------------------------------------------------------
#
# The main tree's generated headers are built /MT and live in its build
# directory. Reusing them would mean linking its capnp, which is the static
# triplet's. So this project runs capnpc itself against the same .capnp file.
# One schema, two generated copies, one per runtime: that is the price of the
# process split and it is paid at build time rather than at a heap boundary.
#
# Output is laid out as if relative to the engine tree, so the include reads
# "core/rpc/revenant.capnp.h" here exactly as it does there.
set(CAPNPC_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(CAPNPC_SRC_PREFIX "${REVENANT_SOURCE_DIR}")
file(MAKE_DIRECTORY "${CAPNPC_OUTPUT_DIR}/core/rpc")
capnp_generate_cpp(revenant_capnp_sources revenant_capnp_headers
    "${REVENANT_SOURCE_DIR}/core/rpc/revenant.capnp")

# ---------------------------------------------------------------------------
# The client half of the wire, compiled from the main tree by path
# ---------------------------------------------------------------------------
#
# TWO FILES AND NOT ONE, AND THE SECOND IS EASY TO FORGET
#
# core/rpc/token.cpp is the other half of revenant_rpc_client: it reads the
# pre-shared token this process has to hand Authenticator.login before it
# holds a Session at all. It is listed here by hand, exactly as client.cpp is,
# because this project shares no code with core/rpc/CMakeLists.txt and CI never
# configures it. A file added there and not here fails at link naming
# revenant::rpc::load_token, which is true and does not say that the omission
# was in this list.
set(revenant_rpc_client_sources
    "${REVENANT_SOURCE_DIR}/core/rpc/client.cpp"
    "${REVENANT_SOURCE_DIR}/core/rpc/token.cpp")

if(NOT EXISTS "${REVENANT_SOURCE_DIR}/core/rpc/client.cpp")
    # Configure still succeeds, because getting this far is worth knowing even
    # while the client implementation is being written. The link is what
    # fails, and it fails naming Client::connect, which is the truth.
    message(WARNING
        "core/rpc/client.cpp does not exist yet. This project will configure "
        "and compile, and the link will fail on revenant::rpc::Client::connect. "
        "Nothing here stubs core/rpc/client.h: the header is the contract.")
    set(revenant_rpc_client_sources "")
endif()

# ---------------------------------------------------------------------------
# The client
# ---------------------------------------------------------------------------
qt_add_executable(revenant_ui
    main.cpp
    audio/audio_player.cpp
    audio/audio_player.h
    audio/audio_ring.cpp
    audio/audio_ring.h
    models/audio_link.cpp
    models/engine_link.cpp
    models/engine_link.h
    models/rds_link.cpp
    models/receiver_link.cpp
    models/source_link.cpp
    render/passband_item.cpp
    render/passband_item.h
    render/spectrum_item.cpp
    render/spectrum_item.h
    render/spectrum_scale.cpp
    render/spectrum_scale.h
    render/waterfall_item.cpp
    render/waterfall_item.h
    ${revenant_capnp_sources}
    ${revenant_rpc_client_sources}
)

qt_add_qml_module(revenant_ui
    URI Revenant
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

# Neither of these is ours and neither passes /W4. Scoped to the files rather
# than relaxed on the target, so the hand-written sources stay at the house
# level. The generated schema has no Q_OBJECT in it and client.cpp knows
# nothing about Qt, so both are kept out of the moc scan as well.
set_source_files_properties(${revenant_capnp_sources}
    PROPERTIES SKIP_AUTOGEN TRUE)
set_source_files_properties(${revenant_rpc_client_sources}
    PROPERTIES SKIP_AUTOGEN TRUE)
if(MSVC)
    set_source_files_properties(${revenant_capnp_sources}
        PROPERTIES COMPILE_OPTIONS "/W0")
endif()

target_include_directories(revenant_ui
    PRIVATE
        "${REVENANT_SOURCE_DIR}"
        "${CAPNPC_OUTPUT_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        # qmltyperegistrar writes <engine_link.h> rather than
        # <models/engine_link.h> into the generated registration unit: it
        # knows the header's basename and not where the build put it. The
        # include is guarded by __has_include, so getting this wrong does not
        # fail there. It fails later, compiling the registration with every
        # QML_ELEMENT type undeclared, and the error names the type rather
        # than the missing path.
        "${CMAKE_CURRENT_SOURCE_DIR}/models"
        "${CMAKE_CURRENT_SOURCE_DIR}/render"
        "${CMAKE_CURRENT_SOURCE_DIR}/audio"
)

target_link_libraries(revenant_ui
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Multimedia
        Qt6::Quick
        Qt6::QuickControls2
        CapnProto::capnp-rpc
        CapnProto::capnp
        CapnProto::kj-async
        CapnProto::kj

        # core/rpc/token.cpp's, and Windows SDK import libraries rather than
        # dependencies. The same four are in core/rpc/CMakeLists.txt; see the
        # note above the source list for why this is a second edit.
        bcrypt
        shell32
        ole32
        advapi32
)

revenant_apply_warnings(revenant_ui)

# A GUI subsystem binary, so a double-click does not open a console behind the
# window. Startup failures before the window exists go to stderr, which is why
# the ninja preset is the one to use when a connection refuses.
set_target_properties(revenant_ui PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    OUTPUT_NAME revenant-ui
)

# ---------------------------------------------------------------------------
# Qt laid down beside the binary, so running it is one command
# ---------------------------------------------------------------------------
#
# The link above resolves Qt6Core.dll and friends from C:/Qt, and nothing
# puts that directory on PATH. Without this the binary builds, and then
# double-clicking it raises a loader dialog naming one DLL, which reads as a
# broken build rather than as a missing environment. Running it from a shell
# that has been set up by hand works, which is worse: it means the failure
# only shows up for whoever did not do that.
#
# windeployqt copies the Qt DLLs, the platform plugin and the QML runtime the
# module imports next to the executable. It is a POST_BUILD step rather than
# an install rule because the build tree is where this gets run from during
# development, and an install step nobody invokes deploys nothing.
if(WIN32)
    get_target_property(revenant_qmake_path Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(revenant_qt_bin "${revenant_qmake_path}" DIRECTORY)
    find_program(REVENANT_WINDEPLOYQT
        NAMES windeployqt windeployqt6
        HINTS "${revenant_qt_bin}"
    )

    if(REVENANT_WINDEPLOYQT)
        # --no-compiler-runtime because windeployqt copies the MSVC
        # redistributable out of VCINSTALLDIR, which is only set inside a
        # developer prompt, and the vs preset's whole point is that it works
        # from any shell. Asked for it anyway, it prints a warning and
        # carries on, which is a warning on every build for a file CMake can
        # find properly on its own. The copy below is that.
        add_custom_command(TARGET revenant_ui POST_BUILD
            COMMAND "${REVENANT_WINDEPLOYQT}"
                    --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                    --no-translations
                    --no-system-d3d-compiler
                    --no-compiler-runtime
                    "$<TARGET_FILE:revenant_ui>"
            COMMENT "Deploying Qt beside revenant-ui"
            VERBATIM
        )

        # This target is /MD, unlike everything in the engine tree, so it
        # needs MSVCP140.dll and the VCRUNTIME140 pair beside it on a machine
        # with no Visual Studio and no redistributable installed. Skipping
        # the install rules and taking only the list is the documented way to
        # use this module for a copy.
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
        include(InstallRequiredSystemLibraries)
        if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
            add_custom_command(TARGET revenant_ui POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
                        "$<TARGET_FILE_DIR:revenant_ui>"
                COMMENT "Copying the MSVC runtime beside revenant-ui"
                VERBATIM
            )
        endif()
    else()
        # A warning and not an error. The binary is correct; it is only
        # unrunnable without Qt's bin directory on PATH, and someone building
        # against a Qt that ships no windeployqt can still do that.
        message(WARNING
            "windeployqt was not found near ${revenant_qt_bin}. revenant-ui will "
            "build, and running it will need C:/Qt/.../bin on PATH.")
    endif()
endif()

# ---------------------------------------------------------------------------
# ui/tests
# ---------------------------------------------------------------------------
#
# WHAT THIS COVERS AND WHAT IT DOES NOT, SAID FIRST SO NOBODY READS A GREEN
# RUN AS A WORKING CLIENT.
#
# It covers AudioRing: the timeline arithmetic, the gap policy, the overrun
# and starve policies and the per-frame source that drives the squelch
# indicator. That class holds every decision on the audio path that can be
# wrong in a way a person would not hear until much later.
#
# It covers plan_history_resize, which is the waterfall's ring arithmetic
# across a height change: which wrapped index a kept row comes from, how
# many rows survive, and where the write cursor lands.
#
# It covers parse_frequency, which decides what unit a bare number typed
# into the frequency box means and therefore where the radio goes.
#
# It covers classify_pacing, which decides when the window says the source
# is behind. The rule and not the label is the part that can be wrong: a
# threshold that fires on measurement noise trains an operator to ignore
# the line, and one that never fires is the state it replaces.
#
# It covers classify_fit and format_width, which are the arithmetic behind
# "16 kHz receiver on a 145 kHz detection" and behind the decision not to
# say anything about the far more common case where the filter fits.
#
# It covers the EN 50067 Annex E mapping, its segment placeholders and the
# four states an empty RDS pane can be in. The mapping is the difference
# between a letter and a different letter that renders just as plausibly,
# and the placeholders are the difference between a call sign and half of
# one. models/rds_view.h includes core/rpc/types.h, which is three standard
# headers and no Cap'n Proto, so this target stays a test binary.
#
# It covers NOTHING with a window or a sound card in it. AudioPlayer opens a
# QAudioSink against WASAPI, and a test binary on a build agent has no output
# device, so the device picker, the format refusal and the unplug path are
# verified by running the client and not here. The QML is not covered at all.
# A green run here means the arithmetic is right. It does not mean the client
# plays.
#
# Catch2 rather than QTest, matching tests/ in the engine tree. It comes from
# the same manifest as capnproto and it is the only reason ui/vcpkg.json has a
# second dependency.
#
# NO CI JOB CONFIGURES THIS PROJECT. .github/workflows/ci.yml builds the
# engine tree and the root CMakeLists.txt refuses REVENANT_BUILD_UI outright,
# because one cache cannot hold both runtimes. So this target is run by
# whoever builds the client and by nobody who merges, exactly like the
# static_asserts in models/engine_link.h. Closing that needs a CI leg
# configuring ui/ against the dynamic triplet and it is not made here.
if(REVENANT_UI_TESTS)
    find_package(Catch2 3 CONFIG QUIET)
    if(Catch2_FOUND)
        include(CTest)
        include(Catch)

        # audio_ring.cpp and nothing else of ours that has a .cpp. The point
        # of that class is that it holds no Qt type, and a test target that
        # linked Qt would stop that being true the first time somebody
        # reached for QString in there.
        #
        # render/history_resize.h is the other side of the same rule and has
        # no source file: it is the waterfall's ring arithmetic lifted out of
        # a QQuickItem so it can be asserted without a window. Anything else
        # pulled out for a test belongs in a header of that shape rather than
        # in a .cpp added here, because a .cpp that includes a Qt type would
        # fail this link with a message about symbols and not about the rule
        # it broke.
        add_executable(revenant_ui_tests
            audio/audio_ring.cpp
            tests/test_audio_ring.cpp
            tests/test_front_end_note.cpp
            tests/test_history_resize.cpp
            tests/test_frequency_entry.cpp
            tests/test_rds_text.cpp
            tests/test_receiver_match.cpp
            tests/test_source_pacing.cpp
        )

        target_include_directories(revenant_ui_tests PRIVATE
            "${REVENANT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )

        target_link_libraries(revenant_ui_tests PRIVATE Catch2::Catch2WithMain)

        revenant_apply_warnings(revenant_ui_tests)

        catch_discover_tests(revenant_ui_tests
            PROPERTIES
                LABELS "ui"
                TIMEOUT 60
        )
    else()
        message(STATUS
            "Catch2 was not found in this triplet, so ui/tests is skipped. "
            "revenant-ui itself is unaffected.")
    endif()
endif()

message(STATUS "revenant_ui")
message(STATUS "  engine tree  ${REVENANT_SOURCE_DIR}")
message(STATUS "  Qt           ${Qt6_VERSION} at ${QT6_INSTALL_PREFIX}")
message(STATUS "  CRT          ${CMAKE_MSVC_RUNTIME_LIBRARY}")
message(STATUS "  triplet      ${VCPKG_TARGET_TRIPLET}")
