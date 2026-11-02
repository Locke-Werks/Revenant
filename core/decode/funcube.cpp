#include "core/decode/funcube.h"

#include <algorithm>
#include <cmath>

#include "core/decode/viterbi_stream.h"

namespace revenant::decode {

namespace {

// AO40 "Reed-Solomon Encoding": (160,128) from (255,223), even and odd
// octets, zero padding of 95 symbols, conventional basis.
constexpr CcsdsRsConfig kRs{16, 2, 190, false};

const ConvolutionalCode& code() {
    static constexpr std::array<std::uint32_t, 2> kGenerators{kCcsdsK7G1, kCcsdsK7G2};
    static const ConvolutionalCode c{kCcsdsK7Memory, kGenerators};
    return c;
}

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> octets) : octets_(octets) {}
    std::uint32_t take(unsigned bits) {
        std::uint32_t v = 0;
        for (unsigned i = 0; i < bits; ++i, ++pos_) {
            v = (v << 1U) | ((octets_[pos_ / 8] >> (7U - (pos_ % 8U))) & 1U);
        }
        return v;
    }

private:
    std::span<const std::uint8_t> octets_;
    std::size_t pos_ = 0;
};

}  // namespace

std::array<std::uint8_t, kFuncubeColumns> funcube_sync_vector() {
    // s(x) = x^7 + x^3 + 1, so s[n+7] = s[n+3] + s[n]; seven ones first. The
    // tests compare it with the 65 bits AO40 prints.
    std::array<std::uint8_t, kFuncubeColumns> s{};
    for (std::size_t i = 0; i < 7; ++i) {
        s[i] = 1;
    }
    for (std::size_t n = 0; n + 7 < s.size(); ++n) {
        s[n + 7] = static_cast<std::uint8_t>(s[n + 3] ^ s[n]);
    }
    return s;
}

std::vector<std::uint8_t> funcube_encode_block(std::span<const std::uint8_t> frame) {
    const auto rs = CcsdsReedSolomon::create(kRs).value();
    std::vector<std::uint8_t> block = rs.encode(frame);
    ccsds_randomize(block, CcsdsRandomizer::Legacy255);
    std::vector<std::uint8_t> bits;
    bits.reserve(2566);
    for (const auto octet : block) {
        for (unsigned i = 0; i < 8; ++i) {
            bits.push_back(static_cast<std::uint8_t>((octet >> (7U - i)) & 1U));
        }
    }
    bits.insert(bits.end(), 6, 0);  // AO40: six tail bits, not scrambled
    auto symbols = convolutional_encode(code(), bits).value();
    for (std::size_t i = 1; i < symbols.size(); i += 2) {
        symbols[i] ^= 1U;
    }
    std::vector<std::uint8_t> grid(kFuncubeBlockSymbols, 0);
    const auto sync = funcube_sync_vector();
    std::copy(sync.begin(), sync.end(), grid.begin());
    std::copy(symbols.begin(), symbols.end(), grid.begin() + kFuncubeColumns);
    std::vector<std::uint8_t> out(kFuncubeBlockSymbols);
    for (std::size_t t = 0; t < kFuncubeBlockSymbols; ++t) {
        const std::size_t row = t % kFuncubeRows;
        const std::size_t col = t / kFuncubeRows;
        out[t] = grid[row * kFuncubeColumns + col];
    }
    return out;
}

FuncubeContent funcube_content(std::uint8_t t) {
    if (t >= 1 && t <= 12) {
        return FuncubeContent::WholeOrbit;
    }
    if (t == 13 || t == 17 || t == 21) {
        return FuncubeContent::HighResolution;
    }
    if (t >= 14 && t <= 24) {
        return FuncubeContent::FitterMessage;
    }
    return FuncubeContent::Unknown;
}

FuncubeFrame funcube_parse(std::span<const std::uint8_t, kFuncubeFrameOctets> octets) {
    FuncubeFrame f;
    std::copy(octets.begin(), octets.end(), f.octets.begin());
    BitReader r(octets);
    f.satellite_id = static_cast<std::uint8_t>(r.take(2));
    f.frame_type = static_cast<std::uint8_t>(r.take(6));
    f.content = funcube_content(f.frame_type);
    auto& t = f.real_time;
    // EPS, 192 bits.
    for (auto& v : t.photo_voltage) {
        v = static_cast<std::uint16_t>(r.take(16));
    }
    t.total_photo_current = static_cast<std::uint16_t>(r.take(16));
    t.battery_voltage = static_cast<std::uint16_t>(r.take(16));
    t.total_system_current = static_cast<std::uint16_t>(r.take(16));
    t.reboot_count = static_cast<std::uint16_t>(r.take(16));
    t.eps_software_errors = static_cast<std::uint16_t>(r.take(16));
    for (auto& v : t.boost_converter_temp) {
        v = static_cast<std::uint8_t>(r.take(8));
    }
    t.battery_temp = static_cast<std::uint8_t>(r.take(8));
    t.latch_up_5v1 = static_cast<std::uint8_t>(r.take(8));
    t.latch_up_3v3_1 = static_cast<std::uint8_t>(r.take(8));
    t.reset_cause = static_cast<std::uint8_t>(r.take(8));
    t.power_point_tracking_mode = static_cast<std::uint8_t>(r.take(8));
    // BOB, 100 bits.
    for (auto& v : t.bob) {
        v = static_cast<std::uint16_t>(r.take(10));
    }
    // RF, 48 bits; PA, 32 bits.
    for (auto& v : t.rf) {
        v = static_cast<std::uint8_t>(r.take(8));
    }
    for (auto& v : t.pa) {
        v = static_cast<std::uint8_t>(r.take(8));
    }
    // ANTS, 20 bits.
    for (auto& v : t.antenna_temp) {
        v = static_cast<std::uint8_t>(r.take(8));
    }
    for (auto& v : t.antenna_deployed) {
        v = r.take(1) != 0U;
    }
    // SW, 48 bits.
    t.sequence_number = r.take(24);
    t.dtmf_command_count = static_cast<std::uint8_t>(r.take(6));
    t.dtmf_last_command = static_cast<std::uint8_t>(r.take(5));
    t.dtmf_command_success = r.take(1) != 0U;
    for (auto& v : t.data_valid) {
        v = r.take(1) != 0U;
    }
    t.in_eclipse = r.take(1) != 0U;
    t.in_safe_mode = r.take(1) != 0U;
    t.hardware_abf = r.take(1) != 0U;
    t.software_abf = r.take(1) != 0U;
    t.deployment_wait = r.take(1) != 0U;
    // 8 + 440 = 448 bits, octet 56: the payload's 200 octets.
    f.payload.assign(octets.begin() + 56, octets.end());
    return f;
}

Expected<FuncubeDecoder> FuncubeDecoder::create(const FuncubeDecoderConfig& config) {
    if (!(config.sync_threshold > 0.0 && config.sync_threshold < 1.0)) {
        return fail("FUNcube decoder: sync_threshold must be between 0 and 1");
    }
    auto rs = CcsdsReedSolomon::create(kRs);
    if (!rs) {
        return std::unexpected(rs.error());
    }
    FuncubeDecoder d;
    d.config_ = config;
    d.rs_ = std::move(*rs);
    const auto sync = funcube_sync_vector();
    for (std::size_t i = 0; i < sync.size(); ++i) {
        d.sync_[i] = sync[i] != 0U ? -1.0F : 1.0F;
    }
    d.ring_.assign(kFuncubeBlockSymbols, 0.0F);
    return d;
}

void FuncubeDecoder::reset() {
    std::fill(ring_.begin(), ring_.end(), 0.0F);
    head_ = 0;
    count_ = 0;
    previous_ = 0.0F;
    hold_until_ = 0;
}

void FuncubeDecoder::push(SoftSpan soft, std::vector<FuncubeFrame>& out) {
    for (const float s : soft) {
        float v = s;
        if (config_.differential) {
            // A transition is a one: the soft value of c_k xor c_(k-1).
            const float m = std::min(std::abs(s), std::abs(previous_));
            v = (s < 0.0F) == (previous_ < 0.0F) ? m : -m;
            previous_ = s;
        }
        ring_[head_] = v;
        head_ = (head_ + 1) % kFuncubeBlockSymbols;
        ++count_;
        if (count_ >= kFuncubeBlockSymbols && count_ >= hold_until_) {
            try_block(out);
        }
    }
}

void FuncubeDecoder::try_block(std::vector<FuncubeFrame>& out) {
    // head_ is now the oldest symbol: the candidate block's first.
    float corr = 0.0F;
    float mag = 0.0F;
    for (std::size_t j = 0; j < kFuncubeColumns; ++j) {
        const float v = ring_[(head_ + j * kFuncubeRows) % kFuncubeBlockSymbols];
        corr += v * sync_[j];
        mag += std::abs(v);
    }
    if (mag <= 0.0F) {
        return;
    }
    double quality = corr / mag;
    const bool inverted = !config_.differential && quality < 0.0;
    if (inverted) {
        quality = -quality;
    }
    if (quality < config_.sync_threshold) {
        return;
    }

    // Deinterleave: transmitted t is row t % 80, column t / 80.
    std::vector<float> coded(kFuncubeCodedSymbols);
    for (std::size_t t = 0; t < kFuncubeBlockSymbols; ++t) {
        const std::size_t w = (t % kFuncubeRows) * kFuncubeColumns + t / kFuncubeRows;
        if (w < kFuncubeColumns || w >= kFuncubeColumns + kFuncubeCodedSymbols) {
            continue;
        }
        float v = ring_[(head_ + t) % kFuncubeBlockSymbols];
        if (inverted) {
            v = -v;
        }
        const std::size_t i = w - kFuncubeColumns;
        coded[i] = (i % 2U) == 1U ? -v : v;  // G2 inverted on the channel
    }
    const auto bits = viterbi_decode(code(), coded, true);
    if (!bits) {
        return;
    }
    std::vector<std::uint8_t> block(320, 0);
    for (std::size_t i = 0; i < 2560; ++i) {
        block[i / 8] = static_cast<std::uint8_t>(block[i / 8] | (((*bits)[i] & 1U) << (7U - (i % 8U))));
    }
    ccsds_randomize(block, CcsdsRandomizer::Legacy255);
    const auto rs = rs_->decode(block);
    if (!rs.ok()) {
        ++failed_;
        return;
    }
    FuncubeFrame f = funcube_parse(std::span<const std::uint8_t, kFuncubeFrameOctets>(block.data(), kFuncubeFrameOctets));
    f.corrected = {*rs.corrected[0], *rs.corrected[1]};
    f.sync_quality = quality;
    f.symbol_position = count_ - kFuncubeBlockSymbols;
    out.push_back(std::move(f));
    // The next block cannot start inside this one; eight symbols of slack
    // for the symbol clock.
    hold_until_ = count_ + kFuncubeBlockSymbols - 8;
}

}  // namespace revenant::decode
