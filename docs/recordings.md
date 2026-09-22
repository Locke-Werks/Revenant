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

## What day this is

2017-08-21 is the total solar eclipse across the United States, and the three
times bracket it: 1359, 1501 and 1603 UT. The pair of bands is the point. HF
propagation depends on ionisation the sun drives, so the same two bands an hour
apart across an eclipse is a controlled experiment somebody else already ran.

That matters for what these are useful for. They are not a quiet reference
recording of a band. They are three hours of a band changing, which is harder,
and better, for anything being asked whether it holds up when conditions move.

## What they unblock

`docs/detection.md` leaves `DetectorConfig::split_gap_bins` at eight bins and
says why the number cannot be settled: eight bins is 293 Hz at the shipped
36.6 Hz VHF grid, which on an HF grid at 7.6 Hz is a gap RTTY's own two tones
clear, so every RTTY signal splits into two detections. That paragraph ends
"measuring a new rule needs real HF with ground truth, and a recording carries
none."

Half of that is now false and half is still true. Real HF exists. Ground truth
does not: nothing in these files says which signals are in them, and the
eclipse makes the population change under you over the three hours, so a
ground-truth record would have to be built and dated rather than stated once.

The honest position is that these make the measurement possible and do not make
it cheap.

## What stops them being used today

The engine refuses them, and the refusal was written before they arrived.
Measured on 2026-09-22 with `revenant-cli`:

    'KF4FIC_wideband_7000_7300kHz_20170821_1359UT.wav' declares format tag 1
    at 24 bits per sample, which is not one this backend reads. It takes 8-bit
    and 16-bit PCM and 32-bit IEEE float, which are the widths the upload
    kernels convert. 24-bit, which some HF recorders write, needs a conversion
    pass this engine deliberately does not do on the host.

`core/source/file_source.cpp` parses the extensible header correctly, resolves
the subformat to PCM and then declines the width. The refusal names this exact
case, so this is a known gap rather than a surprise.

Three ways out. The first is taken, for excerpts, and the section below is it:

- **Convert once, offline.** Turn the file into 32-bit float, which the backend
  already reads. `scripts/wav24_to_float32.py` does that and takes an excerpt
  while it is there. Whole files cost 4/3 of the size, about 2.9 GB each and
  17 GB in total, and nothing has needed that yet.
- **Read 24-bit on the host.** The smallest change in this repository and the
  one the comment argues against: the host pass it needs is the one the upload
  kernels exist to avoid.
- **Convert on the GPU.** A third upload kernel beside cu8, cs16 and cf32.
  24-bit is three bytes a sample and does not divide into a 32-bit lane
  cleanly, which is why it is not already there.

So the files as delivered are unreadable by this engine and an excerpt of one
is not, which is worth saying plainly because they look like they should just
open.

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

## What they are not

Not a corpus. `tests/corpus/README.md` describes what a corpus entry is: a
SigMF sidecar, a checksum, a retrieval URL and a ground-truth record saying
what a correct decoder should recover. These have a filename and nothing else,
and nothing in `tools/bench` knows about them.

Not a replay of anything this project has already done. The RDS decode in
`docs/rds-first-decode.md` was broadcast FM at 98.1 MHz and its samples are
gone; these are HF and cannot stand in for it.
