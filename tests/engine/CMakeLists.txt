# The engine spine: the device ring's host logic and the source abstraction.
#
# No GPU is needed for either. Most of the risk in a ring is in the sizing, the
# retirement floor and the wrap arithmetic, and most of the risk in a source is
# in the URI layer, the seek and the flow control. All of that is host logic
# and all of it fails silently when it fails.

add_executable(revenant_engine_tests
    ../support/no_modal_dialogs.cpp
    test_ring.cpp
    test_sources.cpp
)

target_link_libraries(revenant_engine_tests PRIVATE
    revenant_core
    Catch2::Catch2WithMain
)
target_include_directories(revenant_engine_tests PRIVATE "${CMAKE_SOURCE_DIR}")

revenant_apply_warnings(revenant_engine_tests)
revenant_apply_reference_fp(revenant_engine_tests)

catch_discover_tests(revenant_engine_tests
    PROPERTIES
        LABELS "engine"
        TIMEOUT 300
)
