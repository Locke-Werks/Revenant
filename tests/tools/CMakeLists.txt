# Tests for the M0 tooling. Guarded on the targets existing so that a checkout
# part-way through M0 still configures and the rest of the suite still runs.

if(TARGET revenant_siggen)
    add_executable(revenant_tool_tests test_channel.cpp)

    target_link_libraries(revenant_tool_tests PRIVATE
        revenant_siggen
        Catch2::Catch2WithMain
    )
    target_include_directories(revenant_tool_tests PRIVATE "${CMAKE_SOURCE_DIR}")

    revenant_apply_warnings(revenant_tool_tests)
    revenant_apply_reference_fp(revenant_tool_tests)

    catch_discover_tests(revenant_tool_tests
        PROPERTIES
            LABELS "tools"
            TIMEOUT 300
    )
else()
    message(STATUS "revenant_siggen is not present yet, skipping the channel tests")
endif()

# The BER harness against closed-form theory.
#
# Registered as a CTest entry running the tool itself rather than as a C++ case,
# because the property under test is exactly what the tool reports: a measured
# BPSK curve tracking Q(sqrt(2*Eb/N0)). Testing it through the CLI also covers
# the argument parsing and the exit code that CI depends on.
#
# The sweep is deliberately small here. The full-depth sweep belongs in the
# nightly workflow, not in a per-commit gate.
if(TARGET bench)
    add_test(
        NAME bench_tracks_bpsk_theory
        COMMAND bench validate
                --snr-start 0 --snr-stop 8 --snr-step 2
                --trials 400 --seed 7
    )
    set_tests_properties(bench_tracks_bpsk_theory PROPERTIES
        LABELS "tools"
        TIMEOUT 600
    )
else()
    message(STATUS "bench is not present yet, skipping the BER theory check")
endif()
