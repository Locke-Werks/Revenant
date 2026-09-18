find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

# The reference diff suite. This is the target CI cares about most: it is what
# stands between a kernel that computes the wrong thing and six months of
# confident garbage.
add_executable(revenant_reference_tests
    support/no_modal_dialogs.cpp
    reference/gpu_fixture.cpp
    reference/test_context.cpp
    reference/test_cmul.cpp
    reference/test_convert.cpp
    reference/test_denormal_mode.cpp
    reference/test_divergence.cpp
    reference/test_pfb.cpp
    reference/test_vrx.cpp
)

target_link_libraries(revenant_reference_tests PRIVATE
    revenant_core
    Catch2::Catch2WithMain
)

target_include_directories(revenant_reference_tests PRIVATE "${CMAKE_SOURCE_DIR}")

# The deliberately broken kernel lives with the test that uses it and is never
# linked into the engine.
revenant_add_shaders(revenant_reference_tests
    TARGET_ENV vulkan1.3
    SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/reference/shaders/cmul_wrong.comp"
)

revenant_apply_warnings(revenant_reference_tests)
revenant_apply_reference_fp(revenant_reference_tests)

catch_discover_tests(revenant_reference_tests
    PROPERTIES
        LABELS "reference"
        TIMEOUT 300
)

# Tool suites, added as each tool lands. Guarded so that a checkout mid-way
# through M0 still configures.
add_subdirectory(engine)

if(REVENANT_BUILD_TOOLS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/CMakeLists.txt")
    add_subdirectory(tools)
endif()
