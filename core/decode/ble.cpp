#include "core/decode/ble.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

namespace revenant::decode {

namespace {

// Search window and trim margin, in samples, on the discriminator buffer.
constexpr SampleIndex kTrimMargin = 8;

std::vector<float> design_channel_filter(double rate) {
    // Not from the specification: a receiver choice. Windowed sinc passing
    // +/-800 kHz, which holds a 1 Msym/s GFSK signal at 250 kHz deviation
    // plus the +/-150 kHz carrier tolerance of Core Vol 6 Part A 3.2.
    constexpr double kCutoffHz = 800'000.0;
    const auto taps = static_cast<std::size_t>(2 * std::lround(rate / 200'000.0) + 1);
    std::vector<float> h(taps);
    const double fc = kCutoffHz / rate;
    const double centre = static_cast<double>(taps - 1) / 2.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < taps; ++i) {
        const double n = static_cast<double>(i) - centre;
        const double sinc = n == 0.0 ? 2.0 * fc
                                     : std::sin(2.0 * std::numbers::pi * fc * n) /
                                           (std::numbers::pi * n);
        const double window =
            0.54 - 0.46 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                   static_cast<double>(taps - 1));
        h[i] = static_cast<float>(sinc * window);
        sum += sinc * window;
    }
    for (float& v : h) {
        v = static_cast<float>(v / sum);
    }
    return h;
}

BleAddress address_at(std::span<const std::uint8_t> payload, std::size_t offset) {
    BleAddress a{};
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(offset), 6, a.begin());
    return a;
}

}  // namespace

Expected<std::int64_t> ble_advertising_channel_hz(unsigned channel_index) {
    // Core Vol 6 Part B Table 1.3.
    switch (channel_index) {
        case 37: return 2'402'000'000;
        case 38: return 2'426'000'000;
        case 39: return 2'480'000'000;
        default: return fail("BLE: advertising channel index must be 37, 38 or 39");
    }
}

std::uint32_t ble_crc24_bits(std::span<const std::uint8_t> bits, std::uint32_t init) {
    // Core Vol 6 Part B 3.1.1 and Figure 3.4: position 0 is the LSB of the
    // preset; the output of position 23 is XORed with the incoming bit and
    // fed to position 0 and to the inputs of the positions the polynomial's
    // terms name.
    std::uint32_t reg = init & 0xFFFFFFU;
    for (const std::uint8_t bit : bits) {
        const std::uint32_t feedback = ((reg >> 23U) ^ bit) & 1U;
        reg = (reg << 1U) & 0xFFFFFFU;
        if (feedback != 0U) {
            reg ^= kBleCrcPolynomial;
        }
    }
    return reg;
}

std::uint32_t ble_crc24(std::span<const std::uint8_t> octets, std::uint32_t init) {
    std::vector<std::uint8_t> bits;
    bits.reserve(octets.size() * 8);
    for (const std::uint8_t octet : octets) {
        // Core Vol 6 Part B 1.2: LSB first.
        for (unsigned i = 0; i < 8; ++i) {
            bits.push_back(static_cast<std::uint8_t>((octet >> i) & 1U));
        }
    }
    return ble_crc24_bits(bits, init);
}

std::uint8_t ble_whitening_seed(unsigned channel_index) {
    // Core Vol 6 Part B 3.2: position 0 is one; positions 1 to 6 hold the
    // channel index, its MSB in position 1.
    std::uint8_t seed = 1U;
    for (unsigned p = 1; p <= 6; ++p) {
        const unsigned bit = (channel_index >> (6U - p)) & 1U;
        seed = static_cast<std::uint8_t>(seed | (bit << p));
    }
    return seed;
}

void ble_whiten(std::span<std::uint8_t> bits, unsigned channel_index) {
    // Core Vol 6 Part B 3.2 and Figure 3.5: x^7 + x^4 + 1. Position 6's
    // output whitens the data bit and is fed back to position 0 and XORed
    // into the input of position 4.
    std::uint8_t reg = ble_whitening_seed(channel_index);
    for (std::uint8_t& bit : bits) {
        const auto out = static_cast<std::uint8_t>((reg >> 6U) & 1U);
        bit = static_cast<std::uint8_t>((bit ^ out) & 1U);
        reg = static_cast<std::uint8_t>((reg << 1U) & 0x7FU);
        reg = static_cast<std::uint8_t>(reg | out);
        reg = static_cast<std::uint8_t>(reg ^ (out << 4U));
    }
}

Expected<std::vector<BleAdStructure>> ble_parse_ad(std::span<const std::uint8_t> data) {
    // Core Vol 3 Part C 11 and Figure 11.1: Length, then Length octets of
    // which the first is the AD type. A zero Length starts the
    // non-significant part.
    std::vector<BleAdStructure> out;
    std::size_t i = 0;
    while (i < data.size()) {
        const std::size_t length = data[i];
        if (length == 0) {
            break;
        }
        if (i + 1 + length > data.size()) {
            return fail("BLE: AD structure runs past the end of the data");
        }
        BleAdStructure ad;
        ad.type = data[i + 1];
        ad.data.assign(data.begin() + static_cast<std::ptrdiff_t>(i + 2),
                       data.begin() + static_cast<std::ptrdiff_t>(i + 1 + length));
        out.push_back(std::move(ad));
        i += 1 + length;
    }
    return out;
}

void ble_parse_pdu_fields(BlePacket& packet) {
    if (packet.pdu.size() < 2) {
        return;
    }
    const std::span<const std::uint8_t> payload =
        std::span<const std::uint8_t>(packet.pdu).subspan(2);
    const auto parse_ad_from = [&](std::size_t offset) {
        if (payload.size() > offset) {
            if (auto ad = ble_parse_ad(payload.subspan(offset)); ad.has_value()) {
                packet.ad = std::move(*ad);
            }
        }
    };
    switch (static_cast<BlePduType>(packet.pdu_type)) {
        // Core Vol 6 Part B Figures 2.6, 2.8 and 2.9: AdvA (6), AdvData.
        case BlePduType::AdvInd:
        case BlePduType::AdvNonconnInd:
        case BlePduType::AdvScanInd:
        // Figure 2.14: AdvA (6), ScanRspData.
        case BlePduType::ScanRsp:
            if (payload.size() >= 6) {
                packet.adv_address = address_at(payload, 0);
                parse_ad_from(6);
            }
            break;
        // Figure 2.7: AdvA (6), TargetA (6).
        case BlePduType::AdvDirectInd:
            if (payload.size() >= 12) {
                packet.adv_address = address_at(payload, 0);
                packet.target_address = address_at(payload, 6);
            }
            break;
        // Figure 2.13: ScanA (6), AdvA (6).
        case BlePduType::ScanReq:
            if (payload.size() >= 12) {
                packet.scan_address = address_at(payload, 0);
                packet.adv_address = address_at(payload, 6);
            }
            break;
        // Figure 2.15: InitA (6), AdvA (6), LLData (22).
        case BlePduType::ConnectInd:
            if (payload.size() >= 12) {
                packet.init_address = address_at(payload, 0);
                packet.adv_address = address_at(payload, 6);
            }
            break;
        default:
            break;
    }
}

Expected<BleDecoder> BleDecoder::create(const BleConfig& config) {
    if (config.sample_rate < 2'000'000) {
        return fail("BLE: sample rate must be at least 2 MS/s");
    }
    if (config.channel_index > 39) {
        return fail("BLE: channel index must be 0 to 39");
    }
    if (config.max_access_address_errors > 8) {
        return fail("BLE: more than 8 Access Address errors would accept noise");
    }
    BleDecoder d;
    d.config_ = config;
    const auto rate = static_cast<double>(config.sample_rate);
    d.samples_per_bit_ = rate / kBleSymbolRate;
    std::vector<float> taps = design_channel_filter(rate);
    d.filter_delay_ = (taps.size() - 1) / 2;
    auto fir = ComplexFir::create(std::move(taps));
    if (!fir.has_value()) {
        return std::unexpected(fir.error());
    }
    d.channel_filter_ = std::move(*fir);

    // Core Vol 6 Part B 2.1.1: alternating bits whose first equals the
    // Access Address LSB, then the Access Address LSB first (1.2). Part A
    // 3.1: a one is a positive deviation.
    const std::uint32_t aa = config.access_address;
    const unsigned first = aa & 1U;
    for (std::size_t k = 0; k < kBlePreambleBits; ++k) {
        const unsigned bit = (k % 2 == 0) ? first : 1U - first;
        d.sync_[k] = bit != 0U ? 1.0F : -1.0F;
    }
    for (std::size_t k = 0; k < kBleAccessAddressBits; ++k) {
        d.sync_[kBlePreambleBits + k] = ((aa >> k) & 1U) != 0U ? 1.0F : -1.0F;
    }
    double mean = 0.0;
    for (const float v : d.sync_) {
        mean += v;
    }
    mean /= static_cast<double>(d.sync_.size());
    double energy = 0.0;
    for (const float v : d.sync_) {
        energy += (v - mean) * (v - mean);
    }
    d.sync_mean_ = mean;
    d.sync_energy_ = energy;
    return d;
}

std::complex<double> BleDecoder::value_at(double absolute) const {
    const double rel = absolute - static_cast<double>(base_);
    const double floor_rel = std::floor(rel);
    const auto i = static_cast<std::size_t>(std::max(0.0, floor_rel));
    if (i + 1 >= iq_.size()) {
        return iq_.empty() ? std::complex<double>{} : std::complex<double>(iq_.back());
    }
    const double frac = rel - floor_rel;
    const std::complex<double> a(iq_[i]);
    const std::complex<double> b(iq_[i + 1]);
    return a + frac * (b - a);
}

double BleDecoder::soft_bit(double centre) const {
    // Not from the specification: a receiver choice. A one-bit differential
    // phase detector, the phase advanced across the bit, rather than the
    // instantaneous discriminator: the discriminator clicks below its
    // threshold where this does not. A bit at the nominal 250 kHz deviation
    // advances a quarter turn, so dividing by pi/2 puts the nominal symbol at
    // +/-1 and the result in units of the nominal deviation.
    const double h = 0.5 * samples_per_bit_;
    const std::complex<double> turn = value_at(centre + h) * std::conj(value_at(centre - h));
    return std::arg(turn) / (std::numbers::pi / 2.0);
}

double BleDecoder::correlate(double start, double& amplitude, double& mean) const {
    const double s = start;
    std::array<double, kBlePreambleBits + kBleAccessAddressBits> x{};
    double mx = 0.0;
    for (std::size_t k = 0; k < x.size(); ++k) {
        x[k] = soft_bit(s + (static_cast<double>(k) + 0.5) * samples_per_bit_);
        mx += x[k];
    }
    mx /= static_cast<double>(x.size());
    double sxt = 0.0;
    double sxx = 0.0;
    for (std::size_t k = 0; k < x.size(); ++k) {
        sxt += (sync_[k] - sync_mean_) * x[k];
        sxx += (x[k] - mx) * (x[k] - mx);
    }
    amplitude = sxt / sync_energy_;
    mean = mx - amplitude * sync_mean_;
    if (sxx <= 0.0) {
        return 0.0;
    }
    return sxt / std::sqrt(sync_energy_ * sxx);
}

BleDecoder::Attempt BleDecoder::try_decode(double start, std::vector<BlePacket>& out,
                                           SampleIndex& end) {
    const double spb = samples_per_bit_;
    const auto available = static_cast<double>(base_ + iq_.size());
    const auto centre = [&](std::size_t bit) {
        return static_cast<double>(start) + (static_cast<double>(bit) + 0.5) * spb;
    };
    const auto have = [&](std::size_t bits_after_start) {
        return centre(bits_after_start - 1) + 0.5 * spb + 2.0 < available;
    };

    double amplitude = 0.0;
    double dc = 0.0;
    (void)correlate(start, amplitude, dc);
    if (amplitude <= 0.0) {
        return Attempt::Rejected;
    }

    unsigned aa_errors = 0;
    for (std::size_t k = 0; k < kBleAccessAddressBits; ++k) {
        const bool one = soft_bit(centre(kBlePreambleBits + k)) > dc;
        const bool want = sync_[kBlePreambleBits + k] > 0.0F;
        aa_errors += one != want ? 1U : 0U;
    }
    if (aa_errors > config_.max_access_address_errors) {
        return Attempt::Rejected;
    }

    constexpr std::size_t kSyncBits = kBlePreambleBits + kBleAccessAddressBits;
    if (!have(kSyncBits + 16)) {
        return Attempt::NeedMore;
    }
    const auto decide = [&](std::size_t first, std::size_t count, std::vector<std::uint8_t>& bits) {
        bits.resize(count);
        for (std::size_t k = 0; k < count; ++k) {
            bits[k] = soft_bit(centre(first + k)) > dc ? 1U : 0U;
        }
    };
    std::vector<std::uint8_t> bits;
    decide(kSyncBits, 16, bits);
    ble_whiten(bits, config_.channel_index);
    // Core Vol 6 Part B Figure 2.5: Length is header bits 8 to 15.
    std::size_t length = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        length |= static_cast<std::size_t>(bits[8 + i]) << i;
    }
    const std::size_t pdu_bits = 16 + 8 * length;
    const std::size_t total = pdu_bits + kBleCrcBits;
    if (!have(kSyncBits + total)) {
        return Attempt::NeedMore;
    }
    decide(kSyncBits, total, bits);
    ble_whiten(bits, config_.channel_index);

    const std::uint32_t computed =
        ble_crc24_bits(std::span<const std::uint8_t>(bits).first(pdu_bits), config_.crc_init);
    // Core Vol 6 Part B 3.1.1: the CRC goes out position 23 first.
    std::uint32_t received = 0;
    for (std::size_t i = 0; i < kBleCrcBits; ++i) {
        received |= static_cast<std::uint32_t>(bits[pdu_bits + i]) << (23U - i);
    }
    if (computed != received) {
        return Attempt::Rejected;
    }

    BlePacket p;
    const auto whole = static_cast<SampleIndex>(std::llround(std::max(0.0, start)));
    p.sample = whole >= filter_delay_ ? whole - filter_delay_ : 0;
    p.access_address = config_.access_address;
    p.channel_index = config_.channel_index;
    p.pdu.resize(2 + length);
    for (std::size_t o = 0; o < p.pdu.size(); ++o) {
        std::uint8_t v = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            v = static_cast<std::uint8_t>(v | (bits[8 * o + i] << i));
        }
        p.pdu[o] = v;
    }
    // Core Vol 6 Part B Figure 2.5: PDU Type 4 bits, RFU, ChSel, TxAdd, RxAdd.
    p.pdu_type = static_cast<std::uint8_t>(p.pdu[0] & 0x0FU);
    p.ch_sel = (p.pdu[0] & 0x20U) != 0U;
    p.tx_add = (p.pdu[0] & 0x40U) != 0U;
    p.rx_add = (p.pdu[0] & 0x80U) != 0U;
    p.length = p.pdu[1];
    p.crc = received;
    p.frequency_offset_hz = dc * kBleNominalDeviationHz;
    p.deviation_hz = amplitude * kBleNominalDeviationHz;
    p.access_address_errors = aa_errors;
    ble_parse_pdu_fields(p);
    out.push_back(std::move(p));

    end = static_cast<SampleIndex>(std::ceil(start + static_cast<double>(kSyncBits + total) * spb));
    return Attempt::Decoded;
}

Status BleDecoder::push(dsp::ConstComplexSpan samples, std::vector<BlePacket>& out) {
    if (samples_per_bit_ <= 0.0) {
        return fail("BLE: decoder used before create()");
    }
    scratch_.resize(samples.size());
    if (auto s = channel_filter_.process(samples, scratch_); !s.has_value()) {
        return s;
    }
    iq_.insert(iq_.end(), scratch_.begin(), scratch_.end());

    const double spb = samples_per_bit_;
    constexpr std::size_t kSyncBits = kBlePreambleBits + kBleAccessAddressBits;
    const auto window = static_cast<SampleIndex>(std::ceil(spb));
    const auto sync_span = static_cast<SampleIndex>(std::ceil((kSyncBits + 1) * spb)) + 2;

    SampleIndex s = std::max(search_, base_ + 1);
    while (true) {
        const SampleIndex available = base_ + iq_.size();
        if (s + window + 1 + sync_span > available) {
            break;
        }
        double amplitude = 0.0;
        double dc = 0.0;
        if (correlate(static_cast<double>(s), amplitude, dc) < config_.sync_threshold) {
            ++s;
            continue;
        }
        SampleIndex best = s;
        double best_c = -1.0;
        for (SampleIndex c = s; c <= s + window; ++c) {
            const double v = correlate(static_cast<double>(c), amplitude, dc);
            if (v > best_c) {
                best_c = v;
                best = c;
            }
        }
        // Parabolic interpolation of the correlation peak: at 2.4 samples
        // a bit, a whole-sample start is up to a fifth of a bit off.
        const double cm = correlate(static_cast<double>(best) - 1.0, amplitude, dc);
        const double cp = correlate(static_cast<double>(best) + 1.0, amplitude, dc);
        const double curve = cm - 2.0 * best_c + cp;
        const double delta =
            curve < 0.0 ? std::clamp(0.5 * (cm - cp) / curve, -0.5, 0.5) : 0.0;
        SampleIndex end = 0;
        const Attempt a = try_decode(static_cast<double>(best) + delta, out, end);
        if (a == Attempt::NeedMore) {
            break;
        }
        s = a == Attempt::Decoded ? end : s + 1;
    }
    search_ = s;

    // Keep what the next search can still reach back to.
    if (search_ > base_ + kTrimMargin) {
        const SampleIndex drop = search_ - kTrimMargin - base_;
        iq_.erase(iq_.begin(), iq_.begin() + static_cast<std::ptrdiff_t>(drop));
        base_ += drop;
    }
    return {};
}

}  // namespace revenant::decode
