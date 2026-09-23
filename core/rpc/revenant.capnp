@0x9fd3fcc5514e1c3d;

# The service the engine exposes, and the only way a GUI reaches it.
#
# WHY THERE IS A WIRE HERE AT ALL
#
# core/engine/engine.h says the engine is headless and the GUI is one client
# among several. That was an architectural preference until 2026-09-19, when
# it became a build constraint as well: revenant_core is compiled /MT and
# links no CRT DLL, which is what lets revenant-cli ship as one signed file,
# and the Qt binaries this project uses are built against the dynamic CRT.
# A std::string crossing that boundary in-process is allocated on one heap
# and freed on another. The process edge is where that stops being a problem,
# so the UI is a separate process and this is how it talks.
#
# WHAT IS AND IS NOT ON THE WIRE
#
# Every frequency the engine holds as a rational stays a rational here.
# docs/conventions.md is emphatic that k*rate/M is usually not a whole number
# of hertz and that rounding it anywhere puts a tuning offset into the
# display that nobody can source afterwards. A schema that carried those as
# integers would be that rounding, once per field, for the convenience of
# whoever wrote the schema.
#
# Said carefully, because the first version of this paragraph said "every
# frequency" and that was not true of its own schema. channelSpacing and
# channelRate below are integers, and they are integers because the engine
# already truncated them: dsp::Hertz and dsp::SampleRate are integral types.
# The wire is not losing anything there, but it is not the guarantor of
# exactness for those two either, and a reader entitled to assume otherwise
# would be wrong. The fields that are exact say so by being a Rational.
#
# Spectrum frames cross by copy. A shared GPU texture handle for a local
# client is still the right answer for a frame at the source's own rate. It is
# not required first: a display asks for time decimation and takes 30 frames a
# second, which is 7.5 MB/s at the shipped geometry against the 80 MB/s the
# engine produces. The handle is an optimisation on a path that has to exist
# and be correct either way.
#
# This paragraph used to say "core/rpc/.gitkeep planned a shared GPU texture
# handle", naming a placeholder that has since been deleted. docs/rpc.md
# quotes what it said.
#
# Audio crosses as raw float32 PCM, per receiver and opt in, and the codec
# question has been answered rather than deferred: there is no codec. About
# 1.5 Mbit/s for a mono 48 kHz receiver is free on loopback and on a LAN, and
# that is the deployment that exists. Opus was refused on two counts, 20 ms or
# more of added latency and a lossy stage in the middle of a chain whose whole
# claim is that it is bit exact end to end. 16-bit PCM was refused because it
# adds a quantisation and a dither decision and clips the headroom a settling
# AGC uses. AudioChunk and Session::subscribeAudio below are that decision.
#
# WHAT THIS PARAGRAPH USED TO SAY
#
# Until 2026-09-20 it read "Audio does not cross at all yet. The CLI renders
# its own through WASAPI in the same process as the engine, and a remote
# client wanting audio needs a codec decision this does not have to make
# today." docs/rpc.md and README.md said the same thing in their own words, so
# a reader who checked any of the three and concluded a remote client could
# not listen is owed the retraction rather than a schema that reads as though
# it never said it.
#
# WHAT THIS SECTION USED TO SAY, WHICH WAS TRUE FOR ONE DAY
#
# It was headed "WHAT IS ON THE WIRE HERE AND NOT YET BEHIND IT" and read:
# "rdsStation and setRdsRegion are declared, ordinal-allocated and refused.
# No RDS decoder is wired into the engine; that is its own lane. The refusal
# names the surface and says it is not wired, because a method that answered
# with a zeroed struct would read as a broken engine rather than as
# unfinished work." It then recorded that subscribeAudio had been the third
# of the three and was served.
#
# All three are served now, as of 2026-09-20. The decoder is not in the
# engine and was never going to be: core/rpc/server.cpp builds one per
# receiver on the first call and joins it to that receiver's audio fan-out,
# and the long note at the top of that file has the argument against putting
# it in core/engine, which core/engine/vrx.h had already made from the other
# side. tests/rpc/test_rpc_rds.cpp runs a synthetic station through the GPU
# chain and reads its PI, PS and RadioText back over a socket.
#
# The reason all three ordinals were allocated in one pass is that a Cap'n
# Proto field number is permanent and three lanes racing to append would
# collide. That worked: nothing here moved when each lane landed.

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("revenant::rpc::schema");

# An exact frequency, in hertz. See the note above: this is a rational and
# not a rounded integer because the grid's own arithmetic is rational.
struct Rational {
    numerator @0 :Int64;
    denominator @1 :Int64 = 1;
}

enum Demod {
    # Ordinal for ordinal with revenant::engine::Demod, all eleven of them:
    # the eight demodulators Raw to Cw and the three digital voice taps
    # appended below. Matching it makes the conversion a cast, and
    # core/rpc/convert.h static_asserts every pair rather than trusting
    # that: a mode reordered on one side and not the other would silently
    # retune every receiver in a saved session, and nothing about the
    # failure would point here.
    #
    # WHAT THE FIRST SENTENCE USED TO SAY: "which is Raw, Am, Nfm, Wfm, Usb,
    # Lsb, Dsb, Cw. Matching it makes the conversion a cast", which listed
    # eight and stopped before p25p1, dstar and tetra.
    raw @0;
    am @1;
    nfm @2;
    wfm @3;
    usb @4;
    lsb @5;
    dsb @6;
    cw @7;

    # Appended, never renumbered. The three digital voice physical layers.
    # They are complex taps like raw: the engine hands out baseband and
    # core/decode recovers the symbols, so a client selecting one of these
    # gets two channels at the channel rate rather than mono audio.
    #
    # DMR is absent on purpose. ETSI TS 102 361 carries a live patent whose
    # claim receives a burst and compares its synchronisation pattern; see
    # docs/modes.md. If it is ever added it goes after tetra, like these.
    p25p1 @8;
    dstar @9;
    tetra @10;
}

struct DeviceInfo {
    index @0 :UInt32;
    name @1 :Text;
    vendor @2 :Text;
    discrete @3 :Bool;
    apiVersion @4 :UInt32;
    driverVersion @5 :UInt32;
}

struct GridParams {
    channels @0 :UInt32;
    tapsPerBranch @1 :UInt32;
    decimation @2 :UInt32;
}

struct SpectrumGeometry {
    transform @0 :UInt32;
    binsPerChannel @1 :UInt32;
    channels @2 :UInt32;
    bins @3 :UInt32;
    binWidth @4 :Rational;
    binZero @5 :Rational;
}

# The frequency axis of one receiver's passband frame.
#
# Per frame rather than in EngineInfo, unlike SpectrumGeometry above, because
# only the transform size is engine-wide: the width of the axis is half the
# receiver's own display rate, which steps when the passband's reach crosses
# a rung of the channel rate's ladder.
#
# WHAT THIS USED TO SAY: that the width "is the receiver's own demodulation
# rate and moves whenever its filter does", while the frame was the fine
# stream after the filter.
struct PassbandGeometry {
    # Points in the transform, and the bins kept of it, which are its central
    # half: bins is transform / 2.
    transform @0 :UInt32;
    bins @1 :UInt32;

    # The display stream's rate, which is TWICE the width of the frame: the
    # frame is its central half. The channel rate divided by an integer rung,
    # the largest that keeps the pane at least four passband reaches wide, so
    # it moves by a factor of two or more and only when the reach crosses a
    # rung. core/dsp/vrx_reference.h, "The display tap", has the rule.
    #
    # WHAT THIS USED TO SAY: "The fine stream's rate, which is the whole width
    # of the frame", wider than the passband, "and the margin is where the
    # filter's skirts are drawn". The skirts were the fault: the pane showed
    # the filter instead of the air around it.
    rate @2 :UInt32;

    binWidth @3 :Rational;

    # Centre frequency of bin zero, a quarter of the display rate below
    # whatever the fine stage mixed to DC. (This used to say half the
    # demodulation rate below it, while the frame was the whole fine stream.)
    #
    # NOT always the receiver's centre, and carried rather than derived for
    # exactly that reason. For CW the fine stage translates the carrier to the
    # operator's pitch, so this sits a pitch below VrxParams::center; a
    # display that computed the axis from params.center would be one pitch out
    # on one mode and right on the other seven. It is also what makes an
    # asymmetric passband need no special case on the drawing side: the axis
    # is read off what was mixed, and the filter's edges are drawn against it.
    binZero @4 :Rational;
}

# The air around one receiver, transformed.
#
# The stream is the receiver's display tap: the same coarse channel and the
# same exact mix as its fine stage, then a fixed anti-alias decimator, and
# none of the receiver's own filter. So the noise floor is flat across the
# frame and a signal outside the passband reads at its true level, which is
# what a filter is dragged against: a client draws the filter over this, and
# the filter is not in it.
#
# WHAT THIS USED TO SAY: "The response shaping it is the receiver's own fine
# filter and nothing divides that out. The skirts ARE the feature". Those
# skirts were a hump in the noise floor that moved with every dragged edge
# and hid the neighbours an operator needed to see.
struct PassbandFrame {
    vrx @0 :UInt64;

    # Decibels relative to full scale, ascending in frequency across half the
    # display rate, no gaps and nothing counted twice. (This used to say
    # across the whole demodulation rate.)
    powerDb @1 :List(Float32);

    geometry @2 :PassbandGeometry;

    # Source samples this frame's window covers, [start, start + count).
    # Absolute from the start of the stream, with both the channelizer
    # prototype's group delay and the display filter's already taken off, so
    # it lines up with an AudioChunk's start and with a SpectrumFrame's. (This
    # used to say the fine filter's group delay, which is not in this stream.)
    start @3 :UInt64;
    count @4 :UInt64;

    # Frames the ENGINE delivered to this receiver before this one, so a
    # waterfall that skipped a row knows it skipped a row.
    sequence @5 :UInt64;

    # This receiver's own colour map, scaled on its own passband. A display
    # scaled by something it is not showing is a display that lies, and a
    # passband holding one signal has nothing in common with a 20 MHz span
    # whose percentiles are mostly noise.
    floorDb @6 :Float32;
    ceilingDb @7 :Float32;

    # What the device measured of THIS frame, before any smoothing.
    percentileLowDb @8 :Float32;
    percentileHighDb @9 :Float32;
}

struct EngineInfo {
    device @0 :DeviceInfo;
    grid @1 :GridParams;
    sourceRate @2 :UInt32;
    channelRate @3 :UInt32;
    channelSpacing @4 :Int64;

    # bins == 0 means the engine built no spectrum stage, which is the
    # default and is what a headless recording runs.
    spectrum @5 :SpectrumGeometry;

    # Baseband DC in real radio frequency. Every centre below is an offset
    # from this, because that is the only frame the grid has.
    #
    # IT MOVES, and it is the only field in EngineInfo that does while a
    # connection stays up. setSourceCenter tunes the front end and this
    # follows what the device landed on, so a client that cached it at
    # connect and kept adding it draws every axis label, every receiver
    # frequency and every detection a retune's worth of hertz out, on a
    # display where nothing else looks wrong. Read it back after a tune.
    #
    # This said none of that until 2026-09-21. core/engine/engine.h and
    # core/rpc/types.h were both corrected when the tune call landed in
    # 4463967 and this file was not, which is a claim going missing rather
    # than a false one coming back: there was no wrong sentence here to find,
    # only silence that read as "fixed at open" to everyone who checked.
    sourceCenter @6 :Int64;

    ringSamples @7 :UInt64;
    ringSeconds @8 :Float64;

    # True when the engine did not build what was asked for, and why.
    #
    # These are not ring trivia. core/engine/engine.cpp deliberately overloads
    # RingGeometry::clamp_reason as the one field in EngineInfo that can carry
    # a sentence, so a clamped channel count or block size rides out in it
    # too. Its comment names the failure this prevents: a caller who asked for
    # 4096 channels, got 2048, and finds out when a frequency lands in the
    # wrong channel. Leaving them off the wire handed a remote client exactly
    # that, which is why they are here.
    ringClamped @9 :Bool;
    ringClampReason @10 :Text;

    # Capture seconds delivered per wall second, measured over the last
    # realtimeWindowSeconds.
    #
    # THE DIAGNOSIS NOBODY COULD MAKE. A synthetic source asked for 20 MS/s
    # generates about 0.20 of realtime on the host this was written on, so
    # every stage downstream starves and audio arrives in fragments. The only
    # client-visible symptom was an audio queue that kept running dry, which
    # is true about the queue and names the wrong component: the wire is not
    # slow, the samples were never produced. The number existed, in the
    # engine's own status line as "x 0.20", in a terminal a GUI operator
    # never sees.
    #
    # 1.0 is realtime. Above 1.0 is a recording being replayed faster than it
    # was made, which is the ordinary offline case and not a fault. Below 1.0
    # is the source falling behind, and sourcePacedBy below is what says
    # whether that was asked for.
    #
    # ZERO MEANS NOT MEASURED, which is a third state and not a stalled
    # source. It is what info() answers between opening the source and
    # starting the run. A source that has stopped producing reads lower from
    # the moment its next block is late, down to two blocks over the window,
    # and never as zero.
    #
    # THE SOURCE'S RATE NOW, NOT SINCE THE RUN BEGAN, and a pause the engine
    # made is not charged to it. An RTL-SDR stops its transfers for about
    # 330 ms around every retune and gain change and counts what the device
    # produced meanwhile as lost. Those samples count as delivered here,
    # because they were never late; sourceStats' samplesLost still carries
    # them. core/engine/pacing_window.h has the rule and the window's length.
    #
    # WHAT THIS COMMENT USED TO SAY, before 2026-09-23: "A LIFETIME MEAN AND
    # NOT AN INSTANTANEOUS READING, for the reason RdsHealth gives for
    # carrying no blockErrorRate: it is computed from the whole run, so a
    # source that struggled for the first ten seconds and has been fine since
    # reads low forever." Each retune took a third of a second out of that
    # mean for good, so a client read the source as behind after a few tunes
    # while the audio was not. And "A source that has genuinely stopped
    # producing reports a factor decaying towards zero without arriving,
    # because the elapsed time keeps growing while the sample count does not."
    realtimeFactor @11 :Float64;

    # The --pace setting the engine was started with, as a multiple of
    # realtime. Zero is unthrottled.
    #
    # CARRIED BESIDE THE MEASUREMENT BECAUSE THE MEASUREMENT ALONE CANNOT
    # TELL A FAULT FROM A SETTING. A realtimeFactor of 0.5 is a source that
    # cannot keep up when this is zero, and is exactly what was asked for
    # when this is 0.5. A client that raised an alarm on the factor alone
    # would raise it on every deliberate half-speed replay.
    #
    # Zero on a live radio too, and it means nothing there: a Paced source
    # runs on its own hardware clock and ignores the setting entirely. On one
    # of those a factor below 1.0 is the ring refusing samples, which
    # SourceStats::overrunEvents counts.
    sourcePacedBy @12 :Float64;

    # WHICH STREAM THE SAMPLE INDICES ON THIS WIRE BELONG TO. Zero before any
    # source has been opened, one for the first, and one higher for every
    # openSource after it.
    #
    # THIS IS THE SECOND FIELD IN EngineInfo THAT MOVES WHILE A CONNECTION
    # STAYS UP, and sourceCenter above is the first. The difference between
    # them is the difference between a retune and a source change, and it is
    # the whole reason this field exists.
    #
    # A retune leaves the stream alone: samples keep arriving, the index keeps
    # counting, and what changed is the constant relating baseband to real
    # radio frequency. Closing a source and opening another starts a NEW
    # stream, numbered from zero again, because time in this engine is an
    # exact sample index from the start of the stream and this is a different
    # stream.
    #
    # So AudioChunk::startSample, SpectrumFrame::startSample,
    # PassbandFrame::startSample and every counter derived from them are
    # numbered against whichever stream produced them, and two streams' zeros
    # are different instants. Without this field a client correlating by index
    # across an openSource lines up audio from one radio against a spectrum
    # frame from another and finds the arithmetic consistent, because it is:
    # both are honest indices into streams nothing said were different.
    #
    # COMPARE IT FOR EQUALITY AND RE-DERIVE WHEN IT DIFFERS. There is no offset
    # between two streams to adjust by: the gap between them is however long an
    # operator spent choosing a radio.
    #
    # An engine between sources keeps the epoch of the last one it served,
    # which is the truthful answer to "which stream were those indices in":
    # that one, and it has ended. Monotonic and never reused.
    sourceEpoch @13 :UInt64;

    # The wall seconds realtimeFactor was measured over: two, plus up to a
    # tenth and one block's time, and the whole run while the run is younger
    # than that. Zero from
    # an engine that measured over the whole run instead, which is every
    # engine built before this field, and a client should not call a source
    # behind now on a lifetime mean.
    realtimeWindowSeconds @14 :Float64;
}

# What a source can be pointed at. An ENVELOPE and not a promise, on exactly
# the terms Session::sourceCanRetune states: a device with a gap in its coverage
# reports the ranges it knows about and still refuses a frequency inside a gap,
# in its own words.
struct TuneRange {
    lowHz @0 :Int64;
    highHz @1 :Int64;

    # Zero means continuous within whatever the synthesiser can resolve. A
    # non-zero step is what setSourceCenter rounds to, which is why it answers
    # with the centre the device took rather than the one asked for.
    stepHz @2 :Int64;
}

# Why Session::setSourceCenter removed a receiver. engine::RetuneCause, ordinal
# for ordinal after unknown.
#
# unknown is ordinal zero ON PURPOSE: it is what a server built before this
# field existed sends, because an unset enum reads as zero, and "the server did
# not say" must not read as any one of the causes. A client offers to put a
# receiver back only on shapeChanged, so an old server's removals never get an
# offer that would be refused.
enum RetuneCause {
    unknown @0;

    # Its centre fell strictly outside the new span. Adding it again at the same
    # frequency is refused for the same reason.
    outsideSpan @1;

    # Inside the span, and the channel planner refused it on the new grid.
    unplaceable @2;

    # Placed, and the graph refused the new place because it needs a different
    # filter shape, which it builds only for a new receiver. An add at the same
    # frequency is exactly that new receiver.
    shapeChanged @3;
}

# One receiver Session::setSourceCenter removed: its centre fell outside the new
# span, or its new place in a channel needed a filter the engine will not swap
# into a running receiver. The server also ends the receiver's subscriptions
# with the same sentence as `reason`.
struct RetuneRemoval {
    vrx @0 :UInt64;

    # The absolute frequency its centre was on, before the tune. The id means
    # nothing to an operator; this is the number a client names when it says a
    # receiver went.
    frequencyHz @1 :Int64;

    # Which cause, for a client to act on, and the engine's sentence saying so,
    # for it to show. Both, because they answer different readers: code
    # branching on the sentence's wording breaks silently when the wording is
    # improved, and a cause with no sentence leaves the client composing one
    # that cannot quote the planner's or the graph's own refusal.
    cause @2 :RetuneCause;
    reason @3 :Text;
}

# One gain control on the device, named the way the device names it.
#
# A LIST AND NOT A NUMBER, WHICH IS THE WHOLE POINT. Users judge an SDR
# application in the first five minutes on whether their radio works properly,
# and the way that goes wrong is an abstraction that flattens every device into
# one gain slider. An R820T has a tuner gain with 29 discrete steps; an Airspy
# has three separate stages; a file has none. Reducing those to a percentage is
# how a client comes to offer a control the device does not have.
struct GainStage {
    # The device's own name for it, lowercase: "lna", "mixer", "vga", "if",
    # "tuner".
    name @0 :Text;

    minDb @1 :Float64;
    maxDb @2 :Float64;

    # Empty means continuous between the two above. Populated means the stage
    # only takes these values and a request lands on the nearest one, so a
    # client offering a continuous slider over a stepped stage shows the
    # operator a number the device never took.
    stepsDb @3 :List(Float64);

    # Whether the device will set this stage itself. Offered rather than
    # assumed: the RTL-SDR's own AGC maximises the level at its output and is
    # therefore set by the loudest thing anywhere in the span, which put three
    # intermodulation products in the detector's track list at confidence 1.00
    # when it was the default. README.md has that measurement. A client that
    # offers this offers it as a choice, not as the sensible setting.
    hasAuto @4 :Bool;
}

enum FlowControl {
    # The device's own clock sets the pace and the sink must never block,
    # because there is nowhere to put samples that keep arriving. A sink that
    # cannot keep up causes an overrun, which SourceStats::overrunEvents
    # counts as the correctness event it is.
    paced @0;

    # The consumer's clock sets the pace, and blocking IS the backpressure:
    # the source advances at exactly the rate its consumer retires work. This
    # is the whole mechanism behind faster than realtime and it is not a mode.
    #
    # WHAT A CLIENT DOES WITH IT. On a Demand source EngineInfo::sourcePacedBy
    # is the setting that matters and a zero there means the source runs as
    # fast as the machine retires it, which a listener hears as fragments. On a
    # Paced source that setting is ignored entirely and a realtimeFactor below
    # 1.0 is the ring refusing samples instead. Reading realtimeFactor without
    # knowing which of the two this is names the wrong component.
    demand @1;
}

enum SampleFormat {
    # Matching revenant::source::SampleFormat, ordinal for ordinal. What the
    # device puts on the bus; conversion to the engine's Complex32 happens on
    # the GPU during upload, so native width crosses the bus exactly once.
    cu8 @0;
    cs8 @1;
    cs16 @2;
    cf32 @3;
    cs24 @4;
}

struct SourceDescriptor {
    # WHAT THIS STRUCT USED TO SAY ABOUT ITSELF: "Mirrors the first four fields
    # of revenant::source::SourceCapabilities. The rest of that struct, the tune
    # ranges and gain stages and the format, is not here yet: a picker needs to
    # list what exists before it needs to configure one, and adding fields to a
    # schema is the cheap direction."
    #
    # The cheap direction was taken. Listing what exists was enough while the
    # source URI was an engine command-line argument; openSource made the
    # configuring half reachable, and a client that can open a device but cannot
    # be told what the device accepts has to guess, or ask the operator to type
    # a URI.
    #
    # EVERY FIELD BELOW HAS A READER IN THE PICKER, and the ones with no reader
    # were left off rather than mirrored for symmetry. Not here:
    # preferredBlockSamples, because block size is an EngineConfig field fixed
    # before this wire exists; clockSources, because no backend's URI grammar
    # takes one; timestampAccuracyNs, because nothing displays it; and the
    # resolution request, which is real and belongs to whichever change teaches
    # the engine to size a transform from it.
    uri @0 :Text;
    backend @1 :Text;
    displayName @2 :Text;

    # Empty when the backend is usable. Non-empty is the reason it is not,
    # and the caller shows it rather than hiding the backend: a missing DLL
    # and an unplugged radio are different problems and a list that omits
    # both looks identical.
    unavailable @3 :Text;

    # Conditions worth an operator's attention that did not stop the source
    # opening: a WAV whose auxi chunk carries no centre frequency, a SigMF
    # sidecar with an empty captures array, a recording playing across capture
    # segments that happen to agree.
    #
    # Each of those is the answer to "why is this at the wrong frequency" an
    # hour later, and a source reporting only its failures would say nothing
    # about any of them. Show them; do not fold them into unavailable, which is
    # about a source that cannot be used at all.
    notes @4 :List(Text);

    # Empty for a source that cannot be tuned, which is every file and every
    # synthetic scene, and also for a dongle whose tuner librtlsdr did not
    # recognise: there is no driver for such a tuner, so nothing on it can be
    # tuned at all. A client greys the frequency control out rather than making
    # the operator discover the refusal by trying.
    tuneRanges @5 :List(TuneRange);

    # Discrete rates the device supports. EMPTY MEANS CONTINUOUS between
    # minRate and maxRate, and a client offering a free-text rate box over a
    # device with a populated list here offers rates the device will round.
    #
    # The RTL-SDR's real constraint is neither of those shapes: its sample rate
    # comes from a 28.8 MHz clock divided by an integer, so it is discrete but
    # far too dense to list. The backend reports min and max and leaves this
    # empty, the device rounds, and configure() reads back what it landed on.
    sampleRates @6 :List(UInt32);
    minRate @7 :UInt32;
    maxRate @8 :UInt32;

    nativeFormat @9 :SampleFormat;

    # Bits per I or Q component, which is not eight times the byte width for
    # every device: an Airspy in packed mode puts 12 bits in 16, and a client
    # showing "16-bit" about one of those overstates what the ADC gave it.
    bitsPerComponent @10 :UInt8;

    # Empty for a file and for a synthetic scene. See GainStage: a list rather
    # than a number, because a device's stages are its own.
    gainStages @11 :List(GainStage);

    # Who sets the pace. See FlowControl: it decides how a client reads
    # EngineInfo::realtimeFactor, and reading that without this names the wrong
    # component.
    flow @12 :FlowControl;

    # Whether a client could ask this source to replay from a chosen sample.
    # True for files and false for every live device. NOTHING SEEKS YET: there
    # is no seek on this wire and this field is here for a picker to show a
    # recording as a recording, which is what decides whether it offers a
    # position control at all.
    seekable @13 :Bool;

    # ZERO MEANS UNBOUNDED, which is every live device, and it is not a
    # recording of no length. A file reports the samples it holds, which is how
    # a picker turns a path into "4 minutes 12 seconds at 2.4 MS/s" instead of
    # a filename.
    lengthSamples @14 :UInt64;
}

enum FrontEndState {
    # Matching revenant::detect::FrontEndVerdict, ordinal for ordinal.
    #
    # WHAT THESE ARE NAMED AFTER, WHICH IS THE POINT OF THEM. They say what
    # was OBSERVED about the full-span spectrum, not what is wrong with the
    # radio. core/detect/front_end.h is the measurement and its long note on
    # what it cannot tell apart is what stops a client turning
    # floorFollowsSignal into the word "overload": a broadband interferer
    # coming on at the same time as a strong signal rises reads identically,
    # and compression that was present from the first decision and never
    # lifted reads as steady.

    # No detector running, not enough decisions yet, or the strongest signal
    # on the span has not moved enough to measure against. A third state and
    # not a clean bill of health, the same way EngineInfo::realtimeFactor of
    # zero is not a stalled source.
    unmeasured @0;

    # The floor is not following the strongest signal on the span.
    steady @1;

    # Every part of the span's floor is following it at about one decibel per
    # decibel, which is what a gain change looks like. With gain=auto that is
    # the tuner's own AGC, and it is worth saying because it explains a
    # display that breathes.
    spanScales @2;

    # Every part of the span's floor is following it FASTER than one for one,
    # so the floor is outrunning the signal driving it. That is what a
    # nonlinearity ahead of the measurement does.
    floorFollowsSignal @3;
}

struct SourceStats {
    blocksDelivered @0 :UInt64;
    samplesDelivered @1 :UInt64;

    # An overrun is a correctness event. It travels because a recording that
    # looks continuous and is not is the failure this project counts rather
    # than logs, and a remote client is exactly the caller that cannot see
    # the log.
    overrunEvents @2 :UInt64;
    samplesLost @3 :UInt64;
    lastLossIndex @4 :UInt64;
    writeIndex @5 :UInt64;

    # What the full-span spectrum says about the front end.
    #
    # THE DIAGNOSIS NOBODY COULD MAKE, the same shape as realtimeFactor
    # above. Measured on air 2026-09-20, an RTL-SDR v3 at 95.1 MHz in an
    # ordinary suburban FM environment: with gain=auto the detector reported
    # three intermodulation products as real tracks at confidence 1.00, and
    # setting gain to 20 improved the measured SNR of KKFM at 98.1 MHz by
    # 5.7 dB and removed every phantom. What a client could see was three
    # confident detections, which is true about the tracks and says nothing
    # about the radio.
    #
    # ON SourceStats AND NOT ON EngineInfo, because EngineInfo is what the
    # engine settled on at open and this moves every decision. It rides with
    # the counters a client already polls.
    #
    # MEASURED ONLY WHILE A DETECTOR IS RUNNING. It is computed from
    # core/detect/detector.h's own averaged spectrum and noise floor, which
    # is what makes it nearly free, and the server builds a detector on the
    # first request for detections. Until then this is unmeasured.
    frontEnd @6 :FrontEndState;

    # Decibels of floor movement per decibel the strongest signal on the span
    # moved, from the least-following part of the span. One is a gain change.
    # Above one the floor is outrunning the signal. Zero when unmeasured.
    frontEndSlope @7 :Float64;

    # How far the span's mean noise floor sits above the quietest this
    # engine's monitor has seen, in dB. A session low-water mark, descriptive
    # only, and no verdict rests on it. It is here because it is the number
    # that makes the sentence concrete.
    frontEndFloorLiftDb @8 :Float64;

    # RETUNES THE STAGE REFUSED, WHICH ARE THE ONES setVrxParams REPORTED AS
    # SUCCESSES.
    #
    # setVrxParams answers as soon as the control op is QUEUED. The graph
    # applies it at the next block boundary and a stage that will not take it
    # counts one here and leaves the receiver where it was. So the only thing a
    # client saw until this field existed was a call that returned success and a
    # receiver that did not move, with nothing anywhere saying which.
    #
    # THE REQUIREMENT IS THAT IT STAYS AT ZERO. engine::place refuses a
    # placement the demodulator cannot carry before the op is ever queued, so a
    # non-zero value here is not an operator asking for something impossible: it
    # is a request that passed placement and then failed at the stage, which
    # means those two disagree. Show it, and read it as a defect rather than as
    # a condition to handle.
    #
    # Engine-wide and not per receiver, because GraphStats is. A client that
    # sees it move re-reads vrxStatus on every receiver it holds to find which
    # one did not move.
    vrxRetuneRefusals @9 :UInt64;

    # Times the recording thread waited for a frame slot.
    #
    # EXPECTED ON A DEMAND SOURCE AND A WARNING ON A PACED ONE, which is why it
    # is not a fault and has to be read beside SourceDescriptor::flow. A Demand
    # source advances at exactly the rate its consumer retires work, so waiting
    # IS the backpressure and this climbing is the mechanism running. A Paced
    # source has its own clock and nowhere to put samples that keep arriving, so
    # a wait there is the graph falling behind the device and the next thing to
    # move is overrunEvents.
    #
    # A client that draws this without the flow control beside it reports the
    # healthy case as a fault on every file and every synthetic scene.
    frameStalls @10 :UInt64;
}

struct VrxParams {
    # An offset from the source's baseband DC, NOT an absolute radio
    # frequency, and it is bounded by plus and minus half the source rate.
    #
    # engine::place reads it as an offset, nothing rebases it, VrxStatus reads
    # it back in the same frame, and tools/cli/main.cpp converts on the way in
    # with `baseband = absolute - source_center`. Baseband is the only frame
    # the grid has, which is why it wins: place() is handed the grid, the rate
    # and the request, and is never told where the source is tuned.
    #
    # WHAT THIS PARAGRAPH USED TO SAY
    #
    # Until 2026-09-19 it gave the offset reading as something "said here
    # because the engine's own header says the opposite". core/engine/vrx.h
    # did document the field as an absolute radio frequency, and the header
    # was the half that was wrong: no build of this engine has ever rebased,
    # and place() has read an offset since it was written. The header has been
    # corrected and now agrees, which is why the old sentence had to go: a
    # reader following it to vrx.h found agreement and could not tell which of
    # the two documents had moved.
    #
    # Recorded rather than quietly swapped because the retracted claim was
    # repeated in core/rpc/types.h and docs/detection.md as well, so anyone
    # who took it from one of those three before today is owed the retraction
    # rather than a schema that reads as though it never said it.
    #
    # The consequence that matters for click-to-tune: a Detection's centerHz
    # is ABSOLUTE and cannot be assigned to this field. Subtract
    # EngineInfo::sourceCenter first. On a source that declares no centre the
    # two are the same number and the mistake is invisible; on a radio it is
    # either a receiver tuned megahertz away or an outright refusal, so it is
    # not a mistake that shows up gently.
    center @0 :Int64;

    # A SHORTHAND for the passband below, and frozen at this ordinal.
    #
    # WHAT THIS FIELD USED TO MEAN
    #
    # Until 2026-09-20 it was the whole passband request, and every quantity
    # derived from it read it as a half-width either side of `center`. That
    # reading was never true of all eight modes. plan_vrx already put USB's
    # passband at [center, center + B] and LSB's at [center - B, center], and
    # core/shaders/vrx_fine.comp said so in its own header: "USB and LSB are
    # an asymmetric passband and nothing else". So a caller that set this
    # field and expected a filter centred on the tuned frequency was already
    # not getting one on two of the eight modes, and had no way at all to ask
    # for the shapes real receivers use: USB is the carrier plus 300 to plus
    # 2700 hertz, a transceiver running wide transmit audio wants the edges
    # pulled out to 6 kHz without moving the carrier, and a CW operator wants
    # a window offset from the carrier by the sidetone.
    #
    # What it means now:
    #
    #   On input, with passbandLow and passbandHigh both zero, it is expanded
    #   through the mode's shorthand rule by dsp::resolve_passband, which
    #   reproduces the old geometry exactly for every one of the eight.
    #   Symmetric for raw, AM, NFM, WFM, DSB and CW; [0, B] for USB; [-B, 0]
    #   for LSB.
    #
    #   On input, with either edge set, it is IGNORED.
    #
    #   On output it is echoed exactly as it was sent, because VrxStatus
    #   hands back the params it was given and reading one out and passing
    #   it straight back in has to leave the receiver where it was. So it is
    #   not the width when the pair was given, and a reader that wants the
    #   width reads VrxPlacement::grantedHigh minus grantedLow, which is the
    #   figure the engine built a filter for rather than anything that was
    #   asked for.
    #
    # Recorded rather than renumbered away. The schema is unreleased, so
    # deleting the field breaks nothing today; what it would do is free
    # ordinal 1 for something else to take later, at which point an old
    # `bandwidth = 3000` message reads as whatever now lives there. That trap
    # fires once, silently, on a saved session, and the cost of avoiding it
    # is one frozen Int64.
    bandwidth @1 :Int64;

    demod @2 :Demod;
    audioRate @3 :UInt32;
    squelchDbfs @4 :Float64;
    agcAttackMs @5 :Float64;
    agcDecayMs @6 :Float64;
    agcEnabled @7 :Bool;

    # CW only. The offset the carrier is translated to so it is audible.
    #
    # NOT a passband edge, and the distinction is the whole reason the
    # passband below is stated about `center` rather than about whatever the
    # fine stage mixes to DC. The pitch moves the MIX and leaves the filter
    # where it is, so the filter still passes [center + low, center + high]
    # on CW exactly as on the other seven, and a CW window that sits off the
    # carrier is said with offset edges.
    cwPitch @8 :Int64;

    # The passband, as SIGNED HERTZ FROM `center`, low strictly below high.
    # Both zero means "not stated", and then `bandwidth` above is expanded
    # through the mode's shorthand rule instead.
    #
    # The frame is `center` for every mode with no exceptions, so the filter
    # passes [center + passbandLow, center + passbandHigh]. That is what
    # makes USB read the way an operator says it: a 3.750 MHz carrier with
    # low = +300 and high = +2700. Anchoring on the channel centre or on the
    # mix centre would each have made one mode's numbers unrecognisable.
    #
    # Each edge is fitted to the channel on its own, so a request too wide on
    # one side keeps the other edge where it was. VrxPlacement::grantedLow
    # and grantedHigh are what came back, and comparing them against these
    # two is how a display says WHICH edge was clamped rather than only that
    # something was.
    passbandLow @9 :Int64;
    passbandHigh @10 :Int64;
}

struct VrxPlacement {
    channel @0 :UInt32;
    channelCentre @1 :Rational;
    residual @2 :Rational;
    channelRate @3 :UInt32;

    # True when EITHER passband edge was pulled in to fit one grid channel.
    # The caller is told rather than quietly receiving less than it asked
    # for, and the display needs it because the consequence is audible.
    #
    # WHAT THIS FIELD USED TO MEAN: "the requested bandwidth did not fit one
    # grid channel and the receiver was given the widest that does". That
    # was one width against one limit, and a clamped receiver was still
    # centred where it was asked to be. The fit is per edge now, so a
    # receiver can come back off-centre as well as narrow, and this bool
    # says only that something moved. grantedLow and grantedHigh say what.
    bandwidthClamped @4 :Bool;

    # The passband the receiver was actually given, in the same frame
    # VrxParams::passbandLow and passbandHigh are in: signed hertz from
    # params.center.
    #
    # Equal to the resolved request unless the channel could not carry it.
    # A display draws the requested pair and this pair in two shades, which
    # is the whole reason they are both on the wire: one shade can say a
    # filter is 8 kHz wide, and two can say it was asked to be 10 and lost
    # the top.
    grantedLow @5 :Int64;
    grantedHigh @6 :Int64;

    # Empty when nothing was clamped. Otherwise the sentence an operator
    # needs, naming what was asked for, what one grid channel could carry,
    # and what to do about it.
    #
    # WHY A SENTENCE WHEN THE NUMBERS ARE ALREADY HERE. They are, and
    # nothing drew them. An operator clicking a broadcast FM station on a
    # 2.4 MS/s dongle with the default 64-channel grid asks for 200 kHz and
    # is given about 71, and every surface reported that correctly:
    # bandwidthClamped went true, grantedLow and grantedHigh came back
    # narrow, and what the operator got was mush out of the loudspeaker
    # while the waterfall showed a strong signal. The engine knew the ratio
    # and no client said it, because saying it means knowing that a
    # three-to-one clamp on an FM mode is not a narrower filter.
    #
    # IT IS NOT A NARROWER VERSION OF WHAT WAS ASKED FOR, ON THE FM MODES.
    # A discriminator recovers the instantaneous frequency of what reaches
    # it, so truncating the passband of a signal deviating 75 kHz does not
    # produce quieter audio at less bandwidth: it produces the wrong audio.
    # The linear modes are different and the sentence says so, because an
    # AM or SSB receiver given a narrower filter is exactly a receiver with
    # a narrower filter.
    #
    # WHAT TO DO ABOUT IT IS IN THE SENTENCE TOO, because it is not
    # something a client can work out. The fix is a coarser grid, which is
    # engine-wide and set before the source is opened, so it is the
    # engine's command line rather than anything this session can change.
    #
    # THE FM CASE ABOVE NO LONGER REACHES THIS FIELD, as of 2026-09-21, and
    # the paragraphs above are kept because they are why it exists. What
    # this paragraph used to describe as the field's headline case, a
    # broadcast FM receiver clamped three to one and reported here, is now
    # refused by engine::place before a receiver is created: a status field
    # is read after the audio is heard, and the audio was the complaint.
    # The refusal carries the same facts and the channel count that would
    # have worked.
    #
    # The field is not vestigial. Every linear mode still clamps and still
    # reports here, which is right, because an envelope or product detector
    # given a narrower filter is exactly a receiver with a narrower filter.
    # An FM receiver whose grant fell short of the REQUEST but still covers
    # the mode's own channel plan reaches it too.
    #
    # Prose for a human, and never parsed. A client displays it beside the
    # receiver and decides whether to draw anything from bandwidthClamped
    # and the granted pair, which are the machine-readable half and are not
    # going anywhere.
    clampReason @7 :Text;
}

struct VrxStatus {
    id @0 :UInt64;
    params @1 :VrxParams;
    placement @2 :VrxPlacement;

    # Passband level in dBFS, updated per block. This is the meter in the
    # receiver rack.
    levelDbfs @3 :Float64;
    squelchOpen @4 :Bool;

    # FRAMES this receiver produced, and frames it produced that reached
    # nothing. Carried rather than logged because a remote client cannot read
    # a log.
    #
    # audioDropped IS THE ENGINE'S OWN LOSS AND IS NORMALLY ZERO. A frame
    # counted here was handed to the receiver's sink and refused, which
    # core/engine/graph.cpp also turns into a failing dispatch, so a non-zero
    # value means the run is ending rather than that a listener missed a
    # syllable.
    #
    # WHAT THESE TWO USED TO SAY: "a dropped audio sample is a dropout the
    # operator hears". They are frames rather than samples, and until
    # 2026-09-20 the only site that incremented audioDropped was the squelch
    # mute, which is the opposite of a dropout: the chunk crosses at the full
    # rate with the sample index unbroken and AudioChunk::squelchOpen saying
    # why it is silent. A client drawing a dropout indicator from this field
    # lit it for the whole of every quiet channel.
    #
    # The number a listener wants is per CONSUMER and cannot live here. One
    # receiver can feed a recording and two subscriptions at once, and their
    # losses are three different numbers with one field to hold them.
    # AudioStats::framesDropped is this subscription's, and it moves.
    audioSamples @5 :UInt64;
    audioDropped @6 :UInt64;

    # The rate the fine stage resampled to, which is the width of this
    # receiver's passband frame and what decides whether a change can be
    # applied in place.
    #
    # On the wire because a client cannot derive it. It is
    # dsp::minimum_demod_rate rounded up to a whole multiple of the audio
    # rate, and the audio rate a receiver ended up with is the engine's
    # default when params.audioRate was zero. A retune that moves it is a
    # remove and an add rather than a push constant, so a surface dragging
    # the passband edges reads this to tell a change it can send live from
    # one that will break the audio mid-gesture.
    #
    # Zero from an engine built before this field existed.
    demodRate @7 :UInt32;

    # Times this receiver's input samples were overwritten before the engine
    # could filter them, and the audio frames those restarts skipped past.
    #
    # A receiver in that position restarts from the oldest samples still held,
    # and the frames in between are never produced, so audioSamples cannot show
    # them and AudioChunk::start does not step across them either: that index
    # counts frames delivered, so the hole closes over itself. This pair is the
    # only record that it was there, which is why a client drawing a dropout
    # indicator wants it rather than audioDropped, and audioDropped is an
    # engine fault that ends the run in any case.
    #
    # WHAT A CLIENT DOES WITH IT. reanchors climbing while
    # SourceStats::samplesLost stands still means this machine is not
    # keeping up with its own source, which is a machine to fix and not a
    # radio to retune: the audio is choppy, every other counter reads clean,
    # and nothing else on the wire says so. Both climbing together is the
    # ordinary consequence of a device retune, which stops the transfers for
    # about a third of a second and declares the gap, so a client that draws
    # this pair without samplesLost beside it reports every deliberate tune as
    # a fault.
    #
    # READING THE FRAME COUNT AS A RATE IS THE MISTAKE TO AVOID. It is frames
    # never produced since the receiver was added, on the same terms
    # audioSamples counts frames that were, so what it supports is a ratio
    # against audioSamples and a difference between two polls. Read as an
    # instantaneous figure it says a receiver that skipped once an hour ago is
    # skipping now.
    #
    # Both zero from an engine built before these fields existed, and both
    # zero for a raw tap, which holds no cursor that can fall behind. Zero is
    # "this did not happen" rather than "this is not measured".
    reanchors @8 :UInt64;
    reanchorFramesSkipped @9 :UInt64;

    # The audio rate this receiver ACTUALLY runs at, which params.audioRate is
    # not when the request named none.
    #
    # A RESULT, ON A STRUCT WHOSE OTHER RATE IS AN ECHO. params.audioRate of
    # zero means "the engine's default", the graph resolves it once and builds
    # the stage and the plan from the resolved value, and this is that value.
    # The echo stays zero on purpose, because feeding a status back into
    # setVrxParams must leave the receiver on the default rather than pinning
    # it to whatever the default happened to be.
    #
    # ASK QUESTIONS OF THIS FIELD AND NOT OF THE ECHO. Everything that depends
    # on the audio rate depends on this one: whether a de-emphasis curve or
    # stereo applies, and whether the receiver can carry an RDS composite at
    # all. The engine caught applied_deemphasis and decoding_stereo answering
    # against the echo and fixed them; the server's own RDS guard was still
    # asking the echo until 2026-09-21, and so refused every receiver created
    # the ordinary way with a message saying the rate was unknowable. It was
    # not unknowable, it was on the status.
    #
    # WHAT A CLIENT DOES WITH IT. An operator asking for RDS needs a receiver
    # whose audio rate carries the 57 kHz subcarrier, which in practice means
    # 171000. A client that reads only the echo cannot tell a receiver at the
    # default from one explicitly asked for zero, and cannot tell an operator
    # what their receiver is running at.
    #
    # Zero from an engine built before this field existed, and zero on a
    # default-constructed status. Both mean "nobody filled this in", not "this
    # receiver runs at no rate".
    resolvedAudioRate @10 :UInt32;

    # WHO THIS RECEIVER BELONGS TO, which is the owner decision of 2026-09-22
    # made visible: a receiver belongs to the session that created it and is
    # removed when that session ends, unless addVrx was asked to keep it.
    #
    # creatorSession is a number the server gives each login, counted from
    # one and never reused for the life of the server. Zero means no session
    # created it: the process hosting the engine added it directly, and
    # nothing on this wire will remove it except an explicit removeVrx. It is
    # an identity for comparing two receivers' creators and nothing else, and
    # a kept receiver goes on naming a session that has since ended.
    #
    # ownedByCaller is creatorSession compared against the session asking,
    # which is what a client wants and could not otherwise work out: a client
    # holds no session id of its own. A receiver that is neither the caller's
    # nor kept belongs to another live session and will go when that one
    # does; one that is kept belongs to nobody and stays until somebody
    # removes it. Both zero and false from an engine built before these
    # existed.
    creatorSession @11 :UInt64;
    kept @12 :Bool;
    ownedByCaller @13 :Bool;
}

struct SpectrumFrame {
    # Decibels relative to full scale, ascending in frequency across the
    # whole span, no gaps and nothing counted twice.
    powerDb @0 :List(Float32);

    geometry @1 :SpectrumGeometry;

    # Source samples this frame's window covers, [start, start + count).
    # Absolute from the start of the stream, so it lines up with anything
    # else indexed the way docs/conventions.md indexes time.
    start @2 :UInt64;
    count @3 :UInt64;

    # Frames the ENGINE produced before this one, not frames this
    # subscription received. A client that asked for decimation knows what it
    # asked for; what it cannot otherwise know is whether the engine also
    # skipped, so the two are kept distinguishable.
    sequence @4 :UInt64;

    floorDb @5 :Float32;
    ceilingDb @6 :Float32;
    percentileLowDb @7 :Float32;
    percentileHighDb @8 :Float32;
}

# Where a detection is in its life, ordinal for ordinal with
# revenant::detect::TrackState. Same arrangement as Demod above and for the
# same reason: core/rpc/convert.h static_asserts every pair, so a state
# renumbered on one side and not the other stops the build rather than making
# a held track read as a live one.
enum TrackState {
    # Never appears on the wire. Detector::tracks() publishes Live, Held and
    # Merged only, because a pending candidate has not earned an identity yet
    # and a display that drew one would be drawing the false alarms the birth
    # rule exists to discard. It is here so the ordinals match, not because a
    # reader will see it.
    pending @0;

    live @1;

    # Not detected at the most recent decision, inside its hold, decaying.
    # docs/ui-spectrum.md wants this drawn rather than dropped: almost
    # everything worth detecting is intermittent, and a display that removed a
    # track the moment it went quiet would flicker through CW keying and put a
    # click on something that had just vanished.
    held @2;

    # Not detected because another track's band swallowed it. Distinct from
    # held on purpose: a merged track has evidence, it is simply inside
    # somebody else's measurement, so it is not decaying. Collapsing the two
    # loses every id on the far side of a merge.
    merged @3;
}

# One thing the wideband detector is tracking, as a display needs it.
#
# NOT EVERY FIELD OF revenant::detect::Track, WHICH IS THE POINT
#
# Track carries the tracker's own working state as well: hit and miss counts,
# the signed pre-reduction channel index the hysteresis is carried in, and
# what tier two's probes found. None of that is something a display draws or
# a click resolves against, and a schema is a contract rather than a mirror.
# The family in particular stays off the wire by docs/detection.md's own
# decision, and a probe is not a receiver a client can see.
#
# WHAT THE FIRST SENTENCE USED TO SAY AT ITS END: "and a classification enum with
# exactly one value in it". core/detect/detector.h carries the families
# core/characterise names now, filled by tier two. What is here is what docs/ui-spectrum.md asks for:
# draw every detection above an operator-set confidence, hold it while it
# decays instead of flickering, and let a click tune to it.
#
# THE CENTRE IS A MEASUREMENT AND NOT A TUNING TARGET
#
# centerHz is the centre of the measured occupied band, which is what the
# detector computes and all it computes. docs/ui-spectrum.md is explicit that
# this is not where a receiver wants to sit for several modes: RTTY is two
# tones about 170 Hz apart and which one is radiating depends on the character
# being sent, so a click that lands on the measured centre wobbles with the
# mark-to-space ratio, and one that lands on the peak dances between the two.
# SSB is worse and quieter, because the suppressed carrier the receiver is
# trying to hold sits at the edge of the passband rather than in it.
#
# The logical centre those cases want is a property of the MODULATION, and
# deriving it needs a classification. Track::classification names a family at
# most, never a mode or a shift, and it stays inside the process that runs the
# detector; docs/ui-spectrum.md notes that the enumeration would have to grow
# before RTTY and its shift were even expressible in VrxParams.
#
# WHAT THE SECOND SENTENCE USED TO SAY: "core/detect/detector.h leaves
# Track::classification as a deliberate seam that nothing fills". Tier two
# fills it now, with a family and not a logical centre. So there is no
# logicalCentreHz field here: it would be a field nothing could fill, and a
# client reading a measured centre out of a field named "logical" would tune
# wrong with the wire telling it it was right.
#
# What a click-to-tune surface should do with this today is what AFT does with
# an unidentified signal in docs/ui-spectrum.md: take the measured centre,
# and hold still rather than guess.
struct Detection {
    # Issued in order from one and never reused, so an id names one signal for
    # the life of the engine process. This is what a click resolves against
    # and what a display keys a row on across polls.
    id @0 :UInt64;

    # Absolute radio frequency, EngineInfo::sourceCenter already added.
    #
    # Int64 and not a Rational, and that is not the schema rounding. The
    # detector rounds to integer hertz once, at measurement, out of the exact
    # rational frequency axis the frame carries, because a centre derived from
    # a bin index and an occupied-power fraction is an estimate to well under
    # a bin and carrying it as a ratio would dress a measurement up as an
    # exact grid frequency. This is the same distinction the note at the top
    # of this file draws for channelSpacing and channelRate: a Rational here
    # would claim an exactness that does not exist upstream of the wire.
    #
    # ABSOLUTE IS NOT THE FRAME VrxParams IS IN. Tuning a receiver to this
    # detection means `VrxParams::center = centerHz - EngineInfo::sourceCenter`,
    # because the grid works in baseband and nothing rebases for a caller.
    # See the note on VrxParams::center above, which is where that is settled.
    centerHz @1 :Int64;

    # Occupied bandwidth: the span holding the detector's occupied-power
    # fraction of the band's excess over the noise floor, bounded by fine
    # bins. Integer hertz for the same reason as the centre.
    #
    # A click-to-tune surface turns this into a passband against the mode the
    # operator selected, and then reads VrxPlacement::grantedLow and
    # grantedHigh back, because a passband wider than one grid channel does
    # not fail: place() fits each edge and succeeds, and the operator gets a
    # receiver narrower than the signal they clicked with nothing anywhere
    # saying so. docs/detection.md names that as the failure this pair exists
    # to prevent.
    #
    # WHAT THIS PARAGRAPH USED TO SAY. Until 2026-09-20 it told a
    # click-to-tune surface to pass this straight into VrxParams::bandwidth
    # and read VrxPlacement::bandwidthClamped back. The second half still
    # holds in spirit. The first half is wrong for the sideband modes and was
    # wrong when it was written: the detector measures the band that has
    # energy in it, and on USB that band is entirely above the suppressed
    # carrier, so assigning it to a field read as a half-width either side
    # parks the filter straddling a carrier that is not being transmitted and
    # throws away half the audio.
    #
    # The instruction that replaces it: centre the measured band for the
    # symmetric modes, and for USB, LSB and CW anchor one edge on the
    # measured band's near edge and let the other follow the measured width.
    # dsp::resolve_passband is what the engine does with a bare bandwidth and
    # is the same rule written out.
    bandwidthHz @2 :Int64;

    # Signal to noise in the project's 2500 Hz reference bandwidth, per
    # docs/snr-convention.md. Stated in that bandwidth rather than per bin
    # because a per-bin peak is not comparable across bandwidths, which is
    # exactly what a list holding a 50 Hz carrier and a 200 kHz broadcast
    # needs it to be.
    snr2500Db @3 :Float64;

    # Zero to one, rising on each decision the track was detected in and
    # decaying on each one it was missed from. The track's own and never any
    # single frame's.
    #
    # WHAT THIS COMMENT USED TO SAY, and it described a classifier this engine
    # does not have. It read: "Zero to one, rising on evidence and decaying on
    # silence. The track's own and not any single frame's, which is what makes
    # it worth thresholding: a transmission does not change modulation halfway
    # through, so a track that has been up for five seconds has had five
    # seconds of evidence."
    #
    # IT MEASURES PERSISTENCE AND NOT QUALITY. The value is one minus
    # (1 - rise) to the n over n consecutive detections, and no term in it
    # reads snr2500Db, bandwidthHz, the deflection margin or the shape of the
    # band. Two detections present in the same decisions carry bit-identical
    # confidence however strong or weak each one is, so a steady carrier and a
    # steady bump in the noise floor both read 1.00 after about 1.3 seconds.
    # An operator reported exactly that against intermod products on
    # 2026-09-21.
    #
    # A client thresholding on this is filtering by "how long has this been
    # here", which is worth doing and is not what the name promises. The
    # calibrated number is marginConfidence below.
    #
    # It approaches one without arriving. See Session::detections, which
    # refuses a bar of one for that reason.
    confidence @4 :Float64;

    # How far this stood above the detection threshold it had to clear, mapped
    # to zero to one: a half exactly at the threshold, 0.82 six decibels above
    # it, 0.93 at twelve, approaching one without arriving.
    #
    # THE CALIBRATED NUMBER confidence IS NOT. docs/detection.md asks for one
    # on the grounds that a classifier reporting 0.9 for everything makes an
    # operator's threshold a no-op, and confidence above is exactly that: it
    # counts consecutive detections and reads no signal quality, so a column of
    # it is constant and a bar on it filters by how long something has been
    # there. This reads the measurement and nothing about time. The two are
    # orthogonal on purpose and a client wanting either can have it: a strong
    # station detected once has a high margin and a low confidence, and a weak
    # bump present all afternoon has the reverse.
    #
    # WHAT IT IS NOT. It is not authenticity, and reading it as any would
    # repeat the mistake confidence made. It orders detections by how far their
    # evidence stood above the noise, which is all a margin can say. A strong
    # interferer stands well above the noise and scores high, correctly.
    # Telling a signal from an intermodulation product is a different question,
    # no field here answers it, and core/detect/front_end.h explains why a
    # frequency-coincidence test is worse than saying nothing.
    #
    # Taken against the threshold IN FORCE rather than a constant, so raising
    # the bar does not make every surviving detection look weaker than it did.
    # The map is characterise::margin_confidence, which is written down and
    # tested rather than tuned until the numbers looked right.
    marginConfidence @12 :Float64;

    state @5 :TrackState;

    # Absolute source sample indices, the way docs/conventions.md indexes
    # time. Seconds are (index difference) / EngineInfo::sourceRate, computed
    # by whoever is drawing rather than here, because the detector reads no
    # clock at all: every interval it knows is a difference of sample indices
    # taken from the frames themselves, which is what lets a capture replayed
    # at forty times realtime produce the same tracks with the same ages.
    #
    # lastSeen minus lastDetected is how long this has been holding, and is
    # zero while it is live. lastSeen minus firstSeen is its age.
    firstSeen @6 :UInt64;
    lastSeen @7 :UInt64;
    lastDetected @8 :UInt64;

    # The coarse channel a receiver on this track would read, with the
    # hysteresis docs/detection.md requires: place() takes the nearest channel
    # by rounding, so a track parked on a boundary flips on measurement noise
    # and each flip is a different tap table and a half-megabyte upload.
    #
    # Meaningless unless channelValid, which needs the engine's grid, so a
    # reader checks the flag rather than the value. A display uses it to warn
    # that a track sits where a small retune is expensive.
    channel @9 :UInt32;
    channelValid @10 :Bool;

    # The track this one was merged into, while state is merged. Zero
    # otherwise, and never a valid id, because ids are issued from one.
    mergedInto @11 :UInt64;

    # This band's excess in its strongest three adjacent bins, over its excess
    # in total. detect::BandShape::concentration, which is the same quantity
    # characterise::spectral_concentration answers in, so a client holding both
    # tiers can compare them directly.
    #
    # WHAT IT IS FOR. confidence says how long a track has been there and
    # marginConfidence says how far it stood above the threshold; neither says
    # the band is SHAPED like a transmission, and an operator reported exactly
    # that gap on 2026-09-21 with intermod products and raised patches of floor
    # listed beside stations at confidence 1.00. Measured on 20 m at 1603 UT,
    # an 11.7 kHz patch of noise floor sitting at confidence 1.00 reads 0.02
    # here, against 0.59 to 0.86 for the carriers in the same list.
    #
    # READ IT WITH bandwidthHz, and the bin width the frame reports.
    #
    # Under about five bins it says nothing: the analysis window spreads one
    # line over that many, so a band that is one line reads near one whatever
    # produced it. Measured across one emitter of each family, cw, am, nfm,
    # usb and lsb all read 0.946 to 0.947.
    #
    # Above that it separates a LINE SPECTRUM from a FILLED one. In the same
    # measurement fsk2 reads 0.519, two tones with half the power in one, and
    # bpsk and qpsk read 0.117 and 0.121. A high reading on a wide band is a
    # carrier with sidebands, not a width that was measured wrong: a 489 Hz
    # band on 20 m reading 0.82 keeps 473 Hz of its power down to the 80th
    # percentile and only collapses at the 50th.
    #
    # NOT A VERDICT. Nothing thresholds on it anywhere, and no threshold has
    # been chosen, because choosing one is a measurement against known truth
    # rather than a number somebody liked. It is not authenticity either: a
    # strong interferer can be perfectly concentrated.
    #
    # A BAND OF THREE BINS OR FEWER READS EXACTLY ONE and means nothing by it.
    # Check bandwidthHz against the frame's bin width first.
    concentration @13 :Float64;

    # Whether the shape was measured at all. False for an empty band or one
    # whose numbers could not be formed.
    #
    # CARRIED SO A CLIENT CAN REFUSE RATHER THAN BELIEVE A ZERO, which is the
    # same argument BandShape::skirt_bins_available makes: unmeasured arrives
    # as 0.0, and 0.0 is the most noise-like reading there is, so a client
    # without this flag would draw "we could not tell" as "certainly junk".
    shapeMeasured @14 :Bool;
}

# One answer to Session::detections.
struct DetectionList {
    # Ascending in frequency, holding those detections whose confidence
    # reaches the bar the request asked for.
    detections @0 :List(Detection);

    # Decisions the detector has taken since this server built it, and the
    # source sample index the most recent one was taken at.
    #
    # Both are here because an empty list has two causes a client cannot
    # otherwise tell apart. A detector that has not decided yet reports zero
    # decisions; one that has decided and found nothing reports a count and a
    # band that is genuinely quiet. The first server call that asks for
    # detections is what builds the detector, so the very first answer is
    # always the former.
    #
    # lastDecision also lets a client tell a repeated answer from a fresh one
    # without diffing the list, which is what a poll wants.
    decisions @1 :UInt64;
    lastDecision @2 :UInt64;

    # Detections the detector holds at that decision, before the request's
    # confidence bar was applied. A short list and a filtered one look
    # identical without it.
    total @3 :UInt32;

    # What the detector's detection threshold is set to, in dB of SNR in the
    # reference bandwidth. Read back rather than assumed: it is engine-wide
    # mutable state, so a second client may have moved it, and a display that
    # showed its own last request would be showing a number the detector is
    # not using.
    detectionThresholdDb @4 :Float64;

    # How long the detector keeps a track it is no longer detecting before it
    # drops the track, in seconds of SOURCE time.
    #
    # DetectorConfig::bootstrap_hold_seconds, read back off the running
    # detector for the same reason the threshold above it is: it is
    # engine-side configuration and a client has no other way to see it. A
    # track whose state is held has stopped being detected and is somewhere
    # inside this window, and lastSeen minus lastDetected over
    # EngineInfo::sourceRate is how far through it that track has got.
    #
    # NOT A DISPLAY SETTING. It says when the ENGINE stops publishing a
    # track, and that is the whole of what it says: whether a display fades a
    # box over the interval, dims it in one step, or draws it unchanged until
    # it vanishes is the display's own decision. The two got confused because
    # ui/render/spectrum_item.h carried its own copy of the number, compiled
    # in and labelled as the display's assumption about the engine's
    # configuration, which is exactly the assumption this field removes.
    #
    # Zero means the engine did not state it, which is an engine built before
    # this field existed. A reader must not take zero as a track that is
    # dropped the instant it goes quiet.
    detectorHoldSeconds @5 :Float64;
}

# What a subscriber implements. The engine calls this; the client does not
# poll, because a poll either lags the frame rate or busies a thread.
interface SpectrumReceiver {
    frame @0 (frame :SpectrumFrame) -> ();
}

# Dropping this capability ends the subscription. There is a cancel as well
# because an explicit end is easier to read in a log than a dropped
# reference, and because a client shutting down cleanly should not have to
# rely on garbage collection timing to stop a stream.
interface SpectrumSubscription {
    cancel @0 () -> ();
}

# The same two, for one receiver's passband. Separate interfaces rather than a
# frame union on the spectrum pair, because a client normally wants the span
# at one rate and a receiver's passband at another, and a single subscription
# would make cancelling one cancel both.
interface PassbandReceiver {
    frame @0 (frame :PassbandFrame) -> ();
}

interface PassbandSubscription {
    cancel @0 () -> ();
}

# ---------------------------------------------------------------------------
# Audio
# ---------------------------------------------------------------------------

# One receiver's audio, as raw float32 PCM. The note at the top of this file
# has the codec decision and the retraction that goes with it.
struct AudioChunk {
    # Interleaved when channelCount is above one. Real audio and never complex
    # baseband: subscribeAudio refuses a raw tap, and says why.
    #
    # Float32, and the headroom is the reason rather than convenience. A
    # demodulator puts a fully modulated signal at exactly full scale by
    # convention and a settling AGC overshoots that, so samples above 1.0 are
    # ordinary and are what a 16-bit path would clip.
    samples @0 :List(Float32);

    # IN THE STREAM AND NOT ASSUMED BY THE CLIENT, which costs six bytes a
    # chunk and is deliberate.
    #
    # Both are fixed for the life of a receiver. dsp::VrxShape carries
    # output_rate and Graph::set_vrx_params refuses a shape change in place
    # with "remove and an add", so a client could read them once from
    # VrxStatus and cache them. It should not, and the reason is not
    # defensive: ui/models/receiver_link.cpp turns that very refusal into a
    # remove and an add that keeps the pane's identity, so a pane outlives the
    # receiver behind it and the next receiver can be at another rate. A
    # client playing the new stream at the cached rate sounds like a tape at
    # the wrong speed and points at nothing.
    #
    # channelCount reads 2 on a WFM receiver decoding stereo and 1 on
    # everything else. engine::resolve_stereo decides it from the mode,
    # VrxParams::stereo and the audio rate, and the decision is a request
    # rather than a promise only in what the two channels CARRY: a station
    # with no pilot still arrives as two, bit-identical sample for sample,
    # because the kernel gates the difference channel by multiplying it by
    # zero. A client that hardcoded 1 plays one channel of a pair at half
    # speed, which is why the field is here and why it must be read.
    #
    # WHAT THIS PARAGRAPH USED TO SAY. Until 2026-09-21 it read
    # "channelCount reads 1 on every engine built from this tree. Every
    # demodulator in core/engine/vrx_stage.cpp sets StageOutput::channels to
    # 1, and the one stage that sets 2 is the raw complex tap, which cannot
    # be subscribed to. It is on the wire for WFM stereo, which is the next
    # thing that will change it". WFM stereo landed in f3c0544 and is what
    # changed it. Nothing about the wire moved, because the field was put
    # here for exactly this; only the sentence saying nobody sends two.
    # core/engine/vrx.h carried the replacement text on VrxParams::stereo
    # because the lane that made this false could not edit this file, and a
    # correction filed where it was discovered rather than where it is read
    # is how a schema goes on lying to every client author for a day.
    sampleRate @1 :UInt32;
    channelCount @2 :UInt16;

    # Absolute index of the first FRAME in this chunk, from the start of this
    # receiver's audio stream, at sampleRate. Frames and not interleaved
    # samples, because docs/conventions.md indexes time in frames and a stereo
    # stream indexed in samples counts every instant twice.
    #
    # This is what lets a client DETECT a gap instead of inferring one. The
    # stream is contiguous when this equals the previous chunk's sampleIndex
    # plus its frame count, and the difference when it is not is exactly how
    # much silence keeps the timeline whole.
    #
    # Zero on the first chunk, which arrives later than a client expects. A
    # demodulator produces no frames at all until its filter support is inside
    # real samples, and core/engine/graph.cpp emits no chunk for a dispatch
    # that produced none, so the stream opens with a silent interval that is
    # not a loss and must not be counted as one.
    sampleIndex @3 :UInt64;

    # Frames this subscription lost ON THE WIRE between the previous chunk it
    # was sent and this one, because its queue was full. subscribeAudio below
    # has the queue and the depth.
    #
    # NOT INTERCHANGEABLE WITH A sampleIndex GAP, and carrying both is the
    # point. A gap of exactly this many frames was lost here, by a consumer
    # that could not keep up. A gap LARGER than this lost the remainder
    # upstream, in the engine, where core/engine/audio_egress.cpp already
    # separates a fill from a discontinuity from a drop. Two causes with two
    # different fixes, so a client that wants to say which is at fault
    # subtracts.
    #
    # THIS COUNTER CAN BE NON-ZERO, WHICH IS NOT TRUE OF EVERY COUNTER HERE
    #
    # core/rpc/client.h carries a retraction about a dropped-frame counter
    # that is structurally always zero: its callback runs inline on the event
    # loop thread, and that thread cannot dispatch the next frame until the
    # callback returns, so no interleaving exists in which one could be
    # dropped. This one is taken on the engine's completion thread at the
    # eviction site and drained by the event loop thread at the CLIENT's pace,
    # because the loop cannot take the next chunk off the queue until the
    # client's chunk() promise resolves. Two threads running concurrently with
    # the slow one outside the process is what makes the queue fill, and a
    # client that stalls for two seconds against a 500 ms depth loses
    # (2.0 - 0.5) * sampleRate frames, within one chunk.
    #
    # The eviction is from the FRONT of the queue, so the frames counted here
    # lie exactly between the last chunk sent and this one. That makes the
    # invariant checkable per pair:
    #
    #   sampleIndex == previous.sampleIndex + previous frame count
    #                  + framesDroppedBefore
    #
    # and it fails loudly if eviction is ever taken from the other end.
    framesDroppedBefore @4 :UInt64;

    # The squelch gate at the moment these frames were produced.
    #
    # A closed gate is not a drop and not a gap. core/engine/graph.cpp fills
    # the chunk with zeros and the chunk is sent at the full rate, so the
    # timeline stays whole and the wire stays busy. It is on the chunk rather
    # than left to a vrxStatus poll so an indicator follows the audio instead
    # of lagging it, and so that a test can tell counted silence apart from a
    # server that dropped everything on a quiet channel.
    squelchOpen @5 :Bool;
}

# This subscription's running totals. Per subscription and not per server:
# core/rpc/server.cpp charges one server-wide atomic for every spectrum
# subscriber at once and its own comment admits it over-counts. Audio has no
# shared decimation to excuse that, and a slow client's drops must never
# appear on a fast client's status line.
struct AudioStats {
    framesSent @0 :UInt64;
    framesDropped @1 :UInt64;

    # Times the queue went from not evicting to evicting. One two-second stall
    # and four hundred scattered hitches lose the same frames and sound
    # nothing alike, so the count of events is carried beside the total.
    dropEvents @2 :UInt64;

    # In this subscription's queue right now, and the depth it is enforcing.
    # Both in frames, because the depth was asked for in milliseconds and the
    # queue holds chunks, so the millisecond figure is not the number being
    # applied.
    #
    # bufferFrames is ZERO UNTIL THE FIRST CHUNK ARRIVES, which is not a
    # depth of zero and is not a queue that drops everything. The server
    # cannot turn milliseconds into frames before it has seen a chunk: it
    # needs the receiver's audio rate, which VrxParams::audioRate echoes as
    # zero for a receiver that took the engine's default, and it needs a
    # chunk's length to apply the two-chunk floor. subscribeAudio has the
    # whole of that argument.
    backlogFrames @3 :UInt64;
    bufferFrames @4 :UInt64;
}

# What an audio subscriber implements. The engine calls this; the client does
# not poll, for the reason SpectrumReceiver gives.
interface AudioReceiver {
    chunk @0 (chunk :AudioChunk) -> ();

    # No further chunk will arrive. Called at most once, after the last chunk,
    # and never for a cancel this client asked for.
    #
    # TWO THINGS END A STREAM THE CLIENT DID NOT ASK TO END, and both are
    # answered here. The receiver being removed is one. The other is a
    # chunk() call on THIS capability coming back failed, which the server
    # takes as final and does not retry, and which until 2026-09-20 ended
    # the subscription in silence on the reasoning that a failed capability
    # cannot be spoken to. That reasoning covers a dropped connection and
    # nothing else: a receiver that threw is a client still holding a
    # working capability and a stream that has stopped, which is the exact
    # pair this method exists for. A connection that has genuinely gone is
    # still silent, because nothing can cross it.
    #
    # WHY AUDIO HAS THIS AND THE TWO DISPLAY STREAMS DO NOT
    #
    # A receiver removed out from under a spectrum or passband subscription
    # freezes a display, and a frozen display is visible from across the room.
    # The same event on an audio subscription produces silence, and silence is
    # what a quiet channel with the squelch shut sounds like. There is nothing
    # else in the stream that distinguishes them, so the client is told.
    #
    # BEST EFFORT, stated rather than promised. A server whose event loop has
    # already stopped cannot make this call, and a dropped connection is the
    # other signal. reason carries the engine's own words when there are any,
    # and the failed call's own description when it was the receiver that
    # ended the stream.
    ended @1 (reason :Text) -> ();
}

# Dropping this capability ends the subscription. There is an explicit cancel
# as well, on the same terms as SpectrumSubscription: an explicit end reads
# better in a log than a dropped reference, and a client shutting down cleanly
# should not depend on collection timing to stop a stream.
#
# A CLIENT THAT HAS BEEN SENT ended SHOULD DROP THIS, and the reason is not
# tidiness. The subscription is over on the server's side either way, but the
# capability is what holds the server's node and its queue of up to
# bufferFrames of audio open, so a client that cycles receivers and keeps the
# capability each time accumulates one per removal until it disconnects.
# core/rpc/client.h's client did exactly that until 2026-09-20.
interface AudioSubscription {
    cancel @0 () -> ();

    # Refused once the subscription is over, whether this client cancelled it
    # or the server ended it, and the two refusals say which. Answering an
    # ended subscription's counters reports a healthy stream on one that has
    # stopped, and the numbers never move again.
    #
    # THE SERVER-ENDED REFUSAL USED TO SAY "ended by the engine" AND TO
    # PROMISE ended() HAD CARRIED THE REASON. Neither was reliably true. The
    # engine has no part in the second of the two ways a stream ends, a
    # chunk call this receiver failed, and that path sent no ended() at all
    # until 2026-09-20. Both are fixed on the server; the wording here no
    # longer attributes the cause to a component that may not have been
    # involved.
    stats @1 () -> (stats :AudioStats);
}

# ---------------------------------------------------------------------------
# RDS and RBDS
# ---------------------------------------------------------------------------
#
# WHICH RECEIVER, AND WHY THIS IS NOT A FLAG ON addVrx
#
# RDS is decoded from a receiver's audio, so every method here names one. It
# is the same arrangement subscribePassband uses and it is there for the same
# reason: until a client asks, that receiver runs no decoder and holds no
# state for one, and a recorder serving eight receivers should not pay for
# eight decoders nobody is reading.
#
# WHAT THE RECEIVER HAS TO BE, WHICH IS FOUR CONDITIONS AND NOT ONE
#
# rdsStation is refused, in the engine's own words, unless all four hold.
# Stated here in full because the natural bar is the rate alone and the rate
# alone certifies one reachable shape out of four:
#
#   1. The demodulator is nfm or wfm. Only a discriminator produces an FM
#      composite multiplex. An envelope detector and the four product
#      detectors produce audio with no subcarrier in it at any rate. NFM is
#      admitted as well as WFM: core/shaders/vrx_demod.comp reaches the same
#      atan2 branch for both, only the gain differs, and the decoder is scale
#      invariant.
#   2. The audio is real and mono. The raw tap writes an interleaved complex
#      pair, and handing that to a decoder expecting a real span reads I and
#      Q as consecutive samples of a signal that does not exist.
#   3. The audio rate clears the RECEIVER's bound and not the decoder's.
#      core/decode/rds_bits.h enforces 125000 because a composite from a file
#      or a modulator went through no audio filter. A composite from a
#      receiver did: the audio decimation filter's passband edge is 0.4 of the
#      audio rate, so 59375 Hz is inside it only from 148438 up. The one
#      exception is a receiver whose decimation resolved to 1, where the
#      planner designs no filter at all and 125000 is the true bound. Both are
#      reachable and the refusal distinguishes them. The band 125000 to 148437
#      with decimation above one is the silent trap core/decode/rds_bits.h has
#      now retracted twice.
#   4. The granted passband reaches at least 59375 Hz either side of the mix
#      centre, read off VrxPlacement::grantedLow and grantedHigh rather than
#      off the request, because each edge is fitted on its own. This is
#      NECESSARY AND NOT SUFFICIENT. Carson for a multiplex deviating 75 kHz
#      and reaching 59375 Hz is about 268750 Hz, the ordinary 200 kHz
#      broadcast passband already truncates the sidebands, and a receiver at
#      the bare minimum will decode worse than one at 200000. The health
#      counters below are how a client finds that out; the bar will not.
#
# A CLIENT SETS THIS UP ITSELF AND NEEDS NO NEW FIELD TO DO IT
#
# VrxParams::audioRate is already on the wire. A receiver built with
# demod = wfm and audioRate = 171000 emits the composite through the ordinary
# audio path with no resampling anywhere: 171000 is what
# decode::RdsBitsConfig::rate defaults to, it is three times the 57 kHz
# subcarrier and 144 times the 1187.5 bit/s bit rate. Point a DEDICATED
# receiver at the station rather than reusing the one being listened to, and
# the reason is now the signal rather than the plumbing: a 171 kHz composite
# is not audio anybody wants to hear, and a receiver at a rate somebody does
# want to hear it at has already lost the subcarrier.
#
# THAT SENTENCE USED TO CARRY A SECOND REASON AND IT IS GONE. It read "the
# engine allows one audio sink per receiver and setting a second replaces the
# first". core/engine/engine.h has AudioFanout and Engine::attach_audio_sink
# as of 2026-09-20, so a decoder joins whatever is already listening instead
# of displacing it. It costs a receiver rather than a listener now.
#
# AND THAT RECEIVER COSTS MORE GPU THAN A LISTENING ONE, NOT LESS. The audio
# decimation FIR runs at the OUTPUT rate, one detector evaluation per tap per
# output sample, and for a discriminator each of those is an atan2, so the
# count is output_rate * audio_taps:
#
#   listening   48000 x 353 = 16.9 M atan2/s
#   RDS        171000 x 103 = 17.6 M atan2/s
#
# Fewer taps, three and a half times the rate, 3.9 percent more work. Four
# percent is small enough that it changes nothing a client plans, which is
# why the number is here rather than a warning; docs/rpc.md said "slightly
# LESS GPU than a listening receiver" and the retraction is in place there.
#
# AND audioRate HAS TO BE STATED, not left at zero. VrxParams::audioRate is a
# verbatim echo on the way out, so a receiver that took the engine's default
# reads back as zero, and EngineInfo does not carry that default. The server
# refuses such a receiver naming this, rather than admitting one at whatever
# the default happens to be: it is 48000, where the composite is already
# destroyed. Putting the resolved rate on VrxStatus or EngineInfo would close
# it and is a schema change nobody has needed for anything else yet.
#
# ONE CANDIDATE THAT LOOKS OBVIOUS AND IS PHYSICALLY IMPOSSIBLE
#
# A narrow receiver placed 57 kHz off the station does not work at any price,
# and core/decode/rds_bits.h says so directly. FM is not a linear modulation,
# so a linear filter and mixer on the complex baseband picks up the upper
# skirt of the same FM carrier rather than the data: the composite exists
# only after the discriminator. It is written down here because it is the
# first thing anybody proposes.

enum RdsRegion {
    # The ordinals match revenant::decode::Region, which is kRds then kRbds.
    # core/rpc/convert.h asserts the pair, and core/rpc/convert.cpp converts
    # it in two directions that are deliberately not the same code.
    #
    # OUT, decode::Region to this enum, is an exhaustive switch with no
    # default rather than the cast the assert would allow, because an assert
    # catches a REORDER and cannot catch a third region added to the decoder
    # alone: that one would reach the wire as an ordinal no reader has a name
    # for, and the reader that matters is the one deciding which PTY table to
    # draw.
    #
    # IN, this enum to decode::Region, is a range check that REFUSES anything
    # above rbds and then a cast. A Cap'n Proto enum field may legally hold a
    # value the reader's schema has never heard of, which is how a client
    # built against a newer schema reaches an older engine, and the switch
    # has no case to put that in.
    #
    # This note used to end "what is here today is a matched ordering and not
    # an enforced one", which was true while nothing converted it. It is
    # enforced now. It then said, until 2026-09-20, that the file "converts
    # it through an exhaustive switch rather than the cast" without saying
    # which direction; the inbound half is the cast, guarded, and reading
    # that sentence as covering both is how the refusal came to be missing
    # from core/rpc/types.h's copy of it.
    #
    # A SETTING AND NEVER AN INFERENCE, and core/decode/rds_groups.h has the
    # argument at length. No field names the region; the nearest thing is an
    # optional Extended Country Code in a group type that is about a tenth of
    # traffic. The PI code cannot decide it either, because roughly 16 percent
    # of stations in the western USA compute to a first nibble of 0x01 and the
    # US call sign range collides with European country codes across the
    # board. And guessing wrong is silent: PTY 26 renders as National Music in
    # one region and Hip-Hop in the other, both draw, and nothing downstream
    # can tell. A client may seed this from the tuned frequency as long as it
    # shows it as a setting that can be overridden.
    rds @0;
    rbds @1;
}

enum RdsLock {
    # Matching revenant::decode::RdsLock, on the terms RdsRegion states.
    #
    # No bits are emitted in either of the first two states. An acquiring
    # decoder is tracking timing and measuring biphase consistency and has not
    # reached the lock threshold; it is not a decoder that is half working,
    # and a display that showed partial text from one would be showing text
    # nothing produced.
    unlocked @0;
    acquiring @1;
    locked @2;
}

enum RdsSync {
    # Matching revenant::decode::SyncState. The BLOCK layer's state, which is
    # a different thing from the bit layer's lock above and is carried
    # separately for that reason: a decoder can be locked to the subcarrier
    # and still hunting for the offset words, which is what the first second
    # after a tune looks like, and collapsing the two into one "locked" flag
    # loses the distinction between a wrong station and a station just
    # acquired.
    hunting @0;
    preSync @1;
    synced @2;
}

# EN 50067 clause 3.1.5.6 clock time, as the transmitter stated it.
#
# THE ENGINE READS NO CLOCK AND THIS IS NOT VERIFIED AGAINST ONE. Nothing in
# the DSP path reads a wall clock, deliberately, so this is the station's
# claim and nothing has checked it. A client that wants to know whether the
# station's clock is right compares it against its own; the engine cannot.
struct RdsClockTime {
    mjd @0 :Int32;

    # The Gregorian date the MJD converts to, UTC. Carried as well as the MJD
    # because the conversion is Annex G arithmetic with a validity range of
    # 1900-03-01 to 2100-02-28, and a client that re-derived it would be
    # writing that arithmetic a second time.
    year @1 :Int32;   # full year, not Annex G's years since 1900
    month @2 :Int32;
    day @3 :Int32;

    hour @4 :Int32;   # UTC
    minute @5 :Int32; # UTC

    # Local time offset in signed half hours. EN 50067 note 2 gives the range
    # as -12 h to +12 h and NRSC-4-B, tracking IEC 62106 Edition 2.0, gives
    # -15.5 h to +15.5 h. That is a VERSION difference rather than a region
    # difference and the field is 6 bits either way, so the wider range is
    # accepted in both regions rather than clamped per region.
    offsetHalfHours @6 :Int32;

    # Nothing above means anything unless this is set. A group 4A arrives
    # about once a minute, so this is false for the first minute of every tune
    # on a station that sends clock time at all, and forever on one that does
    # not.
    valid @7 :Bool;
}

# What the physical and block layers are doing, which is what tells a stale
# display from a dead signal.
#
# WHY THERE IS NO blockErrorRate FIELD
#
# Every counter here is cumulative from the moment the decoder was built and
# never resets short of a retune. A rate computed over that whole window is
# not the rate now: a station that faded five minutes ago and has been clean
# since reads badly forever. A client differences two polls and divides by the
# gap, which RdsStation::lastGroupSample gives it. Publishing a lifetime
# average under a name that reads like an instantaneous one is the kind of
# number somebody acts on.
struct RdsHealth {
    lock @0 :RdsLock;
    sync @1 :RdsSync;

    # Biphase sign consistency mapped onto [0, 1]. Zero is indistinguishable
    # from noise and one is a clean eye. This is the quality figure to draw,
    # because it degrades smoothly and means the same thing at every SNR:
    # clause 1.7 makes every symbol an odd impulse pair, so the two halves of
    # a bit always have opposite signs whatever the payload is.
    quality @2 :Float64;
    biphaseConsistency @3 :Float64;

    # |E[z^2]| / E[|z|^2] on the derotated baseband, which equals
    # SNR/(1 + SNR) in the post-filter bandwidth and so is readable as a
    # signal quality number in its own right.
    carrierCoherence @4 :Float64;

    # Residual subcarrier frequency error the carrier loop is holding, in
    # hertz, and the recovered bit rate against a nominal 1187.5.
    #
    # Float64 and not Int64, which is the opposite of what the note at the top
    # of this file says about frequencies, and deliberately so. These are
    # MEASUREMENTS rather than tuning requests: clause 1.1 allows the
    # subcarrier plus or minus 6 Hz, and rounding a measurement to whole hertz
    # throws away the only evidence a drifting transmitter would leave.
    carrierOffsetHz @5 :Float64;
    bitRateHz @6 :Float64;

    # Clause 1.1 permits a mono transmission to carry RDS with no pilot at
    # all, so an absent pilot is not a fault and a decoder that required one
    # would be wrong about the standard. With no pilot the subcarrier NCO runs
    # at exactly 57000 and the carrier loop absorbs the error.
    pilotLocked @7 :Bool;
    pilotLevel @8 :Float64;

    # Composite samples consumed, at RdsStation::compositeRate.
    samplesConsumed @9 :UInt64;

    # Bits the physical layer emitted, and the times it gave up and re-ran
    # timing acquisition. A climbing reacquisition count with bits still
    # flowing is a marginal signal; a climbing count with no bits is the wrong
    # station.
    #
    # THE BIT STREAM HAS GAPS AND NOTHING MARKS THEM. The decoder stops
    # emitting when lock falls away and starts again wherever it relocks, so
    # bitsEmitted is not elapsed time and two bits either side of a fade are
    # indistinguishable from two consecutive ones. That is survivable for the
    # group layer, which frames on the offset words and does not trust bit
    # adjacency, and it is NOT survivable for anything measuring a bit error
    # rate against a reference or timing a clock-time group against its
    # neighbours. Sample reacquisitions beside the bits if that is the
    # question being asked.
    bitsEmitted @10 :UInt64;
    reacquisitions @11 :UInt64;

    # The block layer. Cumulative, for the reason above.
    #
    # blocksCorrected counts blocks where a burst was repaired rather than
    # received clean, so it is trusted less than blocksGood rather than
    # equally: a client archiving decoded text should watch this, and an
    # archive should be taken with correction disabled.
    bitsFed @12 :UInt64;
    groupsDecoded @13 :UInt64;
    blocksGood @14 :UInt64;
    blocksCorrected @15 :UInt64;
    blocksDropped @16 :UInt64;
    syncAcquisitions @17 :UInt64;
    syncLosses @18 :UInt64;
}

# One Enhanced Other Networks entry: what this station says about another.
struct RdsEonEntry {
    pi @0 :UInt16;

    # Annex E code points, not UTF-8. See the note on RdsStation::ps.
    ps @1 :Data;
    psReceived @2 :UInt8;

    tp @3 :Bool;
    ta @4 :Bool;
    taValid @5 :Bool;

    pty @6 :UInt8;
    ptyValid @7 :Bool;

    linkage @8 :UInt16;
    linkageValid @9 :Bool;

    af @10 :List(Int64);
}

# An Open Data Application announcement from a type 3A group.
struct RdsOda {
    groupType @0 :UInt8;
    versionB @1 :Bool;
    message @2 :UInt16;

    # 0x0000 means the group is used for its normal feature rather than for an
    # application.
    aid @3 :UInt16;
}

# A count of the groups put to one use, and the most recent of them, raw.
#
# RAW BECAUSE THE STANDARD DEFINES NOTHING PAST THE HEADER. EN 50067 leaves
# these payloads to whoever owns the feature: the EWS bits are "assigned
# unilaterally by each country". A client shows the count and may show the
# bits in hexadecimal; it must not present them as decoded text or fields.
struct RdsRawGroup {
    # Groups put to this use since the decoder was built or last cleared.
    # Zero means none has arrived, and every field below is then meaningless.
    groups @0 :UInt64;

    groupType @1 :UInt8;
    versionB @2 :Bool;

    # b4..b0 of block 2.
    block2Low @3 :UInt8;

    # Block 3 is not payload in a version B group, where it repeats the PI,
    # and block3Valid is then false.
    block3 @4 :UInt16;
    block3Valid @5 :Bool;
    block4 @6 :UInt16;
    block4Valid @7 :Bool;

    # A block this payload depends on, block 2 included, was corrected, so
    # the bits may not be the station's.
    corrected @8 :Bool;
}

# One distinct 37-bit type 8A payload and how often it arrived.
struct RdsTmcMessage {
    x @0 :UInt8;    # block 2 b4..b0
    y @1 :UInt16;   # block 3
    z @2 :UInt16;   # block 4
    receptions @3 :UInt32;

    # Receptions in which block 2, 3 or 4 had been corrected. Equal to
    # receptions means this payload has never arrived clean.
    correctedReceptions @4 :UInt32;
}

# RDS-TMC, attributed and counted, NOT DECODED.
#
# The bit positions of ALERT-C's event, location, extent, direction,
# duration and diversion fields are ISO 14819-1 clauses 7 and 9, which were
# not in hand when the decoder was written; core/decode/rds_groups.h under
# TmcState says exactly what was. So there is no event code or location code
# here, and a client must not derive one from these bits by guessing which
# of them are which. What there is: whether a service is on air, how it was
# identified, and every distinct payload with its reception count.
struct RdsTmc {
    # A type 3A group announced one of the RDS Forum register's TMC AIDs,
    # 0x0D45, 0xCD46 or 0xCD47, on groupType/versionB, with odaMessage as its
    # message bits, raw.
    announced @0 :Bool;
    aid @1 :UInt16;
    groupType @2 :UInt8;
    versionB @3 :Bool;
    odaMessage @4 :UInt16;

    # Type 1A variant 1, "TMC identification", twelve bits, raw.
    identification @5 :UInt16;
    identificationValid @6 :Bool;

    # Groups attributed to TMC; those attributed through a 3A announcement
    # rather than through 8A's default use; those that lost block 3 or 4 and
    # so joined no message; and table entries evicted to stay within the
    # decoder's bound. All cumulative.
    groups @7 :UInt64;
    odaGroups @8 :UInt64;
    incomplete @9 :UInt64;
    evicted @10 :UInt64;

    # Every distinct payload, in no particular order. User messages, tuning
    # information and encryption administration are all here and are not
    # told apart. A payload with receptions of 2 or more is CONFIRMED: ISO
    # 14819-1 Introduction 0.3 has a terminal use a group's data "once it has
    # been verified by the reception of a second identical group". One with
    # a single reception is as likely to be a damaged copy as a message, and a
    # client should show confirmed payloads and at most count the rest.
    messages @11 :List(RdsTmcMessage);
}

# One station's accumulated RDS state, as a display needs it.
#
# NOT EVERY FIELD OF revenant::decode::StationState, and that is the point,
# the same way Detection is not every field of detect::Track. What is left out
# is the decoder's own assembly state: the partially built AF pair, the
# pending LF/MF flag, the leaky-bucket error credit. A schema is a contract
# rather than a mirror.
#
# EVERY FIELD HAS A VALIDITY COMPANION AND A READER MUST CHECK IT. There is no
# sentinel that means "not received" for most of these: PTY 0 is a real
# programme type, TA false is a real state, and a PI of zero is what an
# uninitialised struct holds. The flags are the only honest answer, and a
# client that draws the value without them shows a station that has sent
# nothing as a station announcing no traffic.
struct RdsStation {
    # Echoed so a poll's answer names the receiver it came from, which matters
    # to a client polling several.
    vrx @0 :UInt64;

    # What the decoder was built with. Read back rather than assumed: it is
    # engine-side state and a second client may have set it, the same reason
    # DetectionList reads back detectionThresholdDb.
    region @1 :RdsRegion;

    # Programme Identification. The raw 16 bits, not split into country code,
    # coverage area and reference, because those three are a pure function of
    # this and splitting them here would be three fields that can disagree
    # with the one they came from.
    pi @2 :UInt16;
    piValid @3 :Bool;

    # Derived from the PI under the region's own rules. Empty means NO CALL
    # SIGN IS DERIVABLE from this PI in this region, which is a different
    # thing from "PI not yet received": piValid answers that one. Text and not
    # Data, unlike the four fields below, because callsign_from_pi produces
    # ASCII letters by construction.
    callSign @4 :Text;

    pty @5 :UInt8;
    ptyValid @6 :Bool;

    # The PTY's display name under the configured region, at the widths clause
    # 3.2.1.1 specifies. Carried rather than left to the client because the
    # table is region dependent and half of it differs between RDS and RBDS: a
    # client with one hardcoded table shows the wrong genre on the other
    # continent and nothing faults.
    ptyShortName @7 :Text;   # 8 characters
    ptyLongName @8 :Text;    # 16 characters

    tp @9 :Bool;
    tpValid @10 :Bool;
    ta @11 :Bool;
    taValid @12 :Bool;

    # Composite sample index at which ta last changed, or zero if it never
    # has. THIS IS WHY THIS SURFACE CAN STAY A POLL. Everything else here
    # persists once received, but a traffic announcement is an event: it goes
    # up, runs for seconds, and goes down, and a client polling every few
    # seconds can see false on both sides of one. Differencing this against
    # the previous poll's value says an announcement happened without needing
    # a subscription, a capability and a backpressure rule to catch it.
    taChangedAt @13 :UInt64;

    music @14 :Bool;
    musicValid @15 :Bool;

    # Decoder Identification, clause 3.2.1.5. diReceived has bit n set once
    # d(n) has arrived, because the four flags come one per group over four
    # groups and a client showing "mono" before the bit arrived is showing the
    # struct's default.
    diStereo @16 :Bool;
    diArtificialHead @17 :Bool;
    diCompressed @18 :Bool;
    diDynamicPty @19 :Bool;
    diReceived @20 :UInt8;

    # Programme Service name, eight characters, and RadioText, up to 64.
    #
    # Data AND NOT Text, WHICH IS A CORRECTNESS DECISION AND NOT A STYLE ONE
    #
    # These are EN 50067 Annex E code points, an 8-bit repertoire that is not
    # ASCII above 0x7F and is not UTF-8 anywhere. core/decode/rds_groups.cpp
    # stores the received bytes verbatim and transcodes nothing. Cap'n Proto
    # Text is defined as NUL-terminated UTF-8, so a station transmitting an
    # accented character would put invalid UTF-8 on the wire: the C++ runtime
    # would not notice and a Python or Rust client would fail to decode it or
    # silently replace it. Transcoding belongs where a font is being chosen,
    # which is the client.
    #
    # The masks are not optional extras. PS arrives as four two-character
    # segments and RadioText as sixteen segments, so a partially received one
    # holds real characters beside placeholders, and the placeholders are not
    # distinguishable from transmitted spaces. psReceived has one bit per
    # segment 0..3, rtReceived one bit per segment 0..15. Unreceived RadioText
    # bytes are NUL rather than space, so a client that ignores the mask and
    # treats this as a C string truncates at the first gap.
    ps @21 :Data;
    psReceived @22 :UInt8;

    rt @23 :Data;
    rtReceived @24 :UInt32;

    # Position of the 0x0D terminator once one has arrived, or the highest
    # character index received plus one until then. Carried because the
    # terminator is inside the payload and a client scanning for it cannot
    # tell an unreceived NUL from a short message.
    #
    # rt[0, rtLength) IS WHAT THE STATION HAS SAID SO FAR, AND THAT IS THE
    # WHOLE OF WHAT THIS PROMISES. It is a span to render. It is not a claim
    # about the byte at rtLength and it is not monotonic.
    #
    # THE VALUE MOVES IN BOTH DIRECTIONS WITHIN ONE MESSAGE. The decoder
    # recomputes it on every RadioText group by scanning the assembled buffer
    # for the first 0x0D, so it falls the moment a terminator arrives in
    # front of the highest index already received, and it rises again if the
    # segment carrying that 0x0D is later rewritten with ordinary characters
    # under the same A/B flag. Both are ordinary: segments arrive in whatever
    # order the transmitter sends them and a shorter message with the flag
    # unchanged is a transmitter the standard allows. A client redraws the
    # span it is given rather than keeping the longest one it has seen.
    #
    # WHAT THIS PARAGRAPH USED TO SAY, AND THE SHIPPING DECODER VIOLATES
    # BOTH HALVES OF IT
    #
    # It was headed "ONCE A TERMINATOR HAS ARRIVED THIS DOES NOT GROW PAST
    # IT" and read: "rt[0, rtLength) is the message and rt[rtLength] is the
    # 0x0D itself, so a client renders exactly that span and never scans for
    # the terminator again. Whatever lands at a higher index afterwards is
    # beyond the end of the message". It then said the value "returns to zero
    # only at the start of a NEW message, which is a toggle of rtAb or a
    # change of rtVersionB", and closed by calling all of it an invariant of
    # the wire rather than an implementation note.
    #
    # rt[rtLength] is the 0x0D only when one has arrived. Until then rtLength
    # is the high-water mark, the byte at it is an unreceived NUL, and on a
    # message that fills the field it is one past the end of a 64-byte
    # payload. A client that read the terminator back to confirm the span
    # would have found neither.
    #
    # And it does grow past a terminator, by the overwrite above.
    # core/decode/rds_groups.cpp says so in its own words where it explains
    # why the scan is a scan rather than a remembered index: "a segment can
    # be overwritten: same A/B flag, a shorter message, and the group
    # carrying the 0x0D is rewritten with ordinary characters." The two
    # documents were written in the same pass and only one of them was right.
    #
    # The paragraph was not harmless. Calling it an invariant of the wire is
    # an instruction to build on it, and what it invited is a client that
    # latches rtLength at the first terminator and never reads it again.
    # On a station that sends a long message and then a short one without
    # toggling the flag, that client renders the tail of the long one
    # forever.
    rtLength @25 :UInt32;

    # The A/B flag. A TOGGLE OF THIS IS THE ONLY SIGNAL THAT THE MESSAGE
    # CHANGED, and a client that does not watch it renders one message
    # overwritten character by character by the next. rtVersionB says whether
    # 2A or 2B is carrying it, which decides whether a segment is four
    # characters or two.
    rtAb @26 :Bool;
    rtAbValid @27 :Bool;
    rtVersionB @28 :Bool;

    ptyn @29 :Data;
    ptynReceived @30 :UInt8;
    ptynAb @31 :Bool;
    ptynAbValid @32 :Bool;

    clock @33 :RdsClockTime;

    # Programme Item Number, clause 3.1.5.6.
    pinDay @34 :Int32;
    pinHour @35 :Int32;
    pinMinute @36 :Int32;
    pinValid @37 :Bool;

    ecc @38 :UInt8;
    eccValid @39 :Bool;

    # Raised when the ECC says ITU region 2 and the decoder is configured for
    # RDS. A DIAGNOSTIC FOR AN OPERATOR AND NEVER A SWITCH: the decoder does
    # not change region on its own and neither should a client, because the
    # converse is deliberately not raised. Only the three region 2 allocations
    # were read, so an ECC outside them is absence of evidence rather than
    # evidence of Europe.
    eccContradictsRegion @40 :Bool;

    language @41 :UInt8;
    languageValid @42 :Bool;

    linkageActuator @43 :Bool;
    linkageActuatorValid @44 :Bool;

    # Alternative frequencies, VHF and LF/MF, deduplicated, in hertz.
    #
    # Int64 and not Rational. These are channel-plan frequencies the standard
    # states as whole numbers and the decoder computes as whole numbers, so
    # there is nothing exact being rounded here, the same distinction the note
    # at the top of this file draws for channelSpacing.
    af @45 :List(Int64);

    # From a count code 225..249, so the transmitter's own statement of how
    # many alternatives exist. Zero if none was seen. Comparing this against
    # af.size() is how a client knows the list is still filling.
    afAnnounced @46 :UInt8;

    # How often the first frequency ever seen at the head of a pair reappeared
    # there.
    #
    # AN INDICATOR, NOT A DETERMINATION, and the difference is not subtle.
    # This does NOT say the transmission uses AF method B. A method A list is a
    # flat list sent round and round, so as soon as it wraps whichever
    # frequency happened to land at the head of a pair lands there again and
    # this counts it, within a second on a short list. And the frequency
    # tracked is not known to be the tuning frequency: the decoder is never
    # told what the radio is tuned to. A nonzero count is consistent with both
    # methods; only a count that stays at zero says anything, and what it says
    # is that the list has not wrapped yet. A client that knows what it tuned
    # can make the determination from the af list and its own knowledge.
    afRepeats @47 :UInt32;

    oda @48 :List(RdsOda);
    eon @49 :List(RdsEonEntry);

    health @50 :RdsHealth;

    # The composite rate this station is being decoded at, which is the
    # receiver's audio rate. Carried so the two sample indices have a stated
    # unit, and so a client can see at a glance that the receiver is
    # delivering a composite rather than audio.
    compositeRate @51 :UInt32;

    # Composite sample index at which the most recently completed group
    # finished, or zero if none has. This is the equivalent of
    # DetectionList::lastDecision and it is here for the same reason: it lets a
    # client tell a repeated answer from a fresh one without diffing the whole
    # struct, and it gives the denominator for differencing the cumulative
    # counters in health between two polls.
    #
    # NOT a source sample index, unlike Detection::firstSeen. The decoder
    # counts in the samples it is fed and has no access to the source
    # timeline, and converting here would mean the engine asserting a ratio
    # the decoder never saw. Divide by compositeRate for seconds.
    lastGroupSample @52 :UInt64;

    # Empty while the decoder is running. Non-empty is the sentence saying
    # why it stopped, and everything above it is frozen at the last chunk it
    # accepted.
    #
    # A DECODER CAN STOP WITHOUT THE RECEIVER GOING, which is the state this
    # field exists to name. The sink must not fail the graph's dispatch, so a
    # chunk that is not what the decoder was built for, an interleaved pair
    # or a rate the loops are not sized for, is recorded here instead of
    # returned. Both are shape, and setVrxParams refuses a change of shape
    # rather than applying it, so a receiver that delivered the wrong thing
    # once will deliver it again: this is TERMINAL for the life of the
    # receiver. Removing it and adding another is the only recovery, and
    # nothing here pretends otherwise.
    #
    # WHAT THIS CALL USED TO DO INSTEAD, because a client written against the
    # old behaviour has a branch it can now delete. A faulted decoder made
    # rdsStation THROW, with this sentence as the exception's message. That
    # told a client why, and it threw away everything the decoder had
    # accumulated before the bad chunk, which is still true about the
    # station; it also made "the decoder stopped" and "no such receiver"
    # arrive on the same channel, so telling them apart meant matching prose.
    # The call answers now and this field is the branch.
    #
    # It is the one place RDS departs from the detector's precedent, which
    # refuses on detectorFault and always will: a detector fault means the
    # track list no longer describes anything, where an RDS fault leaves a
    # station struct that was true when it was last written.
    fault @53 :Text;

    # THE RETUNE FENCE, AS A STATE A CLIENT CAN SEE.
    #
    # setVrxParams only queues the retune, so the server clears this decoder
    # and then discards every chunk the old tuning already produced. While
    # that is happening the decoder is fed nothing, and nothing else in this
    # struct says so: the counters are frozen, fault is empty, and what a
    # client reads is exactly what a receiver pointed at a quiet channel
    # reads. This is the field that tells the two apart.
    #
    # True means the sample path has not yet delivered a chunk from the
    # tuning the client last asked for, so the zeros above are the fence
    # rather than the band. It clears on its own, within about a block
    # period, when the first chunk of the new tuning arrives. A client
    # showing "no RDS" while this is true is showing the wrong thing;
    # "retuning" is the honest label.
    #
    # WHY IT IS HERE AT ALL. A fence built by counting the retunes asked for
    # on one side and the changes of tuning observed on the other could get
    # permanently stuck, and did: two retunes applied in the same control
    # drain move the tuning once, so the count never came back down and the
    # decoder discarded for the rest of the receiver's life with nothing
    # anywhere saying why. The fence is now a comparison and cannot stick,
    # and this field exists so that the next thing that goes wrong in here
    # is visible on the first poll rather than mistaken for dead air.
    discarding @54 :Bool;

    # Chunks the fence has thrown away, cumulative from the moment this
    # decoder was built, like every counter in health. A client differencing
    # two polls sees how much composite a retune cost it; a client that
    # finds this climbing while discarding stays true is looking at a fence
    # that is not clearing, which is a defect in the server and not in the
    # signal.
    discardedChunks @55 :UInt64;

    # One bit per PTYN segment, set when a block the segment depends on was
    # corrected and cleared by a clean reception of the whole segment. A set
    # bit means the four characters may not be the station's: render them
    # marked, or hold the segment until it arrives clean.
    ptynCorrected @56 :UInt8;

    # Traffic Message Channel. See RdsTmc for why it is counts and raw
    # payloads rather than events and locations.
    tmc @57 :RdsTmc;

    # Emergency Warning System, type 9A. EN 50067 clause 3.1.5.13 says these
    # groups are transmitted "very infrequently, unless an emergency occurs or
    # test transmissions are required", so ews.groups above zero is the fact
    # to surface, prominently: this station has sent an emergency warning
    # group, or a test of one. The payload is each country's own format.
    ews @58 :RdsRawGroup;

    # Type 1A variant 7, "Identification of EWS channel", twelve bits, raw.
    ewsChannelIdentification @59 :UInt16;
    ewsChannelIdentificationValid @60 :Bool;
}

# ---------------------------------------------------------------------------
# Decoded messages
# ---------------------------------------------------------------------------
#
# ONE SHAPE FOR EVERY DECODER THAT PRODUCES EVENTS, so that a decoder added
# later reports through this without growing the schema. P25 Phase 1, D-STAR
# and TETRA report through it first; RTTY, APRS, POCSAG, PSK31, CW and M17 are
# written as pure libraries and each needs an adapter in core/rpc/decoders.h
# and nothing here.
#
# WHY RDS DOES NOT MOVE ONTO IT. An RDS station is a STATE: PS, RadioText and
# the AF list accumulate over many groups and a client polls the whole of it.
# What these decoders produce is a stream of EVENTS, one per frame or header
# or burst, and an event polled is an event missed. So the two surfaces have
# different shapes on purpose and RdsStation stays as it is.
#
# WHY KEY AND VALUE RATHER THAN A STRUCT PER MODE. A struct per mode is a
# schema change, a types.h mirror, a conversion and a client change for every
# decoder, and a client built before a mode existed could not show it at all.
# Typed values keep the half a struct would have bought: a talkgroup is an
# integer on the wire and never a string a client has to parse back. The keys
# each decoder emits are listed beside its adapter in core/rpc/decoders.h, and
# a key once published is not renamed, on the ground a field ordinal is not
# renumbered.

struct DecodedField {
    key @0 :Text;
    value :union {
        integer @1 :Int64;
        real @2 :Float64;
        flag @3 :Bool;
        text @4 :Text;

        # What a transmitter sent, where the standard does not say it is
        # ASCII. A client decides how to show it, on RdsStation::ps's argument.
        bytes @5 :Data;
    }
}

struct DecodedMessage {
    vrx @0 :UInt64;

    # The decoder's registry name and the kind of message within it, both
    # lower case and stable: "p25p1" and "hdu", "dstar" and "header".
    decoder @1 :Text;
    kind @2 :Text;

    # [startSample, endSample) in the receiver's own stream at sampleRate, the
    # frame AudioChunk::sampleIndex counts in. It is the span of the delivery
    # that COMPLETED the message, so it bounds when the message ended to
    # within one chunk and says nothing about where it began. Sample indices
    # and not wall clock, per docs/conventions.md.
    startSample @3 :UInt64;
    endSample @4 :UInt64;
    sampleRate @5 :UInt32;

    fields @6 :List(DecodedField);

    # One line for a person, or empty. Never parsed.
    text @7 :Text;

    # Messages this decoder produced before this one, counted per decoder and
    # not per subscription, so two subscribers see the same numbers.
    sequence @8 :UInt64;

    # Messages THIS subscription lost from its queue between the previous one
    # it was sent and this one. A message is the only copy of an event, so a
    # subscription queues, as audio does, and evicts the oldest when full.
    droppedBefore @9 :UInt64;
}

enum DecoderInput {
    # A receiver whose mode is a complex tap: raw, p25p1, dstar or tetra.
    complexBaseband @0;

    # A receiver producing real audio: every other mode.
    realAudio @1;
}

struct DecoderInfo {
    name @0 :Text;
    input @1 :DecoderInput;
    description @2 :Text;

    # The receiver modes this decoder reads, by the names Demod's enumerants
    # carry in lower case: "usb", "lsb", "nfm", "p25p1", "raw". Always the
    # whole list, so a decoder that reads any complex tap names all four of
    # them rather than sending nothing for "any".
    #
    # Added on 2026-09-23 for a client that offers an operator only the
    # decoders a receiver can feed. The description says the same in words,
    # and a client that has to parse prose to fill a menu is the failure the
    # typed fields on DecodedMessage exist to prevent. An engine older than
    # this field sends an empty list, which a client reads as "not stated",
    # never as "reads nothing".
    modes @3 :List(Text);
}

struct DecodedStats {
    messagesSent @0 :UInt64;
    messagesDropped @1 :UInt64;
    backlog @2 :UInt64;

    # Chunks the decoder discarded because a retune of its receiver was still
    # in flight, on the fence RdsStation::discarding describes. Per decoder
    # and so shared by every subscriber to it.
    chunksDiscarded @3 :UInt64;

    # The rate the decoder was built for, zero until the first chunk arrived.
    sampleRate @4 :UInt32;
}

interface DecodedReceiver {
    message @0 (message :DecodedMessage) -> ();

    # No further message will arrive, and why. Called at most once and never
    # for a cancel this client asked for. Three things send it: the receiver
    # being removed, by anybody or by its session ending; the decoder
    # refusing what the receiver delivers, which is terminal for that
    # decoder on that receiver; and a message() call on this capability
    # coming back failed. Best effort, on the terms AudioReceiver::ended
    # states.
    ended @1 (reason :Text) -> ();
}

# Dropping this ends the subscription, and the decoder with it once nobody
# else is subscribed to the same decoder on the same receiver.
interface DecodedSubscription {
    cancel @0 () -> ();
    stats @1 () -> (stats :DecodedStats);
}

# ---------------------------------------------------------------------------
# The root capability
# ---------------------------------------------------------------------------

# The only thing a connection holds before it has proved it may drive the
# radio.
#
# WHY AN INTERFACE HERE AND NOT A CHECK IN EVERY METHOD
#
# Cap'n Proto hands the root capability to whoever opens the connection. Until
# 2026-09-20 that root was Session itself, so opening the port was the whole
# of the authorisation. A login method ON Session, refusing every other call
# until it had succeeded, would also work and is the wrong shape: it puts a
# test in front of every method added from now on, and the test is only as
# good as the next author remembering to write it.
#
# So the root holds one method and no radio. A caller that has not called
# login holds an Authenticator and nothing else. There is no Session in its
# capability table to call, refused or otherwise, which is a different thing
# from a Session that says no. Capability discipline carries the rest: a
# Session reached through login is an ordinary capability, and the caller may
# keep it, drop it, or pass it on, including dropping this Authenticator the
# moment it has served its purpose.
#
# PIPELINING IS PART OF THE CONTRACT, NOT AN OPTIMISATION
#
# A client may send login and then send Session calls on the returned
# capability without waiting for the first to come back, which is what Cap'n
# Proto is for and what core/rpc/client.cpp does. Nothing leaks by allowing
# it. When login fails, the pipelined capability is broken with login's own
# exception, and every call made on it fails with that exception without the
# engine's Session implementation being entered. The first thing a rejected
# caller learns is the rejection, whatever it sent behind it.
#
# WHY THERE IS NOTHING ELSE ON THIS INTERFACE
#
# No version, no banner, no engine name. Every method here is a method an
# unauthenticated caller can reach, and a version string is precisely what a
# scanner is looking for. A caller already knows it reached a Cap'n Proto
# server from the handshake, so a banner would give away something for
# nothing.
#
# WHAT AUTHENTICATION DOES AND DOES NOT CHANGE
#
# It changes what a second process on this machine can do. It changes nothing
# at all about a LAN: this wire is plaintext, TLS was refused as the wrong
# shape for one operator with one radio, and a token crossing a routable
# interface is readable and replayable by anything on the path. The bind
# default stays loopback for that reason and not out of caution.
interface Authenticator {
    # The engine's pre-shared token, as the raw bytes.
    #
    # Data and not Text, although the token file holds printable hex so that a
    # person can select and paste it. Hex on the wire would put two encodings
    # of one secret into the protocol and would make "wrong token" and "wrong
    # encoding" the same failure, at the one place that must not be ambiguous.
    # The client decodes once on its side; the engine decodes its file once at
    # startup.
    #
    # Thirty-two bytes. An unset field arrives as zero bytes and is refused
    # like any other wrong token, and it is a different input from thirty-two
    # zero bytes, which is also refused. The length is not a secret: it is
    # stated here, in docs/rpc.md, and in the size of the token file. So the
    # refusal says the token was rejected and nothing more, not because the
    # length is hidden but because there is nothing else true to say.
    #
    # The comparison is constant-time over the fixed length, after a length
    # check. A compare that returned on the first wrong byte would leak the
    # PREFIX, which turns a 2^256 search into a 32 x 256 one. Whether that is
    # measurable across loopback is another question and docs/rpc.md answers
    # it honestly; the compare costs three lines and the argument does not
    # have to be had again.
    #
    # Refusal is an exception, never a null session, so a caller cannot
    # mistake a refusal for an engine with nothing to give.
    #
    # A second login on a connection that already holds a Session succeeds and
    # returns another, independent one. The caller has already proved it holds
    # the token and a second capability grants it nothing it did not have.
    # Dropping either Session leaves the other and its subscriptions alone.
    login @0 (token :Data) -> (session :Session);
}

interface Session {
    # THIS INTERFACE IS NO LONGER THE ROOT. Until 2026-09-20 it was: the
    # bootstrap capability Cap'n Proto handed to whoever opened the connection
    # was a Session, and docs/rpc.md said in as many words that there was no
    # authentication of any kind and that the loopback default was the whole
    # of the control. A client now reaches this through Authenticator.login
    # above, and holds nothing until that call returns.
    #
    # Recorded here rather than quietly swapped, because the old arrangement
    # is the one an experienced reader expects from a Cap'n Proto service and
    # because anything written against "bootstrap() is the Session" is owed
    # the retraction. core/rpc/client.cpp cast the bootstrap straight to this
    # interface, core/rpc/server.cpp handed one out, and core/rpc/server.h and
    # docs/rpc.md both stated the absence as settled. All four have been
    # corrected.
    #
    # Nothing about the methods below changed. Their ordinals are untouched,
    # and a Session obtained through login behaves exactly as the bootstrap
    # Session did.
    info @0 () -> (info :EngineInfo);
    running @1 () -> (running :Bool);
    listSources @2 () -> (sources :List(SourceDescriptor));
    sourceStats @3 () -> (stats :SourceStats);

    # A receiver BELONGS TO THE SESSION THAT CREATED IT and is removed when
    # that session ends, which is the owner decision recorded on 2026-09-22.
    # Ending is dropping this Session capability or losing the connection it
    # travelled on, a crash included. The removal goes through the same path
    # removeVrx does, so an audio subscriber on the receiver is sent ended()
    # with the reason and an RDS decoder goes with it.
    #
    # keep asks for the other lifetime: the receiver survives its creator and
    # stays until somebody calls removeVrx. It exists for a headless recorder,
    # whose receivers must outlive a client restarting, and it is a field on
    # this call rather than a separate one so that no receiver is ever briefly
    # owned by a session that is about to let go of it. Appended to the
    # parameters, so an older client sends false and gets the session
    # lifetime.
    #
    # Nothing stops one session removing or retuning another's receiver. The
    # lifetime rule decides what happens when a session ENDS, and a receiver
    # is engine-wide state in every other respect, for the reason
    # setRdsRegion states.
    addVrx @4 (params :VrxParams, keep :Bool) -> (id :UInt64);
    removeVrx @5 (id :UInt64) -> ();
    setVrxParams @6 (id :UInt64, params :VrxParams) -> ();
    vrxStatus @7 (id :UInt64) -> (status :VrxStatus);
    vrxIds @8 () -> (ids :List(UInt64));

    # everyNth of 0 or 1 is every frame. A display asking for 30 a second
    # from a 305 a second engine passes 10 and the engine drops the rest
    # before they are copied, which is the point: decimating on the client
    # would have paid for the copy already.
    subscribeSpectrum @9 (receiver :SpectrumReceiver, everyNth :UInt32)
        -> (subscription :SpectrumSubscription);

    # What the wideband detector is tracking right now.
    #
    # A poll rather than a subscription, which is the opposite of the choice
    # made for spectrum frames one method above, so the difference is worth
    # stating. A spectrum frame is produced whether anyone looks or not and is
    # worthless a fraction of a second later, so a poll would either lag the
    # frame rate or tear a frame. A track list is a STATE: it changes ten
    # times a second at the detector's default decision interval, an older one
    # is of no use, and a display redraws it on its own timer anyway. Polling
    # a state costs one small message per poll and needs no capability, no
    # backpressure rule and no fan-out.
    #
    # THE FIRST CALL IS WHAT STARTS THE DETECTOR
    #
    # Nothing runs a detector until somebody asks for one, the same way the
    # server does not start its source-enumeration thread until the first
    # listSources. The detector costs real CPU on the engine's completion
    # thread for every frame once it exists, and a headless engine serving a
    # recorder should not pay it. So the first call installs the spectrum sink
    # if it is not installed, builds the detector, and answers with zero
    # decisions: it cannot answer otherwise, because the detector has seen no
    # frames yet. A client polls again.
    #
    # It is refused, with the engine's own words, on an engine built with no
    # spectrum stage. The detector works on spectrum frames and there are
    # none, which is a different thing from a quiet band.
    #
    # minConfidence is THIS CALLER'S bar and is not stored anywhere. Two
    # displays with different thresholds get different lists from one
    # detector, which is what docs/detection.md means by the confidence
    # threshold belonging to the display: nothing in the detector drops a
    # track for failing it. Zero passes everything the detector holds.
    #
    # A bar of one, or above, is refused rather than answered with an empty
    # list. A track's confidence rises by a fraction of its remaining distance
    # to one, so it approaches one and never arrives, and a bar of one lists
    # nothing however strong the signal is. An empty list is also what a dead
    # band looks like, so the failure would be invisible.
    # minMargin is the same idea against Detection::marginConfidence, which is
    # the calibrated number, and it is the bar an operator watching
    # interference actually wants. Zero passes everything, and so does
    # anything up to a half: every published detection cleared the detection
    # threshold and the margin map is a half AT the threshold, so the useful
    # range is a half to one.
    #
    # BOTH BARS APPLY AND NEITHER REPLACES THE OTHER. They are independent
    # questions: confidence is how long a track has been there and the margin
    # is how far it stood above the noise, so a list can be asked for things
    # that are both persistent and strong, or either alone. A caller wanting
    # one passes zero for the other.
    #
    # Refused at one and above on the same grounds as minConfidence. The map
    # is 1 - 0.5*exp(-(margin - threshold)/6), which approaches one without
    # arriving, so a bar of one lists nothing however strong the signal is and
    # an empty list is also what a dead band looks like.
    detections @10 (minConfidence :Float64, minMargin :Float64)
        -> (detections :DetectionList);

    # The detector's other knob, in dB of SNR in the 2500 Hz reference
    # bandwidth. docs/detection.md: both thresholds belong to the operator,
    # and neither has a right value that suits a quiet VHF band and an HF
    # evening at the same time.
    #
    # Unlike minConfidence this is engine-wide, because it changes what the
    # detector FINDS rather than what a caller is shown, and there is one
    # detector. Two clients setting it fight, and the loser finds out by
    # reading DetectionList::detectionThresholdDb back.
    #
    # It builds the detector on first use, exactly as detections does, so a
    # client may set a threshold before it polls rather than having to poll
    # once at the wrong one.
    setDetectionThreshold @11 (thresholdDb :Float64) -> ();

    # One receiver's passband, on the same terms as subscribeSpectrum above:
    # everyNth of 0 or 1 is every frame, dropping the capability ends it, and
    # a frame is skipped rather than queued when the previous one has not
    # been answered.
    #
    # PER RECEIVER AND OPT IN. Until something subscribes, that receiver
    # records no transform and holds no buffers for one, which is why this
    # takes a receiver id rather than being a flag on addVrx: a recorder
    # serving eight receivers should not pay for eight transforms nobody is
    # looking at. The last subscription going away takes the buffers with it.
    #
    # Refused on an engine built with no passband stage, and refused for a
    # raw tap, which has no fine stage to transform. The engine's own words
    # come back in both cases.
    subscribePassband @12 (vrx :UInt64, receiver :PassbandReceiver, everyNth :UInt32)
        -> (subscription :PassbandSubscription);

    # One receiver's audio, as raw float32 PCM. Dropping the capability ends
    # it, cancel ends it explicitly, and the engine sink behind it is
    # refcounted per receiver exactly as subscribePassband's is.
    #
    # SERVED SINCE 2026-09-20, and this paragraph used to say it was not: "the
    # engine has one AudioSink slot per receiver and no fan-out, so the wire
    # half cannot be built without changing the engine, and that is its own
    # lane." The engine was changed. core/engine/engine.h now carries
    # AudioFanout and Engine::attach_audio_sink, so a receiver feeds a
    # recording, a loudspeaker and any number of subscriptions at once, and
    # the slot the old paragraph describes is still there underneath as the
    # thing the fan-out installs itself into.
    #
    # THERE IS NO everyNth, which is the one place this departs from the two
    # subscriptions above it, and the departure is the point. Dropping every
    # other spectrum frame halves an update rate and loses nothing anyone
    # wanted. Dropping every other audio chunk is a 50 percent duty cycle of
    # silence. A client that wants less audio subscribes to fewer receivers.
    #
    # PER RECEIVER AND OPT IN, like subscribePassband: nothing crosses until
    # something subscribes and the last subscription going away takes the sink
    # off. Unlike the passband there is no device memory behind it, so what is
    # avoided is the wire's cost rather than the GPU's.
    #
    # bufferMillis IS THE WHOLE OF THE BACKPRESSURE RULE
    #
    # It is how much audio the engine holds for this subscription before it
    # starts discarding, and it replaces the one-frame-in-flight rule the two
    # subscriptions above use. That rule is right for a display, where the
    # newest frame is the one worth drawing and an older one is a redundant
    # measurement of a band that is still there. It is wrong here: an audio
    # chunk is the only copy of that instant and the newest one is worth no
    # more than the one before it.
    #
    # Zero asks for the default, 500. The queue evicts from the FRONT, the
    # oldest chunk not yet sent, for the reason core/engine/audio_wasapi.h
    # gives about its own backlog: late audio is worse than no audio when the
    # point is to hear what the radio is doing now. Front eviction is also
    # what makes AudioChunk::framesDroppedBefore exact rather than
    # approximate, because the frames it discards lie precisely between the
    # last chunk sent and the next one.
    #
    # THE DEPTH IS CLAMPED IN MILLISECONDS HERE AND IN FRAMES LATER
    #
    # bufferMillis is clamped to 20..5000 and bufferMillisGranted reports what
    # is in force, on the EngineInfo::ringClamped precedent: a depth that was
    # quietly changed is a dropout nobody can trace. Zero means the default,
    # 500, and comes back as 500 rather than as zero.
    #
    # A depth under two chunks evicts every chunk before the loop thread can
    # send it: the client hears nothing at all, the counters climb at the full
    # sample rate, and every other check still passes. So two chunks is a
    # floor as well, it is a clamp rather than advice, and it is applied in
    # FRAMES when the first chunk arrives. AudioStats::bufferFrames is where a
    # client reads the depth that is actually enforcing, and it is zero until
    # the first chunk has arrived to set it.
    #
    # The floor went unexercised until 2026-09-20, which is worth recording
    # because this paragraph is the whole reason the retraction below exists.
    # Reaching it needs a chunk longer than the shortest depth a client can
    # ask for, and at the test fixture's 16384-sample blocks two chunks are
    # 13.7 ms against a 20 ms floor, so every case in the suite read the
    # millisecond figure straight back out and none of them touched this.
    # tests/rpc/test_rpc_audio.cpp now runs one at 32768-sample blocks, where
    # two chunks are 27.3 ms and the floor is what is enforced.
    #
    # WHAT THIS PARAGRAPH USED TO SAY, AND WHAT THE CODE PROVED WRONG. Until
    # 2026-09-20 it read: "Two chunks can therefore exceed the 5000 ms
    # ceiling, so the effective maximum is 5000 or two chunks, whichever is
    # larger. On a slow source the DEFAULT is clamped up as well, which a
    # client did not ask for and has to be told about." That put the two-chunk
    # floor in bufferMillisGranted, and the server cannot compute it when it
    # answers. It needs two numbers it does not have. A chunk is
    # block_samples / sourceRate long and EngineInfo carries sourceRate and
    # not block_samples. Converting either to frames needs the receiver's
    # audio rate, and VrxParams::audioRate is a verbatim echo that reads zero
    # for every receiver that took the engine's default, which EngineInfo does
    # not carry either. Both are known the instant the first chunk arrives,
    # which is why the floor moved there and why bufferFrames is on AudioStats
    # in frames.
    #
    # The millisecond figure is still worth reporting and is still a clamp a
    # client is told about; it is no longer the whole of the answer, and a
    # client that needs the whole of it reads bufferFrames.
    #
    # Refused, with the engine's own words, for a receiver that does not
    # exist, on an engine whose source is not open, and for a raw tap. The raw
    # tap is refused because RawTapStage hands back interleaved complex I/Q at
    # the coarse channel rate rather than audio. A client playing that as
    # two-channel PCM plays noise at the wrong speed, and the rate is tens of
    # times the 1.5 Mbit/s this design was costed at. An I/Q subscription is a
    # separate method that does not exist yet, and putting it behind this name
    # is the only thing that would make it look like one.
    #
    # The three digital voice modes are refused as well, in words of their
    # own. Since 2026-09-22 they are not raw taps: they go through the fine
    # stage and come out as complex baseband mixed to DC at their decoder's
    # rate, which VrxStatus::demodRate states, and subscribeDecoded is what
    # reads that stream. The refusal names the rate and points there.
    subscribeAudio @13 (vrx :UInt64, receiver :AudioReceiver,
                        bufferMillis :UInt32)
        -> (subscription :AudioSubscription, bufferMillisGranted :UInt32);

    # What the RDS decoder on one receiver has accumulated.
    #
    # THIS USED TO SAY "NOT SERVED YET, on the same terms subscribeAudio
    # states. The decoder exists in core/decode and nothing in core/engine
    # feeds it; wiring it in is its own lane. Refused, naming the surface."
    # It is served as of 2026-09-20, and nothing in core/engine feeds it
    # still: core/rpc/server.cpp holds the decoder and joins it to the
    # receiver's audio fan-out, which is not the lane that paragraph
    # expected and is the one core/engine/vrx.h argued for.
    #
    # A poll rather than a subscription, on exactly the argument
    # Session::detections makes above and for the same reason a track list is
    # polled: this is a STATE. PI, PS, RadioText, PTY and the AF list are
    # accumulated and retained, a group completes at most every 87.6 ms, an
    # older snapshot is of no use, and a display redraws on its own timer.
    # Polling a state costs one small message per poll and needs no
    # capability, no backpressure rule and no fan-out.
    #
    # The one field with an event character is TA, and it is answered with
    # RdsStation::taChangedAt rather than with a subscription. A traffic
    # announcement that began and ended between two polls leaves a changed
    # index behind it; nothing else here can be missed by polling slowly, only
    # seen late.
    #
    # THE FIRST CALL IS WHAT STARTS THE DECODER, the same way the first
    # detections call builds the detector and the first listSources starts the
    # enumeration thread. It joins that receiver's audio fan-out, builds
    # the physical and group layers, and answers with an unlocked decoder and
    # zero groups, because it cannot answer otherwise. A client polls again.
    # From then on the decoder runs on the engine's completion thread for the
    # life of that receiver whether or not anyone is still reading, for the
    # reason the detector does: tearing it down at the last reader would throw
    # away accumulated station state that the disconnecting client does not
    # own.
    #
    # WHAT THIS PARAGRAPH USED TO SAY, AND IT WAS TRUE WHEN IT WAS WRITTEN
    #
    # It was headed "IT TAKES THE RECEIVER'S AUDIO SINK" and read: "There is
    # one sink per receiver and setting a second replaces the first, so this
    # call is refused on a receiver that already has one rather than
    # displacing it."
    #
    # It does not take the sink. Engine::attach_audio_sink landed on
    # 2026-09-20 with AudioFanout behind it, so this call JOINS whatever is
    # already listening: a recording, a loudspeaker, another client's
    # subscribeAudio. Nothing is displaced and nothing is refused on that
    # ground. The retraction matters because that sentence was an
    # instruction to a client as much as a description, and a client that
    # believed it would have torn its own audio subscription down first.
    #
    # Still point it at a receiver created for the purpose: demod = wfm,
    # audioRate = 171000, on the station's frequency. The four conditions in
    # the RDS section above are what the refusal checks and it names which
    # one failed. A listening receiver fails the third of them, so the
    # dedicated receiver is a consequence of the RATE rather than of the
    # sink.
    #
    # A REMOVED RECEIVER TAKES ITS DECODER WITH IT. This is a poll, so there
    # is nothing to notify: the next call answers with the engine's own "no
    # receiver N is registered" and the decoder is dropped there, whether the
    # removal went through removeVrx or happened some other way. What is not
    # kept is the accumulated state, so a client that wants the last reading
    # of a station it removed has to have kept it.
    #
    # A RETUNE CLEARS IT. setVrxParams on a receiver with a decoder resets
    # that decoder, because a receiver that moved is pointed at a different
    # transmitter and PS, RadioText, the AF list and the PI belong to the one
    # it left. Every retune and not only one that moved the centre: the
    # server is handed a whole VrxParams and cannot tell a wider filter from
    # a hundred kilohertz away without keeping a copy that could go stale.
    #
    # AND IT CLEARS THE STREAM AND NOT ONLY THE STRUCT, which is a distinct
    # promise and was not kept until 2026-09-20. A retune is queued to the
    # engine's recording thread and applied at a block boundary, so the
    # frames already recorded carry the OLD tuning and arrive after the
    # reset. Those are discarded now rather than decoded, so the first
    # samples this decoder sees after a retune are the first samples of the
    # station it was moved to. Before, they were the station it left,
    # arriving into a freshly cleared decoder and reading as the new one.
    #
    # What that costs a client is the reacquisition it was already paying,
    # plus up to the engine's pipeline depth of composite. Nothing in the
    # counters says which chunks went: samplesConsumed starts from the first
    # one kept.
    rdsStation @14 (vrx :UInt64) -> (station :RdsStation);

    # The region the decoder for this receiver uses, and it defaults to rds.
    #
    # Settable before the first rdsStation call, so a client does not have to
    # poll once at the wrong region and then correct it, which is the same
    # arrangement setDetectionThreshold has with detections. It BUILDS the
    # decoder when there is not one rather than recording a preference for
    # later, so the four conditions are checked on whichever of these two
    # methods a client calls first.
    #
    # Called on a receiver whose decoder already exists, it rebuilds it and
    # clears the accumulated state: the PTY table and the call sign
    # derivation are both region dependent, so keeping the old state would
    # mix two readings of the same bytes in one struct. That reset reaches
    # the PHYSICAL layer as well, which the region does not touch, and costs
    # about half a second of reacquisition. It is done anyway because every
    # counter in RdsHealth is cumulative from the moment the decoder was
    # built: clearing one layer and not the other would leave one struct
    # holding two epochs, and a client differencing two polls across the
    # change would divide one layer's delta by the other's elapsed samples.
    #
    # This paragraph used to end at "in one struct", which said what was
    # cleared and not how far the clearing reached.
    #
    # Per receiver where setDetectionThreshold is per engine. Both are as
    # wide as the thing they configure: there is one detector, and there is
    # one decoder per receiver. Two receivers can sit on two stations and
    # there is no reason in the standard why they share a continent.
    #
    # AND PER RECEIVER MEANS SHARED BY EVERY SESSION ON THAT RECEIVER, which
    # is the half "per receiver" does not say and which one client can feel
    # through another. Two sessions polling one receiver hold one decoder
    # between them, so this call from either clears what the other had
    # accumulated, with no notification and nothing to tell it from a station
    # that went off the air.
    #
    # That is deliberate and it is the same answer setDetectionThreshold
    # gives, for the same reason. A receiver is engine-wide state: two
    # sessions on one already share its centre, its filter and its squelch,
    # and setVrxParams from either already clears the other's station. The
    # decoder hangs off the receiver, so it is as shared as the receiver is.
    # A per-session decoder would mean one decode per client per receiver on
    # the engine's completion thread, which is the cost that "a receiver
    # nobody asked about runs no decoder" exists to hold down, and it would
    # let two clients disagree about a station that is one station.
    #
    # A client that needs a region of its own creates a receiver of its own.
    # It is already creating a dedicated one to reach 171000, so this asks
    # for nothing it was not already doing.
    setRdsRegion @15 (vrx :UInt64, region :RdsRegion) -> ();

    # Points the front end somewhere else, and answers with the centre the
    # source actually took, which a synthesiser with a tuning step will
    # round.
    #
    # THE BIGGEST USABILITY GAP THIS SURFACE HAD. Until 2026-09-20 the
    # source's centre was fixed by the engine's command line at launch, so
    # every change of band was a process restart: the operator's receivers,
    # the waterfall's history and the audio all went with it. That was
    # tolerable while the only source was a file, whose centre genuinely is
    # a property of bytes on disk, and stopped being tolerable the first
    # time a real radio was on the other end.
    #
    # WHAT MOVES. The device's own oscillator, EngineInfo::sourceCenter, and
    # every receiver's baseband offset, which the engine rebases so the
    # receiver stays on the absolute frequency it was tuned to. A receiver
    # whose CENTRE then falls outside the new span is removed.
    #
    # WHAT THIS PARAGRAPH USED TO SAY, and it has been false since the rebase
    # shipped on 2026-09-21: "nothing about a receiver's placement, filter,
    # audio stream or subscription changes. A receiver sitting at baseband
    # +300 kHz is still at +300 kHz and is now hearing a different piece of
    # spectrum." The operator who retuned from broadcast FM and heard their
    # receiver carry on at a frequency they had not chosen is why it changed.
    #
    # removed LISTS EVERY RECEIVER THE RETUNE TOOK, with the frequency each
    # was on. Before 2026-09-23 nothing reported one: an audio subscriber on
    # it got no ended() and went quiet, and vrxIds changing was the only
    # trace. The server now ends every audio and decoder subscription on a
    # removed receiver with a reason naming the retune, drops its RDS decoder
    # and its ownership record, all before this answers. Empty from a server
    # built before the field existed, which removed receivers just the same.
    #
    # EVERY ABSOLUTE FREQUENCY A CLIENT IS HOLDING THAT IS NOT A RECEIVER'S IS
    # STALE, and that is the trap. A Detection's centerHz is absolute and
    # belongs to the band that was left; a label a client computed by adding
    # sourceCenter to a bin is wrong by the retune. Read info() again after
    # this returns rather than adding the delta, because the delta is not
    # what was asked for: the answer here is where the device landed.
    #
    # WHAT THE ENGINE DOES NOT DO. The device ring still holds samples
    # captured at the old centre, and they are left alone: it is a streaming
    # window rather than a cache, nothing reads behind the write cursor but
    # the channelizer's own filter support, and renumbering the stream to
    # discard them would break the absolute sample index every chunk, frame
    # and recording is correlated against, to avoid a transient of tens of
    # milliseconds. The spectrum's colour map is not reset either: it tracks
    # percentiles over about thirty seconds and recovers on its own, where a
    # reset would make both ends jump after every small retune.
    #
    # WHAT THE SERVER DOES DO, BECAUSE IT WOULD OTHERWISE PUBLISH ONE BAND'S
    # MEASUREMENTS UNDER ANOTHER'S NAME. The wideband detector is dropped,
    # so the next detections call rebuilds it at the new centre with no
    # tracks; every track it held was measured against the old constant and
    # describes a band that is no longer there. Every RDS decoder is cleared
    # and fenced exactly as setVrxParams clears it, because each receiver is
    # now pointed at a different transmitter and PS, RadioText and the AF
    # list belong to the one it left.
    #
    # REFUSED, IN THE SOURCE'S OWN WORDS, on a source that cannot retune,
    # which is every file and every synthetic scene. Each of them says what
    # to do instead and the three answers differ: reopen the URI with
    # another center=, or move the emitters with span_low and span_high, or
    # pick a frequency the dongle reaches. A refusal composed here would
    # replace all three with a category. Call sourceCanRetune first and grey
    # the control out rather than offering one that always refuses.
    setSourceCenter @16 (centerHz :Int64) -> (grantedHz :Int64, removed :List(RetuneRemoval));

    # Whether the surface above will work, and the range it will work over.
    #
    # ONE RANGE AND NOT THE DEVICE'S LIST, WHICH MAKES IT AN ENVELOPE AND
    # NOT A PROMISE. An E4000 reaches 52 to 2200 MHz with a gap in the
    # middle, and this reports the outer pair. A frequency inside the gap is
    # still refused, by the source and in the source's own words. The honest
    # reading is "outside this, do not bother asking".
    #
    # canRetune false comes with lowHz and highHz at zero, and a client
    # greys the control out rather than discovering the refusal by making
    # the operator try. That is the whole reason this exists beside a call
    # that already refuses cleanly: a refusal is the right answer to a
    # request, and a control that can never work should not be offered.
    sourceCanRetune @17 () -> (canRetune :Bool, lowHz :Int64, highHz :Int64);

    # Opens a source on an engine that has none.
    #
    # THE ENTRY IN docs/rpc.md USED TO READ: "What is still absent is opening,
    # starting and stopping a source. Those are the host process's business and
    # an engine owns one source for its life, for the reason
    # Engine::open_source gives." Opening and stopping are here. Starting is
    # still the host's: run() is a blocking call on a thread this service does
    # not own, and tools/engined/main.cpp loops on it.
    #
    # The reason that entry gave was real and has been dealt with rather than
    # waved away. An engine owned one source for its life because
    # Engine::open_source sizes the grid and the ring against the source's
    # rate, and nothing could take either down. Engine::close_source can, and
    # the note on it lists what that costs.
    #
    # REFUSED WHEN A SOURCE IS ALREADY OPEN, and closeSource is how a client
    # gets from one to the other. Not a replace: a replace that failed on the
    # new URI would have destroyed the working one already, and the client
    # would be holding a two-valued answer to a three-valued question. Two
    # calls means a failed open leaves an engine with no source, which is a
    # state the client asked for and can see.
    #
    # The URI is the same grammar the command line takes, which is the one
    # listSources hands back in SourceDescriptor::uri. A client builds one by
    # taking that string and appending the settings an operator chose; it does
    # not have to parse it, and should not, because each backend's grammar is
    # its own.
    #
    # WHAT A CLIENT MUST RE-ESTABLISH AFTERWARDS, ALL OF IT. Every receiver is
    # gone, with its audio, its passband and its RDS decoder; every
    # subscription this session held has ended; the detector is rebuilt on the
    # next detections call and the threshold set on it goes back to the
    # engine's default. None of that is this call being unhelpful: a receiver's
    # centre is an offset from a baseband whose meaning was the closed
    # source's, so carrying one forward would put it at a plausible offset
    # from the wrong centre, which looks exactly like a working receiver.
    #
    # AND THE SAMPLE INDICES START AGAIN. EngineInfo::sourceEpoch is what
    # separates the new stream's index zero from the old one's; read its note
    # before correlating anything across this call.
    openSource @18 (uri :Text) -> ();

    # Closes the source, leaving an engine that openSource can be called on
    # again.
    #
    # A success on an engine with nothing open, rather than a refusal. A client
    # that closes before every open should not have to know which state it was
    # in to read the answer.
    #
    # IT BLOCKS UNTIL THE STREAM HAS STOPPED and that is a device stop plus a
    # GPU flush, milliseconds on the hardware this has run on. It is refused,
    # with nothing torn down, if the stream will not stop inside five seconds:
    # an engine still streaming is a working engine, and a close that gave up
    # half way would leave one that is neither.
    closeSource @19 () -> ();

    # The front end's gain, by the stage's own name, answering with the value
    # the device took.
    #
    # THE STAGE NAMES AND THEIR STEPS COME FROM SourceDescriptor::gainStages,
    # which listSources already carries. An R820T has one stage with 29
    # discrete steps, an Airspy has three, and a file has none, so a client
    # draws the controls the device says it has rather than one gain knob and a
    # percentage. A request between two steps lands on the nearest, and the
    # answer is what the tuner took: a slider showing the request rather than
    # the grant is a slider showing a gain the device never held.
    #
    # WHY IT EXISTS AS A CALL RATHER THAN A URI PARAMETER. Gain was settled
    # when the source was opened and could not be changed after, so an operator
    # whose audio was overloading had to close the source and reopen it to try
    # a different value, which costs every receiver and the waterfall history.
    # Same argument setSourceCenter was added on.
    #
    # ON AN RTL-SDR THIS STOPS THE TRANSFERS BRIEFLY, for the reason the
    # retune section of docs/rpc.md documents at length: the gain registers sit
    # behind the same I2C repeater the tuner does, and the platform stalls a
    # control transfer to a dongle that has been streaming for more than about
    # half a second. So a gain change costs the same third of a second of
    # samples a retune does, reported the same way, through
    # SourceStats::samplesLost and the block's own gap. It is not a source
    # change: sourceEpoch does not move and no receiver is disturbed.
    #
    # Refused in the SOURCE's own words on a source with no such stage and on
    # a stage name the device does not carry, because their sentence names what
    # the device does have.
    setSourceGain @20 (stage :Text, db :Float64) -> (grantedDb :Float64);

    # Hands the stage to the device's own AGC, or takes it back.
    #
    # A CHOICE TO OFFER AND NOT THE SENSIBLE SETTING.
    # SourceDescriptor::gainStages says through hasAuto whether the device will
    # do it at all; whether it should is the operator's and depends on their
    # antenna. Measured on an RTL-SDR v3 at 95.1 MHz in a suburban FM
    # environment, README.md has it: the tuner's AGC made the wideband detector
    # report three intermodulation products as real tracks at confidence 1.00,
    # and a fixed 20 dB removed all three and improved the station's measured
    # SNR by 5.7 dB.
    #
    # Costs the same brief stop in the transfers setSourceGain does, on the
    # same hardware and for the same reason.
    setSourceGainAuto @21 (stage :Text, on :Bool) -> ();

    # What the OPEN source can do, as opposed to what the devices on this
    # machine can do.
    #
    # listSources describes candidates and opens every device to do it. This
    # describes the one that is already open, costs no device access, and is the
    # only way to learn a running source's gain stages, its flow control or its
    # sample formats.
    #
    # WHY IT WAS NEEDED. A client drawing a gain control has to know the stage's
    # name, its range and its discrete steps, and those live on
    # SourceDescriptor::gainStages. A client that opened the source through
    # listSources could remember them from the row it picked; a client that
    # attached to an engine somebody else started with a URI on the command
    # line, which is how revenant-engine is normally run, had never seen a
    # descriptor at all and could not offer the control.
    #
    # `open` is false with no source, and the descriptor is then a default one
    # rather than an error, because "nothing is open" is a state a client polls
    # through rather than a call that failed.
    #
    # FlowControl comes back here too, and reading it is what stops a client
    # misreporting a live radio. EngineInfo::sourcePacedBy is the setting that
    # matters on a Demand source and is ignored entirely on a Paced one, so a
    # client without this field told an operator their dongle was "paced at
    # 1.00x on purpose" when the dongle had never looked at the setting.
    sourceDescriptor @22 () -> (open :Bool, source :SourceDescriptor);

    # The decoders this engine can attach, by name, with what each reads.
    decoders @23 () -> (decoders :List(DecoderInfo));

    # Attaches a decoder to a receiver and streams what it recovers.
    #
    # PER RECEIVER, PER DECODER AND OPT IN, like subscribePassband and
    # subscribeAudio: nothing decodes until something subscribes, two
    # subscribers to the same decoder on the same receiver share one
    # instance, and the last one leaving takes it off. A decoder costs CPU on
    # the engine's completion thread for every chunk the receiver produces,
    # which is the cost "nobody asked, nothing runs" exists to hold down.
    #
    # decoder EMPTY means the one named after the receiver's mode, so a p25p1
    # receiver gets the p25p1 decoder, and it is refused on a mode no decoder
    # is named after. decoderResolved says which ran. The mode chooses the
    # channel filter and the decoder chooses what is read out of it, so they
    # are separate on purpose: the ax25 decoder reads an nfm receiver's audio
    # and rtty a usb or lsb one's, and an empty name on those modes is refused
    # with the list of decoders that read them.
    #
    # REFUSED, in words, for a receiver that does not exist, for a decoder
    # this engine does not have, and for an input the receiver cannot give:
    # a complex-baseband decoder needs a receiver whose mode is a complex tap,
    # and an audio decoder needs one of the modes its description names, which
    # the refusal names again. The rate is NOT
    # checked here. The decoder is built on the first chunk at the rate that
    # chunk carries, because the rate a complex tap delivers is the engine's
    # business and is changing, and a decoder that cannot run at it says so
    # through ended().
    #
    # A RETUNE FENCES IT exactly as it fences an RDS decoder: setVrxParams and
    # setSourceCenter reset the decoder and discard chunks recorded at the
    # old tuning, so a message is never assembled from two transmitters.
    subscribeDecoded @24 (vrx :UInt64, decoder :Text, receiver :DecodedReceiver)
        -> (subscription :DecodedSubscription, decoderResolved :Text);
}
