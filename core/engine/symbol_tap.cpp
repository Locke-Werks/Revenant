#include "core/engine/symbol_tap.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

#include "core/characterise/cyclostationary.h"

namespace revenant::engine {

namespace {

using dsp::Complex32;

constexpr double kPi = std::numbers::pi;

// A third of a second at 48 kS/s, the most a raw receiver's estimate reads,
// and the samples it waits after a refusal before trying again.
constexpr std::size_t kEstimateSamples = 16'384;
constexpr std::size_t kEstimateRetry = 65'536;

// Matched filter span either side of the centre, in symbols.
constexpr double kRrcSpanSymbols = 4.0;

// The discriminator's DC follower, per sample. A carrier offset is the slow
// part of the discriminator, so it is followed and taken off before the
// levels are read; at 48 kS/s this settles in about a second.
constexpr double kDcFollow = 1.0e-4;

const std::vector<float> kFourLevels{-3.0F, -1.0F, 1.0F, 3.0F};
const std::vector<float> kTwoLevels{-1.0F, 1.0F};
const std::vector<float> kQuadrature{
    0.70710678F, 0.70710678F, -0.70710678F, 0.70710678F,
    -0.70710678F, -0.70710678F, 0.70710678F, -0.70710678F,
};

// Root raised cosine at t symbols from the centre.
double rrc(double t, double beta) {
    if (std::abs(t) < 1.0e-9) {
        return 1.0 - beta + 4.0 * beta / kPi;
    }
    if (beta > 0.0 && std::abs(std::abs(t) - 1.0 / (4.0 * beta)) < 1.0e-9) {
        const double a = kPi / (4.0 * beta);
        return beta / std::numbers::sqrt2 *
               ((1.0 + 2.0 / kPi) * std::sin(a) + (1.0 - 2.0 / kPi) * std::cos(a));
    }
    const double num = std::sin(kPi * t * (1.0 - beta)) + 4.0 * beta * t * std::cos(kPi * t * (1.0 + beta));
    const double den = kPi * t * (1.0 - (4.0 * beta * t) * (4.0 * beta * t));
    return num / den;
}

// The quadrature point nearest z, unit magnitude.
Complex32 nearest_quadrature(Complex32 z) {
    constexpr float h = 0.70710678F;
    return {z.real() >= 0.0F ? h : -h, z.imag() >= 0.0F ? h : -h};
}

}  // namespace

void SymbolTap::reset() {
    configured_ = false;
    plan_ = Plan{};
    estimate_buffer_.clear();
    estimate_wait_ = 0;
    timing_.reset();
    taps_.clear();
    filter_history_.clear();
    filtered_.clear();
    boxcar_ = 1;
    boxcar_history_.clear();
    last_sample_ = {};
    have_last_sample_ = false;
    dc_hz_ = 0.0;
    eye_power_ = 0.0;
    symbols_.clear();
    symbols_head_ = 0;
    symbols_count_ = 0;
    eye_.clear();
    eye_head_ = 0;
    eye_count_ = 0;
    previous_strobe_.reset();
    previous_symbol_ = {};
    have_previous_symbol_ = false;
    scope_ = SymbolScope{};
}

void SymbolTap::configure(const Plan& plan, dsp::SampleRate rate) {
    plan_ = plan;
    configured_ = true;
    if (plan.kind == SymbolKind::None || plan.symbol_rate_hz <= 0.0 || rate <= 0) {
        plan_.kind = SymbolKind::None;
        return;
    }
    const double sps = static_cast<double>(rate) / plan.symbol_rate_hz;
    if (sps < 2.0) {
        plan_.kind = SymbolKind::None;
        return;
    }
    timing_.emplace(sps, 0.01);
    symbols_.assign(kSymbolTapMaxSymbols, Complex32{});
    eye_.assign(kSymbolEyeTraces * kSymbolEyeTraceLength, 0.0F);

    if (plan.kind == SymbolKind::Constellation) {
        const auto half = static_cast<std::size_t>(std::ceil(kRrcSpanSymbols * sps));
        taps_.resize(2 * half + 1);
        double sum = 0.0;
        for (std::size_t i = 0; i < taps_.size(); ++i) {
            const double t = (static_cast<double>(i) - static_cast<double>(half)) / sps;
            taps_[i] = static_cast<float>(rrc(t, plan.rolloff));
            sum += taps_[i];
        }
        for (float& tap : taps_) {
            tap = static_cast<float>(tap / sum);
        }
        filter_history_.assign(taps_.size() - 1, Complex32{});
    } else {
        // Half a symbol of averaging takes the discriminator's click noise
        // down without closing the eye.
        boxcar_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(sps / 2.0)));
        boxcar_history_.clear();
    }
}

void SymbolTap::estimate_raw(dsp::SampleRate rate) {
    std::vector<characterise::Complex64> wide(estimate_buffer_.begin(), estimate_buffer_.end());
    Plan plan;

    // PSK first: an FSK signal has a constant envelope, so the squared
    // envelope detector refuses it on its ripple floor rather than guessing,
    // while a PSK signal also shows phase transitions and would satisfy the
    // second detector.
    if (const auto psk = characterise::estimate_symbol_rate(
            wide, rate, characterise::CyclicDetector::SquaredEnvelope);
        psk && psk->found) {
        plan.kind = SymbolKind::Constellation;
        plan.symbol_rate_hz = psk->symbol_rate_hz;
        plan.ideal = kQuadrature;
        plan.rolloff = 0.35;
    } else if (const auto fsk = characterise::estimate_symbol_rate(
                   wide, rate, characterise::CyclicDetector::FrequencyTransition);
               fsk && fsk->found) {
        plan.kind = SymbolKind::Levels;
        plan.symbol_rate_hz = fsk->symbol_rate_hz;
        plan.ideal = kTwoLevels;
        // The mean deviation is the shift's half width for two tones, which
        // is what puts the levels at plus and minus one.
        double mean = 0.0;
        std::vector<double> disc;
        disc.reserve(wide.size());
        for (std::size_t i = 1; i < wide.size(); ++i) {
            disc.push_back(std::arg(wide[i] * std::conj(wide[i - 1])) * static_cast<double>(rate) /
                           (2.0 * kPi));
            mean += disc.back();
        }
        mean /= static_cast<double>(std::max<std::size_t>(disc.size(), 1));
        double spread = 0.0;
        for (const double d : disc) {
            spread += std::abs(d - mean);
        }
        plan.unit_hz = spread / static_cast<double>(std::max<std::size_t>(disc.size(), 1));
        dc_hz_ = mean;
        if (plan.unit_hz <= 0.0) {
            plan.kind = SymbolKind::None;
        }
    }

    estimate_buffer_.clear();
    if (plan.kind == SymbolKind::None) {
        estimate_wait_ = kEstimateRetry;
        return;
    }
    configure(plan, rate);
}

void SymbolTap::push(Demod mode, std::span<const float> iq, dsp::SampleRate rate) {
    if (mode != mode_ || rate != rate_) {
        reset();
        mode_ = mode;
        rate_ = rate;
    }
    if (rate <= 0 || iq.size() < 2) {
        return;
    }
    const std::size_t count = iq.size() / 2;
    std::vector<Complex32> samples(count);
    for (std::size_t i = 0; i < count; ++i) {
        samples[i] = Complex32(iq[2 * i], iq[2 * i + 1]);
    }

    if (!configured_) {
        switch (mode) {
            case Demod::P25p1: configure({SymbolKind::Levels, 4'800.0, 600.0, kFourLevels, 0.0}, rate); break;
            case Demod::Dmr: configure({SymbolKind::Levels, 4'800.0, 648.0, kFourLevels, 0.0}, rate); break;
            // GMSK at h = 0.5 is 1200 Hz either way at 4800 bit/s.
            case Demod::Dstar: configure({SymbolKind::Levels, 4'800.0, 1'200.0, kTwoLevels, 0.0}, rate); break;
            case Demod::Tetra: configure({SymbolKind::Constellation, 18'000.0, 0.0, kQuadrature, 0.35}, rate); break;
            case Demod::Raw: {
                if (estimate_wait_ > 0) {
                    estimate_wait_ -= std::min(estimate_wait_, count);
                    return;
                }
                const std::size_t room = kEstimateSamples - estimate_buffer_.size();
                const std::size_t take = std::min(room, count);
                estimate_buffer_.insert(estimate_buffer_.end(), samples.begin(),
                                        samples.begin() + static_cast<std::ptrdiff_t>(take));
                if (estimate_buffer_.size() < kEstimateSamples) {
                    return;
                }
                estimate_raw(rate);
                if (!configured_) {
                    return;
                }
                samples.erase(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(take));
                break;
            }
            default: configure({}, rate); break;
        }
    }
    if (plan_.kind == SymbolKind::None || samples.empty()) {
        return;
    }
    run(samples);
}

void SymbolTap::run(std::span<const Complex32> samples) {
    filtered_.resize(samples.size());
    const double rate = static_cast<double>(rate_);

    if (plan_.kind == SymbolKind::Levels) {
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const Complex32 x = samples[i];
            double hz = 0.0;
            if (have_last_sample_) {
                hz = static_cast<double>(std::arg(x * std::conj(last_sample_))) * rate / (2.0 * kPi);
            }
            last_sample_ = x;
            have_last_sample_ = true;
            dc_hz_ += kDcFollow * (hz - dc_hz_);
            boxcar_history_.push_back(static_cast<float>((hz - dc_hz_) / plan_.unit_hz));
            if (boxcar_history_.size() > boxcar_) {
                boxcar_history_.erase(boxcar_history_.begin());
            }
            float sum = 0.0F;
            for (const float v : boxcar_history_) {
                sum += v;
            }
            filtered_[i] = Complex32(sum / static_cast<float>(boxcar_history_.size()), 0.0F);
        }
    } else {
        const std::size_t keep = taps_.size() - 1;
        filter_history_.insert(filter_history_.end(), samples.begin(), samples.end());
        for (std::size_t i = 0; i < samples.size(); ++i) {
            Complex32 acc{};
            for (std::size_t k = 0; k < taps_.size(); ++k) {
                acc += taps_[k] * filter_history_[i + k];
            }
            filtered_[i] = acc;
        }
        filter_history_.erase(filter_history_.begin(),
                              filter_history_.end() - static_cast<std::ptrdiff_t>(keep));
    }

    for (const auto& strobe : timing_->push(filtered_)) {
        Complex32 value = strobe.symbol;
        bool keep_symbol = true;
        if (plan_.kind == SymbolKind::Constellation) {
            // The differential point: carrier phase cancels, and a carrier
            // offset is left as one constant rotation that scope() takes off.
            const float previous_magnitude = std::abs(previous_symbol_);
            keep_symbol = have_previous_symbol_ && previous_magnitude > 0.0F;
            if (keep_symbol) {
                value = strobe.symbol * std::conj(previous_symbol_) / previous_magnitude;
            }
            previous_symbol_ = strobe.symbol;
            have_previous_symbol_ = true;
            const double p = std::norm(strobe.symbol);
            eye_power_ = (eye_power_ == 0.0) ? p : eye_power_ + 0.02 * (p - eye_power_);
        }
        if (keep_symbol) {
            symbols_[symbols_head_] = value;
            symbols_head_ = (symbols_head_ + 1) % kSymbolTapMaxSymbols;
            symbols_count_ = std::min(symbols_count_ + 1, kSymbolTapMaxSymbols);
        }

        // The trace for the previous strobe, which now has a symbol either
        // side of it in the loop's history.
        if (previous_strobe_.has_value()) {
            const double period = timing_->period();
            const double centre = previous_strobe_->position;
            Complex32 rotate{1.0F, 0.0F};
            float scale = 1.0F;
            if (plan_.kind == SymbolKind::Constellation) {
                // Decision directed: turn each trace so its centre lands on
                // the quadrature point it is nearest, which is what makes an
                // unlocked carrier's eye readable at all.
                const Complex32 c = previous_strobe_->symbol;
                const float magnitude = std::abs(c);
                if (magnitude > 0.0F) {
                    rotate = std::conj(c / magnitude) * nearest_quadrature(c);
                }
                scale = eye_power_ > 0.0 ? static_cast<float>(1.0 / std::sqrt(eye_power_)) : 1.0F;
            }
            float* trace = eye_.data() + eye_head_ * kSymbolEyeTraceLength;
            for (std::size_t j = 0; j < kSymbolEyeTraceLength; ++j) {
                const double at = centre - period +
                                  period * static_cast<double>(j) / static_cast<double>(kSymbolEyePerSymbol);
                trace[j] = (timing_->at(at) * rotate).real() * scale;
            }
            eye_head_ = (eye_head_ + 1) % kSymbolEyeTraces;
            eye_count_ = std::min(eye_count_ + 1, kSymbolEyeTraces);
        }
        previous_strobe_ = strobe;
    }
}

const SymbolScope* SymbolTap::scope() {
    if (plan_.kind == SymbolKind::None || symbols_count_ == 0) {
        return nullptr;
    }
    scope_.kind = plan_.kind;
    scope_.symbol_rate_hz = plan_.symbol_rate_hz;
    scope_.ideal = plan_.ideal;
    scope_.eye_trace_length = static_cast<std::uint32_t>(kSymbolEyeTraceLength);
    scope_.timing_offset = static_cast<float>(timing_->phase());
    scope_.rate_error_ppm = static_cast<float>(timing_->rate_error() * 1.0e6);

    const std::size_t first = (symbols_head_ + kSymbolTapMaxSymbols - symbols_count_) % kSymbolTapMaxSymbols;
    const auto symbol = [&](std::size_t i) { return symbols_[(first + i) % kSymbolTapMaxSymbols]; };

    float outer = 0.0F;
    for (std::size_t i = 0; i < plan_.ideal.size(); ++i) {
        outer = std::max(outer, std::abs(plan_.ideal[i]));
    }

    double error = 0.0;
    scope_.points.clear();
    if (plan_.kind == SymbolKind::Levels) {
        scope_.points.reserve(symbols_count_);
        for (std::size_t i = 0; i < symbols_count_; ++i) {
            const float v = symbol(i).real();
            scope_.points.push_back(v);
            float best = 1.0e30F;
            for (const float level : plan_.ideal) {
                best = std::min(best, std::abs(v - level));
            }
            error += static_cast<double>(best) * best;
        }
    } else {
        // Unit RMS, and the one rotation that puts the fourth power on the
        // negative real axis, which is where the diagonal points' is.
        double power = 0.0;
        std::complex<double> fourth{};
        for (std::size_t i = 0; i < symbols_count_; ++i) {
            const std::complex<double> z(symbol(i).real(), symbol(i).imag());
            power += std::norm(z);
            fourth += (z * z) * (z * z);
        }
        const double norm = power > 0.0 ? 1.0 / std::sqrt(power / static_cast<double>(symbols_count_)) : 1.0;
        const double turn = (kPi - std::arg(fourth)) / 4.0;
        const auto spin = static_cast<Complex32>(std::polar(norm, turn));
        scope_.points.reserve(2 * symbols_count_);
        outer = 1.0F;
        for (std::size_t i = 0; i < symbols_count_; ++i) {
            const Complex32 p = symbol(i) * spin;
            scope_.points.push_back(p.real());
            scope_.points.push_back(p.imag());
            error += std::norm(p - nearest_quadrature(p));
        }
    }
    scope_.error_rms = static_cast<float>(std::sqrt(error / static_cast<double>(symbols_count_)) /
                                          std::max(outer, 1.0e-6F));

    scope_.eye.clear();
    scope_.eye.reserve(eye_count_ * kSymbolEyeTraceLength);
    const std::size_t eye_first = (eye_head_ + kSymbolEyeTraces - eye_count_) % kSymbolEyeTraces;
    for (std::size_t t = 0; t < eye_count_; ++t) {
        const float* trace = eye_.data() + ((eye_first + t) % kSymbolEyeTraces) * kSymbolEyeTraceLength;
        scope_.eye.insert(scope_.eye.end(), trace, trace + kSymbolEyeTraceLength);
    }
    return &scope_;
}

}  // namespace revenant::engine
