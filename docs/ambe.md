# AMBE, and why you will not find it here

AMBE and AMBE+2 are Digital Voice Systems, Inc.'s proprietary voice codecs,
and they are the largest single obstacle in this project.

There is no published specification. Not a restricted one, not an expensive
one, none. The clean room rule in [clean-room.md](clean-room.md) asks only that a document exist, and for
AMBE+2 no document does, so there is nothing to write from and no amount of
care or citation produces a lawful implementation. A licence would not fix
it. GPL-3.0 section 7 forbids conveying a work under further restrictions,
and a proprietary codec licence is nothing but further restrictions, so it
cannot ship inside Revenant at any price, including zero.

What is left is a dongle. To hear a voice signal, on a machine already
running a polyphase channelizer and an FFT across twenty megahertz in real
time, you buy a chip from DVSI and plug it into a USB port, because the
arithmetic is a secret.

The comparison is what makes it galling. IMBE, the OLDER codec from the same
company, is published in TIA-102.BABA and its patents have expired, so P25
Phase 1 voice decodes here in software with nothing bought and nothing
plugged in. TETRA's ACELP is specified in ETSI EN 300 395-2 and ETSI gives it
away. Codec 2 is open and at these bit rates it is not worse. Every
narrowband voice codec somebody wrote down works fine. The one nobody wrote
down is the one in DMR, D-STAR, P25 Phase 2, NXDN, dPMR, Yaesu Fusion,
Iridium, Inmarsat, Thuraya, ACeS and most of the mobile satellite industry,
and it is there because it reached the standards bodies early rather than
because it sounds good. It does not sound good.

So the line this program draws is the one the documents draw. Identification
and protocol decoding work everywhere, because framing travels in the clear
and framing is where most of the useful information lives. Voice works where
the codec was published. Where it was not, Revenant will speak to a hardware
vocoder if you own one, and will otherwise tell you precisely what it is
hearing and decline to guess.

The way in is a vocoder plugin: a DLL in a `vocoders` folder beside
`revenant-engine.exe`, written against `core/decode/vocoder_abi.h`, which is
plain C, names no codec and is the whole contract. The engine loads what it
finds at startup and the client's radio panel says what loaded, what each
plugin serves, and why any was refused. D-STAR and DMR voice reach a plugin
today. None ships here, for the reasons above.

The patents on this will expire and it will still not be implementable,
because the problem was never the patents. My contempt for the arrangement
will comfortably outlast them.
