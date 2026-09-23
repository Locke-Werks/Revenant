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
// non-coherent FSK reception and Gardner's textbook timing loop,
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
// batch and the next transmitter. A loop that corrects at every bit with a
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

    // Where the input is limited before the boxcar, in multiples of the
    // tracked level either side of the tracked mean; zero leaves it
    // unlimited. See the class comment. An engineering choice, measured on
    // the bench's POCSAG trials at 1200 bit/s, pages lost of 4096 at 8 dB in
    // 2500 Hz: 1996 unlimited, 412 at 2.0, and between 435 and 579 at every
    // other limit tried from 1.5 to 3.0. The level sits a little under the
    // deviation, so 2.0 is a little under twice it.
    double click_limit_levels = 2.0;
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
//
// WHY THE INPUT IS LIMITED
//
// Below its threshold an FM discriminator clicks: the noise carries the phase
// once round the origin, and the output spikes by a whole cycle's worth of
// frequency within a few samples. A one-bit boxcar spreads that cycle over the
// bit, where it is a quarter of POCSAG's 4.5 kHz deviation at 1200 bit/s and
// half of it at 2400, so a click beside a noisy bit flips it, and a few
// together make a code word the BCH code cannot correct. No legitimate input
// goes further from the mean than the deviation, so everything past a margin
// beyond the tracked level is a click or the noise, and is cut before the
// boxcar sees it. Of the 7 pages in 8192 the bench lost at 10 and 11 dB at
// 1200 bit/s, 5 were a message code word with three or more bits wrong and 2
// an address with two; limited, none are lost.
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
    double limit_levels_ = 0.0;
    std::uint64_t settle_samples_ = 0;
};

// ---------------------------------------------------------------------------
// Bit clock
// ---------------------------------------------------------------------------

struct BitClockConfig {
    SampleRate rate = 48000;
    double symbol_rate = 0.0;

    // Fraction of each measured timing error removed at the bit that
    // measured it. An engineering choice: 0.1 pulls in within the 32 flags an
    // AX.25 transmitter leads with and moves the reading instant by a few
    // hundredths of a bit on a noisy bit.
    double phase_gain = 0.1;

    // Fraction of each timing error fed into the clock rate estimate, which
    // is what lets the loop follow a transmitter whose bit rate is not the
    // nominal one without a standing phase error. Well below the square of
    // the phase gain keeps the loop overdamped.
    double frequency_gain = 0.001;

    // Largest departure of the tracked rate from the nominal one, as a
    // fraction. Bounds how far noise can walk the rate estimate during a
    // silence. 2 percent is ten times the rate tolerance any of the modes
    // this serves specifies.
    double max_rate_error = 0.02;

    // The hang-up detector below. Bits over which the centre and boundary
    // magnitudes are averaged, and the ratio of boundary to centre above
    // which the loop is taken to be reading half a bit out. Engineering
    // choices. Counting transitions on a noiseless ramp: locked, the ratio
    // is about 0.5 on random data and 1 on a run of one polarity; half a bit
    // out it is about 2 on random data and 1.6 on SITOR-B's phasing signals,
    // which change level 5 times in 14 units and are the least favourable
    // pattern among the modes this serves. 1.25 sits between the run and the
    // phasing signals.
    double hang_up_bits = 16.0;
    double hang_up_ratio = 1.25;
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

// A second-order loop driven by Gardner's timing error detector.
//
// The phase accumulator counts bits. The soft stream is read twice a bit, at
// the centre the loop believes in and at the boundary halfway between two
// centres, and the timing error is the boundary reading times the change
// across it: zero when the boundary reading sits on the zero crossing of a
// transition, of one sign when the loop reads late and the other when it
// reads early, and zero when there is no transition to measure. The error
// corrects the phase and, through a much smaller gain, the rate.
//
// WHY GARDNER HERE AND NOT THE ZERO CROSSING
//
// The crossing tracker this replaced located every sign change of the soft
// stream and pulled the phase toward it. Every sign change counts the same
// to it, including the ones noise makes in the middle of a bit, and those
// are exactly half a bit out, the largest error it can measure. Measured on
// the Bell 202 round trip at 9 dB Eb/N0, it lost 16 bits in 20000 and put
// every bit after each slip in the wrong place. Gardner weights each error by
// the change across the boundary, which is small where there is no
// transition, so a noise crossing mid-bit pulls the clock by little.
//
// dv_phy.h records a Gardner loop failing on C4FM, and why: with four levels
// the detector carries a data-dependent term that is large bit by bit. On two
// levels that term is absent, which is why the same detector is right here.
//
// A matched filter's output crosses zero half a filter length after the bit
// boundary at its input, and the loop locks the boundary reading to that
// crossing, so the reading instant lands on the filter output's peak with no
// correction for the delay needed here.
//
// WHY A HANG-UP DETECTOR
//
// Gardner's detector reads zero at two phases: the lock, and the point half a
// bit away where the centre reading sits on the boundary. The second is
// unstable, but near it the error is small and its sign is decided by noise,
// so a loop that starts there stays for as long as the noise lets it. The
// transmitters in core/dsp/synth start their first bit at sample zero, which
// puts this loop's first reading exactly there. Measured on SITOR-B, whose
// phasing lasts 232 bits: on one transmission that printed nothing, the
// reading instant was still within a tenth of a bit of the boundary 160 bits
// in, and 1 to 4 transmissions in 1024 printed nothing at each of 0, 1, 2, 6,
// 10 and 20 dB in 2500 Hz. With the detector, none of those 6144 did.
//
// Half a bit out, the boundary reading lands on the bit centre, so it is the
// larger of the two in magnitude; locked, the centre reading is. The loop
// averages both magnitudes and, when the boundary's exceeds the centre's by
// hang_up_ratio, moves the reading instant half a bit, in the direction the
// averaged Gardner error was already pushing it. That is away from the
// boundary it sits beside, so the bit it has been reading is still the bit it
// reads. The direction matters: a first version always moved half a bit
// later, and on SITOR-B at 0 to 2 dB it read one bit twice after the decoder
// had already phased on the readings beside the boundary, which lost 1 or 2
// transmissions in 1024 instead.
class BitClock {
public:
    BitClock() = default;

    [[nodiscard]] static Expected<BitClock> create(const BitClockConfig& config);

    void process(ConstRealSpan soft, std::vector<SoftBit>& out);

    // Bits per sample the loop currently believes, as a ratio to the
    // nominal. Exposed for tests and for a caller reporting clock error.
    [[nodiscard]] double rate_ratio() const { return step_ / nominal_step_; }

    // Times the hang-up detector has moved the reading instant half a bit
    // since the last reset.
    [[nodiscard]] std::uint64_t hang_ups() const { return hang_ups_; }

    void reset();

private:
    BitClockConfig config_{};
    double nominal_step_ = 0.0;
    double step_ = 0.0;

    double phase_ = 0.0;
    bool emitted_ = false;
    float previous_ = 0.0F;
    bool have_previous_ = false;
    std::uint64_t index_ = 0;

    // The last centre reading, and the boundary reading since it.
    double last_centre_ = 0.0;
    bool have_last_centre_ = false;
    double boundary_ = 0.0;
    bool have_boundary_ = false;

    // The hang-up detector's averages of |centre| and |boundary|, and the
    // bits they have seen since the last reset or move.
    double centre_level_ = 0.0;
    double boundary_level_ = 0.0;
    double tau_level_ = 0.0;
    double level_alpha_ = 0.0;
    std::uint64_t level_bits_ = 0;
    std::uint64_t hang_ups_ = 0;
};

}  // namespace revenant::decode
