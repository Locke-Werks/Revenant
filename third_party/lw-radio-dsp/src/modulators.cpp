// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Locke Werks
//
// CW keying follows ITU-R M.1677-1 timing and uses the half-amplitude
// centred raised-cosine edge from Revenant core/dsp/synth/cw_mod.cpp.
#include "lwdsp/modulators.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <numbers>
#include <utility>

namespace lwdsp {

AudioFeed make_tone_feed(double freq_hz, double fs, float amplitude) {
    auto nco = std::make_shared<Nco>(freq_hz, fs);
    return [nco, amplitude](std::span<float> out) {
        for (float& v : out) v = amplitude * nco->step().real();
    };
}

// ---------------------------------------------------------------------------
// AM

AmModulator::AmModulator(double off, double fs, float m) : fs_(fs), m_(m), nco_(off, fs) {}

void AmModulator::set_offset_hz(double hz) noexcept { nco_.set_frequency(hz, fs_); }

void AmModulator::process(std::span<const float> audio, std::span<cf32> out) {
    const float norm = 1.0f / (1.0f + std::fabs(m_));
    const std::size_t n = std::min(audio.size(), out.size());
    for (std::size_t i = 0; i < n; ++i) {
        const float a = std::clamp(audio[i], -1.0f, 1.0f);
        out[i] = nco_.step() * ((1.0f + m_ * a) * norm);
    }
}

void AmModulator::render(std::span<cf32> out) {
    scratch_.resize(out.size());
    if (feed_) feed_(scratch_);
    else std::fill(scratch_.begin(), scratch_.end(), 0.0f);
    process(scratch_, out);
}

// ---------------------------------------------------------------------------
// SSB

SsbModulator::SsbModulator(double off, double fs, bool up, int taps)
    : fs_(fs), upper_(up), nco_(off, fs), hilbert_(design_hilbert(taps)) {
    delay_.assign((hilbert_.size() - 1) / 2 + 1, 0.0f);
    set_audio_band(300.0, 2800.0);
}

void SsbModulator::set_audio_band(double lo, double hi) {
    hp_ = Biquad::highpass(lo, 0.7071, fs_);
    lp_ = Biquad::lowpass(std::min(hi, 0.45 * fs_), 0.7071, fs_);
}

void SsbModulator::set_offset_hz(double hz) noexcept { nco_.set_frequency(hz, fs_); }

void SsbModulator::process(std::span<const float> audio, std::span<cf32> out) {
    const std::size_t n = std::min(audio.size(), out.size());
    const std::size_t d = delay_.size();
    for (std::size_t i = 0; i < n; ++i) {
        const float x = lp_.process(hp_.process(audio[i]));
        const float q = hilbert_.push(x);
        // The Hilbert FIR delays by (taps-1)/2; match the in-phase path.
        delay_[delay_pos_] = x;
        const float in_phase = delay_[(delay_pos_ + 1) % d];
        delay_pos_ = (delay_pos_ + 1) % d;
        const cf32 analytic{in_phase, upper_ ? q : -q};
        out[i] = analytic * nco_.step();
    }
}

void SsbModulator::render(std::span<cf32> out) {
    scratch_.resize(out.size());
    if (feed_) feed_(scratch_);
    else std::fill(scratch_.begin(), scratch_.end(), 0.0f);
    process(scratch_, out);
}

// ---------------------------------------------------------------------------
// CW

namespace {

struct MorseEntry {
    char c;
    const char* code;
};

constexpr std::array<MorseEntry, 54> kMorse{{
    {'A', ".-"},     {'B', "-..."},   {'C', "-.-."},   {'D', "-.."},    {'E', "."},
    {'F', "..-."},   {'G', "--."},    {'H', "...."},   {'I', ".."},     {'J', ".---"},
    {'K', "-.-"},    {'L', ".-.."},   {'M', "--"},     {'N', "-."},     {'O', "---"},
    {'P', ".--."},   {'Q', "--.-"},   {'R', ".-."},    {'S', "..."},    {'T', "-"},
    {'U', "..-"},    {'V', "...-"},   {'W', ".--"},    {'X', "-..-"},   {'Y', "-.--"},
    {'Z', "--.."},   {'0', "-----"},  {'1', ".----"},  {'2', "..---"},  {'3', "...--"},
    {'4', "....-"},  {'5', "....."},  {'6', "-...."},  {'7', "--..."},  {'8', "---.."},
    {'9', "----."},  {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'/', "-..-."},
    {'=', "-...-"},  {'+', ".-.-."},  {'-', "-....-"}, {'(', "-.--."},  {')', "-.--.-"},
    {'\'', ".----."}, {'"', ".-..-."}, {':', "---..."}, {';', "-.-.-."}, {'@', ".--.-."},
    {'!', "-.-.--"}, {'&', ".-..."},  {'_', "..--.-"}, {'$', "...-..-"},
}};

const char* morse_for(char c) {
    const char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const auto& e : kMorse)
        if (e.c == u) return e.code;
    return nullptr;
}

// Half-amplitude centred raised cosine, 0..1 in, 0..1 out.
float raised_cosine(float x) {
    return 0.5f - 0.5f * std::cos(kPi * std::clamp(x, 0.0f, 1.0f));
}

} // namespace

std::string CwModulator::text_to_timing(std::string_view text) {
    std::string out;
    bool pending_char_gap = false;
    bool pending_word_gap = false;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (pending_char_gap) pending_word_gap = true;
            continue;
        }
        const char* code = morse_for(c);
        if (!code) continue;
        if (pending_word_gap) out += "0000000";
        else if (pending_char_gap) out += "000";
        for (const char* p = code; *p; ++p) {
            if (p != code) out += '0';
            out += (*p == '-') ? "111" : "1";
        }
        pending_char_gap = true;
        pending_word_gap = false;
    }
    return out;
}

CwModulator::CwModulator(double off, double fs) : fs_(fs), nco_(off, fs) {}

void CwModulator::set_offset_hz(double hz) noexcept { nco_.set_frequency(hz, fs_); }

void CwModulator::rewind() noexcept {
    run_ = 0;
    in_gap_ = false;
    left_ = runs_.empty() ? 0.0 : runs_[0].samples;
}

void CwModulator::set_timing(std::string_view pattern, double unit_seconds) {
    runs_.clear();
    const double unit = unit_seconds * fs_;
    for (char c : pattern) {
        const bool down = (c == '1' || c == '#' || c == '=');
        if (!runs_.empty() && runs_.back().down == down) runs_.back().samples += unit;
        else runs_.push_back({down, unit});
    }
    rewind();
}

void CwModulator::set_text(std::string_view text, float wpm, float effective_wpm) {
    const std::string timing = text_to_timing(text);
    wpm = std::max(wpm, 1.0f);
    // PARIS: 50 units per word.
    const double unit = 1.2 / wpm;
    if (effective_wpm <= 0.0f || effective_wpm >= wpm) {
        set_timing(timing, unit);
        return;
    }
    // Farnsworth (ARRL): the 19 gap units of PARIS absorb the slowdown.
    const double total = 60.0 / effective_wpm;
    const double gap_unit = (total - 31.0 * unit) / 19.0;
    runs_.clear();
    std::size_t i = 0;
    while (i < timing.size()) {
        const bool down = timing[i] == '1';
        std::size_t j = i;
        while (j < timing.size() && (timing[j] == '1') == down) ++j;
        const std::size_t len = j - i;
        double secs = static_cast<double>(len) * unit;
        if (!down && len >= 3) secs = static_cast<double>(len) * gap_unit;
        runs_.push_back({down, secs * fs_});
        i = j;
    }
    rewind();
}

void CwModulator::set_repeat(bool r, double gap) noexcept {
    repeat_ = r;
    repeat_gap_s_ = gap;
}

void CwModulator::set_edge_ms(float ms) noexcept { edge_ms_ = std::max(ms, 0.0f); }

bool CwModulator::finished() const noexcept { return !repeat_ && run_ >= runs_.size(); }

bool CwModulator::key_down() const noexcept {
    return !in_gap_ && run_ < runs_.size() && runs_[run_].down;
}

void CwModulator::render(std::span<cf32> out) {
    const float edge = static_cast<float>(edge_ms_ * 1e-3 * fs_);
    const float step = edge > 0.5f ? 1.0f / edge : 1.0f;
    for (cf32& o : out) {
        while (left_ <= 0.0 && !finished()) {
            if (in_gap_) {
                rewind();
                break;
            }
            ++run_;
            if (run_ < runs_.size()) {
                left_ += runs_[run_].samples;
            } else if (repeat_) {
                in_gap_ = true;
                left_ += repeat_gap_s_ * fs_;
            }
        }
        const bool down = key_down();
        ramp_ = down ? std::min(1.0f, ramp_ + step) : std::max(0.0f, ramp_ - step);
        o = nco_.step() * raised_cosine(ramp_);
        left_ -= 1.0;
    }
}

// ---------------------------------------------------------------------------
// FSK / RTTY

namespace {

// ITA2 code points, bit 0 sent first. Index is the 5-bit code.
constexpr const char* kItaLetters = "\0E\nA SIU\rDRJNFCKTZLWHYPQOBG\x1BMXV\x1A";
constexpr const char* kItaFigures = "\0" "3\n- '87\r\x05" "4\x07,!:(5+)2#6019?&\x1B./;\x1A";
constexpr std::uint8_t kLtrs = 0x1F;
constexpr std::uint8_t kFigs = 0x1B;

int ita_find(const char* table, char c) {
    for (int i = 1; i < 32; ++i) {
        if (i == kLtrs || i == kFigs) continue;
        if (table[i] == c) return i;
    }
    return -1;
}

} // namespace

std::vector<std::uint8_t> FskModulator::ita2_encode(std::string_view text, float stop_bits) {
    // Packed as bit symbols; stop bit length is carried by the caller via
    // the per-bit length vector, so here a stop bit is marked with value 2.
    std::vector<std::uint8_t> out;
    auto emit = [&](int code) {
        out.push_back(0); // start
        for (int b = 0; b < 5; ++b) out.push_back(static_cast<std::uint8_t>((code >> b) & 1));
        out.push_back(2); // stop
    };
    (void)stop_bits;
    bool figs = false;
    emit(kLtrs);
    for (char raw : text) {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
        if (c == '\n') {
            emit(ita_find(kItaLetters, '\r'));
            emit(ita_find(kItaLetters, '\n'));
            continue;
        }
        int code = ita_find(kItaLetters, c);
        const bool shared = code >= 0 && ita_find(kItaFigures, c) == code; // space, CR, LF
        if (code >= 0) {
            if (figs && !shared) {
                emit(kLtrs);
                figs = false;
            }
            emit(code);
            continue;
        }
        code = ita_find(kItaFigures, c);
        if (code < 0) continue;
        if (!figs) {
            emit(kFigs);
            figs = true;
        }
        emit(code);
    }
    return out;
}

FskModulator::FskModulator(double off, double fs, double sh, double b)
    : fs_(fs), offset_(off), shift_(sh), baud_(b) {}

void FskModulator::set_bits(std::vector<std::uint8_t> b) {
    bits_.clear();
    bit_len_.clear();
    for (auto v : b) {
        bits_.push_back(v ? 1 : 0);
        bit_len_.push_back(1.0f);
    }
    idx_ = 0;
    in_bit_ = 0.0;
}

void FskModulator::set_text(std::string_view text, float stop_bits) {
    const auto sym = ita2_encode(text, stop_bits);
    bits_.clear();
    bit_len_.clear();
    for (auto v : sym) {
        bits_.push_back(v == 0 ? 0 : 1);
        bit_len_.push_back(v == 2 ? stop_bits : 1.0f);
    }
    idx_ = 0;
    in_bit_ = 0.0;
}

bool FskModulator::finished() const noexcept { return !repeat_ && idx_ >= bits_.size(); }

void FskModulator::render(std::span<cf32> out) {
    const double bit_step = baud_ / fs_;
    for (cf32& o : out) {
        bool mark = true;
        if (idx_ < bits_.size()) {
            mark = bits_[idx_] != 0;
            in_bit_ += bit_step;
            if (in_bit_ >= bit_len_[idx_]) {
                in_bit_ -= bit_len_[idx_];
                if (++idx_ >= bits_.size() && repeat_) idx_ = 0;
            }
        }
        const double f = offset_ + (mark ? 0.5 : -0.5) * shift_;
        const double a = 2.0 * std::numbers::pi * phase_;
        o = cf32{static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
        phase_ += f / fs_;
        phase_ -= std::floor(phase_);
    }
}

} // namespace lwdsp
