// The detector on overlapping frames.
//
// "Keep the display's row rate when the source is slow" gave a source under
// 1.97 MS/s a block shorter than the spectrum's window, so consecutive frames
// now overlap: 96 kS/s on 16 channels has a 16384-sample window and a
// 3200-sample hop, five windows on every sample. Frames that share samples are
// correlated, and a rule that counted them as independent would now be wrong
// by that factor. This file measures whether the detector has any such rule.
//
// Three cadences per slow rate, on the grid Engine::open_source settles on:
//
//   contiguous   the hop is the window, nothing overlaps and nothing is
//                skipped. The control: same geometry, no overlap.
//   30 rows/s    the hop EngineConfig::spectrum_rows_per_second gives, which
//                is what revenant-engine does now.
//   65536 block  one frame per 65536-sample block, which is what a slow
//                source got before that change. The window is shorter than
//                the block, so most samples are never transformed.
//
// 2.4 MS/s on 64 channels is the reference: its window is its block, so all
// three are the same cadence there and it is run once.
//
// The frames come from tests/detect/scene_frames.h, so the channelizer and
// spectrum twins, which tests/reference proves bit-exact against the kernels.
// The recordings case runs the engine itself on the KF4FIC excerpts.
//
// The measurement cases are hidden, because the 2.4 MS/s scenes cost about a
// second and a half of wall time per second of scene:
//
//   revenant_detect_tests.exe "[overlap]"
//   revenant_detect_tests.exe "[overlap-recordings]"
//
// "overlapping frames leave the detector where contiguous ones put it" is not
// hidden: it runs 96 kS/s only, where a scene costs about a second.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "core/detect/detector.h"
#include "core/dsp/synth/modulators.h"
#include "core/dsp/synth/wideband.h"
#include "core/engine/engine.h"
#include "scene_frames.h"

using namespace revenant;

namespace {

template <typename T>
[[nodiscard]] std::string message_of(const T& result) {
    return result.has_value() ? std::string("ok") : result.error().message;
}

constexpr double kNoiseDbfs = -60.0;
constexpr std::size_t kLegacyBlock = 65'536;
constexpr double kRowsPerSecond = 30.0;

struct RateCase {
    const char* name;
    dsp::SampleRate rate;
    std::uint32_t channels;
};

// The grids docs/rpc.md measured the row-rate change on, which are what
// Engine::open_source chooses for those rates with a centre below 30 MHz, and
// the shipped VHF geometry the detector's constants were calibrated on.
//
// And 96 kS/s on 64 channels, EngineConfig's own default count and the grid
// docs/recordings.md measured the KF4FIC files on: 1.465 Hz bins and a 0.68 s
// window, so at 30 rows a second every sample is in 20.48 frames. That is the
// case the recordings showed churning, and it is also the case where the old
// 65536-sample block was exactly the window.
constexpr std::array<RateCase, 5> kRates{{
    {"2.4 MS/s", 2'400'000, 64},
    {"250 kS/s", 250'000, 32},
    {"96 kS/s", 96'000, 16},
    {"96k M64", 96'000, 64},
    {"48 kS/s", 48'000, 8},
}};

enum class Cadence : std::uint8_t { Contiguous, Rows30, Block65536 };

constexpr std::array<Cadence, 3> kCadences{Cadence::Contiguous, Cadence::Rows30,
                                           Cadence::Block65536};

[[nodiscard]] const char* cadence_name(Cadence cadence) {
    switch (cadence) {
        case Cadence::Contiguous: return "contiguous";
        case Cadence::Rows30: return "30 rows/s";
        case Cadence::Block65536: return "65536 block";
    }
    return "?";
}

// Coarse-channel blocks between frames for one cadence. The 30 rows/s rule is
// Engine::open_source's, restated so the synthetic cases need no device: the
// largest whole number of decimations at or under rate / rows, and the block
// the source already had when that is not smaller.
[[nodiscard]] std::uint32_t hop_blocks_of(const RateCase& rate, Cadence cadence) {
    const std::size_t d = rate.channels / 2;
    switch (cadence) {
        case Cadence::Contiguous: return 0;
        case Cadence::Block65536: return static_cast<std::uint32_t>(kLegacyBlock / d);
        case Cadence::Rows30: {
            const double hop = static_cast<double>(rate.rate) / kRowsPerSecond;
            const std::size_t decimations =
                std::max<std::size_t>(1, static_cast<std::size_t>(hop / static_cast<double>(d)));
            const std::size_t block = std::min(decimations * d, kLegacyBlock);
            return static_cast<std::uint32_t>(block / d);
        }
    }
    return 0;
}

[[nodiscard]] test::SceneGeometry geometry_of(const RateCase& rate, Cadence cadence) {
    test::SceneGeometry geometry;
    geometry.rate = rate.rate;
    geometry.channels = rate.channels;
    geometry.transform = 2048;
    geometry.hop_blocks = hop_blocks_of(rate, cadence);
    return geometry;
}

// Windows on each sample: the window over the hop.
[[nodiscard]] double overlap_of(const test::SceneGeometry& geometry) {
    return static_cast<double>(geometry.window_samples()) /
           static_cast<double>(geometry.frame_step());
}

[[nodiscard]] detect::DetectorConfig config_of(const RateCase& rate, double threshold_db) {
    detect::DetectorConfig config;
    config.source_rate = rate.rate;
    config.source_center = 0;
    config.grid_channels = rate.channels;
    config.detection_threshold_db = threshold_db;
    return config;
}

// How many independent frames the averaged spectrum is worth, read off the
// spectrum itself: the averaged power over the estimated floor, bin by bin,
// has a relative spread of 1/sqrt(K). Median and MAD rather than mean and
// variance, so a real signal on a recording does not count as spread.
struct Spread {
    double k = 0.0;
    double ratio_median = 0.0;
};

[[nodiscard]] Spread spread_of(std::span<const double> average, std::span<const double> floor,
                               std::vector<double>& scratch) {
    scratch.resize(average.size());
    for (std::size_t i = 0; i < average.size(); ++i) {
        scratch[i] = floor[i] > 0.0 ? average[i] / floor[i] : 0.0;
    }
    const auto middle = scratch.begin() + static_cast<std::ptrdiff_t>(scratch.size() / 2);
    std::nth_element(scratch.begin(), middle, scratch.end());
    const double median = *middle;
    for (double& value : scratch) {
        value = std::abs(value - median);
    }
    std::nth_element(scratch.begin(), middle, scratch.end());
    const double sigma = 1.4826 * *middle;
    Spread out;
    out.ratio_median = median;
    out.k = sigma > 0.0 ? (median / sigma) * (median / sigma) : 0.0;
    return out;
}

// What K would be if every frame counted as independent: the exponential
// average's effective length, (2 - alpha) / alpha, at the hop's alpha.
[[nodiscard]] double k_if_independent(const test::SceneGeometry& geometry, double tau) {
    const double hop = static_cast<double>(geometry.frame_step()) /
                       static_cast<double>(geometry.rate);
    const double alpha = 1.0 - std::exp(-hop / tau);
    return (2.0 - alpha) / alpha;
}

// ---- noise alone --------------------------------------------------------------

// The default and four below it. Nothing is born on noise at the default at
// any cadence, so the lower ones are what show the tail moving.
constexpr std::array<double, 5> kNoiseThresholds{6.0, 3.0, 0.0, -3.0, -6.0};

struct NoiseResult {
    double overlap = 0.0;
    double k_measured = 0.0;
    double k_independent = 0.0;
    double ratio_median = 0.0;
    std::uint64_t decisions = 0;
    double seconds = 0.0;
    std::array<std::uint64_t, kNoiseThresholds.size()> births{};
    std::array<std::uint64_t, kNoiseThresholds.size()> candidates{};
};

[[nodiscard]] NoiseResult run_noise(const RateCase& rate, Cadence cadence, double seconds,
                                    std::uint64_t seed) {
    siggen::SceneSpec spec;
    spec.rate = rate.rate;
    spec.center_hz = 0;
    spec.duration_samples =
        static_cast<dsp::SampleIndex>(std::llround(seconds * static_cast<double>(rate.rate)));
    spec.seed = seed;
    spec.noise_power_full_band_dbfs = kNoiseDbfs;
    spec.worker_threads = 1;

    const test::SceneGeometry geometry = geometry_of(rate, cadence);
    auto frames = test::SceneFrames::create(geometry, spec);
    INFO(message_of(frames));
    REQUIRE(frames.has_value());

    std::vector<detect::Detector> detectors;
    for (const double threshold : kNoiseThresholds) {
        auto made = detect::Detector::create(config_of(rate, threshold), frames->spectrum());
        INFO(message_of(made));
        REQUIRE(made.has_value());
        detectors.push_back(std::move(*made));
    }

    NoiseResult out;
    out.overlap = overlap_of(geometry);
    out.k_independent = k_if_independent(geometry, detect::DetectorConfig{}.average_seconds);

    std::vector<double> scratch;
    double k_sum = 0.0;
    double ratio_sum = 0.0;
    std::size_t k_count = 0;
    std::uint64_t seen = 0;

    const std::uint64_t available = frames->frames_available();
    for (std::uint64_t f = 0; f < available; ++f) {
        auto frame = frames->next();
        REQUIRE(frame.has_value());
        for (detect::Detector& detector : detectors) {
            REQUIRE(detector.consume(*frame).has_value());
        }
        const detect::Detector& reference = detectors.front();
        if (reference.stats().decisions == seen) {
            continue;
        }
        seen = reference.stats().decisions;

        // Past three time constants, so the bias-corrected average has
        // settled onto its steady-state length.
        const double at = static_cast<double>(reference.last_decision()) /
                          static_cast<double>(rate.rate);
        if (at < 3.0 * detect::DetectorConfig{}.average_seconds) {
            continue;
        }
        const Spread spread =
            spread_of(reference.averaged_power(), reference.noise_floor(), scratch);
        k_sum += spread.k;
        ratio_sum += spread.ratio_median;
        ++k_count;
    }

    out.k_measured = k_count > 0 ? k_sum / static_cast<double>(k_count) : 0.0;
    out.ratio_median = k_count > 0 ? ratio_sum / static_cast<double>(k_count) : 0.0;
    out.decisions = detectors.front().stats().decisions;
    out.seconds = seconds;
    for (std::size_t t = 0; t < detectors.size(); ++t) {
        out.births[t] = detectors[t].stats().tracks_born;
        out.candidates[t] = detectors[t].stats().candidates;
    }
    return out;
}

// ---- emitters -----------------------------------------------------------------

struct Kind {
    const char* name;
    siggen::ModulatorSpec spec;
};

// Where every emitter sits, alone in its scene. Two kilohertz is off every
// grid's channel seam at these rates, and off DC.
//
// ALONE, AND THE FIRST DRAFT OF THIS FILE SHOWS WHY. It put all four kinds in
// one scene 9 to 12 kHz apart, and at 2.4 MS/s and 96 kS/s the ladder's widest
// rungs, 300 kHz and 12 kHz there, took two or four of them as one band at
// 3 dB each: a candidate over the cluster clears the threshold when none of its
// members does, and an emitter credited with that track read as detected below
// the threshold. At 48 kS/s the widest rung is 6 kHz and could not, so the
// rates were not even being asked the same question.
constexpr dsp::Hertz kEmitterOffsetHz = 2'000;

// Four shapes the detector treats differently: one line, a carrier with two
// sidebands, a filled band and a Bessel comb.
[[nodiscard]] std::vector<Kind> kinds_at(dsp::SampleRate rate) {
    std::vector<Kind> kinds;
    {
        siggen::ModulatorSpec spec;
        spec.kind = siggen::Modulation::Am;
        spec.common.rate = rate;
        spec.common.carrier_offset = kEmitterOffsetHz;
        spec.common.seed = 21;
        spec.am.modulation_index = 0.0;
        kinds.push_back(Kind{"carrier", spec});
    }
    {
        siggen::ModulatorSpec spec;
        spec.kind = siggen::Modulation::Am;
        spec.common.rate = rate;
        spec.common.carrier_offset = kEmitterOffsetHz;
        spec.common.seed = 22;
        spec.am.modulation_index = 0.8;
        kinds.push_back(Kind{"am tone", spec});
    }
    {
        siggen::ModulatorSpec spec;
        spec.kind = siggen::Modulation::Qpsk;
        spec.common.rate = rate;
        spec.common.carrier_offset = kEmitterOffsetHz;
        spec.common.seed = 23;
        spec.psk.symbol_rate = 2'400.0;
        spec.psk.rolloff = 0.35;
        spec.psk.symbol_count = 4096;
        kinds.push_back(Kind{"qpsk 2k4", spec});
    }
    {
        siggen::ModulatorSpec spec;
        spec.kind = siggen::Modulation::Nfm;
        spec.common.rate = rate;
        spec.common.carrier_offset = kEmitterOffsetHz;
        spec.common.seed = 24;
        spec.nfm.deviation = 2'500;
        spec.nfm.tone_hz = 1'000;
        kinds.push_back(Kind{"nfm 2k5", spec});
    }
    return kinds;
}

// In the 2500 Hz reference, in half decibels across the default 6 dB
// threshold, where a statistic with more variance than it should have would
// show as a shallower step, and one strong level for the time to detection.
constexpr std::array<double, 6> kSnrs{5.0, 5.5, 6.0, 6.5, 7.0, 12.0};
constexpr double kOnSeconds = 3.0;
constexpr double kSceneSeconds = 12.0;

// Pd is counted from this long after the emitter starts, so it measures a
// settled average rather than the fill.
constexpr double kSettleSeconds = 2.0;

struct EmitterResult {
    double snr_2500_db = 0.0;
    std::size_t decisions_on = 0;
    std::size_t decisions_live = 0;
    double first_live_seconds = -1.0;
    std::size_t ids = 0;
    double longest_run_fraction = 0.0;
};

// One emitter of one kind, alone, on from kOnSeconds to the end.
[[nodiscard]] EmitterResult run_emitter(const RateCase& rate, Cadence cadence, const Kind& kind,
                                        double snr_2500_db, std::uint64_t seed) {
    siggen::SceneSpec spec;
    spec.rate = rate.rate;
    spec.center_hz = 0;
    spec.duration_samples = static_cast<dsp::SampleIndex>(
        std::llround(kSceneSeconds * static_cast<double>(rate.rate)));
    spec.seed = seed;
    spec.noise_power_full_band_dbfs = kNoiseDbfs;
    spec.worker_threads = 1;

    siggen::EmitterPlacement placement;
    placement.modulator = kind.spec;
    placement.use_snr = true;
    placement.snr_in_occupied_bandwidth_db = snr_2500_db;
    placement.start_sample = static_cast<dsp::SampleIndex>(
        std::llround(kOnSeconds * static_cast<double>(rate.rate)));
    placement.end_sample = spec.duration_samples;
    spec.emitters.push_back(placement);

    // siggen places an emitter by its SNR in its own truth band, which is a
    // different width for each kind, so the band is read off a first build
    // and the level set again to put the SNR in 2500 Hz where it was asked.
    {
        auto probe = siggen::Scene::create(spec);
        INFO(message_of(probe));
        REQUIRE(probe.has_value());
        REQUIRE(probe->truth().size() == 1);
        const auto band = static_cast<double>(probe->truth().front().extent.bandwidth_hz());
        REQUIRE(band > 0.0);
        spec.emitters.front().snr_in_occupied_bandwidth_db =
            snr_2500_db + 10.0 * std::log10(detect::kReferenceBandwidthHz / band);
    }

    const test::SceneGeometry geometry = geometry_of(rate, cadence);
    auto frames = test::SceneFrames::create(geometry, spec);
    INFO(message_of(frames));
    REQUIRE(frames.has_value());
    REQUIRE(frames->scene().truth().size() == 1);

    auto made = detect::Detector::create(
        config_of(rate, detect::DetectorConfig{}.detection_threshold_db), frames->spectrum());
    INFO(message_of(made));
    REQUIRE(made.has_value());
    detect::Detector& detector = *made;

    const siggen::EmitterTruth& truth = frames->scene().truth().front();
    const auto low = static_cast<double>(truth.extent.low_hz);
    const auto high = static_cast<double>(truth.extent.high_hz);

    // A track describes this emitter when it overlaps the truth band and is
    // not more than twice as wide plus a few bins: wider than that is a band
    // the search grew over noise, and crediting it is how the first draft
    // went wrong. See kEmitterOffsetHz.
    const double widest = 2.0 * (high - low) + 4.0 * frames->spectrum().bin_width_hz();

    // The noise in 2500 Hz, which the truth record's mean power is stated
    // against, so the SNR printed is the one the detector's threshold means.
    const double noise_2500 = std::pow(10.0, kNoiseDbfs / 10.0) * detect::kReferenceBandwidthHz /
                              static_cast<double>(rate.rate);

    EmitterResult out;
    out.snr_2500_db = 10.0 * std::log10(truth.mean_power / noise_2500);
    std::set<std::uint64_t> ids;
    std::uint64_t run_id = 0;
    std::size_t run_length = 0;
    std::size_t best_run = 0;
    std::size_t since_first = 0;

    std::uint64_t seen = 0;
    const std::uint64_t available = frames->frames_available();
    for (std::uint64_t f = 0; f < available; ++f) {
        auto frame = frames->next();
        REQUIRE(frame.has_value());
        REQUIRE(detector.consume(*frame).has_value());
        if (detector.stats().decisions == seen) {
            continue;
        }
        seen = detector.stats().decisions;
        const double at =
            static_cast<double>(detector.last_decision()) / static_cast<double>(rate.rate);
        if (at < kOnSeconds) {
            continue;
        }

        std::uint64_t live_id = 0;
        double live_overlap = 0.0;
        for (const detect::Track& track : detector.tracks()) {
            const double half = 0.5 * static_cast<double>(track.bandwidth);
            const double track_low = static_cast<double>(track.center) - half;
            const double track_high = static_cast<double>(track.center) + half;
            const double overlap = std::min(high, track_high) - std::max(low, track_low);
            if (overlap < 0.0 || track_high - track_low > widest) {
                continue;
            }
            ids.insert(track.id);
            if (track.state == detect::TrackState::Live && overlap >= live_overlap) {
                live_overlap = overlap;
                live_id = track.id;
            }
        }

        if (live_id != 0 && out.first_live_seconds < 0.0) {
            out.first_live_seconds = at - kOnSeconds;
        }
        if (out.first_live_seconds >= 0.0) {
            ++since_first;
            if (live_id != 0 && live_id == run_id) {
                ++run_length;
            } else {
                run_id = live_id;
                run_length = live_id != 0 ? 1 : 0;
            }
            best_run = std::max(best_run, run_length);
        }
        if (at >= kOnSeconds + kSettleSeconds) {
            ++out.decisions_on;
            if (live_id != 0) {
                ++out.decisions_live;
            }
        }
    }

    out.ids = ids.size();
    out.longest_run_fraction =
        since_first > 0 ? static_cast<double>(best_run) / static_cast<double>(since_first) : 0.0;
    return out;
}

// Every rate, and every cadence where the cadences differ.
struct Run {
    const RateCase* rate;
    Cadence cadence;
};

[[nodiscard]] std::vector<Run> runs() {
    std::vector<Run> out;
    for (const RateCase& rate : kRates) {
        // A cadence whose hop is one already run is the same run twice.
        std::vector<std::uint32_t> hops;
        for (const Cadence cadence : kCadences) {
            const std::uint32_t hop = hop_blocks_of(rate, cadence);
            const std::uint32_t effective = hop != 0 ? hop : 2048U;
            if (std::find(hops.begin(), hops.end(), effective) != hops.end()) {
                continue;
            }
            hops.push_back(effective);
            out.push_back(Run{&rate, cadence});
        }
    }
    return out;
}

}  // namespace

TEST_CASE("overlap: the averaged spectrum and false alarms on noise alone", "[.overlap]") {
    constexpr double kSeconds = 60.0;
    const std::array<std::uint64_t, 2> seeds{20260923, 20260924};

    std::println("");
    std::println("noise alone, {} s x {} seeds, default detector apart from the threshold",
                 kSeconds, seeds.size());
    std::println("{:<10} {:<12} {:>8} {:>9} {:>9} {:>9}   births a minute at 6, 3, 0, -3, -6 dB",
                 "rate", "cadence", "overlap", "K meas", "K indep", "median");
    const char* only = std::getenv("REVENANT_OVERLAP_RATE");
    for (const Run& run : runs()) {
        if (only != nullptr && std::string(only) != run.rate->name) {
            continue;
        }
        NoiseResult total;
        for (const std::uint64_t seed : seeds) {
            const NoiseResult one = run_noise(*run.rate, run.cadence, kSeconds, seed);
            total.overlap = one.overlap;
            total.k_independent = one.k_independent;
            total.k_measured += one.k_measured / static_cast<double>(seeds.size());
            total.ratio_median += one.ratio_median / static_cast<double>(seeds.size());
            total.decisions += one.decisions;
            total.seconds += one.seconds;
            for (std::size_t t = 0; t < kNoiseThresholds.size(); ++t) {
                total.births[t] += one.births[t];
                total.candidates[t] += one.candidates[t];
            }
        }
        const double minutes = total.seconds / 60.0;
        std::string births;
        std::string candidates;
        for (std::size_t t = 0; t < kNoiseThresholds.size(); ++t) {
            births += std::format(" {:>7.2f}", static_cast<double>(total.births[t]) / minutes);
            candidates += std::format(" {:>7.4f}", static_cast<double>(total.candidates[t]) /
                                                       static_cast<double>(total.decisions));
        }
        std::println("{:<10} {:<12} {:>8.2f} {:>9.1f} {:>9.1f} {:>9.4f}  {}", run.rate->name,
                     cadence_name(run.cadence), total.overlap, total.k_measured,
                     total.k_independent, total.ratio_median, births);
        std::println("{:<10} {:<12} candidates per decision {} over {} decisions", "", "",
                     candidates, total.decisions);
    }
}

TEST_CASE("overlap: detection probability, time to first detection and track stability",
          "[.overlap]") {
    const std::array<std::uint64_t, 2> seeds{4242, 4243};
    const char* only = std::getenv("REVENANT_OVERLAP_RATE");

    std::println("");
    std::println("one emitter per scene, on from {} s to {} s, {} seeds; Pd from {} s after "
                 "the start",
                 kOnSeconds, kSceneSeconds, seeds.size(), kSettleSeconds);
    std::println("{:<10} {:<12} {:<9} {:>7} {:>6} {:>9} {:>6} {:>7}", "rate", "cadence", "kind",
                 "snr", "Pd", "first s", "ids", "run");
    for (const Run& run : runs()) {
        if (only != nullptr && std::string(only) != run.rate->name) {
            continue;
        }
        for (const Kind& kind : kinds_at(run.rate->rate)) {
            for (const double snr : kSnrs) {
                EmitterResult sum;
                sum.first_live_seconds = 0.0;
                std::size_t found = 0;
                for (const std::uint64_t seed : seeds) {
                    const EmitterResult one = run_emitter(*run.rate, run.cadence, kind, snr, seed);
                    sum.snr_2500_db += one.snr_2500_db / static_cast<double>(seeds.size());
                    sum.decisions_on += one.decisions_on;
                    sum.decisions_live += one.decisions_live;
                    sum.ids += one.ids;
                    sum.longest_run_fraction +=
                        one.longest_run_fraction / static_cast<double>(seeds.size());
                    if (one.first_live_seconds >= 0.0) {
                        sum.first_live_seconds += one.first_live_seconds;
                        ++found;
                    }
                }
                const double pd = sum.decisions_on > 0
                                      ? static_cast<double>(sum.decisions_live) /
                                            static_cast<double>(sum.decisions_on)
                                      : 0.0;
                // A star marks a mean over the seeds that detected it at all.
                const std::string first =
                    found == 0 ? std::string("never")
                               : std::format("{:.2f}{}",
                                             sum.first_live_seconds / static_cast<double>(found),
                                             found < seeds.size() ? "*" : "");
                std::println("{:<10} {:<12} {:<9} {:>7.1f} {:>6.2f} {:>9} {:>6.1f} {:>7.2f}",
                             run.rate->name, cadence_name(run.cadence), kind.name,
                             sum.snr_2500_db, pd, first,
                             static_cast<double>(sum.ids) / static_cast<double>(seeds.size()),
                             sum.longest_run_fraction);
            }
        }
    }
}

TEST_CASE("overlapping frames leave the detector where contiguous ones put it", "[detect]") {
    // 96 kS/s on 16 channels, the KF4FIC grid: at 30 rows a second the hop is
    // 3200 samples against a 16384-sample window, so every sample is in five
    // frames. The control is the same grid with the hop at the window.
    const RateCase& rate = kRates[2];
    REQUIRE(rate.rate == 96'000);
    const test::SceneGeometry overlapped = geometry_of(rate, Cadence::Rows30);
    REQUIRE(overlapped.frame_step() == 3'200);
    REQUIRE(overlapped.window_samples() == 16'384);

    const NoiseResult contiguous_noise = run_noise(rate, Cadence::Contiguous, 20.0, 20260923);
    const NoiseResult overlapped_noise = run_noise(rate, Cadence::Rows30, 20.0, 20260923);
    INFO(std::format("K contiguous {:.1f}, overlapped {:.1f}, overlapped frames if independent "
                     "{:.1f}",
                     contiguous_noise.k_measured, overlapped_noise.k_measured,
                     overlapped_noise.k_independent));

    // Nothing on noise at the default threshold, either way.
    CHECK(contiguous_noise.births.front() == 0);
    CHECK(overlapped_noise.births.front() == 0);

    // The overlap buys averaging, measured about 12 frames' worth contiguous
    // and 34 overlapped, and it buys less than the frame count says, 60.
    // The second is why no rule in the detector may count frames, and it is
    // also what says these frames really do overlap.
    CHECK(overlapped_noise.k_measured > 2.0 * contiguous_noise.k_measured);
    CHECK(overlapped_noise.k_measured < 0.75 * overlapped_noise.k_independent);

    // A filled band a decibel over the threshold: found as often, as soon or
    // sooner, and as one track.
    const Kind qpsk = kinds_at(rate.rate)[2];
    REQUIRE(std::string(qpsk.name) == "qpsk 2k4");
    const EmitterResult contiguous = run_emitter(rate, Cadence::Contiguous, qpsk, 7.0, 4242);
    const EmitterResult overlapping = run_emitter(rate, Cadence::Rows30, qpsk, 7.0, 4242);
    INFO(std::format("Pd {}/{} contiguous, {}/{} overlapped; first {:.2f} s and {:.2f} s",
                     contiguous.decisions_live, contiguous.decisions_on,
                     overlapping.decisions_live, overlapping.decisions_on,
                     contiguous.first_live_seconds, overlapping.first_live_seconds));
    const auto pd = [](const EmitterResult& r) {
        return r.decisions_on > 0 ? static_cast<double>(r.decisions_live) /
                                        static_cast<double>(r.decisions_on)
                                  : 0.0;
    };
    CHECK(pd(contiguous) >= 0.9);
    CHECK(pd(overlapping) >= pd(contiguous) - 0.02);
    REQUIRE(contiguous.first_live_seconds >= 0.0);
    REQUIRE(overlapping.first_live_seconds >= 0.0);
    CHECK(overlapping.first_live_seconds <= contiguous.first_live_seconds);
    CHECK(overlapping.ids == 1);
}

TEST_CASE("the detector decides no more often than one spectrum window", "[detect]") {
    // A window is 1 / bin width. On every grid the constants were calibrated
    // on it is shorter than the 0.1 s interval and nothing changes; on the
    // slow grids it is longer and it is the interval.
    struct Expect {
        const RateCase* rate;
        double seconds;
    };
    const std::array<Expect, 5> expect{{
        {&kRates[0], 0.1},
        {&kRates[1], 32'768.0 / 250'000.0},
        {&kRates[2], 16'384.0 / 96'000.0},
        {&kRates[3], 65'536.0 / 96'000.0},
        {&kRates[4], 8'192.0 / 48'000.0},
    }};
    for (const Expect& e : expect) {
        const test::SceneGeometry geometry = geometry_of(*e.rate, Cadence::Rows30);
        auto made = detect::Detector::create(config_of(*e.rate, 6.0), geometry.spectrum());
        REQUIRE(made.has_value());
        INFO(e.rate->name);
        CHECK(std::abs(made->decision_seconds() - e.seconds) < 1.0e-12);
    }

    // And a configured interval longer than the window is kept as it is.
    detect::DetectorConfig slow = config_of(kRates[3], 6.0);
    slow.decision_interval_seconds = 2.0;
    auto made = detect::Detector::create(slow, geometry_of(kRates[3], Cadence::Rows30).spectrum());
    REQUIRE(made.has_value());
    CHECK(made->decision_seconds() == 2.0);
}

TEST_CASE("overlap: the KF4FIC excerpts through the engine at both row rates",
          "[.overlap-recordings]") {
    // docs/recordings.md: sixty seconds from 600 s of each of the six files,
    // and centre= the midpoint of the filename's band, which is an assumption
    // that only moves the absolute frequencies.
    const std::filesystem::path folder = "C:/Users/vexam/projects/SDR Recordings/excerpts";
    struct Excerpt {
        const char* file;
        dsp::Hertz center;
    };
    const std::array<Excerpt, 6> excerpts{{
        {"kf4fic_7000_7300_1359_60s.wav", 7'150'000},
        {"kf4fic_7000_7300_1501_60s.wav", 7'150'000},
        {"kf4fic_7000_7300_1603_60s.wav", 7'150'000},
        {"kf4fic_14000_14350_1359_60s.wav", 14'175'000},
        {"kf4fic_14000_14350_1501_60s.wav", 14'175'000},
        {"kf4fic_14000_14350_1603_60s.wav", 14'175'000},
    }};
    if (!std::filesystem::exists(folder)) {
        SKIP("the KF4FIC excerpts are not on this machine");
    }

    std::println("");
    std::println("{:<34} {:<3} {:<12} {:>6} {:>7} {:>6} {:>7} {:>7} {:>7} {:>7} {:>7} {:>8}",
                 "excerpt", "M", "cadence", "block", "overlap", "K", "born", "dropped", "merges",
                 "splits", "live", "decided");
    // Zero is what revenant-engine passes, and the resolution request then
    // opens these on 16 channels at 5.86 Hz. 64 is EngineConfig's own default
    // and the grid docs/recordings.md measured on, 1.465 Hz, whose window is
    // 0.68 s: four times as many frames on every sample at the same hop.
    for (const std::uint32_t channels : {0U, 64U}) {
        for (const Excerpt& excerpt : excerpts) {
            for (const double rows : {0.0, kRowsPerSecond}) {
                engine::EngineConfig config;
                config.channels = channels;
                config.taps_per_branch = 17;
                config.ring_seconds = 0.5;
                config.block_samples = kLegacyBlock;
                config.gpu_index = -1;
                config.spectrum_transform = 2048;
                config.spectrum_rows_per_second = rows;

                auto created = engine::Engine::create(config);
                INFO(message_of(created));
                REQUIRE(created.has_value());
                engine::Engine& eng = **created;

                const std::string uri =
                    std::format("file:///{}/{}?center={}", folder.generic_string(), excerpt.file,
                                excerpt.center);
                const auto opened = eng.open_source(uri);
                INFO(uri);
                INFO(message_of(opened));
                REQUIRE(opened.has_value());

                const engine::EngineInfo& info = eng.info();
                detect::DetectorConfig detector_config;
                detector_config.source_rate = info.source_rate;
                detector_config.source_center = info.source_center;
                detector_config.grid_channels = info.grid.channels;
                auto made = detect::Detector::create(detector_config, info.spectrum);
                INFO(message_of(made));
                REQUIRE(made.has_value());
                detect::Detector& detector = *made;

                std::vector<double> scratch;
                double k_sum = 0.0;
                std::size_t k_count = 0;
                std::size_t live_sum = 0;
                std::uint64_t seen = 0;
                const auto sink = [&](const engine::SpectrumFrame& frame) -> Status {
                    if (auto fed = detector.consume(frame); !fed) {
                        return fed;
                    }
                    if (detector.stats().decisions == seen) {
                        return {};
                    }
                    seen = detector.stats().decisions;
                    for (const detect::Track& track : detector.tracks()) {
                        live_sum += track.state == detect::TrackState::Live ? 1U : 0U;
                    }
                    const double at = static_cast<double>(detector.last_decision()) /
                                      static_cast<double>(info.source_rate);
                    if (at >= 3.0) {
                        k_sum +=
                            spread_of(detector.averaged_power(), detector.noise_floor(), scratch).k;
                        ++k_count;
                    }
                    return {};
                };
                REQUIRE(eng.set_spectrum_sink(sink).has_value());
                const auto ran = eng.run();
                INFO(message_of(ran));
                REQUIRE(ran.has_value());

                const detect::DetectorStats& stats = detector.stats();
                const double window = static_cast<double>(info.spectrum.transform) *
                                      static_cast<double>(info.grid.decimation);
                std::println("{:<34} {:<3} {:<12} {:>6} {:>7.2f} {:>6.1f} {:>7} {:>7} {:>7} {:>7} "
                             "{:>7.2f} {:>8}",
                             excerpt.file, info.grid.channels, rows > 0.0 ? "30 rows/s" : "65536 block",
                             info.block_samples, window / static_cast<double>(info.block_samples),
                             k_count > 0 ? k_sum / static_cast<double>(k_count) : 0.0,
                             stats.tracks_born, stats.tracks_dropped, stats.merges, stats.splits,
                             stats.decisions > 0 ? static_cast<double>(live_sum) /
                                                       static_cast<double>(stats.decisions)
                                                 : 0.0,
                             stats.decisions);
            }
        }
    }
}
