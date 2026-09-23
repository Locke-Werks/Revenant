# The librtlsdr cancel trial. See main.cpp for what it counts and why.
#
# Built two ways, and the second is the reason it links nothing of Revenant's.
#
# In the project, as one of the tools, against the librtlsdr and libusb the
# manifest resolves, so it always measures what would ship.
#
# On its own, against any vcpkg install tree, which is how different builds of
# librtlsdr and libusb are compared without rebuilding the engine for each:
#
#   cmake -S tools/rtlsdr-cancel-trial -B <build dir> -G Ninja
#         -DCMAKE_BUILD_TYPE=RelWithDebInfo
#         -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
#         -DVCPKG_MANIFEST_MODE=OFF
#         -DVCPKG_INSTALLED_DIR=<an install root holding x64-windows-static>
#         -DVCPKG_TARGET_TRIPLET=x64-windows-static
#         -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
#
# from an x64 Native Tools prompt. docs/rtlsdr-provenance.md records the
# install roots the 2026-09-23 comparison used.

cmake_minimum_required(VERSION 3.28)

if(NOT PROJECT_NAME)
    project(rtlsdr_cancel_trial LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(trial_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../..")
else()
    set(trial_repo_root "${CMAKE_SOURCE_DIR}")
endif()

# The fault is in libusb's Windows backend and the parent drives its children
# through the Win32 process API, so there is nothing to build elsewhere.
if(NOT WIN32)
    return()
endif()

find_package(rtlsdr CONFIG REQUIRED)

add_executable(rtlsdr-cancel-trial
    main.cpp
    # A child that faults must die with its exception code rather than sit
    # behind an error dialog, or the parent waits out its timeout and the
    # crash is counted as a hang.
    "${trial_repo_root}/tests/support/no_modal_dialogs.cpp"
)
target_link_libraries(rtlsdr-cancel-trial PRIVATE
    $<IF:$<TARGET_EXISTS:rtlsdr::rtlsdr>,rtlsdr::rtlsdr,rtlsdr::rtlsdr_static>
)

if(COMMAND revenant_apply_warnings)
    target_include_directories(rtlsdr-cancel-trial PRIVATE "${CMAKE_SOURCE_DIR}")
    revenant_apply_warnings(rtlsdr-cancel-trial)
    revenant_embed_version(rtlsdr-cancel-trial DESCRIPTION "Revenant librtlsdr cancel trial")
endif()
