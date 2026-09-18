# The house warning set, matching Forge/cmake/LwiWarnings.cmake and
# DeadLetter/cmake/CompilerFlags.cmake.
#
# /WX is off by default and on in CI via -DREVENANT_WERROR=ON. CI is where a new
# warning should stop the line, not a developer's machine.

function(revenant_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /Zc:inline
            /Zc:throwingNew
            /utf-8
            /EHsc
            $<$<BOOL:${REVENANT_WERROR}>:/WX>
        )

        target_compile_definitions(${target} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            UNICODE
            _UNICODE
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow -Wconversion -Wsign-conversion
            -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual
            -Wformat=2
            $<$<BOOL:${REVENANT_WERROR}>:-Werror>
        )

        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE
                -Wduplicated-cond -Wduplicated-branches -Wlogical-op
            )
        endif()
    endif()
endfunction()

# Floating-point settings for a CPU reference twin.
#
# This is load-bearing, not hygiene. The reference exists to catch a GPU kernel
# whose float32 arithmetic diverges across vendors. If the reference is itself
# compiled with contraction enabled, it fuses its own multiply-adds, produces a
# different answer from the same source on a different host compiler, and the
# diff suite starts reporting failures that are the harness rather than the
# kernel. A reference that is not reproducible cannot referee anything.
#
# Speed is irrelevant here: the reference is never in the sample path.
#
# The flags cover command-line defaults. Every reference translation unit also
# opens with an explicit pragma, because a target-level flag is one careless
# refactor away from being dropped and the pragma travels with the file. See
# core/dsp/reference_fp.h.
function(revenant_apply_reference_fp target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /fp:precise)
    else()
        target_compile_options(${target} PRIVATE -ffp-contract=off -fno-fast-math)
    endif()
endfunction()
