# The wideband detector.
#
# No GPU, and not because a GPU would be inconvenient here. The detector is
# host code by design, for the reason core/detect/detector.h states: the
# spectrum frame has already crossed the bus before it sees one. A case that
# ran the engine to produce its frames would be testing the channelizer, the
# FFT kernel and the readback path as well, and would then have to assert
# against a measurement rather than against a number somebody chose. These
# frames are built with a known centre, a known bandwidth and a known SNR in
# the reference bandwidth, so the assertions are arithmetic.
#
# The end-to-end path, engine to radio to track list, is covered by
# revenant-cli --detect rather than here.

add_executable(revenant_detect_tests
    ../support/no_modal_dialogs.cpp
    test_detector.cpp
)

target_link_libraries(revenant_detect_tests PRIVATE
    revenant_core
    Catch2::Catch2WithMain
)
target_include_directories(revenant_detect_tests PRIVATE "${CMAKE_SOURCE_DIR}")

revenant_apply_warnings(revenant_detect_tests)
revenant_apply_reference_fp(revenant_detect_tests)

catch_discover_tests(revenant_detect_tests
    PROPERTIES
        LABELS "detect"
        TIMEOUT 300
)
