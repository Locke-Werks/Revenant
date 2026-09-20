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
# the signed pre-reduction channel index the hysteresis is carried in, and a
# classification enum with exactly one value in it. None of that is something
# a display draws or a click resolves against, and a schema is a contract
# rather than a mirror. What is here is what docs/ui-spectrum.md asks for:
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
# deriving it needs a classification. core/detect/detector.h leaves
# Track::classification as a deliberate seam that nothing fills, and
# docs/ui-spectrum.md notes that the enumeration would have to grow before
# RTTY and its shift were even expressible in VrxParams. So there is no
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

    # Zero to one, rising on evidence and decaying on silence. The track's own
    # and not any single frame's, which is what makes it worth thresholding:
    # a transmission does not change modulation halfway through, so a track
    # that has been up for five seconds has had five seconds of evidence.
    #
    # It approaches one without arriving. See Session::detections, which
    # refuses a bar of one for that reason.
    confidence @4 :Float64;

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
    detections @10 (minConfidence :Float64) -> (detections :DetectionList);

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
}
