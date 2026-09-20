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

### RDS is still declared, allocated, refused

`Session::rdsStation` and `Session::setRdsRegion` remain unwired, with
`RdsStation` and the six structs and enums under it. The decoder exists in
`core/decode` and nothing in `core/engine` feeds it a composite, so both
methods refuse in words saying the surface exists and is not wired. A struct of
zeros would read as a broken engine rather than as unfinished work.

All three ordinals were allocated in one pass with the login bootstrap because
a Cap'n Proto field number is permanent and three branches appending to one
schema would collide. `core/rpc/revenant.capnp` carries the full argument for
each, including the four conditions an RDS receiver will have to meet, which
are four and not the one the audio rate suggests.

### Nothing else crosses

No complex baseband, no GPU handles, no device memory. `core/engine/engine.h`
promises that samples cross the bus once, into the device ring, and that
nothing returns to host memory except audio PCM, decoded symbols and detection
metadata. The wire is downstream of that promise and does not widen it.

What the schema does carry is the state a client needs to draw and control:
`EngineInfo` with the device, grid, rates and spectrum geometry;
`SourceDescriptor` for a picker; `SourceStats` and `VrxStatus` for the
counters; and the `DetectionList` above, which is the detection metadata the
engine's promise names and the reason that clause is in it.
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

**A wrong token is permanent and `core/error.h` cannot say so.** `Error` has a
message and an originating API code and no category, so
`ui/models/engine_link.cpp`'s reconnect supervisor cannot tell a refused token
from a server that is not up yet except by matching on message text, and it
retries both. Widening `Error` with a category is the right fix and touches
every user of `Expected` in the tree.

## Not done yet

**No session persistence.** Nothing here saves or restores a set of receivers.
A client reconnecting starts from whatever the engine currently holds, and an
engine restarting starts empty. Saved sessions are a client-side or a
schema-side feature and neither exists.

**No source control over the wire.** `Session` lists sources and reports
whether the engine is running, and has no method to open one, start it or stop
it. An engine is configured and started by whatever process hosts it.

**`SourceDescriptor` is four fields of `source::SourceCapabilities`.** The tune
ranges, the gain stages and the sample format are not on the wire. A picker
needs to list what exists before it needs to configure one, and adding fields
to a schema is the cheap direction.

**No shared GPU texture handle.** Frames cross by copy, at the cost measured
above. This is the deferred optimisation, not a gap in correctness.

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
