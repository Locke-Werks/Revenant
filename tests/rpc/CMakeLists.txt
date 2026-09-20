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
# spectrum cases need a window to subscribe inside, let a callback fall
# behind in, and unsubscribe from, and an unthrottled synthetic source
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
        test_rpc_session.cpp
        test_rpc_spectrum.cpp
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
if(TARGET revenant-engine)
    add_test(
        NAME engined_serves_and_reports_its_port
        COMMAND revenant-engine
                "synthetic:wideband?rate=2400032&emitters=4&seed=4242&samples=4800064"
                --gpu -1
                --spectrum 256
                --block-samples 16384
                --duration 0.5
                --quiet
    )
    set_tests_properties(engined_serves_and_reports_its_port PROPERTIES
        LABELS "rpc"
        TIMEOUT 300
        PASS_REGULAR_EXPRESSION "listening on 127\\.0\\.0\\.1:[0-9]+"
    )
else()
    message(STATUS "revenant-engine is not present yet, skipping its service test")
endif()
