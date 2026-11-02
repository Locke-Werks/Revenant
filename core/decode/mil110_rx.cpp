#include "core/decode/mil110_rx.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/decode/dv_phy.h"

namespace revenant::decode::mil110_rx {

namespace {

constexpr double kPi = std::numbers::pi;

// Engineering choices for the loops, per symbol. The timing loop is critically
// damped: natural frequency sqrt(kTimingI), 2 * zeta * wn = kTimingP.
constexpr double kLoopP = 0.02;
constexpr double kLoopI = 2e-4;
constexpr double kTimingP = 0.01;
constexpr double kTimingI = 2.5e-5;

C rot(double ph) {
    return {static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph))};
}

// Cubic Lagrange weights for samples -1, 0, 1, 2 at fraction mu.
void lagrange(float mu, float w[4]) {
    w[0] = -mu * (mu - 1.0F) * (mu - 2.0F) / 6.0F;
    w[1] = (mu + 1.0F) * (mu - 1.0F) * (mu - 2.0F) / 2.0F;
    w[2] = -(mu + 1.0F) * mu * (mu - 2.0F) / 2.0F;
    w[3] = (mu + 1.0F) * mu * (mu - 1.0F) / 6.0F;
}

}  // namespace

// ---------------------------------------------------------------------------
// Front end
// ---------------------------------------------------------------------------

Status FrontEnd::init(dsp::SampleRate rate, double carrier_hz, double symbol_rate) {
    rate_ = rate;
    carrier_hz_ = carrier_hz;
    const double sps = static_cast<double>(rate) / symbol_rate;
    const auto taps = static_cast<std::size_t>(std::ceil(8.0 * sps)) | 1U;
    auto rrc = design_rrc(rate, symbol_rate, 0.35, taps);
    if (!rrc) {
        return std::unexpected(rrc.error());
    }
    rrc_ = std::move(*rrc);
    ratio_ = static_cast<double>(rate) / (kSps * symbol_rate);
    reset();
    return {};
}

void FrontEnd::reset() {
    hist_.assign(rrc_.size(), C{});
    hist_pos_ = 0;
    in_count_ = 0;
    for (auto& v : interp_) {
        v = {};
    }
    filt_count_ = 0;
    out_count_ = 0;
    bb_.clear();
    base_ = 0;
}

void FrontEnd::push(dsp::ConstRealSpan audio) {
    const double w = 2.0 * kPi * carrier_hz_ / static_cast<double>(rate_);
    for (const float a : audio) {
        // The phase from the count modulo the rate, so it stays exact.
        const double ph = w * static_cast<double>(in_count_ % rate_);
        ++in_count_;
        hist_[hist_pos_] =
            C{static_cast<float>(a * std::cos(ph)), static_cast<float>(-a * std::sin(ph))};
        C y{};
        std::size_t idx = hist_pos_;
        for (const float t : rrc_) {
            y += t * hist_[idx];
            idx = idx == 0 ? hist_.size() - 1 : idx - 1;
        }
        hist_pos_ = (hist_pos_ + 1) % hist_.size();

        // Cubic Lagrange interpolation onto the baseband grid over the last
        // four filtered samples, filt_count_ - 3 .. filt_count_. Every output
        // whose four neighbours have arrived is made as soon as the newest
        // arrives, whichever way the rates compare.
        interp_[0] = interp_[1];
        interp_[1] = interp_[2];
        interp_[2] = interp_[3];
        interp_[3] = y;
        const std::int64_t newest = filt_count_++;
        for (;;) {
            const double t = static_cast<double>(out_count_) * ratio_;
            const auto b = static_cast<std::int64_t>(std::floor(t));
            if (b + 2 != newest) {
                break;
            }
            float wt[4];
            lagrange(static_cast<float>(t - static_cast<double>(b)), wt);
            bb_.push_back(wt[0] * interp_[0] + wt[1] * interp_[1] + wt[2] * interp_[2] +
                          wt[3] * interp_[3]);
            ++out_count_;
        }
    }
}

C FrontEnd::at(double position) const {
    const auto i = static_cast<std::int64_t>(std::floor(position));
    float w[4];
    lagrange(static_cast<float>(position - static_cast<double>(i)), w);
    return w[0] * at(i - 1) + w[1] * at(i) + w[2] * at(i + 1) + w[3] * at(i + 2);
}

void FrontEnd::discard_before(std::int64_t index) {
    if (index > base_ + 65536) {
        const auto drop = static_cast<std::size_t>(
            std::min<std::int64_t>(index - base_, static_cast<std::int64_t>(bb_.size())));
        bb_.erase(bb_.begin(), bb_.begin() + static_cast<std::ptrdiff_t>(drop));
        base_ += static_cast<std::int64_t>(drop);
    }
}

std::int64_t FrontEnd::to_input(double index) const {
    return static_cast<std::int64_t>(std::llround(index * ratio_)) - delay();
}

// ---------------------------------------------------------------------------
// Equaliser
// ---------------------------------------------------------------------------

void Equaliser::start(std::size_t ff_symbols, std::size_t fb_taps, C centre_gain) {
    ff_symbols_ = ff_symbols;
    ff_.assign(4 * ff_symbols + 1, C{});
    ff_[2 * ff_symbols] = centre_gain;
    fb_.assign(fb_taps, C{});
    past_.assign(fb_taps, C{});
    theta_ = 0.0;
    nu_ = 0.0;
    tau_ = 0.0;
    rate_ = 0.0;
    rate_mean_ = 0.0;
    dy2_ = 0.0;
    sigma2_ = 0.1;
    timing_ = false;
    mark_centre();
}

std::int64_t Equaliser::reach(std::int64_t origin, std::int64_t n) const {
    return static_cast<std::int64_t>(std::floor(centre(origin, n))) +
           4 * static_cast<std::int64_t>(ff_symbols_) + 4;
}

std::int64_t Equaliser::earliest(std::int64_t origin, std::int64_t n) const {
    return static_cast<std::int64_t>(std::floor(centre(origin, n))) -
           4 * static_cast<std::int64_t>(ff_symbols_) - 3;
}

C Equaliser::output(const FrontEnd& fe, std::int64_t origin, double omega, std::int64_t n,
                    Snapshot& s) const {
    s.x.resize(ff_.size());
    s.dx.resize(ff_.size());
    s.theta = theta_;
    const double first = centre(origin, n) - 4.0 * static_cast<double>(ff_symbols_);
    C y{};
    for (std::size_t k = 0; k < ff_.size(); ++k) {
        const double p = first + 2.0 * static_cast<double>(k);
        const C r = rot(-omega * (p - static_cast<double>(origin)));
        s.x[k] = fe.at(p) * r;
        s.dx[k] = 0.5F * (fe.at(p + 1.0) - fe.at(p - 1.0)) * r;
        y += ff_[k] * s.x[k];
    }
    y *= rot(-theta_);
    for (std::size_t i = 0; i < fb_.size(); ++i) {
        y -= fb_[i] * past_[i];
    }
    return y;
}

void Equaliser::adapt(const Snapshot& s, C y, C d, double mu, bool timing) {
    const C e = d - y;
    double p = 1e-6;
    for (const auto& v : s.x) {
        p += std::norm(v);
    }
    for (const auto& v : past_) {
        p += std::norm(v);
    }
    if (timing && timing_) {
        // dy/dtau through the feedforward taps, at the phase the output used.
        C dyff{};
        for (std::size_t k = 0; k < ff_.size(); ++k) {
            dyff += ff_[k] * s.dx[k];
        }
        const C dy = dyff * rot(-s.theta);
        const double m = std::norm(dy);
        dy2_ = dy2_ == 0.0 ? m : 0.99 * dy2_ + 0.01 * m;
        if (dy2_ > 0.0) {
            const double step =
                std::clamp(static_cast<double>((std::conj(dy) * e).real()) / dy2_, -1.0, 1.0);
            tau_ += kTimingP * step;
            rate_ += kTimingI * step;
        }
    }
    const auto g = static_cast<float>(mu / p);
    const C er = e * rot(s.theta);
    for (std::size_t k = 0; k < ff_.size(); ++k) {
        ff_[k] += g * er * std::conj(s.x[k]);
    }
    for (std::size_t i = 0; i < fb_.size(); ++i) {
        fb_[i] -= g * e * std::conj(past_[i]);
    }
}

void Equaliser::loop(C y, C d) {
    loop_angle(std::arg(std::complex<double>(y * std::conj(d))), 1.0);
}

void Equaliser::loop_angle(double err, double weight) {
    theta_ += weight * kLoopP * err;
    nu_ += weight * kLoopI * err;
}

void Equaliser::push_feedback(C d) {
    if (past_.empty()) {
        return;
    }
    std::rotate(past_.rbegin(), past_.rbegin() + 1, past_.rend());
    past_[0] = d;
}

void Equaliser::advance() {
    theta_ += nu_;
    tau_ += rate_;
    if (timing_) {
        rate_mean_ += 1e-4 * (rate_ - rate_mean_);
    }
}

bool Equaliser::solve(const FrontEnd& fe, std::int64_t origin, double omega, std::int64_t first,
                      const std::vector<C>& known) {
    using D = std::complex<double>;
    const std::size_t nf = ff_.size();
    const std::size_t nb = fb_.size();
    const std::size_t n = nf + nb;
    std::vector<D> r(n * n, D{});
    std::vector<D> b(n, D{});
    std::vector<D> a(n);
    Snapshot s;
    const double saved = theta_;
    theta_ = 0.0;
    for (std::size_t m = nb; m < known.size(); ++m) {
        (void)output(fe, origin, omega, first + static_cast<std::int64_t>(m), s);
        for (std::size_t k = 0; k < nf; ++k) {
            a[k] = D(s.x[k]);
        }
        for (std::size_t i = 0; i < nb; ++i) {
            a[nf + i] = -D(known[m - 1 - i]);
        }
        const D d(known[m]);
        for (std::size_t i = 0; i < n; ++i) {
            const D ci = std::conj(a[i]);
            for (std::size_t j = 0; j < n; ++j) {
                r[i * n + j] += ci * a[j];
            }
            b[i] += ci * d;
        }
    }
    theta_ = saved;
    double trace = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        trace += r[i * n + i].real();
    }
    // Diagonal loading, an engineering choice: enough to keep the solve
    // stable when a tap sees almost no energy, small against the signal.
    const double load = 1e-3 * trace / static_cast<double>(n) + 1e-12;
    for (std::size_t i = 0; i < n; ++i) {
        r[i * n + i] += load;
    }
    // Gaussian elimination with partial pivoting.
    for (std::size_t c = 0; c < n; ++c) {
        std::size_t piv = c;
        for (std::size_t i = c + 1; i < n; ++i) {
            if (std::abs(r[i * n + c]) > std::abs(r[piv * n + c])) {
                piv = i;
            }
        }
        if (std::abs(r[piv * n + c]) < 1e-15) {
            return false;
        }
        if (piv != c) {
            for (std::size_t j = 0; j < n; ++j) {
                std::swap(r[c * n + j], r[piv * n + j]);
            }
            std::swap(b[c], b[piv]);
        }
        for (std::size_t i = c + 1; i < n; ++i) {
            const D f = r[i * n + c] / r[c * n + c];
            if (f == D{}) {
                continue;
            }
            for (std::size_t j = c; j < n; ++j) {
                r[i * n + j] -= f * r[c * n + j];
            }
            b[i] -= f * b[c];
        }
    }
    std::vector<D> w(n);
    for (std::size_t i = n; i-- > 0;) {
        D acc = b[i];
        for (std::size_t j = i + 1; j < n; ++j) {
            acc -= r[i * n + j] * w[j];
        }
        w[i] = acc / r[i * n + i];
    }
    for (std::size_t k = 0; k < nf; ++k) {
        ff_[k] = C(w[k]);
    }
    for (std::size_t i = 0; i < nb; ++i) {
        fb_[i] = C(w[nf + i]);
    }
    theta_ = 0.0;
    for (std::size_t i = 0; i < nb; ++i) {
        past_[i] = known[known.size() - 1 - i];
    }
    return true;
}

double Equaliser::centroid() const {
    double num = 0.0;
    double den = 0.0;
    for (std::size_t k = 0; k < ff_.size(); ++k) {
        const double e = std::norm(ff_[k]);
        num += e * static_cast<double>(k);
        den += e;
    }
    return den > 0.0 ? num / den : static_cast<double>(ff_.size() / 2);
}

void Equaliser::mark_centre() { centre_ref_ = centroid(); }

void Equaliser::recentre() {
    const double c = centroid() - centre_ref_;
    if (c > 1.0) {
        std::rotate(ff_.begin(), ff_.begin() + 1, ff_.end());
        ff_.back() = C{};
        tau_ += 2.0;
    } else if (c < -1.0) {
        std::rotate(ff_.rbegin(), ff_.rbegin() + 1, ff_.rend());
        ff_.front() = C{};
        tau_ -= 2.0;
    }
}

}  // namespace revenant::decode::mil110_rx
