#include "core/decode/vdes.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <utility>

#include "core/decode/crc.h"

namespace revenant::decode {

namespace {

using dsp::Complex32;

constexpr double kPi = std::numbers::pi;

// Table 7. Data symbols are the burst less the two ramps, the syncword and
// the Link ID; packet bits are the FEC output bits.
constexpr std::array<VdesAsmFormat, 6> kFormats{{
    {1, 1, 248, 197, 384, true},
    {2, 2, 504, 453, 896, true},
    {3, 3, 760, 709, 1408, true},
    {5, 1, 248, 197, 288, false},
    {6, 2, 504, 453, 672, false},
    {7, 3, 760, 709, 1056, false},
}};

constexpr std::size_t kKnownSymbols = kVdesSyncSymbols + kVdesLinkIdSymbols;
constexpr std::size_t kMaxDataSymbols = 709;

// Clause A2-1.2.5: the H.222.0 Annex A CRC, MSB first, preset to ones, the
// register itself the check sequence.
constexpr CrcParams kVdesCrc32{32, 0x04C11DB7, 0xFFFFFFFF, false, false, 0};

// Decision-directed phase loop gain per symbol. An engineering choice: the
// line fit over the 43 known symbols leaves only the error of its slope, and
// a gain this small averages over about 20 symbols.
constexpr float kPhaseGain = 0.05F;

}  // namespace

std::optional<VdesAsmFormat> vdes_asm_format(unsigned link_id) {
    for (const VdesAsmFormat& f : kFormats) {
        if (f.link_id == link_id) {
            return f;
        }
    }
    return std::nullopt;
}

std::uint32_t vdes_link_id_codeword(unsigned link_id) {
    std::uint32_t word = kVdesLinkIdScramble;
    for (unsigned i = 0; i < 6; ++i) {
        // D0 is the most significant of the six bits.
        if (((link_id >> (5U - i)) & 1U) != 0U) {
            word ^= kVdesLinkIdGenerator[i];
        }
    }
    return word;
}

void vdes_scramble(std::span<std::uint8_t> bits) {
    std::array<std::uint8_t, 15> s = kVdesScramblerPreset;
    for (std::uint8_t& b : bits) {
        // Figure 9: cells 14 and 15 summed, fed back to cell 1 and added to
        // the input.
        const std::uint8_t fb = s[13] ^ s[14];
        b = static_cast<std::uint8_t>((b & 1U) ^ fb);
        for (std::size_t i = 14; i > 0; --i) {
            s[i] = s[i - 1];
        }
        s[0] = fb;
    }
}

std::uint32_t vdes_crc32(std::span<const std::uint8_t> octets) {
    static const Crc crc(kVdesCrc32);
    return static_cast<std::uint32_t>(crc.compute(octets));
}

Complex32 vdes_pi4qpsk_point(std::size_t n, unsigned msb, unsigned lsb) {
    // Figure 11, counter-clockwise from each set's first point: 11, 01, 00, 10.
    static constexpr std::array<unsigned, 4> kStep{2, 1, 3, 0};  // index msb*2+lsb
    const unsigned k = kStep[((msb & 1U) << 1U) | (lsb & 1U)];
    const double base = (n % 2 == 0) ? kPi / 4.0 : 0.0;
    const double angle = base + kPi / 2.0 * static_cast<double>(k);
    return {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
}

namespace {

std::uint32_t bits_at(std::span<const std::uint8_t> octets, std::size_t first, unsigned width) {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < width; ++i) {
        const std::size_t bit = first + i;
        v = (v << 1U) | ((octets[bit / 8] >> (7U - bit % 8)) & 1U);
    }
    return v;
}

// The pi/4-QPSK soft bits of a symbol already rotated so the sent point sits
// on its constellation: positive means 1.
std::pair<float, float> soft_bits(Complex32 z, std::size_t n) {
    const double base = (n % 2 == 0) ? kPi / 4.0 : 0.0;
    // Undo the set's base, then turn by 45 degrees so 11 lands in the first
    // quadrant, 01 in the second, 00 in the third and 10 in the fourth.
    const double turn = kPi / 4.0 - base;
    const Complex32 u = z * Complex32(static_cast<float>(std::cos(turn)),
                                      static_cast<float>(std::sin(turn)));
    return {u.real(), u.imag()};
}

}  // namespace

std::optional<VdesAsmMessage> vdes_asm_parse(std::span<const std::uint8_t> payload) {
    if (payload.size() * 8 < kVdesAsmHeaderBits) {
        return std::nullopt;
    }
    VdesAsmMessage m;
    // Tables 26 to 28, in order.
    m.message_id = static_cast<std::uint8_t>(bits_at(payload, 0, 4));
    m.retransmit = bits_at(payload, 4, 1) != 0U;
    m.repeat = static_cast<std::uint8_t>(bits_at(payload, 5, 2));
    m.session_id = static_cast<std::uint8_t>(bits_at(payload, 7, 6));
    m.source_id = bits_at(payload, 13, 32);
    m.data_count = static_cast<std::uint16_t>(bits_at(payload, 45, 11));
    if (m.message_id == 0 || m.message_id == 2) {
        const std::size_t available = payload.size() * 8 - kVdesAsmHeaderBits;
        if (m.data_count >= 1 && m.data_count <= available) {
            std::vector<std::uint8_t> data((m.data_count + 7U) / 8U, 0);
            for (std::size_t i = 0; i < m.data_count; ++i) {
                if (bits_at(payload, kVdesAsmHeaderBits + i, 1) != 0U) {
                    data[i / 8] = static_cast<std::uint8_t>(data[i / 8] | (0x80U >> (i % 8)));
                }
            }
            if (m.message_id == 2 && m.data_count >= 16) {
                m.asm_identifier = static_cast<std::uint16_t>((data[0] << 8U) | data[1]);
            }
            m.application_data = std::move(data);
        }
    }
    return m;
}

// ---------------------------------------------------------------------------

Expected<VdesAsmDecoder> VdesAsmDecoder::create(const VdesAsmConfig& config) {
    if (config.rate <= 0 || config.rate % 9600 != 0 || config.rate / 9600 < 4) {
        return fail(
                          std::format("VDES ASM needs a whole number of samples per symbol, "
                                      "four or more; {} S/s is not",
                                      config.rate));
    }
    if (config.sync_threshold <= 0.0 || config.sync_threshold >= 1.0 ||
        config.filter_span_symbols < 2) {
        return fail( "VDES ASM configuration out of range");
    }
    VdesAsmDecoder d;
    d.config_ = config;
    d.sps_ = static_cast<std::size_t>(config.rate / 9600);
    const std::size_t taps = config.filter_span_symbols * d.sps_ + 1;
    auto rrc = design_rrc(config.rate, kVdesAsmSymbolRate, kVdesAsmRollOff, taps);
    if (!rrc) {
        return std::unexpected(rrc.error());
    }
    auto fir = ComplexFir::create(std::move(*rrc));
    if (!fir) {
        return std::unexpected(fir.error());
    }
    d.filter_ = std::move(*fir);
    d.filter_delay_ = (taps - 1) / 2;
    for (std::size_t n = 0; n < kVdesSyncSymbols; ++n) {
        // Clause A2-1.2.3.3: 1 is sent as 11 and 0 as 00.
        const unsigned b = kVdesTerSyncword[n];
        d.sync_points_[n] = vdes_pi4qpsk_point(n, b, b);
    }
    for (std::size_t n = 0; n + 1 < kVdesSyncSymbols; ++n) {
        d.sync_diff_[n] = d.sync_points_[n + 1] * std::conj(d.sync_points_[n]);
    }
    return d;
}

void VdesAsmDecoder::reset() {
    filter_.reset();
    buffer_.clear();
    buffer_start_ = total_;
    next_metric_ = total_;
    have_candidate_ = false;
    quiet_until_ = total_;
    pending_.clear();
}

Complex32 VdesAsmDecoder::at(double index) const {
    const double rel = index - static_cast<double>(buffer_start_);
    const auto i = static_cast<std::int64_t>(std::floor(rel));
    if (i < 0 || static_cast<std::size_t>(i + 1) >= buffer_.size()) {
        return {};
    }
    const auto f = static_cast<float>(rel - static_cast<double>(i));
    const auto u = static_cast<std::size_t>(i);
    return buffer_[u] * (1.0F - f) + buffer_[u + 1] * f;
}

double VdesAsmDecoder::metric_at(std::int64_t t, Complex32* corr) const {
    const auto base = static_cast<std::size_t>(t - buffer_start_);
    std::complex<double> c{};
    double norm = 0.0;
    Complex32 prev = buffer_[base];
    for (std::size_t k = 0; k + 1 < kVdesSyncSymbols; ++k) {
        const Complex32 next = buffer_[base + (k + 1) * sps_];
        const Complex32 d = next * std::conj(prev);
        c += std::complex<double>(d * std::conj(sync_diff_[k]));
        norm += static_cast<double>(std::norm(d));
        prev = next;
    }
    if (corr != nullptr) {
        *corr = Complex32(static_cast<float>(c.real()), static_cast<float>(c.imag()));
    }
    // Normalised by Cauchy-Schwarz, so a window whose energy sits in a few
    // symbols, a ramp at the edge of silence, cannot read high.
    const double bound = std::sqrt(static_cast<double>(kVdesSyncSymbols - 1) * norm);
    return bound > 0.0 ? std::abs(c) / bound : 0.0;
}

void VdesAsmDecoder::process(dsp::ConstComplexSpan samples, std::vector<VdesAsmBurst>& out) {
    if (samples.empty()) {
        return;
    }
    scratch_.resize(samples.size());
    (void)filter_.process(samples, scratch_);
    buffer_.insert(buffer_.end(), scratch_.begin(), scratch_.end());
    total_ += static_cast<std::int64_t>(samples.size());

    const auto sps = static_cast<std::int64_t>(sps_);
    const std::int64_t span = static_cast<std::int64_t>(kVdesSyncSymbols - 1) * sps + 1;
    while (next_metric_ + span < total_) {
        const std::int64_t t = next_metric_++;
        if (have_candidate_ && t - candidate_ >= static_cast<std::int64_t>(kVdesSyncSymbols) * sps) {
            pending_.push_back({candidate_, candidate_metric_, candidate_corr_});
            ++stats_.syncs;
            have_candidate_ = false;
            quiet_until_ = candidate_ + static_cast<std::int64_t>(kKnownSymbols) * sps;
        }
        if (t < quiet_until_) {
            continue;
        }
        Complex32 corr;
        const double m = metric_at(t, &corr);
        if (m >= config_.sync_threshold && (!have_candidate_ || m > candidate_metric_)) {
            have_candidate_ = true;
            candidate_ = t;
            candidate_metric_ = m;
            candidate_corr_ = corr;
        }
    }

    // Bursts in order of detection, each once its last symbol has arrived.
    // The Link ID is read first, from the same front end decode_at uses, so
    // the length waited for is the length decoded.
    while (!pending_.empty()) {
        const Pending p = pending_.front();
        const std::int64_t link_end =
            p.t + static_cast<std::int64_t>(kKnownSymbols) * sps + 2 * sps;
        if (link_end >= total_) {
            break;
        }
        const Front f = front(p.t, p.metric, p.corr);
        const auto format = vdes_asm_format(f.link_id);
        const std::size_t data_symbols =
            (format && format->uncoded) ? format->data_symbols : 0;
        const std::int64_t end =
            p.t + static_cast<std::int64_t>(kKnownSymbols + data_symbols) * sps + 2 * sps;
        if (end >= total_) {
            break;
        }
        pending_.erase(pending_.begin());
        decode_at(f, p.metric, out);
    }

    // Keep what the scan and the pending bursts still need.
    std::int64_t keep = next_metric_ - 2 * sps;
    if (have_candidate_) {
        keep = std::min(keep, candidate_ - 2 * sps);
    }
    for (const Pending& p : pending_) {
        keep = std::min(keep, p.t - 2 * sps);
    }
    if (keep > buffer_start_) {
        const auto drop = static_cast<std::size_t>(keep - buffer_start_);
        if (drop >= 65536 || drop * 2 > buffer_.size()) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(drop));
            buffer_start_ = keep;
        }
    }
}

VdesAsmDecoder::Front VdesAsmDecoder::front(std::int64_t t, double metric, Complex32 corr) const {
    Front f;
    // Timing: a parabola through the metric either side of the peak.
    double delta = 0.0;
    if (t - 1 >= buffer_start_) {
        const double m0 = metric_at(t - 1, nullptr);
        const double m2 = metric_at(t + 1, nullptr);
        const double den = m0 - 2.0 * metric + m2;
        if (den < 0.0) {
            delta = std::clamp(0.5 * (m0 - m2) / den, -0.5, 0.5);
        }
    }
    f.t0 = static_cast<double>(t) + delta;
    // Carrier offset from the differential correlation, radians per symbol.
    f.w = std::arg(corr);

    std::complex<double> a{};
    for (std::size_t n = 0; n < kKnownSymbols; ++n) {
        f.z[n] = sample(f, n);
        if (n < kVdesSyncSymbols) {
            a += std::complex<double>(f.z[n] * std::conj(sync_points_[n]));
        }
    }
    f.amplitude = std::abs(a) / static_cast<double>(kVdesSyncSymbols);
    const auto rot = std::complex<float>(std::conj(a) / std::max(std::abs(a), 1e-12));

    // The Link ID: soft correlation with all 64 code words.
    std::array<float, 32> soft{};
    double soft_sum = 0.0;
    for (std::size_t s = 0; s < kVdesLinkIdSymbols; ++s) {
        const std::size_t n = kVdesSyncSymbols + s;
        const auto [m1, l1] = soft_bits(f.z[n] * rot, n);
        soft[2 * s] = m1;
        soft[2 * s + 1] = l1;
        soft_sum += std::abs(m1) + std::abs(l1);
    }
    double best = -1e300;
    for (unsigned c = 0; c < 64; ++c) {
        const std::uint32_t word = vdes_link_id_codeword(c);
        double score = 0.0;
        for (unsigned i = 0; i < 32; ++i) {
            score += ((word >> (31U - i)) & 1U) != 0U ? soft[i] : -soft[i];
        }
        if (score > best) {
            best = score;
            f.link_id = c;
        }
    }
    f.link_id_quality = soft_sum > 0.0 ? best / soft_sum : 0.0;
    return f;
}

Complex32 VdesAsmDecoder::sample(const Front& f, std::size_t n) const {
    const double ph = -f.w * static_cast<double>(n);
    return at(f.t0 + static_cast<double>(n * sps_)) *
           Complex32(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
}

void VdesAsmDecoder::decode_at(const Front& f, double metric, std::vector<VdesAsmBurst>& out) {
    VdesAsmBurst burst;
    burst.sync_metric = metric;
    burst.cfo_hz = f.w * kVdesAsmSymbolRate / (2.0 * kPi);
    burst.sync_sample = static_cast<dsp::SampleIndex>(std::llround(f.t0)) -
                        static_cast<dsp::SampleIndex>(filter_delay_);
    burst.link_id = f.link_id;
    burst.link_id_quality = f.link_id_quality;
    burst.format = vdes_asm_format(f.link_id);
    const unsigned id = f.link_id;
    const auto& z = f.z;
    const double amp_sync = f.amplitude;
    auto sample = [&](std::size_t n) { return this->sample(f, n); };
    if (!burst.format) {
        ++stats_.unknown_link_id;
        out.push_back(std::move(burst));
        return;
    }

    // Phase and residual offset: a line through the phase of the 43 known
    // symbols, the Link ID now known too.
    const std::uint32_t word = vdes_link_id_codeword(id);
    std::array<double, kKnownSymbols> phase{};
    double prev = 0.0;
    for (std::size_t n = 0; n < kKnownSymbols; ++n) {
        Complex32 ref;
        if (n < kVdesSyncSymbols) {
            ref = sync_points_[n];
        } else {
            const std::size_t s = n - kVdesSyncSymbols;
            ref = vdes_pi4qpsk_point(n, (word >> (31U - 2 * s)) & 1U, (word >> (30U - 2 * s)) & 1U);
        }
        double ph = std::arg(z[n] * std::conj(ref));
        if (n > 0) {
            while (ph - prev > kPi) {
                ph -= 2.0 * kPi;
            }
            while (ph - prev < -kPi) {
                ph += 2.0 * kPi;
            }
        }
        phase[n] = ph;
        prev = ph;
    }
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    for (std::size_t n = 0; n < kKnownSymbols; ++n) {
        const auto x = static_cast<double>(n);
        sx += x;
        sy += phase[n];
        sxx += x * x;
        sxy += x * phase[n];
    }
    const auto count = static_cast<double>(kKnownSymbols);
    const double slope = (count * sxy - sx * sy) / (count * sxx - sx * sx);
    const double intercept = (sy - slope * sx) / count;
    burst.cfo_hz += slope * kVdesAsmSymbolRate / (2.0 * kPi);

    if (!burst.format->uncoded) {
        ++stats_.not_decoded;
        out.push_back(std::move(burst));
        return;
    }

    const std::size_t data_symbols = burst.format->data_symbols;
    std::vector<std::uint8_t> bits;
    bits.reserve(data_symbols * 2);
    float theta = 0.0F;
    double err_power = 0.0;
    const auto gain = static_cast<float>(1.0 / std::max(amp_sync, 1e-12));
    for (std::size_t k = 0; k < data_symbols; ++k) {
        const std::size_t n = kKnownSymbols + k;
        const double ph = -(intercept + slope * static_cast<double>(n)) - theta;
        Complex32 v = sample(n) *
                      Complex32(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph))) *
                      gain;
        const auto [m1, l1] = soft_bits(v, n);
        const unsigned mb = m1 > 0.0F ? 1U : 0U;
        const unsigned lb = l1 > 0.0F ? 1U : 0U;
        bits.push_back(static_cast<std::uint8_t>(mb));
        bits.push_back(static_cast<std::uint8_t>(lb));
        const Complex32 point = vdes_pi4qpsk_point(n, mb, lb);
        err_power += static_cast<double>(std::norm(v - point));
        theta += kPhaseGain * std::arg(v * std::conj(point));
    }
    const double mse = err_power / static_cast<double>(data_symbols);
    burst.sinr_db = 10.0 * std::log10(1.0 / std::max(mse, 1e-6));
    // Clause A2-1.2.8.
    burst.cqi = static_cast<std::uint8_t>(
        std::clamp(std::floor(40.0 + 4.0 * burst.sinr_db), 0.0, 255.0));

    vdes_scramble(bits);
    const std::size_t octets = burst.format->packet_bits / 8;
    std::vector<std::uint8_t> packet(octets, 0);
    for (std::size_t i = 0; i < burst.format->packet_bits; ++i) {
        packet[i / 8] = static_cast<std::uint8_t>(packet[i / 8] | (bits[i] << (7U - i % 8)));
    }
    const std::span<const std::uint8_t> body(packet.data(), octets - 4);
    const std::uint32_t sent = (static_cast<std::uint32_t>(packet[octets - 4]) << 24U) |
                               (static_cast<std::uint32_t>(packet[octets - 3]) << 16U) |
                               (static_cast<std::uint32_t>(packet[octets - 2]) << 8U) |
                               packet[octets - 1];
    burst.crc_ok = vdes_crc32(body) == sent;
    if (!burst.crc_ok) {
        ++stats_.crc_failures;
        out.push_back(std::move(burst));
        return;
    }
    ++stats_.packets;
    burst.payload.assign(body.begin(), body.end());
    burst.message = vdes_asm_parse(burst.payload);
    out.push_back(std::move(burst));
}

}  // namespace revenant::decode
