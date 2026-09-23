# Real IQ on this machine

What exists, measured rather than described, and what it is not yet good for.

Nothing in this file is in the repository. Captured IQ is large and git is the
wrong store for it, which is the argument `tests/corpus/README.md` makes at
length. This is a pointer.

## Where

    C:\Users\vexam\projects\SDR Recordings

Six files, 2,146,437,388 bytes each, 12.9 GB in total.

    KF4FIC_wideband_7000_7300kHz_20170821_1359UT.wav
    KF4FIC_wideband_7000_7300kHz_20170821_1501UT.wav
    KF4FIC_wideband_7000_7300kHz_20170821_1603UT.wav
    KF4FIC_wideband_14000_14350kHz_20170821_1359UT.wav
    KF4FIC_wideband_14000_14350kHz_20170821_1501UT.wav
    KF4FIC_wideband_14000_14350kHz_20170821_1603UT.wav

## What they are

Read out of the header of the 7 MHz 1359UT file on 2026-09-22:

    container     RIFF/WAVE
    fmt chunk     40 bytes, WAVE_FORMAT_EXTENSIBLE (0xFFFE)
    subformat     PCM (GUID begins 01 00 00 00)
    channels      2, which is I and Q rather than left and right
    sample rate   96000
    bits          24
    block align   6 bytes per frame
    data          2,146,437,120 bytes, 357,739,520 frames, 3726.5 seconds

Sixty-two minutes each, and the size is not a coincidence: 2,146,437,388 bytes
is a few hundred short of the 2 GiB a RIFF size field can address, so these
were cut by the container rather than by the clock.

**The centre frequency is not in the file.** There is no `auxi` chunk and no
`LIST`: the fmt chunk is followed directly by data. The band is in the
filename and nothing machine-readable carries it, so anything reading these has
to be told where they sit.

**The filename names a band wider than the file covers.** 7000 to 7300 kHz is
300 kHz and 96 kS/s carries 96 kHz. The name is the band that was being
listened to, not the span that was captured.

## Where they came from

Zenodo record 10.5281/zenodo.846442, "KF4FIC wideband 20m and 40m data for 2017
Eclipse HF Wideband Recording Experiment", CC BY 4.0. Jonathan Fields, KF4FIC,
a Flex-6300 receiver on a Butternut HF9V vertical, at 40.548979 N,
112.272100 W, which is the Salt Lake valley in Utah. The record lists eight
recordings per band, at 1359, 1501, 1603, 1705, 1807, 1909, 2011 and 2114 UT.
The first three of each band are the six on this machine. It does not state a
tuning.

## What day this is

2017-08-21 is the total solar eclipse across the United States. The pair of
bands is the point: HF propagation depends on ionisation the sun drives, so the
same two bands through the morning of an eclipse is a controlled experiment
somebody else already ran.

**The six files here run before the eclipse, not across it.** Each is 3726.5
seconds, so the three start times are one continuous recording per band from
13:59 to 17:05 UT, which is 07:59 to 11:05 local time at the receiver (MDT,
UT minus six). The eclipse's first contact anywhere on Earth was 15:46:48 UT
and first umbral contact 16:48:32 UT; greatest eclipse was 18:26:40 UT. At
Salt Lake City the partial phase began at 16:13 UT and peaked at magnitude
0.925 at 17:33 UT (timeanddate.com, to the minute; the receiver is about
30 km west of the city). So 1359 and 1501 are entirely before the eclipse, and
1603 starts before it reached Utah and has its last 52 minutes inside the
partial phase, ending 28 minutes before the maximum. The five later recordings
in the Zenodo record are the ones that cross it, and they are not on this
machine.

WHAT THIS SECTION USED TO SAY: "2017-08-21 is the total solar eclipse across
the United States, and the three times bracket it: 1359, 1501 and 1603 UT", and
"the same two bands an hour apart across an eclipse". That was read off the
filenames before the dataset's own record was looked at.

That matters for what these are useful for. They are not a quiet reference
recording of a band. They are three hours of two bands through a morning,
which moves them, and that is harder, and better, for anything being asked
whether it holds up when conditions move.

## What they unblock

`docs/detection.md` leaves `DetectorConfig::split_gap_bins` at eight bins and
says why the number cannot be settled: eight bins is 293 Hz at the shipped
36.6 Hz VHF grid, which on an HF grid at 7.6 Hz is a gap RTTY's own two tones
clear, so every RTTY signal splits into two detections. That paragraph ends
"measuring a new rule needs real HF with ground truth, and a recording carries
none."

Half of that is now false and half is still true. Real HF exists. Ground truth
does not: nothing in these files says which signals are in them, and the
population changes under you over the three hours, so a
ground-truth record would have to be built and dated rather than stated once.

The honest position is that these make the measurement possible and do not make
it cheap.

## How the engine reads them

Natively, as delivered. `core/source/file_source.cpp` resolves a 24-bit PCM fmt
chunk to `cs24`, six bytes a sample, and `core/shaders/convert_cs24_cf32.comp`
widens it to Complex32 on the GPU as the upload's convert stage, beside the cu8,
cs8 and cs16 kernels. No host pass and no copy on disk.

The file carries no centre frequency, so the URI has to:

    revenant-cli "file:///C:/Users/vexam/projects/SDR Recordings/KF4FIC_wideband_14000_14350kHz_20170821_1603UT.wav?center=14175000" --detect

Without `center=` the stream sits at 0 Hz and `source::resolution_for_span`
returns an unstated request. `--channels 0` then takes the engine's default of
2 channels at 96 kS/s, 46.875 Hz per bin; with `center=14175000` the same run
is widened to 16 channels and 5.859 Hz, and says so. `revenant-cli` pins 64
channels unless told otherwise, which at 96 kS/s is 1.465 Hz per bin whatever
the centre, and every run below uses that.

**`center=` is an assumption here, not a measurement.** Neither the file nor the
dataset's Zenodo record (10.5281/zenodo.846442) states where the receiver was
tuned, and the filename names a band three times wider than the capture. The
runs below pass the midpoint of the filename's band, 7150000 and 14175000, so
that the absolute frequencies they print are consistent with each other. Read
them as offsets from centre: the offsets are measured, the centre is not.

**The kernel's output is the float excerpts' samples, bit for bit.** Six bytes do
not divide a 32-bit lane, so the kernel reads each sample from two words; the
scale is 2^-23 and a 24-bit code fits a float's significand, so nothing rounds.
`scripts/wav24_to_float32.py` divides by 2^23 in double and rounds a value that
needs no rounding, so the two paths hold the same floats. Checked on
2026-09-22 by cutting the same 5,760,000 frames the excerpts hold (from frame
57,600,000) out of the originals as bytes, unconverted, and running both through
`revenant-cli --detect` with default thresholds on the RTX 4090:

| window | cf32 excerpt | cs24, native | snapshots at a common source time |
| --- | --- | --- | --- |
| 40 m 1359, 600 to 660 s | 31 born, 28 dropped, 15 merges, 1 split | the same | 7 of 7 identical |
| 20 m 1603, 600 to 660 s | 55 born, 45 dropped, 39 merges, 12 splits | the same | 4 of 4 identical |

Both ran 86 decisions over 87 frames. The track tables are printed on a
wall-clock interval, so the two runs sample different source times after the
first few; every table taken at the same source second is identical line for
line. Converting the cut bytes separately in numpy and comparing against the
excerpt's floats matched all 11,520,000 values in both windows bit for bit.

WHAT THIS SECTION USED TO SAY, under the heading "What stops them being used
today": "The engine refuses them, and the refusal was written before they
arrived", quoting the refusal "24-bit, which some HF recorders write, needs a
conversion pass this engine deliberately does not do on the host", and ending
"So the files as delivered are unreadable by this engine and an excerpt of one
is not". It listed three ways out: convert offline, which the section below
took for excerpts; read 24-bit on the host, which the upload kernels exist to
avoid; and "a third upload kernel beside cu8, cs16 and cf32". The third is
the one taken.

## Making one readable, and the first real HF run

`scripts/wav24_to_float32.py` converts 24-bit PCM to 32-bit float and takes an
excerpt while it is there. It is offline, is not part of the engine and is on
no sample path, which is the only reason a host conversion pass is acceptable
at all. A minute is 46 MB and takes four seconds.

    python scripts/wav24_to_float32.py \
      "C:/Users/vexam/projects/SDR Recordings/KF4FIC_wideband_7000_7300kHz_20170821_1359UT.wav" \
      "C:/Users/vexam/projects/SDR Recordings/excerpts/kf4fic_7mhz_1359ut_60s.wav" \
      --seconds 60 --start 600

The engine opens that: `cf32`, 96000 S/s, 5760000 samples, seekable. The grid
it builds is the interesting part.

    grid      M 64  D 32  17 taps per branch
      channel rate    3000 S/s
      channel spacing 1.500 kHz

64 channels of a 96 kS/s stream with a 2048-point second stage is 65536 bins at
**1.465 Hz**, against 36.6 Hz on the shipped VHF geometry. Nothing was
reconfigured to get that: the default grid divided into a slow band gives HF
resolution for free.

**What it found**, 55 seconds of 40 m at 1359 UT, default thresholds:

    #id  state            centre    bandwidth         snr   conf  margin    age
    #13   live             193 Hz        10 Hz     12.2 dB   1.00    0.82   42.3s
    #69   live          7.186 kHz         3 Hz      9.7 dB   0.73    0.73    1.4s
    #7    live          7.192 kHz        14 Hz     19.3 dB   0.93    0.95   49.8s
    #60   live         11.132 kHz         9 Hz     14.7 dB   0.98    0.88    5.5s
    #66   live         13.250 kHz        64 Hz     24.8 dB   0.82    0.98    2.0s

    31 tracks born, 26 dropped, 15 merges, 1 split
    7.9 ms per frame across 65536 bins, 1.1 percent of one core

Three things to take from it.

**The bandwidths are real and they are tiny.** Three, nine, ten and fourteen
hertz. At 36.6 Hz per bin not one of those is a bin wide and the detector could
report nothing about any of them; at 1.465 Hz they are two to ten bins and the
widths are measurements. That is the argument for a per-band grid, demonstrated
rather than asserted.

**The margin column does the work the stopwatch cannot.** It runs 0.73 to 0.98
and orders the list by how far each signal stood up, while confidence runs 0.58
to 1.00 and is mostly about age: #13 is the weakest signal in the list at
12.2 dB and carries the highest confidence in it, because it had been there for
42 seconds.

**WHAT THIS SECTION FIRST CLAIMED ABOUT `split_gap_bins`, AND THE SWEEP THAT
REFUTED IT.** It read: "#69 at 7.186 kHz and #7 at 7.192 kHz are six hertz
apart, which at this grid is four bins, under the eight-bin gap, so #69 was
merged into #7 ... this is the case it needs, and it took one excerpt to find."

That was a plausible reading of one observation and it is wrong. The constant
was exposed on `revenant-cli` so it could be swept, and over the same
fifty-five seconds it does almost nothing:

    gap  2 bins (  2.9 Hz): 31 born, 26 dropped, 15 merges, 1 split
    gap  4 bins (  5.9 Hz): 31 born, 26 dropped, 15 merges, 1 split
    gap  8 bins ( 11.7 Hz): 31 born, 26 dropped, 15 merges, 1 split
    gap 16 bins ( 23.4 Hz): 31 born, 26 dropped, 15 merges, 1 split
    gap 32 bins ( 46.9 Hz): 31 born, 26 dropped, 15 merges, 1 split
    gap 64 bins ( 93.8 Hz): 31 born, 26 dropped, 17 merges, 1 split

Identical from two bins to thirty-two. Setting the gap to 2.9 Hz, well under
the six hertz between those two signals, does not separate them.

**Two mistakes, and they are different ones.** A merge is not a split.
`stats_.merges` counts several TRACKS gated to one candidate, which
`core/detect/detector.cpp` decides by frequency overlap against
`association_overlap`, default 0.3. `split_gap_bins` decides whether one
candidate band is cut into several, which happens earlier and elsewhere. The
observed merge was the tracker associating, and the constant it answers to is
the other one.

And no value of the gap could have separated that pair anyway. They are six
hertz apart with bandwidths of three and fourteen hertz, so they overlap: there
is no run of floor between them to be eight bins wide or two. They are one blob
in the spectrum and the detector is right that they are.

**So this recording does not constrain `split_gap_bins`, and
`docs/detection.md`'s entry stays open.** What would constrain it is a signal
with interior nulls wide enough to matter, which is the RTTY case that document
predicts: two tones 170 Hz apart is 116 bins at this grid, far over any gap in
the sweep, so RTTY here should split into two detections and be visibly wrong.
Nothing in this minute obviously is RTTY. Finding one is the next measurement.

What is missing underneath all of it is what that document also says: nothing
here knows which of those five detections are stations and which are artefacts,
so this is an observation and not a score.

## Six excerpts, two bands, two hours

The six files are the same two bands at 1359, 1501 and 1603 UT, so they are a
controlled experiment about the ionosphere. Sixty seconds from ten minutes into
each, converted the same way, run through the detector with default thresholds:

| band | 1359 UT | 1501 UT | 1603 UT |
| --- | --- | --- | --- |
| 40 m, 7 MHz | 31 tracks born | 3 | **0** |
| 20 m, 14 MHz | 10 tracks born | 23 | **51** |

**The two bands move in opposite directions over the same two hours**, and the
detector was told nothing that could produce that.

**It is not a level change, which was the first thing to rule out.** The
spectrum's own fifth and ninety-ninth percentiles barely move: 40 m runs
-102.8 to -74.7 dBFS at 1359 and -103.4 to -75.4 at 1603, and 20 m runs -105.8
to -81.4 and then -106.0 to -81.8. Total band power is the same at both ends.
What changed is whether there is structure standing above it.

**The reading is the ordinary daytime pattern, and the eclipse is ruled out by
the clock.** The three excerpts sit at 14:09, 15:11 and 16:13 UT, which is
08:09, 09:11 and 10:13 at the receiver, a morning with the sun climbing. Rising
solar elevation does two opposite things: it thickens the D layer, which
absorbs the lower HF bands, and it raises the F layer's maximum usable
frequency, which is what carries the higher ones. 7 MHz going quiet while
14 MHz fills up is what that looks like. The first two excerpts precede the
eclipse's first contact anywhere on Earth, 15:46:48 UT. The third is the minute
the partial phase began at the receiver, 16:13 UT, with the sun not yet
measurably covered.

WHAT THIS PARAGRAPH USED TO SAY: "These three times run from mid-morning toward
midday over North America", and after it "three samples on one day cannot
separate that from the eclipse. A real eclipse result needs a control day at
the same times". Both were written before the dataset's own record placed the
receiver in Utah; see "What day this is" above. What stands is the narrower
claim: the detector's track population tracks the ionosphere in the direction
physics says it should, on data nobody here produced, which is evidence it is
measuring the band rather than itself.

### What a busy band looks like to it

20 m at 1603 UT, the fullest of the six:

    #id  state            centre    bandwidth         snr   conf  margin    age
    #1    live        -35.442 kHz   11.726 kHz      6.8 dB   1.00    0.56   38.2s
    #2    live        -10.947 kHz        20 Hz     13.7 dB   0.91    0.86   38.2s
    #54   held          2.504 kHz        19 Hz     10.4 dB   0.62    0.76   15.7s
    #74   live          2.520 kHz        11 Hz     15.9 dB   0.99    0.90    6.1s
    #75   held          2.531 kHz         9 Hz     10.9 dB   0.61    0.78    6.1s
    #47   live          4.495 kHz        30 Hz     16.5 dB   1.00    0.91   20.5s
    #69   live         12.143 kHz    2.033 kHz     15.5 dB   1.00    0.90    8.9s

Two things in that list are worth naming.

**Three signals inside twenty-seven hertz**, at 2.504, 2.520 and 2.531 kHz,
nine to nineteen hertz wide each, two of them holding and one live. That is a
CW pileup and the detector is resolving it into its parts, which at the VHF
grid's 36.6 Hz bins would have been one detection or none.

**And #69 is the centre-wander case `docs/ui-spectrum.md` describes.** It is two
to three and a half kilohertz wide and its measured centre moved 370 Hz between
consecutive decisions, which is what a voice signal does to a centre computed
from where the energy is. That document says a click landing on the measured
centre wobbles with the content; here is the wobble, measured, on a real
station.

## What they are not

Not a corpus. `tests/corpus/README.md` describes what a corpus entry is: a
SigMF sidecar, a checksum, a retrieval URL and a ground-truth record saying
what a correct decoder should recover. These have a filename and nothing else,
and nothing in `tools/bench` knows about them.

Not a replay of anything this project has already done. The RDS decode in
`docs/rds-first-decode.md` was broadcast FM at 98.1 MHz and its samples are
gone; these are HF and cannot stand in for it.
