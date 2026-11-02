// GLONASS L1OF navigation message. See glonass_l1.h for the document.

#include "core/decode/glonass_l1.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace revenant::decode::gnss {

namespace {

// ICD 5.1 4.7, Table 4.13: the explicit lists for C1 and C2, the ranges for
// C3 to C7.
constexpr std::array<unsigned char, 41> kC1 = {9,  10, 12, 13, 15, 17, 19, 20, 22, 24, 26, 28, 30, 32,
                                               34, 35, 37, 39, 41, 43, 45, 47, 49, 51, 53, 55, 57, 59,
                                               61, 63, 65, 66, 68, 70, 72, 74, 76, 78, 80, 82, 84};
constexpr std::array<unsigned char, 41> kC2 = {9,  11, 12, 14, 15, 18, 19, 21, 22, 25, 26, 29, 30, 33,
                                               34, 36, 37, 40, 41, 44, 45, 48, 49, 52, 53, 56, 57, 60,
                                               61, 64, 65, 67, 68, 71, 72, 75, 76, 79, 80, 83, 84};
struct Range {
    unsigned char lo, hi;
};
constexpr std::array<Range, 11> kC3 = {{{10, 12}, {16, 19}, {23, 26}, {31, 34}, {38, 41}, {46, 49},
                                        {54, 57}, {62, 65}, {69, 72}, {77, 80}, {85, 85}}};
constexpr std::array<Range, 5> kC4 = {{{13, 19}, {27, 34}, {42, 49}, {58, 65}, {73, 80}}};
constexpr std::array<Range, 3> kC5 = {{{20, 34}, {50, 65}, {81, 85}}};

template <std::size_t N>
bool in_ranges(const std::array<Range, N>& r, unsigned bit) {
    return std::any_of(r.begin(), r.end(), [&](const Range& x) { return bit >= x.lo && bit <= x.hi; });
}

double signmag(std::uint32_t v, unsigned bits, double lsb) {
    const std::uint32_t mag = v & ((1U << (bits - 1U)) - 1U);
    const bool neg = ((v >> (bits - 1U)) & 1U) != 0;
    return (neg ? -1.0 : 1.0) * static_cast<double>(mag) * lsb;
}

constexpr const char* kTimeMark = "111110001101110101000010010110";

}  // namespace

bool glonass_in_check_set(unsigned check, unsigned bit) {
    switch (check) {
    case 1:
        return std::find(kC1.begin(), kC1.end(), bit) != kC1.end();
    case 2:
        return std::find(kC2.begin(), kC2.end(), bit) != kC2.end();
    case 3:
        return in_ranges(kC3, bit);
    case 4:
        return in_ranges(kC4, bit);
    case 5:
        return in_ranges(kC5, bit);
    case 6:
        return bit >= 35 && bit <= 65;
    case 7:
        return bit >= 66 && bit <= 85;
    default:
        return false;
    }
}

void glonass_fill_check(GlonassStringBits& b) {
    std::uint8_t all = 0;
    for (unsigned k = 1; k <= 7; ++k) {
        std::uint8_t c = 0;
        for (unsigned i = 9; i <= 85; ++i) {
            if (glonass_in_check_set(k, i)) {
                c ^= b[i];
            }
        }
        b[k] = c;
        all ^= c;
    }
    for (unsigned i = 9; i <= 85; ++i) {
        all ^= b[i];
    }
    b[8] = all;
}

GlonassCheck glonass_check_string(GlonassStringBits& b) {
    std::array<unsigned, 8> c{};
    for (unsigned k = 1; k <= 7; ++k) {
        unsigned v = b[k];
        for (unsigned i = 9; i <= 85; ++i) {
            if (glonass_in_check_set(k, i)) {
                v ^= b[i];
            }
        }
        c[k] = v;
    }
    unsigned call = 0;
    for (unsigned q = 1; q <= 85; ++q) {
        call ^= b[q];
    }
    unsigned ones = 0;
    unsigned syndrome = 0;
    unsigned most = 0;
    for (unsigned k = 1; k <= 7; ++k) {
        ones += c[k];
        syndrome |= c[k] << (k - 1U);
        if (c[k] != 0) {
            most = k;
        }
    }
    if (ones == 0 && call == 0) {
        return GlonassCheck::Clean;
    }
    if (ones == 1 && call == 1) {
        return GlonassCheck::Corrected;  // a check bit was wrong; data intact
    }
    if (ones >= 2 && call == 1) {
        const unsigned icor = syndrome + 8U - most;
        if (icor > 85) {
            return GlonassCheck::Erased;
        }
        b[icor] ^= 1U;
        return GlonassCheck::Corrected;
    }
    return GlonassCheck::Erased;
}

std::array<std::uint8_t, 30> glonass_time_mark() {
    std::array<std::uint8_t, 30> t{};
    for (unsigned i = 0; i < 30; ++i) {
        t[i] = kTimeMark[i] == '1' ? 1 : 0;
    }
    return t;
}

std::uint32_t GlonassString::field(unsigned hi, unsigned lo) const {
    std::uint32_t v = 0;
    for (unsigned i = hi; i >= lo && i >= 1; --i) {
        v = (v << 1U) | bits[i];
        if (i == lo) {
            break;
        }
    }
    return v;
}

std::optional<GlonassEphemeris> glonass_ephemeris(std::span<const GlonassString, 4> s) {
    for (unsigned i = 0; i < 4; ++i) {
        if (s[i].number != i + 1 || s[i].check == GlonassCheck::Erased) {
            return std::nullopt;
        }
    }
    // ICD 5.1 Table 4.6 positions, Table 4.5 scales.
    GlonassEphemeris e;
    e.channel = s[0].channel;
    e.p1 = s[0].field(78, 77);
    e.tk_h = s[0].field(76, 72);
    e.tk_min = s[0].field(71, 66);
    e.tk_30s = s[0].field(65, 65);
    const std::array<const GlonassString*, 3> xyz = {&s[0], &s[1], &s[2]};
    for (unsigned a = 0; a < 3; ++a) {
        e.vel_km_s[a] = signmag(xyz[a]->field(64, 41), 24, std::ldexp(1.0, -20));
        e.acc_km_s2[a] = signmag(xyz[a]->field(40, 36), 5, std::ldexp(1.0, -30));
        e.pos_km[a] = signmag(xyz[a]->field(35, 9), 27, std::ldexp(1.0, -11));
    }
    e.bn = s[1].field(80, 78);
    e.p2 = s[1].field(77, 77) != 0;
    e.tb_min = s[1].field(76, 70) * 15U;
    e.p3 = s[2].field(80, 80) != 0;
    e.gamma_n = signmag(s[2].field(79, 69), 11, std::ldexp(1.0, -40));
    e.p = s[2].field(67, 66);
    e.ln3 = s[2].field(65, 65) != 0;
    e.tau_n_s = signmag(s[3].field(80, 59), 22, std::ldexp(1.0, -30));
    e.delta_tau_n_s = signmag(s[3].field(58, 54), 5, std::ldexp(1.0, -30));
    e.en_days = s[3].field(53, 49);
    e.p4 = s[3].field(34, 34) != 0;
    e.ft = s[3].field(33, 30);
    e.nt_days = s[3].field(26, 16);
    e.slot = s[3].field(15, 11);
    e.m_type = s[3].field(10, 9);
    return e;
}

void GlonassStringSync::push(double soft, SampleIndex sample, std::vector<GlonassString>& out) {
    sym_.push_back(soft);
    at_.push_back(sample);
    if (sym_.size() > 400) {
        sym_.erase(sym_.begin(), sym_.begin() + 100);
        at_.erase(at_.begin(), at_.begin() + 100);
    }
    if (sym_.size() < 200) {
        return;
    }
    static const auto tm = glonass_time_mark();
    const std::size_t end = sym_.size();
    double corr = 0.0;
    double mag = 0.0;
    for (std::size_t i = 0; i < 30; ++i) {
        const double v = sym_[end - 30 + i];
        corr += tm[i] != 0 ? -v : v;
        mag += std::abs(v);
    }
    if (mag <= 0.0 || std::abs(corr) < 0.8 * mag) {
        return;
    }
    const std::size_t first = end - 200;
    std::array<std::uint8_t, 85> rel{};
    for (std::size_t j = 0; j < 85; ++j) {
        const double d = sym_[first + 2 * j] - sym_[first + 2 * j + 1];
        rel[j] = d < 0.0 ? 1 : 0;
    }
    GlonassString s;
    s.channel = channel_;
    s.sample = at_[first];
    // Transmission order j = 0..84 is string bit 85 - j; bit 85 is the idle
    // zero that anchors the relative code.
    s.bits[85] = 0;
    for (std::size_t j = 1; j < 85; ++j) {
        s.bits[85 - j] = rel[j] ^ rel[j - 1];
    }
    s.check = glonass_check_string(s.bits);
    s.number = s.field(84, 81);
    out.push_back(s);
    sym_.clear();
    at_.clear();
}

// ---------------------------------------------------------------------------

Expected<GlonassL1Decoder> GlonassL1Decoder::create(GlonassL1Config config) {
    if (config.channels.empty()) {
        const double half = static_cast<double>(config.sample_rate) / 2.0;
        for (int k = -7; k <= 6; ++k) {
            const double f = config.k0_offset_hz + 562500.0 * k;
            if (std::abs(f) + 511000.0 <= half) {
                config.channels.push_back(k);
            }
        }
    }
    if (config.channels.empty()) {
        return fail("glonass l1: no channel fits inside the stream's bandwidth");
    }
    std::vector<SignalSpec> signals;
    const auto code = glonass_st_code();
    for (int k : config.channels) {
        if (k < -7 || k > 6) {
            return fail("glonass l1: frequency number " + std::to_string(k) + " is out of range");
        }
        SignalSpec s;
        s.id = static_cast<unsigned>(k + static_cast<int>(kIdBias));
        s.code = code;
        s.chip_rate_hz = 511000.0;
        s.carrier_hz = 1602.0e6 + 562500.0 * k;
        s.offset_hz = config.k0_offset_hz + 562500.0 * k;
        signals.push_back(std::move(s));
    }
    config.front_end.sample_rate = config.sample_rate;
    auto fe = FrontEnd::create(config.front_end, std::move(signals));
    if (!fe) {
        return std::unexpected(fe.error());
    }
    return GlonassL1Decoder(std::move(config), std::move(*fe));
}

void GlonassL1Decoder::feed(std::span<const dsp::Complex32> samples, GlonassL1Output& out) {
    scratch_.prompts.clear();
    scratch_.acquisitions.clear();
    scratch_.losses.clear();
    fe_.feed(samples, scratch_);
    out.acquisitions.insert(out.acquisitions.end(), scratch_.acquisitions.begin(),
                            scratch_.acquisitions.end());
    for (const auto& loss : scratch_.losses) {
        chans_.erase(loss.id);
    }
    std::vector<std::pair<double, SampleIndex>> syms;
    std::vector<GlonassString> strings;
    for (const auto& p : scratch_.prompts) {
        auto it = chans_.find(p.id);
        if (it == chans_.end()) {
            const int k = static_cast<int>(p.id) - static_cast<int>(kIdBias);
            it = chans_.emplace(p.id, Chan{LnavBitSync{10}, GlonassStringSync{k}, {}}).first;
        }
        Chan& ch = it->second;
        syms.clear();
        ch.sync.push_soft(p, syms);
        if (config_.status_periods != 0 && p.period % config_.status_periods == 0) {
            out.status.push_back(
                {p.id, p.cn0_dbhz, p.doppler_hz, p.phase_locked, ch.sync.synced(), p.sample});
        }
        for (const auto& [v, s] : syms) {
            strings.clear();
            ch.strings.push(v, s, strings);
            for (const auto& str : strings) {
                out.strings.push_back(str);
                if (str.check == GlonassCheck::Erased) {
                    ch.recent.clear();
                    continue;
                }
                if (str.number == 1) {
                    ch.recent.clear();
                }
                if (!ch.recent.empty() && str.number != ch.recent.back().number + 1) {
                    ch.recent.clear();
                }
                if (ch.recent.empty() && str.number != 1) {
                    continue;
                }
                ch.recent.push_back(str);
                if (ch.recent.size() == 4) {
                    if (auto e = glonass_ephemeris(std::span<const GlonassString, 4>(ch.recent.data(), 4))) {
                        out.ephemerides.push_back(*e);
                    }
                    ch.recent.clear();
                }
            }
        }
    }
}

}  // namespace revenant::decode::gnss
