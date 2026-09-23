# The spectrum and waterfall

Written at M1, before any interface exists, because both requirements below
reach further down than a widget does. Auto-scaling decides what the spectrum
stage on the GPU has to compute per frame, and the fine-tuning display decides
which stream gets transformed. Discovering either at M2 means rebuilding a
stage rather than styling a control.

## Where things are

Two top-level windows in one process, since 2026-09-22.

The main window is the span and one strip of chrome. Under a top bar, the
spectrum, a frequency ruler and the waterfall fill the window, in that order
and on one horizontal mapping: the ruler's ticks come from `ui/models/ruler.h`,
1-2-5 steps chosen so no two labels touch at any width, and they are placed by
the same edge-to-edge stretch the two displays use, so a tick sits over the
column it names at both ends of the span. The ruler also marks the receiver's
passband in the receiver's colour, takes the wheel into the same retune
backlog as the displays, and a click on it tunes there.

The top bar carries the front end's frequency dial, a few band shortcuts and
a grouped band menu, and buttons that open panels over the span for what
direct manipulation cannot express: the radio, the detection thresholds, the
bookmarks, and the status drawer behind a pill that names the engine's state
in a word or two. Faults come forward in a strip under the bar that exists
only while one does, as chips naming the problem with the full sentence on
hover. `ui/models/status_summary.h` decides which conditions are faults and
which are notes that wait in the drawer.

The receiver window holds the receiver rack, one strip per receiver in the
receiver's colour with a live meter, and the focused receiver's controls: its
own dial, its mode, its bandwidth, the fine-tuning display below with its own
waterfall under it, RDS and audio. RDS is offered only on a wfm receiver
granted enough filter to pass the subcarrier. The passband waterfall keeps its
history in absolute hertz as the receiver moves, shifting rows by
`ui/render/history_shift.h`. AFT is a tick box beside the receiver's dial, off
until ticked; `ui/models/aft.h` carries the loop and the rules below, and it
drives the same `moveReceiverCentre` path as the wheel. Both windows remember where they were, and the top bar's
"receivers" button brings the second back after it is closed.

A frequency dial steps one digit per wheel notch, with carry and borrow, and
stops at the source's tuning limits; `ui/models/frequency_dial.h` has the
arithmetic. The bands are `ui/models/band_plan.h`, which cites the band plans
its edges came from.

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
Track a low and a high percentile instead, so the scale follows the body of the
distribution. This was written as "something like the fifth and the
ninety-ninth" before either end had a number; both now do, in
`core/dsp/spectrum_levels_reference.h`, as `kSpectrumLowPermille` 50 and
`kSpectrumHighPermille` 990, which is the fifth and the ninety-ninth. That
header is the source of truth and carries why the high end is not higher.

**Asymmetric attack and decay.** Thirty seconds is the decay. Expansion has to
be much faster or a signal that appears suddenly is clipped flat for half a
minute, which on a waterfall reads as a solid bar with no structure in it.
Expand within a frame or two, contract over the thirty.
`core/engine/spectrum_scale.h` holds both as `decay_seconds` 30.0 and
`attack_seconds` 0.025.

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

### The filter is dragged here

The passband is drawn over that transform as two rules and a fill, and the
rules are handles. `ui/render/passband_item.cpp` holds the interaction and
`ui/models/receiver_link.cpp` the round trip; what belongs in this document
is the three decisions that are about the display rather than about the code.

**The rules and the picture are placed from one axis.** `PassbandGeometry`
carries bin zero as the exact rational the fine stage mixed to DC, and both
the trace and the rules go through it. That is what makes the display right
on CW without anything in it knowing about CW: on that one mode the axis sits
a sidetone below the receiver's centre, and a display that derived the axis
from `VrxParams::center` would draw the filter one pitch out of place there
and correctly on the other seven.

**The mapping freezes for the length of a gesture.** The pane's span is the
demodulation rate and the demodulation rate is derived from the passband, so
widening the filter widens the pane. Left alone, the handle jumps backwards
out from under the pointer, which is unusable. The pixel-to-hertz mapping is
therefore taken at drag start and held; frames arriving during the drag are
drawn into it by their own axis, so a narrower frame letterboxes and a wider
one is cropped, and neither is stretched. On release the axis eases back to
the live span over about 150 ms, so the change is seen rather than jumped.

**Two shades, not one.** The requested edges are drawn from the client's own
copy of the request and move with the pointer. What the engine granted comes
back a round trip later on `VrxPlacement::grantedLow` and `grantedHigh` and
is drawn in a second, dimmer pair whenever it differs. One shade can say a
filter is 8 kHz wide; two can say it was asked to be 10 and lost the top,
which is the channel clamp finally visible instead of being a bool nobody
reads.

The keyboard equivalent is `[` and `]` to select an edge, `\` for both,
arrows to move the selection, up and down to widen and narrow symmetrically,
Home for the mode's default and Escape to cancel a drag. Shift is a hundred
hertz and control is one.

## Scroll, and what each axis means

Two gestures, both context sensitive on which display the pointer is over.
The context is the point: one wheel does two different jobs and never has to
be told which, because the operator is already looking at the thing they mean
to change.

### Time runs down: newest at the top

The newest row of the waterfall is at the top edge and the history scrolls
down away from it. SDR#, SDRuno, GQRX, CubicSDR and HDSDR all default to
that, so an operator's eye goes to the top of a waterfall for "now" before
they have read a label, and a display that runs the other way is read
backwards for as long as it takes them to notice.

Written down because this section specified both gestures in detail without
ever saying which way the axis they act on runs, and the first implementation
ran it the other way. `ui/render/waterfall_item.cpp` holds the convention now:
the ring's write cursor decrements, so reading the ring forwards from just
past the cursor is newest to oldest, top to bottom, with no mirror anywhere.

The scrub gesture below inherits this axis. Older rows are the ones below the
bottom edge, so a scrub into the past moves the picture the way scrolling
down a document does, and a scrub back toward now moves it the other way.
Reversing one of the two without the other gives a display that scrolls one
way and scrubs the other, which is the failure this paragraph exists to
prevent.

### A detection means a different thing on each display

The spectrum has no time axis. A detection drawn over it is a frequency
marker and nothing more: this signal, this wide, live or held, with the fade
saying how much evidence the tracker still has for it.

The waterfall has a time axis, so a detection drawn over it is bounded in
both: frequency across, and down, the rows the signal was actually present
in. It scrolls with the history it annotates, so a burst that ended ten
seconds ago keeps its place as it ages and an operator can read off that it
lasted two seconds and started eight seconds ago. A band down the full height
says none of that and hides the history it is pointing at.

Two consequences. The waterfall has to remember which samples each stored row
covers, which is a ring of sample ranges beside the ring of pixels. And the
rectangle's closing edge is the last frame the signal was DETECTED in, never
the last frame the tracker considered: those differ by exactly the hold for a
held track, so closing on the second one would draw every held track as
lasting the length of its own hold.

The fade follows the same split. A held track's rectangle already ends where
the signal stopped, so down there it is drawn solid; fading belongs to the
spectrum marker alone, where it means the tracker has not dropped this track
yet and the signal has stopped.

### Horizontal scroll tunes

Over the fine-tuning display, it moves the receiver. That is
`VrxParams::center` through `Engine::set_vrx_params`, the path AFT already
uses and the one measured continuous across a change.

Over the wide display, it moves the radio. That is `Source::tune`, and it is
a different kind of operation with three consequences the fine case does not
have.

**Both the surface and the gesture exist now.** `Engine` exposes
`set_source_center`, which tunes the front end and writes the landed centre
back into `EngineInfo::source_center`; `Session.setSourceCenter` carries it
and `Session.sourceCanRetune` says whether the source will take it at all, so
a client can grey the control out rather than discover the refusal by trying.
`ui/models/source_link.cpp` drives all three from the frequency entry, and
since 2026-09-21 the wheel over either span display drives them too.

The shipped numbers, both one line each in `ui/models/scroll_tune.h`: one
twentieth of the span per notch, and one tune per 400 ms. The step is a
fraction of the span rather than a hertz count so that 2.4 MS/s and 20 MS/s
feel the same to the same gesture. The interval is the measured 330 ms of dead
stream plus room for the round trip, the supervisor noticing, and the up-to-four
attempts the backend makes because the first `rtlsdr_set_center_freq` after a
cancel fails every time. The first notch goes out immediately and coalescing
starts behind it, so an isolated scroll has no lag.

One compromise worth knowing: each display holds its own accumulator, so moving
the pointer from the spectrum to the waterfall mid-sweep can put two tunes
inside one interval. A shared coalescer belongs on `EngineLink`, because the
settling interval is a property of the radio rather than of a display.

WHAT THIS PARAGRAPH USED TO SAY, the second time. Until the gesture shipped it
was headed "**The Engine surface exists and the gesture does not**" and ended
"What is missing is only the wheel over the wide display, which is a gesture
rather than a surface."

WHAT THIS PARAGRAPH USED TO SAY. Until 2026-09-21 it read "**There is no
Engine surface for it.** `Source::tune` exists; `Engine` does not expose it,
and `EngineInfo::source_center` is a snapshot taken once when the source was
opened", and quoted an `engine.cpp` comment saying that following the tune
was "the change to make then and not now". That change was made in 4463967,
and the comment it quotes no longer exists to be read. Left standing it told
anyone costing out this gesture that the engine work was still ahead of them,
when the only thing left is the gesture itself.

**Every receiver's absolute frequency changes meaning, and the engine handles
it.** The grid is in baseband, so it survives a retune untouched. What moves is
what baseband DC corresponds to, and a receiver left at a fixed baseband offset
drifts in absolute terms, which is not what anybody means by tuning the radio.

`Engine::set_source_center` rebases every receiver's offset to hold the
absolute frequency it was tuned to, and removes one whose centre falls outside
the new span. So this gesture does not have to recompute anything, and must not
try: a client adjusting offsets on top of the engine's would move every
receiver twice.

WHAT THESE TWO PARAGRAPHS USED TO SAY, and it is now false. They read that "the
receivers should hold their absolute frequencies and have their offsets
recomputed. Any that fall outside the new span have to be parked and said to be
parked", and then "The engine does not do that for you. `Engine::set_source_center`
re-places every receiver with the params it already held, which keeps each one's
baseband offset and so drifts every one of them by the whole retune. That is
the right default for an engine that is not told what the operator meant, and
it makes recomputing the offsets this gesture's work rather than something to
assume has happened."

Two things changed. The engine does it, so it is not this gesture's work. And
an out-of-span receiver is REMOVED rather than parked, which was the owner's
call on 2026-09-21: parked-and-said-to-be-parked is a state an operator then
has to tidy up, and what they asked for was a disappearing receiver.

**A SWEEP WILL DROP RECEIVERS, AND NOTHING SAYS SO YET.** Scrolling far enough
takes the front end past whatever a receiver was tuned to, and that receiver
goes. The engine is right to remove it and the client empties its pane, but
neither announces it, so an operator sweeping with a receiver open watches it
vanish without being told why. A sentence on the pane naming the frequency that
was dropped is the missing piece.

**A device retune is not free and not instant.** An RTL-SDR takes time to
settle and the sample stream is discontinuous across it. Measured on 2026-09-21
on an R820T: about 330 ms with no samples at all, roughly 790,000 of them at
2.4 MS/s, because `rtlsdr_set_center_freq` fails on a dongle that has been
streaming for more than about half a second and the backend has to stop the
transfers around the tune. docs/rpc.md, under "The front end can be pointed
somewhere else", has the measurement and the boundary. A mouse wheel emits
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
| USB, LSB | The suppressed carrier, which since the passband became two edges IS `VrxParams::center` by definition. Read, not derived, and never taken from the energy |
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
