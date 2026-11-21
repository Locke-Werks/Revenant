// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Locke Werks
//
// Transmitters. Each one writes complex baseband centred on its own offset
// from the band centre, at the wideband rate, with peak envelope near 1.0.
// The Band scales it to the requested level relative to the noise floor.
#pragma once

#include "lwdsp/primitives.h"
#include "lwdsp/types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lwdsp {

// Pulls mono audio at the wideband rate. Fill the whole span; write zeros
// for silence. The game feeds VO, music or synth output through this.
using AudioFeed = std::function<void(std::span<float>)>;

// Steady sine, mostly for tests and beacons.
AudioFeed make_tone_feed(double freq_hz, double sample_rate, float amplitude = 0.5f);

class Source {
public:
    virtual ~Source() = default;
    // Overwrites out with the next block of baseband.
    virtual void render(std::span<cf32> out) = 0;
    [[nodiscard]] virtual double offset_hz() const noexcept = 0;
    virtual void set_offset_hz(double hz) noexcept = 0;
    // Offset is the carrier, suppressed carrier, or FSK centre frequency.
    [[nodiscard]] virtual Mode mode() const noexcept = 0;
};

// Double sideband full carrier: (1 + m * a(t)) e^{j w t} / (1 + m).
class AmModulator final : public Source {
public:
    AmModulator(double offset_hz, double sample_rate, float mod_index = 0.8f);
    void set_feed(AudioFeed feed) { feed_ = std::move(feed); }
    void set_mod_index(float m) noexcept { m_ = m; }
    // Block form for callers that already hold the audio.
    void process(std::span<const float> audio, std::span<cf32> out);
    void render(std::span<cf32> out) override;
    [[nodiscard]] double offset_hz() const noexcept override { return nco_.frequency(); }
    void set_offset_hz(double hz) noexcept override;
    [[nodiscard]] Mode mode() const noexcept override { return Mode::AM; }

private:
    double fs_;
    float m_;
    Nco nco_;
    AudioFeed feed_;
    std::vector<float> scratch_;
};

// Single sideband via a Hilbert analytic signal. Offset is the suppressed
// carrier: USB audio lands above it, LSB below.
class SsbModulator final : public Source {
public:
    SsbModulator(double offset_hz, double sample_rate, bool upper = true, int hilbert_taps = 255);
    void set_feed(AudioFeed feed) { feed_ = std::move(feed); }
    void set_upper(bool upper) noexcept { upper_ = upper; }
    // Speech-band limits applied before the Hilbert so nothing below 300 Hz
    // leaks across the carrier.
    void set_audio_band(double lo_hz, double hi_hz);
    void process(std::span<const float> audio, std::span<cf32> out);
    void render(std::span<cf32> out) override;
    [[nodiscard]] double offset_hz() const noexcept override { return nco_.frequency(); }
    void set_offset_hz(double hz) noexcept override;
    [[nodiscard]] Mode mode() const noexcept override { return upper_ ? Mode::USB : Mode::LSB; }

private:
    double fs_;
    bool upper_;
    Nco nco_;
    FirRR hilbert_;
    std::vector<float> delay_;
    std::size_t delay_pos_ = 0;
    Biquad hp_, lp_;
    AudioFeed feed_;
    std::vector<float> scratch_;
};

// On/off keyed carrier with raised-cosine edges so the keying does not
// splatter clicks across the band.
class CwModulator final : public Source {
public:
    CwModulator(double offset_hz, double sample_rate);
    // Morse text ("CQ DE UA1ABC"), PARIS timing at wpm. Optional Farnsworth
    // spacing: characters at wpm, gaps stretched to effective_wpm.
    void set_text(std::string_view text, float wpm, float effective_wpm = 0.0f);
    // Timing string: one char per dot unit, '1' or '#' or '=' key down,
    // anything else key up. unit_seconds is the dot length.
    void set_timing(std::string_view pattern, double unit_seconds);
    void set_repeat(bool repeat, double gap_seconds = 2.0) noexcept;
    void set_edge_ms(float ms) noexcept;
    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] bool key_down() const noexcept;
    void render(std::span<cf32> out) override;
    [[nodiscard]] double offset_hz() const noexcept override { return nco_.frequency(); }
    void set_offset_hz(double hz) noexcept override;
    [[nodiscard]] Mode mode() const noexcept override { return Mode::CW; }

    // Exposed for the decoder and tests: "CQ" -> "111010111010001110111010111".
    static std::string text_to_timing(std::string_view text);

private:
    struct Run {
        bool down;
        double samples;
    };
    void rewind() noexcept;
    double fs_;
    Nco nco_;
    std::vector<Run> runs_;
    bool repeat_ = false;
    double repeat_gap_s_ = 2.0;
    float edge_ms_ = 5.0f;
    std::size_t run_ = 0;
    double left_ = 0.0;   // samples remaining in the current run
    bool in_gap_ = false; // inside the repeat gap after the last run
    float ramp_ = 0.0f;   // 0..1 position along the raised-cosine edge
};

// Continuous-phase FSK. Mark is offset + shift/2, space offset - shift/2,
// matching the convention that the offset is the centre of the pair.
class FskModulator final : public Source {
public:
    FskModulator(double offset_hz, double sample_rate, double shift_hz = 170.0, double baud = 45.45);
    void set_shift(double shift_hz) noexcept { shift_ = shift_hz; }
    void set_baud(double baud) noexcept { baud_ = baud; }
    // Raw symbols, 1 = mark.
    void set_bits(std::vector<std::uint8_t> bits);
    // ITA2 Baudot with 1 start bit, 5 data bits LSB first, stop_bits stop.
    void set_text(std::string_view text, float stop_bits = 1.5f);
    void set_repeat(bool repeat) noexcept { repeat_ = repeat; }
    // Idle condition between messages and after the end: mark.
    [[nodiscard]] bool finished() const noexcept;
    void render(std::span<cf32> out) override;
    [[nodiscard]] double offset_hz() const noexcept override { return offset_; }
    void set_offset_hz(double hz) noexcept override { offset_ = hz; }
    [[nodiscard]] Mode mode() const noexcept override { return Mode::FSK; }

    static std::vector<std::uint8_t> ita2_encode(std::string_view text, float stop_bits = 1.5f);

private:
    double fs_, offset_, shift_, baud_;
    std::vector<std::uint8_t> bits_;
    std::vector<float> bit_len_; // in bit periods, lets 1.5 stop bits exist
    bool repeat_ = false;
    std::size_t idx_ = 0;
    double in_bit_ = 0.0;
    double phase_ = 0.0;
};

} // namespace lwdsp
