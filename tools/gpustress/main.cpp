// Dispatch a kernel enough times to find out whether it is actually exact.
//
// WHAT IT IS FOR.
//
// The reference diff suite answers "does this kernel agree with its twin",
// and it answers it a handful of times per case because it is a test suite
// and has to finish. That is the right budget for a deterministic kernel and
// the wrong one for a rare fault: a one-in-two-thousand event is invisible to
// twelve samples, and twelve samples taken while something else is being
// changed look exactly like a deterministic effect of the change. An
// afternoon was spent on that mistake; docs/fft.md carries the corrections.
//
// So this runs the same comparison the suite runs, against the same CPU
// twins, at a sample size a suite cannot afford. Its output is a rate.
//
// WHAT IT ESTABLISHED, 2026-09-19, 240,000 dispatches per device.
//
// The RTX 4090 is bit-exact: 200,000 spectrum dispatches and 40,000
// channelizer branch dispatches, zero disagreements with the twin.
//
// The integrated Radeon fails about one spectrum dispatch in 1,900. Every
// failure is a single isolated dispatch, at every shape including the one
// the graph ships, with no dependence on what ran before it and nothing
// carried into what runs after. `pfb_branch` in the same processes never
// fails, so it is this kernel rather than this device. The full table and
// what it withdraws are in docs/fft.md.
//
// The failures are not bit flips. Of sixty dissected, none corrupted a
// single value; the smallest touched five and the mean was 533, spread
// across a third of the dispatch's workgroups. At a fixed M and N, with the
// workgroup width as the only variable, a width of one gives zero events in
// 40,000 dispatches where 64 gives seven and 128 gives six. The fault needs
// concurrency, which is what the blast-radius fields below are for.
//
// WHAT --batch ESTABLISHED, the same day, 3,696,256 dispatches on the
// discrete card and rather more on the integrated one.
//
// That was one dispatch per command buffer, and the rate above is a property
// of that pattern rather than of the kernel. --batch n records n dispatches
// into ONE command buffer with barriers between them and submits once, which
// is what core/engine/graph.cpp does. On the integrated Radeon, twenty-four
// deep takes the spectrum kernel from one bad dispatch in 1,839 to one in 9,
// and 44% of command buffers then carry a wrong dispatch. At the shape the
// graph ships, M=64 N=2048 L=256 and nothing else in the buffer, four
// dispatches is 1.2% of command buffers, five is 19% and six is 85%. The
// engine records five.
//
// It is not the plumbing. --batch 1 is the control: persistent pipelines,
// persistent buffers, one submission, still one dispatch per command buffer,
// and its rate is one in 1,905. Only sharing a submission changes anything.
//
// Three more things the batched mode measured. The wrong dispatch is the
// first or second in the buffer, 25 times in 9,521 the third and never
// anything later, and the first one is wrong more often the more work is
// recorded AFTER it. None of the wrong
// values is the zero fill the output carried into the dispatch, so the
// kernel finished and computed the wrong answer rather than the readback
// overtaking it. And the validation layer with synchronization validation on
// reports nothing while the failures continue, so the recording is not the
// fault. docs/fft.md has the tables.
//
// WHY IT IS SHAPED LIKE THIS.
//
// Twelve shapes in rotation rather than one repeated, because the earlier
// reading of this fault blamed the sequence and that had to be testable.
// It is not the sequence, but the rotation stays: it costs nothing and it
// covers the narrow grids, the shipped grid and both workgroup extremes in
// one run. --shapes narrows it to one when the question is about one, which
// is how the depth curve above was measured.
//
// Each dispatch is compared against its shape's CPU twin, not against the
// shape's previous answer. Comparing answers to each other cannot see a
// kernel that is stably wrong, which is exactly the fault that was being
// looked for, and it makes the first dispatch a baseline that one bad
// dispatch poisons for the whole run. Both mistakes were made here first.
//
// WHAT IT STILL DOES NOT COVER.
//
// Overlapping submissions. core/engine/graph.cpp records frame k+1 and
// submits it while frame k is still on the device, with no semaphore between
// them; every mode here fences a submission before recording the next. The
// engine also dispatches pfb_fft, a second shared-memory transform, directly
// before the spectrum kernel in the same command buffer, and this tool has no
// pfb_fft shape. Either could raise the rate further and neither is measured.
//
// Not part of the engine, not linked into anything, and off by default. Build
// it with -DREVENANT_BUILD_GPU_STRESS=ON.

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <iterator>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "core/dsp/pfb.h"
#include "core/dsp/pfb_branch_reference.h"
#include "core/dsp/spectrum_reference.h"
#include "core/dsp/types.h"
#include "core/engine/record_util.h"
#include "core/gpu/buffer.h"
#include "core/gpu/context.h"
#include "core/gpu/kernel.h"
#include "core/gpu/shaders.h"

namespace {

using namespace revenant;

// One answer a shape gave, and every round it gave it in.
//
// Counting divergences alone answers the wrong question. The first run of this
// tool reported eleven divergences in eleven comparisons for five shapes,
// which reads as "wrong every time" and is consistent with two quite different
// faults: an answer that is fresh garbage on every dispatch, or two stable
// answers where the first dispatch gets one and every later dispatch gets the
// other. The second would point straight at pipeline creation rather than at
// execution, and nothing in a divergence count can tell them apart. So the
// tool groups identical answers and prints which rounds produced each.
struct Answer {
    std::uint64_t hash = 0;
    std::vector<float> data;
    std::vector<int> rounds;
};

// One dispatch that disagreed with its twin, described by its blast radius.
//
// This is the measurement that separates the two remaining explanations. A
// single value corrupted in storage, one flipped bit in a buffer, gives
// `values == 1` and `flipped_bits == 1`. A transform that went wrong partway
// through gives a spray: the error enters at one butterfly and every later
// stage mixes it across the workgroup's whole output, so `values` runs to
// hundreds and `groups` stays at one because a workgroup cannot corrupt its
// neighbour's shared memory.
//
// `groups` is the discriminator for the third possibility, something wrong
// with the dispatch or the buffer as a whole, which would touch many.
struct Event {
    int round = 0;
    std::size_t values = 0;
    std::size_t groups = 0;
    std::size_t first_index = 0;
    std::uint64_t worst_ulp = 0;
    int flipped_bits = 0;

    // Wrong values that are exactly +0.0f, which is what the output buffer
    // was filled with before the dispatch.
    //
    // This is the discriminator between two faults that look identical in a
    // divergence count. If the readback copy ran before the kernel's writes
    // landed, the untouched bins still hold the fill and every wrong value is
    // zero, which accuses the barrier. If the kernel computed the wrong
    // answer, the wrong values are decibel numbers like the right ones and
    // none of them is zero, which accuses the transform. A bin genuinely at
    // 0.0 dB cannot be confused with this: silence floors at -200 dB and the
    // input is noise thirty decibels below full scale, so an exact zero in
    // the frame means nothing wrote there.
    std::size_t unwritten = 0;
};

struct Shape {
    const char* kind = "spectrum";
    std::uint32_t channels = 0;
    std::uint32_t transform = 0;
    std::uint32_t local_size = 0;

    // Built once. Rebuilding per round would make the tool measure allocation
    // churn and filter design as well, and the fault survives neither being
    // ruled out.
    std::vector<dsp::Complex32> input;
    std::vector<dsp::Complex32> twiddles;
    std::vector<float> window;
    dsp::GridParams grid;
    std::vector<float> taps;

    // What the CPU twin says the answer is.
    //
    // Grouping the GPU's answers against each other is blind to exactly the
    // fault docs/fft.md records. That fault is a STABLE wrong answer: a shape
    // that gives the same result on every dispatch, and the wrong one, because
    // of what preceded it. Every round here dispatches the same sequence, so
    // every round would inherit the same contamination, every answer would
    // match, and the tool would report the shape clean. Only the twin can see
    // it, which is why the twin is here rather than only in the test suite.
    std::vector<float> reference;
    std::uint64_t reference_hash = 0;

    std::vector<Answer> answers;
    std::vector<Event> events;
    std::uint64_t dispatches = 0;
    std::uint64_t wrong = 0;
    std::uint64_t worst_ulp = 0;
    double worst_absolute = 0.0;
    std::size_t first_wrong_index = 0;

    // Output values one workgroup is responsible for. The spectrum kernel is
    // one workgroup per coarse channel, so this is the transform's kept half.
    [[nodiscard]] std::size_t group_size() const {
        return std::string_view(kind) == "spectrum"
                   ? dsp::spectrum_bins_per_channel(transform)
                   : 2U;
    }

    [[nodiscard]] std::string label() const {
        // The branch kernel has no transform, so printing an N for it would
        // invent a parameter. Its second number is the decimation.
        if (std::string_view(kind) == "spectrum") {
            return std::format("{} M={} N={} L={}", kind, channels, transform, local_size);
        }
        return std::format("{} M={} D={} L={}", kind, channels, channels / 2U, local_size);
    }
};

[[nodiscard]] std::uint64_t hash_bytes(std::span<const float> values) {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto* bytes = reinterpret_cast<const unsigned char*>(values.data());
    const std::size_t count = values.size() * sizeof(float);
    for (std::size_t i = 0; i < count; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

// "0", "1-11", "0,4,9". Contiguous runs collapse, because the pattern that
// matters most is one round standing alone against all the others.
//
// Repeats collapse as well as increments. In batched mode a round is a whole
// command buffer, so one shape answers many times per round and the list
// arrives as 0,0,0,0,1,1,1,1,... Collapsing only on increment printed every
// one of them: a run of sixty rounds at twenty-four slots came out as a
// fourteen-hundred-entry line that pushed the rest of the report off screen.
[[nodiscard]] std::string describe_rounds(const std::vector<int>& rounds) {
    std::string out;
    for (std::size_t i = 0; i < rounds.size();) {
        std::size_t j = i;
        while (j + 1 < rounds.size() &&
               (rounds[j + 1] == rounds[j] || rounds[j + 1] == rounds[j] + 1)) {
            ++j;
        }
        if (!out.empty()) {
            out += ',';
        }
        out += rounds[j] > rounds[i] ? std::format("{}-{}", rounds[i], rounds[j])
                                     : std::format("{}", rounds[i]);
        i = j + 1;
    }
    return out;
}

// Deterministic, and generated here rather than taken from the test harness
// so this tool depends on nothing that is itself under suspicion.
[[nodiscard]] std::vector<dsp::Complex32> make_input(std::size_t count, std::uint64_t seed) {
    std::vector<dsp::Complex32> values(count, dsp::Complex32{});
    std::uint64_t state = seed;
    for (auto& value : values) {
        const auto next = [&state]() {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            const auto bits = static_cast<std::uint32_t>(state >> 32U);
            // [-1, 1), exactly representable and clear of the denormals this
            // project flushes to zero.
            return static_cast<float>(static_cast<std::int32_t>(bits)) / 2147483648.0F;
        };
        const float real = next();
        const float imag = next();
        value = dsp::Complex32{real, imag};
    }
    return values;
}

[[nodiscard]] std::uint64_t ulp_distance(float a, float b) {
    auto ordered = [](float v) -> std::int64_t {
        const auto raw = static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(v));
        return raw < 0 ? static_cast<std::int64_t>(0x80000000LL) - raw : raw;
    };
    const std::int64_t ia = ordered(a);
    const std::int64_t ib = ordered(b);
    return static_cast<std::uint64_t>(ia > ib ? ia - ib : ib - ia);
}

struct SpectrumPush {
    std::uint32_t chan_blocks = 0;
    std::uint32_t chan_mask = 0;
    std::uint32_t in_offset = 0;
};

// Per-channel ring capacity, in blocks.
//
// It has to exceed the largest transform in the list, not merely hold it. A
// 1024-point window out of a 512-block ring reads some samples twice, which
// the twin refuses and the kernel does not, so the first version of this tool
// dispatched two shapes the reference considers meaningless and compared them
// against nothing. 4096 leaves headroom above the 2048-point ceiling.
constexpr std::uint32_t kChanBlocks = 4096;

// Both offsets sit near the top of the ring so that every window wraps, which
// is where the modular arithmetic is worth exercising.
constexpr std::uint32_t kSpectrumInOffset = kChanBlocks - 37U;
constexpr std::uint32_t kBranchBlocks = 8;
constexpr std::uint32_t kBranchBaseOffset = kChanBlocks - 40U;
constexpr std::uint32_t kBranchTapsPerBranch = 17;

[[nodiscard]] dsp::SpectrumParams spectrum_params_of(const Shape& shape) {
    dsp::SpectrumParams params;
    params.channels = shape.channels;
    params.transform = shape.transform;
    params.stages = dsp::fft_stages(shape.transform);
    params.chan_blocks = kChanBlocks;
    params.chan_mask = kChanBlocks - 1U;
    params.in_offset = kSpectrumInOffset;
    return params;
}

[[nodiscard]] dsp::PfbBranchParams branch_params() {
    return dsp::PfbBranchParams{
        .ring_mask = kChanBlocks - 1U,
        .base_offset = kBranchBaseOffset,
        .block_count = kBranchBlocks,
    };
}

[[nodiscard]] Status prepare(Shape& shape, std::uint64_t seed) {
    shape.input = make_input(static_cast<std::size_t>(shape.channels) * kChanBlocks, seed);

    if (std::string_view(shape.kind) == "spectrum") {
        auto twiddles = dsp::build_twiddles(shape.transform);
        if (!twiddles) {
            return std::unexpected(twiddles.error());
        }
        shape.twiddles = std::move(*twiddles);

        auto window = dsp::build_spectrum_window(shape.transform);
        if (!window) {
            return std::unexpected(window.error());
        }
        shape.window = std::move(*window);

        shape.reference.assign(dsp::spectrum_bin_count(shape.channels, shape.transform), 0.0F);
        if (auto ok = dsp::reference_spectrum(spectrum_params_of(shape), shape.input,
                                              shape.twiddles, shape.window, shape.reference);
            !ok) {
            return ok;
        }
        shape.reference_hash = hash_bytes(shape.reference);
        return {};
    }

    shape.grid.channels = shape.channels;
    shape.grid.taps_per_branch = kBranchTapsPerBranch;
    shape.grid.decimation = shape.channels / 2U;
    auto prototype = dsp::design_prototype(shape.grid, 120.0);
    if (!prototype) {
        return std::unexpected(prototype.error());
    }
    shape.taps = std::move(prototype->taps);

    std::vector<dsp::Complex32> twin(
        static_cast<std::size_t>(kBranchBlocks) * shape.channels, dsp::Complex32{});
    if (auto ok = dsp::reference_pfb_branches(shape.grid, shape.taps, shape.input,
                                              branch_params(), twin);
        !ok) {
        return ok;
    }
    shape.reference.assign(twin.size() * 2, 0.0F);
    std::memcpy(shape.reference.data(), twin.data(), twin.size() * sizeof(dsp::Complex32));
    shape.reference_hash = hash_bytes(shape.reference);
    return {};
}

[[nodiscard]] Expected<std::vector<float>> dispatch_spectrum(const gpu::Context& context,
                                                             const Shape& shape) {
    std::vector<float> out(dsp::spectrum_bin_count(shape.channels, shape.transform), 0.0F);

    const SpectrumPush push{
        .chan_blocks = kChanBlocks,
        .chan_mask = kChanBlocks - 1U,
        .in_offset = kSpectrumInOffset,
    };
    const std::uint32_t constants[] = {shape.channels, shape.transform,
                                       dsp::fft_stages(shape.transform)};

    gpu::KernelInvocation invocation;
    invocation.spirv = gpu::shaders::spectrum();
    invocation.inputs = {std::as_bytes(std::span<const dsp::Complex32>(shape.input)),
                         std::as_bytes(std::span<const dsp::Complex32>(shape.twiddles)),
                         std::as_bytes(std::span<const float>(shape.window))};
    invocation.outputs = {std::as_writable_bytes(std::span<float>(out))};
    invocation.push_constants = std::as_bytes(std::span<const SpectrumPush>(&push, 1));
    invocation.local_size_x = shape.local_size;
    invocation.group_count_x = shape.channels;
    invocation.invocations = shape.channels * shape.local_size;
    invocation.grid_constants.assign(std::begin(constants), std::end(constants));

    if (auto ran = gpu::run_kernel(context, invocation); !ran) {
        return std::unexpected(ran.error());
    }
    return out;
}

// The channelizer's branch filter: the control.
//
// It reads the same ring under the same driver and has no shared-memory
// transform, so it separates "this device" from "this kernel". It has now
// answered: 40,000 dispatches on the integrated Radeon, interleaved with the
// spectrum dispatches that were failing, all bit-exact.
[[nodiscard]] Expected<std::vector<float>> dispatch_branch(const gpu::Context& context,
                                                           const Shape& shape) {
    std::vector<dsp::Complex32> out(
        static_cast<std::size_t>(kBranchBlocks) * shape.channels, dsp::Complex32{});

    const dsp::PfbBranchParams params = branch_params();
    const std::uint32_t constants[] = {shape.grid.channels, shape.grid.taps_per_branch,
                                       shape.grid.decimation};

    gpu::KernelInvocation invocation;
    invocation.spirv = gpu::shaders::pfb_branch();
    invocation.inputs = {std::as_bytes(std::span<const dsp::Complex32>(shape.input)),
                         std::as_bytes(std::span<const float>(shape.taps))};
    invocation.outputs = {std::as_writable_bytes(std::span<dsp::Complex32>(out))};
    invocation.push_constants =
        std::as_bytes(std::span<const dsp::PfbBranchParams>(&params, 1));
    invocation.invocations = kBranchBlocks * shape.channels;
    invocation.local_size_x = shape.local_size;
    invocation.grid_constants.assign(std::begin(constants), std::end(constants));

    if (auto ran = gpu::run_kernel(context, invocation); !ran) {
        return std::unexpected(ran.error());
    }

    // Reinterpreted as floats only so one comparison path serves both kinds.
    // Nothing here cares what the numbers mean, only whether they changed.
    std::vector<float> flat(out.size() * 2, 0.0F);
    std::memcpy(flat.data(), out.data(), out.size() * sizeof(dsp::Complex32));
    return flat;
}

// One dispatch's answer against its shape's twin.
//
// Shared by both modes on purpose. The two modes exist to have their rates
// compared, and a rate is only comparable if what counts as a disagreement is
// literally the same code on both sides.
//
// `slot` is where the dispatch sat in its command buffer, zero in the
// unbatched mode because there it is the only one. It is on the EVENT line
// because the first thing to rule out about a batched failure is that it is
// always the same position, which would point at the recording rather than at
// the device: position zero in particular would accuse the fill barrier at
// the top of the command buffer.
[[nodiscard]] Status record_answer(Shape& shape, std::span<const float> out, int round,
                                   int slot) {
    ++shape.dispatches;

    if (out.size() != shape.reference.size()) {
        return fail(std::format("{} produced {} values against the twin's {}", shape.label(),
                                out.size(), shape.reference.size()));
    }

    const std::uint64_t hash = hash_bytes(out);
    if (hash != shape.reference_hash) {
        ++shape.wrong;

        Event event;
        event.round = round;
        const std::size_t group = shape.group_size();
        std::size_t last_group = static_cast<std::size_t>(-1);
        std::uint32_t xor_bits = 0;

        for (std::size_t i = 0; i < out.size(); ++i) {
            if (out[i] == shape.reference[i]) {
                continue;
            }
            if (event.values == 0) {
                event.first_index = i;
                if (shape.worst_ulp == 0) {
                    shape.first_wrong_index = i;
                }
            }
            ++event.values;
            if (out[i] == 0.0F) {
                ++event.unwritten;
            }
            const std::size_t g = i / group;
            if (g != last_group) {
                ++event.groups;
                last_group = g;
            }
            xor_bits = std::bit_cast<std::uint32_t>(out[i]) ^
                       std::bit_cast<std::uint32_t>(shape.reference[i]);

            const std::uint64_t ulp = ulp_distance(out[i], shape.reference[i]);
            event.worst_ulp = std::max(event.worst_ulp, ulp);
            shape.worst_ulp = std::max(shape.worst_ulp, ulp);
            shape.worst_absolute = std::max(
                shape.worst_absolute, std::abs(static_cast<double>(out[i]) -
                                               static_cast<double>(shape.reference[i])));
        }

        // Only meaningful for a lone corrupted value. With a spray it is the
        // last one's popcount and says nothing, so it is left at zero rather
        // than printed as if it did.
        if (event.values == 1) {
            event.flipped_bits = std::popcount(xor_bits);
        }

        // Machine-readable, one line per event, so a sweep over many processes
        // can be aggregated without parsing the table.
        std::println("EVENT {} round={} slot={} values={}/{} unwritten={} groups={}/{} "
                     "first={} ulp={} bits={}",
                     shape.label(), event.round, slot, event.values, shape.reference.size(),
                     event.unwritten, event.groups, shape.reference.size() / group,
                     event.first_index, event.worst_ulp, event.flipped_bits);
        shape.events.push_back(event);
    }

    auto found = std::find_if(shape.answers.begin(), shape.answers.end(),
                              [hash](const Answer& a) { return a.hash == hash; });
    if (found != shape.answers.end()) {
        found->rounds.push_back(round);
        return {};
    }

    Answer answer;
    answer.hash = hash;
    answer.data.assign(out.begin(), out.end());
    answer.rounds.push_back(round);
    shape.answers.push_back(std::move(answer));
    return {};
}

// --- the batched mode ------------------------------------------------------

// Everything one shape needs resident on the device, built once.
struct ShapeDevice {
    gpu::ComputePipeline pipeline;
    std::vector<gpu::Buffer> inputs;
    std::vector<VkBuffer> input_handles;
    std::vector<std::byte> push;
    std::uint32_t groups = 0;
    std::size_t output_floats = 0;
};

// One dispatch position inside a batch.
//
// Each position owns its own output and readback buffer, which is the whole
// reason this mode can say anything. Slots sharing one output would leave only
// the last dispatch of a batch to compare, and one answer per submission is
// the measurement the unbatched mode already makes.
struct BatchSlot {
    gpu::Buffer output;
    gpu::Buffer readback;
};

// Copies host bytes into a device-local storage buffer, once, at setup.
[[nodiscard]] Expected<gpu::Buffer> device_copy(const gpu::Context& context,
                                                gpu::CommandRunner& runner,
                                                std::span<const std::byte> bytes) {
    constexpr VkBufferUsageFlags kUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    auto resident = gpu::Buffer::create(context, bytes.size(), kUsage,
                                        gpu::MemoryKind::DeviceLocal);
    if (!resident) {
        return std::unexpected(resident.error());
    }
    auto staging = gpu::Buffer::create(context, bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       gpu::MemoryKind::Upload);
    if (!staging) {
        return std::unexpected(staging.error());
    }
    if (auto wrote = staging->write(bytes); !wrote) {
        return std::unexpected(wrote.error());
    }

    const gpu::CommandRunner::BufferCopy copy{.source = staging->handle(),
                                              .destination = resident->handle(),
                                              .bytes = resident->size()};
    if (auto copied = runner.copy(std::span<const gpu::CommandRunner::BufferCopy>(&copy, 1));
        !copied) {
        return std::unexpected(copied.error());
    }
    return resident;
}

// The engine's dispatch pattern, which gpu::run_kernel is not.
//
// run_kernel differs from core/engine/graph.cpp at every level at once. It
// builds a pipeline per call, allocates its buffers per call, and makes four
// submissions each behind its own fence. The graph builds pipelines and
// buffers once, records the whole chain into one command buffer with a global
// memory barrier between stages, and submits it once. A rate measured through
// the first says nothing about the second, and the second is what the product
// runs, so this is the gap between the number in docs/fft.md and the number
// somebody watching a waterfall sees.
//
// It records through core/engine/record_util.h, the same three helpers the
// graph and the receiver stages record through, rather than through a second
// implementation written to resemble them.
//
// One knob, the slot count. At a slot count of one this is still an engine-
// shaped dispatch, persistent pipeline and persistent buffers and its own
// command buffer, with nothing batched into it. That separates "many
// dispatches in one command buffer" from "not going through run_kernel",
// which are two changes at once and would otherwise be confounded.
class BatchRunner {
public:
    [[nodiscard]] static Expected<std::unique_ptr<BatchRunner>> create(
        const gpu::Context& context, std::span<const Shape> shapes, std::size_t slots);

    BatchRunner() = default;
    ~BatchRunner();

    BatchRunner(const BatchRunner&) = delete;
    BatchRunner& operator=(const BatchRunner&) = delete;
    BatchRunner(BatchRunner&&) = delete;
    BatchRunner& operator=(BatchRunner&&) = delete;

    // Records plan.size() dispatches into one command buffer with a barrier
    // between each pair, submits once, and waits once. plan[i] is the shape
    // slot i runs.
    [[nodiscard]] Status dispatch(std::span<const std::size_t> plan);

    [[nodiscard]] Status read(std::size_t slot, std::span<float> out) const;

private:
    const gpu::Context* context_ = nullptr;
    std::vector<ShapeDevice> shapes_;
    std::vector<BatchSlot> slots_;

    // [slot][shape], written once at setup and never touched again, the way
    // the graph writes a frame's sets when the frame is built. Updating a
    // descriptor per dispatch would put the update inside the measurement.
    std::vector<std::vector<VkDescriptorSet>> sets_;

    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptors_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
};

BatchRunner::~BatchRunner() {
    if (context_ == nullptr) {
        return;
    }
    const VkDevice device = context_->device();
    if (device == VK_NULL_HANDLE) {
        return;
    }
    // Before the members, which is the order the body and the implicit member
    // destruction already give: the pools go here, the buffers and pipelines
    // they point at go after this returns.
    if (fence_ != VK_NULL_HANDLE) {
        vkDestroyFence(device, fence_, nullptr);
    }
    if (descriptors_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, descriptors_, nullptr);
    }
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, pool_, nullptr);
    }
}

Expected<std::unique_ptr<BatchRunner>> BatchRunner::create(const gpu::Context& context,
                                                           std::span<const Shape> shapes,
                                                           std::size_t slots) {
    if (shapes.empty() || slots == 0) {
        return fail("BatchRunner::create needs at least one shape and one slot");
    }

    auto setup = gpu::CommandRunner::create(context);
    if (!setup) {
        return std::unexpected(with_context(setup.error(), "batch setup"));
    }

    auto batch = std::make_unique<BatchRunner>();
    batch->context_ = &context;
    batch->shapes_.reserve(shapes.size());

    std::size_t widest = 0;
    std::uint32_t bindings_total = 0;

    for (const Shape& shape : shapes) {
        const bool is_spectrum = std::string_view(shape.kind) == "spectrum";
        ShapeDevice resident;

        std::uint32_t constants[3] = {0, 0, 0};
        std::uint32_t binding_count = 0;
        std::vector<std::span<const std::byte>> sources;

        if (is_spectrum) {
            constants[0] = shape.channels;
            constants[1] = shape.transform;
            constants[2] = dsp::fft_stages(shape.transform);
            binding_count = 4;

            const SpectrumPush push{.chan_blocks = kChanBlocks,
                                    .chan_mask = kChanBlocks - 1U,
                                    .in_offset = kSpectrumInOffset};
            const auto raw = std::as_bytes(std::span<const SpectrumPush>(&push, 1));
            resident.push.assign(raw.begin(), raw.end());

            // One workgroup per coarse channel, exactly as
            // core/engine/graph.cpp records it.
            resident.groups = shape.channels;
            resident.output_floats = dsp::spectrum_bin_count(shape.channels, shape.transform);
            sources = {std::as_bytes(std::span<const dsp::Complex32>(shape.input)),
                       std::as_bytes(std::span<const dsp::Complex32>(shape.twiddles)),
                       std::as_bytes(std::span<const float>(shape.window))};
        } else {
            constants[0] = shape.grid.channels;
            constants[1] = shape.grid.taps_per_branch;
            constants[2] = shape.grid.decimation;
            binding_count = 3;

            const dsp::PfbBranchParams params = branch_params();
            const auto raw = std::as_bytes(std::span<const dsp::PfbBranchParams>(&params, 1));
            resident.push.assign(raw.begin(), raw.end());

            resident.groups = engine::group_count(
                static_cast<std::uint64_t>(kBranchBlocks) * shape.channels, shape.local_size);
            resident.output_floats =
                static_cast<std::size_t>(kBranchBlocks) * shape.channels * 2U;
            sources = {std::as_bytes(std::span<const dsp::Complex32>(shape.input)),
                       std::as_bytes(std::span<const float>(shape.taps))};
        }

        auto pipeline = gpu::ComputePipeline::create(
            context, gpu::ComputePipeline::Options{
                         .spirv = is_spectrum ? gpu::shaders::spectrum()
                                              : gpu::shaders::pfb_branch(),
                         .storage_buffer_count = binding_count,
                         .local_size_x = shape.local_size,
                         .push_constant_bytes =
                             static_cast<std::uint32_t>(resident.push.size()),
                         .entry_point = "main",
                         .grid_constants = constants,
                     });
        if (!pipeline) {
            return std::unexpected(
                with_context(pipeline.error(), std::format("batch pipeline for {}",
                                                           shape.label())));
        }
        resident.pipeline = std::move(*pipeline);

        for (const auto& source : sources) {
            auto buffer = device_copy(context, *setup, source);
            if (!buffer) {
                return std::unexpected(
                    with_context(buffer.error(), std::format("batch input for {}",
                                                             shape.label())));
            }
            resident.input_handles.push_back(buffer->handle());
            resident.inputs.push_back(std::move(*buffer));
        }

        widest = std::max(widest, resident.output_floats);
        bindings_total += binding_count;
        batch->shapes_.push_back(std::move(resident));
    }

    // Every slot is sized for the widest shape, because any shape can land in
    // any slot and a per-slot size would pin the rotation to the slot count.
    const auto slot_bytes = static_cast<VkDeviceSize>(widest) * sizeof(float);
    batch->slots_.reserve(slots);
    for (std::size_t i = 0; i < slots; ++i) {
        auto output = gpu::Buffer::create(context, slot_bytes,
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          gpu::MemoryKind::DeviceLocal);
        if (!output) {
            return std::unexpected(with_context(output.error(), "batch output buffer"));
        }
        auto readback = gpu::Buffer::create(context, slot_bytes,
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                            gpu::MemoryKind::Readback);
        if (!readback) {
            return std::unexpected(with_context(readback.error(), "batch readback buffer"));
        }
        batch->slots_.push_back(
            BatchSlot{.output = std::move(*output), .readback = std::move(*readback)});
    }

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = context.compute_family();

    VkResult result = vkCreateCommandPool(context.device(), &pool_info, nullptr, &batch->pool_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkCreateCommandPool failed ({})", gpu::result_name(result)),
                    result);
    }

    VkCommandBufferAllocateInfo command_alloc{};
    command_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_alloc.commandPool = batch->pool_;
    command_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_alloc.commandBufferCount = 1;

    result = vkAllocateCommandBuffers(context.device(), &command_alloc, &batch->commands_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkAllocateCommandBuffers failed ({})", gpu::result_name(result)),
                    result);
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    result = vkCreateFence(context.device(), &fence_info, nullptr, &batch->fence_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkCreateFence failed ({})", gpu::result_name(result)), result);
    }

    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    size.descriptorCount = static_cast<std::uint32_t>(slots) * bindings_total;

    VkDescriptorPoolCreateInfo descriptor_info{};
    descriptor_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptor_info.maxSets = static_cast<std::uint32_t>(slots * shapes.size());
    descriptor_info.poolSizeCount = 1;
    descriptor_info.pPoolSizes = &size;

    result = vkCreateDescriptorPool(context.device(), &descriptor_info, nullptr,
                                    &batch->descriptors_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkCreateDescriptorPool failed ({})", gpu::result_name(result)),
                    result);
    }

    batch->sets_.assign(slots, std::vector<VkDescriptorSet>(shapes.size(), VK_NULL_HANDLE));
    for (std::size_t slot = 0; slot < slots; ++slot) {
        for (std::size_t k = 0; k < batch->shapes_.size(); ++k) {
            VkDescriptorSetLayout layout = batch->shapes_[k].pipeline.descriptor_layout();

            VkDescriptorSetAllocateInfo set_alloc{};
            set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            set_alloc.descriptorPool = batch->descriptors_;
            set_alloc.descriptorSetCount = 1;
            set_alloc.pSetLayouts = &layout;

            VkDescriptorSet set = VK_NULL_HANDLE;
            result = vkAllocateDescriptorSets(context.device(), &set_alloc, &set);
            if (result != VK_SUCCESS) {
                return fail(std::format("vkAllocateDescriptorSets failed ({})",
                                        gpu::result_name(result)),
                            result);
            }

            std::vector<VkBuffer> bound = batch->shapes_[k].input_handles;
            bound.push_back(batch->slots_[slot].output.handle());
            if (auto wrote = engine::write_storage_set(context.device(), set, bound); !wrote) {
                return std::unexpected(with_context(wrote.error(), "batch descriptor set"));
            }
            batch->sets_[slot][k] = set;
        }
    }

    return batch;
}

Status BatchRunner::dispatch(std::span<const std::size_t> plan) {
    if (plan.empty() || plan.size() > slots_.size()) {
        return fail(std::format("a batch of {} does not fit {} slots", plan.size(),
                                slots_.size()));
    }
    const VkDevice device = context_->device();

    VkResult result = vkResetCommandBuffer(commands_, 0);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkResetCommandBuffer failed ({})", gpu::result_name(result)),
                    result);
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    result = vkBeginCommandBuffer(commands_, &begin);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkBeginCommandBuffer failed ({})", gpu::result_name(result)),
                    result);
    }

    // Zero every slot at the top of the batch, for the reason
    // gpu::CommandRunner::clear gives at length. Here it matters more than it
    // does there: without it a kernel that wrote only part of its output would
    // be compared against the previous batch's answer in the same slot, and
    // for a kernel that is right nearly every time that is the twin's answer,
    // so the failure this tool exists to count would be the one thing it could
    // not see.
    for (const BatchSlot& slot : slots_) {
        vkCmdFillBuffer(commands_, slot.output.handle(), 0, slot.output.size(), 0U);
    }
    engine::record_barrier(commands_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    for (std::size_t i = 0; i < plan.size(); ++i) {
        const ShapeDevice& resident = shapes_[plan[i]];
        engine::record_dispatch(commands_, resident.pipeline, sets_[i][plan[i]], resident.push,
                                resident.groups);

        // core/engine/graph.cpp's barrier after the spectrum dispatch, copied:
        // shader writes to shader reads and transfer reads, whole buffer. It
        // is also the execution dependency that separates this dispatch from
        // the next one, which is the property under test.
        engine::record_barrier(commands_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_ACCESS_SHADER_WRITE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);

        VkBufferCopy region{};
        region.srcOffset = 0;
        region.dstOffset = 0;
        region.size = static_cast<VkDeviceSize>(resident.output_floats) * sizeof(float);
        vkCmdCopyBuffer(commands_, slots_[i].output.handle(), slots_[i].readback.handle(), 1,
                        &region);
    }

    engine::record_barrier(commands_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                           VK_ACCESS_HOST_READ_BIT);

    result = vkEndCommandBuffer(commands_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkEndCommandBuffer failed ({})", gpu::result_name(result)),
                    result);
    }

    result = vkResetFences(device, 1, &fence_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkResetFences failed ({})", gpu::result_name(result)), result);
    }

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands_;

    result = context_->submit(submit, fence_);
    if (result != VK_SUCCESS) {
        return fail(std::format("vkQueueSubmit failed ({})", gpu::result_name(result)), result);
    }

    // The same generous but finite wait gpu::CommandRunner takes, and for the
    // same reason: an infinite wait on a hung device takes the run down with
    // it and reports nothing.
    constexpr std::uint64_t kTimeoutNs = 30ULL * 1000ULL * 1000ULL * 1000ULL;
    result = vkWaitForFences(device, 1, &fence_, VK_TRUE, kTimeoutNs);
    if (result == VK_TIMEOUT) {
        return fail("a batch did not complete within 30 seconds", result);
    }
    if (result != VK_SUCCESS) {
        return fail(std::format("vkWaitForFences failed ({})", gpu::result_name(result)), result);
    }
    return {};
}

Status BatchRunner::read(std::size_t slot, std::span<float> out) const {
    if (slot >= slots_.size()) {
        return fail(std::format("slot {} is outside a batch of {}", slot, slots_.size()));
    }
    return slots_[slot].readback.read(std::as_writable_bytes(out));
}

[[nodiscard]] std::int64_t parse_int(std::string_view text, std::int64_t fallback) {
    std::int64_t value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    if (std::from_chars(begin, end, value).ec != std::errc{}) {
        return fallback;
    }
    return value;
}

// Comma-separated shape indices, for bisecting which predecessors matter.
[[nodiscard]] std::vector<int> parse_selection(std::string_view text) {
    std::vector<int> picked;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        const std::string_view item = text.substr(start, end - start);
        if (!item.empty()) {
            picked.push_back(static_cast<int>(parse_int(item, -1)));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return picked;
}

void print_usage() {
    std::print(
        "gpustress: dispatch varied shapes and report which answers change\n"
        "\n"
        "Usage: gpustress [--gpu <n>] [--rounds <n>] [--seed <n>] [--shapes <list>]\n"
        "                 [--batch <n>]\n"
        "\n"
        "  --gpu <n>       Device index, default -1 which honours REVENANT_GPU_INDEX.\n"
        "  --rounds <n>    Passes over the shape list, default 40. In batched mode a\n"
        "                  round is one command buffer, so a round is --batch\n"
        "                  dispatches rather than one per shape.\n"
        "  --seed <n>      Input seed, default 20260919.\n"
        "  --shapes <list> Comma-separated indices from --list, dispatched in the\n"
        "                  order given. This is the bisection handle: it is how you\n"
        "                  find which predecessor a contaminated shape needs.\n"
        "  --batch <n>     Record n dispatches into ONE command buffer with a pipeline\n"
        "                  barrier between each pair and submit once, which is how\n"
        "                  core/engine/graph.cpp dispatches. Persistent pipelines and\n"
        "                  persistent buffers come with it, also as the graph has\n"
        "                  them. Default 0, which keeps the one-dispatch-per-command-\n"
        "                  buffer path through gpu::run_kernel. --batch 1 is the\n"
        "                  control that tells the two changes apart.\n"
        "  --list          Print the shape list with its indices and exit.\n"
        "\n"
        "Each shape's answers are grouped, and the report names the rounds that\n"
        "produced each one. Two answers where the first stands alone against the\n"
        "rest is a different fault from an answer that is fresh every dispatch.\n"
        "\n"
        "Exits 1 if any shape gave more than one answer.\n"
        "See docs/fft.md for what this is chasing and why the conformance\n"
        "matrix currently skips this kernel on non-discrete devices.\n");
}

}  // namespace

int main(int argc, char** argv) {
    int device_index = -1;
    std::int64_t rounds = 40;
    std::uint64_t seed = 20260919;
    std::vector<int> selection;
    std::int64_t batch_size = 0;
    bool list_only = false;

    // Each slot costs a device buffer and a readback buffer sized for the
    // widest shape, so an unbounded slot count is an unbounded allocation. The
    // engine's own command buffer holds a handful of dispatches plus two per
    // receiver, so nothing this tool needs to imitate is anywhere near this.
    constexpr std::int64_t kMaxBatch = 256;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_next = i + 1 < argc;
        if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        }
        if (arg == "--list") {
            list_only = true;
            continue;
        }
        if (arg == "--gpu" && has_next) {
            device_index = static_cast<int>(parse_int(argv[++i], -1));
            continue;
        }
        if (arg == "--rounds" && has_next) {
            rounds = parse_int(argv[++i], rounds);
            continue;
        }
        if (arg == "--seed" && has_next) {
            seed = static_cast<std::uint64_t>(parse_int(argv[++i], 20260919));
            continue;
        }
        if (arg == "--shapes" && has_next) {
            selection = parse_selection(argv[++i]);
            continue;
        }
        if (arg == "--batch" && has_next) {
            batch_size = parse_int(argv[++i], batch_size);
            continue;
        }
        std::println(stderr, "gpustress: unrecognised argument '{}'", arg);
        return 2;
    }

    if (batch_size < 0 || batch_size > kMaxBatch) {
        std::println(stderr, "gpustress: --batch must be between 0 and {}", kMaxBatch);
        return 2;
    }

    gpu::Context::Options options;
    options.device_index = device_index;
    auto context = gpu::Context::create(options);
    if (!context) {
        std::println(stderr, "gpustress: {}", context.error().message);
        return 1;
    }
    std::println("device    {}", context->info().describe());
    std::println("shared    {} bytes of workgroup memory",
                 context->info().max_workgroup_shared_memory);

    const std::uint32_t ceiling =
        dsp::max_spectrum_transform_size(context->info().max_workgroup_shared_memory);

    // Both channel counts, both ends of the workgroup range, and every
    // transform size the device can hold, so that one run says something
    // about the shipped shape (M=64 N=2048 L=256) and about the narrow grids
    // the test suite pinned itself to. The two branch entries are the control
    // described above.
    std::vector<Shape> shapes;
    const struct {
        const char* kind;
        std::uint32_t channels;
        std::uint32_t transform;
        std::uint32_t local_size;
    } wanted[] = {
        {"spectrum", 8, 256, 1},     {"spectrum", 8, 256, 32},
        {"spectrum", 8, 256, 64},    {"spectrum", 8, 256, 128},
        {"spectrum", 8, 512, 128},   {"spectrum", 8, 1024, 256},
        {"spectrum", 8, 2048, 64},   {"spectrum", 64, 256, 128},
        {"spectrum", 64, 2048, 64},  {"spectrum", 64, 2048, 256},
        {"branch", 64, 0, 64},       {"branch", 8, 0, 32},
    };

    const int wanted_count = static_cast<int>(std::size(wanted));
    if (list_only) {
        for (int i = 0; i < wanted_count; ++i) {
            Shape shape;
            shape.kind = wanted[i].kind;
            shape.channels = wanted[i].channels;
            shape.transform = wanted[i].transform;
            shape.local_size = wanted[i].local_size;
            std::println("  {:>2}  {}", i, shape.label());
        }
        return 0;
    }

    if (selection.empty()) {
        for (int i = 0; i < wanted_count; ++i) {
            selection.push_back(i);
        }
    }

    for (const int index : selection) {
        if (index < 0 || index >= wanted_count) {
            std::println(stderr, "gpustress: shape index {} is not in 0..{}", index,
                         wanted_count - 1);
            return 2;
        }
        const auto& item = wanted[index];
        if (std::string_view(item.kind) == "spectrum" && item.transform > ceiling) {
            std::println("skip      shape {}, {} points is above this device's {} point "
                         "ceiling", index, item.transform, ceiling);
            continue;
        }
        Shape shape;
        shape.kind = item.kind;
        shape.channels = item.channels;
        shape.transform = item.transform;
        shape.local_size = item.local_size;
        if (auto ok = prepare(shape, seed); !ok) {
            std::println(stderr, "gpustress: preparing {}: {}", shape.label(),
                         ok.error().message);
            return 1;
        }
        shapes.push_back(std::move(shape));
    }

    if (shapes.empty()) {
        std::println(stderr, "gpustress: no shape survived selection");
        return 2;
    }

    std::println("shapes    {}, {} rounds, seed {}", shapes.size(), rounds, seed);
    if (batch_size > 0) {
        std::println("mode      batched, {} dispatches per command buffer with barriers "
                     "between them, one submit and one fence wait per round",
                     batch_size);
    } else {
        std::println("mode      unbatched, one dispatch per command buffer through "
                     "gpu::run_kernel");
    }
    std::println("");

    if (batch_size > 0) {
        auto batch = BatchRunner::create(*context, shapes, static_cast<std::size_t>(batch_size));
        if (!batch) {
            std::println(stderr, "gpustress: {}", batch.error().message);
            return 1;
        }

        // Slot i of round r runs shape (r*slots + i) mod shapes. The rotation
        // walks across rounds rather than resetting inside one, so a batch
        // narrower than the shape list still reaches every shape. That is what
        // makes --batch 1 a control over the same twelve shapes rather than
        // twelve thousand dispatches of shape zero.
        std::vector<std::size_t> plan(static_cast<std::size_t>(batch_size), 0);
        std::vector<float> scratch;
        for (std::int64_t round = 0; round < rounds; ++round) {
            for (std::size_t i = 0; i < plan.size(); ++i) {
                const std::uint64_t step = static_cast<std::uint64_t>(round) * plan.size() + i;
                plan[i] = static_cast<std::size_t>(step % shapes.size());
            }

            if (auto ok = (*batch)->dispatch(plan); !ok) {
                std::println(stderr, "gpustress: {}", ok.error().message);
                return 1;
            }

            for (std::size_t i = 0; i < plan.size(); ++i) {
                Shape& shape = shapes[plan[i]];
                scratch.assign(shape.reference.size(), 0.0F);
                if (auto ok = (*batch)->read(i, scratch); !ok) {
                    std::println(stderr, "gpustress: {}", ok.error().message);
                    return 1;
                }
                if (auto ok = record_answer(shape, scratch, static_cast<int>(round),
                                            static_cast<int>(i));
                    !ok) {
                    std::println(stderr, "gpustress: {}", ok.error().message);
                    return 1;
                }
            }
        }
    } else {
        for (std::int64_t round = 0; round < rounds; ++round) {
            for (Shape& shape : shapes) {
                auto produced = std::string_view(shape.kind) == "spectrum"
                                    ? dispatch_spectrum(*context, shape)
                                    : dispatch_branch(*context, shape);
                if (!produced) {
                    std::println(stderr, "gpustress: {}", produced.error().message);
                    return 1;
                }
                if (auto ok = record_answer(shape, *produced, static_cast<int>(round), 0); !ok) {
                    std::println(stderr, "gpustress: {}", ok.error().message);
                    return 1;
                }
            }
        }
    }

    std::println("{:<26} {:>7} {:>8} {:>9} {:>12}  {}", "shape", "runs", "answers",
                 "vs twin", "worst ulp", "rounds per answer");
    std::size_t unstable = 0;
    std::size_t incorrect = 0;
    for (const Shape& shape : shapes) {
        if (shape.answers.size() > 1) {
            ++unstable;
        }
        if (shape.wrong > 0) {
            ++incorrect;
        }
        std::string pattern;
        for (const Answer& answer : shape.answers) {
            if (!pattern.empty()) {
                pattern += " | ";
            }
            pattern += describe_rounds(answer.rounds);
        }
        std::println("{:<26} {:>7} {:>8} {:>9} {:>12}  {}", shape.label(), shape.dispatches,
                     shape.answers.size(), shape.wrong, shape.worst_ulp, pattern);
    }

    for (const Shape& shape : shapes) {
        if (shape.wrong == 0) {
            continue;
        }
        const std::size_t i = shape.first_wrong_index;
        const auto bad = std::find_if(
            shape.answers.begin(), shape.answers.end(),
            [&shape](const Answer& a) { return a.hash != shape.reference_hash; });
        std::println("");
        std::println("{}: {} of {} dispatches disagreed with the twin. First at index {} "
                     "of {}, twin {} against device {}. Worst {} ulp, {:g} absolute.",
                     shape.label(), shape.wrong, shape.dispatches, i, shape.reference.size(),
                     shape.reference[i], bad->data[i], shape.worst_ulp,
                     shape.worst_absolute);

        std::size_t least = shape.reference.size();
        std::size_t most = 0;
        std::size_t total = 0;
        std::size_t widest_groups = 0;
        std::size_t single_bit = 0;
        std::size_t unwritten = 0;
        std::size_t all_unwritten = 0;
        for (const Event& event : shape.events) {
            least = std::min(least, event.values);
            most = std::max(most, event.values);
            total += event.values;
            widest_groups = std::max(widest_groups, event.groups);
            unwritten += event.unwritten;
            if (event.unwritten == event.values) {
                ++all_unwritten;
            }
            if (event.flipped_bits == 1) {
                ++single_bit;
            }
        }
        std::println("  blast radius: {} to {} values per event, mean {:.1f} of {}. "
                     "Widest spread {} of {} workgroups. Single-bit events: {}.",
                     least, most,
                     static_cast<double>(total) / static_cast<double>(shape.events.size()),
                     shape.reference.size(), widest_groups,
                     shape.reference.size() / shape.group_size(), single_bit);
        std::println("  unwritten: {} of {} wrong values were still the pre-dispatch fill, "
                     "and {} of {} events were nothing but fill.",
                     unwritten, total, all_unwritten, shape.events.size());
    }

    std::println("");
    if (unstable == 0 && incorrect == 0) {
        std::println("every shape gave one answer and it matched the twin, {} rounds",
                     rounds);
        return 0;
    }

    // Said separately because they are separate faults and the fix for one is
    // not the fix for the other. A shape that is stable and wrong is carrying
    // something from the dispatches before it, which is what docs/fft.md
    // records. A shape that is unstable is producing a different answer from
    // the same input on the same device, which is a narrower and worse thing.
    if (incorrect > 0) {
        std::println("{} of {} shapes disagreed with the CPU twin.", incorrect,
                     shapes.size());
    }
    if (unstable > 0) {
        std::println("{} of {} shapes gave more than one answer across rounds.", unstable,
                     shapes.size());
    }
    std::println("See docs/fft.md, \"The integrated device, and an anomaly that got less "
                 "deniable\".");
    return 1;
}
