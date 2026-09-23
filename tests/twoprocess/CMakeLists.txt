# The two-process smoke test: revenant-engine from the root tree, /MT, and a
# client built here /MD, each in its own process.
#
# A CMAKE PROJECT OF ITS OWN, FOR THE REASON ui/ IS ONE. The root tree is built
# against the static triplet and the static CRT and refuses a target that wants
# the dynamic one, because one cache cannot hold both runtimes. The client half
# has to be /MD to be the thing under test, so it cannot be a target in
# tests/CMakeLists.txt. It is not a target in ui/CMakeLists.txt either, which
# already compiles core/rpc/client.cpp /MD, because ui/ belongs to the client
# and this is a test of the wire.
#
# The engine is an input, not a target: REVENANT_ENGINE names a
# revenant-engine.exe built by the root tree. In CI that is the one the
# build-and-test job staged into the payload, so the test runs the binary that
# ships. See docs/building.md for running it by hand.

cmake_minimum_required(VERSION 3.28)

project(revenant_twoprocess
    DESCRIPTION "revenant-engine and a /MD client, in two processes"
    LANGUAGES CXX
)

get_filename_component(REVENANT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(REVENANT_ENGINE "${REVENANT_SOURCE_DIR}/build/ci/tools/engined/revenant-engine.exe"
    CACHE FILEPATH "revenant-engine.exe built by the root tree")

option(REVENANT_WERROR "Treat warnings as errors" OFF)
list(APPEND CMAKE_MODULE_PATH "${REVENANT_SOURCE_DIR}/cmake")
include(CompilerFlags)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

find_package(CapnProto CONFIG REQUIRED)

set(CAPNPC_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(CAPNPC_SRC_PREFIX "${REVENANT_SOURCE_DIR}")
file(MAKE_DIRECTORY "${CAPNPC_OUTPUT_DIR}/core/rpc")
capnp_generate_cpp(revenant_capnp_sources revenant_capnp_headers
    "${REVENANT_SOURCE_DIR}/core/rpc/revenant.capnp")

# The same two sources ui/CMakeLists.txt compiles into revenant-ui, built the
# same way, so a change that breaks the client's /MD build breaks this first.
add_executable(rpc_smoke_client
    main.cpp
    "${REVENANT_SOURCE_DIR}/tests/support/no_modal_dialogs.cpp"
    "${REVENANT_SOURCE_DIR}/core/rpc/client.cpp"
    "${REVENANT_SOURCE_DIR}/core/rpc/token.cpp"
    ${revenant_capnp_sources}
)
target_include_directories(rpc_smoke_client PRIVATE
    "${REVENANT_SOURCE_DIR}"
    "${CAPNPC_OUTPUT_DIR}"
)
# bcrypt for token.cpp's BCryptGenRandom, which ui/ gets through Qt's own
# imports and a bare client does not.
target_link_libraries(rpc_smoke_client PRIVATE CapnProto::capnp-rpc bcrypt)
revenant_apply_warnings(rpc_smoke_client)

enable_testing()

find_program(REVENANT_PWSH NAMES pwsh powershell REQUIRED)

if(NOT EXISTS "${REVENANT_ENGINE}")
    # Loud rather than a test that skips. A smoke test with no engine to start
    # has nothing to say, and a green run without it would be a lie.
    message(WARNING
        "REVENANT_ENGINE is '${REVENANT_ENGINE}' and nothing is there. Build the root "
        "tree's ci preset first, or pass -DREVENANT_ENGINE=<path to revenant-engine.exe>. "
        "The test is registered anyway and will fail saying the same.")
endif()

add_test(
    NAME two_process_smoke
    COMMAND "${REVENANT_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${REVENANT_SOURCE_DIR}/scripts/two-process-smoke.ps1"
            -Engine "${REVENANT_ENGINE}"
            -Client "$<TARGET_FILE:rpc_smoke_client>"
            -Work "${CMAKE_CURRENT_BINARY_DIR}/smoke"
)
set_tests_properties(two_process_smoke PROPERTIES
    LABELS "twoprocess"
    TIMEOUT 300
    PASS_REGULAR_EXPRESSION "two-process smoke: ok"
)
