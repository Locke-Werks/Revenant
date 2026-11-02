// NOAA HRPT decoder. See hrpt.h for the specification and its citations.

#include "core/decode/hrpt.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>

namespace revenant::decode {

namespace {

constexpr std::uint64_t kSyncMask = (std::uint64_t{1} << 60) - 1;
constexpr std::uint32_t kAcquireErrors = 6;
constexpr std::uint32_t kHoldErrors = 15;
constexpr std::size_t kFrameBits = kHrptFrameWords * kHrptWordBits;

// Carrier loop: natural frequency and damping of the phase loop, and the
// frequency-aid gain, engineering choices tuned on the round trip.
constexpr double kPllNaturalHz = 1500.0;
constexpr double kPllDamping = 0.707;
constexpr double kFllGain = 2e-4;

constexpr double kTimingBandwidth = 0.0005;

// Mean of cos(phase error) above which the carrier counts as locked; cos 68
// degrees is 0.37 when locked and clean.
constexpr double kLockThreshold = 0.2;

}  // namespace

std::uint64_t hrpt_frame_sync() {
    // Table 4.1.3.1-1 note 1: x^6+x^5+x^2+x+1, all ones, first 60 bits. The
    // Galois form, shifting left, feeding back x^5, x^2, x and 1.
    std::uint32_t s = 0x3F;
    std::uint64_t out = 0;
    for (int i = 0; i < 60; ++i) {
        const std::uint32_t b = (s >> 5U) & 1U;
        out = (out << 1U) | b;
        s = (s << 1U) & 0x3FU;
        if (b != 0U) {
            s ^= 0b100111U;
        }
    }
    return out;
}

Expected<HrptDecoder> HrptDecoder::create(const HrptConfig& config, HrptEvents events) {
    const double chip_rate = 2.0 * kHrptBitRate;  // split phase halves
    if (!(config.sample_rate >= 2.0 * chip_rate * 0.999) || !(config.sample_rate <= 20e6)) {
        return fail("HRPT: sample rate must be between 2.6616 and 20 MS/s");
    }
    HrptDecoder d;
    d.config_ = config;
    d.events_ = std::move(events);
    const double wn = 2.0 * std::numbers::pi * kPllNaturalHz / config.sample_rate;
    d.kp_ = 2.0 * kPllDamping * wn;
    d.ki_ = wn * wn;
    d.kf_ = kFllGain;
    const auto len = std::max<std::size_t>(
        2, static_cast<std::size_t>(std::lround(config.sample_rate / kHrptBitRate)));
    d.chip_hist_.assign(len, 0.0F);
    d.timing_.emplace_back(config.sample_rate / kHrptBitRate, kTimingBandwidth);
    return d;
}

double HrptDecoder::carrier_hz() const {
    return freq_ * config_.sample_rate / (2.0 * std::numbers::pi);
}

void HrptDecoder::push(std::span<const dsp::Complex32> samples) {
    scratch_.resize(samples.size());
    const auto len = static_cast<float>(chip_hist_.size());
    for (std::size_t n = 0; n < samples.size(); ++n) {
        const dsp::Complex32 rot{static_cast<float>(std::cos(theta_)),
                                 static_cast<float>(-std::sin(theta_))};
        const dsp::Complex32 y = samples[n] * rot;
        const dsp::Complex32 d = y * std::conj(prev_);
        prev_ = y;
        const double fe = std::atan2(d.imag(), d.real());
        const double pe = std::atan2(y.imag(), y.real());
        // The residual carrier puts cos 68 degrees of the amplitude on the
        // in-phase arm once locked and none on average otherwise. The
        // frequency aid integrates noise without bound, so it runs only
        // until that lock measure comes up.
        const float mag = std::abs(y);
        lock_ += 1e-4 * ((mag > 0.0F ? y.real() / mag : 0.0F) - lock_);
        const bool aid = lock_ < kLockThreshold;
        freq_ += (aid ? kf_ * fe : 0.0) + ki_ * pe;
        theta_ += freq_ + kp_ * pe;
        theta_ = std::remainder(theta_, 2.0 * std::numbers::pi);

        // Split-phase matched filter over one bit of the quadrature arm,
        // where the data sits, KLM 4.1.2: the first half added, the second
        // subtracted. Its output peaks mid-bit like an NRZ pulse, so Gardner
        // timing runs on bits directly.
        chip_hist_[chip_pos_] = y.imag();
        chip_pos_ = (chip_pos_ + 1) % chip_hist_.size();
        const std::size_t taps = chip_hist_.size();
        float acc = 0.0F;
        for (std::size_t k = 0; k < taps; ++k) {
            // k = 0 is the oldest sample, the first half of the bit.
            const float v = chip_hist_[(chip_pos_ + k) % taps];
            if (2 * k + 1 < taps) {
                acc += v;
            } else if (2 * k + 1 > taps) {
                acc -= v;
            }
        }
        scratch_[n] = {acc / len, 0.0F};
    }
    for (const auto& s : timing_[0].push(scratch_)) {
        // "0" is +68 degrees first, a positive first half. Table 4.1.2-2.
        on_bit(s.symbol.real() > 0.0F ? 0 : 1, s.position);
    }
}

void HrptDecoder::on_bit(std::uint8_t bit, double position) {
    const std::uint64_t sync = hrpt_frame_sync();
    const double bit_samples = config_.sample_rate / kHrptBitRate;
    if (!in_frame_) {
        shift_ = ((shift_ << 1U) | bit) & kSyncMask;
        const auto e = static_cast<std::uint32_t>(std::popcount(shift_ ^ sync));
        const auto ei = static_cast<std::uint32_t>(std::popcount(shift_ ^ (~sync & kSyncMask)));
        if (images_open_) {
            ++missed_;  // counts bits while searching
            if (missed_ > config_.max_missed_frames * kFrameBits) {
                close_images(false);
            }
        }
        if (std::min(e, ei) > kAcquireErrors) {
            return;
        }
        in_frame_ = true;
        inverted_ = ei < e;
        sync_errors_ = std::min(e, ei);
        frame_sample_ = position - 59.5 * bit_samples;
        words_.fill(0);
        for (std::size_t w = 0; w < kHrptSyncWords; ++w) {
            const std::uint64_t v = (shift_ >> (50 - 10 * w)) & 0x3FFU;
            words_[w] = static_cast<std::uint16_t>(inverted_ ? (~v & 0x3FFU) : v);
        }
        frame_bits_ = 60;
        missed_ = 0;
        return;
    }
    const auto b = static_cast<std::uint16_t>(bit ^ (inverted_ ? 1U : 0U));
    if (frame_bits_ == 0) {
        frame_sample_ = position - 0.5 * bit_samples;
    }
    auto& w = words_[frame_bits_ / kHrptWordBits];
    w = static_cast<std::uint16_t>((w << 1U) | b);
    ++frame_bits_;
    if (frame_bits_ == 60 && frames_ > 0) {
        std::uint64_t got = 0;
        for (std::size_t i = 0; i < kHrptSyncWords; ++i) {
            got = (got << 10U) | words_[i];
        }
        sync_errors_ = static_cast<std::uint32_t>(std::popcount(got ^ sync));
        if (sync_errors_ > kHoldErrors) {
            in_frame_ = false;
            shift_ = inverted_ ? (~got & kSyncMask) : got;
            return;
        }
    }
    if (frame_bits_ == kFrameBits) {
        emit_frame();
        words_.fill(0);
        frame_bits_ = 0;
    }
}

void HrptDecoder::emit_frame() {
    HrptFrame f;
    f.index = frames_++;
    f.sample = frame_sample_;
    f.sync_errors = sync_errors_;
    f.inverted = inverted_;
    f.words = words_;
    // Word 7, bits numbered 1 (MSB) to 10. Table 4.1.3.1-1.
    const std::uint16_t id = words_[kHrptIdWord];
    f.minor_frame = static_cast<std::uint8_t>((id >> 7U) & 0x3U);
    f.spacecraft = static_cast<std::uint8_t>((id >> 3U) & 0xFU);
    f.channel_3a = (id & 1U) != 0;
    // Words 9 to 12.
    f.day = static_cast<std::uint16_t>(words_[kHrptTimeWord] >> 1U);
    f.msec = (static_cast<std::uint32_t>(words_[kHrptTimeWord + 1] & 0x7FU) << 20U) |
             (static_cast<std::uint32_t>(words_[kHrptTimeWord + 2]) << 10U) |
             words_[kHrptTimeWord + 3];
    if (events_.on_frame) {
        events_.on_frame(f);
    }

    if (!images_open_) {
        images_open_ = true;
        rows_ = 0;
        for (std::size_t ch = 0; ch < kHrptChannels; ++ch) {
            image_ids_[ch] = next_image_id_++;
            if (events_.image.on_open) {
                events_.image.on_open(ImageOpen{image_ids_[ch],
                                                static_cast<std::uint32_t>(kHrptEarthSamples),
                                                ImageRow::Gray8,
                                                "NOAA HRPT AVHRR " + std::to_string(ch + 1),
                                                kHrptFrameRate});
            }
        }
    }
    missed_ = 0;
    if (events_.image.on_row) {
        for (std::size_t ch = 0; ch < kHrptChannels; ++ch) {
            ImageRow row;
            row.image_id = image_ids_[ch];
            row.row = rows_;
            row.width = static_cast<std::uint32_t>(kHrptEarthSamples);
            row.model = ImageRow::Gray8;
            row.bytes.resize(kHrptEarthSamples);
            for (std::size_t s = 0; s < kHrptEarthSamples; ++s) {
                // 8 MSBs of the 10-bit sample.
                row.bytes[s] = static_cast<std::uint8_t>(
                    words_[kHrptEarthWord + kHrptChannels * s + ch] >> 2U);
            }
            events_.image.on_row(row);
        }
    }
    ++rows_;
}

void HrptDecoder::close_images(bool complete) {
    if (!images_open_) {
        return;
    }
    if (events_.image.on_close) {
        for (std::uint32_t id : image_ids_) {
            events_.image.on_close(ImageClose{id, rows_, complete});
        }
    }
    images_open_ = false;
}

void HrptDecoder::finish() {
    close_images(true);
}

}  // namespace revenant::decode
