#include "core/decode/dect.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace revenant::decode {

std::uint16_t dect_rcrc(std::uint64_t data48) {
    // EN 300 175-3 clause 6.2.5.2: r0..r(n-1) times x^16, divided by g(x),
    // r0 first; the remainder is the check bits with the x^0 term inverted.
    unsigned reg = 0;
    for (int i = 47; i >= 0; --i) {
        const unsigned bit = static_cast<unsigned>((data48 >> static_cast<unsigned>(i)) & 1U);
        const unsigned feedback = ((reg >> 15U) & 1U) ^ bit;
        reg = (reg << 1U) & 0xFFFFU;
        if (feedback != 0) {
            reg ^= kDectRCrcGenerator & 0xFFFFU;
        }
    }
    return static_cast<std::uint16_t>(reg ^ 0x0001U);
}

namespace {

// A field of the A-field by its a-numbers, a0 in bit 63 (clause 7.0, MSB
// first).
unsigned a_bits(std::uint64_t a_field, unsigned first, unsigned count) {
    const unsigned shift = 64U - first - count;
    return static_cast<unsigned>((a_field >> shift) & ((1ULL << count) - 1ULL));
}

}  // namespace

Expected<DectDecoder> DectDecoder::create(const DectConfig& config) {
    const double sps = static_cast<double>(config.sample_rate) / kDectSymbolRate;
    if (sps < 2.0) {
        return fail("DECT: sample rate must give at least 2 samples per symbol");
    }
    DectDecoder d;
    d.config_ = config;
    d.sps_ = sps;
    // Channel filter: windowed sinc, cutoff 700 kHz, a little over half the
    // 1,728 MHz channel (clause 4.1), about six symbols long.
    const double rate = static_cast<double>(config.sample_rate);
    const auto half = static_cast<int>(std::ceil(3.0 * sps));
    const double fc = 700e3 / rate;
    double sum = 0.0;
    for (int k = -half; k <= half; ++k) {
        const double x = static_cast<double>(k);
        const double sinc = k == 0 ? 2.0 * fc : std::sin(2.0 * std::numbers::pi * fc * x) / (std::numbers::pi * x);
        const double w = 0.54 + 0.46 * std::cos(std::numbers::pi * x / static_cast<double>(half + 1));
        d.taps_.push_back(static_cast<float>(sinc * w));
        sum += sinc * w;
    }
    for (float& t : d.taps_) {
        t = static_cast<float>(t / sum);
    }
    d.history_.assign(d.taps_.size(), dsp::Complex32{});
    d.nco_step_ = -2.0 * std::numbers::pi * config.frequency_offset_hz / rate;
    // Discriminator output normalised so the nominal deviation is 1.
    d.disc_scale_ = rate / (2.0 * std::numbers::pi * kDectDeviationHz);
    return d;
}

void DectDecoder::reset() {
    *this = create(config_).value_or(DectDecoder{});
}

double DectDecoder::at(double index) const {
    const double rel = index - static_cast<double>(base_);
    const auto i = static_cast<std::size_t>(rel);
    const double frac = rel - static_cast<double>(i);
    if (i + 1 >= freq_.size()) {
        return static_cast<double>(freq_.back());
    }
    return static_cast<double>(freq_[i]) * (1.0 - frac) + static_cast<double>(freq_[i + 1]) * frac;
}

double DectDecoder::correlate(std::size_t t0, double& mean) const {
    std::array<double, kDectSFieldBits> x{};
    double s = 0.0;
    for (std::size_t k = 0; k < kDectSFieldBits; ++k) {
        x[k] = at(static_cast<double>(t0) + static_cast<double>(k) * sps_);
        s += x[k];
    }
    mean = s / static_cast<double>(kDectSFieldBits);
    double dot = 0.0;
    double energy = 0.0;
    for (std::size_t k = 0; k < kDectSFieldBits; ++k) {
        const double v = x[k] - mean;
        // Clause 4.6 RFP pattern, s0 first; clause 5.4.2: a "1" is +f.
        const double p = ((kDectSFieldRfp >> (31U - k)) & 1U) != 0 ? 1.0 : -1.0;
        dot += p * v;
        energy += v * v;
    }
    if (energy <= 0.0) {
        return 0.0;
    }
    // The S-field has as many ones as zeros, so the mean carries the
    // frequency offset and none of the pattern.
    return dot / std::sqrt(static_cast<double>(kDectSFieldBits) * energy);
}

void DectDecoder::process(dsp::ConstComplexSpan samples, std::vector<DectBurst>& out) {
    const std::size_t ntaps = taps_.size();
    for (const dsp::Complex32 in : samples) {
        const dsp::Complex32 rot(static_cast<float>(std::cos(nco_phase_)), static_cast<float>(std::sin(nco_phase_)));
        nco_phase_ = std::remainder(nco_phase_ + nco_step_, 2.0 * std::numbers::pi);
        history_[history_pos_] = in * rot;
        history_pos_ = (history_pos_ + 1) % ntaps;
        dsp::Complex32 y{};
        for (std::size_t k = 0; k < ntaps; ++k) {
            y += taps_[k] * history_[(history_pos_ + k) % ntaps];
        }
        const dsp::Complex32 d = y * std::conj(previous_);
        previous_ = y;
        freq_.push_back(static_cast<float>(std::atan2(d.imag(), d.real()) * disc_scale_));
        ++produced_;
    }

    const auto span_s = static_cast<std::size_t>(std::ceil(static_cast<double>(kDectSFieldBits - 1) * sps_)) + 2;
    const auto span_burst = static_cast<std::size_t>(
        std::ceil(static_cast<double>(kDectSFieldBits + kDectDFieldBits + kDectZFieldBits) * sps_)) + 2;

    while (true) {
        if (pending_) {
            if (best_t0_ + span_burst > produced_) {
                break;
            }
            pending_ = false;
            if (decode(best_t0_, best_rho_, best_mean_, out)) {
                next_ = best_t0_ + static_cast<std::size_t>(
                                       static_cast<double>(kDectSFieldBits + kDectDFieldBits) * sps_);
            } else {
                next_ = best_t0_ + 1;
            }
            continue;
        }
        if (next_ + span_s > produced_) {
            break;
        }
        double mean = 0.0;
        const double rho = correlate(next_, mean);
        if (hunting_) {
            if (std::abs(rho) > std::abs(best_rho_)) {
                best_rho_ = rho;
                best_t0_ = next_;
                best_mean_ = mean;
            }
            if (next_ >= window_end_) {
                hunting_ = false;
                pending_ = true;
            }
        } else if (std::abs(rho) >= config_.threshold) {
            hunting_ = true;
            best_rho_ = rho;
            best_t0_ = next_;
            best_mean_ = mean;
            window_end_ = next_ + static_cast<std::size_t>(std::ceil(sps_)) + 1;
        }
        ++next_;
    }
    trim();
}

void DectDecoder::trim() {
    std::size_t keep = next_;
    if (hunting_ || pending_) {
        keep = std::min(keep, best_t0_);
    }
    if (keep > base_ + 4096) {
        const std::size_t drop = keep - base_ - 16;
        freq_.erase(freq_.begin(), freq_.begin() + static_cast<std::ptrdiff_t>(drop));
        base_ += drop;
    }
}

bool DectDecoder::decode(std::size_t t0, double rho, double mean, std::vector<DectBurst>& out) {
    DectBurst b;
    const std::size_t delay = (taps_.size() - 1) / 2;
    b.sample = t0 >= delay ? t0 - delay : 0;
    b.polarity = rho >= 0.0 ? DectPolarity::Rfp : DectPolarity::Pp;
    b.correlation = static_cast<float>(rho);
    b.frequency_hz = config_.frequency_offset_hz + mean * kDectDeviationHz;

    // EN 300 175-3 Table 6.4: the A-field is d0 to d63.
    std::uint64_t a = 0;
    for (std::size_t k = 0; k < kDectAFieldBits; ++k) {
        const double v = at(static_cast<double>(t0) + static_cast<double>(kDectSFieldBits + k) * sps_) - mean;
        a = (a << 1U) | (v > 0.0 ? 1ULL : 0ULL);
    }
    b.a_field = a;
    // Clause 7.1.1, Figure 7.2: TA a0-a2, Q1 a3, BA a4-a6, Q2 a7.
    b.tail_id = static_cast<DectTail>(a_bits(a, 0, 3));
    b.q1 = a_bits(a, 3, 1) != 0;
    b.ba = a_bits(a, 4, 3);
    b.q2 = a_bits(a, 7, 1) != 0;
    // Clause 7.2.1: tail a8 to a47; clause 6.2.5.1: RA a48 to a63.
    b.tail = (a >> 16U) & ((1ULL << 40U) - 1ULL);
    b.rcrc = static_cast<std::uint16_t>(a & 0xFFFFU);
    b.crc_ok = dect_rcrc(a >> 16U) == b.rcrc;
    if (!b.crc_ok && !config_.emit_crc_failures) {
        return false;
    }

    // Clause 7.2.2 and Table 7.1: 011 always, 010 from an RFP.
    const bool nt = b.tail_id == DectTail::Nt ||
                    (b.tail_id == DectTail::NtConnectionless && b.polarity == DectPolarity::Rfp);
    if (nt) {
        b.rfpi = b.tail;
        b.rfpi_e = a_bits(a, 8, 1) != 0;
    } else if (b.tail_id == DectTail::Qt) {
        // Clause 7.2.3.1, Figure 7.5: QH a8-a11, information a12-a47.
        DectQt q;
        q.qh = a_bits(a, 8, 4);
        q.info = (a >> 16U) & ((1ULL << 36U) - 1ULL);
        // Table 7.3: QH 000X is static system information; Figure 7.6.
        if ((q.qh >> 1U) == 0) {
            DectStaticSystemInfo s;
            s.normal_reverse = a_bits(a, 11, 1) != 0;
            s.slot_number = a_bits(a, 12, 4);
            s.start_position = a_bits(a, 16, 2);
            s.escape = a_bits(a, 18, 1) != 0;
            s.transceivers = a_bits(a, 19, 2);
            s.extended_carriers = a_bits(a, 21, 1) != 0;
            s.rf_carriers = a_bits(a, 22, 10);
            s.carrier_number = a_bits(a, 34, 6);
            s.extended_info = a_bits(a, 40, 1) != 0;
            s.primary_scan_carrier = a_bits(a, 42, 6);
            q.static_info = s;
        }
        b.qt = q;
    }
    // Table 7.2: BA 111 is "no B-field".
    b.b_field_present = b.ba != 7U;
    b.b_field_sample = b.sample + static_cast<dsp::SampleIndex>(std::llround(
                                      static_cast<double>(kDectSFieldBits + kDectAFieldBits) * sps_));
    out.push_back(b);
    return b.crc_ok;
}

}  // namespace revenant::decode
