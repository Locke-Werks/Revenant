cmake_minimum_required(VERSION 3.20)
project(lw-radio-dsp VERSION 0.1.0 LANGUAGES CXX)

option(LWDSP_BUILD_TESTS "Build lw-radio-dsp tests" ${PROJECT_IS_TOP_LEVEL})

file(GLOB LWDSP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(lwdsp STATIC ${LWDSP_SOURCES})
add_library(lwdsp::lwdsp ALIAS lwdsp)
target_compile_features(lwdsp PUBLIC cxx_std_20)
target_include_directories(lwdsp
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_target_properties(lwdsp PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(lwdsp PRIVATE /W4 /permissive- /fp:precise)
    # Vendored pocketfft trips these; our own code is held to /W4.
    set_source_files_properties(src/fft.cpp PROPERTIES COMPILE_OPTIONS "/wd4127;/wd4324")
else()
    # No -ffast-math: the noise model must stay bit-identical across builds.
    target_compile_options(lwdsp PRIVATE -Wall -Wextra -ffp-contract=off)
endif()

if(LWDSP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
