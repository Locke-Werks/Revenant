// The three noise mitigation kernels against their CPU twins, bit-exactly,
// and the designs they run against their stated figures.
//
// core/shaders/noise_blank.comp, noise_line.comp and noise_spectral.comp, twins
// in core/dsp/noise_reference.cpp. The two stateful kernels are driven over a
// sequence of dispatches of awkward lengths from the same starting state, and
// the audio of every dispatch and the state after the last are compared,
// which is the claim that matters: a kernel that agreed on one dispatch and
// drifted across a boundary would be a receiver whose notch clicks every
// block.
//
// The design cases need no device. They measure the notch the design
// promises, the spectral stage's reconstruction at unit gain, its noise floor
// against the true floor, and the automatic notch's cancellation of a steady
// tone, on the twins.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <span>
#include <vector>

#include "core/dsp/noise_reference.h"
#include "core/dsp/types.h"
#include "core/gpu/buffer.h"
#include "core/gpu/kernel.h"
#include "core/gpu/shaders.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

namespace {

// ---------------------------------------------------------------------------
// A dispatch whose buffers are all read and written
// ---------------------------------------------------------------------------
//
// gpu::run_kernel zeroes its outputs before dispatching, which is right for a
// kernel with separate inputs and outputs and wrong for one that rewrites its
// audio in place and carries state. This uploads every buffer, dispatches
// once, and reads every buffer back.
struct InPlace {
    std::span<const std::uint32_t> spirv;
    std::uint32_t local_size_x = 64;
    std::vector<std::uint32_t> constants;
    std::span<const std::byte> push;
    std::uint32_t groups = 1;
    std::vector<std::span<std::byte>> buffers;
};

void run_in_place(const InPlace& job) {
    auto& context = test::shared_context();

    gpu::ComputePipeline::Options options;
    options.spirv = job.spirv;
    options.storage_buffer_count = static_cast<std::uint32_t>(job.buffers.size());
    options.local_size_x = job.local_size_x;
    options.push_constant_bytes = static_cast<std::uint32_t>(job.push.size());
    options.grid_constants = job.constants;
    auto pipeline = gpu::ComputePipeline::create(context, options);
    INFO(test::message_of(pipeline));
    REQUIRE(pipeline.has_value());

    constexpr VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    std::vector<gpu::Buffer> device;
    std::vector<gpu::Buffer> upload;
    std::vector<gpu::Buffer> readback;
    std::vector<VkBuffer> handles;
    std::vector<gpu::CommandRunner::BufferCopy> ins;
    std::vector<gpu::CommandRunner::BufferCopy> outs;
    for (const auto& bytes : job.buffers) {
        auto made = gpu::Buffer::create(context, bytes.size(), kStorage,
                                        gpu::MemoryKind::DeviceLocal);
        REQUIRE(made.has_value());
        auto staged = gpu::Buffer::create(context, bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          gpu::MemoryKind::Upload);
        REQUIRE(staged.has_value());
        REQUIRE(staged->write(bytes).has_value());
        auto back = gpu::Buffer::create(context, bytes.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        gpu::MemoryKind::Readback);
        REQUIRE(back.has_value());
        device.push_back(std::move(*made));
        upload.push_back(std::move(*staged));
        readback.push_back(std::move(*back));
    }
    for (std::size_t i = 0; i < job.buffers.size(); ++i) {
        handles.push_back(device[i].handle());
        ins.push_back({upload[i].handle(), device[i].handle(), job.buffers[i].size()});
        outs.push_back({device[i].handle(), readback[i].handle(), job.buffers[i].size()});
    }

    auto runner = gpu::CommandRunner::create(context);
    REQUIRE(runner.has_value());
    REQUIRE(runner->copy(ins).has_value());

    gpu::CommandRunner::Dispatch dispatch;
    dispatch.pipeline = &*pipeline;
    dispatch.buffers = handles;
    dispatch.group_count_x = job.groups;
    dispatch.push_constants = job.push;
    const auto ran = runner->run(dispatch);
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    REQUIRE(runner->copy(outs).has_value());
    for (std::size_t i = 0; i < job.buffers.size(); ++i) {
        REQUIRE(readback[i].read(job.buffers[i]).has_value());
    }
}

// Speech-shaped test audio: two tones, a slow third that moves, and noise,
// at a level that keeps the notch's feedback and the predictor busy.
std::vector<float> test_audio(std::size_t count, std::uint64_t seed, double rate) {
    std::mt19937_64 engine(seed);
    std::normal_distribution<double> noise(0.0, 0.05);
    std::vector<float> out(count);
    for (std::size_t n = 0; n < count; ++n) {
        const double t = static_cast<double>(n) / rate;
        const double glide = 600.0 + 200.0 * std::sin(2.0 * std::numbers::pi * 3.0 * t);
        const double value = 0.3 * std::sin(2.0 * std::numbers::pi * 1000.0 * t) +
                             0.2 * std::sin(2.0 * std::numbers::pi * 1700.0 * t + 0.4) +
                             0.2 * std::sin(2.0 * std::numbers::pi * glide * t) + noise(engine);
        out[n] = static_cast<float>(value);
    }
    return out;
}

template <class T>
std::span<std::byte> bytes_of(std::vector<T>& values) {
    return std::as_writable_bytes(std::span<T>(values));
}

template <class T>
std::span<const std::byte> push_of(const T& params) {
    return std::as_bytes(std::span<const T>(&params, 1));
}

// Dispatch lengths that land on and off every boundary the kernels have: the
// spectral hop, a single sample, zero, and one long run.
constexpr std::uint32_t kLengths[] = {100, 1, 0, 256, 700, 33, 1024, 255, 2, 3000};

}  // namespace

// ---------------------------------------------------------------------------
// The blanker
// ---------------------------------------------------------------------------

TEST_CASE("the blanker's two passes match their twins bit-exactly across a ring wrap",
          "[gpu][reference][noise]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x4E4F495345000001ULL;
    constexpr std::uint32_t kChannelBlocks = 4096;
    constexpr std::uint32_t kChannels = 3;
    constexpr std::uint32_t kOutRing = 2048;

    // Complex noise across three channels, with impulses of several heights
    // on the channel under test, some of them wider than one sample.
    test::SeededInput input(kSeed);
    std::vector<dsp::Complex32> channel =
        input.complexes(static_cast<std::size_t>(kChannelBlocks) * kChannels, -0.05F, 0.05F);
    const std::uint32_t chan_base = kChannelBlocks;
    std::mt19937_64 engine(kSeed);
    std::uniform_int_distribution<std::uint32_t> where(0, kChannelBlocks - 1U);
    for (int k = 0; k < 60; ++k) {
        const std::uint32_t at = where(engine);
        const float height = 0.2F * static_cast<float>(1 + k % 7);
        for (std::uint32_t w = 0; w < 1U + static_cast<std::uint32_t>(k % 3); ++w) {
            channel[chan_base + ((at + w) & (kChannelBlocks - 1U))] =
                dsp::Complex32{height, -0.5F * height};
        }
    }

    dsp::BlankerConfig config = dsp::design_blanker(144'000);
    INFO("window " << config.window << ", guard " << config.guard << ", hang " << config.hang
                   << ", lead " << config.lead);

    // A span that crosses the end of both rings.
    const std::uint64_t first = 3 * kChannelBlocks - 700;
    dsp::BlankerParams params;
    params.chan_base = chan_base;
    params.chan_mask = kChannelBlocks - 1U;
    params.chan_first = static_cast<std::uint32_t>(first & params.chan_mask);
    params.out_mask = kOutRing - 1U;
    params.out_first = static_cast<std::uint32_t>(first & params.out_mask);
    params.count = 1500;
    params.threshold = dsp::blanker_threshold(12.0);

    for (const std::uint32_t local : {32U, 64U, 256U}) {
        INFO("local_size_x = " << local);
        std::vector<float> cpu_flags(kOutRing, 0.0F);
        config.pass = dsp::kBlankDetect;
        REQUIRE(dsp::reference_blank_detect(config, params, channel, cpu_flags).has_value());

        std::vector<float> gpu_flags(kOutRing, 0.0F);
        std::vector<dsp::Complex32> unused(kOutRing);
        {
            gpu::KernelInvocation invocation;
            invocation.spirv = gpu::shaders::noise_blank();
            invocation.inputs = {std::as_bytes(std::span<const dsp::Complex32>(channel))};
            invocation.outputs = {bytes_of(gpu_flags), bytes_of(unused)};
            invocation.push_constants = push_of(params);
            invocation.invocations = params.count;
            invocation.local_size_x = local;
            invocation.grid_constants = {dsp::kBlankDetect, config.window, config.guard,
                                         config.hang, config.lead};
            const auto ran = gpu::run_kernel(test::shared_context(), invocation);
            INFO(test::message_of(ran));
            REQUIRE(ran.has_value());
        }
        const auto flags_diff = test::diff(gpu_flags, cpu_flags, kSeed);
        INFO(flags_diff.report);
        CHECK(flags_diff.identical);

        std::size_t flagged = 0;
        for (const float f : cpu_flags) {
            flagged += (f != 0.0F) ? 1U : 0U;
        }
        INFO("flagged " << flagged);
        CHECK(flagged > 20U);

        // Apply over the part whose flags the detect pass covered.
        dsp::BlankerParams apply = params;
        apply.chan_first = static_cast<std::uint32_t>((first + config.hang) & params.chan_mask);
        apply.out_first = static_cast<std::uint32_t>((first + config.hang) & params.out_mask);
        apply.count = params.count - config.hang - config.lead;
        config.pass = dsp::kBlankApply;
        std::vector<dsp::Complex32> cpu_blanked(kOutRing);
        REQUIRE(dsp::reference_blank_apply(config, apply, channel, cpu_flags, cpu_blanked)
                    .has_value());

        std::vector<dsp::Complex32> gpu_blanked(kOutRing);
        {
            gpu::KernelInvocation invocation;
            invocation.spirv = gpu::shaders::noise_blank();
            invocation.inputs = {std::as_bytes(std::span<const dsp::Complex32>(channel)),
                                 std::as_bytes(std::span<const float>(cpu_flags))};
            invocation.outputs = {bytes_of(gpu_blanked)};
            invocation.push_constants = push_of(apply);
            invocation.invocations = apply.count;
            invocation.local_size_x = local;
            invocation.grid_constants = {dsp::kBlankApply, config.window, config.guard,
                                         config.hang, config.lead};
            const auto ran = gpu::run_kernel(test::shared_context(), invocation);
            INFO(test::message_of(ran));
            REQUIRE(ran.has_value());
        }
        const auto blanked_diff = test::diff(gpu_blanked, cpu_blanked, kSeed);
        INFO(blanked_diff.report);
        CHECK(blanked_diff.identical);
    }
}

TEST_CASE("the blanker finds an impulse against noise and leaves the noise alone",
          "[reference][noise]") {
    constexpr std::uint64_t kSeed = 0x4E4F495345000002ULL;
    constexpr std::uint32_t kRing = 8192;

    std::mt19937_64 engine(kSeed);
    std::normal_distribution<float> gauss(0.0F, 0.01F);
    std::vector<dsp::Complex32> channel(kRing);
    for (auto& s : channel) {
        s = dsp::Complex32{gauss(engine), gauss(engine)};
    }
    // One impulse, five samples wide, 30 dB above the noise.
    constexpr std::uint32_t kAt = 5000;
    for (std::uint32_t w = 0; w < 5; ++w) {
        channel[kAt + w] = dsp::Complex32{0.3F, 0.1F};
    }

    dsp::BlankerConfig config = dsp::design_blanker(144'000);
    dsp::BlankerParams params;
    params.chan_mask = kRing - 1U;
    params.out_mask = kRing - 1U;
    params.chan_first = 1000;
    params.out_first = 1000;
    params.count = 6000;
    params.threshold = dsp::blanker_threshold(12.0);

    std::vector<float> flags(kRing, 0.0F);
    config.pass = dsp::kBlankDetect;
    REQUIRE(dsp::reference_blank_detect(config, params, channel, flags).has_value());
    std::vector<dsp::Complex32> blanked(kRing);
    params.chan_first = 1000 + config.hang;
    params.out_first = params.chan_first;
    params.count = 6000 - config.hang - config.lead;
    config.pass = dsp::kBlankApply;
    REQUIRE(dsp::reference_blank_apply(config, params, channel, flags, blanked).has_value());

    std::size_t zeroed = 0;
    std::size_t zeroed_away = 0;
    for (std::uint32_t i = params.chan_first; i < params.chan_first + params.count; ++i) {
        if (blanked[i] == dsp::Complex32{0.0F, 0.0F}) {
            ++zeroed;
            if (i + config.lead < kAt || i > kAt + 4U + config.hang) {
                ++zeroed_away;
            }
        }
    }
    for (std::uint32_t w = 0; w < 5; ++w) {
        CHECK(blanked[kAt + w] == dsp::Complex32{0.0F, 0.0F});
    }
    INFO("zeroed " << zeroed << ", of them away from the impulse " << zeroed_away);
    // The impulse, its hang and its lead, and nothing the noise did.
    CHECK(zeroed == 5U + config.hang + config.lead);
    CHECK(zeroed_away == 0U);
}

// ---------------------------------------------------------------------------
// The line kernel
// ---------------------------------------------------------------------------

namespace {

void check_line(std::uint32_t flags, std::uint64_t seed) {
    constexpr double kRate = 48'000.0;
    const dsp::LineConfig config{};

    dsp::LineParams params;
    params.flags = flags;
    params.stride = 7;
    params.delay = 48;
    auto design = dsp::design_notch(1'000.0, 40.0, 100.0, 48'000);
    REQUIRE(design.has_value());
    params.b0 = static_cast<float>(design->b0);
    params.b1 = static_cast<float>(design->b1);
    params.b2 = static_cast<float>(design->b2);
    params.a1 = static_cast<float>(design->a1);
    params.a2 = static_cast<float>(design->a2);
    params.mu = 0.002F;
    params.leak = static_cast<float>(1.0 - 1.0e-5);
    params.eps = 1.0e-6F;

    std::vector<float> cpu_state(dsp::line_state_size(config), 0.0F);
    std::vector<float> gpu_state = cpu_state;

    std::size_t offset = 0;
    std::vector<float> all = test_audio(20'000, seed, kRate);
    for (const std::uint32_t length : kLengths) {
        INFO("dispatch of " << length << " at sample " << offset);
        std::vector<float> cpu_audio(all.begin() + static_cast<std::ptrdiff_t>(offset),
                                     all.begin() + static_cast<std::ptrdiff_t>(offset + length));
        cpu_audio.push_back(0.0F);
        std::vector<float> gpu_audio = cpu_audio;
        params.count = length;

        REQUIRE(dsp::reference_line(config, params, cpu_audio, cpu_state).has_value());

        InPlace job;
        job.spirv = gpu::shaders::noise_line();
        job.local_size_x = config.lanes;
        job.constants = {config.lanes, config.taps, config.history};
        job.push = push_of(params);
        job.buffers = {bytes_of(gpu_audio), bytes_of(gpu_state)};
        run_in_place(job);

        const auto audio_diff = test::diff(gpu_audio, cpu_audio, seed);
        INFO(audio_diff.report);
        CHECK(audio_diff.identical);
        const auto state_diff = test::diff(gpu_state, cpu_state, seed);
        INFO(state_diff.report);
        CHECK(state_diff.identical);
        offset += length;
    }

    // The predictor did something, so the diff above compared arithmetic and
    // not two buffers of zeros.
    if ((flags & dsp::kLineAle) != 0U) {
        double energy = 0.0;
        for (std::size_t k = 0; k < config.taps; ++k) {
            energy += std::abs(cpu_state[dsp::kLineStateWeights + k]);
        }
        CHECK(energy > 0.0);
    }
}

}  // namespace

TEST_CASE("the notch matches its twin bit-exactly across dispatch boundaries",
          "[gpu][reference][noise]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());
    check_line(dsp::kLineNotch, 0x4E4F495345000003ULL);
}

TEST_CASE("the automatic notch matches its twin bit-exactly across dispatch boundaries",
          "[gpu][reference][noise]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());
    check_line(dsp::kLineAle, 0x4E4F495345000004ULL);
}

TEST_CASE("both notches together match their twin bit-exactly", "[gpu][reference][noise]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());
    check_line(dsp::kLineNotch | dsp::kLineAle, 0x4E4F495345000005ULL);
}

TEST_CASE("the notch cuts to its stated depth and width", "[reference][noise]") {
    constexpr dsp::SampleRate kRate = 48'000;
    struct Case {
        double hz;
        double depth;
        double width;
    };
    for (const Case c : {Case{1'000.0, 40.0, 100.0}, Case{350.0, 20.0, 50.0},
                         Case{2'400.0, 60.0, 200.0}, Case{15'000.0, 30.0, 400.0}}) {
        INFO(c.hz << " Hz, " << c.depth << " dB, " << c.width << " Hz wide");
        auto design = dsp::design_notch(c.hz, c.depth, c.width, kRate);
        REQUIRE(design.has_value());

        // The response of the coefficients the kernel sees, rounded to float.
        const auto b0 = static_cast<double>(static_cast<float>(design->b0));
        const auto b1 = static_cast<double>(static_cast<float>(design->b1));
        const auto b2 = static_cast<double>(static_cast<float>(design->b2));
        const auto a1 = static_cast<double>(static_cast<float>(design->a1));
        const auto a2 = static_cast<double>(static_cast<float>(design->a2));
        auto gain_db = [&](double hz) {
            const std::complex<double> z1 =
                std::polar(1.0, -2.0 * std::numbers::pi * hz / static_cast<double>(kRate));
            const std::complex<double> z2 = z1 * z1;
            const std::complex<double> h = (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
            return 20.0 * std::log10(std::abs(h));
        };

        const double at = gain_db(c.hz);
        INFO("at the notch " << at << " dB");
        CHECK(std::abs(at + c.depth) < 1.0);

        // The -3 dB points of a notch this deep sit half a width either side.
        const double lower = gain_db(c.hz - 0.5 * c.width);
        const double upper = gain_db(c.hz + 0.5 * c.width);
        INFO("half a width either side " << lower << " and " << upper << " dB");
        CHECK(lower < -1.5);
        CHECK(lower > -4.5);
        CHECK(upper < -1.5);
        CHECK(upper > -4.5);

        // And it cuts nowhere else.
        const double far = gain_db(c.hz < 12'000.0 ? c.hz + 10.0 * c.width : c.hz - 10.0 * c.width);
        INFO("ten widths away " << far << " dB");
        CHECK(std::abs(far) < 0.2);
    }
}

TEST_CASE("the automatic notch removes a steady tone and keeps a moving one",
          "[reference][noise]") {
    constexpr double kRate = 48'000.0;
    constexpr std::size_t kCount = 96'000;
    const dsp::LineConfig config{};

    // The figures plan_noise gives a USB receiver at 48 kS/s: stride 7, a
    // 5 ms delay, and the step and leak in core/dsp/noise_reference.cpp,
    // which carries the sweep that chose them.
    dsp::LineParams params;
    params.flags = dsp::kLineAle;
    params.stride = 7;
    params.delay = 240;
    params.mu = 1.0e-4F;
    params.leak = static_cast<float>(1.0 - 3.0e-6);
    params.eps = 1.0e-10F;
    params.count = kCount;

    // A heterodyne at 1300 Hz and a tone gliding between 400 and 800 Hz four
    // times a second, which is roughly how fast a voice's pitch moves.
    std::vector<float> steady(kCount);
    std::vector<float> moving(kCount);
    double phase = 0.0;
    for (std::size_t n = 0; n < kCount; ++n) {
        const double t = static_cast<double>(n) / kRate;
        steady[n] = static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * 1300.0 * t));
        const double f = 600.0 + 200.0 * std::sin(2.0 * std::numbers::pi * 4.0 * t);
        phase += 2.0 * std::numbers::pi * f / kRate;
        moving[n] = static_cast<float>(0.3 * std::sin(phase));
    }
    std::vector<float> both(kCount);
    for (std::size_t n = 0; n < kCount; ++n) {
        both[n] = steady[n] + moving[n];
    }

    std::vector<float> state(dsp::line_state_size(config), 0.0F);
    std::vector<float> out = both;
    REQUIRE(dsp::reference_line(config, params, out, state).has_value());

    // Over the second half, once it has learned: project the output on each
    // component to see what of it survived.
    auto kept_db = [&](const std::vector<float>& component) {
        double dot = 0.0;
        double self = 0.0;
        for (std::size_t n = kCount / 2; n < kCount; ++n) {
            dot += static_cast<double>(out[n]) * component[n];
            self += static_cast<double>(component[n]) * component[n];
        }
        return 20.0 * std::log10(std::abs(dot / self));
    };
    const double steady_kept = kept_db(steady);
    const double moving_kept = kept_db(moving);
    INFO("steady tone kept at " << steady_kept << " dB, moving tone at " << moving_kept << " dB");
    // Measured -12.6 and -0.3 dB. The step is set for what it costs a voice
    // rather than for the depth of the cut; core/dsp/noise_reference.cpp has
    // the trade.
    CHECK(steady_kept < -10.0);
    CHECK(moving_kept > -1.0);
}

// ---------------------------------------------------------------------------
// Spectral noise reduction
// ---------------------------------------------------------------------------

TEST_CASE("noise reduction matches its twin bit-exactly across dispatch boundaries",
          "[gpu][reference][noise]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    for (const dsp::SampleRate rate : {48'000, 8'000}) {
        const dsp::SpectralConfig config = dsp::spectral_config_for(rate);
        INFO("frame " << config.frame << " at " << rate << " S/s");
        const std::uint64_t seed = 0x4E4F495345000006ULL + static_cast<std::uint64_t>(rate);
        const dsp::SpectralParams params_base = dsp::spectral_params(0.7);
        const std::vector<float> tables = dsp::design_spectral_tables(config);

        std::vector<float> cpu_state(dsp::spectral_state_size(config), 0.0F);
        std::vector<float> gpu_state = cpu_state;
        std::vector<float> all = test_audio(20'000, seed, static_cast<double>(rate));

        std::size_t offset = 0;
        for (const std::uint32_t length : kLengths) {
            INFO("dispatch of " << length << " at sample " << offset);
            std::vector<float> cpu_audio(
                all.begin() + static_cast<std::ptrdiff_t>(offset),
                all.begin() + static_cast<std::ptrdiff_t>(offset + length));
            cpu_audio.push_back(0.0F);
            std::vector<float> gpu_audio = cpu_audio;
            dsp::SpectralParams params = params_base;
            params.count = length;

            REQUIRE(dsp::reference_spectral(config, params, tables, cpu_audio, cpu_state)
                        .has_value());

            std::vector<float> gpu_tables = tables;
            InPlace job;
            job.spirv = gpu::shaders::noise_spectral();
            job.local_size_x = (length % 2U == 0U) ? 64U : 256U;
            job.constants = {config.frame};
            job.push = push_of(params);
            job.buffers = {bytes_of(gpu_audio), bytes_of(gpu_tables), bytes_of(gpu_state)};
            run_in_place(job);

            const auto audio_diff = test::diff(gpu_audio, cpu_audio, seed);
            INFO(audio_diff.report);
            CHECK(audio_diff.identical);
            const auto state_diff = test::diff(gpu_state, cpu_state, seed);
            INFO(state_diff.report);
            CHECK(state_diff.identical);
            offset += length;
        }
    }
}

TEST_CASE("noise reduction at unit gain hands back its input one frame late",
          "[reference][noise]") {
    const dsp::SpectralConfig config = dsp::spectral_config_for(48'000);
    REQUIRE(config.frame == 512U);
    const std::vector<float> tables = dsp::design_spectral_tables(config);

    // Alpha zero and a floor of one is a gain of exactly one in every bin.
    dsp::SpectralParams params = dsp::spectral_params(0.5);
    params.alpha = 0.0F;
    params.floor_gain = 1.0F;

    std::vector<float> input = test_audio(10'000, 0x4E4F495345000007ULL, 48'000.0);
    std::vector<float> output = input;
    std::vector<float> state(dsp::spectral_state_size(config), 0.0F);
    params.count = static_cast<std::uint32_t>(output.size());
    REQUIRE(dsp::reference_spectral(config, params, tables, output, state).has_value());

    double worst = 0.0;
    for (std::size_t n = config.frame; n < output.size(); ++n) {
        worst = std::max(worst, std::abs(static_cast<double>(output[n]) -
                                         static_cast<double>(input[n - config.frame])));
    }
    INFO("worst reconstruction error " << worst);
    CHECK(worst < 2.0e-5);
    for (std::size_t n = 0; n < config.frame / 2U; ++n) {
        CHECK(output[n] == 0.0F);
    }
}

TEST_CASE("noise reduction's floor lands on the true noise floor", "[reference][noise]") {
    const dsp::SpectralConfig config = dsp::spectral_config_for(48'000);
    const std::vector<float> tables = dsp::design_spectral_tables(config);
    const dsp::SpectralParams params_base = dsp::spectral_params(0.5);

    constexpr double kSigma = 0.05;
    std::mt19937_64 engine(0x4E4F495345000008ULL);
    std::normal_distribution<double> gauss(0.0, kSigma);
    std::vector<float> audio(48'000 * 4);
    for (auto& value : audio) {
        value = static_cast<float>(gauss(engine));
    }

    std::vector<float> state(dsp::spectral_state_size(config), 0.0F);
    dsp::SpectralParams params = params_base;
    params.count = static_cast<std::uint32_t>(audio.size());
    REQUIRE(dsp::reference_spectral(config, params, tables, audio, state).has_value());

    // White noise of variance s^2 windowed by w has a mean periodogram of
    // s^2 * sum(w^2) in every bin, and sum(w^2) is N/2 for this window.
    const std::size_t n = config.frame;
    const std::size_t bins = n / 2U + 1U;
    const std::size_t minimum_at = 2U + n + n / 2U + n / 2U + bins;
    double mean = 0.0;
    for (std::size_t k = 1; k + 1U < bins; ++k) {
        mean += static_cast<double>(state[minimum_at + k]) * static_cast<double>(params.bias);
    }
    mean /= static_cast<double>(bins - 2U);
    const double truth = kSigma * kSigma * static_cast<double>(n) / 2.0;
    const double error_db = 10.0 * std::log10(mean / truth);
    INFO("noise estimate " << mean << " against " << truth << ", " << error_db << " dB");
    CHECK(std::abs(error_db) < 1.5);
}
