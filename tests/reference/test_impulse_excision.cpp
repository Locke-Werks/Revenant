// The span-wide impulse excision: core/shaders/impulse_excise.comp against
// its twins in core/dsp/impulse_excision_reference.cpp, bit for bit, and the
// stage's behaviour on synthetic captures run through the twins block by
// block exactly as the graph drives the kernels.
//
// The behaviour cases are CPU only. The conformance case is what lets their
// result stand for the device: the kernel is the twin to the bit, so a
// behaviour measured on one is the behaviour of the other.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <span>
#include <vector>

#include "core/dsp/impulse_excision.h"
#include "core/dsp/types.h"
#include "core/gpu/kernel.h"
#include "core/gpu/shaders.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

namespace {

template <class T>
std::span<std::byte> bytes_of(std::vector<T>& values) {
    return std::as_writable_bytes(std::span<T>(values));
}

template <class T>
std::span<const std::byte> const_bytes_of(const std::vector<T>& values) {
    return std::as_bytes(std::span<const T>(values));
}

struct Rings {
    std::vector<dsp::Complex32> raw;
    std::vector<float> power;
    std::vector<float> level;
    std::vector<std::uint32_t> flags;
    std::vector<dsp::Complex32> cleaned;
    std::vector<dsp::Complex32> removed;
    std::vector<std::uint32_t> counters;
};

Rings make_rings(std::uint32_t ring_mask, const dsp::ExcisionRings& sizes) {
    Rings out;
    out.raw.assign(static_cast<std::size_t>(ring_mask) + 1U, dsp::Complex32{});
    out.power.assign(static_cast<std::size_t>(sizes.seg_mask) + 1U, 0.0F);
    out.level.assign(static_cast<std::size_t>(sizes.seg_mask) + 1U, 0.0F);
    out.flags.assign(static_cast<std::size_t>(sizes.flag_mask) + 1U, 0U);
    out.cleaned.assign(static_cast<std::size_t>(ring_mask) + 1U, dsp::Complex32{});
    out.removed.assign(static_cast<std::size_t>(sizes.removed_mask) + 1U, dsp::Complex32{});
    out.counters.assign(dsp::kExciseCounters, 0U);
    return out;
}

// One pass on the device. Every binding is handed over, inputs first, in the
// kernel's binding order, so each pass gets the rings it reads as inputs and
// the rest as outputs, which run_kernel zeroes.
void run_pass_on_gpu(dsp::ExcisionConfig config, std::uint32_t pass,
                     const dsp::ExcisionParams& params, Rings& rings, std::uint32_t local) {
    config.pass = pass;
    gpu::KernelInvocation invocation;
    invocation.spirv = gpu::shaders::impulse_excise();
    switch (pass) {
    case dsp::kExciseSegment:
        invocation.inputs = {const_bytes_of(rings.raw)};
        invocation.outputs = {bytes_of(rings.power), bytes_of(rings.level),
                              bytes_of(rings.flags),  bytes_of(rings.cleaned),
                              bytes_of(rings.removed), bytes_of(rings.counters)};
        break;
    case dsp::kExciseReference:
        invocation.inputs = {const_bytes_of(rings.raw), const_bytes_of(rings.power)};
        invocation.outputs = {bytes_of(rings.level), bytes_of(rings.flags),
                              bytes_of(rings.cleaned), bytes_of(rings.removed),
                              bytes_of(rings.counters)};
        break;
    case dsp::kExciseFlag:
        invocation.inputs = {const_bytes_of(rings.raw), const_bytes_of(rings.power),
                             const_bytes_of(rings.level)};
        invocation.outputs = {bytes_of(rings.flags), bytes_of(rings.cleaned),
                              bytes_of(rings.removed), bytes_of(rings.counters)};
        break;
    default:
        invocation.inputs = {const_bytes_of(rings.raw), const_bytes_of(rings.power),
                             const_bytes_of(rings.level), const_bytes_of(rings.flags)};
        invocation.outputs = {bytes_of(rings.cleaned), bytes_of(rings.removed),
                              bytes_of(rings.counters)};
        break;
    }
    invocation.push_constants = std::as_bytes(std::span<const dsp::ExcisionParams>(&params, 1));
    invocation.invocations = params.count;
    invocation.local_size_x = local;
    invocation.grid_constants = {pass,          config.seg_log2, config.segments,
                                 config.max_width, config.hang,  config.lead};
    const auto ran = gpu::run_kernel(test::shared_context(), invocation);
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());
}

// Complex Gaussian noise, a tone, and impulses of several heights and widths,
// some of them in clusters so the scan crosses a run boundary.
std::vector<dsp::Complex32> impulsive_capture(std::size_t length, std::uint64_t seed,
                                              float tone_amplitude, double tone_cycles,
                                              std::vector<std::size_t>* impulses,
                                              std::uint32_t impulse_count,
                                              std::uint32_t max_impulse_width) {
    std::mt19937_64 engine(seed);
    std::normal_distribution<float> gauss(0.0F, 0.01F);
    std::vector<dsp::Complex32> out(length);
    for (std::size_t n = 0; n < length; ++n) {
        const double phase = 2.0 * std::numbers::pi * tone_cycles * static_cast<double>(n);
        out[n] = dsp::Complex32{gauss(engine), gauss(engine)} +
                 tone_amplitude * dsp::Complex32{static_cast<float>(std::cos(phase)),
                                                 static_cast<float>(std::sin(phase))};
    }
    std::uniform_int_distribution<std::size_t> where(length / 8, length - 1024);
    for (std::uint32_t k = 0; k < impulse_count; ++k) {
        const std::size_t at = where(engine);
        const std::uint32_t width = 1U + k % max_impulse_width;
        const float height = 1.0F + 0.5F * static_cast<float>(k % 5);
        for (std::uint32_t w = 0; w < width; ++w) {
            out[at + w] += dsp::Complex32{height, -0.7F * height};
        }
        if (impulses != nullptr) {
            impulses->push_back(at);
        }
    }
    return out;
}

// A whole capture through the twins, block by block, the way the graph runs
// the kernels: the ring is the capture, cleaned in place.
struct StreamResult {
    std::vector<dsp::Complex32> cleaned;
    std::vector<dsp::Complex32> removed;
    std::uint32_t removed_mask = 0;
    std::array<std::uint64_t, dsp::kExciseCounters> counters{};
    dsp::SampleIndex cleaned_end = 0;
};

StreamResult run_stream(const std::vector<dsp::Complex32>& capture, dsp::SampleRate rate,
                        std::uint32_t block, double threshold_db) {
    const dsp::ExcisionConfig config = dsp::design_excision(rate);
    const auto ring = static_cast<std::uint32_t>(std::bit_ceil(capture.size()));
    const std::uint32_t ring_mask = ring - 1U;
    const auto sizes = dsp::size_excision_rings(config, block, 1, ring_mask);
    REQUIRE(sizes.has_value());

    Rings rings = make_rings(ring_mask, *sizes);
    std::copy(capture.begin(), capture.end(), rings.raw.begin());
    StreamResult out;
    out.removed.assign(capture.size(), dsp::Complex32{-0.0F, -0.0F});
    out.removed_mask = sizes->removed_mask;

    dsp::ExcisionCursor cursor(config, ring_mask, *sizes, dsp::excision_threshold(threshold_db));
    for (std::size_t start = 0; start < capture.size(); start += block) {
        const auto count =
            static_cast<std::uint32_t>(std::min<std::size_t>(block, capture.size() - start));
        const dsp::ExcisionPlan plan = cursor.plan(start, count);
        dsp::ExcisionConfig pass = config;
        if (plan.segment.count > 0) {
            pass.pass = dsp::kExciseSegment;
            REQUIRE(dsp::reference_excise_segment(pass, plan.segment, rings.raw, rings.power)
                        .has_value());
        }
        if (plan.reference.count > 0) {
            pass.pass = dsp::kExciseReference;
            REQUIRE(dsp::reference_excise_reference(pass, plan.reference, rings.power,
                                                    rings.level)
                        .has_value());
        }
        if (plan.flag.count > 0) {
            pass.pass = dsp::kExciseFlag;
            REQUIRE(dsp::reference_excise_flag(pass, plan.flag, rings.raw, rings.level,
                                               rings.flags)
                        .has_value());
        }
        if (plan.apply.count > 0) {
            pass.pass = dsp::kExciseApply;
            std::fill(rings.counters.begin(), rings.counters.end(), 0U);
            // In place, as the engine binds it.
            REQUIRE(dsp::reference_excise_apply(pass, plan.apply, rings.raw, rings.flags,
                                                rings.raw, rings.removed, rings.counters)
                        .has_value());
            for (std::uint32_t c = 0; c < dsp::kExciseCounters; ++c) {
                out.counters[c] += rings.counters[c];
            }
            // Out of the short removed ring before the next block reuses it,
            // which is the tap the stage is designed to grow.
            const dsp::SampleIndex from = plan.cleaned_end - plan.apply.count;
            for (dsp::SampleIndex i = from; i < plan.cleaned_end; ++i) {
                out.removed[i] = rings.removed[i & sizes->removed_mask];
            }
        }
        out.cleaned_end = plan.cleaned_end;
    }
    out.cleaned.assign(rings.raw.begin(),
                       rings.raw.begin() + static_cast<std::ptrdiff_t>(capture.size()));
    return out;
}

// The tone's power as a correlation, which is blind to the noise and to what
// excision did to it.
double tone_power(std::span<const dsp::Complex32> samples, double cycles) {
    std::complex<double> acc{};
    for (std::size_t n = 0; n < samples.size(); ++n) {
        const double phase = -2.0 * std::numbers::pi * cycles * static_cast<double>(n);
        acc += std::complex<double>(samples[n]) * std::polar(1.0, phase);
    }
    return std::norm(acc / static_cast<double>(samples.size()));
}

}  // namespace

TEST_CASE("the excision kernel's four passes match their twins bit for bit through a wrap",
          "[gpu][reference][excision]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x455843495345ULL;
    constexpr std::uint32_t kRing = 1U << 15;
    constexpr std::uint32_t kRingMask = kRing - 1U;
    constexpr dsp::SampleRate kRate = 2'400'000;
    constexpr std::uint32_t kBlock = 4096;

    const dsp::ExcisionConfig config = dsp::design_excision(kRate);
    INFO("segment " << config.segment_samples() << ", segments " << config.segments
                    << ", max width " << config.max_width << ", hang " << config.hang);
    const auto sizes = dsp::size_excision_rings(config, kBlock, 1, kRingMask);
    REQUIRE(sizes.has_value());

    std::vector<std::size_t> impulses;
    Rings cpu = make_rings(kRingMask, *sizes);
    const auto capture = impulsive_capture(kRing, kSeed, 0.05F, 0.0123, &impulses, 120, 6);
    std::copy(capture.begin(), capture.end(), cpu.raw.begin());

    // Three blocks through the twins to give every side ring a stream's worth
    // of state, then a fourth that crosses the ring's end, compared pass by
    // pass on both sides from identical inputs.
    dsp::ExcisionCursor cursor(config, kRingMask, *sizes, dsp::excision_threshold(15.0));
    const std::uint64_t start = kRing / 2U + 1000U;
    for (std::uint64_t at = start; at < start + 3U * kBlock; at += kBlock) {
        const dsp::ExcisionPlan warm = cursor.plan(at, kBlock);
        dsp::ExcisionConfig pass = config;
        if (warm.segment.count > 0) {
            pass.pass = dsp::kExciseSegment;
            REQUIRE(dsp::reference_excise_segment(pass, warm.segment, cpu.raw, cpu.power)
                        .has_value());
        }
        pass.pass = dsp::kExciseReference;
        REQUIRE(dsp::reference_excise_reference(pass, warm.reference, cpu.power, cpu.level)
                    .has_value());
        pass.pass = dsp::kExciseFlag;
        REQUIRE(dsp::reference_excise_flag(pass, warm.flag, cpu.raw, cpu.level, cpu.flags)
                    .has_value());
    }
    const dsp::ExcisionPlan plan = cursor.plan(start + 3U * kBlock, kBlock);
    REQUIRE(plan.segment.count > 0);
    REQUIRE(plan.apply.count > 0);
    REQUIRE(plan.flag.first + plan.flag.count > kRing);

    for (const std::uint32_t local : {1U, 32U, 64U, 256U}) {
        if (local > test::shared_context().info().max_workgroup_size_x) {
            continue;
        }
        INFO("local_size_x = " << local);
        Rings state = cpu;
        dsp::ExcisionConfig pass = config;

        pass.pass = dsp::kExciseSegment;
        Rings expect = make_rings(kRingMask, *sizes);
        REQUIRE(dsp::reference_excise_segment(pass, plan.segment, state.raw, expect.power)
                    .has_value());
        Rings got = state;
        run_pass_on_gpu(config, dsp::kExciseSegment, plan.segment, got, local);
        {
            const auto d = test::diff(got.power, expect.power, kSeed);
            INFO("segment\n" << d.report);
            CHECK(d.identical);
        }
        REQUIRE(dsp::reference_excise_segment(pass, plan.segment, state.raw, state.power)
                    .has_value());

        pass.pass = dsp::kExciseReference;
        expect = make_rings(kRingMask, *sizes);
        REQUIRE(dsp::reference_excise_reference(pass, plan.reference, state.power, expect.level)
                    .has_value());
        got = state;
        run_pass_on_gpu(config, dsp::kExciseReference, plan.reference, got, local);
        {
            const auto d = test::diff(got.level, expect.level, kSeed);
            INFO("reference\n" << d.report);
            CHECK(d.identical);
        }
        CHECK(std::ranges::any_of(expect.level, [](float v) { return v > 0.0F; }));
        REQUIRE(dsp::reference_excise_reference(pass, plan.reference, state.power, state.level)
                    .has_value());

        pass.pass = dsp::kExciseFlag;
        expect = make_rings(kRingMask, *sizes);
        REQUIRE(dsp::reference_excise_flag(pass, plan.flag, state.raw, state.level, expect.flags)
                    .has_value());
        got = state;
        run_pass_on_gpu(config, dsp::kExciseFlag, plan.flag, got, local);
        CHECK(got.flags == expect.flags);
        REQUIRE(dsp::reference_excise_flag(pass, plan.flag, state.raw, state.level, state.flags)
                    .has_value());

        // Over flags carried from earlier blocks, so the scan reaches back
        // across a block boundary as it does in the engine.
        pass.pass = dsp::kExciseApply;
        expect = make_rings(kRingMask, *sizes);
        REQUIRE(dsp::reference_excise_apply(pass, plan.apply, state.raw, state.flags,
                                            expect.cleaned, expect.removed, expect.counters)
                    .has_value());
        got = state;
        run_pass_on_gpu(config, dsp::kExciseApply, plan.apply, got, local);
        {
            const auto cleaned = test::diff(got.cleaned, expect.cleaned, kSeed);
            INFO("cleaned\n" << cleaned.report);
            CHECK(cleaned.identical);
            const auto removed = test::diff(got.removed, expect.removed, kSeed);
            INFO("removed\n" << removed.report);
            CHECK(removed.identical);
        }
        INFO("excised " << expect.counters[0] << " samples in " << expect.counters[1]
                        << " events, spared " << expect.counters[2]);
        CHECK(got.counters == expect.counters);
        CHECK(expect.counters[dsp::kExciseCounterEvents] > 0U);
    }
}

TEST_CASE("excision removes impulses, keeps a tone to 0.1 dB and moves no sample",
          "[reference][excision]") {
    constexpr std::uint64_t kSeed = 20261009;
    constexpr dsp::SampleRate kRate = 2'400'000;
    constexpr std::size_t kLength = 1U << 19;
    constexpr double kCycles = 0.0371;
    constexpr std::uint32_t kImpulses = 40;
    INFO("seed " << kSeed);

    std::vector<std::size_t> where;
    const auto dirty = impulsive_capture(kLength, kSeed, 0.1F, kCycles, &where, kImpulses, 4);
    const auto clean = impulsive_capture(kLength, kSeed, 0.1F, kCycles, nullptr, 0, 1);

    const StreamResult result = run_stream(dirty, kRate, 65536, 15.0);
    REQUIRE(result.cleaned.size() == dirty.size());
    REQUIRE(result.cleaned_end > kLength - 1024U);
    const auto span = static_cast<std::size_t>(result.cleaned_end);

    // Every impulse found, each one its own event.
    INFO("excised " << result.counters[0] << " samples in " << result.counters[1]
                    << " events, spared " << result.counters[2]);
    CHECK(result.counters[dsp::kExciseCounterEvents] >= kImpulses - 2U);
    CHECK(result.counters[dsp::kExciseCounterEvents] <= kImpulses);
    CHECK(result.counters[dsp::kExciseCounterSpared] == 0U);

    // Nothing of any impulse is left: the largest magnitude near each one is
    // the tone and the noise, 0.1 and a few hundredths, against impulses of
    // 1.2 and up.
    float peak = 0.0F;
    for (const std::size_t at : where) {
        for (std::size_t i = at - 8; i < at + 16; ++i) {
            peak = std::max(peak, std::abs(result.cleaned[i]));
        }
    }
    INFO("largest magnitude left near an impulse " << peak);
    CHECK(peak < 0.25F);

    // The tone, over the span the stage finished.
    const double reference = tone_power(std::span(clean).first(span), kCycles);
    const double kept = tone_power(std::span(result.cleaned).first(span), kCycles);
    const double change_db = 10.0 * std::log10(kept / reference);
    INFO("tone change " << change_db << " dB");
    CHECK(std::abs(change_db) < 0.1);

    // Raw plus removed is the capture, to the bit, everywhere it finished.
    std::size_t mismatched = 0;
    for (std::size_t i = 0; i < span; ++i) {
        const dsp::Complex32 sum{result.cleaned[i].real() + result.removed[i].real(),
                                 result.cleaned[i].imag() + result.removed[i].imag()};
        if (std::bit_cast<std::uint64_t>(sum) != std::bit_cast<std::uint64_t>(dirty[i])) {
            ++mismatched;
        }
    }
    CHECK(mismatched == 0U);

    // Every sample that was not excised is exactly what came in: no shift,
    // no rescale.
    std::size_t moved = 0;
    for (std::size_t i = 0; i < kLength; ++i) {
        const bool excised = result.cleaned[i] == dsp::Complex32{} &&
                             std::signbit(result.cleaned[i].real());
        if (!excised && result.cleaned[i] != dirty[i]) {
            ++moved;
        }
    }
    CHECK(moved == 0U);
}

TEST_CASE("excision leaves a clean strong carrier alone", "[reference][excision]") {
    constexpr std::uint64_t kSeed = 20261010;
    constexpr dsp::SampleRate kRate = 2'400'000;
    constexpr std::size_t kLength = 1U << 18;

    // A carrier 40 dB over the noise, which is every sample standing far over
    // a reference that does not include it. The median does include it.
    std::mt19937_64 engine(kSeed);
    std::normal_distribution<float> gauss(0.0F, 0.005F);
    std::vector<dsp::Complex32> capture(kLength);
    for (std::size_t n = 0; n < kLength; ++n) {
        const double phase = 2.0 * std::numbers::pi * 0.11 * static_cast<double>(n);
        capture[n] = dsp::Complex32{gauss(engine), gauss(engine)} +
                     0.7F * dsp::Complex32{static_cast<float>(std::cos(phase)),
                                           static_cast<float>(std::sin(phase))};
    }
    const StreamResult result = run_stream(capture, kRate, 32768, 15.0);
    INFO("seed " << kSeed << ", excised " << result.counters[0]);
    CHECK(result.counters[dsp::kExciseCounterSamples] == 0U);
    CHECK(result.counters[dsp::kExciseCounterSpared] == 0U);
    CHECK(result.cleaned == capture);
}

TEST_CASE("excision leaves a burst longer than the maximum width untouched",
          "[reference][excision]") {
    constexpr std::uint64_t kSeed = 20261011;
    constexpr dsp::SampleRate kRate = 2'400'000;
    constexpr std::size_t kLength = 1U << 18;

    const dsp::ExcisionConfig config = dsp::design_excision(kRate);

    std::vector<dsp::Complex32> capture =
        impulsive_capture(kLength, kSeed, 0.0F, 0.0, nullptr, 0, 1);

    // A transmission 30 dB over the noise, three maximum widths long, and a
    // second one a single sample past the maximum.
    std::mt19937_64 engine(kSeed);
    std::normal_distribution<float> gauss(0.0F, 0.3F);
    struct Burst {
        std::size_t at;
        std::size_t length;
    };
    const Burst bursts[] = {{100'000, 3U * config.max_width},
                            {180'000, config.max_width + 1U + 2U * config.hang}};
    for (const Burst& b : bursts) {
        for (std::size_t n = 0; n < b.length; ++n) {
            capture[b.at + n] += dsp::Complex32{gauss(engine), gauss(engine)};
        }
    }
    const StreamResult result = run_stream(capture, kRate, 65536, 15.0);
    INFO("seed " << kSeed << ", max width " << config.max_width << ", excised "
                 << result.counters[0] << " in " << result.counters[1] << ", spared "
                 << result.counters[2]);

    for (const Burst& b : bursts) {
        const std::size_t lo = b.at - 64;
        const std::size_t hi = b.at + b.length + 64;
        bool untouched = true;
        for (std::size_t i = lo; i < hi; ++i) {
            untouched = untouched && result.cleaned[i] == capture[i];
        }
        INFO("burst at " << b.at << ", " << b.length << " samples");
        CHECK(untouched);
    }
    CHECK(result.counters[dsp::kExciseCounterSpared] >= 2U);
}
