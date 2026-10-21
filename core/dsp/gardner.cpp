#include "core/dsp/gardner.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace revenant::dsp {

namespace {

// Second order loop at a damping of 0.707, the detector gain taken as one
// because the error is normalised by the running power below.
constexpr double kDamping = 0.70710678118654752;

}  // namespace

GardnerTiming::GardnerTiming(double samples_per_symbol, double loop_bandwidth)
    : sps_(std::max(samples_per_symbol, 2.0)) {
    const double theta = loop_bandwidth / (kDamping + 0.25 / kDamping);
    const double d = 1.0 + 2.0 * kDamping * theta + theta * theta;
    kp_ = 4.0 * kDamping * theta / d;
    ki_ = 4.0 * theta * theta / d;
    reset();
}

void GardnerTiming::reset() {
    integrator_ = 0.0;
    correction_ = 0.0;
    power_ = 0.0;
    // One sample of history ahead of the first strobe, which the cubic needs.
    next_ = sps_ + 1.0;
    origin_ = 0;
    history_.clear();
    out_.clear();
    previous_ = {};
    have_previous_ = false;
}

Complex32 GardnerTiming::at(double position) const {
    if (history_.empty()) {
        return {};
    }
    const double local = position - static_cast<double>(origin_);
    const double base = std::floor(local);
    const auto f = static_cast<float>(local - base);
    const auto last = static_cast<std::int64_t>(history_.size()) - 1;
    const auto i = static_cast<std::int64_t>(base);
    const auto pick = [&](std::int64_t k) {
        return history_[static_cast<std::size_t>(std::clamp<std::int64_t>(k, 0, last))];
    };
    const float cm = -f * (f - 1.0F) * (f - 2.0F) / 6.0F;
    const float c0 = (f + 1.0F) * (f - 1.0F) * (f - 2.0F) / 2.0F;
    const float c1 = -(f + 1.0F) * f * (f - 2.0F) / 2.0F;
    const float c2 = (f + 1.0F) * f * (f - 1.0F) / 6.0F;
    return cm * pick(i - 1) + c0 * pick(i) + c1 * pick(i + 1) + c2 * pick(i + 2);
}

double GardnerTiming::phase() const {
    return correction_ - std::floor(correction_ + 0.5);
}

std::span<const GardnerTiming::Strobe> GardnerTiming::push(std::span<const Complex32> samples) {
    out_.clear();
    history_.insert(history_.end(), samples.begin(), samples.end());

    const auto end = static_cast<double>(origin_ + history_.size());
    while (std::floor(next_) + 2.0 < end) {
        const double step = period();
        const Complex32 y = at(next_);
        const Complex32 mid = at(next_ - 0.5 * step);
        const double p = std::norm(y);
        power_ = (power_ == 0.0) ? p : power_ + 0.01 * (p - power_);

        double error = 0.0;
        if (have_previous_ && power_ > 0.0) {
            // Rice's sign: negative when the strobes are late.
            const Complex32 diff = previous_ - y;
            error = static_cast<double>(mid.real() * diff.real() + mid.imag() * diff.imag()) /
                    power_;
            error = std::clamp(error, -1.0, 1.0);
        }
        out_.push_back(Strobe{y, next_});
        previous_ = y;
        have_previous_ = true;

        integrator_ += sps_ * ki_ * error;
        // A runaway integrator would walk off the symbol rate entirely.
        integrator_ = std::clamp(integrator_, -0.1 * sps_, 0.1 * sps_);
        const double advance = step + sps_ * kp_ * error;
        correction_ += (advance - sps_) / sps_;
        next_ += advance;
    }

    // An eye trace reads a symbol either side of a strobe, so everything from
    // a period before this call's first strobe stays until the next call.
    const double oldest = out_.empty() ? next_ : out_.front().position;
    const double keep_from = std::floor(oldest - 3.0 * period()) - 2.0;
    if (keep_from > static_cast<double>(origin_)) {
        const auto drop = std::min(static_cast<std::size_t>(keep_from - static_cast<double>(origin_)),
                                   history_.size());
        history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(drop));
        origin_ += drop;
    }
    return out_;
}

}  // namespace revenant::dsp
