#include "core/decode/zwave.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/decode/crc.h"

namespace revenant::decode {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

// Receiver structure, not from the Recommendation: about eight samples per
// symbol after decimation, which is plenty for an integrate-and-dump.
constexpr double kTargetSamplesPerSymbol = 8.0;
constexpr double kMinimumSamplesPerSymbol = 4.0;

std::size_t header_bytes(ZWaveLayout layout) {
    // Figures 8-5 and 8-6: HomeID 4, source 1, frame control 2, length 1, and
    // in configuration 3 a sequence number.
    return layout == ZWaveLayout::Config3 ? 9 : 8;
}

}  // namespace

double zwave_bit_rate(ZWaveRate rate) {
    switch (rate) {
        case ZWaveRate::R1:
            return kZWaveBitRateR1;
        case ZWaveRate::R2:
            return kZWaveBitRateR2;
        case ZWaveRate::R3:
            return kZWaveBitRateR3;
    }
    return kZWaveBitRateR2;
}

double zwave_symbol_rate(ZWaveRate rate) {
    // Table 7-2: Manchester doubles R1's 9.6 kbit/s to 19.2 kbaud.
    return rate == ZWaveRate::R1 ? 2.0 * kZWaveBitRateR1 : zwave_bit_rate(rate);
}

double zwave_separation_hz(ZWaveRate rate) {
    switch (rate) {
        case ZWaveRate::R1:
            return kZWaveSeparationR1Hz;
        case ZWaveRate::R2:
            return kZWaveSeparationR2Hz;
        case ZWaveRate::R3:
            return kZWaveSeparationR3Hz;
    }
    return kZWaveSeparationR2Hz;
}

double zwave_frequency_offset_hz(ZWaveRate rate) {
    return rate == ZWaveRate::R1 ? kZWaveFrequencyOffsetR1Hz : 0.0;
}

std::size_t zwave_max_frame_bytes(ZWaveRate rate) {
    return rate == ZWaveRate::R3 ? kZWaveMaxFrameR3 : kZWaveMaxFrameR1R2;
}

std::size_t zwave_fcs_bytes(ZWaveRate rate) { return rate == ZWaveRate::R3 ? 2 : 1; }

std::uint8_t zwave_checksum(std::span<const std::uint8_t> bytes) {
    std::uint8_t sum = kZWaveChecksumInitial;
    for (const std::uint8_t byte : bytes) {
        sum = static_cast<std::uint8_t>(sum ^ byte);
    }
    return sum;
}

std::uint16_t zwave_crc16(std::span<const std::uint8_t> bytes) {
    static constexpr Crc kCrc(CrcParams{16, kZWaveCrcPolynomial, kZWaveCrcInitial, false, false, 0});
    return static_cast<std::uint16_t>(kCrc.compute(bytes));
}

ZWaveFrameControl zwave_parse_frame_control(std::uint16_t raw, ZWaveLayout layout) {
    ZWaveFrameControl fc;
    fc.raw = raw;
    const auto b5 = static_cast<std::uint8_t>(raw >> 8U);
    const auto b6 = static_cast<std::uint8_t>(raw & 0xFFU);
    fc.header_type = static_cast<std::uint8_t>(b5 & 0x0FU);
    if (layout == ZWaveLayout::Config12) {
        // Figure 8-11.
        fc.routed = (b5 & 0x80U) != 0U;
        fc.ack_request = (b5 & 0x40U) != 0U;
        fc.low_power = (b5 & 0x20U) != 0U;
        fc.speed_modified = (b5 & 0x10U) != 0U;
        fc.beaming = static_cast<std::uint8_t>((b6 >> 5U) & 0x3U);
        fc.sequence = static_cast<std::uint8_t>(b6 & 0x0FU);
    } else {
        // Figure 8-12.
        fc.ack_request = (b5 & 0x80U) != 0U;
        fc.low_power = (b5 & 0x40U) != 0U;
        fc.beaming = static_cast<std::uint8_t>((b6 >> 4U) & 0x7U);
    }
    return fc;
}

std::vector<std::uint8_t> zwave_multicast_nodes(std::uint8_t address_offset,
                                                std::span<const std::uint8_t> mask) {
    std::vector<std::uint8_t> nodes;
    // Table 8-16: the bit address offset is 32 per step of the field.
    const unsigned base = 32U * (address_offset & 0x7U);
    for (std::size_t byte = 0; byte < mask.size(); ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            if ((mask[byte] >> bit) & 1U) {
                const unsigned node = base + static_cast<unsigned>(byte) * 8U + bit + 1U;
                if (node <= 0xFFU) {
                    nodes.push_back(static_cast<std::uint8_t>(node));
                }
            }
        }
    }
    return nodes;
}

Expected<ZWaveMpdu> zwave_parse_mpdu(std::span<const std::uint8_t> bytes, ZWaveRate rate,
                                     ZWaveLayout layout) {
    const std::size_t head = header_bytes(layout);
    const std::size_t fcs_len = zwave_fcs_bytes(rate);
    if (bytes.size() < head + 1 + fcs_len) {
        return fail("Z-Wave MPDU too short");
    }
    ZWaveMpdu m;
    // Figure 8-9: HomeID byte 0 is the most significant.
    m.home_id = (std::uint32_t{bytes[0]} << 24U) | (std::uint32_t{bytes[1]} << 16U) |
                (std::uint32_t{bytes[2]} << 8U) | std::uint32_t{bytes[3]};
    m.source = bytes[4];
    m.frame_control = zwave_parse_frame_control(
        static_cast<std::uint16_t>((bytes[5] << 8U) | bytes[6]), layout);
    m.length = bytes[7];
    if (m.length != bytes.size()) {
        return fail("Z-Wave length field disagrees with the frame");
    }
    if (layout == ZWaveLayout::Config3) {
        m.sequence_number = bytes[8];
    }
    const std::size_t body_end = bytes.size() - fcs_len;
    std::size_t pos = head;
    if (m.frame_control.header_type == kZWaveHeaderMulticast) {
        // Figure 8-14: address offset in bits 7..5, mask byte count in 4..0.
        const std::uint8_t control = bytes[pos++];
        m.multicast_offset = static_cast<std::uint8_t>(control >> 5U);
        const std::size_t count = control & 0x1FU;
        if (count == 0 || count > kZWaveMaxMaskBytes || pos + count > body_end) {
            return fail("Z-Wave multicast mask does not fit");
        }
        m.multicast_mask.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                                bytes.begin() + static_cast<std::ptrdiff_t>(pos + count));
        m.multicast_nodes = zwave_multicast_nodes(m.multicast_offset, m.multicast_mask);
        pos += count;
    } else {
        m.destination = bytes[pos++];
    }
    m.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                     bytes.begin() + static_cast<std::ptrdiff_t>(body_end));
    const auto covered = bytes.first(body_end);
    if (fcs_len == 1) {
        m.fcs = bytes[body_end];
        m.fcs_valid = zwave_checksum(covered) == m.fcs;
    } else {
        m.fcs = static_cast<std::uint16_t>((bytes[body_end] << 8U) | bytes[body_end + 1]);
        m.fcs_valid = zwave_crc16(covered) == m.fcs;
    }
    return m;
}

Expected<std::vector<std::uint8_t>> zwave_build_mpdu(const ZWaveMpduFields& f, ZWaveRate rate,
                                                     ZWaveLayout layout) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(f.home_id >> 24U));
    out.push_back(static_cast<std::uint8_t>(f.home_id >> 16U));
    out.push_back(static_cast<std::uint8_t>(f.home_id >> 8U));
    out.push_back(static_cast<std::uint8_t>(f.home_id));
    out.push_back(f.source);
    out.push_back(static_cast<std::uint8_t>(f.frame_control >> 8U));
    out.push_back(static_cast<std::uint8_t>(f.frame_control));
    out.push_back(0);  // length, below
    if (layout == ZWaveLayout::Config3) {
        out.push_back(f.sequence_number);
    }
    if (f.destination.empty()) {
        return fail("Z-Wave MPDU needs a destination");
    }
    out.insert(out.end(), f.destination.begin(), f.destination.end());
    out.insert(out.end(), f.payload.begin(), f.payload.end());
    const std::size_t total = out.size() + zwave_fcs_bytes(rate);
    if (total > zwave_max_frame_bytes(rate)) {
        return fail("Z-Wave MPDU longer than Table 7-27 allows");
    }
    out[7] = static_cast<std::uint8_t>(total);
    if (rate == ZWaveRate::R3) {
        const std::uint16_t crc = zwave_crc16(out);
        out.push_back(static_cast<std::uint8_t>(crc >> 8U));
        out.push_back(static_cast<std::uint8_t>(crc));
    } else {
        out.push_back(zwave_checksum(out));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Receiver
// ---------------------------------------------------------------------------

Expected<ZWave> ZWave::create(const ZWaveConfig& config) {
    if (config.sample_rate <= 0) {
        return fail("Z-Wave sample rate must be positive");
    }
    const double fs = static_cast<double>(config.sample_rate);
    const double symbol_rate = zwave_symbol_rate(config.rate);
    if (fs / symbol_rate < kMinimumSamplesPerSymbol) {
        return fail("Z-Wave sample rate too low for the data rate");
    }
    ZWave z;
    z.config_ = config;
    z.decimation_ = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::floor(fs / (symbol_rate * kTargetSamplesPerSymbol))));
    z.output_rate_ = fs / static_cast<double>(z.decimation_);
    z.samples_per_symbol_ = z.output_rate_ / symbol_rate;
    z.average_length_ =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(z.samples_per_symbol_)));

    // Channel filter: one-sided bandwidth of the deviation plus the symbol
    // rate times 0.6, chosen by measurement against the PER test, not from
    // the Recommendation. Hamming-windowed sinc, unit DC gain.
    const double cutoff =
        std::min(zwave_separation_hz(config.rate) / 2.0 + 0.6 * symbol_rate, 0.45 * z.output_rate_);
    std::size_t taps = static_cast<std::size_t>(std::ceil(4.0 * fs / cutoff)) | 1U;
    taps = std::clamp<std::size_t>(taps, 15, 511) | 1U;
    z.taps_.resize(taps);
    const double mid = static_cast<double>(taps - 1) / 2.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < taps; ++i) {
        const double t = static_cast<double>(i) - mid;
        const double x = kTwoPi * cutoff / fs * t;
        const double sinc = t == 0.0 ? 1.0 : std::sin(x) / x;
        const double window =
            0.54 - 0.46 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(taps - 1));
        z.taps_[i] = static_cast<float>(sinc * window);
        sum += sinc * window;
    }
    for (float& tap : z.taps_) {
        tap = static_cast<float>(tap / sum);
    }

    // The sync pattern: the last preamble byte then the SOF, 7.1.3.2 and
    // Table 7-11, most significant bit first. +1 is the upper tone. Table 7-5:
    // NRZ 0 is the upper tone. Table 7-6: Manchester 1 runs upper then lower,
    // 0 lower then upper.
    for (const std::uint8_t byte : {kZWavePreambleByte, kZWaveSof}) {
        for (int bit = 7; bit >= 0; --bit) {
            const bool one = ((byte >> bit) & 1U) != 0U;
            if (config.rate == ZWaveRate::R1) {
                z.pattern_.push_back(one ? 1.0F : -1.0F);
                z.pattern_.push_back(one ? -1.0F : 1.0F);
            } else {
                z.pattern_.push_back(one ? -1.0F : 1.0F);
            }
        }
    }
    z.mix_step_ = -kTwoPi * (config.channel_offset_hz + zwave_frequency_offset_hz(config.rate)) / fs;
    z.discriminator_scale_ = z.output_rate_ / kTwoPi;
    z.history_.assign(taps, {0.0F, 0.0F});
    return z;
}

void ZWave::reset() {
    input_index_ = 0;
    mix_phase_ = 0.0;
    std::fill(history_.begin(), history_.end(), std::complex<float>{0.0F, 0.0F});
    history_pos_ = 0;
    decimation_phase_ = 0;
    previous_ = {1.0F, 0.0F};
    raw_.clear();
    averaged_.clear();
    base_ = 0;
    running_sum_ = 0.0;
    search_ = 0;
}

Status ZWave::process(dsp::ConstComplexSpan samples, std::vector<ZWaveFrame>& out) {
    const std::size_t taps = taps_.size();
    for (const auto& sample : samples) {
        const std::complex<float> rotator(static_cast<float>(std::cos(mix_phase_)),
                                          static_cast<float>(std::sin(mix_phase_)));
        mix_phase_ += mix_step_;
        if (mix_phase_ > std::numbers::pi) {
            mix_phase_ -= kTwoPi;
        } else if (mix_phase_ < -std::numbers::pi) {
            mix_phase_ += kTwoPi;
        }
        history_[history_pos_] = sample * rotator;
        history_pos_ = (history_pos_ + 1) % taps;
        ++input_index_;
        if (++decimation_phase_ < decimation_) {
            continue;
        }
        decimation_phase_ = 0;
        std::complex<float> y{0.0F, 0.0F};
        std::size_t k = history_pos_;  // oldest
        for (std::size_t i = 0; i < taps; ++i) {
            y += history_[k] * taps_[i];
            k = k + 1 == taps ? 0 : k + 1;
        }
        const float f =
            static_cast<float>(std::arg(y * std::conj(previous_)) * discriminator_scale_);
        previous_ = y;
        raw_.push_back(f);
        running_sum_ += f;
        // Trimming in run() always leaves average_length_ values behind.
        if (raw_.size() > average_length_) {
            running_sum_ -= raw_[raw_.size() - 1 - average_length_];
        }
        averaged_.push_back(static_cast<float>(running_sum_ / static_cast<double>(average_length_)));
    }
    run(out);
    return {};
}

float ZWave::sample_at(double position) const {
    const double rel = position - static_cast<double>(base_);
    const auto i = static_cast<std::size_t>(std::floor(rel));
    const double frac = rel - std::floor(rel);
    if (i + 1 >= averaged_.size()) {
        return averaged_.back();
    }
    return static_cast<float>((1.0 - frac) * averaged_[i] + frac * averaged_[i + 1]);
}

int ZWave::bit_at(std::size_t bit, double origin, float dc) const {
    const double sps = samples_per_symbol_;
    if (config_.rate == ZWaveRate::R1) {
        const double first = origin + static_cast<double>(2 * bit + 1) * sps;
        return sample_at(first) - sample_at(first + sps) > 0.0F ? 1 : 0;
    }
    return sample_at(origin + static_cast<double>(bit + 1) * sps) - dc < 0.0F ? 1 : 0;
}

void ZWave::run(std::vector<ZWaveFrame>& out) {
    const std::size_t p = pattern_.size();
    const double sps = samples_per_symbol_;
    const double span = static_cast<double>(p - 1) * sps;
    const double chips_per_bit = config_.rate == ZWaveRate::R1 ? 2.0 : 1.0;
    const SampleIndex earliest =
        base_ + static_cast<SampleIndex>(std::ceil(span)) + average_length_ + 1;
    search_ = std::max(search_, earliest);

    const auto total = [&] { return base_ + averaged_.size(); };
    // Whether every sample up to `position` has arrived.
    const auto have = [&](double position) {
        return static_cast<double>(total()) > std::ceil(position) + 1.0;
    };
    std::vector<float> values(p);
    const auto correlate = [&](SampleIndex end, float* mean_out) {
        double mean = 0.0;
        for (std::size_t k = 0; k < p; ++k) {
            values[k] = sample_at(static_cast<double>(end) - static_cast<double>(p - 1 - k) * sps);
            mean += values[k];
        }
        mean /= static_cast<double>(p);
        double dot = 0.0;
        double energy = 0.0;
        for (std::size_t k = 0; k < p; ++k) {
            const double v = values[k] - mean;
            dot += pattern_[k] * v;
            energy += v * v;
        }
        if (mean_out != nullptr) {
            *mean_out = static_cast<float>(mean);
        }
        return energy > 0.0 ? static_cast<float>(dot / std::sqrt(energy * static_cast<double>(p)))
                            : 0.0F;
    };

    const std::size_t head = header_bytes(config_.layout);
    const auto peak_window = static_cast<SampleIndex>(std::ceil(sps));
    while (true) {
        if (!have(static_cast<double>(search_ + peak_window))) {
            break;
        }
        const float c = correlate(search_, nullptr);
        if (c < config_.sync_threshold) {
            ++search_;
            continue;
        }
        SampleIndex best = search_;
        float best_c = c;
        for (SampleIndex s = search_ + 1; s <= search_ + peak_window; ++s) {
            const float cs = correlate(s, nullptr);
            if (cs > best_c) {
                best_c = cs;
                best = s;
            }
        }
        float dc = 0.0F;
        (void)correlate(best, &dc);
        const auto origin = static_cast<double>(best);
        const auto bit_end = [&](std::size_t bits) {
            return origin + static_cast<double>(bits) * chips_per_bit * sps;
        };
        if (!have(bit_end(head * 8))) {
            break;
        }
        const auto read_byte = [&](std::size_t index) {
            unsigned v = 0;
            for (std::size_t b = 0; b < 8; ++b) {
                v = (v << 1U) | static_cast<unsigned>(bit_at(index * 8 + b, origin, dc));
            }
            return static_cast<std::uint8_t>(v);
        };
        const std::size_t length = read_byte(7);
        if (length < head + 1 + zwave_fcs_bytes(config_.rate) ||
            length > zwave_max_frame_bytes(config_.rate)) {
            ++search_;
            continue;
        }
        if (!have(bit_end(length * 8))) {
            break;
        }
        std::vector<std::uint8_t> bytes(length);
        for (std::size_t i = 0; i < length; ++i) {
            bytes[i] = read_byte(i);
        }
        auto mpdu = zwave_parse_mpdu(bytes, config_.rate, config_.layout);
        if (!mpdu.has_value() || (!mpdu->fcs_valid && !config_.keep_bad_frames)) {
            ++search_;
            continue;
        }
        ZWaveFrame frame;
        frame.rate = config_.rate;
        // Back to input samples: the decimated index, less the channel
        // filter's group delay and the moving average's.
        const double delay = static_cast<double>(taps_.size() - 1) / 2.0 +
                             static_cast<double>(average_length_ - 1) / 2.0 *
                                 static_cast<double>(decimation_);
        const double input_pos =
            static_cast<double>(best + 1) * static_cast<double>(decimation_) - 1.0 - delay;
        frame.sof_end_sample = static_cast<SampleIndex>(std::max(0.0, input_pos));
        frame.sync_correlation = best_c;
        // The pattern is balanced, so its mean is the carrier error, already
        // in hertz from the discriminator.
        frame.frequency_error_hz = static_cast<double>(dc);
        frame.bytes = std::move(bytes);
        frame.mpdu = std::move(*mpdu);
        out.push_back(std::move(frame));
        search_ = static_cast<SampleIndex>(std::ceil(bit_end(length * 8)));
    }

    // Keep what the next search and the moving average still need.
    const auto keep = static_cast<SampleIndex>(std::ceil(span)) + average_length_ + 4;
    if (search_ > base_ + keep) {
        const auto drop = static_cast<std::size_t>(
            std::min<SampleIndex>(search_ - keep - base_, averaged_.size()));
        if (drop > 4096) {
            raw_.erase(raw_.begin(), raw_.begin() + static_cast<std::ptrdiff_t>(drop));
            averaged_.erase(averaged_.begin(), averaged_.begin() + static_cast<std::ptrdiff_t>(drop));
            base_ += drop;
        }
    }
}

}  // namespace revenant::decode
