// The wideband detector, against frames whose contents are known exactly.
//
// Every case here builds its own spectrum frames rather than running the
// engine, and that is deliberate rather than a shortcut. A detector scored
// against a real capture can only be checked for plausibility: the answer to
// "is that 21 kHz wide" is another measurement with its own error. A
// synthetic frame has a true centre, a true bandwidth and a true SNR in the
// reference bandwidth, all three chosen rather than measured, so the
// assertions below are against arithmetic instead of against opinion.
//
// The frames are honest about the one thing that matters for a detector: bin
// power is EXPONENTIALLY distributed, because the power of one bin of a
// transform of complex Gaussian noise is. Filling a frame with a constant
// noise level would make every threshold in this file pass for the wrong
// reason, and the false-alarm cases would prove nothing at all.
//
// Randomness is seeded and the seed is printed with any failure, per
// docs/conventions.md. Nothing here reads a clock, including the detector:
// every interval is a difference of sample indices.
//
// WHAT THE CASES ARE FOR
//
//   The measurement cases check that a known signal comes back at the right
//   centre, the right bandwidth and the right SNR. The bandwidth one is the
//   sharp one: the summing ladder is powers of two, so a 21-bin signal is
//   seeded by a 16-bin window sitting somewhere arbitrary inside it, and only
//   the growth and trim steps turn that back into 21.
//
//   The reference-bandwidth case is the one that would fail silently if the
//   SNR conversion were wrong. A one-bin carrier and a forty-bin block with
//   the same SNR in 2500 Hz differ by about 16 dB at their peak bins. Both
//   must report the same SNR, because that is the whole claim
//   docs/snr-convention.md makes and the reason a single operator threshold
//   means one thing across the span.
//
//   The summed-bin case puts a signal on the frame whose per-bin excess is
//   about a decibel and asserts both that the detector finds it and that its
//   strongest bin is that far down. Passing the second half is what shows the
//   first half was not single-bin thresholding getting lucky.
//
//   The tracker cases drive the five rules docs/detection.md names: one to
//   one assignment, merge and split, birth, a bootstrap hold that is not per
//   classification, and hysteresis at a coarse channel boundary.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <print>
#include <random>
#include <string>
#include <vector>

#include "core/detect/detector.h"
#include "core/dsp/types.h"
#include "core/engine/engine.h"

using namespace revenant;
using Catch::Approx;

namespace {

// A small grid, sized so a case can be reasoned about by hand. Eight coarse
// channels at 2x oversampling and a 256-point second-stage transform is 1024
// fine bins across 1.024 MS/s, which works out at exactly 1000 Hz per bin and
// 128 kHz per coarse channel. The shipped configuration is sixty-four
// channels and 2048 points; nothing in the detector is sensitive to either
// number, and the CLI run against the radio covers the real one.
constexpr dsp::SampleRate kRate = 1'024'000;
constexpr std::uint32_t kChannels = 8;
constexpr std::uint32_t kTransform = 256;
constexpr std::size_t kBins = 1024;

// 8192 source samples between frames is 125 frames a second, which is close
// enough to the engine's 305 that the integration behaves the same and small
// enough that a few seconds of scene is a few hundred frames.
constexpr std::uint64_t kFrameStep = 8192;
constexpr double kFrameSeconds = static_cast<double>(kFrameStep) / static_cast<double>(kRate);

// Built the same way core/engine/graph.cpp builds it, from the same three
// numbers, so a test frequency and an engine frequency mean the same thing.
[[nodiscard]] engine::SpectrumGeometry test_geometry() {
    const auto decimation = static_cast<std::int64_t>(kChannels / 2);
    engine::SpectrumGeometry geometry;
    geometry.transform = kTransform;
    geometry.bins_per_channel = kTransform / 2;
    geometry.channels = kChannels;
    geometry.bins = kChannels * (kTransform / 2);
    geometry.bin_width_numerator = kRate;
    geometry.bin_width_denominator = decimation * static_cast<std::int64_t>(kTransform);
    geometry.bin_zero_numerator = -kRate * (static_cast<std::int64_t>(kChannels) + 1);
    geometry.bin_zero_denominator = 2 * static_cast<std::int64_t>(kChannels);
    return geometry;
}

// What a scene puts on the frame. Bandwidth is in fine bins so a case can
// state the answer it expects without a conversion in the way.
struct Emitter {
    std::size_t centre_bin = 0;
    std::size_t width_bins = 1;
    double snr_2500_db = 20.0;
};

// Produces frames of a scene, one at a time, advancing the sample index.
//
// The noise is drawn from -log(U) with U taken from the top bits of the
// generator rather than from std::exponential_distribution, whose output is
// implementation defined. A seed printed in a failure has to reproduce the
// failure on the machine that reads it.
class Scene {
public:
    Scene(double noise_dbfs, std::uint64_t seed)
        : geometry_(test_geometry()),
          noise_mean_(std::pow(10.0, noise_dbfs / 10.0)),
          generator_(seed),
          seed_(seed) {
        power_db_.assign(kBins, 0.0F);
        mean_.assign(kBins, noise_mean_);
    }

    void set(std::vector<Emitter> emitters) {
        std::fill(mean_.begin(), mean_.end(), noise_mean_);
        for (const Emitter& emitter : emitters) {
            // Total signal power from the SNR in the reference bandwidth.
            // The noise power spectral density is the per-bin mean over the
            // bin width, so the noise inside 2500 Hz is that times 2500 and
            // the signal power that hits a given SNR follows directly.
            const double total = std::pow(10.0, emitter.snr_2500_db / 10.0) * noise_mean_ *
                                 detect::kReferenceBandwidthHz / bin_width();
            const double per_bin = total / static_cast<double>(emitter.width_bins);
            const std::size_t first = emitter.centre_bin - emitter.width_bins / 2;
            for (std::size_t i = 0; i < emitter.width_bins; ++i) {
                if (first + i < kBins) {
                    mean_[first + i] += per_bin;
                }
            }
        }
    }

    void silence() { set({}); }

    [[nodiscard]] engine::SpectrumFrame next() {
        for (std::size_t i = 0; i < kBins; ++i) {
            power_db_[i] = static_cast<float>(10.0 * std::log10(mean_[i] * exponential()));
        }

        engine::SpectrumFrame frame;
        frame.power_db = power_db_;
        frame.geometry = geometry_;
        frame.start = index_;
        frame.count = kFrameStep;
        frame.sequence = sequence_++;
        index_ += kFrameStep;
        return frame;
    }

    [[nodiscard]] const engine::SpectrumGeometry& geometry() const { return geometry_; }
    [[nodiscard]] double bin_width() const { return geometry_.bin_width_hz(); }
    [[nodiscard]] double noise_mean() const { return noise_mean_; }
    [[nodiscard]] std::uint64_t seed() const { return seed_; }
    [[nodiscard]] dsp::SampleIndex index() const { return index_; }

    // Absolute frequency of a bin centre, with no source centre applied,
    // which is the frame the tests work in.
    [[nodiscard]] dsp::Hertz frequency_of(std::size_t bin) const {
        return static_cast<dsp::Hertz>(
            std::llround(geometry_.bin_zero_hz() + static_cast<double>(bin) * bin_width()));
    }

private:
    [[nodiscard]] double exponential() {
        // 53 bits of mantissa out of the generator, shifted off zero so the
        // logarithm is always finite.
        const double uniform =
            (static_cast<double>(generator_() >> 11) + 0.5) * (1.0 / 9007199254740992.0);
        return -std::log(uniform);
    }

    engine::SpectrumGeometry geometry_;
    double noise_mean_ = 0.0;
    std::mt19937_64 generator_;
    std::uint64_t seed_ = 0;

    std::vector<float> power_db_;
    std::vector<double> mean_;
    dsp::SampleIndex index_ = 0;
    std::uint64_t sequence_ = 0;
};

[[nodiscard]] detect::DetectorConfig base_config() {
    detect::DetectorConfig config;
    config.source_rate = kRate;
    config.source_center = 0;
    config.grid_channels = kChannels;
    config.average_seconds = 0.8;
    config.decision_interval_seconds = 0.08;
    config.detection_threshold_db = 6.0;
    return config;
}

// Feeds whole seconds of the current scene. Returns on the first failure so a
// broken frame is reported where it happened rather than five hundred frames
// later.
void run_for(detect::Detector& detector, Scene& scene, double seconds) {
    const auto frames = static_cast<std::size_t>(std::llround(seconds / kFrameSeconds));
    for (std::size_t i = 0; i < frames; ++i) {
        const engine::SpectrumFrame frame = scene.next();
        auto fed = detector.consume(frame);
        if (!fed) {
            FAIL("consume refused frame " << i << ": " << fed.error().message);
        }
    }
}

[[nodiscard]] const detect::Track* find_near(const detect::Detector& detector, dsp::Hertz centre,
                                             dsp::Hertz tolerance) {
    const detect::Track* best = nullptr;
    for (const detect::Track& track : detector.tracks()) {
        if (std::abs(track.center - centre) > tolerance) {
            continue;
        }
        if (best == nullptr || track.snr_2500_db > best->snr_2500_db) {
            best = &track;
        }
    }
    return best;
}

[[nodiscard]] std::string describe(const detect::Detector& detector) {
    std::string text = std::format("{} tracks", detector.tracks().size());
    for (const detect::Track& track : detector.tracks()) {
        text += std::format("\n  id {} {} at {} Hz, {} Hz wide, {:.1f} dB, confidence {:.2f}, "
                            "channel {}",
                            track.id, detect::track_state_name(track.state), track.center,
                            track.bandwidth, track.snr_2500_db, track.confidence, track.channel);
    }
    return text;
}

}  // namespace

TEST_CASE("the detector refuses a configuration it cannot honour", "[detect]") {
    const engine::SpectrumGeometry geometry = test_geometry();

    SECTION("no spectrum stage") {
        detect::DetectorConfig config = base_config();
        auto made = detect::Detector::create(config, engine::SpectrumGeometry{});
        REQUIRE_FALSE(made);
        CHECK(made.error().message.find("no bins") != std::string::npos);
    }

    SECTION("no source rate, which is the only clock it has") {
        detect::DetectorConfig config = base_config();
        config.source_rate = 0;
        auto made = detect::Detector::create(config, geometry);
        REQUIRE_FALSE(made);
        CHECK(made.error().message.find("source rate") != std::string::npos);
    }

    SECTION("a birth rule of zero, which is the false-alarm knob switched off") {
        detect::DetectorConfig config = base_config();
        config.birth_hits = 0;
        auto made = detect::Detector::create(config, geometry);
        REQUIRE_FALSE(made);
    }

    SECTION("a threshold that is not a number") {
        detect::DetectorConfig config = base_config();
        config.detection_threshold_db = std::nan("");
        auto made = detect::Detector::create(config, geometry);
        REQUIRE_FALSE(made);
    }
}

TEST_CASE("a frame from another geometry is refused rather than misread", "[detect]") {
    Scene scene(-90.0, 20260919);
    auto made = detect::Detector::create(base_config(), scene.geometry());
    REQUIRE(made);

    engine::SpectrumFrame frame = scene.next();
    frame.geometry.channels = 64;
    auto fed = made->consume(frame);
    REQUIRE_FALSE(fed);
    CHECK(made->stats().frames_rejected == 1);
}

TEST_CASE("noise alone at a sane threshold produces no tracks", "[detect]") {
    constexpr std::uint64_t kSeed = 4242;
    INFO("seed " << kSeed);

    Scene scene(-90.0, kSeed);
    scene.silence();

    auto made = detect::Detector::create(base_config(), scene.geometry());
    REQUIRE(made);
    detect::Detector& detector = *made;

    run_for(detector, scene, 4.0);

    INFO(describe(detector));
    CHECK(detector.tracks().empty());
    CHECK(detector.candidates().empty());
    CHECK(detector.stats().tracks_born == 0);

    // The floor has to be the floor. A detector that reported nothing because
    // its noise estimate had drifted twenty decibels high would pass the
    // check above and fail everything else, so the estimate is checked
    // directly against the level the scene was built at.
    const std::span<const double> floor = detector.noise_floor();
    REQUIRE(floor.size() == kBins);
    double worst = 0.0;
    for (const double estimate : floor) {
        worst = std::max(worst, std::abs(10.0 * std::log10(estimate / scene.noise_mean())));
    }
    INFO("worst noise floor error " << worst << " dB");
    CHECK(worst < 1.0);
}

TEST_CASE("a known signal comes back at the right centre and bandwidth", "[detect]") {
    constexpr std::uint64_t kSeed = 90210;
    INFO("seed " << kSeed);

    constexpr std::size_t kCentreBin = 300;
    constexpr std::size_t kWidthBins = 21;
    constexpr double kSnrDb = 20.0;

    Scene scene(-90.0, kSeed);
    scene.set({Emitter{.centre_bin = kCentreBin, .width_bins = kWidthBins, .snr_2500_db = kSnrDb}});

    auto made = detect::Detector::create(base_config(), scene.geometry());
    REQUIRE(made);
    detect::Detector& detector = *made;

    run_for(detector, scene, 4.0);

    INFO(describe(detector));
    REQUIRE(detector.tracks().size() == 1);
    const detect::Track& track = detector.tracks()[0];

    CHECK(track.state == detect::TrackState::Live);

    // Within a bin of the truth on both. The ladder seeds this with a 16-bin
    // window somewhere inside a 21-bin signal, so anything less than the
    // growth and trim steps working lands two or three bins out on the centre
    // and five short on the bandwidth.
    const dsp::Hertz expected_centre = scene.frequency_of(kCentreBin);
    const auto expected_width = static_cast<dsp::Hertz>(
        std::llround(static_cast<double>(kWidthBins) * scene.bin_width()));
    CHECK(std::abs(track.center - expected_centre) <= 1000);
    CHECK(std::abs(track.bandwidth - expected_width) <= 2000);

    CHECK(track.snr_2500_db == Approx(kSnrDb).margin(1.5));
    CHECK(track.confidence > 0.9);
    CHECK(track.age_samples() > 0);
    CHECK(track.silent_samples() == 0);
    CHECK(track.channel_valid);
}

// The width a weak signal comes back at, swept down through the level the
// growth rule stops at.
//
// This is the dimension the growth rule's sweep did not have. A flat
// emitter's per-bin excess over the noise floor IS its SNR in its own
// occupied bandwidth, exactly, so the growth level is what the signal's own
// level is measured against and every scene in this tree ran at 20 or 30 dB
// in band, where any plausible level sits far below the signal and can only
// ever stop growth in a skirt. There is no skirt on these rectangles at all,
// so the level either clears the whole emitter or none of it.
//
// Each rung holds the reported SNR at 12 dB and doubles the width, which
// takes the in-band figure down 3 dB a step: 5.2, 2.2, -0.8 and -3.9 dB.
// Every one of them is a plainly detectable signal, 12 dB over an operator
// threshold of 6, and the last two sit under their own noise floor per bin.
//
// Measured before the level was stated in the noise's ripple rather than its
// power, with edge_floor_fraction at 1.0: the 48-bin rung grew by exactly
// zero bins and came back at 32 kHz against 48 kHz of truth, a coverage of
// 0.67 against the 0.70 the scene bar enforces, and the 96-bin rung was worse.
// The crossover was 12.8 dB reported at 48 bins and 17.8 dB for a 149.85 kHz
// station on the shipped grid, so the whole weak-and-wide corner was lost.
TEST_CASE("a signal under its own noise floor per bin still comes back at its width",
          "[detect]") {
    constexpr std::uint64_t kSeed = 31337;
    INFO("seed " << kSeed);

    for (const std::size_t width : {std::size_t{12}, std::size_t{24}, std::size_t{48},
                                    std::size_t{96}}) {
        Scene scene(-90.0, kSeed);
        scene.set({Emitter{.centre_bin = 500, .width_bins = width, .snr_2500_db = 12.0}});

        auto made = detect::Detector::create(base_config(), scene.geometry());
        REQUIRE(made);
        detect::Detector& detector = *made;
        run_for(detector, scene, 4.0);

        const auto truth =
            static_cast<dsp::Hertz>(std::llround(static_cast<double>(width) * scene.bin_width()));
        const double in_band =
            12.0 - 10.0 * std::log10(static_cast<double>(width) * scene.bin_width() /
                                     detect::kReferenceBandwidthHz);

        INFO(std::format("{} bins, {} Hz of truth, {:.1f} dB in the occupied bandwidth", width,
                         truth, in_band));
        INFO(describe(detector));

        const detect::Track* track = find_near(detector, scene.frequency_of(500), truth);
        REQUIRE(track != nullptr);

        // The same coverage floor the scene bar uses, stated here as a
        // fraction of the truth width because a rectangle has no skirt to
        // lose to the 99 percent trim.
        CHECK(static_cast<double>(track->bandwidth) >= 0.70 * static_cast<double>(truth));
        CHECK(static_cast<double>(track->bandwidth) <= 1.30 * static_cast<double>(truth));
    }
}

TEST_CASE("three signals at once come back as three", "[detect]") {
    constexpr std::uint64_t kSeed = 13579;
    INFO("seed " << kSeed);

    Scene scene(-88.0, kSeed);
    scene.set({
        Emitter{.centre_bin = 150, .width_bins = 3, .snr_2500_db = 25.0},
        Emitter{.centre_bin = 470, .width_bins = 17, .snr_2500_db = 18.0},
        Emitter{.centre_bin = 820, .width_bins = 33, .snr_2500_db = 14.0},
    });

    auto made = detect::Detector::create(base_config(), scene.geometry());
    REQUIRE(made);
    detect::Detector& detector = *made;

    run_for(detector, scene, 4.0);

    INFO(describe(detector));
    CHECK(detector.tracks().size() == 3);
    for (const std::size_t bin : {std::size_t{150}, std::size_t{470}, std::size_t{820}}) {
        const detect::Track* track = find_near(detector, scene.frequency_of(bin), 2000);
        INFO("looking for the emitter at bin " << bin);
        REQUIRE(track != nullptr);
        CHECK(track->state == detect::TrackState::Live);
    }
}

TEST_CASE("a wide signal survives a peak budget too small to hold the narrow ones",
          "[detect]") {
    constexpr std::uint64_t kSeed = 20260919;
    INFO("seed " << kSeed);

    // One wide signal and four strong narrow ones. The narrow ones are what
    // floods the scale-space search: every bin of every signal that clears
    // the width-one threshold is a local maximum somewhere, and the ladder is
    // walked narrowest rung first, so the narrow rungs produce their peaks
    // before the wide rungs are ever reached.
    //
    // This is the case the budget used to lose. It kept the first max_peaks
    // peaks, which on a busy frame means only the narrowest rungs, and the
    // wide signal came back as a row of fragments a rung wide each. Measured
    // against an RTL-SDR at 98.1 MHz the same week: 103 tracks born in eight
    // seconds, a 164 kHz broadcast station read as about thirty tracks of
    // 5 kHz, and ids past two hundred. It keeps the STRONGEST max_peaks now,
    // and the widest rung that fits a signal is where its deflection peaks,
    // so the wide one can no longer be crowded out by narrow ones.
    // Wide for this frame, and no wider, for two reasons that both belong to
    // the 1024-bin test geometry rather than to the detector.
    //
    // The summing ladder's widest rung is an eighth of the frame, 128 bins
    // here, so a signal past that has no rung that fits it. And the noise
    // floor's window is noise_knots by noise_window_knots, 256 bins here, so
    // a signal filling much over half of one hides its own floor: at 201 bins
    // this reported 13.8 dB for a 40 dB signal and came back in three pieces,
    // which is the estimator behaving exactly as detector.h says it will.
    // Both limits scale with the frame, and on the shipped 65536-bin one they
    // land at 300 kHz and about 360 kHz, well past a broadcast station.
    constexpr std::size_t kWideCentre = 512;
    constexpr std::size_t kWideWidth = 97;

    Scene scene(-90.0, kSeed);
    scene.set({
        Emitter{.centre_bin = kWideCentre, .width_bins = kWideWidth, .snr_2500_db = 40.0},
        Emitter{.centre_bin = 80, .width_bins = 1, .snr_2500_db = 20.0},
        Emitter{.centre_bin = 200, .width_bins = 1, .snr_2500_db = 20.0},
        Emitter{.centre_bin = 850, .width_bins = 1, .snr_2500_db = 20.0},
        Emitter{.centre_bin = 950, .width_bins = 1, .snr_2500_db = 20.0},
    });

    const dsp::Hertz wide_centre = scene.frequency_of(kWideCentre);
    const auto wide_width =
        static_cast<dsp::Hertz>(std::llround(static_cast<double>(kWideWidth) * scene.bin_width()));

    SECTION("with a budget that cannot hold the frame") {
        detect::DetectorConfig config = base_config();
        config.max_peaks = 8;

        auto made = detect::Detector::create(config, scene.geometry());
        REQUIRE(made);
        detect::Detector& detector = *made;
        run_for(detector, scene, 4.0);

        INFO(describe(detector));

        // The budget binds, which is what makes this case mean anything: a
        // frame that never reached the bound would pass under either rule.
        CHECK(detector.stats().peaks_overflowed > 0);

        const detect::Track* wide = find_near(detector, wide_centre, wide_width);
        REQUIRE(wide != nullptr);

        // Most of its real width, rather than a fragment of it. Under the old
        // rule this came back at a rung's width or less.
        CHECK(wide->bandwidth > wide_width / 2);
    }

    SECTION("with the budget the frame size derives") {
        auto made = detect::Detector::create(base_config(), scene.geometry());
        REQUIRE(made);
        detect::Detector& detector = *made;
        run_for(detector, scene, 4.0);

        INFO(describe(detector));

        // A budget of one per bin does not bind on a frame with five signals
        // on it, so everything is found and the wide one is one track.
        CHECK(detector.stats().peaks_overflowed == 0);
        CHECK(detector.tracks().size() == 5);

        const detect::Track* wide = find_near(detector, wide_centre, wide_width);
        REQUIRE(wide != nullptr);
        CHECK(std::abs(wide->bandwidth - wide_width) <= 4000);

        for (const std::size_t bin : {std::size_t{80}, std::size_t{200}, std::size_t{850},
                                      std::size_t{950}}) {
            INFO("looking for the narrow emitter at bin " << bin);
            CHECK(find_near(detector, scene.frequency_of(bin), 2000) != nullptr);
        }
    }
}

TEST_CASE("SNR is reported in the reference bandwidth and compares across widths", "[detect]") {
    constexpr std::uint64_t kSeed = 24680;
    INFO("seed " << kSeed);
    constexpr double kSnrDb = 15.0;

    Scene scene(-90.0, kSeed);
    scene.set({
        Emitter{.centre_bin = 200, .width_bins = 1, .snr_2500_db = kSnrDb},
        Emitter{.centre_bin = 700, .width_bins = 41, .snr_2500_db = kSnrDb},
    });

    auto made = detect::Detector::create(base_config(), scene.geometry());
    REQUIRE(made);
    detect::Detector& detector = *made;

    run_for(detector, scene, 4.0);

    INFO(describe(detector));
    const detect::Track* carrier = find_near(detector, scene.frequency_of(200), 2000);
    const detect::Track* block = find_near(detector, scene.frequency_of(700), 4000);
    REQUIRE(carrier != nullptr);
    REQUIRE(block != nullptr);

    // The claim. Same SNR in 2500 Hz, reported the same, from two signals
    // whose loudest bins are sixteen decibels apart.
    CHECK(carrier->snr_2500_db == Approx(kSnrDb).margin(1.5));
    CHECK(block->snr_2500_db == Approx(kSnrDb).margin(1.5));

    const detect::Candidate* carrier_candidate = nullptr;
    const detect::Candidate* block_candidate = nullptr;
    for (const detect::Candidate& candidate : detector.candidates()) {
        if (std::abs(candidate.center - scene.frequency_of(200)) <= 2000) {
            carrier_candidate = &candidate;
        }
        if (std::abs(candidate.center - scene.frequency_of(700)) <= 4000) {
            block_candidate = &candidate;
        }
    }
    REQUIRE(carrier_candidate != nullptr);
    REQUIRE(block_candidate != nullptr);

    const double peak_gap = carrier_candidate->peak_dbfs - block_candidate->peak_dbfs;
    INFO("peak bins differ by " << peak_gap << " dB at equal reference-bandwidth SNR");
    CHECK(peak_gap > 12.0);
}

TEST_CASE("a signal no single bin could have found is found", "[detect]") {
    constexpr std::uint64_t kSeed = 31415;
    INFO("seed " << kSeed);

    // 32 bins at 6 dB in the reference bandwidth puts about 1.2 dB of excess
    // in each bin. docs/detection.md: thresholding the fine bins themselves
    // gives away the difference, and this case is the difference.
    constexpr std::size_t kCentreBin = 512;
    constexpr std::size_t kWidthBins = 32;

    Scene scene(-90.0, kSeed);
    scene.set({Emitter{.centre_bin = kCentreBin, .width_bins = kWidthBins, .snr_2500_db = 6.0}});

    detect::DetectorConfig config = base_config();
    config.detection_threshold_db = 0.0;
    auto made = detect::Detector::create(config, scene.geometry());
    REQUIRE(made);
    detect::Detector& detector = *made;

    run_for(detector, scene, 6.0);

    INFO(describe(detector));
    const detect::Track* track = find_near(detector, scene.frequency_of(kCentreBin), 6000);
    REQUIRE(track != nullptr);
    CHECK(track->snr_2500_db == Approx(6.0).margin(2.0));

    // The other half of the claim: the strongest fine bin in the whole frame
    // is barely above the floor, so nothing that looked at one bin at a time
    // could have produced the detection above.
    const std::span<const double> average = detector.averaged_power();
    const std::span<const double> floor = detector.noise_floor();
    double worst = 0.0;
    for (std::size_t i = 0; i < kBins; ++i) {
        worst = std::max(worst, 10.0 * std::log10(average[i] / floor[i]));
    }
    INFO("strongest single bin sits " << worst << " dB over the floor");
    CHECK(worst < 3.0);
}

TEST_CASE("the operator's threshold is what decides", "[detect]") {
    constexpr std::uint64_t kSeed = 27182;
    INFO("seed " << kSeed);

    const auto run = [&](double threshold_db) {
        Scene scene(-90.0, kSeed);
        scene.set({Emitter{.centre_bin = 400, .width_bins = 9, .snr_2500_db = 12.0}});
        detect::DetectorConfig config = base_config();
        config.detection_threshold_db = threshold_db;
        auto made = detect::Detector::create(config, scene.geometry());
        REQUIRE(made);
        run_for(*made, scene, 3.0);
        return made->tracks().size();
    };

    CHECK(run(6.0) == 1);
    CHECK(run(20.0) == 0);

    // And it moves while running, because an operator turns a knob rather
    // than restarting an engine.
    Scene scene(-90.0, kSeed);
    scene.set({Emitter{.centre_bin = 400, .width_bins = 9, .snr_2500_db = 12.0}});
    detect::DetectorConfig config = base_config();
    config.detection_threshold_db = 20.0;
    auto made = detect::Detector::create(config, scene.geometry());
    REQUIRE(made);
    run_for(*made, scene, 3.0);
    CHECK(made->tracks().empty());

    REQUIRE(made->set_thresholds(6.0, 0.5));
    run_for(*made, scene, 3.0);
    CHECK(made->tracks().size() == 1);

    CHECK_FALSE(made->set_thresholds(std::nan(""), 0.5));
    CHECK_FALSE(made->set_thresholds(6.0, 4.0));
}

TEST_CASE("birth takes consecutive decisions", "[detect]") {
    constexpr std::uint64_t kSeed = 55555;
    INFO("seed " << kSeed);

    Scene scene(-90.0, kSeed);
    scene.silence();

    detect::DetectorConfig config = base_config();
    config.birth_hits = 5;
    // A tenth of a decision's worth of averaging, so a burst that lasts one
    // decision is gone from the average by the next one. With the ordinary
    // second of integration a one-decision burst is still visible several
    // decisions later, which would be the average being tested rather than
    // the birth rule. The threshold goes up to match: two frames of
    // averaging is noisy enough that six decibels would produce detections
    // of its own, and that is a property of the integration rather than of
    // the rule under test.
    config.average_seconds = 0.02;
    config.decision_interval_seconds = 0.08;
    config.detection_threshold_db = 20.0;
    auto made = detect::Detector::create(config, scene.geometry());
    REQUIRE(made);
    detect::Detector& detector = *made;

    run_for(detector, scene, 1.0);
    REQUIRE(detector.tracks().empty());

    // One decision's worth of signal. A candidate appears, a pending track is
    // created, and the silence that follows discards it before it is born.
    scene.set({Emitter{.centre_bin = 600, .width_bins = 9, .snr_2500_db = 30.0}});
    run_for(detector, scene, 0.08);
    scene.silence();
    run_for(detector, scene, 1.0);

    INFO(describe(detector));
    CHECK(detector.tracks().empty());
    CHECK(detector.stats().tracks_born == 0);

    // Long enough to clear the bar, and now it is a track.
    scene.set({Emitter{.centre_bin = 600, .width_bins = 9, .snr_2500_db = 30.0}});
    run_for(detector, scene, 1.5);

    INFO(describe(detector));
    CHECK(detector.tracks().size() == 1);
    CHECK(detector.stats().tracks_born == 1);
}

TEST_CASE("a track holds through a gap and decays after one", "[detect]") {
    constexpr std::uint64_t kSeed = 76543;
    INFO("seed " << kSeed);

    Scene scene(-90.0, kSeed);
    const Emitter emitter{.centre_bin = 350, .width_bins = 13, .snr_2500_db = 25.0};
    scene.set({emitter});

    detect::DetectorConfig config = base_config();
    // Short enough that the average follows a transmission stopping within a
    // fraction of the hold, which is what makes the hold observable at all.
    // The threshold goes up with it: a tenth of a second of integration is
    // eight times noisier than a second of it, and six decibels there is
    // under five standard deviations across eight thousand trials a
    // decision. That is a property of the shortened average rather than of
    // the hold, and pinning it here keeps this case about the hold.
    config.average_seconds = 0.15;
    config.detection_threshold_db = 12.0;
    config.bootstrap_hold_seconds = 1.5;
    config.confidence_half_life_seconds = 1.0;
    auto made = detect::Detector::create(config, scene.geometry());
    REQUIRE(made);
    detect::Detector& detector = *made;

    run_for(detector, scene, 1.5);
    REQUIRE(detector.tracks().size() == 1);
    const std::uint64_t id = detector.tracks()[0].id;
    const double live_confidence = detector.tracks()[0].confidence;
    CHECK(detector.tracks()[0].state == detect::TrackState::Live);

    // The gap. FT8 is silent for 2.4 seconds in every 15 and the hold exists
    // so a track survives that; here the hold is 1.5 s and the gap is 0.8.
    scene.silence();
    run_for(detector, scene, 0.8);

    INFO(describe(detector));
    REQUIRE(detector.tracks().size() == 1);
    CHECK(detector.tracks()[0].id == id);
    CHECK(detector.tracks()[0].state == detect::TrackState::Held);
    CHECK(detector.tracks()[0].confidence < live_confidence);
    CHECK(detector.tracks()[0].silent_samples() > 0);
    CHECK(detector.stats().tracks_dropped == 0);

    // It comes back on the same id rather than as a new signal, which is the
    // whole point: a click that lands on a keyed CW signal must not be a
    // different object between dits.
    scene.set({emitter});
    run_for(detector, scene, 1.0);
    INFO(describe(detector));
    REQUIRE(detector.tracks().size() == 1);
    CHECK(detector.tracks()[0].id == id);
    CHECK(detector.tracks()[0].state == detect::TrackState::Live);

    // And then a silence past the hold, which is what decay is for.
    scene.silence();
    run_for(detector, scene, 3.0);
    INFO(describe(detector));
    CHECK(detector.tracks().empty());
    CHECK(detector.stats().tracks_dropped >= 1);
}

// What a track reports after its signal stops, and for how long.
//
// The case above proves the hold survives a gap, which is what it is for.
// This one asks the opposite question, which nothing asked before it: once
// the signal is gone for good, how long does the row stay on the wire and
// what geometry does it carry while it is there.
//
// Measured 2026-09-20 before the residual rule existed, on this fixture at
// average_seconds 0.8: the row stayed LIVE for 6.08 s after the emitter
// stopped, then Held for 2.96 s, and went at 9.04 s. Nine seconds of
// published row against a three second hold. It was Live because the
// exponential average's residual keeps clearing the detection bar, so
// last_detected kept advancing, silent_samples() read 0.00 s throughout and
// confidence stayed at one: nothing on the published surface said the row
// was stale, and a client that sizes a click target and a waterfall
// rectangle from it had no way to know. Halving average_seconds to 0.4
// halved the Live tail to 3.04 s and left the hold at 2.96, which is the
// signature that separates the two clocks.
//
// A 40 dB emitter is deliberate. The tail is (snr - threshold) over the
// decay rate, so the loudest signals produce the longest ghosts, which is
// the opposite of how a defect usually scales and is why a marginal
// detection would have hidden this.
TEST_CASE("a track stops being published soon after its signal stops", "[detect]") {
    constexpr std::uint64_t kSeed = 24680;
    INFO("seed " << kSeed);

    constexpr std::size_t kCentreBin = 500;
    constexpr std::size_t kWidthBins = 64;

    Scene scene(-90.0, kSeed);
    const Emitter emitter{
        .centre_bin = kCentreBin, .width_bins = kWidthBins, .snr_2500_db = 40.0};
    scene.set({emitter});

    detect::DetectorConfig config = base_config();
    config.bootstrap_hold_seconds = 3.0;
    auto made = detect::Detector::create(config, scene.geometry());
    REQUIRE(made);
    detect::Detector& detector = *made;

    run_for(detector, scene, 3.0);
    REQUIRE(detector.tracks().size() == 1);
    const std::uint64_t id = detector.tracks()[0].id;
    const auto on_centre = static_cast<double>(detector.tracks()[0].center);
    const auto on_bandwidth = static_cast<double>(detector.tracks()[0].bandwidth);

    const auto low = static_cast<double>(scene.frequency_of(kCentreBin - kWidthBins / 2));
    const auto high = static_cast<double>(scene.frequency_of(kCentreBin + kWidthBins / 2));

    scene.silence();
    const dsp::SampleIndex stopped = scene.index();

    double live_seconds = 0.0;
    double held_seconds = 0.0;
    double life_seconds = 0.0;
    double worst_centre_error = 0.0;
    double worst_bandwidth_ratio = 1.0;
    double last_centre_error = 0.0;
    double last_bandwidth_ratio = 1.0;
    std::size_t new_ids = 0;
    bool gone = false;

    // Twelve seconds, which is four times the hold and twice the Live tail
    // this produced before the fix, so a failure reports a number rather
    // than running out of scene.
    for (int step = 0; step < 150 && !gone; ++step) {
        run_for(detector, scene, config.decision_interval_seconds);
        const double since =
            static_cast<double>(scene.index() - stopped) / static_cast<double>(kRate);

        const detect::Track* found = nullptr;
        for (const detect::Track& track : detector.tracks()) {
            if (track.id == id) {
                found = &track;
                continue;
            }
            const double half = 0.5 * static_cast<double>(track.bandwidth);
            if (static_cast<double>(track.center) + half > low &&
                static_cast<double>(track.center) - half < high) {
                ++new_ids;
            }
        }
        if (found == nullptr) {
            gone = true;
            break;
        }

        life_seconds = since;
        if (found->state == detect::TrackState::Live) {
            live_seconds = since;
        } else {
            held_seconds = since - live_seconds;
        }
        last_centre_error = static_cast<double>(found->center) - on_centre;
        last_bandwidth_ratio = static_cast<double>(found->bandwidth) / on_bandwidth;
        worst_centre_error = std::max(worst_centre_error, std::abs(last_centre_error));
        worst_bandwidth_ratio = std::max(worst_bandwidth_ratio, last_bandwidth_ratio);
        worst_bandwidth_ratio = std::max(worst_bandwidth_ratio, 1.0 / last_bandwidth_ratio);
    }

    INFO(std::format("after the stop: live {:.2f} s, held {:.2f} s, last published at {:.2f} s, "
                     "worst centre error {:.0f} Hz, worst bandwidth ratio {:.2f}, "
                     "last centre error {:.0f} Hz, last bandwidth ratio {:.2f}, "
                     "{} new ids in the band, {} candidates withheld as residual",
                     live_seconds, held_seconds, life_seconds, worst_centre_error,
                     worst_bandwidth_ratio, last_centre_error, last_bandwidth_ratio, new_ids,
                     detector.stats().candidates_residual));

    CHECK(gone);

    // The bar, and where each number comes from.
    //
    // The row may outlive its signal by the hold, plus the decisions the
    // residual rule needs to be sure. That rule wants residual_decisions
    // consecutive falls and the first of them cannot be judged until a
    // second decision exists, so the floor is (residual_decisions + 1)
    // intervals; two more are allowed for the decision that straddles the
    // stop and for a run reset on noise. Anything past that is the row
    // outliving its evidence, which is the defect.
    const double allowed =
        config.bootstrap_hold_seconds +
        static_cast<double>(config.residual_decisions + 3) * config.decision_interval_seconds;
    CHECK(life_seconds <= allowed);

    // And the Live part of it is what a consumer cannot see through:
    // silent_samples() is zero and confidence is rising for as long as the
    // row is Live, so that window is the one with no signal on the wire at
    // all. It has to be the short end of the split, not the long one.
    CHECK(live_seconds < config.bootstrap_hold_seconds);

    // Geometry a client can act on has to come from a decision that saw the
    // signal. A tenth of the emitter's own bandwidth is the same figure the
    // scene bar uses for centre error, and the band may neither double nor
    // halve.
    CHECK(worst_centre_error <= 0.10 * on_bandwidth);
    CHECK(worst_bandwidth_ratio <= 2.0);

    // Nothing may be born from the residual after the track is dropped. This
    // is what forced the suppression to sit on the candidate rather than on
    // the track: a track-level rule leaves the candidate free and a fresh id
    // appears in the same band a few decisions later, forever.
    CHECK(new_ids == 0);
}

// A signal that fades keeps its track, and the rate is what decides.
//
// The residual rule withholds a candidate whose band is falling at the
// exponential average's own decay rate, because that is what a stopped
// transmission looks like on the way out. It used to withhold anything
// falling at 0.6 of that rate or faster, on the argument that the cost of a
// false positive was a momentary Held state. It is not: the suppression sits
// on the candidate, so the track stops being fed, reaches its hold three
// seconds later and is DROPPED, and no replacement is born either because
// there is no candidate to born one from. A transmitter still tens of
// decibels over the noise leaves the list entirely.
//
// 8 dB over 3 s is the case, and it is ordinary: mobile VHF driving behind a
// building, an aeronautical signal at low elevation, an HF path at dusk. At
// the shipped second of averaging that is 2.67 dB/s against a predicted 4.343,
// so it cleared the old 2.606 bar with room to spare.
//
// Nothing here reads a clock. The fade is a function of the sample index the
// scene has reached, exactly like everything else in this file.
TEST_CASE("a fading signal keeps its track", "[detect]") {
    constexpr std::uint64_t kSeed = 19283;
    INFO("seed " << kSeed);

    constexpr std::size_t kCentreBin = 480;
    constexpr std::size_t kWidthBins = 33;
    constexpr double kStartDb = 60.0;
    constexpr double kFadeSeconds = 3.0;

    // Feeds one frame at a time with the emitter at whatever level the ramp
    // has reached. Returns the id it saw, or zero.
    const auto fade = [](detect::Detector& detector, Scene& scene, double from_db, double to_db,
                         double seconds) {
        const auto frames = static_cast<std::size_t>(std::llround(seconds / kFrameSeconds));
        for (std::size_t i = 0; i < frames; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(std::max<std::size_t>(1, frames));
            scene.set({Emitter{.centre_bin = kCentreBin,
                               .width_bins = kWidthBins,
                               .snr_2500_db = from_db + t * (to_db - from_db)}});
            auto fed = detector.consume(scene.next());
            if (!fed) {
                FAIL("consume refused a frame: " << fed.error().message);
            }
        }
    };

    const auto run = [&](double start_db, double fall_db, double tolerance) {
        Scene scene(-90.0, kSeed);
        scene.set({Emitter{.centre_bin = kCentreBin,
                           .width_bins = kWidthBins,
                           .snr_2500_db = start_db}});

        detect::DetectorConfig config = base_config();
        // The shipped pair, so the rate in the comment is the rate under test.
        config.average_seconds = 1.0;
        config.decision_interval_seconds = 0.1;
        config.residual_rate_tolerance = tolerance;

        auto made = detect::Detector::create(config, scene.geometry());
        REQUIRE(made);
        detect::Detector detector = std::move(*made);

        run_for(detector, scene, 3.0);
        REQUIRE(detector.tracks().size() == 1);
        const std::uint64_t id = detector.tracks()[0].id;

        fade(detector, scene, start_db, start_db - fall_db, kFadeSeconds);
        run_for(detector, scene, 1.0);

        struct Outcome {
            std::uint64_t id = 0;
            std::size_t tracks = 0;
            std::uint64_t dropped = 0;
            std::uint64_t born = 0;
            std::string text;
        };
        Outcome outcome;
        outcome.tracks = detector.tracks().size();
        outcome.dropped = detector.stats().tracks_dropped;
        outcome.born = detector.stats().tracks_born;
        outcome.text = describe(detector);
        for (const detect::Track& track : detector.tracks()) {
            if (track.id == id) {
                outcome.id = id;
            }
        }
        return outcome;
    };

    const double shipped = detect::DetectorConfig{}.residual_rate_tolerance;

    SECTION("8 dB over 3 s, which is 0.61 of the rate an emptying average falls at") {
        const auto outcome = run(kStartDb, 8.0, shipped);
        INFO(outcome.text);
        CHECK(outcome.tracks == 1);
        CHECK(outcome.id != 0);
        CHECK(outcome.dropped == 0);
        CHECK(outcome.born == 1);
    }

    SECTION("the same fade, with the window opened to its far end") {
        // A tolerance of one puts the window's lower edge at zero, so every
        // fall inside a run qualifies and the rule fires on direction alone.
        // That is the shape the rule had, and this is what it cost: the
        // track is dropped and nothing replaces it, while the signal is
        // still 52 dB over the operator's threshold.
        const auto outcome = run(kStartDb, 8.0, 1.0);
        INFO(outcome.text);
        CHECK(outcome.dropped >= 1);
        CHECK(outcome.id == 0);
    }

    SECTION("30 dB over 3 s, more than twice the rate, and it still keeps its track") {
        // The margin is not one decibel wide. An exponential average fed a
        // ramp lags it: the average's own fall only reaches the input's once
        // the ramp has run for several time constants, and at 10 dB/s over
        // three seconds it never lands inside the window for the four
        // consecutive decisions the rule needs.
        const auto outcome = run(kStartDb, 30.0, shipped);
        INFO(outcome.text);
        CHECK(outcome.tracks == 1);
        CHECK(outcome.id != 0);
        CHECK(outcome.dropped == 0);
    }

    SECTION("40 dB over 3 s is where it does cost a track, and the header says so") {
        // 13.3 dB/s, three times the rate an emptying average falls at.
        // Sustained that long the average cannot tell the ramp from an
        // input that went to zero, and the track is dropped even though the
        // signal ends 14 dB over the threshold. Measured rather than
        // reasoned: the sweep behind this section kept the track at 4, 8,
        // 13, 20 and 30 dB of fall and lost it at 40.
        const auto outcome = run(kStartDb, 40.0, shipped);
        INFO(outcome.text);
        CHECK(outcome.dropped >= 1);
    }
}

TEST_CASE("two signals merge into one track and split back into two", "[detect]") {
    constexpr std::uint64_t kSeed = 86420;
    INFO("seed " << kSeed);

    // Far enough apart that the gap between them is real noise, which is what
    // keeps the summed-bin search from preferring the pair. 60 bins of centre
    // separation at 1 kHz bins with two 15-bin signals leaves 45 bins of
    // silence between them.
    constexpr std::size_t kLeftBin = 420;
    constexpr std::size_t kRightBin = 480;
    const Emitter left{.centre_bin = kLeftBin, .width_bins = 15, .snr_2500_db = 22.0};
    const Emitter right{.centre_bin = kRightBin, .width_bins = 15, .snr_2500_db = 20.0};

    // One signal across both of them and the gap, which is what a wideband
    // transmission starting up on top of two narrow ones looks like.
    const Emitter merged{.centre_bin = (kLeftBin + kRightBin) / 2,
                         .width_bins = 91,
                         .snr_2500_db = 26.0};

    Scene scene(-90.0, kSeed);
    scene.set({left, right});

    detect::DetectorConfig config = base_config();
    // A short average so the scene changing is over inside two decisions.
    // With a longer one the transition itself lasts longer than the birth
    // rule and throws off a short-lived track of its own, which is real
    // behaviour and is not what this case is about. The threshold rises with
    // the shortened average for the reason the hold case gives.
    config.average_seconds = 0.05;
    config.detection_threshold_db = 14.0;
    config.bootstrap_hold_seconds = 2.0;
    auto made = detect::Detector::create(config, scene.geometry());
    REQUIRE(made);
    detect::Detector& detector = *made;

    run_for(detector, scene, 1.5);

    INFO(describe(detector));
    REQUIRE(detector.tracks().size() == 2);
    const std::uint64_t left_id = detector.tracks()[0].id;
    const std::uint64_t right_id = detector.tracks()[1].id;
    CHECK(left_id != right_id);
    CHECK(detector.tracks()[0].center < detector.tracks()[1].center);

    // The merge. One candidate now covers both bands, so the assignment has
    // two tracks wanting one candidate. Without a one-to-one rule both claim
    // it and both look confirmed; with one, the older keeps it and the other
    // becomes its child rather than being dropped and losing its id.
    scene.set({merged});
    run_for(detector, scene, 1.0);

    INFO(describe(detector));
    REQUIRE(detector.tracks().size() == 2);
    CHECK(detector.stats().merges >= 1);

    std::size_t live = 0;
    std::size_t merged_count = 0;
    std::uint64_t parent = 0;
    for (const detect::Track& track : detector.tracks()) {
        if (track.state == detect::TrackState::Live) {
            ++live;
            parent = track.id;
        }
        if (track.state == detect::TrackState::Merged) {
            ++merged_count;
        }
    }
    CHECK(live == 1);
    CHECK(merged_count == 1);
    for (const detect::Track& track : detector.tracks()) {
        if (track.state == detect::TrackState::Merged) {
            CHECK(track.merged_into == parent);
            // The hold clock runs the same as a held track's, so a merge
            // cannot keep a ghost alive indefinitely. What it suspends is
            // the confidence decay, because the parent's detection is
            // evidence that something is in the child's band.
            CHECK(track.silent_samples() > 0);
            CHECK(track.confidence > 0.9);
        }
    }
    // The older of the two is the one that kept the candidate.
    CHECK(parent == std::min(left_id, right_id));
    CHECK(detector.stats().tracks_dropped == 0);

    // The split. Both ids come back rather than one surviving and the other
    // being reborn with a number the operator has never seen.
    scene.set({left, right});
    run_for(detector, scene, 1.0);

    INFO(describe(detector));
    REQUIRE(detector.tracks().size() == 2);
    CHECK(detector.stats().splits >= 1);
    for (const detect::Track& track : detector.tracks()) {
        CHECK(track.state == detect::TrackState::Live);
        CHECK(track.merged_into == 0);
    }
    CHECK(detector.tracks()[0].id == left_id);
    CHECK(detector.tracks()[1].id == right_id);
}

TEST_CASE("assignment is one to one", "[detect]") {
    constexpr std::uint64_t kSeed = 11223;
    INFO("seed " << kSeed);

    // Six signals close enough together that several tracks are gated to
    // several candidates at once, which is where a rule that only tests
    // overlap hands the same candidate to two tracks.
    Scene scene(-90.0, kSeed);
    std::vector<Emitter> emitters;
    for (std::size_t i = 0; i < 6; ++i) {
        emitters.push_back(Emitter{.centre_bin = 300 + i * 60,
                                   .width_bins = 11,
                                   .snr_2500_db = 18.0 + static_cast<double>(i)});
    }
    scene.set(emitters);

    auto made = detect::Detector::create(base_config(), scene.geometry());
    REQUIRE(made);
    detect::Detector& detector = *made;

    run_for(detector, scene, 4.0);

    INFO(describe(detector));
    REQUIRE(detector.tracks().size() == 6);

    // No two tracks on the same frequency, and every id distinct. Two tracks
    // both claiming one candidate shows up as exactly this.
    std::vector<std::uint64_t> ids;
    for (std::size_t i = 0; i < detector.tracks().size(); ++i) {
        ids.push_back(detector.tracks()[i].id);
        if (i > 0) {
            CHECK(detector.tracks()[i].center > detector.tracks()[i - 1].center);
        }
    }
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    for (std::size_t i = 0; i < 6; ++i) {
        const detect::Track* track = find_near(detector, scene.frequency_of(300 + i * 60), 2000);
        INFO("emitter " << i);
        REQUIRE(track != nullptr);
        CHECK(track->state == detect::TrackState::Live);
    }
}

TEST_CASE("a track at a coarse channel boundary does not flip channel on noise", "[detect]") {
    constexpr std::uint64_t kSeed = 99887;
    INFO("seed " << kSeed);

    // Channel spacing is 128 kHz on this grid, so the boundary between
    // channel 0 and channel 1 is at 64 kHz. bin_zero is -576 kHz and bins are
    // 1 kHz, so 64 kHz is bin 640. place() rounds to the nearest channel, so
    // a track sitting here flips every time the measurement crosses, and each
    // flip is a different tap table and a half-megabyte upload.
    constexpr std::size_t kBoundaryBin = 640;
    // Wide against the steps it is walked in. A signal that moves further
    // than a fraction of its own bandwidth between decisions leaves the tail
    // of the average behind it as a detection of its own, which is real and
    // is not what this case is measuring.
    constexpr std::size_t kWidthBins = 41;

    Scene scene(-90.0, kSeed);
    REQUIRE(scene.frequency_of(kBoundaryBin) == 64'000);

    detect::DetectorConfig config = base_config();
    config.average_seconds = 0.2;
    config.detection_threshold_db = 14.0;
    config.centre_smoothing = 1.0;  // no smoothing, so only the hysteresis is under test
    auto made = detect::Detector::create(config, scene.geometry());
    REQUIRE(made);
    detect::Detector& detector = *made;

    const auto place = [&](std::size_t bin, double seconds) {
        scene.set({Emitter{.centre_bin = bin, .width_bins = kWidthBins, .snr_2500_db = 30.0}});
        run_for(detector, scene, seconds);
    };

    place(kBoundaryBin, 1.5);
    REQUIRE(detector.tracks().size() == 1);
    const std::uint32_t settled = detector.tracks()[0].channel;
    const std::uint64_t id = detector.tracks()[0].id;
    CHECK(detector.tracks()[0].channel_valid);
    CHECK(settled == 1);

    // Wobble across the boundary and back. Three bins is 3 kHz against a
    // 128 kHz channel, which is the size of move a measurement makes on its
    // own, and place() rounding alone flips the channel at every crossing:
    // 61 kHz rounds to channel 0 and 67 kHz to channel 1.
    for (int step = 0; step < 8; ++step) {
        place(step % 2 == 0 ? kBoundaryBin + 3 : kBoundaryBin - 3, 0.4);
        INFO(describe(detector));
        REQUIRE(detector.tracks().size() == 1);
        CHECK(detector.tracks()[0].id == id);
        CHECK(detector.tracks()[0].channel == settled);
    }

    // A real drift does change it, and the same track carries the change
    // rather than the old one being dropped and a new one born. Walked down
    // six bins at a time, because a signal that jumps further than its own
    // bandwidth in one decision is not a drift and is not what the
    // association rule is for.
    for (std::size_t bin = kBoundaryBin - 3; bin >= 596; bin -= 6) {
        place(bin, 0.3);
    }
    place(596, 0.8);
    INFO(describe(detector));
    REQUIRE(detector.tracks().size() == 1);
    CHECK(detector.tracks()[0].id == id);
    CHECK(detector.tracks()[0].channel != settled);
    CHECK(detector.tracks()[0].channel == 0);
}

TEST_CASE("the detector reads a sample index and never a clock", "[detect]") {
    constexpr std::uint64_t kSeed = 60606;
    INFO("seed " << kSeed);

    // The same scene fed twice, once with the frames arriving at their own
    // indices and once with the whole stream started an hour in. Everything
    // but the absolute indices has to match, because a capture replayed from
    // the middle is the case retroactive tuning exists for.
    const auto run = [&](dsp::SampleIndex offset) {
        Scene scene(-90.0, kSeed);
        scene.set({Emitter{.centre_bin = 260, .width_bins = 17, .snr_2500_db = 19.0},
                   Emitter{.centre_bin = 730, .width_bins = 7, .snr_2500_db = 23.0}});
        auto made = detect::Detector::create(base_config(), scene.geometry());
        REQUIRE(made);
        std::vector<detect::Track> out;
        const auto frames = static_cast<std::size_t>(std::llround(3.0 / kFrameSeconds));
        for (std::size_t i = 0; i < frames; ++i) {
            engine::SpectrumFrame frame = scene.next();
            frame.start += offset;
            REQUIRE(made->consume(frame));
        }
        for (const detect::Track& track : made->tracks()) {
            out.push_back(track);
        }
        return out;
    };

    const std::vector<detect::Track> plain = run(0);
    const std::vector<detect::Track> shifted = run(dsp::SampleIndex{kRate} * 3600);

    REQUIRE(plain.size() == 2);
    REQUIRE(plain.size() == shifted.size());
    for (std::size_t i = 0; i < plain.size(); ++i) {
        CHECK(plain[i].id == shifted[i].id);
        CHECK(plain[i].center == shifted[i].center);
        CHECK(plain[i].bandwidth == shifted[i].bandwidth);
        CHECK(plain[i].snr_2500_db == shifted[i].snr_2500_db);
        CHECK(plain[i].confidence == shifted[i].confidence);
        CHECK(plain[i].age_samples() == shifted[i].age_samples());
    }
}

TEST_CASE("the source centre puts tracks on absolute frequency", "[detect]") {
    constexpr std::uint64_t kSeed = 70707;
    INFO("seed " << kSeed);

    constexpr dsp::Hertz kSourceCentre = 98'100'000;
    Scene scene(-90.0, kSeed);
    scene.set({Emitter{.centre_bin = 600, .width_bins = 11, .snr_2500_db = 24.0}});

    detect::DetectorConfig config = base_config();
    config.source_center = kSourceCentre;
    auto made = detect::Detector::create(config, scene.geometry());
    REQUIRE(made);
    run_for(*made, scene, 3.0);

    INFO(describe(*made));
    REQUIRE(made->tracks().size() == 1);
    // docs/detection.md: the track carries absolute and converts at the call,
    // so that the detector and the command line cannot disagree about what a
    // number means.
    CHECK(std::abs(made->tracks()[0].center - (kSourceCentre + scene.frequency_of(600))) <= 1000);
}
