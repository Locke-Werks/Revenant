# SNR convention

Three different numbers are all called "SNR" in this field. They differ by
more than 12 dB at 48 kHz. This document fixes which one Revenant means.

## The three conventions

Let `S` be the mean power of the wanted signal, `N0` the noise power spectral
density in watts per hertz, `fs` the sample rate, `B` a reference bandwidth and
`Rb` the information bit rate.

**SNR in a reference bandwidth.** Signal power over the noise power measured in
`B` hertz.

    SNR_B = S / (N0 * B)

With `B = 2500 Hz` this is the WSJT-X convention and the default everywhere in
Revenant.

**SNR in the full sample-rate bandwidth.** Signal power over the total noise
power in the buffer.

    SNR_full = S / (N0 * fs)

This one depends on the sample rate, which is why it is not used for reporting.
The same signal in the same channel scores differently at 12 kHz and at 48 kHz.

**Eb/N0.** Energy per information bit over noise power spectral density.

    Eb/N0 = (S / Rb) / N0

The BER literature plots against this, so it is offered for comparison against
published curves. Information bits, not coded bits: using the coded rate shifts
every curve by the code rate.

## Conversions

    SNR_B_dB    = SNR_full_dB + 10*log10(fs / B)
    SNR_full_dB = SNR_B_dB    - 10*log10(fs / B)

    EbN0_dB     = SNR_full_dB + 10*log10(fs / Rb)
    SNR_full_dB = EbN0_dB     - 10*log10(fs / Rb)

    EbN0_dB     = SNR_B_dB    + 10*log10(B / Rb)
    SNR_B_dB    = EbN0_dB     - 10*log10(B / Rb)

The last pair contains no sample rate. Both sides describe the channel rather
than the buffer the channel was written into, which is the property that makes
either one safe to report.

## Why 2500 Hz

It is the width of a typical SSB receiver audio passband. WSJT-X estimates the
noise in that bandwidth and reports signal power against it, and because every
weak-signal program adopted the same figure, a reported -24 dB from two
different programs means the same thing. The number has no other significance:
it is a convention, and its value is that everyone uses it.

## Complex baseband

Revenant carries complex samples, so a stream at `fs` represents `fs` hertz of
spectrum, from `-fs/2` to `+fs/2`. White noise of total power `N` spread across
that span puts `N * B / fs` inside any `B`-hertz slice of it.

Each complex sample carries its noise across two independent quadratures, so
the per-component variance is half the total power. Dropping that factor of two
is a 3 dB error.

## Worked example at 48 kHz

Request SNR of -24 dB in 2500 Hz against a signal of unit mean power.

    10*log10(48000 / 2500) = 12.833 dB
    SNR_full = -24 - 12.833  = -36.833 dB
    N_full   = 1 / 10^(-36.833/10) = 4822.8
    sigma per quadrature = sqrt(4822.8 / 2) = 49.11

Checking against the definition: the noise inside 2500 Hz is
`4822.8 * 2500 / 48000 = 251.2`, and `10*log10(1 / 251.2) = -24.0 dB`.

## Worked example at 12 kHz

The same request, same unit-power signal.

    10*log10(12000 / 2500) = 6.812 dB
    SNR_full = -24 - 6.812   = -30.812 dB
    N_full   = 1 / 10^(-30.812/10) = 1205.7
    sigma per quadrature = sqrt(1205.7 / 2) = 24.55

Noise inside 2500 Hz is `1205.7 * 2500 / 12000 = 251.2`, the same figure as at
48 kHz, so the channel is the same channel.

The total noise power differs by `10*log10(4) = 6.02 dB` between the two rates
while the reported SNR is identical. That is the convention working. Reading
`N_full` as the SNR instead would have claimed -36.8 dB at one rate and -30.8 dB
at the other for one channel.

## Worked example in Eb/N0

FT8 carries 77 payload bits in a 12.64 second transmission, so
`Rb = 77 / 12.64 = 6.0918 bits/s`.

    EbN0_dB = -24 + 10*log10(2500 / 6.0918)
            = -24 + 26.132
            = 2.132 dB

Sample rate does not enter. The same -24 dB is 2.132 dB of Eb/N0 whether the
capture was taken at 12 kHz or 48 kHz.

## What the error costs

Treating the full-band figure as the reference-bandwidth figure understates a
decoder's sensitivity by `10*log10(fs / 2500)`: 12.83 dB at 48 kHz, 6.81 dB at
12 kHz, 18.85 dB at 192 kHz. In the other direction it overstates by the same
amount. That is most of the distance between an ordinary decoder and a
world-class one, in either direction, from a units mistake.

## What Revenant reports

Every Revenant sensitivity claim is stated in the WSJT-X convention: signal
power relative to noise power in a 2500 Hz reference bandwidth. Numbers
published anywhere else, in a README, a release note or a benchmark table, are
that figure and no other, so they are directly comparable with WSJT-X and with
every other program that adopted it.

Where a number is quoted in one of the other two conventions it is labelled with
the bandwidth or the bit rate it was measured against, in the same sentence.

## In code

`tools/siggen/channel.h`. No parameter in that API is called `snr_db`: each one
names its basis.

    NoiseLevel::snr_in_2500_hz_db(-24.0)
    NoiseLevel::snr_in_reference_bandwidth_db(-24.0, 3000)
    NoiseLevel::snr_in_full_sample_rate_db(-36.833)
    NoiseLevel::eb_over_n0_db(2.132, 6.0918)

The conversions above are `reference_bandwidth_to_full_band_db`,
`full_band_to_reference_bandwidth_db`, `full_band_to_eb_over_n0_db`,
`eb_over_n0_to_full_band_db`, `reference_bandwidth_to_eb_over_n0_db` and
`eb_over_n0_to_reference_bandwidth_db`.

Signal power is measured from the buffer rather than assumed, so nothing
depends on the generator emitting unit amplitude. `measure_snr` differences an
impaired buffer against the clean one and reports what was actually delivered,
which is how a test asserts the simulator produced what it was asked for.
