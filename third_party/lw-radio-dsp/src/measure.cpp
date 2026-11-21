// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Locke Werks
//
// The median-floor noise estimate and parabolic peak interpolation follow
// Revenant core/dsp/carrier_tracker.cpp (textbook spectral estimation).
#include "lwdsp/measure.h"

#include "fft.h"
#include "lwdsp/primitives.h"

#include <algorithm>
#include <cmath>

namespace lwdsp {

PassbandReading measure_passband(std::span<const cf32> iq, double fs, double center_hz,
                                 double bandwidth_hz, int fft_size) {
    PassbandReading r;
    const auto n = static_cast<std::size_t>(std::max(fft_size, 64));
    detail::Fft fft(n);
    const auto window = make_window(Window::BlackmanHarris, static_cast<int>(n));
    std::vector<cf32> scratch;
    std::vector<float> power;
    detail::power_spectrum(fft, window, iq, scratch, power);

    double wsum = 0.0, w2 = 0.0;
    for (float w : window) {
        wsum += w;
        w2 += static_cast<double>(w) * w;
    }
    // Equivalent noise bandwidth in bins: converts window-scaled readings
    // back to true power for both tones and noise (Parseval).
    const double enbw = static_cast<double>(n) * w2 / (wsum * wsum);
    const double bin_hz = fs / static_cast<double>(n);

    std::vector<float> sorted = power;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(n / 2), sorted.end());
    // The median of an exponential distribution is ln 2 times its mean.
    const double noise_bin = static_cast<double>(sorted[n / 2]) / std::log(2.0) / enbw;

    const double lo = center_hz - 0.5 * bandwidth_hz;
    const double hi = center_hz + 0.5 * bandwidth_hz;
    const auto to_bin = [&](double hz) {
        return static_cast<long>(std::lround(hz / bin_hz)) + static_cast<long>(n / 2);
    };
    const long b0 = std::clamp(to_bin(lo), 0L, static_cast<long>(n) - 1);
    const long b1 = std::clamp(to_bin(hi), 0L, static_cast<long>(n) - 1);
    double sig = 0.0;
    long peak = b0;
    for (long b = b0; b <= b1; ++b) {
        sig += power[static_cast<std::size_t>(b)];
        if (power[static_cast<std::size_t>(b)] > power[static_cast<std::size_t>(peak)]) peak = b;
    }
    sig /= enbw;
    const double nbins = static_cast<double>(b1 - b0 + 1);
    const double noise = noise_bin * nbins;

    double frac = 0.0;
    if (peak > 0 && peak + 1 < static_cast<long>(n)) {
        const double a = power_to_db(power[static_cast<std::size_t>(peak - 1)]);
        const double b = power_to_db(power[static_cast<std::size_t>(peak)]);
        const double c = power_to_db(power[static_cast<std::size_t>(peak + 1)]);
        const double den = a - 2.0 * b + c;
        if (std::abs(den) > 1e-12) frac = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
    }

    r.signal_dbfs = power_to_db(static_cast<float>(sig));
    r.noise_dbfs = power_to_db(static_cast<float>(noise));
    const double excess = std::max(sig - noise, 0.0);
    r.snr_db = power_to_db(static_cast<float>(excess / std::max(noise, 1e-30)));
    r.peak_hz = static_cast<float>((static_cast<double>(peak - static_cast<long>(n / 2)) + frac) * bin_hz);
    return r;
}

namespace {

float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// How intelligible a signal of one mode is through a receiver set to
// another. AM through a sideband filter is exalted-carrier reception and
// works; SSB through an AM detector is the classic Donald Duck garble.
float mode_factor(Mode sig, Mode rx) {
    if (sig == rx) return 1.0f;
    switch (sig) {
    case Mode::AM: return (rx == Mode::USB || rx == Mode::LSB) ? 0.85f : 0.3f;
    case Mode::USB:
    case Mode::LSB: return (rx == Mode::AM) ? 0.15f : 0.0f;
    case Mode::CW: return (rx == Mode::USB || rx == Mode::LSB) ? 0.8f : 0.2f;
    case Mode::FSK: return (rx == Mode::USB || rx == Mode::LSB || rx == Mode::CW) ? 0.8f : 0.1f;
    }
    return 0.0f;
}

} // namespace

TuneQuality evaluate_tune(const TuneInput& in) {
    TuneQuality q;
    const bool ssb_rx = in.rx_mode == Mode::USB || in.rx_mode == Mode::LSB;
    const double reference = in.rx_vfo_hz + (ssb_rx ? in.rx_clarifier_hz : 0.0);
    q.offset_error_hz = static_cast<float>(reference - in.signal_offset_hz);
    q.snr_db = in.passband_snr_db;
    q.mode_match = in.signal_mode == in.rx_mode;

    // Gaussian falloff; sigma is where the signal is clearly wrong by ear.
    float sigma = 100.0f;
    switch (in.signal_mode) {
    case Mode::USB:
    case Mode::LSB: sigma = 90.0f; break; // voice pitch is obviously off past ~100 Hz
    case Mode::AM: sigma = std::max(800.0f, 0.3f * in.rx_bandwidth_hz); break;
    case Mode::CW: sigma = std::max(60.0f, 0.35f * in.rx_bandwidth_hz); break;
    case Mode::FSK: sigma = std::max(40.0f, 0.2f * in.rx_bandwidth_hz); break;
    }
    const float e = q.offset_error_hz / sigma;
    q.tune_score = std::exp(-0.5f * e * e);
    q.snr_score = smoothstep(0.0f, 20.0f, q.snr_db);
    q.quality = q.tune_score * q.snr_score * mode_factor(in.signal_mode, in.rx_mode);
    return q;
}

float word_reveal(const TuneQuality& q, float threshold_db) {
    const float clarity = q.quality > 0.0f && q.snr_score > 0.0f ? q.quality / q.snr_score : 0.0f;
    // Logistic over SNR margin, 3 dB from 12% to 88%.
    const float s = 1.0f / (1.0f + std::exp(-(q.snr_db - threshold_db) / 1.5f));
    return std::clamp(clarity * s, 0.0f, 1.0f);
}

} // namespace lwdsp
