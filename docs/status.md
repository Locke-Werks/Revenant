# Status

M1, M2 and M3 are done: v0.1.0, the first public release, shipped on
2026-09-27, and the release page has every one since. M4, the rolling
capture, is in progress.
There are two things to run: a command line, and a Qt client that reaches a
running engine over a socket.

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

The client picks the radio too. It lists what the engine can see, says why an
unavailable backend is unavailable rather than hiding it, offers only the
controls the device describes, and opens it. Changing radio costs the receivers
and the waterfall history and nothing else: the engine closes the source, builds
a grid against the new one and serves the same session, and
`EngineInfo::sourceEpoch` is what tells the client its sample indices have
started again. The process does not restart, which is what it used to take.

What exists: the Vulkan context and allocator, the shader build, the polyphase
channelizer, eight demodulators and a raw complex tap, four digital voice
modes that hand their decoder filtered complex baseband, the per-receiver fine
stage, audio egress to WAV and to the sound card, a synthetic wideband source,
a file source that reads 8, 16, 24 and 32-bit IQ, the 24-bit WAV the HF
recordings arrived as among them, and converts each on the GPU, an
RTL-SDR backend, the full-span spectrum with a waterfall in the terminal,
auto-scaling measured on the device, the per-receiver passband spectrum drawn
from a display tap beside each receiver, the wideband detector, the RDS and
RBDS decoder, the Cap'n Proto session that carries all of it to another
process, the Qt client that draws it and plays its audio, and the conformance
suite that diffs every GPU kernel against a scalar twin and demands identical
bits. A few of those tests skip without an RTL-SDR plugged in. `ui/` is a
separate CMake project with a suite of its own, and CI configures, builds and
runs both trees. The counts move with nearly every commit, so they are dated
rather than kept current: on 2026-09-23 `ctest -N` listed 841 tests in the
engine tree and 334 in `ui/`.

Sixteen decoders run on a live receiver, in the engine, from
`revenant-cli --decode` and over the wire through `Session.subscribeDecoded`:
P25 Phase 1, D-STAR, TETRA, DMR and M17 on complex baseband, and RTTY, SITOR-B,
NAVTEX, PSK31, PSK63, QPSK31, CW, AX.25 with APRS, POCSAG, AIS and VHF DSC on
receiver audio. P25 Phase 1 reads the trunking control channel as well as
voice channels: a receiver on a control channel reports the site's grants,
identifier updates and status broadcasts, with grant channels resolved to
frequencies, and every voice LDU publishes its Link Control and encryption
sync. AX.25 decodes FX.25 codeblocks and IL2P packets as well as plain HDLC
frames. RDS runs beside them on every wfm receiver that asks, read from a
companion receiver on the same tuning so the station stays stereo, and reports
programme type names, TMC and emergency warning groups as well as the text.
Each is written from its specification and checked by a round trip through a
transmitter written from the same clauses; `docs/modes.md` says where each one
stops, and [docs/sensitivity.md](sensitivity.md) has what each needs in
white noise, read off a committed curve, rather than a copy of that table
here. A P25 receiver's audio is its IMBE voice, since 2026-09-23: the wire
carries it at 8000 S/s through `Session.subscribeAudio`, silent between calls
and through an encrypted one, and the client plays it in the receiver's mix.
D-STAR and DMR voice go to a vocoder plugin, since 2026-10-02: a D-STAR
receiver hands each 72-bit voice frame and a DMR receiver each of a burst's
three 72-bit vocoder frames to the first loaded plugin that serves the mode or
its codec, and with no plugin loaded the audio is silence rather than a
refusal. No plugin ships with Revenant; [ambe.md](ambe.md) says why. TETRA voice is not decoded.

The client has had its first design pass. The main window is the span, a
frequency ruler between the spectrum and the waterfall, and a top bar with a
per-digit frequency dial and a band menu backed by a cited band table. The
receivers sit in a rack, docked in the main window by default and able to pop
out into a window of their own: up to 64 receivers, each with a strip, a level,
a gain, mute and solo, their audio mixed, and the focused one's dial, mode,
fine-tuning display, AFT, auto filter, RDS and a decode log of what its
decoders report. A DV slider beside the output lifts digital voice by up to
30 dB ahead of the limiter, and an "auto DV" switch in the rack header opens a
held receiver on every P25, DMR, D-STAR, TETRA or M17 signal the detector
verifies on a frequency no receiver covers. The radio panel lists the vocoder plugins the engine loaded,
what each serves and why any was refused. Every action has a key, one table
decides them all, and a command palette on Ctrl+K lists every action and
band. `docs/ui-spectrum.md` has each of those and what it measured.

Speech to text, since 2026-10-03. One switch, "speech" in the top bar or
Ctrl+Shift+T, has the engine transcribe every receiver that makes speech: P25
by call, D-STAR and DMR by call when a vocoder plugin gives them a voice, and
analogue modes by squelch, or by a level detector where the squelch is open.
WFM is left out unless a receiver is chosen in, and each receiver can be set
to auto, on or off. The recogniser is whisper.cpp on the engine's own GPU
through Vulkan, with its 1.6 GB model downloaded the first time the switch
goes on. The text lands on the span waterfall beside the transmission it came
from, pinned to the rows the speech was on, and in the decode log. On the RTX
4090 it transcribes a 5 s clip in 80 ms, and beside a P25 receiver and four
nfm receivers it lost nothing; `docs/rpc.md`, "Speech to text", has those
measurements and the one that costs, which is a GPU-bound engine's headroom,
halved while Whisper runs flat out.

A receiver belongs to the session that made it, and goes when that session
ends unless it was added with `keep`, which is what a headless recorder asks
for. `revenant-engine` listens on port 17690 by default, which is where
`revenant-ui` looks, and binds it exclusively. The engine also places probe
receivers of its own on detections, to name a signal's family; they are
internal, nothing on the wire can add or see one, and `docs/detection.md` has
what they get right and wrong.

What does not. Nothing stores the band: `core/capture` holds a placeholder and
no code, so there is no rolling capture, and search over stored captures, the
reason the design exists, has nothing to search yet. The engine runs one radio
at a time. Nothing saves a set of receivers across a restart; that one is in
`docs/rpc.md` with what it would take and what the gap costs meanwhile. TETRA
voice is not decoded, and D-STAR and DMR voice is silent without a vocoder
plugin. Speech to text hears English only. RDS2's three extra
subcarriers are not implemented, and RDS-TMC is recognised, counted and kept
raw but not decoded into events and locations, because the field positions are
in clauses of ISO 14819-1 nobody here has read; `docs/modes.md` has both.

A client logs in with a pre-shared token before it holds anything at all, and
still binds loopback by default: the wire is plaintext, so a token crossing a
network is readable and replayable, and off loopback still means a tunnel.
`docs/rpc.md` has where the token lives and how to pass it.

## What has been measured

Fifty receivers on one 20 MHz grid run at 3.28x realtime with every overrun
counter at zero. The claim a channelizer exists to make is that the coarse
chain does not care how many receivers hang off it, and it holds: 10.0
microseconds at zero receivers and 10.2 at a hundred. What is not free is the
per-receiver fine stage, linear at about 18 microseconds each, which dominates
above a handful. The FFT stage reaches 68.1% of VkFFT's 790 GB/s on the grid
the engine actually runs, and 60.2% at its worst size.

The conformance suite is bit-exact, not close, and the RTX 4090 stays that
way under 3.7 million dispatches of deliberate abuse.

**The integrated Radeon in a Ryzen 9 7950X is not supported and not tested.**
Its driver computes a wrong spectrum frame about once in 1,900 dispatches, and
far more often when several dispatches share a command buffer, which is what
the engine records; the kernel's arithmetic is exact on the RTX 4090 over the
same work. That was measured as a driver concurrency fault and not identified
further, and on 2026-09-22 the device was dropped from CI rather than chased.
`docs/fft.md` keeps the measurements as history, and `tools/gpustress` is
still the instrument if a new driver is worth a second look.

## Known shortfalls

What a release does not do is the "What does not" paragraph above, and
what each decoder does not do is `docs/modes.md`. One known shortfall arrived
with v0.1.0 and was still open at v0.1.6: with the receivers popped out into
their own window, that window misses 1.32% of refreshes at full load, over the
one in a hundred the budget allows. Docked in the main window, the default,
they are within it.

**M2 in detail.** The graphical client. It runs as its own process and
reaches the engine over a Cap'n Proto service, because the engine is headless
by design and because it has to be: the engine is built against the static C
runtime and ships as one self-contained signed binary, and Qt is not.
`docs/rpc.md` has the reasoning and the alternatives that were rejected.

Measured against what M2 was set to deliver: the QML shell exists, and so do
AM, FM and SSB demodulation and audio out. It asked for one receiver and the
client holds 64; it held eight when M2 closed. M2 asks for the spectrum and waterfall at monitor refresh
and closes when the render pipeline holds its frame budget at full target
load. That is now measured. On 2026-09-23 it was first not met: at 120 Hz with
both windows open, 20.9% of the main window's frames missed a refresh (p95
16.5 ms, p99 23.3 ms against 8.3), while drawing a frame took about half a
millisecond. The cause was Qt's threaded loop holding the one GUI thread
through each window's vsync wait in turn. The receiver window now presents
without waiting and is drawn after each main-window frame, and with both
windows kept in front the main window missed 0.4% and 1.0% of refreshes in two
60 s runs against 16.9% before, about what it misses alone. An undisturbed
300 s run on an idle machine then gave 154 of 36106 main-window frames over
budget, 0.43%, and 233 of 36045 in the receiver window, 0.65%, with the engine
at 1.000x realtime: under one in a hundred. The display is the streamed
virtual one, which is the display M2 is measured on.

The receivers have docked in the main window by default since 2026-09-23, and
M2 closed on that layout on 2026-09-27: 16 of 7314 frames over budget, 0.22%,
at full load with the engine at 0.999x realtime, and the main window with the
receivers popped out 128 of 36089, 0.35%, over 300 s. The popped-out receiver
window is not within budget: 473 of 35758, 1.32%, against 0.65% on
2026-09-23. It had regressed to 5.42% and two fixes brought it back that far;
what is left is open, and `docs/ui-spectrum.md`, "Frame budget", has the
numbers, the command and what is still open.
