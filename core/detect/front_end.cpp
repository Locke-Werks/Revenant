#include "core/detect/front_end.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "core/dsp/spectrum_reference.h"

namespace revenant::detect {

namespace {

// Ten times the base-ten logarithm, with the same clamp the spectrum kernel
// applies before its own logarithm, so a segment of a silent span lands on
// kSpectrumFloorDb rather than on negative infinity.
[[nodiscard]] double power_db(double linear)
{
    const double floored = std::max(linear, static_cast<double>(dsp::kSpectrumPowerFloor));
    return 10.0 * std::log10(floored);
}

}  // namespace

const char* front_end_verdict_name(FrontEndVerdict verdict)
{
    // No default case; see cmake/CompilerFlags.cmake.
    switch (verdict) {
        case FrontEndVerdict::Unmeasured: return "unmeasured";
        case FrontEndVerdict::Steady: return "steady";
        case FrontEndVerdict::SpanScales: return "span-scales";
        case FrontEndVerdict::FloorFollowsSignal: return "floor-follows-signal";
    }
    return "unknown";
}

void FrontEndMonitor::reset()
{
    *this = FrontEndMonitor{};
}

Status FrontEndMonitor::observe(std::span<const double> averaged_power,
                                std::span<const double> noise_floor,
                                double elapsed_seconds)
{
    if (averaged_power.size() != noise_floor.size()) {
        return fail(std::format(
            "FrontEndMonitor: the averaged spectrum holds {} bins and the noise floor holds "
            "{}. They come from one Detector and cannot differ, so this is two detectors "
            "feeding one monitor",
            averaged_power.size(), noise_floor.size()));
    }
    if (averaged_power.size() < kFrontEndSegments) {
        return fail(std::format(
            "FrontEndMonitor: {} bins is fewer than the {} segments the span is divided "
            "into, so a segment would hold no bins",
            averaged_power.size(), kFrontEndSegments));
    }
    if (!(elapsed_seconds > 0.0) || !std::isfinite(elapsed_seconds)) {
        return fail(std::format(
            "FrontEndMonitor: {} seconds since the previous decision is not a positive "
            "interval. Source time only moves forward here",
            elapsed_seconds));
    }

    const std::size_t bins = averaged_power.size();

    // One pass. Per segment the floor is a mean of the estimated floor over
    // its bins, and the drive is the strongest averaged bin anywhere on the
    // span, which is the level the products of any nonlinearity are driven
    // by. The mean and not a percentile: floor_ is already a percentile
    // estimate interpolated between knots, so re-ranking it would be taking
    // an order statistic of an order statistic.
    std::array<double, kFrontEndSegments> segment_floor{};
    std::array<std::size_t, kFrontEndSegments> segment_bins{};
    double peak = 0.0;
    for (std::size_t i = 0; i < bins; ++i) {
        const std::size_t segment =
            std::min(kFrontEndSegments - 1, i * kFrontEndSegments / bins);
        segment_floor[segment] += noise_floor[i];
        ++segment_bins[segment];
        peak = std::max(peak, averaged_power[i]);
    }

    const double drive_db = power_db(peak);

    // The decay is per source second rather than per decision, so a decision
    // interval that changes, or a decision missed because frames were
    // dropped, costs the window the right amount of weight.
    const double decay = std::exp(-elapsed_seconds / kFrontEndWindowSeconds);

    weight_ = weight_ * decay + 1.0;
    seconds_ = seconds_ * decay + elapsed_seconds;
    sum_p_ = sum_p_ * decay + drive_db;
    sum_pp_ = sum_pp_ * decay + drive_db * drive_db;

    double floor_total_db = 0.0;
    for (std::size_t s = 0; s < kFrontEndSegments; ++s) {
        const double floor_db =
            power_db(segment_bins[s] == 0
                         ? 0.0
                         : segment_floor[s] / static_cast<double>(segment_bins[s]));
        floor_total_db += floor_db;

        Segment& segment = segments_[s];
        segment.sum_f = segment.sum_f * decay + floor_db;
        segment.sum_ff = segment.sum_ff * decay + floor_db * floor_db;
        segment.sum_pf = segment.sum_pf * decay + drive_db * floor_db;
    }

    const double mean_floor_db = floor_total_db / static_cast<double>(kFrontEndSegments);
    if (!have_quiet_ || mean_floor_db < quiet_floor_db_) {
        quiet_floor_db_ = mean_floor_db;
        have_quiet_ = true;
    }

    FrontEndObservation out;
    out.floor_lift_db = mean_floor_db - quiet_floor_db_;

    const double mean_p = sum_p_ / weight_;
    const double var_p = std::max(0.0, sum_pp_ / weight_ - mean_p * mean_p);
    out.drive_spread_db = std::sqrt(var_p);

    if (seconds_ < kFrontEndMinSeconds || out.drive_spread_db < kFrontEndMinDriveSpreadDb) {
        verdict_ = FrontEndVerdict::Unmeasured;
        out.verdict = verdict_;
        last_ = out;
        return {};
    }

    // The weakest segment carries the verdict, because the claim is about
    // every segment of the span and one that is not following is the busy
    // band this has to refuse. Tracked as a pair rather than as two
    // independent minima: quoting one segment's slope beside another's
    // correlation would describe a segment that does not exist.
    double worst_slope = 0.0;
    double worst_correlation = 0.0;
    bool have_worst = false;
    for (const Segment& segment : segments_) {
        const double mean_f = segment.sum_f / weight_;
        const double var_f = std::max(0.0, segment.sum_ff / weight_ - mean_f * mean_f);
        const double covariance = segment.sum_pf / weight_ - mean_p * mean_f;

        const double slope = covariance / var_p;

        // A segment whose floor barely moved divides two small numbers here.
        // Its correlation is not ill-conditioned, it is zero: a floor that
        // did not move is not following anything.
        const double denominator = std::sqrt(var_p * var_f);
        const double correlation = denominator > 0.0 ? covariance / denominator : 0.0;

        if (!have_worst || slope < worst_slope) {
            worst_slope = slope;
            worst_correlation = correlation;
            have_worst = true;
        }
    }

    out.slope = worst_slope;
    out.correlation = worst_correlation;

    if (out.correlation < kFrontEndCorrelationFloor || out.slope < kFrontEndScalingSlope) {
        verdict_ = FrontEndVerdict::Steady;
    } else {
        const double enter = verdict_ == FrontEndVerdict::FloorFollowsSignal
                                 ? kFrontEndNonlinearLeave
                                 : kFrontEndNonlinearEnter;
        verdict_ = out.slope >= enter ? FrontEndVerdict::FloorFollowsSignal
                                      : FrontEndVerdict::SpanScales;
    }

    out.verdict = verdict_;
    last_ = out;
    return {};
}

FrontEndObservation FrontEndMonitor::observation() const
{
    return last_;
}

}  // namespace revenant::detect
