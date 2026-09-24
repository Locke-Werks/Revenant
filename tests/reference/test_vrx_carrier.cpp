// Carrier recovery for sam and dsb: core/shaders/vrx_carrier.comp against its
// twin, and the twin against the jobs it was built for.
//
// Three kinds of case, as in tests/reference/test_vrx.cpp.
//
//   The bit-exact cases run the kernel against reference_vrx_carrier over a
//   stream cut into dispatches of awkward lengths, with a restart and a
//   retune shift landing in the middle, and compare the rotated ring and the
//   eight words of state after every dispatch. A loop is a recursion, so one
//   wrong bit early is every bit wrong after it, and this is the case that
//   says the device and the twin agree on the whole trajectory.
//
//   The host cases pin the design: which modes run which loop, the gains,
//   the retune rule, and that a loop's output does not depend on where the
//   blocks fell.
//
//   The measurement cases are the figures core/dsp/vrx_reference.h cites:
//   how long each loop takes to lock and how far off it can start, how far
//   it follows a carrier that drifts, whether dsb's level still depends on
//   the carrier phase, and what synchronous detection buys over the envelope
//   when the carrier fades under its own sidebands. They run the twins, the
//   carrier loop and then reference_vrx_demod with the planner's own tables,
//   on complex baseband synthesised at the demodulation rate. Each prints its
//   table with WARN, so a run leaves the numbers in the log.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <numbers>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/dsp/types.h"
#include "core/dsp/vrx_reference.h"
#include "core/engine/listener_level.h"
#include "core/engine/vrx.h"
#include "core/gpu/buffer.h"
#include "core/gpu/context.h"
#include "core/gpu/kernel.h"
#include "core/gpu/shaders.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/support/tone_measure.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kSourceRate = 2'400'000;

// The canonical grid tests/reference/test_vrx.cpp uses.
constexpr dsp::GridParams kGrid{
    .channels = 64,
    .taps_per_branch = 17,
    .decimation = 32,
};

constexpr double kToneHz = 1'000.0;

dsp::VrxPlan make_plan(engine::Demod mode, dsp::Hertz bandwidth) {
    engine::VrxParams params;
    params.center = 196'500;
    params.bandwidth = bandwidth;
    params.demod = mode;

    auto placed = engine::place(kGrid, kSourceRate, params);
    INFO(test::message_of(placed));
    REQUIRE(placed.has_value());

    auto planned = dsp::plan_vrx(kGrid, kSourceRate, params, *placed);
    INFO(test::message_of(planned));
    REQUIRE(planned.has_value());
    return *planned;
}

std::vector<dsp::Complex32> nco_for(const dsp::VrxPlan& plan) {
    auto built = dsp::build_nco_table(plan.carrier.nco_log2);
    REQUIRE(built.has_value());
    return *built;
}

// ---------------------------------------------------------------------------
// Signals, synthesised at the demodulation rate
// ---------------------------------------------------------------------------

// What a signal does at each instant: the carrier's offset from the
// receiver's centre in hertz, and its complex envelope before the offset is
// applied. The phase is accumulated in double from the offset, so a
// frequency that moves is a frequency and not a phase jump.
struct Signal {
    std::function<double(double)> offset_hz = [](double) { return 0.0; };
    std::function<std::complex<double>(double)> envelope;
    double phase = 0.0;
};

// Renders a signal a piece at a time, so a long run need not hold all of it.
class Synth {
public:
    Synth(const Signal& signal, double rate, double noise_rms = 0.0, std::uint64_t seed = 1)
        : signal_(signal), rate_(rate), noise_rms_(noise_rms), phase_(signal.phase),
          engine_(seed), gauss_(0.0, noise_rms / std::numbers::sqrt2) {}

    void next(std::span<dsp::Complex32> out) {
        for (dsp::Complex32& value : out) {
            const double t = static_cast<double>(n_) / rate_;
            std::complex<double> z = signal_.envelope(t) * std::polar(1.0, phase_);
            if (noise_rms_ > 0.0) {
                z += std::complex<double>(gauss_(engine_), gauss_(engine_));
            }
            value = dsp::Complex32{static_cast<float>(z.real()), static_cast<float>(z.imag())};
            phase_ = std::fmod(phase_ + test::kTwoPi * signal_.offset_hz(t) / rate_, test::kTwoPi);
            ++n_;
        }
    }

private:
    Signal signal_;
    double rate_;
    double noise_rms_;
    double phase_;
    std::uint64_t n_ = 0;
    std::mt19937_64 engine_;
    std::normal_distribution<double> gauss_;
};

std::vector<dsp::Complex32> render(const Signal& signal, double rate, std::size_t count,
                                   double noise_rms = 0.0, std::uint64_t seed = 1) {
    std::vector<dsp::Complex32> out(count);
    Synth(signal, rate, noise_rms, seed).next(out);
    return out;
}

// A stand-in for speech on the modulation, for the one measurement a steady
// tone gets wrong: twenty tones spread over 300 to 3000 Hz at fixed random
// frequencies and phases, scaled to a peak of `depth`. A steady tone is a
// line a phase-locked loop can take for the carrier; this has no line
// standing above the others.
std::function<double(double)> programme(double depth, std::uint64_t seed) {
    std::mt19937_64 engine(seed);
    std::uniform_real_distribution<double> frequency(300.0, 3'000.0);
    std::uniform_real_distribution<double> turn(0.0, test::kTwoPi);
    std::vector<std::pair<double, double>> tones(20);
    for (auto& tone : tones) {
        tone = {frequency(engine), turn(engine)};
    }
    // Twenty unit tones peak below twenty; scaling by the sum of amplitudes
    // keeps the modulation inside its depth whatever the phases do.
    const double scale = depth / static_cast<double>(tones.size());
    return [tones, scale](double t) {
        double sum = 0.0;
        for (const auto& [hz, phase] : tones) {
            sum += std::cos(test::kTwoPi * hz * t + phase);
        }
        return scale * sum;
    };
}

// AM of a 1 kHz tone at depth m on a carrier of level `carrier`.
std::function<std::complex<double>(double)> am_tone(double carrier, double m) {
    return [carrier, m](double t) {
        return std::complex<double>(carrier + m * std::cos(test::kTwoPi * kToneHz * t), 0.0);
    };
}

// Double sideband, suppressed carrier, a unit 1 kHz tone.
std::function<std::complex<double>(double)> dsb_tone() {
    return [](double t) { return std::complex<double>(std::cos(test::kTwoPi * kToneHz * t), 0.0); };
}

// A two-path channel applied to an envelope: the direct path plus a copy
// `a` as strong, `delay` later and turned by psi(t). A differential Doppler
// is psi turning at a rate, which is what sweeps a selective fade through
// the carrier on a real ionospheric path.
std::function<std::complex<double>(double)> two_path(
    std::function<std::complex<double>(double)> envelope, double a, double delay,
    std::function<double(double)> psi) {
    return [envelope, a, delay, psi](double t) {
        const std::complex<double> late = (t >= delay) ? envelope(t - delay) : 0.0;
        return envelope(t) + a * late * std::polar(1.0, psi(t));
    };
}

// ---------------------------------------------------------------------------
// The chain: carrier loop, then detector, both twins
// ---------------------------------------------------------------------------

// What the loop said at the end of each block.
struct Probe {
    double at_seconds = 0.0;
    bool locked = false;
    double frequency_hz = 0.0;
};

struct Run {
    std::vector<float> audio;       // from sample `history` on
    std::size_t history = 0;        // the detector's reach, where audio starts
    std::vector<Probe> probes;
    std::vector<dsp::Complex32> ring;
};

// A retune the chain applies at a sample: restart, or carry the loop's
// frequency by `moved_hz` of receiver movement.
struct Event {
    std::size_t at = 0;
    bool reset = false;
    double moved_hz = 0.0;
};

Run run_chain(const dsp::VrxPlan& plan, std::span<const dsp::Complex32> input,
              std::size_t block, const std::vector<Event>& events = {}) {
    Run run;
    const std::size_t capacity = std::bit_ceil(input.size());
    const auto mask = static_cast<std::uint32_t>(capacity - 1U);
    run.ring.assign(capacity, dsp::Complex32{});
    std::copy(input.begin(), input.end(), run.ring.begin());

    const double rate = static_cast<double>(plan.demod_rate);
    if (plan.carrier.power != dsp::kCarrierNone) {
        const auto nco = nco_for(plan);
        std::vector<std::uint32_t> state(dsp::kCarrierStateWords, 0U);
        std::size_t next_event = 0;
        for (std::size_t start = 0; start < input.size();) {
            bool reset = false;
            double shift = 0.0;
            std::size_t end = std::min(input.size(), start + block);
            if (next_event < events.size()) {
                if (events[next_event].at == start) {
                    reset = events[next_event].reset;
                    if (!reset) {
                        shift = -test::kTwoPi * events[next_event].moved_hz / rate;
                    }
                    ++next_event;
                } else if (events[next_event].at < end) {
                    end = events[next_event].at;
                }
            }
            auto params = dsp::carrier_block(plan, mask, start,
                                             static_cast<std::uint32_t>(end - start), reset,
                                             static_cast<float>(shift));
            INFO(test::message_of(params));
            REQUIRE(params.has_value());
            REQUIRE(dsp::reference_vrx_carrier(plan.carrier, *params, nco, run.ring, state)
                        .has_value());
            Probe probe;
            probe.at_seconds = static_cast<double>(end) / rate;
            probe.locked = state[dsp::kCarrierStateLocked] != 0U;
            probe.frequency_hz = static_cast<double>(std::bit_cast<float>(
                                     state[dsp::kCarrierStateIntegrator])) *
                                 rate / test::kTwoPi;
            run.probes.push_back(probe);
            start = end;
        }
    }

    run.history = dsp::demod_fine_history(plan.demod);
    dsp::VrxDemodParams params;
    params.in_mask = mask;
    params.in_offset = static_cast<std::uint32_t>(run.history);
    params.count = static_cast<std::uint32_t>(input.size() - run.history);
    params.gain = plan.demod_gain;
    run.audio.assign(params.count, 0.0F);
    const auto demodulated =
        dsp::reference_vrx_demod(plan.demod, params, run.ring, plan.demod_weights, run.audio);
    INFO(test::message_of(demodulated));
    REQUIRE(demodulated.has_value());
    return run;
}

// Audio from `from` to `to` seconds of input time.
std::span<const float> audio_between(const Run& run, double rate, double from, double to) {
    const auto first = static_cast<std::size_t>(from * rate) - run.history;
    const auto last = std::min(run.audio.size(), static_cast<std::size_t>(to * rate) - run.history);
    return std::span<const float>(run.audio).subspan(first, last - first);
}

// Harmonics two to five over the fundamental, in decibels, summed over 50 ms
// windows so a level that fades during the measurement is read window by
// window rather than smeared into its neighbouring bins. Fifty windows of a
// whole number of cycles of each harmonic, so the projections are
// orthogonal.
double harmonic_distortion_db(std::span<const float> audio, double rate) {
    const auto window = static_cast<std::size_t>(0.05 * rate);
    double fundamental = 0.0;
    double harmonics = 0.0;
    for (std::size_t at = 0; at + window <= audio.size(); at += window) {
        const auto piece = audio.subspan(at, window);
        const double a1 = test::measure_audio_tone(piece, rate, kToneHz, true).amplitude;
        fundamental += a1 * a1;
        for (int k = 2; k <= 5; ++k) {
            const double ak =
                test::measure_audio_tone(piece, rate, k * kToneHz, true).amplitude;
            harmonics += ak * ak;
        }
    }
    return 10.0 * std::log10(std::max(harmonics, 1.0e-30) / fundamental);
}

// The time the loop last came out of lock, which is when it locked for good;
// infinite when it ends unlocked.
double lock_time(const Run& run) {
    if (run.probes.empty() || !run.probes.back().locked) {
        return std::numeric_limits<double>::infinity();
    }
    double since = 0.0;
    for (const Probe& probe : run.probes) {
        if (!probe.locked) {
            since = probe.at_seconds;
        }
    }
    return since;
}

// The loop's frequency averaged over its last `seconds`.
double settled_frequency(const Run& run, double seconds) {
    const double end = run.probes.back().at_seconds;
    double sum = 0.0;
    std::size_t count = 0;
    for (const Probe& probe : run.probes) {
        if (probe.at_seconds > end - seconds) {
            sum += probe.frequency_hz;
            ++count;
        }
    }
    return sum / static_cast<double>(std::max<std::size_t>(count, 1));
}

// ---------------------------------------------------------------------------
// The kernel on the device, in place
// ---------------------------------------------------------------------------

// gpu::run_kernel zeroes its outputs, which is wrong for a kernel that
// rewrites a ring in place and carries state; tests/reference/test_noise.cpp
// has the same helper for the same reason. Every buffer is uploaded, the
// kernel dispatched once, and every buffer read back.
void run_carrier_on_gpu(const dsp::VrxCarrierConfig& config, const dsp::VrxCarrierParams& params,
                        std::span<const dsp::Complex32> nco, std::span<dsp::Complex32> ring,
                        std::span<std::uint32_t> state, std::uint32_t local_size) {
    auto& context = test::shared_context();

    const std::uint32_t constants[] = {config.power, config.nco_log2};
    gpu::ComputePipeline::Options options;
    options.spirv = gpu::shaders::vrx_carrier();
    options.storage_buffer_count = 3;
    options.local_size_x = local_size;
    options.push_constant_bytes = sizeof(dsp::VrxCarrierParams);
    options.grid_constants = constants;
    auto pipeline = gpu::ComputePipeline::create(context, options);
    INFO(test::message_of(pipeline));
    REQUIRE(pipeline.has_value());

    constexpr VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    const std::span<const std::byte> in_bytes[] = {std::as_bytes(nco), std::as_bytes(ring),
                                                   std::as_bytes(state)};
    const std::span<std::byte> out_bytes[] = {{}, std::as_writable_bytes(ring),
                                              std::as_writable_bytes(state)};

    std::vector<gpu::Buffer> device;
    std::vector<gpu::Buffer> upload;
    std::vector<gpu::Buffer> readback;
    std::vector<VkBuffer> handles;
    std::vector<gpu::CommandRunner::BufferCopy> ins;
    std::vector<gpu::CommandRunner::BufferCopy> outs;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto bytes = in_bytes[i];
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
    for (std::size_t i = 0; i < 3; ++i) {
        handles.push_back(device[i].handle());
        ins.push_back({upload[i].handle(), device[i].handle(), in_bytes[i].size()});
        outs.push_back({device[i].handle(), readback[i].handle(), in_bytes[i].size()});
    }

    auto runner = gpu::CommandRunner::create(context);
    REQUIRE(runner.has_value());
    REQUIRE(runner->copy(ins).has_value());

    gpu::CommandRunner::Dispatch dispatch;
    dispatch.pipeline = &*pipeline;
    dispatch.buffers = handles;
    dispatch.group_count_x = 1;
    dispatch.push_constants = std::as_bytes(std::span<const dsp::VrxCarrierParams>(&params, 1));
    const auto ran = runner->run(dispatch);
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    REQUIRE(runner->copy(outs).has_value());
    for (std::size_t i = 1; i < 3; ++i) {
        REQUIRE(readback[i].read(out_bytes[i]).has_value());
    }
}

// A stream that makes the loop do everything it can: noise, a carrier off
// the centre, a fade, and a step in frequency.
std::vector<dsp::Complex32> busy_stream(engine::Demod mode, double rate, std::size_t count,
                                        std::uint64_t seed) {
    Signal signal;
    signal.phase = 0.9;
    signal.offset_hz = [](double t) { return t < 0.08 ? 310.0 : 190.0; };
    if (mode == engine::Demod::Sam) {
        signal.envelope = [](double t) {
            const double carrier = (t > 0.05 && t < 0.07) ? 0.05 : 0.5;
            return std::complex<double>(carrier + 0.4 * std::cos(test::kTwoPi * kToneHz * t), 0.0);
        };
    } else {
        signal.envelope = [](double t) {
            return std::complex<double>(0.5 * std::cos(test::kTwoPi * 730.0 * t), 0.0);
        };
    }
    return render(signal, rate, count, 0.05, seed);
}

void check_kernel_against_twin(engine::Demod mode, std::uint32_t local_size, std::uint64_t seed) {
    const auto plan = make_plan(mode, mode == engine::Demod::Sam ? 10'000 : 6'000);
    const double rate = static_cast<double>(plan.demod_rate);
    REQUIRE(plan.carrier.power != dsp::kCarrierNone);
    const auto nco = nco_for(plan);

    constexpr std::size_t kCapacity = 1U << 13;
    constexpr auto kMask = static_cast<std::uint32_t>(kCapacity - 1U);
    const auto stream = busy_stream(mode, rate, 6'000, seed);

    std::vector<dsp::Complex32> cpu_ring(kCapacity, dsp::Complex32{});
    std::vector<std::uint32_t> cpu_state(dsp::kCarrierStateWords, 0U);
    std::vector<dsp::Complex32> gpu_ring = cpu_ring;
    std::vector<std::uint32_t> gpu_state = cpu_state;

    // Dispatches of awkward lengths, starting part way round the ring so the
    // stream wraps, with a restart at the fifth and a retune shift at the
    // seventh.
    const std::uint32_t lengths[] = {1, 7, 64, 333, 1'000, 2'048, 5, 900, 1'642};
    std::uint64_t at = kCapacity - 700U;
    std::size_t taken = 0;
    for (std::size_t d = 0; d < std::size(lengths); ++d) {
        const std::uint32_t length = lengths[d];
        INFO("dispatch " << d << " of " << length << " at stream index " << at);
        for (std::uint32_t i = 0; i < length; ++i) {
            const auto slot = static_cast<std::size_t>((at + i) & kMask);
            cpu_ring[slot] = stream[taken + i];
            gpu_ring[slot] = stream[taken + i];
        }
        const bool reset = d == 4;
        const float shift = (d == 6) ? static_cast<float>(-test::kTwoPi * 120.0 / rate) : 0.0F;
        auto params = dsp::carrier_block(plan, kMask, at, length, reset, shift);
        INFO(test::message_of(params));
        REQUIRE(params.has_value());

        REQUIRE(dsp::reference_vrx_carrier(plan.carrier, *params, nco, cpu_ring, cpu_state)
                    .has_value());
        run_carrier_on_gpu(plan.carrier, *params, nco, gpu_ring, gpu_state, local_size);

        const auto ring_diff = test::diff(std::span<const dsp::Complex32>(gpu_ring),
                                          std::span<const dsp::Complex32>(cpu_ring), seed);
        INFO(ring_diff.report);
        CHECK(ring_diff.identical);
        CHECK(gpu_state == cpu_state);
        at += length;
        taken += length;
    }

    // The loop acquired and locked during the run, so the diff compared a
    // trajectory and not two buffers of unrotated input.
    CHECK(cpu_state[dsp::kCarrierStateLocked] == 1U);
    CHECK(cpu_state[dsp::kCarrierStatePhase] != 0U);
}

}  // namespace

// ---------------------------------------------------------------------------
// Bit-exact, device against twin
// ---------------------------------------------------------------------------

TEST_CASE("the carrier loop matches its twin bit-exactly across dispatch boundaries",
          "[gpu][vrx][carrier]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());
    check_kernel_against_twin(engine::Demod::Sam, 1, 0x5341'4D00'0000'0001ULL);
}

TEST_CASE("the Costas loop matches its twin bit-exactly across dispatch boundaries",
          "[gpu][vrx][carrier]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());
    check_kernel_against_twin(engine::Demod::Dsb, 1, 0x4453'4200'0000'0001ULL);
}

// The host specializes the kernel at one invocation. A caller that asked for
// a whole group must get the same bits, from the first invocation alone.
TEST_CASE("the carrier loop is bit-exact when specialized wider than one invocation",
          "[gpu][vrx][carrier]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());
    check_kernel_against_twin(engine::Demod::Sam, 64, 0x5341'4D00'0000'0002ULL);
    check_kernel_against_twin(engine::Demod::Dsb, 64, 0x4453'4200'0000'0002ULL);
}

// ---------------------------------------------------------------------------
// The design, on the host
// ---------------------------------------------------------------------------

TEST_CASE("sam runs the carrier loop, dsb the Costas loop, and nothing else runs one",
          "[vrx][carrier]") {
    for (std::uint32_t mode = dsp::kDemodRaw; mode <= dsp::kDemodSam; ++mode) {
        INFO("mode " << engine::demod_name(static_cast<engine::Demod>(mode)));
        const std::uint32_t expected = (mode == dsp::kDemodSam)   ? dsp::kCarrierPll
                                       : (mode == dsp::kDemodDsb) ? dsp::kCarrierCostas
                                                                  : dsp::kCarrierNone;
        CHECK(dsp::carrier_power(mode) == expected);
    }
    CHECK(dsp::carrier_power(dsp::kDemodSam + 1U) == dsp::kCarrierNone);

    const auto sam = make_plan(engine::Demod::Sam, 10'000);
    const auto am = make_plan(engine::Demod::Am, 10'000);
    const auto dsb = make_plan(engine::Demod::Dsb, 6'000);
    CHECK(sam.carrier.power == dsp::kCarrierPll);
    CHECK(dsb.carrier.power == dsp::kCarrierCostas);
    CHECK(am.carrier.power == dsp::kCarrierNone);

    // SAM is AM's receiver in every respect but the detector: the same
    // channel, the same DC window, the same audio convention.
    CHECK(sam.passband.low == am.passband.low);
    CHECK(sam.passband.high == am.passband.high);
    CHECK(sam.demod.dc_taps == am.demod.dc_taps);
    CHECK(sam.demod_gain == am.demod_gain);
    CHECK(engine::levelling_for(engine::Demod::Sam) == engine::levelling_for(engine::Demod::Am));

    // The loop is part of the shape, so a stage never has one retuned in or
    // out of existence.
    CHECK(dsp::shape_of(sam).carrier != dsp::shape_of(am).carrier);

    // The integrator's bound is the passband's reach: 5 kHz for sam's
    // 10 kHz channel and 3 kHz for dsb's 6 kHz one.
    const double sam_reach = static_cast<double>(sam.carrier_loop.clamp_radians) *
                             static_cast<double>(sam.demod_rate) / test::kTwoPi;
    const double dsb_reach = static_cast<double>(dsb.carrier_loop.clamp_radians) *
                             static_cast<double>(dsb.demod_rate) / test::kTwoPi;
    CHECK(std::abs(sam_reach - 5'000.0) < 0.01);
    CHECK(std::abs(dsb_reach - 3'000.0) < 0.01);

    // A coherent detector makes none of the envelope's difference products,
    // so SAM needs the shared floor where AM needs twice the width.
    const dsp::Passband band{-5'000, 5'000};
    CHECK(dsp::minimum_demod_rate(dsp::kDemodSam, band) == 15'000);
    CHECK(dsp::minimum_demod_rate(dsp::kDemodAm, band) == 20'000);
}

TEST_CASE("the loop gains are the second-order loop's, and the bandwidth they give is measured",
          "[vrx][carrier]") {
    // The small-theta limits of the two expressions: K1 -> 4*zeta*theta and
    // K2 -> 4*theta^2, which is the continuous loop sampled.
    const double zeta = dsp::kCarrierDamping;
    const auto gains = dsp::carrier_loop_gains(1.0, zeta, 48'000);
    const double theta = (1.0 / 48'000.0) / (zeta + 1.0 / (4.0 * zeta));
    CHECK(std::abs(gains.k1 / (4.0 * zeta * theta) - 1.0) < 1.0e-4);
    CHECK(std::abs(gains.k2 / (4.0 * theta * theta) - 1.0) < 1.0e-4);

    // The noise bandwidth those gains actually deliver, measured from the
    // linearised loop's impulse response: B_n = (sum of h^2) / (2 T), where
    // h is the oscillator's phase after a unit impulse of input phase. The
    // loop is the kernel's with the detector linearised, the oscillator
    // updated after the sample it rotated. Close to the asked-for figure is
    // the check that the formula was transcribed right, since a slip in
    // either gain moves it.
    for (const double asked : {dsp::kCarrierAcquireHz, dsp::kCarrierTrackHz}) {
        const auto g = dsp::carrier_loop_gains(asked, zeta, 48'000);
        double phase = 0.0;
        double integrator = 0.0;
        double sum = 0.0;
        for (int n = 0; n < 480'000; ++n) {
            const double input = (n == 0) ? 1.0 : 0.0;
            const double error = input - phase;
            integrator += g.k2 * error;
            phase += g.k1 * error + integrator;
            sum += phase * phase;
        }
        const double measured = sum * 48'000.0 / 2.0;
        WARN(std::format("asked for a {:.0f} Hz noise bandwidth, the discrete loop gives {:.1f} Hz",
                         asked, measured));
        CHECK(std::abs(measured / asked - 1.0) < 0.1);
    }
}

TEST_CASE("a retune restarts the loop past the passband and carries it inside",
          "[vrx][carrier]") {
    // The AGC's rule in core/engine/graph.cpp: strictly further than the
    // granted width either way is a different signal.
    CHECK(dsp::carrier_retune(10'001, 10'000, 48'000).reset);
    CHECK(dsp::carrier_retune(-10'001, 10'000, 48'000).reset);
    CHECK_FALSE(dsp::carrier_retune(10'000, 10'000, 48'000).reset);

    // Inside it, the carrier moves the other way in the fine stream by
    // exactly the move.
    const auto nudge = dsp::carrier_retune(300, 10'000, 48'000);
    CHECK_FALSE(nudge.reset);
    CHECK(std::abs(nudge.shift + test::kTwoPi * 300.0 / 48'000.0) < 1.0e-15);

    // And a restart is exactly a loop that has seen nothing: station A for a
    // while, then station B behind a restart, gives the same bits as B from
    // a fresh loop.
    const auto plan = make_plan(engine::Demod::Sam, 10'000);
    const double rate = static_cast<double>(plan.demod_rate);
    Signal a;
    a.envelope = am_tone(0.3, 0.8);
    a.offset_hz = [](double) { return 140.0; };
    Signal b;
    b.envelope = am_tone(0.01, 0.008);
    b.offset_hz = [](double) { return -60.0; };
    b.phase = 2.2;
    const auto first = render(a, rate, 9'600);
    const auto second = render(b, rate, 9'600);

    const auto nco = nco_for(plan);
    constexpr std::uint32_t kMask = (1U << 15) - 1U;
    std::vector<dsp::Complex32> ring(kMask + 1U, dsp::Complex32{});
    std::copy(first.begin(), first.end(), ring.begin());
    std::copy(second.begin(), second.end(), ring.begin() + 9'600);
    std::vector<std::uint32_t> state(dsp::kCarrierStateWords, 0U);
    auto head = dsp::carrier_block(plan, kMask, 0, 9'600, false, 0.0F);
    REQUIRE(head.has_value());
    REQUIRE(dsp::reference_vrx_carrier(plan.carrier, *head, nco, ring, state).has_value());
    CHECK(state[dsp::kCarrierStateLocked] == 1U);
    auto tail = dsp::carrier_block(plan, kMask, 9'600, 9'600, true, 0.0F);
    REQUIRE(tail.has_value());
    REQUIRE(dsp::reference_vrx_carrier(plan.carrier, *tail, nco, ring, state).has_value());

    std::vector<dsp::Complex32> fresh(kMask + 1U, dsp::Complex32{});
    std::copy(second.begin(), second.end(), fresh.begin() + 9'600);
    std::vector<std::uint32_t> fresh_state(dsp::kCarrierStateWords, 0U);
    auto alone = dsp::carrier_block(plan, kMask, 9'600, 9'600, false, 0.0F);
    REQUIRE(alone.has_value());
    REQUIRE(dsp::reference_vrx_carrier(plan.carrier, *alone, nco, fresh, fresh_state).has_value());

    CHECK(std::equal(ring.begin() + 9'600, ring.begin() + 19'200, fresh.begin() + 9'600));
    CHECK(state == fresh_state);
}

TEST_CASE("the loop's output does not depend on where the blocks fell", "[vrx][carrier]") {
    for (const engine::Demod mode : {engine::Demod::Sam, engine::Demod::Dsb}) {
        INFO(engine::demod_name(mode));
        const auto plan = make_plan(mode, mode == engine::Demod::Sam ? 10'000 : 6'000);
        const auto stream = busy_stream(mode, static_cast<double>(plan.demod_rate), 12'000, 7);
        const Run whole = run_chain(plan, stream, stream.size());
        for (const std::size_t block : {std::size_t{1}, std::size_t{37}, std::size_t{480},
                                        std::size_t{4'096}}) {
            INFO("blocks of " << block);
            const Run cut = run_chain(plan, stream, block);
            CHECK(cut.ring == whole.ring);
            CHECK(cut.audio == whole.audio);
        }
    }
}

// ---------------------------------------------------------------------------
// Measurements
// ---------------------------------------------------------------------------

TEST_CASE("the loops lock within the times stated", "[vrx][carrier][measure]") {
    // Lock time is when the lock test last went true and stayed true, read at
    // the end of each 1 ms block. Locked to the carrier is the loop's
    // frequency, averaged over the last 100 ms, within 1 Hz of the offset;
    // a loop that locked to something else is reported as such.
    //
    // SNR is the signal's power over the noise's across the 48 kHz
    // demodulation rate. Five seeds per point; the worst is reported.
    //
    // Two modulations, because they answer different questions. A steady
    // 1 kHz tone is a spectral line as strong as a sideband gets, which a
    // phase-locked loop can take for a carrier once the carrier is further
    // off than the tone; programme(), twenty tones over 300 to 3000 Hz, has
    // no such line and is the nearer stand-in for a voice.
    const double offsets_sam[] = {0.0, 50.0, 200.0, 500.0, 800.0, 1'500.0, 3'000.0, 4'500.0};
    const double offsets_dsb[] = {0.0, 50.0, 200.0, 500.0, 800.0, 1'500.0, 2'500.0};

    for (const engine::Demod mode : {engine::Demod::Sam, engine::Demod::Dsb}) {
        const bool sam = mode == engine::Demod::Sam;
        const auto plan = make_plan(mode, sam ? 10'000 : 6'000);
        const double rate = static_cast<double>(plan.demod_rate);
        const std::span<const double> offsets =
            sam ? std::span<const double>(offsets_sam) : std::span<const double>(offsets_dsb);
        for (const bool tone : {true, false}) {
            std::string table = std::format(
                "{} lock time, {} Hz passband, {}, worst of five seeds:\n",
                engine::demod_name(mode), plan.passband.width(),
                tone ? (sam ? "a 1 kHz tone at 80%" : "a 1 kHz tone")
                     : (sam ? "twenty tones at 80% peak" : "twenty tones"));
            for (const double snr : {30.0, 10.0}) {
                for (const double offset : offsets) {
                    double worst = 0.0;
                    int on_carrier = 0;
                    for (std::uint64_t seed = 1; seed <= 5; ++seed) {
                        const auto audio = tone ? std::function<double(double)>([](double t) {
                            return std::cos(test::kTwoPi * kToneHz * t);
                        })
                                                : programme(1.0, seed);
                        Signal signal;
                        if (sam) {
                            signal.envelope = [audio](double t) {
                                return std::complex<double>(1.0 + 0.8 * audio(t), 0.0);
                            };
                        } else {
                            signal.envelope = [audio](double t) {
                                return std::complex<double>(audio(t), 0.0);
                            };
                        }
                        signal.offset_hz = [offset](double) { return offset; };
                        signal.phase = 0.37 * static_cast<double>(seed);
                        const auto input =
                            render(signal, rate, static_cast<std::size_t>(1.0 * rate));
                        double power = 0.0;
                        for (const dsp::Complex32& z : input) {
                            power += static_cast<double>(std::norm(z));
                        }
                        power /= static_cast<double>(input.size());
                        const double noise = std::sqrt(power / std::pow(10.0, snr / 10.0));
                        const auto noisy =
                            render(signal, rate, input.size(), noise,
                                   seed * 977U + static_cast<std::uint64_t>(offset));
                        const Run run = run_chain(plan, noisy, 48);
                        worst = std::max(worst, lock_time(run));
                        if (std::abs(settled_frequency(run, 0.1) - offset) < 1.0) {
                            ++on_carrier;
                        }
                    }
                    table += std::format("  SNR {:>4.0f} dB, carrier {:>6.0f} Hz off: locked by "
                                         "{:>7.1f} ms, on the carrier in {} of 5\n",
                                         snr, offset, worst * 1'000.0, on_carrier);

                    // What core/dsp/vrx_reference.h claims, at either SNR and
                    // with either modulation: sam on the carrier within
                    // 100 ms from up to 500 Hz off; dsb within 100 ms from
                    // up to 200 Hz off and within 600 ms from 500.
                    const double bound = sam ? ((offset <= 500.0) ? 0.1 : 0.0)
                                             : ((offset <= 200.0)   ? 0.1
                                                : (offset <= 500.0) ? 0.6
                                                                    : 0.0);
                    if (bound > 0.0) {
                        INFO(engine::demod_name(mode) << " at " << offset << " Hz, SNR " << snr
                                                      << (tone ? ", tone" : ", programme"));
                        CHECK(worst < bound);
                        CHECK(on_carrier == 5);
                    }
                }
            }
            WARN(table);
        }
    }
}

TEST_CASE("the loop follows a drifting carrier across the passband and no further",
          "[vrx][carrier][measure]") {
    // Locked on the centre, then the carrier drifts at 100 Hz a second to
    // 300 Hz beyond the passband's edge. Tracked is the loop's frequency
    // within 2 Hz of the carrier's; the furthest offset tracked is reported
    // against the filter's reach.
    //
    // 100 Hz a second because that is a drift the tracking loop follows with
    // its lock test passing. A type-2 loop follows a frequency ramp with a
    // standing phase error of the ramp over the natural frequency squared,
    // 0.2 radian here at 30 Hz; at 1 kHz a second it is 2 radians, which is
    // past the lock test and past a Costas loop's half turn, and the loop
    // follows by dropping to acquisition and back.
    constexpr double kDriftHzPerSecond = 100.0;
    for (const engine::Demod mode : {engine::Demod::Sam, engine::Demod::Dsb}) {
        const bool sam = mode == engine::Demod::Sam;
        const auto plan = make_plan(mode, sam ? 10'000 : 6'000);
        const double rate = static_cast<double>(plan.demod_rate);
        const double reach = static_cast<double>(plan.passband.reach_from(0));
        const auto nco = nco_for(plan);
        for (const double direction : {1.0, -1.0}) {
            Signal signal;
            signal.envelope = sam ? am_tone(1.0, 0.8) : dsb_tone();
            signal.offset_hz = [direction](double t) {
                return direction * std::max(0.0, t - 0.2) * kDriftHzPerSecond;
            };
            const double seconds = 0.2 + (reach + 300.0) / kDriftHzPerSecond;
            const auto total = static_cast<std::size_t>(seconds * rate);

            // Streamed through a small ring, since the run is minutes long.
            Synth synth(signal, rate, 0.01, 11);
            constexpr std::uint32_t kRing = 1U << 12;
            constexpr std::uint32_t kBlock = 512;  // divides the ring, so no block wraps
            std::vector<dsp::Complex32> ring(kRing, dsp::Complex32{});
            std::vector<std::uint32_t> state(dsp::kCarrierStateWords, 0U);
            // Tracked is locked with the loop's integrator within 10 Hz of
            // the carrier. Not closer: on a ramp the integrator runs behind
            // the carrier by K1 times the standing phase error, 2.5 Hz here,
            // and the proportional path makes up the rest.
            double tracked = 0.0;
            double furthest = 0.0;
            double final_hz = 0.0;
            for (std::size_t at = 0; at + kBlock <= total; at += kBlock) {
                const std::size_t slot = at % kRing;
                synth.next(std::span<dsp::Complex32>(ring).subspan(slot, kBlock));
                auto params = dsp::carrier_block(plan, kRing - 1U, at, kBlock, false, 0.0F);
                REQUIRE(params.has_value());
                REQUIRE(dsp::reference_vrx_carrier(plan.carrier, *params, nco, ring, state)
                            .has_value());
                final_hz = static_cast<double>(std::bit_cast<float>(
                               state[dsp::kCarrierStateIntegrator])) *
                           rate / test::kTwoPi;
                furthest = std::max(furthest, std::abs(final_hz));
                const double truth = signal.offset_hz(static_cast<double>(at + kBlock) / rate);
                if (state[dsp::kCarrierStateLocked] != 0U && std::abs(final_hz - truth) < 10.0) {
                    tracked = std::max(tracked, std::abs(truth));
                }
            }
            WARN(std::format("{} drifting {}: tracked to {:.0f} Hz of a {:.0f} Hz reach, and "
                             "at {:.1f} Hz 300 Hz after the carrier left the passband, never "
                             "past {:.1f}",
                             engine::demod_name(mode), direction > 0 ? "up" : "down", tracked,
                             reach, final_hz, furthest));
            CHECK(tracked > 0.98 * reach);
            CHECK(furthest <= reach + 0.5);
        }
    }
}

TEST_CASE("dsb comes back at unit level whatever the carrier phase", "[vrx][carrier][measure]") {
    // The defect this loop exists for: a product detector on a suppressed
    // carrier scales the tone by the cosine of the phase against it. Sixteen
    // phases round the turn, on the carrier and 37 Hz off, each measured over
    // 0.3 to 0.6 s at 40 dB SNR across the demodulation rate; the same input
    // through the product detector alone for contrast.
    //
    // WITH NOISE, AND NOT BECAUSE THE LEVEL NEEDS IT. A Costas loop has an
    // unstable null a quarter turn from lock, where its detector reads zero
    // (Gardner calls the slow escape from it hangup). A noise-free signal
    // exactly there leaves the detector at 1e-17, the step that makes is
    // below the phase accumulator's 2^-32 of a turn, and the loop never
    // moves. Any noise at all kicks it off: the worst of these sixteen phases
    // includes that quarter turn, and its lock time is reported.
    const auto plan = make_plan(engine::Demod::Dsb, 6'000);
    const double rate = static_cast<double>(plan.demod_rate);
    dsp::VrxPlan unlocked = plan;
    unlocked.carrier.power = dsp::kCarrierNone;
    const double noise = std::sqrt(0.5 / 1.0e4);

    double lowest = 1.0e9;
    double highest = 0.0;
    double lowest_open = 1.0e9;
    double slowest = 0.0;
    for (int k = 0; k < 16; ++k) {
        for (const double offset : {0.0, 37.0}) {
            Signal signal;
            signal.envelope = dsb_tone();
            signal.offset_hz = [offset](double) { return offset; };
            signal.phase = test::kTwoPi * static_cast<double>(k) / 16.0;
            const auto input = render(signal, rate, static_cast<std::size_t>(0.6 * rate), noise,
                                      static_cast<std::uint64_t>(100 + k));
            const Run run = run_chain(plan, input, 48);
            slowest = std::max(slowest, lock_time(run));
            const double level =
                test::measure_audio_tone(audio_between(run, rate, 0.3, 0.6), rate, kToneHz)
                    .amplitude;
            lowest = std::min(lowest, level);
            highest = std::max(highest, level);
            if (offset == 0.0) {
                const Run open = run_chain(unlocked, input, 480);
                lowest_open = std::min(
                    lowest_open,
                    test::measure_audio_tone(audio_between(open, rate, 0.3, 0.6), rate, kToneHz)
                        .amplitude);
            }
        }
    }
    WARN(std::format("dsb, a unit tone at 16 carrier phases: {:.5f} to {:.5f} with the Costas "
                     "loop, the slowest locked by {:.1f} ms; {:.5f} at the worst phase "
                     "without it",
                     lowest, highest, slowest * 1'000.0, lowest_open));
    CHECK(lowest > 0.995);
    CHECK(highest < 1.005);
    CHECK(slowest < 0.15);
    CHECK(lowest_open < 0.1);
}

TEST_CASE("sam is linear where the envelope is not", "[vrx][carrier][measure]") {
    // Selective fading, which is a carrier falling under its own sidebands.
    // An envelope detector rectifies there; a coherent one does not. AM at
    // 80% of a 1 kHz tone, the carrier 37 Hz off, no noise, harmonics two to
    // five over the fundamental through each detector, over 0.5 to 1.5 s.
    //
    // Three channels:
    //   the carrier alone notched by 20, 30 and 40 dB, after the loop has had
    //   0.3 s of the unfaded signal, and notched 20 dB from the first sample;
    //   two paths, the second 0.9 as strong and a half turn round, so the
    //   carrier sits in a 20 dB notch, static and with a 0.5 Hz differential
    //   Doppler sweeping the notch through the band.
    const auto sam_plan = make_plan(engine::Demod::Sam, 10'000);
    const auto am_plan = make_plan(engine::Demod::Am, 10'000);
    const double rate = static_cast<double>(sam_plan.demod_rate);
    REQUIRE(am_plan.demod_rate == sam_plan.demod_rate);
    const auto count = static_cast<std::size_t>(1.5 * rate);

    struct Channel {
        std::string name;
        Signal signal;
        double sam_below_db;
    };
    std::vector<Channel> channels;
    for (const double notch : {20.0, 30.0, 40.0}) {
        Signal s;
        s.offset_hz = [](double) { return 37.0; };
        const double carrier = std::pow(10.0, -notch / 20.0);
        s.envelope = [carrier](double t) {
            const double c = (t < 0.3) ? 1.0 : carrier;
            return std::complex<double>(c + 0.8 * std::cos(test::kTwoPi * kToneHz * t), 0.0);
        };
        channels.push_back({std::format("carrier notched {:.0f} dB after 0.3 s", notch), s, -40.0});
    }
    {
        Signal s;
        s.offset_hz = [](double) { return 37.0; };
        s.envelope = am_tone(0.1, 0.8);
        channels.push_back({"carrier notched 20 dB from the start", s, -40.0});
    }
    for (const double delay_ms : {0.3, 0.5, 0.7}) {
        for (const double doppler : {0.0, 0.5}) {
            Signal s;
            s.offset_hz = [](double) { return 37.0; };
            s.envelope = two_path(am_tone(1.0, 0.8), 0.9, delay_ms * 1.0e-3,
                                  [doppler](double t) {
                                      return std::numbers::pi + test::kTwoPi * doppler * t;
                                  });
            channels.push_back({std::format("two paths, {:.1f} ms apart, {:.1f} Hz Doppler",
                                            delay_ms, doppler),
                                s, -25.0});
        }
    }

    std::string table = "harmonic distortion, 2nd to 5th over the fundamental:\n";
    for (const Channel& channel : channels) {
        const auto input = render(channel.signal, rate, count);
        const Run envelope = run_chain(am_plan, input, 480);
        const Run coherent = run_chain(sam_plan, input, 480);
        const double env_db = harmonic_distortion_db(audio_between(envelope, rate, 0.5, 1.5), rate);
        const double sam_db = harmonic_distortion_db(audio_between(coherent, rate, 0.5, 1.5), rate);
        table += std::format("  {:<44} envelope {:>7.1f} dB   sam {:>7.1f} dB\n", channel.name,
                             env_db, sam_db);
        INFO(channel.name);
        CHECK(sam_db < channel.sam_below_db);
        CHECK(sam_db < env_db - 20.0);
    }
    WARN(table);
}

TEST_CASE("a retune inside the passband keeps the loop on the carrier", "[vrx][carrier][measure]") {
    // Locked 140 Hz off, then the receiver moves 300 Hz up at 0.3 s, so the
    // carrier steps 300 Hz down in the fine stream. With the shift the retune
    // hands the loop, against the same retune without it.
    const auto plan = make_plan(engine::Demod::Sam, 10'000);
    const double rate = static_cast<double>(plan.demod_rate);
    Signal signal;
    signal.envelope = am_tone(1.0, 0.8);
    signal.offset_hz = [](double t) { return t < 0.3 ? 140.0 : -160.0; };
    const auto input = render(signal, rate, static_cast<std::size_t>(0.8 * rate), 0.05, 3);
    const auto at = static_cast<std::size_t>(0.3 * rate);

    const Run carried = run_chain(plan, input, 48, {Event{at, false, 300.0}});
    const Run blind = run_chain(plan, input, 48, {Event{at, false, 0.0}});

    const auto after = [](const Run& run) {
        double relocked = 0.0;
        bool dropped = false;
        for (const Probe& probe : run.probes) {
            if (probe.at_seconds > 0.3 && !probe.locked) {
                dropped = true;
                relocked = probe.at_seconds - 0.3;
            }
        }
        return std::pair{dropped, relocked};
    };
    const auto [carried_dropped, carried_back] = after(carried);
    const auto [blind_dropped, blind_back] = after(blind);
    WARN(std::format("sam retuned 300 Hz inside a 10 kHz passband: with the shift the lock "
                     "{} ({:.1f} ms), without it {} ({:.1f} ms); settled at {:.2f} and {:.2f} Hz "
                     "for a carrier at -160",
                     carried_dropped ? "dropped" : "held", carried_back * 1'000.0,
                     blind_dropped ? "dropped" : "held", blind_back * 1'000.0,
                     settled_frequency(carried, 0.1), settled_frequency(blind, 0.1)));
    CHECK(std::abs(settled_frequency(carried, 0.1) + 160.0) < 1.0);
    CHECK(carried_back <= blind_back);
}
