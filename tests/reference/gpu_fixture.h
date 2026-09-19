// One Vulkan context per test process.
//
// Device creation costs tens of milliseconds and the validation layers add
// more, so a context per test case would dominate the suite's runtime. The
// context is immutable once created and every test here is read-only with
// respect to it.
//
// The device is chosen by REVENANT_GPU_INDEX, which is how the conformance
// matrix aims the identical suite at each GPU in the machine in turn.
//
// A missing device makes the GPU cases skip rather than killing the process.
// The first version aborted, on the reasoning that a GPU-resident application
// cannot be meaningfully tested without a GPU. That reasoning was half right
// and the implementation was wrong twice over. Aborting also threw away the
// results of every case that needs no GPU at all, the diff harness unit tests
// and the denormal policy tests among them, which are exactly the cases still
// worth running on a machine without Vulkan. And on Windows abort() opens a
// modal dialog, so the process did not even exit: it sat holding a window open
// until somebody clicked it.
//
// Skipping silently has its own failure mode, which is a green CI run on a
// runner whose GPU has quietly stopped working. REVENANT_REQUIRE_GPU closes
// that: CI sets it, and then a missing device is a hard failure with a reason
// instead of a skip.

#pragma once

#include <string>

#include "core/gpu/context.h"

namespace revenant::test {

// True when a context was created. Attempts creation once and caches both the
// outcome and, on failure, why.
[[nodiscard]] bool gpu_available();

// Empty when a device is present. Otherwise the driver's own message.
[[nodiscard]] const std::string& gpu_unavailable_reason();

// Precondition: gpu_available(). Throws otherwise, which Catch2 reports as a
// failed test rather than a dead process.
[[nodiscard]] gpu::Context& shared_context();

// Printed by each GPU case so a CI log says which device produced the result.
// A conformance matrix whose logs do not identify the device is not one.
[[nodiscard]] std::string shared_context_description();

// True when the environment demands a GPU, so its absence is a failure rather
// than a skip. CI sets REVENANT_REQUIRE_GPU=1.
[[nodiscard]] bool gpu_is_required();

// True when this device runs a workgroup-shared-memory transform
// reproducibly: the same kernel, the same input, the same bits, every time.
//
// WHY A TEST SUITE HAS TO MEASURE THIS
//
// A bit-exact diff is a claim about a kernel. It can only be made on a device
// that gives the same answer twice, and one of the two devices in this
// project's conformance matrix does not. Measured on the AMD integrated part:
// core/shaders/pfb_fft.comp, unmodified and at its shipped 64-point size, is
// clean over twelve runs at 1, 4 and 8 workgroups and wrong in 3 of 12 runs
// at 16, 7 of 12 at 32 and 10 of 12 at 64, with the divergence pattern moving
// from run to run. The identity kernel copying 512 KiB through the same
// harness is clean at every size, so it is neither the harness nor the
// memory path; it is workgroup shared memory. The RTX 4090 is clean at every
// size tried.
//
// docs/fft.md predicted exactly this while measuring VkFFT on the same
// device, and recorded one unexplained failure of the channelizer's own
// transform there in 197 runs. This is that, reproduced on demand.
//
// So the probe below is the project's own transform, run repeatedly against
// itself at the largest size any case here dispatches. A case that needs
// bit-exactness skips when the probe fails, which says the device could not
// referee rather than that the kernel is wrong, and it starts running again
// by itself on a driver where shared memory works.
[[nodiscard]] bool shared_memory_is_reproducible();

// What the probe measured, for the skip message. Empty until it has run.
[[nodiscard]] const std::string& shared_memory_report();

}  // namespace revenant::test

// Opens every case that needs a device. Skips when there is none, unless the
// environment requires one, in which case it fails and says why.
#define REVENANT_NEEDS_GPU()                                                            \
    do {                                                                                \
        if (!::revenant::test::gpu_available()) {                                       \
            if (::revenant::test::gpu_is_required()) {                                  \
                FAIL("REVENANT_REQUIRE_GPU is set but no Vulkan device could be used: " \
                     << ::revenant::test::gpu_unavailable_reason());                    \
            }                                                                           \
            SKIP("no Vulkan device: " << ::revenant::test::gpu_unavailable_reason());   \
        }                                                                               \
    } while (false)

// Opens every case that demands bit-exactness from a kernel holding its
// working set in workgroup shared memory. Skips, with the measurement, on a
// device that cannot reproduce one. Not gated on REVENANT_REQUIRE_GPU: the
// device is present and working, it is the shared memory that is not
// reproducible, and a hard failure there would report a driver defect as a
// kernel regression on every run.
#define REVENANT_NEEDS_REPRODUCIBLE_SHARED_MEMORY()                                  \
    do {                                                                             \
        if (!::revenant::test::shared_memory_is_reproducible()) {                    \
            SKIP("this device does not reproduce a workgroup-shared-memory "         \
                 "transform, so it cannot referee a bit-exact claim about one: "     \
                 << ::revenant::test::shared_memory_report());                       \
        }                                                                            \
    } while (false)
