# The rolling capture.
#
# The writer cases need no GPU. A segment is bytes on disk plus a JSON
# sidecar, and the one case that replays a segment does it through the file
# source, which is host code.
#
# test_search.cpp is mostly host logic: the query, the occupancy buckets and
# the meta merge are pure functions of annotations and text, and need no
# device. Its index case renders a scene to a SigMF segment and runs it
# through the engine, which does need one, so the binary takes the shared GPU
# fixture and that case is tagged [gpu] and skips itself without a device the
# way every other engine case does.

add_executable(revenant_capture_tests
    ../support/no_modal_dialogs.cpp
    test_writer.cpp
    test_decode_annotations.cpp
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

add_executable(revenant_capture_search_tests
    ../support/no_modal_dialogs.cpp
    ../reference/gpu_fixture.cpp
    test_search.cpp
)

target_link_libraries(revenant_capture_search_tests PRIVATE
    revenant_core
    Catch2::Catch2WithMain
)
target_include_directories(revenant_capture_search_tests PRIVATE "${CMAKE_SOURCE_DIR}")
revenant_apply_warnings(revenant_capture_search_tests)
revenant_apply_reference_fp(revenant_capture_search_tests)

catch_discover_tests(revenant_capture_search_tests
    TEST_PREFIX "test_search: "
    PROPERTIES
        LABELS "capture"
        TIMEOUT 300
)
