#include "core/decode/ofdm.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <string>
#include <utility>

#include "core/characterise/transform.h"

namespace revenant::decode {

namespace {

using C64 = std::complex<double>;

// Phase of the correction for absolute sample index, reduced before the
// multiply by two pi so a long stream does not lose the fraction.
C64 rotation(double cfo_carriers, std::int64_t index, std::size_t fft_size)
{
    const double cycles = cfo_carriers * static_cast<double>(index) / static_cast<double>(fft_size);
    const double fraction = cycles - std::floor(cycles);
    const double phase = -2.0 * std::numbers::pi * fraction;
    return {std::cos(phase), std::sin(phase)};
}

std::size_t bin_index(int carrier, std::size_t fft_size)
{
    const auto n = static_cast<long long>(fft_size);
    long long b = static_cast<long long>(carrier) % n;
    if (b < 0) {
        b += n;
    }
    return static_cast<std::size_t>(b);
}

}  // namespace

Status validate(const OfdmConfig& config)
{
    if (config.sample_rate <= 0) {
        return fail("ofdm: sample rate must be positive");
    }
    if (!characterise::is_power_of_two(config.fft_size) || config.fft_size < 16 ||
        config.fft_size > 65536) {
        return fail("ofdm: fft_size must be a power of two from 16 to 65536");
    }
    if (config.cp_length == 0 || config.cp_length > config.fft_size) {
        return fail("ofdm: cp_length must be between 1 and fft_size");
    }
    if (config.carriers.empty()) {
        return fail("ofdm: no carriers");
    }
    const int half = static_cast<int>(config.fft_size / 2);
    std::vector<std::uint8_t> seen(config.fft_size, 0);
    for (const int k : config.carriers) {
        if (k < -half || k >= half) {
            return fail("ofdm: carrier " + std::to_string(k) + " is outside the transform");
        }
        auto& flag = seen[bin_index(k, config.fft_size)];
        if (flag != 0) {
            return fail("ofdm: carrier " + std::to_string(k) + " listed twice");
        }
        flag = 1;
    }
    if (config.pilot_table.empty()) {
        return fail("ofdm: pilot table is empty");
    }
    for (const auto& row : config.pilot_table) {
        if (row.size() < 2) {
            return fail("ofdm: every pilot table entry needs at least two pilots");
        }
        for (const auto& pilot : row) {
            if (pilot.carrier < -half || pilot.carrier >= half ||
                seen[bin_index(pilot.carrier, config.fft_size)] == 0) {
                return fail("ofdm: pilot carrier " + std::to_string(pilot.carrier) +
                            " is not a used carrier");
            }
            if (std::abs(pilot.value) == 0.0F) {
                return fail("ofdm: pilot of zero amplitude");
            }
        }
    }
    if (config.acquisition_symbols < 2) {
        return fail("ofdm: acquisition needs at least two symbols");
    }
    if (config.max_integer_cfo < 0 || config.max_integer_cfo >= half) {
        return fail("ofdm: max_integer_cfo out of range");
    }
    if (config.window_backoff > config.cp_length) {
        return fail("ofdm: window_backoff exceeds the prefix");
    }
    return {};
}

Expected<OfdmReceiver> OfdmReceiver::create(OfdmConfig config)
{
    if (auto ok = validate(config); !ok) {
        return std::unexpected(ok.error());
    }
    for (auto& row : config.pilot_table) {
        std::sort(row.begin(), row.end(),
                  [](const OfdmPilot& a, const OfdmPilot& b) { return a.carrier < b.carrier; });
    }
    return OfdmReceiver(std::move(config));
}

OfdmReceiver::OfdmReceiver(OfdmConfig config) : config_(std::move(config))
{
    backoff_ = config_.window_backoff == 0 ? config_.cp_length / 4 : config_.window_backoff;

    // Time interpolation needs the next pilot on every carrier that has one,
    // so output waits for the longest gap between pilots on any carrier.
    const std::size_t period = config_.pilot_table.size();
    lookahead_ = 0;
    for (const int k : config_.carriers) {
        std::vector<std::size_t> rows;
        for (std::size_t s = 0; s < period; ++s) {
            for (const auto& p : config_.pilot_table[s]) {
                if (p.carrier == k) {
                    rows.push_back(s);
                    break;
                }
            }
        }
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const std::size_t next = i + 1 < rows.size() ? rows[i + 1] : rows[0] + period;
            lookahead_ = std::max(lookahead_, next - rows[i]);
        }
    }
}

void OfdmReceiver::reset()
{
    sync_ = {};
    buffer_.clear();
    buffer_start_ = total_;
    next_attempt_ = total_;
    next_demod_ = 0;
    next_emit_ = 0;
    window_.clear();
    noise_variance_ = 0.0;
    noise_updates_ = 0;
}

Complex32 OfdmReceiver::sample_at(std::int64_t index) const
{
    return buffer_[static_cast<std::size_t>(index - buffer_start_)];
}

void OfdmReceiver::fft_at(std::int64_t start, double cfo, std::vector<Complex32>& bins) const
{
    const std::size_t n = config_.fft_size;
    std::vector<C64> work(n);
    const std::int64_t first =
        start + static_cast<std::int64_t>(config_.cp_length) - static_cast<std::int64_t>(backoff_);
    for (std::size_t i = 0; i < n; ++i) {
        const std::int64_t idx = first + static_cast<std::int64_t>(i);
        const Complex32 x = sample_at(idx);
        work[i] = C64(x.real(), x.imag()) * rotation(cfo, idx, n);
    }
    characterise::fft_in_place(work);
    // Starting the window backoff samples early delays each bin's phase by
    // 2 pi b backoff / N; undo it so the estimate sees only the channel.
    const double scale = 1.0 / std::sqrt(static_cast<double>(n));
    bins.resize(n);
    for (std::size_t b = 0; b < n; ++b) {
        const double phase = 2.0 * std::numbers::pi * static_cast<double>(b * backoff_ % n) /
                             static_cast<double>(n);
        const C64 v = work[b] * scale * C64(std::cos(phase), std::sin(phase));
        bins[b] = Complex32(static_cast<float>(v.real()), static_cast<float>(v.imag()));
    }
}

bool OfdmReceiver::try_acquire()
{
    const std::size_t n = config_.fft_size;
    const std::size_t cp = config_.cp_length;
    const std::size_t len = n + cp;
    const std::size_t m = config_.acquisition_symbols;
    const std::size_t span = (m + 1) * len;
    if (total_ < next_attempt_ + static_cast<std::int64_t>(span)) {
        return false;
    }
    const std::int64_t a = next_attempt_;

    // Prefix sums of r[i] conj(r[i+N]) and of the mean energy of the pair.
    const std::size_t count = span - n;
    std::vector<C64> corr(count + 1);
    std::vector<double> energy(count + 1);
    for (std::size_t i = 0; i < count; ++i) {
        const Complex32 x = sample_at(a + static_cast<std::int64_t>(i));
        const Complex32 y = sample_at(a + static_cast<std::int64_t>(i + n));
        const C64 xd(x.real(), x.imag());
        const C64 yd(y.real(), y.imag());
        corr[i + 1] = corr[i] + xd * std::conj(yd);
        energy[i + 1] = energy[i] + 0.5 * (std::norm(xd) + std::norm(yd));
    }
    double best = -1.0;
    std::size_t best_n = 0;
    C64 best_gamma;
    for (std::size_t off = 0; off < len; ++off) {
        C64 gamma;
        double phi = 0.0;
        for (std::size_t s = 0; s < m; ++s) {
            const std::size_t lo = off + s * len;
            gamma += corr[lo + cp] - corr[lo];
            phi += energy[lo + cp] - energy[lo];
        }
        const double metric = phi > 0.0 ? std::abs(gamma) / phi : 0.0;
        if (metric > best) {
            best = metric;
            best_n = off;
            best_gamma = gamma;
        }
    }
    sync_.timing_metric = best;
    if (best < config_.timing_threshold) {
        next_attempt_ += static_cast<std::int64_t>(len);
        return false;
    }

    // r[i] conj(r[i+N]) carries exp(-j 2 pi eps) for an offset of eps carriers.
    const double eps = -std::arg(best_gamma) / (2.0 * std::numbers::pi);
    const std::int64_t s0 = a + static_cast<std::int64_t>(best_n);

    // The window may open before the transmission does. Symbols of silence or
    // bare noise ahead of it would hand the channel estimate pilots of nothing
    // for the first real symbols to interpolate from, so if the leading
    // symbols are far weaker than the strongest, try again from the first
    // strong one. The retry lands on an absolute position, so it is the same
    // however the stream was split.
    {
        std::vector<double> power(m);
        double peak = 0.0;
        for (std::size_t s = 0; s < m; ++s) {
            const std::size_t lo = best_n + s * len;
            for (std::size_t i = 0; i < len; ++i) {
                power[s] += std::norm(sample_at(a + static_cast<std::int64_t>(lo + i)));
            }
            peak = std::max(peak, power[s]);
        }
        std::size_t lead = 0;
        while (lead < m && power[lead] < 0.25 * peak) {
            ++lead;
        }
        if (lead > 0) {
            next_attempt_ = s0 + static_cast<std::int64_t>(lead * len);
            return false;
        }
    }

    std::vector<std::vector<Complex32>> symbols(m);
    for (std::size_t s = 0; s < m; ++s) {
        fft_at(s0 + static_cast<std::int64_t>(s * len), eps, symbols[s]);
    }

    const std::size_t period = config_.pilot_table.size();
    double best_pilot = -1.0;
    int best_q = 0;
    std::size_t best_p = 0;
    for (int q = -config_.max_integer_cfo; q <= config_.max_integer_cfo; ++q) {
        for (std::size_t p = 0; p < period; ++p) {
            double num = 0.0;
            double den = 0.0;
            for (std::size_t s = 0; s < m; ++s) {
                const auto& row = config_.pilot_table[(p + s) % period];
                const auto& bins = symbols[s];
                C64 acc;
                for (std::size_t i = 0; i + 1 < row.size(); ++i) {
                    const Complex32 y1 = bins[bin_index(row[i].carrier + q, n)];
                    const Complex32 y2 = bins[bin_index(row[i + 1].carrier + q, n)];
                    const C64 z1 = C64(y1.real(), y1.imag()) /
                                   C64(row[i].value.real(), row[i].value.imag());
                    const C64 z2 = C64(y2.real(), y2.imag()) /
                                   C64(row[i + 1].value.real(), row[i + 1].value.imag());
                    acc += z1 * std::conj(z2);
                    den += std::abs(z1) * std::abs(z2);
                }
                num += std::abs(acc);
            }
            const double metric = den > 0.0 ? num / den : 0.0;
            if (metric > best_pilot) {
                best_pilot = metric;
                best_q = q;
                best_p = p;
            }
        }
    }
    sync_.pilot_metric = best_pilot;
    if (best_pilot < config_.pilot_threshold) {
        next_attempt_ += static_cast<std::int64_t>(len);
        return false;
    }

    sync_.locked = true;
    sync_.first_symbol_sample = s0;
    sync_.cfo_carriers = eps + static_cast<double>(best_q);
    sync_.cfo_hz = sync_.cfo_carriers * static_cast<double>(config_.sample_rate) /
                   static_cast<double>(n);
    phase0_ = best_p;
    next_demod_ = 0;
    next_emit_ = 0;
    return true;
}

void OfdmReceiver::demodulate_available()
{
    const std::size_t n = config_.fft_size;
    const std::size_t len = n + config_.cp_length;
    const std::size_t period = config_.pilot_table.size();
    std::vector<Complex32> bins;
    while (true) {
        const std::int64_t start =
            sync_.first_symbol_sample + static_cast<std::int64_t>(next_demod_ * len);
        if (start + static_cast<std::int64_t>(len) > total_) {
            return;
        }
        fft_at(start, sync_.cfo_carriers, bins);
        Demodulated d;
        d.index = next_demod_;
        d.phase = (phase0_ + next_demod_) % period;
        d.start = start;
        const std::size_t nc = config_.carriers.size();
        d.values.resize(nc);
        d.pilot_ls.assign(nc, Complex32{});
        d.is_pilot.assign(nc, 0);
        d.pilot_power.assign(nc, 1.0);
        for (std::size_t c = 0; c < nc; ++c) {
            d.values[c] = bins[bin_index(config_.carriers[c], n)];
        }
        for (const auto& pilot : config_.pilot_table[d.phase]) {
            for (std::size_t c = 0; c < nc; ++c) {
                if (config_.carriers[c] == pilot.carrier) {
                    d.pilot_ls[c] = d.values[c] / pilot.value;
                    d.is_pilot[c] = 1;
                    d.pilot_power[c] = std::norm(pilot.value);
                    break;
                }
            }
        }
        window_.push_back(std::move(d));
        ++next_demod_;
    }
}

void OfdmReceiver::emit_ready(std::vector<OfdmSymbol>& out)
{
    const std::size_t nc = config_.carriers.size();
    std::vector<std::size_t> order(nc);
    for (std::size_t i = 0; i < nc; ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
        return config_.carriers[a] < config_.carriers[b];
    });

    while (next_demod_ > next_emit_ + lookahead_) {
        // window_ holds symbols from window_.front().index onwards.
        const std::uint64_t base = window_.front().index;
        const std::size_t at = static_cast<std::size_t>(next_emit_ - base);
        const Demodulated& cur = window_[at];

        // Time interpolation, carrier by carrier.
        std::vector<Complex32> h(nc);
        std::vector<std::uint8_t> have(nc, 0);
        for (std::size_t c = 0; c < nc; ++c) {
            std::ptrdiff_t before = -1;
            std::ptrdiff_t after = -1;
            for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(at); i >= 0; --i) {
                if (window_[static_cast<std::size_t>(i)].is_pilot[c] != 0) {
                    before = i;
                    break;
                }
            }
            for (std::size_t i = at; i < window_.size(); ++i) {
                if (window_[i].is_pilot[c] != 0) {
                    after = static_cast<std::ptrdiff_t>(i);
                    break;
                }
            }
            if (before >= 0 && after >= 0) {
                const Complex32 hb = window_[static_cast<std::size_t>(before)].pilot_ls[c];
                const Complex32 ha = window_[static_cast<std::size_t>(after)].pilot_ls[c];
                if (before == after) {
                    h[c] = hb;
                } else {
                    const float w = static_cast<float>(static_cast<double>(
                                                           static_cast<std::ptrdiff_t>(at) - before) /
                                                       static_cast<double>(after - before));
                    h[c] = hb + (ha - hb) * w;
                }
                have[c] = 1;
            } else if (before >= 0 || after >= 0) {
                h[c] = window_[static_cast<std::size_t>(before >= 0 ? before : after)].pilot_ls[c];
                have[c] = 1;
            }
        }

        // Frequency interpolation across carriers with no estimate yet.
        std::ptrdiff_t prev = -1;
        for (std::size_t r = 0; r < nc; ++r) {
            const std::size_t c = order[r];
            if (have[c] == 0) {
                continue;
            }
            const std::size_t lo = prev < 0 ? 0 : static_cast<std::size_t>(prev) + 1;
            for (std::size_t g = lo; g < r; ++g) {
                const std::size_t gc = order[g];
                if (prev < 0) {
                    h[gc] = h[c];
                } else {
                    const std::size_t pc = order[static_cast<std::size_t>(prev)];
                    const double span = config_.carriers[c] - config_.carriers[pc];
                    const float w = static_cast<float>((config_.carriers[gc] - config_.carriers[pc]) / span);
                    h[gc] = h[pc] + (h[c] - h[pc]) * w;
                }
            }
            prev = static_cast<std::ptrdiff_t>(r);
        }
        if (prev >= 0) {
            const std::size_t pc = order[static_cast<std::size_t>(prev)];
            for (std::size_t g = static_cast<std::size_t>(prev) + 1; g < nc; ++g) {
                h[order[g]] = h[pc];
            }
        }

        // Noise from this symbol's pilots, each against the line through the
        // pilots on the same carrier before and after it, scaled by that
        // difference's noise gain. Along time a static or slowly fading
        // channel is nearly constant, where along frequency an echo of a
        // fraction of the prefix already turns it through a radian or more
        // between pilots, which would read as noise.
        {
            double sum = 0.0;
            std::size_t used = 0;
            for (std::size_t c = 0; c < nc; ++c) {
                if (cur.is_pilot[c] == 0) {
                    continue;
                }
                std::ptrdiff_t before = -1;
                std::ptrdiff_t after = -1;
                for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(at) - 1; i >= 0; --i) {
                    if (window_[static_cast<std::size_t>(i)].is_pilot[c] != 0) {
                        before = i;
                        break;
                    }
                }
                for (std::size_t i = at + 1; i < window_.size(); ++i) {
                    if (window_[i].is_pilot[c] != 0) {
                        after = static_cast<std::ptrdiff_t>(i);
                        break;
                    }
                }
                // At the start of the stream there is no pilot behind; the
                // one ahead alone still gives a difference of known gain.
                if (after < 0) {
                    continue;
                }
                if (before < 0) {
                    before = after;
                }
                const auto& db = window_[static_cast<std::size_t>(before)];
                const auto& da = window_[static_cast<std::size_t>(after)];
                const double w2 =
                    before == after ? 1.0
                                    : static_cast<double>(static_cast<std::ptrdiff_t>(at) - before) /
                                          static_cast<double>(after - before);
                const double w0 = 1.0 - w2;
                const C64 h0(db.pilot_ls[c].real(), db.pilot_ls[c].imag());
                const C64 h1(cur.pilot_ls[c].real(), cur.pilot_ls[c].imag());
                const C64 h2(da.pilot_ls[c].real(), da.pilot_ls[c].imag());
                const C64 e = h1 - (w0 * h0 + w2 * h2);
                const double gain = 1.0 / cur.pilot_power[c] + w0 * w0 / db.pilot_power[c] +
                                    w2 * w2 / da.pilot_power[c];
                sum += std::norm(e) / gain;
                ++used;
            }
            if (used > 0) {
                const double est = sum / static_cast<double>(used);
                ++noise_updates_;
                const double alpha =
                    noise_updates_ < 16 ? 1.0 / static_cast<double>(noise_updates_) : 1.0 / 16.0;
                noise_variance_ += alpha * (est - noise_variance_);
            }
        }

        OfdmSymbol sym;
        sym.index = cur.index;
        sym.pilot_phase = cur.phase;
        sym.start_sample = cur.start;
        sym.channel = h;
        sym.equalised.resize(nc);
        sym.snr.resize(nc);
        sym.noise_variance = noise_variance_;
        const double floor = std::max(noise_variance_, 1e-30);
        for (std::size_t c = 0; c < nc; ++c) {
            const float mag = std::norm(h[c]);
            sym.equalised[c] = mag > 0.0F ? cur.values[c] / h[c] : Complex32{};
            sym.snr[c] = static_cast<float>(static_cast<double>(mag) / floor);
        }
        out.push_back(std::move(sym));
        ++next_emit_;

        // Keep lookahead_ symbols behind the next one to emit for the
        // backward half of the interpolation.
        while (!window_.empty() && window_.front().index + lookahead_ < next_emit_) {
            window_.erase(window_.begin());
        }
    }
}

void OfdmReceiver::trim()
{
    std::int64_t keep = next_attempt_;
    if (sync_.locked) {
        const std::size_t len = config_.fft_size + config_.cp_length;
        keep = sync_.first_symbol_sample + static_cast<std::int64_t>(next_demod_ * len);
    }
    keep = std::min(keep, total_);
    if (keep > buffer_start_) {
        const auto drop = static_cast<std::size_t>(keep - buffer_start_);
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(drop));
        buffer_start_ = keep;
    }
}

void OfdmReceiver::push(ConstComplexSpan samples, std::vector<OfdmSymbol>& out)
{
    buffer_.insert(buffer_.end(), samples.begin(), samples.end());
    total_ += static_cast<std::int64_t>(samples.size());
    while (!sync_.locked) {
        const std::int64_t before = next_attempt_;
        if (try_acquire()) {
            break;
        }
        if (next_attempt_ == before) {
            break;
        }
    }
    if (sync_.locked) {
        demodulate_available();
        emit_ready(out);
    }
    trim();
}

}  // namespace revenant::decode
