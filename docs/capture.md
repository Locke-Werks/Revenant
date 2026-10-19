# Rolling capture

The engine can write everything the radio delivers to disk as it arrives, keep
a bounded window of it, and answer questions about what was in it afterwards.
This is M4 on the roadmap. `core/capture/format.h` is the contract every piece
below follows.

## Recording

```
revenant-cli "rtlsdr://0?freq=98.1M&rate=2400000&gain=20" \
    --detect --capture D:\capture --capture-segment 60s --capture-retain 200G

revenant-engine --capture D:\capture --capture-retain 200G --capture-max-age 7d
```

| Flag | Takes | Default |
| --- | --- | --- |
| `--capture` | Directory to write under | Off |
| `--capture-segment` | A size, `256M`, or a length, `60s`. Only a trailing `s` means seconds | `256M` |
| `--capture-retain` | Total size to keep, `200G`. Sizes are binary | Unbounded |
| `--capture-max-age` | Oldest segment to keep, `24h`, `7d` | Unbounded |

Samples are stored in the radio's own format, so an RTL-SDR at 2.4 MS/s costs
4.8 MB/s, about 17 GB an hour. Measured on 2026-10-12 at 98.1 MHz: 30 s, three
10 s segments, 137.5 MiB, indices contiguous, no block dropped.

The writer never does file I/O on the source's thread. A block that finds the
queue full is dropped and counted, and the segment it would have joined is
closed, so a loss is always a visible boundary between two segments and never
samples that look continuous and are not. A disk error stops the writer and
not the stream; the error is in the counters the CLI prints at exit and in the
session's source stats.

## What is on disk

```
D:\capture\2026-10-12\rev-20261012T203000.000Z-98100000-<epoch>-<index>.sigmf-data
D:\capture\2026-10-12\rev-20261012T203000.000Z-98100000-<epoch>-<index>.sigmf-meta
```

Each segment is a SigMF recording at one centre frequency and one rate with no
samples missing. A retune, a rate change, a loss or the size limit starts the
next one. The meta carries the stream's exact sample index and wall-clock
anchor under the `revenant:` extension, and the detector's tracks as SigMF
annotations, so a segment copied anywhere still says when it was recorded and
what was in it. Any SigMF reader opens one; Revenant's file source replays one
through the same graph a live radio feeds.

A segment still being written ends `.open.sigmf-data` and has no meta. A
writer that dies leaves one of those, which the next start finalises from the
data and a small side file. The ring deletes the oldest whole segments first
and never the one being written.

## Finding things

```
revenant-capture list D:\capture --from -2h
revenant-capture search D:\capture --freq 162.4M:162.6M --from -1d --min-duration 2s
revenant-capture occupancy D:\capture --freq 462.5M:462.75M --from -6h --bucket 10m
revenant-capture search D:\capture --decoder p25p1 --decoded talkgroup=101 --from -1d
revenant-capture index D:\capture
revenant-capture extract D:\capture --from 2026-10-12T20:30:00Z --to 2026-10-12T20:35:00Z --out E:\keep
revenant-capture recover D:\capture
```

`search` reads the annotations. Every hit prints a `capture://` URI for its
stretch and a `revenant-cli` line to listen to it. `occupancy` draws, per time
bucket, how much of it something was present in a frequency range, and tells a
quiet bucket from one nothing recorded. `index` runs the wideband detector over
segments that were captured without it, or with different settings, and writes
what it finds back into their metas; on the RTX 4090 that runs at about 12x
realtime at 2.4 MS/s. `extract` copies whole segments.

Decoders running in the engine write what they read into the metas too;
`revenant-cli --decode --capture` does not. Each identifier a receiver
decodes (a P25 or DMR talkgroup, source or destination; an AIS MMSI or
callsign; an AX.25 or M17 source; a POCSAG address; a D-STAR caller; a TETRA
network's country and network codes) becomes one annotation under the
`revenant-decode` generator, held open across every message carrying it until
it has been quiet for 10 s, its receiver retunes or goes away, or the capture
stops. `--decoder NAME` and `--decoded KEY=VALUE` find them, in `search` and
`occupancy` alike. Repeat `--decoded` to require every term. Keys and values
match whole and ignore case, so `talkgroup=101` does not find 1011 and
`callsign=wdc1234` finds WDC1234. A P25 control channel's grant groups are
filed as `talkgroup`, the same key a voice channel uses. Free text is never
indexed.

## Replaying

```
revenant-cli "capture:///D:/capture?from=2026-10-12T20:30:00Z&to=2026-10-12T20:35:00Z" \
    --vrx 162.55M:nfm --play
```

`from` and `to` take ISO 8601 UTC or Unix nanoseconds, and `pace` works as on a
file. A gap between segments arrives as a loss at the point it happened, and a
retune between segments is applied at the first sample of the new segment, so
a capture of somebody scanning replays as one run. Measured on 2026-10-12: ten
seconds of a 2.4 MS/s capture spanning two segments replayed as exactly
24,000,000 samples at 68x realtime with every counter zero.

## In the client

The settings window's capture section turns the capture on for an engine the
client starts itself: a switch (off by default), the folder (default
`%LOCALAPPDATA%\Locke Werks\Revenant\capture`), the size kept (default `50G`)
and the age kept (default none), in the same forms as the flags above and
checked by the same parser. Applying a change restarts that engine with the
new flags; the radio used last reopens, a recording does not. An engine the
client did not start keeps whatever flags it was started with, and the
section says so.

With the engine started with `--capture`, a capture button appears in the top
bar, and Ctrl+Shift+C opens and closes its panel. The panel lists hits newest
first: by default the last hour over the span being recorded, with any
length. A window of 15 min, 1 h, 6 h or 24 h, a typed band such as
`162.4M:162.6M` (read like the tuning box, so a bare number is MHz), and a
minimum length of 1, 5 or 30 s narrow it. A strip above the list shows the
window's occupancy over that band, dimmer where nothing was captured. A
double click replays a hit, and "back to the radio" opens the radio picker. A
replay is not itself recorded. The session calls behind it,
`captureCatalog`, `captureSearch` and `captureOccupancy`, are in
[rpc.md](rpc.md).

## Limits

- A track the live detector follows is annotated when it ends, as one entry
  per segment it was present in, joined by `revenant:track_id`.
- A decoded identifier's span starts where the first message carrying it
  began, to within one decode chunk: the receiver's samples are placed on the
  capture's stream when the message is handed over, not where it was read.
- Replay stops at a change of sample rate or of stream, which needs a new grid.
- The segment length is the granularity of `extract` and of the ring.
