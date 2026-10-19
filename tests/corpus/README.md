# Real-signal corpus

This directory holds pointers to the corpus, never the corpus itself.

## Why it is not here

Captured on-air IQ is large. A single minute of 20 MS/s at 8 bits is about
2.4 GB, and the corpus needs many recordings per mode across a range of
conditions. Git is the wrong store for that, and a repository that grows to
tens of gigabytes is one nobody can clone.

So the recordings live outside the repository, versioned separately, and this
directory carries the manifests that say which recording a test expects, where
to get it, and what its ground truth is.

## Why the corpus exists at all

Synthetic signals catch algorithmic errors. Only real signals catch the
pathologies: the transmitter with 300 Hz of drift, the pager system with a
nonstandard preamble, the FT8 station whose clock is four seconds off. A
decoder validated only against `tools/siggen` output is a decoder validated
against our own understanding of the specification, which is exactly the thing
most likely to be wrong.

## How it gets built

Layer 2's region extraction is the collection mechanism: select a region in
time and frequency, export it with its metadata and provenance intact. That is
why the corpus and the time machine are scheduled together rather than the
corpus being gathered by hand beforehand.

## Format

Each entry is a SigMF metadata sidecar plus a checksum and a retrieval URL, and
a ground-truth record stating what a correct decoder should recover from it.
The harness in `tools/bench` reads these and skips, loudly, any entry whose
data file is not present locally, so a developer without the corpus still gets
a green synthetic run and an explicit list of what went untested.

An entry is `<name>.sigmf-meta` in this directory: the sidecar
`revenant-capture extract` wrote, with these keys added to `global`:

    "core:sha512": "<dataset checksum>",
    "corpus:url": "<where to fetch <name>.sigmf-data>",
    "corpus:min_recall": 0.9,
    "corpus:max_false_alarms": 3,
    "corpus:truth": [
      {"core:freq_lower_edge": 152000000, "core:freq_upper_edge": 152020000,
       "core:label": "POCSAG"}
    ]

Edges are absolute Hz; `core:label` is optional and, when present, must match
`detect::label_track`'s name. `corpus:max_false_alarms` is optional. The truth
is its own key because the sidecar's `annotations` already hold what the live
detector saw. The dataset is `<data dir>/<name>.sigmf-data`, with the sidecar
extract wrote beside it, since the file source finds a dataset through that.

`bench corpus --data DIR` (or `REVENANT_CORPUS`) replays each entry through the
engine, the detector and tier two, prints one line per entry and a total, and
exits 1 when an entry is under its thresholds. `tools/bench/corpus.h` has the
scoring. The corpus itself is still empty.

WHAT THIS PARAGRAPH USED TO SAY, until 2026-10-18: "The corpus is still empty
and nothing in `tools/bench` reads a manifest yet, so the harness above is the
design and not code. Issue #4 tracks it."

WHAT THIS PARAGRAPH USED TO SAY, until 2026-10-16: "Arrives with extraction,
which needs the rolling capture to extract from: the README's roadmap numbers
that M4, and nothing of it is built."

WHAT THIS LINE USED TO SAY: "Arrives: M5, alongside extraction." M5 was the
time machine's number in the plan this directory was written from. The
README's roadmap, set out for v0.1.0 on 2026-09-27, has no M5 and puts the
rolling capture at M4.

## What exists ahead of this

Real IQ landed on the development machine on 2026-09-22, before any of the
machinery above. `docs/recordings.md` records what it is: six hours of HF
from the morning of the 2017 eclipse, before it reached the receiver, 40 m and
20 m, 96 kS/s. That document says what this paragraph used to say, "six hours
of HF across the 2017 eclipse", and why it was wrong.

It is not a corpus entry and this directory stays empty. There is no sidecar,
no checksum, no retrieval URL and no ground-truth record, which are the four
things an entry is. That document has the detail, and how the engine reads
them.

WHAT THIS PARAGRAPH USED TO SAY: "The engine cannot read the files either,
because they are 24-bit." It reads them natively now, through the cs24 upload
kernel.
