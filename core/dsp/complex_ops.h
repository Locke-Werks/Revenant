// Scalar CPU reference implementations.
//
// Every GPU kernel in Revenant has a twin here producing bit-identical output
// for identical input. These are never called on the sample path: they exist so
// that CI can prove the kernel that is on the sample path computes what it
// claims to, on every vendor's driver.
//
// The rule that makes them worth having is that they are written to match the
// kernel's operation order exactly, not merely its mathematical result. See
// core/dsp/reference_fp.h.

#pragma once

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::dsp {

// Twin of core/shaders/identity.comp.
[[nodiscard]] Status reference_copy(ConstRealSpan source, RealSpan destination);

// Twin of core/shaders/cmul.comp. Element-wise complex multiply, the inner
// operation of the per-VRX fine-tune mixer.
[[nodiscard]] Status reference_cmul(ConstComplexSpan lhs,
                                    ConstComplexSpan rhs,
                                    ComplexSpan destination);

}  // namespace revenant::dsp
