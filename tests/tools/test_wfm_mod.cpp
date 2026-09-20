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
//   fourth order difference resolves the subcarrier. Blocking independence.
//   Constant envelope. The stereo matrix against a hand computation that
//   includes the 38 kHz subcarrier's coherence with the pilot under a clock
//   error. Mono against stereo carrying the same programme on both channels.
//   All three pre-emphasis curves, on the audio and NOT on the data. The
//   over-deviation report. Carson's rule. Every refusal validate() makes.
//
//   Covered in tests/decode/test_rds_bits.cpp instead, because it needs the
//   discriminator and the decoder: whether any of this comes back as bits.
//
//   Not covered anywhere yet, and listed so nobody reads the pair above as
//   complete. A station rendered into a wideband scene and then channelized
//   by the engine's own receiver, rather than discriminated directly by the
//   CPU twin. A programme that is audio rather than tones, which the
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
    // station and still decodes, because the RDS term contributes about
    // 0.02 radian of the carrier phase against the audio's several. Nothing
    // else in the suite would notice.
    //
    // So this differentiates the integral back and compares it against the
    // composite the same object renders. Fourth order central difference,
    // whose error is O(h^4) against the fifth derivative, so it has to run
    // at a rate where the 57 kHz subcarrier is oversampled properly: at the
    // station rate there are twelve samples per subcarrier cycle and the
    // difference is 0.3 percent out on its own. Sixteen times that is
    // 192 samples a cycle and the difference error drops below 1e-7.
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
    };

    const Shape shapes[] = {
        {"stereo, 50us", true, siggen::Preemphasis::Eu50, true, 0.0},
        {"stereo, 75us", true, siggen::Preemphasis::Us75, true, 0.0},
        {"stereo, flat", true, siggen::Preemphasis::None, true, 0.0},
        {"mono, no pilot", false, siggen::Preemphasis::None, false, 0.0},
        {"stereo, 40 ppm clock error", true, siggen::Preemphasis::Eu50, true, 40.0},
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

TEST_CASE("pre-emphasis lands on the audio and nowhere else", "[tools][wfm]")
{
    INFO(std::format("seed {}", kSeed));

    // WHAT THIS CASE IS FOR
    //
    // Running the curve over the finished composite instead of over L and R
    // is a one-line mistake that produces a working station. It boosts the
    // 57 kHz subcarrier by sqrt(1 + (2*pi*57000*50e-6)^2), which is 25 dB,
    // and the decoder downstream reports a clean eye and a high quality
    // figure because it has no idea what injection level it was promised.
    // Every sensitivity number measured against that signal would be 25 dB
    // optimistic and nothing would say so.
    //
    // So the data is compared sample for sample across the three curves,
    // with no tolerance, and the audio is compared against the gain the
    // curve is defined by.

    constexpr std::size_t kWindow = 8192;
    constexpr dsp::SampleIndex kFrom = 300000;

    const siggen::Preemphasis curves[] = {siggen::Preemphasis::None,
                                          siggen::Preemphasis::Us75,
                                          siggen::Preemphasis::Eu50};

    std::vector<float> reference_rds;
    for (const siggen::Preemphasis curve : curves) {
        INFO(siggen::preemphasis_name(curve));

        // Mono, one tone, so the audio channel is exactly one sinusoid and
        // its peak is the gain under test with nothing else in the way.
        siggen::WfmSpec spec = base_station();
        spec.programme.stereo = false;
        spec.programme.preemphasis = curve;
        spec.programme.left_tone_hz = 4000;
        spec.programme.audio_deviation_hz = 30000;
        spec.rds.pilot_enabled = true;

        auto composite = siggen::FmComposite::create(spec);
        REQUIRE(composite.has_value());

        const std::vector<float> rds = render_rds(*composite, kFrom, kWindow);
        if (reference_rds.empty()) {
            reference_rds = rds;
        } else {
            // Bit for bit. The curve must not reach the subcarrier at all,
            // and "close enough" is not the property: a 25 dB error and a
            // 0.1 dB error are the same bug caught at different tolerances.
            CHECK(rds == reference_rds);
        }

        const std::vector<float> audio = render_audio(*composite, kFrom, kWindow);
        double measured = 0.0;
        for (const float value : audio) {
            measured = std::max(measured, std::abs(static_cast<double>(value)));
        }

        const double gain = siggen::preemphasis_gain(curve, spec.programme.left_tone_hz);
        const double expected =
            gain * static_cast<double>(spec.programme.audio_deviation_hz) /
            static_cast<double>(siggen::kCompositePeakDeviationHz);

        INFO(std::format("gain {:.4f}, audio peak {:.6f} against {:.6f}", gain, measured,
                         expected));

        // The tolerance is the sample grid, not the arithmetic: a 4 kHz tone
        // at this rate has 171 samples a cycle, so the largest sample sits
        // below the true crest by about (2*pi/171)^2/2, which is 7e-4
        // relative.
        CHECK(measured == Approx(expected).epsilon(2e-3));
    }

    // And the figures themselves, so that a curve swapped between the two
    // regions is caught here rather than as a level that looks plausible.
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
    // at 10 kHz through the 50 us curve is a gain of 3.28, so the audio
    // alone asks for 197 kHz.
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
