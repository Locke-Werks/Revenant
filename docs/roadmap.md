# Roadmap

| Milestone | What it delivers | State |
| --- | --- | --- |
| M0 | The device path: open the RTL-SDR, read its descriptors, find the bulk endpoint that carries IQ | Done |
| M1 | The engine: the GPU channelizer, receivers, demodulators and the wideband detector, with fifty receivers on one 20 MHz grid faster than realtime | Done |
| M2 | The graphical client, a separate process over Cap'n Proto, drawing the spectrum and waterfall at monitor refresh within its frame budget | Done, 2026-09-27 |
| M3 | The first public release, v0.1.0: a signed installer, with the corresponding sources on the same release page | Done, 2026-09-27 |
| M4 | Rolling capture of the band, and search over what it stored. This is the reason the design exists | In progress |
| M5 | Device support beyond the RTL-SDR, in the order below | Next |
| After M5 | More than one radio at a time, and the modes `docs/modes.md` lists as not done | Not ordered |

M5 device order. Ordered by how many people own the radio against how much work it is.
Today Revenant runs the RTL-SDR family through librtlsdr, including direct
sampling for HF, plus the synthetic and file sources.

| # | Radio | Why here | Approach | Work |
| --- | --- | --- | --- | --- |
| 1 | HackRF One and Pro | Large install base, small documented protocol, BSD-3 host library | Link libhackrf, receive only | Low |
| 2 | SDRplay RSP1B, RSPdx-R2, RSPduo | Among the most widely owned receivers; no open driver exists for current models | Load the SDRplay API at runtime if the user installed it; never shipped | Medium |
| 3 | rtl_tcp and SpyServer clients | Remote RTL-SDR and Airspy servers that already exist in large numbers | Network clients written from the protocols | Low |
| 4 | Airspy R2 and Mini, HydraSDR RFOne | Established 12-bit receivers that share one USB protocol | Own driver on the BSD protocol code, IQ conversion on the GPU | Medium |
| 5 | RX888 MkII | 16-bit direct sampling of 0 to 64 MHz, the radio this design is built for | Own USB 3 streaming and firmware loader | High |
| 6 | ADALM-Pluto and AD936x Zynq boards | One backend covers the family; transport limits full-band use | Link libiio | Medium |
| 7 | The long tail: USRP B2xx, LimeSDR, bladeRF, Airspy HF+ | Small user bases each, one bridge covers them | SoapySDR in a separate process | Medium |


KrakenSDR runs as five single RTL-SDR channels already; coherent direction
finding waits for multi-radio work after M5. Fobos SDR waits on reports of
its imaging problems being resolved.

The order is a plan, not a gate. Send a radio and it gets built for, wherever
it sits in the list. Open an
[issue](https://github.com/Locke-Werks/Revenant/issues) to arrange it.
