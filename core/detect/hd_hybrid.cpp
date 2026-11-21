// core/detect/hd_hybrid.h has the test and why; this is the arithmetic.

#include "core/detect/hd_hybrid.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace revenant::detect {
namespace {

struct Sums {
    bool inside = false;
    double power = 0.0;
    double floor = 0.0;
    std::size_t bins = 0;

    [[nodiscard]] double excess() const { return std::max(0.0, power - floor); }
};

// Bins whose centres sit in [low_hz, high_hz], baseband.
class Bands {
public:
    Bands(std::span<const double> power, std::span<const double> floor, double bin_zero_hz,
          double bin_width_hz)
        : power_(power), floor_(floor), zero_(bin_zero_hz), width_(bin_width_hz) {}

    [[nodiscard]] Sums sum(double low_hz, double high_hz) const {
        Sums out;
        const double first = std::ceil((low_hz - zero_) / width_);
        const double last = std::floor((high_hz - zero_) / width_);
        if (!(first >= 0.0) || !(last < static_cast<double>(power_.size())) || last < first) {
            return out;
        }
        for (auto i = static_cast<std::size_t>(first); i <= static_cast<std::size_t>(last); ++i) {
            out.power += power_[i];
            out.floor += floor_[i];
            ++out.bins;
        }
        out.inside = out.bins > 0;
        return out;
    }

    // The analogue signal's excess-weighted centroid over centre +/- half.
    [[nodiscard]] double centroid(double centre_hz, double half_hz) const {
        const double first = std::max(0.0, std::ceil((centre_hz - half_hz - zero_) / width_));
        const double last = std::min(static_cast<double>(power_.size()) - 1.0,
                                     std::floor((centre_hz + half_hz - zero_) / width_));
        double weight = 0.0;
        double moment = 0.0;
        for (double b = first; b <= last; b += 1.0) {
            const auto i = static_cast<std::size_t>(b);
            const double excess = std::max(0.0, power_[i] - floor_[i]);
            weight += excess;
            moment += excess * (zero_ + b * width_);
        }
        return weight > 0.0 ? moment / weight : centre_hz;
    }

private:
    std::span<const double> power_;
    std::span<const double> floor_;
    double zero_;
    double width_;
};

[[nodiscard]] double ratio_db(double a, double b) {
    if (!(a > 0.0) || !(b > 0.0)) {
        return a > 0.0 ? 99.0 : -99.0;
    }
    return 10.0 * std::log10(a / b);
}

[[nodiscard]] double density(const Sums& s) {
    return s.bins == 0 ? 0.0 : s.power / static_cast<double>(s.bins);
}

}  // namespace

HdSidebandReading measure_hd_sidebands(std::span<const double> power,
                                       std::span<const double> floor,
                                       double bin_zero_hz,
                                       double bin_width_hz,
                                       double centre_hz) {
    HdSidebandReading out;
    if (power.size() != floor.size() || power.empty() || !(bin_width_hz > 0.0) ||
        !std::isfinite(centre_hz) || !std::isfinite(bin_zero_hz)) {
        return out;
    }
    const Bands bands(power, floor, bin_zero_hz, bin_width_hz);

    // The tracker's centre is the middle of whatever band it detected, and a
    // band that took in one block and not the other is off by tens of kHz.
    // The analogue signal is symmetric about the carrier, so its centroid is
    // the carrier; two passes walk in from a centre that started off it, and
    // the clamp keeps a centre that started on a neighbour from wandering.
    double centre = centre_hz;
    for (int pass = 0; pass < 2; ++pass) {
        centre = bands.centroid(centre, kHdAnalogueHalfHz);
    }
    centre = std::clamp(centre, centre_hz - kHdAnalogueHalfHz, centre_hz + kHdAnalogueHalfHz);
    out.centre_hz = centre;

    const Sums analogue = bands.sum(centre - kHdAnalogueHalfHz, centre + kHdAnalogueHalfHz);
    const Sums lower = bands.sum(centre - kHdOuterHz, centre - kHdInnerHz);
    const Sums upper = bands.sum(centre + kHdInnerHz, centre + kHdOuterHz);
    const Sums lower_gap = bands.sum(centre - kHdGapOuterHz, centre - kHdGapInnerHz);
    const Sums upper_gap = bands.sum(centre + kHdGapInnerHz, centre + kHdGapOuterHz);
    const double middle = 0.5 * (kHdInnerHz + kHdOuterHz);
    const Sums lower_inner = bands.sum(centre - middle, centre - kHdInnerHz);
    const Sums lower_outer = bands.sum(centre - kHdOuterHz, centre - middle);
    const Sums upper_inner = bands.sum(centre + kHdInnerHz, centre + middle);
    const Sums upper_outer = bands.sum(centre + middle, centre + kHdOuterHz);
    if (!analogue.inside || !lower.inside || !upper.inside || !lower_gap.inside ||
        !upper_gap.inside || !(analogue.excess() > 0.0)) {
        return out;
    }
    out.measured = true;

    out.lower_dbc = ratio_db(lower.excess(), analogue.excess());
    out.upper_dbc = ratio_db(upper.excess(), analogue.excess());
    out.lower_over_floor_db = ratio_db(lower.power, lower.floor);
    out.upper_over_floor_db = ratio_db(upper.power, upper.floor);
    out.lower_over_gap_db = ratio_db(density(lower), density(lower_gap));
    out.upper_over_gap_db = ratio_db(density(upper), density(upper_gap));
    out.lower_tilt_db = ratio_db(density(lower_inner), density(lower_outer));
    out.upper_tilt_db = ratio_db(density(upper_inner), density(upper_outer));

    out.hybrid = out.lower_over_floor_db >= kHdMinimumOverFloorDb &&
                 out.upper_over_floor_db >= kHdMinimumOverFloorDb &&
                 out.lower_over_gap_db >= kHdMinimumOverGapDb &&
                 out.upper_over_gap_db >= kHdMinimumOverGapDb &&
                 std::abs(out.lower_tilt_db) <= kHdMaximumTiltDb &&
                 std::abs(out.upper_tilt_db) <= kHdMaximumTiltDb &&
                 std::abs(out.lower_dbc - out.upper_dbc) <= kHdMaximumAsymmetryDb;
    return out;
}

}  // namespace revenant::detect
