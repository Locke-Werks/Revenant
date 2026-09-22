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
#include <sstream>
#include <string>
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
[[nodiscard]] siggen::SceneSpec dense_scene(bool keyed_station,
                                            double noise_dbfs = kDenseSceneNoiseDbfs,
                                            double seconds = kDenseSceneSeconds)
{
    siggen::SceneSpec spec;
    spec.rate = kSceneRate;
    spec.center_hz = 98'100'000;
    spec.duration_samples = static_cast<dsp::SampleIndex>(
        std::llround(seconds * static_cast<double>(kSceneRate)));
    spec.seed = 4242;
    spec.noise_power_full_band_dbfs = noise_dbfs;

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

// ---- the product scene, which is about DISCRETE products -----------------

// Three narrow modulated parents whose third-order products land as separate
// bumps in clear space, which is the case neither scene above can produce.
//
// WHY A THIRD SCENE. The two above fail this job in opposite directions, and
// both failures were measured rather than predicted.
//
// The two-tone scene makes products that are themselves pure carriers, so no
// measurement of SHAPE can tell them from a real signal and none should be
// asked to; that scene separates on provenance.
//
// The dense scene makes products that are a pedestal. Measured 2026-09-22, its
// eight 30 kHz stations through the cubic produced ZERO false detections at
// every thermal floor from -70 to -115 dBFS, with the stations reading 504
// candidates at 46.13 dB and 25610 Hz identically at all four. Forty-five
// decibels of thermal noise moving nothing to the hertz is the finding: the
// floor being measured against there is the products' own pedestal, which the
// floor estimator tracks. Eight parents make 56 products of the form 2fi-fj
// and 168 of fi+fj-fk, each three times a parent wide, on a 600 kHz span.
// There is no gap left to detect into.
//
// Three parents make nine products. That is the whole design.
//
// WHY ROOT RAISED COSINE IS THE DECIDING PROPERTY, and not "QPSK is
// noise-like". An RRC spectrum is exactly zero outside (1 + rolloff) times the
// symbol rate, so the convolution of three of them is exactly zero outside
// three times that. The products have hard edges and there is genuinely
// nothing between them but thermal noise. An Nfm or Fsk2 parent has no such
// guarantee: core/detect/detector.h records a 16 kHz NFM comb going from five
// track ids to seven because growth crosses a Bessel comb's nulls, and tails
// like that are what turn nine products back into a pedestal.
constexpr double kProductSymbolRate = 3'000.0;
constexpr double kProductRolloff = 0.35;

// (1 + rolloff) * symbol_rate, which is what occupied_extent computes for a
// PSK emitter. A product is three of these.
constexpr dsp::Hertz kProductParentBandwidth = 4'050;
constexpr dsp::Hertz kProductBandwidth = 3 * kProductParentBandwidth;

// THE SPACING IS A RATIO AND NOT A ROUND NUMBER. With parents at f1, f1 + 2u
// and f1 + 5u, the nine products land at f1 + {-5, -3, -2, 2, 3, 4, 7, 8, 10}u
// and the three parents at {0, 2, 5}u: twelve distinct positions, minimum gap
// one u. Equal spacing is the trap, because then 2*f2 - f1 lands exactly on
// f3 and a product hides inside a parent.
constexpr dsp::Hertz kProductUnit = 34'000;

// Offset so the pattern is not mirror-symmetric about DC. A conjugation fault
// anywhere in the fixture would be invisible in a spectrum that mirrors
// itself, and this costs nothing to rule out.
constexpr dsp::Hertz kProductParentOne = -88'000;
constexpr dsp::Hertz kProductParentTwo = kProductParentOne + 2 * kProductUnit;   // -20'000
constexpr dsp::Hertz kProductParentThree = kProductParentOne + 5 * kProductUnit; // +82'000

// Every third-order product of three parents, in the order they sit on the
// span. Six of the form 2fi - fj and three of fi + fj - fk.
//
// The three mixed products are 3 dB stronger than the six others, and that is
// arithmetic rather than a measurement: expanding |x|^2 x, a term at 2f1 - f2
// arises one way and a term at f1 + f2 - f3 arises two, so the amplitude is
// doubled and the power quadrupled against twice.
constexpr dsp::Hertz kProducts[] = {
    2 * kProductParentOne - kProductParentThree,                        // -258'000
    kProductParentOne + kProductParentTwo - kProductParentThree,        // -190'000
    2 * kProductParentOne - kProductParentTwo,                          // -156'000
    2 * kProductParentTwo - kProductParentThree,                        // -122'000
    kProductParentOne + kProductParentThree - kProductParentTwo,        //  +14'000
    2 * kProductParentTwo - kProductParentOne,                          //  +48'000
    kProductParentTwo + kProductParentThree - kProductParentOne,        // +150'000
    2 * kProductParentThree - kProductParentTwo,                        // +184'000
    2 * kProductParentThree - kProductParentOne,                        // +252'000
};

// RMS per parent, the same number the tone scene uses, deliberately, so the
// two sit in the same validated regime.
//
// BOUNDED BOTH WAYS AND NEITHER BOUND IS SLACK. Product power goes as the
// sixth power of amplitude, so 0.20 costs seven decibels of product and puts
// the weaker six under the detection threshold. Upward, three parents at this
// level give a composite sigma of 0.433, near the 0.45 the fold-over argument
// above was written against, and a3 |x|^2 reaches about 0.42 at the loudest
// point in a five second record. The fold is at 0.389.
constexpr double kProductAmplitude = 0.25;

// Low enough that the products clear the threshold by eighteen decibels and
// high enough that the parents' window leakage stays thirty-two decibels under
// the floor.
//
// THE LEAKAGE WALL IS AT -82 dBFS HERE AND MUST NOT BE APPROACHED. The note on
// the tone scene above has the mechanism: the analysis window's sidelobes are
// 92 dB down, so a band whose strongest bin sits 92 dB over the per-bin floor
// leaks across the whole span at the floor and the detector reports tracks
// everywhere, including at every product frequency, which would make this
// scene pass for entirely the wrong reason.
//
// A modulated parent spreads its power over its own bandwidth rather than one
// bin, which buys 10*log10(4050 / 36.62) = 20.4 dB against that wall and is
// why this scene can run 5 dB quieter than the tone scene. At -50 dBFS the
// parents sit 59.7 dB over the per-bin floor with 32 dB of budget unspent.
// Do not take this below -65.
constexpr double kProductSceneNoiseDbfs = -50.0;

// Long enough for the one second average to fill, for birth_hits consecutive
// decisions, and for three passes of the 1.365 s payload cycle.
constexpr double kProductSceneSeconds = 8.0;

[[nodiscard]] siggen::ModulatorSpec product_parent_at(dsp::Hertz offset, std::uint64_t seed)
{
    siggen::ModulatorSpec spec;
    spec.kind = siggen::Modulation::Qpsk;
    spec.common.rate = kSceneRate;
    spec.common.carrier_offset = offset;
    spec.common.amplitude = kProductAmplitude;
    spec.common.seed = seed;
    spec.psk.symbol_rate = kProductSymbolRate;
    spec.psk.rolloff = kProductRolloff;
    spec.psk.symbol_count = 4096;
    return spec;
}

[[nodiscard]] siggen::SceneSpec product_scene(double noise_dbfs = kProductSceneNoiseDbfs)
{
    siggen::SceneSpec spec;
    spec.rate = kSceneRate;
    spec.center_hz = 98'100'000;
    spec.duration_samples = static_cast<dsp::SampleIndex>(
        std::llround(kProductSceneSeconds * static_cast<double>(kSceneRate)));
    spec.seed = 8080;
    spec.noise_power_full_band_dbfs = noise_dbfs;

    std::uint64_t seed = 400;
    for (const dsp::Hertz offset :
         {kProductParentOne, kProductParentTwo, kProductParentThree}) {
        siggen::EmitterPlacement placement;
        placement.modulator = product_parent_at(offset, seed++);

        // By amplitude and not by SNR, because the fold-over bound is a
        // statement about the composite envelope and an SNR-derived level
        // would move with the noise floor and drag that margin with it.
        placement.use_snr = false;
        placement.start_sample = 0;
        placement.end_sample = spec.duration_samples;
        spec.emitters.push_back(placement);
    }
    return spec;
}

// The cubic alone, with NO gain swing, which is not an omission.
//
// A swing is what the monitor's cases need and is exactly wrong here. Eight
// decibels of parent swing is twenty-four decibels of product swing under the
// three-for-one law, whose peak rate at the five second period is about
// 7.5 dB/s. detector.h's residual rule withholds a band falling faster than
// 3.26 dB/s at the shipped average, and its own measured table has a 25 dB
// depth at 6.0 dB/s starved for 5.72 s against a 3.0 s hold, which loses the
// track. Every product would be withheld on every downswing and this scene
// would report nothing at all.
[[nodiscard]] test::FrontEndModel product_front_end()
{
    return test::FrontEndModel{
        .gain = 1.0,
        .swing_db = 0.0,
        .swing_period_seconds = 0.0,
        .third_order = kThirdOrder,
    };
}

// ---- one emitter of each family, to see what shape reads ------------------

// WHAT THIS IS FOR. peak_to_mean was measured on 2026-09-22 to separate a
// third-order product from its QPSK parents, 2.473 against 1.607, and the
// linear control showed it reads the signal rather than the radio. That is
// spectral shape class, which is what docs/detection.md promises tier one will
// give, and it is NOT a rule yet: one scene with one modulation cannot settle
// a threshold that has to hold for everything else on the air.
//
// This is the scene that says what the other families read. No cubic, nothing
// to separate, no verdict: it is a table of what the measurement says about
// signals that are all unambiguously real.
//
// EIGHT SLOTS, 70 kHz APART, which is five times the widest emitter here and
// leaves every band two of its own widths of clear space to walk its skirts
// into.
constexpr dsp::Hertz kFamilySpacing = 70'000;
constexpr dsp::Hertz kFamilyFirst = -245'000;

// Comfortably detected without approaching the leakage wall the tone scene
// ran into, and stated as SNR because these emitters are not driving a
// nonlinearity and their absolute level does not matter to anything.
constexpr double kFamilySnrDb = 30.0;
constexpr double kFamilySceneSeconds = 6.0;

struct Family {
    const char* name;
    siggen::Modulation kind;
};

// The order is the order they sit on the span, lowest first.
constexpr Family kFamilies[] = {
    {"cw", siggen::Modulation::Cw},     {"am", siggen::Modulation::Am},
    {"nfm", siggen::Modulation::Nfm},   {"usb", siggen::Modulation::Usb},
    {"lsb", siggen::Modulation::Lsb},   {"fsk2", siggen::Modulation::Fsk2},
    {"bpsk", siggen::Modulation::Bpsk}, {"qpsk", siggen::Modulation::Qpsk},
};

[[nodiscard]] siggen::SceneSpec family_scene()
{
    siggen::SceneSpec spec;
    spec.rate = kSceneRate;
    spec.center_hz = 98'100'000;
    spec.duration_samples = static_cast<dsp::SampleIndex>(
        std::llround(kFamilySceneSeconds * static_cast<double>(kSceneRate)));
    spec.seed = 1234;
    spec.noise_power_full_band_dbfs = -60.0;

    std::uint64_t seed = 900;
    for (std::size_t i = 0; i < std::size(kFamilies); ++i) {
        siggen::ModulatorSpec modulator;
        modulator.kind = kFamilies[i].kind;
        modulator.common.rate = kSceneRate;
        modulator.common.carrier_offset =
            kFamilyFirst + static_cast<dsp::Hertz>(i) * kFamilySpacing;
        modulator.common.seed = seed++;

        // Every mode's own defaults except where a default would make the
        // emitter something other than what its name says. siggen's SSB on the
        // streaming path is a pure exponential per tone, so a single-tone USB
        // is a carrier; the two-tone form is the standard SSB test signal and
        // is at least two things rather than one.
        modulator.ssb.tone_hz = 700;
        modulator.ssb.tone2_hz = 1900;

        siggen::EmitterPlacement placement;
        placement.modulator = modulator;
        placement.use_snr = true;
        placement.snr_in_occupied_bandwidth_db = kFamilySnrDb;
        placement.start_sample = 0;
        placement.end_sample = spec.duration_samples;
        spec.emitters.push_back(placement);
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

    // Every decision's candidates, appended. The tracks above are the last
    // decision's and are what a client sees; these are what the shape survey
    // below reads, because BandShape is measured per candidate and each
    // decision's arrays are overwritten by the next one.
    std::vector<detect::Candidate> candidates;
};

// transform overrides the second stage, and only the shape surveys pass it.
// Everything else in this file is measured against the SHIPPED bin width and
// has to stay there: a case that quietly ran on a finer grid would be
// reporting numbers no operator will ever see.
[[nodiscard]] SceneRun run_scene(const siggen::SceneSpec& spec,
                                 const test::FrontEndModel& front_end,
                                 std::uint32_t transform = kSceneTransform)
{
    test::SceneGeometry geometry;
    geometry.rate = kSceneRate;
    geometry.channels = kSceneChannels;
    geometry.transform = transform;

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

        run.candidates.insert(run.candidates.end(), detector->candidates().begin(),
                              detector->candidates().end());
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

// ---- discrete products, which is the case shape work needs ---------------

// THE SCENE THIS TREE DID NOT HAVE. Three narrow modulated parents through a
// cubic put nine separate product bands on the span, each one a thing the
// detector reports as a signal and each one not a signal.
//
// That is the on-air failure of 2026-09-20 reproduced without a radio: three
// intermodulation products listed as real tracks at 95.1 MHz, at confidence
// 1.00, indistinguishable from stations. Neither scene above reproduces it.
// The two-tone products are carriers, and the dense scene's are a pedestal the
// floor estimator absorbs.
TEST_CASE("three modulated parents through a cubic put nine products on the span",
          "[frontend][scene]")
{
    const SceneRun run = run_scene(product_scene(), product_front_end());
    REQUIRE(run.decisions > 0);

    std::size_t found = 0;
    for (const dsp::Hertz product : kProducts) {
        const dsp::Hertz absolute = 98'100'000 + product;
        const bool here = covered(run, absolute);
        INFO("product at " << product << " Hz offset");
        CHECK(here);
        found += here ? 1 : 0;
    }
    INFO(found << " of 9 products carried a track");

    // The parents are still there too. A scene that lost them would be
    // measuring something other than intermodulation.
    CHECK(covered(run, 98'100'000 + kProductParentOne));
    CHECK(covered(run, 98'100'000 + kProductParentTwo));
    CHECK(covered(run, 98'100'000 + kProductParentThree));
}

// THE CONTROL THAT MAKES THE CASE ABOVE MEAN ANYTHING, and the one the tone
// scene's history says is mandatory. The identical scene at the identical
// noise floor with a linear front end has to be empty at every product
// frequency.
//
// Without it the case above passes for a reason that has nothing to do with
// the front end: if the parents are loud enough, the analysis window's own
// sidelobes lift the whole span over the floor and the detector reports tracks
// everywhere, including at all nine products. That is exactly what happened to
// the two-tone scene at -70 dBFS, where the linear control published nineteen
// tracks.
TEST_CASE("a linear front end puts nothing where the nine products would be",
          "[frontend][scene]")
{
    const SceneRun run = run_scene(product_scene(), test::FrontEndModel{});
    REQUIRE(run.decisions > 0);

    for (const dsp::Hertz product : kProducts) {
        const dsp::Hertz absolute = 98'100'000 + product;
        INFO("product at " << product << " Hz offset");
        CHECK_FALSE(covered(run, absolute));
    }

    // And the parents are found, so the emptiness above is the absence of
    // products rather than a detector that found nothing at all. That is the
    // trap the stopped-emitter case in test_detector_scene.cpp names: every
    // upper bound passes vacuously on a detector that is not working.
    CHECK(covered(run, 98'100'000 + kProductParentOne));
    CHECK(covered(run, 98'100'000 + kProductParentTwo));
    CHECK(covered(run, 98'100'000 + kProductParentThree));
}

// ---- does shape separate a station from a product? ------------------------

// THE MEASUREMENT THAT DECIDES WHETHER core/detect/shape.h IS WORTH ANYTHING,
// and it is a survey rather than a bar. It prints; it asserts only that the
// scene it is reading is the one it thinks it is.
//
// The dense scene through a cubic is the one case in this tree where a band
// that IS a signal and a band that is NOT one appear in the same frames at the
// same moment, with truth known by construction: eight QPSK stations at
// offsets this file chose, and products of those stations everywhere else.
// Anything the detector finds away from a station is a product, because
// nothing else was transmitted.
//
// The tone scene deliberately is not used here. A third-order product of two
// pure carriers is itself a pure carrier, so shape cannot tell it from a real
// one and should not be asked to: that scene separates on provenance.
//
// WHAT THIS MEASURED, 2026-09-22, AND IT IS A NEGATIVE RESULT WORTH KEEPING.
// The dense scene through the cubic produces NO false detections at all, at
// any thermal floor from -70 to -115 dBFS:
//
//   floor      cubic, on a station                     cubic, elsewhere
//   -70 dBFS   504 candidates, 46.13 dB, 25610 Hz      none
//   -85 dBFS   504 candidates, 46.13 dB, 25610 Hz      none
//   -100 dBFS  504 candidates, 46.13 dB, 25610 Hz      none
//   -115 dBFS  504 candidates, 46.13 dB, 25610 Hz      none
//
// Identical to the hertz across forty-five decibels of thermal noise, which is
// the finding rather than a coincidence: what the stations are being measured
// against in the cubic run is not thermal noise, it is the pedestal the
// products themselves laid down, and the floor estimator tracks that pedestal.
// The linear control is what proves it, because there the numbers do move with
// the floor: 1050 candidates at 35.91 dB and 14178 Hz at -70 dBFS, settling to
// about 1546 at 26.6 dB and 10930 Hz once the stations' own skirts become the
// floor.
//
// The products in this scene are therefore not phantom signals, they are a
// raised floor, and the detector is right to report nothing there. It reports
// FEWER candidates through the cubic than without it, not more.
//
// SO THE TREE HAS NO FIXTURE FOR THE FAILURE THIS WORK IS AIMED AT. The
// on-air case behind it, three intermod products listed as tracks at 95.1 MHz,
// is discrete products standing clear of the floor, which is the tone scene's
// shape and not this one's. Validating a shape discriminator needs a scene
// with modulated parents whose products land DISCRETELY in clear space, and
// building that is the next piece of work rather than choosing a threshold
// against a population that does not exist.
TEST_CASE("shape survey: parents against their own products", "[.shape-survey]")
{
    // AGAINST THE PRODUCT SCENE, which is the one place in this tree where a
    // band that IS a signal and a band that is NOT one appear in the same
    // frames at the same moment with truth known by construction. The parents
    // are where they were placed and the nine products are where the
    // arithmetic says, and the model carries no fifth-order term, so anything
    // found anywhere else is an artefact rather than a product.
    //
    // WHAT THIS REPLACED, and the negative result is worth keeping. The survey
    // first ran against dense_scene swept from -70 to -115 dBFS and found 1384
    // candidates, every one of them on a station and not one on a product,
    // identical to the hertz at all four floors. That scene's products are a
    // raised floor rather than phantom signals, so there was nothing to
    // separate. This scene was built because of that reading.
    // WHAT THIS MEASURED, 2026-09-22.
    //
    //   parent          263 candidates  peak/mean 1.607  skirt 0.111  48.1 dB
    //   product         567 candidates  peak/mean 2.473  skirt 0.009  24.3 dB
    //   elsewhere       304 candidates  peak/mean 1.863  skirt 0.091  13.9 dB
    //   parent, linear  189 candidates  peak/mean 1.608  skirt 0.028
    //
    // TWO FINDINGS, AND THEY ARE ABOUT DIFFERENT THINGS.
    //
    // peak_to_mean separates a product from a parent and the control says it
    // is measuring the signal and not the radio: 1.607 through the cubic
    // against 1.608 linear, identical to three places, while a product reads
    // 2.473. That is structural. A root raised cosine spectrum is flat topped,
    // and a third-order product is the convolution of three of them, which is
    // domed. It is not an artefact of level either: the ordering is not
    // monotone in SNR, since "elsewhere" is the weakest population at 13.9 dB
    // and reads BELOW the products at 24.3 dB.
    //
    // THIS IS NOT AN INTERFERENCE TEST AND MUST NOT BE SHIPPED AS ONE. What it
    // separates is flat-topped from domed, which is spectral shape class and
    // is exactly the "separates the broad families" tier one is promised to
    // give. An unmodulated carrier is the most domed thing on any span, so a
    // threshold that called domed bands products would call every carrier a
    // product. The parents here are QPSK because the scene needed them to be;
    // one scene with one modulation cannot settle a threshold that has to hold
    // for AM, FM, SSB and CW.
    //
    // CONCENTRATION WAS ADDED TO THIS SURVEY ON 2026-09-22 AND DOES NOT
    // SEPARATE THESE POPULATIONS EITHER, which is worth keeping because it was
    // added on the strength of a result that looked like it would.
    //
    //   parent          conc 0.079   width 2844 Hz
    //   product         conc 0.033   width 7651 Hz
    //   elsewhere       conc 0.155   width 1210 Hz
    //   parent, linear  conc 0.047   width 3538 Hz
    //
    // The artefact population reads HIGHEST, above both the stations and their
    // products, and a real parent moves from 0.047 to 0.079 on nothing but
    // whether the front end is linear. A bar anywhere in that range keeps and
    // drops both populations together.
    //
    // WHY, AND IT IS THE SAME REASON peak_to_mean FAILS HERE. Both are the
    // band's power in its peak against the band's power in total; they differ
    // in whether the width divides in. Every population in this scene is a
    // FILLED wideband band, so all three read low and what is left ordering
    // them is mostly how wide each one is.
    //
    // THIS DOES NOT RETRACT WHAT CONCENTRATION MEASURED ON REAL HF, where an
    // 11.7 kHz patch of noise floor read 0.02 against 0.59 to 0.86 for the
    // carriers beside it, and that separation was not width alone: two bands
    // of 809 Hz and 1210 Hz on the same span read 0.86 and 0.28. It bounds the
    // claim. Concentration separates a band that is nearly all signal from a
    // band that is nearly all floor, which is the complaint it was built for.
    // It does not separate a station from its own intermodulation product,
    // both of those being real filled bands, and it must not ship as though it
    // did. docs/detection.md carries both halves.
    //
    // skirt_fraction is the other finding and it moves the other way. It is
    // flat across the two populations and rises on the PARENTS when the front
    // end is nonlinear, 0.028 linear to 0.111 through the cubic. That is
    // spectral regrowth: the band driving the nonlinearity is the one that
    // smears into its own neighbourhood. It is per-band evidence of
    // distortion, which core/detect/front_end.h says the span-wide monitor
    // cannot give and which it rejects frequency coincidence as a route to.
    // Whether it is strong enough to carry a flag is a separate measurement.
    const SceneRun cubic = run_scene(product_scene(), product_front_end());
    REQUIRE(cubic.decisions > 0);

    struct Tally {
        std::size_t count = 0;
        double peak_to_mean = 0.0;
        double concentration = 0.0;
        double skirt = 0.0;
        double lower = 0.0;
        double snr = 0.0;
        double bandwidth = 0.0;
        std::size_t no_room = 0;

        void add(const detect::Candidate& candidate)
        {
            ++count;
            peak_to_mean += candidate.shape.peak_to_mean;
            concentration += candidate.shape.concentration;
            skirt += candidate.shape.skirt_fraction;
            lower += candidate.shape.lower_fraction;
            snr += candidate.snr_2500_db;
            bandwidth += static_cast<double>(candidate.bandwidth);
            if (candidate.shape.skirt_bins_available == 0) {
                ++no_room;
            }
        }

        [[nodiscard]] std::string line() const
        {
            if (count == 0) {
                return "none";
            }
            const auto n = static_cast<double>(count);
            std::ostringstream out;
            out.setf(std::ios::fixed);
            out.precision(3);
            out << count << " candidates, peak/mean " << peak_to_mean / n << ", conc "
                << concentration / n << ", skirt "
                << skirt / n << ", lower " << lower / n << ", snr " << snr / n << " dB, width "
                << bandwidth / n << " Hz, " << no_room << " with no room to look";
            return out.str();
        }
    };

    // Half a parent's own bandwidth, so a candidate is attributed to whatever
    // it actually sits on rather than to whatever is nearest.
    constexpr dsp::Hertz kNear = kProductParentBandwidth;

    Tally parents;
    Tally products;
    Tally elsewhere;

    for (const detect::Candidate& candidate : cubic.candidates) {
        if (!candidate.shape.measured) {
            continue;
        }
        const dsp::Hertz offset = candidate.center - 98'100'000;

        const bool parent =
            std::abs(offset - kProductParentOne) <= kNear ||
            std::abs(offset - kProductParentTwo) <= kNear ||
            std::abs(offset - kProductParentThree) <= kNear;

        const bool product =
            std::any_of(std::begin(kProducts), std::end(kProducts),
                        [offset](dsp::Hertz at) { return std::abs(offset - at) <= 3 * kNear; });

        if (parent) {
            parents.add(candidate);
        } else if (product) {
            products.add(candidate);
        } else {
            elsewhere.add(candidate);
        }
    }

    // THE CONTROL THAT SAYS WHICH THING IS BEING MEASURED. The parents in the
    // cubic run carry their own third-order regrowth, so a difference between
    // them and the products could be the regrowth rather than the shape. The
    // same parents through a linear front end have none of it, so a parent
    // reading the same in both runs means the number is the signal's own
    // shape and generalises past this scene.
    const SceneRun linear = run_scene(product_scene(), test::FrontEndModel{});
    Tally linear_parents;
    for (const detect::Candidate& candidate : linear.candidates) {
        if (!candidate.shape.measured) {
            continue;
        }
        const dsp::Hertz offset = candidate.center - 98'100'000;
        if (std::abs(offset - kProductParentOne) <= kNear ||
            std::abs(offset - kProductParentTwo) <= kNear ||
            std::abs(offset - kProductParentThree) <= kNear) {
            linear_parents.add(candidate);
        }
    }

    WARN("parent:        " << parents.line());
    WARN("product:       " << products.line());
    WARN("elsewhere:     " << elsewhere.line());
    WARN("parent linear: " << linear_parents.line());

    // Both populations have to exist or the comparison above is one thing read
    // twice. Everything else is printed rather than asserted, because what a
    // threshold on any of it should be is not settled; see the reading
    // recorded on this case.
    CHECK(parents.count > 0);
    CHECK(products.count > 0);
}
// What shape reads for each family of real signal, with nothing to separate.
//
// THE TABLE A THRESHOLD WOULD HAVE TO SURVIVE. The product survey below found
// peak_to_mean separating a product at 2.473 from its QPSK parents at 1.607.
// Every row here is a signal that is unambiguously real, so any rule that
// called a row of this table a product would be wrong about it.
//
// WHAT THIS MEASURED, 2026-09-22, AND IT SETTLES THE QUESTION AGAINST A GLOBAL
// THRESHOLD.
//
//   family   peak/mean   skirt   lower   width
//   cw         2.384     0.000   0.603    183 Hz
//   am         2.323     0.000   0.464    180 Hz
//   nfm        2.256     0.000   0.514    175 Hz
//   usb        2.112     0.000   0.453    165 Hz
//   lsb        2.111     0.000   0.547    165 Hz
//   fsk2       2.854     0.432   0.481    862 Hz
//   bpsk       1.793     0.001   0.495   1465 Hz
//   qpsk       1.703     0.039   0.499   1430 Hz
//
//   product    2.473     0.009   0.507   7651 Hz   (from the survey below)
//
// THERE IS NO PEAK_TO_MEAN THRESHOLD AND THERE CANNOT BE ONE. The product sits
// at 2.473, between nfm at 2.256 and fsk2 at 2.854. A bar that rejected the
// product would reject every FSK signal on the air and would sit below cw, am
// and nfm. The only clean split in the column is the PSK pair at 1.70 and 1.79
// against everything else, which is flat-topped against not, and is a family
// split rather than a signal-against-interference one.
//
// AND MOST OF THAT SPREAD IS RESOLUTION RATHER THAN MODULATION, which the
// width column gives away. cw, am, nfm, usb and lsb are line spectra, and the
// detector reports their lines SEPARATELY: 135 candidates for am over 45
// decisions is three a decision, a carrier and two sidebands, and 675 for nfm
// is fifteen, which is the Bessel comb. Each of those bands is about 170 Hz,
// which at 36.6 Hz a bin is four or five bins. A band that narrow cannot have
// a shape: peak_to_mean is pinned near 2.3 by how the analysis window spreads
// a single line, whatever produced the line.
//
// So peak_to_mean says something only about bands wide compared with the
// window, which the two PSK rows and the product are and the other five are
// not. A consumer has to check the width before believing the number, and
// Candidate carries first_bin and last_bin so it can.
//
// lower_fraction does what it was meant to, at the one thing it can see here:
// usb reads 0.453 and lsb 0.547, mirrored about a half and in the right
// directions. It is a small margin and this is a two-tone test signal rather
// than speech, but the sign is correct and consistent.
//
// skirt_fraction rules itself out as an absolute measure in the same table.
// fsk2 reads 0.432 with a perfectly linear front end, four times what a QPSK
// parent reads when it IS being driven into a cubic. Whatever a distortion
// flag is built on, it cannot be a global skirt threshold.
TEST_CASE("shape survey: what each family reads", "[.shape-survey]")
{
    const SceneRun run = run_scene(family_scene(), test::FrontEndModel{});
    REQUIRE(run.decisions > 0);

    for (std::size_t i = 0; i < std::size(kFamilies); ++i) {
        const dsp::Hertz at = kFamilyFirst + static_cast<dsp::Hertz>(i) * kFamilySpacing;

        // Half the spacing, so every candidate is attributed to exactly one
        // emitter and a band that wandered is still counted rather than
        // silently dropped.
        const dsp::Hertz window = kFamilySpacing / 2;

        std::size_t count = 0;
        double peak_to_mean = 0.0;
        double concentration = 0.0;
        double skirt = 0.0;
        double lower = 0.0;
        double snr = 0.0;
        double width = 0.0;

        for (const detect::Candidate& candidate : run.candidates) {
            if (!candidate.shape.measured) {
                continue;
            }
            if (std::abs(candidate.center - 98'100'000 - at) > window) {
                continue;
            }
            ++count;
            peak_to_mean += candidate.shape.peak_to_mean;
            concentration += candidate.shape.concentration;
            skirt += candidate.shape.skirt_fraction;
            lower += candidate.shape.lower_fraction;
            snr += candidate.snr_2500_db;
            width += static_cast<double>(candidate.bandwidth);
        }

        if (count == 0) {
            WARN(kFamilies[i].name << ": nothing detected at " << at << " Hz");
            continue;
        }

        const auto n = static_cast<double>(count);
        std::ostringstream out;
        out.setf(std::ios::fixed);
        out.precision(3);
        out << kFamilies[i].name << ": " << count << " candidates, peak/mean "
            << peak_to_mean / n << ", conc " << concentration / n << ", skirt "
            << skirt / n << ", lower " << lower / n << ", snr " << snr / n
            << " dB, width " << width / n << " Hz";
        WARN(out.str());
    }

    // The scene has to have produced something or the table above is empty
    // rows. Nothing else is asserted: this case is a measurement.
    CHECK(!run.candidates.empty());
}
// ---- the same families on a grid that resolves them ----------------------

// THE MEASUREMENT THE TABLE ABOVE COULD NOT MAKE.
//
// At the shipped 36.6 Hz, cw, am, nfm, usb and lsb all read concentration
// 0.946 to 0.947, identical to three decimal places. That is not a result
// about those families, it is the window: each of their lines is reported as
// its own five-bin band and three bins of five is most of five whatever put
// the power there. The number only starts saying something about a band once
// the band is wide enough to have an inside.
//
// So this runs the same scene with a 2048 point second stage instead of 512,
// which is 9.16 Hz a bin, and asks whether the five that could not be told
// apart can be told apart when they are resolved. It is a survey and asserts
// only that the scene it read is the one it thinks it is.
//
// 2048 AND NOT MORE: the channelizer's twiddle builder caps the second stage
// at 2048, and asking for 4096 is refused in those words rather than run.
//
// FOUR TIMES THE BINS IS FOUR TIMES THE WORK and this is why the case is
// hidden behind a tag. It is also why run_scene takes the transform rather
// than this file changing kSceneTransform: every other case here is measured
// against the shipped bin width and has to stay there.
//
// WHAT A FINER GRID DOES TO THE DETECTOR'S OWN CONSTANTS is part of what this
// shows. split_gap_bins is eight bins, which is 293 Hz at the shipped width
// and 73 Hz here, so a carrier and its sidebands can fall either side of it:
// docs/detection.md already carries that constant as knowingly wrong on a
// finer grid, and the widths in this table are where it shows.
//
// WHAT IT MEASURED, 2026-09-22, AND IT IS A NEGATIVE RESULT ABOUT THE WHOLE
// APPROACH FOR FIVE OF THESE EIGHT.
//
//   family  conc    width      bins    (against 36.6 Hz: conc, width)
//   cw      0.860   55.0 Hz     6.0    0.946   ~180 Hz
//   am      0.942   43.0 Hz     4.7    0.947   ~180 Hz
//   nfm     0.946   43.5 Hz     4.7    0.947   ~175 Hz
//   usb     0.927   37.0 Hz     4.0    0.946   ~165 Hz
//   lsb     0.927   37.0 Hz     4.0    0.946   ~165 Hz
//   fsk2    0.277  712.5 Hz    77.8    0.519   ~862 Hz
//   bpsk    0.056 1420.8 Hz   155.2    0.117  ~1465 Hz
//   qpsk    0.056 1409.3 Hz   153.9    0.121  ~1430 Hz
//
// THE FIVE NARROW FAMILIES ARE STILL FIVE BINS WIDE. They were about five bins
// at 36.6 Hz and they are about five bins at 9.16, which means their measured
// width fell by four when the grid did: 180 Hz to 43. That width was never the
// signal. Each of these detections is ONE SPECTRAL LINE, and a line is as wide
// as the analysis window makes it whatever the window is.
//
// So no amount of resolution turns a per-detection shape into an answer about
// these families. The information that separates AM from SSB from CW is in the
// RELATIONSHIP BETWEEN the lines, which is to say between detections, and
// nothing that measures one band can reach it. That is a statement about the
// detector's decomposition rather than about concentration, and it bounds
// every per-band number in core/detect/shape.h the same way.
//
// THE THREE THAT ARE RESOLVED SEPARATE, AND MORE SHARPLY THAN BEFORE. fsk2 at
// 0.277 against bpsk and qpsk at 0.056 is five to one, where the shipped grid
// gave 0.519 against 0.117 and 0.121, which is four to one. A finer grid helps
// exactly the bands it resolves, which is the ones already wide enough for the
// number to mean anything.
//
// AND lower_fraction IS THE SAME STORY, WHICH IS WORTH SAYING BECAUSE IT LOOKS
// LIKE A SUCCESS. usb reads 0.465 here and lsb 0.535, against 0.453 and 0.547
// at the shipped grid, so the two separate and separate consistently. But the
// pairs MIRROR ABOUT A HALF TO THREE DECIMAL PLACES at both grids, and two
// independent measurements of two different signals do not do that.
//
// The scene's SSB emitters are two-tone at 700 and 1900 Hz, so USB is two
// lines above the carrier and LSB the same two mirrored below, each reported
// as its own four-bin band. What separates is where a line sits inside its own
// band, and mirrored lines mirror. am moving from 0.464 to 0.372 between the
// two grids is the same instability from the other side: a symmetric mode has
// no business being the most asymmetric row in the table.
//
// Nothing here ever sees the 2.8 kHz asymmetric block that a sideband test is
// supposed to read, so the premise is untested rather than refuted.
TEST_CASE("shape survey: the same families, resolved", "[.shape-survey]")
{
    constexpr std::uint32_t kFineTransform = 2048;
    const SceneRun run =
        run_scene(family_scene(), test::FrontEndModel{}, kFineTransform);
    REQUIRE(run.decisions > 0);

    const double bin_hz = static_cast<double>(kSceneRate) /
                          static_cast<double>(kSceneChannels * kFineTransform / 2);
    WARN("bin width " << bin_hz << " Hz against 36.6 shipped");

    for (std::size_t i = 0; i < std::size(kFamilies); ++i) {
        const dsp::Hertz at = kFamilyFirst + static_cast<dsp::Hertz>(i) * kFamilySpacing;
        const dsp::Hertz window = kFamilySpacing / 2;

        std::size_t count = 0;
        double peak_to_mean = 0.0;
        double concentration = 0.0;
        double lower = 0.0;
        double snr = 0.0;
        double width = 0.0;

        for (const detect::Candidate& candidate : run.candidates) {
            if (!candidate.shape.measured) {
                continue;
            }
            if (std::abs(candidate.center - 98'100'000 - at) > window) {
                continue;
            }
            ++count;
            peak_to_mean += candidate.shape.peak_to_mean;
            concentration += candidate.shape.concentration;
            lower += candidate.shape.lower_fraction;
            snr += candidate.snr_2500_db;
            width += static_cast<double>(candidate.bandwidth);
        }

        if (count == 0) {
            WARN(kFamilies[i].name << ": nothing detected at " << at << " Hz");
            continue;
        }

        const auto n = static_cast<double>(count);
        std::ostringstream out;
        out.setf(std::ios::fixed);
        out.precision(3);
        out << kFamilies[i].name << ": " << count << " candidates, conc "
            << concentration / n << ", peak/mean " << peak_to_mean / n << ", lower "
            << lower / n << ", snr " << snr / n << " dB, width " << width / n
            << " Hz, " << (width / n) / bin_hz << " bins";
        WARN(out.str());
    }

    CHECK(!run.candidates.empty());
}

// Does the skirt rise with the drive, or only with the switch being on?
//
// THE QUESTION THAT DECIDES WHETHER A DISTORTION FLAG IS WORTH BUILDING. The
// family table above rules out any absolute skirt threshold: fsk2 reads 0.432
// through a perfectly linear front end, four times what a QPSK signal reads
// while it is being driven into a cubic. What separated there was the CHANGE,
// 0.028 to 0.111 on the same emitters when the nonlinearity was switched on.
//
// A change between two points is one measurement, and a single pair can be a
// coincidence. If the skirt is really reading spectral regrowth it has to
// climb with the third-order coefficient rather than jump once, because
// regrowth is a continuous function of how hard the stage is driven. If it
// does not climb, whatever the pair measured was not this and nothing should
// be built on it.
//
// A SWEEP OF THE COEFFICIENT AND NOT OF THE SIGNAL LEVEL, so the parents are
// bit-identical at every rung and the only thing moving is the stage. Driving
// them harder would change their own occupied bandwidth as well, and then a
// skirt that grew would have two explanations.
//
// WHAT THIS MEASURED, 2026-09-22.
//
//   a3      skirt   peak/mean   width
//   0.000   0.028     1.608     3538 Hz
//   0.050   0.028     1.608     3537 Hz
//   0.100   0.049     1.652     3057 Hz
//   0.150   0.111     1.607     2844 Hz
//
// It climbs, and it climbs the way regrowth should rather than the way a
// switch would: flat to three places at 0.05, roughly double the baseline at
// 0.10, four times it at 0.15. The excess over the linear reading is 0.000,
// 0.021 and 0.083, which is close to the square of the coefficient, and
// third-order regrowth power goes as exactly that. So the pair measured
// earlier was not a coincidence and the number is reading the stage.
//
// IT IS BLIND BELOW A DRIVE, which the 0.05 row says plainly and which is a
// limit rather than a defect: at that coefficient the regrowth is under the
// level the detector's own edge growth walks to, and no product crosses the
// detection threshold either, so there is nothing to flag and nothing is
// flagged.
//
// peak_to_mean sits between 1.607 and 1.652 across the whole sweep, which is
// the control on the control: the parents' shape CLASS does not move when the
// stage is driven, so that number reads the modulation and this one reads the
// distortion, and neither is reading the other.
//
// The width shrinking from 3538 to 2844 Hz is the occupied-bandwidth trim
// reacting to the pedestal the products lay down under the signal, not the
// signal getting narrower.
TEST_CASE("shape survey: skirt against drive", "[.shape-survey]")
{
    // Zero is the control. 0.15 is what every other nonlinear case in this
    // file uses and is where the fold-over argument puts the ceiling.
    const double drives[] = {0.0, 0.05, 0.10, 0.15};

    for (const double third_order : drives) {
        const SceneRun run = run_scene(product_scene(), test::FrontEndModel{
                                                            .gain = 1.0,
                                                            .swing_db = 0.0,
                                                            .swing_period_seconds = 0.0,
                                                            .third_order = third_order,
                                                        });
        REQUIRE(run.decisions > 0);

        std::size_t count = 0;
        double skirt = 0.0;
        double peak_to_mean = 0.0;
        double width = 0.0;
        double snr = 0.0;

        for (const detect::Candidate& candidate : run.candidates) {
            if (!candidate.shape.measured) {
                continue;
            }
            const dsp::Hertz offset = candidate.center - 98'100'000;
            const bool parent =
                std::abs(offset - kProductParentOne) <= kProductParentBandwidth ||
                std::abs(offset - kProductParentTwo) <= kProductParentBandwidth ||
                std::abs(offset - kProductParentThree) <= kProductParentBandwidth;
            if (!parent) {
                continue;
            }
            ++count;
            skirt += candidate.shape.skirt_fraction;
            peak_to_mean += candidate.shape.peak_to_mean;
            width += static_cast<double>(candidate.bandwidth);
            snr += candidate.snr_2500_db;
        }

        if (count == 0) {
            WARN("a3 " << third_order << ": no parent candidates");
            continue;
        }

        const auto n = static_cast<double>(count);
        std::ostringstream out;
        out.setf(std::ios::fixed);
        out.precision(3);
        out << "a3 " << third_order << ": " << count << " parent candidates, skirt "
            << skirt / n << ", peak/mean " << peak_to_mean / n << ", width " << width / n
            << " Hz, snr " << snr / n << " dB";
        WARN(out.str());
    }

    CHECK(std::size(drives) == 4);
}
