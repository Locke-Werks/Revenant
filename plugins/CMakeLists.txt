# Engine plugins that ship with Revenant, none at present. Each is a DLL built
# against core/plugin/engine_plugin_abi.h alone: none links revenant_core, which
# is the point, since a plugin written outside this tree cannot either.
# docs/plugins.md is the guide. Declare one here with
# revenant_engine_plugin(<name> <name>/<name>.cpp).
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
