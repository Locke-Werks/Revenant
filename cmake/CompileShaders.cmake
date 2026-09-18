# Offline SPIR-V. Shaders are compiled, validated and embedded at build time.
#
# Runtime compilation is a startup cost and a failure mode: a shader that fails
# to compile on a user's driver is a crash on their machine instead of a red
# build on ours. Embedding the words in the binary also removes a class of
# deployment bug where the executable ships without its shader directory.
#
# Three steps per shader, deliberately:
#   1. glslangValidator produces the SPIR-V.
#   2. spirv-val checks it against the target environment. glslang will happily
#      emit modules that violate Vulkan's validation rules; catching that here
#      is much cheaper than catching it in a validation layer at runtime.
#   3. glslangValidator runs again with --vn to emit the same module as a
#      uint32_t array. Compiling twice costs milliseconds and keeps the
#      validated artefact and the embedded artefact byte-identical by
#      construction rather than by a conversion script we would have to trust.

find_program(REVENANT_GLSLANG
    NAMES glslangValidator
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
    DOC "glslangValidator from the Vulkan SDK"
)
find_program(REVENANT_SPIRV_VAL
    NAMES spirv-val
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
    DOC "spirv-val from the Vulkan SDK"
)

if(NOT REVENANT_GLSLANG OR NOT REVENANT_SPIRV_VAL)
    message(FATAL_ERROR
        "glslangValidator and spirv-val are required and were not found. "
        "Install the LunarG Vulkan SDK and ensure VULKAN_SDK is set.")
endif()

# revenant_add_shaders(<target> TARGET_ENV <env> SHADERS <file> [<file>...])
#
# Each <name>.comp becomes generated/shaders/<name>_comp.h declaring
# <name>_comp_spv. The generated directory's parent is added to the target's
# include path, so a consumer writes #include "shaders/<name>_comp.h".
function(revenant_add_shaders target)
    cmake_parse_arguments(ARG "" "TARGET_ENV" "SHADERS" ${ARGN})

    if(NOT ARG_TARGET_ENV)
        set(ARG_TARGET_ENV "vulkan1.3")
    endif()

    set(gen_root "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(gen_dir "${gen_root}/shaders")
    file(MAKE_DIRECTORY "${gen_dir}")

    set(generated_headers "")

    foreach(src IN LISTS ARG_SHADERS)
        get_filename_component(src_abs "${src}" ABSOLUTE)
        get_filename_component(stem "${src}" NAME_WE)
        get_filename_component(suffix "${src}" LAST_EXT)
        string(REPLACE "." "" suffix "${suffix}")

        set(symbol "${stem}_${suffix}_spv")
        set(spv "${gen_dir}/${stem}_${suffix}.spv")
        set(hdr "${gen_dir}/${stem}_${suffix}.h")

        add_custom_command(
            OUTPUT "${hdr}" "${spv}"
            COMMAND "${REVENANT_GLSLANG}" -V --target-env ${ARG_TARGET_ENV}
                    -o "${spv}" "${src_abs}"
            COMMAND "${REVENANT_SPIRV_VAL}" --target-env ${ARG_TARGET_ENV} "${spv}"
            COMMAND "${REVENANT_GLSLANG}" -V --target-env ${ARG_TARGET_ENV}
                    --vn ${symbol} -o "${hdr}" "${src_abs}"
            MAIN_DEPENDENCY "${src_abs}"
            COMMENT "Compiling, validating and embedding ${stem}${suffix}"
            VERBATIM
        )

        list(APPEND generated_headers "${hdr}")
    endforeach()

    add_custom_target(${target}_shaders DEPENDS ${generated_headers})
    add_dependencies(${target} ${target}_shaders)
    target_include_directories(${target} PUBLIC "${gen_root}")
endfunction()
