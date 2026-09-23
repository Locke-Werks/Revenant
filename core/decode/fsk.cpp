#include "core/decode/fsk.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace revenant::decode {

namespace {

// Running sums over a boxcar drift as rounding accumulates in the add and the
// subtract. Rebuilding the sum from the ring this often bounds the drift to
// what a few hundred windows can gather, which in double precision is far
// below anything a float output can show, while costing one extra pass per
// that many windows.
constexpr std::uint64_t kResumEveryWindows = 256;

// exp(-j * 2*pi * f * n / rate), with the phase reduced in integers so it is
// exact at any n. Both factors are below rate after reduction, so the
// product is below rate^2, which fits in 64 bits for any rate this engine
// accepts.
std::complex<double> mixer(Hertz frequency, SampleRate rate, std::uint64_t n) {
    const auto r = static_cast<std::uint64_t>(rate);
    const auto f = static_cast<std::uint64_t>(frequency) % r;
    const std::uint64_t cycle = (f * (n % r)) % r;
    const double angle =
        -2.0 * std::numbers::pi * static_cast<double>(cycle) / static_cast<double>(rate);
    return {std::cos(angle), std::sin(angle)};
}

std::size_t window_for(SampleRate rate, double symbol_rate) {
    const double samples = static_cast<double>(rate) / symbol_rate;
    return std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(samples)));
}

}  // namespace

// ---------------------------------------------------------------------------
// ToneDiscriminator
// ---------------------------------------------------------------------------

Expected<ToneDiscriminator> ToneDiscriminator::create(const ToneDiscriminatorConfig& config) {
    if (config.rate <= 0) {
        return fail("a tone discriminator needs a positive sample rate");
    }
    if (!(config.symbol_rate > 0.0) ||
        config.symbol_rate * 2.0 > static_cast<double>(config.rate)) {
        return fail("a tone discriminator needs a bit rate between zero and half the sample rate");
    }
    if (config.mark_hz <= 0 || config.space_hz <= 0 || config.mark_hz * 2 >= config.rate ||
        config.space_hz * 2 >= config.rate) {
        return fail("both tones must lie between zero and half the sample rate");
    }
    if (config.mark_hz == config.space_hz) {
        return fail("mark and space must be different frequencies");
    }

    ToneDiscriminator d;
    d.rate_ = config.rate;
    d.mark_hz_ = config.mark_hz;
    d.space_hz_ = config.space_hz;
    d.window_ = window_for(config.rate, config.symbol_rate);
    d.reset();
    return d;
}

void ToneDiscriminator::reset() {
    mark_ring_.assign(window_, {});
    space_ring_.assign(window_, {});
    mark_sum_ = {};
    space_sum_ = {};
    ring_pos_ = 0;
    count_ = 0;
}

void ToneDiscriminator::process(ConstRealSpan in, std::vector<float>& soft) {
    soft.reserve(soft.size() + in.size());
    const std::uint64_t resum_period = static_cast<std::uint64_t>(window_) * kResumEveryWindows;

    for (const float sample : in) {
        const double x = static_cast<double>(sample);
        const std::complex<double> m = x * mixer(mark_hz_, rate_, count_);
        const std::complex<double> s = x * mixer(space_hz_, rate_, count_);

        mark_sum_ += m - mark_ring_[ring_pos_];
        space_sum_ += s - space_ring_[ring_pos_];
        mark_ring_[ring_pos_] = m;
        space_ring_[ring_pos_] = s;
        ring_pos_ = (ring_pos_ + 1 == window_) ? 0 : ring_pos_ + 1;
        ++count_;

        if (count_ % resum_period == 0) {
            mark_sum_ = {};
            space_sum_ = {};
            for (std::size_t i = 0; i < window_; ++i) {
                mark_sum_ += mark_ring_[i];
                space_sum_ += space_ring_[i];
            }
        }

        const double pm = std::norm(mark_sum_);
        const double ps = std::norm(space_sum_);
        const double total = pm + ps;
        soft.push_back(total > 0.0 ? static_cast<float>((pm - ps) / total) : 0.0F);
    }
}

// ---------------------------------------------------------------------------
// LevelDiscriminator
// ---------------------------------------------------------------------------

Expected<LevelDiscriminator> LevelDiscriminator::create(const LevelDiscriminatorConfig& config) {
    if (config.rate <= 0) {
        return fail("a level discriminator needs a positive sample rate");
    }
    if (!(config.symbol_rate > 0.0) ||
        config.symbol_rate * 2.0 > static_cast<double>(config.rate)) {
        return fail("a level discriminator needs a bit rate between zero and half the sample rate");
    }
    if (!(config.tracking_bits >= 1.0)) {
        return fail("the level tracker needs at least one bit to average over");
    }

    LevelDiscriminator d;
    d.window_ = window_for(config.rate, config.symbol_rate);
    d.alpha_ = config.symbol_rate / (config.tracking_bits * static_cast<double>(config.rate));
    d.reset();
    return d;
}

void LevelDiscriminator::reset() {
    ring_.assign(window_, 0.0);
    sum_ = 0.0;
    ring_pos_ = 0;
    count_ = 0;
    mean_ = 0.0;
    level_ = 0.0;
    primed_ = false;
}

void LevelDiscriminator::process(ConstRealSpan in, std::vector<float>& soft) {
    soft.reserve(soft.size() + in.size());
    const std::uint64_t resum_period = static_cast<std::uint64_t>(window_) * kResumEveryWindows;
    const double scale = 1.0 / static_cast<double>(window_);

    for (const float sample : in) {
        const double x = static_cast<double>(sample);
        sum_ += x - ring_[ring_pos_];
        ring_[ring_pos_] = x;
        ring_pos_ = (ring_pos_ + 1 == window_) ? 0 : ring_pos_ + 1;
        ++count_;
        if (count_ % resum_period == 0) {
            sum_ = 0.0;
            for (const double v : ring_) {
                sum_ += v;
            }
        }

        const double y = sum_ * scale;
        if (!primed_) {
            // Start the level tracker from the first magnitude rather than
            // from zero, so the first bits out are scaled rather than
            // divided by nothing.
            level_ = std::abs(y);
            primed_ = true;
        }
        mean_ += alpha_ * (y - mean_);
        const double centred = y - mean_;
        level_ += alpha_ * (std::abs(centred) - level_);
        soft.push_back(level_ > 0.0 ? static_cast<float>(centred / level_) : 0.0F);
    }
}

// ---------------------------------------------------------------------------
// BitClock
// ---------------------------------------------------------------------------

Expected<BitClock> BitClock::create(const BitClockConfig& config) {
    if (config.rate <= 0) {
        return fail("a bit clock needs a positive sample rate");
    }
    if (!(config.symbol_rate > 0.0) ||
        config.symbol_rate * 2.0 > static_cast<double>(config.rate)) {
        return fail("a bit clock needs a bit rate between zero and half the sample rate");
    }
    if (!(config.phase_gain > 0.0 && config.phase_gain <= 1.0) ||
        !(config.frequency_gain >= 0.0 && config.frequency_gain <= 1.0) ||
        !(config.max_rate_error >= 0.0 && config.max_rate_error < 0.5)) {
        return fail("bit clock gains must lie in (0, 1] and the rate bound in [0, 0.5)");
    }

    BitClock c;
    c.nominal_step_ = config.symbol_rate / static_cast<double>(config.rate);
    c.phase_gain_ = config.phase_gain;
    c.frequency_gain_ = config.frequency_gain;
    c.max_rate_error_ = config.max_rate_error;
    c.reset();
    return c;
}

void BitClock::reset() {
    step_ = nominal_step_;
    phase_ = 0.0;
    emitted_ = false;
    previous_ = 0.0F;
    have_previous_ = false;
    index_ = 0;
}

void BitClock::process(ConstRealSpan soft, std::vector<SoftBit>& out) {
    const double low = nominal_step_ * (1.0 - max_rate_error_);
    const double high = nominal_step_ * (1.0 + max_rate_error_);

    for (const float s : soft) {
        const double before = phase_;
        phase_ += step_;

        // Read the bit where the accumulator passes the middle of it, between
        // the previous sample and this one.
        if (!emitted_ && phase_ >= 0.5 && have_previous_) {
            const double u = std::clamp((0.5 - before) / step_, 0.0, 1.0);
            SoftBit bit;
            bit.value = static_cast<float>(static_cast<double>(previous_) +
                                           u * static_cast<double>(s - previous_));
            bit.position = (u < 0.5) ? index_ - 1 : index_;
            out.push_back(bit);
            emitted_ = true;
        }

        if (have_previous_ && ((previous_ < 0.0F) != (s < 0.0F))) {
            const double denominator = static_cast<double>(previous_) - static_cast<double>(s);
            const double t = (denominator != 0.0)
                                 ? std::clamp(static_cast<double>(previous_) / denominator, 0.0, 1.0)
                                 : 0.5;
            // Phase at the crossing, relative to the nearest bit boundary.
            // Boundaries sit at whole numbers of the accumulator.
            double error = before + t * step_;
            error -= std::floor(error + 0.5);

            phase_ -= phase_gain_ * error;
            step_ = std::clamp(step_ - frequency_gain_ * error * nominal_step_, low, high);
        }

        if (phase_ >= 1.0) {
            phase_ -= 1.0;
            emitted_ = false;
        } else if (phase_ < 0.0) {
            // A correction backwards across a boundary lands in the tail of
            // the bit already read.
            phase_ += 1.0;
            emitted_ = true;
        }

        previous_ = s;
        have_previous_ = true;
        ++index_;
    }
}

}  // namespace revenant::decode
