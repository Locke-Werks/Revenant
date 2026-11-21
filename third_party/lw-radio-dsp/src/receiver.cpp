// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Locke Werks
//
// The noise blanker's detect/apply rule (reference window ending a guard
// before the tested sample, blank within hang/lead of a flag) is ported from
// Revenant core/dsp/noise_reference.cpp reference_blank_detect/apply, from a
// ring-indexed GPU twin into a streaming CPU form. The notch section design
// (pole radius from width, zero radius from depth, unity gain at the far
// band edge) is ported from design_notch in the same file.
#include "lwdsp/receiver.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace lwdsp {

// ---------------------------------------------------------------------------
// AGC

Agc::Agc(const AgcConfig& c, double fs) { configure(c, fs); }

void Agc::configure(const AgcConfig& c, double fs) {
    cfg_ = c;
    fs_ = fs;
    const auto coef = [fs](float ms) {
        return static_cast<float>(1.0 - std::exp(-1.0 / std::max(1.0, ms * 1e-3 * fs)));
    };
    att_ = coef(c.attack_ms);
    dec_ = coef(c.decay_ms);
    hang_samples_ = static_cast<int>(c.hang_ms * 1e-3 * fs);
    // Three attack time constants of lookahead: the envelope is within 5%
    // of a new peak before that peak reaches the output, so a crash does
    // not punch through at full gain.
    delay_.assign(static_cast<std::size_t>(std::max(1.0, 3.0 * c.attack_ms * 1e-3 * fs)) + 1, 0.0f);
    dpos_ = 0;
}

void Agc::reset() noexcept {
    env_ = 0.0f;
    gain_ = 1.0f;
    hang_ = 0;
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    dpos_ = 0;
}

float Agc::process(float x) noexcept {
    const float delayed = delay_[dpos_];
    delay_[dpos_] = x;
    dpos_ = (dpos_ + 1) % delay_.size();
    if (!cfg_.enabled) {
        gain_ = std::pow(10.0f, cfg_.manual_gain_db / 20.0f);
        return delayed * gain_;
    }
    const float m = std::fabs(x);
    if (m > env_) {
        env_ += att_ * (m - env_);
        hang_ = hang_samples_;
    } else if (hang_ > 0) {
        --hang_;
    } else {
        env_ += dec_ * (m - env_);
    }
    const float gmax = std::pow(10.0f, cfg_.max_gain_db / 20.0f);
    const float gmin = std::pow(10.0f, cfg_.min_gain_db / 20.0f);
    gain_ = std::clamp(cfg_.target / std::max(env_, 1e-12f), gmin, gmax);
    return delayed * gain_;
}

void Agc::process(std::span<float> io) noexcept {
    for (float& v : io) v = process(v);
}

float Agc::gain_db() const noexcept { return lin_to_db(gain_); }

// ---------------------------------------------------------------------------
// Noise blanker

NoiseBlanker::NoiseBlanker(const NoiseBlankerConfig& c, double fs) { configure(c, fs); }

void NoiseBlanker::configure(const NoiseBlankerConfig& c, double fs) {
    cfg_ = c;
    fs_ = fs;
    cfg_.hang_samples = std::max(0, c.hang_samples);
    cfg_.lead_samples = std::max(0, c.lead_samples);
    window_ = std::clamp(static_cast<int>(std::lround(c.window_ms * 1e-3 * fs)), 16, 1024);
    guard_ = cfg_.lead_samples + 2;
    threshold_ = std::pow(10.0f, c.threshold_db / 10.0f);
    const auto need = static_cast<std::size_t>(window_ + guard_ + cfg_.lead_samples + 2);
    samples_.assign(std::bit_ceil(need), cf32{});
    power_.assign(samples_.size(), 0.0f);
    reset();
}

void NoiseBlanker::reset() noexcept {
    std::fill(samples_.begin(), samples_.end(), cf32{});
    std::fill(power_.begin(), power_.end(), 0.0f);
    n_ = 0;
    last_flag_ = 0;
    any_flag_ = false;
    window_sum_ = 0.0;
}

void NoiseBlanker::process(std::span<cf32> io) noexcept {
    if (!cfg_.enabled || samples_.empty()) return;
    const std::uint64_t mask = samples_.size() - 1;
    const auto lead = static_cast<std::uint64_t>(cfg_.lead_samples);
    const auto hang = static_cast<std::uint64_t>(cfg_.hang_samples);
    const auto g = static_cast<std::uint64_t>(guard_);
    const auto w = static_cast<std::uint64_t>(window_);
    for (cf32& x : io) {
        const std::uint64_t j = n_;
        const float p = std::norm(x);
        samples_[j & mask] = x;
        power_[j & mask] = p;
        // Window is [j - g - w, j - g - 1]: slide it forward by one.
        if (j >= g + 1) window_sum_ += power_[(j - g - 1) & mask];
        if (j >= g + w + 1) window_sum_ -= power_[(j - g - w - 1) & mask];
        // A zero reference never flags, so the very first samples after a
        // reset are not blanked against silence.
        const double ref = std::max(window_sum_, 0.0);
        if (j >= g + w && ref > 0.0 && static_cast<double>(p) * static_cast<double>(w) > threshold_ * ref) {
            last_flag_ = j;
            any_flag_ = true;
        }
        // Output is the sample lead behind the newest.
        cf32 y{};
        if (j >= lead) {
            const std::uint64_t i = j - lead;
            y = samples_[i & mask];
            if (any_flag_ && last_flag_ + hang >= i) {
                y = cf32{};
                ++blanked_;
            }
        }
        x = y;
        ++n_;
    }
}

// ---------------------------------------------------------------------------
// Notch

namespace {
Biquad design_notch(double audio_hz, double depth_db, double width_hz, double fa) {
    audio_hz = std::clamp(audio_hz, 1.0, 0.499 * fa);
    width_hz = std::clamp(width_hz, 1.0, 0.24 * fa);
    const double pi = std::numbers::pi;
    const double omega = 2.0 * pi * audio_hz / fa;
    const double rp = 1.0 - pi * width_hz / fa;
    const double rz = 1.0 - (1.0 - rp) * std::pow(10.0, -depth_db / 20.0);
    const double c = std::cos(omega);
    double b0 = 1.0, b1 = -2.0 * rz * c, b2 = rz * rz;
    const double a1 = -2.0 * rp * c, a2 = rp * rp;
    // Unity gain at whichever band edge is further from the notch.
    const double z = (omega < 0.5 * pi) ? -1.0 : 1.0;
    const double scale = (1.0 + a1 * z + a2) / (b0 + b1 * z + b2);
    b0 *= scale;
    b1 *= scale;
    b2 *= scale;
    return Biquad::from_coefficients(b0, b1, b2, a1, a2);
}
} // namespace

Notch::Notch(const NotchConfig& c, double fs) { configure(c, fs); }

void Notch::configure(const NotchConfig& c, double fs) {
    cfg_ = c;
    a_ = design_notch(c.freq_hz, c.depth_db, c.width_hz, fs);
    b_ = a_;
}

void Notch::reset() noexcept {
    a_.reset();
    b_.reset();
}

void Notch::process(std::span<float> io) noexcept {
    if (!cfg_.enabled) return;
    for (float& v : io) v = b_.process(a_.process(v));
}

// ---------------------------------------------------------------------------
// Receiver

float dbfs_to_s_units(float dbfs, float s9) {
    // Above S9 this keeps climbing at 1 per 6 dB; the UI shows that as +dB.
    return std::max(0.0f, 9.0f + (dbfs - s9) / 6.0f);
}

Receiver::Receiver(const ReceiverConfig& c) : cfg_(c) { reset(); }

std::pair<float, float> Receiver::passband() const noexcept {
    const float bw = cfg_.bandwidth_hz;
    const float lc = cfg_.ssb_low_cut_hz;
    switch (cfg_.mode) {
    case Mode::USB: return {lc, lc + bw};
    case Mode::LSB: return {-(lc + bw), -lc};
    case Mode::AM:
    case Mode::CW:
    case Mode::FSK: return {-0.5f * bw, 0.5f * bw};
    }
    return {-0.5f * bw, 0.5f * bw};
}

void Receiver::rebuild_filter() {
    const auto [lo, hi] = passband();
    chan_.set_taps(design_complex_bandpass(cfg_.filter_taps, lo, hi, cfg_.rates.wideband_hz));
}

void Receiver::reset() {
    const double fs = cfg_.rates.wideband_hz;
    const double fa = cfg_.rates.audio_hz;
    vfo_.reset();
    bfo_.reset();
    set_vfo(cfg_.vfo_hz);
    set_bfo(cfg_.bfo_hz);
    rebuild_filter();
    nb_.configure(cfg_.nb, fs);
    notch_.configure(cfg_.notch, fa);
    agc_.configure(cfg_.agc, fa);
    dc_ = Biquad::dc_block(20.0, fa);
    decim_phase_ = 0;
    sig_pow_ = 0.0f;
    floor_pow_ = 0.0f;
    meter_ = {};
}

void Receiver::set_mode(Mode m) {
    cfg_.mode = m;
    set_vfo(cfg_.vfo_hz);
    rebuild_filter();
}

void Receiver::set_vfo(double hz) {
    cfg_.vfo_hz = hz;
    // The clarifier moves the SSB reference without moving the dial.
    const bool ssb = cfg_.mode == Mode::USB || cfg_.mode == Mode::LSB;
    const double f = hz + (ssb ? cfg_.clarifier_hz : 0.0);
    vfo_.set_frequency(-f, cfg_.rates.wideband_hz);
}

void Receiver::set_bandwidth(float hz) {
    cfg_.bandwidth_hz = std::max(hz, 50.0f);
    rebuild_filter();
}

void Receiver::set_bfo(float hz) {
    cfg_.bfo_hz = hz;
    bfo_.set_frequency(hz, cfg_.rates.audio_hz);
}

void Receiver::set_clarifier(float hz) {
    cfg_.clarifier_hz = hz;
    set_vfo(cfg_.vfo_hz);
}

void Receiver::set_agc(const AgcConfig& a) {
    cfg_.agc = a;
    agc_.configure(a, cfg_.rates.audio_hz);
}

void Receiver::set_noise_blanker(const NoiseBlankerConfig& nb) {
    cfg_.nb = nb;
    nb_.configure(nb, cfg_.rates.wideband_hz);
}

void Receiver::set_notch(const NotchConfig& n) {
    cfg_.notch = n;
    notch_.configure(n, cfg_.rates.audio_hz);
}

std::size_t Receiver::process(std::span<const cf32> iq, std::span<float> audio) {
    const int dec = std::max(1, cfg_.rates.decimation());
    const double fa = cfg_.rates.audio_hz;
    work_.assign(iq.begin(), iq.end());
    nb_.process(work_);

    // Meter smoothing ~50 ms; floor creeps up at ~1 dB/s so a carrier
    // that stays keyed for minutes is not mistaken for the floor.
    const float a_sig = static_cast<float>(1.0 - std::exp(-1.0 / (0.05 * fa)));
    const float floor_up = static_cast<float>(std::pow(10.0, 1.0 / (10.0 * fa)));

    std::size_t out_n = 0;
    for (const cf32& x : work_) {
        chan_.shift_in(x * vfo_.step());
        if (++decim_phase_ < dec) continue;
        decim_phase_ = 0;
        const cf32 bb = chan_.output();

        const float p = std::norm(bb);
        sig_pow_ += a_sig * (p - sig_pow_);
        if (floor_pow_ <= 0.0f || sig_pow_ < floor_pow_) floor_pow_ = sig_pow_;
        else floor_pow_ *= floor_up;

        float y = 0.0f;
        switch (cfg_.mode) {
        case Mode::AM: y = dc_.process(std::abs(bb)); break;
        case Mode::USB:
        case Mode::LSB: y = bb.real(); break;
        case Mode::CW:
        case Mode::FSK: y = (bb * bfo_.step()).real(); break;
        }
        if (out_n < audio.size()) audio[out_n] = y;
        ++out_n;
    }
    const std::size_t n = std::min(out_n, audio.size());
    notch_.process(audio.first(n));
    agc_.process(audio.first(n));
    for (std::size_t i = 0; i < n; ++i) audio[i] *= cfg_.volume;

    meter_.signal_dbfs = power_to_db(sig_pow_);
    meter_.noise_dbfs = power_to_db(floor_pow_);
    const float excess = std::max(sig_pow_ - floor_pow_, 0.0f);
    meter_.snr_db = floor_pow_ > 0.0f ? power_to_db(excess / floor_pow_) : 0.0f;
    meter_.s_units = dbfs_to_s_units(meter_.signal_dbfs);
    meter_.agc_gain_db = agc_.gain_db();
    return n;
}

} // namespace lwdsp
