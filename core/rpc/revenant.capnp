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

# The frequency axis of one receiver's passband frame.
#
# Per frame rather than in EngineInfo, unlike SpectrumGeometry above, because
# only the transform size is engine-wide: the width of the axis is the
# receiver's own demodulation rate and moves whenever its filter does.
struct PassbandGeometry {
    transform @0 :UInt32;
    bins @1 :UInt32;

    # The fine stream's rate, which is the whole width of the frame. It is
    # somewhat wider than the receiver's passband rather than equal to it,
    # and the margin is where the filter's skirts are drawn.
    rate @2 :UInt32;

    binWidth @3 :Rational;

    # Centre frequency of bin zero, half the demodulation rate below whatever
    # the fine stage mixed to DC.
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

# One receiver's own complex baseband, transformed.
#
# The response shaping it is the receiver's own fine filter and nothing
# divides that out. The skirts ARE the feature: a filter parked on a signal is
# judged by where its edges fall against the signal's, which is the whole
# reason this frame exists and the surface a passband is dragged over.
struct PassbandFrame {
    vrx @0 :UInt64;

    # Decibels relative to full scale, ascending in frequency across the whole
    # demodulation rate, no gaps and nothing counted twice.
    powerDb @1 :List(Float32);

    geometry @2 :PassbandGeometry;

    # Source samples this frame's window covers, [start, start + count).
    # Absolute from the start of the stream, with both the channelizer
    # prototype's group delay and the fine filter's already taken off, so it
    # lines up with an AudioChunk's start and with a SpectrumFrame's.
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
    # channelCount reads 1 on every engine built from this tree. Every
    # demodulator in core/engine/vrx_stage.cpp sets StageOutput::channels to
    # 1, and the one stage that sets 2 is the raw complex tap, which cannot be
    # subscribed to. It is on the wire for WFM stereo, which is the next thing
    # that will change it, and because a client that hardcoded 1 would then
    # play one channel of a pair at half speed.
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
    # other signal. reason carries the engine's own words when there are any.
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

    # Refused once the subscription is over, whether it was cancelled or
    # ended by the engine, and the two refusals say which. Answering an ended
    # subscription's counters reports a healthy stream on a receiver that no
    # longer exists, and the numbers never move again.
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
    # core/rpc/convert.h asserts the pair and core/rpc/convert.cpp converts
    # it through an exhaustive switch rather than the cast the assert would
    # allow, because an assert catches a REORDER and cannot catch a third
    # region added to the decoder alone.
    #
    # This note used to end "what is here today is a matched ordering and not
    # an enforced one", which was true while nothing converted it. It is
    # enforced now.
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
    # Per receiver and not engine-wide, unlike setDetectionThreshold. Two
    # receivers can sit on two stations and there is no reason in the standard
    # why they share a continent; the detector's threshold is engine-wide
    # because there is one detector.
    setRdsRegion @15 (vrx :UInt64, region :RdsRegion) -> ();
}
