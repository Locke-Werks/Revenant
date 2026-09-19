# Provenance and licensing

The file is still `docs/clean-room.md`. Several source headers, the CI guard and
the README point at that path, and renaming it would break the references
without changing anything they need from it. The rule inside it changed on
2026-09-18. The name did not.

Clean-room is still how this project is built. It is no longer the only way
this project is allowed to be built.

## The licence

Revenant is GPL-3.0-or-later. See `LICENSE`.

It was all rights reserved until 2026-09-18, held there on the reasoning that
permissive, copyleft, dual and source-available were all still reachable and
the choice cost nothing to defer. Getting a radio working made the choice.
librtlsdr is the only practical path to an RTL2832U, it is copyleft, and the
clean-room route to that device was measured and found blocked rather than
merely slow. `docs/rtlsdr-provenance.md` finding 2 is the measurement.

**Why 3.0 rather than 2.0.** librtlsdr is GPL-2.0-or-later, and the "or later"
is the whole reason this decision was cheap. GPL-2.0-only cannot be combined
with LGPL-3.0 code. Qt6 is LGPL-3.0. A 2.0-only dependency would therefore have
taken the user interface with it: the combined work could have satisfied the
radio's licence or the toolkit's and never both, and the interface milestone
would have died on a decision that looked like it was only about a dongle.
Taking the "or later" option up to 3.0 keeps Qt reachable and costs nothing
else.

Verify that grant per file before distributing anything. `COPYING` at the root
of `osmocom/rtl-sdr` is the plain version 2 licence text; the "or later" lives
in the notice at the head of each source file, and the root file alone does not
establish it. Checked against the installed `rtl-sdr.h` of the vcpkg port at
v2.0.2 on 2026-09-18, which reads "either version 2 of the License, or (at your
option) any later version."

**Relicensing is not publishing.** GPL obligations attach to distribution.
Writing the software triggers nothing. Running it triggers nothing. A private
repository whose binaries stay on the machines of the person who built them
triggers nothing. The duty to offer corresponding source begins when a binary is
handed to somebody else, and not before.

This repository went public on 2026-09-18, by choice rather than by
obligation: the licence did not force it and would not have until the first
binary shipped. Publishing the source is now itself how the obligation is
satisfied, which is the cheap way to satisfy it. A written offer alongside
each installer is the alternative, and it is a promise somebody has to keep
for three years against builds nobody kept.

## The policy

Three rules, in the order they come up.

**Clean-room is the default.** Where a published specification exists, work from
the specification and cite it, with a document and a section or table number on
every constant. That covers almost all of this project and the reason has not
changed with the licence: a specification is traceable, and a reader can check
the code against the document years after everyone has forgotten writing it. The
practice section below is what that means in a file.

**Linking a copyleft library is a permitted exception, recorded.** Permitted
where it is the only practical path to a capability. Not permitted as the
quicker of two routes that both work. Every instance gets a row in the table
below naming the library, its licence, what it is used for, and why the
clean-room path was rejected, with a pointer to the evidence for that rejection.
One row today.

**Do not launder.** Reading a GPL library and then reimplementing it in a file
presented as original is now legally unnecessary and it is still forbidden here.
The reason is no longer the licence. It is that a provenance claim nobody can
trace is worth nothing whatever the licence says. A constant whose history is
"somebody remembered a driver" cannot be reviewed, cannot be checked against a
document when the device misbehaves, and cannot be defended if it is ever
questioned. Copy it and say so, or link it and say so. Both are honest and both
are now available. Passing another project's expression off as your own is
neither, and it is the one thing this document has always been about.

## Linked copyleft dependencies

| Library | Licence | Used for | Why the clean-room path was rejected |
| --- | --- | --- | --- |
| `osmocom/rtl-sdr` (librtlsdr) | GPL-2.0-or-later | RTL2832U device access for the RTL-SDR v3 backend: vendor control transfers, tuner programming, and the raw IQ mode | No published specification for the IQ mode exists. `docs/rtlsdr-provenance.md`, finding 2 |

The short form of that finding, so this table can be read without opening the
other document. The Realtek RTL2832U datasheet, Rev 1.4, specifies the transport
in detail: the vendor command layout, the block selector, the I2C repeater, the
resampler and DDC equations, the GPIO block and the bulk endpoint. The signal
path it describes ends in a FIFO of 188-byte transport stream packets. It
contains no raw IQ mode at all, under any of the words that mode might be filed
under, because that mode was found by sniffing a device in 2012 and has never
been published by Realtek. It exists as one project's expression of a discovery,
and that project is librtlsdr. A backend written from the datasheets alone would
configure the device correctly and never receive a sample.

Two clean-room routes to that mode did exist and were both rejected. Capturing
librtlsdr driving the device and transcribing the register sequence copies the
expression and passes it through a protocol analyser instead of a text editor,
which changes how easy it is to notice and nothing else. Writing to undocumented
offsets on the device and watching the endpoint is genuine clean-room work and
is defensible, and it is open-ended research with no schedule attached.

librtlsdr arrives as a vcpkg dependency. No GPL source lands under `core/`,
`tools/` or `ui/`, which is why the CI guard below still means something.

**Adding a row.** Read the licence from the project's own licence file and from
the notice at the head of its source files, because those disagree more often
than they should. State why the clean-room path fails, in a document under
`docs/` that someone else can check. Then link it. A row here is a disclosure,
not a permission slip written after the fact.

**A GPL-2.0-only library still cannot be linked.** Moving to GPL-3.0 did not
open the copyleft ecosystem generally. libhackrf is recorded below as GPL-2.0
from its `COPYING` file, and if a HackRF backend is ever wanted the per-file
grant has to be read first: 2.0-only against a GPL-3.0 work is incompatible in
both directions, and no amount of care in the wrapper fixes it.

## What this does not license

This decision is about one device backend. It is not a general permission to
reach for a GPL implementation whenever one exists.

The decoders have published specifications. ITU, ETSI, RTCM, ICAO and the rest
publish revisioned documents with section numbers, which is the best provenance
available for anything, so the decoder work at M4 stays clean-room. The reason
is quality rather than law: a decoder written from a standards document can be
checked against that document line by line when a frame fails to parse, and a
decoder ported from somebody's C file can only be diffed against the C file. One
of those is debuggable at three in the morning.

The same holds for the DSP. Every kernel and every CPU twin in `core/` was
written from textbook mathematics with the chapter cited in the file header, and
those headers stay exactly as they are. Their claims are about the file that
carries them, they were true when they were written, and they are still true.

## What is allowed

- Published specifications and standards documents.
- Manufacturer datasheets, register descriptions, application notes and product
  documentation.
- Your own measurements: a logic analyser on the bus, a USB capture of a
  proprietary driver, a spectrum observation of the device's own output.
- Permissively licensed source, where the licence is recorded in the commit and
  the attribution requirement is honoured. MIT, BSD, Apache-2.0, Boost. Check,
  do not assume from the ecosystem it sits in.
- Reading a GPL project's documentation, issue tracker or protocol write-up,
  where that text is presented as documentation rather than as source. Record it
  as the source in the provenance comment like any other document.
- Linking a copyleft library that has a row in the table above.

## What is not

- Reading GPL or AGPL source while implementing a component this project writes
  itself. If the capability is coming from a copyleft library, link the library.
  Reading it and writing your own version of it is the worst of both.
- Porting, transliterating, or "reimplementing from memory" after reading.
- Vendoring a GPL file into the tree, in any directory, including tests. A
  linked dependency comes from the package manager and stays there.
- Copying a table of magic values out of a driver into a file this project
  claims as its own. A single value read off a datasheet is a fact. A sequence
  somebody selected and ordered to bring a chip up is their expression of it,
  and a register initialisation table is exactly that.
- Asking a model to reproduce, summarise or "explain what this driver does" for
  a component being implemented here. The output of that is downstream of the
  source and carries no provenance at all.

If you have already read the source for something you are about to write, say so
and hand that component to somebody who has not, or link the library instead.
That is an ordinary thing to happen and a cheap thing to fix. It becomes
expensive only after it is committed without being mentioned.

## The practice

A rule with no practice behind it cannot be tested, because a claim about how
code was written is only checkable against what the code shows.

**Every register write and protocol constant in a device backend carries a
comment naming the source document and its section or table number.**

```cpp
// RTL2832U datasheet section 8.3, table 8-4: USB control endpoint request
// layout. The index field selects the register block, not the register.
```

A number with no citation is the failure mode this catches. It reads as though
somebody knew it, and nobody can tell afterwards whether they knew it from a
datasheet or from a driver.

**Every decoder states, in its header, the specification it implements by
document number, plus any deviation and why.**

```cpp
// Implements ITU-R BT.xxxx-y section 4.2, with one deviation: the sync word
// search runs over two frames rather than one, because a single frame at the
// specified threshold misses the first burst after a squelch tail.
```

**Behaviour derived from observation is labelled as observation.** Where a
specification is inadequate, incomplete, or simply wrong about a real device,
the comment says the behaviour was measured and says how:

```cpp
// Not in the datasheet. Measured on the device on the desk with a USB capture:
// the tuner NACKs for approximately 3 ms after a band switch, so a read issued
// immediately returns the previous value rather than failing.
```

A reader has to be able to tell measurement from documentation at a glance. An
observation dressed up to look like a citation is worse than no citation,
because it defeats the review that would have caught it.

**Provenance is a review item.** The reviewer checks that constants carry
citations and that the citations are to documents, on every pull request that
touches a backend or a decoder. This is part of the review, not a separate pass
that gets skipped when the change looks small.

## The CI guard

`.github/workflows/ci.yml` fails the build on a copyleft licence grant found
under `core/`, `tools/` or `ui/`. It scans those three directories and nothing
else, so `docs/` is untouched and this document can discuss the licences it
names.

What it looks for changed with the licence, and it took two attempts.

The original guard matched the name of a licence, which was right while no
source file here had any business mentioning the GPL. That stopped being right
the moment a backend needed to say in a comment that it links librtlsdr.

The first replacement matched the grant paragraph at the head of a pasted
file, on the reasoning that nobody writes that text by hand. That reasoning was
false in both directions and review caught it before anyone hit it. Revenant's
own files may carry exactly that paragraph, because `LICENSE` itself, under
"How to Apply These Terms to Your New Programs", instructs authors to put it at
the head of every source file: the guard would have failed the build for
complying with the project's licence. And it missed the crudest paste there is,
a file lifted from a kernel tree whose header is one SPDX line and no grant
paragraph at all, which the guard it replaced would have caught.

What the guard matches now is whose copyright a file carries. A pasted file
arrives with its author's copyright line; Revenant's own carry Locke Werks or
nothing. Any SPDX identifier other than this project's own fails as well, which
restores the coverage the first replacement gave up. Read
`.github/workflows/ci.yml` for the exact expressions rather than reproducing
them here, so that this document does not carry the strings it describes.

That keeps the distinction the policy rests on. Linking a copyleft library is
permitted and recorded in the table above. Copying its source into this tree is
not, and the two are different acts with different consequences: a linked
dependency is a version in `vcpkg.json` that updates, carries its own copyright
and can be swapped out, while a pasted file is an unmaintained fork whose
provenance has to be reconstructed from memory the first time anybody asks.

The grep was always a backstop for the crude case. It cannot detect a ported
algorithm, which is what the citation practice and the review are for. Neither
substitutes for the other.

## The prior-art research, and why it is kept

The survey below was done while the rule was still absolute. Its conclusions
have not changed and they are the evidence that the librtlsdr exception was
necessary rather than convenient. A reader asking "did they even look?" gets the
answer here.

**The host libraries for this hardware are copyleft.** Verified on 2026-09-18 by
reading the licence file in each repository:

| Project | Licence | Verified from |
| --- | --- | --- |
| `osmocom/rtl-sdr` (librtlsdr) | GPL-2.0-or-later | `COPYING` at the repository root for the version 2 text, the per-file notice for the "or later" |
| `greatscottgadgets/hackrf` (libhackrf) | GPL-2.0 | `COPYING` at the repository root |
| `pothosware/SoapySDR` | Boost Software License 1.0 | `LICENSE_1_0.txt` |
| `libusb/libusb` | LGPL-2.1 | `COPYING` at the repository root |

SoapySDR itself is permissive and never was the problem. Its device modules are:
`SoapyHackRF` carries an MIT licence on its own source and then links
`libhackrf`, and the RTL module links `librtlsdr` the same way. A permissive
wrapper around a copyleft library is still a copyleft obligation on the binary
that ships the combination. Routing through SoapySDR moves the licence one link
down the chain and leaves it there. That was the finding when the goal was to
avoid the obligation, and it is the same finding now that the obligation is
accepted: the wrapper changes nothing about what the combined work has to do.

**There is no permissively licensed prior art for the RTL2832U.** Seven
candidates were checked and every one was disqualified by reading its licence
and its own README. The recurring pattern is a repository declaring MIT or
Apache while carrying relabelled GPL source or crediting a GPL project for "the
magic numbers". `jtarrio/radioreceiver` is the clean example: Apache-2.0 on its
own file, and a README thanking the RTL-SDR project for figuring out the magic
numbers. Apache-2.0 covers the authors' own expression and cannot relicense
values taken from somebody else's GPL work. Its ancestors and its JavaScript
descendants inherit the same defect. A permissive label on a repository is not a
licence audit, and for this hardware it is usually wrong.

`docs/rtlsdr-provenance.md` finding 3 has the full table, including the adjacent
radios, where the answer differs per device: libairspyhf is genuinely
BSD-3-Clause, libiio is LGPL-2.1, LimeSuite is Apache-2.0, and the SDRplay API
is closed rather than copyleft.

**The asymmetry that drove the old rule, stated once so nobody re-derives it.**
Copyleft cannot be taken back out of a tree it has entered: there is no rewrite
that launders a ported file, and the only remedy for "we read their driver" is a
different person implementing that component from scratch, having never seen it.
Relaxing your own terms is free; relaxing somebody else's is impossible. That
asymmetry is why the position was held open as long as it was, and it is why the
exception process above requires a recorded reason rather than a shrug. What
changed on 2026-09-18 is that the destination was chosen deliberately, with the
cost known. What has not changed is that nothing should arrive there by
accident.

## The RTL-SDR v3 backend and the documents assembled for it

This list was built for a clean-room backend that is not now being written. It
stays because the documents are still the reference for anything the library
does not expose, because the M0 record depends on it, and because a future
backend over libusb remains possible if somebody wants to do the exploratory
work that finding 2 describes.

1. **Realtek RTL2832U datasheet.** "RTL2832U DVB-T COFDM Demodulator + USB 2.0
   Interface", Rev 1.4, 2010-11-01. Covers the USB vendor command layout, the
   register block selector, the I2C repeater and master, the DDC, the resampler
   and the GPIO block, including the equations for the sample rate and IF
   frequency registers. It does not cover the raw IQ mode, and the `bRequest`
   byte is printed as a literal `x` for all twelve vendor commands.
2. **Rafael Micro R820T2 Register Description**, version 1.3, 2014-10-14. All
   thirty-two registers with per-bit semantics, the I2C address and the PLL
   equations. Rafael Micro released it in response to a request for driver
   documentation, which makes it the best-provenance document of the set.
3. **USB 2.0 specification**, chapter 5 for transfer types and chapter 9 for the
   device framework.
4. **libusb-1.0 API documentation**, for asynchronous transfer submission, the
   event loop, and the ownership rules for a transfer between submission and
   callback.
5. **RTL-SDR Blog V3 product documentation**, for what the v3 board adds over a
   generic dongle: the temperature compensated oscillator, the software
   switchable bias tee, the direct sampling input and the expansion pads.

Two honest notes about that list, both of which correct earlier versions of this
document.

The R820T2 register map is fully public, and this note once said the opposite.
The confusion is that there are two Rafael Micro documents: the R820T *datasheet*
describes the functional blocks without enumerating every register, and the
separate *Register Description* enumerates all of them. The second is the one
that matters.

The coverage estimate in the earlier version, "roughly one part in seven"
missing and fillable by measurement, held for the tuner and did not hold for the
demodulator. The demodulator's missing piece is not a fraction of the register
writes. It is the one step the rest of them exist to serve. That correction is
the finding that produced the licence change, and it is the reason a fraction
was the wrong way to measure the gap.

This document states no register numbers. A register number asserted here
without a citation would be the exact provenance failure the practice exists to
prevent. The one place offsets do appear in the record is
`docs/rtlsdr-provenance.md`, where two tables in the datasheet disagree about
the I2C repeater bit and the disagreement is itself the finding.

**The device is already bound to WinUSB.** The dongle on the development machine
was bound by libwdi, so libusb can open it with no driver work, no Zadig step in
the build instructions and no INF to ship. That binding is what librtlsdr needs
on Windows too, since it reaches the device through libusb like everything else
here. Any machine that has not had it done needs it once. It is a Windows driver
association, not a code dependency, and nothing about it touches the licence
position.

**What M0 proved.** `tools/devicespike` opens the dongle, reads its descriptors,
finds the bulk IN endpoint that carries IQ, and claims and releases interface 0.
On the development machine it reports `RTL2838UHIDIR` on endpoint 0x81 with a
512-byte maximum packet. That is the whole device path short of the tuner,
verified against real hardware before anything was built on it.

It stopped short of the tuner deliberately, on the expectation that the
datasheets would carry the register access later. They did not, which is the
finding that settled the licence. The spike itself is unaffected: it reads no
tuner register, it read no GPL source, and what it proved is still what it
proved. Its header keeps the original reasoning and marks it as history rather
than policy, and the file survives as the USB-layer diagnostic for a dongle that
will not enumerate, which is a question `librtlsdr` in the path makes harder to
answer.

## libusb, and one open item

libusb is LGPL-2.1. Linking it dynamically, unmodified, keeps the obligation
where the LGPL puts it: the user must be able to replace the library, which a
DLL beside the executable satisfies on its own.

LGPL-2.1 section 3 carries the option to apply the ordinary GPL, version 2 or
any later version, to a copy of the library. That option is what makes an
LGPL-2.1 dependency combinable with a GPL-3.0 work, so the move to GPL-3.0
raises no new question here. librtlsdr reaches the device through libusb as
well, so there is now one more consumer of the same library and the same
obligation.

Open, and to be decided before anything is distributed: the repository's presets
set `VCPKG_TARGET_TRIPLET` to `x64-windows-static`, which builds libusb as a
static library and pulls it into the executable. A static link to an LGPL
library is permitted and carries the relinking obligation, which means shipping
object files or an equivalent mechanism so the user can relink against their own
libusb. That is a real obligation on the release pipeline rather than a
formality, and it is unchanged by the licence move: GPL-3.0 satisfies the
user's freedom to modify Revenant, and the LGPL asks separately about their
freedom to modify libusb.

The two ways out are a per-port triplet that builds libusb as a DLL while the
rest of the tree stays static, or accepting the relink obligation and building
it into the release process. It is recorded here rather than settled quietly at
build time, because a triplet is the kind of setting somebody changes for an
unrelated reason without knowing a licence depends on it.

## Disclosure log

Every exposure gets recorded here, with what was seen, when, and what was done
about it. The log stays useful after the licence change, for the same reason it
was useful before: it is the record of what the people writing this project
actually read. Four entries, all self-reported by the person who did it, which
is the behaviour the practice needs to keep producing.

**2026-09-18, polyphase channelizer design.** While verifying the licences of
candidate reference implementations, the first 1200 bytes of GNU Radio's
`gr-filter/lib/pfb_channelizer_ccf_impl.cc` were fetched to read its SPDX
identifier, and the fetch over-ran the identifier. What was seen: the copyright
header, the SPDX line, the include block, the factory function signature and
the opening of the constructor's initialiser list. No algorithm: nothing about
the commutator, the branch ordering, the transform or the phase correction, and
nothing further was read. That file is GPL-3.0-or-later.

The channelizer design was complete and numerically verified before the fetch,
and the probe results that verify it precede it in the record.

Resolution: the design stands, and the implementation was handed to sessions
that had never seen the disclosure or the fetched content, working from a brief
with this entry removed. So no one who wrote the channelizer code had any
exposure to that file, which is the property the claim actually needs. The cost
of doing it that way was close to nothing, which is the reason to prefer it over
assessing one's own exposure and deciding it was probably fine. The channelizer
headers say the file was written without reading a GPL implementation, and that
remains true.

**2026-09-18, RTL-SDR prior-art survey.** Seven candidate implementations were
examined to see whether any permissively licensed prior art could lawfully be
read. All seven were disqualified, most of them by reading only a licence file,
a README or an attribution line. No implementation source was read. The
conclusion, recorded above, is that there is no usable permissive prior art for
this hardware.

**2026-09-18, RTL2832U document survey.** The research recorded in
`docs/rtlsdr-provenance.md`. No GPL source was read during it. Licences were
verified from licence files and GitHub licence metadata; a search result linked
directly to a libhackrf source file and it was not opened; librtlsdr, libhackrf
and the Linux kernel DVB drivers were not opened at all. The two datasheets, the
SDRplay specification, vendor product pages, project READMEs and third-party
articles are documentation, which this document permits.

That survey is what produced the licence change, so it is the last entry written
under the absolute rule and the first piece of evidence for the exception.

**2026-09-18, librtlsdr per-file licence check.** During review of the RTL-SDR
backend, the first 2400 bytes of `librtlsdr.c` and `tuner_r82xx.c` were fetched
from `osmocom/rtl-sdr` to verify that the "or later" grant is present in the
files that actually compile into the linked library, rather than only in the
installed `rtl-sdr.h`, which contributes no object code. The grant is present
in both, which is what the licence section above rests on.

The fetch over-ran the notice, the same shape as the channelizer entry above.
What was seen beyond it: in `librtlsdr.c`, the include block, the tuner
interface struct, the async status enum and the opening of a comment about FIR
coefficients; in `tuner_r82xx.c`, the `r82xx_init_array` register table and the
start of the frequency range table.

Recorded rather than waved through, because a register initialisation table is
the exact artefact the no-laundering rule names, and because the licence change
does not retire that rule. Resolution: nothing here implements a tuner, none of
it has been reproduced, and Revenant calls librtlsdr rather than reimplementing
it, so there is no file whose provenance this touches. Had a clean-room tuner
been in progress, this exposure would have disqualified its author from writing
it, and the entry exists so that a future reader can see the difference.

It also demonstrates the thing the log is for: the reviewer volunteered this
against their own work, unprompted, when the cheaper move was silence.

**What the log is for now.** Under the old rule an exposure was a contamination
to be contained. Under the current one it still gets written down, because the
file-level claims in `core/` are what make the DSP's provenance checkable and
those claims are only as good as the record of who read what. An exposure that
touches a file claiming to be clean-room gets an entry and gets resolved,
exactly as before.

## Provenance of the two datasheets

Separate from copyleft, and not to be confused with it. Both documents are
marked confidential on every page.

The R820T2 Register Description was released by Rafael Micro in response to a
request for driver documentation, per rtl-sdr.com. No formal grant is recorded
and the pages still read CONFIDENTIAL. Published in practice.

The RTL2832U datasheet is stamped "CONFIDENTIAL: Development Partners Only" and
overprinted "for V4L". It reached the public through the V4L kernel developers,
and Realtek publishes no datasheet at all. A leaked NDA datasheet is not
straightforwardly a published specification, and the two documents are not equal
on that axis. Neither creates a copyleft problem, and the distinction stays
recorded because "we worked from the register description the manufacturer
released on request" is a stronger sentence than the alternative if the question
is ever put.

## If a question comes up later

The evidence that this practice was followed is in the repository, not in this
file:

- provenance comments on constants, in the commit that introduced them
- specification references in decoder and reference headers
- commit history showing the specification cited before the code appeared
- the CI guard, in every run since the first commit
- the table of linked copyleft dependencies above, and the document each row
  cites for why the exception was taken
- this document, dated and versioned alongside the code it describes

The claim is only ever as good as those six. Keep them.
