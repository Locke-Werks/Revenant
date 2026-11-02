// BeiDou B1I D1. See beidou_b1i.h for the document.

#include "core/decode/beidou_b1i.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace revenant::decode::gnss {

namespace {

constexpr std::array<std::uint8_t, 11> kPreamble = {1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0};

double sext(std::uint32_t v, unsigned bits) {
    const auto x = static_cast<std::int64_t>(v & ((bits >= 32) ? 0xFFFFFFFFU : ((1U << bits) - 1U)));
    return static_cast<double>((x >> (bits - 1U)) != 0 ? x - (std::int64_t{1} << bits) : x);
}

}  // namespace

unsigned b1i_bch_syndrome(std::uint16_t word) {
    // Polynomial division by X^4 + X + 1, highest power (first-sent) first.
    unsigned r = word & 0x7FFFU;
    for (int bit = 14; bit >= 4; --bit) {
        if (((r >> static_cast<unsigned>(bit)) & 1U) != 0) {
            r ^= 0x13U << static_cast<unsigned>(bit - 4);
        }
    }
    return r & 0xFU;
}

std::uint16_t b1i_bch_encode(std::uint16_t info) {
    const auto shifted = static_cast<std::uint16_t>((info & 0x7FFU) << 4U);
    return static_cast<std::uint16_t>(shifted | b1i_bch_syndrome(shifted));
}

std::uint16_t b1i_bch_decode(std::uint16_t word, bool& corrected) {
    corrected = false;
    const unsigned s = b1i_bch_syndrome(word);
    if (s != 0) {
        // Table 5-2 is this lookup: the single error whose remainder is s.
        for (unsigned i = 0; i < 15; ++i) {
            if (b1i_bch_syndrome(static_cast<std::uint16_t>(1U << i)) == s) {
                word = static_cast<std::uint16_t>(word ^ (1U << i));
                corrected = true;
                break;
            }
        }
    }
    return static_cast<std::uint16_t>((word >> 4U) & 0x7FFU);
}

std::uint32_t B1iSubframe::field(unsigned first, unsigned count) const {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < count; ++i) {
        v = (v << 1U) | bits[first + i];
    }
    return v;
}

std::optional<B1iClockIono> b1i_clock_iono(const B1iSubframe& s) {
    if (s.fra_id != 1) {
        return std::nullopt;
    }
    // Figure 5-8 positions; Tables 5-5 and 5-7, 5.2.4.10.
    B1iClockIono c;
    c.sat_h1 = s.field(43, 1) != 0;
    c.aodc = s.field(44, 5);
    c.urai = s.field(49, 4);
    c.week = s.field(61, 13);
    c.toc_s = static_cast<double>((s.field(74, 9) << 8U) | s.field(91, 8)) * 8.0;
    c.tgd1_s = sext(s.field(99, 10), 10) * 0.1e-9;
    c.tgd2_s = sext((s.field(109, 4) << 6U) | s.field(121, 6), 10) * 0.1e-9;
    c.alpha[0] = sext(s.field(127, 8), 8) * std::ldexp(1.0, -30);
    c.alpha[1] = sext(s.field(135, 8), 8) * std::ldexp(1.0, -27);
    c.alpha[2] = sext(s.field(151, 8), 8) * std::ldexp(1.0, -24);
    c.alpha[3] = sext(s.field(159, 8), 8) * std::ldexp(1.0, -24);
    c.beta[0] = sext((s.field(167, 6) << 2U) | s.field(181, 2), 8) * std::ldexp(1.0, 11);
    c.beta[1] = sext(s.field(183, 8), 8) * std::ldexp(1.0, 14);
    c.beta[2] = sext(s.field(191, 8), 8) * std::ldexp(1.0, 16);
    c.beta[3] = sext((s.field(199, 4) << 4U) | s.field(211, 4), 8) * std::ldexp(1.0, 16);
    c.a2 = sext(s.field(215, 11), 11) * std::ldexp(1.0, -66);
    c.a0 = sext((s.field(226, 7) << 17U) | s.field(241, 17), 24) * std::ldexp(1.0, -33);
    c.a1 = sext((s.field(258, 5) << 17U) | s.field(271, 17), 22) * std::ldexp(1.0, -50);
    c.aode = s.field(288, 5);
    return c;
}

std::optional<B1iEphemeris> b1i_ephemeris(const B1iSubframe& s2, const B1iSubframe& s3) {
    if (s2.fra_id != 2 || s3.fra_id != 3) {
        return std::nullopt;
    }
    // Figures 5-9 and 5-10; Table 5-10.
    B1iEphemeris e;
    e.delta_n_sc_s = sext((s2.field(43, 10) << 6U) | s2.field(61, 6), 16) * std::ldexp(1.0, -43);
    e.cuc_rad = sext((s2.field(67, 16) << 2U) | s2.field(91, 2), 18) * std::ldexp(1.0, -31);
    e.m0_sc = sext((s2.field(93, 20) << 12U) | s2.field(121, 12), 32) * std::ldexp(1.0, -31);
    e.e = static_cast<double>((s2.field(133, 10) << 22U) | s2.field(151, 22)) * std::ldexp(1.0, -33);
    e.cus_rad = sext(s2.field(181, 18), 18) * std::ldexp(1.0, -31);
    e.crc_m = sext((s2.field(199, 4) << 14U) | s2.field(211, 14), 18) * std::ldexp(1.0, -6);
    e.crs_m = sext((s2.field(225, 8) << 10U) | s2.field(241, 10), 18) * std::ldexp(1.0, -6);
    e.sqrt_a = static_cast<double>((s2.field(251, 12) << 20U) | s2.field(271, 20)) * std::ldexp(1.0, -19);
    const std::uint32_t toe = (s2.field(291, 2) << 15U) | (s3.field(43, 10) << 5U) | s3.field(61, 5);
    e.toe_s = static_cast<double>(toe) * 8.0;
    e.i0_sc = sext((s3.field(66, 17) << 15U) | s3.field(91, 15), 32) * std::ldexp(1.0, -31);
    e.cic_rad = sext((s3.field(106, 7) << 11U) | s3.field(121, 11), 18) * std::ldexp(1.0, -31);
    e.omega_dot_sc_s = sext((s3.field(132, 11) << 13U) | s3.field(151, 13), 24) * std::ldexp(1.0, -43);
    e.cis_rad = sext((s3.field(164, 9) << 9U) | s3.field(181, 9), 18) * std::ldexp(1.0, -31);
    e.idot_sc_s = sext((s3.field(190, 13) << 1U) | s3.field(211, 1), 14) * std::ldexp(1.0, -43);
    e.omega0_sc = sext((s3.field(212, 21) << 11U) | s3.field(241, 11), 32) * std::ldexp(1.0, -31);
    e.omega_sc = sext((s3.field(252, 11) << 21U) | s3.field(271, 21), 32) * std::ldexp(1.0, -31);
    return e;
}

// ---------------------------------------------------------------------------

void B1iNhSync::push(const Prompt& p, std::vector<std::pair<double, SampleIndex>>& out) {
    static const auto nh = beidou_nh20();
    if (!p.phase_locked && !synced_) {
        seen_ = 0;
        return;
    }
    const auto slot = static_cast<unsigned>(p.period % 20);
    ring_[slot] = p.prompt.real();
    at_[slot] = p.sample;
    if (!synced_) {
        if (++seen_ < 20) {
            return;
        }
        // The window that ends at this period started at slot + 1.
        const unsigned start = (slot + 1) % 20;
        double c = 0.0;
        for (unsigned j = 0; j < 20; ++j) {
            c += ring_[(start + j) % 20] * nh[j];
        }
        metric_[start] += std::abs(c);
        if (seen_ >= 600) {
            const auto best = static_cast<unsigned>(
                std::max_element(metric_.begin(), metric_.end()) - metric_.begin());
            double second = 0.0;
            for (unsigned k = 0; k < 20; ++k) {
                if (k != best) {
                    second = std::max(second, metric_[k]);
                }
            }
            if (metric_[best] > 1.5 * second) {
                synced_ = true;
                offset_ = best;
            } else {
                metric_.fill(0.0);
                seen_ = 0;
            }
        }
        return;
    }
    // Emit when the window starting at offset_ is complete.
    if ((slot + 1) % 20 == offset_) {
        double c = 0.0;
        for (unsigned j = 0; j < 20; ++j) {
            c += ring_[(offset_ + j) % 20] * nh[j];
        }
        out.emplace_back(c, at_[offset_]);
    }
}

void B1iFramer::push(std::uint8_t bit, SampleIndex sample, std::vector<B1iSubframe>& out) {
    bits_.push_back(bit);
    at_.push_back(sample);
    auto preamble_at = [&](std::size_t s, bool& inverted) {
        bool ok = true;
        bool inv = true;
        for (std::size_t i = 0; i < 11; ++i) {
            ok = ok && bits_[s + i] == kPreamble[i];
            inv = inv && bits_[s + i] != kPreamble[i];
        }
        inverted = inv;
        return ok || inv;
    };
    while (bits_.size() >= 311) {
        bool inv0 = false;
        bool inv1 = false;
        if (!preamble_at(0, inv0) || !preamble_at(300, inv1) || inv0 != inv1) {
            bits_.erase(bits_.begin());
            at_.erase(at_.begin());
            continue;
        }
        B1iSubframe sf;
        sf.prn = prn_;
        sf.sample = at_[0];
        auto rx = [&](std::size_t i) { return static_cast<unsigned>(bits_[i] ^ (inv0 ? 1U : 0U)); };
        // Word 1: 15 raw bits, then one codeword.
        for (unsigned i = 0; i < 15; ++i) {
            sf.bits[1 + i] = static_cast<std::uint8_t>(rx(i));
        }
        std::uint16_t cw = 0;
        for (unsigned i = 15; i < 30; ++i) {
            cw = static_cast<std::uint16_t>((cw << 1U) | rx(i));
        }
        bool fixed = false;
        std::uint16_t info = b1i_bch_decode(cw, fixed);
        sf.corrections += fixed ? 1U : 0U;
        for (unsigned i = 0; i < 11; ++i) {
            sf.bits[16 + i] = static_cast<std::uint8_t>((info >> (10U - i)) & 1U);
        }
        // Words 2-10: de-interleave by turns of one bit.
        for (unsigned w = 1; w < 10; ++w) {
            std::uint16_t a = 0;
            std::uint16_t b = 0;
            for (unsigned i = 0; i < 15; ++i) {
                a = static_cast<std::uint16_t>((a << 1U) | rx(30 * w + 2 * i));
                b = static_cast<std::uint16_t>((b << 1U) | rx(30 * w + 2 * i + 1));
            }
            bool fa = false;
            bool fb = false;
            const std::uint16_t ia = b1i_bch_decode(a, fa);
            const std::uint16_t ib = b1i_bch_decode(b, fb);
            sf.corrections += (fa ? 1U : 0U) + (fb ? 1U : 0U);
            for (unsigned i = 0; i < 11; ++i) {
                sf.bits[30 * w + 1 + i] = static_cast<std::uint8_t>((ia >> (10U - i)) & 1U);
                sf.bits[30 * w + 12 + i] = static_cast<std::uint8_t>((ib >> (10U - i)) & 1U);
            }
        }
        sf.fra_id = sf.field(16, 3);
        sf.sow = (sf.field(19, 8) << 12U) | sf.field(31, 12);
        if (sf.fra_id >= 1 && sf.fra_id <= 5) {
            out.push_back(sf);
        }
        bits_.erase(bits_.begin(), bits_.begin() + 300);
        at_.erase(at_.begin(), at_.begin() + 300);
    }
}

// ---------------------------------------------------------------------------

Expected<BeidouB1iDecoder> BeidouB1iDecoder::create(BeidouB1iConfig config) {
    if (config.prns.empty()) {
        for (unsigned p = 1; p <= 63; ++p) {
            config.prns.push_back(p);
        }
    }
    std::vector<SignalSpec> signals;
    for (unsigned prn : config.prns) {
        SignalSpec s;
        s.id = prn;
        s.code = beidou_b1i_code(prn);
        if (s.code.empty()) {
            return fail("beidou b1i: no code for PRN " + std::to_string(prn));
        }
        s.chip_rate_hz = 2.046e6;
        s.carrier_hz = 1561.098e6;
        s.offset_hz = config.offset_hz;
        signals.push_back(std::move(s));
    }
    config.front_end.sample_rate = config.sample_rate;
    auto fe = FrontEnd::create(config.front_end, std::move(signals));
    if (!fe) {
        return std::unexpected(fe.error());
    }
    return BeidouB1iDecoder(std::move(config), std::move(*fe));
}

void BeidouB1iDecoder::feed(std::span<const dsp::Complex32> samples, BeidouB1iOutput& out) {
    scratch_.prompts.clear();
    scratch_.acquisitions.clear();
    scratch_.losses.clear();
    fe_.feed(samples, scratch_);
    out.acquisitions.insert(out.acquisitions.end(), scratch_.acquisitions.begin(),
                            scratch_.acquisitions.end());
    for (const auto& loss : scratch_.losses) {
        sats_.erase(loss.id);
    }
    std::vector<std::pair<double, SampleIndex>> bits;
    std::vector<B1iSubframe> subframes;
    for (const auto& p : scratch_.prompts) {
        auto it = sats_.find(p.id);
        if (it == sats_.end()) {
            it = sats_.emplace(p.id, Sat{B1iNhSync{}, B1iFramer{p.id}, {}}).first;
        }
        Sat& sat = it->second;
        if (config_.status_periods != 0 && p.period % config_.status_periods == 0) {
            out.status.push_back(
                {p.id, p.cn0_dbhz, p.doppler_hz, p.phase_locked, sat.sync.synced(), p.sample});
        }
        if (is_geo(p.id)) {
            continue;  // D2 is not framed here; see the header
        }
        bits.clear();
        sat.sync.push(p, bits);
        for (const auto& [v, s] : bits) {
            subframes.clear();
            sat.framer.push(v < 0.0 ? 1 : 0, s, subframes);
            for (const auto& sf : subframes) {
                out.subframes.push_back(sf);
                if (auto c = b1i_clock_iono(sf)) {
                    out.clocks.emplace_back(sf.prn, *c);
                }
                if (sf.fra_id == 2) {
                    sat.sf2 = sf;
                } else if (sf.fra_id == 3 && sat.sf2 && sat.sf2->sow + 6 == sf.sow) {
                    if (auto e = b1i_ephemeris(*sat.sf2, sf)) {
                        out.ephemerides.emplace_back(sf.prn, *e);
                    }
                    sat.sf2.reset();
                }
            }
        }
    }
}

}  // namespace revenant::decode::gnss
