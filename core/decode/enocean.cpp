#include "core/decode/enocean.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace revenant::decode {

std::uint8_t enocean_checksum(std::span<const std::uint8_t> bytes) {
    // ERP1 clause 4.3.2.
    unsigned sum = 0;
    for (const std::uint8_t byte : bytes) {
        sum += byte;
    }
    return static_cast<std::uint8_t>(sum & 0xFFU);
}

std::uint8_t enocean_crc8(std::span<const std::uint8_t> bytes) {
    // ERP1 clause 4.3.3: generator x^8 + x^2 + x + 1. The remainder of x^8
    // times the running value, XORed with each next byte, is the ordinary
    // MSB-first CRC with a zero start.
    constexpr unsigned kPoly = 0x07U;
    unsigned remainder = 0;
    for (const std::uint8_t byte : bytes) {
        remainder ^= byte;
        for (int i = 0; i < 8; ++i) {
            remainder = (remainder & 0x80U) != 0 ? ((remainder << 1U) ^ kPoly) : (remainder << 1U);
            remainder &= 0xFFU;
        }
    }
    return static_cast<std::uint8_t>(remainder);
}

std::uint16_t enocean_encode_byte(std::uint8_t byte, bool last) {
    // ERP1 clause 1.7 coding table, transmitted left to right:
    // bit7 bit6 bit5 !bit5 bit4 bit3 bit2 !bit2 bit1 bit0 Sync !Sync.
    const auto bit = [byte](unsigned n) { return (static_cast<unsigned>(byte) >> n) & 1U; };
    unsigned out = 0;
    const auto put = [&out](unsigned b) { out = (out << 1U) | (b & 1U); };
    put(bit(7));
    put(bit(6));
    put(bit(5));
    put(bit(5) ^ 1U);
    put(bit(4));
    put(bit(3));
    put(bit(2));
    put(bit(2) ^ 1U);
    put(bit(1));
    put(bit(0));
    const unsigned pair = last ? kEnoceanSyncEof : kEnoceanSyncMore;
    put(pair >> 1U);
    put(pair);
    return static_cast<std::uint16_t>(out);
}

Expected<EnoceanDecoder> EnoceanDecoder::create(const EnoceanConfig& config) {
    if (config.bit_rate <= 0.0) {
        return fail("EnOcean: bit rate must be positive");
    }
    const double sps = static_cast<double>(config.sample_rate) / config.bit_rate;
    if (sps < 3.0) {
        return fail("EnOcean: sample rate must give at least 3 samples per bit");
    }
    EnoceanDecoder decoder;
    decoder.config_ = config;
    decoder.samples_per_bit_ = sps;
    // A boxcar half a bit long: long enough to average noise, short enough
    // that every bit still reaches full level before its middle.
    decoder.box_length_ = std::max<std::size_t>(1, static_cast<std::size_t>(sps / 2.0));
    decoder.box_.assign(decoder.box_length_, 0.0F);
    // The high level decays over about 400 bits so the gaps between the
    // subtelegrams of one telegram (clause 4.2, a few ms) do not lose it,
    // while a later, weaker transmitter is heard within a few ms.
    decoder.high_decay_ = 1.0 / (400.0 * sps);
    decoder.low_rise_ = 1.0 / (400.0 * sps);
    return decoder;
}

void EnoceanDecoder::reset() {
    *this = create(config_).value_or(EnoceanDecoder{});
}

void EnoceanDecoder::process(dsp::ConstComplexSpan samples, std::vector<EnoceanTelegram>& out) {
    const double step = 1.0 / samples_per_bit_;
    for (const dsp::Complex32 sample : samples) {
        const float magnitude = std::abs(sample);
        box_sum_ += static_cast<double>(magnitude) - static_cast<double>(box_[box_pos_]);
        box_[box_pos_] = magnitude;
        box_pos_ = (box_pos_ + 1) % box_length_;
        const double level = box_sum_ / static_cast<double>(box_length_);

        if (level > high_) {
            high_ = level;
        } else {
            high_ -= high_ * high_decay_;
        }
        if (level < low_) {
            low_ = level;
        } else {
            low_ += (level - low_) * low_rise_;
        }
        const double threshold = 0.5 * (high_ + low_);
        const bool high = level > threshold;

        if (high != level_high_) {
            // A transition marks a bit boundary: pull the phase towards zero.
            double error = phase_;
            if (error > 0.5) {
                error -= 1.0;
            }
            phase_ -= 0.4 * error;
            if (phase_ < 0.0) {
                phase_ += 1.0;
            }
            level_high_ = high;
        }

        const double before = phase_;
        phase_ += step;
        if (before < 0.5 && phase_ >= 0.5) {
            // Clause 1.7: a one is low power, a zero high power.
            on_bit(level_high_ ? 0U : 1U, out);
        }
        if (phase_ >= 1.0) {
            phase_ -= 1.0;
        }
        ++sample_;
    }
}

void EnoceanDecoder::on_bit(unsigned bit, std::vector<EnoceanTelegram>& out) {
    if (!in_frame_) {
        shift_ = ((shift_ << 1U) | bit) & 0xFFFU;
        const std::uint32_t diff = shift_ ^ kEnoceanPreambleSof;
        // The SOF must match exactly; one preamble bit may be lost to the
        // receiver settling on the first transition.
        if ((diff & 0xFU) == 0 && std::popcount(diff) <= 1) {
            in_frame_ = true;
            group_bits_ = 0;
            group_ = 0;
            bytes_.clear();
            frame_sample_ = sample_;
            frame_level_ = static_cast<float>(high_);
        }
        return;
    }

    group_ = static_cast<std::uint16_t>((group_ << 1U) | bit);
    if (++group_bits_ < kEnoceanEncodedBits) {
        return;
    }
    const unsigned g = group_;
    group_bits_ = 0;
    group_ = 0;
    // Clause 1.7: positions 8 and 4 carry the inverse of 9 and 5.
    const bool inverse_ok = (((g >> 9U) ^ (g >> 8U)) & 1U) == 1U && (((g >> 5U) ^ (g >> 4U)) & 1U) == 1U;
    const unsigned pair = g & 0x3U;
    if (!inverse_ok || (pair != kEnoceanSyncMore && pair != kEnoceanSyncEof)) {
        in_frame_ = false;
        shift_ = 0;
        return;
    }
    const unsigned byte = (((g >> 9U) & 0x7U) << 5U) | (((g >> 5U) & 0x7U) << 2U) | ((g >> 2U) & 0x3U);
    bytes_.push_back(static_cast<std::uint8_t>(byte));
    if (pair == kEnoceanSyncEof) {
        finish(out);
        in_frame_ = false;
        shift_ = 0;
    } else if (bytes_.size() >= kEnoceanMaxBytes) {
        in_frame_ = false;
        shift_ = 0;
    }
}

void EnoceanDecoder::finish(std::vector<EnoceanTelegram>& out) {
    const std::size_t n = bytes_.size();
    if (n < kEnoceanMinBytes || n > kEnoceanMaxBytes) {
        return;
    }
    const std::span<const std::uint8_t> body(bytes_.data(), n - 1);
    const std::uint8_t status = bytes_[n - 2];
    // Table 3: STATUS bit 7 selects the CRC.
    const bool crc = (status & 0x80U) != 0;
    const std::uint8_t expected = crc ? enocean_crc8(body) : enocean_checksum(body);
    if (expected != bytes_[n - 1]) {
        return;
    }

    EnoceanTelegram t;
    t.sample = frame_sample_;
    t.bytes = bytes_;
    t.rorg = bytes_[0];
    t.inner_rorg = t.rorg;
    t.status = status;
    t.crc8 = crc;
    // Table 4: repeater level in STATUS bits 0 to 3.
    t.repeater_level = status & 0x0FU;
    t.level = frame_level_;
    // Figure 1: TXID is the four bytes before STATUS.
    for (std::size_t i = n - 6; i < n - 2; ++i) {
        t.sender_id = (t.sender_id << 8U) | bytes_[i];
    }
    std::size_t data_begin = 1;
    std::size_t data_end = n - 6;
    // Clause 5.3.2, Figure 3: 0xA6, the original RORG, DATA, DESTID, TXID.
    if (t.rorg == kEnoceanRorgAddressed && n >= kEnoceanMinBytes + 5) {
        t.inner_rorg = bytes_[1];
        data_begin = 2;
        data_end = n - 10;
        std::uint32_t dest = 0;
        for (std::size_t i = n - 10; i < n - 6; ++i) {
            dest = (dest << 8U) | bytes_[i];
        }
        t.destination_id = dest;
    }
    t.data.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(data_begin),
                  bytes_.begin() + static_cast<std::ptrdiff_t>(data_end));

    const auto maturity = static_cast<dsp::SampleIndex>(kEnoceanRxMaturitySeconds *
                                                        static_cast<double>(config_.sample_rate));
    t.repeat = have_last_ && last_bytes_ == bytes_ && t.sample - last_sample_ <= maturity;
    if (!t.repeat) {
        last_sample_ = t.sample;
    }
    last_bytes_ = bytes_;
    have_last_ = true;
    out.push_back(std::move(t));
}

}  // namespace revenant::decode
