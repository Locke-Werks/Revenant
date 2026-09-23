# Sensitivity

The SNR at which each decoder reaches a stated error rate, read off a swept
curve, one command per mode to reproduce it. These are the numbers the project
rule about not shipping a decoder that quietly does worse than the free tool
it replaces is held to, so each one names what was counted and against what.

## No comparison with other tools

Nothing here has been measured against fldigi, multimon-ng, Direwolf, DSD, the
M17 reference implementation or any other decoder. The figures below say where
Revenant's decoders stand against its own transmitters in white noise. They do
not say whether that is better or worse than anything else, and nothing in
this document should be read as saying so. Measuring the free tools on the
same waveforms is the obvious next step: `siggen` writes any trial below to a
file for exactly that.

## How the numbers were made

`bench sweep --mode NAME` runs the mode's transmitter in `core/dsp/synth`,
adds white Gaussian noise through `core/dsp/synth/channel.h` or
`add_real_awgn`, runs the decoder in `core/decode`, and scores what came back.
Each mode's grid, trial count and payload live in
`tools/bench/mode_subjects.h` and its three source files, so the command alone
reproduces the curve; `--seed 20260918` is the seed the nightly and the
committed baselines under `tests/baselines/` use. Every trial of every point
runs, with no early stop, because a failed trial takes its errors in a clump
and stopping on an error count measures that trial rather than the decoder.
Sensitivity is the crossing of the stated error rate, interpolated linearly in
log10 of the rate against dB between the grid points either side.

The channel is white noise and nothing else: no fading, no multipath, no
impulse noise, no transmitter clock error. The PSK and CW tones sit a few
hertz off the receiver's centre, as the tests put them, and that is the only
impairment besides the noise. A real HF or VHF channel will cost more than
these figures show, by an amount nobody has measured here yet.

## The table

SNR is signal power over noise power in a 2500 Hz reference bandwidth, the
convention [snr-convention.md](snr-convention.md) fixes. For the audio modes
it is the audio's SNR in 2500 Hz of one-sided spectrum; for the complex
baseband modes, the RF signal's. RDS keeps the Eb/N0 axis its subject was
written on. Measured on 2026-09-23 on the RTX 4090 workstation's CPU, commit
"Read a decoder curve at its own mode's error rate in compare", except CW,
measured again the same day at "Hold the start of a CW transmission until its
spacing shows two clusters".

| Mode | Unit counted | Error rate | SNR convention | Sensitivity | Command |
| --- | --- | --- | --- | --- | --- |
| RTTY, 45.45 Bd, 170 Hz shift | character | 0.01 | 2500 Hz | -5.2 dB | `bench sweep --mode rtty --seed 20260918` |
| AX.25, 1200 bit/s Bell 202 | frame | 0.01 | 2500 Hz | 10.8 dB | `bench sweep --mode ax25 --seed 20260918` |
| POCSAG, 512 bit/s | page | 0.01 | 2500 Hz | 7.3 dB | `bench sweep --mode pocsag-512 --seed 20260918` |
| POCSAG, 1200 bit/s | page | 0.01 | 2500 Hz | 9.5 dB | `bench sweep --mode pocsag-1200 --seed 20260918` |
| POCSAG, 2400 bit/s | page | 0.01 | 2500 Hz | 12.0 dB | `bench sweep --mode pocsag-2400 --seed 20260918` |
| SITOR-B | character | 0.01 | 2500 Hz | -1.1 dB | `bench sweep --mode sitor-b --seed 20260918` |
| NAVTEX | message | 0.01 | 2500 Hz | -2.6 dB | `bench sweep --mode navtex --seed 20260918` |
| PSK31 | character | 0.01 | 2500 Hz | -8.8 dB | `bench sweep --mode psk31 --seed 20260918` |
| PSK63 | character | 0.01 | 2500 Hz | -5.8 dB | `bench sweep --mode psk63 --seed 20260918` |
| QPSK31 | character | 0.01 | 2500 Hz | -9.5 dB | `bench sweep --mode qpsk31 --seed 20260918` |
| CW, 20 WPM | character | 0.01 | 2500 Hz | -6.2 dB | `bench sweep --mode cw --seed 20260918` |
| M17 stream mode | frame | 0.01 | 2500 Hz | 17.0 dB | `bench sweep --mode m17 --seed 20260918` |
| P25 Phase 1, C4FM | bit | 0.01 | 2500 Hz | 17.9 dB | `bench sweep --mode p25p1 --seed 20260918` |
| D-STAR, GMSK | bit | 0.01 | 2500 Hz | 17.3 dB | `bench sweep --mode dstar --seed 20260918` |
| TETRA, pi/4-DQPSK | bit | 0.01 | 2500 Hz | 20.2 dB | `bench sweep --mode tetra --seed 20260918` |
| RDS | bit | 0.01 | Eb/N0 | 7.2 dB | `bench sweep --mode rds --snr-start 2 --snr-stop 12 --trials 1024 --min-bit-errors 0 --seed 20260918` |

Every figure is the crossing in that mode's committed baseline,
`tests/baselines/ber-vs-snr-NAME.json`, which holds the whole curve. `bench
compare` of a baseline against itself prints it. `siggen NAME`, or `siggen
trial --mode cw`, writes any one trial of any of these sweeps but RDS to a
file with what was sent beside it, through the sweep's own generator, named by
the sweep's seed, the point and the trial: `siggen help` has the options.
RDS has `siggen rds --ebn0`.

What each unit is:

- **character**: edit distance between the text sent and the text decoded,
  over the characters sent, so a character dropped or invented costs one. A
  trial that printed garbage is capped at one error per character sent.
- **frame**: a frame that did not come back byte for byte. AX.25 frames carry
  60 information octets, 83 octets before the FCS; M17 frames are the 16-byte
  stream payloads after one Link Setup Frame.
- **page**: a POCSAG page whose identity and 40-character alphanumeric text
  did not both come back.
- **message**: a NAVTEX message of 120 characters not received exactly with a
  clean preamble, which is what M.540-2 Annex II clause 3 lets a receiver
  print. Each is its own transmission with ten seconds of phasing.
- **bit**: a demodulated bit wrong after aligning the recovered stream to the
  one sent. A trial that cannot be aligned, or a TETRA burst not found, is
  scored as every bit half wrong. P25 and D-STAR are a raw stream through the
  demodulator with the first 64 symbols or bits left out as acquisition;
  TETRA is the 216 bits of block 2 of each synchronisation burst.

## Converting to other conventions

The 2500 Hz figure converts to Eb/N0 without the sample rate:
`EbN0_dB = SNR_2500_dB + 10*log10(2500 / R)`. R below is the rate each mode
signals at, which for the bit modes is the rate their bits are counted at and
for the others is the channel rate, not an information rate after coding or
framing.

| Mode | R | Add to the 2500 Hz figure for Eb/N0 |
| --- | --- | --- |
| RTTY | 45.45 Bd | +17.40 dB |
| AX.25 | 1200 bit/s | +3.19 dB |
| POCSAG 512, 1200, 2400 | 512, 1200, 2400 bit/s | +6.89, +3.19, +0.18 dB |
| SITOR-B, NAVTEX | 100 Bd | +13.98 dB |
| PSK31, QPSK31 | 31.25 Bd | +19.03 dB |
| PSK63 | 62.5 Bd | +16.02 dB |
| M17 | 9600 bit/s gross | -5.84 dB |
| P25 Phase 1 | 9600 bit/s | -5.84 dB |
| D-STAR | 4800 bit/s | -2.83 dB |
| TETRA | 36000 bit/s | -11.58 dB |

Some tests in `tests/decode` state their figures in other bandwidths. To
compare: M17's 9 kHz channel figure is the 2500 Hz one less 5.56 dB; P25's and
D-STAR's full-band figure at 48000 S/s is the 2500 Hz one less 12.83 dB;
TETRA's at 72000 S/s, less 14.59 dB. RDS's 7.19 dB Eb/N0 at 1187.5 bit/s is
3.96 dB in 2500 Hz of the RDS component alone.

## What the curves found

Where a curve and the two or three points its test in `tests/decode` prints
disagree, the curve is the better number: it averages hundreds of
transmissions where the test measured one. Several disagreed by more than
counting error, and each is recorded here rather than smoothed over.

**CW had a floor near 0.01, and it came from the decoder.** The first
baseline sat between 0.0084 and 0.016 from -5 to +3 dB, 0.0028 over 24
transmissions at +10 dB, and was read at 0.05 for that reason. It had three
causes, each fixed and each with a case in `tests/decode/test_cw.cpp`: the
lead-in's noise keyed characters because the first noise estimate included the
front end's start-up and came out about half the real one ("Leave the front
end's start-up out of CW's first noise estimate"); a first word of one
character lost the space after it, because that space was judged alone
("Hold the start of a CW transmission until its spacing shows two clusters");
and a run of T, M and O pulled the unit estimate onto a dash ("Keep the CW unit
until a dash confirms a new one"). The curve now falls to 0.0077 at -6 dB and
stays between 0 and 0.0010 from -5 to +3 dB, and 256 transmissions a point at
+4, +7 and +10 dB gave 0.00013, 0.00013 and 0.00091, so it is read at 0.01
again: -6.2 dB, where the first baseline crossed at -5.2 inside its floor. At
0.05 the crossing moved from -7.7 to -7.9 dB.

What is left at high SNR is the first cause, less often: the first noise
estimate comes from a quarter of a second of noise, about a dozen boxcar
lengths, and when it lands low the lead-in can still key a character. All 18
errors in those 768 transmissions from +4 to +10 dB were at the start of one.
And the curve is worse than the first one below -8 dB, 0.61 against 0.51 at
-11 dB. That is the first fix: at the start of a weak transmission the squelch
used to open on the halved noise estimate, and now it waits for the real one.
Swept from -11 to -8 dB with all three, -11 dB read 0.59; with that one
turned off and the other two kept, 0.51 on the same trials.

**SITOR-B loses whole transmissions.** At 0, +1 and +2 dB, 2, 4 and 2 of 1024
transmissions of 100 characters printed none of their 102 characters, and
every other transmission printed all of them. Why has not been established.
The 0.01 crossing sits where those losses meet the noise, and a different set
of trials over a narrower grid put it at -1.76 dB rather than -1.11.

**QPSK31 either acquires or prints nothing.** At -12 dB, of sixteen single
transmissions six came back with character error rates between 0.05 and 0.13
and ten between 0.98 and 1.0. The test's one run at -12 dB, 0.09, is one of
the six; the curve's 0.78 is the average. Thirty-two transmissions of 600
characters average 0.84 there against 0.85 for 100, so it is not the length.
All or nothing per transmission points at the acquisition rather than the
Viterbi decoder; that has not been confirmed.

**D-STAR's bit error rate varies a lot from transmission to transmission.**
Twelve single transmissions at 14.83 dB in 2500 Hz, which is the test's 2 dB
across 48000 S/s, gave 0.055 to 0.357. The test's 0.056 is the low end; the
curve reads 0.168 at 15 dB. A trial whose bits slip after the alignment is
scored half wrong from there, as RDS's are, which is the likely spread and has
not been confirmed.

**TETRA's last burst is never found.** Before its subject sent one burst past
the payload, every trial lost exactly one of 24, at any SNR, and the curve
floored at 1/48. The test's "0 bit errors in 4968" at 30 dB is 23 bursts, not
the 24 its comment said; `docs/retired-claims.txt` has the correction.

**P25 is not error free at 26 dB.** 4 to 40 bits in 1.5 million from 23 to
26 dB, where D-STAR and TETRA reach zero by 23 and 24. Not examined.

**POCSAG at 1200 bit/s keeps losing a page in a thousand at 10 and 11 dB**, 4
and 3 of 4096, before none from 12 dB. At 512 and 2400 bit/s the fall is
clean. Not examined.

The rest agree with their tests within counting error: RTTY 0.0077 at -5 dB
against the test's 0.0033 over 300 characters, AX.25 0.036 at 10 dB against
0.05 over 60 frames, POCSAG 0.49 at 8 dB against 0.475 over 40 pages, NAVTEX
0.86 lost at -5 dB against eight of ten, PSK31 0.032 at -10 dB against 0.023,
and
M17's crossing at 17.0 dB, which is 11.4 dB in 9 kHz against the test's 0.01
at 12 dB in 9 kHz.

## The nightly

`.github/workflows/nightly.yml` sweeps every mode above against its baseline
with the same command and seed, after the BPSK reference and RDS. Making the
baselines took 664 s for all fifteen on the RTX 4090 workstation's CPU, with
other builds running on it; with RDS's 226 s the job's sweeps come to about 15
minutes, so every mode runs every night. The workflow states the rule for
splitting them if that passes 30 minutes. A regression is a point whose error rate rose by more than half again plus
three errors' worth of counting noise, or a crossing that moved more than
0.2 dB to the right: `tools/bench/curve.h`.

A baseline is replaced by hand, from a curve somebody has read, with the
command in the table and `--out tests/baselines/ber-vs-snr-NAME.json`.
