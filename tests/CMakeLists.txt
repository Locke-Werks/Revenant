find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

# Registers a Catch2 target's cases with ctest, with the ones that open the
# RTL-SDR kept apart. Every case tagged [dongle] gets the ctest label dongle as
# well as the target's own, and RESOURCE_LOCK rtlsdr, so:
#
#   ctest never runs two dongle cases at once, even under -j, and
#   `ctest -LE dongle` leaves every one of them out, which is what the
#   hand-typed `-E 'dongle|rtlsdr|RTL|...'` patterns were reaching for.
#
# RESOURCE_LOCK only orders cases within one ctest run. Two runs, or a run and
# an engine somebody is using, are ordered by the machine-wide lock the cases
# take through tests/support/dongle_lock.h.
#
# TWO DISCOVERIES OVER ONE BINARY because ctest properties are per case and
# catch_discover_tests applies one set to everything it finds. The two labels
# are set by a hook included after the discovered list rather than through
# PROPERTIES, because a two-element LABELS list there means escaping a
# semicolon through three layers of argument passing inside
# catch_discover_tests, and the hook has nothing to escape. Hidden cases are
# left out of the dongle
# discovery by ~[.], as they are out of the ordinary one, so the probes and the
# contender stay run-by-name only.
function(revenant_discover_tests target label)
    catch_discover_tests(${target}
        TEST_SPEC "~[dongle]"
        PROPERTIES
            LABELS "${label}"
            TIMEOUT 300
    )
    catch_discover_tests(${target}
        TEST_SPEC "[dongle]~[.]"
        TEST_LIST ${target}_dongle_cases
        PROPERTIES
            LABELS "${label}"
            RESOURCE_LOCK rtlsdr
            TIMEOUT 300
    )
    set(hook "${CMAKE_CURRENT_BINARY_DIR}/${target}_dongle_labels.cmake")
    file(WRITE "${hook}"
        "if(${target}_dongle_cases)\n"
        "  set_tests_properties(\${${target}_dongle_cases}\n"
        "    PROPERTIES LABELS \"${label};dongle\")\n"
        "endif()\n"
    )
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${hook}")
endfunction()

# The reference diff suite. This is the target CI cares about most: it is what
# stands between a kernel that computes the wrong thing and six months of
# confident garbage.
add_executable(revenant_reference_tests
    support/no_modal_dialogs.cpp
    reference/gpu_fixture.cpp
    reference/test_context.cpp
    reference/test_cmul.cpp
    reference/test_convert.cpp
    reference/test_denormal_mode.cpp
    reference/test_divergence.cpp
    reference/test_pfb.cpp
    reference/test_spectrum.cpp
    reference/test_spectrum_levels.cpp
    reference/test_vrx.cpp
)

target_link_libraries(revenant_reference_tests PRIVATE
    revenant_core
    Catch2::Catch2WithMain
)

target_include_directories(revenant_reference_tests PRIVATE "${CMAKE_SOURCE_DIR}")

# The deliberately broken kernel lives with the test that uses it and is never
# linked into the engine.
revenant_add_shaders(revenant_reference_tests
    TARGET_ENV vulkan1.3
    SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/reference/shaders/cmul_wrong.comp"
)

revenant_apply_warnings(revenant_reference_tests)
revenant_apply_reference_fp(revenant_reference_tests)

catch_discover_tests(revenant_reference_tests
    PROPERTIES
        LABELS "reference"
        TIMEOUT 300
)

# Tool suites, added as each tool lands. Guarded so that a checkout mid-way
# through M0 still configures.
add_subdirectory(characterise)
add_subdirectory(decode)
add_subdirectory(detect)
add_subdirectory(engine)

# The RPC service. Guarded inside its own file on revenant_rpc_server, which
# is not built at all when Cap'n Proto is missing from the toolchain.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/rpc/CMakeLists.txt")
    add_subdirectory(rpc)
endif()

if(REVENANT_BUILD_TOOLS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/CMakeLists.txt")
    add_subdirectory(tools)
endif()

# REJECTS: a CLI that treats a frequency of zero as no frequency at all.
#
# --characterise carried its request in the value itself, so asking about DC
# fell through the "nothing to do" gate and exited 2 as though the whole
# command line were malformed. On a file source EngineInfo::source_center is
# zero, which makes an offset of zero the middle of the span and exactly where
# a strong carrier or an LO artefact sits, so this is the ordinary case rather
# than a corner of one.
#
# It drives the built binary because the defect is in argument parsing that
# lives in the executable and nothing links against it. A synthetic source and
# a tenth of a second keep it to the parse and the first frame; it still wants
# a GPU, like the rest of the suite around it.
if(REVENANT_BUILD_TOOLS AND TARGET revenant-cli)
    add_test(
        NAME cli_characterise_zero
        COMMAND revenant-cli
                "synthetic:wideband?rate=2400000&emitters=8&seed=4242"
                --characterise 0
                --duration 0.1
    )
    # The exit code alone would pass on a build that accepted the argument and
    # then did nothing with it, so the assertion is that the stage was reached.
    # A tenth of a second does not collect the 16384 samples it needs, and
    # saying so IS reaching it.
    set_tests_properties(cli_characterise_zero PROPERTIES
        LABELS "tools"
        TIMEOUT 120
        PASS_REGULAR_EXPRESSION "characterise +[0-9]+ samples collected")

    # REJECTS a grouping that crashes or refuses on an ordinary run. What the
    # groups contain is core/detect/groups.h's and tests/detect/test_groups.cpp
    # checks it against the family scene; what this adds is that asking for it
    # through the executable reaches the grouper and does not take the run
    # down, which is the failure an operator would meet first.
    add_test(
        NAME cli_detect_groups
        COMMAND revenant-cli
                "synthetic:wideband?rate=2400000&emitters=8&seed=4242"
                --detect --detect-groups 50k
                --duration 2
    )
    # Same argument: an exit code cannot tell an option that ran from one that
    # was parsed and dropped, which is exactly the defect cli_characterise_zero
    # above exists for. The summary counts what the grouper did over the whole
    # run and is printed whenever one was made, so it is the proof the option
    # reached core/detect rather than being parsed and dropped. The tables
    # above it depend on the instant each was sampled and are not something to
    # assert on.
    set_tests_properties(cli_detect_groups PROPERTIES
        LABELS "tools"
        TIMEOUT 180
        PASS_REGULAR_EXPRESSION "groups +[0-9]+ formed, [0-9]+ ended")
endif()

# The prose ratchet: no retired claim has come back anywhere in the tree.
#
# It is registered here so an ordinary `ctest --preset ci` catches a stale
# sentence before the push, but CI's guards job is the gate and runs the same
# script on a hosted runner. That job is the authority because it answers when
# the GPU workstation is off and because it answers in a minute; a prose check
# that only speaks after a forty minute conformance run is one people learn to
# route around.
#
# NOT A CATCH2 CASE, and the reason is in the script's own header. The short
# version: its input is the source tree rather than the build, `git ls-files`
# answers exactly which files that is, and a test binary would carry every
# retired phrase in its own read-only data, making the one artifact somebody
# might grep the one guaranteed to contain all of them.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME retired_claims
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/scripts/check_retired_claims.py"
                --root "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(retired_claims PROPERTIES LABELS "guards" TIMEOUT 120)

    # Proves the matcher still reports what it should and still allows a
    # retraction quoting itself. A ratchet nobody has watched fail is the same
    # defect as a stale comment: something everyone believes and nothing has
    # checked.
    add_test(
        NAME retired_claims_self_test
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/scripts/check_retired_claims.py"
                --self-test
    )
    set_tests_properties(retired_claims_self_test PROPERTIES LABELS "guards" TIMEOUT 60)

    # Every test target lists tests/support/no_modal_dialogs.cpp, or is named
    # as an exception. Same arrangement as the ratchet above and for the same
    # reason: CI's guards job is the gate and this is the copy a developer
    # runs before pushing. It reads CMakeLists files rather than the build, so
    # it is a script and not a Catch2 case.
    add_test(
        NAME modal_dialogs
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/scripts/check_modal_dialogs.py"
                --root "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(modal_dialogs PROPERTIES LABELS "guards" TIMEOUT 60)

    # The scanner used to drop a target whose name sat on the line below
    # add_executable(, which is a guard covering less than it claimed. The
    # second self-test case is that shape.
    add_test(
        NAME modal_dialogs_self_test
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/scripts/check_modal_dialogs.py"
                --self-test
    )
    set_tests_properties(modal_dialogs_self_test PROPERTIES LABELS "guards" TIMEOUT 60)
else()
    # Loud rather than silent. CI still runs both checks, so this is a
    # developer losing the local copy of them, not the gate going missing.
    message(WARNING
        "No Python 3 interpreter: the retired-claims and modal-dialog checks "
        "will not run in ctest here. CI's guards job still runs them.")
endif()
