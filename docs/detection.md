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
and click-to-tune is built in `ui/`. Identification's second tier is built as
of 2026-09-23: `core/engine/probe.h` places probe receivers on detections,
`core/detect/tier_two.h` schedules them, and `Track::classification` carries
the family they find. "The probe receiver, built and measured" near the end
has what it gets right and what it gets wrong.

WHAT THE LAST SENTENCE OF THAT PARAGRAPH USED TO SAY: "identification is the
part that is still design, and `Track::classification` is the seam nothing
fills."

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
RTX 4090, and at eight bytes per `Complex32` that is 4096 points. The 4090 is
the one device the project supports and tests; the integrated Radeon was
dropped from both on 2026-09-22 by the owner's decision. A single transform
across twenty megahertz at any useful resolution is sixteen to sixty times
outside that, and `fft.md` explicitly declines to serve it: the alternatives
it names are a multi-pass kernel nobody has written or VkFFT with a documented
loss of bit-exactness.

Neither is needed, because the channelizer already did most of the work. A
second-stage transform of each coarse channel's time series reaches the same
total bin count in pieces that fit:

| Per-channel transform | Shared memory | Bin width at 20 MS/s | Total bins |
| --- | --- | --- | --- |
| 2048 | 16 KiB of the 4090's 48 | 305 Hz | 65536 |
| 4096 | 32 KiB of the 4090's 48 | 153 Hz | 131072 |

The 4096 row fits the device and is still not buildable:
`dsp::kMaxSpectrumTransform` is 2048, because the twiddle table the spectrum
stage shares with the channelizer stops there.

WHAT THIS SECTION USED TO SAY, when two devices were supported: the kernel's
shared memory was "48 KiB on the discrete card and 32 KiB on the integrated
one, and at eight bytes per `Complex32` that is 4096 points on one and 2048 on
the other", and the table's two rows read "16 KiB, fits both devices" and
"32 KiB, discrete only". The arithmetic is unchanged; the second device is no
longer part of what the project claims to run on.

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
the job of the identification below. The label on the wire names what a probe
found, which is not the same question: an intermod product of a BPSK station
can be named BPSK, correctly.

WHAT THE LAST SENTENCE USED TO END WITH: "and nothing on the wire does it
yet." Since 2026-09-23 a label is on the wire and it does not do this either.

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
`revenant-engine` prints the substitution under the ring line, and so does
`revenant-cli`, which leaves the choice to the engine on HF unless
`--channels` names a count.

WHAT THE LAST SENTENCE USED TO SAY: "`revenant-cli` pins 64 channels by
default and takes `--channels 0` to ask for this." Since "Let the engine
choose the channel count on HF in revenant-cli" the 64 holds above 30 MHz
only; "The probe receiver, built and measured" has what the pin cost tier two.

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

**The whole files say the same, and supersede the one-minute table above.**
Swept over 40 m at 1359 UT and 20 m at 1603 UT whole, from 2 bins to 32
nothing moves by more than 5.1 percent, 20 m's splits going from 538 to 512;
between 32 and 128 bins there is a step, merges climbing 25 percent on 40 m and
50 percent on 20 m by 200 bins while births fall 3 to 6 percent, and there is
no knee inside either range. `docs/recordings.md` has the table.

**RTTY does what this section predicted, but only when it is strong.**
Synthetic 45.45 baud, 170 Hz shift RTTY through the same HF path splits into
two tracks 171 Hz apart at 15 dB in the 2500 Hz reference, and at 8 dB is one
detection 163 to 238 Hz wide. `--characterise` at its centre calls the 15 dB
signal 2-FSK with its tones 169.62 Hz apart but no symbol rate, so it never
matches the catalogue's RTTY row, and calls the 8 dB one 4-PSK at 0.92, which
is wrong. That second call is the kind the PSK rules further down were written
for: if it carried no symbol rate it now reads 0.50 flagged and may not drive
detection, and if it carried one only the carrier-power rule could refuse it.
Which it was has not been measured. A synthetic RTTY signal made directly at
the 3 kS/s channel rate at 15, 11 and 8 dB, nine extracts, came back unknown
every time, so the 4-PSK call depends on something in the HF path that the
direct extract does not have.

**And none of the six recordings holds RTTY**, scanned whole for both shapes,
a pair of lines a shift apart and one band 150 to 360 Hz wide. Every candidate
that lasted was a carrier with sidebands, a PSK or voice signal, or a line with
a neighbour merged into it; `docs/recordings.md` lists them. So
`split_gap_bins` stays open: the RTTY case that would test it has been measured
on a synthetic signal and does not exist in the corpus.

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

**WHAT THIS PARAGRAPH USED TO SAY ABOUT `lower_fraction`.** It read:
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

So what was unbuilt was the seam and not the estimators: `characterise` reads
complex baseband and the detector reads spectrum frames. The probe receiver
routes a narrowband extract from one to the other now, and
`Track::classification` carries the families `characterise` names; "The probe
receiver, built and measured" below is the record.

WHAT THIS PARAGRAPH USED TO SAY, after its first clause: "nothing routes a
narrowband extract from one to the other. `Track::classification` is the field
that would carry the answer and its enum still has exactly one value. The
probe-receiver argument below is the design for that seam and remains the
part to do."

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

The flip sits between +3 and -3 dB, and the wrong answer arrived at 0.98 while
the right answer never got above 0.95. Both PSK rows carry no symbol rate, so
since 2026-09-22 they read 0.50, flagged, under the rule the next sections
record: the right rows now outrank the wrong ones.

WHAT THIS PARAGRAPH USED TO SAY, when those rows read 0.98 and 0.88: "An
operator reading the confidence column would trust the wrong row harder",
which it called the whole problem in one line.

Concentration over the same rows is 0.998, 0.950, 0.664, 0.333, 0.112,
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
and one from complex baseband, landing in the same place.

The same check on 40 m at 1359 UT, which is a different band on a different
hour:

| detection | width | detector | characteriser | family |
| --- | --- | --- | --- | --- |
| 193 Hz | 11 Hz | 0.77 | 0.717 | unmodulated carrier |
| 7.192 kHz | 19 Hz | 0.60 | 0.663 | unmodulated carrier |
| 15.194 kHz | 41 Hz | 0.33 | 0.382 | PSK at 0.97 |
| 13.249 kHz | 29 Hz | **0.41** | **0.700** | unmodulated carrier |

Three of the four agree within seven hundredths. **The fourth is the one with a
neighbour**, and it disagrees in the direction that says so: 13.249 kHz has a
five-hertz line eleven hertz below it, so the detector's 29 Hz band holds two
lines and its three-bin window catches one of them, while the characteriser
over the whole 1.5 kHz channel sees the carrier dominate. A gap between the two
numbers is a band with structure in it, which is the same reading the grouping
below gives from a different direction.

The 15.194 kHz row is the artefact again: a weak carrier at concentration 0.38
called PSK at 0.97. Where they differ is
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

So tier one's promise is reachable from the spectrum alone. What it needed was
a surface that follows the lines as a set, because the detector publishes a
flat list of tracks, and the one thing this survey cannot do, following a line
that wanders, is the tracker's job rather than a measurement's. FSK2 is where
that shows: its tones move, so folding one line seen across forty decisions
into one entry by position overcounts it at fifty.

That surface is `core/detect/groups.h`, described in the next section. WHAT
THIS PARAGRAPH USED TO SAY: "What it needs is a surface that does not exist:
the detector publishes a flat list of tracks and nothing groups them".

#### The surface: a group of tracks with an identity of its own

`detect::LineGrouper` takes `Detector::tracks()` after each decision and chains
tracks whose centres sit within a stated gap of a neighbour. A chain of two or
more is a group, and a group keeps its id from one decision to the next by the
track ids it shares with the previous decision's groups: most shared wins, the
older group on a tie, so a group that splits leaves its id with the half
carrying more of its lines. Each group carries its members ascending in
frequency, each member's offset from the strongest line and spacing from the
one below, when each member joined, `together_since`, which is the latest join
and so the decision since which the current set has been whole, and
`birth_spread`, how far apart the members' own births were. Live and Held
tracks are grouped, so a line fading inside the tracker's hold stays in its
group; Merged ones are not, because the track that swallowed one carries the
same energy and counting both counts one line twice.

It classifies nothing and puts nothing on the wire. The gap is the caller's,
because no measurement has chosen one.

`tests/detect/test_groups.cpp` runs the family scene through the detector at
the shipped 36.6 Hz, groups at 3 kHz, and pins what the hand survey above found:

| family | lines | offsets from the strongest line, in hertz |
| --- | --- | --- |
| am | 3 | -989, **0**, +1017 |
| nfm | 15 | -11004 to +3003, a comb, every spacing 989 to 1025 |
| usb | 2 | -1191, **0** |
| lsb | 2 | **0**, +1191 |
| fsk2 | 9 | -1547 to +6371, no pattern: its tones wander |
| cw, bpsk, qpsk | none | one line, or one filled band |

Five groups formed over 46 decisions and none ended, every one whole for the
last 4.7 seconds with its lines born in the same decision. The am group holds
one id from the first decision it had all three lines to the last.

**It cannot tell USB from LSB in general.** Relative to the carrier they differ
only in which side their lines sit, and a suppressed-carrier signal has no
carrier in the group to be relative to. They come out mirrored in the table
because the 1900 Hz tone measures louder than the 700 Hz one in both, so the
anchor is the outer line each time; two tones of equal level would leave the
sign to measurement noise.

#### The same view on real air, which is where the question gets asked

`revenant-cli --detect-groups <hz>` now prints what `LineGrouper` follows:
each group's id, how long its current lines have all been in it, how far apart
their births were, and every line's offset from the strongest with its own age.
The run summary adds what no single table can: groups formed and ended, joins
and leaves, and the longest any group held all of its lines.

WHAT THIS SECTION USED TO SAY about the flag: "It is a display arrangement:
nothing on the engine knows about it, no field carries it". It bracketed rows
of one snapshot inside the CLI. The first half stopped being true when the
grouping moved into `core/detect`; no field on the wire carries it still.

What the display-side grouping printed, 40 m at 1359 UT, at 300 Hz:

    #45   merged       13.238 kHz         5 Hz     11.7 dB   0.88  0.81  0.88
    #1    live         13.249 kHz        29 Hz     22.3 dB   1.00  0.97  0.41
      2 lines across 11 Hz, from 13.249 kHz: -11 Hz(6.8s) *0 Hz(41.6s)

Two rows of a flat table brought together: a strong line at 13.249 kHz with a
five-hertz neighbour eleven hertz below it. Read one row at a time neither
points at the other.

**The ages are the first thing to read and they are why this one is not a
structure.** The anchor has been there 41.6 seconds and its neighbour 6.8, so
what the group shows is a stable carrier with something transient
beside it rather than a carrier and a sideband. A real pair would have aged
together. That reading is available in the line itself, which is the whole
reason the age is in it.

That example is the old display's, and #45 would not be grouped now: it is
Merged, and the surface leaves merged tracks out.

WHAT THE NEXT TWO PARAGRAPHS USED TO SAY, off that snapshot grouping. "All six
excerpts at gaps of 300 Hz, 1 kHz and 3 kHz produce four groups in total, none
of them the carrier-with-a-matched-pair or the comb the scene shows." And "And
the groups move between runs of the same file": on 40 m at 1359 a gap of
300 Hz grouped three lines and a gap of 1 kHz grouped two, because the instant
the final table was sampled was not the same twice. Both were true of four
snapshots and neither survives following every decision.

**The same corpus through the surface**, all six excerpts, 55 seconds each,
default thresholds, the run summary's groups formed and the longest any one
group held all of its lines:

| band, hour | 300 Hz | 1 kHz | 3 kHz |
| --- | --- | --- | --- |
| 40 m 1359 | 7, 9.6 s | 7, 11.6 s | 7, 21.8 s |
| 40 m 1501 | none | none | none |
| 40 m 1603 | none | none | none |
| 20 m 1359 | none | none | 3, 7.5 s |
| 20 m 1501 | 1, one decision | 2, 2.7 s | 4, 6.8 s |
| 20 m 1603 | 10, 10.2 s | 11, 10.2 s | 13, 8.9 s |

**The groups no longer move between runs of the same file.** 40 m at 1359 at
3 kHz, twice: 31 tracks born, 7 groups formed, 6 ended, 15 joins, 19 leaves and
21.8 s both times. The grouper sees every decision, so the instant a table
happens to be sampled no longer decides what a group is; the tables printed
during a run still depend on it.

**Symmetric pairs round six carriers, which the snapshots never showed.** A
line nine to twelve hertz either side of a carrier, on both bands: at 193 Hz,
7.192 kHz and 13.250 kHz on 40 m, and at -10.948 kHz, 2.520 kHz and
8.326 kHz on 20 m. Three of them:

    40 m 1359, 300 Hz   193 Hz        -9 Hz(2.0s)  *0 Hz(40.3s)  +11 Hz(2.0s)
    20 m 1603, 300 Hz   2.520 kHz    -10 Hz(2.7s)  *0 Hz(3.4s)   +11 Hz(3.4s)
    20 m 1603, 3 kHz    -10.948 kHz   -9 Hz(6.8s)  *0 Hz(11.6s)  +10 Hz(6.8s)

That is the carrier-with-a-matched-pair shape at a hundredth of the scene's
spacing, the shape the snapshot grouping said the corpus did not contain.
**Two of those three pairs arrived after their carrier**, by 38.2 seconds on
40 m and 4.8 on 20 m, each pair's two lines born in the same decision; the
third was born within 0.7 seconds of a carrier track that was itself only
3.4 seconds old.

Six carriers doing the same thing points at the analysis before it points at
the transmitters, so the detector was checked first: one steady carrier at
7.19 kHz on the same 1.465 Hz grid, 16 dB in the reference bandwidth and
nothing else on the span, run for 40 seconds (`groups survey: one steady
carrier at the HF grid` in `tests/detect/test_groups.cpp`). One track at every
one of 55 decisions, no second track within 50 Hz at any of them, no group.
**A steady line does not make the pair.** A carrier that drifts, and the
ionosphere, are both still open, and no row here is known to be right.

**The longest-held set is a carrier and one line above it**, on 40 m at 1359
at 3 kHz: 13.249 kHz and a line 1.7 to 1.9 kHz up, whole for 21.2 seconds and
born 21.8 seconds apart. No comb and no matched pair at a family's spacing
appears anywhere.

**Reading a set of lines needs those lines to be there together**, and the
table says how rarely that holds on this corpus: at 300 Hz no set of lines on
four of the six excerpts stayed whole past a decision or formed at all.

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
- **A PSK call carrying no symbol rate is kept, flagged and held to 0.50**, by
  the owner's decision of 2026-09-22, and `characterise::may_drive_detection`
  refuses it. It is not refused outright, because real BPSK loses its rate
  before its order. "A PSK call with no symbol rate" below has the margins the
  cap was read from.
- **A PSK call whose carrier sits at its occupied band's edge is not made at
  all.** That is the narrowed-noise artefact, and 22 of 62 such calls on
  synthetic empty channels carried a symbol rate, so the flag alone would not
  have caught them. "The empty-channel gate, run" below.
- **And the detector does not have to wait for any of it.** `BandShape::
  concentration` is the same measurement off the spectrum the detector already
  holds, on every track, at no extra cost and with no receiver involved. The
  tier that needed a probe receiver is the one that names a family; the number
  that separates a station from a raised patch of floor was available all
  along.

WHAT THE FOURTH BULLET USED TO SAY: "A PSK call carrying no symbol rate, or one
implausible for its band, is the shape of the artefact and is the cheapest
thing to gate on if a gate is wanted before the probe receiver exists." Over a
third of the artefact carries a rate, and a missing rate is also the shape of
a weak real signal; the two bullets that replace it are what the measurement
supported instead.

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
in `core/characterise` rather than in the filter.** No constant of the order
estimator was changed for it, because each was measured against a signal. What
was added instead is a rule about where the reading lands, stated from the
physics and measured on both sides, in "The empty-channel gate, run" below.

WHAT THIS PARAGRAPH USED TO SAY: "it gets the same treatment: written down
rather than changed". The estimator is still unchanged; the characteriser now
refuses the call the corner produces.

#### What that does and does not say about the probe receiver

It does not say a probe receiver would fail. The design section below
concludes that tier two needs the fine stage rather than the raw tap,
precisely so the extract is mixed to DC and filtered near the signal's
bandwidth, and that stage's fine filter is a polyphase one rather than a
hundred taps of direct convolution. A different filter puts the corner
somewhere else.

It does say **the payoff cannot be assumed, because it was assumed here and the
measurement refused it.** A narrower extract did not stabilise the family call;
on empty spectrum it made it confidently wrong. Anything built to feed
`core/characterise` a tighter extract should be able to show, on a channel with
nothing in it, that the answer stays "unknown". That is now a cheap test and it
should be run before the seam is built, not after.

It was run through the seam once it was built, on the probe receiver's own fine
stage rather than a low pass on the host: the empty slot in
`tests/engine/test_engine_probe.cpp` came back unknown in 12 of 12 runs, three
seeds at each of four emitter levels. "The probe receiver, built and measured"
below has the rest.

And it sharpens the standing conclusion rather than softening it. Two carriers
were named correctly at every width; everything that was not a carrier moved
around. The family call is the wrong thing to hang detection on, and narrowing
the extract is not what fixes it.

### The empty-channel gate, run

The test the paragraph above asked for is
`tests/characterise/test_empty_channel.cpp`. Synthetic noise, so every case
knows there is nothing in it, narrowed exactly the way `--characterise-width`
narrows, at the 3 kS/s HF channel rate: Gaussian, and Gaussian with an impulse
of power 25 on one sample in a hundred, which puts the normalised power
variance at 4.92 against the 4.79 and 4.82 empty 40 m reads. Unfiltered and at
800, 300 and 100 Hz, 20 and 55 seconds, two levels eight decades apart as a
control, three seeds a cell, 96 channels:

| | unfiltered | 800 Hz | 300 Hz | 100 Hz |
| --- | --- | --- | --- | --- |
| before, named | 0 of 24 | 24 of 24 | 24 of 24 | 14 of 24 |
| after, named | 0 of 24 | 0 of 24 | 0 of 24 | 0 of 24 |

**The synthetic noise reproduces the real artefact exactly**: every call PSK
of order 2, the carrier just past the cutoff, at ±422 Hz against 400, ±157 to
±172 against 150 and ±63 to ±66 against 50. The two levels gave identical
answers in every cell, and the impulsive noise changed nothing but the 100 Hz
count. **And 22 of the 62 calls carried a symbol rate**, at up to 0.98, so the
missing rate is not the shape of this artefact; the carrier's position is.

So the rule is about where the carrier sits.
`CharacteriseConfig::psk_carrier_level_fraction` refuses a PSK call when the
extract's own power at the named carrier, at its loudest M-fold alias, is under
half the median power across the occupied band. A linear modulation's spectrum
peaks at its carrier; band-limited noise squared has a corner, not a line,
where the band stops, and the carrier the power law reads off a corner sits
where the band's power has already fallen away. Measured either side:

| population | carrier's power over the band's median |
| --- | --- |
| the 62 order lines on empty channels | 0.195 at most |
| real BPSK and QPSK at 0, +3 and -7 kHz, 30 to 0 dB in 2500 Hz | 0.88 at least; 2 to 110 once noise fills the extract |

A half is a factor of 2.6 inside each. **A distance rule was tried first and
refused a real signal**: the artefacts all sit 0.45 to 0.56 of the band's width
from its centre, and real carriers mostly within 0.2, but BPSK at -7 kHz and
20 dB came back with its noise-filled band centred at +6 kHz and its carrier
0.34 of the width away. Power at the carrier does not move with where the
noise drags the band.

On real air, through the CLI at 55 seconds: empty 40 m at 30 kHz, which read
PSK narrowed before, is unknown at 800 and 300 Hz with the carrier on 0.03 of
the band's median, and the two 20 m carriers at -10.947 kHz and 2.520 kHz read
unmodulated carrier at 0.68, 0.69, 0.61, 0.53 and 0.70, 0.70, 0.64, 0.51 across
the four widths, identical to the table above.

### A PSK call with no symbol rate: kept, flagged, capped

The owner's decision of 2026-09-22: such a call is kept rather than refused,
marked `Characterisation::psk_without_symbol_rate`, held to
`kPskWithoutRateConfidence`, 0.5, and refused by
`characterise::may_drive_detection`. The summary says so and
`revenant-cli --characterise` prints a `flagged` line.

Kept, because real PSK loses its rate before its order. 2400 baud BPSK at 48 kS/s:

| population | rate | confidence uncapped | order margin |
| --- | --- | --- | --- |
| BPSK 30, 20, 10 dB in 2500 Hz | found | 0.982 to 0.999 | 26.0 to 47.0 dB |
| QPSK 30, 20 dB | found | 0.972, 0.983 | 23.4, 26.4 dB |
| **BPSK 5, 0 dB** | **none** | 0.929, 0.650 | 17.7, 8.1 dB |
| **a carrier in noise, 0 to -12 dB** | **none** | 0.667 to 0.989 | 8.4 to 28.7 dB |

Capped, because the last two rows overlap over their whole range: with no rate
the call rests on the power law alone, a carrier whose envelope the noise has
taken lights that line exactly as BPSK does, and the order's margin says
nothing about which one a call is. No cap above a half separates them. A half
is the bar the AnalogueFm branch already holds itself to for the same reason,
an elimination between things the remaining test cannot tell apart, and the
least `margin_confidence` gives any test that passed, so a capped call never
outranks a PSK call that found its rate. The carrier walked down through noise
earlier in this document now reads 0.50 flagged at -3 and -9 dB, under the
0.66 the correctly named carrier reads at +3.

`may_drive_detection` is necessary and not sufficient: it refuses Unknown and
the flagged call, and certifies nothing else. Tier two asks it of every probe:
`Detector::record_probe` keeps every answer on the track and lets one change
`Track::classification` only when this says yes, and nothing the detector
decides reads that field. The wire carries a label made from it, by the
owner's decision recorded in "The label on the wire" below.

WHAT THE LAST SENTENCE USED TO SAY, twice. First: "Nothing routes a family
into detection yet and nothing goes on the wire as one." Then: "Nothing goes on
the wire as a family."

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

**What was argued to fail is the multiple.** Six times the median is a fixed
bar against a maximum taken over every lag in the search, and the expected
maximum of a set grows with the size of the set. More lags, which is what a
longer extract buys, means a higher maximum from the same noise.

A bar that held would have to grow with the lag count the way an extreme value
does, rather than sit at a constant multiple of the middle of the distribution.
That is a change to a tested library whose constants were each measured against
a signal, and it wants the same treatment rather than an argument from theory,
so it is written down here instead of made.

WHAT THE FIRST PARAGRAPH USED TO SAY after "the same noise": "That is why the
call appears at forty seconds and not at eleven, twenty or fifty-eight: it is
the tail of a distribution being sampled more times, not a property of the
band." The measurement below refuses the "why": forty and fifty-eight seconds
search exactly the same number of lags.

#### The bar against the lag count, measured and not changed

The owner's call, so this measures and changes nothing: `floor_multiple` is
still 6 and `min_prefix_ratio` still 0.01. `ofdm bar survey` in
`tests/characterise/test_periodicity.cpp` holds the extract at the OFDM
burst's 172800 samples and moves only `OfdmSearch::max_symbol_samples`, so the
number of lags searched is the one thing that changes. Three noises, thirty
draws a cell: Gaussian, Gaussian with an impulse of power 25 on one sample in a
hundred, and Gaussian through a 101-tap low pass at a tenth of the rate and not
decimated. The proposed bar is the Rayleigh extreme value,
`sqrt(ln(L / alpha) / ln 2)` times the median, anchored to equal 6 at the
default band's 8161 lags, which makes it 5.65 at 481 lags and 6.08 at 16353.

The tallest isolated lag over the median, typical of the thirty and worst:

| noise | 481 lags | 2017 lags | 8161 lags | 16353 lags |
| --- | --- | --- | --- | --- |
| gaussian | 3.15, 3.57 | 3.42, 3.87 | 3.77, 4.43 | 3.86, 4.48 |
| impulsive | 2.99, 3.82 | 3.48, 4.07 | 3.65, 4.38 | 3.78, 4.32 |
| coloured | 2.82, 3.82 | 3.27, 3.87 | 3.53, 3.95 | 3.66, 4.36 |

False alarms were **0 of 30 in every cell under both bars**. Detections of the
one-eighth-guard burst, ten noise draws a level, were identical at every lag
count under both bars: 10 of 10 at 10 dB in 2500 Hz, 8 at 5, 1 at 3, 0 at 0.

**The growth is real and it is the size the extreme value predicts.** The
typical Gaussian reading rises from 3.15 to 3.86 across a 34-fold range of lag
counts, against 2.98 to 3.74 from `sqrt(ln L / ln 2)`. But it is growth from
under 4 towards a bar of 6, and no synthetic draw came within a factor of 1.3 of
it.

**Real 40 m did.** The empty channel at 30 kHz on 40 m at 1359 UT, through
`revenant-cli --characterise`, which now prints this ratio:

| extract | lags searched | tallest over median |
| --- | --- | --- |
| 11 s | 4094 | 4.7 |
| 20 s | 7469 | 4.9 |
| 40 s | 8161 | **6.0, taken as OFDM** |
| 58 s | 8161 | 4.8 |

Every real reading sits above the worst synthetic one at a comparable lag
count, though from a different extract length, so the real noise's tail is
heavier than any of the three models here. And the call at
forty seconds is not a lag-count effect: fifty-eight seconds searched the same
8161 lags and read 4.8. The Rayleigh bar is exactly 6 at that count, so **the
proposed fix would not have refused the one call it was proposed for**. A bar
that does has to be read off the real noise's own tail, from more than one
extract, and that is the owner's to choose.

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

So the choice has to be made rather than assumed. The extract has to be mixed
to DC and filtered to something near the signal's bandwidth or the classifier
is working 20 dB down, and what does that is the fine stage,
`core/shaders/vrx_fine.comp`, not a demodulator. **Tier two needs a
complex-output mode routed through the fine stage.** Since 2026-09-22 three
modes take exactly that path: P25p1, D-STAR and TETRA are `DemodStage`
receivers whose `vrx_demod` pass hands the fine ring's complex baseband out
unchanged, mixed to DC, filtered and resampled. A probe is the same thing at a
classifier's rate, for example `Demod::Raw` routed through the fine stage
instead of declined to the raw tap, and which mode carries it is a factory
decision in `core/engine`, not something this document decides. Either way
the construction cost is real after all, because it is the fine stage's, and
the pool is real, for a reason the draft never stated.

WHAT THIS PARAGRAPH USED TO SAY: "tier two uses a real demodulator stage, not
the raw tap. The extract has to be mixed to DC and filtered to something near
the signal's bandwidth or the classifier is working 20 dB down, and only a
`DemodStage` does that." The requirement stands. What meets it is the fine
stage plus a complex passthrough, which the digital voice modes now use, and
no demodulator has to run.

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

### The probe receiver, built and measured

Built 2026-09-23. `core/engine/probe.h` is the pool, `core/detect/tier_two.h`
decides which tracks it looks at, and `revenant-cli --detect` prints what it
found in a last column.

**What a probe is.** `Demod::Raw` built through the fine stage: the graph sets
`VrxStageRequest::fine_stage_complex_tap` for a `VrxRole::Probe` receiver and
the stage factory builds a `DemodStage` for it instead of declining it to the
raw tap. That is the factory decision the section above left open, and it
needed no kernel: `core/shaders/vrx_demod.comp` already has a Raw passthrough,
bit-exact against its twin in `tests/reference/test_vrx.cpp`. An ordinary Raw
receiver keeps the raw tap. A probe is left out of `vrx_ids`, every public
receiver method refuses its id, and `set_source_center` cancels what it is
collecting rather than carrying it across the retune.

**The numbers, each a choice.**

- Pool: `EngineConfig::probe_receivers`, four in `revenant-cli`
  (`--detect-probes`), at most sixteen. A receiver once built is kept, so the
  next probe in its bucket is a retune.
- Buckets: 1500 S/s times a power of two, up to 192000. A probe runs at the
  smallest bucket at least four times the track's occupied bandwidth and at
  least 12000 S/s, and never above the grid's channel rate.
- Passband: half the bucket, centred. That is the widest band whose
  demodulation rate is exactly the bucket, and because the residual never
  exceeds a quarter of the channel rate, `place()` never clamps it, the tap
  count never moves, and a retune inside a bucket is always legal.
  `tests/engine/test_engine_probe.cpp` walks every bucket across a whole
  channel spacing and finds no clamp. So the pool is one probe per bucket, as
  the section above predicted, and not narrow, medium and wide.
- Dwell: two seconds, and never fewer than the characteriser's 16384 samples.
  12000 S/s is the lowest bucket that holds that many in two seconds. On a grid
  whose channels run slower, the 3000 S/s of a 96 kS/s source over 64 channels,
  the probe runs at the channel rate and the dwell stretches to 5.46 s.
- Schedule: oldest unclassified first, and not round robin, which would spend
  the pool on tracks already answered. A Live track never probed goes first,
  oldest birth first; one whose answer could not drive anything waits ten
  seconds and then queues behind every new one; one with a family is not
  probed again. Held and Merged tracks are skipped: one has no transmission and
  the other is inside another track's band.

**What the track carries.** `Track::classification` is the family of the most
recent probe `characterise::may_drive_detection` accepted, with its
`classification_confidence` and `symbol_rate_hz`. `Track::last_probe` holds
every answer, refused or not, and `Track::probes` counts them.
`Detector::record_probe` is the only writer, and nothing the detector decides
reads any of it: `tests/detect/test_detector.cpp` runs two detectors on
bit-identical frames, tells one what probes found, and every track's id, state,
centre, width, confidence and margin come out the same in both.

#### What it costs

On the device, per probe per 65536-sample source block, from
`bench throughput --demod raw`, which times a probe since this change, on the
RTX 4090:

| grid | probe | per block | per second of source |
| --- | --- | --- | --- |
| 2.4 MS/s, 64 channels | 12000 S/s | 10.7 us | 0.39 ms |
| 2.4 MS/s, 64 channels | 48000 S/s | 7.8 us | 0.29 ms |
| 2.4 MS/s, 8 channels | 192000 S/s | 9.1 us | 0.33 ms |
| 20 MS/s, 64 channels | 12000 S/s | 16.5 us | 5.0 ms |
| 96 kS/s, 64 channels | 3000 S/s | 5.8 us | 0.0085 ms |
| 96 kS/s, 16 channels | 12000 S/s | 6.8 us | 0.010 ms |

An NFM receiver on the first grid costs 9.2 us a block on the same rig. Four
probes cost four times one to within 1.3 percent, so the cost is linear in the
pool size. The pool is paid whether or not a probe is collecting, because a
kept receiver is dispatched every block: four at the floor bucket on the
shipped 2.4 MS/s grid is 1.6 ms of GPU a second.

On the host, `characterise()` runs on the pool's own worker thread: 12 to 17 ms
an extract at 12000 S/s, 31 ms at 24000, 58 to 85 ms at 48000 and 6.7 ms on the
3000 S/s HF grid. Across the bus a probe is eight bytes a sample, 96 KB/s at the
floor bucket; `core/engine/engine.h` carries the amendment to its promise.

**A probe counts stream samples and is placed on the wall clock.** The worker
places receivers and notices a full capture on a 10 ms poll. An unthrottled
file source on this machine moved a twelve second synthetic scene through the
GPU in 20 ms, so every probe was placed after the stream it was meant to read
had gone. The tests run at four times realtime, and `revenant-cli` says so
when a file is unthrottled.

#### One probe per emitter, at its own centre and width

`probe survey: one probe per emitter` in `tests/engine/test_engine_probe.cpp`,
hidden: ten slots 50 kHz apart on a 600 kS/s source over 16 channels, one
emitter of each family plus an empty slot, three seeds at each SNR in the
emitter's own occupied bandwidth. Correct, wrong and unknown, where a call
`may_drive_detection` refuses counts as unknown:

| family | right answer | 30 dB | 20 dB | 10 dB | 5 dB |
| --- | --- | --- | --- | --- | --- |
| cw | carrier | 3/0/0 | 2/0/1 | 0/0/3 | 0/0/3 |
| am | carrier | 3/0/0 | 3/0/0 | 3/0/0 | 0/3/0 |
| nfm | analogue FM | 2/1/0 | 3/0/0 | 0/3/0 | 0/3/0 |
| usb | unknown | 0/3/0 | 0/3/0 | 0/3/0 | 0/3/0 |
| lsb | unknown | 0/3/0 | 0/3/0 | 0/3/0 | 0/3/0 |
| fsk2 | FSK | 3/0/0 | 3/0/0 | 2/0/1 | 0/0/3 |
| bpsk | PSK | 3/0/0 | 3/0/0 | 3/0/0 | 0/3/0 |
| qpsk | PSK | 3/0/0 | 3/0/0 | 3/0/0 | 0/2/1 |
| ofdm | OFDM | 3/0/0 | 3/0/0 | 3/0/0 | 3/0/0 |
| empty | unknown | 0/0/3 | 0/0/3 | 0/0/3 | 0/0/3 |

**The empty slot is unknown twelve times in twelve**, through the real fine
stage, which is the test the section on narrowing asked for.

At 20 dB and above everything the characteriser has a family for is named
correctly, BPSK and QPSK with order 2 and 4 and 1199.7 to 1200.1 baud against
1200. **Two-tone SSB is wrong at every level**: PSK of order 2 at 1200.1 baud and
confidence 1.00. 1200 Hz is the gap between its tones, 700 and 1900 Hz; two
tones square to a line at their difference, which the symbol-rate detector reads
as a clock, and the M-th power law lights at exponent two. Its spectral
concentration reads 0.50 against 0.06 for real BPSK in the same run, and nothing
in the PSK branch reads concentration. Below 20 dB NFM goes to PSK at 1000 baud,
its modulating tone, by the same mechanism; at 5 dB AM does too, and BPSK and
QPSK come back OFDM at 0.77 to 0.82, the cyclic-prefix bar "The OFDM branch
still fires on noise" records as the owner's to choose.

#### What the detector hands it, which is the harder table

`probe survey: tier two across the synthetic families`, hidden: the same scenes
at 30, 20 and 10 dB, the detector running on the engine's frames and tier two
probing its tracks with a pool of four. Every track alive at the end of each
scene, scored against the emitter whose band it sits in:

| family | tracks | probed | correct | wrong | unknown |
| --- | --- | --- | --- | --- | --- |
| cw | 3 | 3 | 3 | 0 | 0 |
| am | 21 | 21 | 21 | 0 | 0 |
| nfm | 57 | 57 | 0 | 57 | 0 |
| usb | 18 | 18 | 0 | 18 | 0 |
| lsb | 18 | 15 | 0 | 15 | 0 |
| fsk2 | 60 | 15 | 1 | 8 | 6 |
| bpsk | 9 | 3 | 3 | 0 | 0 |
| qpsk | 9 | 3 | 3 | 0 | 0 |
| ofdm | 9 | 3 | 3 | 0 | 0 |
| empty | 0 | 0 | 0 | 0 | 0 |

**Every NFM track is wrong, and the probe is not what is wrong.** The detector
reports NFM as its Bessel comb, one track per line 146 to 183 Hz wide, and a
probe sized to a line is a 12000 S/s extract centred on one sideband of an
11 kHz signal. It sees an off-centre fragment and names PSK or FSK at 1000 or
2000 baud, the modulating tone and its double. The same emitter probed at its
own centre and width is named correctly, in the table above. The FSK tones do
the same thing from the other side: each wandering tone is its own short track
and reads as a carrier. What was measured is that "What separates AM from SSB
from CW lives in the relationship between the lines" holds for tier two as well
as tier one.

**Time to first classification**, over the same runs: 5.2 to 6.1 s from a
track's birth on average, 2.4 s at best, which is the birth rule plus one dwell,
and 9.7 s at worst. From the start of the scene to the first family on any of an
emitter's tracks: 3.5 s for the carriers and AM, 4.3 s for NFM, 8.2 to 8.3 s for
SSB and BPSK, and 10.5 s for QPSK and OFDM. The spread is the queue: sixteen to
twenty tracks are born within a decision of each other, the oldest-first order
breaks the tie by id, ids run up in frequency, and a pool of four takes five
rounds of two seconds to reach the top of the span.

#### Real HF, through the 24-bit path

The six recordings in `docs/recordings.md`, read natively as cs24, the first
120 s of each at four times realtime, default thresholds, pool of four, once on
the 64 channels `revenant-cli` pins and once with `--channels 16`, which is
what `--channels 0` chooses with a centre given:

    revenant-cli "file:///C:/Users/vexam/projects/SDR Recordings/KF4FIC_wideband_14000_14350kHz_20170821_1603UT.wav?center=14175000" \
        --detect --channels 16 --duration 120 --pace 4

| band, hour | grid | born | characterised | too wide | lost with the track | answers named | first family |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 40 m 1359 | 64 | 22 | 17 | 5 | 9 | carrier 8 | 8.0 s |
| 40 m 1501 | 64 | 13 | 8 | 5 | 4 | carrier 4 | 8.0 s |
| 40 m 1603 | 64 | 28 | 18 | 9 | 6 | carrier 11, psk 1 (0) | 8.1 s |
| 20 m 1359 | 64 | 5 | 2 | 14 | 1 | carrier 1 | 8.2 s |
| 20 m 1501 | 64 | 45 | 29 | 28 | 19 | carrier 8, psk 2 (0) | 12.0 s |
| 20 m 1603 | 64 | 91 | 52 | 27 | 18 | carrier 27, psk 3 (0), fsk 1, unknown 3 | 10.4 s |
| 40 m 1359 | 16 | 16 | 15 | 2 | 0 | carrier 6, ofdm 5, psk 2 (1), unknown 2 | 5.0 s |
| 40 m 1501 | 16 | 13 | 9 | 15 | 0 | carrier 7, unknown 2 | 5.6 s |
| 40 m 1603 | 16 | 23 | 20 | 6 | 0 | carrier 14, psk 3 (2), unknown 3 | 4.8 s |
| 20 m 1359 | 16 | 10 | 3 | 10 | 0 | carrier 3 | 4.1 s |
| 20 m 1501 | 16 | 33 | 25 | 22 | 0 | carrier 15, psk 9 (2), unknown 1 | 7.1 s |
| 20 m 1603 | 16 | 59 | 48 | 19 | 0 | carrier 31, psk 12 (3), unknown 5 | 4.8 s |

"Too wide" and "characterised" count probes, so a track retried after ten
seconds counts again. "Lost with the track" is an answer that came back for a
track the detector had already dropped. The family counts are every answer the
probes gave over the run, with the number the detector was allowed to report in
brackets where that differs; the first-family column is the mean from a track's
birth to its first reported family.

**Nothing here is scored.** There is no ground truth for these recordings, so
there is no correct or wrong column. What can be read off:

- **Carriers are most of what is named**, 59 of 69 answers on the 64-channel
  grid and 76 of 120 on the 16-channel one, which is what "What the two
  measurements agree about" found by hand on the same bands.
- **The PSK calls on HF are mostly refused.** None of the six on the
  64-channel grid carried a symbol rate, so all six were flagged and held back;
  on the 16-channel grid 8 of 26 carried one and were reported.
- **OFDM five times on 40 m at 1359 UT**, on one grid and not the other. That is
  the cyclic-prefix bar this document already records as firing on noise and as
  the owner's to choose; nothing here was done about it.
- **The 64-channel grid costs tier two most of its answers.** Its 3000 S/s
  channels hold a 5.46 s dwell and nothing wider than 750 Hz, and 57 of 126
  characterised probes came back after their track had gone. At 16 channels
  every dwell is 2 s, none were lost, and the first family arrived in 4.1 to
  7.1 s against 8.0 to 12.0 s.

#### What is left for the owner

Three things this lane measured and did not decide, each with the options:

1. **Two-tone SSB reads as confident PSK and may drive detection.** A consistency
   test in the PSK branch between the symbol rate and the spectrum, since a
   linear modulation at 1200 baud cannot hold half its power in three 8 Hz bins;
   or a tone-pair check that attributes a cyclic line at the spacing of the two
   strongest spectral lines to the tones; or leaving it, because voice SSB is not
   two tones and has not been measured. Each is a change to `core/characterise`.
2. **Line-spectrum emitters are probed line by line.** Probe what
   `detect::LineGrouper` groups rather than single tracks, which needs the gap
   nobody has chosen; or refuse, in tier two, a family whose symbol rate is wider
   than the track it was measured on, which would have refused every wrong NFM
   and SSB call in the second table (1000 to 2000 baud on 146 to 183 Hz tracks),
   would not touch the FSK tones read as carriers, and would not catch SSB probed
   at its own 2.7 kHz width; or leave the unit a track, as the task that built it
   asked.
3. **The HF grid revenant-cli pinned was a poor one for probes.** At 64
   channels a 96 kS/s source runs 3000 S/s channels, so nothing wider than
   750 Hz can be probed and a dwell is 5.46 s; `--channels 0` gives 16 channels
   and 12000 S/s at a coarser 5.86 Hz a bin.

**The owner took all three on 2026-09-23**, as the integrator recommended: the
tone-pair check for the first, the symbol-rate rule for the second, and the
engine's own grid on HF for the third. The next section is the re-measurement.

#### The three fixes, re-measured

`CharacteriseConfig::tone_pair_fraction` refuses the PSK branch when the two
strongest three-bin lines hold at least half the band's excess power, the third
holds under a quarter of the weaker, and the two sit the claimed symbol rate
apart. `CharacteriseConfig::detection_bandwidth_hz`, which the probe pool fills
from the track, refuses any family whose symbol rate is wider than the track.
`EngineConfig::channels_yield_to_source` lets revenant-cli's 64 give way below
30 MHz. `tests/characterise/test_consistency.cpp` measures the first two either
side of their bars: at 12000 S/s two-tone SSB holds 0.82 of its excess power in
its two lines at 5 dB in 2500 Hz and 0.997 at 30 dB, real BPSK and QPSK 0.065
to 0.075.

The first survey table, one probe per emitter, re-run: usb and lsb go from
0/3/0 at every level to 0/0/3 at every level, and every other row is unchanged.

The second, tier two on the detector's tracks, before and after:

| family | tracks | before, correct/wrong/unknown | after |
| --- | --- | --- | --- |
| nfm | 57 | 0/57/0 | 0/11/46 |
| usb | 18 | 0/18/0 | 0/0/18 |
| lsb | 15 probed | 0/15/0 | 0/0/15 |
| fsk2 | 15 probed | 1/8/6 | 1/8/6 |
| cw, am, bpsk, qpsk, ofdm | | all correct | unchanged |

**The eleven NFM tracks still wrong are a different wrong.** Every one is a
line 146 to 183 Hz wide now named an unmodulated carrier at 0.50 to 0.62, where
before they were PSK or FSK at 1000 or 2000 baud. A probe sized to one line of a
Bessel comb sees that line as a carrier, which it is; the rule cannot reach it,
because a carrier carries no symbol rate to compare. The FSK tones are the same
case from the other side and are unchanged for the same reason.

The HF corpus through the 24-bit path, the same six recordings, 120 s each at
four times realtime, pool of four. With no `--channels`, which on HF is now the
engine's 16:

| band, hour | born | characterised | too wide | lost with the track | answers named | first family |
| --- | --- | --- | --- | --- | --- | --- |
| 40 m 1359 | 16 | 15 | 2 | 0 | carrier 6, ofdm 5, psk 2 (1), unknown 2 | 4.95 s |
| 40 m 1501 | 13 | 9 | 15 | 0 | carrier 7, unknown 2 | 5.56 s |
| 40 m 1603 | 23 | 21 | 6 | 0 | carrier 15, psk 1 (0), unknown 5 | 5.55 s |
| 20 m 1359 | 10 | 3 | 10 | 0 | carrier 3 | 4.10 s |
| 20 m 1501 | 33 | 27 | 23 | 0 | carrier 16, psk 7 (0), unknown 4 | 9.22 s |
| 20 m 1603 | 59 | 50 | 19 | 0 | carrier 33, psk 10 (1), unknown 7 | 5.38 s |

Against the 16-channel rows of the table above: 8 of 26 PSK calls were reported
before and 2 of 20 are now, unknown rose by 0 to 3 answers a band and carrier by
0 to 2, and OFDM stayed at five on 40 m at 1359 UT. The run summary does not say
which rule refused each call, and whether the refused ones were real PSK is not
known, because nothing in this corpus has ground truth. The first-family times
moved by up to 2.1 s, and a probe is placed on the wall clock, so that column
varies run to run as well as rule to rule.

With `--channels 64` the six runs reproduce the old 64-channel rows to within a
run's variation, lost answers included: 9, 4, 6, 1, 19 and 17 came back after
their track had gone. That is the pin's cost, and the default no longer pays it
on HF.

### Confidence, and "unknown" as a real answer

The confidence threshold is the operator's, alongside the detection threshold.

For it to mean anything the classifier's confidence has to be calibrated: one
that reports 0.9 on everything makes the threshold a no-op. A detection the
classifier cannot place stays on the display as an unidentified signal rather
than being given the nearest label. It is still clickable, it still has a
bandwidth, and the operator can listen and decide, which is what they were
going to do. This is the same rule AFT follows in `docs/ui-spectrum.md`: an
unidentified signal means hold still, not guess.

## The label on the wire

### The decision, as a record

    topic       detect.label_on_wire
    status      confirmed, the owner, 2026-09-23
    decided     "Digital protocols should be detected on the waterfall and
                spectrum along with modulation. The pink block that brackets a
                signal should show what the signal is: modulation if analog,
                detected digi mode if digital, and it should set the receiver
                accordingly based on that information."
    supersedes  what this document used to say under "A PSK call with no
                symbol rate", its own rule since the probe receiver was built:
                "nothing goes on the wire as a family"
    took with it the integrator's three probe fixes, recorded under "The
                three fixes, re-measured" above
    evidence    tests/characterise/test_consistency.cpp,
                tests/characterise/test_identify.cpp, tests/detect/
                test_label.cpp, tests/rpc/test_rpc_detect.cpp ("a probed
                detection crosses the wire with its label")

    topic       identify.dmr
    status      confirmed, the owner, 2026-09-23
    decided     DMR is back in scope and its frame sync may be correlated; the
                risk on US8306071 is accepted. Another lane writes
                core/decode/dmr.* from TS 102 361-1 and -2.
    here        a pending row in core/identify: plausible on a 4FSK-wide
                signal, reporting unavailable until the decoder lands, behind
                IdentifyConfig::dmr. Wiring it is attempt_dmr in
                core/identify/identify.cpp. docs/modes.md's exclusion text is
                the DMR lane's to retract.

### What the label is

`Detection.label` carries a kind, a name, a confidence and whether a client may
set a receiver from it. `detect::label_track` in `core/detect/label.h` is the
whole rule, in this order:

1. **A verified protocol.** `core/identify` runs the decoders `core/decode`
   already has over a probe's extract, where the family and the width make a
   protocol plausible, and claims one only on what the decoder verifies. Its
   header states what counts for each protocol and how many it needs.
2. **An accepted family.** An unmodulated carrier is CW, or AM when its
   sidebands mirror about it; analogue FM is NFM below 50 kHz and WFM above;
   FSK and PSK carry their tone count and order, 2FSK or BPSK; OFDM is OFDM.
3. **Nothing.** A track probed and named nothing gets no label.

**It cannot say USB or LSB.** No family the characteriser names is single
sideband, and relative to a carrier that is not transmitted the two differ only
in which side their power sits, which "The lines as a set" above measured no
per-band field can read. A sideband signal is unlabelled, which is rule 3
rather than a gap in it.

**WFM is reachable only where a probe fits.** A broadcast station is 200 kHz
wide and a probe runs at four times its detection's width, so on the shipped
2.4 MS/s grid a WFM carrier comes back too wide and is never labelled. RDS on
it is the same: `core/identify` lists the row and reports it unavailable.

**TETRA needs a grid with 96 kS/s channels or more.** A 25 kHz signal wants the
96000 bucket, and 2.4 MS/s over 64 channels runs 75 kS/s channels.

### What each protocol took, from one probe's extract

`tests/characterise/test_identify.cpp`, each protocol rendered by its own
transmitter in `core/dsp/synth` at 20 dB in 2500 Hz, at the rate its width
would give it, centred at DC. The count is what the row verified against what
it needs:

| protocol | rate | verified | needed |
| --- | --- | --- | --- |
| P25 | 48000 | 30 data units | 2 |
| D-STAR | 48000 | 6 | 2 |
| TETRA | 96000 | 130 bursts | 1 |
| M17 | 48000 | 48 | 2 |
| POCSAG | 48000 | 7 | 2 |
| AX.25 | 48000 | 3 frames | 1 |
| RTTY | 12000, 5 s | 29 characters | 10 |
| SITOR-B | 12000, 5 s | 29 characters | 8 |
| PSK31 | 12000, 5 s | 16 characters | 6 |
| CW | 12000, 5 s | 10 characters | 4 |

Noise, three draws at each of seven widths covering every row: nothing
verified. A steady carrier: not CW.

**The HF modes need five seconds and get them.** Two seconds held one PSK31
character of the six its row needs and three CW characters of four, and
SITOR-B spends 2.24 s phasing before its first character. A detection no wider
than 600 Hz now collects five seconds, the characteriser still reads the first
two, and its first family arrives three seconds later than it did.

**RTTY needs its characters read cleanly, not only framed.** A SITOR-B signal
at 100 baud on the same 170 Hz shift framed 28 and 31 characters at RTTY's
45.45 baud with 3 to 5 framing errors; RTTY framed 29 with none. What separated
them is the weakest soft reading in each character: 0.94 to 0.99 on average
for RTTY at 20, 10 and 5 dB, 0.44 to 0.49 for SITOR-B and 0.21 to 0.26 for
noise.

**One tone of an RTTY pair is a keyed carrier to the CW decoder**, and CW
verified 13 characters on the RTTY extract. So the rows run in order of how
hard their check is to satisfy by accident and the first to verify wins: codes
over fields first, SITOR-B's constant-ratio code, RTTY's framing, PSK31's
Varicode, and CW's Morse table last.

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

**What a click resolves against exists, and so does what it is.**
`Engine::set_spectrum_sink` delivers the frames, the detector runs on them on
the RPC server's side, tier two runs beside it when the engine has probe
receivers, and `Session::detections` publishes the track list with each
track's label. `ui/render/spectrum_item.cpp` resolves a click against it and
tunes a receiver.

WHAT THE HEADING AND THE LAST SENTENCE USED TO SAY. The heading was "What a
click resolves against exists; what it cannot reach is the classifier", and
the sentence first read "What there is still no route for is complex baseband
to a classifier" and then "Complex baseband reaches a classifier through the
engine's own probe receivers, and the answer stays on the detector's track
rather than reaching a click." The label on the wire above is what reaches it
now.

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
holds `Raw, Am, Nfm, Wfm, Usb, Lsb, Dsb, Cw` and the three digital voice modes
appended after them, `P25p1, Dstar, Tetra`: no RTTY, no FSK, no PSK. The
enumeration is a frozen contract and its value is the demodulator kernel's
specialization constant, so growing it is a deliberate change rather than an
edit.

WHAT THIS PARAGRAPH USED TO SAY: "It currently holds `Raw, Am, Nfm, Wfm, Usb,
Lsb, Dsb, Cw`". The digital voice modes were appended after that was written.
