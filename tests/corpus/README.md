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
