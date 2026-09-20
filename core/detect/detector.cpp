#include "core/detect/detector.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <string_view>
#include <tuple>
#include <utility>

#include "core/dsp/spectrum_reference.h"

namespace revenant::detect {

namespace {

// log2(10)/10. Converts a decibel figure straight into an exp2 argument,
// which is the cheapest correctly-rounded exponential in the standard
// library and is called once per bin per frame.
constexpr float kDbToLog2 = 0.332192809488736234787F;

constexpr std::uint32_t kNoAssignment = 0xFFFF'FFFFU;

[[nodiscard]] double db_to_linear(double db) { return std::exp2(db * static_cast<double>(kDbToLog2)); }

[[nodiscard]] double linear_to_db(double linear) {
    const double floored = linear > static_cast<double>(dsp::kSpectrumPowerFloor)
                               ? linear
                               : static_cast<double>(dsp::kSpectrumPowerFloor);
    return 10.0 * std::log10(floored);
}

// The standard normal quantile, so a percentile of the averaged spectrum can
// be read as a distance from its mean in standard deviations.
//
// Acklam's rational approximation, which is accurate to about 1.15e-9 across
// the open unit interval and is published mathematics rather than anybody's
// implementation. Called twice, at construction, so nothing here is on a
// per-frame path and the tail refinement Acklam describes is left out.
[[nodiscard]] double normal_quantile(double probability) {
    constexpr double a[6] = {-3.969683028665376e+01, 2.209460984245205e+02,
                             -2.759285104469687e+02, 1.383577518672690e+02,
                             -3.066479806614716e+01, 2.506628277459239e+00};
    constexpr double b[5] = {-5.447609879822406e+01, 1.615858368580409e+02,
                             -1.556989798598866e+02, 6.680131188771972e+01,
                             -1.328068155288572e+01};
    constexpr double c[6] = {-7.784894002430293e-03, -3.223964580411365e-01,
                             -2.400758277161838e+00, -2.549732539343734e+00,
                             4.374664141464968e+00,  2.938163982698783e+00};
    constexpr double d[4] = {7.784695709041462e-03, 3.224671290700398e-01,
                             2.445134137142996e+00, 3.754408661907416e+00};
    constexpr double kLowBreak = 0.02425;

    if (probability <= 0.0 || probability >= 1.0) {
        return 0.0;
    }
    if (probability < kLowBreak) {
        const double q = std::sqrt(-2.0 * std::log(probability));
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    if (probability > 1.0 - kLowBreak) {
        const double q = std::sqrt(-2.0 * std::log(1.0 - probability));
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    const double q = probability - 0.5;
    const double r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
}

// The same rounding place() uses, so a track's channel and a receiver's
// channel are the same integer rather than two answers that agree most of the
// time. Halves away from zero.
[[nodiscard]] std::int64_t divide_nearest(std::int64_t numerator, std::int64_t denominator) {
    if (denominator == 0) {
        return 0;
    }
    const std::int64_t half = denominator / 2;
    return numerator >= 0 ? (numerator + half) / denominator : (numerator - half) / denominator;
}

[[nodiscard]] Status require_finite(double value, std::string_view what) {
    if (!std::isfinite(value)) {
        return fail(std::format("Detector: {} is not a finite number", what));
    }
    return {};
}

[[nodiscard]] Status require_range(double value, double low, double high, std::string_view what) {
    if (auto finite = require_finite(value, what); !finite) {
        return finite;
    }
    if (value < low || value > high) {
        return fail(std::format("Detector: {} is {}, outside {} to {}", what, value, low, high));
    }
    return {};
}

// A confidence bar the rise can never reach hides every track and reports
// nothing about why, which is the worst shape a bad threshold can take: the
// list is empty and an empty list is also what a quiet band looks like. Both
// places that accept a threshold refuse that pair here rather than take a
// filter that can only ever be empty.
//
// Confidence rises by confidence_rise of its remaining distance to one, so
// one is a limit rather than a value, and it is reached only when a single
// detection closes the whole gap.
[[nodiscard]] Status require_reachable_confidence(double threshold, double rise) {
    if (threshold >= 1.0 && rise < 1.0) {
        return fail(std::format(
            "Detector: confidence_threshold is {} and confidence_rise is {}, so a track's "
            "confidence approaches one without reaching it and nothing would ever clear the "
            "bar. Use a threshold below one",
            threshold, rise));
    }
    return {};
}

}  // namespace

const char* track_state_name(TrackState state) {
    switch (state) {
        case TrackState::Pending: return "pending";
        case TrackState::Live: return "live";
        case TrackState::Held: return "held";
        case TrackState::Merged: return "merged";
    }
    return "unknown";
}

Expected<Detector> Detector::create(const DetectorConfig& config,
                                    const engine::SpectrumGeometry& geometry) {
    if (!geometry.enabled()) {
        return fail("Detector::create was given a spectrum geometry with no bins in it. The "
                    "spectrum stage is built with the coarse chain when the source is opened, "
                    "so EngineConfig::spectrum_transform has to be non-zero before this");
    }
    if (geometry.bin_width_hz() <= 0.0 || !std::isfinite(geometry.bin_width_hz())) {
        return fail(std::format("Detector::create was given a bin width of {} Hz",
                                geometry.bin_width_hz()));
    }
    if (config.source_rate <= 0) {
        return fail(std::format("Detector::create needs the source rate to turn a difference of "
                                "sample indices into seconds, and got {} S/s",
                                config.source_rate));
    }

    if (auto ok = require_range(config.detection_threshold_db, -200.0, 200.0,
                                "detection_threshold_db");
        !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = require_range(config.confidence_threshold, 0.0, 1.0, "confidence_threshold");
        !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = require_range(config.average_seconds, 1.0e-6, 3600.0, "average_seconds"); !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = require_range(config.decision_interval_seconds, 1.0e-6, 3600.0,
                                "decision_interval_seconds");
        !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = require_range(config.noise_percentile, 0.001, 0.998, "noise_percentile"); !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = require_range(config.noise_percentile_high, 0.002, 0.999, "noise_percentile_high");
        !ok) {
        return std::unexpected(ok.error());
    }
    if (config.noise_percentile_high <= config.noise_percentile) {
        return fail(std::format("Detector::create: noise_percentile_high is {} and must be above "
                                "noise_percentile, which is {}. The two are differenced to get "
                                "the noise spread, and an inverted pair gives a negative one",
                                config.noise_percentile_high, config.noise_percentile));
    }
    if (auto ok = require_range(config.noise_excision_sigma, 0.0, 100.0, "noise_excision_sigma");
        !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = require_range(config.occupied_power_fraction, 0.01, 1.0,
                                "occupied_power_fraction");
        !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = require_range(config.edge_floor_fraction, 0.0, 100.0, "edge_floor_fraction");
        !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = require_range(config.edge_floor_sigma, 0.0, 100.0, "edge_floor_sigma"); !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = require_range(config.residual_decay_fraction, 0.0, 10.0,
                                "residual_decay_fraction");
        !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = require_range(config.split_gap_db, 0.0, 60.0, "split_gap_db"); !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = require_range(config.association_overlap, 0.001, 1.0, "association_overlap");
        !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = require_range(config.confidence_rise, 0.001, 1.0, "confidence_rise"); !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = require_reachable_confidence(config.confidence_threshold, config.confidence_rise);
        !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = require_range(config.confidence_half_life_seconds, 1.0e-6, 3600.0,
                                "confidence_half_life_seconds");
        !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = require_range(config.drop_confidence, 0.0, 0.999, "drop_confidence"); !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = require_range(config.centre_smoothing, 0.001, 1.0, "centre_smoothing"); !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = require_range(config.bootstrap_hold_seconds, 1.0e-6, 3600.0,
                                "bootstrap_hold_seconds");
        !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = require_range(config.channel_hysteresis, 0.0, 10.0, "channel_hysteresis"); !ok) {
        return std::unexpected(ok.error());
    }
    if (config.noise_knots == 0 || config.noise_window_knots == 0) {
        return fail("Detector::create needs at least one noise knot and a window of at least one "
                    "knot spacing");
    }
    if (config.birth_hits == 0) {
        return fail("Detector::create: birth_hits of zero would make every noise excursion a "
                    "track, which is the one thing that knob exists to prevent");
    }
    if (config.max_tracks == 0) {
        return fail("Detector::create: max_tracks must be at least one");
    }

    Detector detector;
    detector.config_ = config;
    detector.geometry_ = geometry;
    detector.bins_ = geometry.bins;
    detector.bin_width_hz_ = geometry.bin_width_hz();
    detector.bin_zero_hz_ = geometry.bin_zero_hz();

    detector.threshold_linear_ = db_to_linear(config.detection_threshold_db);
    detector.gap_ratio_ = db_to_linear(config.split_gap_db);

    // The standard normal quantiles at the two percentiles, so a spread
    // measured between them converts to a standard deviation. Computed once
    // here rather than per window, and from the configured percentiles rather
    // than from the defaults, because an operator moving them must not
    // silently keep the old scaling.
    const double low_quantile = normal_quantile(config.noise_percentile);
    const double high_quantile = normal_quantile(config.noise_percentile_high);
    detector.quantile_span_ = high_quantile - low_quantile;
    detector.high_quantile_ = high_quantile;

    const double interval = config.decision_interval_seconds * static_cast<double>(config.source_rate);
    detector.decision_interval_samples_ =
        interval < 1.0 ? 1ULL : static_cast<std::uint64_t>(std::llround(interval));

    const double warmup = config.average_seconds * static_cast<double>(config.source_rate);
    detector.warmup_samples_ =
        warmup < 1.0 ? 1ULL : static_cast<std::uint64_t>(std::llround(warmup));

    // Knots cannot outnumber bins, and the window cannot outrun the span.
    const std::size_t knots =
        std::min<std::size_t>(config.noise_knots, detector.bins_);
    const std::size_t spacing = std::max<std::size_t>(1, detector.bins_ / knots);
    detector.noise_window_bins_ =
        std::min<std::size_t>(detector.bins_, spacing * config.noise_window_knots);

    // The summing ladder. Powers of two, because the deflection is a broad
    // function of width near its maximum: the loss from being a factor of two
    // off the true bandwidth is 0.5 dB, and a finer ladder costs a full pass
    // over the frame for each extra rung.
    std::uint32_t widest = config.max_sum_bins;
    if (widest == 0) {
        widest = static_cast<std::uint32_t>(std::bit_floor(std::max<std::size_t>(1, detector.bins_ / 8)));
    }
    widest = std::min<std::uint32_t>(widest, static_cast<std::uint32_t>(detector.bins_));
    for (std::uint32_t width = 1; width <= widest; width *= 2) {
        detector.widths_.push_back(width);
    }

    // Zero derives one peak per fine bin. See the note on max_peaks: the
    // ladder cannot report more than about twice the bin count of distinct
    // positions, so this keeps over half of everything it could produce and
    // it tracks the transform size rather than being right at one of them.
    detector.peak_budget_ = config.max_peaks != 0
                                ? config.max_peaks
                                : static_cast<std::uint32_t>(detector.bins_);

    detector.average_.assign(detector.bins_, 0.0);
    detector.floor_.assign(detector.bins_, static_cast<double>(dsp::kSpectrumPowerFloor));
    detector.sigma_.assign(detector.bins_, 0.0);
    detector.excess_cumulative_.assign(detector.bins_ + 1, 0.0);
    detector.floor_cumulative_.assign(detector.bins_ + 1, 0.0);
    detector.sigma_cumulative_.assign(detector.bins_ + 1, 0.0);
    detector.deflection_.assign(detector.bins_, 0.0F);
    detector.window_.reserve(detector.noise_window_bins_);
    detector.knot_floor_.assign(knots, static_cast<double>(dsp::kSpectrumPowerFloor));
    detector.knot_sigma_.assign(knots, 0.0);
    detector.knot_position_.assign(knots, 0.0);
    detector.separator_.reserve(detector.bins_);
    detector.peaks_.reserve(detector.peak_budget_);

    for (std::size_t k = 0; k < knots; ++k) {
        detector.knot_position_[k] = (static_cast<double>(2 * k + 1) *
                                      static_cast<double>(detector.bins_)) /
                                     static_cast<double>(2 * knots);
    }

    return detector;
}

Status Detector::set_thresholds(double detection_threshold_db, double confidence_threshold) {
    if (auto ok = require_range(detection_threshold_db, -200.0, 200.0, "detection_threshold_db");
        !ok) {
        return ok;
    }
    if (auto ok = require_range(confidence_threshold, 0.0, 1.0, "confidence_threshold"); !ok) {
        return ok;
    }
    if (auto ok = require_reachable_confidence(confidence_threshold, config_.confidence_rise);
        !ok) {
        return ok;
    }
    config_.detection_threshold_db = detection_threshold_db;
    config_.confidence_threshold = confidence_threshold;
    threshold_linear_ = db_to_linear(detection_threshold_db);
    return {};
}

Status Detector::consume(const engine::SpectrumFrame& frame) {
    if (frame.geometry.bins != geometry_.bins || frame.geometry.transform != geometry_.transform ||
        frame.geometry.channels != geometry_.channels ||
        frame.power_db.size() != bins_) {
        ++stats_.frames_rejected;
        return fail(std::format(
            "Detector::consume was given a frame of {} bins from a {}-channel, {}-point geometry "
            "and was built for {} bins from a {}-channel, {}-point one. The spectrum stage is "
            "built once with the coarse chain, so this is two engines rather than a resize",
            frame.power_db.size(), frame.geometry.channels, frame.geometry.transform, bins_,
            geometry_.channels, geometry_.transform));
    }

    // The frame's own window length is the bootstrap step, because at the
    // engine's defaults a block and a spectrum window are the same number of
    // source samples. After the first frame the real gap is used, so a frame
    // the engine skipped advances the average by the right amount rather than
    // by one step.
    std::uint64_t delta = frame.count;
    if (have_previous_ && frame.start > previous_start_) {
        delta = frame.start - previous_start_;
    }
    if (delta == 0) {
        delta = 1;
    }
    previous_start_ = frame.start;

    const double elapsed = static_cast<double>(delta) / static_cast<double>(config_.source_rate);
    const double alpha =
        std::clamp(1.0 - std::exp(-elapsed / config_.average_seconds), 1.0e-6, 1.0);
    have_previous_ = true;

    // The bias-corrected exponential average, and the correction is not
    // cosmetic.
    //
    // Seeding the average with the first frame and then running the plain
    // recursion leaves that one frame holding (1-alpha)^n of the answer, and
    // at a second of integration with frames eight milliseconds apart that is
    // still 37 percent of it a whole time constant later. The average then
    // looks settled while carrying the variance of about seven frames, which
    // put the detection threshold at under three standard deviations instead
    // of eight and produced twenty false detections per decision.
    //
    // Dividing by the accumulated weight renormalises the early frames down
    // as later ones arrive. The weight is a scalar and the variance at one
    // time constant comes out at ninety-two frames' worth instead of seven.
    weight_ += alpha * (1.0 - weight_);
    accumulate(frame, weight_ > 0.0 ? std::min(1.0, alpha / weight_) : 1.0);
    ++stats_.frames;

    samples_since_decision_ += delta;
    samples_accumulated_ += delta;

    // Nothing is decided until the average exists.
    //
    // The threshold is calibrated against an integrated statistic, and an
    // average one frame old is not that statistic: its noise is a factor of
    // sqrt(K) wider, so a threshold that sits at eight standard deviations
    // once the integration is up sits at under three at the first frame. Left
    // ungated that produced twenty false detections per decision for the
    // first second and then held every one of them for the length of the
    // bootstrap hold, which reads as a detector that invents signals rather
    // than as an average that has not filled.
    if (samples_accumulated_ < warmup_samples_) {
        return {};
    }

    if (samples_since_decision_ < decision_interval_samples_) {
        return {};
    }

    const dsp::SampleIndex now = frame.start + frame.count;
    const double since = have_decided_
                             ? static_cast<double>(samples_since_decision_) /
                                   static_cast<double>(config_.source_rate)
                             : config_.decision_interval_seconds;
    samples_since_decision_ = 0;
    decide(now, since);
    return {};
}

void Detector::accumulate(const engine::SpectrumFrame& frame, double alpha) {
    const std::span<const float> power = frame.power_db;
    for (std::size_t i = 0; i < bins_; ++i) {
        const double linear = static_cast<double>(std::exp2(power[i] * kDbToLog2));
        average_[i] += alpha * (linear - average_[i]);
    }
}

void Detector::decide(dsp::SampleIndex now, double elapsed_seconds) {
    estimate_noise_floor();
    find_candidates();
    reject_residual(elapsed_seconds);
    update_tracks(now, elapsed_seconds);

    last_decision_ = now;
    have_decided_ = true;
    ++stats_.decisions;
    stats_.candidates += candidates_.size();
}

void Detector::estimate_noise_floor() {
    const std::size_t knots = knot_floor_.size();
    const std::size_t half = noise_window_bins_ / 2;

    for (std::size_t k = 0; k < knots; ++k) {
        const auto centre = static_cast<std::size_t>(knot_position_[k]);
        const std::size_t low = centre > half ? centre - half : 0;
        const std::size_t high = std::min(bins_, centre + half + 1);

        window_.assign(average_.begin() + static_cast<std::ptrdiff_t>(low),
                       average_.begin() + static_cast<std::ptrdiff_t>(high));
        const std::size_t count = window_.size();
        if (count == 0) {
            knot_floor_[k] = static_cast<double>(dsp::kSpectrumPowerFloor);
            knot_sigma_[k] = 0.0;
            continue;
        }

        std::size_t low_rank = static_cast<std::size_t>(config_.noise_percentile *
                                                        static_cast<double>(count));
        std::size_t high_rank = static_cast<std::size_t>(config_.noise_percentile_high *
                                                         static_cast<double>(count));
        low_rank = std::min(low_rank, count - 1);
        high_rank = std::min(std::max(high_rank, low_rank), count - 1);

        std::nth_element(window_.begin(), window_.begin() + static_cast<std::ptrdiff_t>(low_rank),
                         window_.end());
        const double low_percentile = window_[low_rank];

        // The first call already put everything at or below low_rank on its
        // left, so the second only has to select within what is left. Skipped
        // when the two ranks coincide, which a one-bin window does: nth_element
        // with its nth before its first is undefined rather than a no-op.
        double high_percentile = low_percentile;
        if (high_rank > low_rank) {
            std::nth_element(window_.begin() + static_cast<std::ptrdiff_t>(low_rank) + 1,
                             window_.begin() + static_cast<std::ptrdiff_t>(high_rank),
                             window_.end());
            high_percentile = window_[high_rank];
        }

        // Two percentiles give a level and a spread. Anything more than a few
        // standard deviations over that level is a signal and is left out of
        // the mean; the rest is the floor.
        const double deviation =
            std::max(0.0, (high_percentile - low_percentile) / quantile_span_);
        const double level = high_percentile - high_quantile_ * deviation;
        const double ceiling = level + config_.noise_excision_sigma * deviation;

        double sum = 0.0;
        std::size_t kept = 0;
        for (std::size_t i = low; i < high; ++i) {
            if (average_[i] <= ceiling) {
                sum += average_[i];
                ++kept;
            }
        }
        const double estimate = kept > 0 ? sum / static_cast<double>(kept) : level;
        knot_floor_[k] = std::max(estimate, static_cast<double>(dsp::kSpectrumPowerFloor));

        // The same spread the excision used, kept rather than discarded. It
        // is the averaged noise's own ripple in linear power per bin, which
        // is what a growing edge has to clear: see edge_floor_sigma.
        knot_sigma_[k] = deviation;
    }

    if (knots == 1) {
        std::fill(floor_.begin(), floor_.end(), knot_floor_[0]);
        std::fill(sigma_.begin(), sigma_.end(), knot_sigma_[0]);
    } else {
        std::size_t k = 0;
        for (std::size_t i = 0; i < bins_; ++i) {
            const auto position = static_cast<double>(i);
            while (k + 2 < knots && knot_position_[k + 1] < position) {
                ++k;
            }
            const double span = knot_position_[k + 1] - knot_position_[k];
            const double t = span > 0.0 ? std::clamp((position - knot_position_[k]) / span, 0.0, 1.0)
                                        : 0.0;
            floor_[i] = knot_floor_[k] + t * (knot_floor_[k + 1] - knot_floor_[k]);
            sigma_[i] = knot_sigma_[k] + t * (knot_sigma_[k + 1] - knot_sigma_[k]);
        }
    }
}

void Detector::find_candidates() {
    candidates_.clear();

    excess_cumulative_[0] = 0.0;
    floor_cumulative_[0] = 0.0;
    sigma_cumulative_[0] = 0.0;
    for (std::size_t i = 0; i < bins_; ++i) {
        excess_cumulative_[i + 1] = excess_cumulative_[i] + (average_[i] - floor_[i]);
        floor_cumulative_[i + 1] = floor_cumulative_[i] + floor_[i];
        sigma_cumulative_[i + 1] = sigma_cumulative_[i] + sigma_[i];
    }

    // The summed-bin search, which is the whole reason this is not a per-bin
    // threshold.
    //
    // What comes out of each position and width is the deflection: the summed
    // excess over the standard deviation of a sum of that many noise bins.
    // That is what decides which width fits. A point signal's deflection
    // falls as 1/sqrt(W) and a signal of bandwidth B bins grows as
    // min(W,B)/sqrt(W), so the maximum over width sits at W = B, which is the
    // statement docs/detection.md makes about matching the bin width to the
    // signal bandwidth. It selects the scale and it is never the threshold,
    // because a deflection is not comparable across bandwidths.
    //
    // THE PEAK BUDGET IS A BEST-N SET, NOT A CUTOFF, AND THAT IS THE WHOLE
    // DIFFERENCE BETWEEN FINDING A BROADCAST STATION AND FINDING THIRTY
    // PIECES OF ONE
    //
    // max_peaks bounds the allocation. It used to do that by stopping the
    // search at the first peak past the bound, and the ladder is walked
    // narrowest rung first, so the bound was always spent on the rungs that
    // matter least and the widest rungs were never searched at all. The
    // failure is silent, it only appears once the frame is mostly signal, and
    // it gets worse as the threshold is lowered.
    //
    // Measured against an RTL-SDR at 98.1 MHz, 2.4 MS/s, 65536 bins, this
    // session. At the 6 dB default the four FM stations on that span produced
    // 103 tracks in eight seconds and ids past 200: the 98.1 MHz station came
    // back as about thirty tracks of 5 kHz each, evenly spaced, which is a
    // 128-bin rung tiling a 3000-bin signal because nothing wider was ever
    // reached. At 12 dB, where far fewer narrow windows clear their own
    // threshold and the bound is never reached, the same station came back as
    // ONE track of 106 to 116 kHz with a stable id. The signal did not change.
    //
    // So the bound keeps the strongest max_peaks windows instead of the first
    // max_peaks found. That costs a bounded heap past the bound and nothing
    // before it, it is the same set the greedy pass below was going to work
    // from anyway, and it removes the dependence on which order the rungs
    // happen to be walked in.
    //
    // It costs time, and the cost reads as a regression until you see where
    // it came from: stopping early was a speedup that arrived exactly when
    // the band got busy. The whole ladder is walked now whatever the frame
    // holds. Measured on the same eight seconds of 98.1 MHz, 1.56 ms per
    // frame before and 2.00 ms after, 5.7 percent of one core against 7.3.
    // Four tenths of a millisecond a frame buys the wide rungs, which are
    // the rungs that find a broadcast station.
    peaks_.clear();
    bool bounded = false;

    // Strongest first, with position and width breaking ties, so two equal
    // deflections cannot swap between runs. Shared by the heap and the sort
    // below because an eviction rule that disagreed with the sort order would
    // drop a peak the sort was about to rank first.
    const auto stronger = [](const Peak& a, const Peak& b) {
        if (a.deflection != b.deflection) {
            return a.deflection > b.deflection;
        }
        if (a.start != b.start) {
            return a.start < b.start;
        }
        return a.width < b.width;
    };

    for (const std::uint32_t width : widths_) {
        if (static_cast<std::size_t>(width) > bins_) {
            break;
        }
        const std::size_t last = bins_ - width;
        const double root = std::sqrt(static_cast<double>(width));

        // Only the deflection is computed per position. The SNR in the
        // reference bandwidth is a fixed multiple of it at a given width:
        // with E the summed excess and F the summed floor over W bins, the
        // deflection is E*sqrt(W)/F and the SNR is E*W*bin_width/(F*2500),
        // so the second is the first times sqrt(W)*bin_width/2500. Comparing
        // the deflection against a threshold scaled once per width is the
        // same test, and it takes one division per position where the obvious
        // form takes three. Measured at 65536 bins, that took a decision from
        // 9.3 ms to 7.4 ms.
        const double deflection_threshold =
            threshold_linear_ * kReferenceBandwidthHz / (root * bin_width_hz_);

        for (std::size_t s = 0; s <= last; ++s) {
            const double excess = excess_cumulative_[s + width] - excess_cumulative_[s];
            const double summed_floor = floor_cumulative_[s + width] - floor_cumulative_[s];
            deflection_[s] = summed_floor > 0.0
                                 ? static_cast<float>(excess * root / summed_floor)
                                 : 0.0F;
        }

        for (std::size_t s = 0; s <= last; ++s) {
            if (!(static_cast<double>(deflection_[s]) > deflection_threshold)) {
                continue;
            }
            const float here = deflection_[s];
            if (s > 0 && deflection_[s - 1] > here) {
                continue;
            }
            if (s < last && deflection_[s + 1] > here) {
                continue;
            }
            // A window narrower than the signal scores identically at every
            // position wholly inside it, so a maximum here is a plateau
            // rather than a point. Take its middle, which centres the seed on
            // the signal instead of on whichever end the scan reached last,
            // and emit one peak for the run rather than one per position.
            if (s > 0 && deflection_[s - 1] == here) {
                continue;
            }
            std::size_t end = s;
            while (end < last && deflection_[end + 1] == here) {
                ++end;
            }
            const std::size_t middle = s + (end - s) / 2;

            const Peak found{.start = static_cast<std::uint32_t>(middle),
                             .width = width,
                             .deflection = static_cast<double>(here)};

            if (peaks_.size() < peak_budget_) {
                peaks_.push_back(found);
                if (peaks_.size() == peak_budget_) {
                    // Ordered by `stronger`, so the heap's root is the peak
                    // that sorts LAST: the weakest, which is the one an
                    // eviction has to reach.
                    std::make_heap(peaks_.begin(), peaks_.end(), stronger);
                }
                continue;
            }

            bounded = true;
            if (stronger(found, peaks_.front())) {
                std::pop_heap(peaks_.begin(), peaks_.end(), stronger);
                peaks_.back() = found;
                std::push_heap(peaks_.begin(), peaks_.end(), stronger);
            }
        }
    }
    if (bounded) {
        ++stats_.peaks_overflowed;
    }

    // Strongest deflection first, so the width that fits a signal takes its
    // bins before a wider window that merely contains it can.
    std::sort(peaks_.begin(), peaks_.end(), stronger);

    // Strongest first, and each winner grows onto its own shoulders before
    // the next one is considered.
    //
    // The order matters and the growth has to be inside this loop rather than
    // after it. The ladder is powers of two, so a 21-bin signal is won by a
    // 16-bin window and the five bins it does not cover are still over the
    // threshold. Those bins produce their own narrow peaks, and a greedy pass
    // over ungrown seeds accepts them as separate detections sitting on the
    // shoulders of the real one. Growing the winner first makes them overlap
    // something already taken, which is what removes them.
    //
    // That contract has a precondition the paragraph above did not state and
    // an earlier growth rule did not meet: the winner has to grow all the way
    // to its own shoulders. It cannot suppress what it does not reach. The
    // level below was once a fraction of the seed's own mean excess, which on
    // a loud emitter sits tens of times over the noise, and a root raised
    // cosine at rolloff 0.5 has two thirds of its occupied band below that.
    // The skirt stayed outside accepted_, the clash test at the top of this
    // loop rejects a peak only where it OVERLAPS an accepted band, and each
    // shoulder was duly found again as its own candidate. See
    // edge_floor_fraction in detector.h for the measurements.
    accepted_.clear();
    for (const Peak& peak : peaks_) {
        std::size_t begin = peak.start;
        std::size_t end = begin + peak.width;

        const auto at = std::lower_bound(
            accepted_.begin(), accepted_.end(), begin,
            [](const Peak& held, std::size_t value) { return held.start < value; });

        bool clashes = at != accepted_.end() && static_cast<std::size_t>(at->start) < end;
        if (!clashes && at != accepted_.begin()) {
            const Peak& before = *(at - 1);
            clashes = static_cast<std::size_t>(before.start) + before.width > begin;
        }
        if (clashes) {
            continue;
        }

        // Bounded by the seed's own width on each side, so a detection cannot
        // run away across the span, and by whatever is already accepted
        // either side of it, so the set stays disjoint.
        //
        // The level is the larger of a multiple of the averaged noise's own
        // ripple and a multiple of its power, both per bin and both averaged
        // over the seed. The ripple bar is the working one and the power bar
        // is a backstop; see edge_floor_fraction and edge_floor_sigma for why
        // a level stated only in the second of them loses a weak wide signal.
        const double seed_floor = floor_cumulative_[end] - floor_cumulative_[begin];
        const double seed_sigma = sigma_cumulative_[end] - sigma_cumulative_[begin];
        const double level = std::max(config_.edge_floor_fraction * seed_floor,
                                      config_.edge_floor_sigma * seed_sigma) /
                             static_cast<double>(peak.width);
        const std::size_t reach = peak.width;
        const std::size_t low_limit =
            at == accepted_.begin()
                ? 0
                : static_cast<std::size_t>((at - 1)->start) + (at - 1)->width;
        const std::size_t high_limit = at == accepted_.end() ? bins_ : at->start;

        // Growth steps over a gap shorter than the one that would split the
        // band, which is the same constant read from the other side.
        //
        // A shaped signal has interior nulls. A root-raised-cosine BPSK
        // carrier has one between its main lobe and each sidelobe, two bins
        // wide at this resolution, and growth that stopped dead at the first
        // bin near the floor reported that one transmission as five: a main
        // lobe and four sidelobes, each with its own id and its own age. Two
        // bins is not a boundary between signals, and the rule that decides
        // what is has to be the same rule in both directions or the two
        // disagree about the same gap.
        const std::size_t low_stop = std::max(low_limit, begin > reach ? begin - reach : 0);
        while (begin > low_stop) {
            if (average_[begin - 1] - floor_[begin - 1] > level) {
                --begin;
                continue;
            }
            const std::size_t limit =
                std::min(static_cast<std::size_t>(config_.split_gap_bins), begin - low_stop);
            bool resumed = false;
            for (std::size_t back = 2; back <= limit; ++back) {
                if (average_[begin - back] - floor_[begin - back] > level) {
                    begin -= back;
                    resumed = true;
                    break;
                }
            }
            if (!resumed) {
                break;
            }
        }

        const std::size_t high_stop = std::min(high_limit, std::min(bins_, end + reach));
        while (end < high_stop) {
            if (average_[end] - floor_[end] > level) {
                ++end;
                continue;
            }
            const std::size_t limit =
                std::min(static_cast<std::size_t>(config_.split_gap_bins), high_stop - end);
            bool resumed = false;
            for (std::size_t ahead = 1; ahead < limit; ++ahead) {
                if (average_[end + ahead] - floor_[end + ahead] > level) {
                    end += ahead + 1;
                    resumed = true;
                    break;
                }
            }
            if (!resumed) {
                break;
            }
        }

        accepted_.insert(at, Peak{.start = static_cast<std::uint32_t>(begin),
                                  .width = static_cast<std::uint32_t>(end - begin),
                                  .deflection = peak.deflection});
    }

    // accepted_ is sorted by start and its entries are disjoint, so the
    // candidates come out ascending in frequency and the tracker's sweep can
    // stop early.
    for (const Peak& peak : accepted_) {
        const std::size_t begin = peak.start;
        const std::size_t end = begin + peak.width;

        // The occupied band: the ITU definition, the span holding
        // occupied_power_fraction of the excess with the rest split evenly
        // between the two tails. This is where fine bins do their job. The
        // summed window is as wide as the ladder rung that won, which for a
        // narrow carrier can be a thousand bins, and trimming it to the power
        // it actually carries is what turns a detection into a bandwidth.
        double total = 0.0;
        for (std::size_t i = begin; i < end; ++i) {
            total += std::max(0.0, average_[i] - floor_[i]);
        }
        if (total <= 0.0) {
            continue;
        }

        const double tail = total * (1.0 - config_.occupied_power_fraction) * 0.5;

        std::size_t low = begin;
        double running = 0.0;
        for (std::size_t i = begin; i < end; ++i) {
            const double excess = std::max(0.0, average_[i] - floor_[i]);
            if (running + excess > tail) {
                low = i;
                break;
            }
            running += excess;
        }

        std::size_t high = end - 1;
        running = 0.0;
        for (std::size_t i = end; i-- > begin;) {
            const double excess = std::max(0.0, average_[i] - floor_[i]);
            if (running + excess > tail) {
                high = i;
                break;
            }
            running += excess;
        }
        if (high < low) {
            high = low;
        }

        // Two adjacent signals deflect more together than either does alone,
        // so the ladder prefers the pair and hands back one band across both.
        // An interior run of bins sitting at the noise floor says that is
        // what happened.
        const std::size_t count = high - low + 1;
        separator_.assign(count, 0);
        std::size_t i = 0;
        while (i < count) {
            if (average_[low + i] > floor_[low + i] * gap_ratio_) {
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j < count && average_[low + j] <= floor_[low + j] * gap_ratio_) {
                ++j;
            }
            if (j - i >= config_.split_gap_bins) {
                std::fill(separator_.begin() + static_cast<std::ptrdiff_t>(i),
                          separator_.begin() + static_cast<std::ptrdiff_t>(j),
                          std::uint8_t{1});
            }
            i = j;
        }

        bool emitted = false;
        i = 0;
        while (i < count) {
            if (separator_[i] != 0) {
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j < count && separator_[j] == 0) {
                ++j;
            }
            emitted = emit_candidate(low + i, low + j - 1) || emitted;
            i = j;
        }
        if (!emitted) {
            static_cast<void>(emit_candidate(low, high));
        }
    }
}

bool Detector::emit_candidate(std::size_t first, std::size_t last) {
    if (last < first || last >= bins_) {
        return false;
    }
    const auto count = static_cast<double>(last - first + 1);
    const double excess = excess_cumulative_[last + 1] - excess_cumulative_[first];
    const double noise = (floor_cumulative_[last + 1] - floor_cumulative_[first]) / count;
    if (excess <= 0.0 || noise <= 0.0) {
        return false;
    }

    // The conversion the whole convention rests on. The noise power spectral
    // density is the per-bin floor over the bin width, so the noise inside
    // the 2500 Hz reference bandwidth is that density times 2500, and the
    // signal power is the excess summed over the band. The analysis window's
    // noise-equivalent bandwidth multiplies the signal sum and the noise sum
    // by the same factor and cancels out of the ratio exactly, which is why
    // no window term appears here.
    const double snr = excess * bin_width_hz_ / (noise * kReferenceBandwidthHz);
    if (!(snr > threshold_linear_)) {
        return false;
    }

    double peak = 0.0;
    for (std::size_t i = first; i <= last; ++i) {
        peak = std::max(peak, average_[i]);
    }

    const double low_edge = bin_zero_hz_ + (static_cast<double>(first) - 0.5) * bin_width_hz_;
    const double high_edge = bin_zero_hz_ + (static_cast<double>(last) + 0.5) * bin_width_hz_;

    Candidate candidate;
    candidate.first_bin = static_cast<std::uint32_t>(first);
    candidate.last_bin = static_cast<std::uint32_t>(last);
    candidate.snr_2500_db = 10.0 * std::log10(snr);
    candidate.noise_floor_dbfs = linear_to_db(noise);
    candidate.peak_dbfs = linear_to_db(peak);
    candidate.center =
        config_.source_center + static_cast<dsp::Hertz>(std::llround(0.5 * (low_edge + high_edge)));
    candidate.bandwidth = static_cast<dsp::Hertz>(std::llround(high_edge - low_edge));
    candidates_.push_back(candidate);
    return true;
}

void Detector::reject_residual(double elapsed_seconds) {
    // 10/ln(10). An exponential average with no input left decays by one
    // neper per time constant, and a neper is this many decibels, so this
    // over average_seconds is the dB per second a band falls at once the
    // thing that filled it has stopped. Nothing that is still transmitting
    // falls at exactly that rate, which is the whole discriminator. See
    // DetectorConfig::residual_decay_fraction.
    constexpr double kDbPerNeper = 4.342944819032518;

    decaying_next_.clear();
    decaying_next_.reserve(candidates_.size());

    std::size_t kept = 0;
    for (std::size_t c = 0; c < candidates_.size(); ++c) {
        const Candidate candidate = candidates_[c];

        // Matched by bin overlap rather than by identity, because this runs
        // before anything has one. Largest overlap wins, so a band that
        // splits while it decays does not hand its history to whichever
        // piece the sweep reached first.
        const Decaying* matched = nullptr;
        std::uint32_t best_overlap = 0;
        for (const Decaying& held : decaying_) {
            if (held.last_bin < candidate.first_bin || held.first_bin > candidate.last_bin) {
                continue;
            }
            const std::uint32_t overlap = std::min(held.last_bin, candidate.last_bin) -
                                          std::max(held.first_bin, candidate.first_bin) + 1U;
            if (overlap > best_overlap) {
                best_overlap = overlap;
                matched = &held;
            }
        }

        Decaying now{.first_bin = candidate.first_bin,
                     .last_bin = candidate.last_bin,
                     .snr_db = candidate.snr_2500_db,
                     .run_snr_db = candidate.snr_2500_db,
                     .run_seconds = 0.0,
                     .run = 0};

        // A run is consecutive STRICT falls, and the rate is measured over
        // the whole run rather than step by step. Per-decision SNR carries
        // the averaged noise's own ripple, which on a narrow band is a
        // sizeable fraction of one decision's worth of decay, so a
        // step-by-step rate test would need a tolerance wide enough to catch
        // anything. Over four decisions the ripple averages down and the
        // decay does not.
        if (matched != nullptr && candidate.snr_2500_db < matched->snr_db) {
            now.run = matched->run + 1;
            now.run_snr_db = matched->run_snr_db;
            now.run_seconds = matched->run_seconds + elapsed_seconds;
        }
        decaying_next_.push_back(now);

        const double expected = kDbPerNeper * now.run_seconds / config_.average_seconds;
        const bool residual = config_.residual_decisions > 0 &&
                              now.run >= config_.residual_decisions &&
                              (now.run_snr_db - candidate.snr_2500_db) >=
                                  config_.residual_decay_fraction * expected;
        if (residual) {
            ++stats_.candidates_residual;
            continue;
        }
        candidates_[kept] = candidate;
        ++kept;
    }

    candidates_.resize(kept);

    // An entry no candidate matched this decision is stale and is not
    // carried: the band it described is gone, and a run that resumed against
    // it later would be measuring across a hole.
    decaying_.swap(decaying_next_);
}

void Detector::update_tracks(dsp::SampleIndex now, double elapsed_seconds) {
    // Ascending in frequency, so the pair sweep below can stop rather than
    // walk every candidate for every track.
    std::sort(all_.begin(), all_.end(), [](const Track& a, const Track& b) {
        if (a.center != b.center) {
            return a.center < b.center;
        }
        return a.id < b.id;
    });

    const auto track_count = all_.size();
    const auto candidate_count = candidates_.size();

    const auto band_of_track = [](const Track& track) {
        const double half = 0.5 * static_cast<double>(std::max<dsp::Hertz>(1, track.bandwidth));
        return std::pair<double, double>{static_cast<double>(track.center) - half,
                                         static_cast<double>(track.center) + half};
    };
    const auto band_of_candidate = [](const Candidate& candidate) {
        const double half = 0.5 * static_cast<double>(std::max<dsp::Hertz>(1, candidate.bandwidth));
        return std::pair<double, double>{static_cast<double>(candidate.center) - half,
                                         static_cast<double>(candidate.center) + half};
    };

    pairs_.clear();
    for (std::size_t t = 0; t < track_count; ++t) {
        const Track& track = all_[t];
        const auto [track_low, track_high] = band_of_track(track);
        const double track_width = track_high - track_low;

        for (std::size_t c = 0; c < candidate_count; ++c) {
            const auto [low, high] = band_of_candidate(candidates_[c]);
            if (high < track_low) {
                continue;
            }
            if (low > track_high) {
                break;
            }
            const double width = high - low;
            const double overlap = std::min(track_high, high) - std::max(track_low, low);
            if (overlap <= 0.0) {
                continue;
            }

            // Gated on the permissive measure and ranked on the strict one.
            // The gate has to accept a track whose measured bandwidth moved,
            // or an id churns every time a signal breathes. The rank has to
            // prefer a candidate that matches a track's width over one that
            // merely swallows it, or the wide band produced by two signals
            // merging steals the id of whichever narrow track it happens to
            // reach first.
            if (overlap / std::min(track_width, width) < config_.association_overlap) {
                continue;
            }
            pairs_.push_back(Pair{.track = static_cast<std::uint32_t>(t),
                                  .candidate = static_cast<std::uint32_t>(c),
                                  .score = overlap / std::max(track_width, width),
                                  .first_seen = track.first_seen,
                                  .id = track.id});
        }
    }

    std::sort(pairs_.begin(), pairs_.end(), [](const Pair& a, const Pair& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        if (a.first_seen != b.first_seen) {
            return a.first_seen < b.first_seen;
        }
        if (a.id != b.id) {
            return a.id < b.id;
        }
        return a.candidate < b.candidate;
    });

    candidate_of_track_.assign(track_count, kNoAssignment);
    track_of_candidate_.assign(candidate_count, kNoAssignment);

    // One to one, taken best first. docs/detection.md: "associate by overlap"
    // is not a rule when two tracks overlap one candidate, because without an
    // assignment step both claim it and both look confirmed.
    for (const Pair& pair : pairs_) {
        if (candidate_of_track_[pair.track] != kNoAssignment) {
            continue;
        }
        if (track_of_candidate_[pair.candidate] != kNoAssignment) {
            continue;
        }
        candidate_of_track_[pair.track] = pair.candidate;
        track_of_candidate_[pair.candidate] = pair.track;
    }

    // The merge rule, applied after the assignment rather than folded into
    // its ranking. When several tracks are gated to one candidate the
    // candidate belongs to the oldest of them, which is the one with the most
    // evidence behind it, and the rest become its children. Letting the
    // ranking decide would hand the id to whichever track happened to have
    // the closest bandwidth that decision.
    std::vector<std::uint32_t> gated;
    for (std::size_t c = 0; c < candidate_count; ++c) {
        const std::uint32_t owner = track_of_candidate_[c];
        if (owner == kNoAssignment) {
            continue;
        }
        // Only a born track can be a party to a merge, on either side. A
        // pending one has not earned an identity yet, so it neither keeps a
        // candidate an older track wants nor survives as a child: it goes
        // back to being unassigned and the birth rule discards it. Letting a
        // pending track become a child instead handed a permanent id to
        // whatever a transition threw off, with one decision behind it.
        gated.clear();
        if (all_[owner].state != TrackState::Pending) {
            gated.push_back(owner);
        }
        for (const Pair& pair : pairs_) {
            if (pair.candidate != c || pair.track == owner) {
                continue;
            }
            if (candidate_of_track_[pair.track] != kNoAssignment) {
                continue;
            }
            if (all_[pair.track].state == TrackState::Pending) {
                continue;
            }
            gated.push_back(pair.track);
        }
        if (gated.empty() || (gated.size() == 1 && gated.front() == owner)) {
            continue;
        }

        std::uint32_t eldest = gated.front();
        for (const std::uint32_t t : gated) {
            const Track& a = all_[t];
            const Track& b = all_[eldest];
            if (std::tie(a.first_seen, a.id) < std::tie(b.first_seen, b.id)) {
                eldest = t;
            }
        }
        if (eldest != owner) {
            candidate_of_track_[owner] = kNoAssignment;
            candidate_of_track_[eldest] = static_cast<std::uint32_t>(c);
            track_of_candidate_[c] = eldest;
        }

        const std::uint64_t parent = all_[eldest].id;
        for (const std::uint32_t t : gated) {
            if (t == eldest) {
                continue;
            }
            Track& child = all_[t];
            if (child.state != TrackState::Merged || child.merged_into != parent) {
                ++stats_.merges;
            }
            child.state = TrackState::Merged;
            child.merged_into = parent;
            child.last_seen = now;
            // last_detected is deliberately NOT advanced, so the ordinary
            // hold still bounds a child. What the merge suspends is the
            // confidence decay, because the parent's detection is evidence
            // that something is in the child's band and a track being held
            // for a reason is not the same as one being held for none. A
            // merge that outlasts the hold drops the child and a later split
            // issues a new id, which is honest: the operator has not seen
            // that signal separately for longer than anything else is held.
            // Letting a child live for as long as its parent instead makes
            // any track that is ever swallowed immortal, including the
            // short-lived ones a transition throws off.
        }
    }

    const double decay = std::exp2(-elapsed_seconds / config_.confidence_half_life_seconds);
    const double hold_samples = config_.bootstrap_hold_seconds *
                                static_cast<double>(config_.source_rate);

    std::vector<Track> survivors;
    survivors.reserve(all_.size() + candidate_count);

    for (std::size_t t = 0; t < track_count; ++t) {
        Track track = all_[t];
        const std::uint32_t assigned = candidate_of_track_[t];

        if (assigned != kNoAssignment) {
            const Candidate& candidate = candidates_[assigned];

            if (track.state == TrackState::Merged) {
                ++stats_.splits;
            }
            track.merged_into = 0;

            const double smoothing =
                track.state == TrackState::Pending ? 1.0 : config_.centre_smoothing;
            const double centre = static_cast<double>(track.center) +
                                  smoothing * static_cast<double>(candidate.center - track.center);
            const double width =
                static_cast<double>(track.bandwidth) +
                smoothing * static_cast<double>(candidate.bandwidth - track.bandwidth);
            track.center = static_cast<dsp::Hertz>(std::llround(centre));
            track.bandwidth = std::max<dsp::Hertz>(1, static_cast<dsp::Hertz>(std::llround(width)));
            track.snr_2500_db += smoothing * (candidate.snr_2500_db - track.snr_2500_db);

            ++track.hits;
            track.misses = 0;
            track.last_detected = now;
            track.last_seen = now;
            track.confidence += (1.0 - track.confidence) * config_.confidence_rise;

            if (track.state == TrackState::Pending) {
                if (track.hits >= config_.birth_hits) {
                    track.state = TrackState::Live;
                    ++stats_.tracks_born;
                }
            } else {
                track.state = TrackState::Live;
            }

            update_channel(track);
            survivors.push_back(track);
            continue;
        }

        if (track.state == TrackState::Pending) {
            // Birth is consecutive or it is nothing. A candidate that appears
            // every other decision is noise crossing the threshold, and
            // counting it toward a birth would make birth_hits a ceiling on
            // the false-alarm rate rather than a floor under it.
            continue;
        }

        const bool merged = track.state == TrackState::Merged;
        if (!merged) {
            ++track.misses;
            track.state = TrackState::Held;
            track.confidence *= decay;
        }
        track.last_seen = now;

        const auto silent = static_cast<double>(now - track.last_detected);
        if (track.confidence < config_.drop_confidence || silent > hold_samples) {
            ++stats_.tracks_dropped;
            continue;
        }
        survivors.push_back(track);
    }

    for (std::size_t c = 0; c < candidate_count; ++c) {
        if (track_of_candidate_[c] != kNoAssignment) {
            continue;
        }
        if (survivors.size() >= config_.max_tracks) {
            ++stats_.births_refused;
            continue;
        }

        const Candidate& candidate = candidates_[c];
        Track track;
        track.id = next_id_++;
        track.state = TrackState::Pending;
        track.center = candidate.center;
        track.bandwidth = std::max<dsp::Hertz>(1, candidate.bandwidth);
        track.snr_2500_db = candidate.snr_2500_db;
        track.confidence = config_.confidence_rise;
        track.first_seen = now;
        track.last_seen = now;
        track.last_detected = now;
        track.hits = 1;
        if (track.hits >= config_.birth_hits) {
            track.state = TrackState::Live;
            ++stats_.tracks_born;
        }
        update_channel(track);
        survivors.push_back(track);
    }

    all_ = std::move(survivors);
    std::sort(all_.begin(), all_.end(), [](const Track& a, const Track& b) {
        if (a.center != b.center) {
            return a.center < b.center;
        }
        return a.id < b.id;
    });

    tracks_.clear();
    for (const Track& track : all_) {
        if (track.state != TrackState::Pending) {
            tracks_.push_back(track);
        }
    }
}

void Detector::update_channel(Track& track) const {
    if (config_.grid_channels == 0) {
        track.channel_valid = false;
        track.channel = 0;
        track.channel_index = 0;
        return;
    }

    const auto channels = static_cast<std::int64_t>(config_.grid_channels);
    const dsp::Hertz baseband = track.center - config_.source_center;
    const double exact = static_cast<double>(baseband) * static_cast<double>(channels) /
                         static_cast<double>(config_.source_rate);

    // place() rounds to the nearest channel, so a track parked on a boundary
    // flips every time the measurement moves a hertz, and each flip is a
    // different coarse channel, a different residual, a completely different
    // tap table and a half-megabyte upload. The extra distance below is what
    // stops that being a property of the noise.
    const double distance = std::abs(exact - static_cast<double>(track.channel_index));
    if (!track.channel_valid || distance > 0.5 + config_.channel_hysteresis) {
        track.channel_index = divide_nearest(baseband * channels, config_.source_rate);
        track.channel_valid = true;
    }

    std::int64_t wrapped = track.channel_index % channels;
    if (wrapped < 0) {
        wrapped += channels;
    }
    track.channel = static_cast<std::uint32_t>(wrapped);
}

}  // namespace revenant::detect
