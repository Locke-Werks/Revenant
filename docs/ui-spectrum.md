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
waterfall under it, RDS, decoding and audio. RDS is offered only on a wfm
receiver granted enough filter to pass the subcarrier, and decoding only on a
receiver whose mode some decoder reads; see "Decoding" below. The passband waterfall keeps its
history in absolute hertz as the receiver moves, shifting rows by
`ui/render/history_shift.h`. AFT is a tick box beside the receiver's dial, off
until ticked; `ui/models/aft.h` carries the loop and the rules below, and it
drives the same `moveReceiverCentre` path as the wheel. The auto filter
sits beside it, also off until ticked; see "Auto filter" below. Both windows
remember where they were, and the top bar's
"receivers" button brings the second back after it is closed.

A frequency dial steps one digit per wheel notch, with carry and borrow, and
stops at the source's tuning limits; `ui/models/frequency_dial.h` has the
arithmetic. The bands are `ui/models/band_plan.h`, which cites the band plans
its edges came from.

## Keys

One table decides every key. `ui/models/key_actions.h` holds each action's
id, label, group, keys, where the keys apply, what it needs and the name of
its handler. `ui/tests/test_key_actions.cpp` holds the table to three rules:
no two actions share a key where both apply, no window key is one the filter
display or the overlay also takes, and the key map below is the one the
table prints.

Not bound, because the client cannot do them: next and previous receiver,
and solo. The engine holds any number of receivers and this client holds one
per window, so there is nothing to move to and nothing to solo against. The
detections key opens the detection thresholds, which is the panel there is;
nothing in the client lists the detections themselves.

<!-- The key map. Generated from ui/models/key_actions.h and checked by ui/tests/test_key_actions.cpp: edit the table, not these lines. -->

**anywhere.** In either window, except that a text field keeps the keys it types with.

| Group | Action | Keys |
| --- | --- | --- |
| tuning | step the radio up by the tuning digit | `Alt+Up` |
| tuning | step the radio down by the tuning digit | `Alt+Down` |
| tuning | move the tuning digit left | `Alt+Left` |
| tuning | move the tuning digit right | `Alt+Right` |
| tuning | page the radio up a tenth of the span | `PgUp` |
| tuning | page the radio down a tenth of the span | `PgDown` |
| tuning | type a frequency for the radio | `Ctrl+G` |
| tuning | jump to a band | `Ctrl+J` |
| receiver | put the receiver on the span centre | `Ctrl+N` |
| receiver | type a frequency for the receiver | `Ctrl+Shift+G` |
| receiver | remove the receiver | `Ctrl+Del` |
| receiver | turn AFT on or off | `Ctrl+T` |
| receiver | turn the auto filter on or off | `Ctrl+Shift+F` |
| mode | switch the receiver to am | `Alt+1` |
| mode | switch the receiver to nfm | `Alt+2` |
| mode | switch the receiver to wfm | `Alt+3` |
| mode | switch the receiver to usb | `Alt+4` |
| mode | switch the receiver to lsb | `Alt+5` |
| mode | switch the receiver to dsb | `Alt+6` |
| mode | switch the receiver to cw | `Alt+7` |
| mode | switch the receiver to raw | `Alt+8` |
| filter | widen the filter | `Ctrl+=` |
| filter | narrow the filter | `Ctrl+-` |
| filter | put the mode's default filter back | `Ctrl+0` |
| filter | put the arrow keys on the filter edges | `Ctrl+E` |
| audio | mute or unmute the audio | `Ctrl+M` |
| audio | turn the volume up | `Alt+=` |
| audio | turn the volume down | `Alt+-` |
| display | pin or unpin the spectrum floor | `Ctrl+[` |
| display | pin or unpin the spectrum ceiling | `Ctrl+]` |
| panels | open the radio picker | `Ctrl+O` |
| panels | open the detections panel | `Ctrl+Shift+D` |
| panels | open the bookmarks | `Ctrl+B` |
| panels | bookmark the receiver | `Ctrl+D` |
| panels | show or hide the receiver window | `Ctrl+R` |
| help | open the command palette | `Ctrl+K`, `Ctrl+Shift+P` |
| help | show the key map | `F1` |

**on the filter display.** After a click on the display, or Ctrl+E from anywhere. A step is 10 Hz, 100 Hz with Shift and 1 Hz with Ctrl.

| Group | Action | Keys |
| --- | --- | --- |
| filter edges | select the low edge | `[` |
| filter edges | select the high edge | `]` |
| filter edges | select both edges | `\` |
| filter edges | move the selection down | `Left` |
| filter edges | move the selection up | `Right` |
| filter edges | widen both edges | `Up` |
| filter edges | narrow both edges | `Down` |
| filter edges | put the mode's default filter back | `Home` |
| filter edges | cancel a drag | `Esc` |

**in the palette and the key map.** While the command palette or the key map is open.

| Group | Action | Keys |
| --- | --- | --- |
| palette | next entry | `Down` |
| palette | previous entry | `Up` |
| palette | run the chosen entry | `Return`, `Enter` |
| palette | close | `Esc` |

<!-- End of the key map. -->

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

A second spectrum and waterfall showing the receiver's passband and what is
around it rather than the wide span, in the manner of SDRuno's second
spectrum window. It is
what makes parking a filter on a signal precise rather than approximate, and
it is where a drifting carrier is visible as drift rather than as the audio
slowly going wrong.

**It is a transform of the receiver's own display stream, not a zoom of the
wide one, and the receiver's filter is not in it.** The pane shows the
neighbourhood the way SDRuno's does: an honest spectrum of what is on the air
around the receiver, with the filter drawn over it by the client as two rules
and a fill. `core/engine/vrx_stage.cpp` runs a display tap beside each
receiver's fine stage: the same kernel, `core/shaders/vrx_fine.comp`, reading
the same coarse channel and mixing by the same exact rational frequency, so
the pane's DC is the fine stream's DC to the hertz, then a fixed anti-alias
lowpass and an integer decimation, and none of the receiver's own filter.
`core/dsp/vrx_reference.h`, "The display tap", has the design; it is proved
bit-exact against its CPU twin on every rung in
`tests/reference/test_vrx.cpp`.

One small FFT of that stream gives native resolution across the pane.
Zooming the wide transform gives whatever its bins are worth there instead,
and the comparison is the point: at 20 MS/s on 64 channels the narrowest
display rate is 31.25 kS/s, where a 4096-point transform resolves 7.6 Hz,
and a 2^18 transform of the 20 MHz span resolves 76 Hz. Ten times coarser,
from a transform sixty-four times larger, and no amount of interpolation puts
the difference back.

**The rate rule.** The pane is the central half of the transform, which is
what `core/shaders/spectrum.comp` keeps natively, so it spans half the display
rate. The anti-alias filter is flat to the pane's edge and in its stopband by
three quarters of the display rate, the nearest frequency that folds back into
the pane, so nothing is aliased into what is shown. The display rate is the
channel rate divided by an integer R, which makes the resampler a pure
decimator with no fractional phase. R is the largest rung of the channel
rate's ladder that keeps the pane at least four passband reaches wide, where
the reach is how far the passband's further edge sits from the pane's centre:
a symmetric band B wide gets a pane of at least 2B, a one-sided USB band
[0, B] at least 4B. Each rung is the smallest divisor of the channel rate at
least twice the one below it, and the 256-tap filter reaches 80 dB across the
transition only up to R = 25, so at the canonical 75 kS/s and 625 kS/s
channels alike the ladder is 1, 2, 4, 8, 20. The narrowest pane at 625 kS/s is
therefore 15.6 kHz, and the widest, at R = 1, is one channel spacing.

**The pane holds still while an edge is dragged.** The display rate does not
track the passband; it steps only when the reach crosses a rung, and rungs
are at least a factor of two apart. Dragging a USB filter's high edge from
1 kHz to 20 kHz, twenty times wider, in 250 Hz steps on a 75 kS/s channel
moves the display rate three times, 9375 to 18750 to 37500 to 75000 S/s, and
at none of the other 73 positions. A retune that keeps the rate keeps the
display stream running, because the stream is a pure function of the
absolute sample index; one that changes it restarts the stream, and the graph
counts the frames it skips while the new window fills. The client's frozen
mapping below covers the drag itself.

**What it shows, measured.** White noise at -40 dBFS across a 2.4 MS/s
source, a 6 kHz NFM receiver 1 kHz off a 75 kS/s channel's centre, one tone at
-40 dBFS 1 kHz inside its passband and one 2 kHz outside it, a 1024-point
transform, one second of signal. `tests/engine/test_engine_passband.cpp`,
"the passband pane shows the neighbourhood rather than the filter", prints
both sets of figures.

| | fine stream, before | display tap, after |
| --- | --- | --- |
| pane | 48 kHz, 1024 bins at 46.88 Hz | 18.75 kHz, 512 bins at 36.62 Hz |
| noise floor inside the passband | -84.18 dB | -85.06 dB |
| noise floor outside it, averaged | -120.41 dB | -85.16 dB |
| noise floor by eighths of the pane | -88.47 to -175.43 dB, 88.69 dB spread | -85.01 to -85.42 dB, 0.41 dB spread |
| tone inside, peak bin | -40.36 dB | -40.31 dB |
| tone 2 kHz outside, peak bin | -123.98 dB | -40.72 dB |
| the two tones' main-lobe power | | -36.97 and -36.98 dB |

The display filter's own passband ripple, measured on the device with tones
across the pane at R = 2, is 1.8e-5 dB, and tones at the frequencies that
would fold into that pane came out at least 127 dB down, against a design
floor of 80 dB. The 0.41 dB spread across the eighths is the estimate's own
scatter: eight means of 64 correlated bins over one second of noise, by a
rough count of about 37 independent windows and 32 independent bins in each,
are expected to range over about 0.36 dB. The peak-bin difference between the
two tones is the analysis window's scalloping, since they fall at different
fractions of a bin; summed across their main lobes they agree to 0.01 dB.

What the pane still shows is the coarse channel's own shape. A receiver near
the edge of its channel, or one whose pane is a whole channel spacing wide,
sees the channelizer's prototype roll off at one side. That is what the
receiver's channel holds, and the fine stage filters the same samples.

**What it costs.** A receiver with no sink attached records no display
dispatch and no transform; what it holds is a display ring a window deep and
a 257-entry tap table. Attached, the display tap is one more dispatch of the
fine kernel beside the fine stage, and the transform is one pass where it
used to be two. Measured at 20 MS/s, 64 channels and 65536-sample blocks on
the RTX 4090 by the hidden cost case, each figure the minimum of five
end-to-end runs, twice over. Attached, per receiver per block with a
2048-point transform: 15.2 and 16.1 us at eight receivers, 20.0 and 15.3 at
thirty-two; with 512 points, 13.3 and 15.1 at eight. One receiver does not
repeat, 14.7 and 5.3, for the reason core/engine/graph.cpp gives. The
two-pass stage over the fine ring measured 14.6 to 16.2 us at eight, so
attaching a sink costs what it did before, within what this measurement
repeats to.
Detached, against an engine with no passband stage at all: -2.0 to 11.6 us
in the first set and -0.6 to 0.8 in the second, which is the wall clock's
own noise around zero.

WHAT THIS SECTION USED TO SAY. "It is a transform of the fine stream", whose
ring is "mixed to DC and limited to the requested bandwidth", with "Its span
is the demodulation rate" and "Wider is the right direction for this: the
display shows the filter's skirts and what sits just outside them". It showed
the skirts, and they were the fault the owner saw: a pane wider than the
filter carried the filter's own magnitude response in its noise floor, full
level inside the passband and stopband level outside, and the hump moved with
every dragged edge. The resolution figure was "a 4096-point transform of a
48 kS/s fine ring resolves 11.7 Hz". And the section closed "This is also why
the feature is cheap. The expensive part, getting a receiver's baseband onto
the device at the right bandwidth, is already paid for by the demodulator":
the display tap is a second dispatch, measured above.

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

**The mapping freezes for the length of a gesture.** The pane's span is half
the display rate, and the display rate steps when the passband's reach
crosses a rung, so a drag across a rung widens or narrows the pane by a
factor of two or more. Left alone, the handle jumps out from under the
pointer at that moment, which is unusable. The pixel-to-hertz mapping is
therefore taken at drag start and held; frames arriving during the drag are
drawn into it by their own axis, so a narrower frame letterboxes and a wider
one is cropped, and neither is stretched. On release the axis eases back to
the live span over about 150 ms, so the change is seen rather than jumped.

WHAT THAT PARAGRAPH USED TO SAY: that the pane's span was the demodulation
rate, which is derived from the passband, "so widening the filter widens the
pane". The demodulation rate still moves that way; the pane no longer
follows it.

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

**A SWEEP WILL DROP RECEIVERS, AND BOTH ENDS NOW SAY SO.** Scrolling far
enough takes the front end past whatever a receiver was tuned to, and that
receiver goes. The window posts `receiverGoneText` from
`ui/models/receiver_gone.h`, naming the frequency it had recorded, and offers
the retune as the cause only when that frequency really is outside the span,
because all it can see is the receiver missing from the inventory. Since
2026-09-23 the wire says it directly: `setSourceCenter` answers with `removed`,
each receiver's id and the frequency it was on, and every audio and decoder
subscription on it gets `ended()` with a reason naming the retune.
`Client::retune_source` reads the list. The window still calls
`set_source_center`, which answers with the centre alone, so its sentence is
still the inferred one.

WHAT THIS PARAGRAPH USED TO SAY: "The engine is right to remove it and the
client empties its pane, but neither announces it". The window's half was
answered by "Say which receiver went, and refuse to guess why", and the
engine's by the list above.

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

### What counts as a signal on the flat pane

`ui/models/aft.h` carries the derivation; this is the result. Until the
display tap, the no-signal hold compared the loudest bin in the filter
against the frame's smoothed 5th percentile, at 10 dB for any signal and 15
dB for CW key-down. That percentile sat in the filter's stopband, so noise
inside the filter read about 85 dB above it and the hold could never trigger.
On the flat pane the same constants still pass noise: a frame is one windowed
transform, a noise bin's power is exponentially distributed with its 5th
percentile 12.90 dB under its mean, and the loudest of the 164 bins in a
6 kHz filter at 36.6 Hz sits about 7.5 dB over the mean, so noise alone read
about 20 dB "above the floor".

Three things replaced the two constants.

- **The noise reference is the mean**, each frame's raw 5th percentile plus
  12.90 dB, averaged over half a second. One frame's percentile of 512 bins
  scatters by about 0.86 dB, which would let noise through the gate in about
  18% of frames instead of the 5% it is sized for.
- **The per-frame gate is sized by the filter**: the level the loudest of N
  noise bins passes in one frame in twenty, 10 log10(ln(N / 0.05)) dB over the
  mean, which is 8.1 dB for 30 bins, 9.1 dB for 164 and 9.7 dB for 600.
- **Nothing is acted on until it is confirmed** across four distinct analysis
  windows. A peak must reappear within two bins of itself; a centroid must
  keep clearing the gate with no more than two frames missing, through its
  two-second average as well. One frame outside the jump allowance is an
  outlier, not acted on and not announced; two in a row are a jump.

The NFM and WFM centroid is taken over the occupied band only: the run of
bins over the threshold that contains the loudest, bridging gaps of four
bins, with the threshold at 12 dB under the loudest or 6 dB over the noise,
whichever is higher. Weighing every bin in the filter within 12 dB of the
loudest, as it did, pulled the centroid 167 Hz towards a second, weaker band
in the same filter in the case `ui/tests/test_aft.cpp` holds.

Measured in that file on synthetic flat-pane frames, 512 bins at 36.62 Hz and
a 6 kHz filter: two minutes of noise alone moved the receiver zero times on
each of the peak, keyed and centroid rules across twelve seeds, and a carrier
whose peak bin is 10 dB over the noise, 400 Hz off, was followed on both peak
rules and all twelve seeds in exactly four moves, the last of them 1.67 to
1.86 seconds in, ending 0.0 to 9.9 Hz from the carrier. The committed cases
run four of the seeds.

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

## Auto filter

The owner's request of 2026-09-23: when a receiver is tuned onto a signal,
fit its passband to that signal instead of leaving the edges to be dragged.
It is a tick box beside AFT in the receiver window, off by default and not
remembered across a restart, with a chip in AFT's style saying what the last
fit did. `ui/models/auto_filter.h` holds the rules and their cases are in
`ui/tests/test_auto_filter.cpp`; `ui/models/auto_filter_link.cpp` is when
it runs.

**Once, on a tune.** A fit is armed by a click on a detection and by ticking
the box, and by nothing else. A filter that refitted itself on every retune
would be a second pair of hands on the handles. Tuning by hand, the wheel, a
change of mode or a removed receiver drop a fit that is still measuring and
clear the chip.

**Never against the operator.** A fit that comes due while an edge or the
band is under the pointer is dropped, and so is one still measuring when the
operator touches the filter by any route. The drag wins and the fit is not
held over to land after it.

**From the pane, which is why it could not exist before the display tap.**
The pane is the air around the receiver with none of its filter in it, so an
occupied band can be read off it. One frame is too noisy for that, so the
frames are averaged as power over at least 0.4 s and four distinct analysis
windows, starting only once the engine's status confirms the receiver is
where the client put it; a bin is occupied when the average stands 6 dB over
the noise mean, which is each frame's 5th percentile plus 12.90 dB as AFT
reads it. The detection's occupied width, when the tune came from a
detection, widens a fit and never narrows one.

| Mode | Fit |
| --- | --- |
| AM | Symmetric about the carrier, out to the outermost pair of lines mirrored either side of it, chained outward from the carrier across gaps of up to 4 kHz, plus two bins |
| USB, LSB | From the suppressed carrier, the receiver's centre, to the far edge of the occupied band on whichever side holds more occupied power by 3 dB. Neither side ahead is no change |
| CW, and sideband signals under 500 Hz | A window on the tone: its occupied run down to 20 dB under its peak, plus a bin, at least 100 Hz |
| NFM, WFM, DSB | Symmetric about the centre, out to the occupied band's further edge plus two bins, or the detection's width if wider |
| raw | Nothing |

Mirrored lines rather than the loudest nearby, because a neighbour sits on
one side and a station's sidebands on both. The side of an SSB fit comes from
the energy and not from the mode's name, because on this engine the edges are
the sideband: a USB receiver on a signal below its centre is fitted below it.

**What it does not use.** `core/detect/groups.h` follows the lines one
emitter puts on the spectrum as a set, which is what "out to the outer
sideband lines" wants. None of it reaches the client: `rpc::Detection`
carries no group and nothing in `core/rpc` reads that header. So the AM rule
finds its lines on the pane.

**Clamped and shown.** Every fit is clamped to the channel's edge limit and
the minimum width, and is not made until the engine has stated the limit and
the receiver's edges. The edges are written into the request without marking
them as the operator's, so a later change of mode still takes that mode's
default. The pane eases its rules from the old edges to the fitted ones over
150 ms, holds them in the active colour to 700 ms, and arms the same span
ease a drag's release gets, since a fit can cross a rung.

**Measured on the synthetic scene** `synthetic:wideband?rate=2400000&center=100000000&emitters=10&modes=am,nfm&span_low=-100000&span_high=100000&snr_min=20&snr_max=35&seed=3`
on 2026-09-23. The AM station at 99.9922 MHz, whose generator records a
5 kHz band from a 2.5 kHz tone, was fitted to ±2.86 kHz, taking both lines
and leaving the NFM neighbour whose upper edge is 3.8 kHz below the carrier
outside. The NFM station at 99.9090 MHz, recorded as 10.8 kHz wide, was
fitted from the 16 kHz default to ±6.59 kHz. The first run of the AM case
bridged only 1.5 kHz between line pairs and fitted the carrier alone, 0.73
kHz wide; the 4 kHz bridge is the correction.

## Decoding

The decode section sits under RDS in the receiver window and puts
`Session.subscribeDecoded` in front of the operator for the focused receiver.
`ui/models/decoded_log.h` holds the rules, with cases in
`ui/tests/test_decoded_log.cpp`; `ui/models/decoded_link.cpp` is the wire half
and `ui/qml/DecodePane.qml` the layout.

**The menu is the engine's own list**, cut to the decoders whose
`DecoderInfo::modes` include the receiver's mode, in the engine's order, with
auto first wherever it would attach anything. A decoder added to the engine
appears without the client knowing its name. The section is absent where
nothing reads the mode, which is am, dsb and wfm; wfm keeps its RDS section.

| Receiver | Offered |
| --- | --- |
| usb, lsb | auto, rtty, sitor_b, navtex, psk31, psk63, qpsk31, cw |
| nfm | auto, ax25, pocsag |
| cw | auto, cw |
| raw | p25p1, dstar, tetra, m17 |

**Auto is the window's and means what `revenant-cli --decode auto` means**:
the decoder named after the mode where there is one, and otherwise every audio
decoder that reads the mode. The wire has no auto. Its empty name is the first
half alone and is refused on a usb or nfm receiver, so the window resolves auto
to names and subscribes each. A raw tap gets no auto, as the CLI's does not:
four decoders read one and nothing in the samples says which protocol is there.
Auto on a sideband receiver runs seven decoders at once, and a start-stop or
PSK decoder listening to a signal that is not its own prints framing noise, so
a decoder picked by name is the quieter log whenever the mode is known.

**It follows the focused receiver**, reconciled on every supervisor pass the
way the audio is, so a retune, a mode change, a clear and a reconnect are one
path. Subscriptions are cancelled before the window removes a receiver, so an
`ended()` is only ever about a removal somebody else made. A refused
subscription is a chip carrying the engine's sentence; a stream the engine
ended is a chip too, and that receiver is not asked again until the pane holds
a different one or the switch goes round.

**The log.** Newest at the bottom, following the newest line unless the
operator has scrolled up, with a "newest" button while they have. Each line is
a time, the decoder and the text line, in Cascadia Mono with fixed-width
columns. The time is where the message ended in the receiver's own stream,
`end_sample / sample_rate` as hh:mm:ss.t: nothing on the wire anchors a
sample index to a wall clock, so this is the same reading for a live radio and
a replayed capture, and the wall clock the line reached the window at is in
its expansion, named as an arrival. A click opens a line's fields: its kind,
receiver, sample span, sequence and arrival, then every field the decoder
sent, with the P25 identifiers in hexadecimal as the line above writes them.
An encrypted P25 header is a dim chip, "encrypted: talkgroup N", and never an
error. The log keeps 2000 lines; a chip counts what the cap let go and what
the engine's 256-message queue lost, and its sentence names the two apart.
Copy acts on a line, with its fields, or on the whole log, and clear empties
it. A change of receiver does not: what was decoded stays decoded.

**Driven with nobody at the mouse.** `revenant-ui --receiver FREQ:MODE
--decode NAME` opens a receiver and attaches a decoder once a source reaches
the frequency, and `--grab-receivers FILE` writes the receiver window to a PNG
as a `--smoke-seconds` run ends, on the offscreen platform. Measured on
2026-09-23 against revenant-engine on the 30 dB captures
`tests/rpc/test_rpc_decode_audio.cpp` writes, each behind 4 s of silence and
played at `--pace 1` through a 4-channel grid at 288000 S/s: rtty on a usb
receiver logged "CQ DE N0CALL" and "RYRY 0123456789 73", then four lines of
framing noise from the capture's noise tail; psk31 on a usb receiver logged
"CQ DE N0CALL" and "TEST 73" after one two-character line of noise; auto on an
nfm receiver attached ax25 and pocsag and logged all four POCSAG pages, the
512 bit/s one among them, and all four AX.25 frames with the APRS position,
status and message. p25p1 on a raw receiver at the channel centre, fed
`siggen dv --mode p25p1 --rate 288000` twice clear on talkgroup 1201 and twice
with `--algid 132` on talkgroup 2402, logged every header: the clear ones as
lines, the encrypted ones as lines behind an "encrypted: talkgroup 2402" chip.

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
