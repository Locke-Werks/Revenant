#include "core/decode/mfsk_demod.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <string>

namespace revenant::decode {

namespace {

// Correlation of a real window against e^{-j 2 pi f n / rate}, by a rotating
// phasor rather than a table because the AFC moves f every symbol.
std::complex<double> correlate(const float* x, std::size_t count, double hz, double rate) {
    const double w = -2.0 * std::numbers::pi * hz / rate;
    const std::complex<double> step(std::cos(w), std::sin(w));
    std::complex<double> phasor(1.0, 0.0);
    std::complex<double> acc(0.0, 0.0);
    for (std::size_t n = 0; n < count; ++n) {
        acc += static_cast<double>(x[n]) * phasor;
        phasor *= step;
        // Renormalise now and then; the recurrence drifts in magnitude over
        // the tens of thousands of steps a slow mode's window takes.
        if ((n & 1023U) == 1023U) {
            phasor /= std::abs(phasor);
        }
    }
    return acc;
}

// Energies at count frequencies first_hz + k * spacing_hz over one window,
// scaled so a sinusoid of amplitude A on a bin reads A^2.
void bank(const float* x, std::size_t length, double first_hz, double spacing_hz, int count,
          double rate, double* out) {
    const double scale = 4.0 / (static_cast<double>(length) * static_cast<double>(length));
    for (int k = 0; k < count; ++k) {
        out[k] = std::norm(correlate(x, length, first_hz + spacing_hz * k, rate)) * scale;
    }
}

// Vertex offset of a parabola through three equally spaced points, in units
// of the spacing, clamped to half a step either way.
double parabolic(double left, double centre, double right) {
    const double denominator = left - 2.0 * centre + right;
    if (denominator >= 0.0) {
        return 0.0;
    }
    return std::clamp(0.5 * (left - right) / denominator, -0.5, 0.5);
}

int wrap(int value, int modulus) {
    const int r = value % modulus;
    return r < 0 ? r + modulus : r;
}

}  // namespace

// ---------------------------------------------------------------------------
// MfskDemodulator
// ---------------------------------------------------------------------------

Expected<MfskDemodulator> MfskDemodulator::create(const MfskDemodConfig& config) {
    if (config.rate <= 0) {
        return fail("MFSK demodulator: sample rate must be positive");
    }
    if (config.tones < kMfskMinTones || config.tones > kMfskMaxTones) {
        return fail("MFSK demodulator: tone count " + std::to_string(config.tones) +
                    " is outside 2 to 256");
    }
    if (!(config.tone_spacing_hz > 0.0) || !(config.symbol_rate > 0.0)) {
        return fail("MFSK demodulator: tone spacing and symbol rate must be positive");
    }
    const double rate = static_cast<double>(config.rate);
    if (config.symbol_rate * 4.0 > rate) {
        return fail("MFSK demodulator: symbol rate leaves fewer than four samples per symbol");
    }
    const double span = config.tone_spacing_hz * (config.tones - 1);
    if (config.search) {
        if (config.search_low_hz < config.tone_spacing_hz || config.search_high_hz > rate / 2.0 ||
            config.search_high_hz - config.search_low_hz < span) {
            return fail("MFSK demodulator: search band is narrower than the signal or outside "
                        "the audio band");
        }
    } else if (!(config.base_hz > 0.0) || config.base_hz + span >= rate / 2.0) {
        return fail("MFSK demodulator: tones do not fit below half the sample rate");
    }
    if (config.acquisition_symbols < 1 || config.timing_offsets < 2) {
        return fail("MFSK demodulator: need at least one acquisition symbol and two timing "
                    "offsets");
    }
    return MfskDemodulator(config);
}

MfskDemodulator::MfskDemodulator(const MfskDemodConfig& config) : config_(config) {
    samples_per_symbol_ = static_cast<double>(config_.rate) / config_.symbol_rate;
    window_ = static_cast<std::size_t>(std::llround(samples_per_symbol_));
    reset();
}

void MfskDemodulator::reset() {
    buffer_.clear();
    buffer_start_ = 0;
    received_ = 0;
    acquired_ = false;
    next_start_ = 0.0;
    tone0_hz_ = config_.base_hz;
    timing_curve_.assign(static_cast<std::size_t>(config_.timing_offsets), 0.0);
}

float MfskDemodulator::sample(std::int64_t index) const {
    if (index < buffer_start_ || index >= received_) {
        return 0.0F;
    }
    return buffer_[static_cast<std::size_t>(index - buffer_start_)];
}

void MfskDemodulator::energies_at(std::int64_t start, double tone0_hz,
                                  std::vector<double>& out) const {
    scratch_.resize(window_);
    for (std::size_t n = 0; n < window_; ++n) {
        scratch_[n] = sample(start + static_cast<std::int64_t>(n));
    }
    out.resize(static_cast<std::size_t>(config_.tones));
    bank(scratch_.data(), window_, tone0_hz, config_.tone_spacing_hz, config_.tones,
         static_cast<double>(config_.rate), out.data());
}

double MfskDemodulator::share_at(std::int64_t start) const {
    std::vector<double> e;
    energies_at(start, tone0_hz_, e);
    double total = 0.0;
    double best = 0.0;
    for (const double v : e) {
        total += v;
        best = std::max(best, v);
    }
    return total > 0.0 ? best / total : 0.0;
}

// The phase advance of the tone between the two halves of the window, which
// is 2 pi times the frequency error times the half-window duration.
double MfskDemodulator::frequency_error(std::int64_t start, double tone_hz) const {
    const std::size_t half = window_ / 2;
    std::vector<float> x(2 * half);
    for (std::size_t n = 0; n < x.size(); ++n) {
        x[n] = sample(start + static_cast<std::int64_t>(n));
    }
    const double rate = static_cast<double>(config_.rate);
    const auto first = correlate(x.data(), half, tone_hz, rate);
    // Same phase reference as the first half: advance by the half's length.
    const double w = -2.0 * std::numbers::pi * tone_hz * static_cast<double>(half) / rate;
    const auto second =
        correlate(x.data() + half, half, tone_hz, rate) * std::polar(1.0, w);
    const double advance = std::arg(second * std::conj(first));
    return advance * rate / (2.0 * std::numbers::pi * static_cast<double>(half));
}

void MfskDemodulator::process(ConstRealSpan audio, std::vector<MfskSymbol>& out) {
    buffer_.insert(buffer_.end(), audio.begin(), audio.end());
    received_ += static_cast<std::int64_t>(audio.size());

    const auto period = samples_per_symbol_;
    if (!acquired_) {
        const auto needed = static_cast<std::int64_t>(
            std::ceil(period * (config_.acquisition_symbols + 1))) +
                            static_cast<std::int64_t>(window_);
        if (received_ < needed) {
            return;
        }
        acquire();
    }

    const auto reach = static_cast<std::int64_t>(std::ceil(period / 2.0)) +
                       static_cast<std::int64_t>(window_) + 1;
    while (std::llround(next_start_) + reach <= received_) {
        emit(out);
    }
    trim();
}

void MfskDemodulator::acquire() {
    const double rate = static_cast<double>(config_.rate);
    const double period = samples_per_symbol_;
    const int symbols = config_.acquisition_symbols;
    const int m = config_.tones;
    const double spacing = config_.tone_spacing_hz;

    if (config_.search) {
        // A grid of R bins per tone spacing, each R no wider than half the
        // symbol rate, so the residual left for the fine estimate below is
        // inside its unambiguous range. One extra spacing either side of the
        // band supplies the guard bins the score subtracts.
        const double step_limit = std::min(spacing, config_.symbol_rate) / 2.0;
        const int r = std::max(1, static_cast<int>(std::ceil(spacing / step_limit)));
        const double step = spacing / r;
        const double grid_low = config_.search_low_hz - spacing;
        const int candidates = static_cast<int>(
            std::floor((config_.search_high_hz - config_.search_low_hz - spacing * (m - 1)) /
                       step)) + 1;
        const int bins = candidates + r * (m + 1);

        std::vector<std::vector<double>> grid(static_cast<std::size_t>(symbols));
        std::vector<float> x(window_);
        for (int i = 0; i < symbols; ++i) {
            const auto start = static_cast<std::int64_t>(std::llround(i * period));
            for (std::size_t n = 0; n < window_; ++n) {
                x[n] = sample(start + static_cast<std::int64_t>(n));
            }
            auto& row = grid[static_cast<std::size_t>(i)];
            row.resize(static_cast<std::size_t>(bins));
            bank(x.data(), window_, grid_low, step, bins, rate, row.data());
        }
        // A candidate scores its strongest tone per symbol, less the stronger
        // of the two bins one spacing outside it. Without the guard term a
        // candidate one spacing off loses only the symbols on the edge tone
        // it dropped, which for large M is a handful of symbols or none.
        int best = 0;
        double best_score = -1.0e300;
        for (int c = 0; c < candidates; ++c) {
            double score = 0.0;
            for (const auto& row : grid) {
                double inside = 0.0;
                for (int k = 0; k < m; ++k) {
                    inside = std::max(inside, row[static_cast<std::size_t>(c + r + k * r)]);
                }
                const double outside = std::max(row[static_cast<std::size_t>(c)],
                                                 row[static_cast<std::size_t>(c + r * (m + 1))]);
                score += inside - outside;
            }
            if (score > best_score) {
                best_score = score;
                best = c;
            }
        }
        tone0_hz_ = grid_low + (best + r) * step;
    }

    // Timing: the share of energy in the strongest tone, summed over the
    // acquisition symbols, at each offset across one period. A window that
    // straddles a symbol boundary splits its energy between two tones.
    const int offsets = config_.timing_offsets;
    std::vector<double> curve(static_cast<std::size_t>(offsets), 0.0);
    for (int p = 0; p < offsets; ++p) {
        const double d = p * period / offsets;
        for (int i = 0; i < symbols; ++i) {
            curve[static_cast<std::size_t>(p)] += share_at(std::llround(d + i * period));
        }
    }
    const auto peak = static_cast<int>(
        std::distance(curve.begin(), std::max_element(curve.begin(), curve.end())));
    const double fraction =
        parabolic(curve[static_cast<std::size_t>(wrap(peak - 1, offsets))],
                  curve[static_cast<std::size_t>(peak)],
                  curve[static_cast<std::size_t>(wrap(peak + 1, offsets))]);
    double start = (peak + fraction) * period / offsets;
    if (start < 0.0) {
        start += period;
    }
    next_start_ = start;

    // Fine frequency from the acquisition symbols themselves: the median of
    // the per-symbol phase-difference estimates on confident decisions.
    if (config_.afc || config_.search) {
        std::vector<double> errors;
        std::vector<double> e;
        for (int i = 0; i < symbols; ++i) {
            const auto s = std::llround(start + i * period);
            energies_at(s, tone0_hz_, e);
            double total = 0.0;
            std::size_t k = 0;
            for (std::size_t j = 0; j < e.size(); ++j) {
                total += e[j];
                if (e[j] > e[k]) {
                    k = j;
                }
            }
            if (total > 0.0 && e[k] / total >= config_.afc_min_confidence) {
                errors.push_back(frequency_error(s, tone0_hz_ + spacing * static_cast<double>(k)));
            }
        }
        if (!errors.empty()) {
            std::nth_element(errors.begin(), errors.begin() + static_cast<std::ptrdiff_t>(errors.size() / 2),
                             errors.end());
            const double limit = std::min(spacing / 2.0, config_.symbol_rate);
            tone0_hz_ += std::clamp(errors[errors.size() / 2], -limit, limit);
        }
    }

    std::fill(timing_curve_.begin(), timing_curve_.end(), 0.0);
    acquired_ = true;
}

void MfskDemodulator::emit(std::vector<MfskSymbol>& out) {
    const double period = samples_per_symbol_;
    const auto start = std::llround(next_start_);

    std::vector<double> e;
    energies_at(start, tone0_hz_, e);
    double total = 0.0;
    std::size_t tone = 0;
    for (std::size_t j = 0; j < e.size(); ++j) {
        total += e[j];
        if (e[j] > e[tone]) {
            tone = j;
        }
    }
    const double confidence = total > 0.0 ? e[tone] / total : 0.0;

    MfskSymbol symbol;
    symbol.start = start;
    symbol.tone = static_cast<int>(tone);
    symbol.confidence = static_cast<float>(confidence);
    symbol.tone0_hz = tone0_hz_;
    symbol.energies.reserve(e.size());
    for (const double v : e) {
        symbol.energies.push_back(static_cast<float>(v));
    }
    out.push_back(std::move(symbol));

    if (config_.track_timing) {
        // Averaged energy share at offsets spread over one period centred on
        // the current estimate; steer a fraction of the way to its peak.
        const int offsets = config_.timing_offsets;
        const int centre = offsets / 2;
        const double a = config_.timing_average;
        for (int p = 0; p < offsets; ++p) {
            const double share =
                p == centre ? confidence
                            : share_at(start + std::llround((p - centre) * period / offsets));
            auto& v = timing_curve_[static_cast<std::size_t>(p)];
            v = (1.0 - a) * v + a * share;
        }
        const auto peak = static_cast<int>(std::distance(
            timing_curve_.begin(), std::max_element(timing_curve_.begin(), timing_curve_.end())));
        double fraction = 0.0;
        if (peak > 0 && peak < offsets - 1) {
            fraction = parabolic(timing_curve_[static_cast<std::size_t>(peak - 1)],
                                 timing_curve_[static_cast<std::size_t>(peak)],
                                 timing_curve_[static_cast<std::size_t>(peak + 1)]);
        }
        next_start_ += config_.timing_gain * (peak - centre + fraction) * period / offsets;
    }

    if (config_.afc && confidence >= config_.afc_min_confidence) {
        const double limit = std::min(config_.tone_spacing_hz / 2.0, config_.symbol_rate);
        const double error = std::clamp(
            frequency_error(start, tone0_hz_ + config_.tone_spacing_hz * static_cast<double>(tone)),
            -limit, limit);
        tone0_hz_ += config_.afc_gain * error;
    }

    next_start_ += period;
}

void MfskDemodulator::trim() {
    const auto keep = static_cast<std::int64_t>(std::floor(next_start_ - samples_per_symbol_)) - 2;
    if (keep <= buffer_start_) {
        return;
    }
    const auto drop = std::min<std::int64_t>(keep - buffer_start_,
                                             static_cast<std::int64_t>(buffer_.size()));
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(drop));
    buffer_start_ += drop;
}

// ---------------------------------------------------------------------------
// IFK
// ---------------------------------------------------------------------------

int ifk_next_tone(int previous_tone, int symbol, int tones, int offset) {
    return wrap(previous_tone + symbol + offset, tones);
}

int ifk_symbol(int previous_tone, int tone, int tones, int offset) {
    return wrap(tone - previous_tone - offset, tones);
}

std::vector<int> ifk_encode(std::span<const int> symbols, int tones, int offset,
                            int initial_tone) {
    std::vector<int> out;
    out.reserve(symbols.size());
    int previous = initial_tone;
    for (const int s : symbols) {
        previous = ifk_next_tone(previous, s, tones, offset);
        out.push_back(previous);
    }
    return out;
}

std::vector<int> ifk_decode(std::span<const int> tone_sequence, int tones, int offset,
                            int initial_tone) {
    std::vector<int> out;
    out.reserve(tone_sequence.size());
    int previous = initial_tone;
    for (const int t : tone_sequence) {
        out.push_back(ifk_symbol(previous, t, tones, offset));
        previous = t;
    }
    return out;
}

std::vector<float> ifk_soft(std::span<const float> energies, int previous_tone, int offset) {
    const int m = static_cast<int>(energies.size());
    std::vector<float> out(energies.size());
    for (int s = 0; s < m; ++s) {
        out[static_cast<std::size_t>(s)] =
            energies[static_cast<std::size_t>(ifk_next_tone(previous_tone, s, m, offset))];
    }
    return out;
}

// ---------------------------------------------------------------------------
// MfskRaster
// ---------------------------------------------------------------------------

Expected<MfskRaster> MfskRaster::create(const MfskRasterConfig& config) {
    if (config.rate <= 0 || !(config.rows_per_second > 0.0) || config.bins < 1 ||
        !(config.bin_spacing_hz > 0.0) || !(config.low_hz >= 0.0)) {
        return fail("MFSK raster: rate, row rate, bin count and bin spacing must be positive");
    }
    if (config.rows_per_second * 4.0 > static_cast<double>(config.rate)) {
        return fail("MFSK raster: row rate leaves fewer than four samples per row");
    }
    return MfskRaster(config);
}

MfskRaster::MfskRaster(const MfskRasterConfig& config) : config_(config) {
    samples_per_row_ = static_cast<double>(config_.rate) / config_.rows_per_second;
}

void MfskRaster::reset() {
    buffer_.clear();
    buffer_start_ = 0;
    next_row_ = 0;
}

void MfskRaster::process(ConstRealSpan audio, std::vector<std::vector<float>>& rows) {
    buffer_.insert(buffer_.end(), audio.begin(), audio.end());
    const auto length = static_cast<std::size_t>(std::llround(samples_per_row_));
    std::vector<double> e(static_cast<std::size_t>(config_.bins));
    for (;;) {
        const auto start =
            static_cast<std::int64_t>(std::llround(static_cast<double>(next_row_) * samples_per_row_));
        const auto offset = static_cast<std::size_t>(start - buffer_start_);
        if (offset + length > buffer_.size()) {
            break;
        }
        bank(buffer_.data() + offset, length, config_.low_hz, config_.bin_spacing_hz, config_.bins,
             static_cast<double>(config_.rate), e.data());
        auto& row = rows.emplace_back();
        row.reserve(e.size());
        for (const double v : e) {
            row.push_back(static_cast<float>(v));
        }
        ++next_row_;
    }
    const auto next =
        static_cast<std::int64_t>(std::llround(static_cast<double>(next_row_) * samples_per_row_));
    const auto drop = std::min<std::int64_t>(next - buffer_start_,
                                             static_cast<std::int64_t>(buffer_.size()));
    if (drop > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(drop));
        buffer_start_ += drop;
    }
}

}  // namespace revenant::decode
