// One Vulkan context per test process.
//
// Device creation costs tens of milliseconds and the validation layers add
// more, so a context per test case would dominate the suite's runtime. The
// context is immutable once created and every test here is read-only with
// respect to it.
//
// The device is chosen by REVENANT_GPU_INDEX, which is how the conformance
// matrix aims the identical suite at each GPU in the machine in turn.

#pragma once

#include <string>

#include "core/gpu/context.h"

namespace revenant::test {

// Fails the process with a clear message if no Vulkan device is present.
// Revenant is a GPU-resident application: a machine that cannot create a
// compute context cannot run a meaningful subset of this suite, and silently
// skipping would report a green build that proved nothing.
[[nodiscard]] gpu::Context& shared_context();

// Printed once at the start of a run so a CI log says which device produced
// the result. A conformance matrix whose logs do not identify the device is
// not a conformance matrix.
[[nodiscard]] std::string shared_context_description();

}  // namespace revenant::test
