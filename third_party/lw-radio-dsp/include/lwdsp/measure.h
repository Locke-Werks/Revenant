// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Locke Werks
//
// Measurements the game logic reads: passband SNR for the S-meter and lock
// detection, and a per-signal tune quality the transcript reveal keys off.
#pragma once

#include "lwdsp/types.h"

#include <span>

namespace lwdsp {

// Power in [center - bw/2, center + bw/2] against the median-bin noise
// estimate across the whole block. FFT based, one shot.
struct PassbandReading {
    float signal_dbfs = -200.0f;  // total power in the passband
    float noise_dbfs = -200.0f;   // noise power scaled to the passband width
    float snr_db = 0.0f;          // (signal - noise) / noise
    float peak_hz = 0.0f;         // strongest bin in the passband
};

PassbandReading measure_passband(std::span<const cf32> iq, double sample_rate,
                                 double center_hz, double bandwidth_hz,
                                 int fft_size = 4096);

// How well the receiver is set for one known signal. The game owns the
// truth (it placed the signal), so this compares settings to truth instead
// of guessing from audio.
struct TuneInput {
    Mode signal_mode = Mode::USB;
    double signal_offset_hz = 0.0;  // carrier, suppressed carrier, or FSK centre
    Mode rx_mode = Mode::USB;
    double rx_vfo_hz = 0.0;
    float rx_bandwidth_hz = 2700.0f;
    float rx_bfo_hz = 700.0f;
    float rx_clarifier_hz = 0.0f;
    float passband_snr_db = 0.0f;   // from Meter or measure_passband
};

struct TuneQuality {
    float offset_error_hz = 0.0f;  // signed: positive means tuned too high
    float snr_db = 0.0f;
    float tune_score = 0.0f;       // 0..1 from the offset error alone
    float snr_score = 0.0f;        // 0..1 from SNR alone
    float quality = 0.0f;          // 0..1 combined, drives transcript fade-in
    bool mode_match = false;
};

// Tolerances are mode specific: SSB voice becomes unintelligible past about
// 100 Hz, AM tolerates most of its bandwidth, CW needs the tone inside the
// filter, FSK needs both tones inside it.
TuneQuality evaluate_tune(const TuneInput& in);

// Expected word legibility given quality and a per-word SNR threshold.
// Smooth so a transcript fades in rather than snapping.
float word_reveal(const TuneQuality& q, float word_snr_threshold_db);

} // namespace lwdsp
