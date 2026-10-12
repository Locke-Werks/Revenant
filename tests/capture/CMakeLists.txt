# The rolling capture: segment writer, retention ring and recovery.
#
# No GPU. A segment is bytes on disk plus a JSON sidecar, and the one case
# that replays a segment does it through the file source, which is host code.

add_executable(revenant_capture_tests
    ../support/no_modal_dialogs.cpp
    test_writer.cpp
)

target_link_libraries(revenant_capture_tests PRIVATE
    revenant_core
    Catch2::Catch2WithMain
)
target_include_directories(revenant_capture_tests PRIVATE "${CMAKE_SOURCE_DIR}")

revenant_apply_warnings(revenant_capture_tests)

catch_discover_tests(revenant_capture_tests
    PROPERTIES
        LABELS "capture"
        TIMEOUT 300
)
