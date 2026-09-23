# The Cap'n Proto service, exercised over a real socket.
#
# Every case binds an ephemeral loopback port, connects a real client to it
# and drives a real engine through the wire. There is no fake server and no
# fake engine, because the defects this layer can have live in the seams
# rather than in the arithmetic: a field dropped in conversion, a frequency
# rounded to an integer hertz, a frame queued instead of dropped, a
# subscription that outlives the client that made it. A case that called the
# conversion functions directly would see none of them.
#
# It needs a GPU, and takes the shared fixture from tests/reference so that
# REVENANT_GPU_INDEX and REVENANT_REQUIRE_GPU mean the same thing here as in
# every other binary the conformance matrix runs. The two cases that are
# about the client's own argument checking need no device and say so.
#
# The suite runs the engine at pace = 1, which nothing else here does. The
# spectrum and audio cases need a window to subscribe inside, let a callback
# fall behind in, and unsubscribe from, and an unthrottled synthetic source
# retires in tens of milliseconds. That is what the timeouts below are sized
# against.
#
# Linking revenant_rpc_server rather than revenant_rpc_client: this side of
# the split may see the engine, and the cases compare what came off the wire
# against what the engine actually holds, which is the only way to say a
# rational survived rather than that both ends agree about a rounded one.

if(TARGET revenant_rpc_server)
    add_executable(revenant_rpc_tests
        ../support/no_modal_dialogs.cpp
        ../reference/gpu_fixture.cpp
        rpc_fixture.cpp
        test_rpc_audio.cpp
        test_rpc_auth.cpp
        test_rpc_decode.cpp
        test_rpc_detect.cpp
        test_rpc_lifetime.cpp
        test_rpc_rds.cpp
        test_rpc_session.cpp
        test_rpc_spectrum.cpp
        test_rpc_token.cpp
    )

    target_link_libraries(revenant_rpc_tests PRIVATE
        revenant_rpc_server
        Catch2::Catch2WithMain
    )
    target_include_directories(revenant_rpc_tests PRIVATE "${CMAKE_SOURCE_DIR}")

    revenant_apply_warnings(revenant_rpc_tests)
    revenant_apply_reference_fp(revenant_rpc_tests)

    catch_discover_tests(revenant_rpc_tests
        PROPERTIES
            LABELS "rpc"
            TIMEOUT 300
    )
else()
    message(STATUS "revenant_rpc_server is not present yet, skipping the RPC tests")
endif()

# revenant-engine itself, run as a program.
#
# Registered as a CTest entry rather than as a C++ case because what is under
# test is the program: it parses a command line, builds an engine, binds a
# port and prints it, serves until --duration is reached, and exits zero. The
# port line is the one a supervisor parses to find an ephemeral port, so it
# is matched rather than assumed, and it is the thing that would silently
# stop being printed.
#
# --duration rather than an interrupt. The console handler is the one path
# CTest cannot drive: sending a real Ctrl-C to a child would reach the whole
# process group, this runner included.
#
# --token-file into the build tree, because the default is
# %LOCALAPPDATA%\Revenant\rpc-token and a test suite has no business minting a
# token into a real user profile and leaving it there. It also keeps this
# entry from sharing a file with an engine the operator is actually running.
#
# BOTH REGEXES, AND THE SECOND ONE IS WHY THIS ENTRY MEANS ANYTHING.
#
# Setting PASS_REGULAR_EXPRESSION tells CTest to decide the verdict from the
# output alone: the exit code stops being consulted at all. The port line is
# printed within the first few milliseconds, so with only a pass regex this
# entry passes on any run that gets as far as binding, whatever the engine
# does afterwards. Everything this program exists to exercise past that
# point, the serve loop, the shutdown path and every error main.cpp
# classifies, would return 1 to a runner that had already made up its mind.
#
# FAIL_REGULAR_EXPRESSION is honoured alongside it and wins, so the failure
# prefix main.cpp prints to stderr before returning 1 is what closes the gap.
# Matching the message rather than the exit code is not a workaround here: the
# exit code is unavailable by construction once a pass regex is set, and the
# prefix is the one string every one of those paths shares.
#
# --port 0 EXPLICITLY, because the program's own default is now 17690, which
# is revenant-ui's. A test binding that would fail on the machine this suite is
# usually run on, where an operator's engine is holding it, and would take the
# port from under that engine on a machine where it is not yet running. Passing
# zero also keeps the one path that makes two engines on a machine possible
# covered, so an explicit --port 0 cannot quietly start meaning the default.
if(TARGET revenant-engine)
    add_test(
        NAME engined_serves_and_reports_its_port
        COMMAND revenant-engine
                "synthetic:wideband?rate=2400032&emitters=4&seed=4242&samples=4800064"
                --gpu -1
                --port 0
                --spectrum 256
                --block-samples 16384
                --duration 0.5
                --quiet
                --token-file "${CMAKE_CURRENT_BINARY_DIR}/engined-test-token"
    )
    set_tests_properties(engined_serves_and_reports_its_port PROPERTIES
        LABELS "rpc"
        TIMEOUT 300
        PASS_REGULAR_EXPRESSION "listening on 127\\.0\\.0\\.1:[0-9]+"
        FAIL_REGULAR_EXPRESSION "revenant-engine: |listening on 127\\.0\\.0\\.1:17690"
    )

    # The default itself, read off the usage text rather than by binding it,
    # for the collision reason above. The usage prints the same constant the
    # parser defaults to, so this fails if either moves without the other.
    # No FAIL_REGULAR_EXPRESSION: the usage text opens with "revenant-engine: "
    # itself, which is the failure prefix the entry above matches.
    add_test(
        NAME engined_defaults_to_the_ui_port
        COMMAND revenant-engine --help
    )
    set_tests_properties(engined_defaults_to_the_ui_port PROPERTIES
        LABELS "rpc"
        TIMEOUT 60
        PASS_REGULAR_EXPRESSION "--port <n> +Port, default 17690,"
    )
else()
    message(STATUS "revenant-engine is not present yet, skipping its service test")
endif()
