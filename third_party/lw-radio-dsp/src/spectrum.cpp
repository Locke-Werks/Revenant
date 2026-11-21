// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Locke Werks
#include "lwdsp/spectrum.h"

#include "fft.h"

#include <algorithm>

namespace lwdsp {

struct Spectrum::Impl {
    explicit Impl(const SpectrumConfig& c)
        : fft(static_cast<std::size_t>(c.size)), window(make_window(c.window, c.size)) {}
    detail::Fft fft;
    std::vector<float> window;
    std::vector<cf32> scratch;
    std::vector<float> power;
    std::vector<float> avg;
};

Spectrum::Spectrum(const SpectrumConfig& c) { configure(c); }
Spectrum::~Spectrum() = default;
Spectrum::Spectrum(Spectrum&&) noexcept = default;
Spectrum& Spectrum::operator=(Spectrum&&) noexcept = default;

void Spectrum::configure(const SpectrumConfig& c) {
    cfg_ = c;
    cfg_.size = std::clamp(c.size, 16, 1 << 16);
    impl_ = std::make_unique<Impl>(cfg_);
}

void Spectrum::compute(std::span<const cf32> iq, std::span<float> db_out) {
    auto& m = *impl_;
    detail::power_spectrum(m.fft, m.window, iq, m.scratch, m.power);
    const float a = std::clamp(cfg_.averaging, 0.0f, 0.999f);
    if (a > 0.0f) {
        if (m.avg.size() != m.power.size()) m.avg = m.power;
        for (std::size_t i = 0; i < m.power.size(); ++i) m.avg[i] = a * m.avg[i] + (1.0f - a) * m.power[i];
    } else {
        m.avg = m.power;
    }
    const std::size_t n = std::min(db_out.size(), m.avg.size());
    for (std::size_t i = 0; i < n; ++i) db_out[i] = power_to_db(m.avg[i]);
}

std::vector<float> Spectrum::compute(std::span<const cf32> iq) {
    std::vector<float> v(static_cast<std::size_t>(cfg_.size));
    compute(iq, v);
    return v;
}

double Spectrum::bin_hz(int i, double fs) const noexcept {
    return static_cast<double>(i - cfg_.size / 2) * fs / static_cast<double>(cfg_.size);
}

} // namespace lwdsp
