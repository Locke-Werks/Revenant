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
