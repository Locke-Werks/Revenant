#include "core/decode/turbo.h"

#include <algorithm>
#include <format>
#include <limits>
#include <utility>

namespace revenant::decode {

namespace {

constexpr std::size_t kStates = 8;
constexpr std::size_t kTailClocks = 3;
// Vogt and Finger: the extrinsic scale for max-log-MAP.
constexpr float kExtrinsicScale = 0.7F;
constexpr float kNegInf = -std::numeric_limits<float>::infinity();

struct Branch {
    std::uint8_t next = 0;
    std::uint8_t y0 = 0;
    std::uint8_t y1 = 0;
};

// M.2092-2 Figure 8. State bits s1 s2 s3 are the three delay cells, s1 the
// first. The feedback d = 1 + D^2 + D^3 forms a = u + s2 + s3; the parities
// are n0 = 1 + D + D^3 and n1 = 1 + D + D^2 + D^3 applied to a.
constexpr Branch step(unsigned s, unsigned u) {
    const unsigned s1 = (s >> 2U) & 1U;
    const unsigned s2 = (s >> 1U) & 1U;
    const unsigned s3 = s & 1U;
    const unsigned a = u ^ s2 ^ s3;
    return {static_cast<std::uint8_t>((a << 2U) | (s1 << 1U) | s2),
            static_cast<std::uint8_t>(a ^ s1 ^ s3), static_cast<std::uint8_t>(a ^ s1 ^ s2 ^ s3)};
}

// The termination input: the switch at (b) feeds back the register so the
// first cell receives zero.
constexpr unsigned tail_input(unsigned s) { return ((s >> 1U) ^ s) & 1U; }

struct Rsc {
    std::vector<std::array<std::uint8_t, 3>> data;
    std::array<std::array<std::uint8_t, 3>, kTailClocks> tail{};
};

Rsc rsc_encode(std::span<const std::uint8_t> u) {
    Rsc out;
    out.data.reserve(u.size());
    unsigned s = 0;
    for (const std::uint8_t b : u) {
        const Branch br = step(s, b & 1U);
        out.data.push_back({static_cast<std::uint8_t>(b & 1U), br.y0, br.y1});
        s = br.next;
    }
    for (auto& t : out.tail) {
        const unsigned b = tail_input(s);
        const Branch br = step(s, b);
        t = {static_cast<std::uint8_t>(b), br.y0, br.y1};
        s = br.next;
    }
    return out;
}

// Max-log BCJR over one constituent trellis of k data steps and three
// termination steps. `sys`, `p0` and `p1` hold k + 3 channel LLRs, `apriori`
// k. Writes the extrinsic LLR of each data step, and its posterior.
void bcjr(std::span<const float> sys, std::span<const float> p0, std::span<const float> p1,
          std::span<const float> apriori, std::span<float> extrinsic, std::span<float> posterior,
          std::vector<float>& alpha) {
    const std::size_t k = apriori.size();
    const std::size_t n_steps = k + kTailClocks;
    alpha.assign((n_steps + 1) * kStates, kNegInf);
    alpha[0] = 0.0F;

    auto gamma = [&](std::size_t n, unsigned u, const Branch& br) {
        const float ls = sys[n] + (n < k ? apriori[n] : 0.0F);
        return 0.5F * ((u != 0U ? -ls : ls) + (br.y0 != 0U ? -p0[n] : p0[n]) +
                       (br.y1 != 0U ? -p1[n] : p1[n]));
    };

    for (std::size_t n = 0; n < n_steps; ++n) {
        const float* a = &alpha[n * kStates];
        float* next = &alpha[(n + 1) * kStates];
        for (unsigned s = 0; s < kStates; ++s) {
            if (a[s] == kNegInf) {
                continue;
            }
            for (unsigned u = 0; u < 2; ++u) {
                if (n >= k && u != tail_input(s)) {
                    continue;
                }
                const Branch br = step(s, u);
                next[br.next] = std::max(next[br.next], a[s] + gamma(n, u, br));
            }
        }
        // Normalise against growth.
        const float top = *std::max_element(next, next + kStates);
        for (std::size_t s = 0; s < kStates; ++s) {
            next[s] -= top;
        }
    }

    std::array<float, kStates> beta{};
    beta.fill(kNegInf);
    beta[0] = 0.0F;
    for (std::size_t n = n_steps; n-- > 0;) {
        std::array<float, kStates> prev{};
        prev.fill(kNegInf);
        std::array<float, 2> best{kNegInf, kNegInf};
        const float* a = &alpha[n * kStates];
        for (unsigned s = 0; s < kStates; ++s) {
            for (unsigned u = 0; u < 2; ++u) {
                if (n >= k && u != tail_input(s)) {
                    continue;
                }
                const Branch br = step(s, u);
                if (beta[br.next] == kNegInf) {
                    continue;
                }
                const float g = gamma(n, u, br) + beta[br.next];
                prev[s] = std::max(prev[s], g);
                if (n < k && a[s] != kNegInf) {
                    best[u] = std::max(best[u], a[s] + g);
                }
            }
        }
        const float top = *std::max_element(prev.begin(), prev.end());
        for (float& v : prev) {
            v -= top;
        }
        beta = prev;
        if (n < k) {
            const float app = best[0] - best[1];
            posterior[n] = app;
            extrinsic[n] = app - sys[n] - apriori[n];
        }
    }
}

}  // namespace

Expected<std::vector<std::uint32_t>> ccsds_turbo_permutation(
    std::size_t k1, std::size_t k2, std::span<const std::uint32_t, 8> primes) {
    if (k1 == 0 || k1 % 2 != 0 || k2 == 0) {
        return fail(std::format("turbo interleaver needs an even k1 and a nonzero k2, not {} and {}",
                                k1, k2));
    }
    const std::size_t k = k1 * k2;
    const std::size_t half = k1 / 2;
    std::vector<std::uint32_t> pi(k);
    std::vector<std::uint8_t> seen(k, 0);
    for (std::size_t s = 1; s <= k; ++s) {
        const std::size_t m = (s - 1) % 2;
        const std::size_t i = (s - 1) / (2 * k2);
        const std::size_t j = (s - 1) / 2 - i * k2;
        const std::size_t t = (19 * i + 1) % half;
        const std::size_t q = t % 8 + 1;
        const std::size_t c = (primes[q - 1] * j + 21 * m) % k2;
        const std::size_t p = 2 * (t + c * half + 1) - m;
        if (p < 1 || p > k || seen[p - 1] != 0) {
            return fail(std::format("turbo interleaver k1 {} k2 {} is not a permutation", k1, k2));
        }
        seen[p - 1] = 1;
        pi[s - 1] = static_cast<std::uint32_t>(p - 1);
    }
    return pi;
}

Expected<TurboCode> TurboCode::create(TurboCodeSpec spec) {
    const std::size_t k = spec.permutation.size();
    if (k == 0) {
        return fail("turbo code needs at least one information bit");
    }
    std::vector<std::uint8_t> seen(k, 0);
    for (const std::uint32_t p : spec.permutation) {
        if (p >= k || seen[p] != 0) {
            return fail("turbo code interleaver is not a permutation");
        }
        seen[p] = 1;
    }
    if (spec.data_pattern.empty()) {
        return fail("turbo code needs a data puncturing pattern");
    }
    TurboCode code;
    for (std::size_t n = 0; n < k; ++n) {
        for (const std::uint8_t v : spec.data_pattern[n % spec.data_pattern.size()]) {
            if (v > 3) {
                return fail("turbo code pattern entries are 0 to 3");
            }
            code.data_bits_ += v;
        }
    }
    for (std::size_t c = 0; c < 6; ++c) {
        for (std::size_t j = 0; j < 6; ++j) {
            const std::uint8_t v = spec.tail_pattern[c][j];
            if (v > 3) {
                return fail("turbo code pattern entries are 0 to 3");
            }
            // Clause A2-1.2.4.2: the first three termination clocks emit RSC
            // 1 only and the last three RSC 2 only.
            if (v != 0 && ((c < 3) != (j < 3))) {
                return fail("turbo code tail pattern sends an encoder that is not terminating");
            }
            code.tail_bits_ += v;
        }
    }
    code.spec_ = std::move(spec);
    return code;
}

std::vector<std::uint8_t> TurboCode::encode(std::span<const std::uint8_t> information) const {
    const std::size_t k = information_bits();
    std::vector<std::uint8_t> permuted(k);
    for (std::size_t s = 0; s < k; ++s) {
        permuted[s] = information[spec_.permutation[s]];
    }
    const Rsc r1 = rsc_encode(information.first(k));
    const Rsc r2 = rsc_encode(permuted);
    std::vector<std::uint8_t> out;
    out.reserve(encoded_bits());
    for (std::size_t n = 0; n < k; ++n) {
        const TurboClockPattern& row = spec_.data_pattern[n % spec_.data_pattern.size()];
        for (std::size_t j = 0; j < 6; ++j) {
            const std::uint8_t v = j < 3 ? r1.data[n][j] : r2.data[n][j - 3];
            out.insert(out.end(), row[j], v);
        }
    }
    for (std::size_t c = 0; c < 6; ++c) {
        for (std::size_t j = 0; j < 6; ++j) {
            // create() allows only RSC 1 entries in the first three clocks
            // and only RSC 2 entries in the last three.
            const std::uint8_t v = c < 3 ? r1.tail[c][j % 3] : r2.tail[c - 3][j % 3];
            out.insert(out.end(), spec_.tail_pattern[c][j], v);
        }
    }
    return out;
}

TurboCode::Result TurboCode::decode(
    std::span<const float> llr, std::size_t max_iterations,
    const std::function<bool(std::span<const std::uint8_t>)>& accept) const {
    const std::size_t k = information_bits();
    const std::size_t n_steps = k + kTailClocks;
    // Channel LLRs per encoder output, summed over repeats.
    std::array<std::vector<float>, 6> ch;
    for (auto& v : ch) {
        v.assign(n_steps, 0.0F);
    }
    std::size_t at = 0;
    auto take = [&]() { return at < llr.size() ? llr[at++] : 0.0F; };
    for (std::size_t n = 0; n < k; ++n) {
        const TurboClockPattern& row = spec_.data_pattern[n % spec_.data_pattern.size()];
        for (std::size_t j = 0; j < 6; ++j) {
            for (std::uint8_t r = 0; r < row[j]; ++r) {
                ch[j][n] += take();
            }
        }
    }
    for (std::size_t c = 0; c < 6; ++c) {
        for (std::size_t j = 0; j < 6; ++j) {
            for (std::uint8_t r = 0; r < spec_.tail_pattern[c][j]; ++r) {
                ch[j][k + c % 3] += take();
            }
        }
    }

    const auto& pi = spec_.permutation;
    // Systematic LLRs: X observes u, X' observes u permuted.
    std::vector<float> sys1(n_steps), sys2(n_steps);
    for (std::size_t n = 0; n < k; ++n) {
        sys1[pi[n]] += ch[3][n];
    }
    for (std::size_t n = 0; n < k; ++n) {
        sys1[n] += ch[0][n];
    }
    for (std::size_t n = 0; n < k; ++n) {
        sys2[n] = sys1[pi[n]];
    }
    for (std::size_t c = 0; c < kTailClocks; ++c) {
        sys1[k + c] = ch[0][k + c];
        sys2[k + c] = ch[3][k + c];
    }

    std::vector<float> la1(k, 0.0F), la2(k, 0.0F), le(k), app(k), alpha;
    Result result;
    result.bits.assign(k, 0);
    for (std::size_t it = 0; it < max_iterations; ++it) {
        bcjr(sys1, ch[1], ch[2], la1, le, app, alpha);
        for (std::size_t n = 0; n < k; ++n) {
            la2[n] = kExtrinsicScale * le[pi[n]];
        }
        bcjr(sys2, ch[4], ch[5], la2, le, app, alpha);
        for (std::size_t n = 0; n < k; ++n) {
            la1[pi[n]] = kExtrinsicScale * le[n];
            result.bits[pi[n]] = app[n] < 0.0F ? 1 : 0;
        }
        result.iterations = it + 1;
        if (accept && accept(result.bits)) {
            result.accepted = true;
            break;
        }
    }
    return result;
}

}  // namespace revenant::decode
