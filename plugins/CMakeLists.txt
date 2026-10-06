# Engine plugins that ship with Revenant. Each is a DLL built against
# core/plugin/engine_plugin_abi.h alone: none links revenant_core, which is the
# point, since a plugin written outside this tree cannot either. docs/plugins.md
# is the guide and p25trunk/p25trunk.cpp the worked example.
#
# Each lands in "plugins" beside revenant-engine.exe, where the engine scans at
# startup, so a build runs with them loaded and no copy step.

function(revenant_engine_plugin target)
    add_library(${target} SHARED ${ARGN})
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}")
    revenant_apply_warnings(${target})
    revenant_embed_version(${target} DESCRIPTION "Revenant engine plugin ${target}")
    if(TARGET revenant-engine)
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:revenant-engine>/plugins")
    endif()
endfunction()

revenant_engine_plugin(p25trunk p25trunk/p25trunk.cpp)
