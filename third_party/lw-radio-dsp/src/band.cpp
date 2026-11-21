// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Locke Werks
//
// Per-stage seed domains via Rng::derive_seed follow the scheme in Revenant
// core/dsp/synth/channel.cpp: each stage owns a stream, so adding a signal
// or a noise component never shifts the numbers another stage draws.
#include "lwdsp/band.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace lwdsp {

namespace {

constexpr std::uint64_t kDomainFloor = 1;
constexpr std::uint64_t kDomainDrift = 2;
constexpr std::uint64_t kDomainCrash = 3;
constexpr std::uint64_t kDomainImpulse = 4;
constexpr std::uint64_t kDomainFadingBase = 0x1000;

// Slow modulators (drift, fading) update at this many samples. At 48 kHz
// that is 0.67 ms, far faster than anything they model.
constexpr int kControlSamples = 32;

// One-pole smoothed Gaussian scaled back to unit variance, so a depth knob
// means the same thing at every rate.
struct SlowNoise {
    Rng rng;
    float a = 0.0f;
    float y = 0.0f;
    float norm = 1.0f;
    void configure(float rate_hz, double control_rate) {
        a = static_cast<float>(1.0 - std::exp(-2.0 * 3.14159265358979 * rate_hz / control_rate));
        a = std::clamp(a, 1e-6f, 1.0f);
        norm = std::sqrt((2.0f - a) / a);
    }
    float next() {
        y += a * (rng.gaussian() - y);
        return y * norm;
    }
};

} // namespace

struct Band::Entry {
    SignalId id = 0;
    std::shared_ptr<Source> src;
    SignalParams params;
    SlowNoise fade;
    float gain_prev = 1.0f; // linear, start of current control block
    float gain_next = 1.0f;
    float fade_db = 0.0f;
    int phase = 0;
};

struct Band::NoiseState {
    Rng floor;
    Rng crash;
    Rng impulse;
    SlowNoise drift;
    cf32 tilt_prev{};
    float tilt_g = 0.0f;
    float tilt_norm = 1.0f;
    float sigma = 0.0f;          // per-component amplitude of the floor
    float drift_gain = 1.0f;
    float crash_env = 0.0f;
    float crash_decay = 0.0f;
    int phase = 0;
    int impulse_at = -1;
    float impulse_amp = 0.0f;
};

Band::Band(BandConfig cfg) : cfg_(cfg), noise_(std::make_unique<NoiseState>()) { reseed(cfg_.seed); }
Band::~Band() = default;
Band::Band(Band&&) noexcept = default;
Band& Band::operator=(Band&&) noexcept = default;

void Band::reseed(std::uint64_t seed) {
    cfg_.seed = seed;
    auto& n = *noise_;
    n = NoiseState{};
    n.floor.seed(Rng::derive_seed(seed, kDomainFloor));
    n.crash.seed(Rng::derive_seed(seed, kDomainCrash));
    n.impulse.seed(Rng::derive_seed(seed, kDomainImpulse));
    n.drift.rng.seed(Rng::derive_seed(seed, kDomainDrift));
    set_noise(cfg_.noise);
    for (auto& e : entries_) {
        e->fade.rng.seed(Rng::derive_seed(seed, kDomainFadingBase + e->id));
        e->fade.y = 0.0f;
        e->phase = 0;
        e->gain_prev = e->gain_next = 1.0f;
    }
}

void Band::set_noise(const NoiseTexture& t) {
    cfg_.noise = t;
    auto& n = *noise_;
    const double control_rate = cfg_.sample_rate / kControlSamples;
    n.drift.configure(t.drift_rate_hz, control_rate);
    n.sigma = std::sqrt(std::pow(10.0f, t.noise_floor_db / 10.0f));
    // Tilt from a two-tap complex filter 1 + j g z^-1, whose power response
    // 1 + g^2 + 2 g sin(w) rises monotonically from -fs/4 to +fs/4.
    const float r = std::pow(10.0f, t.tilt_db / 20.0f);
    n.tilt_g = (r - 1.0f) / (r + 1.0f);
    n.tilt_norm = 1.0f / std::sqrt(1.0f + n.tilt_g * n.tilt_g);
    n.crash_decay = std::exp(-1.0f / std::max(1.0f, static_cast<float>(t.crash_decay_ms * 1e-3 * cfg_.sample_rate)));
}

SignalId Band::add(std::shared_ptr<Source> src, SignalParams params) {
    auto e = std::make_unique<Entry>();
    e->id = next_id_++;
    e->src = std::move(src);
    e->params = params;
    e->fade.rng.seed(Rng::derive_seed(cfg_.seed, kDomainFadingBase + e->id));
    e->fade.configure(params.qsb.rate_hz, cfg_.sample_rate / kControlSamples);
    const SignalId id = e->id;
    entries_.push_back(std::move(e));
    return id;
}

void Band::remove(SignalId id) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [id](const auto& e) { return e->id == id; }),
                   entries_.end());
}

void Band::set_params(SignalId id, const SignalParams& p) {
    for (auto& e : entries_) {
        if (e->id == id) {
            e->params = p;
            e->fade.configure(p.qsb.rate_hz, cfg_.sample_rate / kControlSamples);
        }
    }
}

SignalParams Band::params(SignalId id) const {
    for (const auto& e : entries_)
        if (e->id == id) return e->params;
    return {};
}

Source* Band::source(SignalId id) const {
    for (const auto& e : entries_)
        if (e->id == id) return e->src.get();
    return nullptr;
}

std::vector<SignalId> Band::ids() const {
    std::vector<SignalId> out;
    for (const auto& e : entries_) out.push_back(e->id);
    return out;
}

float Band::level_to_amplitude(float level_db, double ref_bw_hz) const {
    const double floor_power = std::pow(10.0, cfg_.noise.noise_floor_db / 10.0);
    const double in_ref = floor_power * ref_bw_hz / cfg_.sample_rate;
    return static_cast<float>(std::sqrt(in_ref * std::pow(10.0, level_db / 10.0)));
}

float Band::fading_db(SignalId id) const {
    for (const auto& e : entries_)
        if (e->id == id) return e->fade_db;
    return 0.0f;
}

void Band::render_noise(std::span<cf32> out) {
    auto& n = *noise_;
    const auto& t = cfg_.noise;
    const float control_s = static_cast<float>(kControlSamples / cfg_.sample_rate);
    const float crash_p = t.crash_rate_hz * control_s;
    const float impulse_p = t.impulse_rate_hz * control_s;
    const float crash_peak = n.sigma * std::pow(10.0f, t.crash_level_db / 20.0f);
    const float impulse_peak = n.sigma * std::pow(10.0f, t.impulse_level_db / 20.0f);

    for (cf32& o : out) {
        if (n.phase == 0) {
            // Control-rate decisions. Each draws from its own stream, and
            // each stream is advanced the same number of times whatever the
            // settings, so turning crashes off does not change the floor.
            const float d = n.drift.next();
            n.drift_gain = std::pow(10.0f, 0.5f * t.drift_db * std::clamp(d, -2.0f, 2.0f) / 20.0f);
            const float cu = n.crash.uniform();
            const float cs = n.crash.uniform(0.3f, 1.0f);
            if (cu < crash_p) n.crash_env = std::max(n.crash_env, crash_peak * cs);
            const float iu = n.impulse.uniform();
            const float ipos = n.impulse.uniform();
            const float iamp = n.impulse.uniform(0.5f, 1.0f);
            n.impulse_at = -1;
            if (iu < impulse_p) {
                n.impulse_at = static_cast<int>(ipos * kControlSamples);
                n.impulse_amp = impulse_peak * iamp;
            }
        }
        const cf32 w = n.floor.complex_gaussian();
        const cf32 tilted = (w + cf32{0.0f, n.tilt_g} * n.tilt_prev) * n.tilt_norm;
        n.tilt_prev = w;
        cf32 v = tilted * (n.sigma * n.drift_gain);
        // Crashes are noise bursts, not clicks, so they draw a fresh Gaussian.
        const cf32 cg = n.crash.complex_gaussian();
        v += cg * n.crash_env;
        n.crash_env *= n.crash_decay;
        if (n.phase == n.impulse_at) {
            const float ph = n.impulse.uniform() * kTwoPi;
            v += cf32{std::cos(ph), std::sin(ph)} * n.impulse_amp;
        }
        o = v;
        n.phase = (n.phase + 1) % kControlSamples;
    }
}

void Band::render(std::span<cf32> out) {
    render_noise(out);
    scratch_.resize(out.size());
    for (auto& e : entries_) {
        if (!e->params.enabled || !e->src) continue;
        e->src->render(scratch_);
        const float base = level_to_amplitude(e->params.level_db);
        const auto& q = e->params.qsb;
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (e->phase == 0) {
                e->gain_prev = e->gain_next;
                const float x = e->fade.next();
                // Fades are dips below the nominal level, so map the slow
                // Gaussian onto [-depth, 0] dB.
                e->fade_db = q.depth_db > 0.0f
                                 ? -q.depth_db * (0.5f + 0.25f * std::clamp(x, -2.0f, 2.0f))
                                 : 0.0f;
                e->gain_next = std::pow(10.0f, e->fade_db / 20.0f);
            }
            // Interpolated so a strong carrier does not carry zipper noise.
            const float frac = static_cast<float>(e->phase) / kControlSamples;
            const float g = e->gain_prev + (e->gain_next - e->gain_prev) * frac;
            out[i] += scratch_[i] * (base * g);
            e->phase = (e->phase + 1) % kControlSamples;
        }
    }
}

} // namespace lwdsp
