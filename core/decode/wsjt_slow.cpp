#include "core/decode/wsjt_slow.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace revenant::decode {

namespace {

using C = std::complex<double>;

// Smallest length at or above n whose only prime factors are 2, 3 and 5, so
// wsjt_fft stays fast.
std::size_t nice_size(std::size_t n) {
    for (std::size_t m = std::max<std::size_t>(n, 1);; ++m) {
        std::size_t r = m;
        for (const std::size_t f : {2U, 3U, 5U}) {
            while (r % f == 0) r /= f;
        }
        if (r == 1) return m;
    }
}

double tone_freq(const SlowProtocol& p, double f, double drift, int s, int k) {
    return f + drift * ((s + 0.5) / p.symbols - 0.5) + k * p.tone_spacing_hz;
}

}  // namespace

std::vector<std::complex<float>> slow_reference(const SlowProtocol& p, std::span<const int> tones,
                                                SampleRate rate, double drift_hz) {
    const double fs = static_cast<double>(rate);
    const auto total = static_cast<std::size_t>(std::llround(p.symbols * p.symbol_seconds * fs));
    std::vector<std::complex<float>> out(total);
    double phase = 0.0;
    for (std::size_t i = 0; i < total; ++i) {
        const double t = static_cast<double>(i) / fs;
        const int s = std::min(static_cast<int>(t / p.symbol_seconds), static_cast<int>(tones.size()) - 1);
        const double frac = t / (p.symbols * p.symbol_seconds) - 0.5;
        const double f = drift_hz * frac + tones[static_cast<std::size_t>(s)] * p.tone_spacing_hz;
        out[i] = {static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
        phase += 2.0 * std::numbers::pi * f / fs;
        if (phase > 2.0 * std::numbers::pi) phase -= 2.0 * std::numbers::pi;
        if (phase < 0.0) phase += 2.0 * std::numbers::pi;
    }
    return out;
}

SlowSlot::SlowSlot(const SlowProtocol& protocol, std::span<const float> slot_12k)
    : p_(protocol), audio_(slot_12k) {}

std::vector<SlowCandidate> SlowSlot::candidates(const SlowSearch& search) {
    const double rate = kWsjtRate;
    const auto len = static_cast<std::int64_t>(audio_.size());
    const auto w = static_cast<std::int64_t>(std::llround(p_.symbol_seconds * rate));
    const std::int64_t hop = std::max<std::int64_t>(1, std::llround(static_cast<double>(w) / 4.0));
    const std::size_t nfft = nice_size(static_cast<std::size_t>(2 * w));
    const double bin_hz = rate / static_cast<double>(nfft);
    const double span = (p_.tones - 1) * p_.tone_spacing_hz;
    const auto b_lo = static_cast<std::int64_t>(std::max(0.0, std::floor((search.low_hz - p_.tone_spacing_hz) / bin_hz)));
    const auto b_hi = static_cast<std::int64_t>(std::min(static_cast<double>(nfft / 2 - 1),
                                                         std::ceil((search.high_hz + span + p_.tone_spacing_hz) / bin_hz)));
    if (len < w || b_hi <= b_lo) return {};
    const std::int64_t nb = b_hi - b_lo + 1;
    const std::int64_t frames = (len - w) / hop + 1;

    std::vector<float> pw(static_cast<std::size_t>(frames * nb));
    std::vector<C> buf(nfft);
    for (std::int64_t f = 0; f < frames; ++f) {
        std::fill(buf.begin(), buf.end(), C{});
        for (std::int64_t i = 0; i < w; ++i) buf[static_cast<std::size_t>(i)] = audio_[static_cast<std::size_t>(f * hop + i)];
        wsjt_fft(buf, false);
        for (std::int64_t b = 0; b < nb; ++b) {
            pw[static_cast<std::size_t>(f * nb + b)] = static_cast<float>(std::norm(buf[static_cast<std::size_t>(b_lo + b)]));
        }
    }
    // NOT SPECIFIED: the noise floor of a bin is the median, over 50 bins
    // either side, of the per-bin mean over the slot.
    std::vector<double> mean(static_cast<std::size_t>(nb), 0.0);
    for (std::int64_t f = 0; f < frames; ++f) {
        for (std::int64_t b = 0; b < nb; ++b) mean[static_cast<std::size_t>(b)] += pw[static_cast<std::size_t>(f * nb + b)];
    }
    std::vector<double> floor_of(static_cast<std::size_t>(nb));
    std::vector<double> win;
    for (std::int64_t b = 0; b < nb; ++b) {
        win.assign(mean.begin() + std::max<std::int64_t>(0, b - 50), mean.begin() + std::min(nb, b + 51));
        std::nth_element(win.begin(), win.begin() + static_cast<std::ptrdiff_t>(win.size() / 2), win.end());
        floor_of[static_cast<std::size_t>(b)] = std::max(win[win.size() / 2] / static_cast<double>(frames), 1e-30);
    }
    for (std::int64_t f = 0; f < frames; ++f) {
        for (std::int64_t b = 0; b < nb; ++b) {
            pw[static_cast<std::size_t>(f * nb + b)] = static_cast<float>(pw[static_cast<std::size_t>(f * nb + b)] / floor_of[static_cast<std::size_t>(b)]);
        }
    }

    std::vector<std::int64_t> off(static_cast<std::size_t>(p_.tones));
    for (int k = 0; k < p_.tones; ++k) off[static_cast<std::size_t>(k)] = std::llround(k * p_.tone_spacing_hz / bin_hz);
    std::vector<std::int64_t> sfr(static_cast<std::size_t>(p_.symbols));
    for (int s = 0; s < p_.symbols; ++s) sfr[static_cast<std::size_t>(s)] = std::llround(s * p_.symbol_seconds * rate / static_cast<double>(hop));
    const std::int64_t f_lo = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::ceil(p_.start_min_seconds * rate / static_cast<double>(hop))));
    const std::int64_t f_hi = std::min<std::int64_t>(frames - 1 - sfr.back(),
                                                     static_cast<std::int64_t>(std::floor(p_.start_max_seconds * rate / static_cast<double>(hop))));
    std::size_t n_on = 0;
    std::size_t n_off = 0;
    for (int s = 0; s < p_.symbols; ++s) {
        n_on += p_.sync_on[static_cast<std::size_t>(s)].size();
        n_off += p_.sync_off[static_cast<std::size_t>(s)].size();
    }

    const auto t0_lo = static_cast<std::int64_t>(std::ceil(search.low_hz / bin_hz)) - b_lo;
    const auto t0_hi = static_cast<std::int64_t>(std::floor(search.high_hz / bin_hz)) - b_lo;
    std::vector<double> best(static_cast<std::size_t>(nb), -1e30);
    std::vector<std::int64_t> best_f(static_cast<std::size_t>(nb), 0);
    std::vector<double> best_d(static_cast<std::size_t>(nb), 0.0);
    // NOT SPECIFIED: drift is searched coarsely in steps of two bins.
    std::vector<double> drifts{0.0};
    for (double d = 2.0 * bin_hz; d <= search.max_drift_hz + 1e-9; d += 2.0 * bin_hz) {
        drifts.push_back(d);
        drifts.push_back(-d);
    }
    std::vector<std::int64_t> shift(static_cast<std::size_t>(p_.symbols));
    for (const double d : drifts) {
        for (int s = 0; s < p_.symbols; ++s) {
            shift[static_cast<std::size_t>(s)] = std::llround(d * ((s + 0.5) / p_.symbols - 0.5) / bin_hz);
        }
        const std::int64_t reach = std::llround(std::abs(d) / 2.0 / bin_hz) + 1;
        for (std::int64_t b = std::max<std::int64_t>(reach, t0_lo); b <= t0_hi; ++b) {
            if (b + off.back() + reach >= nb) break;
            for (std::int64_t f0 = f_lo; f0 <= f_hi; ++f0) {
                double on = 0.0;
                double offs = 0.0;
                for (int s = 0; s < p_.symbols; ++s) {
                    const float* row = &pw[static_cast<std::size_t>((f0 + sfr[static_cast<std::size_t>(s)]) * nb + b +
                                                                    shift[static_cast<std::size_t>(s)])];
                    for (const int k : p_.sync_on[static_cast<std::size_t>(s)]) on += row[off[static_cast<std::size_t>(k)]];
                    for (const int k : p_.sync_off[static_cast<std::size_t>(s)]) offs += row[off[static_cast<std::size_t>(k)]];
                }
                const double sc = (n_on ? on / static_cast<double>(n_on) : 0.0) - (n_off ? offs / static_cast<double>(n_off) : 0.0);
                if (sc > best[static_cast<std::size_t>(b)]) {
                    best[static_cast<std::size_t>(b)] = sc;
                    best_f[static_cast<std::size_t>(b)] = f0;
                    best_d[static_cast<std::size_t>(b)] = d;
                }
            }
        }
    }
    // Local maxima within a tone spacing either side.
    const std::int64_t nbhd = std::max<std::int64_t>(1, off.size() > 1 ? off[1] : 1);
    std::vector<SlowCandidate> out;
    for (std::int64_t b = 0; b < nb; ++b) {
        const double v = best[static_cast<std::size_t>(b)];
        if (v < search.sync_threshold) continue;
        bool peak = true;
        for (std::int64_t d = -nbhd; d <= nbhd && peak; ++d) {
            const std::int64_t j = b + d;
            if (d == 0 || j < 0 || j >= nb) continue;
            const double u = best[static_cast<std::size_t>(j)];
            if (u > v || (u == v && d < 0)) peak = false;
        }
        if (!peak) continue;
        SlowCandidate c;
        c.frequency_hz = static_cast<double>(b + b_lo) * bin_hz;
        c.start_seconds = static_cast<double>(best_f[static_cast<std::size_t>(b)] * hop) / rate;
        c.drift_hz = best_d[static_cast<std::size_t>(b)];
        c.sync = v;
        out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const SlowCandidate& a, const SlowCandidate& b) {
        return a.sync > b.sync || (a.sync == b.sync && a.frequency_hz < b.frequency_hz);
    });
    if (out.size() > static_cast<std::size_t>(search.max_candidates)) out.resize(static_cast<std::size_t>(search.max_candidates));
    return out;
}

SlowSlot::Baseband SlowSlot::baseband(double low_hz, double high_hz) {
    const auto len = audio_.size();
    if (spectrum_.empty()) {
        spectrum_.assign(len, C{});
        for (std::size_t i = 0; i < len; ++i) spectrum_[i] = audio_[i];
        wsjt_fft(spectrum_, false);
    }
    const double t = static_cast<double>(len) / kWsjtRate;
    // NOT SPECIFIED: at least 16 samples per symbol.
    const double need = std::max((high_hz - low_hz) * t, 16.0 * t / p_.symbol_seconds);
    const std::size_t m = nice_size(static_cast<std::size_t>(std::ceil(need)));
    const auto kc = static_cast<std::int64_t>(std::llround(0.5 * (low_hz + high_hz) * t));
    Baseband b;
    b.x.assign(m, C{});
    const auto half = static_cast<std::int64_t>(m / 2);
    for (std::int64_t i = 0; i < static_cast<std::int64_t>(m); ++i) {
        const std::int64_t rel = i < half ? i : i - static_cast<std::int64_t>(m);
        const std::int64_t k = kc + rel;
        if (k > 0 && k < static_cast<std::int64_t>(len / 2)) b.x[static_cast<std::size_t>(i)] = spectrum_[static_cast<std::size_t>(k)];
    }
    wsjt_fft(b.x, true);
    b.rate = static_cast<double>(m) / t;
    b.centre_hz = static_cast<double>(kc) / t;
    return b;
}

std::complex<double> SlowSlot::tone_dft(const Baseband& b, int symbol, double start, double freq) const {
    const auto n0 = static_cast<std::int64_t>(std::llround((start + symbol * p_.symbol_seconds) * b.rate));
    const auto ls = static_cast<std::int64_t>(std::llround(p_.symbol_seconds * b.rate));
    const double w = -2.0 * std::numbers::pi * (freq - b.centre_hz) / b.rate;
    const C step(std::cos(w), std::sin(w));
    C rot(std::cos(w * static_cast<double>(n0)), std::sin(w * static_cast<double>(n0)));
    C acc{};
    const auto n = static_cast<std::int64_t>(b.x.size());
    for (std::int64_t i = 0; i < ls; ++i) {
        const std::int64_t j = n0 + i;
        if (j >= 0 && j < n) acc += b.x[static_cast<std::size_t>(j)] * rot;
        rot *= step;
    }
    return acc;
}

double SlowSlot::noise_per_bin(const Baseband& b) const {
    // NOT SPECIFIED: |x|^2 of complex Gaussian noise is exponential, so the
    // median over ln 2 estimates its mean while shrugging off the signal.
    std::vector<double> p(b.x.size());
    for (std::size_t i = 0; i < b.x.size(); ++i) p[i] = std::norm(b.x[i]);
    std::nth_element(p.begin(), p.begin() + static_cast<std::ptrdiff_t>(p.size() / 2), p.end());
    const double ls = std::round(p_.symbol_seconds * b.rate);
    return std::max(p[p.size() / 2] / std::numbers::ln2 * ls, 1e-30);
}

double SlowSlot::score(const Baseband& b, double start, double freq, double drift) const {
    double on = 0.0;
    double off = 0.0;
    std::size_t n_on = 0;
    std::size_t n_off = 0;
    for (int s = 0; s < p_.symbols; ++s) {
        for (const int k : p_.sync_on[static_cast<std::size_t>(s)]) {
            on += std::norm(tone_dft(b, s, start, tone_freq(p_, freq, drift, s, k)));
            ++n_on;
        }
        for (const int k : p_.sync_off[static_cast<std::size_t>(s)]) {
            off += std::norm(tone_dft(b, s, start, tone_freq(p_, freq, drift, s, k)));
            ++n_off;
        }
    }
    return (n_on ? on / static_cast<double>(n_on) : 0.0) - (n_off ? off / static_cast<double>(n_off) : 0.0);
}

SlowSpectra SlowSlot::demodulate(const SlowCandidate& c, const SlowSearch& search) {
    const double df = p_.tone_spacing_hz;
    const double span = (p_.tones - 1) * df;
    // NOT SPECIFIED: three tone spacings of guard each side, plus the drift.
    const double guard = 3.0 * df + 0.5 * search.max_drift_hz + 2.0;
    const Baseband b = baseband(c.frequency_hz - guard, c.frequency_hz + span + guard);
    const double noise = noise_per_bin(b);

    SlowCandidate best = c;
    double best_score = score(b, best.start_seconds, best.frequency_hz, best.drift_hz);
    // NOT SPECIFIED: a grid of nine steps each way in time (a sixteenth of a
    // symbol) and frequency (an eighth of a tone spacing), and drift in
    // half tone spacings, refined twice at half the step.
    double dt = p_.symbol_seconds / 16.0;
    double dfq = df / 8.0;
    for (int round = 0; round < 3; ++round) {
        if (search.max_drift_hz > 0.0) {
            const double step = df / 2.0 / (round + 1);
            const double centre = best.drift_hz;
            for (double d = centre - search.max_drift_hz / (round + 1); d <= centre + search.max_drift_hz / (round + 1) + 1e-9; d += step) {
                if (std::abs(d) > search.max_drift_hz + 1e-9) continue;
                const double v = score(b, best.start_seconds, best.frequency_hz, d);
                if (v > best_score) {
                    best_score = v;
                    best.drift_hz = d;
                }
            }
        }
        const SlowCandidate centre = best;
        for (int i = -4; i <= 4; ++i) {
            for (int j = -4; j <= 4; ++j) {
                const double st = centre.start_seconds + i * dt;
                const double fr = centre.frequency_hz + j * dfq;
                const double v = score(b, st, fr, best.drift_hz);
                if (v > best_score) {
                    best_score = v;
                    best.start_seconds = st;
                    best.frequency_hz = fr;
                }
            }
        }
        dt /= 2.0;
        dfq /= 2.0;
    }
    best.sync = best_score / noise;

    SlowSpectra out;
    out.where = best;
    out.tones = p_.tones;
    out.power.resize(static_cast<std::size_t>(p_.symbols) * static_cast<std::size_t>(p_.tones));
    for (int s = 0; s < p_.symbols; ++s) {
        for (int k = 0; k < p_.tones; ++k) {
            out.power[static_cast<std::size_t>(s * p_.tones + k)] = static_cast<float>(
                std::norm(tone_dft(b, s, best.start_seconds, tone_freq(p_, best.frequency_hz, best.drift_hz, s, k))) / noise);
        }
    }
    return out;
}

double slow_snr_2500(const SlowProtocol& p, const SlowSpectra& s, std::span<const int> sent) {
    double sum = 0.0;
    for (std::size_t i = 0; i < sent.size(); ++i) sum += s.at(static_cast<int>(i), sent[i]) - 1.0;
    const double y = std::max(sum / static_cast<double>(sent.size()), 1e-3);
    return 10.0 * std::log10(y) - 10.0 * std::log10(2500.0 * p.symbol_seconds);
}

double slow_log_i0(double x) {
    x = std::abs(x);
    if (x < 30.0) return std::log(std::cyl_bessel_i(0.0, x));
    // Abramowitz and Stegun 9.7.1, two terms.
    return x - 0.5 * std::log(2.0 * std::numbers::pi * x) + std::log1p(1.0 / (8.0 * x));
}

double slow_amplitude(const SlowSpectra& s, int first_tone, int last_tone) {
    const int n = last_tone - first_tone + 1;
    const int symbols = static_cast<int>(s.power.size()) / s.tones;
    double sum = 0.0;
    for (int i = 0; i < symbols; ++i) {
        float m = 0.0F;
        for (int k = first_tone; k <= last_tone; ++k) m = std::max(m, s.at(i, k));
        sum += m;
    }
    // The largest of n unit exponentials has mean H_n; take that off.
    double h = 0.0;
    for (int k = 1; k <= n; ++k) h += 1.0 / k;
    // NOT SPECIFIED: floor of a quarter of the noise.
    return std::sqrt(std::max(sum / symbols - h, 0.25));
}

Expected<SlowStream> SlowStream::create(double slot_seconds, SampleRate rate, WsjtUtcOfSample utc) {
    auto input = WsjtInputStage::create(rate);
    if (!input) return std::unexpected(input.error());
    if (!utc) utc = wsjt_wall_clock();
    return SlowStream(slot_seconds, std::move(*input), std::move(utc), rate);
}

SlowStream::SlowStream(double slot_seconds, WsjtInputStage input, WsjtUtcOfSample utc, SampleRate rate)
    : slot_seconds_(slot_seconds), input_(std::move(input)), clock_(slot_seconds, std::move(utc), rate) {}

void SlowStream::process(ConstRealSpan audio, const SlotFn& fn) {
    if (!started_) {
        started_ = true;
        next_slot_ = clock_.first_boundary();
        buffer_start_ = 0;
    }
    scratch_.clear();
    input_.process(audio, scratch_);
    buffer_.insert(buffer_.end(), scratch_.begin(), scratch_.end());
    received_ += static_cast<std::int64_t>(scratch_.size());
    const std::int64_t len = clock_.slot_samples();
    while (received_ >= next_slot_ + len) {
        const std::int64_t off = next_slot_ - buffer_start_;
        std::vector<float> slot(static_cast<std::size_t>(len), 0.0F);
        for (std::int64_t i = 0; i < len; ++i) {
            const std::int64_t j = off + i;
            if (j >= 0 && j < static_cast<std::int64_t>(buffer_.size())) slot[static_cast<std::size_t>(i)] = buffer_[static_cast<std::size_t>(j)];
        }
        const std::int64_t k = (next_slot_ - clock_.first_boundary()) / len;
        const double utc = clock_.first_boundary_utc() + static_cast<double>(k) * slot_seconds_;
        fn(slot, utc, static_cast<SampleIndex>(std::max<std::int64_t>(0, next_slot_)) * static_cast<SampleIndex>(input_.factor()));
        next_slot_ += len;
    }
    const std::int64_t drop = std::min<std::int64_t>(next_slot_ - buffer_start_, static_cast<std::int64_t>(buffer_.size()));
    if (drop > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + drop);
        buffer_start_ += drop;
    }
}

std::vector<std::uint8_t> wsjt_conv_encode(std::span<const std::uint8_t> bits) {
    std::vector<std::uint8_t> out;
    out.reserve(bits.size() * 2);
    std::uint32_t reg = 0;
    for (const auto b : bits) {
        reg = (reg << 1U) | (b & 1U);
        out.push_back(static_cast<std::uint8_t>(std::popcount(reg & kWsjtConvG0) & 1));
        out.push_back(static_cast<std::uint8_t>(std::popcount(reg & kWsjtConvG1) & 1));
    }
    return out;
}

namespace {
std::uint32_t reverse8(std::uint32_t i) {
    std::uint32_t j = 0;
    for (int b = 0; b < 8; ++b) j |= ((i >> b) & 1U) << (7 - b);
    return j;
}
}  // namespace

std::vector<std::uint8_t> wsjt_interleave(std::span<const std::uint8_t> s) {
    std::vector<std::uint8_t> d(s.size());
    std::size_t p = 0;
    for (std::uint32_t i = 0; i < 256 && p < s.size(); ++i) {
        const std::uint32_t j = reverse8(i);
        if (j < s.size()) d[j] = s[p++];
    }
    return d;
}

std::vector<float> wsjt_deinterleave(std::span<const float> d) {
    std::vector<float> s(d.size());
    std::size_t p = 0;
    for (std::uint32_t i = 0; i < 256 && p < d.size(); ++i) {
        const std::uint32_t j = reverse8(i);
        if (j < d.size()) s[p++] = d[j];
    }
    return s;
}

}  // namespace revenant::decode
