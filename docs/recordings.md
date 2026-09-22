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

Three ways out, and none of them has been chosen:

- **Convert once, offline.** Turn each file into 32-bit float, which the
  backend already reads. It costs 4/3 of the size, about 2.9 GB a file and
  17 GB in total, and the conversion is somebody else's tool rather than code
  in this tree.
- **Read 24-bit on the host.** The smallest change in this repository and the
  one the comment argues against: the host pass it needs is the one the upload
  kernels exist to avoid.
- **Convert on the GPU.** A third upload kernel beside cu8, cs16 and cf32.
  24-bit is three bytes a sample and does not divide into a 32-bit lane
  cleanly, which is why it is not already there.

Until one of those happens, the recordings are on disk and unreadable by this
engine, which is worth saying plainly because they look like they should just
work.

## What they are not

Not a corpus. `tests/corpus/README.md` describes what a corpus entry is: a
SigMF sidecar, a checksum, a retrieval URL and a ground-truth record saying
what a correct decoder should recover. These have a filename and nothing else,
and nothing in `tools/bench` knows about them.

Not a replay of anything this project has already done. The RDS decode in
`docs/rds-first-decode.md` was broadcast FM at 98.1 MHz and its samples are
gone; these are HF and cannot stand in for it.
