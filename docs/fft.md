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

### 2026-09-18: the spectrum kernel, and what it turned out to be

A spectrum kernel was written, a second-stage transform of each coarse
channel, and on the integrated device it failed a few runs in twelve. The
first reading was that the driver fault this section predicted had finally
arrived, which would have mattered: it would mean bit-exactness is
unreachable on that device by any route, and the argument for writing our own
transform loses its point.

It is not that, and the measurements are worth keeping because two of them
contradict things that were written down confidently.

**The channelizer is fine at the workgroup counts it ships at.** A comment in
the spectrum test file claimed `pfb_fft.comp` was clean at 1, 4 and 8
workgroups and wrong in 3 of 12 runs at 16, 7 of 12 at 32 and 10 of 12 at 64.
Raising `test_pfb.cpp`'s `kBlocks` from 8 to 64, with its output ring raised
to match, gives **0 failures in 12 runs on the integrated device and 0 in 12
on the discrete one**. That claim is withdrawn. It had been used to justify
pinning the spectrum tests to an eight-channel grid, which is how a kernel
went a day without any test dispatching the configuration the engine runs.

**The shipped configuration is clean.** Sixty-four channels at every
workgroup width the graph can pick, including the 256 it actually dispatches,
is exact on every run on both devices.

That last sentence held for twelve runs and does not hold for twenty
thousand. It is corrected below, on the discrete device in its favour and on
the integrated one against it.

**What is not clean is a narrow grid following other dispatches in the same
process.** M=8, N=256, L=64 passes ten runs in ten when it is the only thing
a process dispatches. Placed after six sixty-four channel cells it fails
every run, by about 57000 ulp, with the magnitude steady. Same input, same
seed, same cell: what changed is what ran before it. That is state surviving
between dispatches, and the most likely home for it is the driver's handling
of many pipelines built from one module with different specialization
constants, which this kernel does more of than anything else in the tree.

That paragraph is wrong in every particular except the first sentence, and
the next section replaces it. It is left standing because the reasoning that
produced it is the trap: twelve samples of a one-in-two-thousand event look
exactly like a deterministic effect of whatever changed between them.

Two things it is not. It is not a missing shared-memory barrier: adding
`memoryBarrierShared()` at both barrier sites moved the rate from 4 in 20 to
5 in 20. It is not leaked denormal state: the twin already takes
`ScopedDenormalFlush`.

The denormal half stands. The barrier half does not: twenty trials cannot
tell 0.20 from 0.25, so that experiment had no power to conclude anything,
and the evidence below points the other way.

**It does not make CI flaky, and the reason is worth knowing.**
`catch_discover_tests` registers one ctest entry per Catch2 case, so each
case runs in its own process and never accumulates the pipeline churn that
triggers this. A full `ctest --preset ci` is 0 bad runs in 6 on the
integrated device. What shows the anomaly is running the test binary
directly with a tag filter, which is what a developer does while debugging,
so it is written down here to save the next person the afternoon it cost.

**One more shape, which the engine never asks for.** A workgroup wider than
half the transform leaves some threads with no butterfly, and on the
integrated device in a Debug build that produces a 37 dB error at
M=64 N=256 L=256. The graph picks `min(N/2, ceiling, 256)` so the width is
never above N/2 and the shape is unreachable, which is why the sweep does not
carry it. It is recorded because the sizing rule is the only thing preventing
it, and a future change to that rule would walk straight into this.

A probe cannot currently gate the contamination. The one in `gpu_fixture.cpp` was corrected
to dispatch the spectrum kernel at the exact shape it gates rather than
`pfb_fft` at a different size, and it still reports reproducible, because
twelve repetitions of one identical dispatch are reproducible. The failure
needs *different* dispatches interleaved, which a repeat-the-same-thing probe
is the wrong instrument for.

The conclusion is right and the reason is not. Twelve repetitions report
reproducible because twelve is nowhere near the sample size the event needs,
not because they are identical. A probe that could gate this would have to
run a few thousand dispatches, which is seconds of wall clock but is seconds
paid by every test process on every device.

### 2026-09-19: measured with an instrument instead of an afternoon

Everything above was inferred from runs of the test binary, a dozen at a
time, while changing something between them. `tools/gpustress` exists so that
nobody has to do that again. It dispatches twelve shapes in rotation, keeps
each shape's CPU twin, and reports every dispatch that disagreed with it,
grouped so that one bad dispatch among five hundred is visible as one bad
dispatch rather than as four hundred and ninety-nine divergences from a
poisoned baseline. It is off by default:

```
cmake --preset dev -DREVENANT_BUILD_GPU_STRESS=ON
cmake --build --preset dev --target gpustress
build/dev/tools/gpustress/gpustress --gpu 1 --rounds 500
```

Two runs, 40 processes of 500 rounds each, 240,000 dispatches per device.

| | discrete | integrated |
| --- | --- | --- |
| spectrum dispatches | 200,000 | 200,000 |
| disagreeing with the twin | **0** | **106** |
| branch dispatches | 40,000 | 40,000 |
| disagreeing with the twin | **0** | **0** |

What that says, against what was written above.

**The discrete device is exact, at a sample size that can carry the claim.**
Twenty thousand dispatches of each shape, every one bit-identical to the
twin. The claim that the shipped configuration is clean there survives, three
orders of magnitude better supported than when it was made.

**On the integrated device the rate is about one spectrum dispatch in
1,900,** and nothing about it is positional. Every one of the 106 is a single
isolated dispatch: the shape gives the right answer, gives a wrong one once,
and gives the right answer again immediately. Nothing is carried forward, so
"state surviving between dispatches" is withdrawn. The majority answer was
the twin's answer in every case, at every shape.

**It is not confined to narrow grids.** M=64 N=2048 L=256, which is what the
graph dispatches, is in the list and is hit. So is M=64 N=256 L=128. There is
no narrow-grid effect to explain; the earlier runs saw it at M=8 because M=8
was most of what they dispatched.

**The magnitude is not steady.** Worst divergence per event runs from 1 ulp
to 2.1e9, which is most of the float range. A figure like "about 57000 ulp"
was one sample of a distribution.

**It is the spectrum kernel specifically, not shared memory on this device.**
`pfb_branch` ran 40,000 times in the same processes, interleaved with the
spectrum dispatches, and was bit-exact every time. It reads the same ring and
runs under the same driver; it has no shared-memory transform.

**CI is still not affected, for the reason already given.** One ctest entry
per Catch2 case means a handful of spectrum dispatches per process, and a
one-in-1,900 event needs thousands. The gate in `gpu_fixture.cpp` stays.

One thing this does not explain, and it is the open end. While the spectrum
auto-scaling was being checked on the integrated device, `revenant-cli
--spectrum` was drawing corrupted waterfall rows at roughly one row in four,
which is three orders of magnitude above the rate measured here. That figure
is an eyeball count from a terminal and not an instrumented one, so treat it
as a discrepancy to chase rather than a second measurement.

Same kernel, same device, so the difference is in how it is dispatched. The
graph records many dispatches into one command buffer with barriers between
them and submits once; this tool submits one dispatch per command buffer and
waits on a fence. The engine's pattern is the one that decides whether the
product is affected, and it is the one this tool does not reproduce. Anyone
picking the question back up should add that mode rather than re-run the
sweep.

### What the failures look like from the inside

The sweep above says how often. `gpustress` also records the shape of each
failure, which says what kind of thing it is. Sixty events, dissected:

| | |
| --- | --- |
| wrong values per event | 5 to 4,257, mean 533 |
| events where exactly one value was wrong | **0 of 60** |
| events that were a single flipped bit | **0 of 60** |
| channels touched, as a fraction of the dispatch | 1.6% to 62.5%, mean 36% |
| bins wrong inside a channel that was touched | 0.6% to 94.5%, mean 24% |

**It is not a bit flip.** Not one event in sixty corrupted a single value,
and the smallest touched five. Radiation, a memory upset and anything else
that damages a stored number one at a time are all ruled out by the first
row of that table. The discrete card sitting in the same case through the
same 240,000 dispatches is the other half of that argument.

**It is not confined to one workgroup either.** A workgroup cannot reach
another's shared memory, so an event that corrupts a third of the channels
in a dispatch is not one transform going wrong in isolation. Whatever
happens, happens to many workgroups at once.

**The corruption has structure.** The first wrong value in an affected
channel is bin 0 in 22 events and bin 4 in 29, which is 51 of 60. Bins 1, 2
and 3 are almost never the first. Scattered damage at 24% density would
start at bin 1 nearly as often as bin 0; this does not. Bins 0 and 4 are
what threads 0 and 4 write, because the output loop is
`for (j = tid; j < half_bins; j += threads)`.

**And it needs more than one thread.** Same M, same N, same input, same
40,000 dispatches each, with the workgroup width as the only variable:

| workgroup width | events in 40,000 |
| --- | --- |
| 1 | **0** |
| 64 | 7 |
| 128 | 6 |

That is the measurement that names the class. A single-threaded workgroup
runs the identical arithmetic in the identical order, reads the same ring
and writes the same buffer, and does not fail. What it does not do is
synchronise with anything. Read with the row above it, the fault is a
concurrency fault inside the workgroup, and the earlier "not a missing
shared-memory barrier" is withdrawn.

One caveat on that table, stated because it is the obvious objection. A
single-threaded dispatch of an eight-channel grid is eight threads on a
device with hundreds of lanes, so it differs from the others in occupancy as
well as in synchronisation, and the two cannot be separated by this
experiment alone. It does not weaken the conclusion that the fault needs
concurrency. It does leave open whether the mechanism is inside the
workgroup or in how several are scheduled together.

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
