// GPS and QZSS L1 C/A LNAV. See gps_lnav.h for the document and sections.

#include "core/decode/gps_lnav.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace revenant::decode::gnss {

namespace {

// IS-GPS-200N Table 20-XIV, the d-bit sets of D25..D30, by 1-based d index,
// zero-terminated.
constexpr std::array<std::array<unsigned char, 16>, 6> kParityTerms = {{
    {1, 2, 3, 5, 6, 10, 11, 12, 13, 14, 17, 18, 20, 23},
    {2, 3, 4, 6, 7, 11, 12, 13, 14, 15, 18, 19, 21, 24},
    {1, 3, 4, 5, 7, 8, 12, 13, 14, 15, 16, 19, 20, 22},
    {2, 4, 5, 6, 8, 9, 13, 14, 15, 16, 17, 20, 21, 23},
    {1, 3, 5, 6, 7, 9, 10, 14, 15, 16, 17, 18, 21, 22, 24},
    {3, 5, 6, 8, 9, 10, 11, 13, 15, 19, 22, 23, 24},
}};
// Which of D29* (false) or D30* (true) each equation starts from.
constexpr std::array<bool, 6> kParityStar = {false, true, false, true, true, false};

constexpr std::uint8_t kPreamble = 0b10001011;  // IS-GPS-200N Figure 20-2

double sext(std::uint32_t v, unsigned bits) {
    const std::uint32_t sign = 1U << (bits - 1U);
    const auto x = static_cast<std::int64_t>(v & ((bits >= 32) ? 0xFFFFFFFFU : ((1U << bits) - 1U)));
    return static_cast<double>((x & sign) != 0 ? x - (std::int64_t{1} << bits) : x);
}

double u(std::uint32_t v) { return static_cast<double>(v); }

}  // namespace

std::uint32_t lnav_parity(std::uint32_t d24, bool d29_star, bool d30_star) {
    std::uint32_t out = 0;
    for (std::size_t k = 0; k < 6; ++k) {
        unsigned p = kParityStar[k] ? (d30_star ? 1U : 0U) : (d29_star ? 1U : 0U);
        for (unsigned i : kParityTerms[k]) {
            if (i == 0) {
                break;
            }
            p ^= (d24 >> (24U - i)) & 1U;
        }
        out = (out << 1U) | p;
    }
    return out;
}

std::optional<std::uint32_t> lnav_check_word(std::uint32_t word30, bool d29_star, bool d30_star) {
    std::uint32_t d24 = (word30 >> 6U) & 0xFFFFFFU;
    if (d30_star) {
        d24 ^= 0xFFFFFFU;  // 20.3.5.2: complement D1..D24 when D30* is one
    }
    if (lnav_parity(d24, d29_star, d30_star) != (word30 & 0x3FU)) {
        return std::nullopt;
    }
    return d24;
}

std::uint32_t LnavSubframe::bits(unsigned first, unsigned count) const {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned n = first + i - 1;
        const unsigned w = n / 30;
        const unsigned b = n % 30;
        v = (v << 1U) | ((w < 10 && b < 24) ? ((data[w] >> (23U - b)) & 1U) : 0U);
    }
    return v;
}

std::optional<LnavClock> lnav_clock(const LnavSubframe& sf) {
    if (sf.id != 1) {
        return std::nullopt;
    }
    // IS-GPS-200N Figure 20-1 sheet 1, Table 20-I.
    LnavClock c;
    c.week = sf.bits(61, 10);
    c.l2_codes = sf.bits(71, 2);
    c.ura_index = sf.bits(73, 4);
    c.health = sf.bits(77, 6);
    c.iodc = (sf.bits(83, 2) << 8U) | sf.bits(211, 8);
    c.l2_p_data = sf.bits(91, 1) != 0;
    c.tgd_s = sext(sf.bits(197, 8), 8) * std::ldexp(1.0, -31);
    c.toc_s = u(sf.bits(219, 16)) * 16.0;
    c.af2 = sext(sf.bits(241, 8), 8) * std::ldexp(1.0, -55);
    c.af1 = sext(sf.bits(249, 16), 16) * std::ldexp(1.0, -43);
    c.af0 = sext(sf.bits(271, 22), 22) * std::ldexp(1.0, -31);
    return c;
}

std::optional<LnavEphemeris> lnav_ephemeris(const LnavSubframe& s2, const LnavSubframe& s3) {
    if (s2.id != 2 || s3.id != 3 || s2.bits(61, 8) != s3.bits(271, 8)) {
        return std::nullopt;
    }
    // IS-GPS-200N Figure 20-1 sheets 2 and 3, Table 20-III.
    LnavEphemeris e;
    e.iode = s2.bits(61, 8);
    e.crs_m = sext(s2.bits(69, 16), 16) * std::ldexp(1.0, -5);
    e.delta_n_sc_s = sext(s2.bits(91, 16), 16) * std::ldexp(1.0, -43);
    e.m0_sc = sext((s2.bits(107, 8) << 24U) | s2.bits(121, 24), 32) * std::ldexp(1.0, -31);
    e.cuc_rad = sext(s2.bits(151, 16), 16) * std::ldexp(1.0, -29);
    e.e = u((s2.bits(167, 8) << 24U) | s2.bits(181, 24)) * std::ldexp(1.0, -33);
    e.cus_rad = sext(s2.bits(211, 16), 16) * std::ldexp(1.0, -29);
    e.sqrt_a = u((s2.bits(227, 8) << 24U) | s2.bits(241, 24)) * std::ldexp(1.0, -19);
    e.toe_s = u(s2.bits(271, 16)) * 16.0;
    e.fit_interval = s2.bits(287, 1) != 0;
    e.aodo = s2.bits(288, 5);
    e.cic_rad = sext(s3.bits(61, 16), 16) * std::ldexp(1.0, -29);
    e.omega0_sc = sext((s3.bits(77, 8) << 24U) | s3.bits(91, 24), 32) * std::ldexp(1.0, -31);
    e.cis_rad = sext(s3.bits(121, 16), 16) * std::ldexp(1.0, -29);
    e.i0_sc = sext((s3.bits(137, 8) << 24U) | s3.bits(151, 24), 32) * std::ldexp(1.0, -31);
    e.crc_m = sext(s3.bits(181, 16), 16) * std::ldexp(1.0, -5);
    e.omega_sc = sext((s3.bits(197, 8) << 24U) | s3.bits(211, 24), 32) * std::ldexp(1.0, -31);
    e.omega_dot_sc_s = sext(s3.bits(241, 24), 24) * std::ldexp(1.0, -43);
    e.idot_sc_s = sext(s3.bits(279, 14), 14) * std::ldexp(1.0, -43);
    return e;
}

std::optional<LnavAlmanac> lnav_almanac(const LnavSubframe& sf) {
    // Table 20-V: SV IDs 1 to 32 are almanac pages, in subframe 5 pages 1-24
    // and subframe 4 pages 2-5 and 7-10. Sheet 4 is their layout.
    if ((sf.id != 4 && sf.id != 5) || sf.page_sv_id < 1 || sf.page_sv_id > 32) {
        return std::nullopt;
    }
    LnavAlmanac a;
    a.sv_id = sf.page_sv_id;
    a.e = u(sf.bits(69, 16)) * std::ldexp(1.0, -21);
    a.toa_s = u(sf.bits(91, 8)) * 4096.0;
    a.delta_i_sc = sext(sf.bits(99, 16), 16) * std::ldexp(1.0, -19);
    a.omega_dot_sc_s = sext(sf.bits(121, 16), 16) * std::ldexp(1.0, -38);
    a.health = sf.bits(137, 8);
    a.sqrt_a = u(sf.bits(151, 24)) * std::ldexp(1.0, -11);
    a.omega0_sc = sext(sf.bits(181, 24), 24) * std::ldexp(1.0, -23);
    a.omega_sc = sext(sf.bits(211, 24), 24) * std::ldexp(1.0, -23);
    a.m0_sc = sext(sf.bits(241, 24), 24) * std::ldexp(1.0, -23);
    a.af0_s = sext((sf.bits(271, 8) << 3U) | sf.bits(290, 3), 11) * std::ldexp(1.0, -20);
    a.af1 = sext(sf.bits(279, 11), 11) * std::ldexp(1.0, -38);
    return a;
}

std::optional<LnavUtcIono> lnav_utc_iono(const LnavSubframe& sf) {
    // Page 18 of subframe 4 is SV ID 56 (Table 20-V); layout sheet 8.
    if (sf.id != 4 || sf.page_sv_id != 56) {
        return std::nullopt;
    }
    LnavUtcIono x;
    x.alpha[0] = sext(sf.bits(69, 8), 8) * std::ldexp(1.0, -30);
    x.alpha[1] = sext(sf.bits(77, 8), 8) * std::ldexp(1.0, -27);
    x.alpha[2] = sext(sf.bits(91, 8), 8) * std::ldexp(1.0, -24);
    x.alpha[3] = sext(sf.bits(99, 8), 8) * std::ldexp(1.0, -24);
    x.beta[0] = sext(sf.bits(107, 8), 8) * std::ldexp(1.0, 11);
    x.beta[1] = sext(sf.bits(121, 8), 8) * std::ldexp(1.0, 14);
    x.beta[2] = sext(sf.bits(129, 8), 8) * std::ldexp(1.0, 16);
    x.beta[3] = sext(sf.bits(137, 8), 8) * std::ldexp(1.0, 16);
    x.a1 = sext(sf.bits(151, 24), 24) * std::ldexp(1.0, -50);
    x.a0_s = sext((sf.bits(181, 24) << 8U) | sf.bits(211, 8), 32) * std::ldexp(1.0, -30);
    x.tot_s = u(sf.bits(219, 8)) * 4096.0;
    x.wnt = sf.bits(227, 8);
    x.dt_ls = static_cast<int>(sext(sf.bits(241, 8), 8));
    x.wn_lsf = sf.bits(249, 8);
    x.dn = sf.bits(257, 8);
    x.dt_lsf = static_cast<int>(sext(sf.bits(271, 8), 8));
    return x;
}

// ---------------------------------------------------------------------------

void LnavBitSync::push(const Prompt& p, std::vector<std::pair<std::uint8_t, SampleIndex>>& out) {
    std::vector<std::pair<double, SampleIndex>> soft;
    push_soft(p, soft);
    for (const auto& [v, s] : soft) {
        out.emplace_back(v < 0.0 ? 1 : 0, s);
    }
}

void LnavBitSync::push_soft(const Prompt& p, std::vector<std::pair<double, SampleIndex>>& out) {
    const double i = p.prompt.real();
    const auto phase = static_cast<unsigned>(p.period % n_);
    if (!synced_) {
        if (!p.phase_locked) {
            last_i_ = 0.0;
            return;
        }
        if (last_i_ != 0.0 && (i < 0.0) != (last_i_ < 0.0)) {
            ++hist_[phase];
        }
        last_i_ = i;
        if (++seen_ >= 50 * n_) {
            unsigned best = 0;
            for (unsigned k = 1; k < n_; ++k) {
                if (hist_[k] > hist_[best]) {
                    best = k;
                }
            }
            unsigned second = 0;
            for (unsigned k = 0; k < n_; ++k) {
                if (k != best) {
                    second = std::max(second, hist_[k]);
                }
            }
            if (hist_[best] >= 8 && hist_[best] >= 3 * second) {
                synced_ = true;
                edge_ = best;
                count_ = 0;
                acc_ = 0.0;
            } else if (seen_ >= 200 * n_) {
                std::fill(hist_.begin(), hist_.end(), 0U);
                seen_ = 0;
            }
        }
        if (!synced_) {
            return;
        }
    }
    if (phase == edge_) {
        if (count_ == n_) {
            out.emplace_back(acc_, bit_start_);
        }
        acc_ = 0.0;
        count_ = 0;
        bit_start_ = p.sample;
    }
    acc_ += i;
    ++count_;
}

void LnavFramer::push(std::uint8_t bit, SampleIndex sample, std::vector<LnavSubframe>& out) {
    bits_.push_back(bit);
    samples_.push_back(sample);
    auto word = [&](std::size_t at) {
        std::uint32_t w = 0;
        for (std::size_t k = 0; k < 30; ++k) {
            w = (w << 1U) | bits_[at + k];
        }
        return lnav_check_word(w, bits_[at - 2] != 0, bits_[at - 1] != 0);
    };
    // Subframes start at index 2 once the buffer is trimmed to the two bits
    // before a candidate; search by sliding that window.
    while (bits_.size() >= 62) {
        const auto w1 = word(2);
        const auto w2 = w1 ? word(32) : std::nullopt;
        const bool header = w1 && w2 && ((*w1 >> 16U) & 0xFFU) == kPreamble &&
                            ((*w2 >> 2U) & 7U) >= 1 && ((*w2 >> 2U) & 7U) <= 5;
        if (!header) {
            bits_.erase(bits_.begin());
            samples_.erase(samples_.begin());
            continue;
        }
        if (bits_.size() < 302) {
            return;
        }
        LnavSubframe sf;
        sf.prn = prn_;
        bool ok = true;
        for (std::size_t w = 0; w < 10 && ok; ++w) {
            const auto d = word(2 + 30 * w);
            if (!d) {
                ok = false;
            } else {
                sf.data[w] = *d;
            }
        }
        if (!ok) {
            bits_.erase(bits_.begin());
            samples_.erase(samples_.begin());
            continue;
        }
        sf.id = (sf.data[1] >> 2U) & 7U;
        sf.tow_count = sf.data[1] >> 7U;
        sf.alert = ((sf.data[1] >> 6U) & 1U) != 0;
        sf.anti_spoof = ((sf.data[1] >> 5U) & 1U) != 0;
        sf.integrity_status = ((sf.data[0] >> 1U) & 1U) != 0;
        sf.page_sv_id = (sf.id == 4 || sf.id == 5) ? sf.bits(63, 6) : 0U;
        sf.sample = samples_[2];
        out.push_back(sf);
        bits_.erase(bits_.begin(), bits_.begin() + 300);
        samples_.erase(samples_.begin(), samples_.begin() + 300);
    }
}

// ---------------------------------------------------------------------------

Expected<GpsL1caDecoder> GpsL1caDecoder::create(GpsL1caConfig config) {
    if (config.prns.empty()) {
        for (unsigned p = 1; p <= 32; ++p) {
            config.prns.push_back(p);
        }
    }
    std::vector<SignalSpec> signals;
    for (unsigned prn : config.prns) {
        SignalSpec s;
        s.id = prn;
        s.code = gps_ca_code(prn);
        if (s.code.empty()) {
            return fail("gps l1 c/a: no code is assigned to PRN " + std::to_string(prn));
        }
        s.chip_rate_hz = 1.023e6;
        s.carrier_hz = 1575.42e6;
        s.offset_hz = config.offset_hz;
        signals.push_back(std::move(s));
    }
    config.front_end.sample_rate = config.sample_rate;
    auto fe = FrontEnd::create(config.front_end, std::move(signals));
    if (!fe) {
        return std::unexpected(fe.error());
    }
    return GpsL1caDecoder(std::move(config), std::move(*fe));
}

void GpsL1caDecoder::feed(std::span<const dsp::Complex32> samples, GpsL1caOutput& out) {
    scratch_.prompts.clear();
    scratch_.acquisitions.clear();
    scratch_.losses.clear();
    fe_.feed(samples, scratch_);
    out.acquisitions.insert(out.acquisitions.end(), scratch_.acquisitions.begin(),
                            scratch_.acquisitions.end());
    for (const auto& loss : scratch_.losses) {
        sats_.erase(loss.id);
    }
    std::vector<std::pair<std::uint8_t, SampleIndex>> bits;
    std::vector<LnavSubframe> subframes;
    for (const auto& p : scratch_.prompts) {
        auto it = sats_.find(p.id);
        if (it == sats_.end()) {
            it = sats_.emplace(p.id, Sat{LnavBitSync{}, LnavFramer{p.id}, {}, {}}).first;
        }
        Sat& sat = it->second;
        bits.clear();
        sat.sync.push(p, bits);
        if (config_.status_periods != 0 && p.period % config_.status_periods == 0) {
            out.status.push_back(
                {p.id, p.cn0_dbhz, p.doppler_hz, p.phase_locked, sat.sync.synced(), p.sample});
        }
        for (const auto& [b, s] : bits) {
            subframes.clear();
            sat.framer.push(b, s, subframes);
            for (const auto& sf : subframes) {
                out.subframes.push_back(sf);
                if (auto c = lnav_clock(sf)) {
                    out.clocks.emplace_back(sf.prn, *c);
                }
                if (sf.id == 2) {
                    sat.sf2 = sf;
                }
                if (sf.id == 3) {
                    sat.sf3 = sf;
                }
                if ((sf.id == 2 || sf.id == 3) && sat.sf2 && sat.sf3) {
                    if (auto e = lnav_ephemeris(*sat.sf2, *sat.sf3)) {
                        out.ephemerides.emplace_back(sf.prn, *e);
                        sat.sf2.reset();
                        sat.sf3.reset();
                    }
                }
                if (auto a = lnav_almanac(sf)) {
                    out.almanacs.push_back(*a);
                }
                if (auto x = lnav_utc_iono(sf)) {
                    out.utc_iono.push_back(*x);
                }
            }
        }
    }
}

}  // namespace revenant::decode::gnss
