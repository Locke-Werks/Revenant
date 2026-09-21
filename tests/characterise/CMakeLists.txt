# The waveform characteriser.
#
# No GPU, and none is coming, for the same reason tests/decode/CMakeLists.txt
# gives: nothing in core/characterise has a compute shader behind it, so the
# project's bit-exact-against-a-scalar-twin rule has no twin to compare
# against. What stands in its place is core/dsp/synth: every case below
# generates a signal whose symbol rate, tone spacing, frame period and
# modulation are chosen rather than measured, and asserts the characteriser
# recovers the number that was put in.
#
# No EXISTS guard on any file. tests/decode/CMakeLists.txt records why that
# was removed there and the argument is the same here: a configure-time guard
# can drop a whole file of cases out of the binary and out of ctest, and the
# suite then reports green while being quietly smaller than it was.

add_executable(revenant_characterise_tests
    ../support/no_modal_dialogs.cpp
    signal_lab.cpp
    test_cyclostationary.cpp
    test_periodicity.cpp
    test_tones.cpp
    test_transform.cpp
)

target_link_libraries(revenant_characterise_tests PRIVATE
    revenant_core
    Catch2::Catch2WithMain
)
target_include_directories(revenant_characterise_tests PRIVATE "${CMAKE_SOURCE_DIR}")

revenant_apply_warnings(revenant_characterise_tests)
revenant_apply_reference_fp(revenant_characterise_tests)

catch_discover_tests(revenant_characterise_tests
    PROPERTIES
        LABELS "characterise"
        TIMEOUT 600
)
