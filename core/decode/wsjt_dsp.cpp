#include "core/decode/wsjt_dsp.h"

#include <chrono>
#include <cmath>
#include <numbers>
#include <string>

namespace revenant::decode {

namespace {

using C = std::complex<double>;

std::vector<std::size_t> factorise(std::size_t n) {
    std::vector<std::size_t> f;
    // Radix 4 first: one pass of four costs less than two passes of two.
    while (n % 4 == 0) {
        f.push_back(4);
        n /= 4;
    }
    for (std::size_t p = 2; p * p <= n; ++p) {
        while (n % p == 0) {
            f.push_back(p);
            n /= p;
        }
    }
    if (n > 1) {
        f.push_back(n);
    }
    return f;
}

// Decimation in time: out[0..n) is the DFT of in[0], in[stride], ...
// twiddle[j] = exp(-+ 2 pi i j / total) for the full length `total`.
void fft_rec(const C* in, std::size_t stride, C* out, std::size_t n, const std::size_t* factors,
             const std::vector<C>& twiddle, std::size_t total, std::vector<C>& scratch) {
    if (n == 1) {
        out[0] = in[0];
        return;
    }
    const std::size_t p = factors[0];
    const std::size_t m = n / p;
    for (std::size_t q = 0; q < p; ++q) {
        fft_rec(in + q * stride, stride * p, out + q * m, m, factors + 1, twiddle, total, scratch);
    }
    // Twiddle step for this level: W_n^j = twiddle[j * total / n].
    const std::size_t step = total / n;
    C local[64];
    std::vector<C> big;
    C* t = local;
    if (p > 64) {
        big.resize(p);
        t = big.data();
    }
    for (std::size_t k = 0; k < m; ++k) {
        for (std::size_t q = 0; q < p; ++q) {
            t[q] = out[q * m + k] * twiddle[(q * k * step) % total];
        }
        for (std::size_t r = 0; r < p; ++r) {
            C sum = 0.0;
            for (std::size_t q = 0; q < p; ++q) {
                sum += t[q] * twiddle[((q * r * m) % n) * step];
            }
            scratch[r] = sum;
        }
        for (std::size_t r = 0; r < p; ++r) {
            out[r * m + k] = scratch[r];
        }
    }
}

}  // namespace

void wsjt_fft(std::vector<C>& data, bool inverse) {
    const std::size_t n = data.size();
    if (n <= 1) {
        return;
    }
    const auto factors = factorise(n);
    std::vector<C> twiddle(n);
    const double sign = inverse ? 1.0 : -1.0;
    for (std::size_t j = 0; j < n; ++j) {
        const double a = sign * 2.0 * std::numbers::pi * static_cast<double>(j) /
                         static_cast<double>(n);
        twiddle[j] = C(std::cos(a), std::sin(a));
    }
    std::size_t largest = 0;
    for (const auto f : factors) {
        largest = std::max(largest, f);
    }
    std::vector<C> scratch(largest);
    std::vector<C> out(n);
    fft_rec(data.data(), 1, out.data(), n, factors.data(), twiddle, n, scratch);
    if (inverse) {
        const double s = 1.0 / static_cast<double>(n);
        for (auto& v : out) {
            v *= s;
        }
    }
    data.swap(out);
}

Expected<WsjtInputStage> WsjtInputStage::create(SampleRate input_rate) {
    if (input_rate < kWsjtRate || input_rate % kWsjtRate != 0) {
        return fail("weak-signal decoders take audio at a whole multiple of 12 kHz, not " +
                    std::to_string(input_rate) + " Hz");
    }
    WsjtInputStage s;
    s.factor_ = static_cast<int>(input_rate / kWsjtRate);
    if (s.factor_ > 1) {
        // Windowed sinc, cut off at 5 kHz with the stop band from 6 kHz: every
        // mode served here lives below 4 kHz. Blackman window. Not specified
        // by any mode document.
        const int taps = 24 * s.factor_ + 1;
        const double fc = 5000.0 / static_cast<double>(input_rate);
        s.taps_.resize(static_cast<std::size_t>(taps));
        double sum = 0.0;
        for (int i = 0; i < taps; ++i) {
            const double x = i - (taps - 1) / 2.0;
            const double sinc = x == 0.0 ? 2.0 * fc
                                         : std::sin(2.0 * std::numbers::pi * fc * x) /
                                               (std::numbers::pi * x);
            const double w = 0.42 - 0.5 * std::cos(2.0 * std::numbers::pi * i / (taps - 1)) +
                             0.08 * std::cos(4.0 * std::numbers::pi * i / (taps - 1));
            s.taps_[static_cast<std::size_t>(i)] = static_cast<float>(sinc * w);
            sum += sinc * w;
        }
        for (auto& t : s.taps_) {
            t = static_cast<float>(t / sum);
        }
        s.history_.assign(s.taps_.size() - 1, 0.0F);
    }
    return s;
}

void WsjtInputStage::process(ConstRealSpan audio, std::vector<float>& out) {
    if (factor_ == 1) {
        out.insert(out.end(), audio.begin(), audio.end());
        return;
    }
    const std::size_t nt = taps_.size();
    for (const float x : audio) {
        history_.push_back(x);
        if (++phase_ == factor_) {
            phase_ = 0;
            const float* h = history_.data() + history_.size() - nt;
            double acc = 0.0;
            for (std::size_t i = 0; i < nt; ++i) {
                acc += static_cast<double>(taps_[i]) * h[i];
            }
            out.push_back(static_cast<float>(acc));
        }
        if (history_.size() > 4 * nt) {
            history_.erase(history_.begin(),
                           history_.begin() + static_cast<std::ptrdiff_t>(history_.size() - nt));
        }
    }
}

WsjtUtcOfSample wsjt_wall_clock() {
    return [](SampleIndex) {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration<double>(now).count();
    };
}

WsjtSlotClock::WsjtSlotClock(double slot_seconds, WsjtUtcOfSample utc_of_sample,
                             SampleRate input_rate)
    : slot_seconds_(slot_seconds),
      utc_of_sample_(utc_of_sample ? std::move(utc_of_sample) : wsjt_wall_clock()),
      input_rate_(input_rate),
      slot_samples_(static_cast<std::int64_t>(std::llround(slot_seconds * kWsjtRate))) {}

void WsjtSlotClock::settle() {
    if (settled_) {
        return;
    }
    settled_ = true;
    const double t0 = utc_of_sample_(0);
    const double k = std::ceil(t0 / slot_seconds_ - 1e-9);
    first_boundary_utc_ = k * slot_seconds_;
    first_boundary_ = static_cast<std::int64_t>(
        std::llround((first_boundary_utc_ - t0) * static_cast<double>(kWsjtRate)));
    (void)input_rate_;
}

std::int64_t WsjtSlotClock::first_boundary() {
    settle();
    return first_boundary_;
}

double WsjtSlotClock::first_boundary_utc() {
    settle();
    return first_boundary_utc_;
}

}  // namespace revenant::decode
