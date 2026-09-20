# Conventions

The rules the code already follows, written down here so a reviewer can cite a
file in this repository instead of an external document.

These are not preferences. Each one exists because the alternative has a known
failure mode in a signal processing engine, and the failures are recorded next
to the rules.

## Errors

`std::expected` everywhere, from `core/error.h`.

```cpp
Expected<Context> make_context(const Context::Options& options);
Status upload(ConstComplexSpan samples);
```

`Error` carries a message written for a person plus the originating API's code
where there is one. `fail(message, code)` builds the unexpected side.
`with_context(error, "where")` prefixes an error as it moves up a layer, so the
final message reads as a chain rather than as whichever frame happened to
notice.

Do not throw for control flow. Exceptions cross an API boundary only where the
standard library itself throws, `std::bad_alloc` and the like. A DSP stage that
throws cannot be run inside a batch sweep without the sweep growing a handler
for each stage.

Do not return a bare `bool` and log the reason. The caller cannot report what it
was not told.

## Ownership

Values first. `std::unique_ptr` when something must be polymorphic or must
outlive the scope that made it. `std::shared_ptr` requires a justification in
review, naming the second owner and why its lifetime cannot be ordered.

Shared ownership in a pipeline usually means the buffer handoff was never
designed. A ring with a documented producer and consumer does not need a
reference count.

Vulkan handles are owned by the object that created them and released in its
destructor. A raw handle in a function signature is a borrow for the duration of
the call and nothing longer.

## Sample buffers

Never a raw pointer plus a length. The pair separates at the first refactor and
the length is the half that goes stale.

A contiguous run of samples on one axis is a span: `ComplexSpan`,
`ConstComplexSpan`, `RealSpan`, `ConstRealSpan` from `core/dsp/types.h`.

Anything with more than one axis or a stride that is not one is
`std::mdspan`: channels against time, frames against bins, a strided view of a
device allocation. The extents travel with the data, so a kernel cannot be
handed a buffer laid out the other way round and quietly read garbage.

`Complex32` is `std::complex<float>`, which the standard guarantees is
layout-compatible with `float[2]`. That guarantee is the reason the project uses
it rather than a hand-rolled struct: a span of them maps onto a std430 `vec2`
array on the device with no repacking. `types.h` static asserts the size so a
platform that breaks the guarantee fails at compile time.

## Naming

| Thing | Form |
| --- | --- |
| Functions, variables, parameters | `snake_case` |
| Types, concepts | `PascalCase` |
| Constants, enumerators | `kConstantCase` |
| Private members | trailing underscore, `device_` |
| Namespaces | lowercase, `revenant::dsp`, `revenant::gpu` |
| Files | `snake_case.h`, `snake_case.cpp`, `snake_case.comp` |
| Macros | avoid, and shout when unavoidable |

Four spaces, no tabs. Braces on their own line for functions, on the same line
for control flow. Include the repository root: `#include "core/dsp/types.h"`,
never a relative path climbing out of a directory.

## Frequency is always integer hertz

`Hertz` is `std::int64_t`. Not a double, not kilohertz, not megahertz, not a
scaled fixed-point unit of somebody's choosing.

This is the convention people undo, so the reason is worth stating plainly.

A float carries 24 bits of mantissa, so 100 MHz in a `float` has a spacing of 8
Hz between adjacent representable values. The center frequency alone is already
wrong by more than a narrowband channel is wide. A double gets you to about a
microhertz at that magnitude and looks safe, and it is, right up until a chain
does what chains do: tune, mix to baseband, decimate, mix again for a channel
offset, correct a measured Doppler shift, correct a crystal error in parts per
million. Every one of those is a multiply or an add, each rounds, and the
rounding is not the same on two machines because the compiler is free to
contract or reorder.

What that produces is not a crash. It is a decoder that works on one machine and
fails on another, a demodulated tone that sits 3 Hz off with nobody able to say
which stage put it there, and a bug report describing the symptom five stages
downstream of the cause. An integer count of hertz has no rounding at any stage,
so a frequency that comes out wrong came from arithmetic somebody wrote, and the
difference is exact and searchable.

`int64_t` counts every hertz from DC past 9 exahertz. There is no radio that
needs the headroom and no cost to having it.

Sample rate is integer for the same reason. Ratios are computed where they are
needed, from integers, and `phase_increment(Hertz, SampleRate)` in `types.h` is
the one place a frequency becomes a double, returning radians per sample so a
caller's NCO does not start out already lossy.

## Time is a sample index

`SampleIndex` is `std::uint64_t`, counted from the start of the stream, and it
is authoritative. Wall clock is derived at the edges and only at the edges.

Retroactive tuning means going back into a capture and demodulating something
nobody was listening to at the time. That needs the index to be exact across
hours of samples. A double runs out of integer precision at 2^53, which sounds
like plenty until someone captures at 20 MS/s for a few months. A `uint64_t`
at 100 MS/s overflows after about 5,800 years.

`BlockTimestamp` carries the index, the sample rate, and `epoch_anchor_ns`, the
Unix epoch nanosecond at which sample 0 was captured. The anchor is established
once at stream start and then corrected against GPS or PPS where the hardware
offers one. It is never re-derived per block from a clock read.
`wall_clock_ns()` converts on demand, splitting the division to avoid the
overflow that catches the obvious implementation fifteen minutes into a capture
at 20 MS/s.

## Purity in the sample path

Every DSP function is a pure function of its arguments. No global mutable state,
no wall clock read, no ambient configuration.

This is architectural, not stylistic. The engine has to run faster than
realtime, in batch, and retroactively over a stored capture. A stage that reads
a clock produces a different answer in a replay than it did live, and a stage
with state hidden in a global cannot have two instances, which is what a
multi-channel receiver is.

Randomness takes an explicit seed, always. `std::mt19937_64` with the caller's
seed, never seeded from a clock or from `random_device`. Tests print the seed
they used. A failure that cannot be reproduced from its printed seed is not a
test result, it is an anecdote.

## Concurrency

No mutex in the sample path. Not a fast one, not an uncontended one.

A lock in a stage that runs per block turns a scheduling decision somewhere else
in the system into a dropout here, and a dropout in a capture is unrecoverable
because the samples are gone. Stages are connected by single producer, single
consumer lock-free rings.

Every ring documents, at its declaration:

- which thread writes and which reads
- who owns a block between claim and publish
- what happens when the consumer falls behind, which is a counted, reported
  overrun and never a silent overwrite

A dropped block is reported through the counter, not swallowed. An engine that
hides overruns produces a recording that looks continuous and is not, and
nothing downstream can tell.

## Shaders

One kernel per file, under `core/shaders/`, named for what it does.

Workgroup size is a specialization constant. Never hardcoded, never a `#define`
patched at build time.

```glsl
layout(local_size_x_id = 0) in;
layout(constant_id = 0) const uint kWorkgroupSize = 64;
```

The conformance suite runs the same kernel on every device in the machine and
those devices have different limits. `DeviceInfo::max_workgroup_size_x` in
`core/gpu/context.h` is read at runtime for exactly this reason: a kernel that
bakes in 256 cannot even be launched on a device that caps lower, so the one
device that would have caught the bug is the one that never ran the test.

SPIR-V is compiled, validated and embedded at build time by
`cmake/CompileShaders.cmake`. Runtime compilation would turn a shader that fails
on a user's driver into a crash on their machine instead of a red build on ours.
The module is validated with `spirv-val` and then emitted again as a `uint32_t`
array, so the validated artifact and the embedded artifact are byte-identical by
construction rather than by a conversion step somebody has to trust.

## Reference implementations

Every GPU kernel has a CPU twin, and the twin is the referee. The twins live in
the engine beside the rest of the DSP, currently `core/dsp/complex_ops.cpp`;
`tests/reference/` holds the harness that compares them, not the references
themselves. They are part of the engine because a reference that lives in the
test tree is a reference nobody ships, reviews or keeps current.

A reference translation unit includes `core/dsp/reference_fp.h` first, before
anything else, and its target calls `revenant_apply_reference_fp`. Both exist on
purpose: the flag covers the build, the pragma travels with the file and
survives a restructure by someone who does not know why the flag was there.
`kReferenceFpDisciplineApplied` makes forgetting the include a compile error in
the file that forgot rather than a divergence on somebody else's GPU six months
later.

Floating-point contraction is off in the reference. With it on, the reference
fuses its own multiply-adds, gives a different answer when built by a different
host compiler, and the diff suite starts reporting failures that are the harness
rather than the kernel. A referee that moves is not a referee. Speed is
irrelevant here because the reference is never in the sample path.

## Denormals are flushed to zero

Everywhere, on the CPU as well as the GPU. `core/dsp/denormal_mode.h` carries
the control and the full reasoning.

This is a decision the reference diff suite forced on its first run rather than
one anyone designed up front, which is worth recording as the kind of thing the
suite is for. The complex multiply kernel on an RTX 4090 returned exact zero
where the CPU reference returned `1.17549435e-38`, the smallest normal float,
along with several denormal results. Neither side was wrong. GPUs flush
denormals in fp32 compute, Vulkan permits it, and NVIDIA does not offer
`shaderDenormPreserveFloat32` at all, so it cannot be switched off on the device.
The CPU, left alone, preserves them.

Two implementations that disagree about denormals cannot be compared bit for
bit, and the reference has to model the hardware rather than the other way
round. A reference implementation therefore opens a `ScopedDenormalFlush` for
the duration of its work.

The policy stands on its own merits regardless of the diff. A denormal is below
1.2e-38, which is thirty orders of magnitude under the noise floor of any real
receiver, so nothing is lost. What is gained is speed: a single denormal operand
costs a hundred cycles or more on x86, which is why every serious audio and DSP
engine turns them off.

The mode is per-thread on x86, which is why it is scoped rather than set once at
startup. A reference must give the same answer whatever mode its caller was in.

## Warnings

`/W4 /permissive-` everywhere, `/WX` in CI through `REVENANT_WERROR=ON`. CI is
where a new warning stops the line, not a developer's machine mid-thought. See
`cmake/CompilerFlags.cmake`.

What that means in practice:

- unused parameter: omit the name, do not cast it to void
- signed against unsigned: `static_cast` deliberately, at the point where the
  conversion is known to be safe, and not with a blanket cast at the top
- narrowing: explicit, or change the type
- shadowing: rename, including a parameter shadowing a member
- a switch that must be exhaustive writes no `default`. `/w14062` is on in
  `cmake/CompilerFlags.cmake` and raises a new enumerator to a level 1
  warning. It becomes a build error in CI and only in CI, through the `/WX`
  the paragraph above puts behind `REVENANT_WERROR`; on a developer's machine
  an unhandled enumerator scrolls past with everything else. C4062 is off by
  default at `/W4`, which is not obvious and was got wrong here once, and it
  fires only on a switch with no default label: a switch that omits an
  enumerator and carries a default stays silent. A switch that legitimately
  handles a subset writes a default that does something defensible, and
  nothing checks it for you.

That last rule is the one this section left out until 2026-09-20. The other
four are cleanups applied to code already written; this one changes how a
switch is written in the first place, and the defect it exists to catch was a
comment in this tree claiming a guard that was not on.

**The sentence that added it said "makes a new enumerator a build error
there", and that was the same mistake one round later.** `/w14062` sets a
warning level, not an error; nothing but `/WX` makes any warning fatal, and
this section had already said `/WX` is CI-only two paragraphs earlier.
Corrected in place rather than reworded, because a reader who took it at face
value is expecting their own build to stop and it will not.

## Commits

Subject in the imperative, under 72 characters, no trailing period, no type
prefix. Body only when the change needs explaining, and it explains why rather
than restating the diff. No emoji and no trailers.

This is the house style across the Locke Werks repositories and it is what
every commit here follows. It is a deliberate departure from the handoff
document, which specified Conventional Commits: the `feat:` and `fix:` prefixes
earn their keep when a tool generates a changelog or computes a semantic
version from the log, and nothing here does either. Carrying the ceremony
without the machinery is cost with no return. If a release process later wants
generated changelogs, this is the decision to revisit, and revisiting it means
adopting the prefixes going forward rather than rewriting what is already
pushed.

Every commit that touches DSP or a shader names its reference-diff result in the
body: which kernel, which devices it ran on, and the worst deviation observed.

```
Correct the FIR tail on non-multiple block sizes

reference-diff: fir_decimate, nvidia-4090 and amd-igpu, max |err| 4.1e-7
against tests/reference/fir_decimate.cpp over 2^20 samples, seed 20260918.
```

A DSP change with no such line is not reviewable, because the only question that
matters about it is whether the numbers still agree and the answer is not in the
diff.
