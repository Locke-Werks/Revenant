# Device calibration

Three corrections for what a cheap receiver gets wrong, kept per device and
restored when it opens: the crystal's frequency error, the DC offset that puts
a spike in the middle of the span, and the I/Q imbalance that puts a mirror
image of every signal on the other side of the centre.

The code is `core/source/frequency_correction.h`,
`core/source/corrected_source.h`, `core/source/calibration.h`,
`core/dsp/front_end_correction.h` and the two kernels
`core/shaders/iq_moments.comp` and `core/shaders/iq_correct.comp`. The engine
applies them in `core/engine/engine.cpp` and `core/engine/graph.cpp`, the wire
carries them as `Session.calibration` and `Session.setCalibration`, and the
client's section is under the device picker, with its rules in
`ui/models/calibration.h`.

## Frequency correction

### What it corrects

An RTL-SDR drives its tuner's synthesiser and its sample clock from one 28.8
MHz crystal. A crystal fast by e puts the oscillator at f (1 + e) when f was
asked for, so a station at F reads at F / (1 + e). At 20 ppm a station on
162.550 MHz reads 3251 Hz low, which is most of a narrowband voice channel.

### The sign

Positive means the crystal runs fast and labels read low. It is the sign
librtlsdr's `ppm=` takes, so a stored correction of 1500 ppb and `ppm=1.5`
would describe the same dongle if librtlsdr took fractions.

### Where it is applied, and why not in the device

The engine corrects its own arithmetic and tells the device nothing. Every
opened source is wrapped in a `CorrectedSource`: `tune()` asks the device for
the frequency that lands the real oscillator on the one requested, and
`center()` reports where the real oscillator is. `EngineInfo::source_center`
reads `center()`, and every absolute frequency the engine publishes, a
detection's centre and a receiver's place among them, is that centre plus a
baseband offset.

librtlsdr's `rtlsdr_set_freq_correction` was the alternative and was not used,
for three reasons:

- It takes whole ppm. One step is 100 Hz at 100 MHz and 1.7 kHz at the top of
  an R820T's range, so a dongle 0.4 ppm off cannot be corrected at all.
- It is an RTL-SDR call. A recording made by an uncalibrated dongle could not
  be corrected, and nothing else could be tested without a radio.
- On a streaming dongle it is another control transfer through the I2C
  repeater that stalls, so every change would cost a third of a second of
  samples.

The correction is an integer count of parts per billion, bounded at a million
either way, and each conversion rounds once, half away from zero, in integer
arithmetic. `docs/conventions.md` names crystal correction as one of the steps
that must not be done in floating point.

### What it does not reach

The baseband offsets are counted on the device's sample clock, which runs off
the same crystal. A carrier at offset d from the centre is really at
d (1 + e), so after correction its label is still off by d e: 1.2 Hz at 1 ppm
at the edge of a 2.4 MS/s span, 24 Hz at 20 ppm. The centre's error is 100 Hz
per ppm at 100 MHz, so the centre is the term worth correcting and it is
corrected exactly. Scaling the offsets too means scaling the grid's rational
axis and every receiver's placement, which is the engine's frame rather than
the source's, and is left open.

### A change moves the numbers and not the radio

Setting a correction does not retune. The device keeps listening where it was,
every receiver stays on the signal it was on, and the numbers move to what they
should have read. The server drops the detector's tracks, which were placed
against the old centre, and leaves the decoders alone. A retune afterwards asks
for the corrected frequency.

### Measuring it

The picker's calibration section takes a carrier whose true frequency is known
and measures it against the detection nearest it, within 200 ppm of the typed
frequency and never less than 5 kHz. The result replaces the correction in
force rather than adding to it, and is exact wherever the carrier sits in the
span, because the uncorrected label is recovered from the device's own centre.

A detection's centre is placed to a fraction of a bin, 36.6 Hz on the shipped
RTL-SDR geometry, so a measurement is good to about 20 Hz over the carrier:
0.12 ppm at 162.55 MHz. A narrowband carrier is the better reference: NOAA
weather radio, a GSM or DVB pilot, a beacon. A wideband FM station's detection
is the middle of its occupied band, which is its carrier only while the
programme is symmetric.

Measured on a synthetic front end whose crystal is 20 ppm fast, 162.55 MHz
carrier: 162546765 Hz uncorrected, 162550015 Hz corrected, each within its
36.62 Hz bin; measured back off the uncorrected bin, 19902 ppb.

## DC removal

A constant added to I and Q is a carrier at exactly 0 Hz. The stage subtracts
a running mean: each block's mean is averaged into the estimate with weight
1 - exp(-n / (tau rate)), tau 0.1 s, and the estimate is subtracted from every
sample. That is a notch 1.6 Hz wide that follows a jump in LO leakage within
about half a second.

Measured through the CPU twins, 2.4 MS/s, last half second of two: a
-33.01 dBFS spike falls to -124.09 dBFS, 91.1 dB, and a -20 dBFS tone 5 kHz
off centre moves by less than 0.001 dB. Through the engine on the device the
spectrum shows the spike 58.2 dB down, which is the spectrum's floor rather
than the correction.

## I/Q correction

The two arms of a quadrature mixer differ slightly in gain and are not quite
90 degrees apart, which leaves an image of every signal on the other side of
the centre. A gain ratio of 1.05 and 3 degrees is 28.9 dB of image rejection.

The estimator is blind and feed-forward: it assumes the stream is proper, I and
Q of equal power and uncorrelated, which holds for noise, for any single tone
and for any spectrum that is not its own mirror image. The model is
I_r = I, Q_r = g (Q cos(phi) + I sin(phi)); the second-order moments give
g = sqrt(E[Q^2] / E[I^2]) and sin(phi) = E[IQ] / sqrt(E[I^2] E[Q^2]), and the
correction orthonormalises Q against I. `core/dsp/front_end_correction.h`
cites Moseley and Slump for the feed-forward estimator and Fatadin, Savory and
Ives for the Gram-Schmidt correction. Time constant 1 s. An estimate past 6 dB
or 30 degrees is reported and not applied: a stream like that is not one the
model holds for.

Measured through the CPU twins: image rejection from 28.93 dB to 73.51 dB,
the estimate reading 0.4243 dB and 3.018 degrees against 0.4238 and 3. Through
the engine on the device the spectrum shows 28.9 dB going to 70.8 dB.

### Where it runs

After the convert kernel and before the channelizer, so every consumer of the
ring sees the corrected stream. `iq_moments.comp` sums I, Q, I^2, Q^2 and IQ
over fixed 64-sample chunks in order, and `iq_correct.comp` corrects the block
in place. Both are bit-identical to their twins on the RTX 4090. The estimate
runs on the host in double, reading a frame slot's moments when the graph
reuses it, so a block is corrected with what the blocks three frames earlier
measured: about 41 ms at the default block, against time constants of 0.1 s
and 1 s. Neither kernel runs while both halves are off, and switching the stage
on from off starts the estimate afresh.

## Where it is kept

The engine keeps it, keyed by backend and serial, `rtlsdr:00000001`. The
engine is what opens the radio, including when no client is connected:
revenant-cli and a revenant-engine started with a URI both open a dongle with
nobody watching. A client on another machine holding the correction for a
dongle it cannot see would apply it to whatever that engine opens next.

revenant-engine writes `calibration.txt` beside its token file,
`%LOCALAPPDATA%\Revenant\calibration.txt` by default, or where
`--calibration-file` says. One device a line, tab separated:

```
rtlsdr:00000001	ppb=-1250	dc=on	iq=off
```

A field this build does not know is skipped, so a file from a later engine
does not cost an earlier one every device in it. A file that cannot be read is
never written over; the engine opens the device uncalibrated and says why.

A source with no serial, every file and every synthetic scene, is calibrated
for the session and not kept.

### Two stock dongles share a serial

RTL-SDRs ship with the serial `00000001` and most are never reprogrammed, so
two stock dongles share one calibration. `rtl_eeprom -s <serial>` writes a
distinct one. An index is not a key either: two dongles swap places across a
replug.

### ppm= on the URI

An rtlsdr URI with `ppm=` has the device correct its own crystal through
librtlsdr. The engine then holds the stored correction and does not apply it,
because two corrections of one crystal correct it twice, and the calibration's
note says so.

## What needs the real dongle to confirm

Everything above was measured on synthetic sources. These are the parts only
the hardware can settle:

- That the R820T's synthesiser and the RTL2832U's sample clock share the
  crystal on the owner's dongle, so one correction serves both. An RTL-SDR v3
  is built that way; a clone need not be.
- The owner's dongle's actual error, measured against a known carrier, and how
  far it moves as the dongle warms up.
- That `rtlsdr_get_usb_strings` returns the serial on an open handle on this
  platform, and what it returns for the owner's dongle.
- How large the DC spike and the I/Q imbalance are on an R820T. The R820T
  delivers a real IF that the RTL2832U converts to I/Q digitally, so its
  imbalance is expected to be small and its centre spike to come from the
  digital path; the I/Q estimate reading near zero there is itself a result
  worth recording. An E4000, which is zero-IF, is where both should show.
- That the DC estimate follows the step in LO leakage a retune or a gain change
  brings, at the 0.1 s time constant, without a visible transient on the
  waterfall.
- That the correction survives an engine restart against the real file in
  `%LOCALAPPDATA%\Revenant`.
