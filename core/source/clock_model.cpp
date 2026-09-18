// Sample index to wall clock, with the uncertainty stated rather than
// implied.
//
// Two things happen here and they are kept apart on purpose. Deriving a time
// from an index is exact integer arithmetic against a declared rate, split so
// that a long capture cannot overflow it. Correcting the declaration against
// a reference is a weighted least-squares fit in double, run off the sample
// path, whose output is a new segment in a table that the exact arithmetic
// then reads. Nothing in the first half depends on the second having run.

#include "core/source/clock_model.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace revenant::source {
namespace {

// A fractional frequency error past this is a broken fit rather than a clock.
// One percent is ten thousand times an RTL-SDR v3's TCXO tolerance and a
// hundred times what the worst crystal in a dongle drifts to when hot.
// Clamping keeps a pathological window from producing a divide that lands
// near zero in elapsed_ns.
constexpr double kMaxRateErrorPpm = 10'000.0;

// Backstop on the segment table. The append rule below already collapses a
// correction that sits inside the previous segment's own error bar, so a
// stable disciplined clock produces a handful of segments an hour rather than
// one a second. This bounds the pathological case where an oscillator is
// stepping continuously.
constexpr std::size_t kMaxSegments = 1024;

[[nodiscard]] double clamp_ppm(double ppm)
{
    if (!std::isfinite(ppm)) {
        return 0.0;
    }
    return std::clamp(ppm, -kMaxRateErrorPpm, kMaxRateErrorPpm);
}

}  // namespace

Expected<ClockModel> ClockModel::create(const ClockModelConfig& config)
{
    if (config.rate <= 0) {
        return fail(std::format("clock model needs a positive sample rate, got {}", config.rate));
    }
    if (config.anchor_accuracy_ns < 0) {
        return fail(std::format("anchor accuracy must not be negative, got {} ns",
                                config.anchor_accuracy_ns));
    }
    if (!std::isfinite(config.oscillator_tolerance_ppm) || config.oscillator_tolerance_ppm < 0.0) {
        return fail("oscillator tolerance must be a finite number of parts per million, "
                    "zero or above");
    }
    if (config.fit_window_edges < 2) {
        // One edge determines an offset and nothing about the rate, so a
        // window of one is a model that can never learn the thing the loop
        // exists to learn.
        return fail("the discipline fit needs a window of at least two edges");
    }
    if (!std::isfinite(config.outlier_sigma) || config.outlier_sigma <= 0.0) {
        return fail("outlier rejection threshold must be a positive number of sigma");
    }

    ClockModel model;
    model.config_ = config;
    return model;
}

Status ClockModel::set_anchor(std::int64_t epoch_anchor_ns)
{
    if (anchored_) {
        return fail("the clock anchor is established once at stream start and then corrected, "
                    "never replaced: re-anchoring would discard a disciplined fit in favour of "
                    "a worse number with nothing recording that it happened");
    }

    anchored_ = true;
    stream_anchor_ns_ = epoch_anchor_ns;

    ClockSegment first;
    first.effective_from_index = 0;
    first.anchor_ns = epoch_anchor_ns;
    first.rate_error_ppm = 0.0;
    first.accuracy_ns = config_.anchor_accuracy_ns;

    // Undisciplined, the growth rate is the oscillator's open-loop tolerance.
    // Disciplined, it becomes the measured uncertainty of the rate estimate,
    // which is what refit() writes. One formula covers both because both are
    // answering the same question: how fast does the prediction get worse.
    first.rate_error_accuracy_ppm = config_.oscillator_tolerance_ppm;

    segments_.clear();
    segments_.push_back(first);
    return {};
}

void ClockModel::note_progress(dsp::SampleIndex write_index)
{
    write_index_ = std::max(write_index_, write_index);
}

std::int64_t ClockModel::elapsed_ns(dsp::SampleIndex samples, double rate_error_ppm) const
{
    if (config_.rate <= 0) {
        return 0;
    }

    // Split the division the same way BlockTimestamp::wall_clock_ns does, and
    // for the same reason: samples * 1e9 overflows a signed 64-bit integer
    // fifteen minutes into a 20 MS/s capture.
    const auto rate_u = static_cast<dsp::SampleIndex>(config_.rate);
    const auto whole_seconds = static_cast<std::int64_t>(samples / rate_u);
    const auto remainder = static_cast<std::int64_t>(samples % rate_u);
    const std::int64_t nominal =
        whole_seconds * 1'000'000'000 + (remainder * 1'000'000'000) / config_.rate;

    const double error = clamp_ppm(rate_error_ppm) * 1e-6;
    if (error == 0.0) {
        return nominal;
    }

    // A clock running fast covers the same sample count in less real time, so
    // the true elapsed interval is the nominal one divided by (1 + error).
    // The correction is formed separately and added, so the integer part
    // stays exact and only the small term goes through a double.
    const double correction = -static_cast<double>(nominal) * error / (1.0 + error);
    return nominal + static_cast<std::int64_t>(std::llround(correction));
}

const ClockSegment& ClockModel::segment_for(dsp::SampleIndex index) const
{
    // Segments are sorted and the first begins at index zero, so the answer is
    // the last one that starts at or before the query.
    const auto above = std::upper_bound(
        segments_.begin(), segments_.end(), index,
        [](dsp::SampleIndex value, const ClockSegment& segment) {
            return value < segment.effective_from_index;
        });
    return *std::prev(above);
}

std::int64_t ClockModel::wall_clock_ns(dsp::SampleIndex index) const
{
    if (segments_.empty()) {
        return 0;
    }
    const ClockSegment& segment = segment_for(index);
    return segment.anchor_ns +
           elapsed_ns(index - segment.effective_from_index, segment.rate_error_ppm);
}

std::int64_t ClockModel::accuracy_ns_at(dsp::SampleIndex index) const
{
    if (segments_.empty()) {
        return 0;
    }
    const ClockSegment& segment = segment_for(index);
    const std::int64_t since = elapsed_ns(index - segment.effective_from_index, 0.0);
    const double growth =
        static_cast<double>(since) * std::abs(segment.rate_error_accuracy_ppm) * 1e-6;
    if (!std::isfinite(growth)) {
        return segment.accuracy_ns;
    }
    return segment.accuracy_ns + static_cast<std::int64_t>(std::llround(growth));
}

ClockQuality ClockModel::quality() const
{
    ClockQuality out;
    out.source = config_.source;
    out.disciplined = disciplined_;
    out.residual_ns = last_residual_ns_;
    if (!segments_.empty()) {
        out.ppm_error = segments_.back().rate_error_ppm;
    }
    out.accuracy_ns = accuracy_ns_at(write_index_);
    return out;
}

Status ClockModel::observe(const ReferenceEdge& edge)
{
    if (!anchored_) {
        return fail("a reference edge arrived before the clock anchor was established");
    }
    if (config_.rate <= 0) {
        return fail("the clock model has no sample rate, so an edge cannot be interpreted");
    }

    // Measured against the model as it stands, before this edge is folded in.
    // That makes it an out-of-sample error and therefore the number worth
    // watching: a fit that has started to lag its reference shows up here
    // before any timestamp is visibly wrong.
    const std::int64_t predicted = wall_clock_ns(edge.sample_index);
    const std::int64_t residual = edge.unix_ns - predicted;

    // Rejection only once there is enough history for the model to be worth
    // believing. Before that an outlier and a correct first observation look
    // the same, and throwing the correct one away leaves the loop unable to
    // acquire.
    constexpr std::size_t kMinimumEdgesBeforeRejecting = 4;
    if (window_.size() >= kMinimumEdgesBeforeRejecting) {
        const double edge_sigma = (edge.accuracy_ns > 0)
                                      ? static_cast<double>(edge.accuracy_ns)
                                      : static_cast<double>(accuracy_ns_at(edge.sample_index));
        const double threshold = config_.outlier_sigma * std::max(edge_sigma, 1.0);
        if (static_cast<double>(std::abs(residual)) > threshold) {
            ++edges_rejected_;
            last_residual_ns_ = residual;
            return {};
        }
    }

    window_.push_back(edge);
    if (window_.size() > config_.fit_window_edges) {
        window_.erase(window_.begin(),
                      window_.begin() +
                          static_cast<std::ptrdiff_t>(window_.size() - config_.fit_window_edges));
    }

    ++edges_used_;
    last_residual_ns_ = residual;
    refit();
    return {};
}

void ClockModel::refit()
{
    if (window_.size() < 2) {
        return;
    }

    // The best an edge can be placed against the sample stream, absent a
    // stated figure, is one sample period. Using that as the default weight
    // keeps an edge that declared nothing from dominating one that declared
    // a real uncertainty.
    const double sample_period_ns = 1e9 / static_cast<double>(config_.rate);
    const double default_sigma = std::max(sample_period_ns, 1.0);

    const dsp::SampleIndex origin = window_.front().sample_index;

    double sum_w = 0.0;
    double sum_wx = 0.0;
    double sum_wxx = 0.0;
    double sum_wy = 0.0;
    double sum_wxy = 0.0;

    for (const ReferenceEdge& edge : window_) {
        const double sigma =
            (edge.accuracy_ns > 0) ? static_cast<double>(edge.accuracy_ns) : default_sigma;
        const double weight = 1.0 / (sigma * sigma);

        const double x = static_cast<double>(edge.sample_index - origin);

        // Against the uncorrected declaration, not against the current fit,
        // so the coefficients are absolute rather than incremental and a
        // rounding error in one correction cannot compound into the next.
        const std::int64_t base = stream_anchor_ns_ + elapsed_ns(edge.sample_index, 0.0);
        const double y = static_cast<double>(edge.unix_ns - base);

        sum_w += weight;
        sum_wx += weight * x;
        sum_wxx += weight * x * x;
        sum_wy += weight * y;
        sum_wxy += weight * x * y;
    }

    const double determinant = sum_w * sum_wxx - sum_wx * sum_wx;
    if (!std::isfinite(determinant) || determinant <= 0.0) {
        // Every edge landed on the same index, which is not two observations
        // of a rate however many rows the window holds.
        return;
    }

    const double offset = (sum_wxx * sum_wy - sum_wx * sum_wxy) / determinant;
    const double slope = (sum_w * sum_wxy - sum_wx * sum_wy) / determinant;
    if (!std::isfinite(offset) || !std::isfinite(slope)) {
        return;
    }

    // Covariance of a weighted straight-line fit, with the weights already
    // being inverse variances.
    const double var_offset = sum_wxx / determinant;
    const double var_slope = sum_w / determinant;
    const double cov = -sum_wx / determinant;

    const dsp::SampleIndex at = window_.back().sample_index;
    const double x_at = static_cast<double>(at - origin);

    // Exact inversion rather than the small-error approximation. slope is
    // nanoseconds of correction per sample, so the corrected interval per
    // sample is 1e9/rate + slope.
    const double denominator = 1e9 + static_cast<double>(config_.rate) * slope;
    if (!std::isfinite(denominator) || std::abs(denominator) < 1.0) {
        return;
    }
    const double error = -static_cast<double>(config_.rate) * slope / denominator;
    const double rate_error_ppm = clamp_ppm(error * 1e6);

    // Linearised, which is exact to well beyond the precision that matters at
    // any rate error this clamps to.
    const double rate_error_accuracy_ppm =
        std::sqrt(std::max(var_slope, 0.0)) * static_cast<double>(config_.rate) * 1e-3;

    const double prediction_variance =
        var_offset + 2.0 * cov * x_at + var_slope * x_at * x_at;
    const double prediction_sigma = std::sqrt(std::max(prediction_variance, 0.0));

    const std::int64_t base_at = stream_anchor_ns_ + elapsed_ns(at, 0.0);
    const double correction_at = offset + slope * x_at;
    if (!std::isfinite(correction_at) || !std::isfinite(prediction_sigma)) {
        return;
    }

    ClockSegment fitted;
    fitted.effective_from_index = at;
    fitted.anchor_ns = base_at + static_cast<std::int64_t>(std::llround(correction_at));
    fitted.rate_error_ppm = rate_error_ppm;
    fitted.accuracy_ns = static_cast<std::int64_t>(std::llround(prediction_sigma));
    fitted.rate_error_accuracy_ppm = rate_error_accuracy_ppm;

    disciplined_ = true;

    // Append only when the correction is larger than the error bar the last
    // segment already declared. A refinement that lands inside its own stated
    // uncertainty changes nothing a reader was entitled to rely on, so it
    // goes in place; a correction that lands outside it is a different answer
    // and gets its own row, which is what keeps a dated query resolving with
    // what was known at the time.
    ClockSegment& last = segments_.back();
    const std::int64_t previous_answer =
        last.anchor_ns + elapsed_ns(at - last.effective_from_index, last.rate_error_ppm);
    const std::int64_t moved = std::abs(fitted.anchor_ns - previous_answer);
    const std::int64_t tolerance = std::max<std::int64_t>(accuracy_ns_at(at), 1);

    if (moved <= tolerance || segments_.size() >= kMaxSegments ||
        last.effective_from_index == at) {
        // Keep the interval this segment already covers: only the model
        // inside it is refined.
        last.anchor_ns =
            fitted.anchor_ns -
            elapsed_ns(at - last.effective_from_index, fitted.rate_error_ppm);
        last.rate_error_ppm = fitted.rate_error_ppm;
        last.accuracy_ns = fitted.accuracy_ns;
        last.rate_error_accuracy_ppm = fitted.rate_error_accuracy_ppm;
        return;
    }

    segments_.push_back(fitted);
}

}  // namespace revenant::source
