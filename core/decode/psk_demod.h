// A generic coherent M-PSK demodulator for complex baseband: BPSK, QPSK,
// offset QPSK and 8PSK.
//
// WHAT IT IMPLEMENTS
//
// No single specification. This is the textbook coherent receiver that the
// mode decoders built on it (satellite telemetry, TETRA-like links, anything
// carrying root raised cosine PSK) share, written from published theory per
// docs/clean-room.md:
//
//   - Matched filtering with a root raised cosine, the receive half of the
//     Nyquist split (Proakis, Digital Communications, 5th ed., section 9.2).
//     The taps come from design_rrc in core/decode/dv_phy.h.
//   - Coarse frequency acquisition by the M-th power: raising M-PSK to the
//     M-th power strips the modulation and leaves a spectral line at M times
//     the carrier offset (Proakis section 6.2.4; Mengali and D'Andrea,
//     Synchronization Techniques for Digital Receivers, chapter 3). Samples
//     are normalised to unit magnitude before the power, the Viterbi and
//     Viterbi form (IEEE Trans. Inf. Theory 29(4), 1983). The line is found with an FFT over the first acquisition_symbols of signal and
//     refined by parabolic interpolation between bins.
//   - Symbol timing by the Gardner detector, core/dsp/gardner.h.
//   - A decision-directed Costas loop for carrier phase: the error is the
//     angle between the corrected symbol and its nearest constellation point,
//     driving a second order proportional plus integral loop at a damping of
//     0.707 (Gardner, Phaselock Techniques, 3rd ed., chapter 4; Proakis
//     section 6.2.5).
//
// OFFSET QPSK
//
// The quadrature rail is sent half a symbol after the in-phase rail. The
// receiver corrects carrier phase first, because a phase error mixes the two
// rails and the delay would then misalign the mixture, then delays the
// in-phase rail by half a symbol so both rails strobe together, and from
// there treats the signal as QPSK.
//
// PHASE AMBIGUITY IS REPORTED, NOT RESOLVED
//
// A loop driven by an M-fold symmetric constellation settles at any of M
// phases 2*pi/M apart, and nothing in the physical layer says which. The
// demodulator reports that (ambiguity() returns M) and leaves resolution to
// the caller, which has a sync word to test the M rotations against. A
// rotation by one step adds one to every symbol index, modulo M, except for
// OQPSK: there a quarter turn swaps the rails and re-pairs them half a symbol
// over, so the caller resolves it on the interleaved rail stream I0 Q0 I1 Q1,
// where it is a shift by one rail and a polarity per rail.
//
// CONSTELLATION AND BIT LABELLING
//
// Point k sits at angle offset + 2*pi*k/M, counter-clockwise, where the offset
// is 0 for BPSK and 8PSK and pi/4 for QPSK and OQPSK. bit_map[k] is the bit
// label of point k, most significant bit first; the default is the binary
// reflected Gray code k ^ (k >> 1). Soft bits are max-log likelihood ratios
// on the amplitude-normalised symbol: the squared distance to the nearest
// point labelled 0 minus that to the nearest labelled 1, so positive means 1
// and the magnitude grows with confidence.

#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/dv_phy.h"
#include "core/dsp/gardner.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

enum class PskModulation : std::uint8_t { Bpsk, Qpsk, Oqpsk, Psk8 };

// Points in the constellation: 2, 4, 4, 8.
[[nodiscard]] unsigned psk_order(PskModulation modulation);

// Bits per symbol: 1, 2, 2, 3.
[[nodiscard]] unsigned psk_bits_per_symbol(PskModulation modulation);

// The unit-amplitude constellation point k, per the convention above. The
// transmitter in core/dsp/synth/psk_mod.h uses this, so both ends agree.
[[nodiscard]] Complex32 psk_point(PskModulation modulation, unsigned k);

// The binary reflected Gray labelling, k ^ (k >> 1), for every k.
[[nodiscard]] std::array<std::uint8_t, 8> psk_gray_map();

enum class PskAcquisition : std::uint8_t {
    // The carrier loop alone. Pull-in is then limited to a small fraction of
    // the loop bandwidth, so use only when the offset is known to be small.
    None,
    // The M-th power line over the first acquisition_symbols of signal.
    PowerLine,
};

struct PskDemodConfig {
    SampleRate rate = 0;
    double symbol_rate = 0.0;
    PskModulation modulation = PskModulation::Qpsk;

    // Root raised cosine roll-off and length in symbols.
    double roll_off = 0.35;
    std::size_t filter_span_symbols = 8;

    // Loop noise bandwidths normalised to the symbol rate.
    double timing_bandwidth = 0.01;
    double carrier_bandwidth = 0.02;

    PskAcquisition acquisition = PskAcquisition::PowerLine;
    std::size_t acquisition_symbols = 512;
    // The widest offset the acquisition searches, in hertz either side. Zero
    // means the most it can resolve, symbol_rate / (2*M): 25%, 12.5% and
    // 6.25% of the symbol rate for BPSK, QPSK and 8PSK. A larger value is
    // clamped to that; see acquisition_range_hz().
    double max_offset_hz = 0.0;

    std::array<std::uint8_t, 8> bit_map = psk_gray_map();

    // Lock is declared when the averaged M-th power coherence, which reads 1
    // for noiseless symbols on their points and 0 for noise, exceeds this. It
    // falls with SNR: measured 0.23 to 0.6 at the Es/N0 where SER is 1e-2
    // to 1e-3, and near 0 when unlocked.
    double lock_threshold = 0.15;
};

struct PskSymbol {
    // After timing, carrier and amplitude correction: the points sit on the
    // unit circle.
    Complex32 symbol;
    // Nearest constellation point.
    std::uint8_t index = 0;
    // Max-log soft bits, most significant first; only bits_per_symbol used.
    std::array<float, 3> soft{};
    // Absolute input sample position of the strobe, counted from the first
    // sample since the last reset. For OQPSK, the in-phase rail's strobe.
    double position = 0.0;
};

class PskDemod {
public:
    PskDemod() = default;

    [[nodiscard]] static Expected<PskDemod> create(const PskDemodConfig& config);

    // Consumes samples and returns every symbol they complete, oldest first.
    // The span is valid until the next call. The output for a stream is the
    // same however it is split into calls.
    std::span<const PskSymbol> process(dsp::ConstComplexSpan in);

    // Forgets everything, including the acquisition.
    void reset();

    [[nodiscard]] bool locked() const { return locked_; }
    // Averaged Re((z/|z|)^M), between -1 and 1.
    [[nodiscard]] double lock_metric() const { return lock_metric_; }
    // Estimated carrier offset, acquisition plus loop, in hertz.
    [[nodiscard]] double frequency_offset_hz() const;
    // The coarse estimate alone, or zero before acquisition.
    [[nodiscard]] double coarse_offset_hz() const { return coarse_hz_; }
    [[nodiscard]] bool acquired() const { return acquired_; }
    // The offset range the acquisition searches, either side of zero. Beyond
    // symbol_rate / (2*M) a carrier offset is indistinguishable from one
    // symbol_rate / M nearer, to the M-th power and to the carrier loop.
    [[nodiscard]] double acquisition_range_hz() const;
    // The number of phases the loop may settle at: M.
    [[nodiscard]] unsigned ambiguity() const { return order_; }
    [[nodiscard]] const PskDemodConfig& config() const { return config_; }

private:
    void estimate_offset();
    void run(dsp::ConstComplexSpan in);
    void decide(const dsp::GardnerTiming::Strobe& strobe);

    PskDemodConfig config_;
    unsigned order_ = 0;
    unsigned bits_ = 0;
    double sps_ = 0.0;
    double kp_ = 0.0;
    double ki_ = 0.0;
    std::vector<float> taps_;
    ComplexFir filter_;
    dsp::GardnerTiming timing_;

    // Acquisition.
    std::size_t acquisition_samples_ = 0;
    std::vector<Complex32> pending_;
    bool acquired_ = false;
    double coarse_hz_ = 0.0;
    double coarse_phase_ = 0.0;  // cycles

    // Carrier loop. Phase in radians, frequency in radians per sample.
    double phase_ = 0.0;
    double frequency_ = 0.0;

    // OQPSK half-symbol delay of the in-phase rail.
    std::vector<float> delay_line_;
    std::uint64_t samples_in_ = 0;

    double power_ = 0.0;
    double lock_metric_ = 0.0;
    bool locked_ = false;

    std::vector<Complex32> scratch_;
    std::vector<PskSymbol> out_;
};

}  // namespace revenant::decode
