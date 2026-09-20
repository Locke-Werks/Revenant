// The detector scored against a synthetic scene, emitter by emitter.
//
// test_detector.cpp asks whether a rectangle of known width comes back at the
// right width. This file asks the question that rectangle cannot answer: what
// happens to a real modulated signal whose interior is thirty decibels of
// contrast rather than a flat top, and what happens to one that starts and
// stops.
//
// The scene is a bandwidth ladder. Every emitter is the same mode family and
// the same SNR, and the only thing that changes down the ladder is occupied
// bandwidth, from a CW carrier at fifty hertz to a hundred and fifty kilohertz
// block standing in for a broadcast station. A fault that depends on
// bandwidth shows up as a trend down that ladder, which one signal at one
// moment cannot show at all.
//
// The measurement cases are tagged [.scene] and hidden, because the host
// channelizer twin costs about a second and a half of wall time per second of
// scene. Run them by name, or by tag:
//
//   revenant_detect_tests.exe "[scene]"
//
// The regression case below is not hidden and runs a shorter scene.

#include <array>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/detect/detector.h"
#include "core/dsp/synth/modulators.h"
#include "core/dsp/synth/wideband.h"
#include "scene_frames.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 2'400'000;

// The noise floor the emitters are placed against. Any level works, since
// every emitter is placed by its ratio to it; -60 dBFS full band is siggen's
// own default and is what the URI produces.
constexpr double kNoiseDbfs = -60.0;

// Every emitter on the ladder sits at the same ratio in its own occupied
// bandwidth, so a difference in how the detector treats them is a difference
// of bandwidth and not of level. Twenty decibels is comfortably detectable at
// the default six decibel threshold and is not so loud that its skirts become
// the story.
constexpr double kLadderSnrDb = 20.0;

struct LadderRung {
    const char* name;
    dsp::Hertz offset_hz;
    siggen::ModulatorSpec spec;
    dsp::Hertz expected_bandwidth_hz;
};

// The three rolloffs core/dsp/synth/wideband.cpp's random population picks
// from, which is the set a scene can actually contain.
//
// The bar below used to fix this at 0.35 and certify one of the three. A
// root raised cosine occupies (1 + rolloff) times the symbol rate, so the
// other two are a different width for the same baud AND a different skirt
// shape, and the second of those is what a detector growing a seed outward
// has to cope with. Certifying the middle one and calling the rule proved
// is how a bar reads as settled while two thirds of the cases go untested.
inline constexpr std::array<double, 3> kRolloffs{0.2, 0.35, 0.5};

[[nodiscard]] dsp::Hertz psk_bandwidth_of(double symbol_rate, double rolloff) {
    return static_cast<dsp::Hertz>(std::llround((1.0 + rolloff) * symbol_rate));
}

[[nodiscard]] siggen::ModulatorSpec psk_of(double symbol_rate, dsp::Hertz offset,
                                           std::uint64_t seed, double rolloff = 0.35) {
    siggen::ModulatorSpec spec;
    spec.kind = siggen::Modulation::Qpsk;
    spec.common.rate = kRate;
    spec.common.carrier_offset = offset;
    spec.common.seed = seed;
    spec.psk.symbol_rate = symbol_rate;
    spec.psk.rolloff = rolloff;
    spec.psk.symbol_count = 4096;
    return spec;
}

// The ladder.
//
// QPSK carries most of it on purpose. A root raised cosine spectrum is filled
// across its whole occupied band, which is what a broadcast station integrated
// over a second looks like, and its width is one number. The two tone-driven
// modes at the ends are there because they are not filled: a CW carrier is one
// bin and a tone-modulated FM signal is a Bessel comb with nulls between its
// lines, and a detector that only works on filled blocks has to be caught
// saying so.
[[nodiscard]] std::vector<LadderRung> ladder(double rolloff = 0.35) {
    std::vector<LadderRung> rungs;

    {
        siggen::ModulatorSpec spec;
        spec.kind = siggen::Modulation::Cw;
        spec.common.rate = kRate;
        spec.common.carrier_offset = -1'000'000;
        spec.common.seed = 11;
        spec.cw.words_per_minute = 20.0;
        rungs.push_back(LadderRung{"cw 20wpm", -1'000'000, spec, 50});
    }
    {
        siggen::ModulatorSpec spec;
        spec.kind = siggen::Modulation::Usb;
        spec.common.rate = kRate;
        spec.common.carrier_offset = -820'000;
        spec.common.seed = 12;
        rungs.push_back(LadderRung{"usb voice", -820'000, spec, 2'700});
    }
    rungs.push_back(LadderRung{"qpsk 4k8", -640'000, psk_of(4'800.0, -640'000, 13, rolloff),
                               psk_bandwidth_of(4'800.0, rolloff)});
    {
        siggen::ModulatorSpec spec;
        spec.kind = siggen::Modulation::Nfm;
        spec.common.rate = kRate;
        spec.common.carrier_offset = -440'000;
        spec.common.seed = 14;
        spec.nfm.deviation = 5'000;
        spec.nfm.tone_hz = 3'000;
        rungs.push_back(LadderRung{"nfm 16k", -440'000, spec, 16'000});
    }
    rungs.push_back(LadderRung{"qpsk 20k", -200'000, psk_of(20'000.0, -200'000, 15, rolloff),
                               psk_bandwidth_of(20'000.0, rolloff)});
    rungs.push_back(LadderRung{"qpsk 56k", 100'000, psk_of(56'000.0, 100'000, 16, rolloff),
                               psk_bandwidth_of(56'000.0, rolloff)});
    rungs.push_back(LadderRung{"qpsk 111k", 480'000, psk_of(111'000.0, 480'000, 17, rolloff),
                               psk_bandwidth_of(111'000.0, rolloff)});
    {
        siggen::ModulatorSpec spec;
        spec.kind = siggen::Modulation::Nfm;
        spec.common.rate = kRate;
        spec.common.carrier_offset = 900'000;
        spec.common.seed = 18;
        spec.nfm.deviation = 65'000;
        spec.nfm.tone_hz = 10'000;
        rungs.push_back(LadderRung{"nfm 150k comb", 900'000, spec, 150'000});
    }

    return rungs;
}

// Every rung on for the whole scene.
[[nodiscard]] siggen::SceneSpec continuous_scene(double seconds, double rolloff = 0.35) {
    siggen::SceneSpec spec;
    spec.rate = kRate;
    spec.center_hz = 0;
    spec.duration_samples =
        static_cast<dsp::SampleIndex>(std::llround(seconds * static_cast<double>(kRate)));
    spec.seed = 4242;
    spec.noise_power_full_band_dbfs = kNoiseDbfs;
    spec.worker_threads = 1;

    for (const LadderRung& rung : ladder(rolloff)) {
        siggen::EmitterPlacement placement;
        placement.modulator = rung.spec;
        placement.use_snr = true;
        placement.snr_in_occupied_bandwidth_db = kLadderSnrDb;
        placement.start_sample = 0;
        placement.end_sample = spec.duration_samples;
        spec.emitters.push_back(placement);
    }
    return spec;
}

// Three wide stations and nothing else, laid out like the VHF band the radio
// sees at 98.1 MHz: one near each edge of the 2.4 MHz span and one in the
// middle. This is the scene that reproduces the reported fault, and it is
// separate from the ladder because the ladder deliberately holds only one wide
// emitter and the fault needs the frame to be mostly signal.
[[nodiscard]] siggen::SceneSpec broadcast_scene(double seconds, bool keyed) {
    siggen::SceneSpec spec;
    spec.rate = kRate;
    spec.center_hz = 98'100'000;
    spec.duration_samples =
        static_cast<dsp::SampleIndex>(std::llround(seconds * static_cast<double>(kRate)));
    spec.seed = 4242;
    spec.noise_power_full_band_dbfs = kNoiseDbfs;
    spec.worker_threads = 1;

    // Keyed, every station stops in the first sixty percent of the scene and
    // the three stops are staggered. The windows they leave behind are what
    // the post-stop bar scores, so each one has to be longer than the hold
    // plus the decisions the residual rule needs, and the shortest of the
    // three is the binding one. On-times are a little over three seconds,
    // which is three times the default average_seconds.
    const dsp::Hertz offsets[] = {-1'000'000, 0, 750'000};
    const double starts[] = {0.0, 0.10, 0.20};
    const double stops[] = {0.35, 0.45, 0.55};

    for (std::size_t i = 0; i < 3; ++i) {
        siggen::EmitterPlacement placement;
        placement.modulator = psk_of(111'000.0, offsets[i], 30 + i);
        placement.use_snr = true;
        // Louder than the ladder. A local broadcast station is not a marginal
        // detection, and the fault under investigation is one that gets worse
        // with level rather than better.
        placement.snr_in_occupied_bandwidth_db = 30.0;
        placement.start_sample =
            keyed ? static_cast<dsp::SampleIndex>(
                        std::llround(static_cast<double>(spec.duration_samples) * starts[i]))
                  : 0;
        placement.end_sample =
            keyed ? static_cast<dsp::SampleIndex>(
                        std::llround(static_cast<double>(spec.duration_samples) * stops[i]))
                  : spec.duration_samples;
        spec.emitters.push_back(placement);
    }
    return spec;
}

// The same ladder, keyed. Each rung transmits once, in a window chosen so the
// starts and the stops are staggered rather than simultaneous: a detector that
// only works when the whole band changes at once would pass a scene where
// everything keys together.
[[nodiscard]] siggen::SceneSpec keyed_scene(double seconds, double rolloff = 0.35) {
    siggen::SceneSpec spec = continuous_scene(seconds, rolloff);
    const auto total = static_cast<double>(spec.duration_samples);

    // Fractions of the scene. Every burst is at least a second long at the
    // durations these cases use, which is the detector's own integration time
    // and therefore the shortest transmission it can be asked about.
    const double starts[] = {0.10, 0.22, 0.05, 0.30, 0.15, 0.40, 0.08, 0.25};
    const double stops[] = {0.55, 0.80, 0.45, 0.95, 0.70, 0.90, 0.60, 0.75};

    for (std::size_t i = 0; i < spec.emitters.size(); ++i) {
        const std::size_t slot = i % (sizeof(starts) / sizeof(starts[0]));
        spec.emitters[i].start_sample =
            static_cast<dsp::SampleIndex>(std::llround(total * starts[slot]));
        spec.emitters[i].end_sample =
            static_cast<dsp::SampleIndex>(std::llround(total * stops[slot]));
    }
    return spec;
}

struct RunResult {
    std::vector<test::EmitterScore> scores;
    detect::DetectorStats stats;

    // The last decision's integrated spectrum and floor, kept so a case can
    // interrogate the search that produced the tracks rather than only its
    // output.
    std::vector<double> average;
    std::vector<double> floor;

    // The last decision's candidates, which are what the search accepted
    // before the tracker gave anything an identity. A fault in the search and
    // a fault in the tracker both end up as a long track list, and these
    // separate them.
    std::vector<detect::Candidate> candidates;
    std::size_t decisions = 0;

    // Wall time consume() cost, per frame and per decision, in milliseconds.
    //
    // The TEST reads a clock here. The detector still does not, and must not:
    // every interval inside it is a difference of sample indices, which is
    // what lets a replay at forty times realtime produce the same tracks.
    // This is the wall cost of running it, which is a different question and
    // is the one the peak budget trades against.
    double frame_ms = 0.0;
    double decision_ms = 0.0;
};

// Every frame of a scene, rendered once.
//
// Rendering is the expensive half and the detector is the cheap one, so a
// sweep over five configurations that re-rendered each time would spend all
// its time on the part that does not change. At the default geometry a five
// second scene is about forty megabytes of frames, against a render that
// costs several seconds each time.
class FrameCache {
public:
    [[nodiscard]] static FrameCache render(const test::SceneGeometry& geometry,
                                           const siggen::SceneSpec& spec) {
        auto frames = test::SceneFrames::create(geometry, spec);
        if (!frames) {
            FAIL("scene frames: " << frames.error().message);
        }

        FrameCache cache;
        cache.geometry_ = geometry;
        cache.spectrum_ = frames->spectrum();
        cache.bins_ = geometry.bins();
        cache.center_ = spec.center_hz;
        cache.scene_ = std::make_shared<siggen::Scene>(frames->scene());

        const std::uint64_t total = frames->frames_available();
        cache.power_.reserve(static_cast<std::size_t>(total) * cache.bins_);
        cache.starts_.reserve(static_cast<std::size_t>(total));
        for (std::uint64_t i = 0; i < total; ++i) {
            auto frame = frames->next();
            if (!frame) {
                FAIL("frame " << i << ": " << frame.error().message);
            }
            cache.power_.insert(cache.power_.end(), frame->power_db.begin(),
                                frame->power_db.end());
            cache.starts_.push_back(frame->start);
            cache.count_ = frame->count;
        }
        return cache;
    }

    // post_window_seconds is passed straight to the scorer: zero scores only
    // what happens while each emitter transmits, which is what every case
    // but the post-stop bar wants.
    [[nodiscard]] RunResult run(const detect::DetectorConfig& overrides,
                                double post_window_seconds = 0.0) const {
        detect::DetectorConfig config = overrides;
        config.source_rate = geometry_.rate;
        config.source_center = center_;
        config.grid_channels = geometry_.channels;

        auto made = detect::Detector::create(config, spectrum_);
        if (!made) {
            FAIL("detector: " << made.error().message);
        }
        detect::Detector& detector = *made;

        test::SceneScorer scorer(*scene_, center_, post_window_seconds);
        dsp::SampleIndex previous_decision = 0;
        RunResult result;
        double consume_ns = 0.0;

        for (std::size_t i = 0; i < starts_.size(); ++i) {
            engine::SpectrumFrame frame;
            frame.power_db = std::span<const float>(power_.data() + i * bins_, bins_);
            frame.geometry = spectrum_;
            frame.start = starts_[i];
            frame.count = count_;
            frame.sequence = i;

            const auto before = std::chrono::steady_clock::now();
            auto fed = detector.consume(frame);
            consume_ns += static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - before)
                    .count());
            if (!fed) {
                FAIL("consume " << i << ": " << fed.error().message);
            }
            if (detector.last_decision() != previous_decision) {
                previous_decision = detector.last_decision();
                scorer.observe(previous_decision, detector.tracks());
                ++result.decisions;
            }
        }

        result.scores = scorer.finish();
        result.stats = detector.stats();
        result.average.assign(detector.averaged_power().begin(), detector.averaged_power().end());
        result.floor.assign(detector.noise_floor().begin(), detector.noise_floor().end());
        result.candidates.assign(detector.candidates().begin(), detector.candidates().end());
        if (!starts_.empty()) {
            result.frame_ms = consume_ns / static_cast<double>(starts_.size()) / 1.0e6;
        }
        if (result.decisions > 0) {
            result.decision_ms = consume_ns / static_cast<double>(result.decisions) / 1.0e6;
        }
        return result;
    }

    [[nodiscard]] const test::SceneGeometry& geometry() const { return geometry_; }

private:
    test::SceneGeometry geometry_{};
    engine::SpectrumGeometry spectrum_{};
    std::shared_ptr<siggen::Scene> scene_;
    std::vector<float> power_;
    std::vector<dsp::SampleIndex> starts_;
    std::size_t bins_ = 0;
    dsp::SampleIndex count_ = 0;
    dsp::Hertz center_ = 0;
};

void print_scores(const std::string& title, const RunResult& result) {
    std::println("");
    std::println("{}", title);
    std::println("  {:>10} {:>6} {:>4} {:>4} {:>9} {:>6} {:>9} {:>6} {:>5} {:>5} {:>9} {:>9} {:>6}",
                 "truth BW", "mode", "ids", "peak", "best BW", "cover", "centre e", "spill",
                 "life", "seen", "step mean", "step max", "ovlap");
    for (const test::EmitterScore& score : result.scores) {
        const double seen =
            score.decisions_on > 0 ? static_cast<double>(score.decisions_detected) /
                                         static_cast<double>(score.decisions_on)
                                   : 0.0;
        std::println("  {:>10} {:>6} {:>4} {:>4} {:>9} {:>6.2f} {:>9} {:>6.2f} {:>5.2f} {:>5.2f} "
                     "{:>9.0f} {:>9.0f} {:>6.2f}",
                     score.truth_bandwidth_hz, siggen::modulation_name(score.modulation),
                     score.track_ids, score.peak_simultaneous, score.best_bandwidth_hz,
                     score.best_coverage, score.best_centre_error_hz, score.best_spill,
                     score.best_lifetime_fraction, seen, score.centre_step_mean_hz,
                     score.centre_step_max_hz, score.worst_self_overlap);
    }
    const double per_decision =
        result.decisions > 0
            ? static_cast<double>(result.stats.candidates) / static_cast<double>(result.decisions)
            : 0.0;
    std::println("  {} decisions, {} born, {} dropped, {} merges, {} splits, {:.1f} "
                 "candidates per decision, {} decisions overflowed the peak budget",
                 result.decisions, result.stats.tracks_born, result.stats.tracks_dropped,
                 result.stats.merges, result.stats.splits, per_decision,
                 result.stats.peaks_overflowed);
    std::println("  {:.3f} ms per frame, {:.1f} ms of it per decision (test clock, this machine)",
                 result.frame_ms, result.decision_ms);
}

// The same run, scored over the window after each emitter stops. Separate
// from print_scores because the two are answering different questions and a
// dozen columns of one answer is easier to read than twenty of both.
void print_post_stop(const std::string& title, const RunResult& result) {
    std::println("");
    std::println("{}", title);
    std::println("  {:>10} {:>6} {:>7} {:>7} {:>7} {:>6} {:>9} {:>7} {:>5} {:>4}", "truth BW",
                 "mode", "window", "outlive", "live", "held", "worst ce", "worst bw", "state",
                 "new");
    for (const test::EmitterScore& score : result.scores) {
        if (score.post_window_seconds <= 0.0) {
            continue;
        }
        std::println("  {:>10} {:>6} {:>7.2f} {:>7.2f} {:>7.2f} {:>6.2f} {:>9} {:>7.2f} {:>5} "
                     "{:>4}",
                     score.truth_bandwidth_hz, siggen::modulation_name(score.modulation),
                     score.post_window_seconds, score.residual_seconds,
                     score.after_live_seconds, score.after_held_seconds,
                     score.after_worst_centre_error_hz, score.after_worst_bandwidth_ratio,
                     detect::track_state_name(score.after_state_at_worst), score.after_new_ids);
    }
}

// The summed-bin search, recomputed outside the detector from the integrated
// spectrum it decided on.
//
// This is the instrument for the question the track list cannot answer: the
// detector publishes what its greedy pass accepted, not what the pass was
// choosing between. Printing the ladder's deflection at a band's own position,
// rung by rung, says whether the matched width was ever the strongest
// candidate there.
struct RungScore {
    std::uint32_t width = 0;
    double best_deflection = 0.0;
    std::size_t best_start = 0;
};

[[nodiscard]] std::vector<RungScore> ladder_at(const RunResult& result, std::size_t first_bin,
                                               std::size_t last_bin, std::uint32_t widest) {
    const std::size_t bins = result.average.size();
    std::vector<double> excess(bins + 1, 0.0);
    std::vector<double> floor(bins + 1, 0.0);
    for (std::size_t i = 0; i < bins; ++i) {
        excess[i + 1] = excess[i] + (result.average[i] - result.floor[i]);
        floor[i + 1] = floor[i] + result.floor[i];
    }

    std::vector<RungScore> rungs;
    for (std::uint32_t width = 1; width <= widest; width *= 2) {
        RungScore rung{.width = width, .best_deflection = 0.0, .best_start = 0};
        const double root = std::sqrt(static_cast<double>(width));

        // Any window whose centre falls inside the band, which is the set the
        // greedy pass would be choosing between for that band.
        const std::size_t half = width / 2;
        const std::size_t low = first_bin > half ? first_bin - half : 0;
        const std::size_t high = std::min(bins - width, last_bin);
        for (std::size_t s = low; s <= high; ++s) {
            const double summed_floor = floor[s + width] - floor[s];
            if (summed_floor <= 0.0) {
                continue;
            }
            const double deflection = (excess[s + width] - excess[s]) * root / summed_floor;
            if (deflection > rung.best_deflection) {
                rung.best_deflection = deflection;
                rung.best_start = s;
            }
        }
        rungs.push_back(rung);
    }
    return rungs;
}

// Bin index of an absolute frequency in the frame, clamped into it.
[[nodiscard]] std::size_t bin_of(const test::SceneGeometry& geometry, dsp::Hertz frequency,
                                 std::size_t bins) {
    const double bin_zero = static_cast<double>(geometry.spectrum().bin_zero_numerator) /
                            static_cast<double>(geometry.spectrum().bin_zero_denominator);
    const std::int64_t index =
        std::llround((static_cast<double>(frequency) - bin_zero) / geometry.bin_width_hz());
    return static_cast<std::size_t>(
        std::clamp<std::int64_t>(index, 0, static_cast<std::int64_t>(bins) - 1));
}

void print_ladder(const test::SceneGeometry& geometry, const RunResult& result) {
    std::println("");
    std::println("summing ladder at each emitter's own band, last decision");
    for (const test::EmitterScore& score : result.scores) {
        const std::size_t first = bin_of(geometry, score.truth_low_hz, result.average.size());
        const std::size_t last = bin_of(geometry, score.truth_high_hz, result.average.size());
        const std::vector<RungScore> rungs = ladder_at(result, first, last, 8192);

        std::string line = std::format("  {:>7} Hz ({:>4} bins):", score.truth_bandwidth_hz,
                                       last - first + 1);
        double best = 0.0;
        std::uint32_t best_width = 0;
        for (const RungScore& rung : rungs) {
            line += std::format(" {}:{:.0f}", rung.width, rung.best_deflection);
            if (rung.best_deflection > best) {
                best = rung.best_deflection;
                best_width = rung.width;
            }
        }
        std::println("{}  winner {} bins", line, best_width);
    }
}

// Every peak the detector's scale-space search would generate at one
// decision, recomputed here from the integrated spectrum it decided on.
//
// This exists to show, rather than assert, what the peak budget does. The
// detector keeps max_peaks of these; which ones it keeps is a choice, and the
// two possible choices give completely different answers on a frame that is
// mostly signal. Recomputing the whole set outside the detector is the only
// way to see the set it was choosing from.
[[nodiscard]] std::vector<RungScore> all_peaks(const RunResult& result, double bin_width_hz,
                                               double threshold_db, std::uint32_t widest) {
    const std::size_t bins = result.average.size();
    std::vector<double> excess(bins + 1, 0.0);
    std::vector<double> floor(bins + 1, 0.0);
    for (std::size_t i = 0; i < bins; ++i) {
        excess[i + 1] = excess[i] + (result.average[i] - result.floor[i]);
        floor[i + 1] = floor[i] + result.floor[i];
    }
    const double threshold_linear = std::pow(10.0, threshold_db / 10.0);

    std::vector<RungScore> peaks;
    std::vector<double> deflection(bins, 0.0);
    for (std::uint32_t width = 1; width <= widest && width <= bins; width *= 2) {
        const std::size_t last = bins - width;
        const double root = std::sqrt(static_cast<double>(width));
        const double bar =
            threshold_linear * detect::kReferenceBandwidthHz / (root * bin_width_hz);

        for (std::size_t s = 0; s <= last; ++s) {
            const double summed_floor = floor[s + width] - floor[s];
            deflection[s] =
                summed_floor > 0.0 ? (excess[s + width] - excess[s]) * root / summed_floor : 0.0;
        }
        for (std::size_t s = 0; s <= last; ++s) {
            if (!(deflection[s] > bar)) {
                continue;
            }
            if (s > 0 && deflection[s - 1] >= deflection[s]) {
                continue;
            }
            if (s < last && deflection[s + 1] > deflection[s]) {
                continue;
            }
            peaks.push_back(
                RungScore{.width = width, .best_deflection = deflection[s], .best_start = s});
        }
    }
    return peaks;
}

// Candidates falling inside one emitter's band, which says what the search
// handed the tracker rather than what the tracker made of it.
void print_candidates(const RunResult& result, dsp::Hertz low, dsp::Hertz high,
                      const char* what) {
    std::string line = std::format("  {} candidates in [{}, {}]:", what, low, high);
    std::size_t count = 0;
    for (const detect::Candidate& candidate : result.candidates) {
        const dsp::Hertz candidate_low = candidate.center - candidate.bandwidth / 2;
        const dsp::Hertz candidate_high = candidate.center + candidate.bandwidth / 2;
        if (candidate_high < low || candidate_low > high) {
            continue;
        }
        ++count;
        if (count <= 20) {
            line += std::format(" [{}..{}]", candidate_low, candidate_high);
        }
    }
    std::println("{}  ({} total)", line, count);
}

}  // namespace

TEST_CASE("bandwidth ladder, every emitter on for the whole scene", "[.scene][detect]") {
    const test::SceneGeometry geometry;
    const siggen::SceneSpec spec = continuous_scene(5.0);
    const FrameCache cache = FrameCache::render(geometry, spec);

    detect::DetectorConfig config;
    config.detection_threshold_db = 6.0;

    const RunResult result = cache.run(config);
    print_scores("continuous ladder, 6 dB threshold", result);
    print_ladder(geometry, result);

    CHECK(result.decisions > 0);
}

TEST_CASE("bandwidth ladder, every emitter keyed once", "[.scene][detect]") {
    const test::SceneGeometry geometry;
    const siggen::SceneSpec spec = keyed_scene(8.0);
    const FrameCache cache = FrameCache::render(geometry, spec);

    detect::DetectorConfig config;
    config.detection_threshold_db = 6.0;

    const RunResult result = cache.run(config);
    print_scores("keyed ladder, 6 dB threshold", result);
    CHECK(result.decisions > 0);
}

// Three broadcast stations on the shipped geometry, which is the scene the
// radio's own reading is against.
TEST_CASE("three wide stations across the whole span", "[.scene][detect]") {
    const test::SceneGeometry geometry;
    const siggen::SceneSpec spec = broadcast_scene(5.0, false);
    const FrameCache cache = FrameCache::render(geometry, spec);

    detect::DetectorConfig config;
    config.detection_threshold_db = 6.0;

    const RunResult result = cache.run(config);
    print_scores("three stations, 6 dB threshold", result);
    print_candidates(result, 98'100'000 - 80'000, 98'100'000 + 80'000, "middle station");
    print_ladder(geometry, result);
    CHECK(result.decisions > 0);
}

// What the peak budget spends itself on, and why the order it is spent in
// decides whether a broadcast station is found.
//
// The detector keeps max_peaks peaks out of however many the ladder produces.
// It used to keep the first that many, walking the ladder narrowest rung
// first; it now keeps the strongest that many. This case prints both answers
// over the same peak set, so the difference is arithmetic on the page rather
// than a claim in a comment.
TEST_CASE("the peak budget, spent in arrival order and spent on the strongest",
          "[.scene][detect]") {
    const test::SceneGeometry geometry;
    const siggen::SceneSpec spec = broadcast_scene(5.0, false);
    const FrameCache cache = FrameCache::render(geometry, spec);

    detect::DetectorConfig config;
    config.detection_threshold_db = 6.0;
    const RunResult result = cache.run(config);

    const std::uint32_t widest =
        static_cast<std::uint32_t>(std::bit_floor(result.average.size() / 8));
    std::vector<RungScore> peaks =
        all_peaks(result, geometry.bin_width_hz(), config.detection_threshold_db, widest);

    std::println("");
    std::println("three stations, one decision: {} peaks from the ladder, budget {}",
                 peaks.size(), config.max_peaks);

    // Arrival order is ladder order: every peak at width 1, then width 2, and
    // so on. The truncation therefore lands at whatever rung the budget runs
    // out on, and every wider rung is never searched at all.
    std::size_t taken = 0;
    std::uint32_t widest_in_arrival_order = 0;
    for (std::uint32_t width = 1; width <= widest; width *= 2) {
        std::size_t at_width = 0;
        for (const RungScore& peak : peaks) {
            if (peak.width == width) {
                ++at_width;
            }
        }
        if (taken + at_width > config.max_peaks) {
            break;
        }
        taken += at_width;
        widest_in_arrival_order = width;
        std::println("    width {:>5}: {:>6} peaks, running total {:>6}", width, at_width, taken);
    }

    std::sort(peaks.begin(), peaks.end(), [](const RungScore& a, const RungScore& b) {
        return a.best_deflection > b.best_deflection;
    });
    std::uint32_t widest_in_best_n = 0;
    for (std::size_t i = 0; i < peaks.size() && i < config.max_peaks; ++i) {
        widest_in_best_n = std::max(widest_in_best_n, peaks[i].width);
    }

    std::println("  widest rung reached in arrival order: {} bins ({:.1f} kHz)",
                 widest_in_arrival_order,
                 static_cast<double>(widest_in_arrival_order) * geometry.bin_width_hz() / 1000.0);
    std::println("  widest rung kept by strongest-first:  {} bins ({:.1f} kHz)",
                 widest_in_best_n,
                 static_cast<double>(widest_in_best_n) * geometry.bin_width_hz() / 1000.0);

    // A 150 kHz station at 36.6 Hz per bin is about 4096 bins, so the rung
    // that fits it is the one this has to reach.
    const double station_bins = 149'850.0 / geometry.bin_width_hz();
    std::println("  the station is {:.0f} bins wide, so its matched rung is {} bins",
                 station_bins, std::bit_floor(static_cast<std::size_t>(station_bins)));

    CHECK(peaks.size() > config.max_peaks);
}

// What the budget costs when it binds, which is the one thing the new rule
// does not fix on its own.
//
// Keeping the strongest peaks cannot lose a wide signal to a narrow one, and
// it can still lose a WEAK signal to a loud one: a frame carrying something
// very peaky spends the budget on that emitter's own windows. Raising the
// budget is the lever, and this says what raising it buys.
TEST_CASE("what the peak budget evicts", "[.scene][detect]") {
    const test::SceneGeometry geometry;
    const siggen::SceneSpec spec = continuous_scene(5.0);
    const FrameCache cache = FrameCache::render(geometry, spec);

    for (const std::uint32_t budget : {4096U, 16384U, 65536U, 262144U}) {
        detect::DetectorConfig config;
        config.detection_threshold_db = 6.0;
        config.max_peaks = budget;
        const RunResult result = cache.run(config);
        print_scores(std::format("continuous ladder, max_peaks {}", budget), result);
        CHECK(result.decisions > 0);
    }
}

// THE BAR, as a pass rate over a seeded scene.
//
// An emitter of known extent should read as roughly one track of roughly
// that extent, for roughly as long as it transmits. Stated per emitter so a
// failure names which one, and over a keyed scene so the answer covers
// starting and stopping rather than only steady state.
//
// Scored over the FILLED emitters only, and that exclusion is a property of
// the generator rather than a concession from the detector. A truth record's
// extent is the channel the mode occupies: for QPSK that is where the energy
// is, and for a tone-driven AM, SSB or FM emitter it is not. siggen's
// tone-driven modes put all their power in a handful of discrete lines,
// which a detector correctly reports as a handful of discrete lines, and
// scoring that against a Carson-rule bandwidth would be marking the detector
// down for being right. The behaviour is still measured, by the [.scene]
// cases above; it is not turned into a threshold here.
TEST_CASE("an emitter of known extent reads as one track of that extent", "[detect][scene-bar]") {
    const test::SceneGeometry geometry;

    // Swept over every rolloff the generator can produce, not just the
    // middle one. A root raised cosine at 0.5 is both wider for the same
    // baud and more gradual at its shoulders than one at 0.2, and a
    // detector that grows a seed outward from a peak meets that difference
    // directly. Certifying 0.35 alone left two thirds of the reachable
    // shapes unmeasured while the bar read as settled.
    // Known failures, and the bar is a ratchet in BOTH directions: a new one
    // fails CI, and so does fixing one of these without striking it out.
    //
    // Empty since 2026-09-20. It held {0.5, 30000} and {0.5, 166500}: at
    // rolloff 0.5 those two emitters each came back as three tracks at once,
    // coverage 0.77 and 0.83, spill 0.00. Both were the same fault, and it
    // was in the seed-growth edge rule rather than in the split or the trim.
    // Growth stopped at a fraction of the seed's OWN mean excess, which on a
    // 20 dB emitter is 27.7 to 35.2 times the noise floor, so it stopped
    // inside the transition skirt a root raised cosine at 0.5 spends two
    // thirds of its occupied band in, and each shoulder was then found again
    // as its own candidate. Growing on a multiple of the local noise floor
    // instead collapses both to one track: see edge_floor_fraction in
    // detector.h.
    //
    // Coverage at rolloff 0.5 now reads 0.84 to 0.85 on all four rungs,
    // against 0.77 and 0.83 before. That is the ceiling and not a shortfall:
    // integrating the raised-cosine power spectrum, the symmetric interval
    // holding 99 percent of the power is 0.845 of (1 + rolloff) / T at
    // rolloff 0.5, so occupied_power_fraction is what sets it and the 0.70
    // floor below keeps 0.145 of headroom at the worst rolloff.
    const std::vector<std::pair<double, dsp::Hertz>> kKnownSplits{};

    std::vector<std::pair<double, dsp::Hertz>> failures;
    std::size_t scored = 0;
    std::size_t passed = 0;
    for (const double rolloff : kRolloffs) {
    const siggen::SceneSpec spec = keyed_scene(6.0, rolloff);
    const FrameCache cache = FrameCache::render(geometry, spec);

    detect::DetectorConfig config;
    config.detection_threshold_db = 6.0;
    const RunResult result = cache.run(config);
    print_scores(std::format("the bar, keyed ladder at 6 dB, rolloff {:.2f}", rolloff), result);

    for (const test::EmitterScore& score : result.scores) {
        if (score.modulation != siggen::Modulation::Qpsk) {
            continue;
        }
        ++scored;
        const double seen =
            score.decisions_on > 0 ? static_cast<double>(score.decisions_detected) /
                                         static_cast<double>(score.decisions_on)
                                   : 0.0;
        const bool ok =
            score.track_ids == 1 && score.peak_simultaneous == 1 && score.best_coverage >= 0.70 &&
            score.best_spill <= 0.25 && seen >= 0.85 && score.best_lifetime_fraction >= 0.85 &&
            std::abs(static_cast<double>(score.best_centre_error_hz)) <=
                0.10 * static_cast<double>(score.truth_bandwidth_hz);
        if (ok) {
            ++passed;
            continue;
        }
        failures.emplace_back(rolloff, score.truth_bandwidth_hz);
        // Named rather than counted, so a regression says which bandwidth
        // stopped working instead of only that the rate fell.
        WARN(std::format("rolloff {:.2f}: emitter of {} Hz failed the bar: {} ids, {} at once, "
                         "cover {:.2f}, spill {:.2f}, seen {:.2f}, life {:.2f}, "
                         "centre error {} Hz",
                         rolloff, score.truth_bandwidth_hz, score.track_ids,
                         score.peak_simultaneous, score.best_coverage, score.best_spill, seen,
                         score.best_lifetime_fraction, score.best_centre_error_hz));
    }
    }

    // Four QPSK rungs from 6.5 kHz to 150 kHz, every one of them keyed on and
    // off inside the scene. All four passed when this was written; the bar is
    // all of them, because three out of four means one bandwidth stopped
    // working and the scene is small enough to say which.
    std::ranges::sort(failures);
    INFO(std::format("{} of {} filled emitters passed across {} rolloffs", passed, scored,
                     kRolloffs.size()));
    for (const auto& [rolloff, width] : failures) {
        INFO(std::format("  failed: rolloff {:.2f}, {} Hz", rolloff, width));
    }
    CHECK(scored == 4 * kRolloffs.size());
    CHECK(failures == kKnownSplits);
}

// THE OTHER HALF OF THE BAR: what a track reports once its signal stops.
//
// The bar above scores each emitter while it transmits and stops there,
// which is where the instrument stopped too: SceneScorer::observe returned
// early on every decision outside the transmission, so nothing in the tree
// had ever looked at the window after a stop. broadcast_scene has taken a
// `keyed` flag since it was written and both call sites passed false, so the
// scene this case needs already existed and had never been instantiated.
//
// Three real stations, 111 kbaud QPSK at 30 dB in a 149.85 kHz occupied
// bandwidth, which is 47.7 dB in the 2500 Hz reference. That level is the
// point: the post-stop tail is (snr - threshold) over the decay rate, so the
// loudest station leaves the longest ghost and a marginal one would have
// shown nothing.
//
// Measured on this scene before the residual rule, at the shipped
// average_seconds of 1.0: the first station stayed LIVE for 9.06 s after it
// stopped and Held for a further 2.95 s, twelve seconds of published row
// against a three second hold. Every one of the 83 Live decisions carried
// confidence 1.00 and silent_samples() of 0.00 s, so no consumer could tell
// the row was stale, and the geometry frozen into the hold was taken at the
// lowest residual of the burst: 5783 Hz of centre error and 125023 Hz of
// bandwidth against 129857 Hz measured on signal.
//
// Measured here 2026-09-20. With the residual rule on, the three stations
// outlive their signals by 3.19, 3.28 and 3.27 s, of which 0.24, 0.33 and
// 0.32 s are Live and 2.95 s is the hold, at worst centre errors of 12, -19
// and -3 Hz and a bandwidth ratio of 1.00 on all three. With it off, by
// setting residual_decisions to zero, all three are still Live and still
// published when the four second window closes and nine of the twenty-two
// assertions below fail. That is what makes this a bar and not a report.
TEST_CASE("a stopped emitter stops being published", "[detect][scene-bar]") {
    const test::SceneGeometry geometry;

    // Ten seconds so the last station stops at 5.5 s and the shortest
    // post-stop window is 4.5 s. The window has to be longer than the whole
    // answer or residual_seconds is a lower bound and the case proves
    // nothing, which is what still_published_at_window_end is checked for.
    const siggen::SceneSpec spec = broadcast_scene(10.0, true);
    const FrameCache cache = FrameCache::render(geometry, spec);

    detect::DetectorConfig config;
    config.detection_threshold_db = 6.0;
    const double window = 4.0;
    const RunResult result = cache.run(config, window);
    print_scores("the post-stop bar, three keyed broadcast stations", result);
    print_post_stop("the post-stop bar, after each station stops", result);

    // How long a row may outlive its signal, and where the number is from.
    //
    // The hold is deliberate and stays: detector.h fixes it at three seconds
    // for FT8's 2.4 second silence, and a row that vanishes inside a gap
    // cannot be clicked between transmissions. So the hold is the floor.
    //
    // On top of it, the residual rule needs residual_decisions consecutive
    // falls to be sure, and the first fall cannot be judged until a second
    // decision exists. Three further intervals are allowed for the decision
    // that straddles the stop, for the ramp as the average starts to empty,
    // and for one run reset on noise. Anything past that is the row
    // outliving its evidence, which is the defect this case exists for.
    const double allowed =
        config.bootstrap_hold_seconds +
        static_cast<double>(config.residual_decisions + 4) * config.decision_interval_seconds;

    std::size_t scored = 0;
    for (const test::EmitterScore& score : result.scores) {
        if (score.post_window_seconds <= 0.0) {
            continue;
        }
        ++scored;
        INFO(std::format("emitter {} of {} Hz: outlived its signal by {:.2f} s ({:.2f} live, "
                         "{:.2f} held) in a {:.2f} s window, worst centre error {} Hz at {}, "
                         "worst bandwidth ratio {:.2f}, last ratio {:.2f}, {} new ids, "
                         "published at {} of {} decisions",
                         score.emitter_id, score.truth_bandwidth_hz, score.residual_seconds,
                         score.after_live_seconds, score.after_held_seconds,
                         score.post_window_seconds, score.after_worst_centre_error_hz,
                         detect::track_state_name(score.after_state_at_worst),
                         score.after_worst_bandwidth_ratio, score.after_last_bandwidth_ratio,
                         score.after_new_ids, score.after_published, score.after_decisions));

        // The window has to be big enough to hold the answer, or every
        // figure below is truncated rather than measured.
        CHECK_FALSE(score.still_published_at_window_end);
        CHECK(score.residual_seconds <= allowed);

        // The Live part of the tail is the part nothing on the wire marks:
        // silent_samples() is zero and confidence is rising for as long as
        // the row is Live, so a client has no way to fade it or to know not
        // to trust it. It has to be the short end of the split.
        CHECK(score.after_live_seconds < config.bootstrap_hold_seconds);

        // Geometry a client can act on must come from a decision that saw
        // the signal, because a detection row is clickable and a click tunes
        // a receiver to that centre at that bandwidth. A tenth of the
        // emitter's own bandwidth is the same figure the live bar uses for
        // centre error, and it is about ten kilohertz here, which is a
        // receiver still inside a 150 kHz station rather than next to it.
        CHECK(std::abs(static_cast<double>(score.after_worst_centre_error_hz)) <=
              0.10 * static_cast<double>(score.truth_bandwidth_hz));

        // A factor of two either way. Not tighter, because a stopped signal
        // has a decision or two of decay under it before the rule fires and
        // the measurement moves a little; not looser, because the bandwidth
        // sizes both the click target and the waterfall rectangle, and a
        // band off by more than that draws a box over history the signal was
        // never that wide in.
        CHECK(score.after_worst_bandwidth_ratio >= 0.5);
        CHECK(score.after_worst_bandwidth_ratio <= 2.0);

        // Nothing may be born out of the residual once the first track is
        // dropped. This is the check that forced the suppression onto the
        // candidate rather than onto the track: a track-level rule leaves
        // the candidate free, so a fresh id appears in the same band a few
        // decisions after each drop and the band never goes quiet.
        CHECK(score.after_new_ids == 0);
    }
    CHECK(scored == 3);
}

// Where the wide end stops working, on the shipped grid.
//
// Two limits sit above a broadcast station and neither is the peak budget.
// The summing ladder's widest rung is an eighth of the frame, and the noise
// floor's window is noise_knots by noise_window_knots wide, which detector.h
// says a signal must not fill much over half of or it hides its own floor.
// At 65536 bins and 36.6 Hz those are 300 kHz and about 360 kHz. This walks
// an emitter up to and past them so the ceiling is a measured number rather
// than an inference from two constants.
TEST_CASE("how wide an emitter can be before it comes apart", "[.scene][detect]") {
    const test::SceneGeometry geometry;

    // (1 + rolloff) times the symbol rate, so these are 135, 270, 337 and
    // 405 kHz: inside the widest rung, at it, past it, and well past it.
    for (const double baud : {100'000.0, 200'000.0, 250'000.0, 300'000.0}) {
        siggen::SceneSpec spec;
        spec.rate = kRate;
        spec.duration_samples =
            static_cast<dsp::SampleIndex>(std::llround(4.0 * static_cast<double>(kRate)));
        spec.seed = 4242;
        spec.noise_power_full_band_dbfs = kNoiseDbfs;
        spec.worker_threads = 1;

        siggen::EmitterPlacement placement;
        placement.modulator = psk_of(baud, 0, 77);
        placement.use_snr = true;
        placement.snr_in_occupied_bandwidth_db = 30.0;
        placement.start_sample = 0;
        placement.end_sample = spec.duration_samples;
        spec.emitters.push_back(placement);

        const FrameCache cache = FrameCache::render(geometry, spec);
        detect::DetectorConfig config;
        config.detection_threshold_db = 6.0;
        const RunResult result = cache.run(config);

        const double bins = 1.35 * baud / geometry.bin_width_hz();
        const auto half = static_cast<dsp::Hertz>(std::llround(1.35 * baud * 0.6));
        print_scores(std::format("one emitter at {:.0f} baud, {:.0f} kHz, {:.0f} bins", baud,
                                 1.35 * baud / 1000.0, bins),
                     result);
        print_candidates(result, -half, half, "emitter");
        CHECK(result.decisions > 0);
    }
}

// Which rule fragments the comb.
//
// The fragmentation has two plausible causes and they need different fixes,
// so the scene is run several times over the same frames with one rule
// disabled at a time. Whichever variant stops the fragmentation names the
// rule that was doing it.
TEST_CASE("what fragments a signal with interior nulls", "[.scene][detect]") {
    const test::SceneGeometry geometry;
    const siggen::SceneSpec spec = continuous_scene(5.0);
    const FrameCache cache = FrameCache::render(geometry, spec);

    detect::DetectorConfig base;
    base.detection_threshold_db = 6.0;

    struct Variant {
        const char* name;
        detect::DetectorConfig config;
    };
    std::vector<Variant> variants;
    variants.push_back(Variant{"as shipped", base});

    {
        // The gap split switched off. If the band is being cut after it was
        // found, this restores it.
        detect::DetectorConfig config = base;
        config.split_gap_bins = 1'000'000;
        variants.push_back(Variant{"no gap split", config});
    }
    {
        // Growth allowed to run the whole span from its seed. If a narrow
        // seed is winning the ranking and then being unable to reach the rest
        // of its own signal, this restores it.
        detect::DetectorConfig config = base;
        config.edge_floor_fraction = 0.001;
        variants.push_back(Variant{"edge fraction 0.001", config});
    }
    {
        detect::DetectorConfig config = base;
        config.split_gap_bins = 1'000'000;
        config.edge_floor_fraction = 0.001;
        variants.push_back(Variant{"neither", config});
    }

    for (const Variant& variant : variants) {
        const RunResult result = cache.run(variant.config);
        print_scores(std::format("continuous ladder, {}", variant.name), result);
        print_candidates(result, 900'000 - 75'000, 900'000 + 75'000, "nfm 150k comb");
        CHECK(result.decisions > 0);
    }
}
