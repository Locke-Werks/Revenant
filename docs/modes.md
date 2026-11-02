# Digital modes in scope

Every mode Revenant intends to implement, and every mode somebody will ask
about and not find here. This is scope, not a schedule. Nothing below carries a
date or an implementation order, and the effort column is a size estimate from
the survey work, not a queue position.

## What decides inclusion

A mode is in if four things hold. There is something to work from, meaning a
published specification obtainable at some price, or a signal that can be
measured. No live third-party patent reads on a receiver, because GPL-3.0
section 11 obliges a distributor to shield downstream users and that cannot be
done for somebody else's patent. No licence attaches terms GPL-3.0 section 7
would refuse, which bars a royalty-bearing pool or a field-of-use restriction at
any price including zero. And the mode is unencrypted, or its framing travels in
the clear under an encrypted payload.

**One mode is in against the patent rule, by the owner's decision.** DMR's
framing and metadata are in scope although Motorola's US8306071, live until
2027-02-19, may read on a receiver: the owner accepted that risk on
2026-09-23. "The ETSI IPR query, and what it did to DMR" below keeps the query
that excluded it and records the decision that put it back.

The scope rule underneath all of that: identification is always in scope,
because classifying a waveform practises nothing and needs no document. Decode
requires something to decode from. Decryption is never attempted, and a
published key does not change that.

**Purchasable counts.** TIA-102, RTCA DO-260C, ARINC 635-4, ICAO Annex 10,
ISO/IEC 11172-3 and IEC 62106 all cost money and are all acceptable. Free to
read is a budget question. Free to copy is a different question and the answer
is usually no: ETSI, ITU-R and 3GPP publish at no charge and retain copyright,
so a clause number goes in a comment and a table does not go in the tree.

**A missing specification is a cost, not a bar.** This is the part that has
moved. `docs/clean-room.md` changed on 2026-09-18 and its permitted-source list
now carries own measurement and a project's documentation as distinct from its
source. Where no document exists, the behaviour can be derived from the signal
and labelled as observation, per the practice section of that document. Four
entries below sit there: Meteor-M LRPT's image payload, the HamDRM narrow
parameter sets, Inmarsat-C's frame timing, and the Iridium burst format. What
measurement cannot rescue is a vocoder, because observing its output does not
recover the algorithm. That is why AMBE stays out and why it stays out
permanently rather than until a patent lapses.

**What is still forbidden** is unchanged: reading another decoder's source while
writing ours, porting it, or copying a table of selected constants out of it.
The open decoders for most of this list are GPL-2.0-only, which is separately a
licence bar, but the clean-room rule would apply whatever licence they carried.

## Reading the tables

`Effort` is small, medium or large, and it is the marginal cost once the shared
machinery a mode sits on already exists. A mode marked small often depends on a
large one.

`(unverified)` on a specification means nobody has held that document. The
verdict rests on it existing and saying what secondary sources claim it says.
Those are collected again at the end so they can be worked through as a list.

Where two surveys reached different verdicts on the same mode, the reason is
given in the entry rather than silently resolved.

## Digital voice, collected

This cross-cuts the tables below and exists because it is the question that
prompted the document. Every voice mode Revenant can render as audio in
software, and the one reason the rest cannot be.

| Mode | Codec | Codec specification | Audio in software |
| --- | --- | --- | --- |
| P25 Phase 1 | IMBE, 4400 bit/s voice plus 2800 FEC | TIA-102.BABA, purchasable | Yes |
| TETRA V+D and DMO | ACELP, 4.567 kbit/s | ETSI EN 300 395-2, free | Yes |
| M17 | Codec 2, 3200 or 1600 bit/s | No specification exists; the algorithm description covers method, the quantiser codebooks are trained data | Yes, by linking libcodec2 |
| FreeDV 1600, 700C, 700D, 700E | Codec 2, 1300 or 700C | As above | Yes, linked |
| FreeDV 2400A, 2400B, 800XA | Codec 2, 1300 or 700C | As above | Yes, linked |
| Opulent Voice | Opus, 16 kbit/s, 48 kHz mono | RFC 6716, free, royalty-free grants through the IETF process | Yes |
| ANDVT and any clear 2400 bit/s LPC link | LPC-10e, 2400 bit/s, 54-bit 22.5 ms frames | FED-STD-1015, free US Government work with the coefficient tables in it | Yes |
| Clear 4800 bit/s military CELP | FS-1016 CELP, 30 ms frames | FED-STD-1016, free | Yes |
| UHF milsatcom legacy and wideband military links | CVSD, 16 or 32 kbit/s | MIL-STD-188-113, free | Yes, on a channel not under COMSEC |
| DECT | G.726 ADPCM, 32 kbit/s | ITU-T G.726, free | Yes on a clear bearer; see the entry |
| DAB (classic, not DAB+) | MPEG-1/2 Audio Layer II, 32 to 384 kbit/s | ISO/IEC 11172-3 and 13818-3, purchasable | Yes |
| MELPe 2400 | MELP, 2400 bit/s | MIL-STD-3005, not located | Blocked on the document, not on law |
| DMR, dPMR, NXDN, P25 Phase 2, D-STAR, Fusion, BGAN, Thuraya, mini-M, Iridium | AMBE or AMBE+2 | None published, at any price | No. Hardware vocoder only |
| Tetrapol | RPCELP, roughly 6 kbit/s | Described at some level in the Tetrapol PAS; completeness unconfirmed | No, pending that check |

Three notes on that table.

**Codec 2 is linked, not clean-roomed, and that is a recorded exception.** There
is no Codec 2 specification. The algorithm description is real and the vector
quantiser codebooks are not in it: they were trained on a speech database and
exist only as data in the source, so a decoder written from the document alone
would not be bit-compatible with any transmitter. The licence is LGPL-2.1 with
no "or later", verified in `src/codec2.h` rather than off the repository root,
and LGPL-2.1 section 3 lets any recipient take the library under GPL version 2
or any later version, which reaches GPL-3.0-or-later. So it combines lawfully
and it needs a row in the linked-copyleft table in `docs/clean-room.md` beside
librtlsdr before M17 or FreeDV ships. Applying this project's GPL-2.0-only bar
mechanically to Codec 2 would reject it and kill both open voice modes for no
reason.

**MELPe is the surprise and the vendor framing is wrong.** Every vendor page
says a licence is required. That is true of their reference C and their
certified test vectors, and it is no longer true of the algorithm. The base
patent, US5699477, Texas Instruments, was filed 1994-11-09, granted 1997-12-16,
and shows expired with an anticipated expiration of 2014-12-16; the 1200 bit/s
superframe patent US7286982 has a 1999 priority and shows expired in 2019; the
600 bit/s Harris application WO2004070541A2 has a 2003 priority and has ceased.
Clean-room from the standard text is therefore fine, copying the reference is
not, and validating against Compandent's certified vectors is not. The one thing
missing is the standard: MIL-STD-3005 is absent from EverySpec's
MIL-STD-3000-9999 index and DLA ASSIST did not answer. Not schedulable until
somebody holds it.

**AMBE is a permanent exclusion, not a waiting one.** The bar is the absent
specification, not the patents, so patent expiry changes nothing. This matters
because repositories claiming bit-exact AMBE or P25 half-rate vocoders exist and
would import both an untraceable derivation and a live patent question into a
binary signed under a company name.

What exists instead is a socket. Since 2026-10-02 `revenant-engine` loads
vocoder plugins from the `vocoders` folder beside it at startup, under the plain
C ABI in `core/decode/vocoder_abi.h`, and a D-STAR or DMR receiver's voice goes
to the first loaded one that serves the mode or its codec
(`core/rpc/plugin_voice.h`). The plugin is a device or a library somebody else
owns; with none loaded the receiver is silent. Nothing in this tree implements
AMBE or AMBE+2, so the table's "No" stands for software of this project's own.

### Speech to text, where it touches each voice mode

Since 2026-10-03 the engine can transcribe a receiver's speech with Whisper
(`docs/rpc.md`, "Speech to text"). It reads the same audio a listener hears, so
it reaches exactly the voice this document says Revenant can render and no
further. A digital voice receiver is cut into utterances by its calls.

| Mode | Transcribed |
| --- | --- |
| P25 Phase 1 | Yes, from the IMBE voice, a call at a time, with the call's NAC, talkgroup and source on the transcript. An encrypted call is silence with the gate shut and never reaches the recogniser |
| D-STAR, DMR | Only with a vocoder plugin loaded, since without one the voice is silence. A DMR call marked private never reaches the plugin and so is not transcribed. No call fields are attached today |
| TETRA | No. A tetra receiver makes no audio; TETRA voice is not decoded |
| M17 | Not checked. The decoder renders unencrypted Codec 2 voice since 2026-11-02; whether that audio reaches the recogniser has not been tested |
| Analogue speech: am, sam, nfm, usb, lsb, dsb | Yes, by squelch where one is set and by a level detector where it is open |
| wfm | Only on a receiver chosen in, since broadcast FM is mostly music |
| cw, raw | Never |

WHAT THE M17 ROW USED TO SAY, until 2026-11-02: "M17 is decoded for its
framing; its Codec 2 voice is not rendered".

The recogniser is told the speech is English and does not detect the
language. A transcript is the recogniser's reading of the audio, not a decode:
nothing in the signal checks it.

## Included

### Land mobile, and the open alternatives to it

Voice on the AMBE systems is excluded in every row below that names it. The
framing, addressing, control and text messaging are not, and that is most of
what these systems carry in the clear.

| Mode | Numbers | Specification | Effort |
| --- | --- | --- | --- |
| DMR Tier I/II/III | 4FSK, 4800 sym/s, 9600 bit/s, 12.5 kHz, two-slot TDMA in 30 ms slots | ETSI TS 102 361-1 to -4, free. In by the owner's decision of 2026-09-23 against a live patent; see "The ETSI IPR query, and what it did to DMR" | Medium |
| dPMR446 and licensed modes 2/3 | 4FSK, 2400 sym/s, 4800 bit/s, 6.25 kHz FDMA | ETSI TS 102 490 and TS 102 658, free | Medium |
| NXDN (IDAS, NEXEDGE) | 4FSK; 6.25 kHz at 2400 sym/s, 80 ms frames; 12.5 kHz at 4800 sym/s, 40 ms frames; RRC alpha 0.2 | NXDN TS 1-A v1.3, November 2011, free from the NXDN Forum, download permitted and redistribution forbidden | Medium |
| P25 Phase 1 FDMA | C4FM 4FSK, 4800 sym/s, 9600 bit/s, 12.5 kHz; CQPSK/LSM on simulcast at the same symbol rate | TIA-102.BAAA-A plus the TIA-102.AABx trunking set, purchasable | Large |
| TETRA V+D and Direct Mode | pi/4-DQPSK, 18000 sym/s, 36 kbit/s, 25 kHz, 4-slot TDMA, 14.167 ms slots | ETSI EN 300 392-2 and the EN 300 396 series, free | Large |
| TEDS | pi/8-D8PSK and 4/16/64-QAM in 25, 50, 100 and 150 kHz, 2.7 kHz subcarrier spacing | ETSI EN 300 392-2 V3.x, free | Large |
| D-STAR DV | GMSK BT 0.5, 4800 bit/s, 6.25 kHz; 2400 AMBE plus 1200 FEC plus 1200 data | JARL "アマチュア無線のデジタル化技術の標準方式" Ver 7.0, free at `jarl.com/d-star/STD7_0.pdf`. Japanese only: the English edition this row used to name is gone, see the note below the table | Small |
| D-STAR DD | GMSK 128 kbit/s on 23 cm, Ethernet frames, roughly 150 kHz | Same JARL standard | Medium |
| M17 | 4FSK, 4800 sym/s, h=1/3, 9 kHz occupied, 384-bit 40 ms frames, a 368-bit randomiser table (PRBS9 fills BERT frames only), Golay(24,12), punctured r=1/2 K=5, QPP interleaver, CRC-16 0x5935 init 0xFFFF | M17 Protocol Specification Part I, free at spec.m17project.org, complete to bit level | Medium |
| FreeDV 1600, 700C, 700D, 700E | OFDM in an SSB passband; 1600 is 14 DQPSK data carriers and one DBPSK pilot in 1125 Hz; 700D is 17 coherent QPSK carriers at 25 baud, 160 ms frame, 2 ms cyclic prefix, LDPC | No standards document. Project design notes and the FreeDV user manual, cited per file with a retrieval date | Large |
| FreeDV 2400A, 2400B, 800XA | 4FSK; 2400A at 2400 bit/s in about 5 kHz of RF, 2400B through an analogue FM audio path, 800XA at 800 bit/s | As above | Medium |
| Opulent Voice | 40 ms frames of 134 bytes, CCSDS K=7 r=1/2 to 268 bytes, 67x32 interleaver, 24-bit sync 0x02B8DB, roughly 81.3 kHz | `opulent_voice_protocol.md` in OpenResearchInstitute/interlocutor, free and public | Medium |
| MPT-1327 | FFSK 1200 bit/s sub-audio over analogue FM, 12.5 or 25 kHz, 64-bit codewords, BCH(63,48) plus parity | MPT 1327, UK DTI 1988 rev 1997, now Ofcom, free | Small |

WHAT THE FREEDV 1600, 700C, 700D, 700E ROW USED TO SAY, until 2026-11-02:
"1600 is 16 DQPSK carriers in about 1.1 kHz". The codec2 design notes give 14
DQPSK data carriers and one DBPSK pilot in 1125 Hz. FreeDV is still not
written: those notes lack the LDPC matrix, the unique word and pilot values,
the mappings and the carrier frequencies, which are only in the codec2 source.

Three cautions carried forward.

**The English edition of the JARL standard no longer exists.** This row said
"English edition free at jarl.org" and that was wrong on 2026-09-21. The page
that offered it, `jarl.com/d-star/shogen.htm`, still contains the sentence
"the English version of the standard is here", commented out in the HTML with
an empty `href`. So JARL withdrew the link rather than moved it, and there is
no English edition to find. Ver 7.0 in Japanese is the first-party document
and is what `core/decode/dstar.cpp` is written from. Its clause numbers are
the ones in that file.

**Opulent Voice's modulation as published is self-inconsistent.** Minimum shift
keying carrying two bits per symbol at 27100 sym/s is almost certainly 4-ary FSK
at h=0.5. Confirm with ORI before writing a demodulator. Their software is
stated as "GPL 2.0" with no "or later", so none of it can be vendored, which
does not affect clean-rooming from their description.

**The M17 specification has two licences, and its repository a third
reading.** The document's own Licenses page, in Part I version 2.0.4 of 21
January 2026 as `core/decode/m17.h` records it, puts the specification text
under the GNU Free Documentation License 1.3 or later, and only the software
listings printed inside it under GPL-2.0-or-later. The repository's LICENSE
file is the plain GPL-2.0 text with no "or later" at the root. Both are true
of different things, and neither moves the verdict: sync words, polynomials
and bit layouts are facts, and Revenant writes its own code from them. The
operative rule is unchanged: cite clause and figure numbers, never paste a
table, a figure or a paragraph, and do not implement from the embedded
listings, whose exposure `docs/clean-room.md` logs.

WHAT THIS PARAGRAPH USED TO SAY: "**The M17 specification document is
GPL-2.0.**", and that the earlier survey's GNU Free Documentation License 1.3
"is contradicted by the repository". The document's Licenses page says the
FDL for the text; the repository's LICENSE file is what said GPL-2.0.

### Amateur text, data and image

| Mode | Numbers | Specification | Effort |
| --- | --- | --- | --- |
| FT8 | 8-GFSK, 6.25 baud, 6.25 Hz spacing, 79 symbols, 12.64 s in a 15 s slot, 77-bit payload, CRC-14 0x6757, LDPC(174,91) | QEX July/August 2020 pp. 7-17, free, plus `ft4_ft8_protocols.tgz` which the authors placed in the public domain (unverified: whether ARRL still serves that path) | Large |
| FT4 | 4-GFSK, 20.833 baud, 105 symbols, 4.48 s in a 7.5 s slot, same payload, CRC and LDPC as FT8, plus a published 77-bit scrambler | Same QEX paper, Appendix A | Small |
| JT65 A/B/C | 65-FSK, 2.6917 baud, 126 symbols, 46.8 s; RS(63,12) over GF(64) interleaved with 63 pseudo-random sync symbols | QEX Sept/Oct 2005 pp. 3-12 and QEX May/June 2016 pp. 8-17, both free | Medium |
| JT9 | 9-FSK, 1.736 baud, 85 symbols, 15.6 Hz occupied, K=32 r=1/2 convolutional, published interleaver | "The JT9 Coding Process", G4JNT, free | Medium |
| JT4 A through G | 4-FSK, 4.375 baud, 207 symbols, tone spacing 4.375 Hz to 315 Hz by submode | "The JT4 Coding Process", G4JNT, free | Small |
| WSPR | 4-FSK CPM, 1.4648 baud, 162 symbols, 110.6 s, 50-bit message, K=32 r=1/2 taps 0xF2D05351 and 0xE4613C47 | "The WSPR Coding Process", G4JNT, 2009, free | Medium |
| FST4 and FST4W | 4-GFSK, 160 symbols, five 8-symbol sync words, slots 15 s to 1800 s, CRC-24 0x100065b, LDPC(240,101) and (240,74) | Quick-Start Guide to FST4 and FST4W, free. The two generator matrices are not printed in it and exist only in the GPL-3 WSJT-X tree | Medium |
| MSK144 | MSK, 2000 baud, 1000 Hz shift, 144-bit 72 ms frames, 77-bit payload, CRC-13, LDPC(128,90) | No free document located. The G4JNT LDPC modes document of December 2021 was searched for on 2026-11-02 and not found, and the (128,90) parity-check and generator matrices are only in the GPL-3 WSJT-X tree. The QEX 2017 paper describes the superseded 72-bit form | Medium |
| Q65 | 65-FSK, 85 symbols with a 22-symbol sync vector, submodes 15 s to 300 s, QRA(65,15) over GF(64), CRC-12 | Quick-Start Guide to Q65 and "The Q65 Coding Process", G4JNT, both free | Large |
| JS8 | 8-GFSK, 79 symbols, four speeds from 3.125 to 20 baud, 75-bit frames in six types | JS8Call User Guide, free; Appendix A prints the modified Huffman code. The dictionary compression table is not published. The guide names the code as the source of truth and does not print the FEC for the 75-bit frames, the Costas arrays per speed or the frame bit layouts, so it is not writable from the guide alone | Medium |
| PSK31, PSK63, QPSK31 | BPSK 31.25 or 62.5 baud, cosine envelope, roughly 60 Hz occupied; QPSK adds r=1/2 K=5 | G3PLX in RadCom Dec 1998 and Jan 1999, reprinted free by ARRL with the full varicode table | Small |
| RTTY (ITA2) | 2-FSK, 45.45 baud, 170 Hz shift standard, 5-unit alphabet, 1.5 stop bits | ITU-T Recommendation S.1 for the alphabet and S.3 for the 7.5-unit character, free; the 45.45/170 convention is practice and is cited as such. Done, see below | Small |
| AMTOR SITOR-A and SITOR-B | 2-FSK, 100 baud, 170 Hz shift, CCIR 476 7-unit 4/3 code, 450 ms ARQ cycle or 280 ms time-diversity FEC | ITU-R M.625-4 (03/2012, in force) and M.476-5, free. SITOR-B done, see below | Small |
| Olivia | MFSK 2 to 256 tones over 125 to 2000 Hz; default 32/1000 at 31.25 baud; Walsh FEC scrambled by 0xE257E6D0291574EC | "The Draft Specification For The Olivia HF Transmission System", SP9VRC, hosted free by ARRL | Medium |
| Contestia | Olivia geometry, six-bit alphabet, half the FEC | fldigi mode documentation only. Cite the page and the retrieval date | Small |
| MT63 | 64 DBPSK tones, 10 symbols/s per tone, 15.625 Hz spacing in MT63-1000, Walsh expansion over 32 symbols | ARRL MT-63 technical characteristics page, free | Medium || MFSK16 and MFSK8 | 16 tones, 15.625 baud, about 316 Hz occupied, IZ8BLY varicode, r=1/2 K=7, diagonal interleaver | ARRL MFSK page plus ZL1BPU's pages, free. Done, see below | Small |
| DominoEX | IFK+ over 18 tones, submodes 4 to 88, DominoEX 11 at 11.719 baud | ARRL Domino page and "DominoEX Technical Information", free. Submodes 44 and 88 are in no document read, and the varicode table gives one code to both ":" and "<", and one secondary code to both "J" and "4". Submodes 4 to 22 done, see below | Small |
| THOR | DominoEX IFK+ with the MFSK16 varicode and r=1/2 K=7 FEC | fldigi mode documentation only | Small |
| FSQ and FSQCall | IFK+ over 33 tones at three times the symbol rate, 8.79 Hz spacing at 2.93 baud | Author documentation at w1hkj.org and qsl.net/zl1bpu, publicly disclosed by the authors | Small |
| Hellschreiber (Feld-Hell and variants) | OOK raster, 122.5 baud element rate, 7x14 element cell, 2.5 char/s, about 75 Hz | "Hellschreiber Modes - Technical Specifications", ZL1BPU 1998, free | Small |
| Throb and ThrobX | Multi-tone OOK at 1, 2 or 4 symbols/s over 9 or 11 tones | fldigi mode documentation only | Small |
| AX.25 packet | 1200 baud Bell 202 AFSK, 9600 baud G3RUH scrambled FSK, 300 baud HF FSK; HDLC with bit stuffing and the X.25 FCS | AX.25 v2.2, TAPR/ARRL July 1998, free. The 1200 and 9600 baud modems have no standards document and are cited to Bell 202 and to Miller's 1988 article. No document of any kind gives 300 baud HF packet's tones, shift or sideband, so it is not writable as a cited mode. 1200 baud and 9600 baud G3RUH done, see below | Medium |
| APRS | AX.25 UI frames; position, weather, telemetry, object, status, message, Mic-E and base-91 compressed formats | APRS Protocol Reference 1.0.1, free at aprs.org, plus the WB2OSZ consolidated 1.2. Position, status, message and Mic-E done, see below | Medium |
| FX.25 | AX.25 with a 64-bit correlation tag selecting one of several Reed-Solomon configurations | "FX.25: Forward Error Correction Extension to AX.25 Link Protocol For Amateur Packet Radio", Stensat Group, Version 0.01 DRAFT, document version 0.01.06, 1 September 2006, TAPR DCC 2006, free. Read on 2026-10-02 from the Internet Archive's capture of stensat.org/docs/FX-25_01_06.pdf, which no longer answers. It does not state the Reed-Solomon field or first root. 1200 baud done, see below | Small |
| IL2P | Reed-Solomon header and payload blocks replacing HDLC on the same physical layers | "IL2P Specification Draft v0.6, 16 March 2024", Nino Carrillo KK4HEJ, free. Read on 2026-10-02 from https://tarpn.net/t/il2p/il2p-specification_draft_v0-6.pdf. Its three example packets fix what the prose leaves open. 1200 baud AFSK done, see below | Small |
| PACTOR-I | 2-FSK, 100 or 200 baud, 200 Hz shift, 1.25 s ARQ cycle, 0x55 sync header, ITU-T CRC | ARRL PACTOR technical characteristics page, free. It prints the packet structure and ARQ timing; it does not give the status byte layout or the CRC preset and bit order, and its Huffman table has duplicate and mislabelled codes. Not writable from it alone | Medium |
| ARDOP | 200, 500, 1000 and 2000 Hz sessions, 50/100/167 baud, 4FSK narrow and OFDM wide, two-tone leader at 1450 and 1550 Hz | ARDOP Specification Revision 0.7 (July 2015), Winlink Development Team, free. A draft, which says the protocol "will be" placed in the public domain; no later revision or completed grant was found | Large |
| WINMOR | OFDM in 500 or 1600 Hz, DPSK and 4FSK | ARRL WINMOR technical characteristics page and the author's TNC specification Rev 1.0.6, free. Winlink retired the mode in 2020. The specification lacks the (8,4) Hamming code, the tone mapping, the byte split, the Reed-Solomon field and root, the CRC preset and the Appendix A frame types, and its tone list has typos; not writable from it | Small |
| Winlink B2F | Message structure and forwarding over any bearer, LZHUF-compressed body | "Open B2F", Winlink Development Team, free at winlink.org/B2F | Small |
| CW | OOK, 5 to 40 WPM, dot length 1200/WPM ms, 100 to 200 Hz occupied | ITU-R M.1677-1 (10/2009), free | Small |
| SSDV | 256-byte packets, 205 bytes of image with Reed-Solomon, baseline JPEG with fixed tables | UKHAS wiki `guides:ssdv`, free, which leaves the Reed-Solomon, CRC, alphabet and tables unstated; see below. Payload format is ISO/IEC 10918-1, free as ITU-T T.81 | Small |
| Analog SSTV: common layer | 1500 Hz black, 2300 Hz white, 1200 Hz sync; VIS header of 300 ms 1900 Hz, 10 ms 1200 Hz, 300 ms 1900 Hz, then start bit, seven 30 ms data bits LSB first, parity, stop | N7CXI, "Proposal for SSTV Mode Specifications", Dayton 20 May 2000, free, and carrying an express grant of use (unverified: the archive capture path) | Medium |
| SSTV Martin M1-M4 and HQ | RGB line-sequential, 4.862 ms sync, 0.572 ms porch, 146.432 ms colour scan on M1, 320x256 | Dayton paper for M1 and M2; Bruchanov's handbook for M3, M4 and HQ | Small |
| SSTV Scottie S1-S4 and DX | RGB, sync between blue and red rather than at the line break, 9.0 ms sync, 1.5 ms porch, 138.240 ms on S1 | Dayton paper for S1, S2 and DX | Small |
| SSTV Robot 36 and 72, and monochrome 8/12/24/36 | Y/R-Y/B-Y 4:2:0, alternating even and odd chroma lines with 1500 Hz and 2300 Hz separators | Dayton paper plus Appendix B for the colour matrix | Small |
| SSTV Wraase SC-1 and SC-2 | SC2-180 is RGB, 5.5225 ms sync, 235.000 ms per colour, 320x256; SC-1 syncs before each colour component | Dayton paper for SC2-180; Bruchanov for the rest | Small |
| SSTV PD50 to PD290 | Y/R-Y/B-Y with chroma averaged over two lines, 20.000 ms sync, 2.080 ms porch, two luminance scans per transmitted line, 320x256 to 800x616 | Dayton paper, timings supplied by the mode's author | Small |
| SSTV Pasokon P3, P5, P7 | RGB 640x496 including a 16-line header; sync and porch vary per submode | Dayton paper, timings supplied by the author | Small |
| SSTV AVT 24/90/94/125/188 | Fully synchronous, no per-line sync pulse; 384 to 960 lines per minute; VIS codes in groups of four | Bruchanov's handbook only. The Dayton paper states it had no data from the author | Medium |
| SSTV MMSSTV, MSCAN and Proskan families | YCrCb and RGB at 320x256, 84 to 419 lines per minute, several using a 16-bit VIS extension | Bruchanov's handbook | Small |
| FAX480, Color FAX 240, Ham Color 204 | 512x480 at 262.144 ms per line; opened by a tone header and phasing rather than a VIS code | Dayton paper for FAX480; Bruchanov table 11.1 for the amateur fax start and stop tones | Small |
| DSSTV over HamDRM | COFDM in 2.3 or 2.5 kHz; modes A/B/E at 53/57, 45/51 and 29/31 subcarriers; QAM-4/16/64; 400 ms frames; FAC at QAM-4 with a 40-bit descriptor and CRC-8 | ETSI ES 201 980 free for the parent system; the narrow parameter sets are not in it and are derived by measurement, labelled as observation | Large |
| DSSTV over RDFT | Eight subcarriers 590 to 2200 Hz at 230 Hz spacing, differential phase, outer RS plus inner RS(8,4) | Bruchanov chapter 10, free | Medium |
| Analog fast-scan ATV | NTSC 525/29.97 with 3.579545 MHz subcarrier, PAL 625/25 with 4.43361875 MHz, 6 MHz or wider | ITU-R BT.470 (archived) and BT.1700, free | Large |

WHAT FOUR ROWS OF THIS TABLE USED TO SAY, until 2026-11-02, each found
wrong when somebody went to write the mode from the document named. MSK144:
"G4JNT December 2021 LDPC modes document, free". PACTOR-I: "which prints the
complete Huffman table, packet structure, status byte and ARQ timing". ARDOP:
"released to the public domain by its author"; the only revision found is the
0.7 draft, which says it "will be". AX.25 packet: "The two modems have no
standards document", which counted 300 baud HF FSK as covered by a citation
it does not have.

**Five keyboard modes stop at the free documents.** THOR, Olivia, Contestia,
MT63 and Throb were each tried on 2026-11-02 from the documents named and
stopped: none prints the character-to-Walsh or character-to-tone mapping, the
interleaver geometry or the scrambler's bit order, which are in the fldigi
source only. None is written.

**SSDV waits on its document, not on the JPEG.** The UKHAS wiki page does not
give the Reed-Solomon field or first root, the CRC-32 variant or what it
covers, the base-40 callsign alphabet, the fixed tables or the quality scaling;
the only description of the Reed-Solomon code is C source. The baseline JPEG
decoder it would feed exists, see below.

**Where the WSJT-X generator matrices come from matters.** FT8 and FT4 take
theirs from `ft4_ft8_protocols.tgz`, which the authors placed in the public
domain and which the QEX paper names as reference [14]. FST4, FST4W, MSK144 and
Q65 do not: their matrices exist only in the GPL-3 WSJT-X tree. Copying those
into Revenant is licence-compatible and is not clean-room, so each needs a row
in the linked-and-copied table in `docs/clean-room.md` rather than a file header
citing a Quick-Start Guide that never printed them.

**G4JNT's coding-process papers were written by studying the WSJT-X source.**
They say so on their first page. They are documentation and the current policy
permits them, and the provenance chain still runs through GPL-3 source, so cite
the authors' own QEX papers and Quick-Start guides wherever those cover the same
ground and G4JNT only where nothing else does.

**JS8 decodes in two flavours and only one is publishable.** Uncompressed frames
come out of the user guide. Dictionary-compressed frames need a word list of
about 260,000 entries that exists only in the GPL-3 source, so those will decode
to nothing. Say so in the header rather than shipping a silent partial, which
reads as a working decoder having a bad day.

### Utility, military and maritime HF

| Mode | Numbers | Specification | Effort |
| --- | --- | --- | --- |
| MIL-STD-188-110 serial tone | 1800 Hz subcarrier, 2400 sym/s, BPSK through 64-QAM, 75 to 9600 bit/s coded, 12800 uncoded, 3 kHz | MIL-STD-188-110C Change 1 and -110D, free with unlimited distribution from DLA ASSIST, US Government work with no copyright | Large |
| MIL-STD-188-110 parallel tone (39-tone) | 39 DQPSK subcarriers 675 to 2812.5 Hz on 56.25 Hz spacing, Doppler tone at 393.75 Hz, 44.44 baud | Appendix B of 110B and 110C. Vendor documentation sometimes cites the 110A appendix letter; check the edition | Medium |
| 2G ALE | 8-FSK, 750 to 2500 Hz on 250 Hz spacing, 125 sym/s, 24-bit word Golay(24,12) coded to 49 tones and 392 ms, each word sent three times | MIL-STD-188-141D Appendix A, free. FED-STD-1045A is the equivalent | Medium |
| 3G ALE / ARCS | Burst waveforms BW0 to BW5, 8PSK at 2400 baud on 1800 Hz, 3 kHz | MIL-STD-188-141B (1999) Appendix C, free, which specifies BW0 to BW4. 141D's Appendix C no longer carries the waveforms and points to STANAG 4538 instead. BW0 and BW1 done, see below | Large |
| STANAG 4285 | 1800 Hz subcarrier, 2400 sym/s, BPSK/QPSK/8PSK, 75 to 2400 bit/s coded | STANAG 4285 Edition 1 (1989), purchasable from standards resellers | Large |
| STANAG 4529 | 1240 Hz channel, 1200 baud, BPSK through 8PSK, 75 to 1200 bit/s coded, 213.33 ms frames | STANAG 4529 Edition 1 (1995), purchasable. A parameter variant of 4285 | Medium |
| STANAG 4539 | 1800 Hz, 2400 baud, BPSK through 64-QAM, mini-probe blocks between data blocks | Cite MIL-STD-188-110B Appendix C, which is free and specifies the identical waveform, rather than the restricted STANAG number | Large |
| STANAG 4415 | 1800 Hz, 2400 baud BPSK, 75 bit/s with heavy repetition, usable near -6 dB SNR | The equivalent 75 bit/s low-SNR mode in MIL-STD-188-110B and -110C, free | Small |
| STANAG 4481 | 2-FSK 50 to 600 bit/s, usually 50 or 75 with 850 Hz shift; multi-channel FSK; a 300 bit/s BPSK variant | STANAG 4481, purchasable; the FSK variants need little beyond the emission definitions in Radio Regulations Appendix 1, free | Small |
| STANAG 5066 | Not a waveform. D_PDU framing, ARQ, addressing and the subnetwork interface above 4285, 4539 and 110 | STANAG 5066 Edition 4 (May 2022), purchasable | Medium |
| Link-11 CLEW | 16 tones: 605 Hz Doppler reference, 14 carriers from 935 Hz on 110 Hz spacing, 2915 Hz; DQPSK, 1364 or 2250 bit/s | MIL-STD-188-203-1A, free. The payload catalogue MIL-STD-6011 is distribution limited, so the deliverable stops at bits and net cycle timing. Table III's error-detection polynomial reads x^5+x+1 in the scan; x^6+x^5+1 reproduces the table and is used, on the reading that the scan dropped a 6. Done, see below | Medium |
| HF Selcall | 2-FSK, 170 Hz shift, 100 baud, CW preamble, 4 to 6 digit addresses | No document located. ITU-R M.493-16 (12/2023) was read for it and specifies DSC; it has no 4 to 6 digit selective call and no CW preamble | Small |
| ARQ-M2 and ARQ-M4 | FSK at 96 or 192 baud, two or four interleaved telegraph channels, 7-unit 4/3 code; the shift is practice, not cited | ITU-R F.342-2 (1970), free, for the code, the channel layout and the 96 and 192 Bd rates. It gives no shift and no emission class | Medium |
| Piccolo MK6 and MK12 | MFSK, classic form 32-ary at about 10 baud with 10 Hz spacing, roughly 320 Hz; MK6 is 6 tones in about 180 Hz | IEE 1963 and 1982 papers, purchasable from the IET digital library. They describe the modulation rather than a full profile | Medium |
| LPC-10e | 2400 bit/s, 54-bit frames of 22.5 ms | FED-STD-1015 (1984), free US Government work with the complete algorithm and tables | Small |
| FS-1016 CELP | 4800 bit/s, 30 ms frames, stochastic codebook | FED-STD-1016 (1991), free | Medium |
| CVSD | 16 or 32 kbit/s, one bit per sample, adaptive step | MIL-STD-188-113, free | Small |
| MELPe 2400 | 2400 bit/s, 54-bit frames of 22.5 ms over 180 samples at 8 kHz | MIL-STD-3005, not located (unverified). Patents confirmed expired | Large |
| DRM30 and DRM+ system layer | COFDM, modes A to D in 4.5 to 20 kHz below 30 MHz, mode E in 96 kHz VHF; FAC at 4-QAM, SDC and MSC at 16 or 64-QAM | ETSI ES 201 980, free, with TS 102 979 for Journaline and TS 102 821 and TS 101 968 alongside | Large |

WHAT THE 3G ALE ROW USED TO SAY, until 2026-11-02: "MIL-STD-188-141D
Appendix C, free. STANAG 4538 is the NATO twin and is not needed". 141D's
Appendix C defers to STANAG 4538; the waveforms were read from 141B.

WHAT TWO ROWS OF THIS TABLE USED TO SAY, until 2026-11-02. HF Selcall:
"ITU-R M.493-16 (12/2023, verified in force), free", as its specification;
M.493-16 is the DSC Recommendation and holds neither the selcall format nor
its CW preamble. ARQ-M2 and ARQ-M4: "FSK at 96 or 192 baud, 170 to 400 Hz
shift", all cited to F.342-2, which states the rates and not the shift.

**The normal case here is a clean waveform carrying an encrypted payload.**
STANAG 4481 is KW-46 or KG-84 ciphertext, STANAG 4197 puts COMSEC between the
vocoder and the modem, Link-11 payloads are protected, and much
MIL-STD-188-110 traffic is too. Demodulating to bits is in scope and correct and
will never produce text. The user interface has to say so or each of these reads
as a broken decoder.

**ALE Linking Protection turns a working decoder into a liar.** MIL-STD-188-141
Appendix B scrambles the 2G ALE word stream under a key, and much military ALE
is protected. A decoder that is correct on unprotected nets prints plausible
nonsense addresses on protected ones rather than failing. Detect LP and say so.
Defeating it is decryption and is out of scope.

**DRM's encumbrance is entirely in the audio codec.** The system layer is free
and clean and yields station identification, service labels, Journaline text
pages, slideshow images and emergency warning with no codec involved. Extract
the audio super frames and stop. xHE-AAC is barred and is listed under
Excluded.

### Aeronautical and vehicle data

| Mode | Numbers | Specification | Effort |
| --- | --- | --- | --- |
| Mode A/C | 1090 MHz PPM, 0.45 us pulses, F1/F2 20.3 us apart, 13 slots at 1.45 us | ICAO Annex 10 Volume IV Chapter 3, purchasable | Medium |
| Mode S, all downlink formats and BDS registers | 1090 MHz PPM, 1 Mbit/s, 4-pulse preamble at 0/1.0/3.5/4.5 us, 56 or 112 bits, 24-bit parity overlaid with the address | ICAO Annex 10 Volume IV plus ICAO Doc 9871 Edition 2 Amendment 2 (2023-04-03), both purchasable | Medium |
| ADS-B 1090ES (DF17, DF18) | Same physical layer; position and velocity squitters at 2 Hz, identification every 5 s | RTCA DO-260C (2020-12-17) with Change 1, purchasable | Medium |
| UAT 978 | CPFSK h=0.6, 1.041667 Mbit/s, about 1.3 MHz; RS(30,18) and RS(48,34) over GF(2^8); uplink 432 bytes as six interleaved RS(92,72) | RTCA DO-282C (2022-06-23), purchasable | Large |
| FIS-B | Carried in the UAT ground uplink frame | RTCA DO-358B and DO-267A, purchasable. NEXRAD and METAR content is US NWS public domain | Large |
| TIS-B and ADS-R | DF18 with CF codes 2, 3, 5 and 6 on 1090; dedicated payload types on UAT | DO-260C and DO-282C, purchasable | Small |
| TCAS DF16 and BDS 3,0 | 1090 MHz replies carrying the MV coordination field and the resolution advisory register | RTCA DO-185B or -185C (which is current is unverified) plus ICAO Doc 9871, purchasable | Small |
| ACARS (VHF) | MSK 2400 bit/s, tones 1200 and 2400 Hz, AM on the VHF carrier, 25 kHz; per-character odd parity plus a 16-bit block check | ARINC 618-9, purchasable from SAE ITC | Medium |
| VDL Mode 2 | D8PSK, 10500 sym/s, 31500 bit/s, 25 kHz, raised cosine 0.6; AVLC per ISO 13239, RS(255,249) on the burst | ETSI EN 301 841-1, free from etsi.org, covers the RF and modulation only. The unique word, scrambler, symbol mapping, header FEC, Reed-Solomon parameters and AVLC are in ICAO Doc 9776 and Annex 10, purchasable | Medium |
| VDL Mode 4 | GFSK 19200 bit/s in 25 kHz, self-organising TDMA, 4500 slots per minute | ETSI EN 301 842-1, free | Medium |
| HFDL | 1440 Hz subcarrier, 1800 sym/s, BPSK/QPSK/8PSK giving 300 to 1800 bit/s, 32 s TDMA frame of 13 slots | ARINC 635-4, purchasable from SAE ITC | Large |
| ADS-C and FANS-1/A | No physical layer of its own; rides ACARS, HFDL, VDL2 or satellite | ARINC 622, purchasable, plus ICAO Doc 9880 | Medium |
| ASTERIX | Not a radio mode. Octet-aligned records with an FSPEC bitmap | EUROCONTROL SPEC-0149, free. Category 021 v2.7 (July 2025) for ADS-B target reports | Small |
| RDS-TMC (ALERT-C) | No new physical layer. Group 8A on the 57 kHz subcarrier, attributed and counted raw today; see the RDS entry under what is done | EN ISO 14819-1 through -4, purchasable from ISO and CEN. Only the free preview of 14819-1:2021 has been read, which stops at clause 3.2 | Small once clauses 7 and 9 of 14819-1 are in hand |
| DSRC / ITS-G5 | 5.850 to 5.925 GHz, 10 MHz OFDM, 52 used subcarriers, 8 us symbol, 3 to 27 Mbit/s; BSMs at 10 Hz | IEEE 802.11-2020 free through IEEE GET; SAE J2735 purchasable; ETSI EN 302 663 free | Large |

**RDS-TMC decodes cheaply and the location tables do not.** The message carries
a location code and turning that into a road segment needs a national location
table, which is a licensed database. Emit the raw code and let the user supply
their own table. Shipping one inside a GPL-3.0 work is both a breach of that
database licence and a section 7 further-restriction problem.

**The ADS-B specifications are not on the internet for free.** The formats are
in DO-260C and Doc 9871 at serious prices, and the free PDFs in circulation are
drafts, working papers and leaked copies. A clean room built on a leaked draft
is not a clean room, and a constant citing a working-paper page number will not
survive scrutiny.

**HFDL ground station identifiers and the frequency plan are operational data.**
They are not in ARINC 635, they change, and they must come from a live or
user-supplied source. Hardcoding them out of another decoder's tables is a
clean-room breach and is wrong within a year.

### Maritime safety and data

| Mode | Numbers | Specification | Effort |
| --- | --- | --- | --- |
| AIS Class A, Class B, AtoN, base station, SAR aircraft | GMSK BT 0.4 over FM, 9600 bit/s, 25 kHz at 161.975 and 162.025 MHz; NRZI, HDLC with 0x7E flags and bit stuffing, CRC-16-CCITT; 26.67 ms slots, 2250 per minute per channel | ITU-R M.1371-6 (02/2026, verified in force, supersedes -5 of 02/2014), free. Done from -5, checked against -6, see "AIS and VHF DSC" below | Medium |
| AIS-SART, AIS-MOB, AIS-EPIRB | Identical physical layer; a burst of eight position reports once per minute, navigation status 14, message 14 text | Annex 8 of ITU-R M.1371-6, free; IMO Resolution MSC.246(83), free | Small |
| Satellite AIS and message 27 | Same GMSK; AIS3 at 156.775 and AIS4 at 156.825 MHz; message 27 is the reduced long-range report | ITU-R M.1371-6, free | Medium |
| VDES and AIS ASM channels | ASM at 161.950 and 162.000 MHz is pi/4-QPSK at 9.6 ksym/s, root raised cosine 0.35, with a 27-symbol syncword of 1, Barker 13 and inverted Barker 13 (M.2092-2 Annex 2 Table 1 and Table 7), sharing only the AIS slot frame. ASM-TER Link IDs 1 to 3 done, see below; VDE-TER is pi/4-QPSK, 8PSK and 16QAM across 25, 50 and 100 kHz to about 307 kbit/s | ITU-R M.2092-2 (02/2026, verified in force), free; IALA G1117 Edition 3.0, free | Medium |
| DSC on VHF (channel 70) | FFSK 1200 baud on 156.525 MHz, mark 1300 Hz, space 2100 Hz, 10-bit symbols, time diversity | ITU-R M.493-16 (12/2023, verified in force), free; M.541-11 for procedures. Done from -15, checked against -16, see "AIS and VHF DSC" below | Small |
| DSC on MF and HF | 2-FSK 100 baud, 170 Hz shift, centre 1700 Hz in J2B; same symbol alphabet and diversity | ITU-R M.493-16, free | Small |
| NAVTEX | F1B FSK, 170 Hz shift, 100 baud, CCIR 476 7-unit 4/3 code, 280 ms time-diversity FEC, about 300 Hz; 518, 490 and 4209.5 kHz | ITU-R M.540-2 (06/1990, verified still in force), free, plus the IMO NAVTEX Manual for the B1 to B4 header semantics. Framing done without the IMO semantics, see below | Small |
| NBDP SITOR-A and SITOR-B | FSK 100 baud, 170 Hz shift; SITOR-A is a 450 ms ARQ cycle, SITOR-B is the NAVTEX FEC mode | ITU-R M.625-4 (03/2012, verified in force) and M.476-5, free. SITOR-B done, see below | Small |
| NAVDAT (500 kHz and HF) | OFDM in 10.5 kHz, QPSK, 16-QAM and 64-QAM, 12 to 18 kbit/s | ITU-R M.2010-3 (02/2026, in force, superseding M.2010-2) for 500 kHz and M.2058-1 for HF, free | Large |
| HF radiofax / WEFAX | FM subcarrier, 1900 Hz centre plus or minus 400 Hz, 1500 and 2300 Hz at the extremes, 60 to 240 lines per minute, IOC 576 or 288, start tone 300 or 675 Hz, stop 450 Hz | WMO-No. 386 (frozen 2024-12-31) plus the free NOAA/NWS Worldwide Marine Radiofacsimile Broadcast Schedules, which is the practical citation for the constants | Medium |

WHAT TWO ROWS OF THIS TABLE USED TO SAY, until 2026-11-02. VDES: "ASM at
161.950 and 162.000 MHz reuses the AIS physical layer". AIS is GMSK at 9600
bit/s; M.2092-2 Table 7 makes ASM-TER pi/4-QPSK at 9600 symbol/s with a Barker
syncword, and `core/decode/vdes.h` uses nothing of `core/decode/ais.h`.
NAVDAT named M.2010-2, which M.2010-3 (02/2026) has superseded.

**NAVTEX is not SITOR-B on a different frequency.** The B1 to B4 header
semantics, the station and subject filtering and the 518 kHz channel plan live
in the IMO NAVTEX Manual, not in M.540. A decoder built from M.540 alone emits
characters where users expect filtered, attributed messages.

**Radiofax tone polarity is not universal.** Sources disagree on whether 1500 Hz
is black or white, North American practice uses 2300 or 2400 Hz for black, and
at least one European station in the NWS schedule runs plus and minus 425 Hz.
Hardcode any of it and half the world's charts arrive as negatives.

### Image and weather

| Mode | Numbers | Specification | Effort |
| --- | --- | --- | --- |
| NOAA APT | AVHRR AM on a 2400 Hz subcarrier at 87 percent, FM on 137 MHz with about 17 kHz deviation; 120 lines per minute, 4160 words/s, two 1040-word frames per line with 1040 Hz and 832 Hz sync bursts | NOAA KLM User's Guide section 4.2, free US Government work | Small |
| NOAA HRPT | Split-phase BPSK, 665.4 kbit/s, 1698/1702.5/1707 MHz, minor frame of 11090 ten-bit words at 360 lines per minute | NOAA KLM User's Guide section 4.1, free | Large |
| Metop AHRPT | 1701.3 MHz, QPSK about 2.333 Msym/s, 3.5 Mbit/s user data, about 4.5 MHz | EUMETSAT Metop Direct Readout AHRPT guide, free, plus CCSDS 131.0-B and 732.0-B, free | Large |
| Meteor-M LRPT | 137.1 or 137.9 MHz, QPSK 72 ksym/s (80 on some passes), r=1/2 K=7, four interleaved RS(255,223), CCSDS CADU 1020 bytes, sync 0x1ACFFC1D | Framing from the free CCSDS Blue Books. The MSU-MR packet layout and DCT quantisation have no obtainable document and are derived from received frames, labelled as observation | Large |
| Meteor-M HRPT | 1700 MHz RHCP, full-resolution MSU-MR, symbol rate near 665 kbit/s (unverified, community measurement) | CCSDS for the framing; no official document: Planeta, the guidebook and the OSCAR listing were checked on 2026-11-02, and the sources found disagree on the rate. Not written | Medium |
| GOES HRIT and EMWIN | 1694.1 MHz, BPSK 927 ksym/s, r=1/2 Viterbi, RS(255,223) interleave 4, CCSDS pseudo-randomiser h(x) = 1 + x^3 + x^5 + x^7 + x^8, about 400 kbit/s | CGMS-03 LRIT/HRIT Global Specification issue 2.8, free; NOAA GOES-R HRIT/EMWIN product specification, free; Aerospace report ATR-2010-5482, free | Large |
| GOES GRB | 1686.6 MHz, dual circular polarisation, 15.5 Mbit/s per polarisation; 8PSK at 7.825768 Msym/s r=2/3 or QPSK at 8.665938 Msym/s r=9/10 | GOES-R Product User's Guide Volume 4, GRB, 416-R-PUG-GRB-0348 Rev 2.3 (1 June 2020), free, plus ETSI EN 302 307-1 for DVB-S2 and CCSDS 732.0 and 133.0. Done for one polarisation, see below | Large |
| GOES Data Collection System | Platform uplinks at 401 to 402 MHz, 100/300/1200 bit/s BPSK; DCPR downlink at 1694.5 MHz | GOES DCS Certification Standards and the DCPR channel tables, free from noaasis.noaa.gov (the certification standard was not obtained on 2026-11-02) | Medium |
| GEO-KOMPSAT-2A LRIT | 1692.14 MHz, NRZ-L BPSK, 64 kbit/s information, r=1/2 plus RS(255,223) interleave 4, 128 ksym/s as the mission specification states it | GK2A LRIT Mission Specification Document issue 1.0 (2019-05-31), free from nmsc.kma.go.kr. Image data is JPEG 2000 per ISO/IEC 15444-1, lossless; the DES fields are defined and marked not used | Large |
| DVB-S | QPSK, 1 to 45 MBd, roll-off 0.35, r=1/2 to 7/8 punctured convolutional, RS(204,188), interleave 12, MPEG-2 TS | ETSI EN 300 421, free | Large |

WHAT THE GOES GRB ROW USED TO SAY, until 2026-11-02, as its specification:
"GOES Rebroadcast Downlink specification at goes-r.gov, free, plus CCSDS
133.0-B-1". The decoder was written from the Product User's Guide Volume 4.

WHAT THE GEO-KOMPSAT-2A ROW USED TO SAY, until 2026-11-02: "r=1/2 plus
RS(255,223) interleave 4, so about 146 ksym/s", a figure worked out rather
than read. The mission specification gives 128 ksym/s.

**GOES HRIT's compression is unresolved between sources.** One survey says
Golomb-Rice per CCSDS 121.0-B carried in an undocumented xRIT header type 131;
two say JPEG 2000 per ISO/IEC 15444-1. Both are patent-clean so no verdict
moves, and the constants and their citations differ, so settle it against the
document before writing either.

**Buy nothing for JPEG or JPEG 2000.** ITU-T T.81 is the same text as ISO/IEC
10918-1 and ITU-T T.800 is the same text as ISO/IEC 15444-1, and both are free
from itu.int while the ISO copies cost money. The same works for MPEG-2 systems
and video through ITU-T H.222.0 and H.262.

**NOAA POES is gone.** NOAA-18 was passivated 2025-06-06, NOAA-19 on 2025-08-13
and NOAA-15 on 2025-08-19, and passivation included turning the transmitters
off. APT and NOAA HRPT are archive and recording-playback modes now. Meteor-M
LRPT is the only picture left on 137 MHz, which is the reason its payload is
worth deriving by measurement rather than shipping frames and withholding
pictures.

### Paging, metering and short-range telemetry

| Mode | Numbers | Specification | Effort |
| --- | --- | --- | --- |
| POCSAG | 2-FSK, +/-4.5 kHz, 512/1200/2400 bit/s, 576-bit preamble, batches of 8 frames x 2 codewords, sync 0x7CD215D8, BCH(31,21) plus parity | ITU-R M.584-2 (11/1997), free, for the code and format. The 512 and 1200 bit/s rates, the polarity and the deviation are ITU-R M.539-3 clause 4.3, withdrawn in 2007; 2400 bit/s is practice. Done, see below | Small |
| FLEX | 2-FSK and 4-FSK, 1600 or 3200 sym/s giving 1600/3200/6400 bit/s, 1.875 s frames, 128 frames per 4-minute cycle, (31,21) BCH interleaved | ARIB STD-T43 (unverified: English edition not located, obtainability from outside Japan unconfirmed). Motorola's own specification was never public and there is no TIA or ETSI equivalent | Medium |
| ERMES | 4-PAM/FM, 3125 baud, 6250 bit/s, 25 kHz, 16 channels in 169.4 to 169.8 MHz, (30,18) cyclic coding | ETSI ETS 300 133-4, free | Medium |
| RDS2 | Up to three further data-streams on 66.5, 71.25 and 76 kHz beside the 57 kHz stream, locked to it, and 61.75 kHz not used (IEC 62106-1:2018 clauses 4.2 and 4.3, read in the free preview). The 1187.5 bit/s biphase modulation, the 26-bit blocks and the offset words on the new streams are unverified: clauses 4.4 to 6 and Annex A are past the end of the preview. Carries station logos and larger ODA payloads, so it is an image path as well as a text one | IEC 62106-1:2018, "Part 1: Modulation characteristics and baseband coding", purchasable; the parts carrying group type C and the file transfer are not identified yet | Medium. See the RDS entry under what is done for why it is not small |
| DAB multiplex, FIC and data services | COFDM pi/4-DQPSK; Mode I is 1536 carriers at 1 kHz spacing, 1.536 MHz, 1246 us symbol, 76 symbols per 96 ms frame, 2.4 Mbit/s gross | ETSI EN 300 401 V2.1.1, free, with TS 101 756, TS 101 499 (MOT SlideShow), TS 102 979 (Journaline) and TS 102 818 | Large |
| DAB audio (Layer II) | 48 kHz MPEG-1 or 24 kHz MPEG-2 LSF Layer II, 32 to 384 kbit/s, 24 ms frames with ScF-CRC and trailing X-PAD | ISO/IEC 11172-3 and 13818-3, purchasable; the DAB framing is restated in EN 300 401 clause 5, free | Medium |
| Wireless M-Bus | Mode S 32.768 kbit/s Manchester 2-FSK at 868.3 MHz; Mode T 66.667 kbit/s 3-of-6 at 868.95; Mode C 100 kbit/s NRZ; Mode N 4-GFSK at 169.4 MHz | EN 13757-4, purchasable (unverified: 2025 versus 2019 edition). OMS Specification Volume 2 is free and restates much of the frame detail | Medium |
| Z-Wave | R1 9.6 kbit/s Manchester FSK, R2 40 kbit/s NRZ FSK, R3 100 kbit/s GFSK; 868.42 MHz and 908.42/916.0 MHz; 32-bit HomeID, 8-bit NodeID | ITU-T G.9959 (01/2015), in force and free; Z-Wave Alliance SDS13781 and companions, free | Medium |
| IEEE 802.15.4 and Zigbee | 2.4 GHz O-QPSK half-sine, 2 Mchip/s, 16-ary orthogonal spreading, 62.5 ksym/s, 250 kbit/s; 868/915 MHz BPSK DSSS at 20 and 40 kbit/s; preamble 32 zero chips, SFD 0xA7 | IEEE 802.15.4-2020, free through the IEEE GET Program; Zigbee PRO 2023 free from csa-iot.org | Medium |
| Bluetooth Low Energy advertising | GFSK BT 0.5, index 0.45 to 0.55, 1 or 2 Msym/s, 2 MHz channels; advertising on 2402, 2426 and 2480 MHz; access address 0x8E89BED6, CRC-24, 7-bit LFSR whitening seeded by channel index | Bluetooth Core Specification 6.x, free from bluetooth.com | Medium |
| Wi-SUN FAN / 802.15.4g SUN FSK | 2-FSK and 4-FSK, 50 to 300 kbit/s, index 0.5 or 1.0, 200 to 600 kHz channels in 902 to 928 MHz, 16-bit PHY header, CRC-16 or -32 | IEEE 802.15.4-2020 clause 19, free through GET; Wi-SUN FAN 1.1 Technical Profile, free | Medium |
| EnOcean | ASK 125 kbit/s with ERP1's 8-to-12 bit coding at 868.3 MHz, 315 or 902 MHz elsewhere; 14 to 21 byte telegrams sent in triplicate | ISO/IEC 14543-3-10:2020, purchasable; EnOcean Equipment Profiles free from the Alliance | Small |
| DECT | GFSK BT 0.5, 1.152 Mbit/s, 1.728 MHz spacing, TDMA/TDD, 24 slots per 10 ms frame, 32 kbit/s per duplex bearer, 16-bit A-field CRC | ETSI EN 300 175 parts 1 to 8, free; ITU-T G.726 for the codec, free | Large |

WHAT TWO ROWS OF THESE TABLES USED TO SAY, until 2026-11-02. VDL Mode 2 named
"ETSI EN 301 841-1, free from etsi.org" as its whole specification; it lacks
everything above the modulation. EnOcean: "ASK 125 kbit/s Manchester at 868.3
MHz"; EnOcean Radio Protocol 1 V1.2 specifies 8-to-12 coding, which is what
`core/decode/enocean.h` implements.

**DECT is licence-clean and patent-clean, and rendering its audio is a separate
decision.** The 1992 air interface has nothing live in it and G.726's patents
ran out in the 1990s, so the technical route to audio is open. Two things sit in
front of it that are not licensing questions: the 1994 amendments removed the
cordless-telephone exemption from 18 USC 2510, and most handsets apply DSC
scrambling, which is out of scope by the no-decryption rule. Recording it as
metadata-only would bury a clean legal result under an operational one. The
A-field system information and bearer structure are in scope either way.

**The clear header in front of an encrypted payload is where the value is.**
Wireless M-Bus, Z-Wave, Zigbee, Wi-SUN and Meshtastic all broadcast manufacturer,
device id, device type, network id and node id in the clear so that relays can
forward what they cannot read. Implementing one of these and then reporting
nothing because the payload is encrypted would miss most of it.

### Satellite

| Mode | Numbers | Specification | Effort |
| --- | --- | --- | --- |
| CCSDS TM and AOS with channel coding | BPSK, QPSK, OQPSK, GMSK; ASM 0x1ACFFC1D uncoded and convolutional, 0x034776C7272895B0 turbo; r=1/2 K=7 and its punctures, RS(255,223) and (255,239) interleave 1 to 8, turbo r=1/2 to 1/6, LDPC r=1/2 to 7/8 | CCSDS 131.0-B, 132.0-B, 732.0-B, 133.0-B and 401.0-B, all free Blue Books | Medium |
| CubeSat Space Protocol over AX100-class radios | GMSK and GFSK, 100 bit/s to 38.4 kbit/s, usually 9600, in 435 to 438 and 145.8 to 146.0 MHz; randomiser, RS(255,223), ASM, 32-bit CSP header | GS-CSP-1.1 and the NanoCom AX100 datasheet, free, plus the CCSDS Blue Books. GomSpace does not publish the AX100 framing; on 2026-11-02 it was found only in third-party mirrors, so the radio layer is not writable from a first-party document | Medium |
| AX.25 amateur satellite packet | 1200 baud Bell 202 AFSK and 9600 baud G3RUH in 145.8 to 146.0 and 435 to 438 MHz | AX.25 v2.2, free from TAPR. See the amateur table | Small |
| FUNcube-class BPSK telemetry | 1200 bit/s DBPSK, r=1/2 K=7 with an RS outer code, 256-byte frames, on a 145.9 MHz class downlink | AMSAT-UK published frame layouts and per-mission telemetry dictionaries. No document number exists; cite the page and the dictionary per constant | Small |
| Cospas-Sarsat 406 MHz first generation | 160 ms unmodulated carrier then biphase-L at 400 bit/s, +/-1.1 radians; 440 or 520 ms burst every 50 s; 112 or 144 bits; BCH(82,61) and BCH(106,83) | C/S T.001, free. Issue 4 Revision 7 (March 2021) read, from a copy at sar.mot.go.th; Revision 13 exists and was not read. Done, see below | Small |
| Cospas-Sarsat second generation (SGB) | 406.05 MHz, DSSS OQPSK, 38400 chip/s from a truncated 23-stage m-sequence, 256 chips per bit per arm; 1 s burst, 250-bit message, BCH(250,202) | C/S T.018, free. Issue 1 Revision 7 (March 2021) read from cospas-sarsat.int; its pages are watermarked as superseded and no later issue was found. The LFSR initialisation values are tabulated in it. Done, see below | Medium |
| GPS L1 C/A (LNAV) | 1575.42 MHz, BPSK-R(1), 1.023 Mchip/s, 1023-chip Gold codes, 50 bit/s in 300-bit subframes | IS-GPS-200 Revision N (2022-08-22) with IRN-003, free at gps.gov | Large |
| GPS L2C and L5 (CNAV) | L2C at 1227.60 MHz, CM and CL time-multiplexed at 511.5 kchip/s each; L5 at 1176.45 MHz, 10.23 Mchip/s, 10230-chip codes with NH overlays | IS-GPS-200N and IS-GPS-705J, both 1 August 2022, free at gps.gov. Done, see below | Medium |
| GPS L1C (CNAV-2) | TMBOC(6,1,4/33) pilot and BOC(1,1) data, 10230-chip primaries, 1800-symbol 18 s frames, LDPC | IS-GPS-800J (1 August 2022), free at gps.gov. Done, see below | Medium |
| GLONASS L1OF and L2OF | FDMA, L1 at 1602 + 0.5625k MHz, 511 kchip/s, 50 bit/s Manchester to 100 sym/s, 2 s strings in 30 s frames | GLONASS ICD Edition 5.1 (2008), free in English translation; separate free ICDs for the L1OC/L2OC/L3OC CDMA signals | Medium |
| BeiDou B1I, B1C, B2a | B1I at 1561.098 MHz BPSK 2.046 Mchip/s; B1C at 1575.42 MHz BOC(1,1) with QMBOC pilot; B2a at 1176.45 MHz BPSK(10) | BDS SIS ICD Open Service B1I v3.0 (2019-02) and the matching B1C, B2a, B2b and B3I documents, free in English | Medium |
| QZSS | GPS-interoperable L1 C/A, L1C, L2C and L5, plus L6 CLAS at 2000 bit/s and L1S at 250 bit/s | IS-QZSS-PNT, IS-QZSS-L6 and IS-QZSS-L1S, free (unverified: current revisions) | Small |
| NavIC / IRNSS L5 SPS | 1176.45 MHz, BPSK(1), 1.023 Mchip/s, 50 bit/s r=1/2 to 100 sym/s, 600-bit master frames, CRC-24 | ISRO-IRNSS-ICD-SPS-1.1. Stopped: the document forbids reproduction without ISRO's written consent; see the open questions | Medium |
| SBAS L1 (WAAS, EGNOS, MSAS, GAGAN) | 1575.42 MHz, BPSK(1), PRNs 120 to 158, 250 bit/s r=1/2 K=7 to 500 sym/s, 250-bit messages once per second, CRC-24 | RTCA DO-229F, purchasable; ICAO Annex 10 Volume I is the international equivalent | Medium |
| Inmarsat-C / STD-C NCS TDM | BPSK 1200 sym/s, r=1/2 K=7 with block interleaving, 600 bit/s net, 1530.0 to 1545.0 MHz, 5 kHz raster, 10368-symbol 8.64 s frame, about 2.5 kHz occupied | No lawful air-interface document. ETSI ETS 300 460 is free for RF parameters; IEC 61097-4:2024 is purchasable and its coverage of frame layout is unverified. Frame and symbol timing come from measurement of the live signal, labelled as observation | Medium |
| Inmarsat-C EGC: SafetyNET and FleetNET | Rides the TDM above; LES id, service code, priority, repetition and presentation descriptors; C0 to C5 addressing plus NAVAREA, circular and rectangular | IMO International SafetyNET Manual, free; Inmarsat International SafetyNET Handbook Edition Six (April 2020), free; IMO COMSAR.1/Circ.41 and MSC.1/Circ.1364 | Small |
| Inmarsat Classic Aero P, R and T (data) | A-BPSK at 600 and 1200 bit/s, A-QPSK at 10500 bit/s with r=1/2 convolutional; 1530 to 1559 MHz; 2.5 to 17.5 kHz occupied | ICAO Annex 10 Volume III Part I Chapter 4 and ICAO Doc 9925, purchasable (unverified: whether they specify to demodulator level). Voice on the C channel is AMBE-family and is excluded | Large |
| Iridium Ring Alert | 1626.270833 MHz simplex, 41.667 kHz channel; 12-byte BPSK preamble then about 103 bytes DQPSK at 25 kBd; unencrypted | No official document. Peer-reviewed literature gives the field layout: satellite id, beam id, sub-satellite latitude, longitude and altitude, handover information | Medium |

WHAT THREE ROWS OF THIS TABLE USED TO SAY, until 2026-11-02. GPS L2C and L5:
"IS-GPS-200N and IS-GPS-705 Revision J (2022-08-22), free". GPS L1C:
"IS-GPS-800 Revision J (2022-08-22), free". The cover dates of all three are 1
August 2022. NavIC: "ISRO-IRNSS-ICD-SPS-1.1, free"; it is free to download and
forbids reproduction without ISRO's written consent, so no decoder was written.

**The Inmarsat-C System Definition Manual is off limits.** It is stamped
Inmarsat Confidential and copies sit at the top of a web search for it. That is
a trade-secret exposure as well as a clean-room bar, and it is worse than
reading source because it would taint the whole Inmarsat lane rather than one
file. The third path is the one taken above: measure the live signal, and use
the free ETSI RF standard and the free IMO message-layer documents for
everything else.

**Iridium Ring Alert bursts carry the TMSI of handsets performing handover.** A
decoder that logs IRA continuously builds a handset movement database as a side
effect of mapping the constellation. Decide whether that field is displayed,
hashed or dropped before writing it, not after.

**A Cospas-Sarsat decode is a live distress alert.** It yields a vessel or
aircraft identity and a position during an emergency. Display and discard is the
right default.

## Metadata only

Framing in the clear under a payload that cannot be decoded. This is more
information than it sounds like: identity, network structure, call setup, slot
assignment and mode changes all travel outside the encrypted or vocoded part.

| Mode | What comes out without the payload | Why the payload stops |
| --- | --- | --- |
| P25 Phase 2 | ISCH synchronisation, slot and superframe structure, source and group addressing in the MAC, and the associated Phase 1 control channel. The framing documents are free: TIA-102.BBAB (TDMA physical layer), TIA-102.BBAC and its addendum BBAC-1 (MAC layer), and TSB-102.BBAA (overview) are in archive.org/details/TIA-102_Series_Documents, the collection core/decode/p25p1.h cites | Almost the whole payload is AMBE+2, so unlike Phase 1 there is nothing left after the vocoder. A vocoder plugin could take the voice the way D-STAR and DMR's does, core/rpc/plugin_voice.h, once this framing is decoded |
| Tetrapol | Network and talkgroup metadata, call setup, framing | RPCELP is described at some level in the PAS and the completeness of that description is unconfirmed |
| ARIB STD-T98 (Japanese DCR) | 6.25 kHz 4FSK framing, mostly a parameter set once NXDN and dPMR exist | AMBE+2, and whether the ARIB document is obtainable is unconfirmed |
| PACTOR-II | Mode, two-tone 200 Hz DPSK signature, 1.25 s cycle, speed level, plus the PACTOR-I FSK link setup | Published descriptions stop short of the puncture patterns, the interleaver map and the Pseudo-Markov Compression tables |
| PACTOR-III | The same plus the variable packet headers, the speed level table, byte counts and status byte, all printed in the free SCS description | The same three missing pieces, and the Huffman table is referred back to PACTOR-I |
| PACTOR-IV | That a PACTOR link exists and went to a mode Revenant cannot follow, read off the PACTOR-I FSK handshake it still uses for backwards compatibility | No specification of any kind exists |
| VARA HF and VARA FM | 52-carrier 42-baud OFDM geometry, the 48-tone FSK ACK burst, frame type | The 2018 specification is an overview with no turbo code definition, no interleaver, no scrambler and no bit-level framing |
| CLOVER and CLOVER-2000 | The 8-tone 250 Hz signature | ARRL's pages are summaries prepared to satisfy a publication rule, not implementable specifications (unverified: nobody has read them) |
| G-TOR | Golay(24,12), the interleaver rule, frame lengths, the CRC and the 2.40 s cycle, all published | The Huffman table and the frame sync pattern are not published |
| STANAG 4197 (ANDVT) | 16-tone preamble and 39-tone QPSK library, 2400 bit/s rate | Every real ANDVT link puts COMSEC between the vocoder and the modem. If a plaintext link appears, the LPC-10e decoder turns it into speech with no further work |
| Link-11 SLEW | Telling SLEW from CLEW, which is a modulation question needing no document | Whether the public edition of MIL-STD-188-203-1A carries the SLEW annex is unconfirmed; payload stops at MIL-STD-6011 either way |
| UHF milsatcom DAMA and the Integrated Waveform | Channel type, waveform, burst structure, orderwire timing | COMSEC. Clear legacy channels carry FM or 16 kbit/s CVSD, which is decodable, see the voice table |
| CIS-12 / AT-3004D | Twelve equally spaced tones plus a pilot at 120 baud, named and parameterised | No public document, and the payload is scrambled |
| CIS-45 and the Serdolik family | Tone count, spacing, symbol rate, variant label | Same |
| Inmarsat-C ship-to-shore return TDMA | That a terminal is transmitting and on what assignment | No lawful framing source, and the payload is private point-to-point correspondence rather than a broadcast |
| Inmarsat BGAN, FleetBroadband, SwiftBroadband | Carrier detection, bearer type, symbol rate (8.4 to 168 kBd), modulation, frame length, from the free ETSI TS 102 744 | User traffic is encrypted and voice is AMBE+2 |
| Thuraya GMR-1 and GMR-1 3G | pi/4-CQPSK 23.4 kBd framing, 40 ms frames of 24 slots, GSM-derived signalling, from the free ETSI TS 101 376 | AMBE voice or encrypted packet data |
| Inmarsat mini-M / GMR-2 | pi/4-DQPSK 8.4 kBd framing from the free ETSI TS 101 377 | AMBE voice |
| Iridium L-band burst and TDMA framing | Burst detection, 90 ms frame with a 20.32 ms simplex slot and eight 8.28 ms slots, DQPSK 25 kBd after a BPSK unique word | Literature and expired Motorola patents describe the burst; the patent conclusion rests on the design programme's filing era rather than a docket check, and arXiv:2603.12062 (Jedermann et al., 12 March 2026) resolves but gives no Ring Alert field layout. Ring Alert is a full include |
| ORBCOMM subscriber downlink | SDPSK 4800 bit/s demodulation, the twelve-channel plan in 137.0 to 138.0 MHz, presence and rate, all from the free A80TD0008 Rev G | The ORBCOMM Air Interface Specification carries the packet layer and has never been published (unverified: whether it exists in obtainable form) |
| Argos / A-DCS platform terminals | Burst detection, 401.65 MHz plus or minus 30 kHz, 400 bit/s Manchester, burst interval | No air-interface document comparable to C/S T.001 was found; platform ID and sensor layouts are undocumented, and the traffic belongs to identifiable operators |
| FY-3 AHRPT and FY-1 CHRPT | Signal identification, demodulator lock, frame recovery | No public CMA instrument format document was located |
| FY-2 S-VISSR | The same | An S-VISSR format description exists for FY-2C/D/E; no current public download URL was confirmed |
| Meteosat MSG LRIT and HRIT | Frame synchronisation and xRIT header parsing, stopping at the encrypted data field | The payload is encrypted and the key arrives on a EUMETSAT hardware unit licensed to a named person. Never ship a key path and never accept a user-supplied key |
| Elektro-L and Arktika-M xRIT | Frame and header level, for almost no extra cost once GOES and GK-2A exist | No Russian mission-specific implementation document located |
| ISM device telemetry (the rtl_433 class: TPMS, weather stations, door sensors) | Carrier, modulation class, symbol rate, line coding, packet length, repetition interval, reported as an unidentified ISM burst | No specification exists for any of it. Every published frame format traces to somebody's reverse-engineered decoder, and that one source is also GPL-2.0-only |
| Drone Remote ID (ASTM F3411) | The same as BLE advertising, on the same terms | Not a licence limit at all: ASTM F3411-22a is purchasable and the BLE and Wi-Fi physical layers are free. 2.4 and 5 GHz coverage is a front-end question and belongs in the hardware matrix. Two surveys returned opposite verdicts and this is the resolution |
| 121.5 MHz ELT | Detection and labelling of a swept-tone distress signal | There is no data. Identity comes only from direction finding, and satellite monitoring of 121.5 MHz ended in February 2009, so a hit is a local transmitter or a test |

**Encrypted DMR and TETRA still decode at the framing layer**, which is in scope
and genuinely useful: identification, site and talkgroup tracking work where the
voice does not. TEA1 through TEA4 are restricted-distribution algorithms and are
not to be sought, obtained or implemented.

**Meshtastic's default public channel key is published** in the project's own
documentation, so decrypting the default channel is one AES-256-CTR call away
and will look harmless. It is still decryption. The packet header travels in
clear so relays can forward what they cannot read, which is exactly the case the
scope rule puts in scope. Decode the header and stop. Meshtastic is excluded for
a different reason anyway, below.

## Excluded

### Blocked pending a document

These are not exclusions on law. Somebody has to hold a document before the work
can start, and all three verdicts rest on documents nobody has opened.

| Mode | What is needed |
| --- | --- |
| Yaesu System Fusion (C4FM) | "Amateur Radio Digital Standards" revision 1.02 from yaesu.com. The download endpoint returned HTTP 500 from two independent network paths on 2026-09-20. Everything else about Fusion is favourable: the manufacturer publishes the framing, the physical layer is the same 4FSK 4800 sym/s path DMR, NXDN and P25 Phase 1 need, and FICH, mode identification, callsign and DSQ are clean-roomable from a first-party document. Ask Yaesu. Do not substitute a mirror of unknown provenance and do not fall back to reading gr-ysf |
| FLEX | ARIB STD-T43 in an English edition, confirmed to cover the air interface and purchasable from outside Japan. ARIB's English descriptor pages returned 404 from two paths. POCSAG is unaffected and rests on ITU-R M.584-2 |
| MELPe | MIL-STD-3005, plus the 2001 and 2002 annexes for 1200 bit/s, 600 bit/s and the noise pre-processor. The patent position is settled and confirmed; the document position is not |

### Excluded on a live patent

GPL-3.0 section 11 makes conveying a work while relying on a patent licence an
obligation to shield downstream users, which cannot be done for a third party's
pool. Paying does not fix this. Each row says when to look again.

| Mode | Patent position | Revisit |
| --- | --- | --- |
| LoRa physical layer | EP2449690B1, Semtech, priority 2009-07-02, filed 2010-07-02, granted 2016-01-06, expiry 2030-07-02, with claims covering the receiver: multiply by a locally generated conjugate chirp and apply an FFT. Later receiver-specific grants EP3264622B1 and US10305535B2 run to 2036. There is also no published specification, which measurement would now answer, so the exclusion rests on the patent alone | 2030 for the base claim, 2036 for the receiver grants |
| LoRaWAN | Inherits the LoRa physical layer. The L2 1.0.4 and RP002 documents are free and complete, and the MAC is genuinely specified | With LoRa |
| Meshtastic | Inherits the LoRa physical layer. Nothing above it is a problem: the documentation is good, the protobuf schema is published, and the clear header is the useful part | With LoRa. Put it first in the queue on that day |
| DAB+ audio | HE-AAC v2, Via LA pool, commercially active in 2026, licensing decoders specifically at 0.98 USD per unit falling to 0.10 at volume. Pool members hold parametric-stereo patents expiring 2029-05-14 and 2030-11-05 | 2030, and check AAC-LC separately: its core patents are widely reported expired and a bare AAC-LC decoder may sit differently from a full HE-AAC v2 one |
| DRM audio (xHE-AAC and the legacy HE-AAC v2 path) | Via LA states xHE-AAC was added to the AAC programme. USAC filings run 2009 to 2012, so terms reach 2029 to 2032 | 2032 |
| HD Radio / NRSC-5 | Xperi's 2023-04-04 disclosure to the NRSC, filed under section 7.2.5.1 of the NRSC Procedures Manual, lists in-force receiver patents in Attachment B including 11,190,334 and reissues RE48,655, RE48,966 and RE49,210, and offers a licence "with charge". Separately the HDC codec is published nowhere at any price, so a licensed implementation still could not produce audio | Not before the 2030s, and the codec question does not resolve with the patents |
| mioty (TS-UNB) | Sisvel runs a royalty-bearing MIOTY LPWAN platform with Fraunhofer, Diehl Metering, Apator Miitors and KPN as owners. Fraunhofer filed the telegram-splitting patent in Germany in 2013 and the US counterpart in 2014. ETSI TS 103 357 is free, which does not help | 2034 |
| Sigfox | UnaBiz holds US11,368,184 and US10,855,329 on ultra-narrow-band random medium access and frequency hopping, with claims describing a network receiving such messages. Device Radio Specifications are free, so this is a patent decision rather than a clean-room one | 2030s |
| DVB-S2X | Sisvel launched the pool in September 2018 with Fraunhofer, Hughes, Newtec, RAI, ESA and WORK Microwave. The specification is 2014 and the filings behind it 2012 to 2014 | 2032 to 2034 |
| DATV carrying H.264/AVC or H.265/HEVC | Active pools with filings running into the late 2020s and beyond. MPEG-2 video is clear, the last pool patent having expired 2018-02-13, so the defensible subset is a DVB-S or DVB-S2 demodulator and transport stream demultiplexer that identifies the elementary streams and decodes MPEG-2 only | AVC in the late 2020s; HEVC later |
| DVB-T, DVB-T2, ATSC 1.0 and 3.0, ISDB-T | Live pools on the physical layer profiles and on every codec they carry. Also outside what this receiver is for | Not scheduled regardless |
| C-V2X PC5 | Thousands of declared standard-essential patents from Qualcomm, Huawei, Ericsson, Nokia and LG, licensed FRAND through Avanci and Sisvel, with 2015-onward filings running to 2035. The 3GPP specifications are free, which does not resolve it. Whether a receive-only decoder practises the asserted claims is arguable rather than obviously no | Listed under open legal questions below as well |

WHAT THIS TABLE USED TO SAY, from 2026-09-21 to 2026-09-23, in a first row,
"DMR Tier I/II/III", naming Motorola's US8306071 and ending "A narrowing
limitation in claim 1 gives a receive-only decoder an argument, and an
argument is the reason this is excluded rather than shipped", with 2027-02-19
to revisit. The patent and the argument are unchanged. The owner put DMR back
in scope on 2026-09-23 and accepted the risk, so the row moved to the land
mobile table above; the query section below has both.

### Excluded for want of anything to work from

Under the current rule, "no specification" alone is not decisive: measurement is
a permitted source. These are the cases where measurement does not reach the
answer either, or where the only other route is reading somebody's decoder.

| Mode | Reason |
| --- | --- |
| AMBE and AMBE+2 voice, everywhere it appears | No published algorithm at any price, and a vocoder cannot be recovered by observing its output. Licence is in the silicon. Affects DMR, dPMR, NXDN, P25 Phase 2, D-STAR, Fusion, Tetrapol, BGAN, GMR-1, GMR-2, ACeS, Aero-H, Iridium and the rest of the mobile satellite set. Permanent, not pending |
| ReFLEX 25 and ReFLEX 50 | The Motorola ReFLEX specification went to licensees under non-disclosure. There is no ITU, ETSI, TIA or ARIB equivalent: ARIB STD-T43 covers one-way FLEX only. Every existing decoder came from hardware. The outbound channel can still be classified |
| Iridium SBD payload, voice and Certus / NEXT | No published air interface, a barred codec, and live Iridium patents from 2012 onward. Three bars |
| Starlink Ku-band user downlink | No specification, live SpaceX patents filed continuously since about 2015, and a 240 MHz channel at 11 GHz no supported radio can capture. Three bars |
| OneWeb, Kuiper, Globalstar duplex and simplex, Inmarsat Global Xpress, IsatData Pro | No operator has published an air interface and none is mirrored by ETSI, TIA or the ITU. Globalstar simplex is the one people ask about, because SPOT trackers are everywhere, and the answer is still that nobody has published the format |
| Proprietary DMR trunking (Capacity Plus, Connect Plus, Capacity Max, Hytera XPT) | No document for the trunk-following logic, and live vendor patents from 2010 to 2015. Listed because a Capacity Plus system decodes as ordinary DMR bursts and somebody will ask why the calls are not followed |
| PDT (Chinese police digital trunking) | The GA/T standard is not distributed outside China and no route to a copy was found. The physical layer falls out of the DMR demodulator, so a PDT carrier will be identified as DMR-like 4FSK, which is the correct behaviour and the limit of what should be claimed |
| EDACS and ProVoice | Nothing published, at any price. Everything in circulation about the 9600 bit/s GFSK control channel came from reverse engineering. ProVoice adds an AMBE variant on top. The honest ceiling is classifying the control channel as an unidentified FSK carrier |
| iDEN | Motorola proprietary throughout its life; VSELP not published in obtainable form. Nextel shut the US network in 2013 |
| ARQ-E, ARQ-E3 and FEC-A | No free or purchasable document specifies the waveform; ITU-R F.342-2 covers the ARQ-M ancestor and not these. Modulation and baud rate can still be labelled |
| Link-4A (TADIL-C) | MIL-STD-188-203-3 could not be confirmed to exist in the public catalogue, and it is a UHF link that does not belong in an HF family |
| STANAG 4479 | The 800 bit/s vocoder specification is not confirmed obtainable, and the carrier is a frequency-hopping ECCM system whose hop sequence is not published, so a perfect vocoder would have nothing to decode |
| TSVCIS | NSA-managed, no public copy and no reseller listing. Listed because it is the current US tactical secure voice codec |
| Codan 3012 and 3212 | No waveform specification; the reference manual documents operation and configuration. The 80 baud chirp is distinctive enough to flag a probable Codan link |
| Remote keyless entry (KeeLoq and successors) | KeeLoq was never published as a specification and is known only through cryptanalysis, which is a published attack and not a published specification. The payload is encrypted, which the scope rule bars independently |
| Inmarsat-C air interface via the System Definition Manual | The SDM is confidential and is not a usable source. This is not an exclusion of Inmarsat-C, which is included above by measurement; it is an exclusion of that route to it |
| KG-STV | One author's software, no published format description |
| ROS and Opera | No specification located. Both are closed-source distributions from the author of VARA. This rests on an absence rather than a document that was read, and the search was not completed |
| FreeDV 2020 and RADE | The code is two-clause BSD and the licence on the trained model weights was never established. A weights file has no document to cite and no clause to check against when it misbehaves, and a neural vocoder cannot be clean-roomed from an architecture paper. Two surveys returned opposite verdicts on RADE and both conceded the weights question. Revisit if the weights carry a GPL-3-compatible grant. Whether a model file can be a dependency at all was settled on 2026-10-03, below the table |

**A model file is an acceptable dependency, by the owner's decision of
2026-10-03.** Whisper's weights, `ggml-large-v3-turbo.bin`, MIT, are the
project's first, accepted for speech to text (`docs/rpc.md`, "Speech to
text"; `docs/clean-room.md` has the position). They are downloaded on first
use rather than shipped, and pinned by URL commit, size and SHA-256 in
`core/transcribe/model_store.h`. That leaves FreeDV 2020 and RADE with one
question instead of two: the licence on their weights. The rest of the row's
reasoning stands, and applies to Whisper as much as to RADE: a weights file has
no document to cite, and the transcriber has nothing to check Whisper's output
against but Whisper's own scores and a published list of its failures
(`core/transcribe/transcriber.cpp`).

WHAT THE FREEDV 2020 AND RADE ROW USED TO SAY, at its end: "Revisit if the
weights carry a GPL-3-compatible grant and the project decides a model file is
an acceptable dependency". The second condition was met on 2026-10-03.

### Excluded on scope or reach

| Mode | Reason |
| --- | --- |
| eCall and ERA-GLONASS in-band modem | Not a radio mode. The audio it modulates travels inside a ciphered GSM, UMTS or LTE voice call, so reaching it means breaking the cellular link first. Separately, the free 3GPP TS 26.268 reference C carries 3GPP copyright and is not GPL-compatible |
| Himawari HRIT / HimawariCast | A commercial Ku-band DVB-S2 multicast to a dish and a DVB card, not an SDR-receivable direct broadcast. The free JMA HRIT mission specification remains the right reference if xRIT file handling is ever pointed at a feed from disk |
| GVAR and analog geostationary WEFAX | Nothing transmits either. GOES-15 was the last GVAR source and left operational service in 2020 |
| ANT+ | The specification exists only under an adopter agreement carrying field-of-use terms, which is a further restriction section 7 will not carry. Excluded on the licence of the document rather than on patents, which is unusual and is recorded so nobody re-derives it |
| Maritime 9 GHz radar SART | A radar transponder answering an X-band pulse with a swept response. No data and no realistic front end. Anyone asking for SART means AIS-SART, which is included |

### Open legal questions

Four items turn on a lawyer reading claim language or licence terms rather than
on a lookup. None should be started before an answer.

| Mode | The question |
| --- | --- |
| Galileo E1 Open Service, OSNMA and HAS | The OS SIS ICD is a free download and a licensed document. It forbids alteration or translation without written permission and grants use of the information including the spreading codes under a licence agreement, and Annex H lists patents the EU holds licences on, with six rows added at Issue 2.1. Two questions: whether reproducing the E1-B and E1-C memory code tables inside GPL-3.0 source that recipients may modify is compatible with that licence, and whether the EU's patent authorisation satisfies section 11 or is a section 7 further restriction. This is the only document in the whole survey whose own licence plausibly reaches implementation rather than copying |
| MIL-STD-188-110C Appendix D wideband HF | The document is free with unlimited distribution and the copyright position is clean. The waveform family was standardised in 2011 and Harris and Rockwell Collins filed wideband HF work between roughly 2007 and 2015, running to 2027 through 2035. Whether any live claim covers a receiver was not established. Everything else in the 110 family proceeds without waiting for this |
| DVB-S2 | Sisvel runs a DVB-S2 licensing programme. The underlying LDPC and APSK filings from 2001 to 2003 have run their twenty years, and a live programme means somebody believes otherwise. Section 11 means taking the licence would not solve it, because a licence that stops at the licensee bars conveyance rather than permitting it. Whether the programme reaches receive-only decoder software is the question |
| C-V2X PC5 | Listed under live patents above. The specification being a free 3GPP download is not clearance |

**DMR is in scope again, by the owner's decision of 2026-09-23, with the risk
the ETSI IPR query found accepted.** The query is recorded in full below,
under "The ETSI IPR query, and what it did to DMR", and the decision after it.
The short version: TS 102 361-1 and -2 carry four licensing declarations from
Motorola, the most recent signed 2024-05-16, and one of the declared patents
is live until 2027-02-19 with a claim whose first step is receiving a burst
and comparing its synchronisation pattern. That is the operation a framing
decoder exists to perform, and the one `core/decode/dmr.cpp` performs.

WHAT THIS PARAGRAPH USED TO SAY, from 2026-09-21: "**The ETSI IPR database was
queried on 2026-09-21. DMR is now excluded.**", ending "DMR moves to the
live-patent exclusion table." The query stands; the exclusion it led to was
reversed two days later.

TETRA was queried at the same time and came back clean, which is why EN 300
392-2 work proceeded.

## The ETSI IPR query, and what it did to DMR

Done 2026-09-21, against the question this document had been carrying open.
Recorded at length because the answer removed a scheduled mode, and because
the next person to ask should be able to check the work rather than repeat it.

**How it was queried.** `ipr.etsi.org` is an ASP.NET form and its search
cannot be driven without a browser session, so the query went against the
bulk export the site itself publishes at `docbox.etsi.org/IPR/Open`:
`ISLD-export.zip`, the Information Statement and Licensing Declarations, which
unpacks to a 2.4 GB CSV, and `GD-export.zip`, the General Declarations. Both
were retrieved on 2026-09-21 with the file timestamps ETSI's directory listing
gave as 05:07 and 05:08 that morning. The CSV carries one row per declared
patent family member, with the ETSI deliverable number in its own column, so a
deliverable is queried by matching that column.

**One thing about that column cost a wrong answer for ten minutes and is worth
stating.** The deliverable number carries the part suffix. Matching `102 361`
as a whole field returns nothing at all, because every row reads `102 361-1`
or `102 361-2`, and "nothing found" is exactly what a reader hoping for a
clean answer is primed to accept. A prefix match returns 111 rows.

### What is declared against TS 102 361

Four licensing declarations, all Motorola, plus one general declaration.

| Declaration | Signed | Against | Declared patents |
| --- | --- | --- | --- |
| ISLD-200505-004 | 2005-04-29 | TS 102 361-1 | US7203207, US7339917, and the continuation US8306071 |
| ISLD-200607-003 | 2006-06-19 | TS 102 361-1 | US7200129, US7499441 |
| ISLD-202405-025 | 2024-05-16 | TS 102 361-1 and -2 | US7500170 (sections 6.2, 7.1.1, 8.2.1.0, B.3.12, 9.3.6), US8503409 (section 5.2.2.4), US8457104 and US8599826 (sections 6.2.1.0, 6.2.2, 6.2.3) |
| ISLD-202405-026 | 2024-05-16 | TS 102 361-2 | GB2451015 (sections 6.3.1 to 6.3.4, 7.2.20) |
| GD-200407-001 | 2004-06-25 | work item DTS/ERM-TG32DMR-052 | general declaration, no patents enumerated |

Legal status and anticipated expiry, read from each patent's record on
2026-09-21:

| Patent | Status | Anticipated expiry | Claim 1 begins |
| --- | --- | --- | --- |
| US7203207 | Expired | ran from a 2004-03-12 filing | receiving a burst comprising payload and a synchronization field |
| US7339917 | Expired | same filing | receiving a plurality of bursts comprising a superframe |
| US7200129 | Expired | same filing | at a subscriber, in a TDMA system |
| US7499441 | Expired | 2026-01-23 | determining that a subscriber unit is provisioned for polite access |
| **US8306071** | **Active** | **2027-02-19** | **receiving a burst comprising payload and a synchronization field, then comparing the received synchronization pattern against a first and a second known synchronization pattern** |
| **US7500170** | **Active** | **2027-03-16** | generating a data block with an error detection portion |
| **US8503409** | **Active** | **2031-02-19** | assigning a radio to transmit on a desired time slot |
| **US8599826** | **Active** | **2031-01-17** | receiving, by a first radio, direct mode RF transmissions defining time slot boundaries |
| **US8457104** | **Active** | 2031, same family | receiving a communication defining time slot boundaries |
| **GB2451015** | **Active** | ran from a 2007-03-02 filing | interrupting a transmitting subscriber |

### Why that excludes DMR

The inclusion rule at the top of this document is that no live third-party
patent reads on a receiver. Four of the six live patents are about
transmitting: assigning a slot to transmit on, generating a data block,
interrupting somebody else's transmission, and the pair on how a radio brings
its own transmissions into slot alignment. A receive-only decoder does none of
those, and on those four the rule is satisfied on the merits rather than by
argument.

US8306071 is the one that is not. Its claim 1 is a method whose steps are
receiving a burst, reading the synchronisation field, comparing that pattern
against two known patterns, and selecting an operating mode from the result.
That is a description of burst synchronisation, which is the first thing any
framing decoder for this mode has to do and the specific thing task #39 asked
for. It is declared essential to TS 102 361-1 by its own holder. It is live
until 2027-02-19.

There is a narrowing limitation. The claim requires two known patterns
associated with two different air interface types at two different
frequencies, and dependent claim 3 names those as FDMA and TDMA. A decoder
that correlates only the DMR patterns and never selects between air interface
types has an argument that it does not practise the claim. The argument is
real and it is an argument, which is the problem: whether a receive-only
framing decoder infringes turns on a claim limitation, and resolving that is a
claim construction rather than a reading of a database.

The two paragraphs below are kept as they were written on 2026-09-21, and
what they used to say about DMR's status is now false: the exclusion they
conclude with was reversed on 2026-09-23. The subsection after them has the
decision that reversed it. The reasoning in them is not withdrawn.

So the position is not "DMR infringes". It is that DMR is the one mode in this
document where the receiver question is genuinely open, on a live patent,
against a holder who re-declared essentiality sixteen months ago. For a named
LLC signing and publishing binaries that is not a call to make from a CSV. The
mode is excluded until the patent expires on 2027-02-19, at which point the
whole question disappears along with it, or until somebody qualified says
otherwise.

This is also the answer to the inconsistency this document has been carrying.
DMR was included on "no patent identified" while mioty and Sigfox were
excluded on found pools, and that difference really was search effort rather
than a legal distinction. Having done the search, the verdicts now agree with
each other.

### The owner's decision, 2026-09-23

DMR is back in scope. The owner chose to accept the risk on US8306071, over
waiting for it to expire on 2027-02-19 or identifying DMR by its structure
alone, and Revenant decodes DMR's framing and metadata clean-room from ETSI TS
102 361-1 V2.7.1 and TS 102 361-2 V2.5.1: the syncs, the CACH and its Short
LC, the slot type and colour code, the Full LC with its talkgroup and radio
IDs, the CSBK and the data headers. AMBE+2 voice stays out, because no
document for it exists, which is the voice table's reason and not the
patent's. The decision is recorded in Reliquary under the topic
revenant.modes.dmr.

What the decision does not change is recorded as plainly as what it does.
The patent is live and nothing here argues it away. The four transmitting
patents are still satisfied on the merits, since `core/decode/dmr.cpp`
transmits nothing, and `core/dsp/synth/dmr_mod.cpp` is a test instrument that
renders to a buffer and never reaches an antenna. The narrowing limitation
above is a fact about what the decoder does, not a defence it relies on: it
correlates the ten patterns of Table 9.2, all of them DMR's own, and does not
select between an FDMA and a TDMA air interface. The inclusion rule at the top of this document
is unchanged for every other mode, which is why the paragraph under it names
DMR as the one exception and mioty and Sigfox stay excluded.

### What the same query said about the other three modes

**TETRA, EN 300 392-2: clear.** Eight declarations against the deliverable,
from Alcatel, Ericsson, Nokia, Motorola, Orange and Sepura. The newest
declared basis patent is GB2415332, filed 2005-04-27 and showing expired;
every other one has a filing date between 1987 and 2005. Nothing declared
against EN 300 392-2 is inside a twenty-year term as of 2026-09-21. EN 300
392-7 carries live Motorola declarations, and that part is the security
specification this project does not implement.

**P25 Phase 1 and D-STAR are not ETSI deliverables**, so this database is
silent on both by construction and their rows above are unchanged by it. This
query answers a question about ETSI declarations and nothing wider.

**The other three deliverables this document asked about while there.**
TS 102 744 (BGAN) carries 338 declarations from Inmarsat, TS 101 376 (GMR-1)
carries 551 from Hughes Network Systems and others, and ETS 300 133-4 carries
none. All three are metadata-only rows whose payloads are barred for other
reasons, so none of this changes a verdict. TS 102 490 (dPMR) carries 70
Motorola declarations and TS 102 658 carries 72 from Motorola and Kenwood;
neither mode is built, and both now need the same patent-by-patent pass DMR
just had before either is scheduled.

## Done

**P25 Phase 1, D-STAR DV and TETRA V+D, physical layer and framing.** Three
demodulator modes, `p25p1`, `dstar` and `tetra`, appended to the Demod enum
after `cw`. They are complex taps rather than detectors: the receiver's fine
stage mixes the carrier to DC, filters the mode's channel and resamples to the
rate its decoder was built at, and the demodulator kernel hands that complex
baseband out unchanged. `core/decode` recovers the symbols on the host.
`core/engine/vrx.h` explains the path at `is_complex_tap`.

| Mode | Channel filtered | Delivered at | Samples per symbol |
| --- | --- | --- | --- |
| P25 Phase 1 | 12.5 kHz | 48000 S/s | 10 of TIA-102.BAAA-A clause 9.2's 4800 |
| D-STAR DV | 6 kHz | 48000 S/s | 10 of JARL Ver 7.0 clause 4.1.2 b's 4800 bit/s |
| TETRA V+D | 25 kHz | 72000 S/s | 4 of EN 300 392-2 clause 5.3's 18000 |
| DMR | 12.5 kHz | 48000 S/s | 10 of TS 102 361-1 clause 10.2.1's 4800 |

WHAT THIS PARAGRAPH USED TO SAY, until 2026-09-22: "They are complex taps
rather than kernels: the receiver hands out baseband at the channel's width".
They were the graph's raw tap then, one coarse channel at the channel rate
with the carrier wherever the grid's residual left it, and nothing had
measured what the decoders made of that. `tests/engine/test_engine_dv.cpp`
does now, on a 2.304 MS/s capture with the carrier 5 kHz off a channel centre,
at the noise density `tests/decode` uses: at the low signal to noise point
the fine stage gives P25 a symbol error rate of 0.0069 where the raw tap gave
0.708, and D-STAR 0.0023 and TETRA 0.0375 where the raw tap did not lock at
all.

| Mode | File | Document | What comes out |
| --- | --- | --- | --- |
| P25 Phase 1 | `core/decode/p25p1.cpp`, `core/decode/p25_tsbk.cpp` | TIA-102.BAAA-A clauses 5, 6.2, 7, 8 and 9 and the clause 10.2 to 10.4 annexes, with reserved values from TIA-102.BAAC; the control channel from TIA-102.AABB-B clauses 4 and 5 and TIA-102.AABC-B clauses 2, 4.2 and 6.2 | Frame sync, the Network Access Code and Data Unit ID through the (63,16,23) BCH code; from a header data unit the talkgroup, the manufacturer, the key and algorithm identifiers and the message indicator; from LDU1 the Link Control word, and from LDU2 the encryption sync; the low speed data; the nine IMBE voice frames of every LDU, which `P25Voice` turns into 8 kHz PCM; and on a trunking control channel every Trunking Signaling Block of every TSDU through the rate 1/2 trellis code and its CRC, with the group voice grants, the identifier updates and the RFSS, network and adjacent status broadcasts parsed. See the P25 voice and control channel paragraphs below |
| D-STAR DV | `core/decode/dstar.cpp` | JARL Ver 7.0 clauses 4.1.1, 4.1.2, Ap1 and Ap2 | Bit and frame sync, the radio header through the rate 1/2 convolutional code and the 24 bit interleave, with all five callsigns and the flag byte, and the voice and data frames with the resynchronisation signals marked |
| TETRA V+D | `core/decode/tetra.cpp` | EN 300 392-2 V3.8.1 clauses 5, 8.2, 8.3.1.2, 9.4.4 and 21.4.4.2 | Burst sync from the synchronisation training sequence, and the SYNC PDU off the broadcast synchronisation channel: colour code, system code, timeslot, frame and multiframe number, and the country and network codes |

TETRA stops at the bits: its voice is ACELP and is not implemented. D-STAR's
voice is AMBE, which nothing here decodes; since 2026-10-02 each 72-bit voice
frame goes to a loaded vocoder plugin that serves `dstar` or `ambe`, and with
none the receiver is silent (`core/rpc/plugin_voice.h`, and the note under the
voice table above). P25 goes through to audio, and the paragraph below says
how.

WHAT THE FIRST SENTENCE USED TO SAY: "D-STAR and TETRA stop at the bits:
D-STAR's voice is AMBE and has nowhere to go". True until "Route D-STAR and
DMR voice to a vocoder plugin" on 2026-10-02.

`core/dsp/synth/dv_mod.cpp` is the transmitter for all three, written from the
same clauses, and `tests/decode/test_p25p1.cpp`, `test_p25p1_voice.cpp`,
`test_dstar.cpp` and `test_tetra.cpp` are the round trips. Each measures a bit
or symbol error rate at a high and a low signal to noise and reports the
figure rather than asserting it tight. `bench sweep --mode p25p1`, `dstar` and
`tetra` extend the same measurements to curves, 128 transmissions a point, in
SNR in 2500 Hz: a bit error rate of 0.01 at 17.9 dB for P25, 17.2 dB for
D-STAR and 20.2 dB for TETRA, which across the whole sample rate is 5.0 dB,
4.4 dB and 5.6 dB ([sensitivity.md](sensitivity.md)). D-STAR's transmissions
at its test's low point read about 0.055 each until the bit timing slips on
the discriminator's clicks; since the shaped output is limited at 2.5 times
the deviation, 14 of 128 slip at 15 dB in 2500 Hz rather than 56, and the
curve reads 0.082 there. WHAT THIS PARAGRAPH USED TO SAY: "17.3 dB for
D-STAR", "4.5 dB" across the sample rate, and "twelve single transmissions at
the same level gave 0.055 to 0.357, and the curve reads 0.168 at 15 dB".

**DMR, framing and metadata, 2026-09-23.** A fourth complex tap, `dmr`,
appended after `tetra`, through the fine stage at 48000 S/s in its 12.5 kHz
channel, and `core/decode/dmr.cpp` from ETSI TS 102 361-1 V2.7.1 and
TS 102 361-2 V2.5.1, with Annex B's codes in `core/decode/dmr_codes.cpp`.

| What | Document | What comes out |
| --- | --- | --- |
| Burst sync | TS 102 361-1 clause 9.1.1, Table 9.2 | All ten patterns, voice told from data by the sign of one correlation as the table's note says, each burst timed from its own sync to a fraction of a sample, and the carrier offset and deviation fitted to its 24 known symbols |
| The CACH | Clauses 4.5, 6.3, 7.1.4, B.2.3 and B.4.1 | The TACT through Hamming (7,4), whose TDMA Channel bit names each burst's timeslot, and the Short LC over four CACH bursts through its Hamming (17,12,3) product code and 8-bit CRC, the Activity Update parsed |
| Slot type and EMB | Clauses 9.1.2 and 9.1.3, B.3.1 and B.3.2 | Colour code and Data Type through Golay (20,8); colour code, PI and LCSS through the quadratic residue (16,7,6) code |
| Full LC | Clauses 7.1.1 to 7.1.3, B.1.1, B.2.1, B.3.6, B.3.11 and B.3.12; TS 102 361-2 clause 7.1.1 | From the voice LC header and the terminator through BPTC (196,96) and Reed-Solomon (12,9) with their masks, and from a superframe's four embedded fragments through the Hamming (16,11,4) product code and its checksum: FLCO, FID, Service Options with the emergency and privacy bits, group or individual destination, source |
| CSBK and MBC header | Clauses 7.2 and 7.4; TS 102 361-2 clause 7.1.2 | Opcode, FID and the raw octets through BPTC and the masked CRC-CCITT, with the target, source, options and preamble fields of the standard opcodes parsed |
| Data headers | Clause 8.2.1 | Format, SAP, group flag, destination and source, blocks to follow, and a proprietary packet's second block |

Each timeslot has its own state: the voice superframe, the embedded LC it is
gathering, the level fit it slices with. The slot number comes from the CACH on
a base station channel and from the sync on TDMA direct mode, and reads 0 on an
MS transmission, whose sync names none. A PI header and the Privacy bit are
reported and nothing is decrypted. The decoder hands out the 216 vocoder bits
of each voice burst, which are three 72-bit AMBE+2 frames (TS 102 361-1
clauses 4.2.2 and 6.1), and nothing in this tree renders them. Since
2026-10-02 a DMR receiver's audio passes them, a frame at a time, to a loaded
vocoder plugin that serves `dmr` or `ambe2`, following one slot's call at a
time; a call a PI header or the Privacy bit marks private is silence and never
reaches the plugin (`core/rpc/plugin_voice.h`).

WHAT THE LAST SENTENCE USED TO SAY: "The 216 vocoder bits of each voice burst
are handed out and not rendered, since they are AMBE+2." Until "Route D-STAR
and DMR voice to a vocoder plugin" on 2026-10-02 nothing took them; now a
loaded plugin renders them, though still nothing in this tree does.

`core/dsp/synth/dmr_mod.cpp` is the transmitter, written from the same clauses,
and `tests/decode/test_dmr.cpp` the round trips: every Annex B code against its
printed matrix, the Idle message against figure D.1's worked example, and
bursts of every kind on a base station channel, on both timeslots at once and
on a direct mode channel with the carrier off between bursts.
`test_dmr_blocking.cpp` decodes one capture identically whole and in eight
blockings. The decoder has no SymbolSync: a burst carries its timing in its
sync, and the square-law estimator lost the symbol phase across the carrier-off
gaps of a direct mode call. `bench sweep --mode dmr` counts CSBKs that come
back whole, and crosses a frame error rate of 0.01 at 18.8 dB in 2500 Hz
([sensitivity.md](sensitivity.md)).

**P25 Phase 1 voice.** The header word, the Link Control word and the
encryption sync word each sit under a shortened Reed-Solomon code over
GF(2^6), clause 5.9: (36,20,17) under the (18,6,8) Golay code in the header,
(24,12,13) and (24,16,9) under the (10,6,3) shortened Hamming code in LDU1 and
LDU2. `core/decode/dv_codes.cpp` has all three, encode and errors-and-erasures
decode; an inner word the Golay or Hamming code detects but cannot correct
reaches the Reed-Solomon code as an erasure. The field arithmetic, the three
generator polynomials and all 48 rows of the three printed generator matrices
are checked against clause 5.9's own tables. From LDU1 comes the Link Control
format, the MFID, and for formats $00 and $03 with the standard MFID the
emergency bit, the talkgroup, the source and the destination; formats $80 and
$83 are reported as encrypted Link Control with nothing parsed. From LDU2 come
the message indicator, the algorithm and the key. So a receiver joining a call
mid-transmission has the talkgroup and the source at the end of its first
whole LDU1, at most 540 ms after it tunes in, where before it had them only
from a header that a late joiner never sees.

Each LDU's nine 144-bit voice frames are cut out at the positions the clause
10.3 and 10.4 annexes give and handed, in the Table 5-1 order, to
`core/decode/imbe.cpp`, which owns the Golay and Hamming codes inside the
frame. `P25Voice` collects the PCM. It passes a frame to the vocoder only
once a header or an encryption sync word has said ALGID $80; a receiver that
joins at an LDU1 holds that unit's nine frames until the LDU2 behind it
settles the question. An encrypted call, by header, by encryption sync, or by
Link Control format $80 or $83, is reported with its talkgroup and network and
none of its frames reaches the vocoder.

Measured in `tests/decode/test_p25p1_voice.cpp` on a two-superframe call
through the same channel model as the other round trips: at 30 dB every frame
arrives intact and the PCM is the vocoder's output for the transmitted frames
sample for sample; at 8 and 5 dB every LDU frames and every Link Control and
encryption sync word decodes, with a raw voice channel bit error rate of
0.00019 and 0.0116 and no frame repeated or muted; at 3 dB one LDU of four
frames, and the frame sync search is the limit there rather than any code.

**P25 Phase 1 trunking control channel.** A control channel sends Trunking
Signaling Data Units, DUID $7 (TIA-102.AABB-B clause 4.2): a frame sync and
NID, then one to three 12-octet Trunking Signaling Blocks, each through the
CAI's rate 1/2 trellis code and the Table 7-4 interleave (TIA-102.BAAA-A
clause 7), null padded to the end of a 70-bit micro-slot. `P25Phase1` reads a
TSDU a block at a time, following each block's LB flag, and checks each
block's CRC, the clause 6.2 header CRC. `core/decode/p25_tsbk.cpp` parses the
TIA-102.AABC-B messages that describe a site and its calls: the group voice
channel grant, the grant update and the explicit grant update, the identifier
update in both its layouts and as IDEN_UP_VU, and the RFSS, network and
adjacent status broadcasts. The identifier update is what turns a channel
number into a frequency, and the `p25p1` decoder remembers the last one heard
for each identifier so the grants that follow come out with
`channel_frequency_hz`. Every other block comes out too, by opcode and by its
AABC-B alias, and a protected block (AABB-B clause 5.2) or one under a
manufacturer's MFID is reported as such with nothing past the MFID read. The
decoder is hard decision; the code's free distance is five bits, so any two
bit errors in a block are corrected, which `tests/decode/test_p25_tsbk.cpp`
checks with the rest. Multiple block trunking packets (DUID $C, AABB-B clause
6) are not decoded. Nothing here decrypts; a grant whose service options say
the call is protected is reported as an encrypted call.

**P25 Phase 1 decodes the same however its input is blocked.** Until
2026-09-22 it did not. `P25Phase1::process` restarted its discriminator
against 1+0i and its receive filter from zeros at every call, and took each
call's mean as the carrier offset, so the engine's block length changed what
came out: through `tests/rpc/test_rpc_decode.cpp`'s engine and adapter, six
headers sent gave four back at 16384-sample blocks and one at 65536. The
discriminator and filter now carry their state across calls
(`core/decode/dv_phy.h`, `FmDiscriminator` and `RealFir`), the sync search is
a centred correlation that no carrier offset moves, and each data unit is
sliced after a least-squares fit of its own 24 sync symbols gives the offset
and the deviation. The same case now gives all six at both block sizes.
`tests/decode/test_p25p1_blocking.cpp` decodes one capture, a carrier 350 Hz
off with baseband DC and noise at 20 dB, whole, in eight blockings down to one
sample a call, and holds every data unit identical to the whole-capture
decode; its header records what each of the three steps cost, the filter
restart most. A second case frames a carrier 1200 Hz off with no NID bit
corrected, which the old search did not frame at all once the per-call mean
was gone. `tests/engine/test_engine_dv.cpp` holds the fine stage's output
identical at 16384 and 65536-sample engine blocks. What is still open: the
first header of a transmission is lost in some captures, in every blocking,
at decode level when baseband DC and noise are both present before it and
through that engine case's 2.304 MS/s grid, and the cause is not
established.

**D-STAR and TETRA decode the same however their input is blocked**, since
2026-09-23. Neither had been measured for it. `DStar::process` restarted its
discriminator and receive filter at every call, took the call's mean as the
carrier offset and divided by the call's peak; `Tetra::process` restarted its
matched filter. Both now carry that state (`FmDiscriminator`, `RealFir` and a
new `ComplexFir` in `core/decode/dv_phy.h`), D-STAR fits each frame sync's 15
known bits for the offset and the deviation before slicing and refits on every
clause 4.1.2 d resynchronisation signal, and both number what they find from
the start of the stream. `tests/decode/test_dstar_blocking.cpp` and
`test_tetra_blocking.cpp` decode one capture each, a carrier 350 Hz off with
baseband DC and noise at 20 dB, whole and in seven or eight blockings down to
one sample a call, and hold every blocking identical to the whole capture.
Before: D-STAR's three transmissions gave 0 to 3 headers and 0 to 161 of 90
voice frames depending on the blocking, none at one sample a call; TETRA's 36
bursts gave 4 verified at 1024-sample calls and 28 at 4096. Now all three
headers, all 90 frames and all 36 bursts in every blocking. D-STAR also slices
a carrier 1000 Hz off with every frame whole.

Two D-STAR faults came out of the same measurement. The clause 4.1.2 h last
frame was looked for 40 bits past where the clause puts its tail, so no
transmission ever ended and a whole capture's first header took every later
transmission's bits as its voice: whole, that capture gave one header of
three. And a transmission was reported once, as its header decoded, with only
the voice frames already buffered. A transmission now arrives in pieces set by
its own structure, the header with the first superframe of 21 voice frames and
a piece per superframe after it, and closes on the last frame, or when a frame
where clause 4.1.2 c puts the resynchronisation signal does not carry it, or on
`flush()`. The bit error rates did not move: D-STAR 0 at 30 dB and 0.0563615
at 2 dB over 11994 bits, and through the engine's fine stage 0 and 14 of 5982;
TETRA 0 at 30 dB and 0.0275206 at 2 dB over the 18 of 24 bursts found; all
identical before and after. Those cases read `DStar::last_bits()`, which still
slices a call's bits at that call's mean, because a stream of random bits has
no frame sync to fit; `dstar.h` says so at the declaration.

The transmitter computes every parity the clauses specify, so its header and
LDUs are what a radio expects to receive. What it lacks is an IMBE encoder:
it carries frames a caller has built, the tests build theirs from chosen
quantizer values through TIA-102.BABA section 7.1 and `imbe_pack_frame`, and
it asks for whole LDUs because clause 8.2.3's silence padding would need an
encoder to produce.

Three things these modes do not reach, recorded so nobody assumes otherwise.
P25's terminator with Link Control carries its word under the (24,12,8)
extended Golay code and is framed but not decoded, and Link Control formats
beyond $00 and $03 are reported as octets, because TSB102.AABF, which defines
them, is not in hand. TETRA stops at the broadcast synchronisation channel and
does not follow the traffic or signalling channels that the recovered colour
code would unscramble. And the JARL standard states no bandwidth-time product
for D-STAR's GMSK, so the value at both ends of that round trip is this
project's choice and the round trip cannot see it.

RDS and RBDS, shipped. `core/decode/rds_bits.cpp` is the physical layer, from a
recovered 57 kHz subcarrier to a differentially decoded bitstream, implementing
EN 50067:1998 clauses 1.1 through 1.7 in full with no deviation.
`core/decode/rds_groups.cpp` is the baseband coding and group layer, written
from EN 50067:1998, NRSC-4-B (April 2011), NRSC-4 (April 1998) and Kopitz and
Marks, and for the TMC attribution alone the free preview of ISO 14819-1:2021
and the RDS Forum's ODA register as NRSC publishes it (2020-07-14).

EN 50067 is cited rather than IEC 62106 because it is the text somebody opened.
Clause numbering moved on the way into the IEC series: EN 50067 clause 3.1.5.1
is IEC 62106 clause 6.1.5.1, and the code carries EN 50067's numbers. Nothing in
the physical layer is region dependent, because NRSC-4 clauses 1.1 through 1.7
are word for word identical to EN 50067's. One demodulator serves both regions
and the RDS/RBDS split does not begin until the group layer.

What the group layer does with each group type. 0A and 0B, 1A, 2A and 2B, 3A,
4A, 10A, 14A, 14B and 15B are decoded into fields. 5A and 5B (transparent data
channels, with the channel address), 6A and 6B (in-house, which clause 3.1.5.9
says consumer receivers should ignore), 7A (radio paging, with the Annex M
Table M.2 header and nothing past it), 8A (TMC) and 9A (emergency warning) are
counted and their payloads kept raw, because EN 50067 defines none of those
payloads past the block 2 header. Any group Table 6 lets an Open Data
Application take is attributed to the application a 3A group announced on it,
counted and kept raw on that announcement, and a nonzero AID takes the group
from its Table 3 feature, as clause 3.1.5.4 says. 1B and 15A are dropped.
Programme type name segments carry a correction mark on the same rule as PS
and RadioText.

**RDS-TMC is attributed, counted and confirmed, and not decoded.** EN 50067
clause 3.1.5.12 says 8A's 37 bits "are defined by CEN". The only text of ISO
14819-1 read here is the free preview of the 2021 edition, which reaches
clause 3.2: it gives the widths of the five basic fields (event 11 bits,
location 16, direction and extent 4, duration 3, diversion 1) and no
positions, which are clauses 7.2, 7.4, 7.5 and 9.3 to 9.5. So there is no event
code, location code, extent, direction, duration or diversion bit in the
output, single-group and multi-group messages are not told apart, and neither
is system or tuning information. What there is: the service recognised by the
three ALERT-C AIDs in the RDS Forum register (0x0D45 testing, 0xCD46, 0xCD47),
TMC identification from type 1A variant 1 kept raw, and every distinct 37-bit
payload kept once with its reception count, confirmed on a second identical
reception per the preview's Introduction 0.3. The raw location code, which
the RDS-TMC note under the aeronautical and vehicle table says to emit, is
somewhere in those
bits and is not picked out, because which 16 bits it is was not in any
document read. Buying ISO
14819-1 is what turns this into the small job the table says.

**RDS2 is not the cheapest new capability here, and nothing of it is
implemented.** The free preview of IEC 62106-1:2018 confirms the four
subcarriers and their frequencies (clause 4.3), that streams 1 to 3 are locked
to stream 0 (clause 4.2), and, in clause 3.3.2, a group type C "referenced by
a header byte and 7 data bytes", which is not a shape rds_groups.cpp parses.
Everything a demodulator needs is past the end of the preview: subcarrier
phase and level (4.4, 4.5), the modulation and clock (4.6, 4.7), differential
coding and shaping on the new streams (4.8, 4.9), the symbol phase shifts
between streams (4.10), the baseband coding and cross-stream block sync
(5.1 to 5.3), the transmission options (6) and the offset words in Annex A.
The logo and file transfer are in other parts of the series, not identified.

What it would take, with the document: rds_bits.h fixes its subcarrier at
57000 Hz, the third harmonic of the pilot, so it would need the subcarrier as a
parameter (66.5, 71.25 and 76 kHz are 3.5, 3.75 and 4 times 19 kHz) and one
instance per stream on the same composite. The composite rate is not the
obstacle: 171000 S/s reaches 85.5 kHz, and the highest stream's edge is 78375
Hz if the new streams keep stream 0's 2375 Hz shaping, which is clause 4.9 and
unverified. The
engine's audio filter is: tests/rpc/test_rpc_rds.cpp puts its passband edge at
68400 Hz for the 171000 tap, which cuts 71.25 and 76 kHz off entirely and 66.5
kHz in part, so the WFM receiver path in core/engine would have to widen
first. The group layer would need group type C beside A and B.

WHAT THIS PARAGRAPH USED TO SAY: "RDS2 is in the included list above and is
the cheapest new capability here, for the same reason: the demodulator, the
block synchroniser, the offset words and the CRC already work, and RDS2 reuses
all four on three more subcarriers." Its table row said "same 26-bit blocks
and offset words" and put the effort at small. None of that was read in a
document, the audio filter was not counted, and group type C was not known
about.

**Two-level FSK text and data, from receiver audio.** Pure libraries over a
span of real audio at the receiver's audio rate, the way RDS reads the WFM
composite, so the Demod enum does not grow. Not wired to the engine or the
wire yet; they report through the decoded-message seam once it exists.
`core/decode/fsk.cpp` is the part with no standard in it: a tone-pair
discriminator for audio that carries the shift as two tones, a level
discriminator for audio that is already the data waveform, and a
bit clock driven by Gardner's detector, which SITOR-B and NAVTEX below also
use and DSC can.

| Mode | File | Document | What comes out |
| --- | --- | --- | --- |
| RTTY | `core/decode/rtty.cpp` | ITU-T S.1 (03/93) clauses 3, 4.1 to 4.5 and Tables 1 and 2 for ITA2; ITU-T S.3 (11/88) clauses 1.3 and 1.4 and Table 1 for the 7.5-unit character | Characters with the sample index of their start element, letters and figures case tracked, a decision margin per character, and counts of framing errors and false starts |
| AX.25 over 1200 baud AFSK | `core/decode/ax25.cpp` | AX.25 v2.2 (TAPR, July 1998) clauses 3, 3.1, 3.4, 3.6 to 3.10, 3.12 and 4.2.1; the modem from Finnegan and Benson, "Clarifying the Amateur Bell 202 Modem", TAPR DCC 2014, sections 2 and 3.2 | Frames whose FCS checks, with destination, source and up to eight repeaters, the control field and its frame kind, the PID and the information field, and the sample indices of the first and last bit; counts of candidates, FCS failures and malformed frames |
| FX.25 around AX.25 over 1200 baud AFSK | `core/decode/fx25.cpp` and `core/decode/reed_solomon.cpp`, run by `core/decode/ax25.cpp` | Stensat Group FX.25 document version 0.01.06 (1 September 2006), sections "Protocol Summary", "Physical Layer Considerations", "Correlation Tag Details", "AX.25 Packet Requirements", "Pad Requirements" and "FEC Algorithms" with Table 1 | The same frames as AX.25, once each, marked as having arrived in an FX.25 codeblock with the Table 1 tag index and the octets the Reed-Solomon code corrected; counts of tags found and codeblocks refused |
| IL2P carrying AX.25 over 1200 baud AFSK | `core/decode/il2p.cpp` and `core/decode/reed_solomon.cpp`, run by `core/decode/ax25.cpp` | IL2P Specification Draft v0.6 (16 March 2024), sections "Interface to Physical Layer", "Reed Solomon Forward Error Correction", "Data Scrambling" with the transmit and receive LFSR figures, "Packet Structure", "Sync Word", "IL2P Header Types", "IL2P Type 1 Header" with its field map, PID mapping and control subfield maps, "Payload Block Size Computations", the receive procedure, "Optional Trailing CRC with Hamming Encoding" and "Example Encoded Packets" | The same AX.25 frames as plain AX.25, rebuilt from Type 1 or Type 0 packets, marked as IL2P with the header type, the octets the Reed-Solomon codes corrected and whether a trailing CRC matched; counts of headers decoded, packets refused and packets that did not rebuild |
| APRS | `core/decode/aprs.cpp` | APRS Protocol Reference 1.0.1 (29 August 2000) chapters 5, 6, 7, 8, 9, 10, 14 and 16 | Position reports uncompressed and compressed with timestamp, ambiguity, symbol, course, speed, altitude and range; status with timestamp or Maidenhead locator; messages, acknowledgements and rejections with their numbers; Mic-E position, message type, course, speed, telemetry, status text and altitude |
| POCSAG | `core/decode/pocsag.cpp` | ITU-R M.584-2 Annex 1 clauses 1.1 to 1.4, 2.1, 2.2 and 2.5.1 with Tables 1 to 3; ITU-R M.539-3 clause 4.3 for the rates and polarity | Pages with the 21-bit identity, function bits, numeric or alphanumeric text per the function bits and the raw message bits for the two function values M.584 gives no format, the sample index of the address codeword, the bits BCH corrected and the codewords it could not, and whether the sync arrived inverted |
| SITOR-B | `core/decode/sitor_b.cpp` | ITU-R M.625-4 clauses 1.1 to 1.3, 4.2 to 4.4, 4.6.4, 4.6.5 and 4.6.7 with Tables 1 and 2; M.476-5 Annex 1 agrees | Characters with the sample index of the DX copy, letters and figures case, whether the RX copy was used, whether both copies were lost, and the phasing each came from; counts of phasings, copies lost and ends of transmission |
| NAVTEX | `core/decode/navtex.cpp` | ITU-R M.540-2 Annex II clauses 2, 3 and 6 and Figure 1 | Messages with B1, B2 and the B3 B4 serial as sent, whether the preamble arrived clean per clause 3, the text between the preamble and NNNN, the count of lost characters, and whether NNNN arrived |

RTTY's 45.45 baud, 170 Hz shift and 2125 Hz mark are practice rather than
anything S.1 or S.3 states, and all three are parameters along with the
polarity. `core/dsp/synth/fsk_mod.cpp` is the transmitter and
`tests/decode/test_rtty.cpp` the round trip, at 48 kHz and at 8 and 11.025 kHz.
Swept by `bench sweep --mode rtty` through `add_real_awgn`, 256 trials of 100
characters a point: a character error rate of 0.01 at -5.2 dB in 2500 Hz,
0.0077 at -5 dB, 0.18 at -8 dB and none from -1 dB up
([sensitivity.md](sensitivity.md)). The test's one run of 300 characters gives
0.14 at -8 dB, and there the unit error rate is 0.0069 against 0.0064 for
ideal non-coherent FSK. What it does not reach: no automatic frequency
control, so the tones must be where the caller says, and no diversity or error
correction, because ITA2 has none.

AX.25 is measured in `tests/decode/test_ax25.cpp` at 48 and 22.05 kHz, through
a 0.5 percent transmitter clock error and a 10 dB tilt between the tones either
way, which is what Finnegan and Benson section 4 report emphasis doing on real
stations. Swept by `bench sweep --mode ax25` through `add_real_awgn`, 4096 UI
frames of 60 information octets a point, 83 octets before the FCS: a frame
error rate of 0.01 at 10.8 dB in 2500 Hz, 0.036 at 10 dB, 0.57 at 8 dB, every
frame lost at 6 dB and none from 13 dB up. The test's 60 frames give 0.05 at
10 dB, and its bit error rate after NRZI is 0.0024 at 8 dB and 0.018 at 6 dB.
WHAT THIS PARAGRAPH USED TO SAY: "over 60 frames of 81 octets"; the frame is
83. The
AX.25 FCS is checked against the X.25 check `core/decode/dv_codes.cpp` already
carried for TETRA, which is its only independent check, since ISO 3309 was not
held. The address encoding is checked against Figures 3.4 and 3.7, and in doing
so found that AX.25 2.2 misprints every N in its figures as 0x98, which is L,
and that Figure 3.8 sets an extension bit clause 3.12.4 says is clear; the code
follows the text. What it does not reach: connected-mode procedure, which has
nothing to decode; modulo 128 control fields, which a receiver joining partway
cannot tell from modulo 8; 9600 baud G3RUH and 300 baud HF packet; and FCS-based
bit repair, so one wrong bit loses a frame.

FX.25 is checked in `tests/decode/test_fx25.cpp`. The Stensat document prints
no worked codeblock, and it does not state the Reed-Solomon field polynomial,
the generator's first consecutive root or which end of the codeblock is the
high-order coefficient. `core/decode/fx25.h` sets x^8 + x^4 + x^3 + x^2 + 1,
alpha^1 and information first, as engineering choices, and no on-air FX.25
recording has been checked against them: a station whose code differs has its
codeblocks refused, and its frame still decodes as plain AX.25 when it arrives
clean. The tests check Table 1's tags against the byte order the document
prints for Tag_01, that every Table 1 code corrects up to half its check octets
exactly and refuses words with more, and round trips through the Bell 202
transmitter under all eleven codes, each frame reported once. A frame with one
to eight of its octets hit by bit errors, which the plain deframer loses, comes
back through RS(144,128) with the count of octets corrected, and nine are
refused. A tag is accepted with up to 8 of its 64 bits wrong, an engineering
choice fx25.h argues from Table 1's distances. What it does not reach:
Multi-Frame Blocks decode tag by tag but are not tested, and AX.25
segmentation, which the document leaves to the AX.25 layer, is not done.

IL2P is checked in `tests/decode/test_il2p.cpp`. The document's three example
packets, an S frame, a UI frame and an I frame with nine payload octets, decode
to the AX.25 frames printed beside them and encode back to the same octets,
trailing CRC included. They settled the readings the prose leaves open: the
Reed-Solomon generator starts at alpha^0 over x^8 + x^4 + x^3 + x^2 + 1, the
receive scrambler has no delay of its own, bit 7 of the header figure is each
octet's most significant bit, and the CRC is the AX.25 FCS value, high nibble
first. Three things the document does not settle are engineering choices in
`core/decode/il2p.h`: the P/F bit of a translated S frame sits at bit 6, as in
the I and U maps; PID 0x2, "AX.25 Layer 3", is never sent and rebuilds as 0x20;
and because the trailing CRC is optional by agreement, a receiver holds each
packet for 32 bits, reports it with the CRC verified when the trailer matches,
refuses it when the trailer is four exact Hamming codewords naming another CRC,
and otherwise reports it on the Reed-Solomon decode alone. Translation to and
from Type 1 is checked octet for octet for I frames, S frames and every U-frame
opcode, and the frames Type 1 cannot carry go Type 0. Round trips through the
Bell 202 transmitter, which sends IL2P with no NRZI as the symbol map says,
cover both header types, a 600-octet payload in three blocks, and both with and
without the trailer. One to eight payload octets hit,
plus one header octet, are corrected with the count reported where plain AX.25
loses the frame to one bit; nine payload octets or two header octets are
refused. A sync word with one bit wrong is found and one with two is not. A
stream of plain AX.25, FX.25, two back-to-back IL2P packets and plain AX.25
again decodes all five in order. What it does not reach: no on-air IL2P
recording has been checked; a transmitter still using the Baseline FEC level
that v0.6 removed sends shorter payload parity and is refused; the 9600 baud
GFSK and the BPSK and QPSK symbol maps are not implemented; and a Type 0 packet
whose payload is not an AX.25 frame is counted and not reported.

APRS is checked in `tests/decode/test_aprs.cpp` against 35 examples the
reference prints in chapters 8, 9, 10, 14 and 16, including page 38's
compressed longitude, page 40's GGA altitude, pages 44 and 53's Mic-E
destination and information field and page 55's Mic-E altitude, and against
both encodings of page 52's Mic-E speed and course. What it does not reach:
objects, items, weather, telemetry reports, queries and third-party traffic are
recognised and refused by name; data extensions other than course and speed
stay in the comment; and the WB2OSZ 1.2 revision was not read, so what it adds
arrives as comment text.

POCSAG is measured in `tests/decode/test_pocsag.cpp` at all three rates, at
48 and 22.05 kHz, in both polarities, and through a complex-baseband FSK
signal, a channel filter and the FM discriminator in `core/decode/dv_phy.cpp`
rather than from ideal audio alone. Swept by `bench sweep --mode pocsag-512`,
`pocsag-1200` and `pocsag-2400` through the same receiver, 4096 pages of 40
alphanumeric characters a point: a page error rate of 0.01 at 6.4 dB in
2500 Hz at 512 bit/s, 8.9 dB at 1200 and 11.1 dB at 2400; at 1200 bit/s 0.10
lost at 8 dB and 0.0073 at 9 dB, and none from 10 dB up. Through `add_awgn`
at 1200 bit/s over 40 pages of 15 codewords: no page lost at 20 dB in 2500
Hz, including tuned 1 kHz off; none lost at 12 dB, where the raw bit error
rate is 3.2e-5; 0.10 lost at 8 dB, where the raw bit error rate is 0.012 and
BCH corrected 316 bits and failed on one codeword in ninety; all lost at
4 dB, below the discriminator's threshold. Those figures are with the level
discriminator limiting the discriminator's clicks, which `core/decode/fsk.h`
explains and which moved every rate's crossing by 0.6 to 0.9 dB. The code
corrects every one and two bit error and refuses every three bit error,
checked exhaustively on twelve code words. WHAT THIS PARAGRAPH USED TO SAY,
before the limiter: "7.3 dB in 2500 Hz at 512 bit/s, 9.5 dB at 1200 and
12.0 dB at 2400", "4 and 3 pages of 4096 still lost at 10 and 11 dB", and
"0.475 lost at 8 dB, where the raw bit error rate is 0.023 and BCH corrected
584 bits". And before the address correction budget below: "0.40 lost at
8 dB, where the raw bit error rate is 0.023". Checking Tables 1
and 2 against clause 1.4 found that M.584-2 misprints the idle codeword: Table 2
is one bit from a code word, although clause 1.3.4 calls it a valid address
codeword, and the decoder uses the code word. What it does not reach: one rate
per decoder instance, so a receiver scanning all three runs three; no message
format for function bits 01 and 10, which M.584 does not define; and FLEX,
which is blocked on its document.

**POCSAG refuses what M.584-2 lets it refuse**, since 2026-09-23. Through the
wire at 6 and 4 dB it reported pages for addresses nobody sent, and
`tests/decode/test_pocsag_false_pages.cpp` reproduces that at decode level:
0.625 false pages per page sent at 6 dB, 0.546 at 5 dB and 0.154 at 4 dB, and
from noise alone 8.1 an hour across the three rates. Two rules now stand
between the code and a page. An address codeword is taken with at most one
corrected bit, parity included: clause 1.4's code has distance 6 with its
even parity bit, so correcting one leaves every two to four bit error
detected, where correcting two let a four bit error land on another address,
many of them an idle codeword read one code word over. And a synchronization
codeword found without the clause 1.1 preamble in front of it starts a
provisional batch whose pages are held until the next batch's synchronization
codeword confirms it, clause 2.3's "synchronization on receipt of a number of
valid batches". After: 0.0125 false pages per page sent at 6 dB, 0.029 at 5 dB,
none at 4, 8 and 12 dB, and none in 40 hours of noise. The cost was at 8 dB,
where 113 of 240 pages came through whole rather than 131; at 12 dB all 240
still did. With the click limiter the same audio gives 206 of 240 whole at
8 dB, 1, 5 and 1 false pages at 6, 5 and 4 dB, and still none in 40 hours of
noise. WHAT THE COST USED TO READ, before the limiter, in the present tense:
"where 113 of 240 pages come through whole rather than 131". What is left
below 8 dB is a codeword with five or more errors
within one bit of another, which the code cannot tell from a real one. A
receiver that joins a one-batch transmission after its preamble now reports
nothing from it, which is what clause 2.3 asks.

SITOR-B and NAVTEX sit on the same FSK discriminator and clock as RTTY and
AX.25. Checking M.625-4's Table 1 against ITU-T S.1's through the RTTY table
found the two agree on every combination, and that the 32 traffic signals and
three mode B service signals use all 35 three-of-seven patterns, so every
single bit error is detected and a weight-preserving double error never is.
`tests/decode/test_sitor_b.cpp` measures at 48 and 11.025 kHz on both
sidebands, the lower found by its complemented phasing signals. Through
`add_real_awgn` over 562 characters: nothing lost at 10 dB in 2500 Hz; at
-2 dB the DX copy is lost 0.53 percent of the time and no character is lost
in both; at -5 dB the DX copy is lost 8.9 percent and 1.2 percent are lost in
both, which is what time diversity buys. Swept by `bench sweep --mode sitor-b`,
1024 transmissions of 100 characters a point, scored by edit distance so a
character lost to a loss of phase or read as another valid signal counts as
well: a character error rate of 0.01 at -4.4 dB in 2500 Hz, 0.028 at -5 dB,
0.0012 at -3 dB, and nothing lost from -1 dB up. That is since the FSK bit
clock moves off the boundary its first reading can land on; before it, 1 to 4
transmissions in 1024 printed nothing at every level from 0 to 20 dB,
because the decoder phased late or never ([sensitivity.md](sensitivity.md)).
WHAT THIS PARAGRAPH USED TO SAY: "a character error rate of 0.01 at -1.1 dB
in 2500 Hz, 0.092 at -5 dB, 0.015 at -3 dB", and "Why those transmissions
print nothing has not been established". A 150 ms noise burst never prints a
wrong character, and loses one when noise turns a DX copy into a different
valid signal, which clause 4.3 then refuses. NAVTEX at -2 dB receives all ten
120-character messages exactly, and at -5 dB two of ten, with every
preamble clean. Swept by `bench sweep --mode navtex`, 2048 messages a point,
each its own transmission and counted received only with a clean preamble and
its text exact: a message error rate of 0.01 at -2.6 dB in 2500 Hz, 0.031 at
-3 dB, 0.28 at -4 dB and 0.86 at -5 dB, 3 of 2048 lost at -2 dB and none from
-1 dB up. What they do not reach: SITOR-A, which is an ARQ conversation
and not a broadcast; the selective B-mode, whose inverted traffic reads as
mutilated; DSC on MF and HF, which would reuse the discriminator and has not
been written; and the meaning of NAVTEX's B1 and B2 letters, which is in the
IMO NAVTEX Manual, not held, so they are reported as the letters sent. VHF
DSC is done, on the tone discriminator and the bit clock of
`core/decode/fsk.h`; see "AIS and VHF DSC" below. WHAT THE LIST USED TO SAY
of DSC: "DSC, which reuses the discriminator and has not been written".

**PSK31, PSK63 and QPSK31, receiver audio to text.** `core/decode/psk31.cpp`,
with the alphabet in `core/decode/varicode.cpp` and the mixer and carrier
search it shares with CW in `core/decode/tone_frontend.cpp`. It reads the real
audio a USB demodulator hands to `attach_audio_sink`, at any audio rate, so the
Demod enum does not grow. Written from G3PLX's article as reprinted in QEX
July/August 1999, whose Table 1 is the Varicode source; PSK63's symbol rate is
cited to Wavecom's decoder documentation because the article predates it.

What comes out is text, each character placed at the audio sample where its
first bit was decided, plus the measured carrier offset. The AFC captures a
tone up to 40 Hz either side of the configured centre by squaring the signal,
or raising it to the fourth for QPSK, over the first two seconds, and when
that line falls short, by reading the idle's two tones half the symbol rate
either side of the carrier directly; it will not acquire on noise, whose best
squared line measured 3.9 against a threshold of 6, and whose best pair of
tones 3.4 in 10 hours against 4.5.
QPSK31 decodes through `core/decode/dv_codes.cpp`'s Viterbi decoder: the
article's 32-entry phase table is reproduced exactly by a K=5 code with
generators D+D^2+D^3 and 1+D^3+D^4, read as the in-phase and quadrature signs
of the phase shift advanced by 45 degrees, and the test regenerates all 32
entries and the article's worked example from those two polynomials.

Swept by `bench sweep --mode psk31`, `psk63` and `qpsk31`, transmissions of
100 characters with the tone 8 Hz off centre, signal to noise in 2500 Hz of
audio: a character error rate of 0.01 at -8.8 dB for PSK31, -5.8 dB for PSK63
and -9.5 dB for QPSK31, and none from -4, -1 and -4 dB up
([sensitivity.md](sensitivity.md)). QPSK31 goes from 0.089 at -12 dB to 0.014
at -10, and no transmission of 256 at -12 dB reads worse than 0.5, since
acquisition reads the idle's two tones when the squared line falls short;
before that, 11 of the first 16 at -12 dB printed next to nothing and the
curve read 0.78 there. The test's one run of 600 characters, 2.8e-2 bit error
rate and 0.09 character error rate at -12 dB, agrees with the curve. WHAT
THIS PARAGRAPH USED TO SAY: "QPSK31 goes from 0.78 at -12 dB to 0.014 at -10,
and at -12 dB it is all or nothing per transmission". Measured over those 600 characters at 48 kHz:
BPSK31 has a bit error rate of 2.5e-3 at -10 dB and PSK63 4.0e-3 at -7 dB,
with no errors at +10 dB in any of the three. Against differential BPSK theory
that is about 2 dB of implementation loss, most of it the sixth of each
neighbour a matched filter leaves on a two-symbol pulse.
`core/dsp/synth/psk31_mod.cpp` is the transmitter and `tests/decode/test_psk31.cpp`
the round trips, at 48 kHz and 11025 Hz with the tone 12 and 27 Hz off centre.

Two things it does not do. The extended alphabet of codes longer than ten bits
is reported as unrecognised rather than decoded, which is what the article says
early decoders do. And arrl.org/psk31-spec, the HTML copy of the same table,
has three errors that make two pairs of characters indistinguishable, so it
must not be used as a source; `core/decode/varicode.h` lists them.

**CW, receiver audio to text and speed.** `core/decode/cw.cpp`, on the same
audio seam and front end as PSK31. The table is ITU-R M.1677-1 Annex 1 clause
1.1, service signals included and written as their clause 1.1.3 names in angle
brackets, and the element and space ratios are its clause 2. The Recommendation
states no speed, so words per minute and Farnsworth timing come from the ARRL
Morse Transmission Timing Standard, QEX April 1990, whose PARIS word the test
confirms is fifty units under clause 2.

What comes out is each character with its dots and dashes and the sample where
it started, the character speed, and the overall speed its letter spaces imply,
which is lower than the character speed under Farnsworth. The dot length is
estimated from the signal between 5 and 50 WPM and follows a change of speed
within a few characters. Envelope detection runs through a boxcar sized to the
estimated dot, against a noise floor tracked as a Rayleigh lower quartile, with
a squelch that needs 5.5 noise deviations to open and 2 to stay open.

A receiver's audio is read by `decode::CwBand`, which searches 200 to
2800 Hz of it for keyed tones, starts a decoder on each at the pitch it found,
and labels every character with the pitch and a stream number; each stream
follows its tone as it drifts and prints nothing until its marks fall into
dots and dashes. Through the engine, a tone anywhere from 300 to 1200 Hz on a
usb or lsb receiver, or a cw one whose filter passes it, reads with no
character wrong from 12 to 40 WPM and 0 to 20 dB in 2500 Hz
([sensitivity.md](sensitivity.md), "CW through the engine, at any pitch").

Swept by `bench sweep --mode cw` at 20 WPM through the search, 256
transmissions of 60 random letters and figures a point, signal to noise in
2500 Hz of audio: a character error rate of 0.01 at -6.6 dB, 0.040 at -8 dB
and 0.28 at -10 dB.

WHAT THE SWEEP'S FIGURES USED TO SAY, for the decoder that looked for its tone
within 100 Hz of 700: "a character error rate of 0.01 at -6.2 dB, 0.055 at -8
dB and 0.28 at -10 dB, and between 0 and 0.0010 from -5 to +3 dB". Measured
over the test's fixed 186 characters on 2026-09-22: no errors down to -6 dB at
12 and 20 WPM; a character error rate of 0.086 at -10 dB for 20 WPM, 0.10 for
12 WPM, and 0.075 at -8 dB for 35 WPM. It decodes nothing from a minute of
noise after a transmission ends. `core/dsp/synth/cw_mod.cpp` is the keyer,
with Farnsworth spacing and seeded hand-sending jitter, and
`tests/decode/test_cw.cpp` the round trips at 48 kHz and 11025 Hz with the
tone 50 and 30 Hz off centre.

The start of a transmission is held back until the dot length is known and
its long spaces fall into letter spaces and word spaces, so a first word of
one character keeps the space after it; a pause of twice the longest space so
far prints what is held. A new dot length is taken only when a mark in the
recent runs is a dash by it, so a run of T, M and O cannot pull the estimate
onto a dash. The speed it reports weights the mean dot and the mean element
space equally, because noise shortens one and lengthens the other by about the
same amount; the runs are still read against the pooled mean, which decoded
better. `tests/decode/test_cw.cpp` has the measurement.

Until 2026-09-23 the next paragraph used to say "The measured speed reads slow in
noise, 18.3 WPM for 20 at -10 dB and 33.3 for 35 at -8 dB", with the cause.
The speed estimate no longer pools dots and spaces. Averaged over 40
transmissions at -8 dB it reads 20.13 WPM for 20 and 34.27 for 35, where
pooling read 19.36 and 32.75.

Not done. The lead-in's noise still keys a character now and then: all 18
errors in 768 transmissions from +4 to +10 dB were at the start of one, from a
first noise estimate that landed low. The decoder holds the first characters back until it has seen
runs of two different lengths, then decodes them, so text whose marks and
spaces are all one length, a row of T's with letter spaces between them, is
never decoded at all.

**M17, link setup and stream frames.** `core/decode/m17.cpp`, on the same
complex baseband input as P25 and D-STAR and through the same
`core/decode/dv_phy.cpp` discriminator, root raised cosine and square-law
timing. Written from the M17 Protocol Specification Part I, version 2.0.4 of 21
January 2026, from its prose, tables and equations; the specification's own
Licenses page puts the document under the GNU Free Documentation License 1.3
or later and the software listings inside it under GPL-2.0-or-later, and none
of those listings was used.

What comes out: every frame with its sync burst's sample position and
correlation. From a Link Setup Frame, the destination and source callsigns
decoded from Appendix A's base-40 addresses, the TYPE field of Table 3.2, the
META bytes and whether the CRC checked. From a stream frame, the frame number
and its end-of-stream bit, the 128 payload bits and the LICH chunk and counter;
a receiver that joins mid-stream rebuilds the LSF from six LICH chunks and
reports it once the CRC checks. The End of Transmission marker is reported.
Discriminator polarity is found from the sync burst, and the symbol level and a
carrier offset are calibrated per frame against the burst's known symbols. The
tests regenerate the specification's CRC test vectors, its AB1CD address
example, its 0xB4 symbol example, the Golay generator matrix and the first rows
of the interleaver table.

Swept by `bench sweep --mode m17`, 512 transmissions of 25 stream frames a
point: a stream frame error rate of 0.01 at 17.0 dB in 2500 Hz, which is
11.4 dB in the 9 kHz channel bandwidth, 0.089 at 16 dB and 0.46 at 15 dB, and
none from 20 dB up ([sensitivity.md](sensitivity.md)). Measured in the test
over 100 stream frames, signal to noise in the 9 kHz channel
bandwidth: no frame errors at 14 dB and above, a stream frame error rate of
0.01 at 12 dB and 0.18 at 10 dB, with raw symbol error rates of 1.7e-3, 1.0e-2
and 6.2e-2 at those three points. Clipping the soft values before the Viterbi
decoder is what makes 10 dB usable; unclipped it was 0.85, because a
discriminator's clicks make symbols wrong with full confidence.
`core/dsp/synth/m17_mod.cpp` is the transmitter and `tests/decode/test_m17.cpp`
the round trips, at 48 and 24 kHz, 250 Hz off frequency and with the
discriminator inverted.

Noise alone started M17 transmissions until 2026-09-23: over the wire, two
LSFs, a BERT burst and two packet bursts in 2.5 s, and through a 12.5 kHz
channel at decode level 1615 LSFs and 9808 frames an hour, none of them real.
`tests/decode/test_m17_noise.cpp` has the measurement and three rules now hold
it to none in two hours. An LSF or BERT burst starts a transmission only behind
32 symbols of the 1.4.1 preamble rather than eight. A transmission is tracked
only through the bursts clause 2.4 lets it carry: after an LSF, stream or
packet bursts, one mode to a transmission, and the end marker, and never BERT,
which is two symbols from a stream burst and was where noise kept a false
transmission alive; after a BERT preamble, BERT and the end marker. And a
stream joined late needs a second frame whose Frame Number is one more than
the first's. At 12 dB in 9 kHz, twelve transmissions of 50 frames gave 596
payloads and 11 LSFs right against 592 and 11 before, and at 14 dB all 600 and
12 before and after. At 10 dB, 482 payloads against 487: a transmission lost
mid-stream now needs two good frames in a row to be joined again.

Not done, and why. Since 2026-11-02 an unencrypted voice stream carries 40 ms
of 8 kHz audio a frame through libcodec2; "Decoders merged on 2026-11-02"
below has the measurement. WHAT THIS PARAGRAPH USED TO SAY: "Codec 2 voice in
the stream payload is not decoded". Packet and BERT frames are recognised by
their sync bursts and not decoded. BERT's PRBS9 receiver in Appendix G was read
in the specification's embedded GPL listing, figures G.3 to G.5, before the
licence note was noticed, so it is left to somebody who has not read it, per
docs/clean-room.md. The randomiser the M17 row above used to call PRBS9 is a
fixed 368-bit table, Appendix B.

**AIS and VHF DSC, from an nfm receiver's audio, 2026-09-23.** Two libraries
over the audio an nfm receiver's discriminator hands to `attach_audio_sink`,
at 48000 S/s, and two adapters on the decoded-message seam, `ais` and `dsc`,
so the Demod enum does not grow. AIS needs no complex tap: an nfm receiver's
kernel is the plain discriminator with no de-emphasis, and its default 8 kHz
either side holds 9600 bit/s GMSK at index 0.5, whose deviation is 2400 Hz. A
decoder handed complex baseband would discriminate it first and do the same
thereafter. VHF DSC is audio by its own definition: two tones on an FM
voice channel. `docs/clean-room.md` has the entry recording what both were
written from, with the clause behind every constant.

| Mode | File | Document | What comes out |
| --- | --- | --- | --- |
| AIS | `core/decode/ais.cpp` | ITU-R M.1371-5 Annex 2 clauses 2.1.1 to 2.6, 3.2.2 to 3.2.2.11 and 3.3.7 with Tables 3, 6, 12 and 17; Annex 8 Tables 47 to 52, 70, 71, 73, 78, 79 and 79A and Figure 41. -6 read beside it | Every message whose FCS checks, with its octets and the sample indices of its first data bit and end flag; Messages 1 to 3, 4 and 11, 5, 18, 19, 21 and 24 parsed: MMSI, position, SOG, COG, heading, status, rate of turn, time stamp, UTC, name with Message 21's extension, call sign, destination, ETA, draught, dimensions, ship and AtoN type, the Class B flags and Table 79A's vendor ID |
| DSC on VHF | `core/decode/dsc.cpp` | ITU-R M.493-15 Annex 1 clauses 1.1 to 1.5.1, 3 to 10, Tables A1-1 to A1-3, A1-4.1 to A1-4.9 and A1-5, and Figure 1. -16 read beside it | Each call whose error-check character agrees: format specifier, address or area, category, self-ID, telecommands, the distress ID, nature, position, time and subsequent communications where the format carries them, channel or frequency elements, the end of sequence, and how many characters came from their RX copy |

AIS is received as bursts. The audio is summed over a bit and read at eight
phases of the bit at once, with no clock loop: clause 2.4's 50 ppm moves the
instant 0.064 of a bit over the longest frame. Each phase fits its last 32
readings to the levels the training sequence and start flag put on the air,
and where they correlate above 0.7 a burst starts, sliced against the offset
the fit gave, which is the carrier's error; each burst is NRZI decoded and
deframed on its own and reported from whichever phase finished it first.
`core/decode/ais.h` records the two receivers measured and set aside: a
running mean in place of the fit, which lost 8 of 90 packets at 30 dB where
the fit loses none, and a Gaussian receive filter of BT 0.5, which lost
0.378 at 20 dB where the boxcar lost 0.144, both with the SNR then stated
over bursts and gaps together. The FCS is HDLC's; a
frame whose CRC would check only uncomplemented is counted rather than
reported, so a real capture can say whether that reading of ISO/IEC 13239,
not held, is right. Two sentences real stations sent, from the gpsd
project's AIVDM/AIVDO write-up, read through Tables 48 and 52 as a ship
moored in Seattle and a vessel named MT.MITCHELL bound for SEATTLE; that
checks the field layout against a real transmitter and not the bit order on
the air, which an AIVDM sentence does not carry.

DSC is read on `core/decode/fsk.h`'s tone discriminator and Gardner bit
clock. Phasing is clause 3.3's: two DX and one RX, two RX and one DX, or
three RX, each in its own place among the sixteen phasing slots, tried at
every bit. Each character is taken from its DX copy when that passes the
ten-bit check and from its RX copy when it does not; where both pass and
disagree, the error-check character decides. A call is reported only whole
and with its ECC agreeing, and a distress alert or all ships call only with
both its format specifiers alike, clause 4.2. Figure 1 draws DX and RX as
two rows without saying which of a pair is sent first; DX first is the order
that puts clause 1.2.1's four characters between the copies.

Measured in `tests/decode/test_ais.cpp` and `test_dsc.cpp` at 48000 S/s,
through an 8 kHz channel filter and discriminator with the noise on the RF.
AIS, nine message types ten times over: none of 90 lost at 30 dB in 2500 Hz
with the carrier on the channel centre or 500 Hz either side and the clock
50 ppm either way, and 0.311 lost at 20 dB. DSC, seven calls ten times over:
none of 70 lost at 20 dB, and 27 at 14 dB with 117 characters taken from
their RX copies. Noise alone decoded neither in two minutes; AIS started 174
bursts on it, 6 of which ran to whole octets and none past the FCS, and DSC
never found a phasing. Both give the same output however the audio is cut,
down to a sample a call. Swept by `bench sweep --mode ais` over one-slot
Message 1 bursts, 256 trials of four a point: a frame error rate of 0.01 at
22.5 dB in 2500 Hz, 0.18 at 20 dB and none from 24 dB up. By `bench sweep
--mode dsc` over individual calls: 0.01 at 15.7 dB, 0.39 at 14 dB, none from
18 dB up ([sensitivity.md](sensitivity.md)). Through the engine and the wire
in `tests/rpc/test_rpc_decode_audio.cpp`, on an nfm receiver: three of three
AIS messages at 30 and at 22 dB and three of three DSC calls at 30 and at
16 dB.

What they do not reach. AIS: the binary messages' application data, Message
27 and the long-range channels, the satellite channels AIS 3 and AIS 4, the
transmit power bit M.1371-6 gives Messages 1 to 3 and 18, and one receiver
listening to both channels, which takes two receivers. No message longer
than two slots has been sent through the decoder. DSC: MF and HF, the same
code and alphabet at 100 baud on a sideband receiver, not written or
measured; the M.821 expansion sequence after a distress alert, M.821 not
held; the semi-automatic service's network numbers beyond keeping their
symbols; M.493-16's ACS second telecommands, reported by number; and any
acknowledgement, relay or alarm, which a receiver has no business sending.
A decoded distress alert is a report and not a GMDSS watch.

**Decoders merged on 2026-11-02.** Every file below opens with the documents,
editions and clauses it was written from, and `docs/clean-room.md` logs each
read. Figures are those the named test asserts or records; where a test prints
a value and holds it only to a bound, the bound is what is given here, as "held
to". Not one of these has been checked against an on-air recording unless the
entry says so.

| Mode | File | Document | What comes out |
| --- | --- | --- | --- |
| Wideband tap | `core/rpc/decoders.h`, `subscribeWidebandDecoded` | None; plumbing | Decoded messages from a decoder fed the source's own stream, decimated to at least the decoder's `wideband_rate` |
| CRC | `core/decode/crc.h` | Williams, "A Painless Guide to CRC Error Detection Algorithms" (1993); Lin and Costello section 5.1 | Any CRC of width 1 to 64 over octets or bit spans |
| Binary BCH | `core/decode/bch.h` | Lin and Costello chapter 6; Massey, IEEE Trans. IT, January 1969 | Systematic encoding and hard-decision correction over GF(2^m), m 3 to 10, full or shortened |
| OFDM | `core/decode/ofdm.h` | van de Beek, Sandell and Borjesson 1997; Schmidl and Cox 1997 section III-B; Hoeher, Kaiser and Robertson 1997 | Equalised carriers per symbol with a per-carrier SNR estimate, for a mode decoder to demap |
| MFSK | `core/decode/mfsk_demod.h` | Proakis, non-coherent orthogonal detection | Per-symbol tone energies and decisions for M-ary FSK and IFK |
| LDPC | `core/decode/ldpc.h` | Gallager 1963; Fossorier, Mihaljevic and Imai 1999; Chen and Fossorier 2002 | Min-sum decoding of a caller's H, systematic encoding from a caller's G |
| Viterbi stream and Fano | `core/decode/viterbi_stream.h`, `core/decode/fano.h` | Viterbi 1971; Lin and Costello chapters 12 and 13; Fano 1963; CCSDS 131.0-B section 3 for 171/133 | Continuous punctured or plain Viterbi output; rate 1/2 sequential decoding to K=32 |
| M-PSK | `core/decode/psk_demod.h` | Proakis 5th ed. 9.2, 6.2.4, 6.2.5; Mengali and D'Andrea chapter 3; Viterbi and Viterbi 1983; Gardner, Phaselock Techniques chapter 4 | Symbols and max-log soft bits for BPSK, QPSK, OQPSK and 8PSK |
| Image path | `core/decode/image_sink.h`, `core/rpc/image_bridge.h`, `ui/models/decoded_images.h` | None; plumbing | Image open, row and close events over RPC to a store the client's image pane paints |
| LPC-10 | `core/decode/lpc10.h` | FED-STD-1015 (28 November 1984) as FIPS PUB 137 with Change Notice 1 | 180 samples of 8 kHz audio per 54-bit frame; an encoder |
| FS-1016 CELP | `core/decode/fs1016.h` | FED-STD-1016 (14 February 1991) | 240 samples of 8 kHz audio per 144-bit frame; an encoder |
| CVSD | `core/decode/cvsd.h` | MIL-STD-188-113 (17 February 1987) section 5.2 and Appendix C | 8 kHz audio from 16 or 32 kbit/s CVSD; an encoder |
| 2G ALE | `core/decode/ale_2g.h` | MIL-STD-188-141D (22 December 2017) Appendix A | ALE words, calls and soundings with addresses |
| MIL-STD-188-110 serial tone | `core/decode/mil110.h` | MIL-STD-188-110C with Change Notice 1, 5.3.2 | User bits at 75 to 4800 bit/s |
| dPMR | `core/decode/dpmr.h` | ETSI TS 102 490 V1.9.1 (2016-08); TS 102 658 V2.6.1 (2019-01) | Header, payload and end frames, Mode 1 and Mode 2, with 24-bit IDs and raw TCH blocks |
| Opulent Voice framing | `core/decode/opulent_voice.h` | Opulent Voice Protocol Specification 1.1 (January 2026), ORI | 134-byte frames with the parsed header and base-40 callsign, from soft bits |
| DSC on MF and HF | `core/decode/dsc_hf.h` | ITU-R M.493-16 (12/2023) Annex 1, 1.3.1, 1.3.3, 1.4, 3.4 | The VHF decoder's calls, from SSB audio at 100 baud |
| SITOR-A | `core/decode/sitor_a.h` | ITU-R M.625-4 (03/2012); M.476-5 Annex 1 agrees | Both ends of an ARQ link: information blocks, control signals and accepted text |
| AX.25 over G3RUH 9600 | `core/decode/g3ruh.h` | Miller, "9600 Baud Packet Radio Modem Design", RSGB Data Symposium 1988; AX.25 v2.2 | AX.25 frames whose FCS checks, from discriminator audio |
| DVB-S | `core/decode/dvbs.h` | ETSI EN 300 421 V1.1.2 (1997-08); ITU-T H.222.0 (05/2006); EN 300 468 V1.17.1 (2022-10) | Transport stream packets with PAT, PMT and SDT parsed |
| FT8 | `core/decode/ft8.h`, `wsjt77.h`, `ft_ldpc.h` | Franke, Somerville and Taylor, QEX July/August 2020; `ft4_ft8_protocols.tgz` | 77-bit messages as text with frequency, DT and SNR per 15 s slot |
| FT4 | `core/decode/ft4.h` | The same paper | The same per 7.5 s slot |
| Codec 2 and M17 voice | `core/decode/codec2_voice.h` on libcodec2 1.2.0 | Linked, not written; M17 Part I 2.0.4 for the stream | 8 kHz speech from unencrypted M17 voice streams at 3200 and 1600 |
| AIS 14 and 27, locating devices | `core/decode/ais.h` | ITU-R M.1371-6 (02/2026) | Message 14 safety text, Message 27 long-range position, and SART, MOB and EPIRB recognised on every message |
| VDL Mode 4 | `core/decode/vdl4.h` | ETSI EN 301 842-1 V1.4.1 (2015-04) | Link-layer bursts as octets, unparsed |
| ASTERIX Category 021 | `core/decode/asterix_cat021.h` | EUROCONTROL-SPEC-0149-12 Edition 2.7; SPEC-0149 Part 1 Edition 3.1 | ADS-B target report records with decoded fields |
| Winlink B2F | `core/decode/b2f.h` | Open B2F (February 2018, amended 2020); F6FBB forwarding protocol page | Proposals, answers, blocks and parsed messages from a byte stream |
| NOAA APT | `core/decode/apt.h` | NOAA KLM User's Guide (April 2014) section 4.2 | 2080-sample lines and telemetry wedges |
| NOAA HRPT | `core/decode/hrpt.h` | NOAA KLM User's Guide (April 2014) section 4.1 | 10-bit minor frames and five AVHRR channel images |
| CCSDS TM and AOS | `core/decode/ccsds.h` | CCSDS 131.0-B-5, 132.0-B-3, 732.0-B-4, 133.0-B-2 | Transfer frames and space packets, RS in dual or conventional basis |
| BLE advertising | `core/decode/ble.h` | Bluetooth Core Specification 6.1 (2025-04-29) | Legacy advertising PDUs with addresses and AD structures |
| Z-Wave | `core/decode/zwave.h` | ITU-T G.9959 (01/2015) | MAC frames at R1, R2 or R3 |
| EnOcean | `core/decode/enocean.h` | EnOcean Radio Protocol 1 V1.2 (9 November 2020) | Subtelegrams with RORG, data, sender, status and repeater count |
| DECT | `core/decode/dect.h` | ETSI EN 300 175-2 and -3 V2.9.1 (2022-03) | P32 A-fields: header, tail, R-CRC, NT and QT messages, RFPI |
| ERMES | `core/decode/ermes.h` | ETS 300 133-4 and -2 (July 1992) | Batches, addresses and numeric, alphanumeric and transparent messages |
| DRM30 | `core/decode/drm.h` | ETSI ES 201 980 V4.2.1 (2021-01) | FAC, SDC, MSC stream frames, text messages and packet data units |
| DAB | `core/decode/dab.h` | ETSI EN 300 401 V2.1.1; EN 301 234 V2.1.1; TS 101 499 V3.1.1 | FIC, ensemble labels, sub-channel bytes, packet data groups, MOT objects |
| Analogue ATV | `core/decode/atv.h` | ITU-R BT.470-6 (1998) | Luminance images, systems M and B/G |
| SSTV and FAX480 | `core/decode/sstv.h` | N7CXI, "Proposal for SSTV Mode Specifications", Dayton, 20 May 2000 | Colour images, and Gray8 for FAX480, slant corrected |
| VDES ASM-TER | `core/decode/vdes.h` | ITU-R M.2092-2 (02/2026) Annex 2 and Annex 3 A3-7 | ASM packets with the common message header |
| FUNcube | `core/decode/funcube.h` | FUNcube-1 Downlink Data Issue 1.0 (05/03/2015); Karn, AO-40 coded telemetry proposal v1.2 (2002) | Telemetry frames |
| Cospas-Sarsat 406 MHz FGB | `core/decode/cospas_sarsat.h` | C/S T.001 Issue 4 Revision 7 (March 2021) | Beacon messages with BCH checked and the 15 Hex ID |
| GPS and QZSS L1 C/A | `core/decode/gnss_acq.h`, `gps_lnav.h` | IS-GPS-200N (1 August 2022); IS-QZSS-PNT-006 | Subframes 1 to 5: clock, ephemeris, almanac, UTC, Klobuchar; C/N0 and Doppler per satellite |
| GLONASS L1OF | `core/decode/glonass_l1.h` | GLONASS ICD Edition 5.1 (2008) | Strings 1 to 4 |
| BeiDou B1I | `core/decode/beidou_b1i.h` | BDS-SIS-ICD-B1I-3.0 (February 2019) | D1 subframes 1 to 3 with BCH |

The generic layers come first because the modes below are built on them.
CRC and BCH are held to catalogue check values over "123456789" and to
correction up to t, with POCSAG's (31,21) as a cross-check
(`tests/decode/test_crc.cpp`, `test_bch.cpp`). OFDM, on synthetic geometries
in `tests/decode/test_ofdm.cpp`: a DRM mode B-like 1024-point case, 4.6
carriers off, an echo 3 dB down at 1 ms, 25 dB, measured EVM -25.8 dB; a
256-point case at 25 dB with an echo, -23.9 dB. MFSK, LDPC, the Viterbi
stream, Fano and M-PSK each print their error rates and hold them to bounds:
M-PSK to within the theoretical symbol error rate 1 dB worse, MFSK to the
union bound 3 dB worse, the K=7 Viterbi stream below 1e-4 at Eb/N0 4.5 dB over
two million bits, Fano at K=32 to 95 percent of WSPR-length blocks at 4 dB
(the matching `tests/decode/test_*.cpp`). Not done: the OFDM receiver holds
timing from acquisition, so a long transmission at 100 ppm drifts out of the
prefix after about 2500 symbols of 1024; M-PSK reports its phase ambiguity and
leaves it to a sync word. The wideband tap and the image path are plumbing and
are held to delivery and ordering by `tests/rpc/test_rpc_decode_wideband.cpp`
and `test_rpc_decode_image.cpp`; no figure applies.

**Vocoders.** LPC-10, FS-1016 and CVSD each carry their own encoder, and the
tests hold them to quality floors rather than bit streams, because none of the
three documents prints a test vector: LPC-10 quantisation distortion under 2 dB
voiced, FS-1016 segmental SNR over 2 dB, CVSD over 5 dB at 16 kbit/s and 7 dB at
32 (`tests/decode/test_lpc10.cpp`, `test_fs1016.cpp`, `test_cvsd.cpp`). CVSD
misses MIL-STD-188-113 table VI at the 0 percent pattern, about -29.5 dBm0
against -24, because the standard's 16:1 ratio and that row contradict each
other; `core/decode/cvsd.h` gives the arithmetic. Not done: FS-1016's error
rate smoothing, LPC-10e's transmit enhancements, and any check against
reference bit streams.

**HF utility.** 2G ALE holds 18 of 20 frames at 6 dB in 3 kHz, AWGN and two
path (`tests/decode/test_ale_2g.cpp`); the order of a tone's three bits is not
stated in Appendix A and leftmost first is assumed, to confirm on air.
Linking protection decodes to nothing and AQC-ALE addresses are not
interpreted. MIL-STD-188-110C serial tone holds bit error rates under 0.01 at
18, 9, 3 and 0 dB in 3 kHz for 4800, 2400, 1200 and 75 to 600 bit/s, 75 Hz off
(`tests/decode/test_mil110.cpp`); STANAG 4415 not held. Clock tracking and the
Appendix C rates followed the same day; see "Decoders merged later on
2026-11-02" below. WHAT THE CLAUSE BEFORE STANAG 4415 USED TO SAY: "fixed
frequency only, no Appendix C rates above 4800". MF/HF DSC, measured in
`tests/decode/test_dsc_hf.cpp` over 30 calls: none lost at 6 or -3 dB in 2500
Hz, 20 lost at -6 dB. SITOR-A, monitoring both stations, 301 characters in
`tests/decode/test_sitor_a.cpp`: none lost down to 0 dB, 0.020 lost at -4 dB,
0.26 at -6 dB, never acquired at -8 dB; Table 3a identification signals and
the clause 2.5 check-sum are not interpreted.

**VHF and UHF data.** dPMR holds the header and 16 of 16 CCH at 12 dB in 4 kHz
(`tests/decode/test_dpmr.cpp`); the CRC preset and reflection are not stated
and zero, unreflected, is assumed, to confirm on air; voice is AMBE+2 and goes
to the vocoder socket. Opulent Voice framing holds 40 of 40 frames at 3 dB per
coded bit (`tests/decode/test_opulent_voice.cpp`); there is no demodulator
until ORI confirms the modulation, and the Opus payload is handed out as bytes.
G3RUH 9600, 40 frames of 81 octets in `tests/decode/test_g3ruh.cpp`: none lost
at 20 dB in 2500 Hz, 8 at 16 dB, 38 at 13 dB; through FM in an 8 kHz nfm
receiver, none at 25 dB and 6 at 18 dB. 300 baud HF packet is not done, for
want of a document. AIS Messages 14 and 27 and SART, MOB and EPIRB
identities are held to M.1371-6's tables and to one RF round trip of each at
25 dB in 2500 Hz (`tests/decode/test_ais.cpp`); no error rate was measured.
VDL Mode 4, 60 bursts a point in `tests/decode/test_vdl4.cpp`: burst error
rate 0.10 at 40 dB in 2500 Hz, 0.37 at 35, 0.95 at 30. That is about 8 dB
short of clause 6.1.4's -98 dBm, it decodes strong local signals only, the
HDLC framing is assumed from AIS rather than cited, and the burst fields are
not parsed. VDES ASM-TER, 500 one-slot bursts in `tests/decode/test_vdes.cpp`:
packet error rate 0.122 at 11 dB Es/N0, 0.022 at 12, 0.004 at 13. Link IDs 1
to 3 decode; 5 to 7 are turbo coded and decode since later the same day, below;
VDE-TER is not done. WHAT THE SENTENCE BEFORE USED TO SAY: "5 to 7 are turbo
coded and reported without payload".

**Messages carried over other bearers.** ASTERIX Category 021 and Winlink B2F
are parsers, held to hand-assembled records and the documents' printed
examples (`tests/decode/test_asterix_cat021.cpp`, `test_b2f.cpp`); no figure
applies. ASTERIX leaves REF and SPF raw. B2F hands LZHUF to a caller's
decompressor, since the only description points at source, and reports the
FBB CRC16 unchecked because its parameters are unstated.

**Weak signal.** FT8, ten signals a slot in `tests/decode/test_ft8.cpp`, is
held to 90 percent decoded at -16 dB in 2500 Hz and half decoded at -18 dB or
lower; FT4, eight a slot in `test_ft4.cpp`, to 90 percent at -12 dB. Not done:
ordered statistics decoding, block detection, and signals starting more than
about 2.4 s into the slot. The generator and parity matrices come from
`ft4_ft8_protocols.tgz`; `docs/clean-room.md` has the public-domain basis.

**Voice through Codec 2.** M17 stream voice renders through libcodec2 1.2.0,
linked as a recorded copyleft exception. `tests/decode/test_m17_voice.cpp`
holds every frame to over 40 dB segmental SNR against libcodec2's own decode
of the transmitted bits, clean, 300 Hz off and at 12 dB in 9 kHz; a correct
payload measures near 50 dB. Encrypted or scrambled streams are not rendered,
and M17 packet and BERT modes are still not decoded.

**Satellites.** NOAA APT is held to a mean absolute error under 12 grey levels
at 14 dB in 40 kHz with Doppler of 3 kHz either way (`tests/decode/test_apt.cpp`).
NOAA HRPT to a word error rate under 0.01 at Eb/N0 12 dB and 0.1 at 9 dB
(`test_hrpt.cpp`); TIP and AIP are not decoded. CCSDS, BPSK with K=7 and
RS(255,223), recovers every frame at Eb/N0 6 dB with 300 Hz of drifting Doppler
(`test_ccsds.cpp`); turbo and LDPC codes are not done. FUNcube, in
`tests/decode/test_funcube.cpp`: 4 of 4 frames at Eb/N0 7 and 10 dB, 2 of 4 at
5 dB, 150 Hz off and drifting 5 Hz/s; neither document states differential
encoding and it is assumed. DVB-S holds every RS block at Eb/N0 4.5 dB, rate
1/2, 2.5 kHz off (`tests/decode/test_dvbs.cpp`); elementary streams are
identified by type and not decoded. Cospas-Sarsat first generation, in
`tests/decode/test_cospas_sarsat.cpp`: T.001 Annex B1's worked message
reproduces its BCH and the 15 Hex ID ADCD00800440401; 3 of 3 bursts at Eb/N0
12 and 20 dB with 3 kHz of Doppler drifting 60 Hz/s, 1 of 3 at 9 dB. A decode
is a report, not an alert service.

**GNSS.** GPS and QZSS L1 C/A: all 210 PRN codes match IS-GPS-200N's printed
first chips, acquisition finds 10 of 10 at 35 dB-Hz, 8 at 33 and 2 at 31 from a
single 40 ms block, and the decoder costs 0.23 s of CPU per second at 2.048
MS/s (`tests/decode/test_gnss_acq.cpp`, `test_gps_lnav.cpp`). GLONASS L1OF,
0.30 s per second at 4.096 MS/s (`test_glonass_l1.cpp`); its relative code and
meander conventions were read from a figure and are proven only against this
project's own transmitter. BeiDou B1I, 0.48 s per second
(`test_beidou_b1i.cpp`). Not done: a position fix; BeiDou D2 from the GEO
satellites; SBAS, whose codes are free and whose message layer is only in the
purchasable RTCA DO-229; NavIC, stopped on its document's reproduction
terms. L1C, L2C and L5 are done; see "Decoders merged later on 2026-11-02"
below. WHAT THE LAST SENTENCE USED TO SAY, at its end: "purchasable RTCA
DO-229; L1C, L2C, L5 and NavIC".

**Short-range and paging.** BLE advertising, 200 packets a point in
`tests/decode/test_ble.cpp`: packet error rate 0.99, 0.905, 0.51 and 0.175 at
Eb/N0 8, 10, 12 and 14 dB; LE 2M, LE Coded and data channels are not done.
Z-Wave, 100 frames a point in `test_zwave.cpp`, at Eb/N0 10, 12, 14 and 16 dB:
R1 1.00, 0.73, 0.13, 0; R2 0.55, 0.07, 0, 0; R3 0.39, 0.09, 0.01, 0.01.
EnOcean is held to 0.1 at 18 dB in 250 kHz (`test_enocean.cpp`); SOF variant
1 and the Equipment Profiles are not done. DECT P32 A-fields are held to 0.1
at 18 dB in 1.152 MHz (`test_dect.cpp`); the B-field is located only, and
voice, descrambling and encryption are out. ERMES decodes all five messages at
14 dB in 25 kHz and through a 12 us echo 6 dB down (`test_ermes.cpp`);
long-message joining is left to the caller.

**Broadcast and image.** DRM30, mode B, keeps all but 9 stream frames at 15 dB
in 10 kHz with 16-QAM and at 25 dB with 64-QAM and a 1 ms echo
(`tests/decode/test_drm.cpp`); the audio, AAC or xHE-AAC, is not decoded, and
DRM+ is not done. DAB at 10 dB over the full 2.048 MHz with multipath and 3412
Hz off: no bad FIB, every stream frame and the MOT object recovered
(`test_dab.cpp`); Layer II and DAB+ audio, UEP, TII and Journaline are not
done. ATV luminance holds a mean error under 16 grey levels through FM at 16
dB in 20 MHz (`test_atv.cpp`); colour, SECAM and sound are not done, and
BT.1700 did not download. SSTV, every Dayton mode clean over 24 dB PSNR, and
FAX480 at 28, 18 and 12 dB PSNR at 30, 15 and 8 dB in 2500 Hz, 40 Hz off
(`test_sstv.cpp`). The handbook modes, AVT, MMSSTV and MSCAN, with the 16-bit
VIS extension, followed the same day; see below.
WHAT THE SENTENCE BEFORE USED TO SAY: "modes outside the Dayton paper and the
16-bit VIS extension are not done".

**Decoders merged later on 2026-11-02.** The same rules as the block above:
documents from each file's header, figures from the named test, "held to" for a
bound, nothing checked on air. Where a test asserts every packet from some
point up, that point is given; below it the test prints and does not fail.

| Mode | File | Document | What comes out |
| --- | --- | --- | --- |
| MFSK16 and MFSK8 | `core/decode/mfsk16.h` | Greenman, "MFSK16 Technical Specification" Rev 1.2 (11 July 2000); "The IZ8BLY MFSK Varicode" 1.0; "The MFSK FEC Coder" 1.0; "The IZ8BLY Diagonal Interleaver" | Latin-1 text |
| DominoEX 4 to 22 | `core/decode/dominoex.h` | Greenman, "DominoEX Technical Information"; "DominoEX Varicode Table" 2.0 (28 July 2009) | Primary and secondary text |
| FSQ | `core/decode/fsq.h` | Greenman, "FSQ"; "WSQ Varicode V3.0" (December 2013) | Characters; no FSQCALL layer |
| Feld-Hell | `core/decode/feld_hell.h` | Greenman, "Feld-Hell" page | Columns of 14 slots as an image; no text |
| MIL-STD-188-110 clock tracking | `core/decode/mil110_rx.h` | MIL-STD-188-110C CN1 5.3.2; Proakis and Mengali and D'Andrea for the receiver | The serial-tone decoder's bits with `clock_ppm` |
| MIL-STD-188-110 Appendix C | `core/decode/mil110c.h` | MIL-STD-188-110C CN1 (3 January 2012) Appendix C, Tables C-I to C-XVI | User bits at 3200 to 12800 bit/s |
| 39-tone | `core/decode/mil_39tone.h` | MIL-STD-188-110B (27 April 2000) Appendix B | The synchronous data stream, 75 to 2400 bit/s |
| 3G ALE | `core/decode/ale_3g.h` | MIL-STD-188-141B (1 March 1999) Appendix C | BW0 and BW1 bursts as 3G-ALE and TM PDUs with CRC status |
| Link-11 CLEW | `core/decode/link11.h` | MIL-STD-188-203-1A (8 January 1988) | 30-bit frames, control codes, 24-bit words with Hamming status |
| SSTV handbook modes | `core/decode/sstv.h` | Bruchanov, "Image Communication on Short Waves" (2012, 2019 build) 4.2.5, 4.4.1, 4.4.3, chapter 5 | AVT 24, 90, 94, 125, 188; MMSSTV MP73 to MP175, MR73 to MR175, ML180 to ML320; MSCAN TV-1 and TV-2 |
| HF radiofax | `core/decode/wefax.h` | WMO-No. 386 Vol I (2009) Part III 5; NWS Worldwide Marine Radiofacsimile Broadcast Schedules (7 March 2025) | Chart rows at 60 to 240 lines per minute, IOC 576 or 288 |
| Baseline JPEG | `core/decode/jpeg_baseline.h` | ITU-T T.81 (09/92); T.871 (05/2011) clause 7 | Gray8 or RGB888, with a damaged flag |
| Meteor-M LRPT | `core/decode/lrpt.h`, `ccsds_downlink.h` | CCSDS 131.0-B-5, 732.0-B-4, 133.0-B-2; no Roshydromet document | VCDUs and Space Packets by APID; no imagery |
| Metop AHRPT | `core/decode/ahrpt.h` | EUMETSAT TD 18, EUM/OPS/TEN/08/1663 v3A (12 July 2021); CGMS 2.8 8.3; CCSDS 131.0-B-5 3.4 | Source packets named by instrument; no imagery |
| GK-2A LRIT | `core/decode/gk2a_lrit.h`, `xrit.h` | GK2A LRIT Mission Specification Document 1.0 (31 May 2019); CGMS 2.8 | LRIT files; uncompressed images to the image sink |
| GOES HRIT and EMWIN | `core/decode/goes_hrit.h`, `xrit.h` | Aerospace ATR-2010(5482)-1 (10 October 2009); CGMS LRIT/HRIT Global Specification 2.8 (30 October 2013) | xRIT files and EMWIN text bulletins |
| GOES GRB | `core/decode/grb.h`, `dvbs2.h` | GOES-R PUG Vol 4, 416-R-PUG-GRB-0348 Rev 2.3 (1 June 2020); ETSI EN 302 307-1 V1.4.1 (2014-11) | One polarisation's AOS frames and Space Packets |
| WSPR | `core/decode/wspr.h` | G4JNT, "The WSPR Coding Process" (June 2009) | Type 1 call, grid and power with frequency, DT, drift and SNR |
| JT9 | `core/decode/jt9.h`, `wsjt72.h` | G4JNT, "The JT9 Coding Process" (March 2016); QEX September/October 2005 section 4 | 72-bit messages |
| JT4 | `core/decode/jt4.h`, `wsjt_slow.h` | G4JNT, "The JT4 Coding Process" (July 2009) | 72-bit messages, JT4A to JT4G |
| JT65 | `core/decode/jt65.h`, `rs64.h` | K1JT, QEX September/October 2005; Franke and Taylor, QEX May/June 2016 | 72-bit messages and the OOO form |
| VDES turbo, Link IDs 5 to 7 | `core/decode/turbo.h`, `vdes.h` | ITU-R M.2092-2 (02/2026) Annex 2 A2-1.2.4, Tables 4 to 7, Annex 4 | ASM-TER packets at rate 3/4 |
| Cospas-Sarsat 406 MHz SGB | `core/decode/cospas_sarsat.h` | C/S T.018 Issue 1 Revision 7 (March 2021) | Second-generation messages with BCH(250,202) checked and the 23 Hex ID |
| GPS L1C CNAV-2 | `core/decode/gps_l1c.h`, `gps_l1c_tables.h` | IS-GPS-800J (1 August 2022) | Per satellite: handover, TOI, subframe 2's WN, ITOW, top, toe, URA_ED and L1C health, subframe 3's PRN and page with raw bits |
| GPS L2C CNAV | `core/decode/gps_cnav.h` | IS-GPS-200N (1 August 2022) 3.3.2.4, 3.3.3.1, Appendix III | CRC-checked 300-bit messages: header, and type 10's WN, top and toe |
| GPS L5 CNAV | `core/decode/gps_cnav.h` | IS-GPS-705J (1 August 2022) 3.3.2.2, 3.3.3.1, Appendix II | The same, from I5 |

**Keyboard modes.** Error rates are character error rates, means of three or
four runs in 2500 Hz with drift. MFSK16 is held to 0.01 at 0 and -9 dB, MFSK8
to 0.01 at -10 dB (`tests/decode/test_mfsk16.cpp`); MFSK8's 32-tone Gray code
is tabled nowhere and a 5-bit reflected code is assumed. DominoEX 11 is held to
0.01 at 0 dB and 0.02 at -6, DominoEX 4 to 0.02 at -10 (`test_dominoex.cpp`);
44 and 88 are in no document read, and the varicode duplicates in the scope
table decode as ":" and "4". FSQ is held to 0.03 at 0 dB and 0.02 at -8
(`test_fsq.cpp`); one 0 dB run loses seven characters the -8 dB run keeps, cause
not established, and the slower speeds, sentence mode, FSQCALL and images are
not done. Feld-Hell's slot error rate is held to 0 at 20 and 10 dB and 0.001 at
6 dB (`test_feld_hell.cpp`). The Feld-Hell row's 1998 technical specification
now answers 404, and the decoder was written from the Feld-Hell page instead.

**Military HF.** The 110C serial-tone receiver now tracks symbol timing: 2400
bit/s for 60 s at 12 dB with 100 ppm either way is held to a bit error rate
under 1e-3, `clock_ppm` within 20 ppm (`tests/decode/test_mil110.cpp`). Five
readings of the document are labelled in the header, among them Table XII,
whose printed values contradict its text, and the text is followed. Appendix C
round-trips at Table C-XVII's SNRs, 9, 13, 16, 19, 21 and 27 dB for 3200 to
12800 bit/s, held to 0.01 on AWGN at 75 Hz and on two paths 3 dB higher, and to
1e-3 at 100 ppm over 60 s; acquisition on a reinserted preamble is not done.
STANAG 4539 was not read. The 39-tone decoder is exact on every rate clean and
held to 0.01 at 15 dB and up on AWGN and two paths at 2400 bit/s, and at 3 dB
and up at 75 bit/s (`test_mil_39tone.cpp`); the B.5.7 asynchronous mode is not
done. 3G ALE decodes 29 of 30 BW0 bursts at 0 dB on AWGN and two paths
(`test_ale_3g.cpp`); BW2 to BW4 are not decoded, BW5 is not in 141B, and the
transmit filter is unspecified, so root raised cosine 0.35 is used. Link-11
holds word error rate under 0.02 at 15 dB and up, 40 Hz off, on AWGN at both
rates and on two paths at 1364 bit/s only (`test_link11.cpp`);
the word meanings are MIL-STD-6011's and are not decoded, nor is SLEW.

**Image.** The handbook SSTV modes are held to total times within 2 percent and
lines per minute within 0.1 percent of Bruchanov's tables, PSNR over 24 dB
clean, and AVT 90 to PSNR floors of 28, 16 and 10 dB at 30, 15 and 8 dB
(`tests/decode/test_sstv.cpp`). MP-N and MC-N are not done because the N-VIS bit
order is in a figure the handbook does not resolve, Proskan J120 because the
handbook gives no line structure, and DSSTV over RDFT was not attempted. Where
Table 4.10 and chapter 5 disagree the header names the one followed. Radiofax is
held to PSNR over 24 dB clean at five speed and IOC combinations and to floors
of 28, 19 and 13 dB at 30, 15 and 8 dB (`test_wefax.cpp`); polarity is a
setting, since WMO and North American practice disagree. Baseline JPEG is held
to PSNR floors of 30 to 45 dB on a test encoder written from T.81
(`test_jpeg_baseline.cpp`); progressive, lossless, hierarchical and arithmetic
coding are not done. It is there for SSDV, which is blocked on its document.

**Weather satellites.** All four share `core/decode/ccsds_downlink.h`, which
tries each rail and puncture hypothesis until Reed-Solomon accepts a frame,
because no document says how coded symbols split across I and Q. Meteor-M
LRPT recovers every packet at Eb/N0 4 dB and up for QPSK and 6 dB for OQPSK,
with 2.5 kHz of Doppler (`tests/decode/test_lrpt.cpp`); its rates and
randomiser are Blue Book defaults and the summary above, unchecked against a
recording, and the MSU-MR payload is not decoded. Metop AHRPT recovers every
packet at 6 dB and up with 38 kHz of Doppler (`test_ahrpt.cpp`); 2.333 Msym/s
is arithmetic, MO-IF-MMT-SY-0001 is not public, so no AVHRR image is made.
GK-2A recovers every file at 4 dB and up (`test_gk2a_lrit.cpp`); JPEG 2000 and
DES files are passed on as files. GOES HRIT recovers every file and the EMWIN
text at 4 dB and up (`test_goes_hrit.cpp`). No NOAA HRIT/EMWIN product
specification was located: header type 129's 14-octet layout and the EMWIN
file type were read from two sample `.lrit` files on NOAA's OSPO page, labelled
as observation, and compressed images are passed on as files. GRB, one
polarisation, recovers every packet at Es/N0 8 dB and up for QPSK 9/10 and 10
dB for 8PSK 2/3, synthetic only at 20 MS/s (`test_grb.cpp`); it needs about
10.8 MHz, beyond an RTL-SDR. The PUG gives no randomiser or Reed-Solomon and
none is applied; 8PSK failed at a 250 kHz offset and should be kept within
200 kHz. The DVB-S2 layer does normal frames at those two MODCODs only, and
`core/decode/bch.h` now takes m up to 16 for it, exercised through the GRB test
rather than `test_bch.cpp`.

**Weak signal.** Thresholds are 8 signals, 2 trials a point. WSPR is held to
90 percent decoded at -26 dB and a threshold at or below -27 dB
(`tests/decode/test_wspr.cpp`), JT9 to -23 and -24 (`test_jt9.cpp`), JT4A to
-18 and -20 (`test_jt4.cpp`), JT65 to -21 and -24 (`test_jt65.cpp`). None of the
four documents gives the slot timing or the search window, and each header says
so. The JT65 Reed-Solomon field polynomial, roots and message placement are not
printed in either paper and were derived as the only choice that makes Figure
2's three listings code words; the 2016 paper's error table and constants are
not printed either, and substitutes are labelled. The 72-bit layer packs and
unpacks standard and plain-text messages only: CQ, QRZ, DE, reports, RO, RRR,
73 and the Appendix B prefixes are deferred to source the papers name and this
project does not read. JT9's fast submodes E to H are not done.

**VDES turbo and Cospas-Sarsat second generation.** Link IDs 5, 6 and 7 are
held to a packet error rate of 0.15 at 8 dB and 0 or 400 Hz off, and Link ID 5
to 0.1 at 10 dB through multipath (`tests/decode/test_vdes.cpp`); the turbo
code alone to 2 failed blocks in 40 at Eb/N0 1.5 dB (`test_turbo.cpp`). Table 6
names tail pattern 8b and Table 7 and the Annex 4 example match 8a, which is
used, and the Annex 4 example is reproduced exactly. SGB reproduces T.018's
PRN first chips and Appendix B check bits, corrects 6 errors and refuses a
seventh, and decodes every burst at C/N0 50 dB with chip rates 1.2 off
(`test_cospas_sarsat.cpp`); the sensitivity sweep recorded every burst at 38
dB-Hz, 8 in 10 at 36 and none at 34. T.018 Issue 1 Revision 7 is watermarked as
superseded and no later issue was found. A decode is a report, not an alert
service.

**GPS L1C, L2C and L5.** The codes are checked against the documents' tables:
all 126 L1C ranging codes match Table 3.2-2's first and last 24 chips, the
overlays match Table 3.2-3, and the LDPC submatrix counts match Tables 6.2-14
to 6.2-17 (`tests/decode/test_gps_l1c.cpp`); the L2 CM and CL generator
reproduces Table 3-IIa's end states for all 37 PRNs and the L5 I5 and Q5 codes
Table 3-Ia for all 74 (`test_gps_cnav.cpp`). L1C is not searched for cold: it
is handed over from C/A tracking, and six of six satellites at C/A 35 to 45
dB-Hz hand over on the first try and decode frames. The LDPC decodes 20 of 20
words at Eb/N0 3 dB, held to 18. The pilot replica is plain BOC(1,1), so
TMBOC's BOC(6,1) symbols are not modelled and about 1.1 dB of pilot power is
lost. The TOI bit order was read from Figure 3.2-4 and no test vector checks
it. L2C correlates CM alone, 3 dB under a CM+CL receiver; it decodes six
satellites at 35 to 45 dB-Hz at 2.048 MS/s. L5 is synthesised over the full
20.46 MHz main lobe and decimated to 4.092 MS/s, which loses about 9 dB; it
still decodes four satellites. Only the CNAV header and message type 10's WN,
top and toe are parsed; every other type is passed on as its 300 bits. Not
done: a position fix, CNAV clock and ephemeris types, QZSS on these signals,
and NavIC, stopped on its document's terms.

**Reaching them from a client.** Decoder names a client can now attach,
through `Session::subscribeDecoded`: `mfsk16`, `mfsk8`, `dominoex4`,
`dominoex5`, `dominoex8`, `dominoex11`, `dominoex16`, `dominoex22`, `fsq`,
`feld_hell`, `mil110c`, `mil_39tone`, `ale_3g`, `link11` and `wefax`; and
through `subscribeWidebandDecoded`, which feeds a decoder the source's own
stream decimated to at least its floor, `lrpt` and `lrpt_oqpsk` from 288000
S/s, `ahrpt` from 6000000, `gk2a_lrit` from 512000 and `goes_hrit` from
2400000. The registry also holds `gps_l1c` and `gps_l2c`, each at 2.048 MS/s
or more on a tap at 1575.42 and 1227.60 MHz, and `gps_l5` at 4.092 MS/s or
more at 1176.45 MHz (`core/rpc/decoders.h`). `docs/rpc.md` has the registry table. WSPR, JT9, JT4, JT65, GRB and
SGB decode in `core/` and are not in the registry yet. `revenant-cli` under
`auto` now attaches only the decoders that can be built at the receiver's rate,
so `vdl4` on a 48 kHz `nfm` receiver is skipped rather than refusing its first
chunk and faulting the run; a decoder named explicitly is still attached and
refuses in its own words (`tools/cli/main.cpp`).

## What would change the list

**A patent expiring.** The dated rows are in the patent exclusion table.
2029 and 2030 take the AAC pool's parametric stereo patents, 2030 takes the LoRa
base claim, 2032 takes xHE-AAC and 2032 to 2034 takes DVB-S2X. Meshtastic and
LoRaWAN move with LoRa and nothing else about them is a problem.

**A specification being published or located.** Three modes are blocked purely
on a document: Fusion, FLEX and MELPe. Beyond those, a Roshydromet MSU-MR
interface document would give Meteor-M LRPT's payload a citation instead of an
observation label, a CMA format document would move FY-3 and FY-2 from
metadata-only to include, the ORBCOMM Air Interface Specification would do the
same for ORBCOMM, and the annexes to MIL-STD-3005 would unlock MELPe below 2400
bit/s.

**A hardware vocoder being present.** Every AMBE and AMBE+2 row in the
metadata-only table becomes a voice mode the moment a dongle is in the machine,
and none of the decoder work changes, because the framing decoders are what feed
it. That is the reason the framing is worth building before the dongle exists
rather than after. Task #40 is the dongle and task #43 is the plugin ABI.

The ABI is `core/decode/vocoder_abi.h`, since 2026-09-21, and two modes have
gone through it, both from the included tables: since 2026-10-02 D-STAR's and
DMR's voice reach a loaded plugin that serves them, `revenant-engine` prints
what it loaded at startup, and `Session.vocoderPlugins` shows it in the
client's radio panel. The AMBE rows of the metadata-only table have no framing
decoder yet, so a plugin there would have nothing to be handed.

**US8306071 expiring on 2027-02-19.** It ends the one risk the owner accepted
to put DMR back in scope, and the paragraph under "What decides inclusion"
naming DMR as the exception goes with it.

WHAT THIS PARAGRAPH USED TO SAY: "**The ETSI IPR query coming back.** It
could tighten DMR rather than loosen it." It came back on 2026-09-21 and did
tighten it; "The ETSI IPR query, and what it did to DMR" has the query and the
decision that followed.

## What is not verified

Collected so it can be worked through as a list rather than rediscovered one
mode at a time. Everything here is either a document nobody has held or a
conclusion that rests on reasoning rather than a check.

**Documents nobody has opened.** Yaesu "Amateur Radio Digital Standards" rev
1.02 (HTTP 500 from two paths, 2026-09-20). ARIB STD-T43 English edition (404
from two paths). MIL-STD-3005 (absent from EverySpec's index; DLA ASSIST did not
answer a GET). ARRL technical-characteristics pages for CLOVER, CLOVER-2000,
Q15X25, CHIP64, RWOP and FDMDV. IEC 61097-4:2024, and whether it carries the
Inmarsat-C frame layout or only equipment performance.

**Identifiers that carry a verdict and did not resolve.** The web.archive.org
capture of the barberdsp Dayton SSTV paper. 417-R-IRD-0168 (GOES HRIT header type 131). MIL-STD-188-203-3 (Link-4A). The
ORBCOMM Air Interface Specification.

WHAT THE LIST ABOVE USED TO SAY, until 2026-11-02, also held
"arXiv:2603.12062, the Iridium security analysis under the Iridium burst
entry", which now resolves and lacks the Ring Alert layout, and "Whether ARRL
still serves `ft4_ft8_protocols.tgz` at the QEX Binaries 2020 path". It does
not, nor does princeton.edu; the archive was read from the Wayback Machine's
capture of 2020-01-07, and the public-domain grant is in section 9 of the QEX
2020 paper rather than in the archive. `docs/clean-room.md` records both. It
also held "EPS.ASPI.DS.0675 (Metop)", dropped on 2026-11-02 because the
identifier does not resolve to any document and the AHRPT decoder was written
without it.

**Patent positions resting on the era of the system rather than a search.** For
anything standardised after roughly 2006 the twenty-year argument is
unavailable, and these are in that window: NAVDAT (ITU-R M.2010, 2012 to 2019),
VDES (M.2092, 2015 to 2026), Cospas-Sarsat second-generation beacons, Wi-SUN,
the 10 MHz channelisation and outside-the-context-of-a-BSS operation that
802.11p added to the 802.11a physical layer, and LPCNet and FARGAN. The Iridium
legacy conclusion rests on Motorola's 1990 to 1995 filing era, not a docket
check.

**Editions and revisions to confirm at purchase or implementation time.**
EN 13757-4, 2025 versus 2019. The IEC 62106 part split past part 1, which is
"Modulation characteristics and baseband coding" in the 2018 edition. RTCA DO-185B versus
-185C. C/S T.001 and C/S T.018 issue and revision. IS-QZSS-PNT revision. The
GOES-R HRIT/EMWIN product specification revision. The IMO International
SafetyNET Manual edition. Whether the current revisions of MIL-STD-188-181
through -185 all carry Distribution Statement A.

**Technical questions left open.** Whether GOES HRIT image data is Golomb-Rice
under xRIT header type 131 or JPEG 2000. Whether the Q65 GF(64) log and antilog
tables follow from the primitive polynomial, in which case they are facts and
can be generated, or whether Palermo's construction picks a non-obvious
primitive element. Whether copyright subsists in a designed LDPC generator
matrix or a QRA generator table at all, which four modes now hit and which the
recorded-exception route sidesteps without deciding. Opulent Voice's modulation,
which is self-inconsistent as published. The exact per-mode FreeDV carrier
counts and symbol rates, which live in the codec2 repository documentation.

**Modes not researched that probably belong here.** New Packet Radio (NPR-70),
an open-specification 70 cm data mode with a published design. Ribbit and the
other open text-over-FM projects. Q15X25, CHIP64, RWOP and FDMDV, all of which
have free ARRL pages nobody has read, and the last of which is probably subsumed
by FreeDV 1600.

## Open questions for the owner, 2026-11-02

Raised by the decoder work of that day and not decided by it.

- **Tables reproduced in source.** The MFSK and DominoEX varicode tables sit in
  the tree, from pages marked "All rights reserved". C/S T.001 Table A3's
  modified Baudot code is in `core/decode/cospas_sarsat.cpp`, cited. DVB-S2's
  LDPC tables B.6 and B.11 are transcribed from EN 302 307-1 into
  `core/decode/dvbs2_tables.cpp` under an ETSI copyright note. Keep each, or
  fingerprint it as the DMR ETSI tables are?
- **NXDN.** The NXDN Forum's download sits behind a guest form asking for
  personal details and agreement to its copyright terms; the only other copy
  is a qsl.net mirror. Fill in the form, or leave NXDN unwritten?
- **MPT-1327.** Ofcom's archive is behind Cloudflare and the Wayback Machine
  refuses automated fetches; the only reachable copy is sigidwiki's mirror.
  Fetch it by hand, accept the mirror, or leave it?
- **IEEE GET sign-in.** IEEE 802.15.4-2020 (Zigbee) and IEEE 802.11-2020 (DSRC
  and ITS-G5) both need a signed-in download, and the Wi-SUN profile is
  members-only. Download the IEEE documents?
- **FreeDV.** The design notes lack the HRA_112_112 LDPC matrix, the unique
  word and pilot values, the mappings and the tone and carrier frequencies,
  which are only in codec2 source. Link libcodec2's FreeDV modem as a recorded
  copyleft exception, measure its transmit output, or leave FreeDV undecoded?
- **NavIC.** ISRO-IRNSS-ICD-SPS-1.1 is a free download and forbids
  reproduction without ISRO's written consent, so its code tables and frame
  layout could not be transcribed with citation as the GPS ones are. No
  decoder was written. Write to ISRO for consent, or leave NavIC out?
