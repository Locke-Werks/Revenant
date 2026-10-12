// The comb cancellation kernels: core/shaders/comb_project.comp and
// core/shaders/comb_subtract.comp against their twins in
// core/dsp/comb_kernels_reference.cpp, bit for bit, and what the twins do with
// a synthetic comb.
//
// The behaviour cases are CPU only. The conformance cases are what let their
// result stand for the device: the kernels are the twins to the bit.

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <span>
#include <vector>

#include "core/dsp/comb_cancel.h"
#include "core/dsp/types.h"
#include "core/gpu/kernel.h"
#include "core/gpu/shaders.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

namespace {

constexpr std::uint32_t kRing = 1U << 16;
constexpr std::uint32_t kRingMask = kRing - 1U;
constexpr dsp::SampleRate kRate = 2'400'000;

template <class T>
std::span<std::byte> bytes_of(std::vector<T>& values) {
    return std::as_writable_bytes(std::span<T>(values));
}

template <class T>
std::span<const std::byte> const_bytes_of(const std::vector<T>& values) {
    return std::as_bytes(std::span<const T>(values));
}

// A line's exact tone at sample n of the dispatch, synthesised in double from
// the integer phase rather than from the table.
std::complex<double> exact_tone(const dsp::CombLine& line, std::uint32_t n) {
    const std::uint32_t phase = line.phase0 + n * line.step;
    const double angle = 2.0 * std::numbers::pi * static_cast<double>(phase) / 4294967296.0;
    return {std::cos(angle), std::sin(angle)};
}

// An affine comb: lines at c + k d for k in [k_lo, k_lo + count), random
// phases and amplitudes between 0.02 and 0.2.
std::vector<dsp::CombLine> make_comb(std::uint32_t count, std::uint64_t seed) {
    std::mt19937_64 engine(seed);
    std::uniform_int_distribution<std::uint32_t> phase;
    std::uniform_real_distribution<float> magnitude(0.02F, 0.2F);
    std::uniform_real_distribution<float> angle(0.0F, 6.2831853F);
    std::vector<dsp::CombLine> out(count);
    const double c = 1234.5;
    const double d = 4567.25;
    const int k_lo = -static_cast<int>(count / 2U);
    for (std::uint32_t l = 0; l < count; ++l) {
        out[l].phase0 = phase(engine);
        out[l].step = dsp::comb_step(c + (k_lo + static_cast<int>(l)) * d, kRate);
        out[l].amplitude = std::polar(magnitude(engine), angle(engine));
    }
    return out;
}

std::vector<dsp::Complex32> noise(std::size_t length, std::uint64_t seed, float sigma) {
    std::mt19937_64 engine(seed);
    std::normal_distribution<float> gauss(0.0F, sigma);
    std::vector<dsp::Complex32> out(length);
    for (auto& s : out) {
        s = dsp::Complex32{gauss(engine), gauss(engine)};
    }
    return out;
}

void run_project_on_gpu(const dsp::CombParams& params, const std::vector<dsp::Complex32>& raw,
                        const std::vector<dsp::CombLine>& lines,
                        const std::vector<dsp::Complex32>& table,
                        std::vector<dsp::Complex32>& projections, std::uint32_t local) {
    gpu::KernelInvocation invocation;
    invocation.spirv = gpu::shaders::comb_project();
    invocation.inputs = {const_bytes_of(raw), const_bytes_of(lines), const_bytes_of(table)};
    invocation.outputs = {bytes_of(projections)};
    invocation.push_constants = std::as_bytes(std::span<const dsp::CombParams>(&params, 1));
    invocation.invocations = params.lines * params.sub_blocks;
    invocation.local_size_x = local;
    const auto ran = gpu::run_kernel(test::shared_context(), invocation);
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());
}

void run_subtract_on_gpu(const dsp::CombParams& params, const std::vector<dsp::Complex32>& raw,
                         const std::vector<dsp::CombLine>& lines,
                         const std::vector<dsp::Complex32>& table,
                         std::vector<dsp::Complex32>& cleaned,
                         std::vector<dsp::Complex32>& removed, std::uint32_t local) {
    gpu::KernelInvocation invocation;
    invocation.spirv = gpu::shaders::comb_subtract();
    invocation.inputs = {const_bytes_of(raw), const_bytes_of(lines), const_bytes_of(table)};
    invocation.outputs = {bytes_of(cleaned), bytes_of(removed)};
    invocation.push_constants = std::as_bytes(std::span<const dsp::CombParams>(&params, 1));
    invocation.invocations = params.count;
    invocation.local_size_x = local;
    const auto ran = gpu::run_kernel(test::shared_context(), invocation);
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());
}

// A dispatch that starts near the ring's end and crosses it.
dsp::CombParams wrapping_params(std::uint32_t lines, std::uint32_t sub_blocks) {
    dsp::CombParams params;
    params.ring_mask = kRingMask;
    params.first = kRing - 5000U;
    params.count = 3U * 16384U;
    params.lines = lines;
    params.sub_blocks = sub_blocks;
    params.removed_mask = kRingMask;
    return params;
}

}  // namespace

TEST_CASE("the comb project kernel matches its twin bit for bit through a wrap",
          "[gpu][reference][comb]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());
    constexpr std::uint64_t kSeed = 0x434F4D4250ULL;

    const auto table = dsp::comb_table();
    const auto lines = make_comb(dsp::kCombMaxLines, kSeed);
    auto raw = noise(kRing, kSeed + 1U, 0.05F);

    for (const std::uint32_t sub_blocks : {1U, 16U}) {
        // 48 * 1024 / 16 = 3072, a multiple of the 64 sample chunk, and
        // 48 * 1024 - 48 = 49104 / 16 = 3069, which leaves a short last chunk.
        for (const std::uint32_t count : {49152U, 49104U}) {
            dsp::CombParams params = wrapping_params(dsp::kCombMaxLines, sub_blocks);
            params.count = count;
            std::vector<dsp::Complex32> expect(
                static_cast<std::size_t>(params.lines) * sub_blocks);
            REQUIRE(dsp::reference_comb_project(params, raw, lines, table, expect).has_value());
            for (const std::uint32_t local : {1U, 64U, 256U}) {
                if (local > test::shared_context().info().max_workgroup_size_x) {
                    continue;
                }
                INFO("sub_blocks " << sub_blocks << ", count " << count << ", local_size_x "
                                   << local);
                std::vector<dsp::Complex32> got(expect.size());
                run_project_on_gpu(params, raw, lines, table, got, local);
                const auto d = test::diff(got, expect, kSeed);
                INFO(d.report);
                CHECK(d.identical);
            }
        }
    }
}

TEST_CASE("the comb subtract kernel matches its twin bit for bit through a wrap",
          "[gpu][reference][comb]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());
    constexpr std::uint64_t kSeed = 0x535542ULL;

    const auto table = dsp::comb_table();
    const auto lines = make_comb(dsp::kCombMaxLines, kSeed);
    auto raw = noise(kRing, kSeed + 1U, 0.05F);
    // A few negative zeros, which the no-line case must carry through.
    raw[kRing - 4000U] = dsp::Complex32{-0.0F, -0.0F};
    raw[100U] = dsp::Complex32{-0.0F, 0.3F};

    // The removed ring is smaller than the sample ring, as the engine sizes it.
    const std::uint32_t removed_mask = (1U << 15) - 1U;

    for (const std::uint32_t count_lines : {0U, 1U, 37U, dsp::kCombMaxLines}) {
        dsp::CombParams params = wrapping_params(count_lines, 1);
        params.removed_mask = removed_mask;
        params.count = 24576U;
        std::vector<dsp::Complex32> expect_cleaned(kRing);
        std::vector<dsp::Complex32> expect_removed(static_cast<std::size_t>(removed_mask) + 1U);
        REQUIRE(dsp::reference_comb_subtract(params, raw, lines, table, expect_cleaned,
                                             expect_removed)
                    .has_value());
        for (const std::uint32_t local : {1U, 64U, 256U}) {
            if (local > test::shared_context().info().max_workgroup_size_x) {
                continue;
            }
            INFO("lines " << count_lines << ", local_size_x " << local);
            std::vector<dsp::Complex32> cleaned(kRing);
            std::vector<dsp::Complex32> removed(expect_removed.size());
            run_subtract_on_gpu(params, raw, lines, table, cleaned, removed, local);
            {
                const auto d = test::diff(cleaned, expect_cleaned, kSeed);
                INFO("cleaned\n" << d.report);
                CHECK(d.identical);
            }
            {
                const auto d = test::diff(removed, expect_removed, kSeed);
                INFO("removed\n" << d.report);
                CHECK(d.identical);
            }
        }
    }
}

TEST_CASE("comb_step rounds to the nearest step and wraps a negative frequency",
          "[reference][comb]") {
    CHECK(dsp::comb_step(0.0, kRate) == 0U);
    CHECK(dsp::comb_step(600'000.0, kRate) == 1U << 30);
    CHECK(dsp::comb_step(-600'000.0, kRate) == 3U << 30);
    CHECK(dsp::comb_step(1'200'000.0, kRate) == 1U << 31);
    CHECK(dsp::comb_step(-1'200'000.0, kRate) == 1U << 31);
    // One step is rate / 2^32, 0.000559 Hz at 2.4 MS/s.
    CHECK(dsp::comb_step(0.0006, kRate) == 1U);
    CHECK(dsp::comb_step(-0.0006, kRate) == 0xFFFFFFFFU);
    CHECK(dsp::comb_step(1000.0, 0) == 0U);
}

TEST_CASE("the comb table is a unit circle at 2^12 phases", "[reference][comb]") {
    const auto table = dsp::comb_table();
    REQUIRE(table.size() == dsp::kCombTableSize);
    CHECK(table[0] == dsp::Complex32{1.0F, 0.0F});
    CHECK(table[1024].real() == static_cast<float>(std::cos(std::numbers::pi / 2.0)));
    CHECK(table[1024].imag() == 1.0F);
    CHECK(table[2048].real() == -1.0F);
    for (const auto& t : table) {
        CHECK(std::abs(std::norm(t) - 1.0F) < 1e-6F);
    }
}

TEST_CASE("projecting a pure tone at a line's step returns amplitude times the sub-block",
          "[reference][comb]") {
    const auto table = dsp::comb_table();
    dsp::CombLine line;
    line.phase0 = 0x12345678U;
    line.step = dsp::comb_step(31'415.9, kRate);
    const std::complex<double> amplitude = std::polar(0.4, 1.1);

    dsp::CombParams params = wrapping_params(1, 16);
    std::vector<dsp::Complex32> raw(kRing);
    for (std::uint32_t n = 0; n < params.count; ++n) {
        const std::complex<double> s = amplitude * exact_tone(line, n);
        raw[(params.first + n) & kRingMask] =
            dsp::Complex32{static_cast<float>(s.real()), static_cast<float>(s.imag())};
    }
    std::vector<dsp::Complex32> projections(params.sub_blocks);
    const std::vector<dsp::CombLine> lines{line};
    REQUIRE(dsp::reference_comb_project(params, raw, lines, table, projections).has_value());

    const double length = static_cast<double>(params.count / params.sub_blocks);
    // The table index truncates the phase, so the table's tone lags the true
    // one by an error uniform over [0, 2 pi / 4096): on average half a step,
    // pi / 4096 rad, which the projection carries as a rotation. Its spread
    // costs sinc(1 / 4096) of the magnitude, 1e-7.
    const std::complex<double> want =
        amplitude * length * std::polar(1.0, std::numbers::pi / dsp::kCombTableSize);
    for (std::uint32_t s = 0; s < params.sub_blocks; ++s) {
        const std::complex<double> got(projections[s].real(), projections[s].imag());
        INFO("sub-block " << s << ": got " << got << ", want " << want);
        CHECK(std::abs(got - want) < 2e-4 * std::abs(want));
    }
}

TEST_CASE("subtracting a comb with its true amplitudes leaves the table quantisation floor",
          "[reference][comb]") {
    const auto table = dsp::comb_table();
    const auto lines = make_comb(dsp::kCombMaxLines, 0xF100ULL);
    dsp::CombParams params = wrapping_params(dsp::kCombMaxLines, 1);
    params.count = 16384;

    std::vector<dsp::Complex32> raw(kRing);
    double comb_power = 0.0;
    for (std::uint32_t n = 0; n < params.count; ++n) {
        std::complex<double> s{};
        for (const auto& line : lines) {
            const std::complex<double> a(line.amplitude.real(), line.amplitude.imag());
            s += a * exact_tone(line, n);
        }
        comb_power += std::norm(s);
        raw[(params.first + n) & kRingMask] =
            dsp::Complex32{static_cast<float>(s.real()), static_cast<float>(s.imag())};
    }

    const auto residual_db = [&](const std::vector<dsp::CombLine>& with) {
        std::vector<dsp::Complex32> cleaned(kRing);
        std::vector<dsp::Complex32> removed(kRing);
        REQUIRE(dsp::reference_comb_subtract(params, raw, with, table, cleaned, removed)
                    .has_value());
        double residual_power = 0.0;
        for (std::uint32_t n = 0; n < params.count; ++n) {
            const std::uint32_t slot = (params.first + n) & kRingMask;
            residual_power += std::norm(
                std::complex<double>(cleaned[slot].real(), cleaned[slot].imag()));
        }
        return 10.0 * std::log10(residual_power / comb_power);
    };

    // The true amplitudes. The truncating table index lags each tone by an
    // error uniform over [0, 2 pi / 4096): a mean of pi / 4096 rad, which
    // alone is -62.3 dB, plus the spread around it.
    const double true_db = residual_db(lines);

    // The amplitudes the tracker holds, which come from projections through
    // the same table and so carry the mean lag as a rotation by pi / 4096.
    // What is left is the spread, uniform over +/- pi / 4096 with an rms of
    // pi / (4096 sqrt 3): -67.1 dB.
    auto rotated = lines;
    const dsp::Complex32 lag = std::polar(1.0F, static_cast<float>(std::numbers::pi /
                                                                    dsp::kCombTableSize));
    for (auto& line : rotated) {
        line.amplitude = line.amplitude * lag;
    }
    const double tracked_db = residual_db(rotated);

    INFO("true amplitudes " << true_db << " dB, tracked amplitudes " << tracked_db << " dB");
    WARN("comb subtract residual: " << true_db << " dB with the true amplitudes, " << tracked_db
                                    << " dB with the amplitudes a projection measures");
    CHECK(true_db < -60.0);
    CHECK(tracked_db < -66.0);
}

TEST_CASE("subtracting no lines leaves the ring alone and removes negative zero",
          "[reference][comb]") {
    const auto table = dsp::comb_table();
    dsp::CombParams params = wrapping_params(0, 1);
    auto raw = noise(kRing, 7U, 0.1F);
    raw[(params.first + 3U) & kRingMask] = dsp::Complex32{-0.0F, -0.0F};
    std::vector<dsp::Complex32> cleaned(kRing);
    std::vector<dsp::Complex32> removed(kRing);
    REQUIRE(dsp::reference_comb_subtract(params, raw, {}, table, cleaned, removed).has_value());
    for (std::uint32_t n = 0; n < params.count; ++n) {
        const std::uint32_t slot = (params.first + n) & kRingMask;
        REQUIRE(std::bit_cast<std::uint64_t>(cleaned[slot]) ==
                std::bit_cast<std::uint64_t>(raw[slot]));
        REQUIRE(std::bit_cast<std::uint64_t>(removed[slot]) == 0x8000000080000000ULL);
        const dsp::Complex32 back{cleaned[slot].real() + removed[slot].real(),
                                  cleaned[slot].imag() + removed[slot].imag()};
        REQUIRE(std::bit_cast<std::uint64_t>(back) == std::bit_cast<std::uint64_t>(raw[slot]));
    }
}

TEST_CASE("a subtract across the ring's end matches the same subtract unwrapped",
          "[reference][comb]") {
    const auto table = dsp::comb_table();
    const auto lines = make_comb(40, 99U);
    const auto source = noise(kRing, 11U, 0.05F);

    dsp::CombParams wrapped = wrapping_params(40, 1);
    dsp::CombParams flat = wrapped;
    flat.first = 0;

    std::vector<dsp::Complex32> raw_wrapped(kRing);
    std::vector<dsp::Complex32> raw_flat(kRing);
    for (std::uint32_t n = 0; n < wrapped.count; ++n) {
        raw_wrapped[(wrapped.first + n) & kRingMask] = source[n];
        raw_flat[n] = source[n];
    }
    std::vector<dsp::Complex32> cleaned_wrapped(kRing);
    std::vector<dsp::Complex32> removed_wrapped(kRing);
    std::vector<dsp::Complex32> cleaned_flat(kRing);
    std::vector<dsp::Complex32> removed_flat(kRing);
    REQUIRE(dsp::reference_comb_subtract(wrapped, raw_wrapped, lines, table, cleaned_wrapped,
                                         removed_wrapped)
                .has_value());
    REQUIRE(dsp::reference_comb_subtract(flat, raw_flat, lines, table, cleaned_flat,
                                         removed_flat)
                .has_value());
    for (std::uint32_t n = 0; n < wrapped.count; ++n) {
        const std::uint32_t slot = (wrapped.first + n) & kRingMask;
        REQUIRE(std::bit_cast<std::uint64_t>(cleaned_wrapped[slot]) ==
                std::bit_cast<std::uint64_t>(cleaned_flat[n]));
        REQUIRE(std::bit_cast<std::uint64_t>(removed_wrapped[slot]) ==
                std::bit_cast<std::uint64_t>(removed_flat[n]));
    }
}

TEST_CASE("validate rejects comb parameters the kernels cannot run", "[reference][comb]") {
    const dsp::CombParams good = wrapping_params(8, 16);
    CHECK(dsp::validate(good).has_value());

    auto bad = good;
    bad.ring_mask = 1000U;
    CHECK_FALSE(dsp::validate(bad).has_value());

    bad = good;
    bad.removed_mask = 1000U;
    CHECK_FALSE(dsp::validate(bad).has_value());

    bad = good;
    bad.removed_mask = (kRing * 2U) - 1U;
    CHECK_FALSE(dsp::validate(bad).has_value());

    bad = good;
    bad.lines = dsp::kCombMaxLines + 1U;
    CHECK_FALSE(dsp::validate(bad).has_value());

    bad = good;
    bad.sub_blocks = 0;
    CHECK_FALSE(dsp::validate(bad).has_value());

    bad = good;
    bad.sub_blocks = dsp::kCombMaxSubBlocks + 1U;
    CHECK_FALSE(dsp::validate(bad).has_value());

    bad = good;
    bad.count = good.count + 1U;
    CHECK_FALSE(dsp::validate(bad).has_value());

    bad = good;
    bad.count = 0;
    CHECK_FALSE(dsp::validate(bad).has_value());

    bad = good;
    bad.removed_mask = 1023U;
    CHECK_FALSE(dsp::validate(bad).has_value());

    // And the twins refuse what validate refuses, and a short table.
    const auto table = dsp::comb_table();
    std::vector<dsp::Complex32> ring(kRing);
    std::vector<dsp::Complex32> projections(8U * 16U);
    const std::vector<dsp::CombLine> lines(8);
    bad = good;
    bad.lines = dsp::kCombMaxLines + 1U;
    CHECK_FALSE(dsp::reference_comb_project(bad, ring, lines, table, projections).has_value());
    CHECK_FALSE(dsp::reference_comb_project(good, ring, lines,
                                            std::span(table).first(100), projections)
                    .has_value());
    CHECK_FALSE(dsp::reference_comb_subtract(good, ring, std::span(lines).first(4), table, ring,
                                             ring)
                    .has_value());
}
