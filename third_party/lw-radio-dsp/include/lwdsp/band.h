// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Locke Werks
//
// A slice of spectrum: N transmitters at their offsets plus a seeded noise
// model, summed into one wideband IQ block. Identical seed and identical call
// sequence give bit-identical output on every platform.
#pragma once

#include "lwdsp/modulators.h"
#include "lwdsp/primitives.h"
#include "lwdsp/types.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace lwdsp {

// Band noise texture. The floor is complex Gaussian at noise_floor_db total
// power (dBFS); the rest colours and animates it.
struct NoiseTexture {
    float noise_floor_db = -60.0f;  // total wideband noise power, dBFS
    float tilt_db = 0.0f;           // spectral tilt edge to edge; low bands slope
    float drift_db = 2.0f;          // slow random walk of the floor, dB peak
    float drift_rate_hz = 0.05f;    // how fast the floor wanders
    // Impulsive static crashes (lightning QRN).
    float crash_rate_hz = 0.3f;     // mean crashes per second, Poisson
    float crash_level_db = 25.0f;   // peak above the floor
    float crash_decay_ms = 60.0f;
    // Short clicks (local QRM, ignition) that the noise blanker targets.
    float impulse_rate_hz = 0.0f;
    float impulse_level_db = 30.0f;
};

// Per-signal fading. QSB is modelled as a smoothed random process on the
// amplitude, which reads as fading by ear without a full channel model.
struct Fading {
    float depth_db = 0.0f;   // 0 disables
    float rate_hz = 0.1f;    // fade rate
};

struct SignalParams {
    float level_db = 10.0f;  // carrier or peak envelope power relative to the floor, in the receiver's 3 kHz reference bandwidth
    Fading qsb{};
    bool enabled = true;
};

using SignalId = std::uint32_t;

struct BandConfig {
    double sample_rate = 48000.0;
    std::uint64_t seed = 1;
    NoiseTexture noise{};
};

class Band {
public:
    explicit Band(BandConfig cfg);

    SignalId add(std::shared_ptr<Source> src, SignalParams params = {});
    void remove(SignalId id);
    void set_params(SignalId id, const SignalParams& p);
    [[nodiscard]] SignalParams params(SignalId id) const;
    [[nodiscard]] Source* source(SignalId id) const;
    [[nodiscard]] std::vector<SignalId> ids() const;

    void set_noise(const NoiseTexture& n);
    [[nodiscard]] const NoiseTexture& noise() const noexcept { return cfg_.noise; }
    // Rewinds the noise model to the seed. Sources keep their own state.
    void reseed(std::uint64_t seed);

    // Overwrites out with the next block: noise plus every enabled signal.
    void render(std::span<cf32> out);
    // Noise only, same seed stream as render would use for the noise part.
    void render_noise(std::span<cf32> out);

    // Linear amplitude that puts a unit-envelope source at level_db over the
    // floor measured in ref_bw_hz. Used internally and by tests.
    [[nodiscard]] float level_to_amplitude(float level_db, double ref_bw_hz = 3000.0) const;
    // Current fading gain of a signal in dB, for debug overlays.
    [[nodiscard]] float fading_db(SignalId id) const;

    [[nodiscard]] double sample_rate() const noexcept { return cfg_.sample_rate; }

private:
    struct Entry;
    struct NoiseState;
    BandConfig cfg_;
    std::vector<std::unique_ptr<Entry>> entries_;
    std::unique_ptr<NoiseState> noise_;
    SignalId next_id_ = 1;
    std::vector<cf32> scratch_;
public:
    ~Band();
    Band(Band&&) noexcept;
    Band& operator=(Band&&) noexcept;
};

} // namespace lwdsp
