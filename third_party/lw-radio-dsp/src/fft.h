// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Locke Werks
//
// Thin wrapper over pocketfft so the rest of the library never includes it.
#pragma once

#include "lwdsp/types.h"

#include <memory>
#include <vector>

namespace lwdsp::detail {

class Fft {
public:
    explicit Fft(std::size_t n);
    ~Fft();
    Fft(Fft&&) noexcept;
    Fft& operator=(Fft&&) noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    // In place, forward, unnormalized.
    void forward(std::vector<cf32>& data);

private:
    struct Plan;
    std::size_t n_;
    std::unique_ptr<Plan> plan_;
};

// Windowed power spectrum, FFT-shifted (index 0 is -fs/2), normalized so a
// full-scale complex tone reads 1.0 in its bin. Uses the last n samples of
// iq and zero-pads the front if shorter.
void power_spectrum(Fft& fft, const std::vector<float>& window, std::span<const cf32> iq,
                    std::vector<cf32>& scratch, std::vector<float>& power_out);

} // namespace lwdsp::detail
