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

## Included

### Land mobile, and the open alternatives to it

Voice on the AMBE systems is excluded in every row below that names it. The
framing, addressing, control and text messaging are not, and that is most of
what these systems carry in the clear.

| Mode | Numbers | Specification | Effort |
| --- | --- | --- | --- |
| dPMR446 and licensed modes 2/3 | 4FSK, 2400 sym/s, 4800 bit/s, 6.25 kHz FDMA | ETSI TS 102 490 and TS 102 658, free | Medium |
| NXDN (IDAS, NEXEDGE) | 4FSK; 6.25 kHz at 2400 sym/s, 80 ms frames; 12.5 kHz at 4800 sym/s, 40 ms frames; RRC alpha 0.2 | NXDN TS 1-A v1.3, November 2011, free from the NXDN Forum, download permitted and redistribution forbidden | Medium |
| P25 Phase 1 FDMA | C4FM 4FSK, 4800 sym/s, 9600 bit/s, 12.5 kHz; CQPSK/LSM on simulcast at the same symbol rate | TIA-102.BAAA-A plus the TIA-102.AABx trunking set, purchasable | Large |
| TETRA V+D and Direct Mode | pi/4-DQPSK, 18000 sym/s, 36 kbit/s, 25 kHz, 4-slot TDMA, 14.167 ms slots | ETSI EN 300 392-2 and the EN 300 396 series, free | Large |
| TEDS | pi/8-D8PSK and 4/16/64-QAM in 25, 50, 100 and 150 kHz, 2.7 kHz subcarrier spacing | ETSI EN 300 392-2 V3.x, free | Large |
| D-STAR DV | GMSK BT 0.5, 4800 bit/s, 6.25 kHz; 2400 AMBE plus 1200 FEC plus 1200 data | JARL "アマチュア無線のデジタル化技術の標準方式" Ver 7.0, free at `jarl.com/d-star/STD7_0.pdf`. Japanese only: the English edition this row used to name is gone, see the note below the table | Small |
| D-STAR DD | GMSK 128 kbit/s on 23 cm, Ethernet frames, roughly 150 kHz | Same JARL standard | Medium |
| M17 | 4FSK, 4800 sym/s, h=1/3, 9 kHz occupied, 384-bit 40 ms frames, PRBS9 randomiser, Golay(24,12), punctured r=1/2 K=5, QPP interleaver, CRC-16 0x5935 init 0xFFFF | M17 Protocol Specification Part I, free at spec.m17project.org, complete to bit level | Medium |
| FreeDV 1600, 700C, 700D, 700E | OFDM in an SSB passband; 1600 is 16 DQPSK carriers in about 1.1 kHz; 700D is 17 coherent QPSK carriers at 25 baud, 160 ms frame, 2 ms cyclic prefix, LDPC | No standards document. Project design notes and the FreeDV user manual, cited per file with a retrieval date | Large |
| FreeDV 2400A, 2400B, 800XA | 4FSK; 2400A at 2400 bit/s in about 5 kHz of RF, 2400B through an analogue FM audio path, 800XA at 800 bit/s | As above | Medium |
| Opulent Voice | 40 ms frames of 134 bytes, CCSDS K=7 r=1/2 to 268 bytes, 67x32 interleaver, 24-bit sync 0x02B8DB, roughly 81.3 kHz | `opulent_voice_protocol.md` in OpenResearchInstitute/interlocutor, free and public | Medium |
| MPT-1327 | FFSK 1200 bit/s sub-audio over analogue FM, 12.5 or 25 kHz, 64-bit codewords, BCH(63,48) plus parity | MPT 1327, UK DTI 1988 rev 1997, now Ofcom, free | Small |

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

**The M17 specification document is GPL-2.0.** The repository's own LICENSE file
is the plain version 2 text with no "or later" at the root, which under this
project's per-file rule establishes 2.0-only until a per-file notice says
otherwise. Earlier survey text called it GNU Free Documentation License 1.3 and
that is contradicted by the repository. The verdict does not move, because sync
words, polynomials and bit layouts are facts and Revenant writes its own code
from them. The operative rule is unchanged: cite clause and figure numbers,
never paste a table, a figure or a paragraph.

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
| MSK144 | MSK, 2000 baud, 1000 Hz shift, 144-bit 72 ms frames, 77-bit payload, CRC-13, LDPC(128,90) | G4JNT December 2021 LDPC modes document, free. The QEX 2017 paper describes the superseded 72-bit form | Medium |
| Q65 | 65-FSK, 85 symbols with a 22-symbol sync vector, submodes 15 s to 300 s, QRA(65,15) over GF(64), CRC-12 | Quick-Start Guide to Q65 and "The Q65 Coding Process", G4JNT, both free | Large |
| JS8 | 8-GFSK, 79 symbols, four speeds from 3.125 to 20 baud, 75-bit frames in six types | JS8Call User Guide, free; Appendix A prints the modified Huffman code. The dictionary compression table is not published | Medium |
| PSK31, PSK63, QPSK31 | BPSK 31.25 or 62.5 baud, cosine envelope, roughly 60 Hz occupied; QPSK adds r=1/2 K=5 | G3PLX in RadCom Dec 1998 and Jan 1999, reprinted free by ARRL with the full varicode table | Small |
| RTTY (ITA2) | 2-FSK, 45.45 baud, 170 Hz shift standard, 5-unit alphabet, 1.5 stop bits | ITU-T Recommendation S.1 for the alphabet and S.3 for the 7.5-unit character, free; the 45.45/170 convention is practice and is cited as such. Done, see below | Small |
| AMTOR SITOR-A and SITOR-B | 2-FSK, 100 baud, 170 Hz shift, CCIR 476 7-unit 4/3 code, 450 ms ARQ cycle or 280 ms time-diversity FEC | ITU-R M.625-4 (03/2012, in force) and M.476-5, free | Small |
| Olivia | MFSK 2 to 256 tones over 125 to 2000 Hz; default 32/1000 at 31.25 baud; Walsh FEC scrambled by 0xE257E6D0291574EC | "The Draft Specification For The Olivia HF Transmission System", SP9VRC, hosted free by ARRL | Medium |
| Contestia | Olivia geometry, six-bit alphabet, half the FEC | fldigi mode documentation only. Cite the page and the retrieval date | Small |
| MT63 | 64 DBPSK tones, 10 symbols/s per tone, 15.625 Hz spacing in MT63-1000, Walsh expansion over 32 symbols | ARRL MT-63 technical characteristics page, free | Medium |
| MFSK16 and MFSK8 | 16 tones, 15.625 baud, about 316 Hz occupied, IZ8BLY varicode, r=1/2 K=7, diagonal interleaver | ARRL MFSK page plus ZL1BPU's pages, free | Small |
| DominoEX | IFK+ over 18 tones, submodes 4 to 88, DominoEX 11 at 11.719 baud | ARRL Domino page and "DominoEX Technical Information", free | Small |
| THOR | DominoEX IFK+ with the MFSK16 varicode and r=1/2 K=7 FEC | fldigi mode documentation only | Small |
| FSQ and FSQCall | IFK+ over 33 tones at three times the symbol rate, 8.79 Hz spacing at 2.93 baud | Author documentation at w1hkj.org and qsl.net/zl1bpu, publicly disclosed by the authors | Small |
| Hellschreiber (Feld-Hell and variants) | OOK raster, 122.5 baud element rate, 7x14 element cell, 2.5 char/s, about 75 Hz | "Hellschreiber Modes - Technical Specifications", ZL1BPU 1998, free | Small |
| Throb and ThrobX | Multi-tone OOK at 1, 2 or 4 symbols/s over 9 or 11 tones | fldigi mode documentation only | Small |
| AX.25 packet | 1200 baud Bell 202 AFSK, 9600 baud G3RUH scrambled FSK, 300 baud HF FSK; HDLC with bit stuffing and the X.25 FCS | AX.25 v2.2, TAPR/ARRL July 1998, free. The two modems have no standards document and are cited to Bell 202 and to Miller's 1988 article. 1200 baud done, see below | Medium |
| APRS | AX.25 UI frames; position, weather, telemetry, object, status, message, Mic-E and base-91 compressed formats | APRS Protocol Reference 1.0.1, free at aprs.org, plus the WB2OSZ consolidated 1.2. Position, status, message and Mic-E done, see below | Medium |
| FX.25 | AX.25 with a 64-bit correlation tag selecting one of several Reed-Solomon configurations | Stensat Group, TAPR DCC 2006, free | Small |
| IL2P | Reed-Solomon header and payload blocks replacing HDLC on the same physical layers | IL2P Specification Draft v0.6, 16 March 2024, KK4HEJ, free | Small |
| PACTOR-I | 2-FSK, 100 or 200 baud, 200 Hz shift, 1.25 s ARQ cycle, 0x55 sync header, ITU-T CRC | ARRL PACTOR technical characteristics page, free, which prints the complete Huffman table, packet structure, status byte and ARQ timing | Medium |
| ARDOP | 200, 500, 1000 and 2000 Hz sessions, 50/100/167 baud, 4FSK narrow and OFDM wide, two-tone leader at 1450 and 1550 Hz | ARDOP Specification, Winlink Development Team, free, and released to the public domain by its author | Large |
| WINMOR | OFDM in 500 or 1600 Hz, DPSK and 4FSK | ARRL WINMOR technical characteristics page and the author's TNC specification, free. Winlink retired the mode in 2020 | Small |
| Winlink B2F | Message structure and forwarding over any bearer, LZHUF-compressed body | "Open B2F", Winlink Development Team, free at winlink.org/B2F | Small |
| CW | OOK, 5 to 40 WPM, dot length 1200/WPM ms, 100 to 200 Hz occupied | ITU-R M.1677-1 (10/2009), free | Small |
| SSDV | 256-byte packets, 205 bytes of image with Reed-Solomon, baseline JPEG with fixed tables | UKHAS wiki `guides:ssdv`, free. Payload format is ISO/IEC 10918-1, free as ITU-T T.81 | Small |
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
| 3G ALE / ARCS | Burst waveforms BW0 to BW5, 8PSK at 2400 baud on 1800 Hz, 3 kHz | MIL-STD-188-141D Appendix C, free. STANAG 4538 is the NATO twin and is not needed | Large |
| STANAG 4285 | 1800 Hz subcarrier, 2400 sym/s, BPSK/QPSK/8PSK, 75 to 2400 bit/s coded | STANAG 4285 Edition 1 (1989), purchasable from standards resellers | Large |
| STANAG 4529 | 1240 Hz channel, 1200 baud, BPSK through 8PSK, 75 to 1200 bit/s coded, 213.33 ms frames | STANAG 4529 Edition 1 (1995), purchasable. A parameter variant of 4285 | Medium |
| STANAG 4539 | 1800 Hz, 2400 baud, BPSK through 64-QAM, mini-probe blocks between data blocks | Cite MIL-STD-188-110B Appendix C, which is free and specifies the identical waveform, rather than the restricted STANAG number | Large |
| STANAG 4415 | 1800 Hz, 2400 baud BPSK, 75 bit/s with heavy repetition, usable near -6 dB SNR | The equivalent 75 bit/s low-SNR mode in MIL-STD-188-110B and -110C, free | Small |
| STANAG 4481 | 2-FSK 50 to 600 bit/s, usually 50 or 75 with 850 Hz shift; multi-channel FSK; a 300 bit/s BPSK variant | STANAG 4481, purchasable; the FSK variants need little beyond the emission definitions in Radio Regulations Appendix 1, free | Small |
| STANAG 5066 | Not a waveform. D_PDU framing, ARQ, addressing and the subnetwork interface above 4285, 4539 and 110 | STANAG 5066 Edition 4 (May 2022), purchasable | Medium |
| Link-11 CLEW | 16 tones: 605 Hz Doppler reference, 14 carriers from 935 Hz on 110 Hz spacing, 2915 Hz; DQPSK, 1364 or 2250 bit/s | MIL-STD-188-203-1A, free. The payload catalogue MIL-STD-6011 is distribution limited, so the deliverable stops at bits and net cycle timing | Medium |
| HF Selcall | 2-FSK, 170 Hz shift, 100 baud, CW preamble, 4 to 6 digit addresses | ITU-R M.493-16 (12/2023, verified in force), free | Small |
| ARQ-M2 and ARQ-M4 | FSK at 96 or 192 baud, 170 to 400 Hz shift, two or four interleaved telegraph channels, 7-unit 4/3 code | ITU-R F.342-2 (1970), free | Medium |
| Piccolo MK6 and MK12 | MFSK, classic form 32-ary at about 10 baud with 10 Hz spacing, roughly 320 Hz; MK6 is 6 tones in about 180 Hz | IEE 1963 and 1982 papers, purchasable from the IET digital library. They describe the modulation rather than a full profile | Medium |
| LPC-10e | 2400 bit/s, 54-bit frames of 22.5 ms | FED-STD-1015 (1984), free US Government work with the complete algorithm and tables | Small |
| FS-1016 CELP | 4800 bit/s, 30 ms frames, stochastic codebook | FED-STD-1016 (1991), free | Medium |
| CVSD | 16 or 32 kbit/s, one bit per sample, adaptive step | MIL-STD-188-113, free | Small |
| MELPe 2400 | 2400 bit/s, 54-bit frames of 22.5 ms over 180 samples at 8 kHz | MIL-STD-3005, not located (unverified). Patents confirmed expired | Large |
| DRM30 and DRM+ system layer | COFDM, modes A to D in 4.5 to 20 kHz below 30 MHz, mode E in 96 kHz VHF; FAC at 4-QAM, SDC and MSC at 16 or 64-QAM | ETSI ES 201 980, free, with TS 102 979 for Journaline and TS 102 821 and TS 101 968 alongside | Large |

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
| VDL Mode 2 | D8PSK, 10500 sym/s, 31500 bit/s, 25 kHz, raised cosine 0.6; AVLC per ISO 13239, RS(255,249) on the burst | ETSI EN 301 841-1, free from etsi.org | Medium |
| VDL Mode 4 | GFSK 19200 bit/s in 25 kHz, self-organising TDMA, 4500 slots per minute | ETSI EN 301 842-1, free | Medium |
| HFDL | 1440 Hz subcarrier, 1800 sym/s, BPSK/QPSK/8PSK giving 300 to 1800 bit/s, 32 s TDMA frame of 13 slots | ARINC 635-4, purchasable from SAE ITC | Large |
| ADS-C and FANS-1/A | No physical layer of its own; rides ACARS, HFDL, VDL2 or satellite | ARINC 622, purchasable, plus ICAO Doc 9880 | Medium |
| ASTERIX | Not a radio mode. Octet-aligned records with an FSPEC bitmap | EUROCONTROL SPEC-0149, free. Category 021 v2.7 (July 2025) for ADS-B target reports | Small |
| RDS-TMC (ALERT-C) | No new physical layer. Group 8A on the 57 kHz subcarrier already decoded | EN ISO 14819-1 through -4, purchasable from ISO and CEN | Small |
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
| AIS Class A, Class B, AtoN, base station, SAR aircraft | GMSK BT 0.4 over FM, 9600 bit/s, 25 kHz at 161.975 and 162.025 MHz; NRZI, HDLC with 0x7E flags and bit stuffing, CRC-16-CCITT; 26.67 ms slots, 2250 per minute per channel | ITU-R M.1371-6 (02/2026, verified in force, supersedes -5 of 02/2014), free | Medium |
| AIS-SART, AIS-MOB, AIS-EPIRB | Identical physical layer; a burst of eight position reports once per minute, navigation status 14, message 14 text | Annex 8 of ITU-R M.1371-6, free; IMO Resolution MSC.246(83), free | Small |
| Satellite AIS and message 27 | Same GMSK; AIS3 at 156.775 and AIS4 at 156.825 MHz; message 27 is the reduced long-range report | ITU-R M.1371-6, free | Medium |
| VDES and AIS ASM channels | ASM at 161.950 and 162.000 MHz reuses the AIS physical layer; VDE-TER is pi/4-QPSK, 8PSK and 16QAM across 25, 50 and 100 kHz to about 307 kbit/s | ITU-R M.2092-2 (02/2026, verified in force), free; IALA G1117 Edition 3.0, free | Medium |
| DSC on VHF (channel 70) | FFSK 1200 baud on 156.525 MHz, mark 1300 Hz, space 2100 Hz, 10-bit symbols, time diversity | ITU-R M.493-16 (12/2023, verified in force), free; M.541-11 for procedures | Small |
| DSC on MF and HF | 2-FSK 100 baud, 170 Hz shift, centre 1700 Hz in J2B; same symbol alphabet and diversity | ITU-R M.493-16, free | Small |
| NAVTEX | F1B FSK, 170 Hz shift, 100 baud, CCIR 476 7-unit 4/3 code, 280 ms time-diversity FEC, about 300 Hz; 518, 490 and 4209.5 kHz | ITU-R M.540-2 (06/1990, verified still in force), free, plus the IMO NAVTEX Manual for the B1 to B4 header semantics | Small |
| NBDP SITOR-A and SITOR-B | FSK 100 baud, 170 Hz shift; SITOR-A is a 450 ms ARQ cycle, SITOR-B is the NAVTEX FEC mode | ITU-R M.625-4 (03/2012, verified in force) and M.476-5, free | Small |
| NAVDAT (500 kHz and HF) | OFDM in 10.5 kHz, QPSK, 16-QAM and 64-QAM, 12 to 18 kbit/s | ITU-R M.2010-2 for 500 kHz and M.2058-1 for HF, free | Large |
| HF radiofax / WEFAX | FM subcarrier, 1900 Hz centre plus or minus 400 Hz, 1500 and 2300 Hz at the extremes, 60 to 240 lines per minute, IOC 576 or 288, start tone 300 or 675 Hz, stop 450 Hz | WMO-No. 386 (frozen 2024-12-31) plus the free NOAA/NWS Worldwide Marine Radiofacsimile Broadcast Schedules, which is the practical citation for the constants | Medium |

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
| Meteor-M HRPT | 1700 MHz RHCP, full-resolution MSU-MR, symbol rate near 665 kbit/s (unverified, community measurement) | CCSDS for the framing; no Russian interface document located | Medium |
| GOES HRIT and EMWIN | 1694.1 MHz, BPSK 927 ksym/s, r=1/2 Viterbi, RS(255,223) interleave 4, CCSDS pseudo-randomiser h(x) = 1 + x^3 + x^5 + x^7 + x^8, about 400 kbit/s | CGMS-03 LRIT/HRIT Global Specification issue 2.8, free; NOAA GOES-R HRIT/EMWIN product specification, free; Aerospace report ATR-2010-5482, free | Large |
| GOES GRB | 1686.6 MHz, dual circular polarisation, 15.5 Mbit/s per polarisation; 8PSK at 7.825768 Msym/s r=2/3 or QPSK at 8.665938 Msym/s r=9/10 | GOES Rebroadcast Downlink specification at goes-r.gov, free, plus CCSDS 133.0-B-1 | Large |
| GOES Data Collection System | Platform uplinks at 401 to 402 MHz, 100/300/1200 bit/s BPSK; DCPR downlink at 1694.5 MHz | GOES DCS Certification Standards and the DCPR channel tables, free from noaasis.noaa.gov | Medium |
| GEO-KOMPSAT-2A LRIT | 1692.14 MHz, NRZ-L BPSK, 64 kbit/s information, r=1/2 plus RS(255,223) interleave 4, so about 146 ksym/s | GK2A LRIT Mission Specification Document issue 1.0 (2019-05-31), free from nmsc.kma.go.kr. Image data is JPEG 2000 per ISO/IEC 15444-1, lossless; the DES fields are defined and marked not used | Large |
| DVB-S | QPSK, 1 to 45 MBd, roll-off 0.35, r=1/2 to 7/8 punctured convolutional, RS(204,188), interleave 12, MPEG-2 TS | ETSI EN 300 421, free | Large |

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
| POCSAG | 2-FSK, +/-4.5 kHz, 512/1200/2400 bit/s, 576-bit preamble, batches of 8 frames x 2 codewords, sync 0x7CD215D8, BCH(31,21) plus parity | ITU-R M.584-2 (11/1997), free | Small |
| FLEX | 2-FSK and 4-FSK, 1600 or 3200 sym/s giving 1600/3200/6400 bit/s, 1.875 s frames, 128 frames per 4-minute cycle, (31,21) BCH interleaved | ARIB STD-T43 (unverified: English edition not located, obtainability from outside Japan unconfirmed). Motorola's own specification was never public and there is no TIA or ETSI equivalent | Medium |
| ERMES | 4-PAM/FM, 3125 baud, 6250 bit/s, 25 kHz, 16 channels in 169.4 to 169.8 MHz, (30,18) cyclic coding | ETSI ETS 300 133-4, free | Medium |
| RDS2 | Three further 1187.5 bit/s biphase BPSK streams on 66.5, 71.25 and 76 kHz beside the existing 57 kHz stream; same 26-bit blocks and offset words. Carries station logos and larger ODA payloads, so it is an image path as well as a text one | IEC 62106-1, -2 and -3, purchasable (unverified: the current part split) | Small |
| DAB multiplex, FIC and data services | COFDM pi/4-DQPSK; Mode I is 1536 carriers at 1 kHz spacing, 1.536 MHz, 1246 us symbol, 76 symbols per 96 ms frame, 2.4 Mbit/s gross | ETSI EN 300 401 V2.1.1, free, with TS 101 756, TS 101 499 (MOT SlideShow), TS 102 979 (Journaline) and TS 102 818 | Large |
| DAB audio (Layer II) | 48 kHz MPEG-1 or 24 kHz MPEG-2 LSF Layer II, 32 to 384 kbit/s, 24 ms frames with ScF-CRC and trailing X-PAD | ISO/IEC 11172-3 and 13818-3, purchasable; the DAB framing is restated in EN 300 401 clause 5, free | Medium |
| Wireless M-Bus | Mode S 32.768 kbit/s Manchester 2-FSK at 868.3 MHz; Mode T 66.667 kbit/s 3-of-6 at 868.95; Mode C 100 kbit/s NRZ; Mode N 4-GFSK at 169.4 MHz | EN 13757-4, purchasable (unverified: 2025 versus 2019 edition). OMS Specification Volume 2 is free and restates much of the frame detail | Medium |
| Z-Wave | R1 9.6 kbit/s Manchester FSK, R2 40 kbit/s NRZ FSK, R3 100 kbit/s GFSK; 868.42 MHz and 908.42/916.0 MHz; 32-bit HomeID, 8-bit NodeID | ITU-T G.9959 (01/2015), in force and free; Z-Wave Alliance SDS13781 and companions, free | Medium |
| IEEE 802.15.4 and Zigbee | 2.4 GHz O-QPSK half-sine, 2 Mchip/s, 16-ary orthogonal spreading, 62.5 ksym/s, 250 kbit/s; 868/915 MHz BPSK DSSS at 20 and 40 kbit/s; preamble 32 zero chips, SFD 0xA7 | IEEE 802.15.4-2020, free through the IEEE GET Program; Zigbee PRO 2023 free from csa-iot.org | Medium |
| Bluetooth Low Energy advertising | GFSK BT 0.5, index 0.45 to 0.55, 1 or 2 Msym/s, 2 MHz channels; advertising on 2402, 2426 and 2480 MHz; access address 0x8E89BED6, CRC-24, 7-bit LFSR whitening seeded by channel index | Bluetooth Core Specification 6.x, free from bluetooth.com | Medium |
| Wi-SUN FAN / 802.15.4g SUN FSK | 2-FSK and 4-FSK, 50 to 300 kbit/s, index 0.5 or 1.0, 200 to 600 kHz channels in 902 to 928 MHz, 16-bit PHY header, CRC-16 or -32 | IEEE 802.15.4-2020 clause 19, free through GET; Wi-SUN FAN 1.1 Technical Profile, free | Medium |
| EnOcean | ASK 125 kbit/s Manchester at 868.3 MHz, 315 or 902 MHz elsewhere; 14 to 21 byte telegrams sent in triplicate | ISO/IEC 14543-3-10:2020, purchasable; EnOcean Equipment Profiles free from the Alliance | Small |
| DECT | GFSK BT 0.5, 1.152 Mbit/s, 1.728 MHz spacing, TDMA/TDD, 24 slots per 10 ms frame, 32 kbit/s per duplex bearer, 16-bit A-field CRC | ETSI EN 300 175 parts 1 to 8, free; ITU-T G.726 for the codec, free | Large |

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
| CubeSat Space Protocol over AX100-class radios | GMSK and GFSK, 100 bit/s to 38.4 kbit/s, usually 9600, in 435 to 438 and 145.8 to 146.0 MHz; randomiser, RS(255,223), ASM, 32-bit CSP header | GS-CSP-1.1 and the NanoCom AX100 datasheet, free, plus the CCSDS Blue Books | Medium |
| AX.25 amateur satellite packet | 1200 baud Bell 202 AFSK and 9600 baud G3RUH in 145.8 to 146.0 and 435 to 438 MHz | AX.25 v2.2, free from TAPR. See the amateur table | Small |
| FUNcube-class BPSK telemetry | 1200 bit/s DBPSK, r=1/2 K=7 with an RS outer code, 256-byte frames, on a 145.9 MHz class downlink | AMSAT-UK published frame layouts and per-mission telemetry dictionaries. No document number exists; cite the page and the dictionary per constant | Small |
| Cospas-Sarsat 406 MHz first generation | 160 ms unmodulated carrier then biphase-L at 400 bit/s, +/-1.1 radians; 440 or 520 ms burst every 50 s; 112 or 144 bits; BCH(82,61) and BCH(106,83) | C/S T.001, free from cospas-sarsat.int (unverified: current issue and revision) | Small |
| Cospas-Sarsat second generation (SGB) | 406.05 MHz, DSSS OQPSK, 38400 chip/s from a truncated 23-stage m-sequence, 256 chips per bit per arm; 1 s burst, 250-bit message, BCH(250,202) | C/S T.018, free (unverified: current issue). The LFSR initialisation values are tabulated in it | Medium |
| GPS L1 C/A (LNAV) | 1575.42 MHz, BPSK-R(1), 1.023 Mchip/s, 1023-chip Gold codes, 50 bit/s in 300-bit subframes | IS-GPS-200 Revision N (2022-08-22) with IRN-003, free at gps.gov | Large |
| GPS L2C and L5 (CNAV) | L2C at 1227.60 MHz, CM and CL time-multiplexed at 511.5 kchip/s each; L5 at 1176.45 MHz, 10.23 Mchip/s, 10230-chip codes with NH overlays | IS-GPS-200N and IS-GPS-705 Revision J (2022-08-22), free | Medium |
| GPS L1C (CNAV-2) | TMBOC(6,1,4/33) pilot and BOC(1,1) data, 10230-chip primaries, 1800-symbol 18 s frames, LDPC | IS-GPS-800 Revision J (2022-08-22), free | Medium |
| GLONASS L1OF and L2OF | FDMA, L1 at 1602 + 0.5625k MHz, 511 kchip/s, 50 bit/s Manchester to 100 sym/s, 2 s strings in 30 s frames | GLONASS ICD Edition 5.1 (2008), free in English translation; separate free ICDs for the L1OC/L2OC/L3OC CDMA signals | Medium |
| BeiDou B1I, B1C, B2a | B1I at 1561.098 MHz BPSK 2.046 Mchip/s; B1C at 1575.42 MHz BOC(1,1) with QMBOC pilot; B2a at 1176.45 MHz BPSK(10) | BDS SIS ICD Open Service B1I v3.0 (2019-02) and the matching B1C, B2a, B2b and B3I documents, free in English | Medium |
| QZSS | GPS-interoperable L1 C/A, L1C, L2C and L5, plus L6 CLAS at 2000 bit/s and L1S at 250 bit/s | IS-QZSS-PNT, IS-QZSS-L6 and IS-QZSS-L1S, free (unverified: current revisions) | Small |
| NavIC / IRNSS L5 SPS | 1176.45 MHz, BPSK(1), 1.023 Mchip/s, 50 bit/s r=1/2 to 100 sym/s, 600-bit master frames, CRC-24 | ISRO-IRNSS-ICD-SPS-1.1, free | Medium |
| SBAS L1 (WAAS, EGNOS, MSAS, GAGAN) | 1575.42 MHz, BPSK(1), PRNs 120 to 158, 250 bit/s r=1/2 K=7 to 500 sym/s, 250-bit messages once per second, CRC-24 | RTCA DO-229F, purchasable; ICAO Annex 10 Volume I is the international equivalent | Medium |
| Inmarsat-C / STD-C NCS TDM | BPSK 1200 sym/s, r=1/2 K=7 with block interleaving, 600 bit/s net, 1530.0 to 1545.0 MHz, 5 kHz raster, 10368-symbol 8.64 s frame, about 2.5 kHz occupied | No lawful air-interface document. ETSI ETS 300 460 is free for RF parameters; IEC 61097-4:2024 is purchasable and its coverage of frame layout is unverified. Frame and symbol timing come from measurement of the live signal, labelled as observation | Medium |
| Inmarsat-C EGC: SafetyNET and FleetNET | Rides the TDM above; LES id, service code, priority, repetition and presentation descriptors; C0 to C5 addressing plus NAVAREA, circular and rectangular | IMO International SafetyNET Manual, free; Inmarsat International SafetyNET Handbook Edition Six (April 2020), free; IMO COMSAR.1/Circ.41 and MSC.1/Circ.1364 | Small |
| Inmarsat Classic Aero P, R and T (data) | A-BPSK at 600 and 1200 bit/s, A-QPSK at 10500 bit/s with r=1/2 convolutional; 1530 to 1559 MHz; 2.5 to 17.5 kHz occupied | ICAO Annex 10 Volume III Part I Chapter 4 and ICAO Doc 9925, purchasable (unverified: whether they specify to demodulator level). Voice on the C channel is AMBE-family and is excluded | Large |
| Iridium Ring Alert | 1626.270833 MHz simplex, 41.667 kHz channel; 12-byte BPSK preamble then about 103 bytes DQPSK at 25 kBd; unencrypted | No official document. Peer-reviewed literature gives the field layout: satellite id, beam id, sub-satellite latitude, longitude and altitude, handover information | Medium |

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
| P25 Phase 2 | ISCH synchronisation, slot and superframe structure, source and group addressing in the MAC, and the associated Phase 1 control channel | Almost the whole payload is AMBE+2, so unlike Phase 1 there is nothing left after the vocoder |
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
| Iridium L-band burst and TDMA framing | Burst detection, 90 ms frame with a 20.32 ms simplex slot and eight 8.28 ms slots, DQPSK 25 kBd after a BPSK unique word | Literature and expired Motorola patents describe the burst; the patent conclusion rests on the design programme's filing era rather than a docket check, and the arXiv identifier carrying the field detail is unverified. Ring Alert is a full include |
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
| DMR Tier I/II/III | Motorola's US8306071, active to 2027-02-19, declared essential to ETSI TS 102 361-1 and claiming a method that receives a burst and compares its synchronisation pattern against known patterns. Three further Motorola patents run to 2031 and read on transmitting rather than receiving. A narrowing limitation in claim 1 gives a receive-only decoder an argument, and an argument is the reason this is excluded rather than shipped. The full query is in "The ETSI IPR query, and what it did to DMR" below | 2027-02-19, when US8306071 expires and the question closes with it. The physical layer is 4FSK at 4800 sym/s, which is the same path P25 Phase 1 already has, so the cost on that day is the framing and not the demodulator |
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
| FreeDV 2020 and RADE | The code is two-clause BSD and the licence on the trained model weights was never established. A weights file has no document to cite and no clause to check against when it misbehaves, and a neural vocoder cannot be clean-roomed from an architecture paper. Two surveys returned opposite verdicts on RADE and both conceded the weights question. Revisit if the weights carry a GPL-3-compatible grant and the project decides a model file is an acceptable dependency |

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

**The ETSI IPR database was queried on 2026-09-21. DMR is now excluded.** The
query is recorded in full below, under "The ETSI IPR query, and what it did to
DMR". The short version: TS 102 361-1 and -2 carry four licensing declarations
from Motorola, the most recent signed 2024-05-16, and one of the declared
patents is live until 2027-02-19 with a claim whose first step is receiving a
burst and comparing its synchronisation pattern. That is the operation a
framing decoder exists to perform. DMR moves to the live-patent exclusion
table.

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
| P25 Phase 1 | `core/decode/p25p1.cpp` | TIA-102.BAAA-A clauses 5, 8 and 9 and the clause 10.2 to 10.4 annexes, with reserved values from TIA-102.BAAC | Frame sync, the Network Access Code and Data Unit ID through the (63,16,23) BCH code; from a header data unit the talkgroup, the manufacturer, the key and algorithm identifiers and the message indicator; from LDU1 the Link Control word, and from LDU2 the encryption sync; the low speed data; and the nine IMBE voice frames of every LDU, which `P25Voice` turns into 8 kHz PCM. See the P25 voice paragraph below |
| D-STAR DV | `core/decode/dstar.cpp` | JARL Ver 7.0 clauses 4.1.1, 4.1.2, Ap1 and Ap2 | Bit and frame sync, the radio header through the rate 1/2 convolutional code and the 24 bit interleave, with all five callsigns and the flag byte, and the voice and data frames with the resynchronisation signals marked |
| TETRA V+D | `core/decode/tetra.cpp` | EN 300 392-2 V3.8.1 clauses 5, 8.2, 8.3.1.2, 9.4.4 and 21.4.4.2 | Burst sync from the synchronisation training sequence, and the SYNC PDU off the broadcast synchronisation channel: colour code, system code, timeslot, frame and multiframe number, and the country and network codes |

D-STAR and TETRA stop at the bits: D-STAR's voice is AMBE and has nowhere to
go, and TETRA's is ACELP and is not implemented. P25 goes through to audio,
and the paragraph below says how.

`core/dsp/synth/dv_mod.cpp` is the transmitter for all three, written from the
same clauses, and `tests/decode/test_p25p1.cpp`, `test_p25p1_voice.cpp`,
`test_dstar.cpp` and `test_tetra.cpp` are the round trips. Each measures a bit
or symbol error rate at a high and a low signal to noise and reports the
figure rather than asserting it tight.

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
Marks.

EN 50067 is cited rather than IEC 62106 because it is the text somebody opened.
Clause numbering moved on the way into the IEC series: EN 50067 clause 3.1.5.1
is IEC 62106 clause 6.1.5.1, and the code carries EN 50067's numbers. Nothing in
the physical layer is region dependent, because NRSC-4 clauses 1.1 through 1.7
are word for word identical to EN 50067's. One demodulator serves both regions
and the RDS/RBDS split does not begin until the group layer.

RDS2 is in the included list above and is the cheapest new capability here, for
the same reason: the demodulator, the block synchroniser, the offset words and
the CRC already work, and RDS2 reuses all four on three more subcarriers.

**Two-level FSK text and data, from receiver audio.** Pure libraries over a
span of real audio at the receiver's audio rate, the way RDS reads the WFM
composite, so the Demod enum does not grow. Not wired to the engine or the
wire yet; they report through the decoded-message seam once it exists.
`core/decode/fsk.cpp` is the part with no standard in it: a tone-pair
discriminator for audio that carries the shift as two tones, a level
discriminator for audio that is already the data waveform, and a
transition-tracking bit clock, which SITOR-B, NAVTEX and DSC can reuse.

| Mode | File | Document | What comes out |
| --- | --- | --- | --- |
| RTTY | `core/decode/rtty.cpp` | ITU-T S.1 (03/93) clauses 3, 4.1 to 4.5 and Tables 1 and 2 for ITA2; ITU-T S.3 (11/88) clauses 1.3 and 1.4 and Table 1 for the 7.5-unit character | Characters with the sample index of their start element, letters and figures case tracked, a decision margin per character, and counts of framing errors and false starts |
| AX.25 over 1200 baud AFSK | `core/decode/ax25.cpp` | AX.25 v2.2 (TAPR, July 1998) clauses 3, 3.1, 3.4, 3.6 to 3.10, 3.12 and 4.2.1; the modem from Finnegan and Benson, "Clarifying the Amateur Bell 202 Modem", TAPR DCC 2014, sections 2 and 3.2 | Frames whose FCS checks, with destination, source and up to eight repeaters, the control field and its frame kind, the PID and the information field, and the sample indices of the first and last bit; counts of candidates, FCS failures and malformed frames |
| APRS | `core/decode/aprs.cpp` | APRS Protocol Reference 1.0.1 (29 August 2000) chapters 5, 6, 7, 8, 9, 10, 14 and 16 | Position reports uncompressed and compressed with timestamp, ambiguity, symbol, course, speed, altitude and range; status with timestamp or Maidenhead locator; messages, acknowledgements and rejections with their numbers; Mic-E position, message type, course, speed, telemetry, status text and altitude |

RTTY's 45.45 baud, 170 Hz shift and 2125 Hz mark are practice rather than
anything S.1 or S.3 states, and all three are parameters along with the
polarity. `core/dsp/synth/fsk_mod.cpp` is the transmitter and
`tests/decode/test_rtty.cpp` the round trip, at 48 kHz and at 8 and 11.025 kHz.
Measured through `add_real_awgn` over 300 characters: no errors at 10 dB in
2500 Hz, a character error rate of 0.0033 at -5 dB and 0.14 at -8 dB, where
the unit error rate is 0.0069 against 0.0064 for ideal non-coherent FSK. What
it does not reach: no automatic frequency control, so the tones must be where
the caller says, and no diversity or error correction, because ITA2 has none.

AX.25 is measured in `tests/decode/test_ax25.cpp` at 48 and 22.05 kHz, through
a 0.5 percent transmitter clock error and a 10 dB tilt between the tones either
way, which is what Finnegan and Benson section 4 report emphasis doing on real
stations. Through `add_real_awgn`, over 60 frames of 81 octets: no frame lost
at 20 dB in 2500 Hz, a frame error rate of 0.05 at 10 dB, 0.57 at 8 dB and
0.98 at 6 dB, where the bit error rate after NRZI is 0.0024 and 0.018. The
AX.25 FCS is checked against the X.25 check `core/decode/dv_codes.cpp` already
carried for TETRA, which is its only independent check, since ISO 3309 was not
held. The address encoding is checked against Figures 3.4 and 3.7, and in doing
so found that AX.25 2.2 misprints every N in its figures as 0x98, which is L,
and that Figure 3.8 sets an extension bit clause 3.12.4 says is clear; the code
follows the text. What it does not reach: connected-mode procedure, which has
nothing to decode; modulo 128 control fields, which a receiver joining partway
cannot tell from modulo 8; 9600 baud G3RUH and 300 baud HF packet; and FCS-based
bit repair, so one wrong bit loses a frame.

APRS is checked in `tests/decode/test_aprs.cpp` against 35 examples the
reference prints in chapters 8, 9, 10, 14 and 16, including page 38's
compressed longitude, page 40's GGA altitude, pages 44 and 53's Mic-E
destination and information field and page 55's Mic-E altitude, and against
both encodings of page 52's Mic-E speed and course. What it does not reach:
objects, items, weather, telemetry reports, queries and third-party traffic are
recognised and refused by name; data extensions other than course and speed
stay in the comment; and the WB2OSZ 1.2 revision was not read, so what it adds
arrives as comment text.

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

**The ETSI IPR query coming back.** It could tighten DMR rather than loosen it.
Either answer is better than the current position, which is that nobody looked.

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

**Identifiers that carry a verdict and did not resolve.** arXiv:2603.12062, the
Iridium security analysis under the Iridium burst entry. The web.archive.org
capture of the barberdsp Dayton SSTV paper. EPS.ASPI.DS.0675 (Metop).
417-R-IRD-0168 (GOES HRIT header type 131). MIL-STD-188-203-3 (Link-4A). The
ORBCOMM Air Interface Specification. Whether ARRL still serves
`ft4_ft8_protocols.tgz` at the QEX Binaries 2020 path, which is what makes the
FT8 and FT4 generator matrices public domain rather than a recorded exception.

**Patent positions resting on the era of the system rather than a search.** For
anything standardised after roughly 2006 the twenty-year argument is
unavailable, and these are in that window: NAVDAT (ITU-R M.2010, 2012 to 2019),
VDES (M.2092, 2015 to 2026), Cospas-Sarsat second-generation beacons, Wi-SUN,
the 10 MHz channelisation and outside-the-context-of-a-BSS operation that
802.11p added to the 802.11a physical layer, and LPCNet and FARGAN. The Iridium
legacy conclusion rests on Motorola's 1990 to 1995 filing era, not a docket
check.

**Editions and revisions to confirm at purchase or implementation time.**
EN 13757-4, 2025 versus 2019. The IEC 62106 part split. RTCA DO-185B versus
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
