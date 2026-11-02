#include "core/decode/fano.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <limits>

namespace revenant::decode {

namespace {

// LLRs are quantised to steps of 1/8 over +-32, which is finer than any
// receiver's estimate of its own noise.
constexpr float kLlrStep = 0.125F;
constexpr int kLlrBins = 512;
constexpr int kLlrCentre = kLlrBins / 2;

struct MetricTable {
    // [bin][bit]: Fano metric for a received LLR in that bin given the code
    // bit, rate 1/2, scaled to integers.
    std::array<std::array<int, 2>, kLlrBins + 1> m{};
};

const MetricTable& metric_table() {
    static const MetricTable table = [] {
        MetricTable t;
        constexpr double kRate = 0.5;
        for (int bin = 0; bin <= kLlrBins; ++bin) {
            const double llr = static_cast<double>(bin - kLlrCentre) * kLlrStep;
            for (int bit = 0; bit < 2; ++bit) {
                // For equiprobable bits, p(r|b)/p(r) = 2 / (1 + exp(-s L)),
                // with s = +1 for a zero and -1 for a one.
                const double s = bit == 0 ? 1.0 : -1.0;
                const double ratio = 1.0 - std::log2(1.0 + std::exp(-s * llr));
                t.m[static_cast<std::size_t>(bin)][static_cast<std::size_t>(bit)] =
                    static_cast<int>(std::lround((ratio - kRate) * kFanoMetricScale));
            }
        }
        return t;
    }();
    return table;
}

int quantise(float llr) {
    const float q = std::round(llr / kLlrStep);
    const float clamped = std::clamp(q, static_cast<float>(-kLlrCentre), static_cast<float>(kLlrCentre));
    return static_cast<int>(clamped) + kLlrCentre;
}

}  // namespace

Expected<FanoResult> fano_decode(const FanoConfig& config, SoftSpan llr) {
    if (config.generator0 == 0 || config.generator1 == 0) {
        return fail("fano_decode needs two non-zero generator polynomials");
    }
    if (llr.size() % 2 != 0) {
        return fail(std::format("fano_decode got {} LLRs, which is not two per input bit",
                                llr.size()));
    }
    if (config.delta <= 0) {
        return fail(std::format("fano_decode needs a positive threshold step; got {}",
                                config.delta));
    }
    const std::size_t n = llr.size() / 2;
    if (n == 0 || config.tail_bits > n) {
        return fail(std::format("fano_decode got {} input bits and a tail of {}", n,
                                config.tail_bits));
    }

    const MetricTable& table = metric_table();
    // Per depth: the two branch metrics, best first, and the bit each is for.
    struct Node {
        std::uint32_t reg = 0;  // encoder register on arrival
        int metric = 0;         // path metric on arrival
        std::array<int, 2> branch{};
        std::array<std::uint8_t, 2> bit{};
        std::uint8_t branches = 2;
        std::uint8_t tried = 0;  // 0 = on the best branch, 1 = on the second
    };
    std::vector<Node> nodes(n + 1);

    auto expand = [&](std::size_t depth) {
        Node& node = nodes[depth];
        const int b0 = quantise(llr[2 * depth]);
        const int b1 = quantise(llr[2 * depth + 1]);
        std::array<int, 2> m{};
        for (std::uint32_t bit = 0; bit < 2; ++bit) {
            const std::uint32_t reg = (node.reg << 1U) | bit;
            const auto c0 = static_cast<std::size_t>(std::popcount(reg & config.generator0) & 1);
            const auto c1 = static_cast<std::size_t>(std::popcount(reg & config.generator1) & 1);
            m[bit] = table.m[static_cast<std::size_t>(b0)][c0] +
                     table.m[static_cast<std::size_t>(b1)][c1];
        }
        if (depth >= n - config.tail_bits) {
            node.branches = 1;
            node.branch = {m[0], std::numeric_limits<int>::min() / 2};
            node.bit = {0, 1};
        } else if (m[0] >= m[1]) {
            node.branches = 2;
            node.branch = {m[0], m[1]};
            node.bit = {0, 1};
        } else {
            node.branches = 2;
            node.branch = {m[1], m[0]};
            node.bit = {1, 0};
        }
        node.tried = 0;
    };

    const std::size_t budget = config.max_cycles_per_bit * n;
    const int delta = config.delta;
    int threshold = 0;
    std::size_t depth = 0;
    std::size_t cycles = 0;
    expand(0);

    FanoResult result;
    while (cycles < budget) {
        ++cycles;
        Node& node = nodes[depth];
        const int forward = node.metric + node.branch[node.tried];
        if (forward >= threshold) {
            // Move forward. A node is being visited for the first time when
            // its parent sat below threshold + delta; only then is the
            // threshold tightened, or the search would cycle.
            const bool first_visit = node.metric < threshold + delta;
            Node& child = nodes[depth + 1];
            child.reg = (node.reg << 1U) | node.bit[node.tried];
            child.metric = forward;
            ++depth;
            if (first_visit) {
                while (forward >= threshold + delta) {
                    threshold += delta;
                }
            }
            if (depth == n) {
                result.decoded = true;
                break;
            }
            expand(depth);
            continue;
        }

        // Look back for an untried branch whose parent is still above threshold.
        while (true) {
            if (depth == 0 || nodes[depth - 1].metric < threshold) {
                threshold -= delta;
                nodes[depth].tried = 0;
                break;
            }
            --depth;
            Node& parent = nodes[depth];
            if (parent.tried == 0 && parent.branches == 2) {
                parent.tried = 1;
                break;
            }
        }
    }

    result.cycles = cycles;
    if (result.decoded) {
        result.metric = nodes[n].metric;
        result.bits.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            result.bits[i] = static_cast<std::uint8_t>(nodes[i + 1].reg & 1U);
        }
    }
    return result;
}

}  // namespace revenant::decode
