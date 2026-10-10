# Reference radios: Wraith and Wraith-X

Two receivers are being designed in house as reference hardware for Revenant,
the way a radio vendor pairs its hardware with its own software. Neither
exists yet: both are at the architecture and preliminary parts-list stage.
Nothing in Revenant depends on them, and every radio on the [roadmap](roadmap.md)
keeps its place.

The design follows from the engine rather than the other way round. Revenant
already does the heavy processing on the GPU, so the card's job is to deliver
clean, coherent, raw samples and to do only what has to happen at full rate
before the host link.

## Shared principles

- **Two phase-coherent channels.** One reference, one sample clock and, where
  there is a mixer, one shared LO, so a pair of antennas supports noise
  cancellation, diversity and direction finding.
- **Raw samples to the host.** Every sample the ADCs produce can reach
  Revenant untouched. Any cleaning stage on the card emits the cleaned stream
  and the removed component, and the two sum back to the raw samples exactly,
  so every subtraction can be audited.
- **Real front ends.** Relay-switched preselection, step attenuators, a
  bypassable preamp and a hardware overload loop on each channel.
- **A disciplined reference.** 10 MHz and PPS inputs, with PPS-latched sample
  counters so every sample carries an absolute time.
- **Calibration on the card.** A switched noise diode for noise figure, and a
  comb from the reference for inter-channel phase and gain.
- **One host interface.** A PCIe card, with a USB4 box built as the same card
  behind a USB4-to-PCIe bridge.

## Wraith

The one to build first. Everything learned on it carries over to Wraith-X.

| | |
| --- | --- |
| Direct sampling | LTC2195, dual 16-bit, 122.88 MS/s, DC to about 55 MHz |
| VHF and UHF | Tuner tier, about 24 MHz to 1.7 GHz, about 8 MHz wide per channel |
| Tiers at once | One at a time; a second ADC is an option for both together |
| On-card logic | Artix-7 class FPGA: data movement and the overload loop only |
| Host link | PCIe Gen2 x4 |
| Ring buffer | 1 GB DDR3, about 2 s of both channels |
| Parts estimate | About $850 at 100 units |

Linearization, impulse excision and comb subtraction run in Revenant on the
GPU, which sees every raw sample anyway. The two-tone linearization model is
measured at the bench and stored on the card for Revenant to load.

## Wraith-X

The full design, with cost deliberately set aside.

| | |
| --- | --- |
| Tier A, direct sampling | ADC3669, dual 16-bit, 491.52 MS/s, about 9 kHz to 200 MHz |
| Tier B, superheterodyne | 200 MHz to 6 GHz, about 160 to 200 MHz instantaneous bandwidth, upconverting to a first IF above 6 GHz |
| Tiers at once | Yes: two ADCs, four RF inputs |
| On-card processor | AMD Versal AI Edge Gen 2 |
| Host link | PCIe Gen4/5 x4, about 3.9 GB/s, enough for all four streams raw |
| Clock | LMK04832 jitter cleaner from an OCXO, under 70 fs target |
| Calibration | Noise diode, two-tone generators for each tier, comb |

Over USB4, Tier A goes to the host raw and Tier B is channelized on the card,
because four raw streams do not fit the tunnel.

## Open questions

- Tuner supply for Wraith's VHF/UHF tier, and a second source before layout.
- Whether Wraith's second ADC is worth its cost and board area.
- Card or external box: a box keeps the receiver away from GPU noise.
- The bench calibration rig, and the coefficient format the card stores and
  Revenant loads.
- Replacing the parts estimates with distributor quotes.
