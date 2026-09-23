# RTL-SDR provenance finding

Research only. No code was written and no build file was changed.

The question: can an RTL-SDR v3 backend (RTL2832U demodulator, R820T2 tuner,
USB 0bda:2838) be implemented over libusb with every register write traceable
to a published document, as `docs/clean-room.md` requires?

**Answer: no.** The tuner can. The demodulator's USB protocol can, up to the
point where the device is asked to deliver IQ instead of a DVB-T transport
stream, and that step is not documented by Realtek anywhere. A backend written
from the datasheets alone would configure the device correctly and never
receive a sample.

Because finding 2 is negative, `docs/rtl2832u-protocol.md` was not written. See
the end of finding 2 for why writing a partial one would be worse than writing
none.

All fetches and licence checks below were performed on 2026-09-18.

The last section, "The librtlsdr the engine links", is later and different in
kind. It is dated 2026-09-23, it changed the build, and it read librtlsdr's
and libusb's source, which `docs/clean-room.md` permits for a library Revenant
links and patches. It carries its own disclosure.

---

## Finding 1: the R820T2 register map. Positive, with gaps.

**Document.** "R820T2 Register Description", Rafael Microelectronics, Inc.,
11 pages. The PDF carries no revision number on its pages; the document
properties give the source file as `R820T2_Register_VER1.3-20141014.doc`, so
**version 1.3, 2014-10-14**. Cover page reads "CONFIDENTIAL (c) 2011 by Rafael
Microelectronics, Inc."; body pages read "CONFIDENTIAL (c) 2012".

**Where to obtain it.**
`https://www.rtl-sdr.com/wp-content/uploads/2016/12/R820T2_Register_Description.pdf`
(HTTP 200, 228,847 bytes, PDF 1.3, 11 pages). Mirrored at radiolocman.com,
document id 186047.

**Release provenance.** rtl-sdr.com reports that Luigi Tarenga emailed Rafael
Micro requesting further documentation for driver work, and that Rafael Micro
"decided to publicly release the Register description document". The article
records no formal grant of distribution rights, and the document's own pages
still read CONFIDENTIAL. Published in practice, confidential on its face.

**What it covers, by section.**

| Section | Contents |
| --- | --- |
| 1.1, Table 1-1 | I2C interface: 7-bit slave address, separate write and read addresses, 100 kHz standard bus rate and up to 400 kHz |
| Figures 1-1 to 1-4 | Write mode and read mode sequences. Reads start at register 0 and run upward; the data is transmitted LSB first, so read bytes arrive bit-reversed. Register 0 returns a fixed value usable as a presence check |
| 1.2, Table 1-2 | Register matrix: all 32 registers, every bit position, including the fixed bits that carry no symbol name |
| 1.3, Table 1-3 | Register index and description: per-bit semantics with ranges and units where they exist |

Table 1-2 is the reason this document is usable for a from-scratch
implementation. It gives the fixed and reset values for the bits that have no
functional name, so an initialisation can be assembled from the document rather
than copied from somebody's table.

**Against the four functions the question named:**

- **PLL and VCO programming: covered.** Table 1-3 gives the reference divider,
  the PLL-to-mixer divider selection, the crystal cap setting, the PLL LDO
  settings, the auto-tune clock rate, and the integer and fractional divider
  inputs, with the arithmetic printed: `Nint = 4*Ni2c + Si2c + 13`,
  `Ndiv = (Nint + Nfra)*2`, and `Nfra` as a binary expansion of the sigma-delta
  input word.
- **LNA, mixer and VGA gain: covered.** Manual and automatic mode bits and
  manual codes for each of the three stages, with the VGA range given in dB and
  a stated step size, plus the AGC power-detector voltage thresholds in volts
  and the detector take-off points.
- **Filter bandwidth: partly covered.** The coarse bandwidth field, the fine
  tuning code, the high-pass corner and the filter extension bit are all
  described, but only qualitatively ("widest" through "narrowest", "highest"
  through "lowest corner"). **No bandwidth in hertz is given for any code.**
  Mapping a requested bandwidth onto a code needs measurement.
- **Standby and init sequence: not covered.** Table 1-3 gives a power control
  bit per block (loop-through, LNA, mixer, mixer buffer, IF filter, channel
  filter, VGA, RF filter, the two LDOs and the sigma-delta). There is no
  power-up order, no settling time and no reset procedure. The document is a
  register description and does not claim to be a programming guide.

**Further gaps in that document:**

- Registers 0 through 4 are stated to be reserved for internal use.
- No VCO frequency range. The divider selection field is described by its
  ratios, so choosing one for a given RF requires knowing the VCO's legal
  range, which the document does not state.
- No IF output frequency. Nothing names the frequency the tuner presents to the
  demodulator, so the demodulator's IF setting cannot be derived from this
  document.
- Image rejection: the gain and phase adjustment fields are given with their
  ranges, and no calibration procedure.
- Three bit-width disagreements between Table 1-2 and Table 1-3: the
  PLL-to-mixer divider selection, the LNA AGC threshold field, and the
  power-detector-3 take-off field each appear with one width in the matrix and
  another in the description. Each has to be settled on the device.

---

## Finding 2: the RTL2832U USB protocol. The deciding finding, and it is negative.

**Document.** "RTL2832U DVB-T COFDM Demodulator + USB 2.0 Datasheet", Realtek
Semiconductor Corp., Track ID JATR-2265-11, **Rev. 1.4, release date
2010-11-01**, 53 pages.

**Where to obtain it.**
`https://homepages.uni-regensburg.de/~erc24492/SDR/Data_rtl2832u.pdf`
(HTTP 200, 805,888 bytes, PDF 1.5, 53 pages). Also mirrored at theretroweb.com
and the datasheet aggregators.

**Status of the copy.** Every page is overprinted "Realtek CONFIDENTIAL" and
"for V4L", and the cover reads "CONFIDENTIAL: Development Partners Only". This
is an NDA document that reached the public through the V4L kernel developers.
Realtek's own product page for the RTL2832U
(`https://www.realtek.com/Product/Index?id=615`, read 2026-09-18) publishes a
general description and a feature list and offers no datasheet, no register
documentation and no SDK.

**Practical note for whoever implements from it.** The confidentiality
watermark is a text layer, not an image. `pdftotext` interleaves its characters
into the register tables and scrambles the column order; at least three tables
come out unusable. Read the register tables from rendered page images. Every
claim below was checked against a rendered page, not the text layer.

### What the datasheet does specify

| Thing | Where |
| --- | --- |
| Vendor command layout: twelve commands with `bmRequestType`, `wValue`, `wIndex` and `wLength` | Section 11.2, Table 27 |
| `bmRequestType` 0xC0 for reads and 0x40 for writes | Section 11.2, Table 27 (this is also the USB 2.0 vendor/device encoding) |
| Block addressing: `wIndex` nibble [2] selects the block (demodulator, USB, system, tuner, ROM code, IR, standard I2C), nibble [1] selects get or set, nibble [0] selects the demodulator page 0 to 4 | Section 11.2, Table 28, "Definition of `wIndex`" |
| Register offset in `wValue`, and `(base << 8) + offset` for the USB and system blocks | Section 11.2, Table 27 |
| I2C repeater for the tuner: what it is for, its register, and that it is not cleared automatically by a stop condition | Section 8.3 and Table 3 |
| Tuner register access commands and their `wIndex` values, plus the note that the command is issued in the data stage and depends on the tuner | Section 11.2, Table 27 and the note under Table 28 |
| Sample rate: the resampler ratio register, its page, offsets, field, default, the equation `floor(f_crystal / f_symbol * 4194304)`, and four worked examples | Section 9.4, Table 7 |
| IF frequency: the DDC IF register, the equation `-floor(f_IF_D / f_crystal * 4194304)`, and five worked examples including the zero-IF case | Section 9.3, Table 6 |
| Zero-IF mode: the ADC I and Q enables and the baseband input enable | Section 9.1, Table 4 |
| DC cancellation and IQ compensation enables and the estimated-mismatch readbacks | Section 9.2, Table 5 |
| Spectrum inversion and adjacent channel rejection | Section 9.3, Table 6 |
| Demodulator control: PLL enable, ADC_I and ADC_Q enable, hardware reset | Section 10.1, Table 15 |
| GPIO output value, input value, output enable, direction, pad configuration, and the default function of each of the eight pins | Section 10.2, Tables 16 to 18 |
| Bulk endpoint: endpoint enable, transfer type, direction, max packet size, FIFO size, FIFO reset and flush, drop counter, DMA enable, SIE reset, full-packet mode | Section 11.4, Tables 30 to 34 |
| IF frequencies the part supports (36.125 MHz, 4.57 MHz, zero-IF), the 28.8 MHz crystal, and FM/DAB/DAB+ support | Realtek's public product page, read 2026-09-18 |

That is a substantial amount of citable material, and it is enough to program
the tuning arithmetic, the tuner path and the endpoint.

### What the datasheet does not specify

**1. The `bRequest` byte is not given.** In Table 27 the `bRequest` column
contains the literal character `x` for all twelve commands. Verified on the
rendered page at document page 36 and again at page 37, not inferred from the
text layer.

The document uses `x` elsewhere to mean don't-care: Table 28 marks nibble [3]
as `x`, and marks the undefined block codes as `x`. Read consistently, that
makes `bRequest` a don't-care, with `wIndex` carrying all of the
discrimination, which is also what the completeness of Table 28 implies. That
is an **inference from the document's own notation, not a citation**, and it is
settled in an afternoon against the device on the desk. Record it as an
inference until it is measured, and label the measurement as measurement.

**2. There is no raw IQ output mode in this document.** The full 53 pages were
searched for "raw", "bypass", "test mode", "IQ output", "output mode",
"direct" and "8-bit". Nothing describes an alternative to the transport stream.

The documented USB data path is DVB-T only, and the endpoint registers say so
explicitly: the FIFO is configured in 188-byte blocks, the flush bit discards
"the oldest TS packet (a 188 bytes block)", the valid bit validates one, and
the drop counter counts them. The documented signal path runs ADC, DDC,
resampler, guard interval removal, FFT, synchronisation, channel estimation,
TPS decode, equalisation, de-interleave, FEC, descrambler, PID filter, then the
endpoint FIFO. Nothing in the document taps the DDC output to the endpoint.

That mode is the entire reason this part is used as an SDR, and Realtek did not
document it. Independent accounts agree, both on the fact and on where the
knowledge came from:

- The GNSS-SDR project's tutorial on operating with an RTL2832U dongle
  describes "the V4L/DVB kernel developer Antti Palosaari, who discovered an
  **undocumented** operation mode for some USB DVB-T dongles based on the
  Realtek RTL2832U chipset", found in February 2012 by sniffing the device.
- rtl-sdr.com's own background page credits Antti Palosaari, Eric Fry and
  Osmocom, in particular Steve Markgraf, with discovering the mode and writing
  the driver that exploits it.

That driver is librtlsdr, GPL-2.0, already recorded in `docs/clean-room.md`. It
was not opened during this work.

So the sequence of register writes that produces IQ is not a published fact. It
exists as one project's expression, in a copyleft library this project may not
read, and as whatever an individual can rediscover on their own hardware.

**3. The demodulator page map is by function, not exhaustive.** Section 9
documents the registers belonging to each DVB-T block it describes. There is no
complete page 0 and page 1 map. Any offset the SDR path needs outside a
documented function is absent entirely.

**4. The I2C repeater bit's location is stated twice and inconsistently.**
Table 3 places it on page 1, offset 0x01, bit [3]. Note 2 under Table 27 says
"the 7th bit of the first byte of the demodulator's page1". Those cannot both
be right, and neither reading of "7th bit" yields bit [3]. The offsets are
quoted here because the disagreement itself is the finding. One of the two has
to be settled on the device.

**5. Whether the tuner commands use `wValue` is ambiguous.** Table 27 gives
`(OffsetAdd) << 8 + IICAdd` as the `wValue` for both tuner commands. The note
under Table 28 says the command is issued within the data stage, "ignoring the
`wValue` field value". The document contradicts itself in the space of one
page.

**6. The RTL-SDR Blog V3's bias tee GPIO is not identified.** The V3 user guide
on rtl-sdr.com documents the feature ("a 4.5V bias tee that can be toggled
entirely in software", up to 180 mA) and the direct sampling input (the
Q branch, roughly 500 kHz to 28.8 MHz), and names no GPIO pin. The datasheet
documents the GPIO block and knows nothing about this board. The pin number
needs the board schematic or measurement.

### Why capturing the USB traffic is not the workaround it looks like

`docs/clean-room.md` permits own measurement and names a USB capture
specifically. It also rules out the thing a capture of librtlsdr would produce:
"A sequence somebody selected and ordered to bring a chip up is their
expression of it, and a register initialisation table is exactly that."

Capturing librtlsdr driving the device and transcribing the register sequence
copies that expression. Passing it through a protocol analyser rather than a
text editor changes how hard it is to notice, and nothing else. Do not do it,
and do not accept it in review.

Capturing **Realtek's own Windows DVB-T driver** is different for copyleft
purposes, because that driver is proprietary rather than GPL, and no copyleft
attaches to observing it. It would settle the `bRequest` byte, the repeater bit
and the `wValue` ambiguity. It would not yield the IQ mode, because that mode
is not part of DVB-T operation and the vendor driver never enters it.

The genuinely clean route to the IQ mode is exploratory: write to undocumented
offsets on the device on the desk and watch the bulk endpoint. That is real
clean-room work and it is defensible. It is also open-ended research with no
schedule attached, and it is the wrong thing to be doing on a night when the
goal is a running application.

### Why no protocol specification was written

`docs/rtl2832u-protocol.md` does not exist after this work. Roughly four
fifths of it could be written with a table citation on every line, and the
remaining fifth is the part that makes the device produce samples. A document
that specifies the transport in full detail and goes quiet exactly where the
evidence stops would read as complete to the next session and send someone into
a week of debugging a backend that was never going to work. The gap list at the
end of this document carries the same information without that risk.

---

## Finding 3: independent non-copyleft prior art. None exists for this hardware.

Every licence below was read from the project's own licence file or from
GitHub's licence metadata on 2026-09-18. No implementation source was read.

| Candidate | Licence, verified | Covers | Verdict |
| --- | --- | --- | --- |
| `jtarrio/radioreceiver` | Apache-2.0 (`LICENSE` read) | RTL2832U USB protocol and tuner, TypeScript over WebUSB | **Disqualified by its own README** |
| `google/radioreceiver` | Apache-2.0 | Same; jtarrio's is a fork of it | Same defect, inherited |
| `sandeepmistry/rtlsdrjs`, `@sdr.cool/rtlsdrjs` | permissive as published | Same lineage again, ports of radioreceiver | Same defect |
| `osmocom/rtl-sdr` (librtlsdr) | GPL-2.0, already in `clean-room.md` | Everything | Excluded. Not read |
| Linux `rtl28xxu` / `rtl2832` | GPL-2.0 | Everything | Excluded. Not read |
| BSD kernel drivers | none found | nothing | No native BSD driver exists. FreeBSD reaches this hardware through `webcamd`, which runs the Linux GPL DVB drivers in userspace rather than reimplementing them |
| Realtek vendor SDK | does not exist | nothing | The product page offers no datasheet, no register documentation and no SDK |

The radioreceiver disqualification is worth stating precisely, because it is
clean and it cost nothing. Its README says:

> Kudos and thanks to the [RTL-SDR project] for figuring out the magic numbers
> needed to drive the USB tuner.

That is the exact pattern `docs/clean-room.md` already records from the earlier
survey: a repository declaring a permissive licence while crediting a GPL
project for its register values. Apache-2.0 covers the authors' own expression.
It cannot relicense values taken from somebody else's GPL work. Disqualified
from the README, without reading a line of its source. Its two ancestors and
its JavaScript descendants inherit the same defect.

**Adjacent hardware, checked while here, because the answer changes per radio:**

| Project | Licence, verified | Notes |
| --- | --- | --- |
| `greatscottgadgets/hackrf` (libhackrf) | GPL-2.0 (`COPYING` read) | Not read beyond the licence |
| `airspy/airspyone_host` (libairspy, Airspy R2 and Mini) | **none.** No licence file in the repository; GitHub reports no licence; the README states "This file is part of AirSpy (based on HackRF project ...)" | Derived from a GPL-2.0 codebase with no licence of its own. Treat as GPL and unusable |
| `airspy/airspyhf` (Airspy HF+) | **BSD-3-Clause** (`LICENSE.BSD`, confirmed via GitHub's licence API) | Genuinely permissive and genuinely readable. Covers the HF+ only |
| `myriadrf/LimeSuite` | Apache-2.0 (`COPYING` read) | Covers LimeSDR |
| `analogdevicesinc/libiio` | LGPL-2.1 (`COPYING.txt` read) | Covers the ADALM-Pluto. The same licence posture already accepted for libusb |
| `pothosware/SoapySDRPlay3` | MIT (`LICENSE.txt` read) | MIT on its own source, and it links the SDRplay API, which is not copyleft. Unlike the HackRF and RTL Soapy modules, this wrapper carries no copyleft obligation down the chain |

So: no permissive prior art for the RTL2832U. Permissive and non-copyleft paths
to live RF do exist, for other radios.

---

## Finding 4: the alternatives.

### 4a. rtl_tcp as a separate process

Two questions, with different answers.

**Is the protocol documented independently of the GPL source? Not adequately.**
The Ubuntu man page for `rtl_tcp` describes what the program does and its
command-line options, not the wire format. The 2013 osmocom-sdr thread titled
"RTL_TCP Commands" is a user asking how to format a command, not a
specification. Third-party reference pages give the shape (a continuous stream
of 8-bit unsigned I and Q, and 5-byte commands of one command byte plus a
32-bit value) and then point back at the source for the command list. Command
byte meanings circulate in forum posts. Several forks have added their own
commands, so no single complete list exists. Enough is available in
documentation form to tune and set a rate. It is not a document with a
revision, a date and an author standing behind it, which is what the citation
practice asks for.

**Does running a GPL binary as a separate process keep the licensing position
intact? Yes.** This is the one place the answer is comfortable. Revenant would
not link librtlsdr, would not contain its code and would not ship it. The user
installs `rtl_tcp` themselves from their distribution or from an Osmocom
release. Revenant opens a TCP socket. Copyleft reaches code combined into one
program; two separately distributed processes exchanging a byte stream over a
socket are not that. Nothing in Revenant's tree becomes derivative, and the
clean-room rule is not engaged at all, because nobody reads any source.

**Cost, stated rather than hidden.** Revenant does not own the device, so no
coherent multi-dongle work, no direct control of the bias tee or the clock, and
no access to anything `rtl_tcp` does not expose. It inherits that program's
buffering and its overrun behaviour. It loses the clock and timestamp fidelity
the Source contract is built around: `ClockQuality` would report `Internal`
with an honest and fairly poor `accuracy_ns`, and sample-index authority
becomes a count of bytes received rather than a property of the device.

**It does satisfy the contract.** `FlowControl::Paced`, `SampleFormat::Cu8`
native, `seekable = false`, `length_samples = 0`, one `TuneRange`, a gain stage
list reflecting what the protocol exposes, and `preferred_block_samples` set
from the socket read size. The URI grammar in `core/source/registry.h` already
has room for it without a change to the grammar.

### 4b. The other radios

**SDRplay RSPdx. The best answer of the set, by a wide margin.**

- The vendor publishes a real specification: "Software Defined Radio API",
  SDRplay Limited, **revision 3.15, 10 May 2024**, 41 pages, at
  `https://sdrplay.com/wp-content/uploads/2026/04/SDRplay_API_Specification_v3.15.pdf`
  (HTTP 200, 723,382 bytes). It has a revision history going back to 2.x, and
  it documents every function, structure and enumeration, with a per-device
  header section for the RSPdx at section 2.9.
- Section 7, Legal Information, carries a three-clause BSD grant on the
  information, and then this:

  > SDRPlay modules use a Mirics chipset and software. The information supplied
  > hereunder is provided to you by SDRPlay under license from Mirics. Mirics
  > hereby grants you a perpetual, worldwide, royalty free license to use the
  > information herein for the purpose of designing software that utilizes
  > SDRPlay modules.

- The API itself is a closed binary the user installs. **Closed is not
  copyleft.** Calling a proprietary vendor library imposes nothing on Revenant's
  own licence and leaves every option in `clean-room.md` open: permissive,
  copyleft, dual and source-available all stay reachable.
- There is no clean-room problem here at all, because there is nothing being
  reverse engineered. No register writes, no provenance chain to defend, no
  disclosure risk, no citation practice to enforce on a backend. The
  specification's own example loads the library at runtime and frees it, so the
  dependency can be optional and absent at build time.
- Version 3.15 added a user-mode WinUSB driver, which matches how the RTL-SDR
  is already bound on this machine.
- Cost: a proprietary runtime the user installs, and licence terms limiting use
  to genuine SDRplay hardware. Against that, the RSPdx is a 14-bit receiver
  covering roughly 1 kHz to 2 GHz, which is a better instrument than the dongle
  independently of any licensing question.

**HackRF One. No better than the RTL-SDR, and arguably worse.**

- libhackrf is GPL-2.0, from `COPYING` at the repository root.
- The HackRF documentation at hackrf.readthedocs.io has no USB protocol
  reference. Its sections cover the hardware, firmware updates, software tools,
  gateware and Opera Cake. The wire protocol is not presented as documentation;
  it lives in the GPL host library and the firmware.
- Unlike the RTL2832U, there is no silicon vendor datasheet to fall back on,
  because the USB layer is Great Scott Gadgets' own design rather than a chip's
  published interface. There is no document to cite for anything. A clean-room
  route would be exploratory reverse engineering with no reference at all.

**Malachite / Malahit DSP. Trivially reachable, and close to useless here.**

- It presents IQ as a standard USB Audio Class recording device, around 192 kHz
  wide, alongside a serial CAT port for control. USB Audio Class is a published
  USB-IF specification and Windows exposes the device through WASAPI, so there
  is no vendor protocol to reverse engineer and no licence question of any kind.
- 192 kHz is the problem. Revenant exists to channelize a wide span on the GPU,
  and a 192 kHz input does not exercise what was built. The radio has also made
  every front-end decision before the samples arrive. A legitimate zero-risk
  source, and not a reason to write a backend.

### 4c. Anything else real

Three, and nothing invented to pad the list.

- **ADALM-Pluto** through libiio, LGPL-2.1. The same posture already accepted
  and documented for libusb: dynamic link, unmodified, user can replace it.
  Wideband, transmit as well as receive, around $230.
- **Airspy HF+ Discovery** through libairspyhf, BSD-3-Clause. Permissive,
  readable, reusable with attribution honoured. HF and low VHF only.
- **The file and synthetic sources that already work.** Not a radio, and it
  belongs on this list, because `core/source/source.h` was written so the
  offline path runs the identical graph. Nothing about live RF is required to
  have an application a person can run tonight.

---

## Recommendation

Put the RSPdx on the SDRplay API and do not write a native RTL2832U backend at
all: the vendor publishes a revisioned specification carrying a perpetual
royalty-free grant to use it, the library is closed rather than copyleft so it
imposes nothing on Revenant's licence, and there is no provenance chain to
defend because nothing is being reverse engineered. Tonight, build the
application on the file and synthetic sources that already pass 73 tests, and
add an `rtltcp://` backend if live RF is wanted before the RSPdx arrives, since
a separately installed `rtl_tcp` reached over a socket is the one RTL-SDR route
that touches neither the GPL nor the clean-room rule. Keep the dongle as a
hardware test target, and treat a native RTL2832U backend as open-ended
reverse engineering to be scheduled on its own merits, never as the thing
standing between the engine and its first live signal.

---

## What was actually done

The recommendation above was not taken. Revenant links librtlsdr and moved to
GPL-3.0-or-later to do it, which finding 2 is the evidence for: the datasheet
specifies the transport and no raw IQ mode, so there was nothing to write a
native backend from. No RSPdx was bought and no `rtltcp://` backend was
written. The section above is left standing because it is the reasoning as it
stood on 2026-09-18 and `docs/clean-room.md` cites it, but it is advice the
project declined and not a plan anyone should pick up. The "73 tests" figure
in it is a count from that day; the suite is at 203.

---

## What could not be sourced

Every item here is a gap in the published record, not a gap in the search.

**RTL2832U:**

1. The `bRequest` byte for all twelve vendor commands. Table 27 prints the
   literal `x`. The don't-care reading is an inference from the document's
   notation, not a citation.
2. **The register sequence that makes the device deliver raw IQ instead of a
   transport stream.** Absent from Rev 1.4, absent from every Realtek
   publication found, and reported as undocumented by independent third-party
   accounts. This is the blocking gap.
3. A complete page 0 and page 1 demodulator register map. Only function-by-
   function coverage exists.
4. The I2C repeater bit's location, unambiguously. Table 3 and Table 27 Note 2
   disagree.
5. Whether the tuner commands use or ignore `wValue`. Table 27 gives a formula;
   the note under Table 28 says the field is ignored.
6. The RTL-SDR Blog V3's bias tee GPIO number. The product documentation states
   the feature and not the pin.

**R820T2:**

7. Filter bandwidth in hertz for any coarse, fine or high-pass code. Described
   qualitatively only.
8. Power-up order, settling times and reset procedure. Absent by design.
9. The VCO frequency range, needed to choose the PLL-to-mixer divider.
10. The IF output frequency the tuner presents to the demodulator.
11. The image rejection calibration procedure. Adjustment ranges only.
12. Three bit-width disagreements between Table 1-2 and Table 1-3.

**rtl_tcp:**

13. A complete, authored protocol specification. Partial third-party
    descriptions only, and forks diverge on the command set.

---

## Provenance status of the two documents themselves

Separate from the technical findings, and not to be conflated with them.

Both documents are marked confidential on every page, which is a different
question from copyleft. Neither creates a copyleft problem.

- The **R820T2 Register Description** was released by Rafael Micro in response
  to a request for driver documentation, per rtl-sdr.com. The article records
  no formal grant, and the pages still read CONFIDENTIAL. Published in
  practice.
- The **RTL2832U datasheet** is stamped "CONFIDENTIAL: Development Partners
  Only" and overprinted "for V4L". It reached the public through a leak to
  kernel developers, and Realtek publishes no datasheet at all.

`docs/clean-room.md` lists "published specifications and datasheets" among
allowed sources. A leaked NDA datasheet is not straightforwardly that. If the
licensing position is challenged, "we worked from the register description the
manufacturer released on request" is a stronger sentence than "we worked from
the leaked Realtek NDA datasheet", and the two documents are not equal on this
axis. Decide that deliberately rather than by default. It is one more reason
the RSPdx path is cheaper: its specification is published by the vendor, on the
vendor's website, with a grant attached.

---

## Disclosure for the clean-room log

No GPL source was read during this work.

- Licences were verified by fetching licence files (`COPYING`, `LICENSE`,
  `LICENSE.txt`, `LICENSE.BSD`) and GitHub licence metadata only.
- A search result linked directly to a libhackrf source file. It was not
  opened.
- `jtarrio/radioreceiver` was disqualified from its README attribution line. No
  source was read.
- librtlsdr, libhackrf and the Linux kernel DVB drivers were not opened at all.
- The two datasheets, the SDRplay specification, vendor product pages, project
  READMEs and third-party articles are documentation, which
  `docs/clean-room.md` permits.

This section should be appended to the disclosure log in
`docs/clean-room.md`, alongside the 2026-09-18 prior-art survey it extends.
Two corrections to that document follow from this work, and neither is a
contradiction of it:

- `clean-room.md` states that the RTL2832U datasheet Rev 1.4 "covers the USB
  vendor command layout, the register block selector, the I2C repeater and
  master, the DDC, the resampler and the GPIO block, including the equations
  for the sample rate and IF frequency registers". That is accurate, and it
  understates nothing except by omission: the layout is covered with the
  `bRequest` byte left as a don't-care, and no part of the document describes
  the IQ output mode.
- `clean-room.md` estimates that what the two documents do not cover is "roughly
  one part in seven", to be filled by measurement. For the tuner that estimate
  holds. For the demodulator it does not, because the missing part is not a
  fraction of the register writes but the one step the rest of them exist to
  serve.

---

## The librtlsdr the engine links

Dated 2026-09-23. Owner-approved trial of other librtlsdr and libusb builds,
because the one the engine linked crashed it.

**What ships.** librtlsdr from `osmocom/rtl-sdr` at commit
`797f8143266d983c56d8f35d2d442527529dd8a5`, which was master that day and is
also the v2.0.3 tag, built by the overlay port in `vcpkg-overlays/rtlsdr/`
with four patches: the registry port's `dependencies.diff`, rebased by one
context line, its `library-linkage.diff` and `tools.diff` unchanged, and
Revenant's `cancel-waits-for-transfers.diff`. libusb stays at 1.0.29 from the
registry baseline, unpatched. `vcpkg-configuration.json` lists the overlay, so
every build of the engine, locally and in CI, gets this librtlsdr and no other.

### The fault

`rtlsdr_read_async` sometimes returns -5, `LIBUSB_ERROR_NOT_FOUND`, when it is
cancelled, and in that case it has freed a bulk transfer libusb still holds.
libusb then touches the freed memory: the RTL-SDR lane caught it in
`add_to_flying_list` (io.c) under the next control transfer and in
`windows_iocp_thread` (windows_common.c), under full page heap. Its record is
in `run_usb` in `core/source/rtlsdr_source.cpp`.

Why, from librtlsdr's source. Once a cancel is asked for, the loop in
`rtlsdr_read_async` makes passes over the transfers. In each pass it calls
`libusb_cancel_transfer` on every transfer whose `status` field is not
`LIBUSB_TRANSFER_CANCELLED`, and if no call in the pass succeeds it stops,
frees every transfer and returns the last call's result. Two things make that
exit wrong:

- `status` is only written when libusb hands a transfer back. A transfer
  cancelled in one pass whose completion has not been handled yet still shows
  its previous status, `LIBUSB_TRANSFER_COMPLETED`, so the next pass cancels it
  again.
- libusb 1.0.29's `libusb_cancel_transfer` answers `LIBUSB_ERROR_NOT_FOUND` for
  a transfer that is not in flight and equally for one it is already
  cancelling.

So a pass in which every remaining transfer is cancelling but not yet handed
back looks, to librtlsdr, exactly like a pass in which there is nothing left
to cancel. It frees them, returns -5, and the completions arrive for memory
that has gone. The callback also resubmits every completed transfer whatever
the state, which is why the lane counted 15 to 56 transfers completing after a
cancel.

### What changed between v2.0.2 and 797f814

Eleven commits, read from the repository's history. One touches the async
path: `65f0658`, "Fix application hang on USB transfer errors". It stops the
transfer callback calling `rtlsdr_cancel_async` itself when a transfer fails,
moves `rtlsdr_read_async` into its cancelling state when the device is lost,
and makes it return -1 in that case. The cancel loop's exit condition, the
thing above, is unchanged. The rest: RTL-SDR Blog V4 Lite support in
`librtlsdr.c` and `tuner_r82xx.c` (`0204c9c`), gated on that board's USB
strings; CMake's minimum-version range and a project-relative include path;
the version number; a CI script and Debian packaging. Nothing there was
expected to move the count, and the trial below could not tell master from
v2.0.2.

### What changed between libusb 1.0.29 and 1.0.30

1.0.30 was released 2026-05-17 and vcpkg's registry carries it, at a commit
newer than this project's baseline. `libusb_cancel_transfer` is the same
function in both, still answering NOT_FOUND for a transfer already being
cancelled. The Windows changes are hotplug support, RAW_IO in the WinUSB
backend, a bus number fix and `604a55c`, which takes the transfer's lock
around the handle in the completion path; none of them changes what a caller
is told about a cancel. The trial took the registry's 1.0.30 port verbatim,
from `microsoft/vcpkg` at `2e87314a3f6524e847ac5bb6a7b5d6e7559cb121`, as an
overlay, rather than moving the baseline and every other port with it.

### The fix

`cancel-waits-for-transfers.diff` changes `src/librtlsdr.c` only. Every
transfer is counted in flight from just before it is submitted until libusb
hands it back through the callback without it being resubmitted. The callback
resubmits only while the stream is running, so once a cancel is asked for the
count can only fall. The cancel loop cancels every transfer, treating
NOT_FOUND as the harmless answer it is, and then waits on libusb's events,
repeating the cancels each time, until the count is zero. Only then are the
transfers freed. If libusb's event handling itself fails with transfers still
out, they are leaked rather than freed, which costs a megabyte at the engine's
sixteen 64 KiB transfers and does not write into the heap.

A clean cancel now returns 0 without the per-transfer `Sleep(1)` the old loop
made on Windows. The public API is unchanged.

### The trial

`tools/rtlsdr-cancel-trial` makes the engine's pause over and over: stream with
`rtlsdr_read_async` on its own thread, sleep `(round % 7) * 20` ms, cancel
with the engine's retry, join, one control call of the three kinds the engine
makes, flush, stream again. Sixteen 64 KiB transfers, as the engine. It runs
the cancels in child processes of 300 and counts from the children's output
and exit codes, so a crash is counted rather than ending the run. After a -5 a
child does what the engine now does, which is to stop streaming and close the
device; `rtlsdr_close` makes control transfers, so a use after free the -5 left
can still land in it, and a crash there is counted against that -5.

It calls `rtl-sdr.h` and nothing of Revenant's, and it builds on its own
against any vcpkg install tree, which is how the five builds below were
compared without building the engine five times. Its `CMakeLists.txt` has the
command.

This machine was shared with nine other lanes' builds while it ran, with the
processor 82% busy when it was sampled, and load moves this race. So the five
configurations ran in rotation, one slice of 300 cancels each in turn, ten
rounds from 11:02 to 12:47, and drift in the load fell on all five alike
rather than on whichever happened to run during a busy stretch. Each row is
3000 cancels without page heap.

| Configuration | librtlsdr | libusb | Returned -5 | Processes that died | Died after a -5 |
| --- | --- | --- | --- | --- | --- |
| Baseline, what shipped | v2.0.2, three patches | 1.0.29 | 27 | 22 | 21 |
| librtlsdr master alone | `797f814`, three patches | 1.0.29 | 14 | 11 | 11 |
| libusb new alone | v2.0.2, three patches | 1.0.30 | 9 | 7 | 7 |
| Both | `797f814`, three patches | 1.0.30 | 16 | 13 | 13 |
| The fix | `797f814`, four patches | 1.0.29 | 0 | 0 | 0 |

Every death was 0xC0000005, an access violation. All but one came after a
-5, inside the `rtlsdr_close` that followed it. The one that did not, on the
baseline, died with no -5 reported before it, and page heap showed what that
looks like: run under cdb with full page heap, a v2.0.2 child made 164 clean
cancels and on the 165th faulted in `windows_iocp_thread`
(`windows_common.c` line 478), in the inlined `list_del`, reading a page the
heap had already released, before `rtlsdr_read_async` had returned. That is
the second of the two sites the RTL-SDR lane caught, reached from the other
side of the same race: libusb's completion thread unlinking a transfer
librtlsdr had just freed.

The -5 returns come in bursts. Each of the four unpatched builds had them in
three or four of its ten slices and none in the rest, and a single slice
carried as many as nine: per slice, baseline 0 6 8 9 0 4 0 0 0 0, master
7 0 1 0 4 0 0 2 0 0, libusb 1.0.30 0 0 2 4 0 0 3 0 0 0, both 0 3 0 0 0 6 7 0
0 0. Against that scatter the totals from 9 to 27 do not separate the four,
and none of them reaches zero. The source says why: neither upgrade touches
the exit condition.

A separate block of 3000 baseline cancels, run before the rotation, returned
-5 30 times and lost 27 processes. The rate is higher than the RTL-SDR lane's
one in 275 through the engine, on a busier machine with a callback that does
less; the trial's figures are for comparing builds, not for predicting what
the engine meets.

**Under full page heap** (`gflags /p /enable rtlsdr-cancel-trial.exe /full`),
the fix again: 3000 cancels, none returned -5, no process died. The baseline
under the same page heap, also 3000: 5 returned -5, and 3 processes died, each
during a child's first cancel and before `rtlsdr_read_async` had returned,
which is the `windows_iocp_thread` form caught under cdb above. Page heap
slows every allocation and moved the rate along with the outcome, so those
two figures do not compare with the table's.

**Through the engine.** The RTL-SDR lane's probe, "a streaming dongle takes
control calls back to back" in `tests/engine/test_rtlsdr_source.cpp`, at 200
rounds, three runs, against the `ci` build with the overlay: 600 control calls,
none refused, the stream running at the end of every run, every stop clean,
and not one LIBUSB_ERROR_PIPE line from librtlsdr. With v2.0.2 the lane saw the
fault in one to three runs of five at the same setting.

**Two things the fix changed that were not the target.** Measured over 600
cancels on each build, 2 slices of 300 in rotation, without page heap:

| | v2.0.2 | The fix |
| --- | --- | --- |
| Cancel accepted to `rtlsdr_read_async` returned, median | 249.8 ms | 7.7 ms |
| The same, 90th percentile | 250.7 ms | 12.8 ms |
| First control call after the cancel lands on the first attempt | 190 of 600 | 600 of 600 |

The quarter second is the old loop's `Sleep(1)` after each of sixteen cancels,
at Windows' default timer resolution of 15.6 ms: sixteen of those is 250 ms.
The failed first attempt is the LIBUSB_ERROR_PIPE the backend has retried
around since 2026-09-21 (`kRetunePipeRetries` in
`core/source/rtlsdr_source.cpp`). That it vanishes with the fix points at
the cause, which is inferred and not traced: the device refusing a control
transfer while transfers the old cancel had abandoned were still in flight.
So every control call on a streaming dongle now pauses the stream about a
quarter of a second less. `docs/ui-spectrum.md` has a scroll-tune interval
sized from the old pause, and says so.

### What is recommended, and why

The fix: `osmocom/rtl-sdr` at `797f814` with the four patches, and libusb
1.0.29 from the registry, unchanged. It is the only configuration that
reached zero, under page heap as well as without it.

- Neither upgrade, alone or together, is a fix. Each still returned -5 and
  still lost processes, and neither changes the exit condition that frees the
  transfers early.
- master rather than v2.0.2 underneath the patch, because the patch is
  written against it and because master's `65f0658` is a real fix to a
  neighbouring path: a transfer error no longer calls the cancel from inside
  the callback.
- libusb stays at 1.0.29. 1.0.30 was not separable from it in the trial, the
  fix does not need anything 1.0.30 changed, and keeping the registry's port
  keeps libusb's source in the release archive exactly what the relink
  decision in `docs/clean-room.md` names, with one overlay to maintain rather
  than two. The trial's 1.0.30 overlay was the registry's port verbatim and
  is not kept in the tree; the vcpkg commit named above has it if it is ever
  wanted.
- The engine's own handling of a -5, ending the stream rather than
  restarting it, stays. It no longer fires, and it is still the right answer
  if a future librtlsdr brings the fault back.

Offering the patch to osmocom is how the overlay would retire. Until a
release carries the fix, this overlay is what the engine links, and moving the
pinned commit means rebasing the patch and running the trial again.

### How CI builds it

The overlay is a directory of port files. vcpkg's binary cache keys a package
on the SHA256 of every file in its recipe, not on where the recipe came from,
so the overlay is built once on a runner and restored after that like any
registry port; its `vcpkg_abi_info.txt` lists the four patches, the portfile
and the manifest by hash and no path. Checked on this machine: a `vcpkg install`
of the manifest into a fresh directory, after librtlsdr had been built once
with the same triplets, restored all fourteen ports from the cache, librtlsdr
among them, and built nothing. Editing any file under `vcpkg-overlays/rtlsdr/` changes the
key and rebuilds librtlsdr, which is what should happen.

`scripts/corresponding_source.py` reads an overlay port from this repository.
vcpkg records an overlay's origin as NOASSERTION, so `manifest` finds the
recipe under `vcpkg-overlays/<port>/` and accepts it only when every file
matches the SHA256 the build recorded, and `bundle` publishes it from git at
the commit being archived. The upstream archive is fetched from
`osmocom/rtl-sdr` at the pinned commit and checked against the SHA512 in the
portfile, as before.

### Disclosure

Read for this work, all on 2026-09-23 and all for a library Revenant links
and now patches, which `docs/clean-room.md` permits:

- librtlsdr, `src/librtlsdr.c` at 797f814: the transfer callback,
  `_rtlsdr_alloc_async_buffers`, `_rtlsdr_free_async_buffers`,
  `rtlsdr_read_async`, `rtlsdr_cancel_async`, `rtlsdr_close`,
  `rtlsdr_reset_buffer`, the device structure and the buffer and timeout
  definitions. The whole diff from v2.0.2 to 797f814, which includes the Blog
  V4 Lite changes to `tuner_r82xx.c`, and the licence notice at the head of
  every source file.
- libusb: `libusb_cancel_transfer` in `libusb/io.c` at v1.0.29, the ChangeLog,
  the commit list from v1.0.29 to v1.0.30 for the core and Windows backend
  files, and the diff of `604a55c`.
- vcpkg's rtlsdr and libusb ports at the baseline and libusb's at
  `2e87314a`, which are MIT-licensed build recipes.

Nothing under `core/`, `tools/` or `ui/` implements anything read. The trial
tool calls librtlsdr's public API. The patch is a change to librtlsdr, lives
in the port that builds librtlsdr, and is distributed under librtlsdr's
licence, which its preamble says.

One finding from reading the notices, recorded and not resolved here.
`src/tuner_fc2580.c` carries no licence notice at all: its header says it was
"taken from the kernel driver" at a Terratec URL and nothing else. It is
compiled into the library at v2.0.2 and at 797f814 alike, so this trial
neither caused nor changed it. The "or later" grant `docs/clean-room.md` rests
on was confirmed in `librtlsdr.c` and `tuner_r82xx.c`; the same check at
797f814 finds it in `rtl-sdr.h`, `librtlsdr.c`, `tuner_r82xx.c`,
`tuner_e4k.c`, `tuner_fc0012.c` and `tuner_fc0013.c`, and not in
`tuner_fc2580.c`.
