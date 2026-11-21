// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Locke Werks
//
// The player's receiver: wideband IQ in, mono audio out.
//
//   IQ -> noise blanker -> VFO mix -> channel filter (decimating) ->
//   S-meter tap -> demod -> notch -> AGC -> audio
//
// The blanker sits before the channel filter because the filter smears an
// impulse across its whole length; after it, there is nothing left to blank.
#pragma once

#include "lwdsp/primitives.h"
#include "lwdsp/types.h"

#include <cstdint>
#include <vector>

namespace lwdsp {

// Audio-domain AGC with separate attack and decay and a hang period, the way
// a communications receiver behaves rather than a broadcast compressor.
struct AgcConfig {
    bool enabled = true;
    float attack_ms = 2.0f;
    float decay_ms = 300.0f;
    float hang_ms = 200.0f;
    float target = 0.3f;        // output envelope target, linear
    float max_gain_db = 80.0f;
    float min_gain_db = -20.0f;
    float manual_gain_db = 0.0f; // used when disabled
};

class Agc {
public:
    Agc() = default;
    Agc(const AgcConfig& cfg, double sample_rate);
    void configure(const AgcConfig& cfg, double sample_rate);
    void reset() noexcept;
    float process(float x) noexcept;
    void process(std::span<float> io) noexcept;
    [[nodiscard]] float gain_db() const noexcept;

private:
    AgcConfig cfg_{};
    double fs_ = 48000.0;
    float env_ = 0.0f;
    float gain_ = 1.0f;
    float att_ = 0.0f, dec_ = 0.0f;
    int hang_samples_ = 0, hang_ = 0;
    // Short lookahead so the attack catches the leading edge of a crash.
    std::vector<float> delay_;
    std::size_t dpos_ = 0;
};

// Complex-domain impulse blanker, detection by threshold: a sample whose
// power stands threshold_db above the mean of a reference window (ending a
// guard before it, so the impulse's own skirt is not its reference) is
// flagged, and output samples within hang behind or lead ahead of a flag are
// zeroed. Output is delayed by the lead.
struct NoiseBlankerConfig {
    bool enabled = false;
    // Complex Gaussian noise exceeds its mean power by 12 dB with
    // probability about 1.3e-7, so noise alone almost never blanks.
    float threshold_db = 12.0f;
    float window_ms = 0.5f;     // reference window, clamped to 16..1024 samples
    int hang_samples = 2;
    int lead_samples = 2;
};

class NoiseBlanker {
public:
    NoiseBlanker() = default;
    NoiseBlanker(const NoiseBlankerConfig& cfg, double sample_rate);
    void configure(const NoiseBlankerConfig& cfg, double sample_rate);
    void reset() noexcept;
    void process(std::span<cf32> io) noexcept;
    [[nodiscard]] std::uint64_t blanked_samples() const noexcept { return blanked_; }

private:
    NoiseBlankerConfig cfg_{};
    double fs_ = 48000.0;
    int window_ = 24, guard_ = 4;
    float threshold_ = 15.85f;   // power ratio
    std::vector<cf32> samples_;  // ring, power of two
    std::vector<float> power_;
    std::uint64_t n_ = 0;        // samples consumed
    std::uint64_t last_flag_ = 0;
    bool any_flag_ = false;
    double window_sum_ = 0.0;
    std::uint64_t blanked_ = 0;
};

// Manual audio notch, frequency in demodulated audio Hz.
struct NotchConfig {
    bool enabled = false;
    float freq_hz = 1000.0f;
    float width_hz = 60.0f;   // -3 dB width
    float depth_db = 50.0f;   // per section; two sections run in cascade
};

class Notch {
public:
    Notch() = default;
    Notch(const NotchConfig& cfg, double sample_rate);
    void configure(const NotchConfig& cfg, double sample_rate);
    void reset() noexcept;
    void process(std::span<float> io) noexcept;

private:
    NotchConfig cfg_{};
    Biquad a_, b_; // cascaded for depth
};

struct ReceiverConfig {
    SampleRates rates{};
    Mode mode = Mode::USB;
    double vfo_hz = 0.0;         // tuned frequency as offset from band centre
    float bandwidth_hz = 2700.0f;
    // SSB passband starts this far from the carrier.
    float ssb_low_cut_hz = 200.0f;
    // CW: the BFO offset is the pitch you hear when zero-beat.
    float bfo_hz = 700.0f;
    // Fine BFO trim for SSB clarifier (RIT-style), Hz.
    float clarifier_hz = 0.0f;
    AgcConfig agc{};
    NoiseBlankerConfig nb{};
    NotchConfig notch{};
    // 511 taps at 48 kHz gives roughly a 300 Hz transition, enough to keep the
    // opposite sideband out of an SSB passband that starts 200 Hz from carrier.
    int filter_taps = 511;
    float volume = 1.0f;
};

struct Meter {
    float signal_dbfs = -200.0f;  // passband power after the channel filter
    float noise_dbfs = -200.0f;   // tracked floor estimate in the passband
    float snr_db = 0.0f;
    float s_units = 0.0f;         // S0..S9, then 9 + dB/6 above S9
    float agc_gain_db = 0.0f;
};

class Receiver {
public:
    explicit Receiver(const ReceiverConfig& cfg = {});

    void set_mode(Mode m);
    void set_vfo(double hz);
    void set_bandwidth(float hz);
    void set_bfo(float hz);
    void set_clarifier(float hz);
    void set_agc(const AgcConfig& a);
    void set_noise_blanker(const NoiseBlankerConfig& nb);
    void set_notch(const NotchConfig& n);
    void set_volume(float v) noexcept { cfg_.volume = v; }
    [[nodiscard]] const ReceiverConfig& config() const noexcept { return cfg_; }

    // Consumes iq.size() wideband samples and writes iq.size() / decimation
    // audio samples. Returns the count written. audio must be large enough.
    std::size_t process(std::span<const cf32> iq, std::span<float> audio);

    [[nodiscard]] const Meter& meter() const noexcept { return meter_; }
    void reset();

    // Passband edges relative to the VFO for the current mode, Hz. The
    // waterfall overlay draws these.
    [[nodiscard]] std::pair<float, float> passband() const noexcept;

private:
    void rebuild_filter();
    ReceiverConfig cfg_;
    Nco vfo_;
    Nco bfo_;
    FirCC chan_;
    int decim_phase_ = 0;
    NoiseBlanker nb_;
    Notch notch_;
    Agc agc_;
    Biquad dc_;
    Meter meter_{};
    float sig_pow_ = 0.0f;
    float floor_pow_ = 0.0f;
    std::vector<cf32> work_;
};

// Convert dBFS passband power to S units with S9 at s9_dbfs, 6 dB per S.
float dbfs_to_s_units(float dbfs, float s9_dbfs = -73.0f);

} // namespace lwdsp
