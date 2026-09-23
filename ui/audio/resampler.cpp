#include "audio/resampler.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace revenant::ui {
namespace {

// The default response as fractions of the lower rate. See the header.
constexpr double kPassFraction = 0.42;
constexpr double kStopFraction = 0.50;

// The zeroth-order modified Bessel function of the first kind, by its power
// series. Twenty-five terms reach double precision for the argument a
// Kaiser window of 80 dB takes, which is under 8.
[[nodiscard]] double bessel_i0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    const double quarter = 0.25 * x * x;
    for (int k = 1; k < 25; ++k) {
        term *= quarter / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
    }
    return sum;
}

}  // namespace

void Resampler::configure(std::uint32_t in_rate, std::uint32_t out_rate, double cutoff_hz,
                          double transition_hz)
{
    in_rate_ = in_rate;
    out_rate_ = out_rate;
    table_.clear();
    half_width_ = 0;

    if (in_rate == 0 || out_rate == 0) {
        passthrough_ = true;
        step_ = 1.0;
        return;
    }
    step_ = static_cast<double>(in_rate) / static_cast<double>(out_rate);
    passthrough_ = in_rate == out_rate && cutoff_hz <= 0.0;
    if (passthrough_) {
        return;
    }

    const double lower = static_cast<double>(std::min(in_rate, out_rate));
    const double default_cutoff = 0.5 * (kPassFraction + kStopFraction) * lower;
    double cutoff = cutoff_hz > 0.0 ? std::min(cutoff_hz, default_cutoff) : default_cutoff;
    double transition = transition_hz > 0.0 ? transition_hz
                                             : (kStopFraction - kPassFraction) * lower;

    // The stopband may not start past the lower Nyquist, whatever was asked.
    transition = std::min(transition, 2.0 * (0.5 * lower - cutoff) + 1e-9);
    if (transition <= 0.0) {
        transition = (kStopFraction - kPassFraction) * lower;
        cutoff = default_cutoff;
    }

    // Kaiser's estimates, in input samples: the length from the stopband
    // depth and the transition width, and the window's shape from the depth.
    const double in = static_cast<double>(in_rate);
    const double length = (kStopbandDb - 8.0) * in / (2.285 * 2.0 * std::numbers::pi * transition);
    half_width_ = std::max(1, static_cast<int>(std::ceil(0.5 * length)));
    const double beta = 0.1102 * (kStopbandDb - 8.7);
    const double window_norm = bessel_i0(beta);

    // Cycles per input sample.
    const double f = cutoff / in;
    const double span = static_cast<double>(half_width_);

    const auto points = static_cast<std::size_t>(2 * half_width_ * kPhases + 1);
    table_.resize(points + 1, 0.0F);
    for (std::size_t i = 0; i < points; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(kPhases) - span;
        const double x = 2.0 * f * t;
        const double sinc =
            std::abs(x) < 1e-12 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
        const double r = t / span;
        const double window = r * r >= 1.0 ? 0.0 : bessel_i0(beta * std::sqrt(1.0 - r * r)) /
                                                        window_norm;
        table_[i] = static_cast<float>(2.0 * f * sinc * window);
    }
}

double Resampler::kernel(double t) const
{
    if (passthrough_) {
        return t == 0.0 ? 1.0 : 0.0;
    }
    const double span = static_cast<double>(half_width_);
    if (t <= -span || t >= span) {
        return 0.0;
    }
    const double at = (t + span) * static_cast<double>(kPhases);
    const auto index = static_cast<std::size_t>(at);
    const double frac = at - static_cast<double>(index);
    return static_cast<double>(table_[index]) +
           frac * (static_cast<double>(table_[index + 1]) - static_cast<double>(table_[index]));
}

void Resampler::evaluate(const float* history, std::uint64_t first, std::size_t frames,
                         int channels, double p, float* out) const
{
    for (int c = 0; c < channels; ++c) {
        out[c] = 0.0F;
    }
    if (history == nullptr || frames == 0) {
        return;
    }

    const double base = std::floor(p);
    const auto centre = static_cast<std::int64_t>(base);
    const double frac = p - base;

    if (passthrough_) {
        const std::int64_t at = centre - static_cast<std::int64_t>(first);
        if (at < 0 || at >= static_cast<std::int64_t>(frames)) {
            return;
        }
        for (int c = 0; c < channels; ++c) {
            out[c] = history[static_cast<std::size_t>(at) * static_cast<std::size_t>(channels) +
                             static_cast<std::size_t>(c)];
        }
        return;
    }

    // Accumulated in double: 224 products of a unit signal and a kernel that
    // sums to one lose nothing a float sum would notice at 80 dB, but the
    // accumulator costs nothing here and removes the question.
    double sums[8] = {};
    const int used = std::min(channels, 8);
    const auto first_index = static_cast<std::int64_t>(first);
    const auto last_index = first_index + static_cast<std::int64_t>(frames);
    const auto phases = static_cast<double>(kPhases);

    for (int tap = -half_width_ + 1; tap <= half_width_; ++tap) {
        const std::int64_t n = centre + tap;
        if (n < first_index || n >= last_index) {
            continue;
        }
        // t = p - n = frac - tap, which is inside (-half_width, half_width).
        const double at = (frac - static_cast<double>(tap) + static_cast<double>(half_width_)) *
                          phases;
        const auto index = static_cast<std::size_t>(at);
        const double part = at - static_cast<double>(index);
        const double weight = static_cast<double>(table_[index]) +
                              part * (static_cast<double>(table_[index + 1]) -
                                      static_cast<double>(table_[index]));
        const std::size_t row =
            static_cast<std::size_t>(n - first_index) * static_cast<std::size_t>(channels);
        for (int c = 0; c < used; ++c) {
            sums[c] += weight * static_cast<double>(history[row + static_cast<std::size_t>(c)]);
        }
    }
    for (int c = 0; c < used; ++c) {
        out[c] = static_cast<float>(sums[c]);
    }
}

}  // namespace revenant::ui
