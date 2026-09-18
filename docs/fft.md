# The FFT

Revenant writes its own FFT kernel rather than using VkFFT. This document
exists so the question is not reopened every time someone notices that writing
an FFT is a famously bad idea.

## What the specification said

Two things, and they turned out to be incompatible.

> FFT: VkFFT. Mature, Vulkan-native, benchmarks competitively against cuFFT.
> Do not write this.

> Every GPU block has a bit-exact CPU reference. Float32 recursive filters
> behave differently across vendors and FMA implementations. Without a scalar
> reference and a CI diff suite, Revenant produces confident garbage and nobody
> notices for months.

## Why VkFFT cannot satisfy the second

Measured on the two devices in the development machine, not argued from
principle.

**Same input, same VkFFT version, two GPUs, default configuration: 90.5% of
output floats differ.** The generated kernel calls GLSL `sin()` and `cos()` for
its twiddle factors, and Vulkan permits those several units in the last place.
Each vendor spends them differently. Setting `useLUT = 1` removes the calls and
fixes this for some transform sizes.

**The generated SPIR-V carries zero `NoContraction` decorations.** Revenant's
own `cmul.comp` carries six, and docs/conventions.md explains at length why.
VkFFT hands the driver full licence to fuse and reassociate, which is precisely
the freedom this project's numerical discipline exists to remove.

**It emits different kernels per vendor.** At 256 points the NVIDIA kernel is
`local_size = 32x1` and the AMD one `4x32`, with different shared-memory
strides and a different decomposition. Pinning every device-derived knob does
make the generated GLSL byte-identical across both, verified by hash, so a
build-time SPIR-V set is technically reachable. The outputs still diverge,
because the contraction freedom is below the level any configuration reaches.

The offline-shader rule is the lesser problem and would have been cheap to
amend: VkFFT's runtime compilation costs about 90 ms of one-time glslang
initialisation plus 15 to 21 ms per distinct transform shader, measured. If
bit-exactness had survived, one sentence in the shader rule would have covered
it.

## What "do not write this" was actually about

VkFFT the library is five backends, Bluestein, Rader, four-step multi-upload,
real-to-complex, real-to-real, DCT, DST, convolution, and half through quad
precision. The instruction is right about that library and nobody should write
that.

What the channelizer's M-point stage needs is a batched power-of-two
complex-to-complex transform that fits in shared memory in a single pass. That
is the textbook case, and it is an order of magnitude less work. Treating the
two as the same undertaking is the error the instruction accidentally invites.

## The bar, and why it is reachable

VkFFT runs a 128 MB in-place batched transform at 790 to 800 GB/s on the 4090,
flat across every size from 128 to 16384 points. The card's peak is about
1008 GB/s, so VkFFT sits at roughly 79% of memory bandwidth and is memory-bound
rather than compute-bound at every size the channelizer cares about. A
single-pass kernel that reads once and writes once has a real chance of
matching it, because the ceiling is DRAM and not arithmetic.

If the kernel lands below about 60% of that, this decision is worth reopening.

## What is in scope for the kernel

Power-of-two, complex-to-complex, batched, one workgroup per transform, whole
transform resident in shared memory, twiddles from a host-built table.

Shared memory is the binding limit and it is not uniform: 48 KiB on the
discrete card, 32 KiB on the integrated one. The host discovers it through
`DeviceInfo::max_workgroup_shared_memory` rather than assuming.

Anything outside that envelope, a non-power-of-two length, a prime length, or a
transform too large for shared memory, is not served by this kernel. VkFFT
stays on the shelf, unused, for the day one of those is genuinely needed, and
it would then be taken with an explicitly documented tolerance and a note
saying which block gave up bit-exactness and why.

## An unrelated finding worth keeping

While measuring the above, VkFFT's batched transform turned out not to be
reproducible run to run on the integrated AMD device for transform sizes of
1024 and below: three consecutive runs of the identical binary on identical
input, in one command buffer with explicit barriers, gave three different
answers, wrong by up to 3% of output RMS against a float64 reference. Sizes of
2048 and above are stable on both devices and identical between them. The
discrete card is stable at every size.

The cause was not isolated. It could be VkFFT's index arithmetic or barrier
placement, or this AMD driver miscompiling a correct shader. It matters beyond
VkFFT: if it is the driver, Revenant's own shared-memory kernels will meet the
same thing, and the channelizer's FFT stage is a shared-memory kernel.

### The control experiment, run, and inconclusive

The channelizer's own transform is that control: a hand-written shared-memory
kernel, M = 64, on the same device. It has been run 197 times there since the
output buffers started being zeroed. It failed once, very early, and has not
failed in the 192 runs since. The failure was not captured, so there is no
divergence magnitude to report.

That is too weak to conclude anything and it is recorded rather than
interpreted. One unreproduced event is equally consistent with a rare driver
fault, with residue from the buffer-initialisation bug that was being fixed in
the same minute, and with something not yet imagined. It is written down for
two reasons: the VkFFT observation predicts exactly this, so the next person
who sees an intermittent on that device should know it would not be the first;
and an anomaly nobody records is an anomaly the project gets to be surprised by
twice.

The kernel itself has been checked by hand and is race-free. Every barrier sits
at uniform control flow, and the butterfly index mapping provably partitions
the transform: at stage s the pairs (ia, ib) cover 0..M-1 exactly once, so no
two invocations in a stage touch the same element.

What would settle it: capture a failure with its divergence pattern, run
VkFFT's own test suite on that device, and report upstream. Until then the
honest position is that the discrete card is deterministic across hundreds of
runs and the integrated part has one unexplained event against it.
