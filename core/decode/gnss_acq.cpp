// GNSS front end. See gnss_acq.h for the documents and the clean-room note.

#include "core/decode/gnss_acq.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <utility>

#include "core/characterise/transform.h"

namespace revenant::decode::gnss {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

// IS-GPS-200N G2 delays in chips, by PRN. Index 0 unused.
//   1-37:   Table 3-Ia, "Code Delay Chips, C/A" column.
//   38-63:  Table 3-Ib, "G2 Delay (Chips)".
//   64-210: Table 6-I sheets 1 to 5, "G2 Delay (Chips)".
// PRN 34 and 37 share a code (Table 3-Ia note **).
constexpr std::array<unsigned short, 211> kG2Delay = {
    0,
    // 1-37
    5, 6, 7, 8, 17, 18, 139, 140, 141, 251, 252, 254, 255, 256, 257, 258, 469, 470, 471,
    472, 473, 474, 509, 512, 513, 514, 515, 516, 859, 860, 861, 862, 863, 950, 947, 948, 950,
    // 38-63
    67, 103, 91, 19, 679, 225, 625, 946, 638, 161, 1001, 554, 280, 710, 709, 775, 864, 558,
    220, 397, 55, 898, 759, 367, 299, 1018,
    // 64-95, sheet 1
    729, 695, 780, 801, 788, 732, 34, 320, 327, 389, 407, 525, 405, 221, 761, 260, 326, 955,
    653, 699, 422, 188, 438, 959, 539, 879, 677, 586, 153, 792, 814, 446,
    // 96-125, sheet 2
    264, 1015, 278, 536, 819, 156, 957, 159, 712, 885, 461, 248, 713, 126, 807, 279, 122, 197,
    693, 632, 771, 467, 647, 203, 145, 175, 52, 21, 237, 235,
    // 126-155, sheet 3
    886, 657, 634, 762, 355, 1012, 176, 603, 130, 359, 595, 68, 386, 797, 456, 499, 883, 307,
    127, 211, 121, 118, 163, 628, 853, 484, 289, 811, 202, 1021,
    // 156-185, sheet 4
    463, 568, 904, 670, 230, 911, 684, 309, 644, 932, 12, 314, 891, 212, 185, 675, 503, 150,
    395, 345, 846, 798, 992, 357, 995, 877, 112, 144, 476, 193,
    // 186-210, sheet 5
    109, 445, 291, 87, 399, 292, 901, 339, 208, 711, 189, 263, 537, 663, 942, 173, 900, 30,
    500, 935, 556, 373, 85, 652, 310,
};

// BDS-SIS-ICD-B1I-3.0 Table 4-1: the G2 register stages XORed to form each
// PRN's phase. Two stages for 1 to 37, three for 38 to 63.
constexpr std::array<std::array<unsigned char, 3>, 64> kB1iTaps = {{
    {0, 0, 0},
    {1, 3, 0}, {1, 4, 0}, {1, 5, 0}, {1, 6, 0}, {1, 8, 0}, {1, 9, 0}, {1, 10, 0}, {1, 11, 0},
    {2, 7, 0}, {3, 4, 0}, {3, 5, 0}, {3, 6, 0}, {3, 8, 0}, {3, 9, 0}, {3, 10, 0}, {3, 11, 0},
    {4, 5, 0}, {4, 6, 0}, {4, 8, 0}, {4, 9, 0}, {4, 10, 0}, {4, 11, 0}, {5, 6, 0}, {5, 8, 0},
    {5, 9, 0}, {5, 10, 0}, {5, 11, 0}, {6, 8, 0}, {6, 9, 0}, {6, 10, 0}, {6, 11, 0}, {8, 9, 0},
    {8, 10, 0}, {8, 11, 0}, {9, 10, 0}, {9, 11, 0}, {10, 11, 0},
    {1, 2, 7}, {1, 3, 4}, {1, 3, 6}, {1, 3, 8}, {1, 3, 10}, {1, 3, 11}, {1, 4, 5}, {1, 4, 9},
    {1, 5, 6}, {1, 5, 8}, {1, 5, 10}, {1, 5, 11}, {1, 6, 9}, {1, 8, 9}, {1, 9, 10}, {1, 9, 11},
    {2, 3, 7}, {2, 5, 7}, {2, 7, 9}, {3, 4, 5}, {3, 4, 9}, {3, 5, 6}, {3, 5, 8}, {3, 5, 10},
    {3, 5, 11}, {3, 6, 9},
}};

std::vector<std::int8_t> to_chips(const std::vector<std::uint8_t>& bits) {
    std::vector<std::int8_t> out(bits.size());
    for (std::size_t i = 0; i < bits.size(); ++i) {
        out[i] = bits[i] != 0 ? std::int8_t{-1} : std::int8_t{1};
    }
    return out;
}

// One output per clock from a Fibonacci register: stage `out_stage` is read,
// then the XOR of `feedback` stages enters stage 1 and the rest shift up.
// Stages are 1-based, as the documents number them.
std::vector<std::uint8_t> lfsr(unsigned stages, std::vector<std::uint8_t> state,
                               std::initializer_list<unsigned> feedback, unsigned length,
                               std::initializer_list<unsigned> out_stages) {
    std::vector<std::uint8_t> out(length);
    for (unsigned n = 0; n < length; ++n) {
        std::uint8_t o = 0;
        for (unsigned s : out_stages) {
            o ^= state[s - 1];
        }
        out[n] = o;
        std::uint8_t fb = 0;
        for (unsigned s : feedback) {
            fb ^= state[s - 1];
        }
        for (unsigned s = stages - 1; s > 0; --s) {
            state[s] = state[s - 1];
        }
        state[0] = fb;
    }
    return out;
}

std::size_t next_pow2(std::size_t n) {
    std::size_t p = 1;
    while (p < n) {
        p <<= 1U;
    }
    return p;
}

// Linear interpolation of `block` at fractional index `t`.
std::complex<double> interp(std::span<const Complex32> block, double t) {
    const auto i = static_cast<std::size_t>(t);
    const double f = t - static_cast<double>(i);
    const Complex32 a = block[i];
    const Complex32 b = (i + 1 < block.size()) ? block[i + 1] : a;
    return {a.real() + f * (b.real() - a.real()), a.imag() + f * (b.imag() - a.imag())};
}

// The block resampled to a power-of-two number of samples per code period.
// Spectra are made one Doppler bin at a time and shared by every signal being
// searched, so the memory is a few periods of one bin, not the whole grid: a
// 20 ms L2 CM period over a 10 kHz span at 25 Hz steps would otherwise hold
// gigabytes.
struct SearchGrid {
    std::size_t nfft = 0;
    double samples_per_period = 0.0;
    double period_s = 0.0;
    double offset_hz = 0.0;
    std::vector<double> dopplers;
    std::vector<std::vector<characterise::Complex64>> resampled;  // [period]
};

void build_grid(std::span<const Complex32> block, dsp::SampleRate rate, double period_s,
                double offset_hz, const AcquisitionConfig& cfg, SearchGrid& g) {
    g.period_s = period_s;
    g.offset_hz = offset_hz;
    g.samples_per_period = static_cast<double>(rate) * period_s;
    g.nfft = next_pow2(static_cast<std::size_t>(std::ceil(g.samples_per_period)));
    g.dopplers.clear();
    const int half = static_cast<int>(std::floor(cfg.doppler_max_hz / cfg.doppler_step_hz));
    for (int b = -half; b <= half; ++b) {
        g.dopplers.push_back(b * cfg.doppler_step_hz);
    }
    // As many whole periods as the block holds, up to the configured count.
    const auto avail = static_cast<unsigned>(
        std::floor((static_cast<double>(block.size()) - 1.0) / g.samples_per_period));
    const unsigned m = std::min(cfg.noncoherent_periods, avail);
    g.resampled.assign(m, {});
    const double step = g.samples_per_period / static_cast<double>(g.nfft);
    for (unsigned k = 0; k < m; ++k) {
        g.resampled[k].resize(g.nfft);
        for (std::size_t j = 0; j < g.nfft; ++j) {
            g.resampled[k][j] = interp(block, static_cast<double>(k) * g.samples_per_period +
                                                  static_cast<double>(j) * step);
        }
    }
}

// The spectrum of period k wiped off at Doppler bin b.
void bin_spectrum(const SearchGrid& g, std::size_t b, unsigned k,
                  std::vector<characterise::Complex64>& x) {
    const double f = g.offset_hz + g.dopplers[b];
    const double dt = g.period_s / static_cast<double>(g.nfft);
    x.resize(g.nfft);
    const double t0 = static_cast<double>(k) * g.period_s;
    const std::complex<double> step_rot = std::polar(1.0, -kTwoPi * f * dt);
    std::complex<double> rot = std::polar(1.0, -kTwoPi * f * t0);
    for (std::size_t j = 0; j < g.nfft; ++j) {
        x[j] = g.resampled[k][j] * rot;
        rot *= step_rot;
    }
    characterise::fft_in_place(x);
}

std::vector<characterise::Complex64> code_spectrum_conj(const SignalSpec& spec, std::size_t nfft) {
    std::vector<characterise::Complex64> c(nfft);
    const double len = static_cast<double>(spec.code.size());
    for (std::size_t j = 0; j < nfft; ++j) {
        const auto idx = static_cast<std::size_t>(static_cast<double>(j) * len /
                                                  static_cast<double>(nfft));
        c[j] = static_cast<double>(spec.code[std::min(idx, spec.code.size() - 1)]);
    }
    characterise::fft_in_place(c);
    for (auto& v : c) {
        v = std::conj(v);
    }
    return c;
}

// A code long enough that every period carries a symbol or overlay edge
// somewhere inside the block's periods, as L2 CM's 20 ms does with its 50 sps.
constexpr double kLongCodeS = 0.004;

// The prompt over block periods re-cut to start on the found code epoch, so
// no symbol edge falls inside one, at carrier offset f.
std::complex<double> aligned_prompt(const SearchGrid& g, const SignalSpec& spec,
                                    std::size_t epoch_idx, unsigned j, double f) {
    const std::size_t n0 = epoch_idx + static_cast<std::size_t>(j) * g.nfft;
    const double dt = g.period_s / static_cast<double>(g.nfft);
    const double len = static_cast<double>(spec.code.size());
    const double w = -kTwoPi * (g.offset_hz + f) * dt;
    std::complex<double> rot = std::polar(1.0, w * static_cast<double>(n0));
    const std::complex<double> step = std::polar(1.0, w);
    std::complex<double> acc{};
    for (std::size_t q = 0; q < g.nfft; ++q) {
        const std::size_t n = n0 + q;
        const std::size_t k = n / g.nfft;
        if (k >= g.resampled.size()) {
            break;
        }
        const auto chip = static_cast<std::size_t>(static_cast<double>(q) * len /
                                                   static_cast<double>(g.nfft));
        acc += g.resampled[k][n % g.nfft] * rot *
               static_cast<double>(spec.code[std::min(chip, spec.code.size() - 1)]);
        rot *= step;
    }
    return acc;
}

// For long codes the block's periods each straddle a symbol edge, which
// splits the correlation peak in frequency and can leave the best bin half a
// coherent bandwidth, 1/(2T), off: exactly where the squared-phase refinement
// and the tracker's FLL are both blind. So the bin is re-chosen among d and
// d +- 1/(2T) on epoch-aligned periods, then refined on those.
double refine_aligned(const SearchGrid& g, const SignalSpec& spec, std::size_t best_idx,
                      double d) {
    const auto periods = static_cast<unsigned>(g.resampled.size() - 1);
    const double half = 0.5 / g.period_s;
    double best = -1.0;
    double f_best = d;
    for (double f : {d - half, d, d + half}) {
        double p = 0.0;
        for (unsigned j = 0; j < periods; ++j) {
            p += std::norm(aligned_prompt(g, spec, best_idx, j, f));
        }
        if (p > best) {
            best = p;
            f_best = f;
        }
    }
    std::complex<double> sum{};
    std::complex<double> last = aligned_prompt(g, spec, best_idx, 0, f_best);
    for (unsigned j = 1; j < periods; ++j) {
        const auto p = aligned_prompt(g, spec, best_idx, j, f_best);
        const auto dd = p * std::conj(last);
        sum += dd * dd;
        last = p;
    }
    return f_best + std::arg(sum) / 2.0 / (kTwoPi * g.period_s);
}

std::vector<AcquisitionEvent> search(const SearchGrid& g, std::span<const SignalSpec* const> specs,
                                     const AcquisitionConfig& cfg) {
    struct State {
        std::vector<characterise::Complex64> cconj;
        double best = -1.0;
        std::size_t best_bin = 0;
        std::size_t best_idx = 0;
        double total = 0.0;
        std::size_t cells = 0;
    };
    std::vector<AcquisitionEvent> events(specs.size());
    for (std::size_t i = 0; i < specs.size(); ++i) {
        events[i].id = specs[i]->id;
    }
    const auto m = static_cast<unsigned>(g.resampled.size());
    if (m == 0) {
        return events;
    }
    std::vector<State> st(specs.size());
    for (std::size_t i = 0; i < specs.size(); ++i) {
        st[i].cconj = code_spectrum_conj(*specs[i], g.nfft);
    }
    std::vector<std::vector<characterise::Complex64>> spectra(m);
    std::vector<double> power(g.nfft);
    std::vector<characterise::Complex64> y(g.nfft);
    const double inv_n = 1.0 / static_cast<double>(g.nfft);
    for (std::size_t b = 0; b < g.dopplers.size(); ++b) {
        for (unsigned k = 0; k < m; ++k) {
            bin_spectrum(g, b, k, spectra[k]);
        }
        for (auto& s : st) {
            std::fill(power.begin(), power.end(), 0.0);
            for (unsigned k = 0; k < m; ++k) {
                const auto& x = spectra[k];
                for (std::size_t j = 0; j < g.nfft; ++j) {
                    y[j] = std::conj(x[j] * s.cconj[j]);  // inverse via the conjugate trick
                }
                characterise::fft_in_place(y);
                for (std::size_t j = 0; j < g.nfft; ++j) {
                    power[j] += std::norm(y[j]) * inv_n * inv_n;
                }
            }
            for (std::size_t j = 0; j < g.nfft; ++j) {
                s.total += power[j];
                if (power[j] > s.best) {
                    s.best = power[j];
                    s.best_bin = b;
                    s.best_idx = j;
                }
            }
            s.cells += g.nfft;
        }
    }
    for (std::size_t i = 0; i < specs.size(); ++i) {
        auto& ev = events[i];
        const auto& s = st[i];
        const double mean = (s.total - s.best) / static_cast<double>(s.cells - 1);
        ev.peak_ratio = mean > 0.0 ? s.best / mean : 0.0;
        ev.found = ev.peak_ratio >= cfg.threshold;
        const double len = static_cast<double>(specs[i]->code.size());
        ev.code_phase_chips = static_cast<double>((g.nfft - s.best_idx) % g.nfft) * len /
                              static_cast<double>(g.nfft);
        ev.doppler_hz = g.dopplers[s.best_bin];

        if (g.period_s >= kLongCodeS && m >= 3) {
            ev.doppler_hz = refine_aligned(g, *specs[i], s.best_idx, ev.doppler_hz);
            continue;
        }
        // Refine the Doppler from the prompt's phase advance period to
        // period, squared against sign flips: unambiguous to a quarter of
        // 1/T, which a half-bin error is inside.
        if (m >= 2) {
            // Correlate in the frequency domain at the one lag.
            std::vector<characterise::Complex64> prompts(m);
            std::vector<characterise::Complex64> x;
            for (unsigned k = 0; k < m; ++k) {
                bin_spectrum(g, s.best_bin, k, x);
                std::complex<double> acc{};
                // Parseval: sum_t x[t] c[t-lag] = (1/N) sum_f X[f] conj(C[f]) e^{+j2pi f lag/N}
                const std::complex<double> w = std::polar(
                    1.0, kTwoPi * static_cast<double>(s.best_idx) / static_cast<double>(g.nfft));
                std::complex<double> rot{1.0, 0.0};
                for (std::size_t j = 0; j < g.nfft; ++j) {
                    acc += x[j] * s.cconj[j] * rot;
                    rot *= w;
                }
                prompts[k] = acc * inv_n;
            }
            std::complex<double> sum{};
            for (unsigned k = 0; k + 1 < m; ++k) {
                const auto d = prompts[k + 1] * std::conj(prompts[k]);
                sum += d * d;
            }
            const double dphi = std::arg(sum) / 2.0;
            ev.doppler_hz += dphi / (kTwoPi * g.period_s);
        }
    }
    return events;
}

double code_period_s(const SignalSpec& spec) {
    return static_cast<double>(spec.code.size()) / spec.chip_rate_hz;
}

}  // namespace

// ---------------------------------------------------------------------------

unsigned gps_ca_g2_delay(unsigned prn) {
    return prn < kG2Delay.size() ? kG2Delay[prn] : 0U;
}

std::vector<std::int8_t> gps_ca_code(unsigned prn) {
    const unsigned delay = gps_ca_g2_delay(prn);
    if (delay == 0) {
        return {};
    }
    // IS-GPS-200N 3.3.2.3: G1 = X^10 + X^3 + 1, G2 = X^10 + X^9 + X^8 + X^6 +
    // X^3 + X^2 + 1, both initialised 1111111111, output from stage 10.
    const std::vector<std::uint8_t> ones(10, 1);
    const auto g1 = lfsr(10, ones, {3, 10}, 1023, {10});
    const auto g2 = lfsr(10, ones, {2, 3, 6, 8, 9, 10}, 1023, {10});
    std::vector<std::uint8_t> bits(1023);
    for (unsigned n = 0; n < 1023; ++n) {
        bits[n] = g1[n] ^ g2[(n + 1023 - delay) % 1023];
    }
    return to_chips(bits);
}

std::vector<std::int8_t> glonass_st_code() {
    // GLONASS ICD 5.1 section 3.3.2.1: G(X) = 1 + X^5 + X^9, all ones, out at 7.
    return to_chips(lfsr(9, std::vector<std::uint8_t>(9, 1), {5, 9}, 511, {7}));
}

std::vector<std::int8_t> beidou_b1i_code(unsigned prn) {
    if (prn == 0 || prn > 63) {
        return {};
    }
    // BDS-SIS-ICD-B1I-3.0 section 4.3: G1 = 1+X+X^7+X^8+X^9+X^10+X^11 and
    // G2 = 1+X+X^2+X^3+X^4+X^5+X^8+X^9+X^11, both from 01010101010, G1
    // output from stage 11, G2's phase from the Table 4-1 stages, the
    // 2047-chip Gold code truncated by its last chip.
    const std::vector<std::uint8_t> init = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0};
    const auto g1 = lfsr(11, init, {1, 7, 8, 9, 10, 11}, 2046, {11});
    const auto& t = kB1iTaps[prn];
    std::vector<std::uint8_t> g2;
    if (t[2] == 0) {
        g2 = lfsr(11, init, {1, 2, 3, 4, 5, 8, 9, 11}, 2046, {t[0], t[1]});
    } else {
        g2 = lfsr(11, init, {1, 2, 3, 4, 5, 8, 9, 11}, 2046, {t[0], t[1], t[2]});
    }
    std::vector<std::uint8_t> bits(2046);
    for (unsigned n = 0; n < 2046; ++n) {
        bits[n] = g1[n] ^ g2[n];
    }
    return to_chips(bits);
}

std::vector<std::int8_t> beidou_nh20() {
    const char* nh = "00000100110101001110";
    std::vector<std::int8_t> out(20);
    for (unsigned i = 0; i < 20; ++i) {
        out[i] = nh[i] == '1' ? std::int8_t{-1} : std::int8_t{1};
    }
    return out;
}

unsigned first_chips(std::span<const std::int8_t> code, unsigned count) {
    unsigned v = 0;
    for (unsigned i = 0; i < count && i < code.size(); ++i) {
        v = (v << 1U) | (code[i] < 0 ? 1U : 0U);
    }
    return v;
}

AcquisitionEvent acquire(std::span<const Complex32> block, dsp::SampleRate sample_rate,
                         const SignalSpec& spec, const AcquisitionConfig& config) {
    SearchGrid g;
    build_grid(block, sample_rate, code_period_s(spec), spec.offset_hz, config, g);
    const SignalSpec* one = &spec;
    return search(g, std::span<const SignalSpec* const>(&one, 1), config).front();
}

// ---------------------------------------------------------------------------

TrackingChannel::TrackingChannel(const SignalSpec& spec, const FrontEndConfig& config,
                                 SampleIndex start_sample, double code_phase_chips,
                                 double doppler_hz)
    : spec_(spec),
      cfg_(config.tracking),
      fs_(static_cast<double>(config.sample_rate)),
      code_len_(static_cast<double>(spec.code.size())),
      period_s_(code_period_s(spec)),
      next_(start_sample),
      period_start_(start_sample),
      code_phase_(code_phase_chips),
      start_phase_(code_phase_chips),
      carrier_hz_(doppler_hz),
      nco_hz_(doppler_hz) {
    code_rate_ = spec_.chip_rate_hz * (1.0 + carrier_hz_ / spec_.carrier_hz);
    // A channel handed a code phase part-way through a period integrates a
    // short first period; the loops treat it like any other, which costs one
    // slightly noisier update.
}

void TrackingChannel::process(std::span<const Complex32> samples, SampleIndex first,
                              std::vector<Prompt>& out) {
    if (lost_ || first + samples.size() <= next_) {
        return;
    }
    std::size_t i = next_ > first ? static_cast<std::size_t>(next_ - first) : 0;
    const auto& code = spec_.code;
    const auto len = code.size();
    const double d = cfg_.early_late_spacing_chips;
    if (!nco_ready_) {
        lo_ = std::polar(1.0, -kTwoPi * carrier_phase_);
        lo_step_ = std::polar(1.0, -kTwoPi * (spec_.offset_hz + nco_hz_) / fs_);
        nco_ready_ = true;
    }
    std::complex<double> lo = lo_;
    const std::complex<double>* lo_step = &lo_step_;
    double cp = code_phase_;
    double step = code_rate_ / fs_;
    for (; i < samples.size(); ++i) {
        const std::complex<double> x(samples[i].real(), samples[i].imag());
        const std::complex<double> z = x * lo;
        lo *= *lo_step;
        carrier_phase_ += (spec_.offset_hz + nco_hz_) / fs_;
        auto idx = [&](double p) {
            if (p < 0.0) {
                p += code_len_;
            } else if (p >= code_len_) {
                p -= code_len_;
            }
            auto k = static_cast<std::size_t>(p);
            return k < len ? k : len - 1;
        };
        prompt_ += z * static_cast<double>(code[idx(cp)]);
        early_ += z * static_cast<double>(code[idx(cp + d)]);
        late_ += z * static_cast<double>(code[idx(cp - d)]);
        if (!spec_.aux_code.empty()) {
            aux_ += z * static_cast<double>(spec_.aux_code[idx(cp)]);
        }
        cp += step;
        ++next_;
        if (cp >= code_len_) {
            cp -= code_len_;
            code_phase_ = cp;
            carrier_phase_ -= std::floor(carrier_phase_);
            dump(out);
            if (lost_) {
                lo_ = lo;
                return;
            }
            step = code_rate_ / fs_;
            lo = std::polar(1.0, -kTwoPi * carrier_phase_);
            lo_step_ = std::polar(1.0, -kTwoPi * (spec_.offset_hz + nco_hz_) / fs_);
            period_start_ = next_;
            start_phase_ = cp;
        }
    }
    code_phase_ = cp;
    lo_ = lo;
}

void TrackingChannel::dump(std::vector<Prompt>& out) {
    const double t = period_s_;
    const std::complex<double> p = prompt_;
    const double elapsed = static_cast<double>(periods_);

    // Carrier loop.
    double ef = 0.0;
    if (have_last_) {
        const double cross = last_prompt_.real() * p.imag() - last_prompt_.imag() * p.real();
        const double dot = last_prompt_.real() * p.real() + last_prompt_.imag() * p.imag();
        if (dot != 0.0) {
            ef = std::atan(cross / dot) / (kTwoPi * t);
        }
    }
    const double ep = p.real() != 0.0 ? std::atan(p.imag() / p.real()) / kTwoPi : 0.0;
    const double zeta = 0.7071;
    const double wn = cfg_.pll_bandwidth_hz / 0.53;
    if (elapsed < cfg_.fll_only_periods) {
        const double b = cfg_.fll_bandwidth_hz * 2.5;
        carrier_hz_ += 4.0 * b * t * ef;
        nco_hz_ = carrier_hz_;
    } else {
        if (elapsed < cfg_.fll_only_periods + cfg_.fll_assist_periods) {
            carrier_hz_ += 4.0 * cfg_.fll_bandwidth_hz * t * ef * 0.5;
        }
        carrier_hz_ += wn * wn * t * ep;
        nco_hz_ = carrier_hz_ + 2.0 * zeta * wn * ep;
    }

    // Code loop, carrier aided.
    const double e = std::abs(early_);
    const double l = std::abs(late_);
    double err = 0.0;
    if (e + l > 0.0) {
        err = (e - l) / (e + l) * (1.0 - cfg_.early_late_spacing_chips) / 1.0;
    }
    code_rate_ = spec_.chip_rate_hz * (1.0 + carrier_hz_ / spec_.carrier_hz) +
                 4.0 * cfg_.dll_bandwidth_hz * err;

    // C/N0 by the moments method, and a phase-lock indicator.
    const double p2 = std::norm(p);
    m2_ += p2;
    m4_ += p2 * p2;
    if (++moments_ == 200) {
        const double mm2 = m2_ / moments_;
        const double mm4 = m4_ / moments_;
        const double pd = std::sqrt(std::max(0.0, 2.0 * mm2 * mm2 - mm4));
        const double pn = mm2 - pd;
        if (pn > 0.0 && pd > 0.0) {
            cn0_ = 10.0 * std::log10(pd / pn / t);
        } else if (pd > 0.0) {
            cn0_ = 60.0;
        } else {
            cn0_ = 0.0;
        }
        m2_ = m4_ = 0.0;
        moments_ = 0;
    }
    if (p2 > 0.0) {
        const double c2 = (p.real() * p.real() - p.imag() * p.imag()) / p2;
        lock_metric_ += 0.02 * (c2 - lock_metric_);
    }
    if (cn0_ > 0.0 && cn0_ < cfg_.loss_cn0_dbhz) {
        if (++weak_ >= cfg_.loss_periods) {
            lost_ = true;
        }
    } else if (cn0_ > 0.0) {
        weak_ = 0;
    }

    Prompt pr;
    pr.id = spec_.id;
    pr.sample = period_start_;
    pr.period = periods_;
    pr.prompt = p;
    pr.aux = aux_;
    pr.code_phase_chips = start_phase_;
    pr.doppler_hz = carrier_hz_;
    pr.cn0_dbhz = cn0_;
    pr.phase_locked = lock_metric_ > 0.6 && elapsed >= cfg_.fll_only_periods;
    out.push_back(pr);

    last_prompt_ = p;
    have_last_ = true;
    early_ = prompt_ = late_ = aux_ = {};
    ++periods_;
}

// ---------------------------------------------------------------------------

Expected<FrontEnd> FrontEnd::create(const FrontEndConfig& config,
                                    std::vector<SignalSpec> signals) {
    if (config.sample_rate <= 0) {
        return fail("gnss front end: sample rate must be positive");
    }
    if (signals.empty()) {
        return fail("gnss front end: no signals to search for");
    }
    const double period = code_period_s(signals.front());
    for (const auto& s : signals) {
        if (s.code.empty() || s.chip_rate_hz <= 0.0) {
            return fail("gnss front end: a signal has no code");
        }
        if (std::abs(code_period_s(s) - period) > 1e-12) {
            return fail("gnss front end: signals must share one code period");
        }
    }
    if (config.acquisition.noncoherent_periods == 0 || config.acquisition.doppler_step_hz <= 0.0) {
        return fail("gnss front end: acquisition grid is empty");
    }
    FrontEnd fe;
    fe.config_ = config;
    fe.signals_ = std::move(signals);
    fe.block_periods_ = config.acquisition.noncoherent_periods;
    fe.block_samples_ = static_cast<std::size_t>(std::ceil(
                            static_cast<double>(fe.block_periods_) * period *
                            static_cast<double>(config.sample_rate))) +
                        1;
    fe.block_.reserve(fe.block_samples_);
    return fe;
}

void FrontEnd::feed(std::span<const Complex32> samples, FrontEndOutput& out) {
    std::size_t pos = 0;
    while (pos < samples.size()) {
        const std::size_t n = std::min(samples.size() - pos, block_samples_ - block_.size());
        const auto seg = samples.subspan(pos, n);
        for (auto& ch : channels_) {
            ch.process(seg, next_, out.prompts);
        }
        block_.insert(block_.end(), seg.begin(), seg.end());
        next_ += n;
        pos += n;
        if (block_.size() == block_samples_) {
            run_block(out);
            ++blocks_done_;
            block_.clear();
            block_start_ = next_;
        }
    }
}

void FrontEnd::run_block(FrontEndOutput& out) {
    // Retire lost channels first so their satellites are searched now.
    for (auto it = channels_.begin(); it != channels_.end();) {
        if (it->lost()) {
            out.losses.push_back({it->id(), next_});
            retry_at_block_[it->id()] = blocks_done_;
            it = channels_.erase(it);
        } else {
            ++it;
        }
    }
    if (channels_.size() >= config_.max_channels) {
        return;
    }
    std::vector<const SignalSpec*> todo;
    for (const auto& s : signals_) {
        const bool tracked = std::any_of(channels_.begin(), channels_.end(),
                                         [&](const TrackingChannel& c) { return c.id() == s.id; });
        if (tracked) {
            continue;
        }
        const auto r = retry_at_block_.find(s.id);
        if (r != retry_at_block_.end() && r->second > blocks_done_) {
            continue;
        }
        todo.push_back(&s);
    }
    if (todo.empty()) {
        return;
    }
    // Signals sharing a centre share the transformed block, one Doppler
    // bin at a time.
    std::vector<std::pair<AcquisitionEvent, const SignalSpec*>> found;
    std::map<double, std::vector<const SignalSpec*>> by_offset;
    for (const SignalSpec* s : todo) {
        by_offset[s->offset_hz].push_back(s);
    }
    const double period = code_period_s(signals_.front());
    std::map<const SignalSpec*, AcquisitionEvent> results;
    for (const auto& [offset, group] : by_offset) {
        SearchGrid g;
        build_grid(block_, config_.sample_rate, period, offset, config_.acquisition, g);
        const auto evs = search(g, group, config_.acquisition);
        for (std::size_t i = 0; i < group.size(); ++i) {
            results[group[i]] = evs[i];
        }
    }
    for (const SignalSpec* s : todo) {
        AcquisitionEvent ev = results[s];
        ev.block_start = block_start_;
        out.acquisitions.push_back(ev);
        if (ev.found) {
            found.emplace_back(ev, s);
        } else {
            const std::uint64_t wait =
                std::max<std::uint64_t>(1, config_.acquisition.retry_periods / block_periods_);
            retry_at_block_[s->id] = blocks_done_ + wait;
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.first.peak_ratio > b.first.peak_ratio;
    });
    for (const auto& [ev, s] : found) {
        if (channels_.size() >= config_.max_channels) {
            break;
        }
        TrackingChannel ch(*s, config_, block_start_, ev.code_phase_chips, ev.doppler_hz);
        ch.process(block_, block_start_, out.prompts);
        channels_.push_back(std::move(ch));
    }
}

}  // namespace revenant::decode::gnss
