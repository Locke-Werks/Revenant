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

## What it actually landed at

Measured 2026-09-18 by `bench throughput`, RTX 4090, a 256 MiB batched
transform per point, median of repeated runs. The bar is VkFFT's 790 GB/s.

| M | transforms | median | GB/s | % of bar |
| --- | --- | --- | --- | --- |
| 64 | 262144 | 498.7 us | 538.3 | 68.1% |
| 128 | 131072 | 528.9 us | 507.5 | 64.2% |
| 256 | 65536 | 545.8 us | 491.8 | 62.3% |
| 1024 | 16384 | 517.1 us | 519.1 | 65.7% |
| 2048 | 8192 | 564.7 us | 475.3 | 60.2% |

**The decision stands and the margin is thinner than it reads.** Every size is
above the 60% line, so the reopen condition has not fired. The largest is at
60.2%, which is not a margin. The grid this engine actually runs is M = 64,
where the kernel reaches 68.1%, so nothing in the current configuration is
near the line; a future decision to run a much finer grid would put it there,
and the number to re-measure before taking that decision is this one.

Two things the measurement says that the estimate did not.

**The transform is not where the channelizer's time goes.** At M = 64 the FFT
stage costs 6.1 us per block against the branch filter's 3.8 us, and the pair
together are 10 us against a per-receiver cost of roughly 18 us each. A
hundred receivers spend 2066 us on receivers and 10 us on the coarse chain.
Optimising the transform further would be optimising three percent of the
block at a hundred receivers.

**The branch filter reads the ring 12 times over and the cache absorbs it.**
At M = 64 it issues 2304 MiB of loads against 192 MiB of unique data, so the
issued rate is 8051 GB/s and the unique rate 671 GB/s. The ratio falls with M,
to 3727 against 311 GB/s at M = 2048, which is the tap count per branch
growing while the reuse window stays the same size. That is a real effect and
it is not in the 790 GB/s comparison, which is about the transform alone.

## What the channelizer costs in a whole block

Same run, 20 MS/s, M = 64, a silent synthetic scene so nothing is squelched.

| receivers | realtime | us per block | coarse us | receivers us | barriers us |
| --- | --- | --- | --- | --- | --- |
| 0 | 39.3x | 83.3 | 10.0 | 0.0 | 0.0 |
| 1 | 31.3x | 104.8 | 10.1 | 17.6 | 0.1 |
| 8 | 16.5x | 198.9 | 10.2 | 146.6 | 0.1 |
| 32 | 4.79x | 684.2 | 10.2 | 591.2 | 0.1 |
| 50 | 3.28x | 998.0 | 10.3 | 928.9 | 0.1 |
| 100 | 1.79x | 1832.3 | 10.2 | 2065.8 | 0.1 |

M1's exit criterion is fifty simultaneous receivers, and fifty runs at 3.28
times realtime with every overrun counter at zero. A hundred still beats
realtime on the discrete card, and on the integrated Radeon a hundred manages
1.07x.

**"Two hundred receivers cost what two cost" is half true, and the measured
half is the important one.** The coarse chain is genuinely flat: 10.0 us at
zero receivers and 10.2 us at a hundred. Channelizing for one receiver and
channelizing for a hundred is the same work, which is the architecture's
central claim and it now has a number under it. What is not free is the
per-receiver fine stage, which is linear at about 18 us and dominates
everything above a handful of receivers.

**The 2N barrier prediction was wrong.** `core/engine/vrx_stage.cpp` predicted
that one `record()` call per receiver would cost two global pipeline flushes
each, that no two receivers would overlap, and that this would start to matter
around fifty receivers. The barrier column is 0.1 us at every receiver count.
The per-receiver cost is the dispatch work itself, so the fix that comment
proposes, batching the phases across receivers so the barriers amortise, would
buy nothing. That comment has been corrected.

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

## The integrated device, and an anomaly that got less deniable

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

### 2026-09-18: a second shared-memory kernel, and it is not the same shape

A spectrum kernel was written, a second-stage transform of each coarse
channel, and on the integrated device it failed three times in twelve runs,
worst divergence 65931 ulp. That is not the one-in-197 above. The same twelve
runs are clean for the channelizer's own transform, clean for everything
outside the spectrum tag, and clean for the spectrum kernel on the discrete
card, so it is that kernel and not the device in general.

The tempting reading is that the driver fault the VkFFT finding predicted has
finally shown up, and that would matter a great deal: it would mean
bit-exactness is unreachable on that device by any route and the argument for
writing our own transform loses its point.

**The sizes say otherwise, and they say it clearly.** VkFFT was unstable at
1024 points and below and stable at 2048 and above. The channelizer's own
transform runs at M = 64, which is inside VkFFT's unstable range, and is
clean. The new spectrum kernel runs at 2048, which is inside VkFFT's stable
range, and is not. The pattern is inverted, so the two are unlikely to share
a cause, and the likely reading is an ordinary defect in a day-old kernel: a
missing barrier, a shared-memory overlap, or a race at a boundary the
channelizer's transform does not have.

That is a prediction rather than a finding, and the way to settle it is to
fix the kernel and see whether the failures go with it. If they do not, this
section becomes the most important one in the document.

The kernel itself has been checked by hand and is race-free. Every barrier sits
at uniform control flow, and the butterfly index mapping provably partitions
the transform: at stage s the pairs (ia, ib) cover 0..M-1 exactly once, so no
two invocations in a stage touch the same element.

What would settle it: capture a failure with its divergence pattern, run
VkFFT's own test suite on that device, and report upstream. Until then the
honest position is that the discrete card is deterministic across hundreds of
runs and the integrated part has one unexplained event against it.
