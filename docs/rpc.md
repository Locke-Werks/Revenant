# The wire

Revenant is two processes. One hosts the engine: `revenant-cli` drives it for
the length of one run, and `tools/engined` builds `revenant-engine`, the same
core left running with a Cap'n Proto session on the front of it. The other is
`ui/`, a separate CMake project that builds `revenant-ui` against Qt and
reaches the engine over Cap'n Proto. This file records why, what crosses, and
what does not.

The schema is `core/rpc/revenant.capnp`. The client-visible structs are
`core/rpc/types.h`. `core/rpc/client.h` and `core/rpc/server.h` are the two
ends. `core/rpc/CMakeLists.txt` splits them into two libraries and says why at
the top of the file; this file is the long version.

## Why there are two processes

Two independent reasons arrived at the same answer. Either one alone would
have been arguable. Together they closed it.

### The architecture reason

`core/engine/engine.h` opens by asserting that the engine is headless and has
no Qt anywhere, that the GUI is one client of the RPC surface, a script is
another, and a remote instance is a third. It then says the thing that matters:
that property falls out for free only if it is true from the start, which is
why the engine is built and tested with no client at all.

The root `CMakeLists.txt` backs it with two checks under
`REVENANT_ASSERT_NO_QT`, and the `headless` preset runs them in CI on every
push. One scans every header and source under `core/` for a Qt include. The
other fails if anything in the core tree called `find_package(Qt6)`. They catch
different mistakes: a developer reaching for `QString`, and a CMakeLists
quietly acquiring a dependency.

A headless engine buys remote operation, more than one client at a time, and a
core the test suite can drive without a display. None of that survives being
retrofitted, because by the time anyone notices a Qt header in `core/` it is
load-bearing.

### The build reason, measured 2026-09-19

The presets set `VCPKG_TARGET_TRIPLET` to `x64-windows-static` and
`CMAKE_MSVC_RUNTIME_LIBRARY` to `MultiThreaded`, so `revenant_core` and
everything linked against it is `/MT`. Qt 6.8.3's `msvc2022_64` kit is built
against the dynamic CRT.

Both halves were checked with `dumpbin /dependents` on this machine
(MSVC 14.44.35207) rather than assumed.

`build\ci\tools\cli\revenant-cli.exe` imports five DLLs:

```
vulkan-1.dll
ole32.dll
AVRT.dll
KERNEL32.dll
ADVAPI32.dll
```

No `MSVCP140.dll`, no `VCRUNTIME140.dll`, no `api-ms-win-crt-*`, no
`ucrtbase.dll`. That is what lets the receiver ship as one file with no
redistributable behind it.

`C:\Qt\6.8.3\msvc2022_64\bin\Qt6Core.dll` imports, among others:

```
MSVCP140.dll
MSVCP140_1.dll
VCRUNTIME140.dll
VCRUNTIME140_1.dll
api-ms-win-crt-heap-l1-1-0.dll
```

`Qt6Gui.dll`, `Qt6Qml.dll` and `Qt6Quick.dll` have the same shape.
`api-ms-win-crt-heap-l1-1-0.dll` is the load-bearing one: Qt's allocator is the
UCRT's, reached through that DLL, and the static core's allocator is its own
copy linked into the image.

The engine's API is not a C ABI. `Expected<EngineInfo>` carries a
`std::string`, `Expected<std::vector<SourceDescriptor>>` carries a `std::vector`
of four more, and `SpectrumFrame` owns a `std::vector<float>`. Calling that
in-process from a `/MD` Qt executable means the vector is allocated on the
static core's heap and freed on the UCRT's, or the reverse. The failure is not
a link error and usually not an immediate crash. It is a heap corruption that
surfaces somewhere unrelated, at a time that has nothing to do with the call
that caused it.

A process boundary is where that stops being a problem, because the two heaps
are then in two address spaces and nothing owned by one is ever freed by the
other.

## The three options

**Dynamic CRT project-wide.** Switch the presets to `x64-windows` and `/MD`
everywhere, keep one process. It is the smallest change on paper. It costs the
self-contained executable measured above: the shipped artifact becomes the exe
plus the Visual C++ redistributable as an install prerequisite, which is a
support burden on a program whose whole install story is one signed file.

It had one real point in its favour, recorded in `docs/clean-room.md`. libusb
is LGPL-2.1, the static triplet links it into the executable, and a static link
to an LGPL library carries the relinking obligation: the release has to ship
object files or an equivalent so a user can rebuild against their own libusb.
The dynamic triplet builds libusb as a DLL beside the executable, which
discharges the obligation on its own. That open item stays open under the
option that won, and `docs/clean-room.md` is where it is tracked. It is a
release-pipeline problem, not an architecture problem, and it did not buy
enough to decide this.

**Static Qt from source.** Build Qt `/MT` and link it into one executable. It
works, and it costs a multi-hour build that has to be repeated on every Qt
update, with a configure surface that breaks in ways specific to whichever
modules are enabled. Paying that on every point release, to avoid a boundary
the architecture already wanted, is the wrong trade.

**The split.** The engine stays `/MT` and self-contained, Qt stays a normal
dynamic install, and they talk over a socket. The cost is a schema, a
conversion layer, and a set of near-duplicate structs in `core/rpc/types.h`.
The duplication is bounded and visible: the schema is the contract, `types.h`
mirrors it, and `core/rpc/convert.cpp` is the only file that sees both the
schema and the engine. A field added to the schema and not to `types.h` is a
field the UI cannot see, which fails to compile in the client rather than
arriving as a silent zero.

The split also happens to be what the architecture reason wanted anyway, which
is why it won rather than merely surviving.

## The two builds

| | Main build | `ui/` |
| --- | --- | --- |
| Triplet | `x64-windows-static` | `x64-windows` |
| CRT | `/MT` (`/MTd` in Debug) | `/MD` (`/MDd` in Debug) |
| Links | `revenant_core`, `revenant_rpc_server`, `revenant_rpc_client`, capnp, Vulkan, rtlsdr, libusb | Qt 6.8.3 `msvc2022_64`, capnp |
| Builds | the engine, the tools, the tests, both RPC libraries | the Qt/QML client |
| Hosts | the `Server`, and a `Client` in-process so the suite can test the pair | a `Client` over a socket |

They are separate CMake projects, not one project with an option.
`REVENANT_BUILD_UI` exists in the root `CMakeLists.txt` and fails the configure
outright; it is not the switch for building `ui/` out of the main tree, because
the main tree's cache holds the static triplet and the wrong runtime library.
The refusal is still right and its wording is now stale: it says there is no UI
yet, and there is. `ui/` configures and builds from its own presets, which is
what the message should point at.

`core/rpc/CMakeLists.txt` splits the wire into two static libraries for the
same reason:

- `revenant_rpc_client` compiles the generated schema and `client.cpp`, and
  links capnp and nothing else of ours.
- `revenant_rpc_server` adds `convert.cpp` and `server.cpp`, and links
  `revenant_core`.

`ui/` does not consume `revenant_rpc_client` as a built library. It compiles
the same sources itself, against its own triplet, which is the only way to get
a `/MD` object file out of them.

**The rule that keeps this working: nothing in `core/rpc/client.cpp`,
`core/rpc/types.h` or `core/rpc/revenant.capnp` may depend on `core/engine`,
`core/dsp` or `core/source`.** `core/rpc/convert.h` and `core/rpc/convert.cpp`
are the server's alone, and they are the only files in the directory that
include an engine header. `core/error.h` is the one core header the client side
may include, because it is header-only and pulls in nothing but `<expected>`,
`<string>`, `<string_view>` and `<utility>`.

Break that rule and the client stops compiling in the Qt project. That is the
intended failure: a build error over there instead of a heap corruption.

## What crosses, and what does not

### Frequencies stay rational

`docs/conventions.md` requires integer hertz and explains at length why a
`double` frequency produces a decoder that works on one machine and fails on
another. The wire holds the same line one step further out.

A grid channel centre is `k * rate / M`, which is usually not a whole number of
hertz. Carrying it as an integer would round it, once per field, and the
resulting tuning offset in the display cannot be sourced afterwards because
nothing records where the rounding happened. So the schema has a `Rational`
struct of an `Int64` numerator and an `Int64` denominator, `types.h` mirrors it
with a `hertz()` accessor, and a display converts once, at the point where it
draws the label.

`core/rpc/convert.h` states the same invariant for the conversion layer:
nothing in it divides a rational or calls a `_hz()` helper.

### The demodulator enum is checked, not trusted

`schema::Demod` is declared ordinal for ordinal with `engine::Demod`, so the
conversion is a cast. `core/rpc/convert.h` carries a `static_assert` per mode,
all eight, so reordering one side without the other stops the build there.
Without that, a reordered enum would retune every receiver in a saved session
to a different mode, and nothing about the symptom would point at the schema.

The inbound direction rejects an out-of-range ordinal rather than casting it,
because a Cap'n Proto enum field can legally hold a value the reader's schema
has never heard of. That is how a newer client reaches an older engine, and a
blind cast would turn it into whichever mode happens to sit at that ordinal.

### Spectrum frames cross by copy

A shared GPU texture handle for a local client is still the right answer for a
frame at the source's own rate. It is a deferred optimisation on a path that
has to exist and be correct either way, not a missing feature.

This paragraph used to say "`core/rpc/.gitkeep` planned a shared GPU texture
handle for a local client". That file was deleted with the other placeholders
in "Remove the placeholders from directories that have their contents", so the
pointer led nowhere. What the placeholder used to say was "Spectrum frames to
a local client pass by shared GPU texture handle, not by copy", which was the
plan and not the build.

The arithmetic that makes deferring it safe, from the shipped defaults in
`core/engine/engine.h`: 64 channels at a 2048-point per-channel transform,
central half kept, is 65536 bins of four bytes, so 256 KiB a frame. A display
asking for 30 frames a second therefore takes 7.5 MiB/s. `engine.h` measures
the engine's own readback at the full block rate, about 305 frames a second at
20 MS/s, at 80 MB/s. The display costs under a tenth of what the engine is
already paying to bring the frame back from the device.

Decimation happens on the engine side, which is the point of
`subscribeSpectrum`'s `everyNth`: the engine skips those frames before copying
them, so asking for fewer costs less and not more. Decimating on the client
would have paid for the copy already. A skipped frame is not a dropped one and
the counters below keep them apart.

`rpc::SpectrumFrame` owns its bins, unlike `engine::SpectrumFrame`, which hands
out a span valid only for the duration of the sink call. That is right in the
engine and wrong here: by the time a client sees a frame it has already been
copied onto the wire, and handing a UI a span into a capnp message it does not
own is a lifetime bug waiting for the first consumer that keeps a row.

Each frame carries its own `geometry`, the `[start, start + count)` source
sample range of its window, and a `sequence` counting frames the **engine**
produced, not frames this subscription received. A client that asked for
decimation knows what it asked for; what it cannot otherwise know is whether
the engine also skipped. Keeping the two distinguishable is what lets a
waterfall say it is behind instead of silently lying about the band.

### A receiver's passband crosses the same way, and only when asked

`subscribePassband` is `subscribeSpectrum` per receiver: the same `everyNth`,
the same one-frame-in-flight backpressure, the same newest-wins drop, and a
capability whose release ends the subscription.

Two things differ, and both follow from it being per receiver rather than
per engine.

**It is opt in on the engine side, not just on the wire.** Until something
subscribes, that receiver records no transform and holds no device buffers
for one, and the last subscription going away frees them again. A recorder
serving eight receivers pays for none of it. That is why the method takes a
receiver id rather than there being a flag on `addVrx`: the cost is a
pipeline and a readback per frame in flight, per receiver, and it should be
borne by the pane that is looking.

**The geometry travels on the frame and not in `EngineInfo`.** Only the
transform size is engine-wide. The width of a passband's axis is the
receiver's own demodulation rate, which moves whenever its filter does, so
`PassbandGeometry` rides on every frame. Its `binZero` is the frequency the
fine stage mixed to DC, carried as an exact rational rather than derived: on
CW that is a sidetone below `VrxParams::center`, and a client that computed
the axis from the centre would draw one mode's filter a pitch out of place
and the other seven correctly.

### The detector's tracks cross, and they are polled rather than pushed

**This section did not mention the detection surface until 2026-09-20.** It
listed spectrum, passband, audio and then "nothing else", while
`Session::detections` had been on the wire since the schema was written and
`revenant-ui` had been drawing the result for as long as it has existed. The
omission is recorded rather than quietly filled, because a reader checking
whether the detector was reachable from a client found the wire document
saying it was not.

`detections(minConfidence)` answers with a `DetectionList`: the tracks above
that bar, ascending in frequency, plus the decision counters, the total held
before the bar, the threshold in force and `detectorHoldSeconds`.

**Polled and not subscribed**, which is the opposite of everything above it.
A spectrum frame is produced whether anyone is looking or not and an old one
is still a measurement of the band at that instant. A track list is state: it
changes about ten times a second, an older one is of no use to anybody, and a
subscription would spend the wire on revisions nobody reads. So this one call
is a poll, and it is the only surface here that is.

**The first call builds the detector and answers with nothing.** No detector
runs until a client asks for one, because it costs the engine real CPU per
frame, so the first call starts it and comes back empty with `decisions` at
zero. Zero decisions is the one answer that cannot be mistaken for a quiet
band, and it exists for that reason.

**`minConfidence` is the caller's and the threshold is the engine's.** Two
clients can hold different bars and neither sees the other's, because the bar
only filters the answer. `setDetectionThreshold` changes what the detector
decides at all, so it is engine-wide and the last writer wins;
`DetectionList::detectionThresholdDb` reads back the winner rather than
echoing a request. `ui/models/engine_link.h` holds the client half of that
split and says the same thing from the other side.

A bar of exactly one is refused rather than answered emptily. The comparison
is `>=` and a track's confidence approaches one without arriving, so a bar of
one lists nothing however strong the signal is, and an empty list is what a
dead band looks like too. How near the top of that range a real signal gets
is a separate question and not a happy one: `ui/models/engine_link.h` has the
arithmetic, and the short version is that the last fraction of the range
lists an unbroken carrier and nothing else.

**Frequencies here are integer hertz and not `Rational`**, which is the one
place this document's own rule does not apply. The detector rounds once, at
measurement, out of the frame's exact rational axis. A ratio on the wire
would dress an estimate up as a grid frequency, and a client tuning to a
detection is asking to be put near a signal rather than on a channel centre.
`Detection::centerHz` is absolute and `VrxParams::center` is a baseband
offset, so tuning is `centerHz - EngineInfo::sourceCenter`.

### Audio crosses as raw PCM, and it queues rather than skipping

`AudioChunk`, `AudioReceiver`, `AudioSubscription` and
`Session::subscribeAudio` are in the schema as of 2026-09-20, with the codec
question answered rather than deferred: there is no codec. Raw float32 PCM, per
receiver, opt in, about 1.5 Mbit/s for a mono 48 kHz receiver, which is free on
loopback and on a LAN. Opus was refused for 20 ms of added latency and for
putting a lossy stage in the middle of a chain whose whole claim is that it is
bit exact; 16-bit PCM was refused because it adds a quantisation and a dither
decision and clips headroom a settling AGC uses.

**The engine grew a composition seam to make this possible.**
`core/engine/engine.h` now carries `AudioFanout` and `attach_audio_sink`, so a
receiver feeds a recording, a loudspeaker and any number of subscriptions at
once. The single `set_audio_sink` slot is still there underneath and is what
the fan-out installs itself into; a caller reaching it directly still displaces
everything. The server attaches once per receiver however many clients are
listening, and detaches with the last of them.

**Reaching `set_audio_sink` directly is the one way left to lose audio, and it
is a case rather than a warning.** No caller in this tree does it any more:
`tools/cli/main.cpp` was the last and moved to `attach_audio_sink` on
2026-09-20, so its recording and its monitor are two consumers of one fan-out
instead of a two-way one hand-rolled in a lambda.
`tests/rpc/test_rpc_audio.cpp` pins what happens to anyone who does reach the
slot: every other consumer goes silent, no `ended` is sent, and
`AudioSubscription.stats` goes on reporting a healthy subscription, because the
server was not asked to end anything and a sink that stops being called looks
exactly like a receiver nobody is transmitting on. A VU meter or a decoder tap
wired that way kills every wire subscriber with a green suite.

**A fan-out can outlive its receiver.** `Engine` keys them by receiver id and
nothing prunes that map on `remove_vrx`, so a receiver removed while a consumer
is still attached leaves its entry behind. A later `attach_audio_sink` on that
id is refused in the engine's own words rather than joining a fan-out no chunk
will ever reach, and the stale entry is dropped on the way out. Ids are
monotonic and never reused within one engine, so the only caller who can reach
one is a caller attaching to an id it removed itself.

**This section used to say the method was refused.** Until 2026-09-20: "the
engine holds one `AudioSink` per receiver and a second `set_audio_sink`
replaces the first, so fanning audio out to subscribers means changing
`core/engine`, and that is its own branch." That was the branch. The paragraph
before it, which said audio did not cross at all, had already been retracted
once when the codec decision landed; both retractions stay because `README.md`
and `core/rpc/revenant.capnp` carried the same claims and a reader may have
taken it from any of them.

**Backpressure is a different rule from the two display streams, not a looser
one.** A spectrum frame is a measurement of a band that is still there, so an
older one is redundant and the engine keeps the newest. An audio chunk is the
only copy of that instant. So a subscription queues, up to the depth it was
granted, and when the queue is full the OLDEST chunk goes: late audio is worse
than no audio when the point is to hear what the radio is doing now, and front
eviction is what makes `framesDroppedBefore` exact rather than approximate. A
client can check `sampleIndex == previous.sampleIndex + previous frame count +
framesDroppedBefore` on every consecutive pair; a gap larger than that was lost
upstream in the engine, which is a different fault with a different fix.

**There is no `everyNth`.** Dropping every other spectrum frame halves an
update rate and loses nothing anyone wanted. Dropping every other audio chunk
is a 50 percent duty cycle of silence. A client that wants less audio
subscribes to fewer receivers.

**The depth is clamped twice and only one clamp is in the answer.**
`bufferMillis` is clamped to 20..5000 and `bufferMillisGranted` reports it,
with zero meaning the 500 ms default and coming back as 500. A two-chunk floor
is then applied in frames when the first chunk arrives, because the server
cannot convert milliseconds to frames before it knows the receiver's audio rate
and how long a chunk is, and `EngineInfo` carries neither `block_samples` nor
the engine's default audio rate while `VrxParams::audioRate` echoes zero for a
receiver that took that default. `AudioStats::bufferFrames` is the depth
actually enforcing and is zero until the first chunk sets it. The schema's note
on `subscribeAudio` records what it used to claim instead.

The floor went untested until 2026-09-20, which is why the retraction in the
schema outlived the code that was wrong. Reaching it needs a chunk longer than
the shortest depth a client can ask for, and at the test fixture's
16384-sample blocks two chunks are 13.7 ms against a 20 ms floor, so every case
read the millisecond figure straight back out. One case now runs at
32768-sample blocks, where two chunks are 27.3 ms and the floor is what
`bufferFrames` reports.

**`AudioStats` is per subscription.** `Server::frames_dropped` is server-wide
and its own comment admits it over-counts across spectrum subscribers; a slow
client's drops must never appear on a fast client's status line, and audio has
no shared decimation to excuse it. Audio does not touch that counter at all.

**`ended` exists because audio has no visible failure.** A receiver removed out
from under a spectrum subscription freezes a picture and a frozen picture is
obvious from across the room. The same event here produces silence, and silence
is what a quiet channel with the squelch shut sounds like, so the subscriber is
told in words. It is best effort, it is never sent for a cancel the client
asked for, and a server whose event loop has already stopped cannot send it at
all; a dropped connection is the other signal.

**A client that has been sent `ended` drops the subscription capability**, and
`core/rpc/client.h`'s client did not until 2026-09-20. Keeping it had two
consequences and the visible one is the worse: `audio_stats` found the stale
capability, asked the server, and came back Ok with the dead stream's frozen
counters, so a status line polling it showed a healthy subscription on a
receiver that had been removed. The other is that the capability holds the
server's node and its queue open until the connection drops, so a client
cycling receivers accumulated one per removal. Both paths now go through the
same teardown, and the server refuses `stats` on a subscription it ended
whatever the client does.

**`squelchOpen` rides on every chunk** for the same reason: a closed gate is
not a drop and not a gap, `core/engine/graph.cpp` fills the chunk with zeros
and sends it at the full rate, and nothing else in the stream distinguishes
that from a transmitter that stopped.

**The raw tap is refused**, in the server's own words rather than the engine's,
because the engine would install a sink on it happily. `RawTapStage` hands back
interleaved complex I/Q at the coarse channel rate, which a client playing it
as two-channel PCM renders as noise at the wrong speed, at tens of times the
rate this design was costed at. An I/Q subscription is a separate method that
does not exist.

D-STAR and TETRA are refused too, in their own words. The three digital voice
modes left the raw tap for the fine stage on 2026-09-22 and hand out complex
baseband mixed to DC at the rate their decoder was built for, 48000 S/s for
P25 and D-STAR and 72000 for TETRA, which `VrxStatus::demodRate` states. Until
that change the refusal called every one of them "a raw tap" at "the coarse
channel rate", which named the wrong path and the wrong rate for three of the
four. It names the mode and the rate, says the engine has no AMBE or ACELP
voice codec, and points at `subscribeDecoded`, which is what reads that
stream.

**A P25 receiver's audio is its voice**, since 2026-09-23 and on the owner's
decision that for a digital voice receiver the decoded voice takes the place
of the receiver's analog audio. The next section but one has how it is served.
WHAT THE PARAGRAPH ABOVE USED TO SAY, in its first sentence: "The three
digital voice modes are refused too, in their own words."

### RDS is served, per receiver, off the audio fan-out

This section used to be headed "RDS is still declared, allocated, refused" and
said that `Session::rdsStation` and `Session::setRdsRegion` remained unwired
because nothing in `core/engine` fed the decoder a composite. Both are served
as of 2026-09-20 and nothing in `core/engine` feeds the decoder still. That
part of the sentence was a prediction about which lane would do the work, and
it was wrong.

**The decoder lives in `core/rpc/server.cpp`**, one per receiver, built on the
first call to either method. It joins that receiver's audio through
`Engine::attach_audio_sink` and runs on the engine's completion thread inside
the sink, under that route's own lock. That is the detector's shape, and the
detector is only half an argument for it: the detector had nowhere else it
could go, and RDS did. `core/engine` could have grown a per-receiver decode
stage. Three things decided against it. `attach_audio_sink` is the composition
point and it exists, so a second decoder seam inside the engine would mean the
one written first gets deleted, which is the argument `core/engine/vrx.h`
already made from its own side. A decoder in the engine would have to hand
state back through an `Engine` method, which means `core/engine` depending on
`core/decode` and an engine-level mirror of `decode::StationState` beside the
schema's. And nothing in the engine knows a client asked, which is the whole
saving: a receiver nobody has asked about runs no decoder.

**The composite reaches it through the ordinary audio path and costs no new
stage.** A WFM receiver whose `audioRate` is 171000 demodulates at 342000 and
decimates by two, so its audio filter's passband edge lands at 68400 Hz and the
whole composite to 59375 survives. At the usual 48 kHz the demodulation rate is
336 kHz, the passband edge is 19.2 kHz, and 57 kHz is deep in the stopband.
171000 is three times the subcarrier and 144 times the bit rate, both exact,
and it is `decode::RdsBitsConfig::rate`'s default. No new kernel, no readback
and no resampling.

**IT COSTS MORE GPU THAN A LISTENING RECEIVER, NOT LESS.** This paragraph used
to end "slightly LESS GPU than a listening receiver: the audio FIR runs at the
output rate and needs 103 taps instead of 353", and the arithmetic in that same
sentence disproves it. `core/dsp/vrx_reference.cpp` runs the audio decimation
FIR at the OUTPUT rate, one detector evaluation per tap per output sample, and
for a discriminator each of those evaluations is an `atan2`. So the count is
`output_rate * audio_taps`:

    listening   48000 x 353 = 16.9 M atan2/s
    RDS        171000 x 103 = 17.6 M atan2/s

Fewer taps, three and a half times the rate, and the product is 3.9 percent
higher. The tap counts are what the planner designs: 48 kHz of audio demodulates
at 336000 and decimates by seven, 171000 demodulates at 342000 and decimates by
two, and `kaiser_taps_for(80 dB, 0.1 * audio_rate / demod_rate)` rounded up to
odd gives 353 and 103. The error arrived with the task brief that asked for this
decoder and was repeated rather than checked. Four percent is still small, and
small is the honest claim; "less" was not.

**It is a dedicated receiver, and the rate is what makes it one** rather than
the sink. The schema used to say this call took the receiver's audio sink and
was refused on a receiver that already had one; `AudioFanout` made that false
and the retraction is in place. What is still true is that a listening receiver
at 48 kHz fails the rate condition, so the decode and the listening cannot
share one receiver unless the operator is willing to listen to a composite. It
costs one extra receiver on the grid.

**Four conditions, not the one the audio rate suggests**, each refused in its
own words naming which failed: the demodulator has to be `nfm` or `wfm`, the
audio has to be real and mono, the audio rate has to clear 148438 when the
planner designed an audio filter and `core/decode`'s own 125000 when the
decimation resolved to one, and the granted passband has to reach 59375 Hz
either side of the mix centre. A receiver that took the engine's default audio
rate is refused as well: `VrxParams::audioRate` is a verbatim echo and comes
back zero, `EngineInfo` does not carry the default, so it is a rate the server
cannot name rather than one it can check. Putting the resolved rate on
`VrxStatus` would close that and is a schema change nothing else has needed.

**Lifecycle.** Every poll asks `vrx_status` first, so a receiver removed by any
path, including one this session never saw, is answered in the engine's own
words and its decoder dropped there. `removeVrx` takes the sink off promptly.
`setVrxParams` resets the decoder, on every retune and not only one that moved
the centre: the server is handed a whole `VrxParams` and cannot tell a wider
filter from a hundred kilohertz away without keeping a copy that could go
stale, and a receiver that moved is on a different transmitter whose PS and
RadioText would otherwise be assembled over the old one's.

**The reset fences the stream and not only the struct**, which is a second
thing and was not done until 2026-09-20. `Engine::set_vrx_params` QUEUES a
control op; the recording thread applies it at the next block boundary, and
every frame already recorded is the old tuning and still on its way. Clearing
the decoder on the loop thread and stopping there handed those frames to a
decoder that had just been told it was somewhere else, so the old station's
bits became the new station's first samples. That reads as a decode rather
than as staleness, which is the worse of the two failures. `AudioChunk` now
carries a per-receiver `tuning_epoch`, the graph moves it when it applies a
retune, and the decoder discards until it crosses the boundary. Two retunes
back to back wait for two boundaries. The one case it does not fence is a
retune arriving before the decoder has seen a single chunk, where there is no
epoch to fence against and nothing accumulated to protect.

**The region defaults to `rds` and is per receiver**, where
`setDetectionThreshold` is per engine. Both are as wide as the thing they
configure: there is one detector, and there is one decoder per receiver.
Nothing infers it: no field names the region, the PI cannot decide it because
the US call sign range collides with European country codes, and getting it
wrong is silent, since PTY 26 draws as National Music in one region and Hip-Hop
in the other. A client may seed it from the tuned frequency as long as it shows
it as a setting the operator can override. Setting it on a decoder that already
exists clears everything accumulated, INCLUDING the physical layer, which the
region does not reach: every counter in `RdsHealth` is cumulative from the
moment the decoder was built, and clearing one layer would leave one struct
holding two epochs.

**Per receiver means shared by every session on that receiver**, which is the
half "per receiver" does not say and which one client can feel through
another. Two sessions polling one receiver hold one decoder between them, so
`setRdsRegion` from either clears what the other had accumulated, with no
notification and no way to tell it from a station that went off the air.

That is the same answer as `setDetectionThreshold` rather than a different
one, and the deciding argument is the same argument. A receiver is engine-wide
state: two sessions on one already share its centre, its filter and its
squelch, and either of them calling `setVrxParams` clears the other's station
too. Nobody would call that a bug, because the receiver is the shared thing
and the decoder hangs off it. A per-session decoder would mean one decode per
client per receiver on the completion thread, which is the cost the whole
"nobody asked, nothing runs" arrangement exists to keep down, and it would
make two clients disagree about a station that is one station. A client that
needs a region of its own creates a receiver of its own. It is already
creating a dedicated one to get the 171 kHz rate, so this costs nothing it was
not already paying.

**`RdsStation::fault` is how a decoder says it stopped.** Empty while it runs.
Non-empty means the receiver delivered a chunk the decoder was not built for,
an interleaved pair or a rate its loops are not sized for, and every other
field is frozen at the last chunk it accepted. Terminal for the life of the
receiver: both faults are shape, and a retune that changes the shape is
refused rather than applied, so there is nothing a client can do to that
receiver that would make it deliver a composite again. Remove it and add
another. Until 2026-09-20 a faulted decoder made `rdsStation` throw with this
sentence as the message, which threw away the state the decoder had built
before the bad chunk and put "the decoder stopped" on the same channel as "no
such receiver". The detector still refuses on its own fault and should: a
detector fault means the track list no longer describes anything, where this
one leaves a struct that was true when it was written.

**`tmc`, `ews` and `ptynCorrected` are counts and raw bits, and a client must
draw them as that.** `ews.groups` above zero is the one to surface
prominently: EN 50067 has 9A sent only in an emergency or a test of one, and
the payload is each country's own format, so show that it happened and at
most the bits in hexadecimal. `tmc` says whether a service is on air, whether
it came in through a 3A announcement, and every distinct 37-bit payload with
its reception count; show the ones with two or more receptions, which is ISO
14819-1's own confirmation rule, and count the rest. There is no event or
location code in it, because the field positions were not in the part of ISO
14819-1 that was read, and a client that picks 16 bits out and calls them a
location is inventing a layout. `ptynCorrected` marks programme type name
segments a correction touched; the decoder keeps the same mark for PS and
RadioText, and neither of those two is on the wire yet. Transparent data,
in-house and paging groups are counted in the
decoder and deliberately not on the wire: none has anything an operator reads,
and paging is somebody else's pager.

**What it costs**, measured rather than asserted: 12.07 ms of one core per
second of composite at 171000 S/s, which is 0.145 ms per chunk against the
detector's 0.201 ms per frame, per decoding receiver rather than per engine.
The CPU share is not the number that matters. It runs on the thread retiring
GPU readbacks, so what it costs is latency in front of every other sink there:
0.145 ms inside a 12 ms chunk interval. Eight decoders would be 1.2 ms of that
interval, still inside it and no longer negligible, which is where a decoder
wants its own thread instead of the sink.

All three ordinals were allocated in one pass with the login bootstrap because
a Cap'n Proto field number is permanent and three branches appending to one
schema would collide. Nothing moved when each landed, which is the whole of
what that bought.

### Every other decoder reports through one seam

`Session::decoders` lists what the engine can attach and
`Session::subscribeDecoded(vrx, decoder, receiver)` attaches one to a receiver
and streams what it recovers as `DecodedMessage`s. Fourteen decoders report
through it as of 2026-09-23, each one adapter and one registry row in
`core/rpc/decoders.h`, with the keys it emits listed above its adapter.
WHAT THE SENTENCE BEFORE THAT USED TO SAY: "Thirteen decoders report through
it as of 2026-09-22"; `dmr` is the fourteenth.

| Name | What comes out | What it needs from its receiver |
| --- | --- | --- |
| `p25p1` | NAC and DUID of every data unit, with the carrier offset and deviation its sync word measured; the header's talkgroup, algorithm, key and encrypted flag | Complex baseband of a `p25p1` receiver, or a `raw` tap up to 192000 S/s |
| `dstar` | The radio header's four callsigns, suffix and flags, then one message per superframe of voice frames | Complex baseband of a `dstar` receiver, or a `raw` tap up to 192000 S/s |
| `tetra` | Synchronisation bursts: MCC, MNC, colour code, timeslot, frame numbers | Complex baseband of a `tetra` receiver, or a `raw` tap up to 192000 S/s |
| `dmr` | Per timeslot: the voice LC header, embedded LC and terminator with talkgroup, source, service options and privacy; CSBKs, data and PI headers; Short LC from the CACH | Complex baseband of a `dmr` receiver, or a `raw` tap up to 192000 S/s |
| `m17` | Each link setup whose CRC checked: callsigns, type, encrypted flag; each stream's end; end of transmission | Complex baseband of a `p25p1` receiver, 48000 S/s in its 12.5 kHz channel, or a `raw` tap up to 192000 S/s |
| `rtty` | Lines of ITA2 text, 45.45 baud, 170 Hz shift, mark on 2125 Hz | Audio of a `usb` or `lsb` receiver; the sideband sets the polarity |
| `sitor_b` | Lines of text, each character from whichever of its two copies arrived | Audio of a `usb` or `lsb` receiver, tones about 1700 Hz |
| `navtex` | Each message with its B1 to B4 letters, serial and whether the preamble was clean | Audio of a `usb` or `lsb` receiver, tones about 1700 Hz |
| `psk31`, `psk63`, `qpsk31` | Lines of Varicode text, with the measured tone offset | Audio of a `usb` or `lsb` receiver, tone at 1000 Hz; QPSK31 takes its sideband from the receiver |
| `cw` | Lines of Morse text with the dots and dashes, character and overall speed | Audio of a `cw` receiver at its default 700 Hz pitch, or a `usb` or `lsb` one with the tone at 700 Hz |
| `ax25` | Every AX.25 frame whose FCS checked, with its APRS position, Mic-E, status or message parsed when it is APRS | Audio of an `nfm` receiver |
| `pocsag` | Pages at 512, 1200 and 2400 bit/s at once: address, function, numeric or alphanumeric message | Audio of an `nfm` receiver |

WHAT THIS PARAGRAPH USED TO SAY: that RTTY, APRS, POCSAG, PSK31, CW and M17
would each reach the wire "as one adapter and one registry row in
`core/rpc/decoders.h`, with nothing in the schema, `types.h`, the client or the
server moving". The schema, `types.h` and the client did not move. The server
did, once, for the list of modes in the right-hand column: before it, a
decoder said only whether it read complex baseband or audio, so RTTY on a `wfm`
receiver was accepted and would have printed broadcast audio as teleprinter
noise. A decoder added now is one adapter and one row again.

**`DecoderInfo::modes` names the right-hand column as a list**, since
2026-09-23, for a client that offers an operator only the decoders a receiver
can feed. It is always the whole list, never empty for "any", because empty is
what an engine older than the field sends and a client has to be able to tell
the two apart. The descriptions still say the modes in words, for a person
reading the list.

**A raw tap is held to 192000 S/s for the complex decoders.** `p25p1`, `dstar`,
`tetra` and `dmr` read their own mode's fine stage, at the rate each was written for,
or a `raw` tap, and `m17` a `p25p1` receiver or a `raw` tap. A raw tap runs at
the grid's channel rate, which nothing else caps, and a decoder's receive
filter grows with the rate and runs on the completion thread that delivers
every receiver's output: at 600 kS/s P25 took 602 ms of a core per second of
input on the RTX 4090 machine, against 66 ms at 192000. So `subscribeDecoded`
refuses a raw tap above `kRawTapRateCap` in `core/rpc/decoders.h`, naming the
cap and the mode to use instead, and the adapter refuses the same on its first
chunk for `revenant-cli`. WHAT THIS PARAGRAPH'S PREDECESSOR USED TO SAY: "the
three complex decoders, which read any complex tap, cross as `raw`, `p25p1`,
`dstar` and `tetra`".

**One message shape and not a struct per mode.** A message carries the
receiver, the decoder's registry name, a `kind` within that decoder, a sample
span, a list of fields and an optional line of text. A field is a key and a
value that is one of an integer, a real, a flag, text or bytes, so a talkgroup
is an integer on the wire and a client sorting by it parses nothing. A struct
per mode would be a schema change, a mirror, a conversion and a client change
per decoder, and a client built before a mode existed could not show it. The
keys each decoder emits are listed above its adapter, and a key once published
is not renamed, on the ground a field ordinal is not renumbered.

**RDS stays where it is.** A station is a state that accumulates over many
groups and is polled whole; what these decoders produce is events, and an event
polled is an event missed. The two surfaces have different shapes on purpose.

**Attaching is subscribing.** Nothing decodes until something subscribes, two
subscribers to one decoder on one receiver share one instance, and the last
leaving takes it off, which is `subscribePassband`'s and `subscribeAudio`'s
rule and for their reason: a decoder costs the completion thread on every chunk.
An empty decoder name means the one named after the receiver's mode and
`decoderResolved` says which ran; the mode chooses the channel filter and the
decoder what is read out of it, so they are separate, and `ax25` reads an `nfm`
receiver's audio. On a `usb`, `lsb` or `nfm` receiver, which no decoder is
named after, an empty name is refused with the list of decoders that read it,
because a sideband receiver may be carrying any of seven. Refused in words for
a receiver that is not there, a name the engine does not have, an input the
receiver cannot give, and a receiver outside the decoder's modes: `rtty` on a
`wfm` receiver is told it needs `usb` or `lsb`, and `m17` on a `dstar` one that
it needs `p25p1` or `raw`.

**Where it runs.** On the engine's completion thread inside an audio sink
joined through `attach_audio_sink`, exactly as the RDS decoder does, with the
retune fence it uses: `setVrxParams` and `setSourceCenter` reset every decoder
on the receiver and discard chunks recorded at the old tuning. The decoder is
built on the first chunk at the rate that chunk carries and never from
`VrxStatus`, because the two complex paths deliver at different rates: a
digital voice receiver's fine stage at 48000 or 72000, which `demodRate` states,
and a raw tap at the channel rate, which `demodRate` does not.

**The audio decoders take the same path.** To the engine a demodulator's audio
and a complex tap are both a receiver's output through the same sink, one float
a frame or two, so an audio decoder is fed the mono audio a `usb`, `lsb`, `cw`
or `nfm` receiver produces, at the receiver's resolved audio rate, 48000 unless
it asked for another, behind the same fence and into the same 256-message
queue. The adapter refuses a chunk of any other channel count, which is what a
stereo `wfm` receiver would deliver, and the mode check above stops it getting
that far. The factory is told the receiver's mode as well as the rate, because
the sideband decides which audio tone the higher radio frequency lands on: it
sets RTTY's polarity, the polarity SITOR-B and NAVTEX try first, and the phase
direction QPSK31 reads.

**Text arrives as lines.** RTTY, SITOR-B, PSK and CW produce characters, and a
message a character would fill a subscription's 256 in 42 seconds of RTTY, so
their adapters hand out a line: at a line end, at 80 characters, or when the
channel has been quiet for ten characters, measured from when the last
character arrived rather than from where it sits, because PSK31 and CW release
their first characters in one batch once they have found the tone. Every line
carries `began_sample`, the receiver-stream index where it started, alongside
the span of the chunk that completed it. A start-stop receiver on noise frames
characters out of it, as a teleprinter does, so an RTTY line carries
`min_margin` for a client to judge it by.

**Delivery queues, as audio's does.** A message is the only copy of an event,
so each subscription holds up to 256 and evicts the oldest when full, and
`droppedBefore` on the next message says how many went. `sequence` is counted
per decoder, so two subscribers see the same numbers.

**`ended()` says why a stream stopped**, on `AudioReceiver::ended`'s argument
that a stream which simply stops looks like a quiet channel: the receiver was
removed, by anybody or by its session ending; the decoder refused what the
receiver delivered, which is terminal for that decoder on that receiver; or a
`message()` call failed.

**The sample span is where the message completed**, `[startSample,
endSample)` in the receiver's own stream at `sampleRate`, which is the chunk
that delivered its last sample. It bounds when the message ended to within one
chunk and does not say where it began.

**The adapters are a header** because two programs run them and only one links
the wire. `revenant-cli --decode <name>` attaches the same code to its `--vrx`
receivers in-process and prints each message; `revenant_core` is all it links.

**Measured, not asserted**, by `tests/rpc/test_rpc_decode.cpp` on 2026-09-22
against the RTX 4090. A P25 capture carrying six headers, three in clear and
three encrypted, through a four-channel grid at 288000 S/s with the carrier 5 kHz
off the channel centre: four headers crossed with their talkgroup, algorithm,
key id and encrypted flag intact, from both of two subscribers under the same
sequence numbers. A D-STAR header crossed with every callsign and the flags.
TETRA crossed at least nine of eighteen synchronisation bursts, with MCC, MNC,
colour code and timeslot. The P25 adapter spent 83.58 ms of one core per second
of 48000 S/s input in a Debug build, and 21.50 to 24.53 ms over three runs of
the ci preset's RelWithDebInfo build. WHAT THIS SENTENCE USED TO SAY after the
Debug figure: "an optimised build was not measured".

**The audio decoders, measured** by `tests/rpc/test_rpc_decode_audio.cpp` on
2026-09-22 against the RTX 4090, on the same grid. Each transmitter from
`core/dsp/synth` is put on a carrier the way a station would: by single
sideband for the HF modes, as a keyed carrier for CW, by FM for AFSK and as
direct FSK for POCSAG, and M17 as its own 4FSK. Noise is stated in 2500 Hz at
the radio frequency and covers the silence either side of the transmission.
At 30 dB every decoder's content crossed exactly: both RTTY lines on `usb` and
on `lsb`, both SITOR-B lines, the NAVTEX message with `EA07` and a clean
preamble, all four AX.25 frames with the APRS position, status and message
parsed and the plain I frame reported as one, all four POCSAG pages including
the one at 512 bit/s, both lines of PSK31, PSK63 and QPSK31 on `lsb`, CW at
20.0 WPM, and M17's link setup, stream end and end of transmission through a
`p25p1` receiver. At a lower point near where each library test says it
degrades:

| Decoder | Point, dB in 2500 Hz | What crossed |
| --- | --- | --- |
| `rtty` on `usb` | -8 | character error rate 0.097; none at -5, 0.45 at -10 |
| `sitor_b` | -5 | character error rate 0.097 |
| `navtex` | -5 | the message exact with a clean preamble; damaged at -6, nothing at -8 |
| `psk31` | -10 | both lines exact |
| `psk63` | -7 | both lines exact |
| `qpsk31` on `lsb` | -11 | character error rate 0.10; at -12 it never acquired and printed nothing |
| `cw` | -10 | character error rate 0.059, read at 19.1 WPM |
| `ax25` | 16 | two of four frames; none at 14, all four at 20. Noise ahead of the discriminator, which has a threshold, unlike the library test's noise on the audio |
| `pocsag` | 8 | four of four pages, one with an uncorrectable codeword; all intact at 10 |
| `m17` | 15.6 | the link setup, stream end and end of transmission; 15.6 dB is `test_m17.cpp`'s 10 dB in the 9 kHz channel |

Two properties of the libraries, found through the receiver and not fixed
here because `core/decode` is not this seam's to change. POCSAG at 6 and 4 dB
reports pages for addresses nobody sent, several with no uncorrectable
codeword, which is the BCH code correcting noise into an address; the page's
`corrected_bits` is how a client weighs one. M17 in the silence after a 30 dB
transmission found two link setups whose CRC failed, one BERT burst and two
packet bursts in 2.5 s, so the adapter drops all three kinds and counts the
failed link setups on the next good one.

What they cost, read off `revenant-cli` in the RelWithDebInfo build as the wall
time of a whole run with one decoder attached, not a per-decoder profile, the
sideband decoders on the 9.95 s RTTY capture and the FM ones on the 9.54 s
POCSAG capture: RTTY, SITOR-B and NAVTEX
0.031 s, AX.25 0.031 s, POCSAG 0.021 s, M17 0.041 s over its own 4.12 s,
PSK31 0.323 s, PSK63 0.353 s, CW 0.372 s and QPSK31 0.888 s. The last four mix
and filter every audio sample before decimating, and QPSK31 runs a Viterbi
decoder besides, all on the completion thread.

**P25 voice is on `subscribeAudio`**, since 2026-09-23. `core/decode/p25p1.h`'s
`P25Voice` turns a clear call's LDU voice frames into 8 kHz PCM; it is audio
out rather than events, so it went on `subscribeAudio` rather than this seam,
built as the smallest change this section had named: an audio route in
`core/rpc/server.cpp` for a `p25p1` receiver that runs `P25Phase1` and
`P25Voice` in the sink, under the fence the decoder routes use, with no schema
change because `AudioChunk` carries its own rate. `core/rpc/voice_audio.h`
holds it and answers the three questions:

- `sampleIndex` counts frames at 8000 S/s, `floor(start * 8000 / rate)` of the
  receiver's own chunk, so a chunk covers exactly the time the receiver chunk
  behind it covers and an upstream gap is a gap of the same length here.
- The time between calls, and an encrypted call, are silence at the full 8000
  S/s with `squelchOpen` false, which is how a closed squelch already crosses.
  `P25Voice` hands nothing out for an encrypted call, so nothing of one plays.
- The refusal became the subscription. D-STAR and TETRA are still refused,
  saying there is no AMBE or ACELP codec in the engine.

`P25Voice` hands a call over an LDU at a time, 1440 samples every 180 ms of
air, detected at the end of whichever engine block holds the LDU's last
symbol, so the voice of a call starts behind a pre-roll of the longer of 50 ms
and the longest chunk seen. A shorter one underruns by up to a chunk on every
LDU. `tests/rpc/test_rpc_voice.cpp` holds the voice to the vocoder's own PCM
for the frames the transmitter sent, sample for sample and with no gap inside
the call, at 328, 2731 and 16384 samples a chunk and over the wire through a
p25p1 receiver on a 288000 S/s capture; a fixed 50 ms underran at 16384. An
encrypted call crosses as chunks of zeros with the gate shut.

WHAT THIS PARAGRAPH USED TO SAY, headed "P25 voice is not on the wire": "and
nothing serves it", and "`subscribeAudio` refuses a `p25p1` receiver today
because its output is a complex tap", followed by the questions above.

**Two things found on the way, both in `core/decode/p25p1.cpp`.** A Header Data
Unit whose sync and NID arrived in one call and whose body arrived in the next
was reported without its header and never tried again, so through the engine a
P25 stream reported NACs and no talkgroups; that is fixed and
`tests/decode/test_p25p1.cpp` splits a header across two calls to hold it. The
other was that the decoder's output depended on how its input was blocked: the
same capture gave four of its six headers at 16384-sample engine blocks and one
at 65536, `revenant-cli`'s default. Three steps restarted at every call: the
receive filter from zeros, the discriminator against 1+0i, and each call's mean
taken as the carrier offset, costing most to least in that order. The filter and
discriminator now carry their state across calls, and each data unit is sliced
against a least-squares fit of its own sync word, so no per-call estimate
remains. The decoder's output is now identical in every blocking down to one
sample per call, and this route recovers six of six at both block sizes.
`docs/modes.md` has the measurements. D-STAR and TETRA carry their state across
calls as well since 2026-09-23, and `docs/modes.md` has theirs too: every
blocking down to one sample a call gives the whole capture's answer. WHAT THIS
SENTENCE USED TO SAY: "D-STAR and TETRA still restart their state per call and
have not been measured for it."

**The D-STAR adapter reports a transmission a superframe at a time.** `DStar`
hands a transmission over in the pieces its own structure sets, the header with
the first 21 voice frames and a piece per superframe after it, and the adapter
sends one message per piece: `header` for the first, `superframe` for each after
it, with the frame count, the running total, `ended` on the piece the clause
4.1.2 h last frame closed, and `my` and `ur` repeated so a superframe can be
placed without its header. Before 2026-09-23 it sent the header alone, with
`voice_frames` counting whatever had arrived in the same call, and dropped every
superframe after the first. A transmission still open when its receiver goes is
flushed: the server asks the decoder for what it holds, sends that and anything
still queued ahead of `ended()`, and flags the piece `flushed`. `revenant-cli
--decode` flushes every decoder when the run ends. The fifty-frame transmission
in `tests/rpc/test_rpc_decode.cpp` arrives as pieces of 21, 21 and 8.

Every other adapter that holds something is flushed the same way. POCSAG hands
over a page whose closing idle codeword the stream cut short and NAVTEX a
message still waiting for its `NNNN`, both flagged `flushed`; RTTY, SITOR-B,
the PSK modes and CW hand over the line they were gathering, CW with the
character being keyed, QPSK31 with the characters its Viterbi decoder was
still holding back and SITOR-B with the characters whose RX copy never came,
taken from the DX copy alone and counted in the line's `single_copy`, and
`ended` reads `stream_end`. A flushed message is
stamped where the last chunk ended, with no length, since no chunk completed
it. `tests/rpc/test_rpc_decode_audio.cpp` removes a receiver on each of them
with its last page, message or line still open and reads it arriving ahead of
`ended()`. P25, TETRA, AX.25 and M17 hold nothing a client could read at the
end of a stream, and a flush of one appends nothing.

### The front end can be pointed somewhere else

`Session::setSourceCenter` retunes the source and answers with the centre the
device took, which a synthesiser with a tuning step will round.
`Session::sourceCanRetune` says whether it will work at all and over what
range, so a client can grey a control out rather than offering one that
always refuses.

The channelizer, every receiver and the whole spectrum stage work in the
source's baseband frame and are never told where the front end is pointed:
`core/engine/vrx_place.cpp` is handed the grid, the rate and the request and
nothing else. So a retune moves the device's own oscillator,
`EngineInfo::sourceCenter`, and every receiver's baseband offset, which the
engine rebases so the receiver stays on the absolute frequency it was tuned
to. A receiver whose centre then falls outside the new span is removed.

**The answer names every receiver the retune removed.** `removed` carries each
one's id, the absolute frequency it was on before the tune, why, and the
engine's sentence saying so. `cause` is `outsideSpan`, `unplaceable` when the
channel planner refused the new place, or `shapeChanged` when the new place
needs a different filter, which the graph builds for a new receiver and not a
running one; `unknown`, ordinal zero, is what a server built before the field
sends. An enum as well as `reason` because a client acts on it: an add at the
same frequency brings back a `shapeChanged` receiver and is refused for the
other two, and matching that out of the sentence would break the day the
sentence was reworded. `tests/rpc/test_rpc_session.cpp` reads both off a real
engine's refusal. By the time
it arrives the server has done for each what `removeVrx` does: every audio and
decoder subscription on it has been sent `ended()` with a reason naming both
frequencies, and its RDS decoder and ownership record are gone.
`Client::retune_source` reads the list; `Client::set_source_center` still
answers with the centre alone. Before 2026-09-23 the list was discarded in the
server: an audio subscriber on a removed receiver got no `ended()` and went
quiet, which is what a shut squelch sounds like, and `vrxIds` changing was the
only trace. `tests/rpc/test_rpc_audio.cpp` strands one of two receivers and
holds both halves.

WHAT THE TWO PARAGRAPHS BEFORE THIS USED TO SAY, and they have been false
since the rebase shipped on 2026-09-21: "a retune moves the device's own
oscillator and `EngineInfo::sourceCenter`, and nothing about a placement, a
filter, an audio stream or a subscription changes", then "A receiver stays
where it is in baseband, so it is now hearing a different piece of spectrum.
Deciding what each open receiver was *for* and moving it is the client's job".
The engine does the moving now, for the reason "Not done yet" below records.

Every other absolute frequency a client is holding is stale when the call
returns. Read `info()` again rather than adding the delta: the answer is where
the device landed, not what was asked for.

**What is deliberately not reset.** The device ring still holds samples
captured at the old centre. It is a streaming window rather than a cache,
nothing reads behind the write cursor but the channelizer's own filter
support, and the stale span is bounded by the prototype length plus one
block. Renumbering the stream to discard it would break the absolute sample
index every chunk, frame, recording and counter is correlated against, to
avoid a transient of tens of milliseconds. The spectrum's colour map is not
reset either: it tracks percentiles over about thirty seconds and recovers on
its own, where a reset would make both ends jump after every small retune.

**What is reset.** Every receiver's tuning epoch, by re-queueing each
receiver's own params so the graph advances it through the path it already
applies at a block boundary. That is what lets the per-receiver RDS fence
work for a change that is not per receiver. Above the engine, the server
drops the wideband detector, whose tracks were measured against the old
constant and describe a band that is no longer there, and clears every
decoder.

A refusal comes back in the **source's** own words. A file says a recording's
centre is a property of bytes on disk and to reopen the URI; a synthetic
scene says to move the emitters with `span_low` and `span_high`; a dongle
names the ranges it reaches. Three different things to do about it, and a
refusal composed in this layer would have replaced all three with a category.

**On an RTL-SDR the transfers stop while the tuner moves, and that is
librtlsdr's limit rather than a choice this layer made.**
`rtlsdr_set_center_freq` on a dongle that `rtlsdr_read_async` has been running
on for more than about half a second fails with `LIBUSB_ERROR_PIPE`, and
librtlsdr prints its own account of it:

```
rtlsdr_demod_write_reg failed with -9
r82xx_write: i2c wr failed=-9 reg=1a len=1
r82xx_set_freq: failed=-9
```

The first line is the transfer that failed: the DEMOD register write that opens
the RTL2832U's I2C repeater, not a tuner register. Measured on 2026-09-21 on a
Generic RTL2832U OEM with an R820T tuner, Windows 11, librtlsdr from vcpkg's
static triplet.

- One retune per process run, so no result is attributable to a previous failed
  attempt. A retune at 252 ms lands. One at 522 ms, 836 ms, 1225 ms, 2026 ms and
  5028 ms is refused.
- The URB length does not move the boundary. 16 KiB, 64 KiB and 512 KiB
  transfers all fail at around 330 ms.
- It is not this tree's code. A probe calling librtlsdr directly, `rtlsdr_open`
  then centre, rate, gain mode and agc, then `rtlsdr_reset_buffer`, then
  `rtlsdr_read_async` on a bare thread with a counting callback, then
  `rtlsdr_set_center_freq` from the calling thread, returns 0 at 310 ms and -9 at
  1026 ms and 3024 ms. So this is librtlsdr, libusb or the WinUSB binding.
- Issuing the call from inside the `read_async` callback returns
  `LIBUSB_ERROR_BUSY` instead, because a synchronous control transfer submitted
  from within libusb's own event handling cannot complete. That route is closed.

What does work, six consecutive rounds out of six: cancel the async read, join,
retune, reset the buffer, restart the async read.
`RtlSdrSource::retune_streaming_locked` is that sequence. The first
`rtlsdr_set_center_freq` after the cancel returned -9 every single time and the
second returned 0 every single time, which is why the retune is attempted up to
four times rather than once.

What it costs a client is about 330 ms of stream, roughly 790,000 samples at
2.4 MS/s. It is still a retune rather than a source change: the stream is not
ended and `EngineInfo::sourceEpoch` does not move. The gap is reported through
the path a consumer too slow to keep up already uses, `SourceStats::samplesLost`
and `overrunEvents` on the wire and `SourceBlock::dropped_before` on the block
boundary inside the engine. The sample index is advanced by the number of
samples that went missing rather than carrying on from where it stopped, which
is what keeps every timestamp after the retune right: a Paced source's timestamp
is its anchor plus its index over the rate, so an index that did not skip the
pause would put the rest of the stream a third of a second early and leave it
there.

**It is not only the retune, and that was found the hard way.** The tuner's gain
registers sit behind the same I2C repeater, so `setSourceGain` and
`setSourceGainAuto` stall exactly as `setSourceCenter` did and pay the same
pause. Only the retune stopped the transfers at first, and an operator whose
broadcast FM audio sounded overloaded reached for the dongle's own AGC from the
window and locked it up. Any control transfer to a streaming RTL-SDR goes
through the same stop now; `RtlSdrSource::with_transfers_paused` is the one
place it is written.

A receiver survives a retune, and what it hears changes. A receiver is placed in
the source's BASEBAND frame, so moving the front end drags every receiver with
it: one at an offset of +100 kHz was hearing 98.2 MHz at a centre of 98.1 and
hears 435.1 MHz at a centre of 435. Nothing refuses and nothing is dropped.
Whether that is right is an open question rather than a settled design; see "Not
done yet".

**WHAT THE TREE USED TO SAY ABOUT THIS REFUSAL, and it is now false.** The
symptom was first diagnosed as a dongle opened with no centre frequency and left
at DC. `core/source/rtlsdr_source.cpp` said of that open: **"The tuner is in a
failed state from that moment: every later rtlsdr_set_center_freq returns
LIBUSB_ERROR_PIPE, which reaches an operator as `the tuner refused 435000000 Hz:
librtlsdr returned -9` and points at the frequency they asked for rather than at
the one nobody asked for."** And `tests/engine/test_rtlsdr_source.cpp` said **"The
symptom reached the operator as `the tuner refused 435000000 Hz: librtlsdr
returned -9`, naming the frequency they asked for rather than the one nobody
asked for. Six narrower cases were written chasing it and all six passed, because
every one of them supplied a centre."**

The refusal-to-open-at-DC guard is still right and stays: asking an R820T to
lock DC does fail and does leave the tuner unable to tune. What it does not do is
explain the reported symptom, which happens on a dongle opened correctly at
98.1 MHz. Confirmed by the frequency axis in the window reading 96.75 to
99.15 MHz while every retune was still refused. The six narrower cases passed
because each of them retuned within about a quarter second of starting the
stream, not because each of them supplied a centre.

**The measurements are reproducible, and a later librtlsdr or a dongle without
this defect gets checked rather than assumed.**
`tests/engine/test_rtlsdr_source.cpp` carries four `[.probe]` cases. They are
hidden from the default run because each measures a threshold rather than
answering yes or no, and a hidden Catch2 case runs when it is named:

```powershell
$exe = "build\dev\tests\engine\revenant_engine_tests.exe"
$env:REVENANT_PROBE_DELAY_MS = "250"      # then 520, 1000, 3000
& $exe "a dongle streaming for minutes can still be tuned"
& $exe "librtlsdr on its own retunes a streaming dongle"
& $exe "librtlsdr retunes from inside its own callback"
& $exe "librtlsdr retunes a dongle whose stream is paused"
```

`REVENANT_PROBE_DELAY_MS` is milliseconds of streaming before the one retune the
first two cases issue, defaulting to 2000. Walking it across 250, 520, 1000 and
3000 is what pins the boundary, and one retune per run is what keeps a result
from being attributable to a previous failed attempt. The first case also takes
`REVENANT_PROBE_BLOCK`, in samples, which sizes the URB: 8192, 32768 and 262144
are the three transfer lengths above. Each case reports its number through
`WARN`, so it prints on a pass, and all four skip themselves with a reason when
no dongle is attached.

### Whether the source is keeping up

`EngineInfo` carries `realtimeFactor`, `sourcePacedBy` and
`realtimeWindowSeconds`.

The first is capture seconds delivered per wall second, measured over the
last two seconds, which the third states. A synthetic source asked for 20 MS/s generates about 0.20 of
realtime on the host this was written on, so everything downstream starves
and audio arrives in fragments. Before this pair existed the only
client-visible symptom was an audio queue that kept running dry, which is
true about the queue and names the wrong component: the wire is not slow, the
samples were never produced. The number itself existed, in
`revenant-engine`'s own status line as `x 0.20`, in a terminal a GUI operator
never sees.

The second is the `--pace` the engine was started with, and the pair is the
point. A factor of 0.5 is a source that cannot keep up when the pace is zero,
and is exactly what was asked for when the pace is 0.5. A client that drew
one without the other would raise an alarm on every deliberate half-speed
replay.

Zero in the factor means **not measured**, which is a third state and not a
stalled source: it is what `info()` answers between opening the source and
starting the run. A source that has stopped producing reads lower from the
moment its next block is late, down to two blocks over the window, and never
as zero.

**A pause the engine made is not charged to the source.** An RTL-SDR stops
its transfers around every retune and gain change, about 330 ms each on an
R820T, and counts the samples the device produced meanwhile as lost so the
index stays on the device's clock. `Engine::set_source_center`,
`set_source_gain` and `set_source_gain_auto` add what the source lost inside
the call to the delivered count the factor is taken from, because those
samples were never late; `SourceStats::samplesLost` still counts every one.
A stall nobody asked for, a consumer too slow to drain the device for one,
pulls the factor down and leaves the window two seconds after it ends.
`core/engine/pacing_window.h` has the rule, and
`tests/engine/test_pacing_window.cpp` drives an RTL-SDR-shaped pause through
it both ways.

Measured on 2026-09-23 through the real engine on the RTX 4090, in
`tests/engine/test_engine_pacing.cpp`: a synthetic scene at 2 MS/s paced at
realtime, wrapped so that a retune throws its blocks away for 330 ms and
counts them lost the way the dongle does, polled every 50 ms. Steady, the
factor read 0.9986 to 1.0013 over three runs. Across five retunes 700 ms
apart, polls inside the pauses included, it read 0.9954 to 1.0125. The same
330 ms pause with no control call behind it pulled it to 0.838, and it was
back above 0.99 1.70 s after the pause ended.

WHAT THIS PARAGRAPH USED TO SAY, before 2026-09-23: "It is also a lifetime
mean rather than an instantaneous reading, for the reason `RdsHealth` carries
no `blockErrorRate`: a source that struggled for ten seconds and has been fine
since reads low forever." The first sentence of the section said "measured
over the whole run". Each retune took a third of a second out of that mean for
good, so after a few tunes the client called the RTL-SDR behind while its
audio was not: after N retunes in T seconds the factor was (T - 0.33 N) / T.

An engine built before the window sends `realtimeWindowSeconds` as zero, and
the client does not call a source behind on that engine's lifetime mean.

### A clamped passband says so in words, and an FM one is refused instead

`VrxPlacement::clampReason` is empty unless something was clamped, and is
otherwise the sentence to put in front of the operator. It names both
widths and the ratio, says what the widest receiver on that grid is, and
says the fix is the engine's channel count rather than anything the session
can change.

**WHAT THIS SECTION USED TO SAY**, before 2026-09-21: an operator clicking a
broadcast FM station on a 2.4 MS/s dongle with a 64-channel grid "asks for
200 kHz and is given about 71", and the sentence was the fix. Reporting the
clamp was not enough and could not have been. The operator hears the audio
several seconds before they read a status line, and on an FM mode a
truncated passband is not a quieter version of the station: a discriminator
recovers the instantaneous frequency of whatever reaches it, so what comes
out is the wrong audio at full strength while the waterfall shows a strong
clean carrier.

So `addVrx` and `setVrxParams` now **refuse** that placement rather than
narrowing it. The refusal is on the call that asked for it, carries the same
facts, and names the channel count that would have worked, because the grid
is sized in `Engine::open_source` and cannot be changed while the source is
running: it fixes the prototype filter, the twiddle table, the whole graph,
the ring's floor capacity and `EngineInfo::spectrum`, whose geometry every
spectrum subscriber read once.

The refusal is narrow on purpose, two conditions together. The mode has to
be one where a clamp changes what the demodulator produces rather than how
much of it, which is NFM and WFM and nothing else. And the grant has to
have fallen below the mode's own entry in `dsp::default_passband`: a WFM
receiver asking for 300 kHz and granted 250 still has the whole broadcast
channel. Everything else clamps and reports exactly as before, because an
envelope or product detector given a narrower filter is exactly a receiver
with a narrower filter, and refusing those would train an operator to
ignore the refusal on the mode where it matters.

`clampReason` is prose and is never parsed. `bandwidthClamped` and the
granted pair are the machine-readable half and are not going anywhere.

### Where a mode comes from

**WHAT THIS SECTION USED TO SAY**: "Nothing infers a mode from a bandwidth.
Occupied bandwidth does not determine modulation, and a click-to-tune
surface that guessed would be a different wrong answer." That was true about
modulation and wrong about what it implied, which was that the honest move
was to infer nothing. Inferring nothing is not neutral: `VrxParams::demod`
defaults to `nfm`, so a client that chose nothing chose NFM, and NFM with a
16 kHz filter on a 145 kHz broadcast station is the guess that cost the
evening.

`engine::demod_for_signal` is the rule now. It takes the occupied bandwidth
the detector measured and, when a `characterise::ModulationFamily` is known,
that too; `core/engine/vrx_place.cpp` carries the derivation and the list of
what the rule gets wrong, which is long and starts with AM. Nothing on the
wire carries a family, so a client working from `Detection` alone uses the
width. The engine does characterise detections now, on probe receivers of its
own (`core/engine/probe.h`), and the answer stays on the detector's track:
`docs/detection.md` settled that nothing goes on the wire as a family.

WHAT THE SECOND SENTENCE USED TO SAY: "Nothing on the wire carries a family
yet, because `core/characterise` reads complex baseband and is not wired into
the engine". `ui/models/receiver_match.h` is the Qt
client's copy of the width half, on the same terms as `kDemodNames`: the
client links no part of the engine.

### Nothing else crosses

No complex baseband, no GPU handles, no device memory. `core/engine/engine.h`
promises that samples cross the bus once, into the device ring, and that
nothing returns to host memory except audio PCM, decoded symbols and detection
metadata. The wire is downstream of that promise and does not widen it.

What the schema does carry is the state a client needs to draw and control:
`EngineInfo` with the device, grid, rates and spectrum geometry;
`SourceDescriptor` for a picker; `SourceStats` and `VrxStatus` for the
counters; the `DetectionList` above, which is the detection metadata the
engine's promise names and the reason that clause is in it; and `RdsStation`
and `DecodedMessage`, which are the "decoded symbols" clause of the same
promise now that something decodes. Nothing new crosses the bus for either:
the composite is audio PCM the engine already returns, a digital voice
receiver's baseband comes back through the same readback at its decoder's
rate, and the decode happens on the host.
Overruns and lost samples travel because they are correctness events, and a
remote client is exactly the caller that cannot read the log.
`VrxStatus::audioDropped` travels beside them and is narrower than it reads:
it is frames the engine handed to a receiver's sink and had refused, which
also ends the run, so it is normally zero. It used to be incremented by the
squelch mute, which is not a dropout at all. What a listener missed belongs to
a consumer rather than to a receiver, and `AudioStats::framesDropped` is where
a subscription's own losses are counted.

`EngineInfo` also carries `ringClamped` and `ringClampReason`. They are how a
remote client learns the engine did not build what it was asked for, and they
are not ring trivia. `core/engine/engine.cpp` overloads
`RingGeometry::clamp_reason` as the only field in `EngineInfo` that can hold a
sentence, so a clamped channel count or block size rides out in it alongside a
clamped ring, appended after the ring's own reason when there is one. That
file's comment names the failure it prevents: a caller who asked for 4096
channels, got 2048, and finds out when a frequency lands in the wrong channel.
Leaving the pair off the wire handed a remote client that failure exactly.

The schema, `core/rpc/types.h`, `core/rpc/convert.cpp` and
`core/rpc/client.cpp` all carry them, so a UI holding an `rpc::EngineInfo`
sees what the engine said.

**This paragraph used to end by saying `read_engine_info` did not read the
pair back, so `ring_clamped` arrived false whatever the engine had set.** It
was written in the same round that made it untrue, and the correction is
recorded rather than swapped out, because a field the schema carries and the
client silently drops is exactly the shape of gap a reader assumes is still
open. `read_engine_info` reads `getRingClamped()` and `getRingClampReason()`
into the struct. `tests/rpc/test_rpc_session.cpp` compares the reason against
the engine's own string byte for byte and checks that it names both the
transform that was asked for and the smaller one that was built, with a
control arm on an engine that clamps nothing so two empty strings cannot pass
for a match.

## Threading

Four threads on the server side, and none of them is the same thread.

The caller's thread constructs the `Server` and later stops it. The Cap'n Proto
event loop, which the `Server` owns, is the only thread that may touch a
capability. That is not a performance guideline: kj's promise machinery is not
thread safe, and calling a capability from elsewhere corrupts it. The engine's
completion thread is where the `SpectrumSink` fires, which makes it the one
thread that must never touch a capability at all.

**This section used to say there were three, and the correction is recorded
rather than quietly renumbered**, because for as long as there were three that
was the whole of the threading story, and a reader who learned it here would
have no reason to go looking for a fourth. The fourth is the listing worker.
`Session::listSources` calls `source::describe_sources()`, which
`core/source/registry.h` documents as costing more than enumeration because
some backends must briefly open a device to answer; for the RTL-SDR backend
that is `rtlsdr_open`, a libusb open, claim and reset. Run on the loop thread
it dispatched nothing for the whole of that, so one dongle's enumeration held
up every other client's calls, the spectrum fan-out, and the shutdown
fulfiller `stop()` waits on. `core/rpc/server.cpp` starts the worker lazily on
the first `listSources` and returns the result through the same cross-thread
fulfiller the sink uses. One worker and one queue, so listings stay serialised
the way the loop thread serialised them, and two clients asking at once do not
open the same dongle twice. `stop()` closes the queue and joins the worker
before it ends the loop: a listing's result arms the loop, so the thread that
can fire one has to be joined while the loop is still there to be armed.
`core/rpc/server.h` states the same inventory.

The bridge between the engine's completion thread and the loop thread is a
cross-thread promise fulfiller. The sink copies the frame into a single-frame
slot, fulfils a promise the loop thread is already waiting on, and returns.
The loop thread wakes, drains the slot and does the capability work. A mutex around the capability would not help, because
the problem is the promise system's ownership model rather than a data race on
one field.

**This section used to prescribe `kj::Executor::executeAsync`, and that was
wrong.** The correction is recorded rather than quietly swapped, because
`executeAsync` is the answer an experienced kj reader would expect to be right.
`kj::Executor::send` sets `event.replyExecutor = getCurrentThreadExecutor()` for
every asynchronous request, and `getCurrentThreadExecutor()` requires a kj event
loop on the calling thread. The engine's completion thread runs none, so the
call throws instead of delivering anything. Dropping the result is not a way
round it: kj's own header says the promise `executeAsync` hands back belongs to
the requesting thread, and that destroying it blocks until the executor thread
acknowledges cancellation, so there is no fire-and-forget form of the call even
from a thread that does have a loop.

`executeSync` does work from a thread with no loop, and is still wrong here for
a reason specific to this service. It blocks its caller until the loop thread
reaches the work, and the caller here is the thread retiring GPU readbacks.
The loop thread owes that thread nothing in return: it is serving every other
client on the socket, so whatever the slowest of them asked for is what the
completion thread would be parked behind.

This paragraph used to offer a device enumeration as the example of what the
loop might be busy with. That is the one call which has since been moved off
the loop thread, so the example now argues against itself, and
`core/rpc/server.h` has dropped it too. The objection never rested on it:
blocking the completion thread on the loop thread is wrong whatever the loop
is doing.

`kj::newPromiseAndCrossThreadFulfiller` is the primitive kj documents as safe to
fire from any thread, and it does not block the thread that fires it. The
executor is still captured at startup and held with `addRef`, because the
cross-thread promise keeps a bare reference to the executor of the thread that
created it and kj destroys an unreferenced `Executor` along with its loop.
`core/rpc/server.h` states the same contract and `core/rpc/server.cpp` opens by
proving it.

On the client side a `Client` owns a thread running its own event loop. Every
method is synchronous from the caller's point of view: it hands work to that
loop and waits. Calls are serialised, so two threads may call one `Client`
without tearing, though they will queue.

The frame callback is the exception, and the one thing a UI has to get right.
It is invoked **on the event loop thread**. It must not call back into the same
`Client`, and it must return quickly, because everything else that connection
is doing is waiting behind it. A UI copies the frame and posts a wake-up to
its own thread, not the frame. `ui/models/engine_link.h` does that with a
queued metacall carrying no payload, and deliberately not with a queued signal
carrying the frame: a frame is 256 KiB, so a queued signal would copy the bins
into Qt's event queue and grow it without bound the moment the GUI thread
stalled, which is the failure the wire already decided against one layer down.
Three buffers cycle instead, a latch keeps one wake-up in flight at a time,
and the newest frame replaces the one waiting.

**"A UI copies the frame and posts it to its own thread" is how that sentence
used to read**, and the correction is recorded because the old version is
almost right. What crosses is the wake-up; the frame stays in a buffer the
link owns. A consumer following the earlier wording literally rebuilds the
unbounded queue one layer above where the wire refused it.
`core/rpc/client.h` has been corrected the same way and points at
`ui/models/engine_link.h` as the worked example.

One subscription per `Client`. Subscribing again replaces the first. Two
waterfalls in one process is a reason to want two rates, not two connections,
and allowing two would put the drop policy somewhere it cannot be reasoned
about.

## Backpressure

Decided in `core/rpc/server.h` rather than discovered later.

A slow client must not grow a queue. At the shipped geometry a frame is 256 KiB
and the engine makes about 305 a second, so a client stalled for two seconds
would be roughly 150 MB behind if anything buffered on its behalf.

So: at most one frame in flight per subscription. A frame arriving while the
previous call has not resolved is dropped and counted, never queued. For a
waterfall that is the right answer regardless, since the newest frame is the
one worth drawing.

The counters that mean something are the server's. `Server::frames_sent` is
frames handed to a subscriber. `Server::frames_dropped` is frames a subscriber
was owed at the rate it asked for and did not get, from either of the two
places a frame can be thrown away: its previous call had not resolved, or the
loop thread had not drained the single-frame slot before the completion thread
filled it again. The second kind is charged once per live subscription, so
with several subscribers at different rates the total can exceed what any one
of them missed. Frames skipped by `everyNth` are in neither number, because
they are what the subscription asked for.

`Client::frames_received` is every frame this client was sent, including one
that arrives after an unsubscribe and has nowhere to go.

**`Client::frames_dropped` is structurally always zero, and this section used
to say it counted frames that arrived while the callback was still running.**
The correction is recorded rather than swapped out, because that is the
reading the name invites, and a UI that wires a status line to it ships
"0 dropped" however badly it stalls. The callback runs inline on the event
loop thread and `frame()` is not answered until it returns, so the engine
cannot start a second frame while the first is being handled, and the one
increment behind that counter is unreachable. `core/rpc/client.h` says so at
the declaration, and `tests/rpc/test_rpc_spectrum.cpp` pins the value at zero
against a subscriber slow enough to make the engine drop.

So a client cannot read its own losses off a counter. What it can read is
`SpectrumFrame::sequence`: it counts frames the **engine** produced, so the
distance between two arrivals says how many were made in between and not sent.
`ui/models/engine_link.h` derives three separate numbers from it.
`framesSkipped` is what `everyNth` asked the engine not to send, which grows
steadily on a healthy display and is not a loss. `framesDroppedByEngine` is
what the engine had at the rate asked for and threw away. `framesDroppedByUi`
is what that link replaced in its hand-off slot before the Qt thread came for
it. Three causes with three different fixes, which is why each is exposed on
its own and not only as the total `framesDropped` rolls up.

## Logging in

A connection holds an `Authenticator` and nothing else until it has passed the
engine's token. `Authenticator` has one method, `login(token :Data) ->
(session :Session)`, and no radio on it at all, so there is no `Session` in an
unauthenticated caller's capability table to refuse. That is the difference
between "a Session that says no" and "no Session", and it is why this is a
bootstrap interface rather than a check at the top of every method: a check has
to be remembered by whoever adds the next method, and this does not.

**What this section used to say.** Until 2026-09-20 the "Not done yet" list
below opened with "**There is no authentication of any kind.** Not a token, not
a password, not a TLS client certificate. Anything that can open the port gets
full control of the engine." That was true and is not. It is retracted here
rather than deleted because `core/rpc/server.h` and the schema said it too, and
because the sentence that followed it, about the loopback default, still holds
for a reason the old text did not give.

### Where the token lives

`%LOCALAPPDATA%\Revenant\rpc-token`, resolved with `SHGetKnownFolderPath` and
not `getenv`: the environment variable is absent under a service and lies under
impersonation. Local and not Roaming, because a token identifying one machine's
engine must not follow a domain profile to another machine.

`revenant-engine` mints it on first run and prints the path, never the token.
The file is 64 lowercase hex characters and a newline, and its ACL is built
before `CreateFileW` and passed in `SECURITY_ATTRIBUTES` with `CREATE_NEW`, so
it grants this account and `SYSTEM` and inherits nothing. Setting the ACL after
writing would leave the token on disk under an inherited ACL for the length of
the write, which passes any check that only reads the final ACL.

`CREATE_NEW` rather than `CREATE_ALWAYS` for two reasons that are not caution.
Two engines starting at once: the loser is told the file exists, loads the
winner's and serves the same token, rather than clobbering a token the winner
has already handed to a client. And `CREATE_NEW` will not follow a symlink
somebody pre-placed at that path.

**Loading refuses a file anything else can read**, naming the principal, and
the refusal states the two recoveries because the operator reading it is
looking at a headless process that would not start: delete the file so a fresh
one is minted, or point `--token-file` somewhere private. `Administrators` is
allowed, because an administrator can take ownership whatever the ACL says;
`Users`, `Everyone` and `Authenticated Users` are not.

### Reading it and passing it

    type %LOCALAPPDATA%\Revenant\rpc-token

There is no `--print-token`. `revenant-engine`'s stdout is scrollback, a
supervisor's log and CI output at once, and a secret written to any of those
has left the machine.

`revenant-engine --token-file <path>` moves the file. Name one when the engine
runs as a service: under LocalSystem, `FOLDERID_LocalAppData` resolves inside
`C:\Windows\System32\config\systemprofile` and an operator's client cannot read
it. `revenant-engine --new-token` mints a fresh one over the existing file.

`revenant-ui` looks in three places, in order: `REVENANT_RPC_TOKEN` holding the
hex, `REVENANT_RPC_TOKEN_FILE` naming a file, then the default path. The
ordinary case, one operator with the engine and the window running as the same
account, needs none of them.

In C++, `core/rpc/token.h` is both halves of the file format in one translation
unit, compiled into `revenant_rpc_client` so the Qt process gets it too.
`Client::connect(address, port, token)` takes exactly 32 bytes.
`ServerOptions::token` takes the same, and **empty is a `create()` failure
rather than "no authentication"**: an off switch is the thing that ends up on
by accident, so there is not one.

### What it does and does not change

It changes what a second process on this machine can do. It changes nothing at
all about a LAN. This wire is plaintext; TLS with client certificates was
refused as the wrong shape for one operator with one radio, and the consequence
is that a token crossing a routable interface is readable and replayable by
anything on the path. `bind_address` still defaults to `127.0.0.1`, off
loopback still means a tunnel, and `revenant-engine` prints a warning on stderr
when it binds anywhere else.

The comparison is constant time over the fixed length, after a length check.
Branching on the length leaks nothing, because the length is published in the
schema, in this file, and in the size of the file on disk. What the loop
protects is the **prefix**: a compare returning at the first wrong byte turns a
2^256 search into a 32 x 256 one. Whether that is measurable across loopback
under Windows scheduler jitter, from a separate process, against a
once-per-connection call, is doubtful, and it is not the argument. 256 bits of
entropy and the file ACL are what protect the token; the compare costs three
lines and means a future shorter token or a per-call check would be bad rather
than catastrophic.

**Rotating does not disconnect anybody.** A `Session` already granted is a
capability and capabilities do not re-check. An operator rotating because a
token leaked has to restart the engine to kill the sessions the leak already
bought.

**A second login on one connection succeeds**, returning another independent
`Session`. The caller has already proved it holds the token, so a second
capability grants it nothing it did not have, and refusing would need
per-connection state that capnp 1.4.0's `TwoPartyServer` does not offer: it
takes exactly one bootstrap capability and has no `BootstrapFactory`
constructor. A `SessionImpl` holds a reference to the server and a login
number, so minting one per login costs nothing and two of them cannot disagree
about the radio. What they can differ in is what they own: each takes the
receivers it created with it when it ends, which is "Receivers belong to the
session that made them" below.

This paragraph used to say "`SessionImpl` is stateless, its only member a
reference to the server". The login number arrived with the receiver lifetime
rule on 2026-09-22.

**A client pipelines.** `core/rpc/client.cpp` sends `login` and takes the
`Session` off the unresolved promise, so `Session` calls travel behind the
login rather than after it. When login fails, Cap'n Proto breaks that
capability with login's own exception and every call on it fails with that
exception without the engine's `Session` implementation being entered.
`tests/rpc/test_rpc_auth.cpp` proves the second half by sending `addVrx` behind
a bad token and then asking the engine whether a receiver appeared.

`connect()` now round-trips, which it did not before: the bootstrap capability
is lazy, so until this landed `connect` returned the moment the TCP connect did
and nothing was exchanged. It waits for the login answer, so a wrong token is a
connect failure carrying the engine's refusal.

**A wrong token is permanent and the failure says so.** `Error` carries an
`ErrorCategory` as of 2026-09-21. A token this engine will not take, a token of
the wrong length, and a token file that cannot be read or holds something that
is not hexadecimal are all `Unauthenticated`; nothing answering at the address
is `Unreachable`, and that includes the token file not existing yet, because the
engine mints it on its first run and a client started first finds no engine and
no token for one reason. `ui/models/engine_link.cpp` backs a refused credential
off to ten seconds and presents an absent engine as "waiting for an engine at
127.0.0.1:17690" rather than as a syscall.

The two are separated by how far the handshake got and not by what the message
said. `ClientImpl::run` records a phase as it walks the handshake, and the phase
decides: a throw before the stream connected is `Unreachable` whatever kj called
it, and a `FAILED` during login is the token, which is login's only documented
refusal. A `DISCONNECTED` during login is an engine that died mid-handshake and
stays `Disconnected`, because that is worth asking about again.

**This entry used to read "A wrong token is permanent and `core/error.h` cannot
say so. `Error` has a message and an originating API code and no category, so
`ui/models/engine_link.cpp`'s reconnect supervisor cannot tell a refused token
from a server that is not up yet except by matching on message text, and it
retries both. Widening `Error` with a category is the right fix and touches
every user of `Expected` in the tree."** It did not touch every user. The
category sits on `Error`, so it reaches every `Expected` in the tree by
construction, and it defaults to `Unclassified` so the roughly eleven hundred
`fail()` sites whose sentence is already the whole answer did not have to be
visited and guessed at.

**What a category survives is narrower than what it carries, and the narrowing
is the protocol's.** `capnp::rpc::Exception` is a reason, a four-value type and a
trace, with no detail blob, so nothing can be carried beside the type.
`server.cpp`'s `to_exception_type` maps `Disconnected`, `Overloaded` and
`Unimplemented` onto kj's three and everything else onto `FAILED`;
`client.cpp`'s `translate` maps them back, and `FAILED` becomes `Unclassified`
because that is what `rpc.capnp` says it means. `Unreachable` and
`Unauthenticated` never cross: a client generates both locally, at the point
where it failed to reach or failed to log in.

So a refusal a client has to **act** on is a result field in the schema and not
an exception. `rpc.capnp` argues this under its own `Exception` struct:
exceptions should not be used to flag conditions a client is expected to handle
in an application-specific way.

## Receivers belong to the session that made them

The owner's decision, recorded 2026-09-22: a receiver belongs to the session
that created it and is removed when that session ends, unless it was created
with `keep` on `addVrx`. A desktop operator's crashed window takes its
receivers with it; a headless recorder asks for `keep` and its receivers
survive a client restarting.

**A session ends when its `Session` capability is released**, which covers
every way a client can leave: dropping the capability, closing the connection,
the process dying with the socket open, and the server itself stopping.
capnp 1.4.0's `TwoPartyServer` has no per-connection hook, as the login section
above says, but it does release everything a connection exported when the
connection goes, so `SessionImpl`'s destructor is the one event that happens
exactly once for each of those. It calls `ServerImpl::end_session`, which
removes the session's own non-kept receivers through the same path `removeVrx`
takes: an audio subscriber on one is sent `ended()` with the reason, and its RDS
decoder goes with it.

**Ownership is creation and nothing else.** A session that retunes or listens
to another session's receiver does not come to own it, and the reap leaves it
alone. Nothing stops one session removing or retuning another's receiver
either: the rule decides what happens when a session ends, and a receiver is
engine-wide state in every other respect, for the reason the RDS region section
gives. A receiver the host process added directly, with `Engine::add_vrx`, has
no creator on this wire and is never reaped.

**`VrxStatus` says whose a receiver is.** `creatorSession` is the creating
login's number, counted from one and never reused for the life of the server;
zero means the host added it. `kept` is the flag. `ownedByCaller` compares the
creator against the session asking, which is what a client wants and could not
otherwise work out, since it holds no session number of its own. A kept
receiver goes on naming a session that has ended.

Measured with `tests/rpc/test_rpc_lifetime.cpp` on 2026-09-22 against the RTX
4090: the first `vrxIds` a surviving client sent after the other client's
destructor returned already listed neither of the departed session's two
receivers. The cases poll with a ten second deadline regardless, because how
soon the server notices a closed socket is a property of the loopback stack
and not of this code.

**A receiver the engine removes on its own goes the same way.** A front-end
retune removes one whose centre falls outside the new span, and
`setSourceCenter` drops its ownership record along with its subscriptions
before answering. `closeSource` clears every record, because every receiver
goes with the source.

WHAT THIS PARAGRAPH USED TO SAY, under "What this does not do": "A receiver the
ENGINE removed on its own, which a front-end retune does to one whose centre
falls outside the new span, leaves its ownership record behind until its
session ends, when the removal is attempted, refused and dropped." True until
2026-09-23, when the server started reading the engine's list of removals.

## Not done yet

**No session persistence.** Nothing here saves or restores a set of receivers.
A client reconnecting starts from whatever the engine currently holds, and an
engine restarting starts empty. Saved sessions are a client-side or a
schema-side feature and neither exists.

`revenant-ui`'s bookmarks are not that and do not close this entry.
A bookmark is a frequency, a mode and a passband under a name, recalled when an
operator picks it; it restores nothing on its own and `ui/models/settings.h`
still refuses to bring a receiver back at startup. What is missing here is a
SET of receivers coming back without being asked for, which is the thing a
headless recorder would want and which no client offers.

**WHAT THIS ENTRY USED TO SAY, and it was answered the same day it was
written.** It read "A retune drags every receiver with it, and nobody has
decided whether it should", and set out the alternative and the decision it
needed: whether "can no longer reach it" means outside the span or outside the
receiver's own passband.

Decided and shipped. A retune rebases every receiver to hold the absolute
frequency it was tuned to, and one whose CENTRE falls outside the new span is
removed. The centre and not the passband, because the centre is the frequency
somebody typed or clicked. `setSourceCenter` lists every receiver it removed,
and ends their subscriptions with a reason; see "The front end can be pointed
somewhere else".

**WHAT THIS ENTRY USED TO SAY, and the owner decided it on 2026-09-22.** It was
headed "A receiver outlives the client that created it, and nothing reaps one
whose client died", measured a `taskkill /F` leaving its receiver behind for
good, and went on: "Whether that is a leak or a feature is an open decision
rather than an oversight. A headless recorder wants receivers to survive a
client restarting, and a desktop operator wants a crashed window to take its
receiver with it." The decision is both, chosen per receiver, and it shipped:
see "Receivers belong to the session that made them" above.

The entry then said the only recovery was `revenant-ui` polling `vrxIds` and
offering to release what the window was not on, deliberately not as a reaper
"because nothing here says who created one". That was true of the wire at the
time and is not now: `VrxStatus` carries `creatorSession`, `kept` and
`ownedByCaller`. The window's action still works unchanged, and what it finds
has narrowed to kept receivers and other live sessions' receivers, because a
crashed window's receivers are gone before it could report them.

**Only starting a source is still the host's, and that is `run()` being a
blocking call rather than a gap in this wire.**

**This entry used to read "`Session` lists sources and reports whether the
engine is running, and has no method to open one, start it or stop it. An
engine is configured and started by whatever process hosts it."** Then, on
2026-09-20, it read **"What is still absent is opening, starting and stopping a
source. Those are the host process's business and an engine owns one source for
its life, for the reason `Engine::open_source` gives."** `setSourceCenter` and
`sourceCanRetune` retired the first of those, because the gap it described as a
deferred nicety turned out to be the biggest usability problem the program had:
the source centre was fixed at launch, so every change of band was a process
restart that took the operator's receivers, their waterfall history and their
audio with it.

`openSource` and `closeSource` retire the second. The reason it gave was real
rather than an excuse, and it was dealt with rather than waived: an engine owned
one source for its life because `Engine::open_source` sizes the grid and the
ring against the source's rate and nothing could take either down.
`Engine::close_source` can.

What is left is starting, and it is not a method this wire is missing.
`Engine::run` blocks for the length of a stream on a thread this service does
not own, so the host runs it. `tools/engined/main.cpp` loops on it: `run()`
returns when the stream ends, and `EngineInfo::sourceEpoch` is what tells that
loop whether a client changed radios underneath it or the source simply ran out.
`has_source()` alone cannot, because a close followed immediately by an open
looks like a source that is open and running. Since 2026-09-23 only the
command line's source running out ends the process: a recording a client opened
that plays to its end stays open, ended, and the engine waits for the client
to close it or open another, so the window that opened it can go back to the
radio without restarting the engine.

### What a source change costs, which is more than a retune

Closing a source is not a bigger retune. A retune keeps the stream and moves one
constant; this ends the stream. Every one of these is gone when `closeSource`
returns, and the client re-establishes them:

- **Every receiver**, with its audio, its passband and its RDS decoder. A
  receiver's centre is an offset from a baseband whose meaning was the closed
  source's, so carrying one forward would place it at a plausible offset from the
  wrong centre, and a receiver at the wrong absolute frequency looks exactly like
  a working one.
- **Every subscription.** An audio subscriber is told, through `ended()`, with
  the reason naming the close. A spectrum or passband subscriber finds out by the
  frames stopping, which is the shape those two already have and is why neither
  grew an `ended()` for this.
- **The detector**, and any threshold set on it. It is sized in bins against the
  old geometry; fed the next source's frames at a matching bin count and a
  different bin width it would report tracks at frequencies that do not exist.
- **The server's own sink state.** `sink_installed_` is sticky, so without this
  the next `subscribeSpectrum` would skip `ensure_sink` and hand back a
  subscription nothing feeds. `ServerImpl::release_source_state` is the whole
  teardown and it runs before the engine's, so every sink is detached while the
  graph it names still exists.

**And the sample indices start again.** `EngineInfo::sourceEpoch` is the only
thing separating the new stream's index zero from the old one's. Read it on every
poll, compare it for equality, and re-derive rather than adjust: there is no
offset between two streams, because the gap between them is however long an
operator spent choosing a radio. A client that ignores it lines up audio from one
radio against a spectrum frame from another and finds the arithmetic consistent,
because both are honest indices into streams nothing said were different.

`openSource` is refused when a source is already open, and that is deliberate
rather than a missing convenience. A replace that failed on the new URI would
have destroyed the working source already, and the client would hold a two-valued
answer to a three-valued question. Two calls means a failed open leaves an engine
with no source, which is a state the client asked for and can see.

Both calls run on the event loop and block it: one named device opened or closed,
which is what `setSourceCenter` already does there. `core/rpc/server.cpp` has the
measurement and what would change it. `listSources` is the one engine-facing call
that gets a worker thread, and the reason is its own shape rather than a general
rule: it opens every index, including ones with nothing behind them, and pays a
libusb timeout per absent dongle.

See "The front end can be pointed somewhere else" above for what a retune
does and does not move.

**`SourceDescriptor` carries what a picker configures with.**

This entry used to read: **"`SourceDescriptor` is four fields of
`source::SourceCapabilities`. The tune ranges, the gain stages and the sample
format are not on the wire. A picker
needs to list what exists before it needs to configure one, and adding fields
to a schema is the cheap direction."** The cheap direction was taken. Listing
what exists was enough while the source URI was a command-line argument;
`openSource` made the configuring half reachable, and a client that can open a
device but cannot be told what the device accepts has to guess or ask the
operator to type a URI.

On the wire now: the notes, the tune ranges, the discrete rates with the min and
max, the native format and its bit width, the gain stages with their discrete
steps and whether each has an auto mode, the flow control, and a recording's
seekability and length.

Three of those are worth reading the schema's notes on before using them.

**`gainStages` is a list and not a number.** An R820T has one tuner gain with 29
discrete steps, an Airspy has three separate stages, a file has none. Flattening
those to a percentage is how a client comes to offer a control the device does
not have. `stepsDb` empty means continuous and populated means the stage takes
nothing else, so a client that ignores it shows the operator numbers the device
never took.

**`sampleRates` empty means continuous between `minRate` and `maxRate`,** and the
RTL-SDR is neither shape: its rate is a 28.8 MHz clock over an integer, so it is
discrete and far too dense to list. The backend reports bounds, the device
rounds, and `configure()` reads back what it landed on.

**`flow` decides how `realtimeFactor` is read,** which is why it is here rather
than left to a client to infer from the backend name. On a Demand source
`sourcePacedBy` is the setting that matters and a zero means the source runs as
fast as the machine retires it; on a Paced source that setting is ignored and a
factor below 1.0 is the ring refusing samples. Reading the factor without knowing
which of the two this is names the wrong component, which is the failure the
field exists to prevent.

Left off deliberately, because nothing reads them: `preferredBlockSamples`, since
block size is an `EngineConfig` field fixed before this wire exists;
`clockSources`, since no backend's URI grammar takes one; `timestampAccuracyNs`,
since nothing displays it; and `ResolutionRequest`, which is real and belongs to
whichever change teaches the engine to size a transform from it.

An inverted range, high below low, is dropped on the way out rather than carried.
That is `Engine::source_tuning`'s filter for its reason: a backend that could not
describe its tuner leaves an inverted entry rather than guessing, librtlsdr has
no driver for a tuner it did not recognise, and carrying the pair would offer a
client a control that refuses everything.

**No shared GPU texture handle.** Frames cross by copy, at the cost measured
above. This is the deferred optimisation, not a gap in correctness.

**Two conditions the engine knows about and this wire does not carry, from a
list that used to be five.**

This entry used to read **"Five conditions the engine knows about and this wire
does not carry. Kept as a list rather than fixed one at a time, because the
shape is the point: each is something the engine detected, counted or
substituted, and no client can see it."** Three of the five are gone. Working
through them one at a time is exactly what the entry said not to do, and it was
right about two of them and wrong about the third in a way only a fix would have
found.

**Carried now.** `SourceStats::vrxRetuneRefusals` and `SourceStats::frameStalls`
cross on the message this wire already polls once a second.
`Engine::graph_conditions` is the accessor and `engine::GraphConditions` is the
struct, which is separate from `source::SourceStats` because that one is the
SOURCE's and neither of these is a loss the source could know about.

The refusal counter is the one that matters. `setVrxParams` answers success as
soon as the control op is queued, the graph applies it at the next block
boundary, and a stage that will not take it counts one and leaves the receiver
where it was, so what a client saw was a successful call and a receiver that did
not move. It is required to stay at zero, which is why it has never bitten, and
that is exactly the kind of requirement that stops being true quietly. Read a
non-zero value as a defect rather than as a condition to handle: `engine::place`
refuses a placement the demodulator cannot carry before the op is ever queued, so
a refusal here means placement and the stage disagree.

`frameStalls` has to be read beside `SourceDescriptor::flow`. It is expected on a
Demand source, where waiting IS the backpressure, and a warning on a Paced one,
where the next thing to move is `overrunEvents`. A client that draws it without
the flow control reports the healthy case as a fault on every file and every
synthetic scene.

**Was never true, and the fix is what found it.** The entry used to read
**"`Server::frames_dropped` is one server-wide counter charged for every spectrum
subscriber at once, and `core/rpc/server.cpp` admits it over-counts. A slow
client's drops appear on a fast client's status line. Audio has the
per-subscription counters this lacks, in `AudioStats`."** The second sentence
cannot happen. `Server::frames_dropped` never crosses this wire: it is a host
accessor, `revenant-engine`'s status line and one test are its only readers, and
no client has ever been able to see it. A client's own drops are exact and always
were, from `SpectrumFrame::sequence` jumping by more than the `everyNth` it asked
for, which is what `ui/models/engine_link.cpp` counts and publishes as
`framesDroppedByEngine`. What is true is narrower and is what the server's own
comment says: the host-side counter over-counts **only** when two subscriptions
ask for different rates, because the filter that ran was the gcd of both.

**Still absent, both with a reason rather than a plan.**

The recording's own counters. `AudioEgressStats` carries drops, trims,
discontinuities, silence inserted over a gap and a faulted backend, and none of it
crosses, because there is no recording surface on this wire at all. An engine
writing a WAV for a remote client reports nothing about that file. This one is
not a field to add: it needs a recording to exist on the wire first.

What an RTL-SDR landed on at OPEN. `configure()` in
`core/source/rtlsdr_source.cpp` reads the achieved rate, centre and gain back from
the device and substitutes them for what was asked, which is correct; what nothing
states is the DIFFERENCE. It is deliberately not a field, because the asker
already holds both halves: `openSource` is given a URI the client composed, and
`EngineInfo::sourceRate` and `sourceCenter` are what the device took. The picker
in `ui/` compares its own `SourceChoice` against them. A wire field would be the
engine re-deriving a request the client never forgot.

**The two processes are paired in CI now, and what is left is narrow.** This
entry's heading used to read "Nothing pairs the two processes automatically,
and CI never builds the client" until 2026-09-22, and before that it said
nothing served or drove the wire at all. Both headings are retired; the
paragraphs below say what replaced each.

What exists: `tools/engined` builds `revenant-engine`, which links
`revenant_rpc_server`, binds a port, prints it, and serves a real engine until
`--duration` expires. The port defaults to 17690, which is `revenant-ui`'s
default, so the two meet with neither being told a port; `--port 0` binds a
free one for a second engine on the machine. `ServerOptions::port` in the
library stays at zero, which is what the suite needs.

The bind is exclusive, and kj's own `listen()` is not. kj sets `SO_REUSEADDR`,
which on Windows lets a second socket bind a port another is listening on:
with the fixed default, a second engine printed `listening on
127.0.0.1:17690` beside the first and served. `core/rpc/listen.cpp` binds with
`SO_EXCLUSIVEADDRUSE` instead, so the second is refused with `WSAEADDRINUSE`,
and an engine restarted while its old connections sit in `TIME_WAIT` still
binds, measured on 2026-09-22. `tests/rpc` binds an ephemeral loopback port per case,
connects a real `Client` and drives a real engine through it. A channel centre
that is not a whole hertz is compared against what the engine holds rather than
against the other end of the wire, all eight demodulator modes round-trip, an
unknown ordinal is refused, `everyNth` decimates, a second subscription
replaces the first, and a client disappearing mid-stream takes its subscription
and nothing else. Backpressure has its own case: it delays the callback by the
25 ms it names, against a frame interval the same case puts at 6.8 ms, then
asserts that `Server::frames_dropped` rises, that the sequence numbers that did
arrive skip forward by more than one rather than draining a queue in order, and
that `Client::frames_dropped` stays zero because the callback runs inline inside
`frame()` and the engine cannot start a second frame while the first is being
handled. The two-runtime arrangement this document argues for has been built as
well: `ui/build/vs/RelWithDebInfo/revenant-ui.exe` imports five Qt DLLs plus
`MSVCP140.dll`, `VCRUNTIME140.dll` and the `api-ms-win-crt-*` forwarders,
checked with `dumpbin /dependents` on 2026-09-19, and `client.obj` in that
project's object tree is `core/rpc/client.cpp` compiled `/MD`.

The two processes run together as well. `tests/twoprocess` is a small CMake
project of its own that compiles `core/rpc/client.cpp` `/MD`, the way `ui/`
does, into a client that logs in, lists sources, takes spectrum frames and a
receiver's audio, and tears down; `scripts/two-process-smoke.ps1` starts the
`/MT` `revenant-engine` with `--port 0`, reads the port it prints and runs that
client against it. CI's `two-process` job runs it against the engine binary
that ships, and CI's `ui` job configures and builds `ui/` with warnings as
errors, so a change to `core/rpc/client.cpp` or to the schema that breaks the
`/MD` build stops the line. `package` and `release` wait for both.

**WHAT THIS PARAGRAPH USED TO SAY, until 2026-09-22:** "What has not happened:
nothing in the tree runs the client and the engine as two processes. Every RPC
case puts the engine, the server and the client in one address space ... CI does
not configure `ui/` at all: `.github/workflows/ci.yml` runs the `ci` and
`headless` presets from the root and both stop at the engine tree". Both halves
were answered by the change that added `tests/twoprocess` and the two jobs. The
in-process part of it is still true of `tests/rpc`: every case there shares one
heap between engine, server and client, which is why the two-process job exists
rather than a reason to doubt it.

What is still absent: nothing binds off loopback, which is still the right
default now that there is a token, because the wire is plaintext and a token
crossing a network is readable and replayable. Nothing exercises the
off-loopback warning `revenant-engine` prints either: it is a property of that
program's stderr rather than of this library, and would need another CTest
entry.
