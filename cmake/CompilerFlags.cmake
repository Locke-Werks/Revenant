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

            # C4062: a switch over an enum that omits an enumerator and has no
            # default label. Off by default at /W4, which is not obvious and
            # has already been got wrong here: a comment in core/rpc/convert.cpp
            # claimed an exhaustive switch made a new demodulator a build error
            # and it did not. Measured 2026-09-19 with MSVC 19.44.35228 and
            # these exact flags: the unhandled enumerator compiled clean and
            # silent, and only /w14062 produced the diagnostic.
            #
            # On at level 1 rather than 4 so it survives any target that ever
            # drops below /W4. The GCC and Clang branch below gets this from
            # -Wswitch inside -Wall, which is why there is no counterpart there.
            #
            # Know the boundary before trusting it, because the mistake this
            # option exists to correct was trusting a guard that was not there.
            # C4062 fires only on a switch with NO default label. Measured the
            # same day with the same compiler: a switch that omits an
            # enumerator but carries a default stays silent under /w14062, and
            # C4061 is the warning that owns that shape. C4061 is deliberately
            # not on. It fires on every subset switch by design, and in this
            # tree the only sites it reaches are core/gpu/context.cpp's
            # switches over VkResult and VkPhysicalDeviceType, open vendor
            # enums with MAX_ENUM sentinels where a default is the correct
            # answer and the warning would be pure noise.
            #
            # So: a switch that must be exhaustive writes no default, and a
            # new enumerator then raises C4062 at warning level 1. A WARNING,
            # not an error. Nothing in this file makes any warning fatal
            # except the /WX two lines below, and only CI sets the variable
            # behind it; on a developer's machine an unhandled enumerator
            # scrolls past with the rest of the build output. A switch that
            # legitimately handles a subset writes a default that does
            # something defensible, and nothing checks it for you. /wd4062 is
            # not the answer to either and is not used anywhere in this tree.
            #
            # WHAT THIS PARAGRAPH USED TO SAY
            #
            # Until 2026-09-20 it said this option "makes a new enumerator a
            # build error", forty lines under the line at the top of this file
            # saying /WX is CI only, so the file contradicted itself. It is
            # also the exact sentence docs/conventions.md corrected the same
            # day, and that section now points HERE as the authority. A reader
            # who followed the pointer to check the corrected claim landed on
            # the uncorrected copy of it and went away expecting their own
            # build to stop on a new enumerator. It does not.
            #
            # Retracted in place rather than quietly swapped, because the
            # defect this option exists to correct was a comment promising a
            # guard that was not switched on, and the comment introducing it
            # made that same mistake one round later.
            /w14062

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
