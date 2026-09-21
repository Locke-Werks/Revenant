find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

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
else()
    # Loud rather than silent. CI still runs the check, so this is a developer
    # losing the local copy of it, not the gate going missing.
    message(WARNING
        "No Python 3 interpreter: the retired-claims check will not run in "
        "ctest here. CI's guards job still runs it.")
endif()
