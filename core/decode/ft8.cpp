#include "core/decode/ft8.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <set>
#include <string>

#include "core/decode/ft_ldpc.h"

namespace revenant::decode {

namespace {

using C = std::complex<double>;

FtProtocol make_ft8() {
    FtProtocol p;
    p.name = "FT8";
    p.tones = 8;
    p.bits_per_symbol = 3;
    // Table 3, columns 1 and 2: tone k carries these bits.
    p.gray = {0b000, 0b001, 0b011, 0b010, 0b110, 0b100, 0b101, 0b111};
    // Section 4: S, M_A (29), S, M_B (29), S, with S = 3 1 4 0 6 5 2.
    static constexpr int kCostas[7] = {3, 1, 4, 0, 6, 5, 2};
    for (int block = 0; block < 3; ++block) {
        for (const int c : kCostas) p.layout.push_back(c);
        if (block < 2) p.layout.insert(p.layout.end(), 29, kFtData);
    }
    // Table 4: T = 0.160 s, 1920 samples at 12 kHz. Section 5: BT = 2 and a
    // ramp of T/8.
    p.samples_per_symbol = 1920;
    p.gaussian_bt = 2.0;
    p.ramp_samples = 240;
    p.slot_seconds = 15.0;
    p.nominal_start_seconds = 0.5;
    // NOT SPECIFIED: 200 Hz baseband, 32 samples per symbol.
    p.decimation = 60;
    return p;
}

// Equation 3 times T: the Gaussian-smoothed pulse, area 1 in units of T.
double pulse(double t_over_t, double bt) {
    const double k = std::numbers::pi * std::sqrt(2.0 / std::log(2.0));
    return 0.5 * (std::erf(k * bt * (t_over_t + 0.5)) - std::erf(k * bt * (t_over_t - 0.5)));
}

}  // namespace

const FtProtocol& ft8_protocol() {
    static const FtProtocol p = make_ft8();
    return p;
}

std::vector<int> ft_channel_tones(const FtProtocol& p, const Wsjt77& payload) {
    std::array<std::uint8_t, kFtK> msg{};
    for (std::size_t i = 0; i < 77; ++i) {
        msg[i] = static_cast<std::uint8_t>(payload[i] ^ (p.scramble ? p.scrambler[i] : 0U));
    }
    const auto crc = wsjt_crc14(std::span<const std::uint8_t>(msg.data(), 77));
    std::copy(crc.begin(), crc.end(), msg.begin() + 77);
    std::array<std::uint8_t, kFtN> cw{};
    ft_ldpc_encode(msg, cw);
    // Inverse of the Gray table.
    std::vector<int> tone_of(static_cast<std::size_t>(p.tones));
    for (int k = 0; k < p.tones; ++k) tone_of[static_cast<std::size_t>(p.gray[static_cast<std::size_t>(k)])] = k;
    std::vector<int> out;
    std::size_t bit = 0;
    for (const int l : p.layout) {
        if (l >= 0) {
            out.push_back(l);
        } else if (l == kFtRamp) {
            out.push_back(0);
        } else {
            int v = 0;
            for (int b = 0; b < p.bits_per_symbol; ++b) v = (v << 1) | cw[bit++];
            out.push_back(tone_of[static_cast<std::size_t>(v)]);
        }
    }
    return out;
}

std::vector<Complex32> ft_reference(const FtProtocol& p, std::span<const int> tones,
                                    SampleRate rate) {
    const int factor = static_cast<int>(rate / kWsjtRate);
    const int sps = p.samples_per_symbol * factor;
    const int ramp = p.ramp_samples * factor;
    const auto nsym = static_cast<int>(tones.size());
    const std::size_t total = static_cast<std::size_t>(nsym) * static_cast<std::size_t>(sps);
    std::vector<Complex32> out(total);
    const double spacing = ft_tone_spacing(p);
    // Equation 1 with h = 1: deviation in tone spacings. Section 5 says only
    // the neighbouring pulses overlap, so three are summed. Outside the
    // transmission the first and last symbols are taken to continue: NOT
    // SPECIFIED, it keeps the deviation flat into the ramps.
    const auto b = [&](int n) {
        n = std::clamp(n, 0, nsym - 1);
        return static_cast<double>(tones[static_cast<std::size_t>(n)]);
    };
    double phase = 0.0;
    for (std::size_t i = 0; i < total; ++i) {
        const double t = (static_cast<double>(i) + 0.5) / sps;  // in symbols
        const int n0 = static_cast<int>(std::floor(t));
        double d = 0.0;
        for (int n = n0 - 1; n <= n0 + 1; ++n) {
            d += b(n) * pulse(t - (n + 0.5), p.gaussian_bt);
        }
        double a = 1.0;
        const auto ii = static_cast<int>(i);
        if (ii < ramp) {
            a = 0.5 * (1.0 - std::cos(std::numbers::pi * (ii + 0.5) / ramp));
        } else if (static_cast<int>(total) - ii <= ramp) {
            const int j = static_cast<int>(total) - ii - 1;
            a = 0.5 * (1.0 - std::cos(std::numbers::pi * (j + 0.5) / ramp));
        }
        out[i] = Complex32(static_cast<float>(a * std::cos(phase)), static_cast<float>(a * std::sin(phase)));
        phase += 2.0 * std::numbers::pi * spacing * d / static_cast<double>(rate);
        if (phase > 2.0 * std::numbers::pi) phase -= 2.0 * std::numbers::pi;
    }
    return out;
}

FtDecoder::FtDecoder(FtConfig config, WsjtInputStage input, LdpcDecoder ldpc)
    : config_(std::move(config)),
      input_(std::move(input)),
      ldpc_(std::move(ldpc)),
      clock_(config_.protocol.slot_seconds, config_.utc_of_sample, config_.rate) {}

Expected<FtDecoder> FtDecoder::create(FtConfig config) {
    const auto& p = config.protocol;
    const auto slot = static_cast<std::int64_t>(std::llround(p.slot_seconds * kWsjtRate));
    if (p.tones < 2 || p.layout.empty() || p.decimation <= 0 ||
        p.samples_per_symbol % p.decimation != 0 || slot % p.decimation != 0 ||
        static_cast<int>(p.gray.size()) != p.tones) {
        return fail("inconsistent FT protocol description");
    }
    if (!(config.low_hz >= 0.0 && config.high_hz > config.low_hz && config.high_hz < 5500.0)) {
        return fail("FT search range must lie within 0 to 5500 Hz");
    }
    auto input = WsjtInputStage::create(config.rate);
    if (!input) return std::unexpected(input.error());
    auto ldpc = ft_ldpc_decoder(config.ldpc_iterations);
    if (!ldpc) return std::unexpected(ldpc.error());
    return FtDecoder(std::move(config), std::move(*input), std::move(*ldpc));
}

Status FtDecoder::process(ConstRealSpan audio, std::vector<FtDecode>& out) {
    if (!started_) {
        started_ = true;
        next_slot_ = clock_.first_boundary();
        buffer_start_ = 0;
    }
    scratch_.clear();
    input_.process(audio, scratch_);
    buffer_.insert(buffer_.end(), scratch_.begin(), scratch_.end());
    received_ += static_cast<std::int64_t>(scratch_.size());
    const std::int64_t len = clock_.slot_samples();
    while (received_ >= next_slot_ + len) {
        const std::int64_t off = next_slot_ - buffer_start_;
        std::vector<float> slot(static_cast<std::size_t>(len), 0.0F);
        for (std::int64_t i = 0; i < len; ++i) {
            const std::int64_t j = off + i;
            if (j >= 0 && j < static_cast<std::int64_t>(buffer_.size())) {
                slot[static_cast<std::size_t>(i)] = buffer_[static_cast<std::size_t>(j)];
            }
        }
        const std::int64_t k = (next_slot_ - clock_.first_boundary()) / len;
        const double utc = clock_.first_boundary_utc() + static_cast<double>(k) * config_.protocol.slot_seconds;
        decode_slot(slot, utc, static_cast<SampleIndex>(std::max<std::int64_t>(0, next_slot_)) * static_cast<SampleIndex>(input_.factor()), out);
        next_slot_ += len;
    }
    // Keep only what the next slot still needs.
    const std::int64_t drop = std::min<std::int64_t>(next_slot_ - buffer_start_,
                                                     static_cast<std::int64_t>(buffer_.size()));
    if (drop > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + drop);
        buffer_start_ += drop;
    }
    return {};
}

namespace {

struct Candidate {
    double score = 0.0;
    int frame = 0;
    int bin = 0;
};

// Section 7's subtraction: g = LPF[s r*], s' = s - 2 Re(g r).
void subtract(std::vector<float>& x, std::span<const Complex32> ref, std::int64_t start,
              double freq, int lpf) {
    const auto n = static_cast<std::int64_t>(ref.size());
    std::vector<C> r(static_cast<std::size_t>(n));
    std::vector<C> prod(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) {
        const double ph = 2.0 * std::numbers::pi * freq * static_cast<double>(start + i) / kWsjtRate;
        r[static_cast<std::size_t>(i)] = C(ref[static_cast<std::size_t>(i)]) * C(std::cos(ph), std::sin(ph));
        const std::int64_t j = start + i;
        const double s = (j >= 0 && j < static_cast<std::int64_t>(x.size())) ? x[static_cast<std::size_t>(j)] : 0.0;
        prod[static_cast<std::size_t>(i)] = s * std::conj(r[static_cast<std::size_t>(i)]);
    }
    // Moving average of `lpf` samples, centred: NOT SPECIFIED. Divided by
    // the same average of |r|^2, so the gain holds through the ramps.
    std::vector<C> cum(static_cast<std::size_t>(n) + 1);
    std::vector<double> cume(static_cast<std::size_t>(n) + 1);
    for (std::int64_t i = 0; i < n; ++i) {
        const auto u = static_cast<std::size_t>(i);
        cum[u + 1] = cum[u] + prod[u];
        cume[u + 1] = cume[u] + std::norm(r[u]);
    }
    for (std::int64_t i = 0; i < n; ++i) {
        const auto a = static_cast<std::size_t>(std::max<std::int64_t>(0, i - lpf / 2));
        const auto b = static_cast<std::size_t>(std::min<std::int64_t>(n, i + lpf / 2 + 1));
        const double e = cume[b] - cume[a];
        if (e < 1e-6 * static_cast<double>(b - a)) continue;
        const C g = (cum[b] - cum[a]) / e;
        const std::int64_t j = start + i;
        if (j >= 0 && j < static_cast<std::int64_t>(x.size())) {
            x[static_cast<std::size_t>(j)] -= static_cast<float>(2.0 * std::real(g * r[static_cast<std::size_t>(i)]));
        }
    }
}

}  // namespace

void FtDecoder::decode_slot(std::span<const float> slot_12k, double slot_utc,
                            SampleIndex slot_start, std::vector<FtDecode>& out) {
    const FtProtocol& p = config_.protocol;
    const int M = p.tones;
    const int sps = p.samples_per_symbol;
    const int nsym = static_cast<int>(p.layout.size());
    const auto L = static_cast<std::int64_t>(std::llround(p.slot_seconds * kWsjtRate));
    const int D = p.decimation;
    const auto Nb = static_cast<std::size_t>(L / D);
    const int spsb = sps / D;
    const double spacing = ft_tone_spacing(p);
    const double rate_b = static_cast<double>(kWsjtRate) / D;

    std::vector<float> x(static_cast<std::size_t>(L), 0.0F);
    std::copy_n(slot_12k.begin(), std::min<std::size_t>(slot_12k.size(), x.size()), x.begin());

    // Sync positions.
    std::vector<std::pair<int, int>> sync;  // (symbol, tone)
    std::vector<int> data_pos;
    for (int i = 0; i < nsym; ++i) {
        const int l = p.layout[static_cast<std::size_t>(i)];
        if (l >= 0) sync.emplace_back(i, l);
        else if (l == kFtData) data_pos.push_back(i);
    }

    std::set<Wsjt77> found;
    const int nfft = 2 * sps;
    const int hop = sps / 4;
    const auto max_start = L - static_cast<std::int64_t>(nsym) * sps;
    const int frames = static_cast<int>((L - sps) / hop) + 1;
    const int start_frames = static_cast<int>(max_start / hop) + 1;
    const int bin_lo = std::max(0, static_cast<int>(std::floor(config_.low_hz / (spacing / 2))));
    const int bin_hi = std::min(nfft / 2 - 2 * M, static_cast<int>(std::ceil(config_.high_hz / (spacing / 2))));

    for (int pass = 1; pass <= config_.passes; ++pass) {
        // Step 1: spectrogram.
        const int nb = nfft / 2 + 1;
        std::vector<float> P(static_cast<std::size_t>(frames) * static_cast<std::size_t>(nb));
        std::vector<C> buf(static_cast<std::size_t>(nfft));
        for (int f = 0; f < frames; ++f) {
            std::fill(buf.begin(), buf.end(), C{});
            for (int i = 0; i < sps; ++i) buf[static_cast<std::size_t>(i)] = x[static_cast<std::size_t>(f * hop + i)];
            wsjt_fft(buf, false);
            for (int b = 0; b < nb; ++b) P[static_cast<std::size_t>(f) * nb + b] = static_cast<float>(std::norm(buf[static_cast<std::size_t>(b)]));
        }
        const auto score_at = [&](int t, int b) {
            double s = 0.0;
            int cnt = 0;
            for (const auto& [sym, tone] : sync) {
                const int f = t + 4 * sym;
                if (f >= frames) continue;
                const float* row = &P[static_cast<std::size_t>(f) * nb];
                double all = 0.0;
                for (int k = 0; k < M; ++k) all += row[b + 2 * k];
                if (all <= 0.0) continue;
                s += row[b + 2 * tone] / (all / M);
                ++cnt;
            }
            return cnt ? s / cnt : 0.0;
        };
        std::vector<double> S(static_cast<std::size_t>(start_frames) * static_cast<std::size_t>(nb), 0.0);
        for (int t = 0; t < start_frames; ++t) {
            for (int b = bin_lo; b <= bin_hi; ++b) S[static_cast<std::size_t>(t) * nb + b] = score_at(t, b);
        }
        std::vector<Candidate> cands;
        for (int t = 0; t < start_frames; ++t) {
            for (int b = bin_lo; b <= bin_hi; ++b) {
                const double s = S[static_cast<std::size_t>(t) * nb + b];
                if (s < config_.sync_threshold) continue;
                bool peak = true;
                // NOT SPECIFIED: a peak must beat its neighbours within two
                // frames and one bin.
                for (int dt = -2; dt <= 2 && peak; ++dt) {
                    for (int db = -1; db <= 1; ++db) {
                        const int tt = t + dt;
                        const int bb = b + db;
                        if ((dt == 0 && db == 0) || tt < 0 || tt >= start_frames || bb < bin_lo || bb > bin_hi) continue;
                        const double o = S[static_cast<std::size_t>(tt) * nb + bb];
                        if (o > s || (o == s && (dt < 0 || (dt == 0 && db < 0)))) {
                            peak = false;
                            break;
                        }
                    }
                }
                if (peak) cands.push_back({s, t, b});
            }
        }
        std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
            if (a.score != b.score) return a.score > b.score;
            if (a.frame != b.frame) return a.frame < b.frame;
            return a.bin < b.bin;
        });
        if (static_cast<int>(cands.size()) > config_.max_candidates) cands.resize(static_cast<std::size_t>(config_.max_candidates));

        // Step 2: one FFT of the slot to cut every candidate from.
        std::vector<C> X(static_cast<std::size_t>(L));
        for (std::int64_t i = 0; i < L; ++i) X[static_cast<std::size_t>(i)] = x[static_cast<std::size_t>(i)];
        wsjt_fft(X, false);

        struct Hit {
            std::int64_t start12 = 0;
            double freq = 0.0;
            std::vector<int> tones;
        };
        std::vector<Hit> hits;
        std::vector<C> y(Nb);
        for (const auto& cand : cands) {
            const double f0 = cand.bin * spacing / 2;
            const auto kc = static_cast<std::int64_t>(std::llround(f0 * static_cast<double>(L) / kWsjtRate));
            const double f0q = static_cast<double>(kc) * kWsjtRate / static_cast<double>(L);
            std::fill(y.begin(), y.end(), C{});
            const auto half = static_cast<std::int64_t>(Nb / 2);
            for (std::int64_t m = -half; m < half; ++m) {
                const std::int64_t k = kc + m;
                if (k <= 0 || k >= L / 2) continue;
                const double fr = static_cast<double>(m) * kWsjtRate / static_cast<double>(L);
                // Pass band from 1.5 spacings below tone 0 to 1.5 above the
                // top tone, cosine edges one spacing wide: NOT SPECIFIED.
                const double lo = -1.5 * spacing;
                const double hi = (M - 1 + 1.5) * spacing;
                double w = 1.0;
                if (fr < lo - spacing || fr > hi + spacing) continue;
                if (fr < lo) w = 0.5 * (1 + std::cos(std::numbers::pi * (lo - fr) / spacing));
                if (fr > hi) w = 0.5 * (1 + std::cos(std::numbers::pi * (fr - hi) / spacing));
                y[static_cast<std::size_t>((m + static_cast<std::int64_t>(Nb)) % static_cast<std::int64_t>(Nb))] = X[static_cast<std::size_t>(k)] * w;
            }
            wsjt_fft(y, true);

            // Refinement.
            const int t0b = cand.frame * hop / D;
            double best = -1.0;
            int best_s = t0b;
            double best_df = 0.0;
            const int srange = hop / D + 2;
            std::vector<C> basis(static_cast<std::size_t>(spsb));
            const auto window = [&](int s0, int tone, double frac, double& energy) {
                C z{};
                energy = 0.0;
                for (int n = 0; n < spsb; ++n) {
                    const int idx = s0 + n;
                    if (idx < 0 || idx >= static_cast<int>(Nb)) continue;
                    const double ph = -2.0 * std::numbers::pi * (tone + frac) * n / spsb;
                    const C v = y[static_cast<std::size_t>(idx)];
                    z += v * C(std::cos(ph), std::sin(ph));
                    energy += std::norm(v);
                }
                return z;
            };
            for (int fi = -6; fi <= 6; ++fi) {
                const double frac = fi / 16.0;  // fraction of a tone spacing
                for (int s = t0b - srange; s <= t0b + srange; ++s) {
                    double num = 0.0;
                    double den = 0.0;
                    for (const auto& [sym, tone] : sync) {
                        double e = 0.0;
                        const C z = window(s + sym * spsb, tone, frac, e);
                        num += std::norm(z);
                        den += e * spsb;
                    }
                    const double metric = den > 0 ? num / den : 0.0;
                    if (metric > best) {
                        best = metric;
                        best_s = s;
                        best_df = frac;
                    }
                }
            }

            // Step 3: single-symbol detection.
            std::vector<std::vector<double>> mag(static_cast<std::size_t>(nsym), std::vector<double>(static_cast<std::size_t>(M)));
            for (int i = 0; i < nsym; ++i) {
                for (int k = 0; k < M; ++k) {
                    double e = 0.0;
                    mag[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)] = std::abs(window(best_s + i * spsb, k, best_df, e));
                }
            }
            std::array<float, kFtN> llr{};
            std::size_t bit = 0;
            for (const int i : data_pos) {
                const auto& m = mag[static_cast<std::size_t>(i)];
                for (int j = p.bits_per_symbol - 1; j >= 0; --j) {
                    double m0 = 0.0;
                    double m1 = 0.0;
                    for (int k = 0; k < M; ++k) {
                        const bool one = ((p.gray[static_cast<std::size_t>(k)] >> j) & 1) != 0;
                        double& t = one ? m1 : m0;
                        t = std::max(t, m[static_cast<std::size_t>(k)]);
                    }
                    llr[bit++] = static_cast<float>(m0 - m1);
                }
            }
            double var = 0.0;
            for (const float v : llr) var += static_cast<double>(v) * v;
            var /= kFtN;
            if (var <= 0.0) continue;
            // NOT SPECIFIED: unit variance, then a scale found by test.
            const double scale = 2.83 / std::sqrt(var);
            for (auto& v : llr) v = static_cast<float>(v * scale);

            // Step 4.
            std::array<std::uint8_t, kFtN> cw{};
            const auto res = ldpc_.decode(llr, cw);
            if (!res.converged) continue;
            if (std::all_of(cw.begin(), cw.end(), [](std::uint8_t v) { return v == 0; })) continue;
            const auto crc = wsjt_crc14(std::span<const std::uint8_t>(cw.data(), 77));
            if (!std::equal(crc.begin(), crc.end(), cw.begin() + 77)) continue;
            Wsjt77 payload{};
            for (std::size_t i = 0; i < 77; ++i) payload[i] = static_cast<std::uint8_t>(cw[i] ^ (p.scramble ? p.scrambler[i] : 0U));
            const auto tones = ft_channel_tones(p, payload);
            const std::int64_t start12 = static_cast<std::int64_t>(best_s) * D;
            const double freq = f0q + best_df * spacing;
            if (found.contains(payload)) continue;
            found.insert(payload);
            hits.push_back({start12, freq, tones});

            // SNR: decoded tones against the others, over every symbol.
            double es = 0.0;
            double en = 0.0;
            int ns = 0;
            int nn = 0;
            for (int i = 0; i < nsym; ++i) {
                if (p.layout[static_cast<std::size_t>(i)] == kFtRamp) continue;
                for (int k = 0; k < M; ++k) {
                    const double e = mag[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)];
                    if (k == tones[static_cast<std::size_t>(i)]) { es += e * e; ++ns; }
                    else { en += e * e; ++nn; }
                }
            }
            es /= std::max(1, ns);
            en /= std::max(1, nn);
            const double ratio = std::max(1e-3, (es - en) / std::max(en, 1e-30));
            FtDecode d;
            d.slot_utc = slot_utc;
            d.slot_start_sample = slot_start;
            d.frequency_hz = freq;
            d.dt_seconds = static_cast<double>(start12) / kWsjtRate - p.nominal_start_seconds;
            d.snr_db = 10.0 * std::log10(ratio * spacing / 2500.0);
            d.pass = pass;
            d.ldpc_iterations = res.iterations;
            d.payload = payload;
            d.message = wsjt_unpack(payload, &hashes_);
            out.push_back(std::move(d));
        }
        (void)rate_b;
        if (hits.empty()) break;
        if (pass == config_.passes) break;
        for (const auto& h : hits) {
            const auto ref = ft_reference(p, h.tones, kWsjtRate);
            subtract(x, ref, h.start12, h.freq, sps);
        }
    }
}

}  // namespace revenant::decode
