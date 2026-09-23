# Tests for the M0 tooling. Guarded on the targets existing so that a checkout
# part-way through M0 still configures and the rest of the suite still runs.

if(TARGET revenant_core)
    add_executable(revenant_tool_tests
        ../support/no_modal_dialogs.cpp
        test_channel.cpp
        test_wfm_mod.cpp
    )

    target_link_libraries(revenant_tool_tests PRIVATE
        revenant_core
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
    message(STATUS "revenant_core is not present yet, skipping the channel tests")
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

    # The rds mode, run small: one point at 12 dB Eb/N0, eight trials. It
    # proves the transmitter, the noise and the decoder are wired into the
    # harness and that every trial locks, which is what the decoded count
    # says. It asserts no BER: the curve belongs to the nightly sweep, which
    # compares it against tests/baselines/ber-vs-snr-rds.json.
    add_test(
        NAME bench_sweeps_rds
        COMMAND bench sweep --mode rds
                --snr-start 12 --snr-stop 12 --trials 8 --seed 7 --quiet
    )
    set_tests_properties(bench_sweeps_rds PROPERTIES
        LABELS "tools"
        TIMEOUT 600
        PASS_REGULAR_EXPRESSION "\"decoded_count\": 8,"
    )

    # The throughput command, run small, as a smoke test and nothing more.
    #
    # There is no performance threshold here and there must not be one. Every
    # number the command produces depends on what else is running on the
    # machine, and a timing assertion on a shared workstation fails for
    # reasons that have nothing to do with the code. This project does not
    # ship flaky tests, so what is asserted is what can be asserted: the
    # command builds an engine, drives a source through it with receivers
    # attached, times the device, counts bytes and exits zero.
    #
    # It needs a GPU. --gpu -1 honours REVENANT_GPU_INDEX, which is how the
    # conformance matrix aims every other binary here.
    add_test(
        NAME bench_throughput_runs
        COMMAND bench throughput
                --gpu -1
                --rate 2400000 --seconds 0.25 --block-samples 16384
                --receivers 0,2
                --iterations 4 --warmup 1
                --transform-sizes 64 --transform-bytes 4194304
                --bandwidth-iterations 2
    )
    set_tests_properties(bench_throughput_runs PROPERTIES
        LABELS "tools"
        TIMEOUT 600
    )
else()
    message(STATUS "bench is not present yet, skipping the BER theory check")
endif()
