#include "core/decode/jt65.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <random>
#include <string>

namespace revenant::decode {

namespace {

// Figure 3, the pseudo-random sync vector as printed.
constexpr std::array<std::uint8_t, 126> kSync = {
    1, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0,
    0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, 0, 1, 1,
    0, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1,
    0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1,
    1, 1, 1, 1, 1, 1,
};

// Section 6 as Figure 2 resolves it: channel position 9c + r carries code
// word symbol 7r + c.
int word_index(int channel) { return 7 * (channel % 9) + channel / 9; }

int gray(int v) { return v ^ (v >> 1); }

SlowProtocol make_protocol(int submode, bool ooo) {
    SlowProtocol p;
    p.name = "JT65";
    p.symbols = 126;
    p.tones = 66;
    // Section 8: 4096 samples at 11025 Hz; tones 2.6917 m Hz apart, which is
    // 11025 / 4096 to the precision printed.
    p.symbol_seconds = 4096.0 / 11025.0;
    p.tone_spacing_hz = 11025.0 / 4096.0 * static_cast<double>(1 << submode);
    p.slot_seconds = 60.0;
    p.nominal_start_seconds = 1.0;
    // NOT SPECIFIED: the window searched around the nominal start.
    p.start_min_seconds = 0.0;
    p.start_max_seconds = 3.5;
    for (const auto s0 : kSync) {
        const bool sync = (s0 != 0) != ooo;
        p.sync_on.push_back(sync ? std::vector<int>{0} : std::vector<int>{});
        p.sync_off.push_back(sync ? std::vector<int>{} : std::vector<int>{0});
    }
    return p;
}

std::vector<int> data_positions(bool ooo) {
    std::vector<int> out;
    for (int i = 0; i < 126; ++i) {
        if ((kSync[static_cast<std::size_t>(i)] != 0) == ooo) out.push_back(i);
    }
    return out;
}

}  // namespace

std::span<const std::uint8_t> jt65_sync_vector() { return kSync; }

const SlowProtocol& jt65_protocol(int submode, bool ooo) {
    static const std::array<SlowProtocol, 6> all = [] {
        std::array<SlowProtocol, 6> a;
        for (int i = 0; i < 3; ++i) {
            a[static_cast<std::size_t>(2 * i)] = make_protocol(i, false);
            a[static_cast<std::size_t>(2 * i + 1)] = make_protocol(i, true);
        }
        return a;
    }();
    return all[static_cast<std::size_t>(2 * std::clamp(submode, 0, 2) + (ooo ? 1 : 0))];
}

Rs64Message jt65_symbols(const Wsjt72& payload) {
    Rs64Message m{};
    for (int i = 0; i < 12; ++i) {
        int v = 0;
        for (int j = 0; j < 6; ++j) v = (v << 1) | payload[static_cast<std::size_t>(6 * i + j)];
        m[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v);
    }
    return m;
}

Wsjt72 jt65_payload(const Rs64Message& m) {
    Wsjt72 b{};
    for (int i = 0; i < 12; ++i) {
        for (int j = 0; j < 6; ++j) b[static_cast<std::size_t>(6 * i + j)] = static_cast<std::uint8_t>((m[static_cast<std::size_t>(i)] >> (5 - j)) & 1U);
    }
    return b;
}

std::vector<int> jt65_channel_symbols(const Wsjt72& payload) {
    const Rs64Word w = rs64_encode(jt65_symbols(payload));
    std::vector<int> out(63);
    for (int k = 0; k < 63; ++k) out[static_cast<std::size_t>(k)] = gray(w[static_cast<std::size_t>(word_index(k))]);
    return out;
}

std::vector<int> jt65_channel_tones(const Wsjt72& payload, bool ooo) {
    const auto sym = jt65_channel_symbols(payload);
    std::vector<int> out(126, 0);
    std::size_t k = 0;
    for (const int i : data_positions(ooo)) out[static_cast<std::size_t>(i)] = sym[k++] + 2;
    return out;
}

Jt65Decoder::Jt65Decoder(Jt65Config config, SlowStream stream) : config_(std::move(config)), stream_(std::move(stream)) {}

Expected<Jt65Decoder> Jt65Decoder::create(Jt65Config config) {
    if (config.submode < 0 || config.submode > 2) return fail("JT65 submode must be 0 to 2 (A to C)");
    if (!(config.low_hz >= 0.0 && config.high_hz > config.low_hz && config.high_hz < 5000.0)) {
        return fail("JT65 search range must lie within 0 to 5000 Hz");
    }
    if (config.trials < 0) return fail("JT65 trial count must not be negative");
    auto stream = SlowStream::create(60.0, config.rate, config.utc_of_sample);
    if (!stream) return std::unexpected(stream.error());
    return Jt65Decoder(std::move(config), std::move(*stream));
}

Status Jt65Decoder::process(ConstRealSpan audio, std::vector<Jt65Decode>& out) {
    stream_.process(audio, [&](std::span<const float> slot, double utc, SampleIndex start) {
        decode_slot(slot, utc, start, out);
    });
    return {};
}

namespace {

struct FtResult {
    Rs64Word word{};
    int trials = 0;
    int hard_errors = 0;
    bool ok = false;
};

// Algorithm 1 over power[i][v]: code word symbol i holding value v.
FtResult franke_taylor(const std::vector<std::array<float, 64>>& power, double amplitude, int max_trials,
                       std::uint32_t seed) {
    Rs64Word hard{};
    std::array<double, 63> erase_p{};
    for (int i = 0; i < 63; ++i) {
        const auto& p = power[static_cast<std::size_t>(i)];
        int best = 0;
        for (int v = 1; v < 64; ++v) {
            if (p[static_cast<std::size_t>(v)] > p[static_cast<std::size_t>(best)]) best = v;
        }
        hard[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(best);
        // Posterior of the strongest value under the non-coherent model.
        std::array<double, 64> m{};
        double top = -1e300;
        for (int v = 0; v < 64; ++v) {
            m[static_cast<std::size_t>(v)] = slow_log_i0(2.0 * amplitude * std::sqrt(static_cast<double>(p[static_cast<std::size_t>(v)])));
            top = std::max(top, m[static_cast<std::size_t>(v)]);
        }
        double z = 0.0;
        for (const double x : m) z += std::exp(x - top);
        const double perr = 1.0 - std::exp(m[static_cast<std::size_t>(best)] - top) / z;
        erase_p[static_cast<std::size_t>(i)] = perr;
    }
    // Algorithm 1, step 1: 1.3 times the symbol error probability. NOT
    // SPECIFIED, and measured here: when that would erase more than 38
    // symbols on average, the factor is lowered until it does not. Without
    // that the 50% point in AWGN sits about a decibel higher, because these
    // error estimates separate right from wrong symbols less sharply than
    // the paper's empirical table and too many correct symbols get erased.
    {
        constexpr double kFactor = 1.3;
        constexpr double kMeanErasures = 38.0;
        const auto total = [&](double k) {
            double s = 0.0;
            for (const double p : erase_p) s += std::min(1.0, k * p);
            return s;
        };
        double c = kFactor;
        if (total(c) > kMeanErasures) {
            double lo = 0.0;
            double hi = c;
            for (int it = 0; it < 40; ++it) {
                const double mid = 0.5 * (lo + hi);
                (total(mid) > kMeanErasures ? hi : lo) = mid;
            }
            c = lo;
        }
        for (auto& p : erase_p) p = std::min(1.0, c * p);
    }
    const auto syn = rs64_syndromes(hard);
    const auto metric = [&](const Rs64Word& w) {
        double u = 0.0;
        for (int i = 0; i < 63; ++i) u += power[static_cast<std::size_t>(i)][w[static_cast<std::size_t>(i)]];
        return u / 63.0;
    };
    const auto distance = [&](const Rs64Word& w) {
        int x = 0;
        for (int i = 0; i < 63; ++i) x += w[static_cast<std::size_t>(i)] != hard[static_cast<std::size_t>(i)] ? 1 : 0;
        return x;
    };

    FtResult r;
    // Errors only first: within 25 errors the code word is certain.
    if (auto w = rs64_decode(hard, syn, {})) {
        r.word = *w;
        r.hard_errors = distance(*w);
        r.ok = true;
        return r;
    }
    // NOT SPECIFIED, standing in for the paper's unprinted X0, D0, D1, R1.
    // u is in units of the noise per bin, so a code word unrelated to the
    // signal scores about 1 and one sharing k of 63 symbols with it about
    // 1 + k y / 63. The test is on the excess over 1, (u2 - 1) / (u1 - 1),
    // rather than the paper's u2 / u1, so that it means the same at any SNR:
    // stop early at u1 >= 2.5 with a ratio of 0.35 or less; after the
    // timeout accept u1 >= 1.8 with 0.5 or less. Either way the code word
    // must lie within 43 symbols of the hard decisions, the largest distance
    // the 2016 paper's Figure 3 shows a correct decode at. u2 starts at 1.
    constexpr double kEarlyU = 2.5;
    constexpr double kEarlyR = 0.35;
    constexpr double kFinalU = 1.8;
    constexpr double kFinalR = 0.5;
    constexpr int kFinalX = 43;
    double u1 = 0.0;
    double u2 = 1.0;
    Rs64Word best{};
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::vector<int> erased;
    std::vector<std::pair<double, int>> drawn;
    int t = 0;
    for (; t < max_trials; ++t) {
        drawn.clear();
        for (int i = 0; i < 63; ++i) {
            const double x = uni(rng);
            if (x < erase_p[static_cast<std::size_t>(i)]) drawn.emplace_back(erase_p[static_cast<std::size_t>(i)], i);
        }
        // Step 2's cap of 51 erasures. NOT SPECIFIED how: a random 51 of
        // those drawn, since keeping the least reliable would make every
        // trial at low SNR erase the same set.
        if (drawn.size() > static_cast<std::size_t>(kRs64Roots)) {
            std::shuffle(drawn.begin(), drawn.end(), rng);
            drawn.resize(kRs64Roots);
        }
        erased.clear();
        for (const auto& [p, i] : drawn) erased.push_back(i);
        auto w = rs64_decode(hard, syn, erased);
        if (!w) continue;
        const double u = metric(*w);
        if (u > u1) {
            if (*w != best) u2 = std::max(u2, u1);
            u1 = u;
            best = *w;
        } else if (u > u2 && *w != best) {
            u2 = u;
        }
        if (u1 >= kEarlyU && (u2 - 1.0) / (u1 - 1.0) <= kEarlyR && distance(best) <= kFinalX) {
            ++t;
            break;
        }
    }
    r.trials = t;
    if (u1 >= kFinalU && (u2 - 1.0) / (u1 - 1.0) <= kFinalR && distance(best) <= kFinalX) {
        r.word = best;
        r.hard_errors = distance(best);
        r.ok = true;
    }
    return r;
}

}  // namespace

void Jt65Decoder::decode_slot(std::span<const float> slot_12k, double slot_utc, SampleIndex slot_start,
                              std::vector<Jt65Decode>& out) const {
    std::map<std::string, Jt65Decode> found;
    std::vector<double> taken;  // frequencies of decodes
    for (const bool ooo : {false, true}) {
        if (ooo && !config_.ooo) break;
        const SlowProtocol& p = jt65_protocol(config_.submode, ooo);
        const auto positions = data_positions(ooo);
        SlowSlot slot(p, slot_12k);
        SlowSearch search;
        search.low_hz = config_.low_hz;
        search.high_hz = config_.high_hz;
        search.max_candidates = config_.max_candidates;
        search.sync_threshold = config_.sync_threshold;
        search.max_drift_hz = config_.max_drift_hz;
        std::uint32_t seed = 1;
        for (const auto& c : slot.candidates(search)) {
            ++seed;
            // A candidate within half a tone of a decode is that signal again,
            // whatever its start time or sync form.
            const bool seen = std::any_of(taken.begin(), taken.end(), [&](double f) {
                return std::abs(f - c.frequency_hz) < 0.5 * p.tone_spacing_hz;
            });
            if (seen) continue;
            const SlowSpectra s = slot.demodulate(c, search);
            // NOT SPECIFIED: the refined sync, the sync tone's mean power above
            // the data intervals' in units of the noise per bin, must reach 1
            // before erasure trials are spent on it.
            if (s.where.sync < 1.0) continue;
            std::vector<std::array<float, 64>> power(63);
            for (int k = 0; k < 63; ++k) {
                const int sym = positions[static_cast<std::size_t>(k)];
                auto& row = power[static_cast<std::size_t>(word_index(k))];
                for (int v = 0; v < 64; ++v) row[static_cast<std::size_t>(v)] = s.at(sym, gray(v) + 2);
            }
            // The sync tone measures the signal better than the strongest of 64
            // data tones does: its on-off difference is the power SNR per bin.
            const double a = std::sqrt(std::max(s.where.sync, 0.25));
            const FtResult r = franke_taylor(power, a, config_.trials, seed);
            if (!r.ok) continue;
            Rs64Message m{};
            std::copy_n(r.word.begin() + kRs64Roots, kRs64K, m.begin());
            const Wsjt72 bits = jt65_payload(m);
            auto msg = wsjt72_unpack(bits);
            const auto tones = jt65_channel_tones(bits, ooo);
            Jt65Decode d;
            d.slot_utc = slot_utc;
            d.slot_start_sample = slot_start;
            d.frequency_hz = s.where.frequency_hz;
            d.dt_seconds = s.where.start_seconds - p.nominal_start_seconds;
            d.drift_hz = s.where.drift_hz;
            d.snr_db = slow_snr_2500(p, s, tones);
            d.payload = bits;
            d.message = std::move(msg);
            if (ooo) d.message.text += " OOO";
            d.ooo = ooo;
            d.trials = r.trials;
            d.hard_errors = r.hard_errors;
            taken.push_back(c.frequency_hz);
            auto it = found.find(d.message.text);
            if (it == found.end() || it->second.snr_db < d.snr_db) found[d.message.text] = d;
        }
    }
    std::vector<Jt65Decode> got;
    for (auto& [k, v] : found) got.push_back(std::move(v));
    std::sort(got.begin(), got.end(), [](const Jt65Decode& a, const Jt65Decode& b) { return a.frequency_hz < b.frequency_hz; });
    out.insert(out.end(), got.begin(), got.end());
}

}  // namespace revenant::decode
