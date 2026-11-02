#include "core/decode/opulent_voice.h"

#include <cmath>
#include <format>

#include "core/decode/dv_codes.h"

namespace revenant::decode {
namespace {

ConvolutionalCode conv_code() {
    ConvolutionalCode code;
    code.memory = kOpvConvMemory;
    code.generators = kOpvConvGenerators;
    return code;
}

// OPV 2.3's table.
int station_value(char c) {
    if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 1;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 27;
    }
    switch (c) {
        case '-':
            return 37;
        case '/':
            return 38;
        case '.':
            return 39;
        default:
            return -1;
    }
}

char station_char(unsigned value) {
    if (value >= 1 && value <= 26) {
        return static_cast<char>('A' + value - 1);
    }
    if (value >= 27 && value <= 36) {
        return static_cast<char>('0' + value - 27);
    }
    switch (value) {
        case 37:
            return '-';
        case 38:
            return '/';
        case 39:
            return '.';
        default:
            return '?';
    }
}

std::vector<float> sync_pattern() {
    std::vector<float> out;
    for (std::size_t i = kOpvSyncBits; i-- > 0;) {
        out.push_back(((kOpvSync >> i) & 1U) != 0U ? -1.0F : 1.0F);
    }
    return out;
}

}  // namespace

Expected<std::uint64_t> opv_encode_station(std::string_view station) {
    std::uint64_t value = 0;
    std::uint64_t place = 1;
    constexpr std::uint64_t limit = (std::uint64_t{1} << 48U) - 1U;
    for (const char c : station) {
        const int digit = station_value(c);
        if (digit < 0) {
            return fail(std::format("'{}' is not in the OPV 2.3 base-40 table", c));
        }
        if (place > limit) {
            return fail(std::format("\"{}\" does not fit the 48 bits of OPV 2.3", station));
        }
        const std::uint64_t add = static_cast<std::uint64_t>(digit) * place;
        if (add > limit - value) {
            return fail(std::format("\"{}\" does not fit the 48 bits of OPV 2.3", station));
        }
        value += add;
        place *= 40U;
    }
    return value;
}

std::string opv_decode_station(std::uint64_t value) {
    std::string out;
    while (value != 0) {
        const auto digit = static_cast<unsigned>(value % 40U);
        value /= 40U;
        // 0 is "not used" (OPV 2.3); skip rather than end, so a value with an
        // interior zero still shows the characters around it.
        if (digit != 0) {
            out.push_back(station_char(digit));
        }
    }
    return out;
}

void opv_randomize(std::span<std::uint8_t> bytes) {
    // OPV 8.4: output the top bit, feed back s7 ^ s6 ^ s4 ^ s2 into the
    // bottom, eight clocks per byte, most significant bit first.
    unsigned state = kOpvRandomizerSeed;
    for (std::uint8_t& byte : bytes) {
        unsigned mask = 0;
        for (int i = 0; i < 8; ++i) {
            const unsigned out = (state >> 7U) & 1U;
            const unsigned feedback =
                ((state >> 7U) ^ (state >> 6U) ^ (state >> 4U) ^ (state >> 2U)) & 1U;
            state = ((state << 1U) | feedback) & 0xFFU;
            mask = (mask << 1U) | out;
        }
        byte = static_cast<std::uint8_t>(byte ^ mask);
    }
}

std::size_t opv_interleave_position(std::size_t p) {
    return (p % kOpvInterleaveColumns) * kOpvInterleaveRows + p / kOpvInterleaveColumns;
}

Expected<std::vector<std::uint8_t>> opv_encode_frame(std::span<const std::uint8_t> frame) {
    if (frame.size() != kOpvFrameBytes) {
        return fail(std::format("an OPV frame is {} bytes (OPV 2.1); got {}", kOpvFrameBytes,
                                frame.size()));
    }
    std::vector<std::uint8_t> randomized(frame.begin(), frame.end());
    opv_randomize(randomized);
    std::vector<std::uint8_t> bits;
    bits.reserve(kOpvFrameBytes * 8);
    for (const std::uint8_t byte : randomized) {
        for (int i = 7; i >= 0; --i) {
            bits.push_back(static_cast<std::uint8_t>((byte >> i) & 1U));
        }
    }
    // OPV 8.5: unterminated, no tail.
    auto coded = convolutional_encode(conv_code(), bits);
    if (!coded) {
        return std::unexpected(coded.error());
    }
    std::vector<std::uint8_t> out;
    out.reserve(kOpvFrameBits);
    for (std::size_t i = kOpvSyncBits; i-- > 0;) {
        out.push_back(static_cast<std::uint8_t>((kOpvSync >> i) & 1U));
    }
    std::vector<std::uint8_t> interleaved(kOpvEncodedBits, 0);
    for (std::size_t p = 0; p < kOpvEncodedBits; ++p) {
        interleaved[opv_interleave_position(p)] = (*coded)[p];
    }
    out.insert(out.end(), interleaved.begin(), interleaved.end());
    return out;
}

OpvHeader opv_parse_header(std::span<const std::uint8_t> frame) {
    OpvHeader out;
    if (frame.size() < kOpvHeaderBytes) {
        return out;
    }
    for (std::size_t i = 0; i < 6; ++i) {
        out.station_value = (out.station_value << 8U) | frame[i];
    }
    out.station = opv_decode_station(out.station_value);
    out.token = (static_cast<std::uint32_t>(frame[6]) << 16U) |
                (static_cast<std::uint32_t>(frame[7]) << 8U) | frame[8];
    out.reserved = (static_cast<std::uint32_t>(frame[9]) << 16U) |
                   (static_cast<std::uint32_t>(frame[10]) << 8U) | frame[11];
    return out;
}

Expected<OpvDeframer> OpvDeframer::create(const OpvConfig& config) {
    if (!(config.hunting_threshold > 0.0 && config.hunting_threshold <= 1.0) ||
        !(config.locked_threshold > 0.0 && config.locked_threshold <= 1.0)) {
        return fail("OPV sync thresholds must lie in (0, 1]");
    }
    OpvDeframer deframer;
    deframer.config_ = config;
    return deframer;
}

void OpvDeframer::reset() {
    buffer_.clear();
    buffer_start_ = 0;
    cursor_ = 0;
    locked_ = false;
    inverted_ = false;
}

double OpvDeframer::score_at(std::size_t offset) const {
    static const std::vector<float> pattern = sync_pattern();
    double dot = 0.0;
    double energy = 0.0;
    for (std::size_t i = 0; i < kOpvSyncBits; ++i) {
        const double v = static_cast<double>(buffer_[offset + i]);
        dot += v * static_cast<double>(pattern[i]);
        energy += std::abs(v);
    }
    return energy > 0.0 ? dot / energy : 0.0;
}

std::optional<OpvFrame> OpvDeframer::decode_at(std::size_t offset, double score) {
    const float sign = score < 0.0 ? -1.0F : 1.0F;
    std::vector<float> soft(kOpvEncodedBits);
    for (std::size_t p = 0; p < kOpvEncodedBits; ++p) {
        soft[p] = sign * buffer_[offset + kOpvSyncBits + opv_interleave_position(p)];
    }
    auto bits = viterbi_decode(conv_code(), soft, false);
    if (!bits || bits->size() < kOpvFrameBytes * 8) {
        return std::nullopt;
    }
    auto recoded = convolutional_encode(conv_code(), *bits);
    if (!recoded) {
        return std::nullopt;
    }
    std::size_t agree = 0;
    for (std::size_t p = 0; p < kOpvEncodedBits; ++p) {
        const std::uint8_t hard = soft[p] < 0.0F ? 1U : 0U;
        agree += hard == (*recoded)[p] ? 1U : 0U;
    }
    OpvFrame frame;
    frame.first_bit = buffer_start_ + offset;
    frame.sync_score = std::abs(score);
    frame.inverted = sign < 0.0F;
    frame.agreement = static_cast<double>(agree) / static_cast<double>(kOpvEncodedBits);
    if (frame.agreement < config_.agreement_threshold) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < kOpvFrameBytes; ++i) {
        unsigned byte = 0;
        for (std::size_t b = 0; b < 8; ++b) {
            byte = (byte << 1U) | (*bits)[i * 8 + b];
        }
        frame.bytes[i] = static_cast<std::uint8_t>(byte);
    }
    opv_randomize(frame.bytes);
    frame.header = opv_parse_header(frame.bytes);
    return frame;
}

Status OpvDeframer::process(std::span<const float> soft, std::vector<OpvFrame>& out) {
    buffer_.insert(buffer_.end(), soft.begin(), soft.end());
    while (cursor_ + kOpvFrameBits <= buffer_.size()) {
        if (locked_) {
            // OPV 8.2 flywheel: the next sync is due exactly here.
            const double score = score_at(cursor_);
            const double signed_score = inverted_ ? -score : score;
            std::optional<OpvFrame> frame;
            if (signed_score >= config_.locked_threshold) {
                frame = decode_at(cursor_, inverted_ ? -1.0 : 1.0);
            }
            if (frame) {
                frame->sync_score = signed_score;
                out.push_back(*frame);
                cursor_ += kOpvFrameBits;
                continue;
            }
            locked_ = false;
        }
        const double score = score_at(cursor_);
        if (std::abs(score) >= config_.hunting_threshold) {
            if (auto frame = decode_at(cursor_, score)) {
                out.push_back(*frame);
                locked_ = true;
                inverted_ = frame->inverted;
                cursor_ += kOpvFrameBits;
                continue;
            }
        }
        ++cursor_;
    }
    if (cursor_ > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(cursor_));
        buffer_start_ += cursor_;
        cursor_ = 0;
    }
    return {};
}

}  // namespace revenant::decode
