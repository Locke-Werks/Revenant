// Floating-point discipline for CPU reference implementations.
//
// Include this at the top of every translation unit that implements the scalar
// twin of a GPU kernel, before anything else.
//
// The reference exists to referee a GPU kernel whose float32 arithmetic may
// differ across vendors, drivers and FMA decisions. If the reference is itself
// compiled with contraction enabled, it fuses its own multiply-adds, gives a
// different answer from the same source when built by a different host
// compiler, and the diff suite starts reporting failures that are the harness
// rather than the kernel. A referee that moves is not a referee.
//
// cmake/CompilerFlags.cmake applies the equivalent flags at target level. Both
// exist on purpose: the flag covers the build, the pragma travels with the file
// and survives a target being restructured by someone who does not know why the
// flag was there.

#pragma once

#if defined(_MSC_VER)
// MSVC does not implement the standard FP_CONTRACT pragma. This is its
// spelling, and it must appear before any function definition in the file.
#pragma fp_contract(off)
#elif defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
// GCC honours the standard pragma only with -fno-fast-math, which
// revenant_apply_reference_fp also passes.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma STDC FP_CONTRACT OFF
#pragma GCC diagnostic pop
#endif

namespace revenant::dsp {

// Marker for a translation unit that has taken the pledge above. A reference
// implementation asserts on this so that omitting the include is a compile
// error in the file that forgot, rather than a mystery divergence six months
// later on somebody else's GPU.
inline constexpr bool kReferenceFpDisciplineApplied = true;

}  // namespace revenant::dsp
