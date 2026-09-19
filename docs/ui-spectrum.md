# The spectrum and waterfall

Written at M1, before any interface exists, because both requirements below
reach further down than a widget does. Auto-scaling decides what the spectrum
stage on the GPU has to compute per frame, and the fine-tuning display decides
which stream gets transformed. Discovering either at M2 means rebuilding a
stage rather than styling a control.

## Auto-scaling, both ends

The colour map's floor and ceiling both track the signal automatically, over a
time constant of about thirty seconds. Not a fixed floor with an automatic
ceiling, and not two sliders the operator is expected to ride.

Each display scales on what that display is showing. The wide spectrum scales
on its visible span, so zooming into a hundred kilohertz of a twenty megahertz
capture rescales to what is in that hundred kilohertz. The fine-tuning display
below scales on the receiver's passband. A display scaled by something it is
not showing is the version of this that looks broken for reasons nobody can
name.

Three things decide whether this feels right or feels like a fault.

**Percentiles, not extremes.** The minimum and maximum of a frame are a dead
bin and a spur. One stuck bin at the bottom of the range pins the floor there
forever, and a single carrier pins the ceiling, and between them the noise
floor and the signals all end up compressed into the middle of the colour map.
Track a low and a high percentile instead, something like the fifth and the
ninety-ninth, so the scale follows the body of the distribution.

**Asymmetric attack and decay.** Thirty seconds is the decay. Expansion has to
be much faster or a signal that appears suddenly is clipped flat for half a
minute, which on a waterfall reads as a solid bar with no structure in it.
Expand within a frame or two, contract over the thirty.

**On the device.** The spectrum frame is produced on the GPU and the
percentiles are computed from it there. Reading a frame back to the host to
measure it would put the one thing this architecture exists to avoid back into
the path, per the note in `core/engine/engine.h` about what is permitted to
cross the bus.

The operator can still pin either end. Automatic is the default because it is
right almost always, and the exception is comparing two captures, where a
scale that moves is a scale that lies about which signal was stronger.

A pinned end is drawn exactly where it was pinned. That reads as obvious and
is the rule a display will break as soon as it has a minimum span to enforce
or a correction of its own to apply, so it is written down: when something
has to give, it comes out of the end that is still automatic, and if both
ends are pinned nothing gives at all. The CLI got this wrong first and drew
a pinned ceiling up to 21.4 dB above where it was asked for, which defeats
the one job pinning has.

## The fine-tuning display

A second spectrum and waterfall showing the receiver's own passband rather
than the wide span, in the manner of SDRuno's second spectrum window. It is
what makes parking a filter on a signal precise rather than approximate, and
it is where a drifting carrier is visible as drift rather than as the audio
slowly going wrong.

**It is a transform of the fine stream, not a zoom of the wide one.** The
per-receiver fine stage in `core/engine/vrx_stage.cpp` already writes complex
baseband at the demodulation rate into a ring on the device, mixed to DC and
limited to the requested bandwidth. Its span is the demodulation rate, which
`plan_vrx` rounds up to a whole multiple of the audio rate, so it is somewhat
wider than the receiver's own bandwidth rather than equal to it. Wider is the
right direction for this: the display shows the filter's skirts and what sits
just outside them, which is where an adjacent signal about to become a problem
is visible. One small FFT of that ring gives native resolution across the
passband. Zooming the wide transform gives whatever its bins are worth there
instead, and the comparison is the point: a 4096-point transform of a 48 kS/s
fine ring resolves 11.7 Hz, where a 2^18 transform of a 20 MHz span resolves
76 Hz. Six times coarser, from a transform sixty-four times larger, and no
amount of interpolation puts the difference back.

This is also why the feature is cheap. The expensive part, getting a
receiver's baseband onto the device at the right bandwidth, is already paid
for by the demodulator.

## Scroll, and what each axis means

Two gestures, both context sensitive on which display the pointer is over.
The context is the point: one wheel does two different jobs and never has to
be told which, because the operator is already looking at the thing they mean
to change.

### Horizontal scroll tunes

Over the fine-tuning display, it moves the receiver. That is
`VrxParams::center` through `Engine::set_vrx_params`, the path AFT already
uses and the one measured continuous across a change.

Over the wide display, it moves the radio. That is `Source::tune`, and it is
a different kind of operation with three consequences the fine case does not
have.

**There is no Engine surface for it.** `Source::tune` exists; `Engine` does
not expose it, and `EngineInfo::source_center` is a snapshot taken once when
the source was opened. The comment next to that line says it becomes stale
the moment a retunable device arrives and that following the tune is "the
change to make then and not now". Scrolling the wide waterfall is then.

**Every receiver's absolute frequency changes meaning.** The grid is in
baseband, so it survives a retune untouched. What moves is what baseband DC
corresponds to. A receiver left at a fixed baseband offset drifts in absolute
terms, which is not what anybody means by tuning the radio: the receivers
should hold their absolute frequencies and have their offsets recomputed. Any
that fall outside the new span have to be parked and said to be parked rather
than silently producing noise from wherever they landed.

**A device retune is not free and not instant.** An RTL-SDR takes time to
settle and the sample stream is discontinuous across it. A mouse wheel emits
events far faster than a tuner can follow, so the wide-scroll path has to
coalesce: accumulate the wheel delta, issue one tune per settling interval,
and let the waterfall smear while it happens rather than queueing a hundred
retunes. The fine case needs none of that, which is the other reason the two
gestures are not the same operation wearing different hats.

### Vertical scroll scrubs, and it should sound like tape

Over the wide waterfall, on a recording rather than a live radio, vertical
scroll moves through the capture. Scrubbing that pitches the audio the way
dragging a tape reel does is the right behaviour.

The arithmetic works. With `Fs` the source rate, `Fc` the channel rate, `Fd`
the demodulation rate and `Fa` the 48 kHz the sound card wants: replay at `k`
times realtime and the source delivers `k*Fc` channel samples per wall
second, so the fine stage emits `k*Fd` audio samples per wall second against
a card that consumes `Fa`. Those balance when

    Fd = Fa / k

and playing an `Fd`-sampled stream out at `Fa` multiplies every frequency by
`Fa/Fd = k` and divides every duration by `k`. That is exactly tape, and the
relation was checked rather than asserted.

**The route through `Fd` does not exist, and this document said so before it
proposed it.** `plan_vrx` sets `demod_rate = decimation * audio_rate` with
the decimation at least one, so `Fd` is always an integer multiple of `Fa`
and never below it; `design_audio_taps` refuses the other case outright, as
"an interpolation rather than a decimation". So `Fd = Fa/k` has a solution
only at `k = 1/n`. Every `k` above one, which is the fast half of the gesture
and the half the worked example above uses, is unreachable by construction.
The section on the fine-tuning display states the same rounding rule a page
earlier, which is where the contradiction should have been caught.

Three further things sit under that, and together they close the route:

**`Fd` is not a knob.** Nothing in `VrxParams` sets it. It is derived from
the mode, the bandwidth and the CW pitch through `minimum_demod_rate`. The
only nearby input is `VrxParams::audio_rate`, which also becomes the plan's
output rate and therefore the rate the sound card is opened at, which defeats
the purpose.

**Even the reachable slow direction breaks on the common case.** At
`k = 0.25`, `Fd` would be 192 kHz, which is a legal multiple of `Fa`. But
`Fd` cannot exceed `Fc`: the fine filter's stopband edge clamps to `Fc/2` and
the resampler's whole step goes to zero. A 20 MS/s capture on a 64-channel
grid has `Fc = 625 kHz` and fits; a 2.4 MS/s dongle capture on the same grid
has `Fc = 75 kHz` and does not. Slow scrub would fail first on exactly the
recordings anybody actually has.

**`pace` is not live either.** `EngineConfig` is consumed at `Engine::create`,
and the file source's copy is a plain non-atomic double written once in
`start()` and read on the delivery thread. Changing `k` today means stopping
and restarting the source. Making it live is an atomic and a memory-ordering
rule, not a parameter pass.

And a fourth, which is about the gesture rather than the plumbing: **`pace`
is validated non-negative everywhere**, so a backwards scrub, the single most
characteristic thing dragging a tape reel does, is not expressible anywhere
in the stack.

### So the varispeed goes in the monitor

Not in the DSP. The monitor backend reads the audio ring at `k` samples per
sample it delivers, and nothing upstream changes: the recording path stays
bit-correct, no plan is rebuilt, `Fd` never moves, and `k` above one and
below one are equally reachable. Rough resampling, which for a scrub is the
point, since the artefacts are indistinguishable from the effect being asked
for. It also keeps a user-interface convenience out of the sample path, which
`docs/conventions.md` has a rule about.

`AudioTap` supports it: `read`, `read_or_fill`, `readable_frames` and
`trim_to_frames`, all non-allocating and single-reader, and `read_or_fill`
already turns a shortfall into counted silence, so a starved varispeed read
degrades the way the rest of the monitor does. The device period does not
move, because the WASAPI backend opened with `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`
and only the tap-side read count changes.

Three specifics that have to be got right:

- **`read` consumes and there is no peek.** The backend owns a carry buffer
  and a fractional phase across callbacks, or every period boundary is a
  click.
- **The render thread must not allocate.** The scratch input buffer is sized
  at `open()` from the largest `k` that will ever be asked for, so the scrub
  range is capped up front rather than discovered at the first fast drag.
- **`trim_to_frames` runs immediately before the read.** At `k` above one the
  250 ms default backlog cap discards exactly the samples the varispeed read
  is about to want. The cap has to scale with `k` or be suspended while
  scrubbing.

### What the gesture needs that does not exist

The scrub position itself is `Source::seek`, which is available where it is
needed: the file and synthetic sources both implement it, both defer to a
block boundary rather than tearing a block, and the file source resets its
pacing origin afterwards, which is the behaviour a scrub wants. A Paced
source refuses with a message naming the capability rather than the symptom,
so "live sources do not scrub" is already true and already well worded.

What is missing is the way through. `Engine` takes ownership of the source at
`open_source` and exposes only its capabilities and its stats: there is no
`seek`, no `tune`, no live `pace`, and no accessor that would let a caller
reach past it. Horizontal scroll over the wide display needs one new surface
and vertical scroll needs two more. Naming one of them and stopping, which an
earlier draft of this section did, understates the work by two thirds.

## AFT

Automatic frequency tracking, as an option and off by default. It follows a
drifting carrier by nudging the receiver's centre so the signal stays where
the filter is.

It rides the retune path that already exists. A small frequency move is a push
constant and a new tap table, taken in place by the stage with no gap in the
audio; that is measured, not assumed, in the engine suite's retune case.

Two limits on that, because the measured case was a 300 Hz nudge and AFT will
not always be nudging. A move that crosses a coarse channel boundary changes
the channel the receiver reads and the whole residual with it, which is a
different tap table rather than a shifted one, so a track sitting near a
boundary needs hysteresis or it will flip channels on measurement noise. And
`retune()` refuses any change that alters the plan's derived shape, which a
frequency move can do at a channel edge, because `max_channel_bandwidth` is
`Fc - 2*|residual|` and a wide receiver's bandwidth clamps differently as the
residual grows. Neither bites a narrow receiver drifting slowly, which is the
case AFT is for.

Two properties it must have, because without them it is worse than nothing:

**A deadband.** A loop with no deadband hunts, and a receiver that walks
around its own signal while the operator is trying to tune it is infuriating
in a way that is hard to diagnose from a bug report.

**A rate limit, and it yields to the operator.** Drift is slow. Anything fast
is either a second signal or the operator turning the dial, and in both cases
the correct response is to stop tracking rather than to chase. Manual tuning
always wins; AFT resumes from wherever it was left.

### What AFT should actually aim at

Not a peak, and not a centroid. Both are statistics of the instantaneous
spectrum, and for most modes neither one sits where the receiver wants to be.

RTTY is the clear case. It is two tones, mark and space, typically 170 Hz
apart, and which one is radiating depends on the character being sent. A peak
tracker follows whichever is loudest, so it sits on mark through an idle and
then jumps back and forth through traffic. A centroid tracker lands between
them and wobbles with the mark-to-space ratio, which is a property of the text
rather than of the tuning. The place the demodulator wants is the midpoint,
and the midpoint is not where the energy is. It is derived: either tone plus
or minus half the shift.

Single sideband is worse and quieter about it. There is no carrier at all, and
the energy centroid sits roughly half a bandwidth from the suppressed carrier
the receiver is trying to hold, wandering with whatever the speaker is saying.
RTTY dances in place; SSB walks off.

So the target is the LOGICAL centre of the signal, and that is a property of
the modulation rather than of the frame. Each mode carries its own rule for
deriving it:

| Mode | Where the logical centre is |
| --- | --- |
| AM | The carrier. A peak is correct here, which is why peak-tracking looks fine until it is tried on anything else |
| CW | The carrier, but keyed, so track only through key-down and hold through the gaps |
| NFM, WFM | The centre of the deviation swing, which is a long average rather than any one frame |
| USB, LSB | The suppressed carrier, at the edge of the passband and not in it. Derived from the passband edge, never from the energy |
| RTTY and FSK | The midpoint of the tones, derived from one tone and the known shift |
| PSK | The centre, which for these is where the energy already is |

## Identification, and why AFT does not have to wait for it

The rule above needs to know what the signal is, and most of the time the
operator has already said, by choosing a demodulator. That is the cheap half
of the problem and it needs no classifier.

It does need something that does not exist yet, and the RTTY example is
exactly where it shows. `Demod` is `Raw, Am, Nfm, Wfm, Usb, Lsb, Dsb, Cw`;
there is no RTTY in it, and `VrxParams` carries no shift field, only
`cw_pitch`. So "the operator selected RTTY with a 170 Hz shift" is not a state
this engine can currently be in. The centre rule for the modes that do exist
follows from `VrxParams` today; the modes the table below names as the
interesting cases need the enumeration and the parameters to grow first. That
is a small change and it is not a free one, because `Demod` is a frozen
contract and its value is the demodulator kernel's specialization constant.

Classification is for the unattended case: a scan, a wideband survey, a
receiver parked on something the operator has not named. `core/detect/` is
reserved for it and nothing is written there yet.

The useful property, and the reason this is not a dependency to be afraid of,
is that **a classification result is stable**. A transmission does not change
modulation halfway through. So the loop does not need realtime identification,
only eventual identification that sticks: spend a second or two deciding it is
RTTY and then track correctly for as long as the signal lasts. Classification
latency does not bound AFT quality, which decouples a hard problem from a loop
that has to run smoothly.

What AFT does need is a defined answer for "not identified yet, and nobody
told me". That answer is to hold still. An AFT that guesses a centre rule from
an unidentified signal is the dancing behaviour with extra steps.
