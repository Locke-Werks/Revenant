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
baseband at the demodulation rate into a ring on the device: already mixed to
DC, already limited to the requested bandwidth, already the exact span this
display wants. One small FFT of that ring gives native resolution across the
passband. Zooming the wide transform instead gives however many of its bins
happen to fall inside sixteen kilohertz, which at any usable wide span is a
handful, and no amount of interpolation puts the resolution back.

This is also why the feature is cheap. The expensive part, getting a
receiver's baseband onto the device at the right bandwidth, is already paid
for by the demodulator.

## AFT

Automatic frequency tracking, as an option and off by default. It follows a
drifting carrier by nudging the receiver's centre so the signal stays where
the filter is.

It rides the retune path that already exists. A retune that changes only the
frequency is a push constant and a new tap table, taken in place by the stage
with no gap in the audio; that is measured, not assumed, in the engine suite's
retune case. So AFT is a control loop over `Engine::set_vrx_params` and needs
no new machinery in the sample path.

Two properties it must have, because without them it is worse than nothing:

**A deadband.** A loop with no deadband hunts, and a receiver that walks
around its own signal while the operator is trying to tune it is infuriating
in a way that is hard to diagnose from a bug report.

**A rate limit, and it yields to the operator.** Drift is slow. Anything fast
is either a second signal or the operator turning the dial, and in both cases
the correct response is to stop tracking rather than to chase. Manual tuning
always wins; AFT resumes from wherever it was left.

Whether AFT locks to a carrier peak or to the centroid of the passband energy
depends on the mode, and is not settled here. A carrier is the right target
for AM and for CW; a suppressed-carrier mode has no peak to find and wants the
centroid or nothing at all.
