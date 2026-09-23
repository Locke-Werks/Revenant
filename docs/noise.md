# Noise mitigation

Three stages per receiver, each off by default: an impulse blanker on the
receiver's channel, a manual notch and an automatic notch on its audio, and
spectral noise reduction on its audio. Each is a GPU kernel with a CPU twin
that matches it bit for bit, and each is a field on the receiver's params, so
it rides a retune, a place in the client's rack and the wire like the
receiver's frequency does.

| Stage | Kernel | Twin | Runs on |
| --- | --- | --- | --- |
| impulse blanker | `core/shaders/noise_blank.comp` | `reference_blank_detect`, `reference_blank_apply` | the receiver's coarse channel, ahead of its fine filter |
| manual and automatic notch | `core/shaders/noise_line.comp` | `reference_line` | the demodulated audio, in place |
| noise reduction | `core/shaders/noise_spectral.comp` | `reference_spectral` | the demodulated audio, in place, after the notches |

The twins, the designs and the citations are in
`core/dsp/noise_reference.h`; `core/engine/noise_stage.h` is the engine side
and the three points in `core/engine/vrx_stage.cpp` it is called from.
`tests/reference/test_noise.cpp` holds each kernel to its twin, across ring
wraps and across dispatch boundaries of awkward lengths for the two that
carry state, and `tests/engine/test_engine_noise.cpp` is where every figure
below comes from.

## What each stage is offered on

| Mode | Blanker | Notch | Automatic notch | Noise reduction |
| --- | --- | --- | --- | --- |
| AM, USB, LSB, DSB | yes | yes | yes | yes |
| CW | yes | yes | refused | yes |
| NFM, WFM | yes | no | no | yes |
| raw, P25, D-STAR, TETRA | no | no | no | no |

A request for a stage a mode does not offer is refused by `engine::place` on
the call, in a sentence naming the stage and the mode, rather than accepted
and quietly ignored. The automatic notch is refused by name on CW because
the steady tone it exists to remove is the signal a CW operator is copying.
The manual notch needs a mode whose audio frequency follows from where a
signal sits in the passband, which the FM modes do not have. The complex taps
have no audio. A stereo WFM receiver runs the blanker and not the two audio
stages, since a notch or a noise floor per channel would pull the stereo image
around; it is declined at plan time rather than refused, because whether a
receiver is decoding stereo is the station's pilot's decision.

The client carries the same table in `ui/models/noise_controls.h`, so it can
grey a control out rather than let it be clicked into a refusal, and a mode
change keeps whichever settings the new mode offers.

## The impulse blanker

On the coarse channel at the channel rate, before the receiver's filter
narrows it. That placement is the whole design: an impulse a few source
samples long is a few channel samples long there, and after a 2.7 kHz filter
the same impulse is a millisecond of the filter's own ringing that no
threshold can cut out without cutting the programme with it.

Two passes. The first flags a sample whose energy stands more than the
threshold above the mean energy of a reference window ending a guard before
it; the second zeroes every sample within the hang after, or the lead before,
a flagged one. It blanks and does not interpolate. With it on, the fine stage
reads the receiver's own blanked copy of its channel instead of the channel
ring, and trails the channel by the lead.

| Figure | Value | Why |
| --- | --- | --- |
| threshold | 12 dB, adjustable in [3, 40] | complex Gaussian noise passes 12 dB over its mean with probability 1.3e-7 |
| reference window | 0.5 ms of channel samples, clamped to [16, 256] | 72 samples at 144 kS/s |
| guard | the lead plus 2 samples | keeps the impulse's own leading edge out of its reference |
| hang, lead | 2 channel samples each, 14 us at 144 kS/s | measured, below |

The hang was chosen by measurement and the obvious answer was wrong. An
impulse reaches the channel through the channelizer's prototype filter and
arrives smeared over twice the taps per branch in channel samples, 34 at the
default grid, so the first design blanked half of that either side. On the
test's impulses, with the threshold at 12 dB, the voice band's SNR after
blanking was 25.9 dB at a hang and lead of 1, 30.4 at 2, 29.6 at 3, 26.8 at 6,
24.6 at 9 and 20.9 at 17, from -0.2 dB unblanked. The threshold already flags
every sample of the smear that stands clear of the background, and a gap a
fraction of a millisecond long puts its distortion straight into the voice's
band, so hang beyond two samples only cuts programme. Across 10 to 15 dB of
threshold at a hang of 2 the figure stayed between 29.8 and 30.7 dB.

A receiver started with the blanker on starts its fine stage one reference
reach later, a fraction of a millisecond, so its first window is channel
samples and not the ring's clear value. A blanker switched on mid-stream, or
left behind by a re-anchor, starts again from where the fine stage next
reads.

## The manual notch

A second-order section with a zero pair and a pole pair on the notch's angle
(Oppenheim and Schafer, chapter 5): the pole radius sets the width and the
zero radius the depth, scaled to unit gain at whichever of DC and Nyquist is
further from the notch. Depth in [3, 80] dB, default 40; width in [10, 2000]
Hz, default 100.

Its frequency is signed hertz from the receiver's centre, in the same frame as
the passband edges, so a display draws it where the interference is. The
engine maps it to the audio frequency it lands on: itself on USB, its negative
on LSB, its magnitude on AM and DSB, and its sum with the pitch on CW. One
outside the granted passband is kept but not applied, because the filter has
already removed what it would cut, and dragging an edge past it does not fail
the drag. A notch that lands on no audio at all, the discarded side of a
sideband or a CW carrier's own zero, is refused.

`tests/reference/test_noise.cpp` measures the section the kernel actually runs,
float coefficients and all, at four frequencies from 350 Hz to 15 kHz: the
depth within 1 dB of what was asked, between 1.5 and 4.5 dB down half a width
either side, and within 0.2 dB of unity ten widths away.

## The automatic notch

An adaptive line enhancer (Widrow et al., Proc. IEEE 63(12), 1975): a
128-tap predictor sees the audio 5 ms late and learns what it can predict,
and the output is its error, so what it learned is subtracted. Normalized LMS
with leakage (Haykin, "Adaptive Filter Theory"), a step of 1e-4 and a leak of
3e-6 per sample.

The predictor reads every S-th sample, with S the largest whole number that
keeps the audio rate over S at 2.5 times the passband's reach, up to 8. At
48 kS/s 128 adjacent taps resolve 375 Hz and would take a wide bite out of any
voice near the tone; a USB receiver gets S = 7 and resolves about 54 Hz. The
images the stride makes land where the passband put no audio, which is why it
is offered on the linear modes only.

It costs a voice something, and the step is where that is decided. A voiced
syllable is a harmonic series that holds long enough to be learned, so a
predictor quick enough to learn a whistle also learns part of a vowel. On the
test's voice rendered as audio, with the stride and delay a USB receiver gets:

| Step | Voice alone, output SNR | Voice and a whistle at its level, output SNR |
| --- | --- | --- |
| 5e-5 | 23.6 dB | 9.4 dB |
| 1e-4, shipped | 18.0 dB | 13.1 dB |
| 2.5e-4 | 11.0 dB | 14.8 dB |
| 1e-3, the first value tried, at a 2 ms delay and a leak of 1e-5 | 2.9 dB | 13.4 dB |

The whistle untouched is -0.6 dB. The first value tried was the obvious one for a
tone and would have wrecked a voice with no whistle to cancel. At 1e-4 a
steady tone is 12.6 dB down in its second second in the conformance suite.
It is a control to switch on when there is a heterodyne and off when there is
not, and the panel says so.

## Noise reduction

Short-time power subtraction on 50 percent overlapped frames with a
square-root Hann window both sides (Allen 1977; Boll 1979), with an
over-subtraction factor and a spectral floor (Berouti, Schwartz and Makhoul
1979), against a noise floor from continuous minimum tracking per bin
(Doblinger 1995). The transform is a direct DFT against a table the host
designs, which a workgroup finishes in microseconds at audio frame rates and
which a twin can transcribe.

| Figure | Value |
| --- | --- |
| frame | the largest power of two at or below Fa / 90, in [64, 512]: 512 at 48 kS/s, a bin every 94 Hz |
| latency | one frame, 10.7 ms at 48 kS/s |
| power smoothing | 0.7 per hop, an 18 ms time constant at 48 kS/s |
| floor tracker | gamma 0.998 and beta 0.96 per hop: the floor falls at once and rises with a 2.7 s time constant |
| bias | 2.4, measured: the tracked minimum settles 3.8 dB under the true mean on white noise, and the conformance suite holds the corrected estimate within 1.5 dB |
| strength s | over-subtraction 1 + s, floor at -(6 + 12s) dB; default 0.5 |

A Wiener gain with the SNR taken from the smoothed power is the same function
as the subtraction at an over-subtraction of one, so it is not a second
option. A decision-directed SNR estimate in its place measured worse on the
same voice, as noted in `core/dsp/noise_reference.h`.

Switching it on starts it from an empty frame, so the first frame is silence;
switching it off drops the frame in progress. Either is a 10 ms step in the
audio, once.

All three stages are indifferent to level: the blanker compares a ratio, the
predictor is normalized by its own input, and the subtraction's gain is a
ratio of powers. That matters here because the engine has no AGC yet.

## Measured

`tests/engine/test_engine_noise.cpp`, through the real engine on the RTX 4090:
a file source at 1.152 MS/s, 16 channels, a USB receiver at the default 300 to
2700 Hz passband and 48 kS/s audio. The voice is a harmonic series on a
gliding pitch with three formants and a syllabic envelope with pauses, 60 dB
under full scale, 9 kHz off a channel centre. The impairments, each alone and
all together, are white noise at 15 dB SNR in 2500 Hz, a carrier 1300 Hz above
the voice's carrier at the voice's own RMS, and 100 impulses a second, three
source samples long, at a thousand times the voice's RMS, which is full scale.

OUTPUT SNR is the clean receiver's audio against each receiver's, at the best
alignment within the stages' latency, with a least-squares gain, over 2.25 s
after 0.75 s of settling. Whatever a stage does to the voice counts against it.

| Capture | Configuration | Output SNR |
| --- | --- | --- |
| clean | blanker | 67.9 dB |
| clean | automatic notch | 14.9 dB |
| clean | noise reduction 0.5 | 21.4 dB |
| noisy | all off | 12.9 dB |
| noisy | noise reduction 0 | 14.5 dB |
| noisy | noise reduction 0.5 | 14.5 dB |
| noisy | noise reduction 1 | 13.2 dB |
| whistle | all off | -3.2 dB |
| whistle | manual notch at the whistle | 8.5 dB |
| whistle | automatic notch | 11.0 dB |
| whistle | both notches | 8.1 dB |
| impulsive | all off | -2.2 dB |
| impulsive | blanker | 29.0 dB |
| dirty | all off | -5.7 dB |
| dirty | blanker | -3.3 dB |
| dirty | manual notch | -3.5 dB |
| dirty | automatic notch | -2.8 dB |
| dirty | noise reduction 0.5 | -3.4 dB |
| dirty | blanker, manual notch, noise reduction | 7.3 dB |
| dirty | blanker, automatic notch, noise reduction | 10.6 dB |

On the dirty capture each stage alone barely moves the figure because the
other two impairments still dominate it; together they take it from -5.7 to
10.6 dB. The manual notch scores under the automatic one on the whistle
because a 100 Hz notch at 1300 Hz also takes the voice's harmonics that pass
through it, and the output SNR counts that.

The output SNR does not show what noise reduction does in the pauses, where
there is no voice to distort. There, against the untouched receiver, the noise
is 6.0 dB down at strength 0, 11.9 dB at 0.5 and 17.8 dB at 1.

IMPULSE ENERGY REMOVED, on the coarse channel, from a raw tap on the voice's
centre in the clean and impulsive runs and the blanker's twin over the
impulsive tap: 27.6 dB of the impulse energy across the channel, and in the
voice's band the voice-to-impulse ratio goes from 0.0 dB to 30.4 dB. The same
twin over the clean tap cut the voice's band by 71 dB, which is to say it
found nothing to cut.

## What is not done

- The blanker blanks and does not interpolate across what it removes.
- The blanker is not offered on the complex taps, which the digital voice
  decoders read.
- The automatic notch costs a voice with no whistle about 15 dB of output SNR
  on the test's voice. A detector that switched it off with no steady tone
  present would remove that cost, and does not exist.
- Stereo WFM gets the blanker and neither audio stage.
- Nothing here has been listened to on air. Every figure is the synthetic
  voice above, and a voice is not a harmonic series with three formants.
