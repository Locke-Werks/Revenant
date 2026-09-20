// Reference floating point discipline, before anything else in the file.
#include "core/dsp/reference_fp.h"

#include "core/dsp/synth/wfm_mod.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <utility>

#include "core/decode/rds_bits.h"

namespace revenant::siggen {

static_assert(dsp::kReferenceFpDisciplineApplied,
              "wfm_mod.cpp must include core/dsp/reference_fp.h first");

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kTwoPi = 2.0 * kPi;

// The top of the FM audio band. The sum channel has to stay below the pilot
// at 19 kHz, and the difference channel's upper sideband has to stay below
// the RDS subcarrier's lower edge at 57000 - 2375 = 54625 Hz, which puts the
// same 15 kHz ceiling on it with 1.6 kHz to spare.
constexpr Hertz kAudioCeilingHz = 15000;

// And a floor, so that a "tone" at a fraction of a hertz cannot make the
// closed-form integral below divide by something near zero. 30 Hz is the
// bottom of the FM audio band in ordinary practice.
constexpr Hertz kAudioFloorHz = 30;

// The highest frequency the composite reaches: the RDS subcarrier plus the
// clause 1.7 shaping cutoff. Carson's rule is stated against this.
constexpr Hertz kCompositeTopHz = decode::kSubcarrierHz + decode::kShapingCutoffHz;

[[nodiscard]] double turns_to_sine(double turns)
{
    return std::sin(kTwoPi * (turns - std::floor(turns)));
}

[[nodiscard]] double turns_to_cosine(double turns)
{
    return std::cos(kTwoPi * (turns - std::floor(turns)));
}

// Carson's rule: the occupied bandwidth of an angle-modulated carrier is
// twice the sum of the peak deviation and the highest modulating frequency.
// Carlson, "Communication Systems", chapter 5, which is the same textbook
// core/dsp/vrx_reference.h cites for the detectors.
[[nodiscard]] Hertz carson_bandwidth(Hertz peak_deviation_hz)
{
    return 2 * (peak_deviation_hz + kCompositeTopHz);
}

}  // namespace

// ---------------------------------------------------------------------------
// Pre-emphasis
// ---------------------------------------------------------------------------

std::string_view preemphasis_name(Preemphasis kind)
{
    // No default label. /w14062 makes an unhandled enumerator a warning and
    // /WX makes it fatal, so a third curve cannot be added without deciding
    // what it is called.
    switch (kind) {
        case Preemphasis::None: return "none";
        case Preemphasis::Us75: return "75us";
        case Preemphasis::Eu50: return "50us";
    }
    return "unknown";
}

Expected<Preemphasis> preemphasis_from_name(std::string_view name)
{
    if (name == "none") {
        return Preemphasis::None;
    }
    if (name == "75us") {
        return Preemphasis::Us75;
    }
    if (name == "50us") {
        return Preemphasis::Eu50;
    }
    return fail(std::format("unknown pre-emphasis '{}', expected none, 75us or 50us", name));
}

double preemphasis_seconds(Preemphasis kind)
{
    switch (kind) {
        case Preemphasis::None: return 0.0;
        case Preemphasis::Us75: return 75e-6;
        case Preemphasis::Eu50: return 50e-6;
    }
    return 0.0;
}

double preemphasis_gain(Preemphasis kind, Hertz tone_hz)
{
    const double tau = preemphasis_seconds(kind);
    if (tau <= 0.0) {
        return 1.0;
    }
    const double x = kTwoPi * static_cast<double>(tone_hz) * tau;
    return std::sqrt(1.0 + x * x);
}

double preemphasis_phase_radians(Preemphasis kind, Hertz tone_hz)
{
    const double tau = preemphasis_seconds(kind);
    if (tau <= 0.0) {
        return 0.0;
    }
    return std::atan(kTwoPi * static_cast<double>(tone_hz) * tau);
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] Status check_tone(Hertz tone_hz, std::string_view which)
{
    if (tone_hz < kAudioFloorHz || tone_hz > kAudioCeilingHz) {
        return fail(std::format(
            "the {} programme tone is {} Hz, outside the {} to {} Hz FM audio band. Above "
            "the ceiling the difference channel's upper sideband runs into the RDS "
            "subcarrier's lower edge at {} Hz.",
            which, tone_hz, kAudioFloorHz, kAudioCeilingHz,
            decode::kSubcarrierHz - decode::kShapingCutoffHz));
    }
    return {};
}

}  // namespace

Status validate(const WfmSpec& spec)
{
    if (spec.rate <= 0 || spec.rate > kMaxSampleRate) {
        return fail(std::format("WFM station sample rate {} is outside (0, {}]", spec.rate,
                                kMaxSampleRate));
    }
    if (spec.peak_deviation_hz <= 0) {
        return fail("WFM peak deviation must be positive");
    }
    if (!std::isfinite(spec.amplitude) || spec.amplitude < 0.0) {
        return fail(std::format("WFM amplitude {} is not a finite non-negative number",
                                spec.amplitude));
    }
    if (!std::isfinite(spec.initial_phase)) {
        return fail("WFM initial phase is not finite");
    }

    const Hertz occupied = carson_bandwidth(spec.peak_deviation_hz);
    if (occupied > spec.rate) {
        return fail(std::format(
            "a {} Hz deviation station occupies {} Hz by Carson's rule and the sample rate "
            "is {}. An FM signal folded onto itself is not a quieter station, it is a "
            "different signal, and nothing downstream reports that it happened.",
            spec.peak_deviation_hz, occupied, spec.rate));
    }

    // Placement, not just width. An offset station whose skirt runs past
    // Nyquist aliases exactly as badly as one that never fitted.
    const Hertz reach = std::abs(spec.carrier_offset) + occupied / 2;
    if (reach > spec.rate / 2) {
        return fail(std::format(
            "a station at an offset of {} Hz reaches {} Hz from baseband DC, past the {} Hz "
            "Nyquist edge of a {} S/s stream",
            spec.carrier_offset, reach, spec.rate / 2, spec.rate));
    }

    if (spec.programme.audio_deviation_hz < 0) {
        return fail("WFM programme deviation cannot be negative");
    }
    if (Status checked = check_tone(spec.programme.left_tone_hz, "left"); !checked) {
        return checked;
    }
    if (spec.programme.stereo) {
        if (Status checked = check_tone(spec.programme.right_tone_hz, "right"); !checked) {
            return checked;
        }
        if (!spec.rds.pilot_enabled) {
            return fail(
                "a stereo programme needs the pilot: the 38 kHz difference subcarrier is "
                "suppressed, and without the pilot a receiver has nothing to regenerate it "
                "from. Set rds.pilot_enabled, or set programme.stereo false for a mono "
                "transmission.");
        }
    }

    if (spec.rds.mono_tone_hz != 0 || spec.rds.mono_deviation_hz != 0) {
        return fail(
            "rds.mono_tone_hz and rds.mono_deviation_hz are not usable on a WFM station. "
            "The programme audio is FmProgramme's, and a tone arriving through the RDS spec "
            "would skip the stereo matrix and the pre-emphasis curve, which is exactly the "
            "path those two are there to be measured against.");
    }
    if (spec.rds.subcarrier_offset_hz != 0) {
        return fail(
            "rds.subcarrier_offset_hz is not usable on a WFM station. The FM phase is the "
            "integral of the composite, and the closed form for the RDS term rests on the "
            "subcarrier being exactly 48 times the bit rate, which that field deliberately "
            "breaks. clock_error_ppm moves the transmitter's whole clock and does not break "
            "it, so use that instead.");
    }

    RdsModSpec rds = spec.rds;
    rds.rate = spec.rate;
    if (Status checked = validate(rds); !checked) {
        return std::unexpected(with_context(checked.error(), "WFM station RDS layer"));
    }
    return {};
}

Expected<SpectralExtent> occupied_extent(const WfmSpec& spec)
{
    if (Status checked = validate(spec); !checked) {
        return std::unexpected(checked.error());
    }
    const Hertz half = carson_bandwidth(spec.peak_deviation_hz) / 2;
    return SpectralExtent{spec.carrier_offset - half, spec.carrier_offset + half};
}

// ---------------------------------------------------------------------------
// The composite
// ---------------------------------------------------------------------------

Expected<FmComposite> FmComposite::create(WfmSpec spec)
{
    if (Status checked = validate(spec); !checked) {
        return std::unexpected(checked.error());
    }

    // One composite, one sample rate. The header says this is replaced rather
    // than refused.
    spec.rds.rate = spec.rate;

    Expected<RdsModulator> rds = RdsModulator::create(spec.rds);
    if (!rds) {
        return std::unexpected(with_context(rds.error(), "WFM station RDS layer"));
    }
    if (!rds->supports_integral()) {
        // validate() already refused the one field that can cause this. If
        // this ever fires, the two have gone out of step.
        return fail("the RDS layer of this station cannot be integrated in closed form, so "
                    "the FM phase cannot be a pure function of the sample index");
    }

    FmComposite composite(std::move(*rds));
    composite.spec_ = std::move(spec);

    const WfmSpec& s = composite.spec_;
    const FmProgramme& programme = s.programme;
    const auto full_scale = static_cast<double>(kCompositePeakDeviationHz);
    const auto rate = static_cast<double>(s.rate);

    const double level = static_cast<double>(programme.audio_deviation_hz) / full_scale;
    const double left_gain = preemphasis_gain(programme.preemphasis, programme.left_tone_hz);
    const double right_gain = preemphasis_gain(programme.preemphasis, programme.right_tone_hz);
    const double left_phase =
        preemphasis_phase_radians(programme.preemphasis, programme.left_tone_hz) / kTwoPi;
    const double right_phase =
        preemphasis_phase_radians(programme.preemphasis, programme.right_tone_hz) / kTwoPi;

    // The programme tones do not carry clock_error_ppm. That field is the
    // transmitter's own clock, which the pilot, the stereo subcarrier and the
    // bit clock are all derived from; the programme comes from a studio and
    // has no reason to share it. Pushing it through the audio too would make
    // a clock error look like a pitch change, which is not what a clock error
    // does to a broadcast.
    const double left_turns = static_cast<double>(programme.left_tone_hz) / rate;
    const double right_turns = static_cast<double>(programme.right_tone_hz) / rate;

    double audio_bound = 0.0;
    if (level > 0.0) {
        if (!programme.stereo) {
            composite.audio_.push_back(Sinusoid{level * left_gain, left_turns, left_phase});
            audio_bound = level * left_gain;
        } else {
            // The matrix: (L+R)/2 at baseband, (L-R)/2 on the 38 kHz
            // subcarrier. |(L+R)/2| + |(L-R)/2| is max(|L|, |R|) for any L
            // and R, so the pair peaks exactly where a mono programme of the
            // same level does and the bound below is not a stereo penalty.
            composite.audio_.push_back(
                Sinusoid{0.5 * level * left_gain, left_turns, left_phase});
            composite.audio_.push_back(
                Sinusoid{0.5 * level * right_gain, right_turns, right_phase});

            // The second harmonic of the pilot, taken from the pilot itself
            // rather than from a nominal 38000, so clock_error_ppm moves the
            // two together as EN 50067 clause 1.5's coherence requires.
            const double stereo_turns = 2.0 * composite.rds_.pilot_turns_per_sample();

            // sin(a)*sin(b) is [cos(a-b) - cos(a+b)]/2, and a cosine is a
            // sine a quarter turn ahead. Four entries rather than a product
            // evaluated twice, once in render and once in the integral.
            const double left_quarter = 0.25 * level * left_gain;
            const double right_quarter = 0.25 * level * right_gain;
            composite.audio_.push_back(
                Sinusoid{left_quarter, left_turns - stereo_turns, left_phase + 0.25});
            composite.audio_.push_back(
                Sinusoid{-left_quarter, left_turns + stereo_turns, left_phase + 0.25});
            composite.audio_.push_back(
                Sinusoid{-right_quarter, right_turns - stereo_turns, right_phase + 0.25});
            composite.audio_.push_back(
                Sinusoid{right_quarter, right_turns + stereo_turns, right_phase + 0.25});

            audio_bound = level * std::max(left_gain, right_gain);
        }
    }

    // A sum of the three parts' own peaks. They do not peak together, so this
    // is an upper bound and not a prediction; what it is for is saying
    // whether a spec CAN over deviate, which a measurement over one buffer
    // cannot.
    const double pilot_bound =
        s.rds.pilot_enabled ? static_cast<double>(s.rds.pilot_deviation_hz) / full_scale : 0.0;
    composite.peak_bound_ =
        pilot_bound + audio_bound + composite.rds_.envelope_peak();

    // Read while still zero, so it comes back as the bare antiderivative at
    // the origin, which is what every later value has to have subtracted.
    // Same trick RdsModulator::create uses, for the same reason.
    composite.integral_base_ = 0.0;
    composite.integral_base_ = composite.integral(0);

    return composite;
}

double FmComposite::audio_at(SampleIndex index) const
{
    const double t = static_cast<double>(index) + spec_.rds.start_offset_samples;
    double total = 0.0;
    for (const Sinusoid& component : audio_) {
        total += component.amplitude *
                 turns_to_sine(component.turns_per_sample * t + component.phase_turns);
    }
    return total;
}

void FmComposite::render(SampleIndex start, dsp::RealSpan out) const
{
    // The pilot and the RDS subcarrier come from RdsModulator. The mono tone
    // it can also render is refused by validate(), so this is exactly those
    // two.
    rds_.render(start, out);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<float>(static_cast<double>(out[i]) + audio_at(start + i));
    }
}

void FmComposite::render_rds_only(SampleIndex start, dsp::RealSpan out) const
{
    rds_.render_rds_only(start, out);
}

void FmComposite::render_pilot_only(SampleIndex start, dsp::RealSpan out) const
{
    const auto full_scale = static_cast<double>(kCompositePeakDeviationHz);
    const double amplitude =
        spec_.rds.pilot_enabled
            ? static_cast<double>(spec_.rds.pilot_deviation_hz) / full_scale
            : 0.0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<float>(amplitude * turns_to_sine(rds_.pilot_turns_at(start + i)));
    }
}

void FmComposite::render_audio_only(SampleIndex start, dsp::RealSpan out) const
{
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<float>(audio_at(start + i));
    }
}

double FmComposite::integral(SampleIndex index) const
{
    const double t = static_cast<double>(index) + spec_.rds.start_offset_samples;

    double total = rds_.composite_integral(index);
    for (const Sinusoid& component : audio_) {
        if (component.amplitude == 0.0 || component.turns_per_sample == 0.0) {
            continue;
        }
        // The integral of a*sin(2*pi*(f*t + phi)) is -a*cos(...)/(2*pi*f).
        total -= component.amplitude *
                 turns_to_cosine(component.turns_per_sample * t + component.phase_turns) /
                 (kTwoPi * component.turns_per_sample);
    }
    return total - integral_base_;
}

// ---------------------------------------------------------------------------
// The modulator
// ---------------------------------------------------------------------------

Expected<WfmModulator> WfmModulator::create(WfmSpec spec)
{
    // Qualified: the member function of the same name is in scope here and
    // hides the free one.
    Expected<SpectralExtent> extent = siggen::occupied_extent(spec);
    if (!extent) {
        return std::unexpected(extent.error());
    }

    Expected<FmComposite> composite = FmComposite::create(std::move(spec));
    if (!composite) {
        return std::unexpected(composite.error());
    }

    WfmModulator modulator(std::move(*composite));
    const WfmSpec& s = modulator.composite_.spec();
    modulator.extent_ = *extent;
    modulator.nominal_power_ = s.amplitude * s.amplitude;

    // The composite's integral is in samples of normalised composite, and the
    // instantaneous frequency is peak_deviation times the composite, so the
    // phase in radians is 2*pi*deviation/rate times the integral.
    modulator.phase_scale_ = kTwoPi * static_cast<double>(s.peak_deviation_hz) /
                             static_cast<double>(s.rate);
    return modulator;
}

void WfmModulator::render(SampleIndex start, ComplexSpan out) const
{
    const WfmSpec& s = composite_.spec();
    for (std::size_t i = 0; i < out.size(); ++i) {
        const SampleIndex index = start + i;

        // No anchored phasor walk here, unlike every mode in modulators.cpp.
        // The anchor exists there to replace a transcendental pair per sample
        // with a complex multiply, and this mode pays that pair anyway for
        // the FM phase, which does not factor into a product. exact_phase per
        // sample is exact at every index by construction, so it needs no
        // anchor to stay independent of how the caller blocked the call.
        const double phase = s.initial_phase +
                             exact_phase(s.carrier_offset, s.rate, index) +
                             phase_scale_ * composite_.integral(index);
        out[i] = Complex32(static_cast<float>(s.amplitude * std::cos(phase)),
                           static_cast<float>(s.amplitude * std::sin(phase)));
    }
}

void WfmModulator::accumulate(SampleIndex start, ComplexSpan out, double gain) const
{
    const WfmSpec& s = composite_.spec();
    const double level = gain * s.amplitude;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const SampleIndex index = start + i;
        const double phase = s.initial_phase +
                             exact_phase(s.carrier_offset, s.rate, index) +
                             phase_scale_ * composite_.integral(index);
        out[i] += Complex32(static_cast<float>(level * std::cos(phase)),
                            static_cast<float>(level * std::sin(phase)));
    }
}

// ---------------------------------------------------------------------------
// One shot
// ---------------------------------------------------------------------------

Expected<GeneratedStation> generate_wfm(const WfmSpec& spec, std::size_t sample_count)
{
    Expected<WfmModulator> modulator = WfmModulator::create(spec);
    if (!modulator) {
        return std::unexpected(modulator.error());
    }

    const std::size_t count =
        (sample_count > 0) ? sample_count : modulator->nominal_sample_count();

    GeneratedStation out;
    out.samples.assign(count, Complex32{});
    modulator->render(0, ComplexSpan(out.samples));

    out.composite.assign(count, 0.0F);
    modulator->composite().render(0, dsp::RealSpan(out.composite));

    std::vector<float> rds_only(count, 0.0F);
    modulator->composite().render_rds_only(0, dsp::RealSpan(rds_only));

    out.payload_bits = modulator->payload_bits();
    out.transmitted_bits = modulator->transmitted_bits();
    out.extent = modulator->occupied_extent();
    out.measured_mean_power = mean_power(ConstComplexSpan(out.samples));
    out.peak_magnitude = peak_magnitude(ConstComplexSpan(out.samples));
    out.composite_peak = real_peak(dsp::ConstRealSpan(out.composite));
    out.composite_mean_power = real_mean_power(dsp::ConstRealSpan(out.composite));
    out.rds_peak = real_peak(dsp::ConstRealSpan(rds_only));
    out.rds_mean_power = real_mean_power(dsp::ConstRealSpan(rds_only));
    out.over_deviated = out.composite_peak > 1.0;
    return out;
}

}  // namespace revenant::siggen
