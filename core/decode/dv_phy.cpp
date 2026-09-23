#include "core/decode/dv_phy.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace revenant::decode {
namespace {

constexpr double kPi = std::numbers::pi;

// Below this the normalised correlation denominator is dominated by rounding
// rather than by signal, and reporting a score from it would be reporting
// noise as a lock. A run of symbols this quiet is a squelched channel.
constexpr double kEnergyFloor = 1e-20;

}  // namespace

// ---------------------------------------------------------------------------
// Frequency discrimination
// ---------------------------------------------------------------------------

Status fm_discriminate(ConstComplexSpan in, RealSpan out, SampleRate rate) {
    if (in.size() != out.size()) {
        return fail(std::format(
            "fm_discriminate needs equal spans; got {} input and {} output samples", in.size(),
            out.size()));
    }
    if (rate <= 0) {
        return fail(std::format("fm_discriminate needs a positive sample rate; got {}", rate));
    }

    const double scale = static_cast<double>(rate) / (2.0 * kPi);
    Complex32 previous{1.0F, 0.0F};
    for (std::size_t i = 0; i < in.size(); ++i) {
        const Complex32 product = in[i] * std::conj(previous);
        out[i] = static_cast<float>(
            std::atan2(static_cast<double>(product.imag()), static_cast<double>(product.real())) *
            scale);
        previous = in[i];
    }
    return {};
}

Expected<FmDiscriminator> FmDiscriminator::create(SampleRate rate) {
    if (rate <= 0) {
        return fail(std::format("FmDiscriminator needs a positive sample rate; got {}", rate));
    }
    FmDiscriminator discriminator;
    discriminator.scale_ = static_cast<double>(rate) / (2.0 * kPi);
    return discriminator;
}

Status FmDiscriminator::process(ConstComplexSpan in, RealSpan out) {
    if (in.size() != out.size()) {
        return fail(std::format(
            "FmDiscriminator needs equal spans; got {} input and {} output samples", in.size(),
            out.size()));
    }
    // The same expression as fm_discriminate, so a stream given in one call
    // comes out bit for bit as it would from there.
    for (std::size_t i = 0; i < in.size(); ++i) {
        const Complex32 product = in[i] * std::conj(previous_);
        out[i] = static_cast<float>(
            std::atan2(static_cast<double>(product.imag()), static_cast<double>(product.real())) *
            scale_);
        previous_ = in[i];
    }
    return {};
}

void FmDiscriminator::reset() { previous_ = Complex32{1.0F, 0.0F}; }

// ---------------------------------------------------------------------------
// Filter design
// ---------------------------------------------------------------------------

Expected<std::vector<float>> design_from_response(SampleRate rate, std::size_t taps,
                                                  ResponseFn response, const void* context) {
    if (rate <= 0) {
        return fail(std::format("design_from_response needs a positive rate; got {}", rate));
    }
    if (taps == 0 || taps % 2 == 0) {
        return fail(std::format(
            "design_from_response needs an odd tap count so the group delay is a whole "
            "number of samples; got {}",
            taps));
    }
    if (response == nullptr) {
        return fail("design_from_response was given no response function");
    }

    // Oversample the response well past the tap count. The inverse transform
    // below is a Riemann sum, and a grid only as dense as the filter aliases
    // the tails of the impulse response back onto itself.
    const std::size_t grid = std::max<std::size_t>(4096, taps * 16);
    const double rate_d = static_cast<double>(rate);
    const std::size_t half = (taps - 1) / 2;

    std::vector<float> out(taps, 0.0F);
    for (std::size_t n = 0; n <= half; ++n) {
        // h(n) = (2/fs) * integral over [0, fs/2] of H(f) cos(2*pi*f*n/fs) df,
        // which is the inverse transform of a real even response.
        double sum = 0.0;
        for (std::size_t k = 0; k <= grid; ++k) {
            const double hertz = 0.5 * rate_d * static_cast<double>(k) / static_cast<double>(grid);
            const double weight = (k == 0 || k == grid) ? 0.5 : 1.0;  // trapezoid
            sum += weight * response(hertz, context) *
                   std::cos(2.0 * kPi * hertz * static_cast<double>(n) / rate_d);
        }
        const double df = 0.5 * rate_d / static_cast<double>(grid);
        const double value = 2.0 * sum * df / rate_d;

        // Hamming. Applied about the centre tap, so the index into the window
        // is half +/- n rather than n.
        const auto window_at = [&](std::size_t index) {
            return 0.54 - 0.46 * std::cos(2.0 * kPi * static_cast<double>(index) /
                                          static_cast<double>(taps - 1));
        };
        out[half + n] = static_cast<float>(value * window_at(half + n));
        out[half - n] = static_cast<float>(value * window_at(half - n));
    }
    return out;
}

Expected<std::vector<float>> design_rrc(SampleRate rate, double symbol_rate, double alpha,
                                        std::size_t taps) {
    if (rate <= 0 || symbol_rate <= 0.0) {
        return fail(std::format("design_rrc needs a positive rate and symbol rate; got {} and {}",
                                rate, symbol_rate));
    }
    if (alpha <= 0.0 || alpha > 1.0) {
        return fail(std::format("design_rrc needs a roll-off in (0, 1]; got {}", alpha));
    }
    if (taps == 0 || taps % 2 == 0) {
        return fail(std::format("design_rrc needs an odd tap count; got {}", taps));
    }

    const double period = 1.0 / symbol_rate;
    const double step = 1.0 / static_cast<double>(rate);
    const auto half = static_cast<std::ptrdiff_t>((taps - 1) / 2);

    std::vector<float> out(taps, 0.0F);
    for (std::ptrdiff_t i = -half; i <= half; ++i) {
        const double t = static_cast<double>(i) * step;
        double value = 0.0;

        // The two removable singularities of the RRC, taken as limits.
        if (std::abs(t) < 1e-12) {
            value = (1.0 + alpha * (4.0 / kPi - 1.0)) / period;
        } else if (std::abs(std::abs(4.0 * alpha * t / period) - 1.0) < 1e-9) {
            const double a = (1.0 + 2.0 / kPi) * std::sin(kPi / (4.0 * alpha));
            const double b = (1.0 - 2.0 / kPi) * std::cos(kPi / (4.0 * alpha));
            value = alpha * (a + b) / (period * std::numbers::sqrt2);
        } else {
            const double x = t / period;
            const double numerator = std::sin(kPi * x * (1.0 - alpha)) +
                                     4.0 * alpha * x * std::cos(kPi * x * (1.0 + alpha));
            const double denominator = kPi * x * (1.0 - 16.0 * alpha * alpha * x * x);
            value = numerator / (denominator * period);
        }
        out[static_cast<std::size_t>(i + half)] = static_cast<float>(value);
    }

    // Unit energy. A matched filter's gain is a scaling the slicer would
    // otherwise have to know about, and normalising here keeps the decision
    // thresholds in every mode file in the units its standard states.
    double energy = 0.0;
    for (const float tap : out) {
        energy += static_cast<double>(tap) * static_cast<double>(tap);
    }
    if (energy <= 0.0) {
        return fail("design_rrc produced an all-zero filter");
    }
    const auto norm = static_cast<float>(1.0 / std::sqrt(energy));
    for (float& tap : out) {
        tap *= norm;
    }
    return out;
}

Expected<std::vector<float>> design_gaussian(SampleRate rate, double symbol_rate, double bt,
                                             std::size_t taps) {
    if (rate <= 0 || symbol_rate <= 0.0) {
        return fail(std::format(
            "design_gaussian needs a positive rate and symbol rate; got {} and {}", rate,
            symbol_rate));
    }
    if (bt <= 0.0) {
        return fail(std::format("design_gaussian needs a positive BT product; got {}", bt));
    }
    if (taps == 0 || taps % 2 == 0) {
        return fail(std::format("design_gaussian needs an odd tap count; got {}", taps));
    }

    const double period = 1.0 / symbol_rate;
    const double step = 1.0 / static_cast<double>(rate);
    const auto half = static_cast<std::ptrdiff_t>((taps - 1) / 2);

    // A Gaussian whose 3 dB bandwidth is B has impulse response
    // exp(-t^2 / (2*sigma^2)) with sigma = sqrt(ln 2) / (2*pi*B). The
    // convolution with the one-symbol rectangle is done by integrating the
    // Gaussian across the symbol, which is the difference of two error
    // functions.
    const double bandwidth = bt * symbol_rate;
    const double sigma = std::sqrt(std::log(2.0)) / (2.0 * kPi * bandwidth);

    std::vector<float> out(taps, 0.0F);
    double sum = 0.0;
    for (std::ptrdiff_t i = -half; i <= half; ++i) {
        const double t = static_cast<double>(i) * step;
        const double upper = (t + 0.5 * period) / (sigma * std::numbers::sqrt2);
        const double lower = (t - 0.5 * period) / (sigma * std::numbers::sqrt2);
        const double value = 0.5 * (std::erf(upper) - std::erf(lower));
        out[static_cast<std::size_t>(i + half)] = static_cast<float>(value);
        sum += value;
    }
    if (sum <= 0.0) {
        return fail("design_gaussian produced an all-zero filter");
    }

    // Unit area, not unit energy. This filter shapes a frequency pulse, and the
    // integral of that pulse is the phase the modulator accumulates over a
    // symbol. Normalising the area is what makes the modulation index come out
    // as the caller asked rather than scaled by the filter.
    const auto norm = static_cast<float>(1.0 / sum);
    for (float& tap : out) {
        tap *= norm;
    }
    return out;
}

Status filter_real(ConstRealSpan in, ConstRealSpan taps, RealSpan out) {
    if (in.size() != out.size()) {
        return fail(std::format("filter_real needs equal spans; got {} in and {} out", in.size(),
                                out.size()));
    }
    if (taps.empty()) {
        return fail("filter_real was given no taps");
    }
    for (std::size_t n = 0; n < in.size(); ++n) {
        double sum = 0.0;
        const std::size_t limit = std::min(taps.size(), n + 1);
        for (std::size_t k = 0; k < limit; ++k) {
            sum += static_cast<double>(taps[k]) * static_cast<double>(in[n - k]);
        }
        out[n] = static_cast<float>(sum);
    }
    return {};
}

Status filter_complex(ConstComplexSpan in, ConstRealSpan taps, ComplexSpan out) {
    if (in.size() != out.size()) {
        return fail(std::format("filter_complex needs equal spans; got {} in and {} out",
                                in.size(), out.size()));
    }
    if (taps.empty()) {
        return fail("filter_complex was given no taps");
    }
    for (std::size_t n = 0; n < in.size(); ++n) {
        double real = 0.0;
        double imag = 0.0;
        const std::size_t limit = std::min(taps.size(), n + 1);
        for (std::size_t k = 0; k < limit; ++k) {
            const double tap = static_cast<double>(taps[k]);
            real += tap * static_cast<double>(in[n - k].real());
            imag += tap * static_cast<double>(in[n - k].imag());
        }
        out[n] = Complex32{static_cast<float>(real), static_cast<float>(imag)};
    }
    return {};
}

Expected<RealFir> RealFir::create(std::vector<float> taps) {
    if (taps.empty()) {
        return fail("RealFir was given no taps");
    }
    RealFir filter;
    filter.history_.assign(taps.size() - 1, 0.0F);
    filter.taps_ = std::move(taps);
    return filter;
}

Status RealFir::process(ConstRealSpan in, RealSpan out) {
    if (in.size() != out.size()) {
        return fail(std::format("RealFir needs equal spans; got {} in and {} out", in.size(),
                                out.size()));
    }
    if (taps_.empty()) {
        return fail("RealFir was never created with taps");
    }
    const std::size_t memory = taps_.size() - 1;
    history_.insert(history_.end(), in.begin(), in.end());

    // history_[memory + n] is in[n], and history_[memory + n - k] is the
    // input k samples before it, whether that arrived in this call or an
    // earlier one. The loop order matches filter_real's so the sums round
    // alike.
    for (std::size_t n = 0; n < in.size(); ++n) {
        double sum = 0.0;
        for (std::size_t k = 0; k < taps_.size(); ++k) {
            sum += static_cast<double>(taps_[k]) * static_cast<double>(history_[memory + n - k]);
        }
        out[n] = static_cast<float>(sum);
    }

    history_.erase(history_.begin(),
                   history_.begin() + static_cast<std::ptrdiff_t>(history_.size() - memory));
    return {};
}

void RealFir::reset() { std::fill(history_.begin(), history_.end(), 0.0F); }

// ---------------------------------------------------------------------------
// Symbol timing recovery
// ---------------------------------------------------------------------------

namespace {

// Cubic Farrow interpolation at fractional position mu between h[1] and h[2].
// The four-point Lagrange form, which is what a Farrow structure evaluates and
// is cheaper to read written out.
Complex32 interpolate(const Complex32* h, double mu) {
    const double m = mu;
    const double c0 = -m * (m - 1.0) * (m - 2.0) / 6.0;
    const double c1 = (m + 1.0) * (m - 1.0) * (m - 2.0) / 2.0;
    const double c2 = -(m + 1.0) * m * (m - 2.0) / 2.0;
    const double c3 = (m + 1.0) * m * (m - 1.0) / 6.0;
    const double real = c0 * h[0].real() + c1 * h[1].real() + c2 * h[2].real() + c3 * h[3].real();
    const double imag = c0 * h[0].imag() + c1 * h[1].imag() + c2 * h[2].imag() + c3 * h[3].imag();
    return Complex32{static_cast<float>(real), static_cast<float>(imag)};
}

// Samples the interpolator needs either side of the position it lands on.
constexpr std::size_t kInterpolationMargin = 2;

// How much of each window's estimate is taken. The estimate has variance, and
// snapping the phase all the way onto every window's own answer puts that
// variance straight into the symbol instants: it showed up as a symbol error
// rate of about one in a thousand that did not improve with signal to noise,
// because it was not noise.
//
// A quarter averages four windows deep, which is a few hundred symbols, and
// still follows a crystal offset far faster than a crystal moves.
constexpr double kPhaseSmoothing = 0.25;

}  // namespace

Expected<SymbolSync> SymbolSync::create(const SymbolSyncConfig& config) {
    if (config.rate <= 0 || config.symbol_rate <= 0.0) {
        return fail(std::format(
            "SymbolSync needs a positive rate and symbol rate; got {} and {}", config.rate,
            config.symbol_rate));
    }
    const double sps = static_cast<double>(config.rate) / config.symbol_rate;
    if (sps < 2.0) {
        return fail(std::format(
            "SymbolSync needs at least two samples per symbol for the square-law line to "
            "sit below Nyquist; {} Hz over {} symbols per second gives {:.3f}",
            config.rate, config.symbol_rate, sps));
    }
    if (config.window_symbols < 4) {
        return fail(std::format(
            "SymbolSync needs a window of at least four symbols to estimate from; got {}",
            config.window_symbols));
    }

    SymbolSync sync;
    sync.samples_per_symbol_ = sps;
    sync.window_symbols_ = config.window_symbols;
    sync.window_samples_ = static_cast<std::size_t>(
        std::llround(static_cast<double>(config.window_symbols) * sps));
    if (sync.window_samples_ == 0) {
        return fail("SymbolSync resolved a zero-length window");
    }
    return sync;
}

void SymbolSync::reset() {
    buffer_.clear();
    buffer_start_ = 0;
    next_window_ = 0;
    next_symbol_ = 0.0;
    have_phase_ = false;
}

void SymbolSync::process(ConstComplexSpan in, std::vector<RecoveredSymbol>& out) {
    if (window_samples_ == 0) {
        return;
    }
    buffer_.insert(buffer_.end(), in.begin(), in.end());

    while (true) {
        const std::uint64_t window_begin = next_window_;
        const std::uint64_t window_end = window_begin + window_samples_;

        // The last symbol of a window can sit right at its end, and the
        // interpolator reaches two samples past whatever it lands on.
        const std::uint64_t buffer_end = buffer_start_ + buffer_.size();
        if (window_end + kInterpolationMargin + 1 > buffer_end) {
            break;
        }
        if (window_begin < buffer_start_) {
            // The caller reset mid-stream, or the trim below was wrong. Either
            // way there is nothing to recover from, so start again here.
            next_window_ = buffer_start_;
            have_phase_ = false;
            continue;
        }

        const std::size_t base = static_cast<std::size_t>(window_begin - buffer_start_);

        // The square-law estimator. The squared magnitude of a linearly
        // modulated signal has a spectral line at the symbol rate, and the
        // phase of that line is the timing offset:
        //
        //   tau = -(T / 2pi) * arg( sum over n of |y[n]|^2 * exp(-j*2*pi*n/T) )
        //
        // with T the symbol period in samples. Everything here is in samples,
        // so T is samples_per_symbol_ and tau comes out in samples.
        double line_real = 0.0;
        double line_imag = 0.0;
        for (std::size_t n = 0; n < window_samples_; ++n) {
            const Complex32 sample = buffer_[base + n];
            const double energy = static_cast<double>(sample.real()) * sample.real() +
                                  static_cast<double>(sample.imag()) * sample.imag();
            const double angle = -2.0 * kPi * static_cast<double>(n) / samples_per_symbol_;
            line_real += energy * std::cos(angle);
            line_imag += energy * std::sin(angle);
        }

        double offset = 0.0;
        if (line_real * line_real + line_imag * line_imag > kEnergyFloor) {
            offset = -std::atan2(line_imag, line_real) / (2.0 * kPi) * samples_per_symbol_;
        }
        // atan2 returns a principal value, so the offset is only defined
        // modulo a symbol. Bring it into [0, T).
        offset = std::fmod(offset, samples_per_symbol_);
        if (offset < 0.0) {
            offset += samples_per_symbol_;
        }

        const double estimated = static_cast<double>(window_begin) + offset;
        if (!have_phase_) {
            next_symbol_ = estimated;
            have_phase_ = true;
        } else {
            // Move the running symbol position towards this window's
            // estimate, choosing the representative nearest to where the last
            // window left off. Rounding to a whole number of symbols is what
            // keeps the symbol count continuous across windows: a half-symbol
            // correction would otherwise drop or repeat one.
            const double steps =
                std::round((next_symbol_ - estimated) / samples_per_symbol_);
            const double target = estimated + steps * samples_per_symbol_;
            next_symbol_ += kPhaseSmoothing * (target - next_symbol_);
        }

        while (next_symbol_ < static_cast<double>(window_end)) {
            const double position = next_symbol_;
            const auto whole = static_cast<std::int64_t>(std::floor(position));
            const double fraction = position - static_cast<double>(whole);

            const std::int64_t first = whole - 1;
            if (first < static_cast<std::int64_t>(buffer_start_) ||
                static_cast<std::uint64_t>(first) + 4 > buffer_end) {
                // Outside what is buffered. Skip this symbol rather than read
                // past the end; it can only happen at the very start of a
                // stream, where there is nothing behind the first sample.
                next_symbol_ += samples_per_symbol_;
                continue;
            }

            const std::size_t index =
                static_cast<std::size_t>(static_cast<std::uint64_t>(first) - buffer_start_);
            out.push_back(RecoveredSymbol{interpolate(&buffer_[index], fraction), position});
            next_symbol_ += samples_per_symbol_;
        }

        next_window_ = window_end;
    }

    // Trim everything the next window and its interpolation margin cannot
    // reach back to.
    const std::uint64_t keep_from =
        (next_window_ > kInterpolationMargin + 1) ? next_window_ - kInterpolationMargin - 1 : 0;
    if (keep_from > buffer_start_) {
        const std::size_t drop = static_cast<std::size_t>(keep_from - buffer_start_);
        if (drop < buffer_.size()) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(drop));
            buffer_start_ = keep_from;
        }
    }
}

// ---------------------------------------------------------------------------
// Sync pattern correlation
// ---------------------------------------------------------------------------

double correlation_at(ConstRealSpan symbols, ConstRealSpan pattern, std::size_t offset) {
    if (pattern.empty() || offset + pattern.size() > symbols.size()) {
        return 0.0;
    }
    double dot = 0.0;
    double signal_energy = 0.0;
    double pattern_energy = 0.0;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const double s = static_cast<double>(symbols[offset + i]);
        const double p = static_cast<double>(pattern[i]);
        dot += s * p;
        signal_energy += s * s;
        pattern_energy += p * p;
    }
    const double denominator = std::sqrt(signal_energy * pattern_energy);
    if (denominator < kEnergyFloor) {
        return 0.0;
    }
    return dot / denominator;
}

double centred_correlation_at(ConstRealSpan symbols, ConstRealSpan pattern, std::size_t offset) {
    if (pattern.size() < 2 || offset + pattern.size() > symbols.size()) {
        return 0.0;
    }
    const auto count = static_cast<double>(pattern.size());
    double symbol_mean = 0.0;
    double pattern_mean = 0.0;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        symbol_mean += static_cast<double>(symbols[offset + i]);
        pattern_mean += static_cast<double>(pattern[i]);
    }
    symbol_mean /= count;
    pattern_mean /= count;

    double dot = 0.0;
    double signal_energy = 0.0;
    double pattern_energy = 0.0;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const double s = static_cast<double>(symbols[offset + i]) - symbol_mean;
        const double p = static_cast<double>(pattern[i]) - pattern_mean;
        dot += s * p;
        signal_energy += s * s;
        pattern_energy += p * p;
    }
    const double denominator = std::sqrt(signal_energy * pattern_energy);
    if (denominator < kEnergyFloor) {
        return 0.0;
    }
    return dot / denominator;
}

Expected<LevelFit> fit_levels(ConstRealSpan symbols, ConstRealSpan pattern, std::size_t offset) {
    if (pattern.empty() || offset + pattern.size() > symbols.size()) {
        return fail(std::format(
            "fit_levels needs {} symbols from offset {}; the run holds {}", pattern.size(), offset,
            symbols.size()));
    }
    const auto count = static_cast<double>(pattern.size());
    double symbol_mean = 0.0;
    double pattern_mean = 0.0;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        symbol_mean += static_cast<double>(symbols[offset + i]);
        pattern_mean += static_cast<double>(pattern[i]);
    }
    symbol_mean /= count;
    pattern_mean /= count;

    double covariance = 0.0;
    double variance = 0.0;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const double p = static_cast<double>(pattern[i]) - pattern_mean;
        covariance += p * (static_cast<double>(symbols[offset + i]) - symbol_mean);
        variance += p * p;
    }
    if (variance < kEnergyFloor) {
        return fail("fit_levels was given a pattern with no variation to fit a gain against");
    }
    LevelFit fit;
    fit.gain = covariance / variance;
    fit.level = symbol_mean - fit.gain * pattern_mean;
    return fit;
}

Expected<SyncHit> correlate_pattern(ConstRealSpan symbols, ConstRealSpan pattern) {
    if (pattern.empty()) {
        return fail("correlate_pattern was given an empty pattern");
    }
    if (symbols.size() < pattern.size()) {
        return fail(std::format(
            "correlate_pattern needs at least as many symbols as pattern; got {} against {}",
            symbols.size(), pattern.size()));
    }

    SyncHit best;
    double best_magnitude = -1.0;
    const std::size_t last = symbols.size() - pattern.size();
    for (std::size_t offset = 0; offset <= last; ++offset) {
        const double score = correlation_at(symbols, pattern, offset);
        const double magnitude = std::abs(score);
        if (magnitude > best_magnitude) {
            best_magnitude = magnitude;
            best.offset = offset;
            best.score = score;
            best.inverted = score < 0.0;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Bit and symbol packing
// ---------------------------------------------------------------------------

std::uint64_t pack_bits_msb(std::span<const std::uint8_t> bits, std::size_t first,
                            std::size_t count) {
    if (count == 0 || count > 64 || first + count > bits.size()) {
        return 0;
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        value = (value << 1) | static_cast<std::uint64_t>(bits[first + i] & 1U);
    }
    return value;
}

double bit_error_rate(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
    if (a.size() != b.size()) {
        return -1.0;
    }
    if (a.empty()) {
        return 0.0;
    }
    std::size_t errors = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if ((a[i] & 1U) != (b[i] & 1U)) {
            ++errors;
        }
    }
    return static_cast<double>(errors) / static_cast<double>(a.size());
}

}  // namespace revenant::decode
