// Two-level frequency shift keying from receiver audio: a tone-pair
// discriminator, a baseband level discriminator, and a bit clock.
//
// WHY THIS FILE EXISTS SEPARATELY FROM THE MODES
//
// RTTY, AX.25 over Bell 202, POCSAG, SITOR-B, NAVTEX and DSC are six framings
// over one physical idea: a signal that sits at one of two frequencies for a
// bit period at a time. Underneath every one of them the receiver does the
// same three things. It turns audio into a soft value per sample that leans
// positive for one frequency and negative for the other, it finds where the
// bit centres are, and it reads the soft value there. Only the tone
// frequencies and the bit rate differ, and those are parameters.
//
// Two front ends, because receiver audio arrives in two shapes:
//
//   - ToneDiscriminator, for audio that carries the two frequencies as audio
//     tones. That is Bell 202 AFSK on an FM voice channel, and it is every HF
//     FSK mode received on a single sideband receiver, where the RF shift
//     lands in the audio passband as a pair of tones.
//   - LevelDiscriminator, for audio that is already the data waveform. That
//     is what an FM discriminator makes of direct FSK on VHF and UHF, POCSAG
//     among them: the RF frequency becomes an audio level and the bit is its
//     sign.
//
// One back end, BitClock, for the synchronous modes. RTTY is start-stop and
// does not use it; core/decode/rtty.h frames on the start element instead.
//
// WHAT IS NOT HERE
//
// No constant in this file comes from any standard. It is textbook
// non-coherent FSK reception and a textbook transition-tracking clock loop,
// and the numbers with citations are the ones a mode file supplies: the tone
// frequencies and the bit rate. A reader auditing provenance can skip this
// file, which is the arrangement core/decode/dv_phy.h describes for the
// digital voice modes and for the same reason.
//
// WHY NOT dv_phy.h's SymbolSync
//
// SymbolSync estimates timing once per window of 32 symbols from the
// square-law spectral line. That suits a continuous stream, and it is the
// wrong shape for a two-level burst mode: an AX.25 frame can start with a
// handful of flags and a POCSAG transmission switches rate between one
// batch and the next transmitter. A loop that corrects at every data
// transition has pulled in within tens of bits, and two-level NRZ has none of
// the data-dependent error term that made a tracking loop fail on C4FM, which
// is the reason dv_phy.h gives for not using one there.
//
// NOTHING HERE IS BIT EXACT AGAINST A GPU TWIN
//
// Host code with no kernel behind it, like the rest of core/decode. What
// stands in place of the twin is the round trip through the transmitters in
// core/dsp/synth/fsk_mod.h.

#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::ConstRealSpan;
using dsp::Hertz;
using dsp::SampleIndex;
using dsp::SampleRate;

// ---------------------------------------------------------------------------
// Tone-pair discriminator
// ---------------------------------------------------------------------------

struct ToneDiscriminatorConfig {
    SampleRate rate = 48000;

    // The two audio frequencies. Positive soft values mean mark. Which of
    // the two a mode calls mark, and whether that is the higher or the lower
    // tone, is the mode's to cite.
    Hertz mark_hz = 0;
    Hertz space_hz = 0;

    // Bits per second. Sets the length of the correlation window.
    double symbol_rate = 0.0;
};

// The non-coherent orthogonal FSK receiver: the audio is mixed against each
// tone and summed over one bit period, and the soft value is the difference
// of the two energies over their sum.
//
// WHY A ONE-BIT BOXCAR
//
// A bit of FSK is a tone burst one bit long, and the filter matched to a
// rectangular burst is a rectangle of the same length. Summing the mixed
// product over one bit is that filter, so read at the bit centre it is the
// optimum non-coherent decision in white noise, and it doubles as the
// bandpass around each tone with no separate filter design.
//
// WHY NORMALISED
//
// (|m|^2 - |s|^2) / (|m|^2 + |s|^2) is bounded by one whatever the audio
// level, so a caller's threshold means the same thing on a quiet receiver
// and a loud one. The sign, which is the decision, is unchanged by the
// normalisation.
//
// BLOCK INVARIANCE
//
// The mixer phase is computed from the absolute sample count, reduced
// modulo the rate in integers, so it is exact at any distance into a stream
// and the output does not depend on how the caller blocked the input.
class ToneDiscriminator {
   public:
    ToneDiscriminator() = default;

    [[nodiscard]] static Expected<ToneDiscriminator> create(const ToneDiscriminatorConfig& config);

    // Appends one soft value per input sample, in [-1, 1].
    void process(ConstRealSpan in, std::vector<float>& soft);

    // Samples between an input sample and the output the window is centred
    // on: an event at input index n shows in the output at n + group_delay().
    [[nodiscard]] std::size_t group_delay() const { return (window_ - 1) / 2; }
    [[nodiscard]] std::size_t window() const { return window_; }

    void reset();

   private:
    SampleRate rate_ = 0;
    Hertz mark_hz_ = 0;
    Hertz space_hz_ = 0;
    std::size_t window_ = 0;

    std::vector<std::complex<double>> mark_ring_;
    std::vector<std::complex<double>> space_ring_;
    std::complex<double> mark_sum_{};
    std::complex<double> space_sum_{};
    std::size_t ring_pos_ = 0;
    std::uint64_t count_ = 0;
};

// ---------------------------------------------------------------------------
// Baseband level discriminator
// ---------------------------------------------------------------------------

struct LevelDiscriminatorConfig {
    SampleRate rate = 48000;
    double symbol_rate = 0.0;

    // Bits over which the DC offset and the level are tracked. An engineering
    // choice: long enough that a run of one polarity inside a code word does
    // not drag the threshold across the signal, short enough to follow a
    // transmitter that keys up off frequency. 64 bits is two POCSAG code
    // words.
    double tracking_bits = 64.0;
};

// For audio that is already the data waveform. A one-bit boxcar, the matched
// filter for a rectangular bit, followed by removal of the running mean and
// division by the running mean magnitude, so the output is centred on zero
// and sits near plus and minus one whatever the deviation and whatever the
// tuning error.
//
// A tuning error on direct FSK arrives from an FM discriminator as a DC
// offset on the audio, which is why the mean is removed rather than assumed
// zero: a receiver 1 kHz off a 4.5 kHz deviation signal otherwise slices a
// fifth of the way up one eye.
class LevelDiscriminator {
   public:
    LevelDiscriminator() = default;

    [[nodiscard]] static Expected<LevelDiscriminator> create(
        const LevelDiscriminatorConfig& config);

    // Appends one soft value per input sample. Positive follows positive
    // audio.
    void process(ConstRealSpan in, std::vector<float>& soft);

    [[nodiscard]] std::size_t group_delay() const { return (window_ - 1) / 2; }

    void reset();

   private:
    std::size_t window_ = 0;
    std::vector<double> ring_;
    double sum_ = 0.0;
    std::size_t ring_pos_ = 0;
    std::uint64_t count_ = 0;

    // One-pole trackers, with the coefficient set from tracking_bits.
    double alpha_ = 0.0;
    double mean_ = 0.0;
    double level_ = 0.0;
    bool primed_ = false;
};

// ---------------------------------------------------------------------------
// Bit clock
// ---------------------------------------------------------------------------

struct BitClockConfig {
    SampleRate rate = 48000;
    double symbol_rate = 0.0;

    // Fraction of each measured timing error removed at the transition that
    // measured it. An engineering choice. Larger pulls in faster and follows
    // noise more; 0.15 settles in about twenty transitions and moves the
    // sampling instant by a few hundredths of a bit on a noisy one.
    double phase_gain = 0.15;

    // Fraction of each timing error fed into the clock rate estimate, which
    // is what lets the loop follow a transmitter whose bit rate is not the
    // nominal one without a standing phase error. Two orders below the phase
    // gain keeps the loop well damped.
    double frequency_gain = 0.002;

    // Largest departure of the tracked rate from the nominal one, as a
    // fraction. Bounds how far noise can walk the rate estimate during a
    // silence. 2 percent is ten times the rate tolerance any of the modes
    // this serves specifies.
    double max_rate_error = 0.02;
};

// One bit, read at the instant the loop placed the bit centre.
struct SoftBit {
    // The soft value there, interpolated between the two samples either side
    // of the instant. Positive is mark, or positive level.
    float value = 0.0F;

    // Index into the soft stream of the sample nearest that instant. The
    // discriminators emit one soft value per audio sample, so subtracting
    // their group_delay() maps this back to the audio sample index.
    SampleIndex position = 0;
};

// A second-order transition-tracking loop.
//
// The phase accumulator counts bits. At every sign change of the soft stream
// the crossing is located to a fraction of a sample by linear interpolation,
// and its phase is compared with the bit boundary the loop expected there;
// the difference corrects the phase and, through a much smaller gain, the
// rate. A bit is read each time the accumulator passes the middle of a bit.
//
// A matched filter's output crosses zero half a filter length after the bit
// boundary at its input, and the loop locks to the crossing, so the reading
// instant lands on the filter output's peak with no correction for the delay
// needed here.
class BitClock {
   public:
    BitClock() = default;

    [[nodiscard]] static Expected<BitClock> create(const BitClockConfig& config);

    void process(ConstRealSpan soft, std::vector<SoftBit>& out);

    // Bits per sample the loop currently believes, as a ratio to the
    // nominal. Exposed for tests and for a caller reporting clock error.
    [[nodiscard]] double rate_ratio() const { return step_ / nominal_step_; }

    void reset();

   private:
    double nominal_step_ = 0.0;
    double step_ = 0.0;
    double phase_gain_ = 0.0;
    double frequency_gain_ = 0.0;
    double max_rate_error_ = 0.0;

    double phase_ = 0.0;
    bool emitted_ = false;
    float previous_ = 0.0F;
    bool have_previous_ = false;
    std::uint64_t index_ = 0;
};

}  // namespace revenant::decode
