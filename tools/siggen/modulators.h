// Modulators for every mode Revenant is going to demodulate.
//
// Two jobs. The first is to produce clean IQ whose ground truth is known
// exactly, so a demodulator can be scored rather than eyeballed. The second is
// less obvious and matters more: writing the modulator beside the demodulator
// is how the specification gets checked. If the pair does not round-trip, one
// of the two misread the document, and finding that out here costs an hour
// instead of a week.
//
// Every modulator is a pure function of its spec, its payload and its seed.
// There is no per-object state that a render mutates, no clock is read, and
// nothing is cached between calls. That is not tidiness. The synthesizer in
// wideband.h streams hours of 20 MS/s in blocks, and a generator whose output
// depends on how the caller chose to block it cannot be a reference for
// anything.
//
// Three properties hold, and the tests should assert all three:
//
//   1. The same spec and seed produce bit-identical samples, on any machine.
//   2. Rendering [0, N) in one call and in N/B calls of B samples produces
//      bit-identical output. Phase is re-derived exactly at anchor points
//      fixed by the absolute sample index, never accumulated across a call
//      boundary.
//   3. Carrier phase does not drift. Frequency and sample rate are both
//      integers, so the phase at sample n is (f*n mod rate) turns, computed in
//      integer arithmetic. After an hour at 20 MS/s the error is still zero,
//      which is the payoff for the integer-hertz rule in core/dsp/types.h.
//
// The payload repeats for the life of an emitter. That is deliberate: a BER
// measurement wants a known pattern it can keep comparing against, and a
// finite repeating payload is what keeps memory bounded when the emitter has
// to run for an hour.
//
// mean_power(ConstComplexSpan) is declared in channel.h, not here, so that the
// calibration everything else is measured against has exactly one definition.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Complex32;
using dsp::ComplexSpan;
using dsp::ConstComplexSpan;
using dsp::Hertz;
using dsp::SampleIndex;
using dsp::SampleRate;

// The largest sample rate a spec may ask for. The exact-phase reduction
// multiplies two values that are each below the sample rate, so the product has
// to stay inside a signed 64-bit integer. One gigasample per second leaves
// three orders of magnitude of headroom and is already far beyond anything that
// reaches a host over USB or ethernet.
inline constexpr SampleRate kMaxSampleRate = 1'000'000'000;

enum class Modulation : std::uint8_t {
    Cw = 0,
    Am,
    Nfm,

    // Upper and lower sideband are separate modes rather than one mode with a
    // flag, because the truth record a classifier is scored against has to name
    // which one it was.
    Usb,
    Lsb,

    Fsk2,
    Bpsk,
    Qpsk,
};

// Lower-case, stable, and safe to write into a truth file or a CLI argument.
[[nodiscard]] std::string_view modulation_name(Modulation kind);
[[nodiscard]] Expected<Modulation> modulation_from_name(std::string_view name);
[[nodiscard]] std::span<const Modulation> all_modulations();

// True when the mode carries a bit payload, which is to say when a BER
// measurement against it means anything.
[[nodiscard]] bool carries_bits(Modulation kind);

// The band an emitter occupies, as edges rather than a centre and a width.
// Edges are the unambiguous form: a USB signal's energy sits entirely above its
// suppressed carrier, so its spectral centre is not its carrier frequency, and
// a burst detector reporting the centre of the energy is not wrong. Carrying
// both, separately, is what stops that turning into a scoring argument in M7.
struct SpectralExtent {
    Hertz low_hz = 0;
    Hertz high_hz = 0;

    [[nodiscard]] constexpr Hertz center_hz() const { return (low_hz + high_hz) / 2; }
    [[nodiscard]] constexpr Hertz bandwidth_hz() const { return high_hz - low_hz; }
};

struct ModulatorConfig {
    SampleRate rate = 48000;

    // Offset of the carrier from baseband DC. For the suppressed-carrier modes
    // this is where the carrier would have been, which is the number an
    // operator dials and the number a demodulator has to recover.
    Hertz carrier_offset = 0;

    // Peak carrier amplitude for the constant-envelope modes, and the RMS level
    // for the shaped ones. nominal_mean_power() states the exact relationship
    // per mode without rendering a sample.
    double amplitude = 1.0;

    double initial_phase = 0.0;

    // Seed for a randomly generated payload. Nothing here ever reads a clock.
    std::uint64_t seed = 0;
};

struct CwParams {
    // PARIS timing: the word PARIS plus its trailing space is exactly 50 dot
    // units, so a dot is 1.2/WPM seconds. Every amateur and military CW
    // specification uses this definition.
    double words_per_minute = 20.0;

    // Raised-cosine rise and fall on each keyed element. A hard-keyed carrier
    // is a rectangular envelope, whose spectrum falls off as 1/f and splatters
    // across neighbouring channels as an audible click. Five milliseconds is
    // the figure the amateur literature settled on: slow enough to kill the
    // click, fast enough that 40 WPM is still readable.
    double rise_fall_ms = 5.0;

    std::string text = "CQ DE REVENANT";

    // Silence appended after the message before it repeats, in dot units. Seven
    // is one word space, so the repeat sounds like continuous sending.
    double message_gap_dots = 7.0;
};

struct AmParams {
    // Above 1.0 the envelope goes through zero and inverts. That is
    // overmodulation and it is left reachable on purpose: a receiver that
    // cannot cope with it should fail a test rather than never meet it.
    double modulation_index = 0.8;

    Hertz tone_hz = 1000;

    // Used only to state the occupied bandwidth, which is twice this. The
    // 300 Hz to 3 kHz voice channel makes the usual figure 3 kHz.
    Hertz audio_bandwidth_hz = 3000;
};

struct NfmParams {
    // Peak deviation. 5 kHz is the land mobile standard that 25 kHz channel
    // spacing was built around; 2.5 kHz is the narrowband refarming figure.
    Hertz deviation = 5000;

    Hertz tone_hz = 1000;

    // Feeds Carson's rule for the occupied bandwidth: 2*(deviation + audio).
    Hertz audio_bandwidth_hz = 3000;
};

struct SsbParams {
    Hertz tone_hz = 1000;

    // Zero disables. Two equal tones is the standard SSB linearity test: the
    // envelope becomes a full-depth sinusoid, so any amplitude non-linearity
    // downstream shows up immediately as intermodulation products. Each tone is
    // scaled to half amplitude so the peak envelope still equals the configured
    // amplitude, which is what PEP means.
    Hertz tone2_hz = 0;

    // The audio passband, used for the occupied-bandwidth record and to check
    // the Hilbert transformer actually covers it.
    Hertz audio_low_hz = 300;
    Hertz audio_high_hz = 3000;

    // Length of the Hilbert transformer used when real audio is supplied.
    // Must be odd. The usable band of an N-tap windowed Hilbert transformer is
    // roughly [fs/N, fs/2 - fs/N], so at 48 kHz a 255-tap filter reaches down
    // to about 188 Hz and covers a 300 Hz passband with margin. create()
    // checks this and says what tap count would be needed.
    std::size_t hilbert_taps = 255;
};

struct Fsk2Params {
    double symbol_rate = 1200.0;

    // Peak deviation either side of the carrier, so the tone spacing is twice
    // this. Bit 1 is the upper tone and bit 0 the lower. State that convention
    // in any demodulator written against this: plenty of real systems invert
    // it, Bell 202 among them, and an inverted decoder produces a clean
    // constellation and a 100 percent bit error rate.
    Hertz deviation = 2400;

    // Symbols of random payload when none is supplied.
    std::size_t symbol_count = 256;
};

struct PskParams {
    double symbol_rate = 1200.0;

    // Root raised cosine excess bandwidth. 0.35 is the classic satellite and
    // land mobile figure and is the default everywhere in this project.
    double rolloff = 0.35;

    // Pulse truncation, in symbols either side of the centre. Eight is the
    // usual compromise: the RRC tail is down around 60 dB by then.
    std::size_t span_symbols = 8;

    std::size_t symbol_count = 256;

    // Forces the pulse shape to be evaluated at every output sample.
    //
    // Off by default, because at a wideband sample rate it is wasted work by a
    // wide margin. A 1200 baud signal in a 20 MS/s scene has 16666 samples per
    // symbol, so its envelope is oversampled about ten thousand times against
    // its own Nyquist rate, and evaluating seventeen pulse taps for every one
    // of those samples is what made PSK forty times the cost of any other mode.
    // The renderer instead evaluates the envelope on a stride chosen from the
    // oversampling ratio and interpolates between the points. Measured against
    // this flag at 1200 baud and 20 MS/s, the difference is 100 dB below the
    // signal, which is 0.001 percent EVM: about three orders of magnitude
    // better than a good laboratory signal generator. The stride drops to
    // every sample on its own whenever the oversampling is low enough for the
    // pulse shape to be doing real work.
    //
    // Turn this on for a test that has to compare against an independently
    // computed root raised cosine sample for sample.
    bool exact_envelope = false;
};

struct ModulatorSpec {
    Modulation kind = Modulation::Cw;
    ModulatorConfig common{};

    CwParams cw{};
    AmParams am{};
    NfmParams nfm{};
    SsbParams ssb{};
    Fsk2Params fsk{};
    PskParams psk{};

    // Modulating audio for AM, NFM and SSB, at the output sample rate. No
    // resampling happens here. Empty means the mode's built-in test tone.
    //
    // A supplied buffer bounds the emitter: the signal is silent past its end,
    // so this path does not stream and is not what the wideband synthesizer
    // uses. It also costs eight bytes per sample of precomputed phase for NFM
    // and a full Hilbert transformer per sample for SSB.
    std::vector<float> audio{};

    // One bit per element, value 0 or 1. Deliberately not packed: every packed
    // representation invites an MSB-versus-LSB disagreement between the
    // modulator and the demodulator, and a BER harness comparing bit vectors
    // has nowhere to hide that bug.
    //
    // Empty means a deterministic random payload derived from common.seed.
    std::vector<std::uint8_t> payload_bits{};
};

// A deterministic bit payload. Uses std::mt19937_64 with the caller's seed and
// draws bits straight out of the 64-bit words, avoiding std::bernoulli_
// distribution and friends: the standard does not specify how a distribution
// consumes its engine, so two standard libraries can return different bits from
// the same seed. A payload that is not reproducible across machines defeats the
// point of seeding it.
[[nodiscard]] std::vector<std::uint8_t> random_bits(std::size_t count, std::uint64_t seed);

// The symbol grid a symbol-rate mode runs on.
//
// The payload repeats, so the signal is periodic with cycle_samples, and the
// boundaries are laid out to close the cycle on an exact sample. That is what
// lets the renderer be a pure function of the absolute sample index with
// bounded memory: symbol positions never have to be accumulated forward, they
// are recovered from n modulo the cycle. The cost is that the realised symbol
// rate is quantised to symbol_count*rate/cycle_samples, which for any usable
// cycle is a relative shift of well under a part per million.
struct SymbolClock {
    std::size_t symbol_count = 0;
    SampleIndex cycle_samples = 0;

    // symbol_count + 1 entries. boundary.front() is 0, boundary.back() is
    // cycle_samples, and boundary[k] is symbol k's decision instant.
    std::vector<SampleIndex> boundary;

    [[nodiscard]] std::size_t index_at(SampleIndex position_in_cycle) const;
    [[nodiscard]] double effective_symbol_rate(SampleRate rate) const;
};

// Checks a spec without building anything. create() runs this first, so calling
// it separately is only useful for validating a CLI or a scene before
// committing to the allocation.
[[nodiscard]] Status validate(const ModulatorSpec& spec);

// The band the spec occupies, from its nominal parameters. Modulator::
// occupied_extent() gives the same thing computed from the realised symbol
// rate, which is the one a truth record should carry.
[[nodiscard]] Expected<SpectralExtent> occupied_extent(const ModulatorSpec& spec);

class Modulator {
public:
    [[nodiscard]] static Expected<Modulator> create(ModulatorSpec spec);

    // Writes absolute samples [start, start + out.size()) into out. Pure: the
    // same absolute range always produces the same samples, whatever blocking
    // the caller used to ask for it.
    void render(SampleIndex start, ComplexSpan out) const;

    // The same, summed into out and scaled. This is what the wideband
    // synthesizer uses to place an emitter at a chosen power without having to
    // rebuild the modulator.
    void accumulate(SampleIndex start, ComplexSpan out, double gain) const;

    [[nodiscard]] Modulation kind() const { return spec_.kind; }
    [[nodiscard]] const ModulatorSpec& spec() const { return spec_; }

    // The payload as transmitted, for a BER comparison. Empty for the modes
    // that carry no bits.
    [[nodiscard]] const std::vector<std::uint8_t>& payload_bits() const { return bits_; }

    // The text as keyed, for CW. Empty otherwise.
    [[nodiscard]] std::string_view text() const;

    [[nodiscard]] SpectralExtent occupied_extent() const { return extent_; }

    // Mean power the render will produce, derived analytically from the spec
    // rather than measured. A scene can set an emitter's level from its target
    // SNR without generating a sample first.
    [[nodiscard]] double nominal_mean_power() const { return nominal_power_; }

    [[nodiscard]] const SymbolClock& symbol_clock() const { return clock_; }

    // Zero for the modes that have no symbol rate.
    [[nodiscard]] double effective_symbol_rate() const;

    // Absolute sample index of symbol k's decision instant, counting through
    // repeats of the payload. This is the alignment a BER harness needs, and it
    // is exact: it is recovered from the cycle structure, not accumulated.
    [[nodiscard]] SampleIndex symbol_start_sample(std::uint64_t symbol) const;

    // One full repeat of the payload, in samples. Zero for the modes with no
    // cycle. CW repeats its message on this period too.
    [[nodiscard]] SampleIndex cycle_samples() const;

private:
    Modulator() = default;

    struct CwElement {
        bool keyed = false;
        SampleIndex start = 0;
        SampleIndex stop = 0;
    };

    [[nodiscard]] double audio_at(SampleIndex index) const;
    [[nodiscard]] double cw_envelope(SampleIndex index, std::size_t& cursor) const;

    // Interpolates the tabulated pulse at a position already expressed in
    // table steps. The render works in table steps directly so that the hot
    // loop has no division in it at all.
    [[nodiscard]] double rrc_tap_at(double table_position) const;
    [[nodiscard]] double rrc_lookup(double symbol_offset) const;

    [[nodiscard]] Complex32 psk_envelope(SampleIndex position_in_cycle, std::size_t symbol) const;

    // The same, resolving the cycle position from an absolute index. Used on
    // the interpolated path, where the cost of the two divisions is spread
    // over a whole stride.
    [[nodiscard]] Complex32 psk_envelope_at(SampleIndex index) const;
    [[nodiscard]] double nfm_phase_at(SampleIndex index, double tone_sine) const;
    [[nodiscard]] double hilbert_at(SampleIndex index) const;

    void accumulate_cw(SampleIndex start, ComplexSpan out, double gain) const;
    void accumulate_am(SampleIndex start, ComplexSpan out, double gain) const;
    void accumulate_nfm(SampleIndex start, ComplexSpan out, double gain) const;
    void accumulate_ssb(SampleIndex start, ComplexSpan out, double gain) const;
    void accumulate_fsk(SampleIndex start, ComplexSpan out, double gain) const;
    void accumulate_psk(SampleIndex start, ComplexSpan out, double gain) const;

    ModulatorSpec spec_{};
    std::vector<std::uint8_t> bits_{};
    SpectralExtent extent_{};
    double nominal_power_ = 0.0;

    std::vector<CwElement> cw_elements_{};
    SampleIndex cw_cycle_ = 0;
    double cw_ramp_samples_ = 0.0;

    // NFM with supplied audio: the running phase deviation, reduced into
    // [0, 2pi). Precomputed because frequency modulation is the integral of the
    // modulating signal, and an integral cannot be evaluated at an arbitrary
    // index without either keeping state or keeping this.
    std::vector<double> fm_phase_{};

    // NFM with a tone: the modulation index, deviation over tone frequency. The
    // integral of a sinusoid is closed form, so the tone path needs no table.
    double nfm_beta_ = 0.0;

    // SSB with supplied audio: the Hilbert transformer, zero-centred so that
    // the in-phase path needs no matching delay.
    std::vector<double> hilbert_taps_{};

    // SSB with tones: the two analytic components, already folded onto the
    // carrier so each is a single exact-phase rotation.
    Hertz ssb_freq1_ = 0;
    Hertz ssb_freq2_ = 0;
    bool ssb_two_tone_ = false;

    SymbolClock clock_{};

    // FSK: deviation phase accumulated to the start of each symbol, reduced
    // into [0, 2pi), plus the advance over one whole cycle so repeats stay
    // phase continuous.
    std::vector<double> fsk_prefix_{};
    double fsk_cycle_phase_ = 0.0;

    std::vector<Complex32> symbols_{};

    // The RRC sampled on a uniform grid in symbol periods, linearly
    // interpolated at render time. Evaluating the closed form per sample would
    // mean seventeen transcendental calls per output sample, which is about two
    // orders of magnitude off what streaming needs.
    std::vector<float> rrc_{};
    double rrc_gain_ = 1.0;

    // Everything the pulse-shaping loop needs precomputed into multiplies.
    //
    // This is the only hot loop in the file that touches more than one value
    // per output sample, seventeen of them, and the obvious way to write it
    // puts an integer division and a floating point division inside that inner
    // loop. Measured on a wideband scene it made PSK forty times the cost of
    // every other mode and put the synthesizer under realtime on its own.
    // Converting the sample position straight into table steps removes both.
    std::vector<double> boundary_seconds_{};
    double rrc_scale_ = 0.0;   // sample offset to table steps
    double rrc_base_ = 0.0;    // table step of the pulse centre
    double rrc_limit_ = 0.0;   // one past the last interpolable step
    double cycle_span_ = 0.0;  // cycle length as a double, for the pulse wrap

    // Samples between envelope evaluations. Always a power of two dividing the
    // phase anchor interval, so the evaluation points are a property of the
    // absolute sample index and the output stays independent of blocking. One
    // means every sample, which is the exact path.
    SampleIndex psk_stride_ = 1;
};

// What a one-shot generate() hands back: the samples plus everything a test
// needs to score them.
struct GeneratedSignal {
    std::vector<Complex32> samples;
    std::vector<std::uint8_t> payload_bits;
    SpectralExtent extent{};

    // Measured over the buffer that was actually produced, not assumed.
    double measured_mean_power = 0.0;
    double peak_magnitude = 0.0;

    double effective_symbol_rate = 0.0;
    SampleIndex cycle_samples = 0;
};

[[nodiscard]] Expected<GeneratedSignal> generate(const ModulatorSpec& spec,
                                                 std::size_t sample_count);

// Named entry points, for a test that wants to say what it is generating. Each
// one fills in spec.kind and forwards to generate().
[[nodiscard]] Expected<GeneratedSignal> generate_cw(const ModulatorConfig& common,
                                                    const CwParams& params,
                                                    std::size_t sample_count);

[[nodiscard]] Expected<GeneratedSignal> generate_am(const ModulatorConfig& common,
                                                    const AmParams& params,
                                                    std::size_t sample_count,
                                                    std::vector<float> audio = {});

[[nodiscard]] Expected<GeneratedSignal> generate_nfm(const ModulatorConfig& common,
                                                     const NfmParams& params,
                                                     std::size_t sample_count,
                                                     std::vector<float> audio = {});

[[nodiscard]] Expected<GeneratedSignal> generate_ssb(const ModulatorConfig& common,
                                                     const SsbParams& params,
                                                     bool upper_sideband,
                                                     std::size_t sample_count,
                                                     std::vector<float> audio = {});

[[nodiscard]] Expected<GeneratedSignal> generate_fsk2(const ModulatorConfig& common,
                                                      const Fsk2Params& params,
                                                      std::size_t sample_count,
                                                      std::vector<std::uint8_t> payload_bits = {});

[[nodiscard]] Expected<GeneratedSignal> generate_bpsk(const ModulatorConfig& common,
                                                      const PskParams& params,
                                                      std::size_t sample_count,
                                                      std::vector<std::uint8_t> payload_bits = {});

[[nodiscard]] Expected<GeneratedSignal> generate_qpsk(const ModulatorConfig& common,
                                                      const PskParams& params,
                                                      std::size_t sample_count,
                                                      std::vector<std::uint8_t> payload_bits = {});

// Largest |x| in the buffer. Zero for an empty span. Used to pick a scale
// factor when quantising to eight bits, where clipping is silent and expensive.
// The mean-power counterpart lives in channel.h.
[[nodiscard]] double peak_magnitude(ConstComplexSpan samples);

}  // namespace revenant::siggen
