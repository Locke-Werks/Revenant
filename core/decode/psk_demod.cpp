#include "core/decode/psk_demod.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace revenant::decode {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;
// Second order loop damping, the usual compromise between overshoot and
// settling (Gardner, Phaselock Techniques, chapter 2).
constexpr double kDamping = 0.70710678118654752;
// Lock metric averaging: about 64 symbols.
constexpr double kLockAlpha = 1.0 / 64.0;
constexpr double kPowerAlpha = 1.0 / 128.0;

// In-place iterative radix-2 FFT. Size must be a power of two.
void fft(std::vector<std::complex<double>>& x) {
    const std::size_t n = x.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1U;
        for (; (j & bit) != 0; bit >>= 1U) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(x[i], x[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1U) {
        const double angle = -kTwoPi / static_cast<double>(len);
        const std::complex<double> w_len(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = x[i + k];
                const auto v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
                w *= w_len;
            }
        }
    }
}

double point_offset(PskModulation modulation) {
    return (modulation == PskModulation::Qpsk || modulation == PskModulation::Oqpsk)
               ? std::numbers::pi / 4.0
               : 0.0;
}

}  // namespace

unsigned psk_order(PskModulation modulation) {
    switch (modulation) {
        case PskModulation::Bpsk: return 2;
        case PskModulation::Qpsk:
        case PskModulation::Oqpsk: return 4;
        case PskModulation::Psk8: return 8;
    }
    return 2;
}

unsigned psk_bits_per_symbol(PskModulation modulation) {
    switch (psk_order(modulation)) {
        case 2: return 1;
        case 4: return 2;
        default: return 3;
    }
}

Complex32 psk_point(PskModulation modulation, unsigned k) {
    const unsigned m = psk_order(modulation);
    const double angle = point_offset(modulation) + kTwoPi * static_cast<double>(k % m) /
                                                        static_cast<double>(m);
    return {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
}

std::array<std::uint8_t, 8> psk_gray_map() {
    std::array<std::uint8_t, 8> map{};
    for (unsigned k = 0; k < map.size(); ++k) {
        map[k] = static_cast<std::uint8_t>(k ^ (k >> 1U));
    }
    return map;
}

Expected<PskDemod> PskDemod::create(const PskDemodConfig& config) {
    if (config.rate <= 0 || !(config.symbol_rate > 0.0)) {
        return fail("PSK demodulator: rate and symbol rate must be positive");
    }
    const double sps = static_cast<double>(config.rate) / config.symbol_rate;
    if (sps < 2.0) {
        return fail("PSK demodulator: needs at least two samples per symbol");
    }
    if (!(config.roll_off > 0.0 && config.roll_off <= 1.0)) {
        return fail("PSK demodulator: roll-off must be in (0, 1]");
    }
    if (!(config.carrier_bandwidth > 0.0 && config.carrier_bandwidth < 0.5) ||
        !(config.timing_bandwidth > 0.0 && config.timing_bandwidth < 0.5)) {
        return fail("PSK demodulator: loop bandwidths must be in (0, 0.5)");
    }
    const unsigned order = psk_order(config.modulation);
    for (unsigned k = 0; k < order; ++k) {
        if (config.bit_map[k] >= order) {
            return fail("PSK demodulator: bit map label out of range");
        }
    }

    PskDemod demod;
    demod.config_ = config;
    demod.order_ = order;
    demod.bits_ = psk_bits_per_symbol(config.modulation);
    demod.sps_ = sps;

    auto taps_count = static_cast<std::size_t>(
        std::ceil(sps * static_cast<double>(std::max<std::size_t>(config.filter_span_symbols, 2))));
    taps_count |= 1U;
    auto taps = design_rrc(config.rate, config.symbol_rate, config.roll_off, taps_count);
    if (!taps) {
        return std::unexpected(with_context(taps.error(), "designing the PSK matched filter"));
    }
    demod.taps_ = std::move(*taps);
    auto filter = ComplexFir::create(demod.taps_);
    if (!filter) {
        return std::unexpected(with_context(filter.error(), "creating the PSK matched filter"));
    }
    demod.filter_ = std::move(*filter);
    demod.timing_ = dsp::GardnerTiming(sps, config.timing_bandwidth);

    const double theta = config.carrier_bandwidth / (kDamping + 0.25 / kDamping);
    const double d = 1.0 + 2.0 * kDamping * theta + theta * theta;
    demod.kp_ = 4.0 * kDamping * theta / d;
    demod.ki_ = 4.0 * theta * theta / d;

    demod.acquisition_samples_ =
        static_cast<std::size_t>(std::ceil(sps * static_cast<double>(config.acquisition_symbols))) +
        demod.taps_.size();
    demod.reset();
    return demod;
}

void PskDemod::reset() {
    filter_.reset();
    timing_.reset();
    pending_.clear();
    acquired_ = config_.acquisition == PskAcquisition::None;
    coarse_hz_ = 0.0;
    coarse_phase_ = 0.0;
    phase_ = 0.0;
    frequency_ = 0.0;
    delay_line_.clear();
    samples_in_ = 0;
    power_ = 0.0;
    lock_metric_ = 0.0;
    locked_ = false;
    out_.clear();
}

double PskDemod::acquisition_range_hz() const {
    // The M-th power also carries the symbol clock, as lines at M*f + n*Rs,
    // and at M = 8 the n = +/-1 lines were measured to beat the carrier line
    // over 512 symbols of random data at 14 dB Es/N0. Taking one of them puts
    // the carrier Rs/M out, which no decision-directed loop can see, because
    // that offset turns every symbol by exactly one constellation step. Only
    // within +/- Rs/(2M) is the carrier line the only one in range.
    const double unambiguous = config_.symbol_rate / (2.0 * static_cast<double>(order_));
    return config_.max_offset_hz > 0.0 ? std::min(config_.max_offset_hz, unambiguous)
                                       : unambiguous;
}

double PskDemod::frequency_offset_hz() const {
    return coarse_hz_ + frequency_ * static_cast<double>(config_.rate) / kTwoPi;
}

void PskDemod::estimate_offset() {
    // Matched filter a copy, so the line stands on in-band noise only, and
    // drop the filter's ramp up from silence.
    auto filter = ComplexFir::create(taps_);
    std::vector<Complex32> filtered(pending_.size());
    if (filter) {
        (void)filter->process(pending_, filtered);
    } else {
        filtered = pending_;
    }
    const std::size_t skip = std::min(taps_.size(), filtered.size());
    const std::size_t n = filtered.size() - skip;
    if (n < 16) {
        return;
    }
    std::size_t size = 1;
    while (size < 4 * n && size < (std::size_t{1} << 20U)) {
        size <<= 1U;
    }
    std::vector<std::complex<double>> x(size);
    for (std::size_t i = 0; i < n && i < size; ++i) {
        // Unit magnitude first, the Viterbi and Viterbi nonlinearity with
        // the magnitude raised to the zeroth power: the raw M-th power
        // amplifies noise and envelope ripple by the M-th power too, and at
        // M = 8 that buried the line in measurement.
        std::complex<double> s(filtered[skip + i]);
        const double mag = std::abs(s);
        s = mag > 0.0 ? s / mag : std::complex<double>(0.0, 0.0);
        std::complex<double> p(1.0, 0.0);
        for (unsigned k = 0; k < order_; ++k) {
            p *= s;
        }
        x[i] = p;
    }
    fft(x);

    const double rate = static_cast<double>(config_.rate);
    const double max_hz = std::min(acquisition_range_hz(), rate / (2.0 * static_cast<double>(order_)));
    const double bin_hz = rate / static_cast<double>(size);
    const auto reach = static_cast<long long>(
        std::floor(max_hz * static_cast<double>(order_) / bin_hz));
    const auto wrap = [&](long long k) {
        const auto s = static_cast<long long>(size);
        return static_cast<std::size_t>(((k % s) + s) % s);
    };
    long long best = 0;
    for (long long k = -reach; k <= reach; ++k) {
        if (std::abs(x[wrap(k)]) > std::abs(x[wrap(best)])) {
            best = k;
        }
    }
    // Parabolic interpolation on the magnitude between neighbouring bins.
    const double a = std::abs(x[wrap(best - 1)]);
    const double b = std::abs(x[wrap(best)]);
    const double c = std::abs(x[wrap(best + 1)]);
    const double denom = a - 2.0 * b + c;
    double delta = denom != 0.0 ? 0.5 * (a - c) / denom : 0.0;
    delta = std::clamp(delta, -0.5, 0.5);
    coarse_hz_ = (static_cast<double>(best) + delta) * bin_hz / static_cast<double>(order_);
}

void PskDemod::decide(const dsp::GardnerTiming::Strobe& strobe) {
    const Complex32 raw = strobe.symbol;
    const double p = std::norm(raw);
    power_ = (power_ == 0.0) ? p : power_ + kPowerAlpha * (p - power_);
    const float scale = power_ > 0.0 ? static_cast<float>(1.0 / std::sqrt(power_)) : 1.0F;
    const Complex32 z = raw * scale;

    // Nearest point by angle.
    const double offset = point_offset(config_.modulation);
    const double step = kTwoPi / static_cast<double>(order_);
    const double angle = std::arg(std::complex<double>(z)) - offset;
    const auto index = static_cast<unsigned>(
        ((static_cast<long long>(std::lround(angle / step)) % order_) + order_) % order_);
    const Complex32 decided = psk_point(config_.modulation, index);

    const double error = std::arg(std::complex<double>(z * std::conj(decided)));
    phase_ += kp_ * error;
    frequency_ += ki_ * error / sps_;

    const double coherence = std::cos(static_cast<double>(order_) * error);
    lock_metric_ += kLockAlpha * (coherence - lock_metric_);
    locked_ = lock_metric_ > config_.lock_threshold;

    PskSymbol symbol;
    symbol.symbol = z;
    symbol.index = static_cast<std::uint8_t>(index);
    std::array<float, 3> d0{1e30F, 1e30F, 1e30F};
    std::array<float, 3> d1{1e30F, 1e30F, 1e30F};
    for (unsigned k = 0; k < order_; ++k) {
        const float dist = std::norm(z - psk_point(config_.modulation, k));
        const unsigned label = config_.bit_map[k];
        for (unsigned b = 0; b < bits_; ++b) {
            const bool one = ((label >> (bits_ - 1U - b)) & 1U) != 0;
            auto& slot = one ? d1[b] : d0[b];
            slot = std::min(slot, dist);
        }
    }
    for (unsigned b = 0; b < bits_; ++b) {
        symbol.soft[b] = d0[b] - d1[b];
    }
    double position = strobe.position - 0.5 * static_cast<double>(taps_.size() - 1);
    if (config_.modulation == PskModulation::Oqpsk) {
        position -= 0.5 * sps_;
    }
    symbol.position = position;
    out_.push_back(symbol);
}

void PskDemod::run(dsp::ConstComplexSpan in) {
    if (in.empty()) {
        return;
    }
    scratch_.resize(in.size());
    const double rate = static_cast<double>(config_.rate);
    const double coarse_step = coarse_hz_ / rate;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const double a = -kTwoPi * coarse_phase_;
        scratch_[i] = in[i] * Complex32(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
        coarse_phase_ += coarse_step;
        coarse_phase_ -= std::floor(coarse_phase_);
    }
    (void)filter_.process(scratch_, scratch_);

    const bool offset_rail = config_.modulation == PskModulation::Oqpsk;
    const double half = 0.5 * sps_;
    const std::size_t line = offset_rail ? static_cast<std::size_t>(std::ceil(half)) + 3 : 0;
    for (const Complex32 sample : scratch_) {
        phase_ += frequency_;
        if (phase_ > std::numbers::pi || phase_ < -std::numbers::pi) {
            phase_ -= kTwoPi * std::floor((phase_ + std::numbers::pi) / kTwoPi);
        }
        Complex32 y = sample * Complex32(static_cast<float>(std::cos(-phase_)),
                                         static_cast<float>(std::sin(-phase_)));
        if (offset_rail) {
            // Delay the in-phase rail by half a symbol, cubic Lagrange for the
            // fractional part.
            delay_line_.push_back(y.real());
            if (delay_line_.size() > line) {
                delay_line_.erase(delay_line_.begin());
            }
            const double back = std::floor(half);
            const auto f = static_cast<float>(half - back);
            const auto newest = static_cast<long long>(delay_line_.size()) - 1;
            const auto pick = [&](long long age) {
                const long long idx = newest - age;
                return idx >= 0 ? delay_line_[static_cast<std::size_t>(idx)] : 0.0F;
            };
            // Value at age `back + f`, from ages back-1 .. back+2.
            const auto b = static_cast<long long>(back);
            const float cm = -f * (f - 1.0F) * (f - 2.0F) / 6.0F;
            const float c0 = (f + 1.0F) * (f - 1.0F) * (f - 2.0F) / 2.0F;
            const float c1 = -(f + 1.0F) * f * (f - 2.0F) / 2.0F;
            const float c2 = (f + 1.0F) * f * (f - 1.0F) / 6.0F;
            const float delayed =
                cm * pick(b - 1) + c0 * pick(b) + c1 * pick(b + 1) + c2 * pick(b + 2);
            y = Complex32(delayed, y.imag());
        }
        ++samples_in_;
        for (const auto& strobe : timing_.push(std::span<const Complex32>(&y, 1))) {
            decide(strobe);
        }
    }
}

std::span<const PskSymbol> PskDemod::process(dsp::ConstComplexSpan in) {
    out_.clear();
    if (order_ == 0) {
        return out_;
    }
    if (!acquired_) {
        const std::size_t take = std::min(in.size(), acquisition_samples_ - pending_.size());
        pending_.insert(pending_.end(), in.begin(), in.begin() + static_cast<std::ptrdiff_t>(take));
        in = in.subspan(take);
        if (pending_.size() < acquisition_samples_) {
            return out_;
        }
        estimate_offset();
        acquired_ = true;
        std::vector<Complex32> held;
        held.swap(pending_);
        run(held);
    }
    run(in);
    return out_;
}

}  // namespace revenant::decode
