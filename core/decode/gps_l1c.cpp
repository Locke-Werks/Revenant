// GPS L1C CNAV-2. See gps_l1c.h for the document.

#include "core/decode/gps_l1c.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

#include "core/decode/gps_l1c_tables.h"

namespace revenant::decode::gnss {

namespace {

constexpr std::size_t kWeilLen = 10223;
constexpr std::size_t kCodeLen = 10230;
constexpr std::size_t kFrame = 1800;
constexpr std::size_t kToiSymbols = 52;
constexpr double kChipRate = 1.023e6;
constexpr double kCarrier = 1575.42e6;
constexpr double kTwoPi = 2.0 * std::numbers::pi;

// Rows of each subframe's matrices: H is m x n with g = 1 (B is a column,
// D is 1 x 1), IS-GPS-800J 3.2.3.4.
struct LdpcShape {
    std::size_t m, n;
    std::span<const l1c_tables::Coord> a, b, t, c, d, e;
};

LdpcShape shape(unsigned subframe) {
    using namespace l1c_tables;
    if (subframe == 2) {
        return {600, 1200, kSf2A, kSf2B, kSf2T, kSf2C, kSf2D, kSf2E};
    }
    return {274, 548, kSf3A, kSf3B, kSf3T, kSf3C, kSf3D, kSf3E};
}

unsigned bits_at(std::span<const std::uint8_t> b, unsigned first, unsigned count) {
    unsigned v = 0;
    for (unsigned i = 0; i < count; ++i) {
        v = (v << 1U) | b[first - 1 + i];
    }
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------

const std::vector<std::uint8_t>& l1c_legendre() {
    // 3.2.2.1.1: L(t) = 1 when t is a nonzero quadratic residue mod 10223.
    static const std::vector<std::uint8_t> seq = [] {
        std::vector<std::uint8_t> l(kWeilLen, 0);
        for (std::size_t x = 1; x < kWeilLen; ++x) {
            l[(x * x) % kWeilLen] = 1;
        }
        l[0] = 0;
        return l;
    }();
    return seq;
}

std::vector<std::int8_t> l1c_code(unsigned prn, bool pilot) {
    if (prn < 1 || prn > 63) {
        return {};
    }
    const auto& row = l1c_tables::kRanging[prn - 1];
    const std::size_t w = pilot ? row.pilot_weil : row.data_weil;
    const std::size_t p = pilot ? row.pilot_insert : row.data_insert;
    const auto& l = l1c_legendre();
    std::vector<std::uint8_t> weil(kWeilLen);
    for (std::size_t t = 0; t < kWeilLen; ++t) {
        weil[t] = l[t] ^ l[(t + w) % kWeilLen];
    }
    static constexpr std::array<std::uint8_t, 7> kExpansion = {0, 1, 1, 0, 1, 0, 0};
    std::vector<std::int8_t> out;
    out.reserve(kCodeLen);
    auto put = [&](std::uint8_t b) { out.push_back(b != 0 ? std::int8_t{-1} : std::int8_t{1}); };
    for (std::size_t t = 0; t + 1 < p; ++t) {
        put(weil[t]);
    }
    for (auto b : kExpansion) {
        put(b);
    }
    for (std::size_t t = p - 1; t < kWeilLen; ++t) {
        put(weil[t]);
    }
    return out;
}

std::vector<std::int8_t> l1c_overlay(unsigned prn) {
    if (prn < 1 || prn > 63) {
        return {};
    }
    // 3.2.2.1.2 and Table 3.2-3: the printed initial bits are the first 11
    // of the sequence, n11 first, and the recurrence is the polynomial's,
    // s[n + 11] = sum over j = 1..11 of m_j s[n + 11 - j]. Checked against
    // the table's final 11 bits for every PRN by test.
    const auto& row = l1c_tables::kOverlay[prn - 1];
    std::vector<std::uint8_t> s;
    s.reserve(kFrame);
    for (unsigned i = 0; i < 11; ++i) {
        s.push_back(static_cast<std::uint8_t>((row.initial >> (10 - i)) & 1U));
    }
    while (s.size() < kFrame) {
        const std::size_t n = s.size() - 11;
        std::uint8_t b = 0;
        for (unsigned j = 1; j <= 11; ++j) {
            if (((row.polynomial >> j) & 1U) != 0) {
                b ^= s[n + 11 - j];
            }
        }
        s.push_back(b);
    }
    std::vector<std::int8_t> out(kFrame);
    for (std::size_t i = 0; i < kFrame; ++i) {
        out[i] = s[i] != 0 ? std::int8_t{-1} : std::int8_t{1};
    }
    return out;
}

std::vector<std::int8_t> boc11(std::span<const std::int8_t> code) {
    // Figure 3.3-2a: one square-wave cycle per chip, sine phased, so the
    // first half carries the chip and the second its inverse.
    std::vector<std::int8_t> out;
    out.reserve(code.size() * 2);
    for (auto c : code) {
        out.push_back(c);
        out.push_back(static_cast<std::int8_t>(-c));
    }
    return out;
}

// ---------------------------------------------------------------------------

std::array<std::uint8_t, 52> l1c_toi_encode(unsigned toi) {
    // Figure 3.2-4: stages 1 to 8 shifting towards 8, output from stage 8,
    // feedback into stage 1 from taps 1, 4, 5, 6, 7 and 8 (1 + X + X^4 +
    // X^5 + X^6 + X^7 + X^8, 763 octal). The 8 LSBs are shifted in MSB
    // first, so the MSB of the eight sits in stage 8.
    std::array<std::uint8_t, 9> s{};  // s[1]..s[8]
    for (unsigned k = 1; k <= 8; ++k) {
        s[k] = static_cast<std::uint8_t>((toi >> (k - 1)) & 1U);
    }
    const auto msb = static_cast<std::uint8_t>((toi >> 8) & 1U);
    std::array<std::uint8_t, 52> out{};
    out[0] = msb;
    for (unsigned i = 0; i < 51; ++i) {
        out[1 + i] = static_cast<std::uint8_t>(s[8] ^ msb);
        const auto fb = static_cast<std::uint8_t>(s[1] ^ s[4] ^ s[5] ^ s[6] ^ s[7] ^ s[8]);
        for (unsigned k = 8; k > 1; --k) {
            s[k] = s[k - 1];
        }
        s[1] = fb;
    }
    return out;
}

unsigned l1c_toi_decode(std::span<const float> soft) {
    // 3.2.3.2's own example technique.
    static const auto words = [] {
        std::array<std::array<std::uint8_t, 52>, 256> w{};
        for (unsigned v = 0; v < 256; ++v) {
            w[v] = l1c_toi_encode(v);
        }
        return w;
    }();
    double best = -1.0;
    unsigned best_v = 0;
    bool best_neg = false;
    for (unsigned v = 0; v < 256; ++v) {
        double c = 0.0;
        for (std::size_t i = 0; i < kToiSymbols && i < soft.size(); ++i) {
            c += words[v][i] != 0 ? -soft[i] : soft[i];
        }
        if (std::abs(c) > best) {
            best = std::abs(c);
            best_v = v;
            best_neg = c < 0.0;
        }
    }
    return best_v | (best_neg ? 256U : 0U);
}

std::uint32_t crc24q(std::span<const std::uint8_t> bits) {
    // 3.2.3.3: the remainder of m(X) X^24 by g(X), seed 0.
    std::uint32_t r = 0;
    for (auto b : bits) {
        const std::uint32_t fb = ((r >> 23) & 1U) ^ (b & 1U);
        r = (r << 1) & 0xFFFFFFU;
        if (fb != 0) {
            r ^= 0x864CFBU;
        }
    }
    return r;
}

std::vector<std::vector<std::uint32_t>> l1c_ldpc_checks(unsigned subframe) {
    const auto s = shape(subframe);
    const std::size_t k = s.n - s.m;
    const std::size_t top = s.m - 1;
    std::vector<std::vector<std::uint32_t>> rows(s.m);
    auto add = [&](std::span<const l1c_tables::Coord> mat, std::size_t row0, std::size_t col0) {
        for (const auto& c : mat) {
            rows[row0 + c.row - 1].push_back(static_cast<std::uint32_t>(col0 + c.col - 1));
        }
    };
    add(s.a, 0, 0);
    add(s.b, 0, k);
    add(s.t, 0, k + 1);
    add(s.c, top, 0);
    add(s.d, top, k);
    add(s.e, top, k + 1);
    for (auto& r : rows) {
        std::sort(r.begin(), r.end());
    }
    return rows;
}

std::vector<std::uint8_t> l1c_ldpc_encode(unsigned subframe, std::span<const std::uint8_t> info) {
    const auto s = shape(subframe);
    const std::size_t k = s.n - s.m;
    const std::size_t top = s.m - 1;
    // T is lower triangular with a unit diagonal (checked by test), so
    // T^-1 v is forward substitution over its strictly lower part.
    std::vector<std::vector<std::uint32_t>> t_lower(top);
    for (const auto& c : s.t) {
        if (c.col < c.row) {
            t_lower[c.row - 1].push_back(c.col - 1U);
        }
    }
    auto solve_t = [&](std::vector<std::uint8_t> v) {
        for (std::size_t i = 0; i < top; ++i) {
            for (auto j : t_lower[i]) {
                v[i] ^= v[j];
            }
        }
        return v;
    };
    std::vector<std::uint8_t> as(top, 0), bcol(top, 0);
    for (const auto& c : s.a) {
        as[c.row - 1] ^= info[c.col - 1];
    }
    for (const auto& c : s.b) {
        bcol[c.row - 1] ^= 1;
    }
    const auto x = solve_t(as);       // T^-1 A s
    const auto tb = solve_t(bcol);    // T^-1 B
    std::uint8_t phi = static_cast<std::uint8_t>(s.d.empty() ? 0 : 1);  // D, 1 x 1
    std::uint8_t rhs = 0;
    for (const auto& c : s.e) {
        phi ^= tb[c.col - 1];
        rhs ^= x[c.col - 1];
    }
    for (const auto& c : s.c) {
        rhs ^= info[c.col - 1];
    }
    // phi = E T^-1 B + D is 1 for both subframes (checked by test), so
    // p1 = rhs.
    const std::uint8_t p1 = phi != 0 ? rhs : 0;
    std::vector<std::uint8_t> out(info.begin(), info.end());
    out.push_back(p1);
    for (std::size_t i = 0; i < top; ++i) {
        out.push_back(static_cast<std::uint8_t>(x[i] ^ (p1 & tb[i])));
    }
    (void)k;
    return out;
}

std::vector<std::uint8_t> l1c_interleave(std::span<const std::uint8_t> in) {
    std::vector<std::uint8_t> out(in.size());
    for (std::size_t r = 0; r < 38; ++r) {
        for (std::size_t c = 0; c < 46; ++c) {
            out[c * 38 + r] = in[r * 46 + c];
        }
    }
    return out;
}

// ---------------------------------------------------------------------------

Expected<GpsL1cDecoder> GpsL1cDecoder::create(GpsL1cConfig config) {
    if (config.sample_rate <= 0) {
        return fail("gps l1c: sample rate must be positive");
    }
    if (config.prns.empty()) {
        for (unsigned p = 1; p <= 32; ++p) {
            config.prns.push_back(p);
        }
    }
    GpsL1cDecoder d;
    std::vector<SignalSpec> specs;
    for (auto prn : config.prns) {
        if (prn < 1 || prn > 63) {
            return fail("gps l1c: PRN out of 1 to 63");
        }
        SignalSpec s;
        s.id = prn;
        s.code = gps_ca_code(prn);
        s.offset_hz = config.offset_hz;
        specs.push_back(std::move(s));
        Sat sat;
        sat.prn = prn;
        sat.pilot = boc11(l1c_code(prn, true));
        sat.data = boc11(l1c_code(prn, false));
        sat.overlay = l1c_overlay(prn);
        d.sats_.emplace(prn, std::move(sat));
    }
    config.ca.sample_rate = config.sample_rate;
    auto fe = FrontEnd::create(config.ca, std::move(specs));
    if (!fe) {
        return std::unexpected(fe.error());
    }
    d.ca_.emplace(std::move(*fe));
    for (unsigned sf : {2U, 3U}) {
        LdpcConfig lc;
        lc.checks = l1c_ldpc_checks(sf);
        lc.variables = sf == 2 ? 1200 : 548;
        auto dec = LdpcDecoder::create(std::move(lc));
        if (!dec) {
            return std::unexpected(dec.error());
        }
        (sf == 2 ? d.ldpc2_ : d.ldpc3_).emplace(std::move(*dec));
    }
    d.config_ = std::move(config);
    return d;
}

double GpsL1cDecoder::correlate(const Sat& sat, SampleIndex start, double code_phase_chips,
                                double doppler_hz) const {
    const double fs = static_cast<double>(config_.sample_rate);
    const double step = 2.0 * kChipRate * (1.0 + doppler_hz / kCarrier) / fs;
    const double len = static_cast<double>(sat.pilot.size());
    const auto first = static_cast<std::size_t>(start - ring_start_);
    std::complex<double> lo(1.0, 0.0);
    const std::complex<double> lo_step =
        std::polar(1.0, -kTwoPi * (config_.offset_hz + doppler_hz) / fs);
    double cp = code_phase_chips;
    double power = 0.0;
    std::complex<double> acc{};
    // Two pilot periods, summed non-coherently because the overlay bit may
    // change between them.
    for (std::size_t i = first; i < ring_.size(); ++i) {
        const std::complex<double> x(ring_[i].real(), ring_[i].imag());
        auto idx = static_cast<std::size_t>(cp);
        acc += x * lo * static_cast<double>(sat.pilot[std::min(idx, sat.pilot.size() - 1)]);
        lo *= lo_step;
        cp += step;
        if (cp >= len) {
            cp -= len;
            power += std::norm(acc);
            acc = {};
            if (power > 0.0 && i > first + static_cast<std::size_t>(fs * 0.015)) {
                break;
            }
        }
    }
    return power;
}

void GpsL1cDecoder::try_handover(Sat& sat, GpsL1cOutput& out) {
    if (!sat.handover_pending || sat.epochs.size() < 10) {
        return;
    }
    const double fs = static_cast<double>(config_.sample_rate);
    // Two 10 ms periods past the last candidate, with a little to spare for
    // Doppler stretching the period.
    const SampleIndex need =
        sat.epochs[9].sample + static_cast<SampleIndex>(std::ceil(fs * 0.0205)) + 2;
    if (need > next_) {
        return;
    }
    sat.handover_pending = false;
    std::array<double, 10> power{};
    for (unsigned k = 0; k < 10; ++k) {
        const auto& e = sat.epochs[k];
        // The C/A period began code_phase_chips before its first sample; at
        // that sample the L1C half-chip phase is twice that.
        power[k] = correlate(sat, e.sample, 2.0 * e.code_phase_chips, e.doppler_hz);
    }
    const auto best = static_cast<unsigned>(std::max_element(power.begin(), power.end()) -
                                            power.begin());
    double rest = 0.0;
    for (unsigned k = 0; k < 10; ++k) {
        rest += k == best ? 0.0 : power[k];
    }
    rest /= 9.0;
    L1cHandover h;
    h.prn = sat.prn;
    h.sample = sat.epochs[best].sample;
    h.ca_epochs_after = best;
    h.ratio = rest > 0.0 ? power[best] / rest : 0.0;
    h.success = h.ratio >= config_.handover_threshold;
    out.handovers.push_back(h);
    if (!h.success) {
        sat.locked_periods = 0;
        sat.epochs.clear();
        return;
    }
    const auto& e = sat.epochs[best];
    SignalSpec spec;
    spec.id = sat.prn;
    spec.code = sat.pilot;
    spec.aux_code = sat.data;
    spec.chip_rate_hz = 2.0 * kChipRate;
    spec.carrier_hz = kCarrier;
    spec.offset_hz = config_.offset_hz;
    FrontEndConfig fc;
    fc.sample_rate = config_.sample_rate;
    fc.tracking = config_.l1c_tracking;
    sat.channel.emplace(spec, fc, e.sample, 2.0 * e.code_phase_chips, e.doppler_hz);
    sat.handed_over = true;
    sat.epochs.clear();
    // Catch the new channel up on what is buffered.
    std::vector<Prompt> prompts;
    const auto from = static_cast<std::size_t>(e.sample - ring_start_);
    sat.channel->process(std::span<const dsp::Complex32>(ring_).subspan(from), e.sample, prompts);
    for (const auto& p : prompts) {
        handle_l1c(sat, p, out);
    }
}

void GpsL1cDecoder::handle_l1c(Sat& sat, const Prompt& p, GpsL1cOutput& out) {
    if (p.period >= sat.last_status + config_.status_periods) {
        sat.last_status = p.period;
        out.status.push_back({sat.prn, p.cn0_dbhz, p.doppler_hz, p.phase_locked, sat.synced});
    }
    if (!sat.synced) {
        if (!p.phase_locked || (!sat.prompts.empty() && sat.prompts.back().period + 1 != p.period)) {
            sat.prompts.clear();
        }
        if (p.phase_locked) {
            sat.prompts.push_back(p);
        }
        if (sat.prompts.size() < 300) {
            return;
        }
        // Every rotation of the overlay against the pilot's in-phase signs.
        double best = 0.0;
        std::size_t shift = 0;
        for (std::size_t s = 0; s < kFrame; ++s) {
            double c = 0.0;
            for (std::size_t i = 0; i < sat.prompts.size(); ++i) {
                c += sat.prompts[i].prompt.real() * sat.overlay[(s + i) % kFrame];
            }
            if (std::abs(c) > std::abs(best)) {
                best = c;
                shift = s;
            }
        }
        sat.synced = true;
        sat.polarity = best < 0.0 ? -1.0 : 1.0;
        // prompts[0] is overlay bit `shift`; the next frame starts where the
        // overlay wraps.
        sat.frame_period = sat.prompts.front().period + (kFrame - shift) % kFrame;
        sat.prompts.clear();
        return;
    }
    if (p.period < sat.frame_period) {
        return;
    }
    const std::size_t pos = (p.period - sat.frame_period) % kFrame;
    if (pos == 0) {
        sat.symbols.clear();
        sat.frame_sample = p.sample;
    }
    if (sat.symbols.size() != pos) {
        sat.symbols.clear();
        return;
    }
    sat.symbols.push_back(static_cast<float>(p.aux.real() * sat.polarity));
    if (sat.symbols.size() == kFrame) {
        decode_frame(sat, out);
        sat.symbols.clear();
    }
}

void GpsL1cDecoder::decode_frame(Sat& sat, GpsL1cOutput& out) {
    L1cFrame f;
    f.prn = sat.prn;
    f.sample = sat.frame_sample;
    const std::span<const float> sym(sat.symbols);
    f.toi = l1c_toi_decode(sym.first(kToiSymbols));
    const auto coded = l1c_deinterleave(sym.subspan(kToiSymbols));
    // Normalised min-sum does not care about the scale of the LLRs, only
    // their signs and ratios, so the soft symbols go in as they are.
    auto run = [&](LdpcDecoder& dec, std::span<const float> llr, std::size_t info,
                   std::vector<std::uint8_t>& data, bool& ok, std::size_t& iters) {
        std::vector<std::uint8_t> bits(llr.size());
        const auto r = dec.decode(llr, bits);
        iters = r.iterations;
        const std::span<const std::uint8_t> word(bits.data(), info);
        ok = r.converged && crc24q(word) == 0;
        data.assign(bits.begin(), bits.begin() + static_cast<std::ptrdiff_t>(info - 24));
    };
    run(*ldpc2_, std::span<const float>(coded).first(1200), 600, f.sf2, f.sf2_ok,
        f.sf2_iterations);
    run(*ldpc3_, std::span<const float>(coded).subspan(1200), 274, f.sf3, f.sf3_ok,
        f.sf3_iterations);
    if (f.sf2_ok) {
        // Figure 3.5-1.
        f.clock.wn = bits_at(f.sf2, 1, 13);
        f.clock.itow = bits_at(f.sf2, 14, 8);
        f.clock.top = bits_at(f.sf2, 22, 11);
        f.clock.l1c_health = bits_at(f.sf2, 33, 1) != 0;
        const unsigned u = bits_at(f.sf2, 34, 5);
        f.clock.ura_ed = u >= 16 ? static_cast<int>(u) - 32 : static_cast<int>(u);
        f.clock.toe = bits_at(f.sf2, 39, 11);
    }
    if (f.sf3_ok) {
        // 3.5.4.
        f.sf3_prn = bits_at(f.sf3, 1, 8);
        f.sf3_page = bits_at(f.sf3, 9, 6);
    }
    out.frames.push_back(std::move(f));
}

void GpsL1cDecoder::feed(std::span<const dsp::Complex32> samples, GpsL1cOutput& out) {
    if (samples.empty()) {
        return;
    }
    const SampleIndex first = next_;
    ring_.insert(ring_.end(), samples.begin(), samples.end());
    next_ += samples.size();

    FrontEndOutput fo;
    ca_->feed(samples, fo);
    out.acquisitions.insert(out.acquisitions.end(), fo.acquisitions.begin(),
                            fo.acquisitions.end());

    // Channels already running take the new samples first, so a channel
    // started below from the buffer does not see them twice.
    for (auto& [prn, sat] : sats_) {
        if (sat.channel && !sat.channel->lost()) {
            std::vector<Prompt> prompts;
            sat.channel->process(samples, first, prompts);
            for (const auto& p : prompts) {
                handle_l1c(sat, p, out);
            }
        } else if (sat.channel && sat.channel->lost()) {
            sat.channel.reset();
            sat.handed_over = false;
            sat.synced = false;
            sat.prompts.clear();
            sat.symbols.clear();
            sat.locked_periods = 0;
        }
    }
    for (const auto& p : fo.prompts) {
        auto it = sats_.find(p.id);
        if (it == sats_.end()) {
            continue;
        }
        Sat& sat = it->second;
        if (sat.handed_over) {
            continue;
        }
        if (sat.handover_pending) {
            sat.epochs.push_back({p.sample, p.code_phase_chips, p.doppler_hz});
            continue;
        }
        sat.locked_periods = p.phase_locked ? sat.locked_periods + 1 : 0;
        if (sat.locked_periods >= config_.handover_after_periods) {
            sat.handover_pending = true;
            sat.epochs.clear();
            sat.epochs.push_back({p.sample, p.code_phase_chips, p.doppler_hz});
        }
    }
    for (auto& [prn, sat] : sats_) {
        try_handover(sat, out);
    }

    // Keep 100 ms, more while a handover still needs its first epoch.
    const double fs = static_cast<double>(config_.sample_rate);
    SampleIndex keep_from = next_ - std::min<SampleIndex>(next_, static_cast<SampleIndex>(fs * 0.1));
    for (const auto& [prn, sat] : sats_) {
        if (sat.handover_pending && !sat.epochs.empty()) {
            keep_from = std::min(keep_from, sat.epochs.front().sample);
        }
    }
    if (keep_from > ring_start_) {
        ring_.erase(ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(keep_from - ring_start_));
        ring_start_ = keep_from;
    }
}

}  // namespace revenant::decode::gnss
