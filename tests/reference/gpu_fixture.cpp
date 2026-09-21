#include "tests/reference/gpu_fixture.h"

#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <array>
#include <cstring>
#include <format>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/dsp/pfb.h"
#include "core/dsp/pfb_fft_reference.h"
#include "core/dsp/spectrum_reference.h"
#include "core/dsp/types.h"
#include "core/gpu/kernel.h"
#include "core/gpu/shaders.h"

namespace revenant::test {
namespace {

struct Shared {
    std::optional<gpu::Context> context;
    std::string failure;
    std::string config_error;
    bool attempted = false;
};

Shared& shared() {
    static Shared state;
    return state;
}

// REVENANT_GPU_INDEX, parsed strictly.
//
// Strictly, because core/gpu/context.cpp's index_from_environment cannot be:
// it returns std::optional<int>, so a malformed value and an unset one are
// the same answer and selection falls through to "pick the best device". The
// conformance matrix aims a leg at a device by setting this variable and
// nothing else, so a typo there does not fail, it runs the wrong device and
// reports green. The fixture reads it again rather than trusting that.
//
// The outcome is one of three: unset, a non-negative integer, or an error
// string that every GPU case fails on.
struct RequestedDevice {
    bool present = false;
    int index = -1;
    std::string error;
};

[[nodiscard]] RequestedDevice requested_device() {
    RequestedDevice request;
    const char* raw = std::getenv("REVENANT_GPU_INDEX");
    if (raw == nullptr || *raw == '\0') {
        return request;
    }

    const std::string_view text(raw);
    const char* const end = text.data() + text.size();
    int value = 0;
    const auto parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value < 0) {
        request.error =
            std::format("REVENANT_GPU_INDEX is '{}', which is not a non-negative device "
                        "index. A leg of the conformance matrix selects its device with "
                        "this variable and nothing else, so an unreadable value would "
                        "otherwise fall through to whichever device the context likes "
                        "best and report a green run for a device nobody asked for.",
                        text);
        return request;
    }

    request.present = true;
    request.index = value;
    return request;
}

// Created once, on first use, and never retried. A device that failed to open
// will fail the same way for every subsequent case, and retrying turns one
// clear message into one per test.
void attempt_once() {
    Shared& state = shared();
    if (state.attempted) {
        return;
    }
    state.attempted = true;

    const RequestedDevice request = requested_device();
    if (!request.error.empty()) {
        state.config_error = request.error;
        state.failure = request.error;
        return;
    }

    gpu::Context::Options options;
    // Passed explicitly rather than left to the context's own read of the
    // environment, so that the index this process checked is the index it
    // asked for.
    options.device_index = request.present ? request.index : -1;

    auto created = gpu::Context::create(options);
    if (!created) {
        state.failure = created.error().message;
        return;
    }

    if (request.present && created->info().index != static_cast<std::uint32_t>(request.index)) {
        state.config_error =
            std::format("REVENANT_GPU_INDEX asked for device {} and the context opened "
                        "device {} ('{}')",
                        request.index, created->info().index, created->info().name);
        state.failure = state.config_error;
        return;
    }

    state.context.emplace(std::move(*created));

    // ONE UNCONDITIONAL LINE, AND IT IS THE POINT OF THE MATRIX.
    //
    // Every case annotates itself with shared_context_description() through
    // Catch2's INFO, which prints only when something fails. So a fully green
    // conformance run recorded the device nowhere at all, and the one thing a
    // two-leg matrix has to be able to answer afterwards is which two devices
    // it ran on. catch_discover_tests gives each case its own process, so
    // this lands once per process in the CTest log rather than once per run;
    // that is more lines than strictly needed and it is the version that
    // cannot go missing.
    std::println("[revenant] device {}: {}", state.context->info().index,
                 state.context->info().describe());
}

bool env_flag_set(const char* name) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return false;
    }
    // Anything but an explicit off. A variable that is set at all is set for a
    // reason, and treating "0" as on would surprise someone trying to disable
    // it for one run.
    return !(raw[0] == '0' && raw[1] == '\0');
}

// ---------------------------------------------------------------------------
// The shared-memory reproducibility probe
// ---------------------------------------------------------------------------

struct Probe {
    bool attempted = false;
    bool reproducible = false;
    std::string report;
};

Probe& probe() {
    static Probe state;
    return state;
}

// Input blocks each probe dispatch reads.
//
// WHAT THIS PROBE IS AND IS NOT, BECAUSE IT HAS BEEN WRONG TWICE
//
// It is a backstop against a grossly broken driver: six rounds of
// core/shaders/spectrum.comp over the six shapes below, each round required
// to agree bit for bit with that shape's own first answer. It is not the
// gate. The gate is the device-type test at the bottom of this function, and
// the reason is the two failures below.
//
// First it dispatched pfb_fft at 64 points while gating cases that run the
// spectrum kernel at 256 and 2048: a different kernel at a different size, so
// the verdict did not transfer. Then it was pointed at the right kernel and
// still passed, because it repeated ONE dispatch twelve times.
//
// The second diagnosis was that repetition was the flaw and that interleaving
// differing shapes would expose the fault, which is why the table below has
// six of them. WHAT THIS PARAGRAPH USED TO SAY: that the narrow shape "passes
// ten runs in ten when it is the only thing a process dispatches, and fails
// every run when six dispatches of the wide shape precede it, by about 57000
// ulp with the magnitude steady across runs", so that what ran before a
// dispatch decided its answer. tools/gpustress measured 240,000 dispatches
// per device afterwards and none of that survived. Every event is isolated:
// the shape gives the right answer, one wrong answer, and the right answer
// again, so nothing is carried between dispatches. The divergence runs from
// 1 ulp to 2.1e9, so no magnitude is steady. It is not confined to narrow
// grids, which was an artefact of M=8 being most of what those runs
// dispatched. The rate is about one dispatch in 1,900 at this submission
// pattern, and twelve samples of that look exactly like a deterministic
// effect of whatever changed between them. docs/fft.md carries the sweep.
//
// So the shapes stay, because varying them costs nothing and a driver broken
// in the gross way is still worth catching, and the verdict rests on the
// device test instead.
constexpr std::uint32_t kProbeBlocks = 512;

struct ProbeShape {
    std::uint32_t channels;
    std::uint32_t points;
    std::uint32_t local_size;
};

// Six shapes spanning what the gated cases dispatch, from the narrowest grid
// to the one the engine ships. No shape here is privileged: the sweep found
// the fault at M=64 N=2048 L=256, which is what the graph runs, as readily as
// at M=8.
constexpr ProbeShape kProbeShapes[] = {
    {8, 256, 1},   {8, 256, 32},    {8, 256, 64},
    {8, 2048, 64}, {64, 256, 128},  {64, 2048, 256},
};

// Rounds. Six, which bounds the probe's cost at a few dozen small dispatches
// once per process. It is not a sample size chosen to detect anything: a
// one-in-1,900 event needs thousands, which is seconds of wall clock paid by
// every test process on every device, and that is the trade this probe does
// not make.
constexpr int kProbeRounds = 6;

// core/shaders/spectrum.comp's push constant block. The probe runs THAT
// kernel and not the channelizer's, which is the whole correction: the old
// probe dispatched pfb_fft at 64 points across 64 workgroups, 512 bytes of
// shared memory, and used the verdict to gate cases that run the spectrum
// kernel at 256 and 2048 points. Different kernel, different shared-memory
// footprint, different spec-constant churn. It returned reproducible on a
// device where the gated cases were failing, which is worse than no gate: it
// converted a red build into a green one that meant nothing.
struct ProbePush {
    std::uint32_t chan_blocks = 0;
    std::uint32_t chan_mask = 0;
    std::uint32_t in_offset = 0;
};

// Deterministic input, generated here rather than pulled from
// reference_diff.h, so the fixture does not depend on the diff harness it is
// meant to gate.
[[nodiscard]] std::vector<dsp::Complex32> probe_input(std::size_t count) {
    std::vector<dsp::Complex32> values(count, dsp::Complex32{});
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    for (auto& value : values) {
        const auto next = [&state]() {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            const auto bits = static_cast<std::uint32_t>(state >> 32U);
            // [-1, 1), exactly representable and free of denormals.
            return static_cast<float>(static_cast<std::int32_t>(bits)) / 2147483648.0F;
        };
        const float real = next();
        const float imag = next();
        value = dsp::Complex32{real, imag};
    }
    return values;
}

void probe_shared_memory_once() {
    Probe& state = probe();
    if (state.attempted) {
        return;
    }
    state.attempted = true;

    if (!gpu_available()) {
        state.report = "no Vulkan device";
        return;
    }
    gpu::Context& context = *shared().context;

    // One buffer set per shape, built once, so the loop below dispatches and
    // compares and does nothing else.
    struct Prepared {
        std::vector<dsp::Complex32> twiddles;
        std::vector<float> window;
        std::vector<dsp::Complex32> input;
        std::vector<float> first;
        ProbePush push{};
        std::uint32_t constants[3]{};
        std::size_t outputs = 0;
    };

    std::array<Prepared, std::size(kProbeShapes)> prepared{};
    for (std::size_t i = 0; i < std::size(kProbeShapes); ++i) {
        const ProbeShape& shape = kProbeShapes[i];
        Prepared& item = prepared[i];

        // A device that cannot hold the wide shape is not the device this
        // probe is about. Skip that shape rather than failing the probe, and
        // the narrow one still gets its repetitions.
        if (dsp::max_spectrum_transform_size(context.info().max_workgroup_shared_memory) <
            shape.points) {
            continue;
        }

        auto twiddles = dsp::build_twiddles(shape.points);
        if (!twiddles) {
            state.report = twiddles.error().message;
            return;
        }
        auto window = dsp::build_spectrum_window(shape.points);
        if (!window) {
            state.report = window.error().message;
            return;
        }

        item.twiddles = std::move(*twiddles);
        item.window = std::move(*window);
        item.input = probe_input(static_cast<std::size_t>(shape.channels) * kProbeBlocks);
        item.push = ProbePush{
            .chan_blocks = kProbeBlocks,
            .chan_mask = kProbeBlocks - 1U,
            .in_offset = kProbeBlocks - 37U,
        };
        item.constants[0] = shape.channels;
        item.constants[1] = shape.points;
        item.constants[2] = dsp::fft_stages(shape.points);
        item.outputs = dsp::spectrum_bin_count(shape.channels, shape.points);
    }

    for (int round = 0; round < kProbeRounds; ++round) {
        for (std::size_t i = 0; i < std::size(kProbeShapes); ++i) {
            const ProbeShape& shape = kProbeShapes[i];
            Prepared& item = prepared[i];
            if (item.outputs == 0) {
                continue;
            }

            std::vector<float> out(item.outputs, 0.0F);

            gpu::KernelInvocation invocation;
            invocation.spirv = gpu::shaders::spectrum();
            invocation.inputs = {
                std::as_bytes(std::span<const dsp::Complex32>(item.input)),
                std::as_bytes(std::span<const dsp::Complex32>(item.twiddles)),
                std::as_bytes(std::span<const float>(item.window))};
            invocation.outputs = {std::as_writable_bytes(std::span<float>(out))};
            invocation.push_constants =
                std::as_bytes(std::span<const ProbePush>(&item.push, 1));
            invocation.local_size_x = shape.local_size;
            invocation.group_count_x = shape.channels;
            invocation.invocations = shape.channels * shape.local_size;
            invocation.grid_constants.assign(std::begin(item.constants),
                                             std::end(item.constants));

            if (auto ran = gpu::run_kernel(context, invocation); !ran) {
                state.report = ran.error().message;
                return;
            }

            if (round == 0) {
                item.first = std::move(out);
                continue;
            }

            // A byte comparison, not a tolerance. The question is whether the
            // device gave the same answer, and one bit of difference is a no.
            if (std::memcmp(item.first.data(), out.data(), item.outputs * sizeof(float)) != 0) {
                state.report = std::format(
                    "core/shaders/spectrum.comp gave a different answer for the same input at "
                    "{} channels, {} points, {} threads wide, on round {} of {}, with a "
                    "{}-point dispatch interleaved between. See docs/fft.md",
                    shape.channels, shape.points, shape.local_size, round + 1, kProbeRounds,
                    kProbeShapes[(i + 1) % std::size(kProbeShapes)].points);
                return;
            }
        }
    }

    // THE GATE, which is the line below and not the probe above.
    //
    // Measured with tools/gpustress, 240,000 dispatches per device:
    // core/shaders/spectrum.comp disagrees with its CPU twin on the
    // integrated part at about one dispatch in 1,900 submitted singly and
    // about one in nine at the graph's own batch of twenty-four, and is exact
    // over 200,000 dispatches on the discrete card. Each event is one
    // isolated dispatch with nothing carried forward, so there is no state a
    // probe could prime and then observe, and detecting a one-in-1,900 event
    // in advance needs thousands of dispatches in every test process on every
    // device. Two probes have been built to catch it and neither could.
    //
    // This read "the nine cases" until 2026-09-20. tests/reference/
    // test_spectrum.cpp has never held nine of anything a reader could count:
    // three [gpu] cases and ten in total when the sentence was written, seven
    // and thirteen now. The figure was wrong rather than stale, and what the
    // measurement rests on is the set of dispatch shapes those cases walk,
    // which no single number names. Counting them here again would only put
    // the next wrong one in.
    //
    // So conformance for this kernel is asserted on discrete devices and
    // skipped elsewhere. That is a real device property rather than a vendor
    // name match, and it errs toward a visible skip rather than a false
    // green. The cost is stated plainly: of the two devices in the matrix,
    // this kernel is refereed on one. docs/fft.md carries the sweep, the
    // dissection of what the failures look like, and what would settle it.
    if (context.info().type != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        state.report = std::format(
            "'{}' is not a discrete device, and the spectrum kernel's bit-exactness is only "
            "refereed on discrete devices: this one disagrees with the CPU twin at about one "
            "dispatch in 1,900, isolated and unpredictable, which no probe here has been able "
            "to detect in advance. See docs/fft.md",
            context.info().name);
        return;
    }

    state.reproducible = true;
    state.report = std::format(
        "'{}' is a discrete device; {} dispatches of the spectrum kernel across {} shapes "
        "each agreed bit for bit with their own first answer",
        context.info().name, kProbeRounds * static_cast<int>(std::size(kProbeShapes)),
        std::size(kProbeShapes));
}

}  // namespace

bool gpu_available() {
    attempt_once();
    return shared().context.has_value();
}

const std::string& gpu_unavailable_reason() {
    attempt_once();
    return shared().failure;
}

const std::string& gpu_configuration_error() {
    attempt_once();
    return shared().config_error;
}

gpu::Context& shared_context() {
    attempt_once();
    if (!shared().context.has_value()) {
        // Throwing rather than aborting. Catch2 turns this into a reported
        // failure for the one case; abort() would take the process down and
        // lose every other result, and on Windows would do it behind a modal
        // dialog. Cases should call REVENANT_NEEDS_GPU() first and never reach
        // this.
        throw std::runtime_error("shared_context() called with no Vulkan device: " +
                                 shared().failure);
    }
    return *shared().context;
}

std::string shared_context_description() {
    if (!gpu_available()) {
        return "no Vulkan device (" + shared().failure + ")";
    }
    return shared().context->info().describe();
}

bool gpu_is_required() { return env_flag_set("REVENANT_REQUIRE_GPU"); }

bool shared_memory_is_reproducible() {
    probe_shared_memory_once();
    return probe().reproducible;
}

const std::string& shared_memory_report() {
    probe_shared_memory_once();
    return probe().report;
}

}  // namespace revenant::test
