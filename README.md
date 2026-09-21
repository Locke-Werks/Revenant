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

Those are the design's reasons for existing. None of them is a benchmark. What
has been measured is below, with the numbers rather than the adjectives.

## Status

M1 is done and M2 is under way. There are two things to run: a command line,
and a Qt client that reaches a running engine over a socket.

```
revenant-cli "rtlsdr://0?freq=98.1M&rate=2400000&gain=20" \
    --vrx 98.1M:wfm:200k --play --record fm.wav

revenant-cli "rtlsdr://0?freq=98.1M&rate=2400000&gain=20" \
    --spectrum --detect
```

That path is real and measured: bytes from the dongle cross the bus once as
native unsigned 8-bit pairs, are widened by a compute kernel, channelized on
the GPU, tapped by a receiver that mixes and filters and demodulates on the
device, and only the finished audio comes back. Five seconds of 98.1 MHz
broadcast FM through it recorded with every drop counter at zero, checked by
measuring the 19 kHz stereo pilot against its own neighbourhood rather than by
listening: 1037x on the station against 2.86x on an empty channel.

`gain=20` is written out because it matters and because it is the default
rather than in spite of it. The default used to be `gain=auto`, the tuner's
own AGC, which maximises the level at its output and is therefore set by the
loudest thing anywhere in 2.4 MHz. Measured on air on 2026-09-20 at 95.1 MHz
in an ordinary suburban FM environment, that put three intermodulation
products in the detector's track list at confidence 1.00; `gain=20` improved
the measured SNR of KKFM at 98.1 MHz by 5.7 dB and removed all three. Twenty
is the one figure that has been measured, in one place on one band, so it is
a starting point and not a right answer: a quiet band wants more and a
stronger environment wants less. What tells you it has become wrong is the
front end line described below. `gain=auto` is still there if you ask for it.

The second command draws the whole 2.4 MHz as a waterfall in the terminal and
lists what it finds in it. Under the track list it says when the noise floor
across the whole span is following the strongest signal on it, and how fast:
about one decibel per decibel is a gain control moving, and faster than that
is a front end being driven past its linear range, at which point some of the
tracks in the list are products of the others. `core/detect/front_end.h` is
the measurement and is explicit about what it cannot tell apart. The Qt client
carries the same line under its tuning controls. Both ends of the colour map track the band on their own,
from percentiles measured on the device rather than from extremes, expanding
in a frame or two and contracting over thirty seconds. `--spectrum-floor` and
`--spectrum-ceiling` hold either end still, which is what comparing two
captures needs.

`revenant-engine` is the same core left running with a Cap'n Proto session on
the front of it, and `revenant-ui` is the client. Spectrum and waterfall are
scene graph nodes rather than rasterised images, the newest waterfall row is
at the top, and the detector's tracks are drawn over both: a frequency marker
that fades on the spectrum, a rectangle bounded by the rows the signal was
actually in on the waterfall, and a click tunes a receiver to one.

What exists: the Vulkan context and allocator, the shader build, the polyphase
channelizer, seven demodulators and a raw complex tap, the per-receiver fine
stage, audio egress to WAV and to the sound card, a synthetic wideband source,
a file source, an RTL-SDR backend, the full-span spectrum with a waterfall in
the terminal, auto-scaling measured on the device, the per-receiver passband
spectrum, the wideband detector, the RDS and RBDS decoder, the Cap'n Proto
session that carries all of it to another process, the Qt client that draws it
and plays its audio, and the conformance suite that diffs every GPU kernel
against a scalar twin and demands identical bits. 361 tests on an RTX 4090,
0 failures; 3 of them skip without an RTL-SDR plugged in. `ui/` is a separate
CMake project with a suite of its own, 26 tests, and CI configures, builds and
runs both trees.

What does not: every decoder but RDS. `docs/modes.md` is the scoped list and
none of the rest of it is written. WFM is mono and has no de-emphasis, so
broadcast stations decode bright and one channel only. Nothing saves a set of
receivers across a restart; that one is in `docs/rpc.md` with what it would take
and what the gap costs meanwhile.

This paragraph used to end "The session cannot open or stop a source, and
nothing saves a set of receivers across a restart. Those last two are in
`docs/rpc.md`, each with what it would take and what the gap costs meanwhile."
The first half is no longer true. `openSource` and `closeSource` are on the
session, an engine's source can be closed and another opened without restarting
the process, and `EngineInfo::sourceEpoch` is what tells a client the sample
indices started again. Starting is still the host's, because `run()` blocks for
the length of a stream; `revenant-engine` loops on it.

This paragraph used to read "every decoder. Audio and RDS have a shape on the
wire and nothing behind them, so the client is still silent and the command
line is what listens; both methods refuse in words saying the surface exists
and is not wired, because a stream that produced nothing would read as a
broken radio." Every clause of it is now false. `subscribeAudio` carries
float32 PCM and the Qt client plays it through a `QAudioSink`, and
`rdsStation` and `setRdsRegion` serve a real decoder that runs per receiver
on the engine's completion thread. The retraction is left here rather than
swapped out because the sentence was an instruction to whoever read it: it
told them not to look for audio in the client, and there is audio in the
client.

A client logs in with a pre-shared token before it holds anything at all, and
still binds loopback by default: the wire is plaintext, so a token crossing a
network is readable and replayable, and off loopback still means a tunnel.
`docs/rpc.md` has where the token lives and how to pass it. This paragraph
used to say the session had no authentication of any kind and binds loopback
for that reason; the first half is no longer true and the second half survives
for a different reason, which is why the sentence is replaced rather than
edited.

### What has been measured

Fifty receivers on one 20 MHz grid run at 3.28x realtime with every overrun
counter at zero. The claim a channelizer exists to make is that the coarse
chain does not care how many receivers hang off it, and it holds: 10.0
microseconds at zero receivers and 10.2 at a hundred. What is not free is the
per-receiver fine stage, linear at about 18 microseconds each, which dominates
above a handful. The FFT stage reaches 68.1% of VkFFT's 790 GB/s on the grid
the engine actually runs, and 60.2% at its worst size.

The conformance suite is bit-exact, not close, and the RTX 4090 stays that
way under 3.7 million dispatches of deliberate abuse.

The integrated Radeon does not, and the size of that is the thing to state
plainly. One dispatch per submission, it computes a wrong spectrum frame
about once in 1,900. Share a command buffer between several dispatches, which
is what the engine records, and the rate climbs by two orders of magnitude:
at the five dispatches per frame the graph actually submits, the shipped
transform shape is wrong 19% of the time, and at six it is 85%. Nothing else
changes, and the channelizer's branch filter in the same command buffers
never fails once in 528,000 dispatches.

So the spectrum kernel is refereed on the discrete card and skipped
elsewhere, and on that device the waterfall is not to be trusted. The cause
is narrowed to a concurrency fault in that driver rather than identified.
`docs/fft.md` carries the measurements and `tools/gpustress` is the
instrument, so the next person reruns it against a new driver instead of
re-arguing it.

### Where it is going

**M2, in progress.** The graphical client. It runs as its own process and
reaches the engine over a Cap'n Proto service, because the engine is headless
by design and because it has to be: the engine is built against the static C
runtime and ships as one self-contained signed binary, and Qt is not.
`docs/rpc.md` has the reasoning and the alternatives that were rejected.

**M3 is the first public release.** Until then the layout moves and there are
no binaries. The repository is public because the licence made it the
straightforward way to satisfy the source obligation, not because anything
here is finished.

**M4 and beyond:** the decoders, and search over stored captures.

## Requirements

Windows 11, x64.

A GPU with Vulkan 1.3 or newer, which means essentially any discrete card from
the last several years and most integrated graphics. Development and CI run
against an NVIDIA RTX 4090 and the integrated Radeon in a Ryzen 9 7950X.

An SDR device, optionally: an RTL-SDR v3 works today, through librtlsdr. The
synthetic wideband source and the file source need no hardware at all and are
what the conformance suite runs against, so the engine is fully exercisable
without a radio.

A sound card, if you want to listen rather than record. The monitor is
WASAPI in shared mode.

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

## AMBE, and why you will not find it here

AMBE and AMBE+2 are Digital Voice Systems, Inc.'s proprietary voice codecs,
and they are the largest single obstacle in this project.

There is no published specification. Not a restricted one, not an expensive
one, none. The clean room rule above asks only that a document exist, and for
AMBE+2 no document does, so there is nothing to write from and no amount of
care or citation produces a lawful implementation. A licence would not fix
it. GPL-3.0 section 7 forbids conveying a work under further restrictions,
and a proprietary codec licence is nothing but further restrictions, so it
cannot ship inside Revenant at any price, including zero.

What is left is a dongle. To hear a voice signal, on a machine already
running a polyphase channelizer and an FFT across twenty megahertz in real
time, you buy a chip from DVSI and plug it into a USB port, because the
arithmetic is a secret.

The comparison is what makes it galling. IMBE, the OLDER codec from the same
company, is published in TIA-102.BABA and its patents have expired, so P25
Phase 1 voice decodes here in software with nothing bought and nothing
plugged in. TETRA's ACELP is specified in ETSI EN 300 395-2 and ETSI gives it
away. Codec 2 is open and at these bit rates it is not worse. Every
narrowband voice codec somebody wrote down works fine. The one nobody wrote
down is the one in DMR, D-STAR, P25 Phase 2, NXDN, dPMR, Yaesu Fusion,
Iridium, Inmarsat, Thuraya, ACeS and most of the mobile satellite industry,
and it is there because it reached the standards bodies early rather than
because it sounds good. It does not sound good.

So the line this program draws is the one the documents draw. Identification
and protocol decoding work everywhere, because framing travels in the clear
and framing is where most of the useful information lives. Voice works where
the codec was published. Where it was not, Revenant will speak to a hardware
vocoder if you own one, and will otherwise tell you precisely what it is
hearing and decline to guess.

The patents on this will expire and it will still not be implementable,
because the problem was never the patents. My contempt for the arrangement
will comfortably outlast them.

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
- [docs/modes.md](docs/modes.md), every mode Revenant will implement, what each one
  needs and which are out of reach
- [docs/rpc.md](docs/rpc.md), why the client is a second process, what crosses
  the wire and what does not
- [docs/ui-spectrum.md](docs/ui-spectrum.md), how the spectrum and waterfall scale
  themselves and why the fine-tuning display transforms a different stream
- [docs/detection.md](docs/detection.md), wideband detection and click-to-tune, and
  why the detection spectrum is built per channel rather than across the span
- [docs/rtlsdr-provenance.md](docs/rtlsdr-provenance.md), what the RTL2832U
  datasheet does and does not specify, and why that settled the licence
- [docs/rds-first-decode.md](docs/rds-first-decode.md), the first decode of a
  signal nobody here generated, what it establishes and why it is not yet a test
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

Relicensing is not publishing, and the two happened at different times here.
Copyleft obligations attach to distribution, so a repository whose binaries
stay on their author's machines owes nothing to anyone; the duty to offer
corresponding source begins when a binary is handed to someone else. This
repository is public and its source is published, which discharges the
source obligation in advance for anything built from it. No binary has been
handed to anyone yet, so the release obligations in
[docs/clean-room.md](docs/clean-room.md) are the ones still ahead rather
than the ones outstanding.

WHAT THIS PARAGRAPH USED TO SAY

Until 2026-09-20 it ended "This repository is private today and goes public
when there is a reason to, not because the licence changed." The repository
is public and has been for some time: `gh repo view` reports
`"isPrivate": false`. Recorded rather than swapped because the false half
was the premise the rest of the paragraph reasoned from, so anyone who
worked out their obligations by following it was told the private case
applied to them when it does not.

Clean-room is still the default everywhere it is affordable, which is
everywhere a specification is published. The policy, the exceptions and the
reasoning are in [docs/clean-room.md](docs/clean-room.md).

Copyright (c) 2026 Locke Werks.
