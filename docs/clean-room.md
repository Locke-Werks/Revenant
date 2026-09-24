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
| `osmocom/rtl-sdr` (librtlsdr) at `797f814`, with four patches, built from `vcpkg-overlays/rtlsdr/` | GPL-2.0-or-later | RTL2832U device access for the RTL-SDR v3 backend: vendor control transfers, tuner programming, and the raw IQ mode | No published specification for the IQ mode exists. `docs/rtlsdr-provenance.md`, finding 2 |

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

**Which librtlsdr, since 2026-09-23.** Not the registry's. v2.0.2 freed
transfers libusb still held when a stream was cancelled, 27 to 30 times in
3000 cancels on the trial's pattern, and killed the process most times it did.
The fix is a change to librtlsdr, so the engine links a librtlsdr this
repository patches. `vcpkg-overlays/rtlsdr/` builds
`osmocom/rtl-sdr` at `797f8143266d`, which is master on that day and the v2.0.3
tag, with the registry port's three patches and a fourth,
`cancel-waits-for-transfers.diff`, that Revenant wrote. That fourth patch is
librtlsdr's expression, modified, and it stays under librtlsdr's licence; its
preamble says so, and `docs/rtlsdr-provenance.md`, "The librtlsdr the engine
links", is the record of what it changes and what it was measured against.

A port patch under `vcpkg-overlays/` is the one form in which GPL expression
sits in this tree, and it is not the vendoring the list below forbids. It is
a change to a linked library, applied by the package manager when it builds
that library, and it never reaches `core/`, `tools/` or `ui/`. Anything that
would copy librtlsdr code into one of those three is still forbidden, patch or
no patch.

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
  linked dependency comes from the package manager and stays there. The
  exception is a patch to a linked library, carried in that library's port
  under `vcpkg-overlays/` and recorded in the table above; see "Which
  librtlsdr, since 2026-09-23".
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
| `libusb/libusb` | LGPL-2.1-or-later | `COPYING` at the repository root for the version 2.1 text, the per-file notice for the "or later". The survey recorded only the root file and so recorded only "LGPL-2.1"; corrected 2026-09-19, see below |

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

## The static-link relink question, and what a release owes

The open item, as it was recorded: the presets set `VCPKG_TARGET_TRIPLET` to
`x64-windows-static`, which pulls libusb into the executable, and a static link
to an LGPL library carries a relinking obligation that a DLL beside the
executable would have discharged for free. It was written down while Revenant
was proprietary and in a private repository, where none of it had come due.

Both of those facts changed on 2026-09-18, so the question is worked through
here rather than left sitting.

**Decided 2026-09-20. The obligation is discharged by section 6d, not 6a.** A
release offers the binary and the corresponding sources from one designated
place, which satisfies LGPL-2.1 section 6d and GPL-3.0 section 6d in a single
act. The sources it carries are libusb v1.0.29 upstream, which is the complete
corresponding source because the vcpkg port applies no patches, and the three
vcpkg rtlsdr diffs, which are Corresponding Source for the executable under
GPL-3.0 section 1 whatever happens on the LGPL side. Moving libusb to a DLL
was considered and rejected: 6b would discharge the clause for free, and it
costs the single self-contained signed binary that the static triplet was
chosen to produce, which is a worse trade than a file in the release.

The sources moved on 2026-09-23 and the route did not. librtlsdr is
now built from `vcpkg-overlays/rtlsdr/`, so the release carries its upstream
at `797f814` and all four of its patches, the three from vcpkg's port and
Revenant's own, taken from this repository at the tagged commit. libusb is
still v1.0.29 from the registry, unpatched. `docs/packaging.md` lists the
archive's contents.

The reasoning below is unchanged and is what the decision rests on. Read it to
check the decision, not to reopen it. The last subsection says what is still
open, which is no longer this.

Everything below was checked on 2026-09-19 against the installed tree at
`build/ci/vcpkg_installed/x64-windows-static`, against `LICENSE` in this
repository, and against the LGPL-2.1 text that the libusb port ships as its own
copyright file. Versions are the ones in `vcpkg_installed/vcpkg/status`.

### First correction: libusb is LGPL-2.1-or-later

This document has said "LGPL-2.1" since the prior-art survey, and that was the
root `COPYING` file being read. It is the same trap this document already
documents two sections up for librtlsdr, applied to librtlsdr and then not
applied here.

The installed `include/libusb-1.0/libusb.h` at port version 1.0.29 reads
"either version 2.1 of the License, or (at your option) any later version." The
vcpkg port declares `LGPL-2.1-or-later` in its SPDX document, which corroborates
that as the packager's reading rather than establishing it: the per-file notice
is the grant.

That matters for which argument below has to carry weight.

### What LGPL-2.1 section 6 asks of somebody shipping a static binary

Section 6 is the clause that governs distributing a work linked against the
library, statically or otherwise. It asks for four things and the relink
obligation is only the fourth of them.

1. The terms the combined work goes out under must "permit modification of the
   work for the customer's own use and reverse engineering for debugging such
   modifications". GPL-3.0-or-later permits both without limit, so this is
   satisfied by a wide margin rather than by a reading.
2. Prominent notice with each copy that the library is used in the work and
   that the library and its use are covered by the LGPL.
3. A copy of the LGPL-2.1 text supplied with the work.
4. One of subsections 6a through 6e.

The relink obligation lives in 6a, and the reason the static triplet raises the
question at all is that it gives up 6b, the "suitable shared library mechanism"
route that a DLL satisfies on its own.

**Whether publishing source under GPL-3.0-or-later satisfies 6a.** 6a asks for
two separate things. The first is "the complete corresponding machine-readable
source code for the Library including whatever changes were used in the work".
The second is, for an executable, "the complete machine-readable 'work that
uses the Library', **as object code and/or source code**, so that the user can
modify the Library and then relink".

The "and/or source code" is the operative phrase and it is why the original
framing of this item, "shipping object files or an equivalent mechanism", was
stricter than the clause. Source is a listed form, not an equivalent mechanism
argued for. A user who has Revenant's complete source, its `vcpkg.json`, its
`vcpkg-configuration.json` baseline and its CMake presets can build against a
libusb they modified, which is the capability 6a exists to preserve. GPL-3.0
already forces all of that to be published, so the second limb of 6a is
discharged by the obligation the project is already under.

The first limb is not. It asks for libusb's source, not Revenant's, and nothing
in this repository carries it. Two facts narrow that down:

- The vcpkg libusb port applies no patches. Its `vcpkg_abi_info.txt` lists a
  portfile and no diffs, and its SPDX names the upstream download as
  `git+https://github.com/libusb/libusb@v1.0.29`. So "whatever changes were used
  in the work" is nothing, and upstream v1.0.29 is the complete corresponding
  source.
- The rtlsdr port is the opposite case and it is a GPL question rather than an
  LGPL one. Its ABI includes `dependencies.diff`, `library-linkage.diff`,
  `tools.diff` and, since 2026-09-23, `cancel-waits-for-transfers.diff`, so
  the librtlsdr compiled into the binary is `osmocom/rtl-sdr` at `797f814`,
  modified. Those diffs are part of the Corresponding Source for the executable
  under GPL-3.0 section 1. They live in this repository under
  `vcpkg-overlays/rtlsdr/`, which the release's source archive carries at the
  tagged commit, so their reachability no longer rests on anybody else's
  hosting.

  This item used to say the librtlsdr in the binary was a patched v2.0.2 whose
  three diffs lived in the public vcpkg repository at the commit the port's
  SPDX names. True from 2026-09-19 to 2026-09-23, when the overlay replaced
  the registry's port.

6d is the cheaper subsection and the one that matches how this project already
satisfies the GPL: "If distribution of the work is made by offering access to
copy from a designated place, offer equivalent access to copy the above
specified materials from the same place." A release carrying the binary and the
sources together satisfies 6d and GPL-3.0 section 6d by the same act. 6c, the
three-year written offer, is the same promise the licence section above already
rejects as the expensive option.

So the static triplet is not the problem it was recorded as. It removes the free
route and leaves a route that costs a file in the release.

### Whether LGPL-2.1 and GPL-3.0-or-later combine at all

Two independent routes, and the "or later" finding above means the weaker one
does not have to hold.

**Via the "or later" grant, which is the simple route.** libusb may be taken
under LGPL-3.0 at the recipient's option. LGPL-3.0 is drafted as a set of
additional permissions layered on GPL-3.0, so a GPL-3.0 work linking it is
combining GPL-3.0 with GPL-3.0-plus-permissions, and GPL-3.0 section 7 says
additional permissions may be removed from a copy. No conversion step, no
irreversible act.

**Via LGPL-2.1 section 3, which is the route this document previously named.**
Section 3 permits applying "the ordinary GNU General Public License, version 2"
to a given copy, and adds parenthetically that if a newer version of the
ordinary GPL has appeared, that version may be specified instead. GPL-3.0 has
appeared. So the option reaches GPL-3.0 and the previous wording of this
section was right about the destination.

Two cautions on that route that the previous wording left out. It is an option
and not a requirement, so nothing is converted by accident. And section 3 says
the change "is irreversible for that copy", so exercising it deliberately on a
vendored copy is a decision with no undo, which is a reason to prefer the "or
later" route that needs no exercise at all.

Under LGPL-3.0 the relink clause is section 4d rather than 6a, worded
differently and reaching the same place: 4d(0) takes the Minimal Corresponding
Source plus the Corresponding Application Code "in a form suitable for, and
under terms that permit, the user to recombine or relink", and source is such a
form. 4d(1) is the shared-library route. Same shape, same answer.

### librtlsdr, GPL-2.0-or-later, and why the "or later" is load-bearing

GPL-2.0-only and GPL-3.0 are mutually incompatible. Each requires that the
combined work be distributed under its own terms and neither tolerates the
other's conditions, so there is no version of the combination that satisfies
both. A GPL-2.0-only librtlsdr would therefore bar a GPL-3.0 Revenant outright,
which is why the grant is checked per file rather than read off the repository
root.

It reproduces. The installed `include/rtl-sdr.h` at port version 2.0.2 reads
"either version 2 of the License, or (at your option) any later version", the
same as the 2026-09-18 check above, and the disclosure log records that the
grant was confirmed in `librtlsdr.c` and `tuner_r82xx.c`, which is where it has
to be, because a header contributes no object code. The vcpkg port declares
`GPL-2.0-or-later`, again as corroboration and not as the grant.

Checked again at `797f814` on 2026-09-23, when the overlay moved to it: the
grant is in `rtl-sdr.h`, `librtlsdr.c`, `tuner_r82xx.c`, `tuner_e4k.c`,
`tuner_fc0012.c` and `tuner_fc0013.c`. `tuner_fc2580.c`, the sixth source file
compiled into the library, carries no notice of any kind, only a line saying
it was taken from a kernel driver, and it is the same at v2.0.2. That is open
and is item 3 of the list below.

The licence section above gives one reason the "or later" mattered: Qt6 is
LGPL-3.0 and a 2.0-only radio would have taken the interface with it. There is a
second reason, which applies to the engine alone and does not need the UI to
exist.

**pthreads4w is Apache-2.0 and it is already in the engine binary.** The rtlsdr
port depends on `libusb` and `pthreads`, and the installed
`rtlsdrTargets.cmake` puts `PThreads4W::PThreads4W` in the interface link
libraries, so anything linking `rtlsdr::rtlsdr_static` links it. `core/` does.
The installed port is pthreads4w 3.0.0, SPDX `Apache-2.0`, and the static
triplet means `pthreadVC3.lib` is copied into the executable rather than loaded
beside it.

The FSF's stated position is that Apache-2.0 is incompatible with GPL-2.0,
because its patent termination and indemnification provisions are further
restrictions of the kind GPL-2.0 section 6 forbids, and that it is compatible
with GPL-3.0, whose section 7 enumerates exactly those kinds of additional term
as permitted. Taken at face value, that means the engine as it links today would
be an incompatible combination under GPL-2.0-only, with no Qt anywhere near it.
The "or later" is not a convenience that bought the user interface. It is what
makes the current link line lawful.

The policy section's rule stands unchanged and now has a demonstration attached
to it. A GPL-2.0-only library still cannot be linked, and what that would break
is no longer hypothetical: it is the link line the engine already has.

### The rest of the dependency set

Read from `vcpkg_installed/vcpkg/status` and the SPDX document of each port on
2026-09-19. "In the binary" means the static engine build, which is the only
thing that could ship today.

| Port | Version | Licence, as the port declares it | In the binary | What it asks of a binary release |
| --- | --- | --- | --- | --- |
| rtlsdr | 2.0.3, `osmocom/rtl-sdr` at `797f814`, Revenant's overlay port | GPL-2.0-or-later | yes, static | Corresponding Source, including the overlay's four diffs; licence text; notices |
| libusb | 1.0.29 | LGPL-2.1-or-later | yes, static, pulled in by rtlsdr | section 6: notice, a copy of the LGPL, and one of 6a to 6e. 6d, decided 2026-09-20 |
| pthreads (pthreads4w) | 3.0.0 | Apache-2.0 | yes, static, pulled in by rtlsdr | section 4: retain the copyright, patent, trademark and attribution notices, and reproduce upstream's `NOTICE` file if it has one |
| capnproto | 1.4.0 | MIT | yes | the copyright notice and the permission notice |
| zlib | 1.3.1 | Zlib | yes, pulled in by capnproto | nothing on a binary; the notice requirement binds source distributions |
| vulkan-memory-allocator | 3.3.0 | MIT | yes, header only | the copyright notice and the permission notice |
| catch2 | 3.13.0 | BSL-1.0 | no, tests only | nothing: BSL-1.0 exempts machine-executable object code by its own terms |
| pkgconf | 2.5.1 | ISC-style text, no SPDX declared | no, build tool | nothing |
| vcpkg-cmake and the other helper ports | various | MIT, one Apache-2.0 | no, build tooling | nothing |

Vulkan comes from the system SDK rather than vcpkg: `find_package(Vulkan)`
resolves to `vulkan-1.lib`, an import library for the loader that arrives with
the graphics driver. Whether that loader is a System Library under GPL-3.0
section 1 does not need deciding, because the loader is Apache-2.0 and so is
compatible either way.

**Qt, which the engine does not link and the UI does.** `ui/` is a separate
CMake project against the dynamic triplet, requiring Qt6 6.8 or newer, and it
links `Qt6::Core`, `Qt6::Gui`, `Qt6::Quick` and `Qt6::QuickControls2`. The open
source Qt is LGPL-3.0. Three things follow. This sentence used to say none of
them is settled because nothing packages the UI yet; `scripts/stage-payload.ps1`
has packaged it since 2026-09-21, and how each of the three is met is at the
end of the list below it.

- The dynamic link is the LGPL-3.0 section 4d(1) route, and DLLs beside the
  executable satisfy it without a relink package, which is the position this
  document originally wanted for libusb. That is a property of the deployment
  layout rather than of the licence, so a static Qt build, or any packaging that
  stops a replaced `Qt6Core.dll` being picked up, gives it away.
- Section 4a's notice requirement and the licence copies apply regardless of
  which of 4d(0) and 4d(1) is used. A Qt application ships the LGPL-3.0 text
  and the GPL-3.0 text it references.
- Qt bundles third-party code with its own attribution requirements, Harfbuzz,
  FreeType and PCRE2 among them. `windeployqt` copies the binaries and does not
  generate that attribution, so a notices file built only from the vcpkg tree
  would miss the entire Qt subtree.

As packaged on 2026-09-22: Qt ships as DLLs beside `revenant-ui.exe`, so the
first holds as long as that layout does. `licenses/LGPL-3.0.txt` and
`LICENSE.txt` are payload members, which is the second.
`THIRD-PARTY-NOTICES-client.txt` is generated from Qt's own SPDX documents and
attributes every bundled component in the four modules shipped, which is the
third. docs/packaging.md has the detail.

### What is actually still open

The relink obligation is not the open item. These are.

1. **Nothing has been distributed, so nothing is in breach.** The distinction
   the licence section above draws holds: these obligations attach to handing a
   binary to somebody. Today the repository ships source and no binaries. The
   deadline is the first release, not now.
2. **The Qt and FFmpeg sources.** The client payload conveys Qt 6.8.3 and
   FFmpeg 7.1 as DLLs, and both LGPLs ask for their source to be offered with
   the object code. The corresponding-source archive does not carry either,
   and the notices point at where each is published upstream, which is the
   weaker promise the section below describes. Undecided, and due at the first
   tag.
3. **`tuner_fc2580.c` has no licence notice.** Found 2026-09-23 while reading
   the notices at `797f814`, and present at v2.0.2 as well. Its header says it
   was taken from a Terratec kernel driver and names no licence, so the file's
   terms are whatever `COPYING` at the root of librtlsdr gives it, which is
   the plain version 2 text, and possibly whatever the driver it came from
   carried. Whether that reads as "any version" under GPL-2.0 section 9 or as
   2.0-only is exactly the question this document says a GPL-2.0-only file
   raises for a GPL-3.0 work. It drives the FC2580 tuner, which the dongle on
   the desk does not have. The owner decided on 2026-09-23 to leave it as it
   is: the file stays in the build, read under librtlsdr's `COPYING` like the
   rest of the library, and the question is an accepted risk rather than an
   open one. Taking it out would cost nothing but FC2580 support, since the
   library is already built from `vcpkg-overlays/rtlsdr/` with patches of our
   own, and that is the way out if the reading is ever challenged.

WHAT THIS LIST USED TO SAY. Two of its items were "There is no notices file"
and "There is no published Corresponding Source for the dependencies as
built", and a third said the UI's Qt obligations were unexamined, with a
packaging step that did not exist yet. On 2026-09-22 `scripts/generate_notices.py`
began writing a notices file per program into the payload, and
`scripts/corresponding_source.py` began building the archive the `release` job
publishes beside the installer, with libusb's upstream source, rtlsdr's
upstream source and its port's three patches, and Revenant's own. The Qt and
FFmpeg sources above were missing from the list the whole time.

### What a release has to carry

Concretely, so that the first release is not the place this gets worked out.

**A notices file, generated rather than written.** Everything it needs is
already on disk: `vcpkg_installed/x64-windows-static/share/<port>/copyright`
holds each port's licence text verbatim, and `vcpkg.spdx.json` beside it holds
the name, the version and the declared SPDX identifier. A script over that
directory produces a file that stays correct when a version moves, which a
hand-written one does not. It carries, at minimum: each linked port with its
version and identifier, the verbatim LGPL-2.1 text, the verbatim GPL-3.0 text,
and the section 6 notice naming libusb specifically as a library used in the
work and covered by the LGPL.

**Where it lives.** The root or `docs/`, not `core/`, `tools/` or `ui/`. The CI
guard greps those three directories for any SPDX identifier other than this
project's own, so a notices file listing `MIT` and `Apache-2.0` as bare
identifiers would fail the build if it landed in one of them. That is the guard
working as designed and it is a cheap mistake to make once.

**The source offer, as a release artefact rather than a promise.** GPL-3.0
section 6d and LGPL-2.1 section 6d both accept equivalent access from the same
place. A release carrying the binary, Revenant's source archive and the resolved
dependency sources satisfies both in one act and avoids the three-year written
offer entirely. The dependency sources are the piece that needs a build step:
`vcpkg export` produces exactly this, and the alternative is naming the upstream
tags and the vcpkg registry commit in the notices file and relying on those
remaining reachable, which is a weaker promise made about somebody else's
hosting.

**In the shipped artefact.** The notices file is a payload member beside the
binary, not a link. `installer.toml` is the Forge configuration and
`scripts/stage-payload.ps1` assembles what it packs; docs/packaging.md says what
the container carries and what it does not yet.

This paragraph used to say "`res/revenant.rc.in` and `signing/` exist and there
is no `installer.toml` yet, so the Forge packaging is unwritten." That stopped
being true on 2026-09-21, when the installer configuration and the package and
release jobs landed, and the sentence went on telling a reader there was nothing
to check.

### The relink question is closed, 2026-09-20

Archon's decision, recorded here because the analysis above is long enough
that a reader arriving at the end of it should not have to work out which way
it went.

**The route is 6d.** A release offers the binary and the corresponding
sources from the same designated place, and that one act satisfies LGPL-2.1
section 6d and GPL-3.0 section 6d together. Nothing turns on whether source
is an acceptable form under 6a, so the "and/or source code" reading above is
corroboration rather than the thing being relied on. What the release carries
is libusb v1.0.29 upstream source, complete because the vcpkg port applies no
patches, and the rtlsdr port's `dependencies.diff`, `library-linkage.diff`
and `tools.diff`, which the GPL-3.0 section 1 obligation already requires
whatever the LGPL asks. Since 2026-09-23 that is four rtlsdr diffs against
`797f814` rather than three against v2.0.2; see the paragraph dated 2026-09-23
under the decision above.

**The DLL was considered and rejected.** Shipping libusb beside the
executable would take the 6b route and discharge the clause with no release
artefact at all. It costs the single self-contained signed binary the static
triplet exists to produce, and that binary is worth more than the file it
would save.

Nobody who worked on this is a lawyer and neither is Archon. What the analysis
above is good for is that each licence's text was read against what this
project actually links, with the versions and the link line checked rather
than remembered, so somebody qualified can check the decision without starting
from nothing.

Two things from this section remain unconfirmed and neither blocks a release
on the relink question:

1. That the Apache-2.0 finding is right, and therefore that the "or later" in
   librtlsdr's grant is what makes the current engine link line lawful and not
   only what keeps Qt reachable.
2. That the open points above are the full list, since the purpose of
   doing this was to find what is not satisfied rather than to confirm what is.

## Disclosure log

Every exposure gets recorded here, with what was seen, when, and what was done
about it. The log stays useful after the licence change, for the same reason it
was useful before: it is the record of what the people writing this project
actually read. Seven entries, all self-reported by the person who did it,
which is the behaviour the practice needs to keep producing.

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

**2026-09-19, licence audit of the installed dependency tree.** The relink
analysis above rests on reading files from
`build/ci/vcpkg_installed/x64-windows-static`, so what was read is recorded
here rather than left implicit in a citation.

Read in full: every `share/<port>/copyright` file, which is licence text; every
`share/<port>/vcpkg.spdx.json`, which is package metadata; `vcpkg/status`;
`share/rtlsdr/rtlsdrTargets.cmake` and the two `vcpkg_abi_info.txt` files,
which are build metadata. None of that is expression of an algorithm.

Read in part: the licence notice at the head of `include/rtl-sdr.h` and
`include/libusb-1.0/libusb.h`, which is what this document instructs a reader
to check before distributing, plus the few lines past each notice that a
line-range read picks up. In `rtl-sdr.h` that was the include guard and the
`extern "C"` opening; in `libusb.h` an MSVC warning pragma. No function
declaration, no constant and no implementation file from either project.

Resolution: nothing to resolve. No file in this tree claims clean-room
provenance for anything libusb or librtlsdr does, because Revenant links both
rather than reimplementing either. The entry exists because the analysis above
cites those two headers as its evidence, and a reader checking that citation
should be able to see exactly how far the read went.

**2026-09-23, M17 specification's embedded listings.** The M17 decoder was
written from the M17 Protocol Specification Part I, version 2.0.4 of 21
January 2026. The document's Licenses page puts its prose under the GNU Free
Documentation License 1.3 or later and the software listings printed inside it
under GPL-2.0-or-later. While reading the document from a text dump, the
session writing the decoder saw three things. It read the PRBS9 generator and
synchroniser listings of Appendix G, figures G.3 to G.5, in full, before
noticing their licence. The last line of the Appendix A.5 decoder example, a
closing brace and a return, showed above Appendix B. The first lines of the
MATLAB snippet under Appendix D's Golay matrix appeared in a search for
section headings.

Resolution: nothing in `core/decode/m17.cpp` implements Appendix G. BERT
frames are recognised by their sync burst and left undecoded. The PRBS9
receiver is left to someone who has not read those listings, and
`docs/modes.md` says so. The Golay code is built from Appendix D's generator
polynomial, which the appendix prose states, and is checked against the
printed matrix, not against the snippet. The Appendix A.5 fragment carries
nothing to reproduce. `core/decode/m17.h` records the same exposure in its
CLEAN ROOM section, next to the code it concerns.

**2026-09-23, librtlsdr's and libusb's cancel paths.** The engine crashed
inside libusb after librtlsdr freed transfers still in flight, and the owner
approved trying other builds of both and patching librtlsdr. Fixing a library
means reading it, which the policy above permits for a library this project
links and patches. What was read: in librtlsdr at `797f814`, the transfer
callback, the async buffer allocation and release, `rtlsdr_read_async`,
`rtlsdr_cancel_async`, `rtlsdr_close`, `rtlsdr_reset_buffer`, the device
structure, and the whole diff from v2.0.2, which takes in the Blog V4 Lite
changes to `tuner_r82xx.c`; in libusb, `libusb_cancel_transfer` at v1.0.29,
the ChangeLog and the core and Windows backend commits up to v1.0.30.
`docs/rtlsdr-provenance.md` has the list in full.

Resolution: the patch that came of it, `cancel-waits-for-transfers.diff`, is
librtlsdr's code modified, carried in its port under `vcpkg-overlays/rtlsdr/`
and distributed under librtlsdr's licence, which is the patch exception the
policy now names. `tools/rtlsdr-cancel-trial` calls `rtl-sdr.h` and nothing
else. Nothing under `core/`, `tools/` or `ui/` implements what was read, and
the reader wrote no clean-room component touching USB transfers.

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
