add_executable(test_webgate
    ../support/no_modal_dialogs.cpp
    test_certs.cpp
    test_static_files.cpp
    test_hls.cpp
    test_http.cpp
)

target_link_libraries(test_webgate PRIVATE
    revenant_webgate_lib
    Catch2::Catch2WithMain
)

target_include_directories(test_webgate PRIVATE "${CMAKE_SOURCE_DIR}")
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
