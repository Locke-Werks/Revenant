// This include must come first. It carries the pragma that disables
// floating-point contraction for this translation unit, and a pragma that
// appears after a function definition does not apply to it.
#include "core/dsp/reference_fp.h"

#include "core/dsp/complex_ops.h"

#include <format>

#include "core/dsp/denormal_mode.h"

namespace revenant::dsp {
namespace {

static_assert(kReferenceFpDisciplineApplied,
              "core/dsp/reference_fp.h must be included before anything else in a reference "
              "implementation");

}  // namespace

Status reference_copy(ConstRealSpan source, RealSpan destination) {
    if (source.size() != destination.size()) {
        return fail(std::format("reference_copy size mismatch: {} in, {} out", source.size(),
                                destination.size()));
    }

    for (std::size_t i = 0; i < source.size(); ++i) {
        destination[i] = source[i];
    }
    return {};
}

Status reference_cmul(ConstComplexSpan lhs, ConstComplexSpan rhs, ComplexSpan destination) {
    if (lhs.size() != rhs.size() || lhs.size() != destination.size()) {
        return fail(std::format("reference_cmul size mismatch: {}, {}, {}", lhs.size(),
                                rhs.size(), destination.size()));
    }

    // The GPU flushes denormals to zero and cannot be told not to for fp32, so
    // the reference does the same or the two can never agree bit for bit. See
    // core/dsp/denormal_mode.h for why this is the project's policy rather than
    // a workaround for one kernel.
    const ScopedDenormalFlush flush_denormals;

    for (std::size_t i = 0; i < lhs.size(); ++i) {
        // Written out rather than using std::complex's operator*. Two reasons,
        // and both matter for a reference.
        //
        // The operation order has to match core/shaders/cmul.comp exactly:
        // multiply, multiply, subtract. std::complex is free to evaluate its
        // product however the implementation likes, and an implementation that
        // handles the Annex G infinity and NaN cases does extra work with extra
        // rounding that the shader does not do.
        //
        // Second, being explicit here is what makes the comparison auditable. A
        // reader can put these four lines beside the shader and see that they
        // are the same computation.
        const float a_real = lhs[i].real();
        const float a_imag = lhs[i].imag();
        const float b_real = rhs[i].real();
        const float b_imag = rhs[i].imag();

        const float real = a_real * b_real - a_imag * b_imag;
        const float imaginary = a_real * b_imag + a_imag * b_real;

        destination[i] = Complex32{real, imaginary};
    }
    return {};
}

}  // namespace revenant::dsp
