# The first on-air RDS decode

2026-09-20. Until this run the RDS decoder had only ever been pointed at
`core/dsp/synth/rds_mod.h`, the synthetic transmitter written from the same
clauses of the same standard as the decoder. A round trip through that proves
the two halves agree with each other. It cannot prove either one agrees with a
real broadcaster, because a shared misreading of the document produces a
perfect round trip.

This is the record of the first decode of a signal nobody in this project
generated.

## Setup

    Radio        RTL-SDR v3, USB, gain 20 (not auto, see below)
    Sample rate  2400000
    Centre       98100000
    Receiver     WFM, audio_rate 171000, RDS on
    Antenna      the whip that came in the box, indoors, Colorado Springs

`audio_rate = 171000` is the RDS tap. It is not an audio rate anybody listens
to: it is 3 x 57000 and 144 x 1187.5 exactly, which puts the subcarrier and
the bit clock both on integer relationships to the sample grid, and it makes
the ordinary `AudioSink` carry the FM composite rather than demodulated audio.
The arithmetic is written out on `VrxParams::audioRate` in
`core/rpc/revenant.capnp`; `core/engine/vrx.h` carries the 114 kHz bound at
which a WFM receiver stops being an audio receiver and stops taking a
de-emphasis curve by default.

Gain is 20 and not `auto` because `auto` overloads the front end here. That
was measured in the same session and is filed as task #54; with `auto` the
detector reported three intermodulation products as tracks at confidence 1.00,
and gain 20 improved this station's measured SNR by 5.7 dB.

## What came back

    PI           0x2AF6
    Callsign     KKFM
    PS           "98.1 KKFM"
    RadioText    "98.1 KKFM Weekends" (all 16 segments, no gaps)
    PTY          6, Classic Rock
    Bit rate     1187.51 bit/s measured against the recovered clock
    Offset       -0.4 Hz from nominal 1187.5

The callsign is the part that is evidence rather than decoration. Nothing in
the decoder was told that 0x2AF6 is KKFM. `core/decode/rds_groups.cpp`
recovers the PI code from block A, and the NRSC-4-B callsign arithmetic turns
a PI in the 0x1000 to 0x994F range into a three or four letter call by base-26
division. Two parts of the decoder that share no data arrived at the same
fact, and the fact is independently checkable against the FCC database. A
decoder that had misread the group format would still produce a PI, but it
would not produce a PI that divides into a real station's callsign.

## Negative controls

Three frequencies with no RDS carrier, checked in the same session with the
same receiver settings:

    96900000   pilot locked, carrier coherence 0.05
    98900000   pilot locked, carrier coherence 0.16
    103900000  pilot locked, carrier coherence 0.09

Pilot locked and coherence near zero is the correct answer: there is a 19 kHz
pilot to lock to on every one of these, and no 57 kHz subcarrier bearing data.
The decoder did not invent a PI on any of them. Without this the KKFM result
would only show that the decoder emits something, not that it emits something
conditional on the signal.

## The BLER problem, which is the reason this document exists

Two runs on KKFM an hour apart, identical command lines, identical receiver
settings, identical decoded content:

    Run 1    0.0 percent block error rate
    Run 2    12.7 percent block error rate

The flags were the same. The station did not change its name between runs. In
run 2 a station whose PS never changes read `-FFM-FM` at one point, which is
the mis-correction the CRC's minimum distance permits: the RDS checkword has
minimum distance 5, so some weight-3 error patterns are a weight-2 pattern
plus a valid codeword and get rewritten into a DIFFERENT valid block rather
than refused. `core/decode/rds_groups.h` always said this; no test asserted
the real trade until the same session measured it at 136 wrong characters.

So on-air BLER is not a property of this decoder. Nothing in the run
controlled the antenna, the multipath, or the hour. Quoting a BLER figure from
an uncontrolled on-air run as a decoder quality number would be wrong, and
this document quotes both numbers so that nobody later quotes one of them.

## What this result is, and is not

It IS evidence that the decoder reads a real broadcaster's group format, sync,
offset words and checkword correctly, and that the PI to callsign arithmetic
is right. That is the thing a synthetic round trip could never show.

It is NOT a regression test. **No IQ was captured.** The run was live off the
radio and the samples are gone, so this cannot be replayed and a future
regression cannot be caught by it. That is the gap, and closing it needs the
capture path: record thirty seconds of 2.4 MS/s centred on 98.1 MHz, store it
as a SigMF pair, and add it to `tests/corpus/` with this decode as its
ground-truth record. `tests/corpus/README.md` describes that format and why
the recording itself does not live in the repository.

Until that capture exists, the synthetic round trip remains the only RDS test
that runs in CI, and this page is a written result rather than a fixture.
