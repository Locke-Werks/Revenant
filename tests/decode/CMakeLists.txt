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
# parsers. test_vocoder_plugin.cpp covers the vocoder seam and the plugin
# loader, and the five DLLs below are what it loads. test_imbe.cpp covers the
# P25 Phase 1 vocoder: the two error control codes on their own, the annex
# tables against the constraints the standard states elsewhere, and the
# decoder end to end.
#
# test_p25p1.cpp, test_dstar.cpp and test_tetra.cpp are the three digital
# voice physical layers, and they follow the RDS pattern with one addition.
# Each has two groups of cases. The first checks the standard's own codes
# against the standard's own printed tables, which is what lets the
# implementation carry a generator rather than a copied matrix: a case that
# regenerates the matrix establishes that the rule in the code is the rule in
# the document, where retyping the matrix would establish only that it was
# retyped correctly. The second is the round trip against the transmitters in
# core/dsp/synth/dv_mod.cpp.
#
# test_p25p1_voice.cpp carries P25 voice end to end: the Logical Link Data
# Units, Link Control and encryption sync through their Reed-Solomon codes,
# and IMBE frames from the transmitter through the air interface into the
# vocoder. Its frames are built by imbe_test_frames.h, which test_imbe.cpp
# shares, so both suites agree on what a frame is.
#
# Every error rate in those three is measured and reported in an INFO line and
# the assertion around it is loose. A bit error rate asserted to three decimal
# places is a test that fails when somebody improves the receiver, and the
# number a reader wants is in the log either way.
#
# IMBE has no synthetic transmitter behind it and is not getting one, which is
# the one place this directory departs from the rule above. An IMBE encoder is
# sections 5 and 10 of the same standard, four fifths of the work again, and a
# round trip through a twin this project wrote would prove the two halves
# agree rather than that either matches the document. Those tests assert
# against invariants the standard states instead, and each one names the
# clause at the assertion.
#
# Both were behind an EXISTS guard while they landed on separate branches in
# the same session. Both have landed, and the guard is gone. On a test
# executable it was the worse of the two: a guard that can drop a whole file
# of cases out of the binary at configure time takes them out of ctest as
# well, and ctest then reports green over a suite that is quietly smaller
# than it was. A test that is not run does not fail, which is the one thing a
# missing test must never look like.

# The plugin fixtures.
#
# One source compiled five times, each build carrying exactly one defect, so
# the real loader can be driven against a real DLL for every refusal it can
# produce. tests/decode/vocoder_test_plugin.cpp says what each one does and
# what this arrangement does NOT prove: these are built by this toolchain and
# are therefore /MT like everything else here, so they cannot demonstrate the
# cross-CRT heap crash the caller-allocates rule in core/decode/vocoder_abi.h
# exists to prevent. Showing that needs a second toolchain and does not belong
# in this suite.
#
# Not guarded on WIN32. The loader is Windows-only today and says so in its
# own status line, and a guard here would quietly remove eleven cases from
# ctest on the day somebody starts a port, which is precisely when they are
# wanted. The comment above about a test that is not run applies to a platform
# guard exactly as it applies to an EXISTS guard.
set(revenant_vocoder_fixtures
    rv_test_vocoder_good
    rv_test_vocoder_nosym
    rv_test_vocoder_oldabi
    rv_test_vocoder_overrun
    rv_test_vocoder_refuse
)

foreach(fixture IN LISTS revenant_vocoder_fixtures)
    add_library(${fixture} SHARED vocoder_test_plugin.cpp)
    target_include_directories(${fixture} PRIVATE "${CMAKE_SOURCE_DIR}")
    revenant_apply_warnings(${fixture})
endforeach()

target_compile_definitions(rv_test_vocoder_nosym PRIVATE RV_TEST_PLUGIN_OMIT_DECODE)
target_compile_definitions(rv_test_vocoder_oldabi PRIVATE RV_TEST_PLUGIN_ABI_VERSION=0u)
target_compile_definitions(rv_test_vocoder_overrun PRIVATE RV_TEST_PLUGIN_OVERRUN_REPORT)
target_compile_definitions(rv_test_vocoder_refuse PRIVATE RV_TEST_PLUGIN_REFUSE_CREATE)

# The test needs the built path of each fixture, and a compile definition
# cannot carry a Windows path without the backslashes being eaten somewhere
# between CMake, the response file and the preprocessor. A generated header
# holding raw string literals has no escaping in it at any stage.
#
# $<CONFIG> is in the output path because the paths differ per configuration
# under a multi-config generator, and file(GENERATE) refuses content that
# varies by config unless the output does too. Without it the vs preset fails
# to configure while ninja is perfectly happy, which is the shape of bug that
# gets found by somebody else.
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/vocoder_test_plugin_paths.h"
    CONTENT [[// Generated by tests/decode/CMakeLists.txt. Do not edit.
#pragma once

inline constexpr const char* kTestVocoderPluginGood = R"($<TARGET_FILE:rv_test_vocoder_good>)";
inline constexpr const char* kTestVocoderPluginNoSymbol = R"($<TARGET_FILE:rv_test_vocoder_nosym>)";
inline constexpr const char* kTestVocoderPluginOldAbi = R"($<TARGET_FILE:rv_test_vocoder_oldabi>)";
inline constexpr const char* kTestVocoderPluginOverrun = R"($<TARGET_FILE:rv_test_vocoder_overrun>)";
inline constexpr const char* kTestVocoderPluginRefuseCreate = R"($<TARGET_FILE:rv_test_vocoder_refuse>)";
]]
)

add_executable(revenant_decode_tests
    ../support/no_modal_dialogs.cpp
    test_aprs.cpp
    test_ax25.cpp
    test_cw.cpp
    test_dstar.cpp
    test_imbe.cpp
    test_navtex.cpp
    test_p25p1.cpp
    test_p25p1_voice.cpp
    test_pocsag.cpp
    test_psk31.cpp
    test_rds_bits.cpp
    test_rds_groups.cpp
    test_rtty.cpp
    test_sitor_b.cpp
    test_tetra.cpp
    test_vocoder_plugin.cpp
)

target_link_libraries(revenant_decode_tests PRIVATE
    revenant_core
    Catch2::Catch2WithMain
)
target_include_directories(revenant_decode_tests PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
)

# The fixtures are loaded at run time, not linked, so nothing else would build
# them before the test ran. A ctest run against a stale or absent DLL is the
# one failure mode of this suite that would not be the loader's fault.
add_dependencies(revenant_decode_tests ${revenant_vocoder_fixtures})

revenant_apply_warnings(revenant_decode_tests)
revenant_apply_reference_fp(revenant_decode_tests)

catch_discover_tests(revenant_decode_tests
    PROPERTIES
        LABELS "decode"
        TIMEOUT 300
)
