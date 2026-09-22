# Wideband detection, identification and click-to-tune

The wanted behaviour, stated by the operator at M1: watching twenty megahertz,
identify every signal detected above a confidence threshold the operator sets,
hold each identification for a while after the signal stops and let it decay,
and let a click on any live detection tune a receiver to it.

Written before any of it existed, because several conclusions below constrain
the spectrum stage, which was the next thing to build. Since then the spectrum
stage and `core/detect/detector.cpp` have both been written, so the sections
that describe them now describe code rather than intent. Where a number
appears below it was measured. The tracker is written too, in the same file,
and click-to-tune is built in `ui/`; identification is the part that is still
design, and `Track::classification` is the seam nothing fills.

That sentence read "the tracker, identification and click-to-tune are still
design" until 2026-09-20, in the same breath as conceding that
`core/detect/detector.cpp` had been written. The tracker is that file, states
and all, and `ui/render/spectrum_item.cpp` resolves a click and emits
`tuneRequested`. A reader planning work off the old sentence would have set
out to build two things that exist.

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

## The channel shape, and why it is a measurement error

The central half tiles the span exactly once because it is one channel spacing
wide. One channel spacing wide is also where the prototype's cutoff is. The
section above states the first half of that and stops, and the second half is
an artifact that was visible in the display for as long as the display existed.

`core/dsp/pfb.h` puts the prototype's cutoff at `rate/(2M)`, so adjacent
channels cross at half amplitude there. The channelizer decimates by `D = M/2`,
so a channel runs at `2*rate/M` and an N-point transform of it spans that much
bandwidth. Keeping the central half of that transform keeps `+/- rate/(2M)`
around the channel centre. Those are the same frequency. Every channel's kept
band therefore ends on the prototype's cutoff at both ends, every channel
arrives about 6 dB down at both of its own edges, and the frame tiles that
droop once per coarse channel.

Measured, not derived. White noise through the channelizer twins and the
spectrum twin, mean linear power per bin position within a channel, averaged
over all channels and frames:

| | measured |
| --- | --- |
| Outermost kept bin, M = 64, N = 2048, 8192 averages per bin | -5.96 dB |
| Peak to peak across one channel | 6.13 dB |
| Agreement with the prototype's own magnitude response, 49152 averages | 0.076 dB |
| Bins more than 0.1 dB down | 30 percent |
| Bins more than 1 dB down | 17 percent |
| Bins more than 3 dB down | 8 percent |
| Mean level across a whole channel | -0.43 dB |

On a waterfall that is a vertical striation every channel spacing. On a trace
it is a scallop with a cusp at every seam, which reads as a binned or
quantised display rather than as a filter.

**It is not only a display problem.** A candidate's SNR in the 2500 Hz
reference bandwidth is a signal level over an estimated noise floor, and the
floor is estimated at `noise_knots` points across the span from a window of
`noise_window_knots` knot spacings. At the defaults that is 32 knots and a
window a quarter of the span wide, which on the shipped grid is sixteen coarse
channels. A droop that repeats once per channel repeats sixteen times inside
one of those windows, so the floor estimate averages it flat. The candidate's
own peak does not. The same signal therefore reports up to 6 dB less SNR at a
seam than at a channel centre, and whether a marginal one crosses the
operator's threshold depends on where in the grid it happened to land rather
than on anything about the signal.

Widening or narrowing the floor's window does not fix that. The window is sized
by the widest signal that must not hide its own floor, which is the argument in
`core/detect/detector.h`, and a window narrow enough to follow the droop would
be narrower than a broadcast carrier.

**The correction.** `build_spectrum_window` in
`core/dsp/spectrum_reference.h` evaluates the prototype's magnitude response at
each of the `bins_per_channel` kept offsets, in double, rounds once, and hands
the kernel one power gain per bin alongside the analysis window.
`core/shaders/spectrum.comp` multiplies each bin's power by it before the floor
and the logarithm. The same measurement after: 0.15 dB peak to peak at N = 256
with 32768 averages per bin, 0.33 dB at N = 2048 with 8192. Both are what the
estimation noise leaves behind at that many averages rather than residual
droop.

It could not be folded into the analysis window, which would have been free.
The window multiplies the transform's input and the correction scales its
output. A per-sample multiply is a convolution in frequency, not a per-bin
gain, and the two are not even the same length.

The correction rides in the window's buffer instead of one of its own, because
the spectrum kernel has four bindings and `core/engine/graph.cpp` declares
four. The table inverts the prototype, so it depends on the prototype length:
measured against the canonical 17 taps per branch, 9 taps differ by 0.93 dB and
33 taps by 1.75 dB. The channel count does not matter at all, measured at
0.0000 dB from M = 8 to M = 1024. `build_spectrum_window` takes the length as
an argument and defaults it to `GridParams{}.taps_per_branch`, which is 17, and
the full-span stage in `graph.cpp` passes `impl.grid.taps_per_branch`, so
`revenant-engine --taps` away from 17 is corrected for the prototype the grid
actually built.

One caller still defaults it deliberately. `passband_window` in `graph.cpp`
overwrites the correction half of the buffer with unity, because a passband
frame has no seam to hide, so the parameters that built that half do not
matter there.

WHAT THIS PARAGRAPH USED TO SAY: "`graph.cpp` calls it without one, so
`revenant-engine --taps` away from 17 leaves up to about 1.8 dB of droop near
the seams. Closing that is one argument at the call site." The argument was
added at that call site and `core/dsp/spectrum_reference.h` was corrected then;
this copy was not. Left standing it tells anyone chasing a seam-edge level that
1.8 dB of known droop is still in the data and that the cause is elsewhere.

**What the correction does at the edges.** It amplifies, so the question is how
far. Not far: the kept band ends at the cutoff and never reaches the stopband,
which starts at `0.75/M` while the band ends at `0.5/M`. The deepest point is
about 6.02 dB, the largest gain is a factor of four in power, and nothing is
divided by anything near zero.

Measured at a 120 dB target and a 2048-point transform, the deepest point in the
kept band is its outer edge in every case, and its depth settles on the
half-power crossover as the prototype lengthens. -6.0206 dB, which is
20·log10(0.5), at 16 taps per branch and upward: 16, 17, 24, 33, 48, 64 and 129
all agree to four decimals. Below that the transition has not finished by the
band edge and the figure drifts either way: -6.0208 dB at 12, -6.0279 at 8,
-5.9909 at 5, -6.0627 at 4 and -8.0045 at 3. `kMaxSpectrumCorrectionDb` clamps
at 12 dB, which clears the deepest of them by four decibels, so the clamp never
fires; it exists because a response of zero would put an infinity in a table
that gets uploaded to a device, and an infinity there poisons a whole frame
rather than one bin.

WHAT THIS PARAGRAPH USED TO SAY: "Measured at a 120 dB target at 4, 5, 8, 12,
16, 17, 24, 33, 48, 64 and 129 taps per branch, the deepest point in the kept
band is -6.0206 dB every time; the one outlier is a three-tap branch at -8.0045
dB." Four of those eleven do not measure -6.0206 and one of them, five taps per
branch, is not even below it. The list read as eleven independent confirmations
of one number and was one number plus four that had been rounded into it.
`core/dsp/spectrum_reference.h` is the source of truth for these figures and
was corrected at the time; this copy was not, so the withdrawn list stayed in
front of every reader of this document while the correction sat in a header
they had no reason to open.

**What it costs.** The noise at a seam is lifted by the same factor as the
signal, so the correction buys no sensitivity: a signal at a channel edge is
exactly as detectable as it was. What it fixes is the number. And in decibels
the lift costs nothing, because an unaveraged power bin is exponentially
distributed and its spread in decibels does not depend on its level. Measured
per-bin standard deviation across a channel, uncorrected and corrected: 5.570
dB and 5.570 dB. The mean goes flat and the display gets no noisier.

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

### The scale-space search, and the budget that decides what it finds

The summed-bin search runs a ladder of widths, powers of two from one bin up
to an eighth of the frame, and scores every position at every width by its
deflection. `max_peaks` bounds how many of those scored positions one decision
keeps, so that a pathological frame cannot allocate without bound.

**Which ones it keeps is the whole question, and the obvious answer is wrong.**
Keeping the first `max_peaks` found spends the budget in ladder order, the
ladder is walked narrowest rung first, and a frame that is mostly signal
produces more peaks at the narrow rungs alone than the budget holds. The wide
rungs are then never searched at all. Nothing reports this: the track list is
full, every track is confident, and each one is about as wide as whatever rung
the budget happened to run out on.

Measured, 2026-09-19, RTL-SDR at 98.1 MHz, 2.4 MS/s, 64 channels, a 2048-point
second-stage transform, 65536 bins at 36.6 Hz, eight seconds, GPU 0, the
default 6 dB threshold:

| | keeping the first 4096 | keeping the strongest |
| --- | --- | --- |
| Tracks born | 103 | 26 |
| Highest id issued | 204 | 45 |
| The 98.1 MHz station | about thirty tracks, 5 kHz each, evenly spaced | one track, 140.4 kHz, alive the whole run |
| Detector cost | 1.56 ms per frame, 5.7 percent of one core | 2.00 ms, 7.3 percent |

The evenly spaced 5 kHz fragments are the signature. 5 kHz is 137 bins at that
resolution, which is the 128-bin rung grown until it met its neighbours: the
widest rung the budget reached, tiling a signal about 4000 bins wide.

Raising the threshold cures it and is not the fix. At 12 dB far fewer narrow
windows clear their own threshold, the budget is never reached, and the same
station comes back as one track of 106 to 116 kHz. That is the same detector
reading the same signal and giving two different answers depending on a
number that is supposed to control sensitivity, which is how the fault was
found rather than what to do about it.

So the budget keeps the strongest `max_peaks` peaks. The greedy pass takes
peaks strongest first anyway, so this is the set it was going to work from;
the change is that the set no longer depends on which order the rungs were
walked in. A wide signal cannot be crowded out by narrow ones at any budget,
because the deflection of a signal of bandwidth B peaks at the rung that fits
it.

What a small budget can still do is lose a WEAK signal to a loud one, because
a peaky emitter spends the budget on its own windows. `max_peaks` of zero
therefore derives one per fine bin: the ladder's rungs are powers of two, so
the positions it can report sum to under twice the bin count whatever the
frame holds, and one per bin keeps over half of everything that could exist.
Measured over a seeded eight-emitter scene from 50 Hz to 150 kHz, at 65536
bins, a 6.5 kHz emitter is lost entirely at a budget of 4096, found at 16384,
and the budget stops binding at 65536; the whole range costs under half a
millisecond against a decision that costs eleven.

### What the wide end costs, and where it stops

Two limits sit above a broadcast station, and neither is the budget. The
ladder's widest rung is an eighth of the frame. The noise floor's window is
`noise_knots` by `noise_window_knots` bins, and a signal filling much over
half of one hides its own floor, which `core/detect/detector.h` states at
length. At 2.4 MS/s across 65536 bins those are 300 kHz and about 360 kHz.

Measured against single QPSK emitters of known occupied bandwidth on that
geometry, one per scene, 30 dB in their own bandwidth. Re-measured 2026-09-20
after the growth rule changed, and the retraction below says what moved:

| Occupied bandwidth | Bins | Tracks | Best track covers | Candidate edges |
| --- | --- | --- | --- | --- |
| 135 kHz | 3686 | 1 | 86 percent | one, -57.9 to 58.8 kHz |
| 270 kHz | 7373 | 1 | 86 percent | one, -115.7 to 117.3 kHz |
| 338 kHz | 9216 | 1 | 86 percent | one, -144.7 to 146.5 kHz |
| 405 kHz | 11059 | 2 | 54 percent | -177.3 to 41.4, 42.4 to 184.2 kHz |

So the wide end holds to one track past the widest rung rather than at it.
Growth runs a seed's own width outward on each side, so the 8192-bin rung
reaches 24576 bins between its limits and a 9216-bin signal is inside that.
405 kHz is where it stops: the body itself comes in two, each piece about half
of it, and above that the answer degrades to pieces about a rung wide.

The reported bandwidth of a signal that IS found sits at 86 percent of the
nominal occupied bandwidth throughout. That is the 99 percent occupied-power
trim meeting a shaped signal, not an error: integrating the raised-cosine
spectrum, the symmetric interval holding 99 percent of the power at rolloff
0.35 is 0.87 of the occupied band.

WHAT THIS SECTION USED TO SAY. The table read 5, 8, 5 and 2 tracks at 79, 78,
84 and 54 percent, and the paragraph under it explained the counts: "below the
widest rung the body is one candidate and the extra tracks are the
root-raised-cosine skirts, which fall away smoothly and register separately
once they drop under the growth rule's level". That was true of the growth
rule of the day and is not true now. The level was a fixed multiple of the
mean noise floor, which a 30 dB emitter's skirt crosses well inside its own
occupied band, so each shoulder was left outside the accepted band and found
again as its own candidate. The level is now stated in the averaged noise's
own ripple, which is about four percent of the floor at a second of
integration, and the skirts stay inside the one candidate. See
`edge_floor_sigma` in `core/detect/detector.h` for why that is the right unit
rather than a smaller number in the old one.

### Re-running any of this

Every figure in the two sections above came from `tests/detect/`, and the
harness is the point rather than the numbers: an emitter with a known
occupied band, a known start sample and a known stop sample is something to
generate on demand and score against, and it does not need a radio or a
device.

`tests/detect/scene_frames.h` renders a `siggen` scene and pushes it through
the channelizer and spectrum TWINS, which `tests/reference` proves bit-exact
against the kernels, so the frames are the ones the device would produce.
`SceneScorer` then scores the track list against `EmitterTruth` per emitter:
distinct ids, most tracks at once, how much of the truth extent the best
track covers, how far outside it spills, how far the centre is out, what
fraction of the transmission it was detected at, how long one id survived,
and how far the measurement moves between decisions.

The measurement cases are hidden behind the `[.scene]` tag because the twins
cost about a second and a half of wall time per second of scene:

    revenant_detect_tests.exe "[scene]"

Three bars are not hidden and run with the suite.

The first is the live one: over a seeded keyed scene, every filled emitter
must read as exactly one track, present at one decision in one, covering at
least 70 percent of its extent with at most 25 percent spill, detected at 85
percent of the decisions it transmits at, one id lasting 85 percent of the
transmission, and centred within a tenth of its own bandwidth. Twenty of
twenty pass as this is written, at coverage 0.83 to 0.92 and spill zero: four
QPSK rungs across five combinations of rolloff and level.

Level is a dimension of that sweep and it took three goes to get there. A flat
emitter's per-bin excess over the noise floor IS its SNR in its own occupied
bandwidth, exactly, whatever the bin width, so any rule stated against the
floor is measured against the signal's level and nothing else. Every emitter
in the tree was at 20 or 30 dB in band, where such a rule sits far below the
signal and can only ever act inside a skirt, and a sweep of the rule at that
level certified insensitivity that did not hold anywhere else. Two of the five
combinations now run at 4 dB.

The second bar is the other end of the same transmission: after each of three
broadcast stations stops, its row must be gone inside the hold plus the
decisions the residual rule needs, must spend most of that in Held rather than
Live, must not move its geometry while it waits, and must not be replaced by a
fresh id. It carries lower bounds as well as upper ones, because a detector
that never found the stations satisfies every upper bound there is.

The third is the opposite question, and it is the one the residual rule can
fail: a station that fades 8 dB over three seconds and keeps transmitting must
keep its track. One birth, no drops.

That third bar is one point on a surface, and how much of the surface is safe
has now been got wrong twice, both times by stating the answer as a fade RATE.
It is not a rate. The rule withholds candidates, a withheld candidate is a
track that is not fed, and a track is dropped once it has gone
`bootstrap_hold_seconds` without a detection, so the quantity that decides is
how many seconds of continuous suppression a fade produces. Rate decides
whether the rule fires at all: an exponential average of time constant tau
lags a falling input, and the measured rate converges to the input rate below
`1/tau` and to `1/tau` above it, so a fade slower than the window's lower edge
(0.75 / tau, which is 3.26 dB/s at the shipped second) never reaches the
window at any depth. Depth decides how long it stays fired, because
suppression continues past the end of the fade until the average has emptied
back down to the input.

Measured over both axes in `how long the residual rule starves a fade`, in
`tests/detect/test_detector.cpp`: below 3.26 dB/s nothing is suppressed at any
depth up to 40 dB, and above it the budget is a depth between 13 and 25 dB,
narrowing as the rate rises. A 30 dB fade loses its track at every rate above
3.6 dB/s. The header at `DetectorConfig::residual_rate_tolerance` carries the
table and the history of the two wrong claims.

Tone-driven emitters are measured and deliberately not scored against that
bar. A truth extent is the channel a mode occupies, which for QPSK is where
the energy is and for a tone-modulated AM, SSB or FM emitter is not: those
put their power in a few discrete lines, a detector correctly reports a few
discrete lines, and scoring that against a Carson-rule bandwidth marks the
detector down for being right. A real broadcast station is not that shape,
which is itself a measurement: at 12 dB on the radio the 98.1 MHz station
reads as one track, so its interior never reaches the floor.

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

One threshold meaning one thing also requires the level it is measured against
to be free of grid position, which is what "The channel shape, and why it is a
measurement error" above is about.

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

**WHAT THIS PARAGRAPH USED TO SAY, and it was read as a description of the
`confidence` field rather than of the identification that is still design.**
It read: "A track is also where identification accumulates, for the reason in
`docs/ui-spectrum.md`: a classification is stable because a transmission does
not change modulation halfway through, so a track that has been up for five
seconds has had five seconds of evidence. The confidence the operator
thresholds on is the track's, not any single frame's."

Every clause of that is still true of the classifier this does not have yet.
Sitting directly under the field list, it reads as though it describes the
number the wire actually carries, and on 2026-09-21 that misreading was the
whole of an operator's complaint: intermod bumps on a noise floor, reported at
1.00.

What `detector.cpp` computes is a stopwatch. Birth sets confidence to
`confidence_rise`, every later decision the track is detected in moves it the
same fraction of the way to one, and a decision it is missed from decays it by
half-life. So after n consecutive detections it is `1 - 0.65^n` at the shipped
rise, which passes 0.994 at twelve and prints as 1.00 from about 1.3 seconds
onward. Nothing in either expression reads SNR, bandwidth, deflection margin
or spectral shape. Two tracks with the same detection history arrive at
bit-identical confidence however strong or weak each one is, which
`tests/rpc/test_rpc_detect.cpp` measured on 2026-09-19 and had to work around:
to get any spread into the column at all, that test raises the detection
threshold until half the tracks stop being detected and start decaying.

It answers "has this been here continuously, and for how long", which is worth
knowing and is not what the name promises. A signal that is genuinely present
and genuinely interference earns 1.00 honestly.

**A calibrated number now rides beside it.** `Detection::marginConfidence` is
how far a detection stood above the threshold it had to clear, through
`characterise::margin_confidence`: a half exactly at the threshold, 0.82 six
decibels above, 0.93 at twelve. It is the shape "Confidence, and unknown as a
real answer" below asks for, a stated monotone function of a measured margin,
so two detections can be ordered and a bar on it is a bar on how far the
evidence stood above the noise.

The two are orthogonal and stay that way. Confidence counts detections and
reads no signal quality; the margin reads the measurement and nothing about
time. A track that has stopped being detected loses confidence and keeps its
margin, because decaying both would leave nothing answering how strong it was
while it was there.

What the margin is not is authenticity. A strong interferer stands well above
the noise and scores high, correctly. Separating a signal from a product is
the job of the identification below, and nothing on the wire does it yet.

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

### The association window was never the problem

When one wide signal reads as many narrow tracks, the association rule is the
first suspect: if a candidate moves further between decisions than
`association_overlap` tolerates, every decision starts new tracks and the ids
churn. It is not what happens, and the measurement is cheap to re-run.

Scored against `siggen` ground truth on the shipped geometry, over the filled
emitters of a seeded bandwidth ladder from 6.5 kHz to 150 kHz, keyed on and
off inside the scene:

| Occupied bandwidth | Centre movement per decision, mean | Worst | Worst overlap with itself |
| --- | --- | --- | --- |
| 6.5 kHz | 5 Hz | 15 Hz | 1.00 |
| 27 kHz | 9 Hz | 38 Hz | 1.00 |
| 75.6 kHz | 5 Hz | 26 Hz | 1.00 |
| 149.85 kHz | 6 Hz | 12 Hz | 1.00 |

The last column is the quantity `association_overlap` is compared against:
overlap between one decision's measurement and the next, over the narrower of
the two. The bar is 0.3 and the measured worst case is 1.00, three orders of
magnitude of margin in the sense that matters. A wide signal's measured centre
moves tens of hertz on a band a hundred and fifty kilohertz across, because
the centre is a power-weighted average over thousands of bins and averaging is
what makes it steady.

So an id that churns on a wide signal is the search handing the tracker a
different set of candidates each decision, not the tracker failing to match
the same one. Look at `candidates()` before `tracks()`.

### The band to point a real radio at

460 to 464 MHz. US FCC Part 90 land mobile, business and industrial across
most of it, and the standing band for anything that needs a real signal that
starts and stops. Dispatch, site crews, hotel and store staff, on 12.5 kHz
narrowband FM channels: a few seconds of speech at a time with long dead air
between. Amateur repeaters and marine VHF have the same shape and are quieter
through a working day.

One 2.4 MS/s tuning does not cover it. 461 MHz and 463 MHz between them do:

    revenant-cli "rtlsdr://0?freq=461M&rate=2400000&gain=20" \
        --spectrum --detect

At 64 channels and a 2048-point second stage that is 65536 bins at 36.6 Hz. A
narrowband channel's occupied bandwidth by Carson is about 11 kHz, 2.5 kHz
deviation against 3 kHz of audio, which is roughly 300 bins and sits between
the ladder's 256 and 512 rungs. A rung fits the signal, so none of the
wide-end limits above bind and nothing here is testing the search. This band
is for the track lifecycle.

What to expect. A channel is empty until somebody keys up. A track appears a
fraction of a second behind the carrier: `birth_hits` consecutive decisions at
the default 0.1 second interval, on top of however much of the one second
average the carrier has filled by then. It holds one id for the length of the
over. A gap between overs shorter than `bootstrap_hold_seconds` keeps that id,
so a conversation is one track rather than one per transmission, and a longer
gap starts a new one, which is the hold working rather than failing. After the
last over the track stays up for the hold and then goes: from a confident
track the confidence decay would take a second or so longer, so the hold is
what fires. The measured centre sits on the channel and does not move while
the carrier is up.

What counts as the detector getting it wrong. A new id for every over, which
is association or hold and not the search. One 12.5 kHz channel reading as
several tracks at once. A centre or a bandwidth that keeps changing after the
carrier drops, which is geometry published from an average with no signal left
in it. A track that never drops at all. Tracks on channels nobody transmitted
on, which is the threshold rather than the tracker.

And one that is not the detector at all. On 2026-09-20, at 95.1 MHz with
`gain=auto`, three intermodulation products were listed as tracks at
confidence 1.00. The detector was right about the energy: the products were
there, in the samples, put there by the dongle's own front end. `gain=20`
improved the measured SNR of KKFM at 98.1 MHz by 5.7 dB and every phantom
disappeared. That is why the shipped default is a stated number rather than
the tuner's AGC, and why `core/detect/front_end.h` exists: it watches whether
the noise floor across the whole span is following the strongest signal on it,
and at what rate, so a track list taken through a compressed front end says so
on screen. About one decibel per decibel is a gain control moving; faster is a
nonlinearity. Read that header's note on what the measurement cannot tell
apart before quoting the flag at anyone: it is a correlated floor lift and not
a measurement of compression, and it cannot see an overload that was there
from the first decision and never lifted.

This is a confirmation and not a measurement. What is on the air cannot be
replayed and carries no truth record, so nothing here scores: the referee
stays the seeded scenes in `tests/detect`, which know their own start and stop
samples. What the band adds is that the keying was a person on a PTT rather
than `siggen`.

### HF is the opposite regime, and the grid follows the source now

Every number above was measured at 36.6 Hz per bin, which is 2.4 MS/s over 64
channels with a 2048-point transform. HF signals are one to three orders
narrower: FT8 is 50 Hz, PSK31 about 31 Hz, CW 50 to 150 Hz, RTTY around 261 Hz.
At 36.6 Hz FT8 is 1.4 bins and PSK31 is under one, and what the detector
publishes there is not a miss. It fires, and reports a centre it cannot place
inside the signal and a width that is the bin's rather than the signal's, which
reads as a working detector.

`Engine::open_source` reads `source::SourceCapabilities::resolution` and widens
the grid to meet it, when the caller named no channel count.
`source::resolution_for_span` asks for four bins across PSK31's 31 Hz below
30 MHz and across 12.5 kHz above it, so nothing about a VHF or UHF session
changes. Measured on a 2 MS/s capture centred on 7.1 MHz:

    grid        M=256 D=128 taps/branch=17
                channel 15625 S/s, spacing 7.812 kHz
    spectrum    256 channels x 2048 points, 262144 bins across the span
                7.629 Hz per bin

**The channel count is the lever, not the transform.** Bin width is
`rate / (D * N)` and both narrow it, but `dsp::kMaxSpectrumTransform` is 2048 and
the shipped default is already at it. That cap is a twiddle-table limit shared
with the channelizer rather than a device one, so raising it is a change to
`core/dsp/pfb_design.cpp` and wants its own justification.

**What it costs is a narrower widest receiver,** and the two trade directly.
7.8 kHz of channel spacing is right for HF, where the widest thing in the band
plan is a few kilohertz, and would refuse a 12.5 kHz NFM channel on VHF.
`revenant-engine` prints the substitution under the ring line; `revenant-cli`
pins 64 channels by default and takes `--channels 0` to ask for this.

**One constant here does not follow the grid.** `DetectorConfig::split_gap_bins`
is eight bins and its justification is a frequency: RTTY's two tones 170 Hz
apart, which is 4.6 bins at 36.6 Hz. At 7.6 Hz that is 22 bins, so eight is a gap
RTTY's own tones clear and every RTTY signal splits into two detections. Making
it a frequency is wrong the other way, because eight bins at 36.6 Hz is about
293 Hz and carrying that to HF would refuse to split two FT8 signals 60 Hz apart.
The honest rule is relative to the narrowest signal worth telling apart, which
changes what the detector does at the shipped VHF geometry, so it is left
configurable and recorded rather than re-tuned: measuring a new rule needs real
HF with ground truth, and a recording carries none.

**Real HF arrived on 2026-09-22 and the entry is still open.**
`docs/recordings.md` has six hours of 40 m and 20 m at 96 kS/s, and an excerpt
of one runs at 1.465 Hz per bin. Swept over that excerpt from two bins to
thirty-two, this constant changes nothing at all: 31 tracks born, 26 dropped,
15 merges and 1 split at every value. It does not constrain the number.

Two things that reading taught, both worth keeping. A merge is not a split:
`stats_.merges` counts several tracks gated to one candidate, which is
`association_overlap`, and this constant decides whether one candidate is cut
into several. And two signals that OVERLAP cannot be separated by any gap,
because there is no run of floor between them to measure.

What would constrain it is the RTTY case this section predicts: two tones
170 Hz apart is 116 bins at that grid, far over any value swept, so RTTY there
should split into two detections and be visibly wrong. Nothing found in that
minute obviously is RTTY.

`revenant-cli --detect-split-gap` exists now so the next person can sweep it
without rebuilding.

**It is not inert, and the quiet band made it look that way.** Swept again on
20 m at 1603 UT, the busiest of the six excerpts, it does move:

| gap | born | merges | splits |
| --- | --- | --- | --- |
| 8 bins, 12 Hz | 51 | 36 | 10 |
| 32 bins, 47 Hz | 55 | 28 | 10 |
| 64 bins, 94 Hz | 50 | 40 | 12 |
| 128 bins, 188 Hz | 51 | 45 | 11 |
| 200 bins, 293 Hz | 51 | 40 dropped, 57 | 19 |

Births stay between 50 and 55 throughout, so the constant is not changing what
is detected. Merges climb from 36 to 57, which is the expected direction and
worth stating because it is easy to get backwards: a LARGER gap demands more
consecutive floor bins before one candidate is cut in two, so candidates stay
wide, so more tracks are gated to one of them and merged.

What the sweep does not show is a knee. Nothing in either band picks a value
out, which is the same answer as before arrived at with better evidence: this
wants ground truth rather than another sweep.

## Identification

Two tiers, split on cost.

**From the spectrum**, free, on frames the detector already has: bandwidth,
symmetry about the centre, presence of a carrier spike, how the shape moves
over time. That separates the broad families, and often finishes the job: a
2.8 kHz asymmetric block with no carrier is SSB, known from the same frames
that found it.

### What the first tier measures today, and what it turned out to separate

`core/detect/shape.h` measures three of those on every candidate:
`peak_to_mean`, `lower_fraction` and `skirt_fraction`. Nothing thresholds on
any of them, and the reason is a measurement rather than caution.

The scene it was measured against is new and had to be built first.
`tests/detect/test_front_end.cpp` has three now. The two-tone scene's products
are themselves carriers, so shape cannot tell them from a real signal. The
dense scene's are a pedestal the floor estimator absorbs, which produced zero
false detections at any thermal floor from -70 to -115 dBFS. The product scene
is three narrow QPSK parents whose nine third-order products land discretely in
clear space, which is the on-air failure of 2026-09-20 reproduced without a
radio.

Measured against it on 2026-09-22:

| | candidates | peak/mean | skirt | SNR |
| --- | --- | --- | --- | --- |
| parent | 263 | 1.607 | 0.111 | 48.1 dB |
| product | 567 | 2.473 | 0.009 | 24.3 dB |
| elsewhere | 304 | 1.863 | 0.091 | 13.9 dB |
| parent, linear front end | 189 | 1.608 | 0.028 | |

**`peak_to_mean` separates a product from a parent, and it is measuring the
signal rather than the radio.** A parent reads 1.607 through the cubic and
1.608 without it, identical to three places, while a product reads 2.473. That
is structural: a root raised cosine spectrum is flat topped and a third-order
product is the convolution of three of them, which is domed. It is not an
artefact of level, because the ordering is not monotone in SNR: the weakest
population reads below the products.

**It is not an interference test.** What it separates is flat topped from
domed, which is spectral shape class, which is what this section promises tier
one will give. An unmodulated carrier is the most domed thing on any span, so a
rule calling domed bands products would call every carrier one.

### One emitter of each family, which settles it against a threshold

The scene those three QPSK parents could not settle. One emitter per mode, no
nonlinearity, nothing to separate, measured the same day:

| family | peak/mean | skirt | lower | width |
| --- | --- | --- | --- | --- |
| cw | 2.384 | 0.000 | 0.603 | 183 Hz |
| am | 2.323 | 0.000 | 0.464 | 180 Hz |
| nfm | 2.256 | 0.000 | 0.514 | 175 Hz |
| usb | 2.112 | 0.000 | 0.453 | 165 Hz |
| lsb | 2.111 | 0.000 | 0.547 | 165 Hz |
| fsk2 | 2.854 | 0.432 | 0.481 | 862 Hz |
| bpsk | 1.793 | 0.001 | 0.495 | 1465 Hz |
| qpsk | 1.703 | 0.039 | 0.499 | 1430 Hz |
| **product** | **2.473** | **0.009** | **0.507** | **7651 Hz** |

**There is no `peak_to_mean` threshold and there cannot be one.** The product
sits at 2.473, between `nfm` at 2.256 and `fsk2` at 2.854. A bar that rejected
it would reject every FSK signal on the air and would sit below CW, AM and NFM.
The one clean split in the column is the PSK pair against everything else,
which is flat topped against not: a family split, not a signal-against-
interference one.

**And most of that spread is resolution rather than modulation.** The width
column gives it away. CW, AM, NFM, USB and LSB are line spectra and the
detector reports their lines separately: 135 candidates for AM over 45
decisions is three a decision, a carrier and two sidebands, and 675 for NFM is
fifteen, which is the Bessel comb. Each band is about 170 Hz, four or five bins
at 36.6 Hz. A band that narrow cannot have a shape, and `peak_to_mean` is
pinned near 2.3 by how the window spreads one line whatever produced it. The
number says something only about bands wide compared with the window.

**WHAT THIS PARAGRAPH USED TO CLAIM ABOUT `lower_fraction`.** It read:
"`lower_fraction` does what it was meant to at the one thing it can see here:
USB reads 0.453 and LSB 0.547, mirrored about a half in the right directions."

The mirroring is the reason to doubt it rather than to believe it. Those two
values mirror about a half to three decimal places, and so do 0.465 and 0.535
when the same scene runs on a grid four times finer. Two independent
measurements of two different signals do not agree to three places twice.

What is actually being measured: the scene's SSB emitters are two-tone at 700
and 1900 Hz, so USB is two lines above the carrier and LSB the same two
mirrored below, each reported as its own four-bin band. The number reads where
a line sits inside its own band, and mirrored lines mirror. AM moving from
0.464 to 0.372 between the two grids is the same instability from the other
side, a symmetric mode reading as the most asymmetric row in the table.

Nothing in this tree produces the 2.8 kHz asymmetric block a sideband test is
supposed to read, so the premise is untested rather than refuted.
`core/detect/shape.h` says so where a reader will meet it.

**`skirt_fraction` rules itself out as an absolute measure in the same table.**
FSK2 reads 0.432 through a perfectly linear front end, four times what a QPSK
signal reads while it *is* being driven into a cubic. Some modulations have
skirts. What separated was the change, 0.028 to 0.111 on the same emitters when
the nonlinearity was switched on, so a distortion flag built on this has to
compare a track with itself over time rather than against a constant.

**Back on the product scene, it says something the family table cannot.**
There it is flat across the parents and their products alike and rises on the
PARENTS when the front end is nonlinear, 0.028 to 0.111. That is spectral
regrowth: the band driving the nonlinearity is the one that smears into its own
neighbourhood. It is per-band evidence of distortion, which
`core/detect/front_end.h` says the span-wide monitor cannot give and which it
rejects frequency coincidence as a route to. Whether it is strong enough to
carry a per-detection flag is the next measurement rather than a conclusion.

**WHAT THE SECOND TIER USED TO READ AS WORK TO DO.** The line was: "**From a
narrowband extract**, for what shape cannot settle: envelope variance, tone
structure, symbol rate, cyclostationary features."

All four of those are written, tested and sitting in `core/characterise`,
which this document never named, so a reader planning the uplift off this
section would set out to build a library that exists. `characterise()` takes
complex baseband and answers with a
`ModulationFamily` of Unmodulated, AnalogueFm, Fsk, Psk or Ofdm, plus the
symbol rate, the tone structure, the modulation order and the OFDM frame where
it found them. Its own confidence is a monotone function of the measured
margin above each test's threshold, which is the calibrated shape the section
below asks for and the detector's stopwatch is not. The AnalogueFm branch caps
itself at half because it is an elimination rather than a positive finding.

So what is unbuilt is the seam and not the estimators: `characterise` reads
complex baseband, the detector reads spectrum frames, and nothing routes a
narrowband extract from one to the other. `Track::classification` is the field
that would carry the answer and its enum still has exactly one value. The
probe-receiver argument below is the design for that seam and remains the
part to do.

### The cheap half of the seam exists, and what it measured

`revenant-cli --characterise <hz>` places a raw receiver, collects one coarse
channel of complex baseband off the existing in-process audio fan-out and hands
it to `characterise()`. It is not the probe receiver designed below and does not
replace it. It exists because on a slow source the thing that design rejects is
much less bad: a coarse channel at 2.4 MS/s on a 64-channel grid is 37.5 kHz and
useless for this, and the same channel on a 96 kS/s HF source is 1.5 kHz, which
is narrower than an SSB signal.

Measured on 55 seconds of 40 m from `docs/recordings.md`, at five frequencies,
four of which the detector had found and one of which is empty band:

| offset | family | confidence | occupied | envelope variance | concentration |
| --- | --- | --- | --- | --- | --- |
| 193 Hz | unmodulated carrier | 0.54 | 1298 Hz | 20.50 | 0.544 |
| 7.192 kHz | PSK, 10.46 baud | 0.97 | 921 Hz | 4.63 | 0.449 |
| 11.132 kHz | PSK | 0.79 | 1550 Hz | 4.14 | 0.018 |
| 13.250 kHz | unmodulated carrier | 0.52 | 144 Hz | 4.23 | 0.521 |
| 30 kHz, empty | unknown | 0.00 | 1553 Hz | 4.79 | 0.005 |
| 41 kHz, empty | unknown | 0.00 | 1538 Hz | 4.82 | 0.006 |

**The empty band is the row that matters.** Two frequencies with nothing in
them answer `unknown` at zero confidence, with the estimators' refusals
attached. A stage that named a family everywhere would be worthless and this
one does not.

**No row here is known to be right.** There is no ground truth for this
recording, which `docs/recordings.md` says at length, so what the table shows is
that the stage discriminates and not that it is correct.

**Two tells for a non-answer, both visible above.** An occupied bandwidth near
1550 Hz is the whole coarse channel, which means `occupied_band` found no band
and returned everything; and a concentration near 0.005 with a family attached
says the family came from somewhere other than the spectrum. The 11.132 kHz row
has both and should be read as a shrug rather than as PSK.

**And one assumption in the estimators does not survive real HF.**
`CharacteriseConfig::constant_envelope_variance` is 0.05 and its comment says
circularly symmetric complex Gaussian noise reads exactly 1.0. Through this
path, on this band, empty noise reads 4.79 and 4.82. Atmospheric noise on 40 m
is impulsive rather than Gaussian and the channelizer shapes what arrives, so
the constant-envelope test cannot fire here at all, and with it the AnalogueFm
and Fsk branches that sit behind it. That is a property of real HF through a raw
tap rather than a defect in the estimator, and it is the sort of thing only a
recording was ever going to show.

### Mixing and filtering the extract, and the answer that got worse

The table above is an unmixed extract, so the signal sits wherever it sits
inside the channel. `--characterise` mixes it to DC now, and
`--characterise-width` will low pass it about DC first. Both are host arithmetic
in a command line tool, which is why they are allowed to exist at all.

**Mixing removed a confident wrong answer, which is the result it should have.**
Unmixed, 7.192 kHz read `4-PSK at 10.456 baud` with 0.97 confidence. Mixed to
DC it reads `unknown` at zero. `estimate_modulation_order` folds a carrier
offset modulo the rate over the order, so an offset extract hands the M-th
power law a line it did not earn; putting the signal at DC takes it away. The
refusal that replaces it is better than the answer it replaces.

**Filtering made it worse, and the prediction that it would help was wrong.**
The reasoning was that the 7.192 kHz signal sits at 0.444 concentration against
the 0.50 a carrier needs, with an envelope variance inflated by 1.5 kHz of
noise around it, so narrowing the extract should push it over. Measured across
three widths:

| offset | unfiltered | 100 Hz | 300 Hz | 800 Hz |
| --- | --- | --- | --- | --- |
| 7.192 kHz | unknown 0.00 | **PSK 1.00** | unknown 0.00 | **PSK 0.99** |
| 13.250 kHz | carrier 0.53 | carrier 0.55 | carrier 0.53 | carrier 0.53 |
| 30 kHz, empty | unknown 0.00 | unknown 0.00 | unknown 0.00 | **PSK 0.98** |

**The bottom right cell is the finding.** Empty band, filtered to 800 Hz, comes
back PSK at 0.98 confidence. A low pass narrow against the extract's own rate
colours the noise, and a cyclostationary detector reads the correlation that
introduces as a symbol rate. The stage is not wrong about its own arithmetic;
it is being handed something that is no longer noise and is answering about it.

The 7.192 kHz row says the same thing more quietly: a family that appears,
disappears and reappears as a filter width moves is not a property of the
signal.

**What was stable was the `Unmodulated` branch.** 13.250 kHz reads carrier at
0.52 to 0.55 through every width and unfiltered, because `spectral_concentration`
is a power ratio over the whole extract and does not care where the noise went.

### The extract's LENGTH changes the answer, and nothing says so

Both results above were partly confounded, and finding out why is the most
useful thing this exercise produced.

`analysis_segment` picks a transform length from the sample count: a sixteenth
of it, floored to a power of two, clamped to 512 and 32768. So a longer extract
gets finer bins. And `spectral_concentration` is the power in the strongest
THREE adjacent bins, which makes its window a frequency that shrinks as the
extract grows. At a 3 kS/s channel rate that window is 4.4 Hz over eleven
seconds and 1.1 Hz over sixty.

A real 40 m carrier, swept over extract length with everything else fixed:

| extract | 13.250 kHz, a carrier | 30 kHz, empty band |
| --- | --- | --- |
| 11 s | **unmodulated carrier**, concentration 0.531 | unknown, 0.005 |
| 20 s | unknown, 0.467 | unknown, 0.004 |
| 40 s | unknown, 0.288 | **OFDM**, 0.002 |
| 58 s | unknown, 0.184 | unknown, 0.002 |

**The carrier's concentration falls by a factor of three and crosses the 0.5
threshold between eleven and twenty seconds.** Nothing about the signal
changed. A carrier on HF drifts further than 1.1 Hz in a minute, between
transmitter stability and propagation Doppler, so over sixty seconds its power
is genuinely spread across more than three of those finer bins. The measure is
doing exactly what it says; what it says is a function of how much was handed
to it.

That undercuts a claim `spectral_concentration` makes for itself. Its comment
argues a power fraction is preferable to a frequency spread because "a power
fraction is the same number at any sample rate". It is not the same number at
any extract LENGTH, and on a drifting carrier that is the axis that matters.

**And empty band reads OFDM at forty seconds.** One length out of four, on
noise, with a confident family attached. Whatever `find_cyclic_prefix` locked
onto is an artefact of that particular segmentation; the point is that the
false positives are not only about filtering, and a single run at a single
length cannot tell you which kind of answer you have.

`--characterise-seconds` exists so the length is a stated parameter rather than
an accident. `--characterise-drift` fixes the other half: it states how far a
carrier may drift and still read as concentrated, derives the transform length
from that, and hands it over through `CharacteriseConfig::segment`, which is
new and defaults to the old behaviour.

With the segment stated from a 5 Hz allowance, the same sweep stops moving:

| extract | 13.250 kHz, a carrier | 30 kHz, empty band |
| --- | --- | --- |
| 11 s | unmodulated carrier, 0.760 | unknown, 0.008 |
| 20 s | unmodulated carrier, 0.713 | unknown, 0.008 |
| 40 s | unmodulated carrier, 0.726 | **OFDM**, 0.008 |
| 58 s | unmodulated carrier, 0.707 | unknown, 0.007 |

The carrier is named at every length and its concentration holds near 0.72
instead of collapsing from 0.53 to 0.18.

### What the two measurements agree about, and what the family call is worth

The standing direction is that detection should be driven by modulation rather
than by power. That makes one question worth answering before any of it is
built: **does the family core/characterise reports carry enough information to
decide anything?** On real HF the answer is that one of its numbers does and
the family name does not, and both halves were measured rather than argued.

Everything below is 20 m at 1603 UT, the 1.5 kHz coarse channel as the extract,
and a segment named from a 5 Hz drift allowance:

    revenant-cli "file:///.../kf4fic_14000_14350_1603_60s.wav" \
      --characterise <offset> --characterise-seconds 20 --duration 30

#### Where the detector and the characteriser agree

The detector measures a bandwidth from the spectrum and the characteriser
measures a family from complex baseband. They share no arithmetic, so where
they agree it is because they are both describing the same signal.

| detector width | centre | characteriser | concentration |
| --- | --- | --- | --- |
| 11 Hz | -10.947 kHz | unmodulated carrier | 0.700 |
| 15 Hz | 2.520 kHz | unmodulated carrier | 0.713 |
| 11.725 kHz | -35.388 kHz | unknown | 0.039 |

Two detections a few bins wide are called carriers, and the one that fills the
channel is refused. Nothing told the characteriser what the detector had
measured.

**The agreement stops at the channel edge.** At 96 kS/s on a 64-channel grid a
coarse channel is spaced 1.500 kHz and comes out at 3000 S/s, so anything the
detector measures wider than that does not fit in the extract and the answer is
about the fragment. The detector's own bandwidth is what says which case you
are in, and it is already on the same screen.

#### The family call fires on a third of the band

Thirty-one channels at 3 kHz spacing across the whole span, the same settings:

| family | channels | confidence | concentration |
| --- | --- | --- | --- |
| unknown | 16 | 0.00 | 0.008 to 0.272 |
| PSK | 10 | 0.52 to 0.98 | **0.019 to 0.356** |
| unmodulated carrier | 4 | 0.58 to 0.71 | 0.585 to 0.712 |

**Ten channels called PSK is not ten PSK signals.** Only one of the ten sits
near anything the detector found. Every one is order 2. Their symbol rates are
0, 0, 11.11, 104.22, 298.97 and 738.30 baud, with four carrying none at all,
which is not a population of modes in one band, it is a number with no signal
under it. Their occupancy runs 1.26 to 1.68 kHz in every case, which is the
extract's own width rather than any property of a signal.

**Concentration separates on the same sweep.** Four carriers between 0.585 and
0.712, everything else at 0.356 or below, and nothing at all in the gap
between. One number orders the band and the other does not.

#### Why, exactly, and it is not a bug

`ModulationOrder`'s own comment has it: squaring a tone gives another tone. A
carrier lights the M-th power line at exponent 2 the same way BPSK does. What
separates them is the envelope, and the envelope is the first thing the noise
takes. Once normalised power variance climbs past
`constant_envelope_variance`, the unmodulated branch is unreachable and the PSK
branch is the next one down.

`tests/characterise/test_characterise.cpp` walks one carrier down through noise
and catches it happening:

| SNR | family | confidence | concentration | power variance |
| --- | --- | --- | --- | --- |
| clean | unmodulated carrier | 1.00 | 0.998 | 0.000 |
| 13 dB | unmodulated carrier | 0.95 | 0.950 | 0.093 |
| 3 dB | unmodulated carrier | 0.66 | 0.664 | 0.560 |
| **-3 dB** | **PSK** | **0.98** | 0.333 | 0.885 |
| -9 dB | PSK | 0.88 | 0.112 | 0.984 |

The flip sits between +3 and -3 dB, and the wrong answer arrives at 0.98 while
the right answer never got above 0.95. **An operator reading the confidence
column would trust the wrong row harder**, which is the whole problem in one
line. Concentration over the same rows is 0.998, 0.950, 0.664, 0.333, 0.112,
against the 1/(1+N) that S/(S+N) predicts at each step: it is measuring the
signal-to-noise ratio and it keeps measuring it after the family name has
stopped meaning anything.

#### Carrying that number back into the detector

`spectral_concentration` is the one that works, and the detector holds a
spectrum of its own, so it does not have to wait for a probe receiver to have
it. `BandShape::concentration` is the same quantity over a detected band
instead of over a whole extract: its excess in the strongest three adjacent
bins, over its excess in total.

**`peak_to_mean` was tried there first and it does not do this job.** Measured
across 20 m at 1603 UT:

| detection | width | peak_to_mean |
| --- | --- | --- |
| a real carrier | 10 Hz | 2.23 |
| a real carrier | 15 Hz | 2.93 |
| a signal | 301 Hz | 4.11 |
| a patch of noise | 11.7 kHz | 68.95 |
| a carrier inside a wide band | 3.5 kHz | 250.64 |
| a carrier inside a wide band | 3.7 kHz | 405.45 |

It is the wrong way round. `peak_to_mean` rises with how much of the band is
**empty**, so a narrow band measured correctly is nearly all signal and scores
like noise by construction, which is what its own header warns about from the
other direction. What it is actually good at is the bottom two rows: a narrow
thing inside a wide reported bandwidth. That is a bandwidth being wrong, not a
signal being interference.

`concentration` is a fraction, so it does not grow with the width. The same
run, with the column the CLI now prints:

    #id  state            centre    bandwidth         snr   conf  margin    conc
    #1    live        -35.386 kHz   11.733 kHz      6.7 dB   1.00    0.55   0.02
    #2    live        -10.947 kHz        10 Hz     11.7 dB   0.98    0.81   0.82
    #83   live         -4.027 kHz    1.210 kHz      8.7 dB   0.88    0.68   0.28
    #54   live          1.313 kHz       809 Hz      9.4 dB   0.83    0.72   0.86
    #74   live          2.520 kHz        15 Hz     12.5 dB   1.00    0.83   0.59
    #69   live         12.626 kHz       288 Hz     14.3 dB   1.00    0.87   0.49

**Track #1 is the complaint, in one row.** An 11.7 kHz patch of raised noise
floor, sitting at confidence 1.00 because it has been there the whole time,
indistinguishable in that column from a station. Concentration reads 0.02
against 0.59 to 0.86 for the carriers. The operator's "just a bump in the noise
floor, all interference" now has a number beside it that says so.

**And it agrees with the other tier where both were asked.** Independently, on
the same three bands:

| band | detector | characteriser |
| --- | --- | --- |
| 11.7 kHz of noise | 0.02 | 0.039 |
| 10 Hz carrier | 0.82 | 0.700 |
| 15 Hz carrier | 0.59 | 0.713 |

Two measurements that share no arithmetic, one from an averaged power spectrum
and one from complex baseband, landing in the same place. Where they differ is
informative rather than contradictory: #54 reads 0.86 over its own 809 Hz band
and 0.267 over the 1.5 kHz channel around it, which says it is concentrated
within itself without dominating its neighbourhood, and only having both
numbers says that.

**Read it with the bandwidth, which is the one instruction it needs.** Under
about five bins it says nothing, because the analysis window spreads one line
over that many. Above that it separates a line spectrum from a filled one, and
the section after next is the measurement of that.

Nothing thresholds on it. `core/detect/shape.h` says why: what counts as high
is a measurement against known truth, and that has not been made.

**And it is not an interference test, which was checked rather than assumed.**
The product scene is the one place in the tree where a station and its own
third-order products appear in the same frames with truth known by
construction. Concentration across it:

| population | concentration | width |
| --- | --- | --- |
| parent stations | 0.079 | 2.8 kHz |
| their products | 0.033 | 7.7 kHz |
| artefacts elsewhere | **0.155** | 1.2 kHz |
| parents, linear front end | 0.047 | 3.5 kHz |

The population that is nothing at all reads highest, and a real station moves
from 0.047 to 0.079 on nothing but whether the front end is linear. Any bar in
that range keeps and drops both populations together.

That is the same answer `peak_to_mean` gets on the same scene, and for the same
reason: both are the band's power in its peak against its power in total, and
they differ only in whether the width divides in. Where every population is a
**filled** wideband band, both come down to how wide each one is.

**It bounds the claim rather than retracting it.** On real HF the separation
was not width alone: two bands of 809 Hz and 1210 Hz on the same span read 0.86
and 0.28. What concentration separates is a band that is nearly all signal from
a band that is nearly all floor, which is the complaint it was built for.
Telling a station from its own intermod product is a different question and no
field here answers it.

#### The same reading across the whole corpus

One excerpt is an anecdote. All six, run identically, are four populated lists
and two empty ones, which is twenty-five detections across two bands and three
hours of a day when conditions moved hard:

| band, hour | tracks born | live at the end | widths | concentration |
| --- | --- | --- | --- | --- |
| 40 m 1359 | 31 | 6 | 5 Hz to 188 Hz | 0.88 down to 0.28 |
| 40 m 1501 | 3 | 0 | | |
| 40 m 1603 | 0 | 0 | | |
| 20 m 1359 | 10 | 3 | 16 Hz to 8.6 kHz | 0.61 down to 0.06 |
| 20 m 1501 | 23 | 8 | 9 Hz to 11.4 kHz | 0.92 down to 0.06 |
| 20 m 1603 | 51 | 8 | 10 Hz to 11.7 kHz | 0.92 down to 0.02 |

**No band wider than 1.5 kHz read above 0.39, and no band narrower than 50 Hz
read below 0.33.** Twenty-five detections, four independent lists, two bands
and three hours. They overlap by six hundredths, which is why this is a
consistent reading and not a bar: a separating threshold would need a gap, and
there is not one.

**Confidence and margin do not do this.** The wide bands run confidence 0.92 to
1.00 and margin 0.55 to 0.65, so on those two columns they sit among the
stations exactly as reported. The eleven-kilohertz patch on 20 m at 1603 holds
confidence 1.00 for the whole minute.

**Two bands read far above what their widths would suggest.** On 20 m at 1603 a
489 Hz band read 0.82 and a 2.37 kHz band read 0.39, both out of order in a list
sorted by width. The next two sections are what those turned out to be.

#### What a high reading on a wide band actually is

The first guess was that the bandwidth was an overestimate and the detection
wanted splitting. `--detect-occupied` exposes the ITU fraction the reported
width holds, so the guess could be swept, and it does not survive:

| band on 20 m | 0.99 | 0.95 | 0.90 | 0.80 | 0.50 |
| --- | --- | --- | --- | --- | --- |
| the 10 Hz carrier | 10 Hz | 10 Hz | 8 Hz | 7 Hz | 4 Hz |
| the 15 Hz carrier | 15 Hz | 12 Hz | 10 Hz | 8 Hz | 3 Hz |
| **the 489 Hz band** | 489 Hz | 788 Hz | 474 Hz | **473 Hz** | **3 Hz** |
| the 11.7 kHz patch | 11.7 kHz | 10.4 kHz | 9.7 kHz | gone | gone |

**A carrier shrinks gently and the 489 Hz band does not.** It holds about
473 Hz all the way down to the eightieth percentile and then collapses to three
hertz at the fiftieth. Its power is bimodal: a narrow core holding about half,
and a distinct ring out to a couple of hundred hertz. A pedestal of noise would
have shrunk smoothly, and the eleven-kilohertz patch, which is a genuinely
filled band, does exactly that.

So the width is not an overestimate. **The band is a line spectrum**, and a
carrier with sidebands is what that looks like.

Swept finely, it is not even ambiguous. A continuous skirt would grow smoothly
between the core and the full width; this one steps:

| occupied fraction | 0.50 | 0.55 | 0.60 | 0.65 | 0.70 | 0.80 |
| --- | --- | --- | --- | --- | --- | --- |
| width | 3 Hz | 3 Hz | 3 Hz | 5 Hz | 4 Hz | **473 Hz** |

**Seventy percent of the band is in five hertz and there is nothing at all
between five and four hundred and seventy.** The next tenth of the power sits
in a pair about 235 Hz either side. That is a carrier and a discrete sideband
pair, measured off real 20 m rather than inferred, and it is what the
concentration of 0.82 was reporting.

#### Which makes it a family measurement, and the first one here that works

The family survey runs one emitter of each modulation through a linear front
end. With concentration added:

| family | concentration | what it is |
| --- | --- | --- |
| cw, am, nfm, usb, lsb | **0.946 to 0.947** | all too narrow to resolve |
| fsk2 | **0.519** | two tones, half the power in one |
| bpsk | 0.117 | filled |
| qpsk | 0.121 | filled |

**The first five are identical to three decimal places** and that is not a
result about them, it is the window: at 36.6 Hz the detector reports each of
their lines as its own five-bin band, and three bins of five is most of five
whatever produced it. Below about five bins this number says nothing, the same
way it says nothing below three.

**Above that it separates a line spectrum from a filled one.** 0.519 against
0.117 is more than four to one between a two-tone signal and a linear one, on
the same span in the same frames. That is the "separates the broad families"
that tier one is promised to give, and it is the first thing measured in this
tree that has done it: `peak_to_mean` reads 2.854, 1.793 and 1.703 over the
same three, which is the same ordering with a fifth of the separation and no
scale.

It still is not an interference test, for the reason two sections up. A line
spectrum can be an intermod product and a filled band can be a station.

#### And a finer grid does not rescue the five that cannot be told apart

The obvious next move is more resolution, so the same scene ran with a 2048
point second stage instead of 512: 9.16 Hz a bin against the shipped 36.6, four
times finer. The channelizer's twiddle builder caps the second stage at 2048,
so this is as far as it goes.

| family | concentration | width | bins | at 36.6 Hz |
| --- | --- | --- | --- | --- |
| cw | 0.860 | 55 Hz | **6.0** | 0.946, ~180 Hz |
| am | 0.942 | 43 Hz | **4.7** | 0.947, ~180 Hz |
| nfm | 0.946 | 43 Hz | **4.7** | 0.947, ~175 Hz |
| usb | 0.927 | 37 Hz | **4.0** | 0.946, ~165 Hz |
| lsb | 0.927 | 37 Hz | **4.0** | 0.946, ~165 Hz |
| fsk2 | **0.277** | 712 Hz | 77.8 | 0.519, ~862 Hz |
| bpsk | **0.056** | 1421 Hz | 155.2 | 0.117, ~1465 Hz |
| qpsk | **0.056** | 1409 Hz | 153.9 | 0.121, ~1430 Hz |

**The five narrow families are still five bins wide.** Their measured width
fell by four when the grid did, 180 Hz to 43, which means it was never the
signal's width at all. Each of those detections is one spectral line, and a
line is as wide as the analysis window makes it whatever the window is.

So resolution is not what is missing. **What separates AM from SSB from CW
lives in the relationship between the lines**, which is to say between
detections, and no number that measures a single band can reach it. That bounds
every per-band field in `core/detect/shape.h` the same way, not just this one.

#### And the lines as a set do separate them

That is worth nothing as a suggestion, so it was measured. Grouping the same
scene's detections by emitter and printing their offsets from each emitter's
own carrier, on the 9.16 Hz grid:

| family | lines | offsets in hertz, width beside |
| --- | --- | --- |
| cw | 1 | +0 (55) |
| bpsk | 1 | -2 (1410) |
| qpsk | 1 | -14 (1382) |
| usb | 2 | **+700** (37), **+1899** (37) |
| lsb | 2 | **-1899** (37), **-700** (37) |
| am | 3 | -1001 (46), **-3** (46), +999 (37) |
| nfm | 15 | -7001, -5999, -5001, -4003, -3000, -1998, a comb |

**Every row is the textbook answer.** AM is a carrier with a matched pair at
plus and minus its 1000 Hz tone. USB is the two modulating tones, 700 and 1900,
both above the carrier; LSB is the same two mirrored below. NFM is a comb at
the modulation frequency, which is what an FM sideband set is. CW is one line.
BPSK and QPSK are each a single filled band about 1.4 kHz wide.

**The count and the sign separate all five analogue families**, and neither is
a property of any single band. USB and LSB differ by nothing whatever except
which side their lines sit on, which is exactly the sideband test
`lower_fraction` was meant to give and has never been shown to give.

So tier one's promise is reachable from the spectrum alone. What it needs is a
surface that does not exist: the detector publishes a flat list of tracks and
nothing groups them, and the one thing this survey cannot do, following a line
that wanders, is the tracker's job rather than a measurement's. FSK2 is where
that shows: its tones move, so folding one line seen across forty decisions
into one entry by position overcounts it at fifty.

#### The same view on real air, which is where the question gets asked

`revenant-cli --detect-groups <hz>` brackets tracks sitting within a stated gap
and prints where they sit relative to the strongest of them. It is a display
arrangement: nothing on the engine knows about it, no field carries it, and the
gap is an argument because no measurement has chosen one. A number chosen in
the CLI would be a classification smuggled in as a layout.

40 m at 1359 UT, grouping at 500 Hz:

    #45   merged       13.238 kHz         5 Hz     11.7 dB   0.88  0.81  0.88
    #1    live         13.249 kHz        29 Hz     22.3 dB   1.00  0.97  0.41
      2 lines across 11 Hz, from 13.249 kHz: -11 Hz(8s) *0 Hz(42s)

Two rows of a flat table brought together: a strong line at 13.249 kHz with a
five-hertz neighbour eleven hertz below it. Read one row at a time neither
points at the other.

**The ages are the first thing to read and they are why this one is not a
structure.** The anchor has been there forty-two seconds and its neighbour
eight, so what the group shows is a stable carrier with something transient
beside it rather than a carrier and a sideband. A real pair would have aged
together. That reading is available in the line itself, which is the whole
reason the age is in it.

**No family pattern has come out of this corpus yet, and that is the result
rather than a pending one.** All six excerpts at gaps of 300 Hz, 1 kHz and
3 kHz produce four groups in total, none of them the carrier-with-a-matched-
pair or the comb the scene shows. The cluster above is the only one that
persists, and it is a strong line with close neighbours rather than anything
symmetric.

**And the groups move between runs of the same file**, which is a trap worth
naming before somebody reads two of these tables side by side. On 40 m at 1359
a gap of 300 Hz grouped three lines and a gap of 1 kHz grouped two, and a wider
gap cannot group fewer. What changed was the track list: a file source runs as
fast as the GPU retires it, so the instant the final table is sampled is not
the same twice, and the population of a live HF band at that instant is not
either.

That is the same limit the survey hit from the other side. Reading a set of
lines needs those lines to be there together, and following one that fades and
returns is the tracker's job. A grouping over one snapshot is the cheap half.

**The three that are resolved separate more sharply than before**: fsk2 at
0.277 against 0.056 is five to one, where the shipped grid gave four to one. A
finer grid helps exactly the bands that were already wide enough for the number
to mean anything, and does nothing for the rest.

#### So what a modulation-driven detector can stand on today

- **`spectral_concentration` can carry a decision.** It is scale-free, it is
  monotone in SNR, it degrades instead of flipping, and on this band it
  separates carriers from everything else with a clear gap.
- **`family` cannot, on its own.** A PSK call is what this stage says about a
  weak carrier, and it says it confidently.
- **`family_confidence` is not a quality measure** and ordering a list by it
  puts the worst signals at the top.
- A PSK call carrying no symbol rate, or one implausible for its band, is the
  shape of the artefact and is the cheapest thing to gate on if a gate is
  wanted before the probe receiver exists.
- **And the detector does not have to wait for any of it.** `BandShape::
  concentration` is the same measurement off the spectrum the detector already
  holds, on every track, at no extra cost and with no receiver involved. The
  tier that needed a probe receiver is the one that names a family; the number
  that separates a station from a raised patch of floor was available all
  along.

None of this is an argument against driving detection from modulation. It is
the measurement of what the current extract supports.

**WHAT THIS PARAGRAPH USED TO CLAIM, AND THE TEST THAT REFUSED IT.** It ended:
"and it is the second argument for the probe receiver: sized to the signal
rather than to the grid, the envelope test is being asked about the signal
instead of about the channel around it." That was written before it was tried
and the section below is what happened when it was.

### Narrowing the extract does not rescue the family call

The probe receiver's whole payoff was supposed to be an extract sized to the
signal. `--characterise-width` already narrows one, so the payoff could be
measured before anything was built. Four widths, 55 seconds so the sample floor
never caps the decimation, a stated drift allowance, on 20 m at 1603 UT:

| target | unfiltered | 800 Hz | 300 Hz | 100 Hz |
| --- | --- | --- | --- | --- |
| a carrier, 10 Hz | carrier 0.68 | carrier 0.69 | carrier 0.61 | carrier 0.53 |
| a carrier, 15 Hz | carrier 0.70 | carrier 0.70 | carrier 0.64 | carrier 0.51 |
| **an empty channel** | **unknown** | **PSK 0.67** | **PSK 0.81** | unknown |
| a channel reading PSK | PSK 0.86 | unknown | PSK 0.80 | unknown |
| another reading PSK | PSK 0.86 | PSK 0.70 | PSK 0.77 | unknown |

**The carriers survive it and nothing else does.** Both are named at every
width, which is the good half. The empty channel is the bad half: it answers
correctly unfiltered and, narrowed, manufactures a confident PSK call that was
not there. The two channels that read PSK flip between PSK and unknown with no
pattern.

**The empty channel is the control and it is genuinely empty**, at
concentration 0.007, which is as close to nothing as this corpus has.

#### Where the false carrier comes from, and why a sharper filter is worse

Both false calls report **no symbol rate at all**, and both name a carrier just
past the filter's own cutoff:

| width | cutoff | reported carrier | ratio |
| --- | --- | --- | --- |
| 800 Hz | 400 Hz | -422.6 Hz | 1.06 |
| 300 Hz | 150 Hz | +172.1 Hz | 1.15 |

That is where the transition band of a 101-tap windowed sinc sits, so the
obvious move was a sharper filter. **It was tried and it made the artefact
stronger**, which is the finding rather than the fix: at 233 and 621 taps, sized
to put the transition inside a tenth of the cutoff, both channels went from
0.67 and 0.81 to **0.94 and 0.94**, and the reported carrier moved from -422.6
to -410.25, closer in as the transition narrowed.

A sharper edge is a sharper spectral discontinuity. Squaring band-limited noise
gives a broad triangular spectrum with a corner at the band edge and no line in
it, and `estimate_modulation_order` compares a peak against a LOCAL baseline,
which a corner defeats. The tap change was reverted, because the filter was
made better at filtering and worse at the only job this flag has.

**So the fault is a baseline estimate at a spectral discontinuity, and it lives
in `core/characterise` rather than in the filter.** That is the same class as
the OFDM branch below, and it gets the same treatment: written down rather than
changed, because those constants were each measured against a signal and a
theory-led edit to them is not an improvement.

#### What that does and does not say about the probe receiver

It does not say a probe receiver would fail. The design section below already
concluded that tier two has to use a real `DemodStage` rather than the raw tap,
precisely so the extract is mixed to DC and filtered near the signal's
bandwidth, and that stage's fine filter is a planned polyphase one rather than
a hundred taps of direct convolution. A different filter puts the corner
somewhere else.

It does say **the payoff cannot be assumed, because it was assumed here and the
measurement refused it.** A narrower extract did not stabilise the family call;
on empty spectrum it made it confidently wrong. Anything built to feed
`core/characterise` a tighter extract should be able to show, on a channel with
nothing in it, that the answer stays "unknown". That is now a cheap test and it
should be run before the seam is built, not after.

And it sharpens the standing conclusion rather than softening it. Two carriers
were named correctly at every width; everything that was not a carrier moved
around. The family call is the wrong thing to hang detection on, and narrowing
the extract is not what fixes it.

### The OFDM branch still fires on noise, and why that one is not mine to fix

The 40 second cell survives, and it is a different mechanism that the segment
does not touch. What it reported:

    OFDM: a 679.7 ms useful symbol, so 1.4713 Hz between subcarriers, with a
    guard of about 44 samples read off a correlation of 0.021

A correlation of 0.021 is not evidence of anything. `OfdmSearch` requires a
peak above the larger of `min_prefix_ratio`, which is 0.01, and
`floor_multiple` times the profile's own median, which is six times. On flat
noise the median collapses, so the multiple stops binding and the absolute
floor is all that is left.

**The floor is not the thing to raise**, and its comment says why with a
measurement: on a one-eighth-guard burst at 48 kS/s, 0.01 reaches 5 dB in the
reference bandwidth and 0.02 stops at 10 dB. Doubling it costs five decibels of
reach on real OFDM to reject this.

**What actually fails is the multiple.** Six times the median is a fixed bar
against a maximum taken over every lag in the search, and the expected maximum
of a set grows with the size of the set. More lags, which is what a longer
extract buys, means a higher maximum from the same noise. That is why the call
appears at forty seconds and not at eleven, twenty or fifty-eight: it is the
tail of a distribution being sampled more times, not a property of the band.

A bar that held would have to grow with the lag count the way an extreme value
does, rather than sit at a constant multiple of the middle of the distribution.
That is a change to a tested library whose constants were each measured against
a signal, and it wants the same treatment rather than an argument from theory,
so it is written down here instead of made.

**So none of this goes on the wire.** `core/rpc/revenant.capnp` already refuses
`logicalCentreHz` on the grounds that a field nothing can fill is worse than no
field, and a family field filled from this would be worse still: it would be
filled confidently and wrongly. What the exercise establishes is that the seam
runs and what it would take to trust it, which is a narrower extract that came
from a real receiver rather than a low pass over a coarse channel. That is the
probe receiver below, and this is the measurement that says why it is worth
building rather than a cheaper thing.

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
on the full span produces baseband offsets and adds `source_center` before it
publishes, so a track carries absolute radio frequency. `VrxParams::center`
does not: it is hertz from the source's baseband DC, `place()` reads it that
way, and nothing between a caller and the grid rebases it. Tuning a track is
therefore `params.center = center_hz - source_center`, which is the conversion
`revenant-cli` does for a frequency the operator types and the one
`tests/rpc/test_rpc_detect.cpp` pins from both sides: it tunes a detection by
subtracting, and it checks that passing the absolute centre straight through
is refused.

An earlier version of that paragraph said `VrxParams::center` "is documented as
absolute while `place()` reads it as an offset and the engine is expected to
rebase". Both halves are retracted. `core/engine/vrx.h` did document the field
as absolute, and that documentation was the error rather than the code, so it
was corrected on 2026-09-19 and the header now says baseband. The second half
was never true at all: no build of this engine has rebased, and `place()` is
handed the grid, the rate and the request without ever being told where the
source is tuned, so it has nothing to rebase against. The correction is
recorded rather than swapped in because the same claim stood in
`core/rpc/revenant.capnp` and `core/rpc/types.h`, and a reader who took it from
any of the three is owed the retraction.

**A bandwidth wider than one channel does not fail, which is worse.**
`place()` sets `bandwidth_clamped` and succeeds, `plan_vrx` silently takes the
minimum of requested and available, and `add_vrx` returns an id. The operator
gets a receiver narrower than the signal they clicked with no error anywhere,
and `VrxStatus::params` reports the bandwidth asked for rather than the one
delivered. Click-to-tune has to read `placement.bandwidth_clamped` and say so.

**What a click resolves against exists; what it cannot reach is the
classifier.** `Engine::set_spectrum_sink` delivers the frames, the detector
runs on them on the RPC server's side, and `Session::detections` publishes the
track list a client filters by confidence. `ui/render/spectrum_item.cpp`
resolves a click against it and tunes a receiver. What there is still no route
for is complex baseband to a classifier.

This paragraph used to say there was no Engine surface for any of it and that
the track list was exposed by nothing. That was true when it was written and
stopped being true with the RPC session. It is retracted here rather than
deleted, because a reader taking it at face value would go and build a second
surface beside the one that exists.

## What has to exist first

The spectrum stage and an Engine surface carrying spectrum frames were both on
this list and are both done: the stage is the per-channel second transform
`EngineConfig::spectrum_transform` sizes, and the surface is
`Engine::set_spectrum_sink`, with `Session::detections` carrying the tracks
the rest of the way. They are struck here rather than dropped, because the
sizing argument above was written to constrain a stage that did not exist and
reads differently once it does.

`Demod` gaining the modes this document keeps using as examples. It currently
holds `Raw, Am, Nfm, Wfm, Usb, Lsb, Dsb, Cw`: no RTTY, no FSK, no PSK. The
enumeration is a frozen contract and its value is the demodulator kernel's
specialization constant, so growing it is a deliberate change rather than an
edit.
