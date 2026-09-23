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

    # Every decoder subject in tools/bench/mode_subjects.h, run small: two
    # points, four trials each, so a subject cannot rot between nightly runs.
    # The high point is well above the mode's crossing and every trial there
    # must decode; the low point is on the curve's slope and must show errors,
    # which is what proves the noise is reaching the decoder. No rate is
    # asserted: the curve belongs to the nightly, which compares it against
    # tests/baselines/ber-vs-snr-<mode>.json. Measured on 2026-09-23: every
    # entry under a second on the RTX 4090 workstation.
    set(bench_mode_smoke
        "rtty:-6:2"
        "ax25:8:16"
        "pocsag-512:6:12"
        "pocsag-1200:8:14"
        "pocsag-2400:10:16"
        "sitor-b:-5:2"
        "navtex:-4:1"
        "psk31:-10:-2"
        "psk63:-7:0"
        "qpsk31:-10:-2"
        "cw:-8:6"
        "m17:16:22"
        "p25p1:16:26"
        "dstar:16:24"
        "tetra:18:26"
        "dmr:16:24"
    )
    foreach(entry IN LISTS bench_mode_smoke)
        string(REPLACE ":" ";" parts "${entry}")
        list(GET parts 0 mode)
        list(GET parts 1 low)
        list(GET parts 2 high)
        math(EXPR step "${high} - ${low}")
        add_test(
            NAME bench_sweeps_${mode}
            COMMAND bench sweep --mode ${mode}
                    --snr-start ${low} --snr-stop ${high} --snr-step ${step}
                    --trials 4 --seed 7 --quiet
        )
        set_tests_properties(bench_sweeps_${mode} PROPERTIES
            LABELS "tools"
            TIMEOUT 600
            PASS_REGULAR_EXPRESSION "\"decoded_count\": 4,[\r\n ]+\"snr_db\": ${high}\\.000000"
            FAIL_REGULAR_EXPRESSION "\"points\": \\[[\r\n ]+{[\r\n ]+\"ber\": 0\\.000000000000e\\+00"
        )
    endforeach()

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
