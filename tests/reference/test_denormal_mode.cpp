// The denormal policy, asserted rather than assumed.
//
// Bit-exact agreement between the GPU kernels and their CPU twins depends on
// both sides flushing denormals. The GPU does it whether we like it or not; the
// CPU does it only because ScopedDenormalFlush says so. If that guard ever
// stops working, every reference diff starts failing at once and the cause
// looks like an arithmetic bug in whichever kernel is touched next. These cases
// make the real cause name itself.

#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "core/dsp/denormal_mode.h"

using namespace revenant;

TEST_CASE("the denormal guard takes effect and then restores", "[reference][m0.2]") {
    const bool before = dsp::denormals_are_flushed();

    {
        const dsp::ScopedDenormalFlush flush;
        CHECK(dsp::denormals_are_flushed());
    }

    // Scoped, not global. A reference function must not leave the calling
    // thread in a different floating-point mode than it found it, or a caller's
    // unrelated arithmetic silently changes behaviour depending on whether a
    // reference happened to run first.
    CHECK(dsp::denormals_are_flushed() == before);
}

TEST_CASE("denormal arithmetic actually flushes under the guard", "[reference][m0.2]") {
    // Checking the mode bit proves the register was written. This proves the
    // hardware honours it, which is the part that matters.
    constexpr float kSmallestNormal = std::numeric_limits<float>::min();

    // Volatile so the optimiser cannot fold these at compile time, where the
    // MXCSR mode does not apply and the answer would be whatever the compiler's
    // constant folder decided.
    volatile float numerator = kSmallestNormal;
    volatile float divisor = 4.0F;

    {
        const dsp::ScopedDenormalFlush flush;
        const float flushed = numerator / divisor;
        // A quarter of the smallest normal is a denormal, and flush-to-zero
        // turns it into exactly zero.
        CHECK(flushed == 0.0F);
    }
}

TEST_CASE("the guard nests without losing the outer state", "[reference][m0.2]") {
    const dsp::ScopedDenormalFlush outer;
    REQUIRE(dsp::denormals_are_flushed());

    {
        const dsp::ScopedDenormalFlush inner;
        CHECK(dsp::denormals_are_flushed());
    }

    // The inner guard restores what it found, which was already flushing. A
    // naive implementation that restored to a hardcoded default rather than to
    // the saved state would turn flushing off here and break the outer scope.
    CHECK(dsp::denormals_are_flushed());
}
