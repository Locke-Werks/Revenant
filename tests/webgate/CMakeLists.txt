add_executable(test_webgate
    ../support/no_modal_dialogs.cpp
    test_certs.cpp
    test_static_files.cpp
    test_hls.cpp
    test_http.cpp
    test_spectrum_reducer.cpp
    test_display_sync.cpp
    test_display_bench.cpp
    test_voice_gain.cpp
    test_desktop_settings.cpp
    test_phone_mix.cpp
    test_audio_opus.cpp
    test_audio_rate.cpp
    test_protocol.cpp
    test_dispatch.cpp
    test_session_streams.cpp
    test_phone_hub.cpp
    test_recreate.cpp
    test_decoders.cpp
    ../reference/gpu_fixture.cpp
    test_engine_link.cpp
    test_lease.cpp
    test_cold_start.cpp
    test_cold_claim.cpp
)

target_link_libraries(test_webgate PRIVATE
    revenant_webgate_lib
    revenant_rpc_fixture
    Catch2::Catch2WithMain
)

target_include_directories(test_webgate PRIVATE "${CMAKE_SOURCE_DIR}")
# The Opus fixture the web app's decoder test reads lives in the source tree.
target_compile_definitions(test_webgate PRIVATE REVENANT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
revenant_apply_warnings(test_webgate)

# Plain discovery, not revenant_discover_tests: nothing here opens the dongle,
# and that function's second pass lists only [dongle] cases, which is empty
# here. Catch2 exits non-zero on a spec that matches nothing, so ctest would be
# left holding a test_webgate_NOT_BUILT placeholder that fails every run. Same
# reason as tests/transcribe/CMakeLists.txt.
catch_discover_tests(test_webgate
    PROPERTIES
        LABELS "webgate"
        TIMEOUT 300
)

# revenant-webgate end to end against a real revenant-engine, each in its own
# process. A binary of its own so test_webgate stays quick to build; the two
# executables are dependencies, so building this builds what it runs.
add_executable(test_webgate_twoprocess
    ../support/no_modal_dialogs.cpp
    test_webgate_twoprocess.cpp
)
target_link_libraries(test_webgate_twoprocess PRIVATE
    revenant_webgate_lib
    Catch2::Catch2WithMain
)
target_include_directories(test_webgate_twoprocess PRIVATE "${CMAKE_SOURCE_DIR}")
target_compile_definitions(test_webgate_twoprocess PRIVATE
    REVENANT_ENGINE_EXE="$<TARGET_FILE:revenant-engine>"
    REVENANT_WEBGATE_EXE="$<TARGET_FILE:revenant-webgate>"
)
add_dependencies(test_webgate_twoprocess revenant-engine revenant-webgate)
revenant_apply_warnings(test_webgate_twoprocess)

catch_discover_tests(test_webgate_twoprocess
    PROPERTIES
        LABELS "webgate"
        TIMEOUT 300
)
