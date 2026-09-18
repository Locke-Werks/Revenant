# Clean room

## The rule

Every line of Revenant, including all device backends and all decoders, is
written from published specifications and datasheets only.

No GPL-licensed source is read, ported, linked or consulted while implementing
a corresponding component. Not for a register number, not for "just to see how
they handle the reset", not for ten minutes with the tab closed afterwards.

This applies to everyone who commits here, and it applies from the first commit
rather than from the first release, because the thing it protects cannot be
restored once it is gone.

## Why it is not optional here

The obvious host libraries for the hardware this project targets are copyleft.
Verified on 2026-09-18 by reading the licence file in each repository:

| Project | Licence | Verified from |
| --- | --- | --- |
| `osmocom/rtl-sdr` (librtlsdr) | GPL-2.0 | `COPYING` at the repository root |
| `greatscottgadgets/hackrf` (libhackrf) | GPL-2.0 | `COPYING` at the repository root |
| `pothosware/SoapySDR` | Boost Software License 1.0 | `LICENSE_1_0.txt` |
| `libusb/libusb` | LGPL-2.1 | `COPYING` at the repository root |

SoapySDR itself is permissive and is not the problem. Its device modules are:
`SoapyHackRF` carries an MIT licence on its own source and then links
`libhackrf`, and the RTL module links `librtlsdr` the same way. A permissive
wrapper around a GPL library is still a GPL obligation on the binary that ships
the combination, so routing through SoapySDR is not a way around anything. It
moves the licence one link down the chain and leaves it there.

The chosen route is a clean-room implementation over libusb, which is LGPL-2.1
and linked dynamically.

The reason for the care is asymmetry. Copyleft, once inherited, cannot be taken
back out: there is no rewrite that launders a file somebody ported, and the
remedy for "we read their driver" is that a different person implements that
component from scratch, having never seen it. Revenant's own licence is
deliberately still an open question, and it stays open only while nothing in the
tree has already answered it.

This is also why the document exists in this shape. If the licensing position is
ever questioned, the answer has to be a practice with evidence in the
repository, not a claim made after the fact.

## What is allowed

- Published specifications and standards documents.
- Manufacturer datasheets, register descriptions, application notes and product
  documentation.
- Your own measurements: a logic analyser on the bus, a USB capture, a spectrum
  observation of the device's own output.
- Permissively licensed source, where the licence is recorded in the commit and
  the attribution requirement is honoured. MIT, BSD, Apache-2.0, Boost. Check,
  do not assume from the ecosystem it sits in.
- Reading a GPL project's documentation, issue tracker or protocol write-up,
  where that text is presented as documentation rather than as source. Record it
  as the source in the provenance comment like any other document.

## What is not

- Reading GPL or AGPL source while implementing the corresponding component.
- Porting, transliterating, or "reimplementing from memory" after reading.
- Vendoring a GPL file into the tree, in any directory, including tests.
- Linking a GPL library, statically or dynamically.
- Copying a table of magic values out of a driver. A single value read off a
  datasheet is a fact. A sequence somebody selected and ordered to bring a chip
  up is their expression of it, and a register initialisation table is exactly
  that.
- Asking a model to reproduce, summarise or "explain what this driver does" for
  a component being implemented. The output of that is downstream of the source.

If you have already read the source for something you are about to write, say so
and hand that component to somebody who has not. That is an ordinary thing to
happen and a cheap thing to fix. It becomes expensive only after it is committed
without being mentioned.

## The practice

The rule is worth nothing without the practice, because a claim about how code
was written can only be tested against what the code shows.

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

The point is that a reader can tell measurement from documentation at a glance.
An observation dressed up to look like a citation is worse than no citation,
because it defeats the review that would have caught it.

**Provenance is a review item.** The reviewer checks that constants carry
citations and that the citations are to documents, on every pull request that
touches a backend or a decoder. This is part of the review, not a separate pass
that gets skipped when the change looks small.

**CI greps for vendored licence text.** `.github/workflows/ci.yml` fails the
build on GPL licence text or an SPDX GPL identifier anywhere under `core/`,
`tools/` or `ui/`. It excludes `docs/`, which is why this document can name the
licences it is avoiding. Do not remove that exclusion, and do not extend it to
the source directories.

The grep is a backstop for the crude case, someone pasting a file in. It cannot
detect a ported algorithm, which is what the citation practice and the review
are for. Neither substitutes for the other.

## The RTL-SDR v3 backend

The first backend, landing in the M0.7 spike. It is built from these documents
and nothing else:

1. **Realtek RTL2832U datasheet.** "RTL2832U DVB-T COFDM Demodulator + USB 2.0
   Interface", Realtek Semiconductor Corp. Covers the USB interface, the
   endpoint layout, the bulk transfer path carrying sample data, the I2C
   repeater through which the tuner is reached, and the demodulator register
   banks.
2. **Rafael Micro R820T2 datasheet and register description.** "R820T2 High
   Performance Low Power Advanced Digital TV Silicon Tuner". Covers the PLL,
   the mixer, the IF path and the gain stages.
3. **USB 2.0 specification**, chapter 5 for transfer types and chapter 9 for the
   device framework. Control transfer layout and bulk endpoint behaviour come
   from here rather than from an example.
4. **libusb-1.0 API documentation**, for the host side: asynchronous transfer
   submission, the event handling loop, and the ownership rules for a transfer
   between submission and callback.
5. **RTL-SDR Blog V3 product documentation**, for what the v3 board adds over a
   generic dongle: the temperature compensated oscillator, the software
   switchable bias tee on a demodulator GPIO line, the direct sampling input,
   and the expansion pads.

Two honest notes about that list.

The R820T2 register map is only partially public. The datasheet describes the
functional blocks; it does not enumerate every register and bit. Anything needed
beyond what the datasheet states is derived by measurement against the device
on the desk and is labelled as measured, per the practice above. It is not taken
from a driver, and a commit that cannot say which of the two it is does not
merge.

This document deliberately states no register numbers. A register number
asserted here without a citation would be the exact provenance failure the
practice exists to prevent, and a document about clean-room discipline is a poor
place to start one.

**The device is already bound to WinUSB.** The dongle on the development machine
was bound by libwdi, so libusb can open it with no driver work, no Zadig step in
the build instructions, and no INF to ship. Any machine that has not had that
done needs it once. The binding is a Windows driver association, not a code
dependency, and nothing about it touches the licence position.

**What M0 proved, and what it deliberately did not.** `tools/devicespike` opens
the dongle, reads its descriptors, finds the bulk IN endpoint that will carry IQ
at M1, and claims and releases interface 0. On the development machine it
reports `RTL2838UHIDIR` on endpoint 0x81 with a 512-byte maximum packet. That is
the whole device path short of the tuner, verified against real hardware before
anything is built on it.

It stops there on purpose. The spike reads no tuner register and writes no
vendor control transfer, because the RTL2832U control protocol and the R820T2
register map are both in librtlsdr as well as in their datasheets, and code
written from recollection cannot be distinguished afterwards from code written
from the GPL source. That distinction is the entire value of the claim. Register
access waits for M1 and for the datasheets, so that each write can cite a
document and a table. Proving the USB path works needs none of that: descriptors,
configurations, interfaces and endpoints are specified by USB 2.0 and by libusb's
own public API, neither of which carries any provenance question.

## libusb, and one open item

libusb is LGPL-2.1. Linking it dynamically, unmodified, keeps the obligation
where the LGPL puts it: the user must be able to replace the library, which a
DLL beside the executable satisfies on its own.

Open, and to be decided before M0.7 ships anything: the repository's presets set
`VCPKG_TARGET_TRIPLET` to `x64-windows-static`, which would build libusb as a
static library and pull it into the executable. A static link to an LGPL library
is permitted, but it carries the relinking obligation, which means shipping
object files or an equivalent mechanism for the user to relink against their own
libusb. That is a real obligation on the release pipeline, not a formality.

The two ways out are to build libusb as a DLL while the rest of the tree stays
static, through a per-port triplet for that package, or to accept the relink
obligation and build it into the release process. This is recorded here rather
than settled quietly at build time, because the triplet is the kind of setting
that gets changed for an unrelated reason by someone who does not know a licence
depends on it.

## If a question comes up later

The evidence that this practice was followed is in the repository, not in this
file:

- provenance comments on constants, in the commit that introduced them
- specification references in decoder headers
- commit history showing the specification cited before the code appeared
- the CI guard, in every run since the first commit
- this document, dated and versioned alongside the code it describes

The claim is only ever as good as those four. Keep them.
