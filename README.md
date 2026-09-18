<div align="center">

<img src="assets/revenant.ico" width="96" alt="Revenant">

# Revenant

**Samples land in GPU memory once, and the whole radio runs there.**

[![license](https://img.shields.io/badge/license-undecided-d6262a?style=flat-square)](#license)
[![platform](https://img.shields.io/badge/platform-Windows%2011-d6262a?style=flat-square)](#requirements)

</div>

---

Software radio on a CPU spends most of its time moving samples. The device
hands them to the host, the host copies them into a chain, each stage reads and
writes them again, and the display gets whatever survives the budget. That
shape has a consequence people stop noticing: you have to decide what you are
listening to before the samples arrive. Tune somewhere, demodulate one thing,
and everything else in the band that moment goes past unrecorded.

Revenant moves the whole chain onto the GPU and leaves the samples there.

## What it is

A software defined radio application where samples cross the bus once, into
device memory, and every stage after that is a compute shader operating on
buffers that never come back. Tuning, filtering, decimation, demodulation and
decoding all run on the device. The host schedules work and reads out results.

The engine is Vulkan compute, written against the API rather than against a
vendor's toolkit, so the same kernels run on NVIDIA, AMD and Intel hardware.
Every kernel has a CPU reference implementation, and a conformance suite diffs
the two on every device in the machine. That suite is the first thing the
project built, before anything user-facing, because a signal processing engine
whose numbers nobody has checked is an engine that produces confident garbage.

## What that buys

**A second channel is cheap.** Demodulating another signal in the same
bandwidth means dispatching more work over samples that are already resident,
not another pass across the bus. The cost of a channel is the arithmetic it
needs, and nothing else.

**The capture persists, so tuning can happen afterwards.** Samples that stay in
device memory and spill to disk as raw IQ are a record of the whole bandwidth,
not of the one signal somebody chose at the time. Going back into last
Tuesday's capture and demodulating something nobody was listening to is an
ordinary operation rather than a lost opportunity. This is why time in the
engine is an exact sample index from the start of the stream and never a
floating-point timestamp.

**A capture is something you can search.** Once the band is stored rather than
consumed, detection runs over it: find every burst, every carrier that came up
and went away, every transmission matching a shape. The question moves from
"what is on this frequency right now" to "what happened in this band, and
when".

Those are the design's reasons for existing. None of them is a benchmark, and
nothing here has been measured yet.

## Status

M0, Foundations. Nothing user-facing exists and there is nothing to run.

M0 ships the parts everything later rests on: the vocabulary types, the Vulkan
context and allocator, the shader build, the CPU reference implementations and
the conformance suite that diffs kernels against them. It exists so that later
milestones rest on numerics somebody has verified rather than on numerics that
looked right.

**The first public release is M3.** Until then this repository is work in
progress, the layout moves, and there are no binaries.

## Requirements

Windows 11, x64.

A GPU with Vulkan 1.3 or newer, which means essentially any discrete card from
the last several years and most integrated graphics. Development and CI run
against an NVIDIA RTX 4090 and the integrated Radeon in a Ryzen 9 7950X.

A supported SDR device, once there is a device backend. The first one is the
RTL-SDR v3, over libusb.

macOS and Linux are not targets yet. The engine is written to the Vulkan API
and does not use Windows-specific graphics, but nobody has built or run it
elsewhere, and claiming a platform nobody has tested is how a project acquires
bug reports it cannot answer.

## Build

`cmake --preset vs`, then `cmake --build --preset vs`. Visual Studio 2022, CMake
4.3, the LunarG Vulkan SDK and vcpkg.

[docs/building.md](docs/building.md) has the full list, the preset table, how to
point the suite at a particular GPU, and two traps involving a Strawberry Perl
installation that have each cost an afternoon.

## GPU conformance

Every compute kernel is diffed against a CPU reference implementation whose
floating-point behaviour is pinned, on every device in the CI machine, on every
push. The reference is compiled with contraction disabled so that it gives the
same answer regardless of which host compiler built it. A referee that moves
cannot referee anything.

CI runs on a self-hosted Windows workstation, because a hosted runner has no
discrete GPU and therefore cannot execute the one suite this project's
correctness argument depends on.

| Vendor | Coverage |
| --- | --- |
| NVIDIA | RTX 4090, discrete, Vulkan 1.4.351. Every push. |
| AMD | Radeon integrated graphics on a Ryzen 9 7950X, Vulkan 1.4.315. Every push. |
| Intel | **No coverage.** There is no Intel GPU in the machine and one cannot be added to it. |

The Intel gap is real and is recorded rather than papered over. Two of the three
target vendors get genuine driver coverage on every commit; Intel gets none, and
a kernel that behaves differently there would not be caught until somebody runs
it on one. Adding an Intel device to the conformance machine is the fix, and
until that happens this table is the honest statement of what is verified.

macOS through MoltenVK is out of scope until there is something to render.

## Clean room

Every device backend and every decoder is written from published
specifications and datasheets. No GPL-licensed source is read, ported, linked
or consulted while implementing the corresponding component.

That is not an aesthetic position. The obvious host libraries for this hardware
are copyleft, inherited copyleft cannot be removed once it is in the tree, and
this project's own licence is deliberately still open. Backends go over libusb,
constants carry a citation to the document they came from, and CI greps for
vendored licence text.

[docs/clean-room.md](docs/clean-room.md) is the full position and the practice
that backs it up.

## Documentation

- [docs/building.md](docs/building.md), prerequisites, presets and device selection
- [docs/conventions.md](docs/conventions.md), the rules the code follows and why
- [docs/clean-room.md](docs/clean-room.md), the licensing position

## License

Undecided.

This is a deliberate position, not an oversight. The choice between permissive
and copyleft is one a project makes once, and making it early, before there is a
release or a contributor or a downstream user, means making it with the least
information anyone will ever have about what this turns into.

What the absence of a licence means in the meantime is the default: no rights
are granted. Until a `LICENSE` file exists, treat this as source you can read
and not as source you can ship.

The clean-room discipline above is what keeps the choice open. A single GPL
library linked in would decide it by accident, and nobody would notice until it
mattered.

Copyright (c) 2026 Locke Werks.
