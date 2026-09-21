// The front end monitor, against arithmetic and against a nonlinear radio.
//
// Two instruments, for two different questions, on the split
// tests/detect/CMakeLists.txt already describes.
//
// The first half builds the two arrays by hand. A slope is a regression over
// two numbers per decision and a case can state both, so those cases assert
// against arithmetic: a floor that moves three decibels for every one the
// drive moves has a slope of three, and nothing is measured.
//
// The second half runs a real scene through a memoryless cubic and then
// through the channelizer and spectrum twins, which tests/reference proves
// bit-exact against the kernels. That is the only way to ask whether the
// thing actually fires on intermodulation rather than on an array somebody
// wrote to make it fire, and it is what makes the on-air failure of
// 2026-09-20 reproducible without a radio.
//
// WHAT THE SCENE CASES ARE FOR, STATED SO THEY ARE NOT MISREAD. They prove
// the monitor separates a nonlinearity from a gain change and from a busy
// band. They do not measure a dongle. core/detect/front_end.h lists what the
// measurement cannot tell apart and none of that is testable here.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "core/detect/detector.h"
#include "core/detect/front_end.h"
#include "core/dsp/synth/modulators.h"
#include "core/dsp/synth/wideband.h"
#include "scene_frames.h"

using namespace revenant;

namespace {

// ---------------------------------------------------------------------------
// The arithmetic half
// ---------------------------------------------------------------------------

constexpr std::size_t kBins = 1024;

// Far enough apart that the regression has leverage at every step and short
// enough that a case runs instantly.
constexpr double kStep = 0.1;

[[nodiscard]] double from_db(double db)
{
    return std::pow(10.0, db / 10.0);
}

// One decision, built from a floor level and a peak level in dBFS.
//
// The floor array is flat and the peak is one bin, which is the simplest
// arrangement that exercises every segment: the monitor takes a mean of the
// floor per segment and a maximum of the power over the whole span, so a flat
// floor puts the same number in all sixteen segments and one loud bin sets
// the drive.
struct Decision {
    std::vector<double> power;
    std::vector<double> floor;
};

[[nodiscard]] Decision decision_at(double floor_db, double peak_db,
                                   std::span<const double> segment_offsets_db = {})
{
    Decision out;
    out.power.assign(kBins, from_db(floor_db));
    out.floor.assign(kBins, from_db(floor_db));

    // Per-segment offsets, when a case wants one part of the span to behave
    // differently from the rest. That is the busy-band shape: a station
    // occupying one segment raises its floor and nobody else's.
    for (std::size_t s = 0; s < segment_offsets_db.size(); ++s) {
        const std::size_t first = s * kBins / detect::kFrontEndSegments;
        const std::size_t last = (s + 1) * kBins / detect::kFrontEndSegments;
        for (std::size_t i = first; i < last && i < kBins; ++i) {
            out.floor[i] = from_db(floor_db + segment_offsets_db[s]);
            out.power[i] = out.floor[i];
        }
    }

    out.power[kBins / 2] = from_db(peak_db);
    return out;
}

// Drives the monitor along a sinusoidal excursion of the drive level, with
// the floor following it at the given slope. Returns what it last said.
//
// A sinusoid and not a ramp, because an exponentially weighted window down a
// monotone ramp is fitting a line to a moving mean and the answer drifts. A
// closed excursion is what a fade or an AGC hunt actually looks like.
[[nodiscard]] detect::FrontEndObservation sweep(detect::FrontEndMonitor& monitor,
                                                double slope, double swing_db,
                                                std::size_t decisions,
                                                double period_decisions = 40.0,
                                                std::span<const double> segment_slopes = {})
{
    constexpr double kBaseFloorDb = -90.0;
    constexpr double kBaseDriveDb = -20.0;

    for (std::size_t d = 0; d < decisions; ++d) {
        const double turns = static_cast<double>(d) / period_decisions;
        const double excursion =
            0.5 * swing_db * std::sin(2.0 * std::numbers::pi * turns);

        std::vector<double> offsets;
        if (!segment_slopes.empty()) {
            offsets.reserve(segment_slopes.size());
            for (const double per_segment : segment_slopes) {
                offsets.push_back((per_segment - slope) * excursion);
            }
        }

        const Decision point = decision_at(kBaseFloorDb + slope * excursion,
                                           kBaseDriveDb + excursion, offsets);
        const Status fed = monitor.observe(point.power, point.floor, kStep);
        REQUIRE(fed.has_value());
    }
    return monitor.observation();
}

// ---------------------------------------------------------------------------
// The scene half
// ---------------------------------------------------------------------------

// 600 kS/s over 64 channels with a 512-point second stage is 16384 bins at
// 36.6 Hz, which is the SHIPPED bin width at a quarter of the shipped span.
//
// The bin width is what matters and the span is what costs: the host
// channelizer twin's work is per input sample, so twenty seconds here costs
// a quarter of what twenty seconds at 2.4 MS/s would.
//
// 16384 bins over kFrontEndSegments is 1024 bins a segment, which is 37.5 kHz
// here.
constexpr dsp::SampleRate kSceneRate = 600'000;
constexpr std::uint32_t kSceneChannels = 64;
constexpr std::uint32_t kSceneTransform = 512;

// The cubic coefficient every nonlinear case uses.
//
// Bounded above by the model folding over. y = x - a3 |x|^2 x turns the
// transfer curve back on itself once a3 |x|^2 passes one, which is not
// compression any more, it is a different function. Both scenes below run a
// composite whose standard deviation is near 0.45 with peaks around four of
// those, so a3 |x|^2 reaches about 0.5 at the loudest point: hard
// compression on the peaks, which is the condition being reproduced, and
// clear of the fold.
constexpr double kThirdOrder = 0.15;

// ---- the two-tone scene, which is about the discrete products ------------

// Two strong tones, and the third-order products they make. 2*f1 - f2 and
// 2*f2 - f1 is the two-tone test. Both products land inside the span and
// both are 40 kHz clear of either tone.
constexpr dsp::Hertz kToneOne = -180'000;
constexpr dsp::Hertz kToneTwo = -140'000;
constexpr dsp::Hertz kProductLow = 2 * kToneOne - kToneTwo;   // -220 kHz
constexpr dsp::Hertz kProductHigh = 2 * kToneTwo - kToneOne;  // -100 kHz

constexpr double kToneAmplitude = 0.25;
constexpr double kToneSceneSeconds = 4.0;

// THE NOISE FLOOR IN THIS SCENE IS SET BY THE ANALYSIS WINDOW AND NOT BY
// TASTE, and getting it wrong produced a scene where the LINEAR control
// found nineteen tracks.
//
// core/dsp/spectrum_reference.h uses a four-term Blackman-Harris window
// whose sidelobes are 92 dB down. A tone concentrated in one bin at 80 dB
// over the floor therefore leaks across the whole span at 12 dB under the
// floor and is invisible; the same tone at 100 dB over leaks at 8 dB OVER
// the floor and the detector, correctly, reports a track everywhere. That is
// the window and not the front end, so a scene that reaches it is measuring
// the wrong thing. Measured 2026-09-21: at -70 dBFS full band these tones sit
// 100 dB over the floor per bin and the linear control published nineteen
// tracks; at -45 dBFS they sit 75 dB over and it publishes the two tones.
constexpr double kToneSceneNoiseDbfs = -45.0;

[[nodiscard]] siggen::ModulatorSpec tone_at(dsp::Hertz offset, double amplitude)
{
    // AM at a modulation index of zero is an unmodulated carrier, which is
    // the only steady tone in the palette: Modulation::Cw is keyed, and a
    // keyed tone would move the drive for a reason that has nothing to do
    // with the front end.
    siggen::ModulatorSpec spec;
    spec.kind = siggen::Modulation::Am;
    spec.common.rate = kSceneRate;
    spec.common.carrier_offset = offset;
    spec.common.amplitude = amplitude;
    spec.am.modulation_index = 0.0;
    return spec;
}

[[nodiscard]] siggen::SceneSpec two_tone_scene()
{
    siggen::SceneSpec spec;
    spec.rate = kSceneRate;
    spec.center_hz = 98'100'000;
    spec.duration_samples = static_cast<dsp::SampleIndex>(
        std::llround(kToneSceneSeconds * static_cast<double>(kSceneRate)));
    spec.seed = 4242;
    spec.noise_power_full_band_dbfs = kToneSceneNoiseDbfs;

    for (const dsp::Hertz offset : {kToneOne, kToneTwo}) {
        siggen::EmitterPlacement placement;
        placement.modulator = tone_at(offset, kToneAmplitude);
        placement.use_snr = false;
        placement.start_sample = 0;
        placement.end_sample = spec.duration_samples;
        spec.emitters.push_back(placement);
    }
    return spec;
}

// ---- the dense scene, which is about the floor ---------------------------

// Eight noise-like stations across the span, and both halves of that
// sentence are forced.
//
// NOISE-LIKE, because a tone's third-order products are two more tones and
// the monitor measures a floor rather than a track list. A wideband signal's
// products with itself and with its neighbours are noise shaped and fill
// three times the bandwidth they came from, which is what a floor lift IS.
//
// EIGHT AND SPREAD OUT, because the products have to reach every segment.
// The weakest segment carries the verdict, so one segment left at a thermal
// floor reads, correctly, as a gain change.
//
// NARROWER THAN THE FLOOR ESTIMATOR'S WINDOW, which is the constraint that
// decides the width. detector.h sets noise_window_knots so each estimate
// spans a quarter of the span, 150 kHz here, and says plainly that a signal
// filling most of one window becomes its own floor. A first draft of this
// scene used one 162 kHz station: every window inside it measured the
// station, those segments' floors moved one for one with the gain, and the
// weakest-segment rule reported SpanScales at a slope of 1.002 on a front
// end in hard compression. Thirty kilohertz is a fifth of a window.
constexpr double kStationAmplitude = 0.16;
constexpr double kStationSymbolRate = 22'000.0;
constexpr double kDenseSceneSeconds = 20.0;

// Low, because the products have to dominate it for the floor to move with
// the signal at all.
//
// WHAT THIS ACTUALLY BUYS, MEASURED 2026-09-21 rather than predicted. The
// three scene cases below report, at the end of a twenty second run:
//
//   cubic and gain swing   slope 2.13, correlation 0.97, spread 1.65 dB
//   gain swing alone       slope 0.99, correlation 1.00, spread 1.74 dB
//   keyed station alone    slope 0.01, correlation 0.70, spread 2.36 dB
//
// The middle row is a multiplication and lands on one, exactly as it should.
// The top row is 2.13 and NOT the three a pure third-order product predicts,
// because the measured floor is a mixture: where the products are strong it
// rises three for one and where they are weak the thermal noise underneath
// still rises one for one, and the weakest segment carries the verdict. That
// is the honest number for a real front end too, and it is why
// kFrontEndNonlinearEnter is 1.5 rather than something nearer three.
//
// The tone scene's leakage problem does not arise here, because a 30 kHz
// station's strongest bin is far under a tone's.
constexpr double kDenseSceneNoiseDbfs = -70.0;

[[nodiscard]] siggen::ModulatorSpec station_at(dsp::Hertz offset, double amplitude,
                                               std::uint64_t seed)
{
    siggen::ModulatorSpec spec;
    spec.kind = siggen::Modulation::Qpsk;
    spec.common.rate = kSceneRate;
    spec.common.carrier_offset = offset;
    spec.common.amplitude = amplitude;
    spec.common.seed = seed;
    spec.psk.symbol_rate = kStationSymbolRate;
    spec.psk.rolloff = 0.35;
    spec.psk.symbol_count = 4096;
    return spec;
}

// keyed_station doubles the first station and bursts it on and off, which is
// what the busy-band case needs: something has to move the span's strongest
// level when the gain is not moving it.
[[nodiscard]] siggen::SceneSpec dense_scene(bool keyed_station)
{
    siggen::SceneSpec spec;
    spec.rate = kSceneRate;
    spec.center_hz = 98'100'000;
    spec.duration_samples = static_cast<dsp::SampleIndex>(
        std::llround(kDenseSceneSeconds * static_cast<double>(kSceneRate)));
    spec.seed = 4242;
    spec.noise_power_full_band_dbfs = kDenseSceneNoiseDbfs;

    const dsp::Hertz offsets[] = {-260'000, -185'000, -110'000, -35'000,
                                  40'000,   115'000,  190'000,  265'000};
    std::uint64_t seed = 70;
    bool first = true;
    for (const dsp::Hertz offset : offsets) {
        const bool keyed = first && keyed_station;
        siggen::EmitterPlacement placement;
        placement.modulator =
            station_at(offset, keyed ? 2.0 * kStationAmplitude : kStationAmplitude, seed++);
        placement.use_snr = false;
        placement.start_sample =
            keyed ? static_cast<dsp::SampleIndex>(
                        std::llround(static_cast<double>(spec.duration_samples) * 0.30))
                  : 0;
        placement.end_sample =
            keyed ? static_cast<dsp::SampleIndex>(
                        std::llround(static_cast<double>(spec.duration_samples) * 0.70))
                  : spec.duration_samples;
        spec.emitters.push_back(placement);
        first = false;
    }
    return spec;
}

// The gain swing, which is the tuner AGC hunting.
//
// THE PERIOD IS SET BY THE DETECTOR AND NOT BY TASTE. The monitor reads the
// detector's averaged spectrum, which is an exponential average with
// average_seconds of one, so a swing near that period is attenuated before
// the monitor ever sees it: measured on an earlier form of this scene
// 2026-09-21, a two second period at 4 dB left a drive spread of 0.35 dB,
// under kFrontEndMinDriveSpreadDb, and every scene case answered Unmeasured.
// Five seconds against a one second average passes about two thirds of the
// swing, and 8 dB through that is a measured spread near 1.7 dB.
//
// The nominal gain puts the TOP of the swing at unity rather than the middle,
// so the fold-over bound on kThirdOrder is the one that holds at the loudest
// point.
constexpr double kSwingDb = 8.0;
constexpr double kSwingPeriodSeconds = 5.0;
const double kSwungGain = std::pow(10.0, -0.5 * kSwingDb / 20.0);

struct SceneRun {
    detect::FrontEndObservation observation{};
    std::size_t decisions = 0;
    std::vector<detect::Track> tracks;
};

[[nodiscard]] SceneRun run_scene(const siggen::SceneSpec& spec,
                                 const test::FrontEndModel& front_end)
{
    test::SceneGeometry geometry;
    geometry.rate = kSceneRate;
    geometry.channels = kSceneChannels;
    geometry.transform = kSceneTransform;

    auto frames = test::SceneFrames::create(geometry, spec, front_end);
    if (!frames) {
        FAIL("scene frames: " << frames.error().message);
    }

    detect::DetectorConfig config;
    config.source_rate = geometry.rate;
    config.source_center = spec.center_hz;
    config.grid_channels = geometry.channels;

    auto detector = detect::Detector::create(config, frames->spectrum());
    if (!detector) {
        FAIL("detector: " << detector.error().message);
    }

    detect::FrontEndMonitor monitor;
    SceneRun run;

    bool have_previous = false;
    dsp::SampleIndex previous = 0;

    const std::uint64_t total = frames->frames_available();
    for (std::uint64_t i = 0; i < total; ++i) {
        auto frame = frames->next();
        if (!frame) {
            FAIL("frame " << i << ": " << frame.error().message);
        }

        const Status fed = detector->consume(*frame);
        if (!fed) {
            FAIL("consume: " << fed.error().message);
        }

        const dsp::SampleIndex decided = detector->last_decision();
        if (!have_previous) {
            have_previous = decided != 0;
            previous = decided;
            continue;
        }
        if (decided == previous) {
            continue;
        }

        const double elapsed = static_cast<double>(decided - previous) /
                               static_cast<double>(geometry.rate);
        previous = decided;
        ++run.decisions;

        const Status watched =
            monitor.observe(detector->averaged_power(), detector->noise_floor(), elapsed);
        REQUIRE(watched.has_value());
    }

    run.observation = monitor.observation();
    run.tracks.assign(detector->tracks().begin(), detector->tracks().end());
    return run;
}

// Whether any published track covers an absolute frequency.
[[nodiscard]] bool covered(const SceneRun& run, dsp::Hertz absolute)
{
    return std::any_of(run.tracks.begin(), run.tracks.end(), [absolute](const auto& track) {
        const dsp::Hertz half = track.bandwidth / 2;
        return absolute >= track.center - half - 200 &&
               absolute <= track.center + half + 200;
    });
}

}  // namespace

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------

TEST_CASE("a monitor with no history answers unmeasured", "[frontend]")
{
    // An implementation that answered Steady before it had seen anything
    // would report a clean front end on every fresh connection, which is a
    // lie told confidently at exactly the moment an operator is deciding
    // whether their gain setting is right.
    detect::FrontEndMonitor monitor;
    CHECK(monitor.observation().verdict == detect::FrontEndVerdict::Unmeasured);

    const detect::FrontEndObservation after = sweep(monitor, 3.0, 8.0, 5);
    CHECK(after.verdict == detect::FrontEndVerdict::Unmeasured);
}

TEST_CASE("a span that does not move is not measured against", "[frontend]")
{
    // The slope's denominator is how far the drive moved. An implementation
    // that fitted one anyway divides by a number near zero and reports
    // whatever the floor's own ripple happened to do, at any magnitude,
    // which is the shape of a warning that fires at random.
    detect::FrontEndMonitor monitor;
    const detect::FrontEndObservation observed = sweep(monitor, 3.0, 0.02, 200);

    CHECK(observed.verdict == detect::FrontEndVerdict::Unmeasured);
    CHECK(observed.drive_spread_db < detect::kFrontEndMinDriveSpreadDb);
}

TEST_CASE("a floor that ignores the signal is steady", "[frontend]")
{
    // The ordinary case, and the one a linear receiver on a moving band
    // produces: the strongest signal fades and flutters and the thermal
    // floor does not care. An implementation keying off the floor's
    // absolute height rather than off its relationship to the drive would
    // flag a quiet receiver on a hot band.
    detect::FrontEndMonitor monitor;
    const detect::FrontEndObservation observed = sweep(monitor, 0.0, 10.0, 300);

    CHECK(observed.verdict == detect::FrontEndVerdict::Steady);
    CHECK(observed.drive_spread_db > detect::kFrontEndMinDriveSpreadDb);
}

TEST_CASE("a gain change is one for one and is not called a nonlinearity",
          "[frontend]")
{
    // This is the case that separates a useful flag from a useless one. A
    // tuner AGC multiplies, so the floor and the signal move by the same
    // number of decibels, and an implementation that only looked at whether
    // the floor rose across the whole span at once would fire on every AGC
    // excursion. gain=auto leaves that loop running, so it would fire
    // constantly on exactly the configuration it is meant to be diagnosing.
    detect::FrontEndMonitor monitor;
    const detect::FrontEndObservation observed = sweep(monitor, 1.0, 10.0, 300);

    CHECK(observed.verdict == detect::FrontEndVerdict::SpanScales);
    CHECK(std::abs(observed.slope - 1.0) < 0.05);
    CHECK(observed.correlation > 0.99);
}

TEST_CASE("a floor outrunning the signal is reported with its slope", "[frontend]")
{
    // Third-order products rise three decibels per decibel at the input.
    // The arithmetic is exact here, so the slope this reports is the slope
    // the case built, and an implementation that normalised or clamped it
    // would report a verdict with no number behind it.
    detect::FrontEndMonitor monitor;
    const detect::FrontEndObservation observed = sweep(monitor, 3.0, 8.0, 300);

    CHECK(observed.verdict == detect::FrontEndVerdict::FloorFollowsSignal);
    CHECK(std::abs(observed.slope - 3.0) < 0.05);
    CHECK(observed.floor_lift_db > 0.0);
}

TEST_CASE("one part of the span lifting is a busy band and not a front end",
          "[frontend]")
{
    // The whole reason the test is taken per segment. A station appearing
    // raises the floor where it is and nowhere else; a nonlinearity raises
    // it everywhere. An implementation that averaged the sixteen segments
    // reports 1.125 here, which is under the gain-change slope and reads as
    // nothing at all, and at a larger single-segment excursion it crosses
    // into FloorFollowsSignal and blames the radio for a neighbour.
    std::vector<double> slopes(detect::kFrontEndSegments, 0.0);
    slopes[3] = 18.0;

    detect::FrontEndMonitor monitor;
    const detect::FrontEndObservation observed = sweep(monitor, 0.0, 8.0, 300, 40.0, slopes);

    CHECK(observed.verdict == detect::FrontEndVerdict::Steady);
}

TEST_CASE("the verdict is carried by the weakest segment", "[frontend]")
{
    // Fifteen segments at three and one at one. The one is a gain change
    // and the fifteen are a nonlinearity, and there is no front end that
    // does that, so the honest answer is the weaker claim. An
    // implementation taking the median or the mean reports
    // FloorFollowsSignal on a span where a sixteenth of the evidence
    // contradicts it.
    std::vector<double> slopes(detect::kFrontEndSegments, 3.0);
    slopes[11] = 1.0;

    detect::FrontEndMonitor monitor;
    const detect::FrontEndObservation observed = sweep(monitor, 3.0, 8.0, 300, 40.0, slopes);

    CHECK(observed.verdict == detect::FrontEndVerdict::SpanScales);
    CHECK(std::abs(observed.slope - 1.0) < 0.05);
}

TEST_CASE("the verdict holds through the hysteresis band", "[frontend]")
{
    // Entering at 1.5 and leaving at 1.3 means a span sitting at 1.4 keeps
    // whatever it reached. An implementation with one threshold flickers
    // the line across it, which models/source_pacing.h records as the worst
    // presentation of a warning: the operator learns it is decorative.
    detect::FrontEndMonitor entered;
    CHECK(sweep(entered, 1.7, 8.0, 300).verdict == detect::FrontEndVerdict::FloorFollowsSignal);
    CHECK(sweep(entered, 1.4, 8.0, 300).verdict == detect::FrontEndVerdict::FloorFollowsSignal);

    detect::FrontEndMonitor never;
    CHECK(sweep(never, 1.4, 8.0, 300).verdict == detect::FrontEndVerdict::SpanScales);
}

TEST_CASE("reset forgets the window and the low-water mark", "[frontend]")
{
    // A retune puts a different piece of spectrum under every segment, so
    // a slope fitted across it is a slope through two bands and a lift is
    // measured against a floor that was somewhere else. An implementation
    // that kept the history would answer confidently about the band that
    // was left.
    detect::FrontEndMonitor monitor;
    REQUIRE(sweep(monitor, 3.0, 8.0, 300).verdict ==
            detect::FrontEndVerdict::FloorFollowsSignal);

    monitor.reset();
    CHECK(monitor.observation().verdict == detect::FrontEndVerdict::Unmeasured);

    const detect::FrontEndObservation after = sweep(monitor, 1.0, 10.0, 300);
    CHECK(after.verdict == detect::FrontEndVerdict::SpanScales);
}

TEST_CASE("a mismatched pair of arrays is refused rather than measured",
          "[frontend]")
{
    detect::FrontEndMonitor monitor;
    const std::vector<double> power(kBins, 1.0);
    const std::vector<double> floor(kBins / 2, 1.0);

    CHECK_FALSE(monitor.observe(power, floor, kStep).has_value());
    CHECK_FALSE(monitor.observe(power, power, 0.0).has_value());

    const std::vector<double> tiny(4, 1.0);
    CHECK_FALSE(monitor.observe(tiny, tiny, kStep).has_value());
}

// ---------------------------------------------------------------------------
// A scene through a cubic
// ---------------------------------------------------------------------------

TEST_CASE("two strong tones through a cubic put phantom tracks on the span",
          "[frontend][scene]")
{
    // The on-air failure of 2026-09-20, reproduced without a radio: three
    // intermodulation products reported as real tracks at confidence 1.00.
    // The detector is right about the energy and there is nothing in a
    // track list that could tell it apart from a station, which is the
    // whole reason the monitor exists.
    test::FrontEndModel front_end;
    front_end.third_order = kThirdOrder;

    const SceneRun run = run_scene(two_tone_scene(), front_end);

    INFO("tracks " << run.tracks.size());
    CHECK(covered(run, 98'100'000 + kProductLow));
    CHECK(covered(run, 98'100'000 + kProductHigh));
}

TEST_CASE("a linear front end puts nothing where the products would be",
          "[frontend][scene]")
{
    // The control for the case above. Same scene, same emitters, a3 at
    // zero. A track at either product frequency here would mean the scene
    // itself put something there and the case above measures nothing.
    const SceneRun run = run_scene(two_tone_scene(), test::FrontEndModel{});

    INFO("tracks " << run.tracks.size());
    CHECK_FALSE(covered(run, 98'100'000 + kProductLow));
    CHECK_FALSE(covered(run, 98'100'000 + kProductHigh));
}

TEST_CASE("the flag raises on a front end driven into its third order",
          "[frontend][scene]")
{
    // The measurement this whole lane is for. The gain swing is the tuner
    // AGC hunting, which is what gain=auto leaves running and what gives
    // the slope something to be fitted against; the cubic is the front end
    // being driven past its linear range. The dense scene's stations make
    // products that fill the span, and those rise three decibels for every
    // one at the input.
    test::FrontEndModel front_end;
    front_end.gain = kSwungGain;
    front_end.swing_db = kSwingDb;
    front_end.swing_period_seconds = kSwingPeriodSeconds;
    front_end.third_order = kThirdOrder;

    const SceneRun run = run_scene(dense_scene(false), front_end);

    INFO("decisions " << run.decisions << ", slope " << run.observation.slope
                      << ", correlation " << run.observation.correlation << ", spread "
                      << run.observation.drive_spread_db << " dB, lift "
                      << run.observation.floor_lift_db << " dB");
    CHECK(run.observation.verdict == detect::FrontEndVerdict::FloorFollowsSignal);
    CHECK(run.observation.slope > detect::kFrontEndNonlinearEnter);
}

TEST_CASE("the same gain swing through a linear front end is a gain swing",
          "[frontend][scene]")
{
    // Identical to the case above but for a3. An implementation that keyed
    // off the floor moving across the whole span at once passes that case
    // and fails this one, and failing this one means crying wolf on every
    // AGC excursion of every dongle.
    test::FrontEndModel front_end;
    front_end.gain = kSwungGain;
    front_end.swing_db = kSwingDb;
    front_end.swing_period_seconds = kSwingPeriodSeconds;

    const SceneRun run = run_scene(dense_scene(false), front_end);

    INFO("decisions " << run.decisions << ", slope " << run.observation.slope
                      << ", correlation " << run.observation.correlation << ", spread "
                      << run.observation.drive_spread_db << " dB");
    CHECK(run.observation.verdict == detect::FrontEndVerdict::SpanScales);
}

TEST_CASE("a crowded but linear band does not raise the flag", "[frontend][scene]")
{
    // The false positive the lane has to refuse. Eight stations across the
    // span, the loudest keying on and off, no gain change and no
    // nonlinearity. The strongest signal moves by several decibels, which
    // is exactly the leverage the regression wants, and the floor does not
    // follow it anywhere.
    const SceneRun run = run_scene(dense_scene(true), test::FrontEndModel{});

    INFO("decisions " << run.decisions << ", slope " << run.observation.slope
                      << ", correlation " << run.observation.correlation << ", spread "
                      << run.observation.drive_spread_db << " dB");
    CHECK(run.observation.verdict != detect::FrontEndVerdict::FloorFollowsSignal);
    CHECK(run.observation.verdict != detect::FrontEndVerdict::SpanScales);
}
