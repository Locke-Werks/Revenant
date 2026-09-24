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

revenant-cli is the exception to the paragraph above: it starts its engine
with no calibration file and never sets one, so a dongle opened there runs
uncorrected whatever the file says. Measured on 2026-09-23: its detector
listed the DC offset as a track at exactly the centre, 14.175000 MHz, 183 Hz
wide, 12.9 dB.

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

## On the owner's dongle

Everything in the sections above was measured on synthetic sources. This one
is the hardware, measured on 2026-09-23: the RTL-SDR on the owner's machine,
which librtlsdr names "Generic RTL2832U OEM" and whose tuner answers as an
R820T/R820T2. The instruments are the three `[.probe]` cases in
`tests/engine/test_rtlsdr_front_end.cpp`, which take the machine-wide dongle
lock and write raw captures and engine traces to `REVENANT_PROBE_DIR`; the
file says how each is driven. Unless a paragraph says otherwise the dongle
was at 98.5 MHz, 2.4 MS/s and gain 20.

### The serial

`rtlsdr_get_usb_strings` on the open handle returns the serial on Windows,
on every open and every describe in the session: `20202020`. The key is
`rtlsdr:20202020`, not the stock `00000001`. The owner's own
`%LOCALAPPDATA%\Revenant\calibration.txt` already carried a line under that
key, written by revenant-engine.

### The DC offset is a constant from the digital path

-58.87 dBFS: I and Q each -8.05e-4 of full scale, which is -0.103 of one
8-bit step, the same on both arms. Over 61 three-second raw captures in 30
minutes it read -59.24 to -58.26 dBFS.

It does not move with gain. The engine's running estimate at gain 0 read
-58.87 dBFS with a standard deviation of 0.06 dB across 23 readings, I and Q
each -8.05e-4 with a deviation of 9e-6. It does not move with frequency, and
it is there with the tuner out of circuit: direct sampling at 2.5, 5, 7.15
and 10 MHz read -58.55 to -58.96 dBFS. So the spike comes from the
RTL2832U's digital path, as the R820T's real IF predicted, and there is no
LO leakage for a retune or a gain change to step.

On the spectrum, at 36.6 Hz bins (64 channels), it stands 6.2 and 7.2 dB over
the local floor on two three-second averages, across bins -1 to +1, which is
the spectrum window's spread of one line. At 293 Hz bins, which is
revenant-engine's own choice at 2.4 MS/s with 8 channels, it stands 1.7 and
2.4 dB over, because the floor in a bin eight times wider is 9 dB higher. At
gain 0 the floor falls 22 dB and the offset does not, which is where it is
most visible.

### What DC removal leaves: nothing

On raw captures, a 65536-point Hann FFT at 36.6 Hz bins over three seconds:
the centre bin 7.8 dB over the floor before, 0.4 dB after subtracting the
capture's mean, and no bin within 64 of the centre more than 3 dB over the
floor. Through the engine on the device, with the 0.1 s running mean: 0.1 to
1.2 dB over the floor on three-second averages in four separate segments at
36.6 Hz bins, and no bin over 3 dB. In direct sampling at 2.5, 5 and 7.15
MHz the same subtraction leaves 0.5 to 1.0 dB.

There is no low-frequency hump beside the 0 Hz term on this dongle, so there
is nothing for software offset tuning to move aside, and it was not built.

### Following a retune or a gain change

Three retunes onto an empty centre (99.3 to 98.5, 98.5 to 98.7 and 98.7 to
98.5 MHz) and four gain increases (20 to 40 and 0 to 20, twice each) left no
frame after them above what the centre reads on noise alone: a frame's centre
is the strongest of three bins against a median, so noise alone takes single
frames to 8 to 12 dB over the floor, and the three-second averages after
each event were -0.1 to 1.2 dB. A fourth retune, onto 99.3 MHz, landed on a
station's carrier and says nothing either way.

A gain DECREASE does leave a visible transient. From 40 to 0: at 36.6 Hz bins
nine frames covering the first 250 ms of samples after the pause stood 8 to
24 dB over the floor; at 293 Hz bins seven frames stood 11 to 19 dB over.

It is not a step in the offset, which stays at -58.87 dBFS. It is the
estimate's own noise. A tenth of a second of a strong band does not average
to its offset: at gain 40 the estimate read -59.93 dBFS with a standard
deviation of 3.71 dB and I scattered by 5.4e-4 against a true -8.05e-4; at
gain 20 the deviation was 0.79 dB and at gain 0 0.06 dB. Whatever error the
estimate held when the gain dropped stands against a floor 22 dB lower until
the average replaces it. Because the offset is a constant of the device, a
longer time constant or a stored per-device value would remove both the
noise and the transient; neither is done.

### I/Q imbalance: nothing to correct

The blind estimate over the 61 raw captures: gain error +0.0008 dB with a
deviation of 0.0063 dB, phase error -0.012 degrees with a deviation of 0.044
degrees, both indistinguishable from zero, and an image rejection per capture
of 58 to 105 dB. The engine's own one-second estimate read 0.0024 dB and
-0.018 degrees, 73.6 dB of image rejection. Direct sampling at five centres:
gain within 0.01 dB of zero, phase within 0.11 degrees. The estimate stays
plausible, so `iq=on` applies a correction within a hair of the identity and
costs nothing but the two dispatches.

### The crystal: 0.72 ppm slow

The reference is WWV, whose carriers NIST holds to its frequency standard,
received through direct sampling on the Q branch, which runs the whole path
off the 28.8 MHz crystal with the tuner out of it. Eight seconds each, the
carrier's frequency from the slope of its phase:

| Carrier | Centre | Read | Error | ppm | SNR |
| --- | --- | --- | --- | --- | --- |
| 2.5 MHz | 2,503,125 Hz | +1.84 Hz | high | -0.735 | 26 dB |
| 5 MHz | 5,006,250 Hz | +3.62 Hz | high | -0.723 | 37 dB |
| 10 MHz | 10,012,500 Hz | +7.00 Hz | high | -0.700 | 16 dB |

The 10 MHz reading is the weakest and its phase wandered by 4.4 radians
against 0.31 at 5 MHz. Labels read high, so the crystal is slow: the stored
correction for this dongle is -725 ppb. At 162.55 MHz that is 118 Hz of label.

Through the tuner, the carrier of the broadcast station on 98.1 MHz, from the
mean of its FM discriminator over 61 captures, read -0.751 ppm with a
standard error of 0.043, which agrees with WWV to 0.03 ppm. That is what one
crystal shared by the R820T's synthesiser and the RTL2832U predicts. A
broadcast carrier is not a reference on its own: the station on 98.9 MHz read
-1.14 ppm, standard error 0.21, and the tuner's own step is part of each
reading. Nor is a stereo pilot: the two stations' 19 kHz pilots, read through
the same clock, differ from each other by 97 ppm.

### Warming up

The same 30 minutes, streaming continuously from the session's first open.
How long the dongle had been idle before that is not known, so this is not a
cold start. The 98.1 MHz pilot is steady enough to read the dongle's sample
clock against: over the 43 captures it came through cleanly (the others are
in the next section), the mean of
the first five minutes and of the last five were -6.49 and -6.50 ppm off
19 kHz, and every capture fell between -6.59 and -5.93. The carrier through the
tuner read -0.78 ppm over the first ten captures and -0.76 over the last ten,
with a deviation of 0.33 per capture. No drift was resolved: under 0.1 ppm
on the sample clock, and under the carrier's scatter on the synthesiser.

### Samples lost without being counted

11 of those 61 captures carry a step in the phase of both stations' pilots at
the same instant, to within the 10 ms the analysis resolves, with 0.3 to 2.5
radians of residual against 0.02 on a clean capture; 7 more are disturbed
without a step it could place in both. A step common to two transmitters is a slip
in the receiver's timeline, which is what samples lost inside the dongle or
the USB stack would produce, and SourceStats reported no loss for any of
them. The machine was running several builds at the time. It was not
investigated further; a pilot's phase is how to look.

### The calibration across a restart

One engine opened the dongle with a calibration file, set -725 ppb with DC
removal and I/Q correction on, and was destroyed. The file then held

```
rtlsdr:20202020	ppb=-725	dc=on	iq=on
```

and a second engine opening the same URI on the same file came up with all
three in force, persisted and applied: `source_center` 98,499,929 Hz against
the device's 98,500,000, the 71 Hz that 725 ppb is of 98.5 MHz. Two engines
in one process on a scratch file, which is the same load and save the
revenant-engine process runs; the owner's own file was left as it was.

### HF through direct sampling

`direct=q` on an rtlsdr URI puts the v3's HF input on the Q branch. The
reachable range is then 0 to 14.4 MHz, half the crystal, and a centre above
it is refused: 15,018,750 Hz was. The URIs that worked:

```
rtlsdr://0?freq=7150000&rate=2400000&direct=q     40 m
rtlsdr://0?freq=14175000&rate=2400000&direct=q    20 m
```

The top of a span centred on 14.175 MHz passes 14.4 MHz, and what lies above
that is folded rather than received.

The antenna hears HF: WWV at 2.5, 5 and 10 MHz above. What the detector found,
`revenant-cli <uri> --detect --duration 30`, at 22:08 local time:

- A comb of lines about 15.79 kHz apart across the whole span on both bands,
  0.2 to 2.0 kHz wide, 6 to 14 dB, which tier two calls a carrier or 120 Bd
  PSK. Something local is radiating it. It is most of what either run
  lists: 134 to 139 tracks on 20 m, 119 to 120 on 40 m.
- The DC offset at exactly the centre, 183 Hz wide: 12.9 dB at 14.175000 MHz
  and 9.0 dB at 7.150000 MHz, because revenant-cli applies no calibration.
- On 20 m, off the comb: 14.279931 MHz, 10.7 kHz wide, 7.8 dB; 14.288911
  MHz, 4.0 kHz, 6.0 dB; 14.226252 MHz, 2.0 kHz, 6.6 dB. None classified.
- On 40 m, nothing inside 7.000 to 7.300 MHz but the comb and the centre.
  Above it, 7.381593 MHz, 15.5 kHz wide, 14.4 dB, a broadcast by its width.

No amateur CW or SSB was identified on either band at that hour. Live HF CW
on 40 m would sit at -150 to -25 kHz on the 7.15 MHz URI, clear of the
centre, with comb lines through it at 7.010492, 7.042096, 7.073663, 7.089544,
7.105231 and 7.121089 MHz.

### Still open

- A cold start. The warm-up above began from an idle time nobody recorded.
- The transient after a gain decrease, and the estimator noise behind it.
- The comb's source, which is the first thing between this antenna and HF
  work.
- The uncounted slips.
- An E4000, which is zero-IF, is where LO leakage and a real imbalance would
  show. There is none here to measure.

