// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Locke Werks
#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

namespace lwdsp {

using cf32 = std::complex<float>;
using IqBlock = std::vector<cf32>;
using AudioBlock = std::vector<float>;

inline constexpr float kPi = std::numbers::pi_v<float>;
inline constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;

// Wideband IQ rate and audio rate. The receiver decimates by an integer
// factor, so wideband_hz must be an integer multiple of audio_hz.
struct SampleRates {
    double wideband_hz = 48000.0;
    double audio_hz = 48000.0;

    [[nodiscard]] int decimation() const noexcept {
        return static_cast<int>(wideband_hz / audio_hz + 0.5);
    }
};

enum class Mode : std::uint8_t { AM, USB, LSB, CW, FSK };

// db_to_lin and lin_to_db are amplitude ratios; power_to_db is power.
inline float db_to_lin(float db) noexcept { return std::pow(10.0f, db / 20.0f); }
inline float lin_to_db(float lin) noexcept { return 20.0f * std::log10(std::fmax(lin, 1e-20f)); }
inline float power_to_db(float p) noexcept { return 10.0f * std::log10(std::fmax(p, 1e-30f)); }
} // namespace lwdsp
