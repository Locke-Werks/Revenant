cmake_minimum_required(VERSION 3.28)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(RevenantVersion)

project(revenant
    VERSION ${REVENANT_VERSION}
    DESCRIPTION "A GPU-resident software defined radio application"
    LANGUAGES CXX
)

# The resource compiler, for the icon and VERSIONINFO on Windows binaries.
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC 19.44 has no stable /std:c++23; CMake resolves the standard above to
# -std:c++latest, which is what provides std::expected, std::mdspan and
# std::print. Nothing here uses C++20 modules, and leaving the scan on costs a
# dependency-scanning pass over every translation unit for nothing.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(REVENANT_BUILD_TESTS "Build the test suite" ON)
option(REVENANT_BUILD_TOOLS "Build siggen and bench" ON)
option(REVENANT_BUILD_UI "Build the Qt/QML client" OFF)
option(REVENANT_WERROR "Treat warnings as errors" OFF)
option(REVENANT_ASSERT_NO_QT "Fail the configure if anything in core references Qt" OFF)
option(REVENANT_BUILD_DEVICE_SPIKE "Build the M0.7 libusb device-path spike" OFF)
option(REVENANT_BUILD_GPU_STRESS "Build the cross-dispatch determinism diagnostic" OFF)

include(CompilerFlags)
include(CompileShaders)
include(EmbedVersion)

find_package(Vulkan REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)

add_subdirectory(core)

# The headless requirement, checked rather than asserted.
#
# The engine is headless and the GUI is one RPC client among several. That
# property buys remote operation, multi-user and a testable core, and it is
# only ever true if it is true from the start: a Qt dependency that creeps into
# core is not noticed until someone tries to run the engine on a box with no
# display, by which time it is load-bearing.
#
# Two checks, because they catch different mistakes. The source scan catches a
# developer including a Qt header. The target check catches a CMakeLists
# calling find_package(Qt6) inside the core tree.
if(REVENANT_ASSERT_NO_QT)
    file(GLOB_RECURSE revenant_core_sources
        "${CMAKE_CURRENT_SOURCE_DIR}/core/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp"
    )
    foreach(source IN LISTS revenant_core_sources)
        file(READ "${source}" contents)
        if(contents MATCHES "#[ \t]*include[ \t]*[<\"]Q" OR contents MATCHES "#[ \t]*include[ \t]*[<\"]qt")
            message(FATAL_ERROR
                "core must not depend on Qt, and ${source} includes a Qt header. "
                "The engine is headless; the GUI is a client. See docs/conventions.md.")
        endif()
    endforeach()

    if(TARGET Qt6::Core OR DEFINED Qt6_FOUND)
        message(FATAL_ERROR
            "core must not depend on Qt, and something under core/ called find_package(Qt6).")
    endif()

    message(STATUS "Headless check passed: core references no Qt")
endif()

# Guarded so a checkout part-way through M0 still configures. Once both tools
# exist the guards are noise and come out.
if(REVENANT_BUILD_TOOLS)
    foreach(tool IN ITEMS siggen bench devices cli engined)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/${tool}/CMakeLists.txt")
            add_subdirectory(tools/${tool})
        else()
            message(STATUS "tools/${tool} is not present yet, skipping")
        endif()
    endforeach()
endif()

# A one-off, not a component. See tools/devicespike/main.cpp for what it does
# and, more importantly, for what it deliberately does not do.
if(REVENANT_BUILD_DEVICE_SPIKE)
    add_subdirectory(tools/devicespike)
endif()

# Also a one-off. It reproduces the fault docs/fft.md records, which is what
# gates re-enabling spectrum conformance on the integrated device.
if(REVENANT_BUILD_GPU_STRESS)
    add_subdirectory(tools/gpustress)
endif()

if(REVENANT_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()

# The refusal is permanent, and the reason it gives changed on 2026-09-20. It
# used to say the QML client arrives at M2, which was true when written and
# would now send a reader looking for something that is already built. ui/ is
# its own CMake project against x64-windows and the dynamic CRT; this cache
# holds x64-windows-static and MultiThreaded, and one cache cannot hold both.
if(REVENANT_BUILD_UI)
    message(FATAL_ERROR
        "REVENANT_BUILD_UI cannot build the client from this tree. ui/ is its "
        "own CMake project against x64-windows and the dynamic CRT, and this "
        "cache holds x64-windows-static and MultiThreaded. Configure ui/ "
        "separately: cmake --preset vs from inside ui/. See docs/building.md.")
endif()

message(STATUS "Revenant ${REVENANT_VERSION}")
message(STATUS "  Vulkan   ${Vulkan_VERSION} at ${Vulkan_LIBRARY}")
message(STATUS "  tests    ${REVENANT_BUILD_TESTS}")
message(STATUS "  tools    ${REVENANT_BUILD_TOOLS}")
