# Wideband detection, identification and click-to-tune

The wanted behaviour, stated by the operator at M1: watching twenty megahertz,
identify every signal detected above a confidence threshold the operator sets,
hold each identification for a while after the signal stops and let it decay,
and let a click on any live detection tune a receiver to it.

Nothing here exists. `core/detect/` is an empty directory and the full-span
spectrum path it depends on has not been written either. This is written now
because several conclusions below constrain the spectrum stage, which is the
next thing to build.

A first draft of this document was checked against the code and got four
things wrong in ways that mattered: it specified a transform the project's own
FFT kernel cannot run, it argued that finer bins detect better when the
opposite is true, and it built a whole section on a cost that the receiver
mode it chose does not pay. Those corrections are the substance of what
follows, and they are marked where they land.

## Where the spectrum comes from

Not from one enormous transform. `docs/fft.md` scopes the project's FFT kernel
to a whole transform resident in shared memory, which is 48 KiB on the
discrete card and 32 KiB on the integrated one, and at eight bytes per
`Complex32` that is 4096 points on one and 2048 on the other. A single
transform across twenty megahertz at any useful resolution is sixteen to sixty
times outside that, and `fft.md` explicitly declines to serve it: the
alternatives it names are a multi-pass kernel nobody has written or VkFFT with
a documented loss of bit-exactness.

Neither is needed, because the channelizer already did most of the work. A
second-stage transform of each coarse channel's time series reaches the same
total bin count in pieces that fit:

| Per-channel transform | Shared memory | Bin width at 20 MS/s | Total bins |
| --- | --- | --- | --- |
| 2048 | 16 KiB, fits both devices | 305 Hz | 65536 |
| 4096 | 32 KiB, discrete only | 153 Hz | 131072 |

The arithmetic, for a 64-channel grid at 20 MS/s: each channel runs at
625 kS/s and carries 312.5 kHz of unique spectrum, because the grid is 2x
oversampled. A 2048-point transform of one channel gives 305 Hz bins, of which
the central 1024 span that channel's 312.5 kHz. Sixty-four of those tile the
span without gaps and without counting anything twice.

**The oversampling is what the central half is for.** Adjacent channels
overlap by half, so a signal in an overlap region appears in two channels.
Keeping only the central half of each channel's bins makes exactly one channel
own each frequency, which is the same reason the grid is 2x oversampled in the
first place: no signal sits on a boundary in the half that gets used.

This also means the detection spectrum is per channel and inherently parallel,
which suits the hardware, and that it reuses a kernel already proved bit-exact
against a twin rather than introducing one that is not.

## Resolution: two jobs, two bin widths

The first draft argued that the detector needs finer bins than the display and
left it there. The first half is true and the reasoning was wrong, in a way
that would have cost most of the sensitivity the integration was bought for.

Bin width and frame rate are the same number. A transform of N points over a
rate of `fs` gives `fs/N` hertz per bin and `fs/N` frames per second, so
finer bins buy resolution by spending averages. That is a trade, not a free
byproduct.

Worse, for detecting a signal of bandwidth B, the deflection of the test
statistic is maximised when the bin width equals B, and falls as the square
root of the ratio when bins are finer. For a 2.8 kHz SSB signal measured in
76 Hz bins that is a factor of six, **7.8 dB of sensitivity given away**, plus
four times as many bins to raise the threshold against for the same
false-alarm rate. Single-bin thresholding on a fine transform is the worst
available detector.

So the two jobs want different widths and the resolution of the transform is
not the answer to either on its own:

- **Bounding a signal's edges** wants fine bins. 305 Hz resolves a 2.8 kHz
  signal into nine bins, which is enough to place its edges to within a few
  hundred hertz.
- **Deciding whether a signal is there** wants bins matched to the signal, so
  the detection statistic is computed on **sums of adjacent fine bins** across
  a small set of candidate bandwidths, not on the fine bins themselves. Summing
  is cheap and it is the step that recovers the 7.8 dB.

And the display wants neither: about four thousand columns on a 4K screen,
which is a reduction of the detection spectrum by a factor of sixteen at 2048
points per channel. The display is a decimation of the detector's frames, not
the other way round, and it is about six powers of two smaller rather than the
one or two the first draft claimed.

## Detection

Averaged summed-bin power, a noise floor estimated across frequency, a
threshold above it, and contiguous runs that cross it.

The noise floor is a running median or low percentile over neighbouring bins
rather than a mean, because a mean over a region containing a signal is raised
by the signal it is measuring.

That is a shared primitive with the auto-scaling in `docs/ui-spectrum.md` and
**not a shared estimate**, which the first draft got wrong. The detector wants
a median over a local neighbourhood in frequency, integrated over about a
second. The display wants a fifth and ninety-ninth percentile over whatever
span is currently visible, expanding within a frame and contracting over
thirty seconds. Different support, different statistic, different time
constant. Sharing the percentile routine is right; computing one estimate and
feeding both is how you get something that serves neither.

Averaging is what makes a low threshold usable. One frame's noise varies
enough that a threshold near the floor produces constant false detections;
integrating about a second settles it.

### What the threshold is measured in

`docs/snr-convention.md` exists because three different numbers get called
SNR and they differ by more than 12 dB. A per-bin peak is none of them, and it
is not comparable across bandwidths: a 50 Hz CW carrier and a 2.8 kHz SSB
signal with the same SNR in a 2500 Hz reference bandwidth have very different
per-bin peaks, so one threshold would mean different things for different
modes.

Candidates therefore carry SNR in the project's 2500 Hz reference bandwidth,
converted from whatever bin sum produced the detection. That is what makes a
single operator-set threshold mean one thing across the whole span.

### The threshold is the operator's

Set by the operator, not compiled in. It is the one knob that decides whether
the display shows everything including things that are not there, or only the
loud ones, and where a person wants it depends on the band, the antenna and
what they are doing. A default that suits a quiet VHF band buries an HF
evening.

The same applies to the confidence threshold on identification, below.

## The tracker, which is where hold and decay live

Candidates are per frame. What the operator clicks is a track: a thing with an
identity that persists across frames, and that is a different object.

Each track carries an id, a centre and bandwidth that update as the signal
moves, a confidence, first and last seen times, and its classification.

The decay is the feature, not smoothing. Almost everything worth detecting is
intermittent: CW is keyed, RTTY has gaps, FT8 transmits 12.6 seconds in every
15 and is silent for the remaining 2.4. Without hold the display flickers and
a click lands on something that has just vanished.

A track is also where identification accumulates, for the reason in
`docs/ui-spectrum.md`: a classification is stable because a transmission does
not change modulation halfway through, so a track that has been up for five
seconds has had five seconds of evidence. The confidence the operator
thresholds on is the track's, not any single frame's.

Five rules the first draft left out, each of which breaks something:

**Assignment has to be one to one.** "Associate by frequency overlap" is not a
rule when two tracks overlap one candidate or one candidate overlaps two
tracks. Without an assignment step both tracks claim it and both look
confirmed.

**Merge and split need an answer.** Two signals inside each other's bandwidth
become one run, doubling a track's measured bandwidth and changing the
classification underneath it, which is the one thing the stability argument
assumed could not happen. On a split, which id survives has to be decided.

**Birth needs a rule.** Decay and a drop floor are specified above; how many
consecutive frames a candidate needs before it becomes a track is the actual
false-alarm knob and belongs next to the threshold.

**The initial hold cannot be per classification.** Setting hold from the mode
is circular: the track has to survive the silence long enough to be
classified. FT8's 2.4 second gap kills a track before tier two runs unless the
bootstrap hold is the longest plausible one, and it is narrowed once the mode
is known.

**Channel boundaries need hysteresis.** `place()` takes the nearest channel by
rounding, so a track near a boundary flips channel on measurement noise, and
each flip is a channel-sized change in the residual, a completely different
tap table and a half-megabyte upload.

## Identification

Two tiers, split on cost.

**From the spectrum**, free, on frames the detector already has: bandwidth,
symmetry about the centre, presence of a carrier spike, how the shape moves
over time. That separates the broad families, and often finishes the job: a
2.8 kHz asymmetric block with no carrier is SSB, known from the same frames
that found it.

**From a narrowband extract**, for what shape cannot settle: envelope
variance, tone structure, symbol rate, cyclostationary features.

### What a probe receiver actually is, which the first draft got wrong

The first draft said a detection becomes a `Demod::Raw` probe whose complex
baseband the classifier reads, and built a cost argument on filter design and
pipeline creation. Both halves were wrong, and in opposite directions.

`Demod::Raw` is declined by the demodulator factory and falls back to the
graph's own `RawTapStage`, which is a buffer copy of one coarse channel with
no mixing, no filtering and no resampling. On a 64-channel grid at 20 MS/s
that is 625 kS/s of complex baseband spanning 312.5 kHz, with the signal at an
arbitrary offset rather than at DC. Against a 2.8 kHz signal that is 112 times
the bandwidth, about 20 dB of extra noise into the classifier, and roughly
5 MB/s per probe across the bus. A hundred of those is 500 MB/s, three times
the input stream, against the promise in `core/engine/engine.h` that audio,
symbols and metadata are the only things that come back.

And the cost the draft worried about is not one a Raw probe pays.
`plan_vrx`, the two pipelines, the tap table and the fine ring all belong to
`DemodStage`, which Raw never reaches; `RawTapStage`'s constructor is four
assignments and its retune is one.

So the choice has to be made rather than assumed, and it is: **tier two uses a
real demodulator stage, not the raw tap.** The extract has to be mixed to DC
and filtered to something near the signal's bandwidth or the classifier is
working 20 dB down, and only a `DemodStage` does that. Which means the
construction cost is real after all, and the pool is real, but for a reason
the draft never stated.

### What the pool can and cannot be

A pool of reusable probes, retuned rather than rebuilt, because construction
runs `plan_vrx`, creates two compute pipelines and uploads a tap table of up
to half a megabyte.

Two constraints the first draft missed, which together mean the pool is
simpler than "narrow, medium and wide":

**`retune()` does not refuse on bandwidth.** It compares the plan's derived
integers: tap count, phases, NCO size, decimation, audio taps, DC taps and the
three rates. A bandwidth change that leaves those alone is accepted. What has
to match is the demodulation-rate bucket, not the filter shape.

**The tap cap flattens the shapes anyway.** `kMaxFineTaps` is 256, and at a
625 kHz channel rate a 2.8 kHz receiver wants roughly 2240 taps for its
transition. It gets 256, and an achieved transition near 12 kHz. Every
receiver narrower than about 25 kHz on that grid gets the same filter, so
below 25 kHz "narrow" and "medium" and "wide" are one probe with three names.

The real lever is the channel count, and it is a genuine trade the draft did
not name: a finer grid buys narrower fine filters and lowers
`max_channel_bandwidth`, which is what wideband FM needs. Choosing it is a
decision for when this is built, against measurements rather than against
this paragraph.

### Confidence, and "unknown" as a real answer

The confidence threshold is the operator's, alongside the detection threshold.

For it to mean anything the classifier's confidence has to be calibrated: one
that reports 0.9 on everything makes the threshold a no-op. A detection the
classifier cannot place stays on the display as an unidentified signal rather
than being given the nearest label. It is still clickable, it still has a
bandwidth, and the operator can listen and decide, which is what they were
going to do. This is the same rule AFT follows in `docs/ui-spectrum.md`: an
unidentified signal means hold still, not guess.

## Click to tune

The track carries a centre, a bandwidth measured from the signal rather than
defaulted, and a classification. Three things stand between that and one call
to `add_vrx`.

**Which frequency frame the track is in has to be stated.** A detector working
on the full span produces baseband offsets. `VrxParams::center` is documented
as absolute while `place()` reads it as an offset and the engine is expected
to rebase; `EngineInfo::source_center` exists so a caller can convert, and
`revenant-cli` does. The track should carry absolute and convert at the call,
the same as the command line, so that two places do not disagree about what a
number means.

**A bandwidth wider than one channel does not fail, which is worse.**
`place()` sets `bandwidth_clamped` and succeeds, `plan_vrx` silently takes the
minimum of requested and available, and `add_vrx` returns an id. The operator
gets a receiver narrower than the signal they clicked with no error anywhere,
and `VrxStatus::params` reports the bandwidth asked for rather than the one
delivered. Click-to-tune has to read `placement.bandwidth_clamped` and say so.

**There is no Engine surface for any of this yet.** `Engine` has no method
returning a spectrum frame, none returning detections or tracks, and no route
for complex baseband to a classifier. The click resolves against a track list
that nothing currently exposes.

## What has to exist first

The spectrum stage, as a second-stage transform of the channelizer's output
rather than a transform of the whole span, sized from the table above and
keeping the central half of each channel's bins.

An Engine surface carrying spectrum frames and, later, tracks.

`Demod` gaining the modes this document keeps using as examples. It currently
holds `Raw, Am, Nfm, Wfm, Usb, Lsb, Dsb, Cw`: no RTTY, no FSK, no PSK. The
enumeration is a frozen contract and its value is the demodulator kernel's
specialization constant, so growing it is a deliberate change rather than an
edit.
