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
// WHY IT IS SHAPED LIKE THIS.
//
// Twelve shapes in rotation rather than one repeated, because the earlier
// reading of this fault blamed the sequence and that had to be testable.
// It is not the sequence, but the rotation stays: it costs nothing and it
// covers the narrow grids, the shipped grid and both workgroup extremes in
// one run.
//
// Each dispatch is compared against its shape's CPU twin, not against the
// shape's previous answer. Comparing answers to each other cannot see a
// kernel that is stably wrong, which is exactly the fault that was being
// looked for, and it makes the first dispatch a baseline that one bad
// dispatch poisons for the whole run. Both mistakes were made here first.
//
// WHAT IT DOES NOT COVER.
//
// One dispatch per command buffer, one fence wait each, through
// gpu::run_kernel. The engine records many dispatches into one command buffer
// with barriers between them and submits once, and on the integrated device
// it appears to corrupt frames far more often than this tool fails
// dispatches. That gap is unexplained. Adding a batched mode is the next
// thing to build here, and it is what would make the tool say anything about
// the pattern the product actually uses.
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
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/dsp/pfb.h"
#include "core/dsp/pfb_branch_reference.h"
#include "core/dsp/spectrum_reference.h"
#include "core/dsp/types.h"
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
[[nodiscard]] std::string describe_rounds(const std::vector<int>& rounds) {
    std::string out;
    for (std::size_t i = 0; i < rounds.size();) {
        std::size_t j = i;
        while (j + 1 < rounds.size() && rounds[j + 1] == rounds[j] + 1) {
            ++j;
        }
        if (!out.empty()) {
            out += ',';
        }
        out += j > i ? std::format("{}-{}", rounds[i], rounds[j]) : std::format("{}", rounds[i]);
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
        "\n"
        "  --gpu <n>       Device index, default -1 which honours REVENANT_GPU_INDEX.\n"
        "  --rounds <n>    Passes over the shape list, default 40.\n"
        "  --seed <n>      Input seed, default 20260919.\n"
        "  --shapes <list> Comma-separated indices from --list, dispatched in the\n"
        "                  order given. This is the bisection handle: it is how you\n"
        "                  find which predecessor a contaminated shape needs.\n"
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
    bool list_only = false;

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
        std::println(stderr, "gpustress: unrecognised argument '{}'", arg);
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
    std::println("");

    for (std::int64_t round = 0; round < rounds; ++round) {
        for (Shape& shape : shapes) {
            auto produced = std::string_view(shape.kind) == "spectrum"
                                ? dispatch_spectrum(*context, shape)
                                : dispatch_branch(*context, shape);
            if (!produced) {
                std::println(stderr, "gpustress: {}", produced.error().message);
                return 1;
            }
            ++shape.dispatches;

            const std::vector<float>& out = *produced;
            if (out.size() != shape.reference.size()) {
                std::println(stderr, "gpustress: {} produced {} values against the twin's {}",
                             shape.label(), out.size(), shape.reference.size());
                return 1;
            }

            const std::uint64_t hash = hash_bytes(out);
            if (hash != shape.reference_hash) {
                ++shape.wrong;

                Event event;
                event.round = static_cast<int>(round);
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
                    shape.worst_absolute =
                        std::max(shape.worst_absolute,
                                 std::abs(static_cast<double>(out[i]) -
                                          static_cast<double>(shape.reference[i])));
                }

                // Only meaningful for a lone corrupted value. With a spray it
                // is the last one's popcount and says nothing, so it is left
                // at zero rather than printed as if it did.
                if (event.values == 1) {
                    event.flipped_bits = std::popcount(xor_bits);
                }

                // Machine-readable, one line per event, so a sweep over many
                // processes can be aggregated without parsing the table.
                std::println("EVENT {} round={} values={}/{} groups={}/{} first={} "
                             "ulp={} bits={}",
                             shape.label(), event.round, event.values,
                             shape.reference.size(), event.groups,
                             shape.reference.size() / group, event.first_index,
                             event.worst_ulp, event.flipped_bits);
                shape.events.push_back(event);
            }

            auto found = std::find_if(shape.answers.begin(), shape.answers.end(),
                                      [hash](const Answer& a) { return a.hash == hash; });
            if (found != shape.answers.end()) {
                found->rounds.push_back(static_cast<int>(round));
                continue;
            }

            Answer answer;
            answer.hash = hash;
            answer.data = std::move(*produced);
            answer.rounds.push_back(static_cast<int>(round));
            shape.answers.push_back(std::move(answer));
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
        for (const Event& event : shape.events) {
            least = std::min(least, event.values);
            most = std::max(most, event.values);
            total += event.values;
            widest_groups = std::max(widest_groups, event.groups);
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
