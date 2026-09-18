// The GPU versus CPU reference diff.
//
// The handoff document calls this the single highest-value piece of
// infrastructure in the project and the easiest to skip, and it is right on
// both counts. Float32 recursive filters behave differently across vendors,
// across driver versions, and across the fused-multiply-add decisions a shader
// compiler makes on its own. Without a scalar reference and a diff that runs in
// CI, a numerical bug produces output that looks entirely plausible and is
// quietly wrong, and nobody finds it for months.
//
// Two design decisions here are what make it tell the truth.
//
// It compares bit patterns, not values. An epsilon comparison hides exactly the
// class of bug this exists to catch: a kernel that is off by one unit in the
// last place on one vendor is not "close enough", it is a kernel whose
// arithmetic is not the arithmetic the reference describes, and by the
// twentieth stage of a filter chain that difference is audible. A per-block
// tolerance is available for the cases where bit exactness is genuinely
// unattainable, but it must be asked for explicitly and the block must document
// why.
//
// It reports the seed. A failure that cannot be reproduced on a developer's
// machine from the CI log becomes folklore rather than a bug. Every failure
// prints the seed that produced it and the exact index that diverged, with both
// bit patterns.

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <format>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "core/dsp/types.h"

namespace revenant::test {

struct DiffResult {
    bool identical = true;
    std::size_t compared = 0;

    // Index of the first element that differed, meaningful only when identical
    // is false.
    std::size_t first_divergence = 0;

    double max_absolute_error = 0.0;

    // Largest difference in units in the last place. This is the number that
    // says how bad a divergence is: 1 ULP is a single rounding decision, 2^20
    // ULP is a different algorithm.
    std::uint64_t max_ulp_error = 0;

    std::string report;

    [[nodiscard]] explicit operator bool() const { return identical; }
};

namespace detail {

inline std::uint32_t bits_of(float value) { return std::bit_cast<std::uint32_t>(value); }

// Distance in representable floats. Maps the sign-magnitude float ordering onto
// a monotonic integer ordering first, which is what makes the subtraction
// meaningful across zero.
inline std::uint64_t ulp_distance(float a, float b) {
    if (std::isnan(a) || std::isnan(b)) {
        return (std::isnan(a) && std::isnan(b)) ? 0 : UINT64_MAX;
    }

    auto ordered = [](float v) -> std::int64_t {
        const auto raw = static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(v));
        // Negative floats run downwards in sign-magnitude, so reflect them
        // about the integer origin to get a single increasing sequence.
        return raw < 0 ? static_cast<std::int64_t>(0x80000000LL) - raw : raw;
    };

    const std::int64_t ia = ordered(a);
    const std::int64_t ib = ordered(b);
    return static_cast<std::uint64_t>(ia > ib ? ia - ib : ib - ia);
}

inline std::string describe(std::size_t index, float gpu, float cpu, const char* component) {
    return std::format(
        "  [{}]{} gpu={:.9g} (0x{:08X})  cpu={:.9g} (0x{:08X})  ulp={}\n", index, component, gpu,
        bits_of(gpu), cpu, bits_of(cpu), ulp_distance(gpu, cpu));
}

}  // namespace detail

// Compares two float buffers. tolerance_ulp of 0 demands bit-identical output.
inline DiffResult diff(std::span<const float> gpu,
                       std::span<const float> cpu,
                       std::uint64_t seed,
                       std::uint64_t tolerance_ulp = 0) {
    DiffResult result;

    if (gpu.size() != cpu.size()) {
        result.identical = false;
        result.report = std::format("size mismatch: gpu produced {}, reference produced {}\n",
                                    gpu.size(), cpu.size());
        return result;
    }

    result.compared = gpu.size();
    std::string divergences;
    std::size_t shown = 0;
    constexpr std::size_t kMaxShown = 8;

    for (std::size_t i = 0; i < gpu.size(); ++i) {
        const std::uint64_t ulp = detail::ulp_distance(gpu[i], cpu[i]);
        const double absolute = std::abs(static_cast<double>(gpu[i]) - static_cast<double>(cpu[i]));

        result.max_ulp_error = std::max(result.max_ulp_error, ulp);
        result.max_absolute_error = std::max(result.max_absolute_error, absolute);

        if (ulp > tolerance_ulp) {
            if (result.identical) {
                result.identical = false;
                result.first_divergence = i;
            }
            if (shown < kMaxShown) {
                divergences += detail::describe(i, gpu[i], cpu[i], "");
                ++shown;
            }
        }
    }

    if (!result.identical) {
        result.report = std::format(
            "GPU output diverges from the CPU reference.\n"
            "  seed              {}\n"
            "  elements compared {}\n"
            "  first divergence  index {}\n"
            "  worst error       {} ulp, {:.9g} absolute\n"
            "  tolerance         {} ulp\n"
            "first {} divergent element(s):\n{}"
            "\nReproduce locally with this seed. If the tolerance is genuinely\n"
            "unattainable for this block, raise it deliberately and say why in the\n"
            "block's header comment, per docs/conventions.md.\n",
            seed, result.compared, result.first_divergence, result.max_ulp_error,
            result.max_absolute_error, tolerance_ulp, shown, divergences);
    }

    return result;
}

// Complex overload. Reinterprets as interleaved floats, which is exact:
// std::complex<float> is layout-compatible with float[2] by guarantee.
inline DiffResult diff(std::span<const dsp::Complex32> gpu,
                       std::span<const dsp::Complex32> cpu,
                       std::uint64_t seed,
                       std::uint64_t tolerance_ulp = 0) {
    return diff(std::span<const float>(reinterpret_cast<const float*>(gpu.data()), gpu.size() * 2),
                std::span<const float>(reinterpret_cast<const float*>(cpu.data()), cpu.size() * 2),
                seed, tolerance_ulp);
}

// Extracts the message from a failed Expected or Status, or an empty string
// from a successful one.
//
// This exists because Catch2's INFO builds its argument with operator<<, which
// binds tighter than the conditional operator. Writing
// INFO(x.has_value() ? "" : x.error().message) parses as
// ((stream << x.has_value()) ? "" : ...) and fails to compile, or worse, would
// quietly log the wrong thing if the types happened to line up. A named helper
// removes the trap rather than relying on everyone remembering it.
template <class T>
[[nodiscard]] std::string message_of(const T& result) {
    return result.has_value() ? std::string{} : result.error().message;
}

// Deterministic input generation.
//
// Every case derives its own seed and prints it on failure. std::mt19937_64 is
// specified exactly by the standard, so the same seed produces the same bytes
// on every platform and every library implementation, which a clock-seeded or
// implementation-defined generator would not.
class SeededInput {
public:
    explicit SeededInput(std::uint64_t seed) : seed_(seed), engine_(seed) {}

    [[nodiscard]] std::uint64_t seed() const { return seed_; }

    [[nodiscard]] std::vector<float> reals(std::size_t count, float low = -1.0F,
                                           float high = 1.0F) {
        std::uniform_real_distribution<float> distribution(low, high);
        std::vector<float> out(count);
        for (auto& value : out) {
            value = distribution(engine_);
        }
        return out;
    }

    [[nodiscard]] std::vector<dsp::Complex32> complexes(std::size_t count, float low = -1.0F,
                                                        float high = 1.0F) {
        std::uniform_real_distribution<float> distribution(low, high);
        std::vector<dsp::Complex32> out(count);
        for (auto& value : out) {
            value = dsp::Complex32{distribution(engine_), distribution(engine_)};
        }
        return out;
    }

    // Values chosen to stress rounding rather than to look like a signal:
    // denormals, values near the exponent boundaries, and magnitudes far enough
    // apart that a fused multiply-add gives a visibly different answer from
    // separate operations. Uniform random input in [-1, 1] will not catch a
    // contraction bug; these will.
    [[nodiscard]] std::vector<dsp::Complex32> adversarial_complexes(std::size_t count) {
        static constexpr float kAwkward[] = {
            1.0F,           -1.0F,          0.0F,
            -0.0F,          1.0e-38F,       -1.0e-38F,
            1.0e38F,        -1.0e38F,       1.1920929e-7F,
            0.99999994F,    1.0000001F,     3.14159274F,
            8388608.0F,     8388609.0F,     1.4012984e-45F,
            16777216.0F,
        };
        constexpr std::size_t kAwkwardCount = sizeof(kAwkward) / sizeof(kAwkward[0]);

        std::uniform_int_distribution<std::size_t> pick(0, kAwkwardCount - 1);
        std::vector<dsp::Complex32> out(count);
        for (auto& value : out) {
            value = dsp::Complex32{kAwkward[pick(engine_)], kAwkward[pick(engine_)]};
        }
        return out;
    }

private:
    std::uint64_t seed_;
    std::mt19937_64 engine_;
};

}  // namespace revenant::test
