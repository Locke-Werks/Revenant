<div align="center">

<img src="assets/revenant.ico" width="96" alt="Revenant">

# Revenant

**Samples land in GPU memory once, and the whole radio runs there.**

[![license](https://img.shields.io/badge/license-GPLv3-d6262a?style=flat-square)](LICENSE)
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

## Clean room, and its one exception

Decoders and device backends are written from published specifications and
datasheets, with every constant carrying a citation to the document it came
from. That discipline is the default and it has not changed.

It is a default rather than an absolute, and the RTL-SDR is why. The RTL2832U
datasheet specifies the USB transport in full and specifies no raw IQ mode at
all: the mode that makes the chip a receiver was found by sniffing in 2012 and
exists as one GPL library's expression of it, so there was nothing to write
from. Revenant links librtlsdr and is GPL-3.0-or-later as a result. Every
copyleft dependency is recorded with what it is used for and why the
specification route was rejected.

The rule that survives intact is the one about laundering: do not read an
implementation and then present the result as written from a specification.
Copy it or link it and say which. Provenance that cannot be traced to a
document is worthless whatever the licence permits.

[docs/clean-room.md](docs/clean-room.md) is the full position, the exception
table and the disclosure log.

## Documentation

- [docs/building.md](docs/building.md), prerequisites, presets and device selection
- [docs/conventions.md](docs/conventions.md), the rules the code follows and why
- [docs/clean-room.md](docs/clean-room.md), the licensing position
- [docs/snr-convention.md](docs/snr-convention.md), how SNR is reported and why it
  matters that everyone means the same thing by it
- [docs/fft.md](docs/fft.md), why the FFT is written here rather than taken from
  a library, with the measurements that decided it
- [docs/ci.md](docs/ci.md), why the GPU jobs are self-hosted, what the matrix covers and
  what it does not
- [docs/ui-spectrum.md](docs/ui-spectrum.md), how the spectrum and waterfall scale
  themselves and why the fine-tuning display transforms a different stream
- [docs/rtlsdr-provenance.md](docs/rtlsdr-provenance.md), what the RTL2832U
  datasheet does and does not specify, and why that settled the licence
- [CONTRIBUTING.md](CONTRIBUTING.md), how to contribute and the provenance rules
- [CLA.md](CLA.md), the contributor agreement and why it exists

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).

This was all rights reserved, held open on the reasoning that the choice
between permissive, copyleft, dual and source-available is made once and was
better made later. Getting a radio working settled it. librtlsdr is the only
practical path to an RTL2832U, it is GPL-2.0-or-later, and the clean-room
route was measured and found blocked: the datasheet specifies the transport
and contains no raw IQ mode at all, because that mode was discovered by
sniffing in 2012 and exists as one project's expression of it. The evidence
is in [docs/rtlsdr-provenance.md](docs/rtlsdr-provenance.md).

The version is GPL-3.0 rather than 2.0 because librtlsdr is "or later", and
that matters more than it looks: GPL-2.0-only cannot be combined with
LGPL-3.0, which is Qt6, so a 2.0-only dependency would have killed the user
interface along with the licence question.

Relicensing is not publishing. Copyleft obligations attach to distribution,
so a private repository whose binaries stay on their author's machines owes
nothing to anyone; the duty to offer corresponding source begins when a
binary is handed to someone else. This repository is private today and goes
public when there is a reason to, not because the licence changed.

Clean-room is still the default everywhere it is affordable, which is
everywhere a specification is published. The policy, the exceptions and the
reasoning are in [docs/clean-room.md](docs/clean-room.md).

Copyright (c) 2026 Locke Werks.
