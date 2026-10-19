# libhackrf alone, for the hackrf overlay port.
#
# Upstream's host/libhackrf/CMakeLists.txt builds both linkages at once and on
# Windows installs the import library, the static library and the DLL together
# into bin/, which vcpkg's post-build checks reject. Its build files are also
# GPL-2.0, where hackrf.c and hackrf.h are BSD-3-Clause. This file replaces it,
# so the only upstream files the port compiles are those two.

cmake_minimum_required(VERSION 3.20)
project(libhackrf LANGUAGES C VERSION 0.9.2)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
if(WIN32 AND NOT MINGW)
    find_package(PThreads4W REQUIRED)
else()
    find_package(Threads REQUIRED)
endif()

add_library(hackrf host/libhackrf/src/hackrf.c)
target_compile_definitions(hackrf PRIVATE
    LIBRARY_VERSION="${PROJECT_VERSION}"
    LIBRARY_RELEASE="${HACKRF_RELEASE}")
target_include_directories(hackrf PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/host/libhackrf/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/libhackrf>)
# The resolved library paths rather than PkgConfig::LIBUSB, the way the
# rtlsdr overlay does it, so the exported targets need no pkg-config in the
# consuming project. vcpkg_cmake_config_fixup rewrites the paths.
target_include_directories(hackrf PRIVATE ${LIBUSB_INCLUDE_DIRS})
target_link_libraries(hackrf PRIVATE ${LIBUSB_LINK_LIBRARIES})
if(TARGET PThreads4W::PThreads4W)
    target_link_libraries(hackrf PRIVATE PThreads4W::PThreads4W)
else()
    target_link_libraries(hackrf PRIVATE Threads::Threads)
endif()

install(TARGETS hackrf EXPORT HackRFTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES host/libhackrf/src/hackrf.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libhackrf)
install(EXPORT HackRFTargets NAMESPACE HackRF:: DESTINATION share/hackrf)
install(FILES hackrf-config.cmake DESTINATION share/hackrf)
