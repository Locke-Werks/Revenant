# The decoders.
#
# No GPU, and none is coming. Nothing in core/decode has a compute shader
# behind it: the physical layer is a pure function of a span of samples and
# the group layer is a pure function of a span of bits, so the project's
# bit-exact-against-a-scalar-twin rule has nothing to compare against here.
# What stands in its place is the synthetic transmitter in
# core/dsp/synth/rds_mod.h, written from the same clauses of the same
# standard: a round trip that fails means one of the two ends misread the
# document, which is the failure a clean-room decoder is most likely to have
# and the one a real capture would never localise.
#
# test_rds_bits.cpp covers recovered subcarrier to differentially decoded
# bits. test_rds_groups.cpp covers block sync, the checkword and the group
# parsers, and is landing on a separate branch in the same session; the EXISTS
# guard is there so the two build independently and merge without either
# branch editing the other's line.

set(REVENANT_DECODE_TEST_SOURCES
    ../support/no_modal_dialogs.cpp
    test_rds_bits.cpp
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_rds_groups.cpp")
    list(APPEND REVENANT_DECODE_TEST_SOURCES test_rds_groups.cpp)
endif()

add_executable(revenant_decode_tests ${REVENANT_DECODE_TEST_SOURCES})

target_link_libraries(revenant_decode_tests PRIVATE
    revenant_core
    Catch2::Catch2WithMain
)
target_include_directories(revenant_decode_tests PRIVATE "${CMAKE_SOURCE_DIR}")

revenant_apply_warnings(revenant_decode_tests)
revenant_apply_reference_fp(revenant_decode_tests)

catch_discover_tests(revenant_decode_tests
    PROPERTIES
        LABELS "decode"
        TIMEOUT 300
)
