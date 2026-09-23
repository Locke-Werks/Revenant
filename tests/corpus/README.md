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

Arrives: M5, alongside extraction. Empty until then, on purpose.

## What exists ahead of this

Real IQ landed on the development machine on 2026-09-22, before any of the
machinery above. `docs/recordings.md` records what it is: six hours of HF
across the 2017 eclipse, 40 m and 20 m, 96 kS/s.

It is not a corpus entry and this directory stays empty. There is no sidecar,
no checksum, no retrieval URL and no ground-truth record, which are the four
things an entry is. That document has the detail, and how the engine reads
them.

WHAT THIS PARAGRAPH USED TO SAY: "The engine cannot read the files either,
because they are 24-bit." It reads them natively now, through the cs24 upload
kernel.
