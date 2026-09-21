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

`core/rpc/.gitkeep` planned a shared GPU texture handle for a local client, and
that is still the right answer for a frame at the source's own rate. It is a
deferred optimisation on a path that has to exist and be correct either way,
not a missing feature.

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

### The front end can be pointed somewhere else

`Session::setSourceCenter` retunes the source and answers with the centre the
device took, which a synthesiser with a tuning step will round.
`Session::sourceCanRetune` says whether it will work at all and over what
range, so a client can grey a control out rather than offering one that
always refuses.

This is a small engine change and the reason is structural rather than lucky.
The channelizer, every receiver and the whole spectrum stage work in the
source's baseband frame and are never told where the front end is pointed:
`core/engine/vrx_place.cpp` is handed the grid, the rate and the request and
nothing else. So a retune moves the device's own oscillator and
`EngineInfo::sourceCenter`, and nothing about a placement, a filter, an audio
stream or a subscription changes.

**Which is also the trap.** A receiver stays where it is in baseband, so it
is now hearing a different piece of spectrum. Deciding what each open
receiver was *for* and moving it is the client's job, because there is no
reading of "keep this one on 145.1 MHz" that is right for a scanner as well
as for a panadapter.

Every absolute frequency a client is holding is stale when the call returns.
Read `info()` again rather than adding the delta: the answer is where the
device landed, not what was asked for.

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

### Whether the source is keeping up

`EngineInfo` carries `realtimeFactor` and `sourcePacedBy`.

The first is capture seconds delivered per wall second, measured over the
whole run. A synthetic source asked for 20 MS/s generates about 0.20 of
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
starting the run. It is also a lifetime mean rather than an instantaneous
reading, for the reason `RdsHealth` carries no `blockErrorRate`: a source
that struggled for ten seconds and has been fine since reads low forever. A
client that wants the rate now differences two polls against
`SourceStats::samplesDelivered`.

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
wire carries a family yet, because `core/characterise` reads complex
baseband and is not wired into the engine, so a client working from
`Detection` alone uses the width. `ui/models/receiver_match.h` is the Qt
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
engine's promise names and the reason that clause is in it; and `RdsStation`,
which is the "decoded symbols" clause of the same promise now that something
decodes. Nothing new crosses the bus for it: the composite is audio PCM the
engine already returns, and the decode happens on the host.
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
constructor. `SessionImpl` is stateless, its only member a reference to the
server, so minting one per login costs nothing and two of them cannot disagree.

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

## Not done yet

**No session persistence.** Nothing here saves or restores a set of receivers.
A client reconnecting starts from whatever the engine currently holds, and an
engine restarting starts empty. Saved sessions are a client-side or a
schema-side feature and neither exists.

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
looks like a source that is open and running.

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

**Nothing pairs the two processes automatically, and CI never builds the
client.** This entry used to say that nothing served or drove the wire and that
none of the threading or backpressure behaviour had been exercised. Neither
claim is true any more, and the replacement is narrower rather than equally
sweeping.

What exists: `tools/engined` builds `revenant-engine`, which links
`revenant_rpc_server`, binds a port, prints it, and serves a real engine until
`--duration` expires. `tests/rpc` binds an ephemeral loopback port per case,
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

What has not happened: nothing in the tree runs the client and the engine as
two processes. Every RPC case puts the engine, the server and the client in one
address space, talking over a real socket but sharing one heap, so nothing here
has yet allocated on the static core's heap and freed on the UCRT's, which is
the failure the split exists to prevent. The only cross-process CTest entry
starts `revenant-engine` on its own and matches the port line it prints. CI does not configure `ui/` at
all: `.github/workflows/ci.yml` runs the `ci` and `headless` presets from the
root and both stop at the engine tree, so a change to `core/rpc/client.cpp` or
to the schema can break the `/MD` build and nothing will say so until somebody
runs `cmake --preset vs` in `ui/` by hand. Nothing binds off loopback, which is
still the right default now that there is a token, because the wire is
plaintext and a token crossing a network is readable and replayable. Nothing
exercises the off-loopback warning `revenant-engine` prints either: it is a
property of that program's stderr rather than of this library, and would need
another CTest entry.
