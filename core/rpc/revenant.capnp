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
# Spectrum frames cross by copy. core/rpc/.gitkeep planned a shared GPU
# texture handle for a local client, and that is still the right answer for a
# frame at the source's own rate. It is not required first: a display asks
# for time decimation and takes 30 frames a second, which is 7.5 MB/s at the
# shipped geometry against the 80 MB/s the engine produces. The handle is an
# optimisation on a path that has to exist and be correct either way.
#
# Audio does not cross at all yet. The CLI renders its own through WASAPI in
# the same process as the engine, and a remote client wanting audio needs a
# codec decision this does not have to make today.

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("revenant::rpc::schema");

# An exact frequency, in hertz. See the note above: this is a rational and
# not a rounded integer because the grid's own arithmetic is rational.
struct Rational {
    numerator @0 :Int64;
    denominator @1 :Int64 = 1;
}

enum Demod {
    # Ordinal for ordinal with revenant::engine::Demod, which is
    # Raw, Am, Nfm, Wfm, Usb, Lsb, Dsb, Cw. Matching it makes the conversion
    # a cast, and core/rpc/convert.h static_asserts every pair rather than
    # trusting that: a mode reordered on one side and not the other would
    # silently retune every receiver in a saved session, and nothing about
    # the failure would point here.
    raw @0;
    am @1;
    nfm @2;
    wfm @3;
    usb @4;
    lsb @5;
    dsb @6;
    cw @7;
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
}

struct SourceDescriptor {
    # Mirrors the first four fields of revenant::source::SourceCapabilities.
    # The rest of that struct, the tune ranges and gain stages and the
    # format, is not here yet: a picker needs to list what exists before it
    # needs to configure one, and adding fields to a schema is the cheap
    # direction.
    uri @0 :Text;
    backend @1 :Text;
    displayName @2 :Text;

    # Empty when the backend is usable. Non-empty is the reason it is not,
    # and the caller shows it rather than hiding the backend: a missing DLL
    # and an unplugged radio are different problems and a list that omits
    # both looks identical.
    unavailable @3 :Text;
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
}

struct VrxParams {
    center @0 :Int64;
    bandwidth @1 :Int64;
    demod @2 :Demod;
    audioRate @3 :UInt32;
    squelchDbfs @4 :Float64;
    agcAttackMs @5 :Float64;
    agcDecayMs @6 :Float64;
    agcEnabled @7 :Bool;
    cwPitch @8 :Int64;
}

struct VrxPlacement {
    channel @0 :UInt32;
    channelCentre @1 :Rational;
    residual @2 :Rational;
    channelRate @3 :UInt32;

    # True when the requested bandwidth did not fit one grid channel and the
    # receiver was given the widest that does. The caller is told rather
    # than quietly receiving less than it asked for, and the display needs
    # it because the consequence is audible.
    bandwidthClamped @4 :Bool;
}

struct VrxStatus {
    id @0 :UInt64;
    params @1 :VrxParams;
    placement @2 :VrxPlacement;

    # Passband level in dBFS, updated per block. This is the meter in the
    # receiver rack.
    levelDbfs @3 :Float64;
    squelchOpen @4 :Bool;

    # A dropped audio sample is a dropout the operator hears, so it is
    # counted and carried rather than logged where a remote client cannot
    # read it.
    audioSamples @5 :UInt64;
    audioDropped @6 :UInt64;
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

interface Session {
    info @0 () -> (info :EngineInfo);
    running @1 () -> (running :Bool);
    listSources @2 () -> (sources :List(SourceDescriptor));
    sourceStats @3 () -> (stats :SourceStats);

    addVrx @4 (params :VrxParams) -> (id :UInt64);
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
}
