// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Locke Werks
#pragma once

#include "lwdsp/primitives.h"
#include "lwdsp/types.h"

#include <memory>
#include <span>
#include <vector>

namespace lwdsp {

struct SpectrumConfig {
    int size = 4096;                  // 2048..8192, power of two not required
    Window window = Window::BlackmanHarris;
    float averaging = 0.0f;           // 0 = none, else exponential alpha on power
};

// Windowed FFT of the wideband block producing one waterfall row in dBFS.
// Bins are FFT-shifted: index 0 is -fs/2, index size/2 is DC.
// A full-scale complex tone reads 0 dBFS in its bin.
class Spectrum {
public:
    explicit Spectrum(const SpectrumConfig& cfg = {});
    ~Spectrum();
    Spectrum(Spectrum&&) noexcept;
    Spectrum& operator=(Spectrum&&) noexcept;

    void configure(const SpectrumConfig& cfg);
    [[nodiscard]] int size() const noexcept { return cfg_.size; }

    // Uses the last size() samples of iq (zero-pads if shorter).
    void compute(std::span<const cf32> iq, std::span<float> db_out);
    std::vector<float> compute(std::span<const cf32> iq);

    // Frequency of bin i relative to centre.
    [[nodiscard]] double bin_hz(int i, double sample_rate) const noexcept;

private:
    struct Impl;
    SpectrumConfig cfg_;
    std::unique_ptr<Impl> impl_;
};

} // namespace lwdsp
