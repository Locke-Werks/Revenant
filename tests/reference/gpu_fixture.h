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

// Empty unless the environment asked for a device it cannot have.
//
// REVENANT_GPU_INDEX that is not a non-negative decimal integer, or an index
// the context did not honour. core/gpu/context.cpp's index_from_environment
// returns an optional, so "1" and "one" and "1; rm -rf" are all indis-
// tinguishable from unset there and selection falls through to "pick the
// best device". On this machine that is the discrete card, which is leg zero,
// so a leg aimed at index 1 by a typo would run the discrete device twice and
// report two green conformance runs.
//
// This is operator error rather than a missing device, so a case that hits it
// FAILS whatever REVENANT_REQUIRE_GPU says. A skip here is the outcome the
// variable exists to prevent.
[[nodiscard]] const std::string& gpu_configuration_error();

// Precondition: gpu_available(). Throws otherwise, which Catch2 reports as a
// failed test rather than a dead process.
[[nodiscard]] gpu::Context& shared_context();

// The device, as a line for a log or a failure message.
//
// Cases pass this to Catch2's INFO, which holds it back until something
// fails, so it identifies the device on a red run and on no other. That is
// the right behaviour for a per-case annotation and it is not a conformance
// record: a matrix whose GREEN logs do not name the device cannot tell a leg
// that ran on the discrete card from a leg that ran on the integrated one
// twice, which is the failure REVENANT_GPU_INDEX exists to prevent.
//
// WHAT THIS PARAGRAPH USED TO SAY: "Printed by each GPU case so a CI log says
// which device produced the result. A conformance matrix whose logs do not
// identify the device is not one." The second sentence is right and the first
// was not: nothing printed it unless a case failed. The unconditional line is
// now emitted once per process by the fixture itself, on the first successful
// device creation. See gpu_fixture.cpp.
[[nodiscard]] std::string shared_context_description();

// True when the environment demands a GPU, so its absence is a failure rather
// than a skip. CI sets REVENANT_REQUIRE_GPU=1.
[[nodiscard]] bool gpu_is_required();

// True when this device is one whose bit-exactness this suite will referee
// for core/shaders/spectrum.comp.
//
// THE PREDICATE, PLAINLY
//
// Device type: discrete, or not. Nothing in this call measures shared memory
// and the name is older than the answer. There is a probe underneath it, six
// rounds of the spectrum kernel over six dispatch shapes with each round
// required to match the shape's own first answer, and that is a backstop
// against a grossly broken driver rather than the gate. The fault this gate
// exists for walks past that probe every time; see gpu_fixture.cpp for why,
// and for the two attempts at a probe that could catch it.
//
// WHAT IS BEING GATED OUT
//
// core/shaders/spectrum.comp disagrees with its CPU twin on the AMD
// integrated part at about one dispatch in 1,900 when each is submitted on
// its own, and at roughly one in nine when twenty-four share a command
// buffer, which is what core/engine/graph.cpp does. The RTX 4090 is exact
// over 200,000 dispatches. pfb_branch, dispatched 40,000 times in the same
// processes on the same device, is exact. Each event is isolated: the shape
// gives the right answer, one wrong answer, and the right answer again, with
// nothing carried forward. docs/fft.md has the sweep, the dissection and
// what would settle it.
//
// WHAT THIS PARAGRAPH USED TO SAY, which is worth keeping because
// gpu_fixture.cpp went on to size a probe from it: that
// core/shaders/pfb_fft.comp on the integrated part is "clean over twelve
// runs at 1, 4 and 8 workgroups and wrong in 3 of 12 runs at 16, 7 of 12 at
// 32 and 10 of 12 at 64"; that an identity kernel through the same harness
// ruled out everything but workgroup shared memory; and that this reproduced
// on demand the VkFFT anomaly docs/fft.md recorded. None of it holds.
// Raising tests/reference/test_pfb.cpp's kBlocks from 8 to 64, with its
// output ring raised to match, gives 0 failures in 12 runs on the integrated
// device and 0 in 12 on the discrete one: the channelizer is reproducible at
// the workgroup counts it ships at. The fault is one kernel and not a
// property of shared memory on that device, which pfb_branch's 40,000 clean
// dispatches settle. The withdrawal was written into test_spectrum.cpp and
// docs/fft.md at the time and not into this header, which every GPU case
// includes, so the dead number stayed in front of every reader while the
// correction sat in two files they had no reason to open.
[[nodiscard]] bool shared_memory_is_reproducible();

// Why the verdict went the way it did, for the skip message. Empty until the
// call above has run.
[[nodiscard]] const std::string& shared_memory_report();

}  // namespace revenant::test

// Opens every case that needs a device. Skips when there is none, unless the
// environment requires one, in which case it fails and says why.
#define REVENANT_NEEDS_GPU()                                                            \
    do {                                                                                \
        if (!::revenant::test::gpu_configuration_error().empty()) {                     \
            FAIL(::revenant::test::gpu_configuration_error());                          \
        }                                                                               \
        if (!::revenant::test::gpu_available()) {                                       \
            if (::revenant::test::gpu_is_required()) {                                  \
                FAIL("REVENANT_REQUIRE_GPU is set but no Vulkan device could be used: " \
                     << ::revenant::test::gpu_unavailable_reason());                    \
            }                                                                           \
            SKIP("no Vulkan device: " << ::revenant::test::gpu_unavailable_reason());   \
        }                                                                               \
    } while (false)

// Opens every case that demands bit-exactness from core/shaders/spectrum.comp.
// Skips, with the reason, on a device this suite does not referee that kernel
// on. Not gated on REVENANT_REQUIRE_GPU: the device is present and working
// and the kernel is not known to be wrong there, so a hard failure would
// report a driver defect as a kernel regression on every run.
//
// The macro's name says shared memory. The predicate no longer does; see
// shared_memory_is_reproducible above. Renaming both is a separate change
// across the files that call it.
#define REVENANT_NEEDS_REPRODUCIBLE_SHARED_MEMORY()                                  \
    do {                                                                             \
        if (!::revenant::test::shared_memory_is_reproducible()) {                    \
            SKIP("this device does not reproduce a workgroup-shared-memory "         \
                 "transform, so it cannot referee a bit-exact claim about one: "     \
                 << ::revenant::test::shared_memory_report());                       \
        }                                                                            \
    } while (false)
