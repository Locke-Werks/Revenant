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
# windowsmediaplugin and ffmpegmediaplugin, and the client loads the FFmpeg
# one. Measured 2026-09-21 by launching revenant-ui.exe from a directory with
# no Qt on PATH and reading the process module list:
# multimedia/ffmpegmediaplugin.dll is loaded together with avcodec-61,
# avformat-61, avutil-59, swresample-5 and swscale-8, and
# windowsmediaplugin.dll is not loaded at all. FFmpeg is the default Qt
# Multimedia backend in Qt 6.8 and nothing here sets QT_MEDIA_BACKEND, so the
# backend serving QMediaDevices and QAudioSink is that one. Those six files are
# 18.3 MB of the installer payload; see docs/packaging.md.
#
# WHAT THIS PARAGRAPH USED TO SAY. "Raw PCM through QAudioSink is served by the
# windows platform backend, so nothing on this path depends on FFmpeg or on the
# five shared libraries beside it", and that an install that carries only the
# windows one still plays audio. It was written to decide the Forge payload and
# was not measured; the payload decision is the one place it would have done
# real damage, so it is recorded rather than swapped.
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
    audio/audio_mix.cpp
    audio/audio_mix.h
    audio/audio_player.cpp
    audio/audio_player.h
    audio/audio_ring.cpp
    audio/audio_ring.h
    audio/drift_trim.h
    audio/mix_stages.h
    audio/resampler.cpp
    audio/resampler.h
    models/audio_counters.h
    models/audio_link.cpp
    models/bookmark_link.cpp
    models/calibration.h
    models/calibration_link.cpp
    models/decoded_link.cpp
    models/decoded_log.h
    models/decoded_model.cpp
    models/decoded_model.h
    models/engine_link.cpp
    models/engine_link.h
    models/frequency_manager.cpp
    models/frequency_manager.h
    models/json_lite.h
    models/memories.h
    models/memory_import.h
    models/band_plan.h
    models/frame_stats.h
    models/frequency_dial.h
    models/key_actions.h
    models/key_map.h
    models/key_steps.h
    models/level_meter.h
    models/mode_choice.h
    models/palette_match.h
    models/receiver_scroll.h
    models/status_summary.h
    models/ruler.h
    models/scale_settings.h
    models/ui_rules.h
    models/aft.h
    models/aft_link.cpp
    models/auto_filter.h
    models/auto_filter_link.cpp
    models/noise_controls.h
    models/noise_link.cpp
    models/label_tune.h
    models/rack_link.cpp
    models/rds_link.cpp
    models/receiver_link.cpp
    models/receiver_palette.h
    models/receiver_rack.h
    models/open_source_link.cpp
    models/pace_link.cpp
    models/recent_recordings.h
    models/recording_header.h
    models/recording_link.cpp
    models/recording_link.h
    models/recording_plan.h
    models/recording_status.h
    models/source_link.cpp
    models/window_raise.h
    render/colour_map.h
    render/frame_probe.cpp
    render/frame_probe.h
    render/history_shift.h
    render/passband_item.cpp
    render/passband_item.h
    render/passband_waterfall_item.cpp
    render/passband_waterfall_item.h
    render/spectrum_item.cpp
    render/spectrum_item.h
    render/spectrum_scale.h
    render/waterfall_item.cpp
    render/waterfall_item.h
    render/window_pacer.cpp
    render/window_pacer.h
    ${revenant_capnp_sources}
    ${revenant_rpc_client_sources}
)

# The palette and the type sizes, as one object every section file reads. A
# singleton rather than a property on the window, because a section in its own
# file cannot see the window's id.
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(revenant_ui
    URI Revenant
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/Plate.qml
        qml/PinPlate.qml
        qml/NoticeRow.qml
        qml/TuneSelection.qml
        qml/StatusRow.qml
        qml/SourcePicker.qml
        qml/FrequencyManagerPanel.qml
        qml/RecordingSection.qml
        qml/RecordingStrip.qml
        qml/DetectionControls.qml
        qml/SpanView.qml
        qml/Ruler.qml
        qml/BandPicker.qml
        qml/FrequencyDial.qml
        qml/RdsPane.qml
        qml/DecodePane.qml
        qml/AudioPane.qml
        qml/ClickReadout.qml
        qml/TopBar.qml
        qml/TuneBar.qml
        qml/NoticeBanner.qml
        qml/StatusDrawer.qml
        qml/ReceiverDetail.qml
        qml/ReceiverRack.qml
        qml/ReceiverStrip.qml
        qml/VrxWindow.qml
        qml/Commands.qml
        qml/CommandPalette.qml
        qml/KeyMapView.qml
        qml/controls/KeyChip.qml
        qml/controls/Popover.qml
        qml/controls/Readout.qml
        qml/controls/StatusChip.qml
        qml/controls/RButton.qml
        qml/controls/RCheckBox.qml
        qml/controls/RComboBox.qml
        qml/controls/RSegmented.qml
        qml/controls/RSlider.qml
        qml/controls/RTextField.qml
)

# qmlcachegen's output, which compiles each QML file to C++ and is ours no more
# than the capnp schema's is. It inlines Qt's own headers into code paths the
# compiler can prove are dead, and MSVC reports C4702 from inside qmetatype.h,
# qvariant.h and qjsengine.h for them. Nothing in this tree can fix that, and
# with REVENANT_WERROR on it stopped the build at Main_qml.cpp before this was
# here. Scoped to the generated files by path, so the hand-written sources keep
# the warning.
if(MSVC)
    get_target_property(revenant_ui_all_sources revenant_ui SOURCES)
    set(revenant_qmlcache_sources ${revenant_ui_all_sources})
    list(FILTER revenant_qmlcache_sources INCLUDE REGEX "/[.]rcc/qmlcache/")
    set_source_files_properties(${revenant_qmlcache_sources}
        PROPERTIES COMPILE_OPTIONS "/wd4702")
endif()

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
        # render/frame_probe.cpp's <rhi/qrhi.h>, the only place Qt reports a
        # frame's GPU time, and render/window_pacer.cpp's
        # <qpa/qplatformwindow.h>, the only place it says whether a window has
        # a frame asked for and not yet delivered.
        Qt6::GuiPrivate
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
        #
        # --include-plugins qoffscreen for --smoke-seconds, which runs on the
        # offscreen platform. windeployqt deploys qwindows alone by default,
        # and a platform plugin that is missing is not a failed exit: Qt puts
        # up a modal message box and waits, which on CI is a job that hangs
        # until its timeout.
        add_custom_command(TARGET revenant_ui POST_BUILD
            COMMAND "${REVENANT_WINDEPLOYQT}"
                    --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                    --include-plugins qoffscreen
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
# It covers AudioMix and what it is built from: the resampler's accuracy and
# stopband, the alignment of receivers whose indices share no origin, the
# level AGC for the amplitude-detected modes, the multiplex's de-emphasis and
# the soft limiter on the sum, and the drift trim that holds the lead ring's
# fill against an engine clock off the card's, over ten simulated minutes.
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
# It covers plan_receiver_marker, which places the receiver's passband on
# the span displays. Every wrong version of it draws something plausible:
# a baseband placement marks a frequency, a stretched band marks a width,
# and an off-span edge pinned to the display boundary marks a filter edge
# that is not there. A picture that is wrong and looks right is exactly
# what a test can catch and an operator cannot.
#
# It covers plan_scroll_tune, which paces the mouse wheel against a front end
# that goes deaf for about 330 ms per retune. The frequency it picks is
# something an operator sees and corrects; how many retunes a flick of the
# wheel issues is not, and a coalescer that lets a burst through leaves the
# picture moving for seconds after the gesture stopped.
#
# It covers plan_composite_probe and composite_grant_refusal, which decide
# whether the RDS switch may raise the pane's receiver to 171000 S/s and what
# it says when it may not. Two of the engine's four conditions are answered
# here without a round trip, and getting either wrong costs the operator the
# receiver they were listening to: a rate change is a remove and an add. The
# fourth condition is the one a successful add does not answer, and it is read
# off the grant rather than the request, which is the mistake that looks
# correct.
#
# It covers the EN 50067 Annex E mapping, its segment placeholders and the
# four states an empty RDS pane can be in. The mapping is the difference
# between a letter and a different letter that renders just as plausibly,
# and the placeholders are the difference between a call sign and half of
# one. models/rds_view.h includes core/rpc/types.h, which is three standard
# headers and no Cap'n Proto, so this target stays a test binary.
#
# It covers the rules behind the controls added in the design pass of
# 2026-09-22: what one wheel notch on a dial digit does (models/frequency_dial.h),
# where the ruler's ticks go and what they say (models/ruler.h), the band table
# and which bands a device reaches (models/band_plan.h), and how loudly the
# status pill speaks and how far a receiver's meter reads
# (models/status_summary.h, models/level_meter.h). Each is the part of its
# control that can be wrong while still looking right.
#
# It covers the keyboard: the one table of actions and their keys
# (models/key_actions.h), which it holds to having no two actions on one key
# and to matching the key map in docs/ui-spectrum.md, the steps the keys take
# (models/key_steps.h), and the command palette's matching and ranking
# (models/palette_match.h). A dead key is what a conflict looks like, since Qt
# runs neither of two shortcuts on one sequence, and nothing on screen says so.
#
# It covers the eight receiver colours (models/receiver_palette.h), which are
# held to a minimum CIEDE2000 difference in normal and simulated protan, deutan
# and tritan vision and to being the output of the search that chose them, and
# the rack they are worn in: its slots, focus, mute and solo, the strip gain
# and what a click on the span does (models/receiver_rack.h), with when that
# click brings the receiver window forward (models/window_raise.h).
#
# It covers the frequency manager's model and its importers: the memory file
# and its schema, the migration of the old bookmark list, search, sort, tags,
# delete and undo, scan lists (models/memories.h), the JSON reader under them
# (models/json_lite.h), and reading SDR#, SDR++ and CHIRP files and writing
# CHIRP's (models/memory_import.h), against sample files in tests/data/memories.
# A memory list is kept for years; a reader that drops an entry or reads a
# frequency a few hertz off produces a list that looks complete.
# It covers the recording section of the picker: the header read that has to
# agree with core/source/file_source.cpp about every container it opens and
# every file it refuses (models/recording_header.h), the URI and the reasons an
# open is not offered yet (models/recording_plan.h), the recent list
# (models/recent_recordings.h) and the playing strip's position and pace
# (models/recording_status.h). A preview that disagreed with the engine would
# show one file and open another, which looks correct until the frequencies
# are read.
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
# CI CONFIGURES THIS PROJECT ON ITS OWN, as the ui job in
# .github/workflows/ci.yml: cmake --preset vs with REVENANT_WERROR on, then
# ctest --preset vs from ui/. The root CMakeLists.txt still refuses
# REVENANT_BUILD_UI outright, because one cache cannot hold both runtimes, so
# that separate configure is the only way this target is built by anyone who
# merges.
#
# WHAT THIS PARAGRAPH USED TO SAY. It began "NO CI JOB CONFIGURES THIS
# PROJECT" and ended "Closing that needs a CI leg configuring ui/ against the
# dynamic triplet and it is not made here." That leg was made on 2026-09-22
# and the sentence was true until then.
if(REVENANT_UI_TESTS)
    find_package(Catch2 3 CONFIG QUIET)
    if(Catch2_FOUND)
        include(CTest)
        include(Catch)

        # audio_ring.cpp, audio_mix.cpp and resampler.cpp, and nothing else
        # of ours that has a .cpp. The point of those classes is that they
        # hold no Qt type, and a test target that linked Qt would stop that
        # being true the first time somebody reached for QString in there.
        # WHAT THIS PARAGRAPH USED TO SAY: "audio_ring.cpp and nothing else of
        # ours that has a .cpp." The mix and its resampler joined it on
        # 2026-09-23 on the same terms.
        #
        # render/history_resize.h is the other side of the same rule and has
        # no source file: it is the waterfall's ring arithmetic lifted out of
        # a QQuickItem so it can be asserted without a window. Anything else
        # pulled out for a test belongs in a header of that shape rather than
        # in a .cpp added here, because a .cpp that includes a Qt type would
        # fail this link with a message about symbols and not about the rule
        # it broke.
        add_executable(revenant_ui_tests
            audio/audio_mix.cpp
            audio/audio_ring.cpp
            audio/resampler.cpp
            tests/test_aft.cpp
            tests/test_audio_counters.cpp
            tests/test_audio_mix.cpp
            tests/test_audio_ring.cpp
            tests/test_drift_trim.cpp
            tests/test_mix_stages.cpp
            tests/test_resampler.cpp
            tests/test_auto_filter.cpp
            tests/test_band_plan.cpp
            tests/test_bookmarks.cpp
            tests/test_calibration.cpp
            tests/test_colour_map.cpp
            tests/test_composite_probe.cpp
            tests/test_decoded_log.cpp
            tests/test_frame_stats.cpp
            tests/test_front_end_note.cpp
            tests/test_history_resize.cpp
            tests/test_history_shift.cpp
            tests/test_json_lite.cpp
            tests/test_key_actions.cpp
            tests/test_key_steps.cpp
            tests/test_memories.cpp
            tests/test_memory_import.cpp
            tests/test_label_tune.cpp
            tests/test_mode_choice.cpp
            tests/test_noise_controls.cpp
            tests/test_palette_match.cpp
            tests/test_frequency_dial.cpp
            tests/test_frequency_entry.cpp
            tests/test_gain_control.cpp
            tests/test_rds_services.cpp
            tests/test_rds_text.cpp
            tests/test_receiver_gone.cpp
            tests/test_receiver_marker.cpp
            tests/test_receiver_match.cpp
            tests/test_receiver_palette.cpp
            tests/test_receiver_rack.cpp
            tests/test_receiver_scroll.cpp
            tests/test_recent_recordings.cpp
            tests/test_recording_header.cpp
            tests/test_recording_plan.cpp
            tests/test_recording_status.cpp
            tests/test_ruler.cpp
            tests/test_scroll_tune.cpp
            tests/test_source_choice.cpp
            tests/test_source_pacing.cpp
            tests/test_spectrum_scale.cpp
            tests/test_status_summary.cpp
            tests/test_window_raise.cpp
        )

        target_include_directories(revenant_ui_tests PRIVATE
            "${REVENANT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}"
        )

        # Where docs/ is, for the one case that reads a document: the key map
        # in docs/ui-spectrum.md is checked against models/key_actions.h so
        # the two cannot drift.
        target_compile_definitions(revenant_ui_tests PRIVATE
            REVENANT_DOCS_DIR="${REVENANT_SOURCE_DIR}/docs")

        # The sample memory files the frequency manager's importers are tested
        # against, which were written for those tests; see
        # models/memory_import.h for what each was written from.
        target_compile_definitions(revenant_ui_tests PRIVATE
            REVENANT_UI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")

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
