#include "core/dsp/denormal_mode.h"

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#define REVENANT_X86_DENORMAL_CONTROL 1
// pmmintrin brings in the SSE3-era denormals-are-zero control alongside the
// SSE2 flush-to-zero one. Both are needed: flush-to-zero handles denormal
// results, denormals-are-zero handles denormal inputs, and the GPU does both.
#include <pmmintrin.h>
#include <xmmintrin.h>
#else
#define REVENANT_X86_DENORMAL_CONTROL 0
#endif

namespace revenant::dsp {
namespace {

#if REVENANT_X86_DENORMAL_CONTROL
constexpr unsigned int kFlushToZero = 0x8000U;
constexpr unsigned int kDenormalsAreZero = 0x0040U;
constexpr unsigned int kBoth = kFlushToZero | kDenormalsAreZero;
#endif

}  // namespace

ScopedDenormalFlush::ScopedDenormalFlush() {
#if REVENANT_X86_DENORMAL_CONTROL
    saved_state_ = _mm_getcsr();
    restore_ = true;
    _mm_setcsr(saved_state_ | kBoth);
#endif
}

ScopedDenormalFlush::~ScopedDenormalFlush() {
#if REVENANT_X86_DENORMAL_CONTROL
    if (restore_) {
        _mm_setcsr(saved_state_);
    }
#endif
}

bool denormals_are_flushed() {
#if REVENANT_X86_DENORMAL_CONTROL
    return (_mm_getcsr() & kBoth) == kBoth;
#else
    // AArch64 carries the equivalent control in FPCR bit 24. Apple Silicon is
    // a target from M2 onwards and this gets implemented then rather than
    // guessed at now. Returning false is the honest answer: the policy is not
    // in force on this architecture yet, and a test asserting it will say so
    // instead of passing vacuously.
    return false;
#endif
}

}  // namespace revenant::dsp
