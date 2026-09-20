// The broadcast FM station generator: the composite, the pre-emphasis curve
// and the FM phase.
//
// THE SHAPES THIS FILE HAS TO REACH, ENUMERATED BEFORE THE ASSERTIONS
//
// core/dsp/synth/wfm_mod.h can produce a station in any combination of:
// stereo or mono; three pre-emphasis curves; on centre or offset; with or
// without a transmitter clock error; at any deviation and any injection
// level inside EN 50067 clause 1.3's range. A bar that pinned one of those
// and read as though it covered the generator would be the failure this
// project keeps having, so here is what is covered and what is not.
//
//   Covered here. The FM phase against the composite, at a rate where a
//   fourth order difference resolves the subcarrier, including at clause
//   1.2's quadrature subcarrier phase and at a fractional start offset,
//   which are the two RdsModSpec fields the closed form rests on and that
//   nothing else reaches. Blocking independence.
//   Constant envelope. The stereo matrix against a hand computation that
//   includes the 38 kHz subcarrier's coherence with the pilot under a clock
//   error. Mono against stereo carrying the same programme on both channels.
//   All three pre-emphasis curves, as component levels measured in the
//   rendered composite: on the audio, and on neither subcarrier. The
//   over-deviation report. Carson's rule. Every refusal validate() makes.
//   A station placed in a wideband scene, which is dispatch and a truth row
//   rather than any new arithmetic.
//
//   Covered in tests/decode/test_rds_bits.cpp instead, because it needs the
//   discriminator and the decoder: whether any of this comes back as bits.
//
//   Not covered anywhere yet, and listed so nobody reads the pair above as
//   complete. A station channelized by the engine's own receiver, rather
//   than discriminated directly by the CPU twin: the scene case below
//   renders one and compares samples, and nothing runs the polyphase stage
//   over it. A programme that is audio rather than tones, which the
//   generator does not offer. Deviation other than 75 kHz, which rescales
//   every stated level and is exercised nowhere.
//
// Every random input is seeded from a printed constant, per
// docs/conventions.md.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numbers>
#include <string>
#include <vector>

#include "core/dsp/synth/modulators.h"
#include "core/dsp/synth/rds_mod.h"
#include "core/dsp/synth/wfm_mod.h"
#include "core/dsp/synth/wideband.h"
#include "core/dsp/types.h"

using namespace revenant;
using Catch::Approx;

namespace {

constexpr std::uint64_t kSeed = 20260920;

// Four times the 171000 the RDS decoder wants. Carson puts a 75 kHz station
// at 268750 Hz, so this leaves the aliased residue far below the data and is
// the rate the end-to-end case in tests/decode uses.
constexpr dsp::SampleRate kStationRate = 684000;

constexpr double kPi = std::numbers::pi;
constexpr double kTwoPi = 2.0 * kPi;

[[nodiscard]] siggen::WfmSpec base_station(std::size_t bit_count = 400)
{
    siggen::WfmSpec spec;
    spec.rate = kStationRate;
    spec.rds.bits = siggen::random_bits(bit_count, kSeed);
    spec.rds.rds_deviation_hz = 2000;
    return spec;
}

[[nodiscard]] std::vector<float> render_real(const siggen::FmComposite& composite,
                                             dsp::SampleIndex start, std::size_t count)
{
    std::vector<float> out(count, 0.0F);
    composite.render(start, dsp::RealSpan(out));
    return out;
}

[[nodiscard]] std::vector<float> render_rds(const siggen::FmComposite& composite,
                                            dsp::SampleIndex start, std::size_t count)
{
    std::vector<float> out(count, 0.0F);
    composite.render_rds_only(start, dsp::RealSpan(out));
    return out;
}

[[nodiscard]] std::vector<float> render_audio(const siggen::FmComposite& composite,
                                              dsp::SampleIndex start, std::size_t count)
{
    std::vector<float> out(count, 0.0F);
    composite.render_audio_only(start, dsp::RealSpan(out));
    return out;
}

[[nodiscard]] std::vector<float> render_pilot(const siggen::FmComposite& composite,
                                              dsp::SampleIndex start, std::size_t count)
{
    std::vector<float> out(count, 0.0F);
    composite.render_pilot_only(start, dsp::RealSpan(out));
    return out;
}

// The scalar multiple of `part` that best fits `whole`, least squares. For a
// component the composite is supposed to carry at unit scale this reads 1.0,
// and a generator that scaled that component reads back the scale it used.
//
// It is a measurement of the whole composite and needs no assumption about
// where a scaling mistake was made, which is the difference between it and
// subtracting the other components off first.
[[nodiscard]] double component_level(const std::vector<float>& whole,
                                     const std::vector<float>& part)
{
    double numerator = 0.0;
    double denominator = 0.0;
    for (std::size_t i = 0; i < part.size(); ++i) {
        numerator += static_cast<double>(whole[i]) * static_cast<double>(part[i]);
        denominator += static_cast<double>(part[i]) * static_cast<double>(part[i]);
    }
    return (denominator > 0.0) ? numerator / denominator : 0.0;
}

// Amplitude of a tone at hz, by a single DFT bin taken at the absolute
// sample indices the buffer holds.
//
// Rectangular window on purpose: the component-level case below runs this
// over a multiple of 684 samples at 684000, so the programme tone, the
// 19 kHz pilot and the 57 kHz subcarrier are all exact bins of it and there
// is nothing for a window function to suppress.
[[nodiscard]] double tone_amplitude(const std::vector<float>& signal,
                                    dsp::SampleIndex start, dsp::SampleRate rate,
                                    dsp::Hertz hz)
{
    const double turns_per_sample = static_cast<double>(hz) / static_cast<double>(rate);
    double real = 0.0;
    double imaginary = 0.0;
    for (std::size_t i = 0; i < signal.size(); ++i) {
        const double turns = turns_per_sample * static_cast<double>(start + i);
        const double angle = kTwoPi * (turns - std::floor(turns));
        real += static_cast<double>(signal[i]) * std::cos(angle);
        imaginary += static_cast<double>(signal[i]) * std::sin(angle);
    }
    return 2.0 * std::hypot(real, imaginary) / static_cast<double>(signal.size());
}

}  // namespace

TEST_CASE("the FM phase is the integral of the composite", "[tools][wfm]")
{
    INFO(std::format("seed {}", kSeed));

    // WHAT THIS CASE IS FOR
    //
    // FmComposite::integral() is a closed form, not a quadrature: the pilot,
    // the two programme tones and the four products that make up the 38 kHz
    // difference channel each integrate analytically, and the RDS term does
    // too, through the running integral RdsModulator tabulates. A
    // slip in any of those produces a station that still looks like a
    // station and still decodes, because the RDS term contributes an FM
    // modulation index of 2000/57000, which is 0.035 radian of carrier
    // phase, against the audio's 27.5 at this spec's 1 kHz tone and half of
    // its 52.5 kHz of deviation. Nothing else in the suite would notice.
    //
    // So this differentiates the integral back and compares it against the
    // composite the same object renders. Fourth order central difference,
    // whose error is O(h^4) against the fifth derivative, so it has to run
    // at a rate where the 57 kHz subcarrier is oversampled properly. At the
    // station rate there are exactly twelve samples per subcarrier cycle,
    // which puts the difference's own error term at (2*pi/12)^4/30, a
    // quarter of a percent; sixteen times that is 192 samples a cycle and
    // the same term falls by 16^4, which is 65536, to 3.8e-8. The figure
    // the run actually reaches is in the
    // INFO line below rather than asserted as a constant here, because it
    // is a property of the difference and not of the code under test.
    constexpr dsp::SampleRate kFineRate = 16 * kStationRate;
    constexpr std::size_t kWindow = 20000;

    // Far enough in that the payload is running, the shaping filter is fully
    // populated, and neither run-in nor run-out is in the window.
    constexpr dsp::SampleIndex kFrom = 400000;

    struct Shape {
        const char* name;
        bool stereo;
        siggen::Preemphasis preemphasis;
        bool pilot;
        double clock_error_ppm;
        double subcarrier_phase_radians;
        double start_offset_samples;
    };

    // The last two rows are the two RdsModSpec fields the closed form has to
    // carry and that nothing else here reaches.
    //
    // subcarrier_phase_radians is folded into RdsModulator's tabulated
    // anchor phase at construction AND read again per sample in
    // rds_integral(), so a version that applied it in one place and not the
    // other renders a correct composite and integrates a different signal.
    // Clause 1.2 permits quadrature, which is the far end of the field's
    // range and therefore the value that separates the two.
    //
    // start_offset_samples shifts the time base of the programme tones, the
    // pilot and the grid position rds_integral() reads, and the fraction is
    // deliberately not a round number of grid steps: 0.37 of a sample at
    // 512 steps per bit period lands between table entries, which is the
    // part-interval the closed form adds in closed form rather than
    // interpolating.
    const Shape shapes[] = {
        {"stereo, 50us", true, siggen::Preemphasis::Eu50, true, 0.0, 0.0, 0.0},
        {"stereo, 75us", true, siggen::Preemphasis::Us75, true, 0.0, 0.0, 0.0},
        {"stereo, flat", true, siggen::Preemphasis::None, true, 0.0, 0.0, 0.0},
        {"mono, no pilot", false, siggen::Preemphasis::None, false, 0.0, 0.0, 0.0},
        {"stereo, 40 ppm clock error", true, siggen::Preemphasis::Eu50, true, 40.0, 0.0,
         0.0},
        {"stereo, quadrature subcarrier", true, siggen::Preemphasis::Eu50, true, 0.0,
         kPi / 2.0, 0.0},
        {"stereo, 0.37 of a sample of start offset", true, siggen::Preemphasis::Eu50, true,
         0.0, 0.0, 0.37},
    };

    for (const Shape& shape : shapes) {
        INFO(shape.name);

        siggen::WfmSpec spec = base_station(200);
        spec.rate = kFineRate;
        spec.programme.stereo = shape.stereo;
        spec.programme.preemphasis = shape.preemphasis;
        spec.programme.left_tone_hz = 1000;
        spec.programme.right_tone_hz = shape.stereo ? 3300 : 1000;
        spec.rds.pilot_enabled = shape.pilot;
        spec.rds.clock_error_ppm = shape.clock_error_ppm;
        spec.rds.subcarrier_phase_radians = shape.subcarrier_phase_radians;
        spec.rds.start_offset_samples = shape.start_offset_samples;

        auto composite = siggen::FmComposite::create(spec);
        REQUIRE(composite.has_value());

        // Exactly zero at the origin is part of the contract, so that a
        // caller adding its own terms knows where the constant went.
        CHECK(composite->integral(0) == 0.0);

        const std::vector<float> rendered = render_real(*composite, kFrom, kWindow);

        double worst = 0.0;
        double peak = 0.0;
        for (std::size_t i = 2; i + 2 < kWindow; ++i) {
            const dsp::SampleIndex index = kFrom + i;
            const double derivative = (composite->integral(index - 2) -
                                       8.0 * composite->integral(index - 1) +
                                       8.0 * composite->integral(index + 1) -
                                       composite->integral(index + 2)) /
                                      12.0;
            const double value = static_cast<double>(rendered[i]);
            worst = std::max(worst, std::abs(derivative - value));
            peak = std::max(peak, std::abs(value));
        }

        INFO(std::format("worst |d(integral)/dn - composite| {:.3e} over a peak of {:.4f}",
                         worst, peak));
        REQUIRE(peak > 0.1);
        CHECK(worst < 1e-5);
    }
}

TEST_CASE("the curve moves the audio's level in the composite and not the subcarriers'",
          "[tools][wfm]")
{
    INFO(std::format("seed {}", kSeed));

    // WHAT THIS CASE IS FOR
    //
    // Running the curve over the finished composite instead of over L and R
    // is a one-line mistake that produces a working station. It lifts the
    // 57 kHz subcarrier by sqrt(1 + (2*pi*57000*tau)^2), 25.1 dB at 50 us
    // and 28.6 dB at 75 us, and the decoder downstream reports a clean eye
    // and a high quality figure because it has no idea what injection level
    // it was promised. Every sensitivity number measured against that signal
    // would be optimistic by that much and nothing would say so.
    //
    // WHAT CARRIES THE RISK, AND WHERE THE BAR THEREFORE GOES
    //
    // The quantity that moves under that mistake is the RDS subcarrier's
    // LEVEL IN THE COMPOSITE THAT REACHES THE MODULATOR, which is
    // FmComposite::render(). It is not render_rds_only(): that forwards
    // straight to RdsModulator, RdsModSpec has no pre-emphasis field, and
    // comparing it across curves is the same computation on the same inputs
    // three times.
    //
    // WHAT THIS CASE USED TO DO
    //
    // Until 2026-09-20 it did exactly that: render_rds_only() across the
    // three curves, compared with ==, under a comment saying the tolerance
    // was zero on purpose. It could not fail for any implementation this
    // design admits. Rewriting FmComposite::render as
    // out[i] = float((rds + audio) * preemphasis_gain(curve, 57000)) left it
    // passing bit for bit, and wfm_mod.h cited it as the guard against that
    // exact mistake.
    //
    // So the composite is projected onto each of its three components and
    // the coefficient is what gets asserted. A component the composite is
    // supposed to carry at unit scale reads 1.0; a generator that scaled it
    // reads back the scale. The data and the pilot must read 1.0 under every
    // curve, and the audio must move by the curve's own gain.

    // A multiple of 684, so 4 kHz, 19 kHz and 57 kHz are all exact bins and
    // the pilot and the programme tone cannot leak into each other's
    // measurement. Long enough, too, that the audio-against-data cross term
    // in the projection stays near 1e-4: the product of a 4 kHz tone and a
    // 57 kHz subcarrier has no DC component, so its running sum is bounded
    // and the mean of it falls as 1/N.
    constexpr std::size_t kWindow = 684 * 192;
    constexpr dsp::SampleIndex kFrom = 300000;

    // Enough bits that the payload is still running at the far end of the
    // window: 1200 bits is 1.01 s and the window ends at 0.63 s.
    constexpr std::size_t kBits = 1200;

    constexpr dsp::Hertz kTone = 4000;
    constexpr dsp::Hertz kAudioDeviation = 30000;

    const siggen::Preemphasis curves[] = {siggen::Preemphasis::None,
                                          siggen::Preemphasis::Us75,
                                          siggen::Preemphasis::Eu50};

    const double level = static_cast<double>(kAudioDeviation) /
                         static_cast<double>(siggen::kCompositePeakDeviationHz);

    for (const siggen::Preemphasis curve : curves) {
        INFO(siggen::preemphasis_name(curve));

        // Mono, one tone, so the audio channel is exactly one sinusoid and
        // the DFT bin below reads the gain under test with nothing else in
        // the way.
        siggen::WfmSpec spec = base_station(kBits);
        spec.programme.stereo = false;
        spec.programme.preemphasis = curve;
        spec.programme.left_tone_hz = kTone;
        spec.programme.audio_deviation_hz = kAudioDeviation;
        spec.rds.pilot_enabled = true;

        auto composite = siggen::FmComposite::create(spec);
        REQUIRE(composite.has_value());

        const std::vector<float> whole = render_real(*composite, kFrom, kWindow);
        const std::vector<float> rds = render_rds(*composite, kFrom, kWindow);
        const std::vector<float> pilot = render_pilot(*composite, kFrom, kWindow);

        // The component has to be present at all, or a projection onto a
        // buffer of zeros reads 0.0 and the assertion below would pass on an
        // empty signal.
        REQUIRE(component_level(rds, rds) == Approx(1.0).epsilon(1e-12));
        REQUIRE(component_level(pilot, pilot) == Approx(1.0).epsilon(1e-12));

        const double data_level = component_level(whole, rds);
        const double pilot_level = component_level(whole, pilot);
        const double audio_level =
            tone_amplitude(whole, kFrom, spec.rate, kTone) / level;

        const double gain = siggen::preemphasis_gain(curve, kTone);
        INFO(std::format("data x{:.6f}, pilot x{:.6f}, audio x{:.6f} against a curve gain "
                         "of {:.6f} at the tone and {:.4f} at 57 kHz",
                         data_level, pilot_level, audio_level, gain,
                         siggen::preemphasis_gain(curve, 57000)));

        // The two subcarriers, unscaled. The tolerance is the cross term
        // between the components, not the arithmetic: the smallest mistake
        // this is guarding against is a factor of 6.05, at the pilot under
        // 50 us, and the largest is 26.9 at the data under 75 us.
        CHECK(data_level == Approx(1.0).epsilon(5e-3));
        CHECK(pilot_level == Approx(1.0).epsilon(5e-3));

        // And the audio, which the curve is supposed to move. This is the
        // half that fails if the curve is not applied at all.
        CHECK(audio_level == Approx(gain).epsilon(1e-3));
    }

    // The figures the paragraph above is stated in, pinned, so that a curve
    // swapped between the two regions is caught here rather than as a level
    // that looks plausible.
    CHECK(siggen::preemphasis_seconds(siggen::Preemphasis::Us75) == Approx(75e-6));
    CHECK(siggen::preemphasis_seconds(siggen::Preemphasis::Eu50) == Approx(50e-6));
    CHECK(siggen::preemphasis_seconds(siggen::Preemphasis::None) == 0.0);
    CHECK(siggen::preemphasis_gain(siggen::Preemphasis::None, 15000) == 1.0);

    // 15 kHz against 50 us is 2*pi*15000*50e-6 = 4.712, so the gain is
    // sqrt(1 + 4.712^2) = 4.817, which is 13.7 dB. That is the headroom a
    // station running the curve has to give up at the top of the band, and
    // it is why audio_deviation_hz defaults below the 75 kHz cap.
    CHECK(siggen::preemphasis_gain(siggen::Preemphasis::Eu50, 15000) ==
          Approx(4.81733).epsilon(1e-5));

    // The size of the mistake the projections above are guarding against,
    // which is what wfm_mod.h states in decibels: 17.935 is 25.07 dB and
    // 26.879 is 28.59 dB.
    CHECK(siggen::preemphasis_gain(siggen::Preemphasis::Eu50, 57000) ==
          Approx(17.93498).epsilon(1e-6));
    CHECK(siggen::preemphasis_gain(siggen::Preemphasis::Us75, 57000) ==
          Approx(26.87923).epsilon(1e-6));
    CHECK(siggen::preemphasis_gain(siggen::Preemphasis::Eu50, 19000) ==
          Approx(6.05221).epsilon(1e-6));
}

TEST_CASE("the stereo matrix is the pilot's second harmonic, under a clock error",
          "[tools][wfm]")
{
    INFO(std::format("seed {}", kSeed));

    // WHAT THIS CASE IS FOR
    //
    // The 38 kHz difference subcarrier has to be the second harmonic of the
    // same pilot the 57 kHz RDS subcarrier is the third harmonic of. Taking
    // it from a nominal 38000 Hz instead works perfectly until
    // clock_error_ppm is non-zero, and then it is a stereo subcarrier
    // drifting against its own pilot, which a stereo decoder hears as
    // crosstalk that comes and goes. Nothing in the RDS path would notice.
    //
    // So the whole audio channel is recomputed here from the pilot phase
    // FmComposite was given, rather than from 38000, and compared against
    // what it rendered.

    constexpr std::size_t kWindow = 4096;
    constexpr dsp::SampleIndex kFrom = 250000;
    constexpr double kClockErrorPpm = 120.0;

    siggen::WfmSpec spec = base_station();
    spec.programme.stereo = true;
    spec.programme.left_tone_hz = 1000;
    spec.programme.right_tone_hz = 6000;
    spec.programme.preemphasis = siggen::Preemphasis::Eu50;
    spec.programme.audio_deviation_hz = 40000;
    spec.rds.clock_error_ppm = kClockErrorPpm;

    auto composite = siggen::FmComposite::create(spec);
    REQUIRE(composite.has_value());

    const std::vector<float> audio = render_audio(*composite, kFrom, kWindow);

    const auto rate = static_cast<double>(spec.rate);
    const double level = static_cast<double>(spec.programme.audio_deviation_hz) /
                         static_cast<double>(siggen::kCompositePeakDeviationHz);
    const double left_gain =
        siggen::preemphasis_gain(spec.programme.preemphasis, spec.programme.left_tone_hz);
    const double right_gain =
        siggen::preemphasis_gain(spec.programme.preemphasis, spec.programme.right_tone_hz);
    const double left_phase = siggen::preemphasis_phase_radians(
        spec.programme.preemphasis, spec.programme.left_tone_hz);
    const double right_phase = siggen::preemphasis_phase_radians(
        spec.programme.preemphasis, spec.programme.right_tone_hz);

    double worst = 0.0;
    for (std::size_t i = 0; i < kWindow; ++i) {
        const dsp::SampleIndex index = kFrom + i;
        const auto t = static_cast<double>(index);

        const double left =
            level * left_gain *
            std::sin(kTwoPi * static_cast<double>(spec.programme.left_tone_hz) * t / rate +
                     left_phase);
        const double right =
            level * right_gain *
            std::sin(kTwoPi * static_cast<double>(spec.programme.right_tone_hz) * t / rate +
                     right_phase);

        // Second harmonic of the pilot as the modulator actually has it,
        // clock error and all. A literal 38000 here would pass at zero ppm
        // and fail at 120, which is the point.
        const double pilot_turns = composite->rds().pilot_turns_at(index);
        const double stereo = std::sin(2.0 * kTwoPi * pilot_turns);

        const double expected = 0.5 * (left + right) + 0.5 * (left - right) * stereo;
        worst = std::max(worst, std::abs(expected - static_cast<double>(audio[i])));
    }

    INFO(std::format("worst audio disagreement {:.3e} at {} ppm", worst, kClockErrorPpm));
    CHECK(worst < 1e-6);

    // And the property that makes audio_deviation_hz mean one thing whatever
    // the two channels are doing: |(L+R)/2| + |(L-R)/2| is max(|L|, |R|), so
    // a stereo pair never peaks above a mono programme at the same level.
    double peak = 0.0;
    for (const float value : audio) {
        peak = std::max(peak, std::abs(static_cast<double>(value)));
    }
    CHECK(peak <= level * std::max(left_gain, right_gain) * 1.000001);
}

TEST_CASE("a stereo transmitter carrying the same programme both sides is the mono signal",
          "[tools][wfm]")
{
    INFO(std::format("seed {}", kSeed));

    // Most of the FM band is this: a stereo transmitter with a mono
    // programme. The difference channel is identically zero, so the
    // composite has to be the mono one plus the pilot, and a matrix with a
    // sign or a factor of two wrong shows up here as a level change rather
    // than as anything subtle.

    constexpr std::size_t kWindow = 4096;
    constexpr dsp::SampleIndex kFrom = 180000;

    siggen::WfmSpec stereo = base_station();
    stereo.programme.stereo = true;
    stereo.programme.left_tone_hz = 1000;
    stereo.programme.right_tone_hz = 1000;

    siggen::WfmSpec mono = stereo;
    mono.programme.stereo = false;

    auto stereo_composite = siggen::FmComposite::create(stereo);
    auto mono_composite = siggen::FmComposite::create(mono);
    REQUIRE(stereo_composite.has_value());
    REQUIRE(mono_composite.has_value());

    const std::vector<float> stereo_audio = render_audio(*stereo_composite, kFrom, kWindow);
    const std::vector<float> mono_audio = render_audio(*mono_composite, kFrom, kWindow);

    double worst = 0.0;
    double peak = 0.0;
    for (std::size_t i = 0; i < kWindow; ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(stereo_audio[i]) -
                                         static_cast<double>(mono_audio[i])));
        peak = std::max(peak, std::abs(static_cast<double>(mono_audio[i])));
    }

    INFO(std::format("worst {:.3e} over a peak of {:.4f}", worst, peak));
    REQUIRE(peak > 0.1);
    CHECK(worst < 1e-6);
}

TEST_CASE("the station renders the same samples from any start index", "[tools][wfm]")
{
    INFO(std::format("seed {}", kSeed));

    // Property 2 from modulators.h, which the wideband scene depends on: the
    // same absolute range has to produce the same samples whatever partition
    // the caller used to ask for it. An FM modulator is where that is most
    // easily lost, because the obvious way to write one accumulates the
    // phase forward from wherever the block started.

    constexpr std::size_t kCount = 30011;
    constexpr std::size_t kBlock = 997;

    siggen::WfmSpec spec = base_station();
    spec.carrier_offset = -31'000;
    spec.programme.stereo = true;
    spec.programme.right_tone_hz = 2500;

    auto modulator = siggen::WfmModulator::create(spec);
    REQUIRE(modulator.has_value());

    std::vector<dsp::Complex32> one(kCount);
    modulator->render(0, dsp::ComplexSpan(one));

    std::vector<dsp::Complex32> many(kCount);
    for (std::size_t at = 0; at < kCount; at += kBlock) {
        const std::size_t length = std::min(kBlock, kCount - at);
        modulator->render(at, dsp::ComplexSpan(many.data() + at, length));
    }

    CHECK(one == many);

    // Seeking, too: a block taken from the middle on its own has to match
    // the same range inside the whole render.
    constexpr std::size_t kSeekFrom = 20000;
    std::vector<dsp::Complex32> seeked(1024);
    modulator->render(kSeekFrom, dsp::ComplexSpan(seeked));
    for (std::size_t i = 0; i < seeked.size(); ++i) {
        CHECK(seeked[i] == one[kSeekFrom + i]);
    }
}

TEST_CASE("the station is constant envelope and reports its deviation", "[tools][wfm]")
{
    INFO(std::format("seed {}", kSeed));

    siggen::WfmSpec spec = base_station();
    spec.amplitude = 0.75;
    spec.programme.audio_deviation_hz = 40000;
    spec.programme.preemphasis = siggen::Preemphasis::None;

    auto station = siggen::generate_wfm(spec, 40000);
    REQUIRE(station.has_value());

    double worst = 0.0;
    for (const dsp::Complex32& sample : station->samples) {
        const double magnitude = std::hypot(static_cast<double>(sample.real()),
                                            static_cast<double>(sample.imag()));
        worst = std::max(worst, std::abs(magnitude - spec.amplitude));
    }
    INFO(std::format("worst envelope error {:.3e}", worst));
    CHECK(worst < 1e-6);
    CHECK(station->measured_mean_power == Approx(spec.amplitude * spec.amplitude).epsilon(1e-6));

    // 40000 of audio, 6750 of pilot and 2000 of RDS is 48750 out of 75000,
    // so this one is inside the cap and the report has to say so.
    CHECK_FALSE(station->over_deviated);
    CHECK(station->composite_peak < 1.0);

    // The RDS component is at the injection level clause 1.3 was asked for,
    // whatever else is on the multiplex. rds_peak reads below the envelope
    // peak because the sample grid misses the subcarrier's crest, which
    // rds_mod.h explains at length; the bound is what is checked.
    const double injection = static_cast<double>(spec.rds.rds_deviation_hz) /
                             static_cast<double>(siggen::kCompositePeakDeviationHz);
    CHECK(station->rds_peak <= injection * 1.000001);
    CHECK(station->rds_peak > 0.85 * injection);
}

TEST_CASE("over deviation is reported rather than clamped", "[tools][wfm]")
{
    INFO(std::format("seed {}", kSeed));

    // Reachable on purpose, for the same reason AmParams::modulation_index
    // above 1.0 is: a receiver that cannot cope with an over-deviating
    // station should fail a test rather than never meet one. 60 kHz of audio
    // at 10 kHz through the 50 us curve is a gain of sqrt(1 + pi^2), which
    // is 3.297, so the audio alone asks for 198 kHz.
    siggen::WfmSpec spec = base_station();
    spec.programme.stereo = false;
    spec.programme.left_tone_hz = 10000;
    spec.programme.audio_deviation_hz = 60000;
    spec.programme.preemphasis = siggen::Preemphasis::Eu50;

    auto composite = siggen::FmComposite::create(spec);
    REQUIRE(composite.has_value());
    CHECK(composite->peak_bound() > 1.0);

    auto station = siggen::generate_wfm(spec, 20000);
    REQUIRE(station.has_value());
    INFO(std::format("composite peak {:.4f}, bound {:.4f}", station->composite_peak,
                     composite->peak_bound()));
    CHECK(station->over_deviated);

    // The bound is a sum of the three parts' own peaks, so it is an upper
    // bound on what any buffer can show and never a prediction of it.
    CHECK(station->composite_peak <= composite->peak_bound() * 1.000001);
}

TEST_CASE("the occupied band is Carson against the top of the composite", "[tools][wfm]")
{
    siggen::WfmSpec spec = base_station();
    spec.carrier_offset = 50'000;

    auto extent = siggen::occupied_extent(spec);
    REQUIRE(extent.has_value());

    // 2 * (75000 + 57000 + 2375) = 268750.
    CHECK(extent->bandwidth_hz() == 268'750);
    CHECK(extent->center_hz() == 50'000);
    CHECK(extent->low_hz == 50'000 - 134'375);
    CHECK(extent->high_hz == 50'000 + 134'375);
}

TEST_CASE("the station refuses what it cannot render", "[tools][wfm]")
{
    INFO(std::format("seed {}", kSeed));

    SECTION("a rate that folds the signal onto itself") {
        siggen::WfmSpec spec = base_station();
        spec.rate = 171000;
        const Status checked = siggen::validate(spec);
        REQUIRE_FALSE(checked.has_value());
        INFO(checked.error().message);
        CHECK(checked.error().message.find("Carson") != std::string::npos);
    }

    SECTION("an offset that pushes the skirt past Nyquist") {
        siggen::WfmSpec spec = base_station();
        spec.carrier_offset = 250'000;
        const Status checked = siggen::validate(spec);
        REQUIRE_FALSE(checked.has_value());
        INFO(checked.error().message);
        CHECK(checked.error().message.find("Nyquist") != std::string::npos);
    }

    SECTION("stereo without a pilot") {
        siggen::WfmSpec spec = base_station();
        spec.programme.stereo = true;
        spec.rds.pilot_enabled = false;
        const Status checked = siggen::validate(spec);
        REQUIRE_FALSE(checked.has_value());
        INFO(checked.error().message);
        CHECK(checked.error().message.find("pilot") != std::string::npos);
    }

    SECTION("mono without a pilot is legal") {
        siggen::WfmSpec spec = base_station();
        spec.programme.stereo = false;
        spec.rds.pilot_enabled = false;
        CHECK(siggen::validate(spec).has_value());
    }

    SECTION("a programme tone outside the audio band") {
        siggen::WfmSpec spec = base_station();
        spec.programme.left_tone_hz = 17'000;
        const Status checked = siggen::validate(spec);
        REQUIRE_FALSE(checked.has_value());
        INFO(checked.error().message);
        CHECK(checked.error().message.find("audio band") != std::string::npos);
    }

    SECTION("the RDS spec's own mono tone") {
        siggen::WfmSpec spec = base_station();
        spec.rds.mono_tone_hz = 1000;
        spec.rds.mono_deviation_hz = 20000;
        const Status checked = siggen::validate(spec);
        REQUIRE_FALSE(checked.has_value());
        INFO(checked.error().message);
        CHECK(checked.error().message.find("pre-emphasis") != std::string::npos);
    }

    SECTION("a subcarrier offset, which the closed-form integral cannot carry") {
        siggen::WfmSpec spec = base_station();
        spec.rds.subcarrier_offset_hz = 30;
        const Status checked = siggen::validate(spec);
        REQUIRE_FALSE(checked.has_value());
        INFO(checked.error().message);
        CHECK(checked.error().message.find("48 times the bit rate") != std::string::npos);

        // And the modulator that depends on it refuses too, rather than
        // rendering NaN.
        CHECK_FALSE(siggen::WfmModulator::create(spec).has_value());

        // RdsModulator itself still accepts the field: it is the FM phase
        // that cannot carry it, not the composite. A reader who found the
        // refusal above and concluded the field was dead would be wrong.
        siggen::RdsModSpec rds = spec.rds;
        rds.rate = spec.rate;
        auto bare = siggen::RdsModulator::create(rds);
        REQUIRE(bare.has_value());
        CHECK_FALSE(bare->supports_integral());
    }

    SECTION("no bits at all") {
        siggen::WfmSpec spec = base_station();
        spec.rds.bits.clear();
        CHECK_FALSE(siggen::validate(spec).has_value());
    }
}

TEST_CASE("a wideband scene can carry a broadcast FM station", "[tools][wfm]")
{
    INFO(std::format("seed {}", kSeed));

    // WHAT THIS CASE IS FOR
    //
    // The scene is where a station has to arrive for the engine to see one,
    // and the wiring has two things that can be silently wrong: which
    // generator a burst dispatches to, and what the truth table says the
    // burst was. The second is the one that would not show up as a broken
    // signal. EmitterTruth carries a Modulation field that means nothing on
    // a station's row, and a scorer that read it would score a 268 kHz
    // broadcast station as a keyed carrier, so the row has to say which kind
    // it is and the CSV has to leave that cell empty.

    siggen::WfmSpec station = base_station(300);
    station.carrier_offset = 0;

    siggen::SceneSpec scene_spec;
    scene_spec.rate = kStationRate;
    scene_spec.duration_samples = 40000;
    scene_spec.add_noise = false;

    siggen::WfmStationPlacement placement;
    placement.station = station;
    placement.use_snr = false;
    placement.end_sample = scene_spec.duration_samples;
    scene_spec.fm_stations.push_back(placement);

    auto scene = siggen::Scene::create(scene_spec);
    REQUIRE(scene.has_value());

    REQUIRE(scene->truth().size() == 1);
    const siggen::EmitterTruth& truth = scene->truth().front();
    CHECK(truth.kind == siggen::EmitterKind::BroadcastFm);
    CHECK(truth.extent.bandwidth_hz() == 268'750);
    CHECK(truth.carrier_offset_hz == 0);
    CHECK(truth.start_sample == 0);
    CHECK(truth.end_sample == scene_spec.duration_samples);
    CHECK(truth.symbol_rate_baud == 0.0);

    // The row names the generator, and the modulation cell is empty rather
    // than carrying the enum's default.
    const std::string csv = siggen::truth_csv(*scene);
    INFO(csv);
    CHECK(csv.find("id,kind,modulation,") == 0);
    CHECK(csv.find("\n0,wfm,,0,") != std::string::npos);

    // With no noise and a unit gain, the scene is the station and nothing
    // else, sample for sample.
    auto modulator = siggen::WfmModulator::create(station);
    REQUIRE(modulator.has_value());

    constexpr std::size_t kWindow = 8192;
    constexpr dsp::SampleIndex kFrom = 12000;
    std::vector<dsp::Complex32> from_scene(kWindow);
    std::vector<dsp::Complex32> from_modulator(kWindow);
    scene->render(kFrom, dsp::ComplexSpan(from_scene));
    modulator->render(kFrom, dsp::ComplexSpan(from_modulator));
    CHECK(from_scene == from_modulator);

    // And blocking independence survives the scene's own worker split.
    std::vector<dsp::Complex32> blocked(kWindow);
    for (std::size_t at = 0; at < kWindow; at += 701) {
        const std::size_t length = std::min<std::size_t>(701, kWindow - at);
        scene->render(kFrom + at, dsp::ComplexSpan(blocked.data() + at, length));
    }
    CHECK(blocked == from_scene);
}

TEST_CASE("a scene places a station by SNR, which is what everything asks for",
          "[tools][wfm]")
{
    INFO(std::format("seed {}", kSeed));

    // WHAT THIS CASE IS FOR
    //
    // WfmStationPlacement::use_snr defaults to true and siggen sets it true
    // on every station it places, so the SNR path is the one the tool
    // actually runs. The only scene case there was set it false, which took
    // the branch that copies WfmSpec::amplitude through untouched, so
    // nothing had ever exercised the arithmetic that turns a requested
    // ratio into a gain on a station.
    //
    // That arithmetic has two places to be wrong in a way no signal shows:
    // the noise in the emitter's own band, which is the full-band figure
    // scaled by the station's 268750 Hz out of the scene's rate, and the
    // conversion from a power ratio to an amplitude gain. Both produce a
    // station at the wrong level with the truth row agreeing, because the
    // row is computed from the same numbers.
    //
    // So the gain is measured off the rendered scene instead. The scene is
    // the station at some gain plus noise that does not correlate with it,
    // so projecting the scene onto the station the placement was built from
    // recovers that gain directly.

    CHECK(siggen::WfmStationPlacement{}.use_snr);

    constexpr double kRequestedSnrDb = 20.0;
    constexpr double kNoiseFullBandDbfs = -60.0;
    constexpr dsp::SampleRate kSceneRate = 1'000'000;
    constexpr std::size_t kWindow = 40000;

    siggen::WfmSpec station = base_station(300);
    station.rate = kSceneRate;

    siggen::SceneSpec scene_spec;
    scene_spec.rate = kSceneRate;
    scene_spec.duration_samples = kWindow;
    scene_spec.add_noise = true;
    scene_spec.noise_power_full_band_dbfs = kNoiseFullBandDbfs;
    scene_spec.seed = kSeed;

    siggen::WfmStationPlacement placement;
    placement.station = station;
    placement.snr_in_occupied_bandwidth_db = kRequestedSnrDb;
    placement.end_sample = scene_spec.duration_samples;
    scene_spec.fm_stations.push_back(placement);

    auto scene = siggen::Scene::create(scene_spec);
    REQUIRE(scene.has_value());
    REQUIRE(scene->truth().size() == 1);
    const siggen::EmitterTruth& truth = scene->truth().front();

    // Stated as the arithmetic. Total noise power 1e-6 across 1 MHz, of
    // which the station's 268750 Hz holds 2.6875e-7; 20 dB above that is
    // 2.6875e-5 of station power, and the station's nominal power is
    // amplitude squared, which is 1, so the gain is its square root.
    constexpr double kNoiseInBand = 1e-6 * 268750.0 / 1'000'000.0;
    const double expected_power = kNoiseInBand * 100.0;
    const double expected_gain = std::sqrt(expected_power);

    CHECK(scene->noise_power_full_band() == Approx(1e-6).epsilon(1e-12));
    CHECK(truth.mean_power == Approx(expected_power).epsilon(1e-9));
    CHECK(truth.snr_in_occupied_bandwidth_db == Approx(kRequestedSnrDb).epsilon(1e-9));

    // 2.6875e-5 over 1e-6 is 14.29 dB across the whole rate, which is what a
    // measurement over the raw buffer reads and is 5.7 dB below the in-band
    // figure because the station fills 27 percent of the scene.
    CHECK(truth.snr_in_full_band_db == Approx(14.2935).epsilon(1e-4));

    auto modulator = siggen::WfmModulator::create(station);
    REQUIRE(modulator.has_value());
    CHECK(modulator->nominal_mean_power() == Approx(1.0).epsilon(1e-12));

    std::vector<dsp::Complex32> from_scene(kWindow);
    std::vector<dsp::Complex32> bare(kWindow);
    scene->render(0, dsp::ComplexSpan(from_scene));
    modulator->render(0, dsp::ComplexSpan(bare));

    double numerator = 0.0;
    double denominator = 0.0;
    for (std::size_t i = 0; i < kWindow; ++i) {
        numerator += static_cast<double>(from_scene[i].real()) *
                         static_cast<double>(bare[i].real()) +
                     static_cast<double>(from_scene[i].imag()) *
                         static_cast<double>(bare[i].imag());
        denominator += static_cast<double>(bare[i].real()) *
                           static_cast<double>(bare[i].real()) +
                       static_cast<double>(bare[i].imag()) *
                           static_cast<double>(bare[i].imag());
    }
    const double measured_gain = numerator / denominator;

    INFO(std::format("measured gain {:.9f} against {:.9f}", measured_gain, expected_gain));

    // The noise is uncorrelated with the station, so it does not bias this
    // estimate, but it does scatter it: one standard deviation is
    // sqrt(N * noise_power / 2) over N, which is 3.5e-6 against a gain of
    // 5.18e-3, so 6.8e-4 relative. This run, at the seed printed above,
    // reads 1.0e-3 low, and the tolerance is set well clear of that rather
    // than on it. What it is guarding against is a factor of 193: taking
    // the power ratio as a gain instead of its square root.
    CHECK(measured_gain == Approx(expected_gain).epsilon(5e-3));

    // And a station placed by SNR into a scene with no noise is refused
    // rather than silently placed at zero, which is the failure the default
    // would otherwise cause in a scene somebody turned the noise off in.
    siggen::SceneSpec quiet = scene_spec;
    quiet.add_noise = false;
    auto refused = siggen::Scene::create(quiet);
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);
    CHECK(refused.error().message.find("noise floor") != std::string::npos);
}

TEST_CASE("a scene's truth row carries the station's deviation bound", "[tools][wfm]")
{
    INFO(std::format("seed {}", kSeed));

    // WHAT THIS CASE IS FOR
    //
    // generate_wfm() measures over-deviation on its own buffer and the
    // siggen wfm report prints it. A scene had neither, so a station placed
    // in one could deviate past WfmSpec::peak_deviation_hz in silence while
    // its truth row carried a Carson extent computed from the deviation it
    // was exceeding and a scorer read that as the occupied band.
    //
    // A scene cannot measure: it never renders a station on its own. What
    // it can carry is FmComposite::peak_bound(), which is a property of the
    // spec, and that is what the row and the CSV hold.

    siggen::WfmSpec legal = base_station(300);
    legal.carrier_offset = -120'000;

    // Mono at 10 kHz, 60 kHz of audio through the 50 us curve: a gain of
    // 3.297, so the audio alone asks for 2.64 of full deviation.
    siggen::WfmSpec hot = base_station(300);
    hot.carrier_offset = 120'000;
    hot.programme.stereo = false;
    hot.programme.left_tone_hz = 10000;
    hot.programme.audio_deviation_hz = 60000;
    hot.programme.preemphasis = siggen::Preemphasis::Eu50;

    siggen::SceneSpec scene_spec;
    scene_spec.rate = 1'000'000;
    scene_spec.duration_samples = 20000;
    scene_spec.add_noise = false;

    // One ordinary emitter too, so the column can be seen to be empty on a
    // row where a deviation bound means nothing.
    siggen::EmitterPlacement carrier;
    carrier.modulator.kind = siggen::Modulation::Cw;
    carrier.modulator.common.carrier_offset = 300'000;
    carrier.modulator.common.rate = scene_spec.rate;
    carrier.modulator.common.seed = kSeed;
    carrier.use_snr = false;
    carrier.end_sample = scene_spec.duration_samples;
    scene_spec.emitters.push_back(carrier);

    for (const siggen::WfmSpec& station : {legal, hot}) {
        siggen::WfmStationPlacement placement;
        placement.station = station;
        placement.use_snr = false;
        placement.end_sample = scene_spec.duration_samples;
        scene_spec.fm_stations.push_back(placement);
    }

    auto scene = siggen::Scene::create(scene_spec);
    REQUIRE(scene.has_value());
    REQUIRE(scene->truth().size() == 3);

    const auto row_at = [&scene](dsp::Hertz offset) -> const siggen::EmitterTruth* {
        for (const siggen::EmitterTruth& record : scene->truth()) {
            if (record.carrier_offset_hz == offset) {
                return &record;
            }
        }
        return nullptr;
    };

    // 9 percent of pilot, 70 percent of audio through a 1.0482 gain at
    // 1 kHz, and 2.67 percent of RDS envelope: 0.85040. Stated as the
    // arithmetic rather than read back off peak_bound(), so the row is
    // checked against the spec and not against the same call that filled
    // it.
    const siggen::EmitterTruth* legal_row = row_at(-120'000);
    REQUIRE(legal_row != nullptr);
    INFO(std::format("legal bound {:.6f}", legal_row->composite_peak_bound));
    CHECK(legal_row->composite_peak_bound == Approx(0.85040).epsilon(1e-4));
    CHECK(legal_row->composite_peak_bound < 1.0);

    // 0.09 + 0.8 * 3.29691 + 0.026667 = 2.75422.
    const siggen::EmitterTruth* hot_row = row_at(120'000);
    REQUIRE(hot_row != nullptr);
    INFO(std::format("hot bound {:.6f}", hot_row->composite_peak_bound));
    CHECK(hot_row->composite_peak_bound == Approx(2.75422).epsilon(1e-4));
    CHECK(hot_row->composite_peak_bound > 1.0);

    // And the extent it is being read against is still Carson at the
    // nominal deviation, which is the whole reason the bound has to be on
    // the row: 268750 Hz says nothing about a station reaching 2.75 times
    // the deviation that figure was computed from.
    CHECK(hot_row->extent.bandwidth_hz() == 268'750);

    // Meaningless on a Modulated row and left at zero there.
    const siggen::EmitterTruth* carrier_row = row_at(300'000);
    REQUIRE(carrier_row != nullptr);
    CHECK(carrier_row->composite_peak_bound == 0.0);

    const std::string csv = siggen::truth_csv(*scene);
    INFO(csv);
    CHECK(csv.find(",payload_seed,composite_peak_bound\n") != std::string::npos);
    CHECK(csv.find(",0.8504\n") != std::string::npos);
    CHECK(csv.find(",2.7542\n") != std::string::npos);

    // The Cw row's cell is empty rather than 0.0000, which would read as a
    // station that never deviates at all.
    CHECK(csv.find(",cw,300000,") != std::string::npos);
    CHECK(csv.find(",0.0000\n") == std::string::npos);

    // The bound is an upper bound and a render has to stay under it, or the
    // row is telling a scorer something a buffer can contradict.
    auto rendered = siggen::generate_wfm(hot, 20000);
    REQUIRE(rendered.has_value());
    INFO(std::format("hot composite peak {:.6f}", rendered->composite_peak));
    CHECK(rendered->over_deviated);
    CHECK(rendered->composite_peak <= hot_row->composite_peak_bound * 1.000001);
}

TEST_CASE("the pre-emphasis names round trip", "[tools][wfm]")
{
    const siggen::Preemphasis curves[] = {siggen::Preemphasis::None,
                                          siggen::Preemphasis::Us75,
                                          siggen::Preemphasis::Eu50};
    for (const siggen::Preemphasis curve : curves) {
        auto parsed = siggen::preemphasis_from_name(siggen::preemphasis_name(curve));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == curve);
    }
    CHECK_FALSE(siggen::preemphasis_from_name("25us").has_value());
}
