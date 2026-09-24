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
impulse noise, no transmitter clock error. The PSK tones sit a few hertz off
the receiver's centre, as the tests put them, and the CW tone at 720 Hz, which
the CW decoder searches the audio for; that is the only impairment besides the
noise.

WHAT THE SENTENCE ABOVE USED TO SAY: "The PSK and CW tones sit a few hertz off
the receiver's centre". The CW decoder has no centre since "Decode every keyed
tone in a receiver's audio, wherever it is". A real HF or VHF channel will cost more than
these figures show, by an amount nobody has measured here yet.

## The table

SNR is signal power over noise power in a 2500 Hz reference bandwidth, the
convention [snr-convention.md](snr-convention.md) fixes. For the audio modes
it is the audio's SNR in 2500 Hz of one-sided spectrum; for the complex
baseband modes, the RF signal's. RDS keeps the Eb/N0 axis its subject was
written on. Measured on 2026-09-23 on the RTX 4090 workstation's CPU, commit
"Read a decoder curve at its own mode's error rate in compare". CW was
measured again the same day at "Hold the start of a CW transmission until its
spacing shows two clusters", and again at "Decode every keyed tone in a
receiver's audio, wherever it is", which scores it through the search a client
is now served. SITOR-B, NAVTEX, AX.25, the three POCSAG rates,
PSK31, PSK63, QPSK31 and D-STAR were swept again the same day at commit "Limit
FM discriminator clicks before POCSAG's level discriminator", after the fixes
the section below records, and their baselines replaced; of those, SITOR-B's,
POCSAG's and D-STAR's crossings moved. DMR was added and swept the same day
at commit "Serve DMR bursts over the wire as decoded messages", and AIS and
VHF DSC at commit "Serve AIS and DSC as decoded messages over the wire".

WHAT FIVE ROWS OF THE TABLE USED TO SAY, before those fixes: POCSAG 7.3 dB at
512 bit/s, 9.5 dB at 1200 and 12.0 dB at 2400, SITOR-B -1.1 dB and D-STAR
17.3 dB. And the CW row said -6.2 dB, scored through `decode::Cw` looking for
its tone within 100 Hz of 700; "CW through the engine, at any pitch" below
has why that changed.

| Mode | Unit counted | Error rate | SNR convention | Sensitivity | Command |
| --- | --- | --- | --- | --- | --- |
| RTTY, 45.45 Bd, 170 Hz shift | character | 0.01 | 2500 Hz | -5.2 dB | `bench sweep --mode rtty --seed 20260918` |
| AX.25, 1200 bit/s Bell 202 | frame | 0.01 | 2500 Hz | 10.8 dB | `bench sweep --mode ax25 --seed 20260918` |
| POCSAG, 512 bit/s | page | 0.01 | 2500 Hz | 6.4 dB | `bench sweep --mode pocsag-512 --seed 20260918` |
| POCSAG, 1200 bit/s | page | 0.01 | 2500 Hz | 8.9 dB | `bench sweep --mode pocsag-1200 --seed 20260918` |
| POCSAG, 2400 bit/s | page | 0.01 | 2500 Hz | 11.1 dB | `bench sweep --mode pocsag-2400 --seed 20260918` |
| SITOR-B | character | 0.01 | 2500 Hz | -4.4 dB | `bench sweep --mode sitor-b --seed 20260918` |
| NAVTEX | message | 0.01 | 2500 Hz | -2.6 dB | `bench sweep --mode navtex --seed 20260918` |
| PSK31 | character | 0.01 | 2500 Hz | -8.8 dB | `bench sweep --mode psk31 --seed 20260918` |
| PSK63 | character | 0.01 | 2500 Hz | -5.8 dB | `bench sweep --mode psk63 --seed 20260918` |
| QPSK31 | character | 0.01 | 2500 Hz | -9.5 dB | `bench sweep --mode qpsk31 --seed 20260918` |
| CW, 20 WPM | character | 0.01 | 2500 Hz | -6.6 dB | `bench sweep --mode cw --seed 20260918` |
| M17 stream mode | frame | 0.01 | 2500 Hz | 17.0 dB | `bench sweep --mode m17 --seed 20260918` |
| P25 Phase 1, C4FM | bit | 0.01 | 2500 Hz | 17.9 dB | `bench sweep --mode p25p1 --seed 20260918` |
| D-STAR, GMSK | bit | 0.01 | 2500 Hz | 17.2 dB | `bench sweep --mode dstar --seed 20260918` |
| TETRA, pi/4-DQPSK | bit | 0.01 | 2500 Hz | 20.2 dB | `bench sweep --mode tetra --seed 20260918` |
| DMR, CSBKs on both timeslots | frame | 0.01 | 2500 Hz | 18.8 dB | `bench sweep --mode dmr --seed 20260918` |
| AIS, Message 1 bursts, through an nfm receiver | frame | 0.01 | 2500 Hz | 22.5 dB | `bench sweep --mode ais --seed 20260918` |
| DSC on VHF, individual calls, through an nfm receiver | message | 0.01 | 2500 Hz | 15.7 dB | `bench sweep --mode dsc --seed 20260918` |
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
  stream payloads after one Link Setup Frame; DMR frames are CSBKs, ten
  octets each through the BPTC (196,96) and the CRC-CCITT, one a slot on
  both timeslots of a base station channel with its CACH. AIS frames are
  Message 1 position reports, 168 bits of data each in a one-slot burst
  with the carrier off between bursts; AIS's SNR is the burst's own, its
  power taken over the samples where the carrier is on.
- **page**: a POCSAG page whose identity and 40-character alphanumeric text
  did not both come back.
- **message**: a NAVTEX message of 120 characters not received exactly with a
  clean preamble, which is what M.540-2 Annex II clause 3 lets a receiver
  print. Each is its own transmission with ten seconds of phasing. For DSC, a
  routine individual call on a VHF working channel, 19 information
  characters, not received with every one of them and its error-check
  character right.
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
| DMR | 9600 bit/s gross | -5.84 dB |
| AIS | 9600 bit/s | -5.84 dB |
| DSC on VHF | 1200 bit/s | +3.19 dB |

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
until a dash confirms a new one"). The curve then fell to 0.0077 at -6 dB and
stayed between 0 and 0.0010 from -5 to +3 dB, and 256 transmissions a point at
+4, +7 and +10 dB gave 0.00013, 0.00013 and 0.00091, so it was read at 0.01
again: -6.2 dB, where the first baseline crossed at -5.2 inside its floor. At
0.05 the crossing moved from -7.7 to -7.9 dB. "CW's curve since the search",
below, is the curve now.

What is left at high SNR is the first cause, less often: the first noise
estimate comes from a quarter of a second of noise, about a dozen boxcar
lengths, and when it lands low the lead-in can still key a character. All 18
errors in those 768 transmissions from +4 to +10 dB were at the start of one.
And the curve is worse than the first one below -8 dB, 0.61 against 0.51 at
-11 dB. That is the first fix: at the start of a weak transmission the squelch
used to open on the halved noise estimate, and now it waits for the real one.
Swept from -11 to -8 dB with all three, -11 dB read 0.59; with that one
turned off and the other two kept, 0.51 on the same trials.

**SITOR-B lost whole transmissions to its bit clock.** 1 to 4 transmissions
in 1024 printed nothing at each of 0, 1, 2, 6, 10 and 20 dB. The
transmitter starts its first unit at sample zero, which puts the FSK bit
clock's first reading on a boundary, where Gardner's detector reads nothing
either way; on the transmissions that failed the clock was still there after
most of the 232 bits of phasing, so the decoder phased late or never.
`core/decode/fsk.h` now moves the clock half a bit when its boundary readings
outweigh its centre readings, commit "Move the FSK bit clock off the boundary
it can start on", and none of those 6144 prints nothing. The same fault was
most of the curve between -5 and -1 dB: 0.0137 at -2 dB became 2.9e-5, and
the crossing moved from -1.1 to -4.4 dB.

**QPSK31 failed to acquire on its idle preamble.** At -12 dB, 11 of the
sweep's first 16 transmissions printed almost nothing. The squared line of
the 64 symbol idle turned down most of their first windows, and QPSK data
carries no line the square or the fourth power finds at that level, so
acquisition waited for the postamble. `core/decode/psk31.cpp` now reads the
idle's two tones directly when the squared line falls short, commit "Acquire
PSK31 on its idle's two tones when the squared line falls short", and none
of 256 transmissions at -12 dB reads worse than 0.5. The curve went from 0.78
to 0.089 at -12 dB and from 0.996 to 0.35 at -14; from -10 dB up it did not
move, so neither did the crossing. BPSK31 and PSK63 improve at their bottom
two or three points and read about a tenth worse where their curves fall
through 0.1 to 0.2: the text bits come back with fewer errors, and the extra
characters are printed from the idle, which acquisition now reads from its
first window.

**D-STAR's transmissions slipped a bit.** Twelve single transmissions at
14.83 dB in 2500 Hz gave 0.055 to 0.357; every one read about 0.055 until the
bit timing slipped, after which it read half wrong. Not the frame sync, the
offset refit or the deviation: below the FM threshold the discriminator
clicks, and the square-law timing estimator weights a click by its energy.
`core/decode/dstar.cpp` now limits the shaped output at 2.5 times the
deviation, commit "Limit D-STAR's discriminator output before the bit
timing": 56 of the first 128 trials at 15 dB slipped before and 14 after, the
curve reads 0.082 there against 0.168, and the crossing moved from 17.3 to
17.2 dB. From 18 to 22 dB it reads 1 to 6 percent more bits wrong.

WHAT THE THREE PARAGRAPHS ABOVE USED TO SAY: "SITOR-B loses whole
transmissions. At 0, +1 and +2 dB, 2, 4 and 2 of 1024 transmissions of 100
characters printed none of their 102 characters ... Why has not been
established"; "QPSK31 either acquires or prints nothing ... All or nothing per
transmission points at the acquisition rather than the Viterbi decoder; that
has not been confirmed"; and "D-STAR's bit error rate varies a lot from
transmission to transmission ... A trial whose bits slip after the alignment
is scored half wrong from there, as RDS's are, which is the likely spread and
has not been confirmed".

**TETRA's last burst is never found.** Before its subject sent one burst past
the payload, every trial lost exactly one of 24, at any SNR, and the curve
floored at 1/48. The test's "0 bit errors in 4968" at 30 dB is 23 bursts, not
the 24 its comment said; `docs/retired-claims.txt` has the correction.

**P25 is not error free at 26 dB.** 4 to 40 bits in 1.5 million from 23 to
26 dB, where D-STAR and TETRA reach zero by 23 and 24. Not examined.

**POCSAG lost pages to the discriminator's clicks.** At 1200 bit/s 4 and 3
pages of 4096 were lost at 10 and 11 dB. Of those 7, 5 had a message code
word with three or more bits wrong and 2 an address with two, which the
address correction budget of one refuses; the errors came in bursts, and the
bursts were clicks. The budget and the rule holding unconfirmed batches were
not the cause, and no rule the clauses back takes the two addresses without
taking back what the budget refuses. `core/decode/fsk.h`'s level
discriminator now limits its input, commit "Limit FM discriminator clicks
before POCSAG's level discriminator", and nothing is lost from 10 dB at 1200
bit/s. The crossings moved by 0.6 to 0.9 dB at all three rates. False pages
stay where the budget put them: none in 40 hours of noise, and at 1200 bit/s
1, 5 and 1 in 240 pages sent at 6, 5 and 4 dB.

WHAT THE PARAGRAPH ABOVE USED TO SAY: "POCSAG at 1200 bit/s keeps losing a
page in a thousand at 10 and 11 dB ... Not examined." And the one below
compared "POCSAG 0.49 at 8 dB against 0.475 over 40 pages".

**AIS and DSC sit at the FM discriminator's threshold.** Both are read the
way the engine hands them over, through an nfm receiver's 16 kHz channel and
its discriminator, and both curves fall from all lost to none in about five
dB. The carrier to noise ratio in that channel is the 2500 Hz figure less
8.06 dB: 11.9 dB for AIS at 20 dB and 5.9 dB for DSC at 14 dB. AIS's curve
reads 0.61 at 18 dB, 0.18 at 20 and 0.0049 at 23; DSC's, whose tones are
phase modulated at index 2.0, 0.87 at 13 dB, 0.39 at 14 and 0.0049 at 16,
with 1023 of 1024 calls lost at 12 dB. Their tests agree where they send the
same thing: DSC's 27 of 70 calls lost at 14 dB against the curve's 0.39.
AIS's test lost 0.311 at 20 dB over a mix of message types up to two slots
long, against the curve's 0.18 over one-slot Message 1s; nothing longer
than one slot has been swept.

The rest agree with their tests within counting error: RTTY 0.0077 at -5 dB
against the test's 0.0033 over 300 characters, AX.25 0.036 at 10 dB against
0.05 over 60 frames, POCSAG 0.10 at 8 dB against 0.10 over 40 pages, NAVTEX
0.86 lost at -5 dB against eight of ten, PSK31 0.032 at -10 dB against 0.023,
and
M17's crossing at 17.0 dB, which is 11.4 dB in 9 kHz against the test's 0.01
at 12 dB in 9 kHz.

**CW's curve since the search.** `bench sweep --mode cw` now scores what a
client is served, `decode::CwBand`, which has to find the tone before it
decodes it. It crosses 0.01 at -6.6 dB against -6.2 before, and reads 0.28 at
-10 dB, 0.040 at -8 and 0.013 at -7, against 0.28, 0.055 and 0.024. From -5 to
+3 dB it lost 50 characters in 2304 transmissions, against 44; 28 of them are
one transmission at 0 dB whose first six characters, "U860P", printed as "KT
TTTI 6 0 P", and `bench compare` against the old baseline calls that point
worse. The start of a transmission is still where the errors are.

## CW through the engine, at any pitch

The sweep above hands the decoder audio. What an operator hears goes through
a receiver first, and on the air the tone lands wherever the tuning put it.
`bench cw-engine` measures that: a keyed carrier in a 96 kS/s file at 14 MHz,
read by receivers tuned so the tone lands at 300 to 1200 Hz, and the `cw`
adapter of `core/rpc/decoders.h` on each receiver's audio, exactly as a
client's decode pane is fed. Four receivers: `cw` at its then default
+/-250 Hz filter and 700 Hz pitch (widened to +/-600 Hz the same day, on
these figures, so a default `cw` receiver now reads what `cw-wide` does), tuned off the carrier by the pitch less 700;
`cw-wide`, the same with its filter pulled out to -500..+600 Hz; and `usb`
and `lsb` at their default 300 to 2700 Hz, tuned the pitch below or above the
carrier. Four transmissions of 40 random letters and figures a cell, at 12,
20, 30 and 40 WPM, noise in 2500 Hz at the radio frequency over the whole
file. Each table below pools the four speeds; the JSON has every cell.

    bench cw-engine --pitch 300,500,600,700,800,900,1200 --out cw-engine.json

Before is commit "Measure CW through the engine at any pitch", the decoder
that looked for its tone within 100 Hz of 700; after is "Decode every keyed
tone in a receiver's audio, wherever it is". Character error rate, before /
after, measured on 2026-09-23 on the RTX 4090 workstation, 80 files and 28
receivers in 180 s before and 83 s after.

`cw`, before / after:

| tone | 0 dB | 5 dB | 10 dB | 15 dB | 20 dB |
| --- | --- | --- | --- | --- | --- |
| 300 Hz | 1.000 / 1.000 | 1.000 / 1.000 | 1.000 / 1.000 | 1.000 / 1.000 | 1.000 / 1.000 |
| 500 Hz | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 0.940 / 0 | 0.895 / 0 |
| 600 Hz | 0 / 0 | 0 / 0 | 0 / 0 | 0.008 / 0 | 0.005 / 0 |
| 700 Hz | 0 / 0 | 0 / 0 | 0 / 0 | 0.008 / 0 | 0.005 / 0 |
| 800 Hz | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0.005 / 0 |
| 900 Hz | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 0.915 / 0 | 0.904 / 0 |
| 1200 Hz | 1.000 / 1.000 | 1.000 / 1.000 | 1.000 / 1.000 | 1.000 / 1.000 | 1.000 / 1.000 |

`cw-wide`, before / after:

| tone | 0 dB | 5 dB | 10 dB | 15 dB | 20 dB |
| --- | --- | --- | --- | --- | --- |
| 300 Hz | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 |
| 500 Hz | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 0.904 / 0 | 0.901 / 0 |
| 600 Hz | 0 / 0 | 0.003 / 0 | 0 / 0 | 0.008 / 0 | 0 / 0 |
| 700 Hz | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| 800 Hz | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| 900 Hz | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 0.868 / 0 | 0.906 / 0 |
| 1200 Hz | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 |

`usb`, before / after:

| tone | 0 dB | 5 dB | 10 dB | 15 dB | 20 dB |
| --- | --- | --- | --- | --- | --- |
| 300 Hz | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 |
| 500 Hz | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 0.939 / 0 | 0.915 / 0 |
| 600 Hz | 0 / 0 | 0 / 0 | 0 / 0 | 0.008 / 0 | 0.005 / 0 |
| 700 Hz | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| 800 Hz | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| 900 Hz | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 0.910 / 0 | 0.909 / 0 |
| 1200 Hz | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 |

`lsb`, before / after:

| tone | 0 dB | 5 dB | 10 dB | 15 dB | 20 dB |
| --- | --- | --- | --- | --- | --- |
| 300 Hz | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 |
| 500 Hz | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 0.920 / 0 | 0.918 / 0 |
| 600 Hz | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| 700 Hz | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| 800 Hz | 0 / 0 | 0 / 0 | 0 / 0 | 0.008 / 0 | 0.005 / 0 |
| 900 Hz | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 0.936 / 0 | 0.911 / 0 |
| 1200 Hz | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 | 1.000 / 0 |

What the before tables settle, suspect by suspect:

- **The decoder looked only near 700 Hz.** Confirmed: inside 600 to 800 Hz
  every receiver read at most 0.031 of a cell wrong, and outside it at least
  0.606, all of it below 15 dB.
- **CW mode's narrow filter against its pitch translation.** Ruled out as a
  cause of errors: at 600, 700 and 800 Hz the `cw` receiver read the same as
  `usb` and `lsb`. What the `cw` receiver's default filter does is pass only
  450 to 950 Hz of tone, so a station more than 250 Hz off zero beat is not in
  its audio at all, which is the 300 and 1200 Hz rows after. The `cw-wide`
  rows show the decoder reads them once the filter passes them.
- **A threshold that did not follow the level.** Not seen from 0 to 20 dB, nor
  on the recording below, whose audio was 1.2e-3 RMS on the -10950 Hz station.
- **Timing that did not lock on machine keying.** It locked on every speed
  here. On the air it lost the word spaces, which is the recording below.

Characters printed on a pitch other than the one the tone landed on, after:
38 in the `cw` rows, 29 in `cw-wide`, 25 in `lsb` and none in `usb`, over 80
files each, all at 20 dB or at 0 dB and 800 Hz. None before, because nothing
looked there.

**A hand sender, speech and two stations**, from `tests/decode/test_cw_band.cpp`
on 2026-09-23. At 18 WPM and +10 dB, with every element and space stretched at
random by a standard deviation of 5, 10, 15 and 20 per cent of its length, the
error rate was 0, 0.018, 0.054 and 0.161 over four transmissions of 56
characters: a hand sender degrades into wrong characters, not into streams
that print for ever. A minute of speech on a usb receiver at +20 dB printed no
characters; the search's first version printed 747. Two stations keying at
once 160 Hz apart, and a conversation of two stations 170 Hz apart taking
turns, each read every character at its own pitch.

**The cost.** The 62-minute recording below with eleven `cw` decoders ran at
11.7 times realtime on the RTX 4090 workstation, against 34.1 before. Each
decoder runs the search and up to six streams, each stream a low-pass at the
audio rate and a boxcar at 500 S/s.

### On the air: KF4FIC, 20 m, 1603 UT

There is no text to score against, so the text is shown.
`docs/recordings.md` has the file and why `center=` is an assumption; the
offsets are measured. Eleven receivers over the whole 62 minutes, placed on
the persistent narrow tracks `revenant-cli --detect` listed, with the
command both before and after:

    revenant-cli "file:///C:/Users/vexam/projects/SDR Recordings/KF4FIC_wideband_14000_14350kHz_20170821_1603UT.wav?center=14175000"
      --vrx +2500:cw --vrx +2650:cw --vrx +6950:cw --vrx +5500:cw --vrx -10950:cw
      --vrx +4500:cw --vrx +1000:usb --vrx +4000:usb --vrx +6400:usb
      --vrx -11600:usb --vrx +3300:lsb --decode cw

The +2650 cw receiver sits 130 Hz off the station at +2519.6, the tone at
570 Hz; the others sit on a station's carrier or, for usb and lsb, a kilohertz
or so from several. Counts over the hour, before / after, of the characters
printed, of codes the table does not hold, and of five words standing as
words, with a space or a line's end either side:

| receiver | characters | U+FFFD | "CQ" | "TEST" | "DE" | "5NN" | "TU" |
| --- | --- | --- | --- | --- | --- | --- | --- |
| +2500 cw | 2089 / 3297 | 32 / 52 | 1 / 18 | 0 / 10 | 0 / 14 | 0 / 7 | 2 / 55 |
| +2650 cw | 2929 / 3002 | 340 / 48 | 0 / 17 | 0 / 11 | 3 / 13 | 0 / 5 | 0 / 54 |
| +6950 cw | 2911 / 4949 | 38 / 88 | 2 / 12 | 0 / 0 | 1 / 6 | 2 / 7 | 3 / 28 |
| +5500 cw | 3395 / 4266 | 30 / 36 | 0 / 72 | 2 / 39 | 0 / 0 | 1 / 2 | 0 / 1 |
| -10950 cw | 4735 / 5207 | 33 / 19 | 0 / 0 | 0 / 131 | 0 / 137 | 0 / 0 | 0 / 0 |
| +4500 cw | 3070 / 3240 | 71 / 58 | 0 / 5 | 0 / 2 | 0 / 4 | 0 / 3 | 1 / 40 |
| +1000 usb | 3789 / 14029 | 64 / 200 | 1 / 187 | 0 / 126 | 0 / 66 | 0 / 84 | 0 / 156 |
| +4000 usb | 1095 / 12562 | 6 / 178 | 0 / 99 | 0 / 85 | 0 / 12 | 0 / 14 | 0 / 59 |
| +6400 usb | 3153 / 14783 | 85 / 255 | 0 / 95 | 0 / 40 | 0 / 47 | 0 / 52 | 0 / 85 |
| -11600 usb | 4689 / 10008 | 33 / 100 | 0 / 9 | 0 / 136 | 0 / 142 | 0 / 12 | 0 / 15 |
| +3300 lsb | 2111 / 4212 | 33 / 60 | 1 / 31 | 0 / 11 | 0 / 15 | 0 / 10 | 2 / 55 |

Before, the words were there and the spaces were not: the -10950 cw receiver
printed "TEST" 129 times and never as a word, "TESTDEW7EW7E ECLIPSE CN88SE".
The station keys at 20 WPM with letter spaces of 175 to 188 ms, word spaces of
360 to 369 ms and pauses of 3.5 and 4.5 s between calls, measured off the
receiver's recorded audio. With nine letter spaces, two word spaces and one
3.5 s pause among the twelve long spaces the timing decoder clusters, its
arithmetic puts the cut between letter and word spaces at 1.85 s, midway
between the pause and the rest, and every word space falls under it. After,
the same lines:

    before   27.97s  vrx 5  TESTDEW7EW7E ECLIPSE CN88SE
    after    27.97s  vrx 5  TEST DE W7E W7E ECLIPSE CN88SE

The receiver 130 Hz off zero beat, before and after, the same stretch:

    before   19.09s  vrx 2  TOKE�BEII�HI
             32.06s  vrx 2  WQ��TE5TOTKN2ZDI O K�S�Z J
    after    19.09s  vrx 2  TU K9BGL SE
             32.75s  vrx 2  RQ CQ TEST DE K9BM I K9BG K
             85.31s  vrx 2  CQ CQ TEST DE KN GL KE GBGL K

And one usb receiver hearing several stations at once, from
`bench cw-file` over the first four minutes, which prints the stream and
pitch revenant-cli does not:

     14.33s  vrx  1  stream   1   1519.6 Hz  23.6 WPM  8CM
     19.11s  vrx  1  stream   5   1519.8 Hz  24.9 WPM  TU K9BGL SE
     32.76s  vrx  1  stream   5   1519.8 Hz  24.3 WPM  RQ CQ TEST DE K9BM I K9BG K
     33.44s  vrx  1  stream   2    732.3 Hz  23.1 WPM  KA4WJ 5NN DM12F� UA4WJ
     45.73s  vrx  1  stream   3    266.0 Hz  29.7 WPM  CQ TEST A T3B
     83.28s  vrx  1  stream  13    732.0 Hz  23.4 WPM  EST D E W6RDF W6 NDF K
     98.30s  vrx  1  stream   3    266.4 Hz  30.6 WPM  NOT B 5NN FN20EI
    103.76s  vrx  1  stream  14   1519.6 Hz  25.5 WPM  K6BHH 5 9 EM58CM

    bench cw-file "file:///...1603UT.wav?center=14175000" 1000:usb 2650:cw 6400:usb --seconds 240

This is a contest: calls, "TEST", "5NN" and a grid square. The printed text
is not clean. "K9BGL" comes out as "K9BM I K9BG" and "KN GL KE GBGL". The
usb receivers printed 10008 to 14783 characters in the hour through a
2.4 kHz filter, the cw receivers 3002 to 5207 through 500 Hz; how many of the
extra are stations and how many are noise the table does not say.

**Not done.** Two tones within about 50 Hz are one stream. A station much
stronger than one within about 90 Hz of it keys that one's stream. Four or
more stations keying at once in one receiver's audio read to the search as
speech and are not started. A transmission's first characters are still where
the synthetic errors are, and the one bad trial at 0 dB is one of them.

## The nightly

`.github/workflows/nightly.yml` sweeps every mode above against its baseline
with the same command and seed, after the BPSK reference and RDS. Making the
baselines took 664 s for all fifteen on the RTX 4090 workstation's CPU, with
other builds running on it; with RDS's 226 s the job's sweeps come to about 15
minutes, so every mode runs every night. AIS and DSC, added later, take 0.7 s
and 4.2 s. The workflow states the rule for
splitting them if that passes 30 minutes. A regression is a point whose error rate rose by more than half again plus
three errors' worth of counting noise, or a crossing that moved more than
0.2 dB to the right: `tools/bench/curve.h`.

A baseline is replaced by hand, from a curve somebody has read, with the
command in the table and `--out tests/baselines/ber-vs-snr-NAME.json`.
