// CombSpectrumAverager from core/dsp/comb_spectrum.h.

#include "core/dsp/comb_spectrum.h"

#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

namespace revenant::dsp {

std::uint32_t comb_spectrum_points(SampleRate rate)
{
    if (rate <= 0) {
        return kCombSpectrumPoints;
    }
    const double want = static_cast<double>(rate) / kCombSpectrumBinHz;
    std::uint32_t points = 1024;
    while (points < kCombSpectrumPoints && static_cast<double>(points) * 1.5 < want) {
        points *= 2;
    }
    return points;
}

CombSpectrumAverager::CombSpectrumAverager(std::uint32_t points)
    : points_(std::has_single_bit(points) && points >= 4 ? points : kCombSpectrumPoints)
{
    const std::uint32_t n = points_;
    const int bits = std::countr_zero(n);
    window_.resize(n);
    reversed_.resize(n);
    twiddles_.resize(n / 2);
    work_.resize(n);
    sum_.assign(n, 0.0);
    for (std::uint32_t i = 0; i < n; ++i) {
        window_[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / n);
        std::uint32_t r = 0;
        for (int b = 0; b < bits; ++b) {
            r |= ((i >> b) & 1U) << (bits - 1 - b);
        }
        reversed_[i] = r;
    }
    for (std::uint32_t k = 0; k < n / 2; ++k) {
        twiddles_[k] = std::polar(1.0, -2.0 * std::numbers::pi * k / n);
    }
}

void CombSpectrumAverager::add(std::span<const Complex32> frame)
{
    const std::uint32_t n = points_;
    if (frame.size() != n) {
        return;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        const Complex32 x = frame[i];
        work_[reversed_[i]] =
            std::complex<double>(x.real(), x.imag()) * window_[i];
    }
    for (std::uint32_t len = 2; len <= n; len <<= 1) {
        const std::uint32_t half = len / 2;
        const std::uint32_t stride = n / len;
        for (std::uint32_t base = 0; base < n; base += len) {
            for (std::uint32_t j = 0; j < half; ++j) {
                const std::complex<double> t = work_[base + j + half] * twiddles_[j * stride];
                const std::complex<double> u = work_[base + j];
                work_[base + j] = u + t;
                work_[base + j + half] = u - t;
            }
        }
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        sum_[i] += std::norm(work_[i]);
    }
    ++frames_;
}

std::vector<float> CombSpectrumAverager::take()
{
    const std::uint32_t n = points_;
    std::vector<float> out(n, 0.0F);
    if (frames_ > 0) {
        const double scale = 1.0 / frames_;
        // Bin 0 is DC; natural order puts it at n / 2.
        for (std::uint32_t i = 0; i < n; ++i) {
            out[(i + n / 2) % n] = static_cast<float>(sum_[i] * scale);
        }
    }
    sum_.assign(n, 0.0);
    frames_ = 0;
    return out;
}

}  // namespace revenant::dsp
