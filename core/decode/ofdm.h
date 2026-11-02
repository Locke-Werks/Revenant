// A generic OFDM receiver: symbol timing, carrier frequency offset, pilot
// channel estimation and equalisation, for the mode decoders that carry their
// own demapping on top (DRM, FreeDV 700D, NAVDAT, DAB).
//
// WHAT IS NOT HERE
//
// No constant in this file comes from any OFDM standard. The FFT size, prefix
// length, carrier set and pilot table are all supplied by the caller, and the
// mode file that supplies them is the one that cites its standard, exactly as
// core/decode/dv_phy.h splits it for the narrowband voice modes.
//
// SOURCES
//
// Symbol timing and the fractional part of the carrier offset come from the
// cyclic prefix, by the maximum-likelihood correlation of van de Beek,
// Sandell and Borjesson, "ML estimation of time and frequency offset in OFDM
// systems", IEEE Transactions on Signal Processing 45(7), July 1997: the
// prefix is a copy of the symbol's tail, so the product of the signal with
// itself one useful symbol later sums coherently over the prefix, its
// magnitude marks the timing and its angle is the offset modulo one carrier
// spacing. The correlation is averaged over several symbols before the peak
// is taken. The integer part is found after the FFT against the known pilots,
// in the manner of Schmidl and Cox, "Robust frequency and timing
// synchronization for OFDM", IEEE Transactions on Communications 45(12),
// December 1997, section III-B, but correlating adjacent pilots within one
// symbol rather than a dedicated training pair, so any pilot pattern with two
// or more pilots per symbol serves. Channel estimation is least squares at
// the pilots followed by linear interpolation in time and then in frequency,
// the ordinary pilot-aided scheme of Hoeher, Kaiser and Robertson,
// "Two-dimensional pilot-symbol-aided channel estimation by Wiener
// filtering", ICASSP 1997, with the Wiener filters replaced by linear ones.
// No GPL-licensed implementation was read; see docs/clean-room.md.
//
// SCALING
//
// The FFT is unitary: a carrier transmitted with value X through a flat unit
// channel comes out as X, and white noise of variance s2 per complex sample
// comes out as s2 per carrier. core/dsp/synth/ofdm_mod.h uses the same
// convention.
//
// ONE STREAM, ANY BLOCKING
//
// OfdmReceiver is fed blocks of any size and produces the same symbols, bit
// for bit, however the stream is split. Acquisition is attempted only on
// windows anchored to absolute sample positions, and the frequency correction
// is computed from the absolute sample index rather than a running phasor, so
// no decision depends on where a block happened to end.
//
// WHAT IT DOES NOT TRACK
//
// Timing and frequency are measured once at acquisition and then held. A
// sample clock error drifts the FFT window through the prefix; the linear
// phase that produces is absorbed by the channel estimate until the window
// leaves the prefix, which for a 100 ppm clock and a 1/4 prefix of a 1024
// point symbol is about 2500 symbols. Reacquire (reset) on a longer
// transmission.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::Complex32;
using dsp::ConstComplexSpan;
using dsp::SampleRate;

struct OfdmPilot {
    // Carrier index relative to the centre: 0 is the DC bin, negative
    // carriers lie below it.
    int carrier = 0;
    Complex32 value{1.0F, 0.0F};
};

struct OfdmConfig {
    SampleRate sample_rate = 48000;

    // Useful symbol length in samples. A power of two, from 16 to 65536.
    std::size_t fft_size = 256;

    // Guard interval in samples, at most fft_size.
    std::size_t cp_length = 64;

    // The carriers the receiver reports, in the order it reports them. Every
    // pilot carrier must be among them.
    std::vector<int> carriers;

    // pilot_table[s % pilot_table.size()] is the pilot set of symbol s of the
    // transmitter's pattern. The receiver finds which entry the first symbol
    // it locks to corresponds to. A mode whose pilots do not repeat (none
    // known) would need a table covering a whole frame, which is what DRM's
    // does anyway.
    std::vector<std::vector<OfdmPilot>> pilot_table;

    // Symbols averaged for the prefix correlation at acquisition.
    std::size_t acquisition_symbols = 8;

    // Integer carrier offsets searched either side of zero.
    int max_integer_cfo = 0;

    // Normalised prefix correlation (0..1) below which a window is not taken
    // as a signal, and normalised pilot correlation below which the integer
    // and pattern search is not taken as a lock.
    double timing_threshold = 0.3;
    double pilot_threshold = 0.4;

    // How far before the end of the prefix the FFT window starts, in samples.
    // Zero means cp_length / 4. Starting inside the prefix leaves room for an
    // early timing estimate and a channel's delay spread on the late side.
    std::size_t window_backoff = 0;
};

struct OfdmSymbol {
    // Count of symbols since lock, from zero.
    std::uint64_t index = 0;

    // Index into OfdmConfig::pilot_table for this symbol.
    std::size_t pilot_phase = 0;

    // Absolute sample index of the start of this symbol's prefix, counting
    // from the first sample ever pushed.
    std::int64_t start_sample = 0;

    // Per carrier, in OfdmConfig::carriers order: the equalised value, the
    // channel estimate it was divided by, and the estimated SNR of the
    // equalised value as a linear power ratio, |H|^2 / noise variance.
    std::vector<Complex32> equalised;
    std::vector<Complex32> channel;
    std::vector<float> snr;

    // The noise variance per carrier used for snr, before equalisation.
    double noise_variance = 0.0;
};

struct OfdmSync {
    bool locked = false;
    std::int64_t first_symbol_sample = 0;
    // Total offset corrected, in carrier spacings, and in hertz.
    double cfo_carriers = 0.0;
    double cfo_hz = 0.0;
    double timing_metric = 0.0;
    double pilot_metric = 0.0;
};

[[nodiscard]] Status validate(const OfdmConfig& config);

class OfdmReceiver {
public:
    [[nodiscard]] static Expected<OfdmReceiver> create(OfdmConfig config);

    // Appends every symbol completed by this block. A symbol is completed once
    // the pilots needed to interpolate its channel have arrived, so output
    // lags input by up to the pilot pattern's time spacing.
    void push(ConstComplexSpan samples, std::vector<OfdmSymbol>& out);

    [[nodiscard]] const OfdmSync& sync() const { return sync_; }
    [[nodiscard]] const OfdmConfig& config() const { return config_; }

    // Drops lock and all buffered samples; the absolute sample count carries on.
    void reset();

private:
    struct Demodulated {
        std::uint64_t index = 0;
        std::size_t phase = 0;
        std::int64_t start = 0;
        std::vector<Complex32> values;      // per carrier
        std::vector<Complex32> pilot_ls;    // per carrier, valid where is_pilot
        std::vector<std::uint8_t> is_pilot; // per carrier
        std::vector<double> pilot_power;    // per carrier, |pilot|^2
    };

    explicit OfdmReceiver(OfdmConfig config);

    bool try_acquire();
    void demodulate_available();
    void emit_ready(std::vector<OfdmSymbol>& out);
    void fft_at(std::int64_t start, double cfo, std::vector<Complex32>& bins) const;
    [[nodiscard]] Complex32 sample_at(std::int64_t index) const;
    void trim();

    OfdmConfig config_;
    OfdmSync sync_;
    std::vector<int> bin_of_carrier_;
    std::size_t lookahead_ = 0;
    std::size_t backoff_ = 0;

    std::vector<Complex32> buffer_;
    std::int64_t buffer_start_ = 0;
    std::int64_t total_ = 0;
    std::int64_t next_attempt_ = 0;

    std::size_t phase0_ = 0;
    std::uint64_t next_demod_ = 0;
    std::uint64_t next_emit_ = 0;
    std::vector<Demodulated> window_;
    double noise_variance_ = 0.0;
    std::uint64_t noise_updates_ = 0;
};

}  // namespace revenant::decode
