// Reference floating point discipline, before anything else in the file.
#include "core/dsp/reference_fp.h"

#include "core/dsp/synth/modulators.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <random>
#include <utility>

namespace revenant::siggen {

static_assert(dsp::kReferenceFpDisciplineApplied,
              "modulators.cpp must include core/dsp/reference_fp.h first");

namespace {

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kTwoPi = 6.283185307179586476925286766559;

// Phase is re-derived exactly at every multiple of this many samples, and
// rotated by complex multiply in between.
//
// Three things set the value. It has to be small enough that the rotation
// cannot accumulate a visible error: over 1024 steps the unit-modulus drift is
// around 1e-13, which is four orders below what a float32 mantissa can hold.
// It has to be large enough that the two transcendental calls at each anchor
// vanish into the per-sample cost. And it bounds the work thrown away when a
// caller starts a render part way through an interval, since the renderer has
// to rotate up from the anchor rather than starting where it was asked: at
// most 1023 discarded rotations, once per call.
constexpr SampleIndex kPhaseAnchorInterval = 1024;

// Resolution of the tabulated root raised cosine, in points per symbol period.
// Linear interpolation between points leaves an error near 1e-7 relative,
// roughly 140 dB down, which is well under the float32 output it feeds.
constexpr std::size_t kRrcStepsPerSymbol = 512;

// Guards the symbol grid arithmetic against overflow. symbol_count multiplied
// by cycle_samples has to stay inside 64 bits when the boundaries are laid out.
constexpr std::size_t kMaxSymbolCount = 1'000'000;

// Exact carrier phase at an absolute sample index, reduced into [0, 2pi).
//
// The phase is 2*pi*f*n/rate, and the only part that survives reduction is
// (f*n) mod rate. Both f and rate are integers by project convention, so that
// reduction is ((f mod rate) * (n mod rate)) mod rate, computed entirely in
// integers. Nothing accumulates, so nothing drifts: the phase at sample 10^11
// is as exact as the phase at sample 1. The same thing done by multiplying
// doubles is off by about 5e-5 radians after an hour at 20 MS/s, which does not
// matter until two emitters are meant to stay coherent, and then it does.
//
// Both operands are below rate, so the product needs rate^2 to fit in a signed
// 64-bit integer. kMaxSampleRate is what keeps that true.
[[nodiscard]] double exact_phase(Hertz frequency, SampleRate rate, SampleIndex index)
{
    std::int64_t reduced = frequency % rate;
    if (reduced < 0) {
        reduced += rate;
    }
    const auto position = static_cast<std::int64_t>(index % static_cast<SampleIndex>(rate));
    const std::int64_t turns = (reduced * position) % rate;
    return kTwoPi * static_cast<double>(turns) / static_cast<double>(rate);
}

[[nodiscard]] double radians_per_sample(Hertz frequency, SampleRate rate)
{
    return kTwoPi * static_cast<double>(frequency) / static_cast<double>(rate);
}

// A unit phasor advanced by complex multiply.
//
// The multiply is written out rather than using operator*= on std::complex
// because libstdc++ routes that through __mulsc3, which adds infinity and NaN
// fixups. They are slower, and more to the point they are not guaranteed to
// produce the same result as the plain four-multiply form on every
// implementation. A reference that gives different bits on different standard
// libraries is not a reference.
struct Phasor {
    double re = 1.0;
    double im = 0.0;
    double step_re = 1.0;
    double step_im = 0.0;

    void anchor(double phase)
    {
        re = std::cos(phase);
        im = std::sin(phase);
    }

    void set_rate(double increment)
    {
        step_re = std::cos(increment);
        step_im = std::sin(increment);
    }

    void advance()
    {
        const double next_re = re * step_re - im * step_im;
        const double next_im = re * step_im + im * step_re;
        re = next_re;
        im = next_im;
    }
};

// Walks an absolute sample range in anchored segments.
//
// init(anchor) builds the per-segment state exactly, from the absolute index
// alone. emit(state, index, offset) writes one sample. The state is advanced
// after every sample, including through the catch-up from the anchor to where
// the caller actually asked to start.
//
// That catch-up is the whole point. Anchors are a property of the sample index
// and nothing else, so sample n is reached by the same sequence of operations
// whatever block it arrives in, and a scene rendered in 4096-sample blocks is
// bit-identical to the same scene rendered in one call. Anchoring at the
// caller's start index instead would be cheaper and would quietly make the
// output depend on the block size.
template <class Init, class Emit>
void walk_anchored(SampleIndex start, std::size_t count, SampleIndex interval,
                   Init&& init, Emit&& emit)
{
    if (count == 0 || interval == 0) {
        return;
    }

    const SampleIndex end = start + count;
    SampleIndex index = start;

    while (index < end) {
        const SampleIndex anchor = index - (index % interval);
        const SampleIndex stop = std::min<SampleIndex>(end, anchor + interval);

        auto state = init(anchor);
        for (SampleIndex skipped = anchor; skipped < index; ++skipped) {
            state.advance();
        }
        for (; index < stop; ++index) {
            emit(state, index, static_cast<std::size_t>(index - start));
            state.advance();
        }
    }
}

struct MorseEntry {
    char character;
    const char* code;
};

// Letters, digits, and the punctuation and prosigns that actually appear on
// the air. BT and AR are sent as the fused characters they are, not as their
// component letters.
constexpr MorseEntry kMorseTable[] = {
    {'A', ".-"},     {'B', "-..."},   {'C', "-.-."},   {'D', "-.."},
    {'E', "."},      {'F', "..-."},   {'G', "--."},    {'H', "...."},
    {'I', ".."},     {'J', ".---"},   {'K', "-.-"},    {'L', ".-.."},
    {'M', "--"},     {'N', "-."},     {'O', "---"},    {'P', ".--."},
    {'Q', "--.-"},   {'R', ".-."},    {'S', "..."},    {'T', "-"},
    {'U', "..-"},    {'V', "...-"},   {'W', ".--"},    {'X', "-..-"},
    {'Y', "-.--"},   {'Z', "--.."},
    {'0', "-----"},  {'1', ".----"},  {'2', "..---"},  {'3', "...--"},
    {'4', "....-"},  {'5', "....."},  {'6', "-...."},  {'7', "--..."},
    {'8', "---.."},  {'9', "----."},
    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'/', "-..-."},
    {'=', "-...-"},  {'+', ".-.-."},  {'-', "-....-"}, {':', "---..."},
};

// Deliberately not std::toupper: that consults the current C locale, and a
// generator whose output depends on the locale the process happens to be
// running under is not deterministic.
[[nodiscard]] const char* morse_for(char character)
{
    const char upper = (character >= 'a' && character <= 'z')
                           ? static_cast<char>(character - 'a' + 'A')
                           : character;
    for (const MorseEntry& entry : kMorseTable) {
        if (entry.character == upper) {
            return entry.code;
        }
    }
    return nullptr;
}

// Root raised cosine impulse response. t is in symbol periods, normalised so
// that h(0) is 1 + beta*(4/pi - 1).
//
// Two removable singularities have to be special cased: t = 0, and
// t = +/-1/(4*beta) where the denominator vanishes. The second one is the trap.
// It is easy to guard t = 0 and forget the other, and 1/(4*beta) lands exactly
// on the sample grid for the common combination of beta = 0.25 and an integer
// number of samples per symbol, so the filter comes out with a NaN in it for
// some settings and not others.
[[nodiscard]] double root_raised_cosine(double t, double beta)
{
    constexpr double kSingularityTolerance = 1e-9;

    if (std::fabs(t) < kSingularityTolerance) {
        return 1.0 + beta * (4.0 / kPi - 1.0);
    }
    if (beta <= 0.0) {
        return std::sin(kPi * t) / (kPi * t);
    }

    const double quarter = 1.0 / (4.0 * beta);
    if (std::fabs(std::fabs(t) - quarter) < kSingularityTolerance) {
        const double leading = (1.0 + 2.0 / kPi) * std::sin(kPi / (4.0 * beta));
        const double trailing = (1.0 - 2.0 / kPi) * std::cos(kPi / (4.0 * beta));
        return (beta / std::sqrt(2.0)) * (leading + trailing);
    }

    const double four_beta_t = 4.0 * beta * t;
    const double numerator = std::sin(kPi * t * (1.0 - beta)) +
                             four_beta_t * std::cos(kPi * t * (1.0 + beta));
    const double denominator = kPi * t * (1.0 - four_beta_t * four_beta_t);
    return numerator / denominator;
}

// Mean of |x|^2. Named apart from channel.h's mean_power on purpose: that one
// is the project's single calibration point and this translation unit must not
// provide a second definition of it.
[[nodiscard]] double buffer_mean_power(ConstComplexSpan samples)
{
    if (samples.empty()) {
        return 0.0;
    }
    double total = 0.0;
    for (const Complex32& value : samples) {
        const auto re = static_cast<double>(value.real());
        const auto im = static_cast<double>(value.imag());
        total += re * re + im * im;
    }
    return total / static_cast<double>(samples.size());
}

// Lays out the repeating symbol grid. See SymbolClock in the header for why the
// cycle has to close on an exact sample.
[[nodiscard]] Expected<SymbolClock> build_symbol_clock(std::size_t symbols,
                                                       SampleRate rate,
                                                       double symbol_rate)
{
    if (symbols == 0) {
        return fail("symbol payload is empty");
    }
    if (symbols > kMaxSymbolCount) {
        return fail(std::format("payload of {} symbols exceeds the {} symbol limit",
                                symbols, kMaxSymbolCount));
    }

    const double samples_per_symbol = static_cast<double>(rate) / symbol_rate;
    const double cycle_real = static_cast<double>(symbols) * samples_per_symbol;
    if (!std::isfinite(cycle_real) || cycle_real >= 9.0e15) {
        return fail(std::format("symbol rate {} baud at {} S/s gives an unusable cycle length",
                                symbol_rate, rate));
    }

    const auto cycle = static_cast<SampleIndex>(std::llround(cycle_real));
    if (cycle < static_cast<SampleIndex>(symbols) * 2) {
        return fail(std::format(
            "symbol rate {} baud at {} S/s leaves under two samples per symbol",
            symbol_rate, rate));
    }

    // boundary[k] is computed as k*cycle/symbols, so the largest intermediate
    // is symbols*cycle.
    constexpr double kUnsignedLimit = 1.8e19;
    if (static_cast<double>(symbols) * static_cast<double>(cycle) >= kUnsignedLimit) {
        return fail("symbol grid is too large to lay out without overflow");
    }

    SymbolClock clock;
    clock.symbol_count = symbols;
    clock.cycle_samples = cycle;
    clock.boundary.resize(symbols + 1);
    for (std::size_t k = 0; k <= symbols; ++k) {
        clock.boundary[k] = (static_cast<SampleIndex>(k) * cycle) / static_cast<SampleIndex>(symbols);
    }
    return clock;
}

// Occupied band from a spec plus whichever symbol rate the caller considers
// authoritative. Shared so the nominal and realised answers cannot drift apart.
[[nodiscard]] SpectralExtent extent_for(const ModulatorSpec& spec, double symbol_rate)
{
    const Hertz carrier = spec.common.carrier_offset;
    Hertz half = 0;

    switch (spec.kind) {
    case Modulation::Cw: {
        // ITU-R SM.1138 gives the necessary bandwidth of on-off keyed
        // telegraphy as Bn = B*K, with B the keying rate in bauds and K = 3 for
        // a non-fading circuit. At 20 WPM that is 50 Hz, which is why a 500 Hz
        // CW filter feels generous.
        const double baud = spec.cw.words_per_minute / 1.2;
        half = static_cast<Hertz>(std::llround(1.5 * baud));
        break;
    }
    case Modulation::Am:
        half = spec.audio.empty() ? spec.am.tone_hz : spec.am.audio_bandwidth_hz;
        break;
    case Modulation::Nfm: {
        // Carson's rule: effectively all of an FM signal's power lies within
        // twice the sum of the peak deviation and the highest modulating
        // frequency.
        const Hertz audio_max = spec.audio.empty() ? spec.nfm.tone_hz : spec.nfm.audio_bandwidth_hz;
        half = spec.nfm.deviation + audio_max;
        break;
    }
    case Modulation::Usb:
        // The energy of an upper sideband signal sits entirely above the
        // suppressed carrier, so the band is not centred on it.
        return SpectralExtent{carrier + spec.ssb.audio_low_hz, carrier + spec.ssb.audio_high_hz};
    case Modulation::Lsb:
        return SpectralExtent{carrier - spec.ssb.audio_high_hz, carrier - spec.ssb.audio_low_hz};
    case Modulation::Fsk2:
        // Carson again, with the symbol rate standing in for the modulating
        // frequency. Continuous phase keeps this honest; a discontinuous-phase
        // FSK would be far wider than this says.
        half = spec.fsk.deviation + static_cast<Hertz>(std::llround(symbol_rate * 0.5));
        break;
    case Modulation::Bpsk:
    case Modulation::Qpsk:
        // A root raised cosine occupies (1 + rolloff) times the symbol rate,
        // and the truncation to span_symbols only affects the skirts.
        half = static_cast<Hertz>(std::llround((1.0 + spec.psk.rolloff) * symbol_rate * 0.5));
        break;
    }

    return SpectralExtent{carrier - half, carrier + half};
}

[[nodiscard]] Status validate_common(const ModulatorConfig& config)
{
    if (config.rate <= 0) {
        return fail(std::format("sample rate must be positive, got {}", config.rate));
    }
    if (config.rate > kMaxSampleRate) {
        return fail(std::format("sample rate {} exceeds the {} S/s limit the exact-phase "
                                "reduction is bounded by",
                                config.rate, kMaxSampleRate));
    }
    if (!std::isfinite(config.amplitude) || config.amplitude < 0.0) {
        return fail("amplitude must be finite and not negative");
    }
    if (!std::isfinite(config.initial_phase)) {
        return fail("initial phase must be finite");
    }
    if (std::llabs(config.carrier_offset) * 2 > config.rate) {
        return fail(std::format("carrier offset {} Hz is outside +/-{} Hz, the Nyquist limit "
                                "at {} S/s",
                                config.carrier_offset, config.rate / 2, config.rate));
    }
    return {};
}

[[nodiscard]] Status validate_audio_tone(Hertz tone, SampleRate rate, const char* what)
{
    if (tone <= 0) {
        return fail(std::format("{} must be positive, got {} Hz", what, tone));
    }
    if (tone * 2 >= rate) {
        return fail(std::format("{} of {} Hz is at or above Nyquist for {} S/s", what, tone, rate));
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

std::string_view modulation_name(Modulation kind)
{
    switch (kind) {
    case Modulation::Cw:   return "cw";
    case Modulation::Am:   return "am";
    case Modulation::Nfm:  return "nfm";
    case Modulation::Usb:  return "usb";
    case Modulation::Lsb:  return "lsb";
    case Modulation::Fsk2: return "fsk2";
    case Modulation::Bpsk: return "bpsk";
    case Modulation::Qpsk: return "qpsk";
    }
    return "unknown";
}

std::span<const Modulation> all_modulations()
{
    static constexpr Modulation kAll[] = {
        Modulation::Cw,   Modulation::Am,   Modulation::Nfm,  Modulation::Usb,
        Modulation::Lsb,  Modulation::Fsk2, Modulation::Bpsk, Modulation::Qpsk,
    };
    return std::span<const Modulation>(kAll);
}

Expected<Modulation> modulation_from_name(std::string_view name)
{
    for (Modulation kind : all_modulations()) {
        if (modulation_name(kind) == name) {
            return kind;
        }
    }

    std::string known;
    for (Modulation kind : all_modulations()) {
        if (!known.empty()) {
            known += ", ";
        }
        known += modulation_name(kind);
    }
    return fail(std::format("unknown modulation '{}', expected one of: {}", name, known));
}

bool carries_bits(Modulation kind)
{
    return kind == Modulation::Fsk2 || kind == Modulation::Bpsk || kind == Modulation::Qpsk;
}

// ---------------------------------------------------------------------------
// SymbolClock
// ---------------------------------------------------------------------------

std::size_t SymbolClock::index_at(SampleIndex position_in_cycle) const
{
    if (symbol_count == 0 || cycle_samples == 0) {
        return 0;
    }

    // boundary[k] is floor(k*cycle/symbols), so inverting it as
    // floor(position*symbols/cycle) is never more than one symbol out either
    // way. Two short corrections beat a binary search at these sizes.
    auto estimate =
        static_cast<std::size_t>((position_in_cycle * static_cast<SampleIndex>(symbol_count)) /
                                 cycle_samples);
    if (estimate >= symbol_count) {
        estimate = symbol_count - 1;
    }
    while (estimate + 1 < symbol_count && boundary[estimate + 1] <= position_in_cycle) {
        ++estimate;
    }
    while (estimate > 0 && boundary[estimate] > position_in_cycle) {
        --estimate;
    }
    return estimate;
}

double SymbolClock::effective_symbol_rate(SampleRate rate) const
{
    if (cycle_samples == 0) {
        return 0.0;
    }
    return static_cast<double>(symbol_count) * static_cast<double>(rate) /
           static_cast<double>(cycle_samples);
}

// ---------------------------------------------------------------------------
// Payload
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> random_bits(std::size_t count, std::uint64_t seed)
{
    std::vector<std::uint8_t> bits(count, 0);
    std::mt19937_64 engine(seed);

    std::uint64_t word = 0;
    unsigned remaining = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (remaining == 0) {
            word = engine();
            remaining = 64;
        }
        bits[i] = static_cast<std::uint8_t>(word & 1u);
        word >>= 1;
        --remaining;
    }
    return bits;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

Status validate(const ModulatorSpec& spec)
{
    if (auto common = validate_common(spec.common); !common) {
        return common;
    }
    const SampleRate rate = spec.common.rate;

    switch (spec.kind) {
    case Modulation::Cw: {
        const CwParams& cw = spec.cw;
        if (!std::isfinite(cw.words_per_minute) || cw.words_per_minute <= 0.0) {
            return fail("words per minute must be positive");
        }
        if (!std::isfinite(cw.rise_fall_ms) || cw.rise_fall_ms < 0.0) {
            return fail("CW rise and fall time must not be negative");
        }
        if (!std::isfinite(cw.message_gap_dots) || cw.message_gap_dots < 0.0) {
            return fail("CW message gap must not be negative");
        }
        const double dot_samples = 1.2 * static_cast<double>(rate) / cw.words_per_minute;
        if (dot_samples < 2.0) {
            return fail(std::format("{} WPM at {} S/s gives a {:.2f} sample dot",
                                    cw.words_per_minute, rate, dot_samples));
        }
        bool has_character = false;
        for (char character : cw.text) {
            if (character == ' ') {
                continue;
            }
            if (morse_for(character) == nullptr) {
                return fail(std::format("no Morse code for character '{}'", character));
            }
            has_character = true;
        }
        if (!has_character) {
            return fail("CW text has nothing to key");
        }
        break;
    }

    case Modulation::Am: {
        const AmParams& am = spec.am;
        if (!std::isfinite(am.modulation_index) || am.modulation_index < 0.0) {
            return fail("AM modulation index must be finite and not negative");
        }
        if (spec.audio.empty()) {
            if (auto tone = validate_audio_tone(am.tone_hz, rate, "AM tone"); !tone) {
                return tone;
            }
        } else if (am.audio_bandwidth_hz <= 0) {
            return fail("AM audio bandwidth must be positive");
        }
        break;
    }

    case Modulation::Nfm: {
        const NfmParams& nfm = spec.nfm;
        if (nfm.deviation <= 0) {
            return fail("NFM deviation must be positive");
        }
        if (spec.audio.empty()) {
            if (auto tone = validate_audio_tone(nfm.tone_hz, rate, "NFM tone"); !tone) {
                return tone;
            }
        } else if (nfm.audio_bandwidth_hz <= 0) {
            return fail("NFM audio bandwidth must be positive");
        }
        break;
    }

    case Modulation::Usb:
    case Modulation::Lsb: {
        const SsbParams& ssb = spec.ssb;
        if (ssb.audio_low_hz <= 0 || ssb.audio_high_hz <= ssb.audio_low_hz) {
            return fail("SSB audio passband must be a positive increasing range");
        }
        if (spec.audio.empty()) {
            if (auto tone = validate_audio_tone(ssb.tone_hz, rate, "SSB tone"); !tone) {
                return tone;
            }
            if (ssb.tone_hz < ssb.audio_low_hz || ssb.tone_hz > ssb.audio_high_hz) {
                return fail(std::format("SSB tone {} Hz is outside the {} to {} Hz passband",
                                        ssb.tone_hz, ssb.audio_low_hz, ssb.audio_high_hz));
            }
            if (ssb.tone2_hz != 0) {
                if (auto tone = validate_audio_tone(ssb.tone2_hz, rate, "SSB second tone"); !tone) {
                    return tone;
                }
                if (ssb.tone2_hz < ssb.audio_low_hz || ssb.tone2_hz > ssb.audio_high_hz) {
                    return fail(std::format(
                        "SSB second tone {} Hz is outside the {} to {} Hz passband",
                        ssb.tone2_hz, ssb.audio_low_hz, ssb.audio_high_hz));
                }
            }
        } else {
            if (ssb.hilbert_taps < 3 || (ssb.hilbert_taps % 2) == 0) {
                return fail("Hilbert transformer length must be odd and at least 3");
            }
            // The usable band of an N-tap windowed Hilbert transformer runs from
            // about fs/N to fs/2 - fs/N. Below that the response rolls off and
            // the unwanted sideband comes back, which shows up as a demodulator
            // that half works rather than as an obvious failure.
            const double lowest =
                static_cast<double>(rate) / static_cast<double>(ssb.hilbert_taps);
            if (static_cast<double>(ssb.audio_low_hz) < lowest) {
                const auto needed = static_cast<std::int64_t>(
                    std::ceil(static_cast<double>(rate) / static_cast<double>(ssb.audio_low_hz)));
                return fail(std::format(
                    "a {}-tap Hilbert transformer reaches down to {:.0f} Hz, short of the {} Hz "
                    "passband edge; use at least {} taps",
                    ssb.hilbert_taps, lowest, ssb.audio_low_hz, needed | 1));
            }
        }
        break;
    }

    case Modulation::Fsk2: {
        const Fsk2Params& fsk = spec.fsk;
        if (!std::isfinite(fsk.symbol_rate) || fsk.symbol_rate <= 0.0) {
            return fail("FSK symbol rate must be positive");
        }
        if (fsk.deviation <= 0) {
            return fail("FSK deviation must be positive");
        }
        if (spec.payload_bits.empty() && fsk.symbol_count == 0) {
            return fail("FSK needs either a payload or a positive symbol count");
        }
        const Hertz edge = std::llabs(spec.common.carrier_offset) + fsk.deviation;
        if (edge * 2 > rate) {
            return fail(std::format("FSK tones reach {} Hz, outside +/-{} Hz at {} S/s",
                                    edge, rate / 2, rate));
        }
        break;
    }

    case Modulation::Bpsk:
    case Modulation::Qpsk: {
        const PskParams& psk = spec.psk;
        if (!std::isfinite(psk.symbol_rate) || psk.symbol_rate <= 0.0) {
            return fail("PSK symbol rate must be positive");
        }
        if (!std::isfinite(psk.rolloff) || psk.rolloff < 0.0 || psk.rolloff > 1.0) {
            return fail("PSK rolloff must be between 0 and 1");
        }
        if (psk.span_symbols == 0) {
            return fail("PSK pulse span must be at least one symbol");
        }
        if (spec.payload_bits.empty() && psk.symbol_count == 0) {
            return fail("PSK needs either a payload or a positive symbol count");
        }
        if (spec.kind == Modulation::Qpsk && !spec.payload_bits.empty() &&
            (spec.payload_bits.size() % 2) != 0) {
            return fail(std::format("QPSK takes two bits per symbol, so a payload of {} bits "
                                    "cannot be mapped",
                                    spec.payload_bits.size()));
        }
        break;
    }
    }

    for (std::uint8_t bit : spec.payload_bits) {
        if (bit > 1) {
            return fail("payload bits must each be 0 or 1, one bit per element");
        }
    }
    return {};
}

Expected<SpectralExtent> occupied_extent(const ModulatorSpec& spec)
{
    if (auto ok = validate(spec); !ok) {
        return std::unexpected(ok.error());
    }
    const double symbol_rate =
        (spec.kind == Modulation::Fsk2) ? spec.fsk.symbol_rate : spec.psk.symbol_rate;
    return extent_for(spec, symbol_rate);
}

// ---------------------------------------------------------------------------
// Modulator
// ---------------------------------------------------------------------------

Expected<Modulator> Modulator::create(ModulatorSpec spec)
{
    if (auto ok = validate(spec); !ok) {
        return std::unexpected(with_context(ok.error(), "siggen modulator"));
    }

    Modulator modulator;
    modulator.spec_ = std::move(spec);

    const ModulatorConfig& common = modulator.spec_.common;
    const auto rate = static_cast<double>(common.rate);
    const double amplitude = common.amplitude;
    const double power = amplitude * amplitude;
    double realised_symbol_rate = 0.0;

    switch (modulator.spec_.kind) {
    case Modulation::Cw: {
        const CwParams& cw = modulator.spec_.cw;
        const double dot_samples = 1.2 * rate / cw.words_per_minute;

        // The message as a run-length list in dot units, before it is quantised
        // to samples.
        std::vector<std::pair<bool, double>> runs;
        const auto add_off = [&runs](double dots) {
            if (dots <= 0.0) {
                return;
            }
            if (!runs.empty() && !runs.back().first) {
                runs.back().second += dots;
            } else {
                runs.emplace_back(false, dots);
            }
        };
        const auto add_on = [&runs](double dots) { runs.emplace_back(true, dots); };

        for (char character : cw.text) {
            if (character == ' ') {
                // A word space is seven dots and three of them are already
                // there from the gap after the previous character.
                add_off(4.0);
                continue;
            }
            const char* code = morse_for(character);
            for (const char* element = code; *element != '\0'; ++element) {
                add_on(*element == '.' ? 1.0 : 3.0);
                add_off(1.0);
            }
            // One dot of intra-character gap is already down; two more make the
            // three-dot gap between characters.
            add_off(2.0);
        }
        add_off(std::max(0.0, cw.message_gap_dots - 3.0));

        double cumulative = 0.0;
        SampleIndex previous = 0;
        modulator.cw_elements_.reserve(runs.size());
        for (const auto& run : runs) {
            cumulative += run.second;
            auto edge = static_cast<SampleIndex>(std::llround(cumulative * dot_samples));
            if (edge <= previous) {
                edge = previous + 1;
            }
            modulator.cw_elements_.push_back(CwElement{run.first, previous, edge});
            previous = edge;
        }
        modulator.cw_cycle_ = previous;
        modulator.cw_ramp_samples_ = cw.rise_fall_ms * 0.001 * rate;

        double keyed_energy = 0.0;
        for (const CwElement& element : modulator.cw_elements_) {
            if (!element.keyed) {
                continue;
            }
            const auto length = static_cast<double>(element.stop - element.start);
            const double ramp = std::min(modulator.cw_ramp_samples_, length * 0.5);
            // A raised-cosine edge integrates to 3/8 of its length in mean
            // square rather than the full 1, so two edges cost 2*(1 - 3/8)*ramp.
            keyed_energy += length - 1.25 * ramp;
        }
        modulator.nominal_power_ = power * keyed_energy / static_cast<double>(modulator.cw_cycle_);
        break;
    }

    case Modulation::Am: {
        double mean_square_audio = 0.5;  // a unit-amplitude tone
        if (!modulator.spec_.audio.empty()) {
            double total = 0.0;
            for (float value : modulator.spec_.audio) {
                total += static_cast<double>(value) * static_cast<double>(value);
            }
            mean_square_audio = total / static_cast<double>(modulator.spec_.audio.size());
        }
        const double index = modulator.spec_.am.modulation_index;
        modulator.nominal_power_ = power * (1.0 + index * index * mean_square_audio);
        break;
    }

    case Modulation::Nfm: {
        const NfmParams& nfm = modulator.spec_.nfm;
        if (modulator.spec_.audio.empty()) {
            // The integral of a sinusoid is closed form, so the tone path needs
            // no precomputation: the phase deviation is beta*sin(wt) with beta
            // the deviation over the tone frequency.
            modulator.nfm_beta_ =
                static_cast<double>(nfm.deviation) / static_cast<double>(nfm.tone_hz);
        } else {
            // Frequency modulation is the integral of the modulating signal, and
            // an integral cannot be evaluated at an arbitrary index without
            // either keeping state across calls, which would break block
            // independence, or keeping this.
            const std::size_t count = modulator.spec_.audio.size();
            modulator.fm_phase_.assign(count + 1, 0.0);
            const double scale = kTwoPi * static_cast<double>(nfm.deviation) / rate;
            double accumulated = 0.0;
            for (std::size_t i = 0; i < count; ++i) {
                accumulated += scale * static_cast<double>(modulator.spec_.audio[i]);
                accumulated = std::fmod(accumulated, kTwoPi);
                modulator.fm_phase_[i + 1] = accumulated;
            }
        }
        modulator.nominal_power_ = power;  // constant envelope
        break;
    }

    case Modulation::Usb:
    case Modulation::Lsb: {
        const SsbParams& ssb = modulator.spec_.ssb;
        const Hertz sense = (modulator.spec_.kind == Modulation::Usb) ? 1 : -1;

        if (modulator.spec_.audio.empty()) {
            // The analytic signal of a cosine is exactly exp(j*w*t), so a tone
            // needs no phasing network at all: the sideband is a single
            // rotation, already folded onto the carrier. That is not a shortcut
            // around the phasing method, it is the phasing method's exact
            // answer. It also happens to be the only thing that works at a
            // wideband sample rate, where a Hilbert transformer reaching down
            // to an audio frequency would need tens of thousands of taps.
            modulator.ssb_freq1_ = common.carrier_offset + sense * ssb.tone_hz;
            modulator.ssb_two_tone_ = ssb.tone2_hz != 0;
            modulator.ssb_freq2_ = common.carrier_offset + sense * ssb.tone2_hz;
            // Two equal tones at half amplitude each keep the peak envelope at
            // the configured amplitude, which is what PEP means, and halve the
            // mean power.
            modulator.nominal_power_ = modulator.ssb_two_tone_ ? 0.5 * power : power;
        } else {
            // Phasing rather than filtering, for real audio.
            //
            // The filtering method needs a brick wall at the carrier with a
            // transition region narrower than the lowest audio frequency, a few
            // hundred hertz out of the sample rate. At any rate above a few tens
            // of kilohertz that is a filter of several thousand taps whose
            // passband ripple and group delay land directly on the wanted
            // signal. The phasing method needs only a wideband 90 degree
            // network, its unwanted-sideband suppression follows analytically
            // from the transformer length rather than from a filter design
            // tool, and it is the structure real phasing transmitters have used
            // since the 1950s. Getting the same answer by a different route
            // from the receiver under test is the point.
            const std::size_t taps = ssb.hilbert_taps;
            const auto half = static_cast<std::int64_t>(taps / 2);
            modulator.hilbert_taps_.assign(taps, 0.0);
            for (std::size_t i = 0; i < taps; ++i) {
                const std::int64_t lag = static_cast<std::int64_t>(i) - half;
                if (lag == 0 || (lag % 2) == 0) {
                    continue;  // the ideal transformer has no even-lag taps
                }
                const double ideal = 2.0 / (kPi * static_cast<double>(lag));
                const double position =
                    static_cast<double>(i) / static_cast<double>(taps - 1);
                // Blackman: about 58 dB of stopband attenuation, which sets the
                // unwanted sideband suppression.
                const double window = 0.42 - 0.5 * std::cos(kTwoPi * position) +
                                      0.08 * std::cos(2.0 * kTwoPi * position);
                modulator.hilbert_taps_[i] = ideal * window;
            }

            double total = 0.0;
            for (float value : modulator.spec_.audio) {
                total += static_cast<double>(value) * static_cast<double>(value);
            }
            const double mean_square =
                modulator.spec_.audio.empty()
                    ? 0.0
                    : total / static_cast<double>(modulator.spec_.audio.size());
            // |a + jH(a)|^2 averages to twice the mean square of a for any
            // signal the transformer actually covers.
            modulator.nominal_power_ = power * 2.0 * mean_square;
        }
        break;
    }

    case Modulation::Fsk2: {
        const Fsk2Params& fsk = modulator.spec_.fsk;
        modulator.bits_ = modulator.spec_.payload_bits.empty()
                              ? random_bits(fsk.symbol_count, common.seed)
                              : modulator.spec_.payload_bits;

        auto clock = build_symbol_clock(modulator.bits_.size(), common.rate, fsk.symbol_rate);
        if (!clock) {
            return std::unexpected(with_context(clock.error(), "siggen FSK"));
        }
        modulator.clock_ = std::move(*clock);
        realised_symbol_rate = modulator.clock_.effective_symbol_rate(common.rate);

        // Phase accumulated to the start of each symbol. Continuous phase is
        // not a refinement here: a keyed oscillator that restarts each symbol
        // has a spectrum spread far wider than Carson's rule predicts, and no
        // real transmitter produces one.
        const std::size_t symbols = modulator.clock_.symbol_count;
        modulator.fsk_prefix_.assign(symbols + 1, 0.0);
        const double upper = radians_per_sample(fsk.deviation, common.rate);
        const double lower = -upper;
        double accumulated = 0.0;
        for (std::size_t k = 0; k < symbols; ++k) {
            modulator.fsk_prefix_[k] = accumulated;
            const auto length = static_cast<double>(modulator.clock_.boundary[k + 1] -
                                                    modulator.clock_.boundary[k]);
            accumulated += (modulator.bits_[k] != 0 ? upper : lower) * length;
            accumulated = std::fmod(accumulated, kTwoPi);
        }
        modulator.fsk_prefix_[symbols] = accumulated;
        modulator.fsk_cycle_phase_ = accumulated;
        modulator.nominal_power_ = power;  // constant envelope
        break;
    }

    case Modulation::Bpsk:
    case Modulation::Qpsk: {
        const PskParams& psk = modulator.spec_.psk;
        const bool quadrature = modulator.spec_.kind == Modulation::Qpsk;
        const std::size_t bits_per_symbol = quadrature ? 2u : 1u;

        modulator.bits_ = modulator.spec_.payload_bits.empty()
                              ? random_bits(psk.symbol_count * bits_per_symbol, common.seed)
                              : modulator.spec_.payload_bits;

        const std::size_t symbols = modulator.bits_.size() / bits_per_symbol;
        if (psk.span_symbols >= symbols) {
            // The pulse wrap in psk_envelope assumes a tail crosses the cycle
            // boundary at most once, which is what lets it resolve without a
            // division. A payload shorter than the pulse would break that, and
            // a payload that short is not a useful test signal anyway.
            return fail(std::format("a {} symbol pulse needs a payload longer than {} symbols",
                                    psk.span_symbols, psk.span_symbols));
        }

        auto clock = build_symbol_clock(symbols, common.rate, psk.symbol_rate);
        if (!clock) {
            return std::unexpected(with_context(clock.error(), "siggen PSK"));
        }
        modulator.clock_ = std::move(*clock);
        realised_symbol_rate = modulator.clock_.effective_symbol_rate(common.rate);

        // BPSK maps bit 0 to -1 and bit 1 to +1.
        //
        // QPSK takes two bits per symbol, first bit first: the first sets the
        // sign of the in-phase arm and the second the sign of the quadrature
        // arm. That is Gray coding by construction, since adjacent quadrants
        // then differ in exactly one bit, and it is why Gray-coded QPSK has the
        // same bit error rate as BPSK at the same Eb/N0. A demodulator using a
        // different map produces a textbook constellation and a useless bit
        // stream, so this convention has to be copied, not guessed.
        const double unit = 1.0 / std::sqrt(2.0);
        modulator.symbols_.resize(symbols);
        for (std::size_t k = 0; k < symbols; ++k) {
            if (quadrature) {
                const double in_phase = (modulator.bits_[2 * k] != 0) ? -unit : unit;
                const double quadrature_arm = (modulator.bits_[2 * k + 1] != 0) ? -unit : unit;
                modulator.symbols_[k] = Complex32(static_cast<float>(in_phase),
                                                  static_cast<float>(quadrature_arm));
            } else {
                modulator.symbols_[k] =
                    Complex32((modulator.bits_[k] != 0) ? 1.0f : -1.0f, 0.0f);
            }
        }

        const std::size_t table_size = 2 * psk.span_symbols * kRrcStepsPerSymbol + 1;
        modulator.rrc_.resize(table_size);
        for (std::size_t i = 0; i < table_size; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(kRrcStepsPerSymbol) -
                             static_cast<double>(psk.span_symbols);
            modulator.rrc_[i] = static_cast<float>(root_raised_cosine(t, psk.rolloff));
        }

        // Pin the two ends to zero.
        //
        // A root raised cosine truncated at eight symbols still has about 2e-3
        // of its peak left at the cut, so a neighbouring symbol entering or
        // leaving the window steps the envelope by that much. It is a real
        // discontinuity, once per symbol, and it is the largest error in the
        // pulse path: it dominates both the table interpolation and the
        // envelope stride by an order of magnitude. Zeroing the endpoints
        // turns the step into a ramp over one table step, changing the pulse
        // only where it was already down 54 dB.
        modulator.rrc_.front() = 0.0f;
        modulator.rrc_.back() = 0.0f;

        const double samples_per_symbol = static_cast<double>(modulator.clock_.cycle_samples) /
                                          static_cast<double>(modulator.clock_.symbol_count);

        // Fold the whole sample-position-to-table-step conversion into one
        // multiply and one add, so the inner loop over seventeen neighbouring
        // pulses has no division in it.
        modulator.rrc_scale_ =
            static_cast<double>(kRrcStepsPerSymbol) / samples_per_symbol;
        modulator.rrc_base_ =
            static_cast<double>(psk.span_symbols) * static_cast<double>(kRrcStepsPerSymbol);
        modulator.rrc_limit_ = static_cast<double>(table_size - 1);
        modulator.cycle_span_ = static_cast<double>(modulator.clock_.cycle_samples);
        modulator.boundary_seconds_.resize(symbols);
        for (std::size_t k = 0; k < symbols; ++k) {
            modulator.boundary_seconds_[k] =
                static_cast<double>(modulator.clock_.boundary[k]);
        }

        // Scale so an independent unit-power symbol stream leaves at unit mean
        // power, which is what makes ModulatorConfig::amplitude mean RMS here.
        // The mean power of the pulse train is the sum of h^2 over the output
        // sample grid divided by the samples per symbol. The sum is taken
        // through the same table the renderer uses, so the calibration matches
        // what actually comes out rather than the ideal it was built from.
        const auto reach = static_cast<std::int64_t>(
            std::ceil(static_cast<double>(psk.span_symbols) * samples_per_symbol));
        double energy = 0.0;
        for (std::int64_t offset = -reach; offset <= reach; ++offset) {
            const double tap =
                modulator.rrc_lookup(static_cast<double>(offset) / samples_per_symbol);
            energy += tap * tap;
        }
        modulator.rrc_gain_ = (energy > 0.0) ? std::sqrt(samples_per_symbol / energy) : 1.0;
        modulator.nominal_power_ = power;

        // How often the envelope actually has to be evaluated.
        //
        // The envelope's highest frequency is symbol_rate*(1+rolloff)/2, so the
        // output is oversampled against it by roughly samples_per_symbol. A
        // stride of samples_per_symbol/1024 still leaves on the order of a
        // thousand evaluation points per envelope cycle. Measured against the
        // exact path at 1200 baud and 20 MS/s the difference is 100 dB below
        // the signal, which is 0.001 percent EVM.
        //
        // The stride is a power of two and capped at the phase anchor interval
        // so that evaluation points divide the anchor grid, which is what keeps
        // the output identical however the caller blocks the render. Low
        // oversampling gives a stride of one and the exact path, which is the
        // case where the pulse shape is doing real work and an approximation
        // would be visible.
        modulator.psk_stride_ = 1;
        if (!psk.exact_envelope) {
            const double budget = samples_per_symbol / 1024.0;
            while (modulator.psk_stride_ * 2 <= kPhaseAnchorInterval &&
                   static_cast<double>(modulator.psk_stride_ * 2) <= budget) {
                modulator.psk_stride_ *= 2;
            }
        }
        break;
    }
    }

    modulator.extent_ = extent_for(modulator.spec_, realised_symbol_rate);
    return modulator;
}

std::string_view Modulator::text() const
{
    return (spec_.kind == Modulation::Cw) ? std::string_view(spec_.cw.text) : std::string_view{};
}

double Modulator::effective_symbol_rate() const
{
    return clock_.effective_symbol_rate(spec_.common.rate);
}

SampleIndex Modulator::symbol_start_sample(std::uint64_t symbol) const
{
    if (clock_.symbol_count == 0 || clock_.cycle_samples == 0) {
        return 0;
    }
    const auto count = static_cast<std::uint64_t>(clock_.symbol_count);
    const std::uint64_t repeat = symbol / count;
    const auto within = static_cast<std::size_t>(symbol % count);
    return repeat * clock_.cycle_samples + clock_.boundary[within];
}

SampleIndex Modulator::cycle_samples() const
{
    return (spec_.kind == Modulation::Cw) ? cw_cycle_ : clock_.cycle_samples;
}

double Modulator::audio_at(SampleIndex index) const
{
    return (index < spec_.audio.size())
               ? static_cast<double>(spec_.audio[static_cast<std::size_t>(index)])
               : 0.0;
}

double Modulator::cw_envelope(SampleIndex index, std::size_t& cursor) const
{
    const SampleIndex position = index % cw_cycle_;

    // A linear cursor rather than a binary search. Renders walk forward, so the
    // cursor is amortised constant time, and it is only a search hint: resetting
    // it changes nothing about the value returned.
    if (cursor >= cw_elements_.size() || position < cw_elements_[cursor].start) {
        cursor = 0;
    }
    while (cursor + 1 < cw_elements_.size() && cw_elements_[cursor].stop <= position) {
        ++cursor;
    }

    const CwElement& element = cw_elements_[cursor];
    if (!element.keyed) {
        return 0.0;
    }

    const auto length = static_cast<double>(element.stop - element.start);
    const double ramp = std::min(cw_ramp_samples_, length * 0.5);
    if (ramp <= 0.0) {
        return 1.0;
    }

    const auto from_start = static_cast<double>(position - element.start);
    const auto to_end = static_cast<double>(element.stop - position);
    double envelope = 1.0;
    if (from_start < ramp) {
        envelope *= 0.5 - 0.5 * std::cos(kPi * from_start / ramp);
    }
    if (to_end <= ramp) {
        envelope *= 0.5 - 0.5 * std::cos(kPi * to_end / ramp);
    }
    return envelope;
}

double Modulator::rrc_tap_at(double table_position) const
{
    if (table_position <= 0.0 || table_position >= rrc_limit_) {
        return 0.0;
    }
    const auto lower = static_cast<std::size_t>(table_position);
    const double fraction = table_position - static_cast<double>(lower);
    const auto a = static_cast<double>(rrc_[lower]);
    const auto b = static_cast<double>(rrc_[lower + 1]);
    return a + fraction * (b - a);
}

double Modulator::rrc_lookup(double symbol_offset) const
{
    if (rrc_.size() < 2) {
        return 0.0;
    }
    return rrc_tap_at(symbol_offset * static_cast<double>(kRrcStepsPerSymbol) + rrc_base_);
}

Complex32 Modulator::psk_envelope(SampleIndex position_in_cycle, std::size_t symbol) const
{
    const auto symbols = static_cast<std::int64_t>(clock_.symbol_count);
    const auto span = static_cast<std::int64_t>(spec_.psk.span_symbols);
    const auto centre_symbol = static_cast<std::int64_t>(symbol);
    const auto position = static_cast<double>(position_in_cycle);

    double in_phase = 0.0;
    double quadrature = 0.0;
    for (std::int64_t offset = -span; offset <= span; ++offset) {
        std::int64_t wrapped = centre_symbol + offset;
        double shift = 0.0;

        // The payload repeats, so a pulse tail running off one end of the cycle
        // arrives at the other, which is what makes the signal genuinely
        // periodic instead of glitching once per repeat. create() guarantees
        // the span is shorter than the payload, so the wrap is never more than
        // one cycle and needs no division to resolve.
        if (wrapped < 0) {
            wrapped += symbols;
            shift = -cycle_span_;
        } else if (wrapped >= symbols) {
            wrapped -= symbols;
            shift = cycle_span_;
        }

        const auto slot = static_cast<std::size_t>(wrapped);
        const double centre = boundary_seconds_[slot] + shift;
        const double tap = rrc_tap_at((position - centre) * rrc_scale_ + rrc_base_);
        if (tap == 0.0) {
            continue;
        }
        const Complex32& value = symbols_[slot];
        in_phase += static_cast<double>(value.real()) * tap;
        quadrature += static_cast<double>(value.imag()) * tap;
    }

    return Complex32(static_cast<float>(in_phase * rrc_gain_),
                     static_cast<float>(quadrature * rrc_gain_));
}

Complex32 Modulator::psk_envelope_at(SampleIndex index) const
{
    const SampleIndex position = index % clock_.cycle_samples;
    return psk_envelope(position, clock_.index_at(position));
}

double Modulator::nfm_phase_at(SampleIndex index, double tone_sine) const
{
    if (fm_phase_.empty()) {
        return nfm_beta_ * tone_sine;
    }
    const std::size_t last = fm_phase_.size() - 1;
    const auto position = (index < last) ? static_cast<std::size_t>(index) : last;
    return fm_phase_[position];
}

double Modulator::hilbert_at(SampleIndex index) const
{
    const std::size_t taps = hilbert_taps_.size();
    if (taps == 0) {
        return 0.0;
    }

    const auto half = static_cast<std::int64_t>(taps / 2);
    const auto centre = static_cast<std::int64_t>(index);

    // Only odd lags are non-zero, so step by two from whichever index gives an
    // odd lag. Half the taps are structurally zero and skipping them halves the
    // cost of the one path in this file that is genuinely filter bound.
    const std::size_t first = ((half % 2) == 0) ? 1u : 0u;
    double total = 0.0;
    for (std::size_t i = first; i < taps; i += 2) {
        const std::int64_t source = centre + half - static_cast<std::int64_t>(i);
        if (source < 0) {
            continue;
        }
        total += hilbert_taps_[i] * audio_at(static_cast<SampleIndex>(source));
    }
    return total;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void Modulator::render(SampleIndex start, ComplexSpan out) const
{
    std::fill(out.begin(), out.end(), Complex32(0.0f, 0.0f));
    accumulate(start, out, 1.0);
}

void Modulator::accumulate(SampleIndex start, ComplexSpan out, double gain) const
{
    if (out.empty() || gain == 0.0) {
        return;
    }
    switch (spec_.kind) {
    case Modulation::Cw:   accumulate_cw(start, out, gain);  return;
    case Modulation::Am:   accumulate_am(start, out, gain);  return;
    case Modulation::Nfm:  accumulate_nfm(start, out, gain); return;
    case Modulation::Usb:
    case Modulation::Lsb:  accumulate_ssb(start, out, gain); return;
    case Modulation::Fsk2: accumulate_fsk(start, out, gain); return;
    case Modulation::Bpsk:
    case Modulation::Qpsk: accumulate_psk(start, out, gain); return;
    }
}

void Modulator::accumulate_cw(SampleIndex start, ComplexSpan out, double gain) const
{
    const ModulatorConfig& common = spec_.common;
    const double level = gain * common.amplitude;
    const double step = radians_per_sample(common.carrier_offset, common.rate);
    std::size_t cursor = 0;

    walk_anchored(
        start, out.size(), kPhaseAnchorInterval,
        [&](SampleIndex anchor) {
            Phasor carrier;
            carrier.anchor(common.initial_phase +
                           exact_phase(common.carrier_offset, common.rate, anchor));
            carrier.set_rate(step);
            return carrier;
        },
        [&](const Phasor& carrier, SampleIndex index, std::size_t offset) {
            const double envelope = level * cw_envelope(index, cursor);
            out[offset] += Complex32(static_cast<float>(envelope * carrier.re),
                                     static_cast<float>(envelope * carrier.im));
        });
}

void Modulator::accumulate_am(SampleIndex start, ComplexSpan out, double gain) const
{
    const ModulatorConfig& common = spec_.common;
    const AmParams& am = spec_.am;
    const double level = gain * common.amplitude;
    const double carrier_step = radians_per_sample(common.carrier_offset, common.rate);
    const double tone_step = radians_per_sample(am.tone_hz, common.rate);
    const bool supplied_audio = !spec_.audio.empty();

    struct State {
        Phasor carrier;
        Phasor tone;
        void advance()
        {
            carrier.advance();
            tone.advance();
        }
    };

    walk_anchored(
        start, out.size(), kPhaseAnchorInterval,
        [&](SampleIndex anchor) {
            State state;
            state.carrier.anchor(common.initial_phase +
                                 exact_phase(common.carrier_offset, common.rate, anchor));
            state.carrier.set_rate(carrier_step);
            state.tone.anchor(exact_phase(am.tone_hz, common.rate, anchor));
            state.tone.set_rate(tone_step);
            return state;
        },
        [&](const State& state, SampleIndex index, std::size_t offset) {
            const double audio = supplied_audio ? audio_at(index) : state.tone.re;
            const double envelope = level * (1.0 + am.modulation_index * audio);
            out[offset] += Complex32(static_cast<float>(envelope * state.carrier.re),
                                     static_cast<float>(envelope * state.carrier.im));
        });
}

void Modulator::accumulate_nfm(SampleIndex start, ComplexSpan out, double gain) const
{
    const ModulatorConfig& common = spec_.common;
    const NfmParams& nfm = spec_.nfm;
    const double level = gain * common.amplitude;
    const double carrier_step = radians_per_sample(common.carrier_offset, common.rate);
    const double tone_step = radians_per_sample(nfm.tone_hz, common.rate);

    struct State {
        Phasor carrier;
        Phasor tone;
        void advance()
        {
            carrier.advance();
            tone.advance();
        }
    };

    walk_anchored(
        start, out.size(), kPhaseAnchorInterval,
        [&](SampleIndex anchor) {
            State state;
            state.carrier.anchor(common.initial_phase +
                                 exact_phase(common.carrier_offset, common.rate, anchor));
            state.carrier.set_rate(carrier_step);
            state.tone.anchor(exact_phase(nfm.tone_hz, common.rate, anchor));
            state.tone.set_rate(tone_step);
            return state;
        },
        [&](const State& state, SampleIndex index, std::size_t offset) {
            const double deviation = nfm_phase_at(index, state.tone.im);
            // exp(j*beta*sin(x)) does not factor into a product of phasors, so
            // this is the one mode that costs a real transcendental pair per
            // sample. Everything else in this file rotates.
            const double cosine = std::cos(deviation);
            const double sine = std::sin(deviation);
            const double re = state.carrier.re * cosine - state.carrier.im * sine;
            const double im = state.carrier.re * sine + state.carrier.im * cosine;
            out[offset] += Complex32(static_cast<float>(level * re),
                                     static_cast<float>(level * im));
        });
}

void Modulator::accumulate_ssb(SampleIndex start, ComplexSpan out, double gain) const
{
    const ModulatorConfig& common = spec_.common;
    const double level = gain * common.amplitude;

    if (spec_.audio.empty()) {
        const double scale = ssb_two_tone_ ? 0.5 * level : level;
        const double step1 = radians_per_sample(ssb_freq1_, common.rate);
        const double step2 = radians_per_sample(ssb_freq2_, common.rate);
        const bool two_tone = ssb_two_tone_;

        struct State {
            Phasor first;
            Phasor second;
            void advance()
            {
                first.advance();
                second.advance();
            }
        };

        walk_anchored(
            start, out.size(), kPhaseAnchorInterval,
            [&](SampleIndex anchor) {
                State state;
                state.first.anchor(common.initial_phase +
                                   exact_phase(ssb_freq1_, common.rate, anchor));
                state.first.set_rate(step1);
                state.second.anchor(common.initial_phase +
                                    exact_phase(ssb_freq2_, common.rate, anchor));
                state.second.set_rate(step2);
                return state;
            },
            [&](const State& state, SampleIndex, std::size_t offset) {
                double re = state.first.re;
                double im = state.first.im;
                if (two_tone) {
                    re += state.second.re;
                    im += state.second.im;
                }
                out[offset] += Complex32(static_cast<float>(scale * re),
                                         static_cast<float>(scale * im));
            });
        return;
    }

    const double sense = (spec_.kind == Modulation::Usb) ? 1.0 : -1.0;
    const double carrier_step = radians_per_sample(common.carrier_offset, common.rate);

    walk_anchored(
        start, out.size(), kPhaseAnchorInterval,
        [&](SampleIndex anchor) {
            Phasor carrier;
            carrier.anchor(common.initial_phase +
                           exact_phase(common.carrier_offset, common.rate, anchor));
            carrier.set_rate(carrier_step);
            return carrier;
        },
        [&](const Phasor& carrier, SampleIndex index, std::size_t offset) {
            const double in_phase = audio_at(index);
            const double quadrature = sense * hilbert_at(index);
            const double re = in_phase * carrier.re - quadrature * carrier.im;
            const double im = in_phase * carrier.im + quadrature * carrier.re;
            out[offset] += Complex32(static_cast<float>(level * re),
                                     static_cast<float>(level * im));
        });
}

void Modulator::accumulate_fsk(SampleIndex start, ComplexSpan out, double gain) const
{
    const ModulatorConfig& common = spec_.common;
    const double level = gain * common.amplitude;
    const SampleIndex cycle = clock_.cycle_samples;
    const double deviation_step = radians_per_sample(spec_.fsk.deviation, common.rate);
    const double carrier_step = radians_per_sample(common.carrier_offset, common.rate);

    const SampleIndex end = start + out.size();
    SampleIndex index = start;

    while (index < end) {
        const SampleIndex repeat = index / cycle;
        const SampleIndex position = index - repeat * cycle;
        const std::size_t symbol = clock_.index_at(position);
        const SampleIndex cycle_base = repeat * cycle;
        const SampleIndex symbol_start = cycle_base + clock_.boundary[symbol];
        const SampleIndex symbol_stop = cycle_base + clock_.boundary[symbol + 1];

        // Anchors are the coarse phase grid and the symbol boundaries together.
        // Both are functions of the absolute index alone, so the segment a given
        // sample falls in does not depend on how the caller blocked the render.
        const SampleIndex grid = index - (index % kPhaseAnchorInterval);
        const SampleIndex anchor = std::max(grid, symbol_start);
        const SampleIndex stop =
            std::min({end, symbol_stop, grid + kPhaseAnchorInterval});

        const double sense = (bits_[symbol] != 0) ? 1.0 : -1.0;
        const double phase =
            common.initial_phase + exact_phase(common.carrier_offset, common.rate, anchor) +
            std::fmod(static_cast<double>(repeat) * fsk_cycle_phase_, kTwoPi) +
            fsk_prefix_[symbol] +
            sense * deviation_step * static_cast<double>(anchor - symbol_start);

        Phasor tone;
        tone.anchor(phase);
        tone.set_rate(carrier_step + sense * deviation_step);

        for (SampleIndex skipped = anchor; skipped < index; ++skipped) {
            tone.advance();
        }
        for (; index < stop; ++index) {
            const auto offset = static_cast<std::size_t>(index - start);
            out[offset] += Complex32(static_cast<float>(level * tone.re),
                                     static_cast<float>(level * tone.im));
            tone.advance();
        }
    }
}

void Modulator::accumulate_psk(SampleIndex start, ComplexSpan out, double gain) const
{
    const ModulatorConfig& common = spec_.common;
    const double level = gain * common.amplitude;
    const double carrier_step = radians_per_sample(common.carrier_offset, common.rate);
    const SampleIndex cycle = clock_.cycle_samples;

    // Written out rather than going through walk_anchored so the position in
    // the payload cycle and the current symbol can be carried forward instead
    // of recovered with two 64-bit divisions per sample. Both are integers
    // stepped by one, so the values are identical to recomputing them, and the
    // anchoring rule is unchanged.
    const SampleIndex end = start + out.size();
    SampleIndex index = start;

    while (index < end) {
        const SampleIndex anchor = index - (index % kPhaseAnchorInterval);
        const SampleIndex stop = std::min<SampleIndex>(end, anchor + kPhaseAnchorInterval);

        Phasor carrier;
        carrier.anchor(common.initial_phase +
                       exact_phase(common.carrier_offset, common.rate, anchor));
        carrier.set_rate(carrier_step);
        for (SampleIndex skipped = anchor; skipped < index; ++skipped) {
            carrier.advance();
        }

        if (psk_stride_ == 1) {
            SampleIndex position = index % cycle;
            std::size_t symbol = clock_.index_at(position);

            for (; index < stop; ++index) {
                const Complex32 envelope = psk_envelope(position, symbol);
                const auto in_phase = static_cast<double>(envelope.real());
                const auto quadrature = static_cast<double>(envelope.imag());
                const double re = in_phase * carrier.re - quadrature * carrier.im;
                const double im = in_phase * carrier.im + quadrature * carrier.re;
                out[static_cast<std::size_t>(index - start)] +=
                    Complex32(static_cast<float>(level * re), static_cast<float>(level * im));

                carrier.advance();
                ++position;
                if (position >= cycle) {
                    position = 0;
                    symbol = 0;
                } else if (position >= clock_.boundary[symbol + 1]) {
                    ++symbol;
                }
            }
            continue;
        }

        // Interpolated envelope. The two endpoints are evaluated at absolute
        // multiples of the stride, so which pair of points a sample falls
        // between depends on the sample index and nothing else.
        const SampleIndex stride = psk_stride_;
        const double inverse_stride = 1.0 / static_cast<double>(stride);
        SampleIndex node = index - (index % stride);
        Complex32 near_point = psk_envelope_at(node);
        Complex32 far_point = psk_envelope_at(node + stride);

        for (; index < stop; ++index) {
            if (index >= node + stride) {
                node += stride;
                near_point = far_point;
                far_point = psk_envelope_at(node + stride);
            }

            const double weight = static_cast<double>(index - node) * inverse_stride;
            const auto low_re = static_cast<double>(near_point.real());
            const auto low_im = static_cast<double>(near_point.imag());
            const double in_phase =
                low_re + weight * (static_cast<double>(far_point.real()) - low_re);
            const double quadrature =
                low_im + weight * (static_cast<double>(far_point.imag()) - low_im);

            const double re = in_phase * carrier.re - quadrature * carrier.im;
            const double im = in_phase * carrier.im + quadrature * carrier.re;
            out[static_cast<std::size_t>(index - start)] +=
                Complex32(static_cast<float>(level * re), static_cast<float>(level * im));

            carrier.advance();
        }
    }
}

// ---------------------------------------------------------------------------
// One-shot generation
// ---------------------------------------------------------------------------

double peak_magnitude(ConstComplexSpan samples)
{
    double peak = 0.0;
    for (const Complex32& value : samples) {
        const auto re = static_cast<double>(value.real());
        const auto im = static_cast<double>(value.imag());
        peak = std::max(peak, re * re + im * im);
    }
    return std::sqrt(peak);
}

Expected<GeneratedSignal> generate(const ModulatorSpec& spec, std::size_t sample_count)
{
    auto modulator = Modulator::create(spec);
    if (!modulator) {
        return std::unexpected(modulator.error());
    }

    GeneratedSignal result;
    result.samples.assign(sample_count, Complex32(0.0f, 0.0f));
    modulator->render(0, ComplexSpan(result.samples));

    result.payload_bits = modulator->payload_bits();
    result.extent = modulator->occupied_extent();
    result.effective_symbol_rate = modulator->effective_symbol_rate();
    result.cycle_samples = modulator->cycle_samples();
    result.measured_mean_power = buffer_mean_power(result.samples);
    result.peak_magnitude = peak_magnitude(result.samples);
    return result;
}

Expected<GeneratedSignal> generate_cw(const ModulatorConfig& common,
                                      const CwParams& params,
                                      std::size_t sample_count)
{
    ModulatorSpec spec;
    spec.kind = Modulation::Cw;
    spec.common = common;
    spec.cw = params;
    return generate(spec, sample_count);
}

Expected<GeneratedSignal> generate_am(const ModulatorConfig& common,
                                      const AmParams& params,
                                      std::size_t sample_count,
                                      std::vector<float> audio)
{
    ModulatorSpec spec;
    spec.kind = Modulation::Am;
    spec.common = common;
    spec.am = params;
    spec.audio = std::move(audio);
    return generate(spec, sample_count);
}

Expected<GeneratedSignal> generate_nfm(const ModulatorConfig& common,
                                       const NfmParams& params,
                                       std::size_t sample_count,
                                       std::vector<float> audio)
{
    ModulatorSpec spec;
    spec.kind = Modulation::Nfm;
    spec.common = common;
    spec.nfm = params;
    spec.audio = std::move(audio);
    return generate(spec, sample_count);
}

Expected<GeneratedSignal> generate_ssb(const ModulatorConfig& common,
                                       const SsbParams& params,
                                       bool upper_sideband,
                                       std::size_t sample_count,
                                       std::vector<float> audio)
{
    ModulatorSpec spec;
    spec.kind = upper_sideband ? Modulation::Usb : Modulation::Lsb;
    spec.common = common;
    spec.ssb = params;
    spec.audio = std::move(audio);
    return generate(spec, sample_count);
}

Expected<GeneratedSignal> generate_fsk2(const ModulatorConfig& common,
                                        const Fsk2Params& params,
                                        std::size_t sample_count,
                                        std::vector<std::uint8_t> payload_bits)
{
    ModulatorSpec spec;
    spec.kind = Modulation::Fsk2;
    spec.common = common;
    spec.fsk = params;
    spec.payload_bits = std::move(payload_bits);
    return generate(spec, sample_count);
}

Expected<GeneratedSignal> generate_bpsk(const ModulatorConfig& common,
                                        const PskParams& params,
                                        std::size_t sample_count,
                                        std::vector<std::uint8_t> payload_bits)
{
    ModulatorSpec spec;
    spec.kind = Modulation::Bpsk;
    spec.common = common;
    spec.psk = params;
    spec.payload_bits = std::move(payload_bits);
    return generate(spec, sample_count);
}

Expected<GeneratedSignal> generate_qpsk(const ModulatorConfig& common,
                                        const PskParams& params,
                                        std::size_t sample_count,
                                        std::vector<std::uint8_t> payload_bits)
{
    ModulatorSpec spec;
    spec.kind = Modulation::Qpsk;
    spec.common = common;
    spec.psk = params;
    spec.payload_bits = std::move(payload_bits);
    return generate(spec, sample_count);
}

}  // namespace revenant::siggen
