// The per-receiver fine stage and the eight demodulators, GPU against CPU.
//
// These two kernels were written, compiled and validated and then never
// dispatched, which made their bit-exactness a claim rather than a
// measurement. It was the one hole in the project's central correctness
// argument, and it sat exactly where the arithmetic is hardest to eyeball:
// core/shaders/vrx_demod.comp builds its own sqrt, reciprocal and atan2 out of
// Newton iterations precisely because Vulkan's built-ins are specified too
// loosely to referee, and until this file ran nothing had checked that the
// sequence on the device is the sequence in the twin.
//
// Three kinds of case, and they catch different things.
//
//   The host cases check the deterministic transcendentals and the NCO
//   against their own stated properties. A twin and a kernel can agree bit
//   for bit on an atan2 that is simply wrong, and only a comparison against
//   the real function notices.
//
//   The bit-exact cases run each kernel against its twin and demand identical
//   bits. They are what the conformance matrix runs on every device.
//
//   The behavioural cases put a known signal through and check what comes
//   out: a tone at the receiver's centre arrives at DC at unit magnitude, a
//   USB receiver rejects the sideband it is not listening to, and each
//   detector recovers its own modulation at the level the audio convention
//   promises. This is the class that catches a kernel that is bit-exact
//   against a twin implementing the wrong convention.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "core/dsp/pfb.h"
#include "core/dsp/synth/modulators.h"
#include "core/dsp/synth/wfm_mod.h"
#include "core/dsp/types.h"
#include "core/dsp/vrx_reference.h"
#include "core/engine/vrx.h"
#include "core/gpu/kernel.h"
#include "core/gpu/shaders.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/support/tone_measure.h"

using namespace revenant;
using Catch::Approx;

namespace {

constexpr dsp::SampleRate kSourceRate = 2'400'000;

// The canonical grid, the same one core/engine and tests/reference/test_pfb
// use: 64 channels, 2x oversampled, so the channel rate is 75 kS/s and the
// spacing is 37.5 kHz.
constexpr dsp::GridParams kGrid{
    .channels = 64,
    .taps_per_branch = 17,
    .decimation = 32,
};

// A coarser grid for wideband FM. A 200 kHz broadcast channel does not fit
// inside a 75 kS/s channel stream, and clamping it to what fits would test the
// clamp rather than the demodulator. Sixteen channels give 300 kS/s, which
// holds the signal with room for the fine filter's transition.
constexpr dsp::GridParams kWideGrid{
    .channels = 16,
    .taps_per_branch = 17,
    .decimation = 8,
};

// Rings for the bit-exact cases. Powers of two, as the kernels' mask
// arithmetic requires.
constexpr std::uint32_t kChanCapacity = 1U << 12;
constexpr std::uint32_t kChanMask = kChanCapacity - 1U;

// The channel ring is channel-major, so a receiver's chan_base is a non-zero
// origin inside a larger buffer. Using channel 3 rather than channel 0 means a
// kernel that ignored chan_base would read the wrong channel and the diff
// would say so.
constexpr std::uint32_t kChanIndex = 3;
constexpr std::uint32_t kChanBase = kChanIndex * kChanCapacity;

constexpr std::uint32_t kFineCapacity = 1U << 12;
constexpr std::uint32_t kFineMask = kFineCapacity - 1U;

// ---------------------------------------------------------------------------
// Plans and dispatches
// ---------------------------------------------------------------------------

dsp::VrxPlan make_plan(engine::Demod mode, dsp::Hertz centre, dsp::Hertz bandwidth,
                       const dsp::GridParams& grid,
                       engine::Deemphasis curve = engine::Deemphasis::Default,
                       dsp::SampleRate audio_rate = 0, bool stereo = true) {
    engine::VrxParams params;
    params.center = centre;
    params.bandwidth = bandwidth;
    params.demod = mode;
    params.deemphasis = curve;
    params.audio_rate = audio_rate;
    params.stereo = stereo;

    auto placed = engine::place(grid, kSourceRate, params);
    INFO(test::message_of(placed));
    REQUIRE(placed.has_value());

    auto planned = dsp::plan_vrx(grid, kSourceRate, params, *placed);
    INFO(test::message_of(planned));
    REQUIRE(planned.has_value());
    return *planned;
}

// The same, asking for the passband by its two edges rather than by a width
// the mode reshapes.
dsp::VrxPlan make_edge_plan(engine::Demod mode, dsp::Hertz centre, dsp::Hertz low,
                            dsp::Hertz high, const dsp::GridParams& grid) {
    engine::VrxParams params;
    params.center = centre;
    params.passband_low = low;
    params.passband_high = high;
    params.demod = mode;

    auto placed = engine::place(grid, kSourceRate, params);
    INFO(test::message_of(placed));
    REQUIRE(placed.has_value());

    auto planned = dsp::plan_vrx(grid, kSourceRate, params, *placed);
    INFO(test::message_of(planned));
    REQUIRE(planned.has_value());
    return *planned;
}

std::vector<dsp::Complex32> make_nco(const dsp::VrxPlan& plan) {
    auto built = dsp::build_nco_table(plan.fine.nco_log2);
    INFO(test::message_of(built));
    REQUIRE(built.has_value());
    return *built;
}

std::vector<dsp::Complex32> run_fine_on_gpu(const dsp::VrxFineConfig& config,
                                            const dsp::VrxFineParams& params,
                                            std::span<const dsp::Complex32> channel_ring,
                                            std::span<const dsp::Complex32> taps,
                                            std::span<const dsp::Complex32> nco,
                                            std::uint32_t local_size) {
    auto& context = test::shared_context();

    std::vector<dsp::Complex32> out(static_cast<std::size_t>(params.out_mask) + 1U,
                                    dsp::Complex32{});

    const std::uint32_t grid_constants[] = {config.taps, config.phases, config.nco_log2};

    gpu::KernelInvocation invocation;
    invocation.spirv = gpu::shaders::vrx_fine();
    invocation.inputs = {std::as_bytes(channel_ring), std::as_bytes(taps), std::as_bytes(nco)};
    invocation.outputs = {std::as_writable_bytes(std::span<dsp::Complex32>(out))};
    invocation.push_constants = std::as_bytes(std::span<const dsp::VrxFineParams>(&params, 1));
    invocation.invocations = params.count;
    invocation.local_size_x = local_size;
    invocation.grid_constants.assign(std::begin(grid_constants), std::end(grid_constants));

    const auto ran = gpu::run_kernel(context, invocation);
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());
    return out;
}

std::vector<float> run_demod_on_gpu(const dsp::VrxDemodConfig& config,
                                    const dsp::VrxDemodParams& params,
                                    std::span<const dsp::Complex32> fine_ring,
                                    std::span<const float> weights,
                                    std::uint32_t local_size) {
    auto& context = test::shared_context();

    std::vector<float> out(static_cast<std::size_t>(params.count) * config.channels, 0.0F);

    const std::uint32_t grid_constants[] = {config.mode,     config.decimation,
                                            config.audio_taps, config.dc_taps,
                                            config.channels,  config.pilot_taps};

    gpu::KernelInvocation invocation;
    invocation.spirv = gpu::shaders::vrx_demod();
    invocation.inputs = {std::as_bytes(fine_ring), std::as_bytes(weights)};
    invocation.outputs = {std::as_writable_bytes(std::span<float>(out))};
    invocation.push_constants = std::as_bytes(std::span<const dsp::VrxDemodParams>(&params, 1));
    invocation.invocations = params.count;
    invocation.local_size_x = local_size;
    invocation.grid_constants.assign(std::begin(grid_constants), std::end(grid_constants));

    const auto ran = gpu::run_kernel(context, invocation);
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());
    return out;
}

// Push constants built by hand rather than by fine_block, so the case can put
// the read window exactly where it wants it. Both sides get the same struct,
// so what fine_block would have produced is not what these cases are about;
// where the ring wraps is.
dsp::VrxFineParams fine_params(const dsp::VrxPlan& plan, std::uint32_t in_offset,
                               std::uint32_t count, dsp::SampleIndex phase_index) {
    const auto channel_rate = static_cast<std::uint32_t>(plan.channel_rate);
    const auto demod_rate = static_cast<std::uint32_t>(plan.demod_rate);

    dsp::VrxFineParams params;
    params.chan_base = kChanBase;
    params.chan_mask = kChanMask;
    params.in_offset = in_offset;
    params.out_offset = 17;
    params.out_mask = kFineMask;
    params.count = count;
    params.step_whole = channel_rate / demod_rate;
    params.step_rem = channel_rate % demod_rate;
    params.out_rate = demod_rate;
    params.frac0 = 12'345 % demod_rate;

    const std::uint64_t phase = dsp::nco_phase(plan.fine_nco_delta, phase_index);
    params.nco_phase_high = static_cast<std::uint32_t>(phase >> 32U);
    params.nco_phase_low = static_cast<std::uint32_t>(phase);
    params.nco_delta_high = static_cast<std::uint32_t>(plan.fine_nco_delta >> 32U);
    params.nco_delta_low = static_cast<std::uint32_t>(plan.fine_nco_delta);
    params.inv_out_rate = 1.0F / static_cast<float>(demod_rate);
    return params;
}

// The most audio samples a dispatch can ask for out of a ring of this
// capacity, which is the same bound core/dsp/vrx_reference.cpp's validate
// enforces. Computed rather than guessed because it differs by an order of
// magnitude between AM, which reaches back over its whole DC-removal window,
// and a product detector, which reaches back not at all.
std::uint32_t demod_count(const dsp::VrxDemodConfig& config, std::uint32_t capacity,
                          std::uint32_t desired) {
    // Through dsp::demod_fine_history rather than reproduced here. This
    // used to be a third copy of the reach, beside the one in
    // dsp::validate and the one in core/engine/vrx_stage.cpp, and a stereo
    // receiver's pilot filter would have been missing from all three.
    const std::uint32_t overhead = dsp::demod_fine_history(config) + 1U;
    REQUIRE(capacity > overhead);
    const std::uint32_t bound = (capacity - overhead) / config.decimation + 1U;
    return std::min(desired, bound);
}

dsp::VrxDemodParams demod_params(const dsp::VrxPlan& plan, std::uint32_t in_offset,
                                 std::uint32_t count) {
    dsp::VrxDemodParams params;
    params.in_mask = kFineMask;
    params.in_offset = in_offset;
    params.count = count;
    params.gain = plan.demod_gain;
    return params;
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------
//
// The tone estimators live in tests/support/tone_measure.h, because the
// engine suite checks the same things about the same chain and a second copy
// of a Goertzel would drift from this one.

// A bit-exact diff of two buffers that are both entirely zero passes, and so
// does a diff of two kernels that both did nothing. Every bit-exact case below
// asserts that something was actually computed before it asserts the two
// agree.
template <class T>
std::size_t nonzero_count(std::span<const T> values) {
    return static_cast<std::size_t>(
        std::count_if(values.begin(), values.end(), [](const T& v) { return v != T{}; }));
}

// A complex exponential at an absolute sample index, with the phase reduced
// before it reaches the transcendentals so a long ring does not lose
// precision to a large argument.
dsp::Complex32 tone_at(double frequency_hz, double rate, std::size_t index) {
    const double turns =
        std::fmod(frequency_hz * static_cast<double>(index) / rate, 1.0);
    const double angle = test::kTwoPi * turns;
    return dsp::Complex32{static_cast<float>(std::cos(angle)),
                          static_cast<float>(std::sin(angle))};
}

}  // namespace

// ---------------------------------------------------------------------------
// The deterministic transcendentals, no GPU needed
// ---------------------------------------------------------------------------

TEST_CASE("the deterministic transcendentals agree with the real functions", "[vrx][m1]") {
    // Bit-exactness against a twin says the device computes what the twin
    // computes. It says nothing about whether either computes atan2. These
    // four are built from Newton iterations and a fitted polynomial precisely
    // so they can be refereed, and this is the case that referees them.

    double worst_sqrt = 0.0;
    for (int i = -60; i <= 60; ++i) {
        const auto value = static_cast<float>(std::pow(2.0, i * 0.5) * 1.3);
        const double expected = std::sqrt(static_cast<double>(value));
        const double got = dsp::det_sqrt(value);
        worst_sqrt = std::max(worst_sqrt, std::abs(got - expected) / expected);
    }
    INFO("worst relative sqrt error " << worst_sqrt);
    CHECK(worst_sqrt < 1.0e-6);

    double worst_recip = 0.0;
    for (int i = -60; i <= 60; ++i) {
        const auto value = static_cast<float>(std::pow(2.0, i * 0.5) * 1.3);
        const double expected = 1.0 / static_cast<double>(value);
        const double got = dsp::det_recip(value);
        worst_recip = std::max(worst_recip, std::abs(got - expected) / expected);
    }
    INFO("worst relative reciprocal error " << worst_recip);
    CHECK(worst_recip < 1.0e-6);

    // The header claims 9.6e-08 radian over [0, 1], which it calls the float
    // evaluation floor rather than the fit's. Checked at twice that so the
    // case reports a changed polynomial rather than a rounding difference.
    double worst_atan = 0.0;
    for (int i = 0; i <= 2048; ++i) {
        const auto t = static_cast<float>(i) / 2048.0F;
        worst_atan = std::max(worst_atan, std::abs(static_cast<double>(dsp::det_atan_unit(t)) -
                                                   std::atan(static_cast<double>(t))));
    }
    INFO("worst atan error over [0, 1] " << worst_atan << " radian");
    CHECK(worst_atan < 2.0e-7);

    // Every octant, which is what the reflections exist for. A sign error in
    // one of them is a demodulator that works on half the waveform.
    double worst_atan2 = 0.0;
    for (int i = 0; i < 719; ++i) {
        const double angle = -3.14159265358979323846 + test::kTwoPi * static_cast<double>(i) / 719.0;
        const auto y = static_cast<float>(std::sin(angle) * 1.7);
        const auto x = static_cast<float>(std::cos(angle) * 1.7);
        const double expected = std::atan2(static_cast<double>(y), static_cast<double>(x));
        worst_atan2 =
            std::max(worst_atan2, std::abs(static_cast<double>(dsp::det_atan2(y, x)) - expected));
    }
    INFO("worst atan2 error over the full circle " << worst_atan2 << " radian");
    CHECK(worst_atan2 < 5.0e-7);

    // Both inputs zero is the one case with no angle to return.
    CHECK(dsp::det_atan2(0.0F, 0.0F) == 0.0F);

    // And the one documented disagreement with the IEEE function, asserted so
    // that it stays a decision rather than becoming a surprise.
    CHECK(dsp::det_atan2(-0.0F, -1.0F) > 3.0F);
}

TEST_CASE("the NCO table has exact quadrature entries and the phase does not drift",
          "[vrx][m1]") {
    constexpr std::uint32_t kLog2 = 12;
    constexpr std::uint32_t kSize = 1U << kLog2;

    auto built = dsp::build_nco_table(kLog2);
    INFO(test::message_of(built));
    REQUIRE(built.has_value());
    const auto& table = *built;
    REQUIRE(table.size() == kSize);

    // The same argument core/dsp/pfb.h makes for the twiddle table: a residue
    // in the imaginary part of the half-turn entry turns a free sign flip into
    // a real complex multiply and costs bit-exactness for nothing.
    CHECK(table[0] == dsp::Complex32{1.0F, 0.0F});
    CHECK(table[kSize / 4] == dsp::Complex32{0.0F, -1.0F});
    CHECK(table[kSize / 2] == dsp::Complex32{-1.0F, 0.0F});
    CHECK(table[3 * kSize / 4] == dsp::Complex32{0.0F, 1.0F});

    double worst = 0.0;
    for (const auto& entry : table) {
        worst = std::max(worst, std::abs(std::abs(std::complex<double>(entry)) - 1.0));
    }
    INFO("worst |W| deviation " << worst);
    CHECK(worst < 1.0e-7);

    // The claim the fixed-point phase exists for: at a billion samples the
    // phase is still where exact rational arithmetic says it should be. A
    // float32 accumulator is 0.26 radian out after four million.
    constexpr std::int64_t kFrequency = 2500;
    constexpr dsp::SampleRate kRate = 48'000;
    auto delta = dsp::nco_delta(kFrequency, 1, kRate);
    INFO(test::message_of(delta));
    REQUIRE(delta.has_value());

    constexpr dsp::SampleIndex kIndex = 1'000'000'000;
    const std::uint64_t got = dsp::nco_phase(*delta, kIndex);

    // The exact phase in turns is (index * f / rate) mod 1. Reducing the index
    // modulo the rate first keeps every product inside 64 bits without
    // changing the answer.
    const auto rate = static_cast<std::int64_t>(kRate);
    const auto reduced = static_cast<std::int64_t>(kIndex % static_cast<dsp::SampleIndex>(rate));
    const std::int64_t numerator = (reduced * kFrequency) % rate;
    const std::uint64_t exact = dsp::turn_fixed64(numerator, rate);

    const std::uint64_t difference = (got > exact) ? got - exact : exact - got;
    const double turns = static_cast<double>(difference) / std::pow(2.0, 64);
    INFO("phase error at sample " << kIndex << " is " << turns * test::kTwoPi << " radian");
    CHECK(turns * test::kTwoPi < 1.0e-8);
}

// ---------------------------------------------------------------------------
// The passband: resolution, defaults and the fit to one channel
// ---------------------------------------------------------------------------

TEST_CASE("the bandwidth shorthand expands to the geometry the planner used to build",
          "[vrx][m1]") {
    // The check that the generalisation to two edges reduces correctly. Every
    // mode's shorthand expansion is the band plan_vrx derived from one
    // symmetric bandwidth before edges existed: symmetric for six of the
    // eight, [0, B] for USB and [-B, 0] for LSB, which is the table in
    // core/shaders/vrx_fine.comp read as a passband rather than as three
    // special cases.
    constexpr dsp::Hertz kWidth = 3'000;

    struct Expectation {
        engine::Demod mode;
        dsp::Passband band;
    };

    const Expectation expected[] = {
        {engine::Demod::Raw, {-1'500, 1'500}}, {engine::Demod::Am, {-1'500, 1'500}},
        {engine::Demod::Nfm, {-1'500, 1'500}}, {engine::Demod::Wfm, {-1'500, 1'500}},
        {engine::Demod::Usb, {0, 3'000}},      {engine::Demod::Lsb, {-3'000, 0}},
        {engine::Demod::Dsb, {-1'500, 1'500}}, {engine::Demod::Cw, {-1'500, 1'500}},
    };

    for (const auto& want : expected) {
        INFO("mode " << engine::demod_name(want.mode));
        engine::VrxParams params;
        params.demod = want.mode;
        params.bandwidth = kWidth;

        auto got = dsp::resolve_passband(params);
        INFO(test::message_of(got));
        REQUIRE(got.has_value());
        CHECK(got->low == want.band.low);
        CHECK(got->high == want.band.high);
        CHECK(got->width() == kWidth);
    }

    // An odd bandwidth resolves symmetric and one hertz narrow rather than
    // asymmetric by one, because that is the filter the planner has always
    // built: it passed bandwidth/2 to design_fine_taps as a half-width, so
    // only the reported figure ever carried the odd hertz.
    engine::VrxParams odd;
    odd.demod = engine::Demod::Nfm;
    odd.bandwidth = 501;
    auto resolved_odd = dsp::resolve_passband(odd);
    REQUIRE(resolved_odd.has_value());
    CHECK(resolved_odd->low == -250);
    CHECK(resolved_odd->high == 250);

    // The pair wins over the shorthand, and the shorthand is not consulted.
    engine::VrxParams both;
    both.demod = engine::Demod::Usb;
    both.bandwidth = 99'999;
    both.passband_low = 300;
    both.passband_high = 2'700;
    auto resolved_both = dsp::resolve_passband(both);
    REQUIRE(resolved_both.has_value());
    CHECK(resolved_both->low == 300);
    CHECK(resolved_both->high == 2'700);

    // An empty or inverted band is refused with both numbers in the message,
    // rather than being reordered into something nobody asked for.
    engine::VrxParams inverted;
    inverted.passband_low = 2'700;
    inverted.passband_high = 300;
    auto refused = dsp::resolve_passband(inverted);
    CHECK_FALSE(refused.has_value());
    if (!refused) {
        const std::string text = test::message_of(refused);
        CHECK(text.find("2700") != std::string::npos);
        CHECK(text.find("300") != std::string::npos);
    }
}

TEST_CASE("every mode has a default passband and SSB's is anchored on the carrier",
          "[vrx][m1]") {
    // The table exists so that a client changing mode has one place to read
    // what that mode wants, rather than each client inventing its own. These
    // are the widths the CLI carried privately, reshaped.
    struct Expectation {
        engine::Demod mode;
        dsp::Passband band;
    };

    const Expectation expected[] = {
        {engine::Demod::Raw, {-6'000, 6'000}},     {engine::Demod::Am, {-5'000, 5'000}},
        {engine::Demod::Nfm, {-8'000, 8'000}},     {engine::Demod::Wfm, {-100'000, 100'000}},
        {engine::Demod::Usb, {300, 2'700}},        {engine::Demod::Lsb, {-2'700, -300}},
        {engine::Demod::Dsb, {-3'000, 3'000}},     {engine::Demod::Cw, {-250, 250}},
    };

    for (const auto& want : expected) {
        INFO("mode " << engine::demod_name(want.mode));
        const dsp::Passband got = dsp::default_passband(want.mode);
        CHECK(got.low == want.band.low);
        CHECK(got.high == want.band.high);
        CHECK(got.width() > 0);
    }

    // The property that a width could not express: neither sideband mode's
    // default contains the carrier, and the two are mirror images.
    const dsp::Passband usb = dsp::default_passband(engine::Demod::Usb);
    const dsp::Passband lsb = dsp::default_passband(engine::Demod::Lsb);
    CHECK(usb.low > 0);
    CHECK(lsb.high < 0);
    CHECK(usb.low == -lsb.high);
    CHECK(usb.high == -lsb.low);
}

TEST_CASE("the channel fit moves the edge that does not fit and leaves the other",
          "[vrx][m1]") {
    engine::VrxParams params;
    params.center = 196'500;
    params.demod = engine::Demod::Usb;
    params.passband_low = 300;
    params.passband_high = 2'700;

    auto placed = engine::place(kGrid, kSourceRate, params);
    INFO(test::message_of(placed));
    REQUIRE(placed.has_value());

    // A passband this narrow is nowhere near the channel's limit, so nothing
    // moves and nothing claims to have moved.
    CHECK(placed->granted_low == 300);
    CHECK(placed->granted_high == 2'700);
    CHECK_FALSE(placed->bandwidth_clamped);

    const dsp::Hertz limit = dsp::max_channel_bandwidth(*placed) / 2;
    REQUIRE(limit > 0);

    // One edge past the limit and the other well inside it. The edge that
    // does not fit is pulled to the limit; the one that does is untouched,
    // which is the whole difference from the single width this replaced.
    const dsp::Passband asked{-1'000, limit + 5'000};
    const dsp::Passband got = dsp::clamp_to_channel(*placed, asked);
    CHECK(got.low == -1'000);
    CHECK(got.high == limit);

    // A band inside the limit on both sides comes back untouched.
    const dsp::Passband inside{-limit, limit};
    CHECK(dsp::clamp_to_channel(*placed, inside) == inside);

    // THE LOW EDGE, WHICH NOTHING EXERCISED. The case above and every RPC
    // case moved the high edge, so the mirror branch was live code with no
    // test behind it. LSB is the mode that reaches that way by default, so
    // it is the one asked here.
    const dsp::Passband asked_low{-limit - 5'000, 1'000};
    const dsp::Passband got_low = dsp::clamp_to_channel(*placed, asked_low);
    CHECK(got_low.low == -limit);
    CHECK(got_low.high == 1'000);

    // Both at once: a request wider than the channel on both sides keeps
    // its centre and loses the same amount at each end, which is exactly
    // what the one-width clamp this replaced always did.
    const dsp::Passband asked_both{-limit - 5'000, limit + 5'000};
    const dsp::Passband got_both = dsp::clamp_to_channel(*placed, asked_both);
    CHECK(got_both.low == -limit);
    CHECK(got_both.high == limit);
}

TEST_CASE("a band the channel cannot reach comes back empty rather than inverted",
          "[vrx][m1]") {
    // The post-condition. Clamping only the outside of each edge left a
    // band lying wholly past one limit inverted, so every caller measured a
    // negative width and reported a full channel when the truth was a
    // receiver pointed somewhere the channel does not reach.
    engine::VrxParams params;
    params.center = 196'500;
    params.demod = engine::Demod::Usb;
    params.passband_low = 300;
    params.passband_high = 2'700;

    auto placed = engine::place(kGrid, kSourceRate, params);
    REQUIRE(placed.has_value());

    const dsp::Hertz limit = dsp::max_channel_bandwidth(*placed) / 2;
    REQUIRE(limit > 0);

    const dsp::Passband above{limit + 1'000, limit + 2'000};
    const dsp::Passband got_above = dsp::clamp_to_channel(*placed, above);
    CHECK(got_above.low <= got_above.high);
    CHECK(got_above.width() == 0);

    const dsp::Passband below{-limit - 2'000, -limit - 1'000};
    const dsp::Passband got_below = dsp::clamp_to_channel(*placed, below);
    CHECK(got_below.low <= got_below.high);
    CHECK(got_below.width() == 0);

    // And the planner refuses it naming where the request was, not the
    // residual. The residual is a rational nobody reads a position off, and
    // leading with it said the channel was full when the receiver had been
    // pointed out of it.
    engine::VrxParams outside = params;
    outside.passband_low = limit + 1'000;
    outside.passband_high = limit + 2'000;

    auto refused = dsp::plan_vrx(kGrid, kSourceRate, outside, *placed);
    REQUIRE_FALSE(refused.has_value());
    const std::string text = test::message_of(refused);
    INFO(text);
    CHECK(text.find(std::to_string(limit + 1'000)) != std::string::npos);
    CHECK(text.find(std::to_string(limit)) != std::string::npos);

    // The cheap query refuses the same request, because the two share the
    // fit rather than each carrying their own copy of it.
    auto query = dsp::demod_rate_for(outside, *placed, 48'000);
    CHECK_FALSE(query.has_value());
}

TEST_CASE("a placement one channel can carry nothing for is refused, not planned",
          "[vrx][m1]") {
    // max_channel_bandwidth is zero when the receiver sits a whole half
    // channel or more off the channel's centre, and plan_vrx used to have
    // its own guard against that. The guard went when clamp_to_channel took
    // over the fit, and because the clamp handed the request straight back
    // in that case the receiver was planned instead of refused and place()
    // called it unclamped.
    //
    // place() cannot produce this placement: it puts a receiver on the
    // NEAREST channel, so on the 2x-oversampled grid the residual is at
    // most a quarter of a channel rate. A caller that builds a placement by
    // hand can, and the RPC surface takes one from the wire.
    engine::VrxPlacement broken;
    broken.channel = 3;
    broken.channel_rate = 75'000;
    broken.residual_numerator = 40'000;
    broken.residual_denominator = 1;
    REQUIRE(dsp::max_channel_bandwidth(broken) == 0);

    const dsp::Passband band{-4'000, 4'000};
    CHECK(dsp::clamp_to_channel(broken, band).width() == 0);

    engine::VrxParams params;
    params.center = 196'500;
    params.demod = engine::Demod::Nfm;
    params.passband_low = band.low;
    params.passband_high = band.high;

    auto refused = dsp::plan_vrx(kGrid, kSourceRate, params, broken);
    REQUIRE_FALSE(refused.has_value());
    INFO(test::message_of(refused));
    CHECK(test::message_of(refused).find("nothing") != std::string::npos);

    CHECK_FALSE(dsp::demod_rate_for(params, broken, 48'000).has_value());
}

TEST_CASE("the cheap demodulation rate query answers what the planner builds",
          "[vrx][m1]") {
    // demod_rate_for exists so a client dragging a passband can tell a
    // retune that is a push constant from one that is a rebuild without
    // paying for three filter tables per frame of the gesture. That claim
    // is only worth anything if the two agree, so it is checked across
    // every mode rather than asserted in the header.
    struct Case {
        engine::Demod mode;
        dsp::Hertz low;
        dsp::Hertz high;
        dsp::SampleRate audio_rate;
    };

    const Case cases[] = {
        {engine::Demod::Raw, -6'000, 6'000, 48'000},
        {engine::Demod::Am, -5'000, 5'000, 48'000},
        {engine::Demod::Nfm, -8'000, 8'000, 48'000},
        {engine::Demod::Usb, 300, 2'700, 48'000},
        {engine::Demod::Lsb, -2'700, -300, 16'000},
        {engine::Demod::Dsb, -3'000, 3'000, 24'000},
        {engine::Demod::Cw, -250, 250, 8'000},

        // Off-centre, so the reach term rather than the shape floor is what
        // decides, and asymmetric so the two edges cannot cancel.
        {engine::Demod::Nfm, 1'000, 9'000, 16'000},
        {engine::Demod::Usb, -9'000, -1'000, 12'000},
    };

    for (const auto& want : cases) {
        INFO("mode " << engine::demod_name(want.mode) << " [" << want.low << ", " << want.high
                     << "] at " << want.audio_rate << " S/s");
        engine::VrxParams params;
        params.center = 196'500;
        params.demod = want.mode;
        params.passband_low = want.low;
        params.passband_high = want.high;
        params.audio_rate = want.audio_rate;

        auto placed = engine::place(kGrid, kSourceRate, params);
        REQUIRE(placed.has_value());

        auto cheap = dsp::demod_rate_for(params, *placed, want.audio_rate);
        INFO(test::message_of(cheap));
        REQUIRE(cheap.has_value());

        auto planned = dsp::plan_vrx(kGrid, kSourceRate, params, *placed);
        INFO(test::message_of(planned));
        REQUIRE(planned.has_value());

        CHECK(*cheap == planned->demod_rate);

        // A whole multiple of the audio rate, which is the property that
        // makes the decimation an integer.
        CHECK(*cheap % want.audio_rate == 0);
    }
}

namespace {

// The function minimum_demod_rate's shorthand overload replaced: one
// symmetric bandwidth, a pitch, and four cases. Written out here rather
// than referred to, because the header's table is a comparison against it
// and a comparison needs both sides present to mean anything.
//
// The switch is over the 32-bit word and therefore carries a default, which
// is the opposite of the rule the three functions in vrx_reference.cpp
// follow. They switch over engine::Demod so that /w14062 makes a ninth
// demodulator a build error. This one is a frozen copy of a function that
// no longer exists and must not move when a ninth is added, so it takes the
// word and answers the shared floor for anything it does not know, exactly
// as the original did.
[[nodiscard]] dsp::Hertz retired_minimum_demod_rate(std::uint32_t mode, dsp::Hertz bandwidth,
                                                    dsp::Hertz cw_pitch) {
    if (bandwidth <= 0) {
        return 0;
    }
    const dsp::Hertz floor_rate = (3 * bandwidth + 1) / 2;
    switch (mode) {
        case dsp::kDemodAm:
        case dsp::kDemodUsb:
        case dsp::kDemodLsb: return std::max(floor_rate, 2 * bandwidth);
        case dsp::kDemodCw: return std::max(floor_rate, 2 * cw_pitch + bandwidth);
        default: return floor_rate;
    }
}

}  // namespace

TEST_CASE("the generalised minimum rate against the shorthand it replaced, mode by mode",
          "[vrx][m1]") {
    // The claim first written down was that the generalisation produces the
    // same hertz as the cases it replaced. The correction to that was that
    // it is exact for an even bandwidth and 1 to 2 Hz lower for an odd one
    // on every mode. Both are wrong, and the second was contradicted by the
    // USB line of the test that was here.
    //
    // The gap follows the shorthand expansion, not the parity. USB and LSB
    // expand to a one-sided band that states the full width, so they are
    // exact at every bandwidth. The six symmetric modes take a half-width
    // twice, so an odd request resolves one hertz narrow, which costs 2 Hz
    // at the shape floor and 1 Hz at the reach term.
    //
    // Every cell of the header's table is asserted below, per mode, so the
    // table cannot go stale without this failing.
    for (dsp::Hertz pitch : {0, 1, 250, 700, 5'000}) {
        for (dsp::Hertz bandwidth : {1, 2, 3, 500, 501, 3'000, 3'001, 12'000, 12'001}) {
            const dsp::Hertz half = bandwidth / 2;
            const bool even = bandwidth % 2 == 0;

            for (std::uint32_t mode = dsp::kDemodRaw; mode <= dsp::kDemodCw; ++mode) {
                INFO("mode " << engine::demod_name(static_cast<engine::Demod>(mode))
                             << ", bandwidth " << bandwidth << ", pitch " << pitch);

                const dsp::Hertz now = dsp::minimum_demod_rate(mode, bandwidth, pitch);
                const dsp::Hertz before = retired_minimum_demod_rate(mode, bandwidth, pitch);

                // The closed form the header states, and then the gap. Both,
                // because a closed form that is merely self-consistent with
                // the gap would let the pair drift together.
                dsp::Hertz expect = 0;
                dsp::Hertz gap_low = 0;
                dsp::Hertz gap_high = 0;
                switch (mode) {
                    case dsp::kDemodUsb:
                    case dsp::kDemodLsb:
                        expect = 2 * bandwidth;
                        break;
                    case dsp::kDemodAm:
                        expect = 4 * half;
                        gap_low = gap_high = even ? 0 : 2;
                        break;
                    case dsp::kDemodCw:
                        // The one row with two answers for an odd width:
                        // the shape floor loses 2 Hz to the narrower band
                        // and the reach term loses 1, and which of them
                        // wins depends on the pitch.
                        expect = std::max(3 * half, 2 * pitch + 2 * half);
                        gap_low = even ? 0 : 1;
                        gap_high = even ? 0 : 2;
                        break;
                    default:
                        expect = 3 * half;
                        gap_low = gap_high = even ? 0 : 2;
                        break;
                }

                if (bandwidth == 1 && mode != dsp::kDemodUsb && mode != dsp::kDemodLsb) {
                    // The row that is not 1 to 2 Hz off anything. A
                    // one-hertz symmetric request expands to [0, 0], so the
                    // empty-band rule answers before any mode's case runs.
                    // CW's closed form would say 2*pitch here and does not
                    // get the chance.
                    //
                    // The gap is stated in closed form rather than as
                    // `before`, which is what it said until 2026-09-20.
                    // Taking it from `before` made the bound whatever the
                    // retired function happened to return, so the header's
                    // figure for this row could be wrong and this assertion
                    // could not notice. It was: the header read the CW gap
                    // as 2P + 1, which is 1 at a pitch of zero, where the
                    // shape floor of 2 actually wins.
                    expect = 0;
                    gap_low = gap_high = mode == dsp::kDemodCw
                                             ? std::max<dsp::Hertz>(2, 2 * pitch + 1)
                                             : 2;
                }

                CHECK(now == expect);
                CHECK(before - now >= gap_low);
                CHECK(before - now <= gap_high);
            }
        }
    }

    // The planner refuses the one-hertz symmetric request the row above
    // answers zero for, so nothing downstream ever sees that zero. place()
    // is not the refusal: it grants an empty band and reports it as one.
    engine::VrxParams narrow;
    narrow.center = 196'500;
    narrow.demod = engine::Demod::Nfm;
    narrow.bandwidth = 1;
    auto narrow_place = engine::place(kGrid, kSourceRate, narrow);
    REQUIRE(narrow_place.has_value());
    CHECK(narrow_place->granted_high == narrow_place->granted_low);
    CHECK_FALSE(dsp::plan_vrx(kGrid, kSourceRate, narrow, *narrow_place).has_value());
    CHECK_FALSE(dsp::demod_rate_for(narrow, *narrow_place, 48'000).has_value());

    // A pitch below zero is read as zero, the way plan_vrx and
    // demod_rate_for read VrxParams::cw_pitch. Passed through signed it
    // translated the band the wrong way: -3000 on a 1 kHz request reaches
    // 3500 Hz from the mix centre and asked for 7000 S/s where the engine
    // runs at 1500.
    for (dsp::Hertz bandwidth : {1'000, 3'000}) {
        INFO("bandwidth " << bandwidth);
        CHECK(dsp::minimum_demod_rate(dsp::kDemodCw, bandwidth, -3'000) ==
              dsp::minimum_demod_rate(dsp::kDemodCw, bandwidth, 0));
    }
}

TEST_CASE("a plan's fine tap table is exactly the length its config implies", "[vrx][m1]") {
    // dsp::VrxShape leaves the table's length out and cites
    // fine_tap_table_size for why. Nothing measured that, and the
    // consequence of it being false is not a refusal: core/engine/-
    // vrx_stage.cpp sizes its device and staging buffers once and every
    // retune past the shape comparison copies that fixed byte count out of
    // the new plan's table.
    //
    // So the property is asserted here for every mode, and at two
    // bandwidths per mode that land on different tap counts, rather than
    // being left to the one shape a GPU test happens to build.
    struct Case {
        engine::Demod mode;
        dsp::Hertz bandwidth;
        const dsp::GridParams* grid;
    };

    const Case cases[] = {
        {engine::Demod::Raw, 12'000, &kGrid},      {engine::Demod::Raw, 2'400, &kGrid},
        {engine::Demod::Am, 10'000, &kGrid},       {engine::Demod::Am, 6'000, &kGrid},
        {engine::Demod::Nfm, 12'000, &kGrid},      {engine::Demod::Nfm, 25'000, &kGrid},
        {engine::Demod::Wfm, 200'000, &kWideGrid}, {engine::Demod::Wfm, 150'000, &kWideGrid},
        {engine::Demod::Usb, 2'700, &kGrid},       {engine::Demod::Usb, 1'800, &kGrid},
        {engine::Demod::Lsb, 2'700, &kGrid},       {engine::Demod::Lsb, 1'800, &kGrid},
        {engine::Demod::Dsb, 6'000, &kGrid},       {engine::Demod::Dsb, 3'000, &kGrid},
        {engine::Demod::Cw, 500, &kGrid},          {engine::Demod::Cw, 250, &kGrid},
    };

    for (const Case& want : cases) {
        INFO("mode " << engine::demod_name(want.mode) << ", bandwidth " << want.bandwidth);
        const dsp::Hertz centre = (want.grid == &kWideGrid) ? 160'000 : 196'500;
        const auto plan = make_plan(want.mode, centre, want.bandwidth, *want.grid);
        CHECK(plan.fine_taps.size() == dsp::fine_tap_table_size(plan.fine));

        // And the shape a retune compares carries the config that implies
        // it, so two plans with equal shapes have equally long tables.
        const dsp::VrxShape shape = dsp::shape_of(plan);
        CHECK(dsp::fine_tap_table_size(shape.fine) == plan.fine_taps.size());
    }
}

// ---------------------------------------------------------------------------
// The fine stage, bit for bit
// ---------------------------------------------------------------------------

TEST_CASE("the fine stage matches its CPU twin bit-exactly", "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x5652580000000001ULL;

    // Deliberately off the channel centre, so the residual is non-zero and
    // the output mixer does real work. A receiver placed exactly on a grid
    // centre has an NCO delta of zero, and the rotation is then the table's
    // first entry forever, which tests nothing.
    const auto plan = make_plan(engine::Demod::Nfm, 196'500, 12'000, kGrid);
    INFO("taps " << plan.fine.taps << ", phases " << plan.fine.phases << ", channel rate "
                 << plan.channel_rate << ", demod rate " << plan.demod_rate);

    const auto nco = make_nco(plan);

    test::SeededInput input(kSeed);
    const auto channel_ring = input.complexes(kChanBase + kChanCapacity);

    // Close to the top of the ring, so every read crosses the wrap. The
    // (base - k) & chan_mask line is the most suspicious-looking arithmetic in
    // the kernel and the one most worth exercising on a real driver.
    const auto params = fine_params(plan, kChanCapacity - 40U, 1024, 7'654'321);

    const auto gpu_result =
        run_fine_on_gpu(plan.fine, params, channel_ring, plan.fine_taps, nco, 64);

    std::vector<dsp::Complex32> cpu_result(kFineCapacity, dsp::Complex32{});
    const auto computed = dsp::reference_vrx_fine(plan.fine, params, channel_ring,
                                                  plan.fine_taps, nco, cpu_result);
    INFO(test::message_of(computed));
    REQUIRE(computed.has_value());

    INFO("gpu wrote " << nonzero_count(std::span<const dsp::Complex32>(gpu_result))
                      << " non-zero samples, cpu "
                      << nonzero_count(std::span<const dsp::Complex32>(cpu_result)) << " of "
                      << params.count << " outputs");
    REQUIRE(nonzero_count(std::span<const dsp::Complex32>(cpu_result)) > params.count / 2);
    REQUIRE(nonzero_count(std::span<const dsp::Complex32>(gpu_result)) > params.count / 2);

    const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
    INFO(comparison.report);
    CHECK(comparison.identical);
    CHECK(comparison.max_ulp_error == 0);
}

TEST_CASE("the fine stage is bit-exact on values chosen to provoke rounding",
          "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The tap loop is a complex multiply-accumulate, which is exactly the
    // shape a shader compiler fuses. Uniform input in [-1, 1] rarely
    // distinguishes a fused multiply-add from two separate operations; these
    // values do. The `precise` qualifiers in the kernel are what has to hold
    // here, and nothing else in the suite tests them on this kernel.
    constexpr std::uint64_t kSeed = 0x5652580000000002ULL;

    const auto plan = make_plan(engine::Demod::Nfm, 196'500, 12'000, kGrid);
    const auto nco = make_nco(plan);

    test::SeededInput input(kSeed);
    const auto channel_ring = input.adversarial_complexes(kChanBase + kChanCapacity);

    // A small in_offset, so base - k underflows and wraps from the bottom of
    // the ring rather than over the top. Between this case and the one above,
    // both directions of the wrap are covered.
    const auto params = fine_params(plan, 7, 1024, 7'654'321);

    const auto gpu_result =
        run_fine_on_gpu(plan.fine, params, channel_ring, plan.fine_taps, nco, 64);

    std::vector<dsp::Complex32> cpu_result(kFineCapacity, dsp::Complex32{});
    REQUIRE(dsp::reference_vrx_fine(plan.fine, params, channel_ring, plan.fine_taps, nco,
                                    cpu_result)
                .has_value());

    const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
    INFO(comparison.report);
    CHECK(comparison.identical);
}

TEST_CASE("the fine stage is bit-exact at every workgroup size", "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    auto& context = test::shared_context();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x5652580000000003ULL;

    const auto plan = make_plan(engine::Demod::Usb, 196'500, 3'000, kGrid);
    const auto nco = make_nco(plan);

    test::SeededInput input(kSeed);
    const auto channel_ring = input.complexes(kChanBase + kChanCapacity);
    const auto params = fine_params(plan, kChanCapacity - 40U, 512, 11);

    std::vector<dsp::Complex32> cpu_result(kFineCapacity, dsp::Complex32{});
    REQUIRE(dsp::reference_vrx_fine(plan.fine, params, channel_ring, plan.fine_taps, nco,
                                    cpu_result)
                .has_value());

    // The workgroup size is a scheduling decision and must not change the
    // answer. A kernel whose output depends on it has a race or a benign-
    // looking out-of-bounds read.
    for (const std::uint32_t local_size : {32U, 64U, 128U, 256U}) {
        if (local_size > context.info().max_workgroup_size_x) {
            continue;
        }
        const auto gpu_result =
            run_fine_on_gpu(plan.fine, params, channel_ring, plan.fine_taps, nco, local_size);
        const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
        INFO("local_size_x = " << local_size);
        INFO(comparison.report);
        CHECK(comparison.identical);
    }
}

TEST_CASE("the fine stage gives the same samples however the stream is blocked",
          "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The property the whole fixed-point NCO and the stateless resampler
    // recurrence exist for. An earlier version of the kernel accumulated a
    // 0.32 increment from the block's first output, which is more accurate
    // and is not this: a third of the outputs changed when the block size
    // did. Retroactive decode and faster-than-realtime replay both re-enter
    // the stream at an arbitrary index, so an output that depends on where
    // the blocks fell is not usable for either.
    constexpr std::uint64_t kSeed = 0x5652580000000004ULL;
    constexpr dsp::SampleIndex kFirstOutput = 5'000'000'011ULL;
    constexpr std::uint32_t kTotal = 768;
    constexpr std::uint32_t kPiece = 256;

    const auto plan = make_plan(engine::Demod::Nfm, 196'500, 12'000, kGrid);
    const auto nco = make_nco(plan);

    test::SeededInput input(kSeed);
    const auto channel_ring = input.complexes(kChanBase + kChanCapacity);

    auto whole_block =
        dsp::fine_block(plan, kChanBase, kChanMask, kFineMask, kFirstOutput, kTotal);
    INFO(test::message_of(whole_block));
    REQUIRE(whole_block.has_value());

    const auto whole = run_fine_on_gpu(plan.fine, whole_block->params, channel_ring,
                                       plan.fine_taps, nco, 64);

    for (std::uint32_t piece = 0; piece < kTotal / kPiece; ++piece) {
        auto part = dsp::fine_block(plan, kChanBase, kChanMask, kFineMask,
                                    kFirstOutput + piece * kPiece, kPiece);
        INFO(test::message_of(part));
        REQUIRE(part.has_value());

        const auto partial = run_fine_on_gpu(plan.fine, part->params, channel_ring,
                                             plan.fine_taps, nco, 64);

        // Both dispatches write output j to slot j & out_mask, so the slots
        // line up without any bookkeeping here.
        for (std::uint32_t i = 0; i < kPiece; ++i) {
            const std::uint32_t slot = (part->params.out_offset + i) & kFineMask;
            INFO("piece " << piece << " sample " << i << " at ring slot " << slot);
            CHECK(partial[slot] == whole[slot]);
        }
    }
}

TEST_CASE("the fine stage is bit-exact three hours into a stream", "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The case a 32-bit truncation survives for three and a half minutes at
    // 20 MS/s and then does not. Every push constant the kernel reads is 32
    // bits; the absolute index is 64 and stays on the host, so this is the
    // case that checks the seam between them rather than the kernel alone.
    constexpr std::uint64_t kSeed = 0x5652580000000005ULL;
    constexpr dsp::SampleIndex kThreeHours = 48'000ULL * 3600ULL * 3ULL;

    const auto plan = make_plan(engine::Demod::Am, 196'500, 10'000, kGrid);
    const auto nco = make_nco(plan);

    test::SeededInput input(kSeed);
    const auto channel_ring = input.complexes(kChanBase + kChanCapacity);

    auto block = dsp::fine_block(plan, kChanBase, kChanMask, kFineMask, kThreeHours, 1024);
    INFO(test::message_of(block));
    REQUIRE(block.has_value());
    INFO("first input " << block->first_input << ", in_offset " << block->params.in_offset
                        << ", frac0 " << block->params.frac0);

    const auto gpu_result =
        run_fine_on_gpu(plan.fine, block->params, channel_ring, plan.fine_taps, nco, 64);

    std::vector<dsp::Complex32> cpu_result(kFineCapacity, dsp::Complex32{});
    REQUIRE(dsp::reference_vrx_fine(plan.fine, block->params, channel_ring, plan.fine_taps,
                                    nco, cpu_result)
                .has_value());

    const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
    INFO(comparison.report);
    CHECK(comparison.identical);
}

// ---------------------------------------------------------------------------
// The demodulators, bit for bit
// ---------------------------------------------------------------------------

TEST_CASE("every demodulator matches its CPU twin bit-exactly", "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x5652580000000006ULL;

    struct Case {
        engine::Demod mode;
        dsp::Hertz bandwidth;
        const dsp::GridParams* grid;
    };

    const Case cases[] = {
        {engine::Demod::Raw, 12'000, &kGrid},   {engine::Demod::Am, 10'000, &kGrid},
        {engine::Demod::Nfm, 12'000, &kGrid},   {engine::Demod::Wfm, 200'000, &kWideGrid},
        {engine::Demod::Usb, 3'000, &kGrid},    {engine::Demod::Lsb, 3'000, &kGrid},
        {engine::Demod::Dsb, 6'000, &kGrid},    {engine::Demod::Cw, 500, &kGrid},
    };

    test::SeededInput input(kSeed);
    const auto fine_ring = input.complexes(kFineCapacity);

    for (const auto& item : cases) {
        const dsp::Hertz centre = (item.grid == &kWideGrid) ? 160'000 : 196'500;
        const auto plan = make_plan(item.mode, centre, item.bandwidth, *item.grid);

        const std::uint32_t count = demod_count(plan.demod, kFineCapacity, 2048);

        // A small in_offset, so the detector's history reads and the audio
        // decimation filter's both underflow and wrap. AM reaches back over a
        // whole DC-removal window, which is hundreds of samples, so this is
        // not a corner case for that mode: it is every sample.
        const auto params = demod_params(plan, 7, count);

        // WFM arrives here in its default shape, which from 2026-09-20 is
        // 75 us de-emphasis folded into the audio filter and a stereo
        // decoder with a pilot bandpass in front of it. Asserted rather
        // than assumed, because if the default ever moved back to mono this
        // case would go on passing while covering half the kernel.
        if (item.mode == engine::Demod::Wfm) {
            REQUIRE(plan.demod.channels == 2U);
            REQUIRE(plan.demod.pilot_taps > 0);
        }

        INFO("mode " << engine::demod_name(item.mode) << ", decimation "
                     << plan.demod.decimation << ", audio taps " << plan.demod.audio_taps
                     << ", dc taps " << plan.demod.dc_taps << ", pilot taps "
                     << plan.demod.pilot_taps << ", channels " << plan.demod.channels
                     << ", gain " << plan.demod_gain << ", " << count << " audio samples");

        const auto gpu_result =
            run_demod_on_gpu(plan.demod, params, fine_ring, plan.demod_weights, 64);

        std::vector<float> cpu_result(
            static_cast<std::size_t>(count) * plan.demod.channels, 0.0F);
        const auto computed = dsp::reference_vrx_demod(plan.demod, params, fine_ring,
                                                       plan.demod_weights, cpu_result);
        INFO(test::message_of(computed));
        REQUIRE(computed.has_value());

        INFO("gpu wrote " << nonzero_count(std::span<const float>(gpu_result))
                          << " non-zero values, cpu "
                          << nonzero_count(std::span<const float>(cpu_result)) << " of "
                          << cpu_result.size());
        REQUIRE(nonzero_count(std::span<const float>(cpu_result)) > cpu_result.size() / 2);
        REQUIRE(nonzero_count(std::span<const float>(gpu_result)) > gpu_result.size() / 2);

        const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
        INFO(comparison.report);
        CHECK(comparison.identical);
        CHECK(comparison.max_ulp_error == 0);
    }
}

TEST_CASE("the demodulators are bit-exact at every workgroup size", "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    auto& context = test::shared_context();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x5652580000000007ULL;

    test::SeededInput input(kSeed);
    const auto fine_ring = input.complexes(kFineCapacity);

    // AM and NFM between them cover every deterministic transcendental the
    // file had: AM runs det_sqrt once per DC-window tap, NFM runs det_atan2.
    // Stereo WFM is here because it added the third, det_recip, and because
    // it is the only branch with two accumulations running over one loop and
    // two outputs written per invocation, which is the shape a workgroup
    // size is most likely to disturb.
    struct Case {
        engine::Demod mode;
        dsp::Hertz centre;
        dsp::Hertz bandwidth;
        const dsp::GridParams* grid;
    };

    const Case cases[] = {
        {engine::Demod::Am, 196'500, 10'000, &kGrid},
        {engine::Demod::Nfm, 196'500, 10'000, &kGrid},
        {engine::Demod::Wfm, 160'000, 200'000, &kWideGrid},
    };

    for (const auto& item : cases) {
        const auto plan = make_plan(item.mode, item.centre, item.bandwidth, *item.grid);
        const std::uint32_t count = demod_count(plan.demod, kFineCapacity, 1024);
        const auto params = demod_params(plan, 7, count);

        std::vector<float> cpu_result(
            static_cast<std::size_t>(count) * plan.demod.channels, 0.0F);
        REQUIRE(dsp::reference_vrx_demod(plan.demod, params, fine_ring, plan.demod_weights,
                                         cpu_result)
                    .has_value());

        for (const std::uint32_t local_size : {32U, 64U, 128U, 256U}) {
            if (local_size > context.info().max_workgroup_size_x) {
                continue;
            }
            const auto gpu_result =
                run_demod_on_gpu(plan.demod, params, fine_ring, plan.demod_weights, local_size);
            const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
            INFO("mode " << engine::demod_name(item.mode) << ", " << plan.demod.channels
                         << " channels, local_size_x = " << local_size);
            INFO(comparison.report);
            CHECK(comparison.identical);
        }
    }
}

TEST_CASE("the product detectors are bit-exact on values chosen to provoke rounding",
          "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // Restricted to the modes whose arithmetic is linear, which is the raw tap
    // and the four product detectors. The adversarial set reaches 1e38 and
    // 1e-45, and det_rsqrt's stated domain is roughly 2^-100 to 2^100: outside
    // it an intermediate overflows or lands in the denormals this project
    // flushes to zero. Feeding AM and FM values their own header says are out
    // of range would test undefined behaviour rather than conformance, and a
    // green result would mean nothing.
    constexpr std::uint64_t kSeed = 0x5652580000000008ULL;

    test::SeededInput input(kSeed);
    const auto fine_ring = input.adversarial_complexes(kFineCapacity);

    for (const auto mode : {engine::Demod::Raw, engine::Demod::Usb, engine::Demod::Lsb,
                            engine::Demod::Dsb, engine::Demod::Cw}) {
        const dsp::Hertz bandwidth = (mode == engine::Demod::Cw) ? 500 : 3'000;
        const auto plan = make_plan(mode, 196'500, bandwidth, kGrid);
        const std::uint32_t count = demod_count(plan.demod, kFineCapacity, 1024);
        const auto params = demod_params(plan, 7, count);

        const auto gpu_result =
            run_demod_on_gpu(plan.demod, params, fine_ring, plan.demod_weights, 64);

        std::vector<float> cpu_result(
            static_cast<std::size_t>(count) * plan.demod.channels, 0.0F);
        REQUIRE(dsp::reference_vrx_demod(plan.demod, params, fine_ring, plan.demod_weights,
                                         cpu_result)
                    .has_value());

        const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
        INFO("mode " << engine::demod_name(mode));
        INFO(comparison.report);
        CHECK(comparison.identical);
    }
}

// ---------------------------------------------------------------------------
// Behaviour, which is what catches a convention that is wrong consistently
// ---------------------------------------------------------------------------

namespace {

// A larger channel ring for the behavioural cases, sized so the filter's whole
// support and the block's read window fit without wrapping. The tone then has
// no discontinuity anywhere the kernel looks, so what comes out is the
// filter's answer rather than the ring's.
constexpr std::uint32_t kToneChanCapacity = 1U << 13;
constexpr std::uint32_t kToneChanMask = kToneChanCapacity - 1U;
constexpr std::uint32_t kToneFineCapacity = 1U << 12;
constexpr std::uint32_t kToneFineMask = kToneFineCapacity - 1U;
constexpr std::uint32_t kToneOutputs = 2048;

// Chosen so fine_block lands the first input at 512, which leaves room below
// it for the longest fine filter the planner will build (256 taps) and room
// above for the whole block.
constexpr dsp::SampleIndex kToneFirstOutput = 328;

// Runs one receiver's fine stage over a channel carrying a single tone, and
// returns the block's outputs in order.
std::vector<dsp::Complex32> fine_tone_response(const dsp::VrxPlan& plan, double tone_hz) {
    const auto nco = make_nco(plan);

    std::vector<dsp::Complex32> channel_ring(kToneChanCapacity);
    for (std::size_t n = 0; n < channel_ring.size(); ++n) {
        channel_ring[n] = tone_at(tone_hz, static_cast<double>(plan.channel_rate), n);
    }

    auto block = dsp::fine_block(plan, 0, kToneChanMask, kToneFineMask, kToneFirstOutput,
                                 kToneOutputs);
    INFO(test::message_of(block));
    REQUIRE(block.has_value());

    // The ring index is the absolute channel index here, which is what makes
    // the tone above the signal the kernel actually reads. Both ends of the
    // read window have to stay inside the ring for that to hold, filter
    // history included.
    REQUIRE(block->first_input >= plan.fine.taps);
    REQUIRE(block->newest_input < kToneChanCapacity);
    REQUIRE(block->params.in_offset == block->first_input);

    const auto ring = run_fine_on_gpu(plan.fine, block->params, channel_ring, plan.fine_taps,
                                      nco, 64);

    // Contiguous, because the block was placed so it does not wrap the output
    // ring either.
    const auto start = static_cast<std::ptrdiff_t>(block->params.out_offset);
    REQUIRE(block->params.out_offset + kToneOutputs <= kToneFineCapacity);
    return std::vector<dsp::Complex32>(ring.begin() + start,
                                       ring.begin() + start + kToneOutputs);
}

}  // namespace

TEST_CASE("a tone at the receiver's centre arrives at DC with unit magnitude",
          "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The receiver sits 9 kHz off the channel centre, so the fine stage has a
    // real residual to undo. What arrives should be a constant: unit
    // magnitude, because the tap table is normalised to unit gain at the
    // filter's centre, and zero frequency, because the mixer translates the
    // residual to DC.
    const auto plan = make_plan(engine::Demod::Nfm, 196'500, 12'000, kGrid);
    const double residual = static_cast<double>(plan.placement.residual_numerator) /
                            static_cast<double>(plan.placement.residual_denominator);
    INFO("channel " << plan.placement.channel << ", residual " << residual << " Hz, "
                    << plan.fine.taps << " taps");
    REQUIRE(residual != 0.0);

    const auto out = fine_tone_response(plan, residual);
    const auto fit = test::measure_complex_tone(out, static_cast<double>(plan.demod_rate));

    INFO("magnitude " << fit.magnitude << ", residual frequency " << fit.frequency_hz << " Hz");
    CHECK(fit.magnitude == Approx(1.0).margin(0.01));
    CHECK(std::abs(fit.frequency_hz) < 0.5);
}

TEST_CASE("a CW receiver lands the carrier on the operator's pitch", "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // CW is the one mode whose filter centre and mix frequency differ by
    // something the operator chose. A carrier at the receiver's centre must
    // come out at the pitch rather than at DC, because a carrier at DC is
    // inaudible. Getting the sign wrong puts it at minus the pitch, which
    // sounds identical on a loudspeaker and is wrong.
    engine::VrxParams request;
    request.center = 196'500;
    request.bandwidth = 500;
    request.demod = engine::Demod::Cw;

    auto placed = engine::place(kGrid, kSourceRate, request);
    REQUIRE(placed.has_value());
    auto planned = dsp::plan_vrx(kGrid, kSourceRate, request, *placed);
    INFO(test::message_of(planned));
    REQUIRE(planned.has_value());
    const auto& plan = *planned;

    const double residual = static_cast<double>(plan.placement.residual_numerator) /
                            static_cast<double>(plan.placement.residual_denominator);

    const auto out = fine_tone_response(plan, residual);
    const auto fit = test::measure_complex_tone(out, static_cast<double>(plan.demod_rate));

    INFO("magnitude " << fit.magnitude << ", output frequency " << fit.frequency_hz << " Hz");
    CHECK(fit.magnitude == Approx(1.0).margin(0.01));
    CHECK(fit.frequency_hz == Approx(static_cast<double>(request.cw_pitch)).margin(1.0));
}

TEST_CASE("an asymmetric passband is bit-exact against the twin", "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    constexpr std::uint64_t kSeed = 0x565258000000000AULL;

    // The property the asymmetric passband rests on, asserted rather than
    // argued. The fine kernel consumes a complex tap table and one NCO
    // delta; it has no bandwidth, no edges and no mode, so an asymmetric
    // filter is different numbers in a buffer both sides read. Off the
    // channel centre so the residual is non-zero and the tap table's
    // modulation carries a filter centre that is neither the mix centre nor
    // a half bandwidth from it.
    const auto plan = make_edge_plan(engine::Demod::Usb, 196'500, 300, 2'700, kGrid);
    INFO("passband " << plan.passband.low << " to " << plan.passband.high << " Hz, "
                     << plan.fine.taps << " taps, demod rate " << plan.demod_rate);
    REQUIRE(plan.passband.low == 300);
    REQUIRE(plan.passband.high == 2'700);

    const auto nco = make_nco(plan);

    test::SeededInput input(kSeed);
    const auto channel_ring = input.complexes(kChanBase + kChanCapacity);
    const auto params = fine_params(plan, kChanCapacity - 40U, 1024, 7'654'321);

    const auto gpu_result =
        run_fine_on_gpu(plan.fine, params, channel_ring, plan.fine_taps, nco, 64);

    std::vector<dsp::Complex32> cpu_result(kFineCapacity, dsp::Complex32{});
    const auto computed = dsp::reference_vrx_fine(plan.fine, params, channel_ring,
                                                  plan.fine_taps, nco, cpu_result);
    INFO(test::message_of(computed));
    REQUIRE(computed.has_value());

    REQUIRE(nonzero_count(std::span<const dsp::Complex32>(cpu_result)) > params.count / 2);

    const auto comparison = test::diff(gpu_result, cpu_result, kSeed);
    INFO(comparison.report);
    CHECK(comparison.identical);
    CHECK(comparison.max_ulp_error == 0);
}

TEST_CASE("an asymmetric USB passband passes inside its edges and rejects below them",
          "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // What a width could not ask for and this can: the carrier plus 300 to
    // plus 2700 hertz, which is where an SSB signal actually is. Measured in
    // decibels rather than eyeballed, at three places.
    constexpr dsp::Hertz kLow = 300;
    constexpr dsp::Hertz kHigh = 2'700;
    const auto plan = make_edge_plan(engine::Demod::Usb, 196'500, kLow, kHigh, kGrid);

    const double residual = static_cast<double>(plan.placement.residual_numerator) /
                            static_cast<double>(plan.placement.residual_denominator);
    const double centre = 0.5 * static_cast<double>(kLow + kHigh);
    INFO(plan.fine.taps << " taps, transition " << plan.fine_transition_hz << " Hz, stopband "
                        << plan.fine_stopband_db << " dB, passband centre " << centre << " Hz");

    // Unity at the middle of the band the operator asked for, which is 1500
    // Hz above the carrier and not the carrier itself. A symmetric filter of
    // the same width would have its unity gain at the carrier.
    const auto inside = fine_tone_response(plan, residual + centre);
    const auto inside_fit =
        test::measure_complex_tone(inside, static_cast<double>(plan.demod_rate));
    CHECK(inside_fit.magnitude == Approx(1.0).margin(0.01));
    CHECK(inside_fit.frequency_hz == Approx(centre).margin(1.0));

    const auto response_db = [&](double offset_from_carrier) {
        const auto out = fine_tone_response(plan, residual + offset_from_carrier);
        const auto fit = test::measure_complex_tone(out, static_cast<double>(plan.demod_rate));
        return 20.0 * std::log10(std::max(fit.magnitude, 1.0e-12) / inside_fit.magnitude);
    };

    // 200 Hz below the low edge. This is inside the filter's transition
    // rather than out in its stopband, and deliberately so: a 2.4 kHz filter
    // asks for a 1.2 kHz transition and the tap cap gives it about 1.5, so
    // the stopband does not begin until nearly 2 kHz from the centre.
    // Rejection here is a statement about where the EDGE is, and a
    // symmetric filter of the same width centred on the carrier would be
    // passing this tone at very nearly unity.
    const double below_db = response_db(static_cast<double>(kLow) - 200.0);
    INFO("200 Hz below the low edge: " << below_db << " dB");
    CHECK(below_db < -8.0);

    // The suppressed carrier itself, 300 Hz below the low edge.
    const double carrier_db = response_db(0.0);
    INFO("at the suppressed carrier: " << carrier_db << " dB");
    CHECK(carrier_db < -15.0);

    // 900 Hz below the carrier, which is one whole width below the filter's
    // centre and so past the stopband edge the planner designed for. The
    // three figures together are the skirt, measured rather than asserted
    // from the design.
    const double deep_db = response_db(-900.0);
    INFO("900 Hz below the carrier: " << deep_db << " dB");
    CHECK(deep_db < -40.0);
    CHECK(deep_db < carrier_db);
    CHECK(carrier_db < below_db);
}

TEST_CASE("a USB receiver keeps its own sideband and rejects the other", "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The measurement the planner's transition-width choice was made for. Set
    // by the anti-alias bound alone the fine filter becomes a gentle hump with
    // no flat region, and a 3 kHz USB receiver rejects its unwanted sideband
    // by 2.1 dB. Asking for the stopband one half-bandwidth past the passband
    // edge takes it past 80 dB. Nothing but this case notices the difference,
    // because both filters look plausible and both are bit-exact.
    constexpr dsp::Hertz kBandwidth = 3'000;
    const auto plan = make_plan(engine::Demod::Usb, 196'500, kBandwidth, kGrid);

    const double residual = static_cast<double>(plan.placement.residual_numerator) /
                            static_cast<double>(plan.placement.residual_denominator);
    const double half = static_cast<double>(kBandwidth) / 2.0;
    INFO(plan.fine.taps << " taps, transition " << plan.fine_transition_hz << " Hz, stopband "
                        << plan.fine_stopband_db << " dB");

    // The passband runs from the suppressed carrier upwards, so its centre is
    // half a bandwidth above the residual, and that is where the tap table is
    // centred and where the gain is unity.
    const auto wanted = fine_tone_response(plan, residual + half);
    const auto wanted_fit = test::measure_complex_tone(wanted, static_cast<double>(plan.demod_rate));

    // The mirror image, one whole bandwidth below the filter's centre, which
    // is past the stopband edge.
    const auto unwanted = fine_tone_response(plan, residual - half);
    const auto unwanted_fit =
        test::measure_complex_tone(unwanted, static_cast<double>(plan.demod_rate));

    const double rejection_db =
        20.0 * std::log10(std::max(unwanted_fit.magnitude, 1.0e-12) / wanted_fit.magnitude);
    INFO("wanted magnitude " << wanted_fit.magnitude << " at " << wanted_fit.frequency_hz
                             << " Hz, unwanted magnitude " << unwanted_fit.magnitude
                             << ", rejection " << rejection_db << " dB");

    CHECK(wanted_fit.magnitude == Approx(1.0).margin(0.01));
    CHECK(wanted_fit.frequency_hz == Approx(half).margin(1.0));

    // 60 dB rather than the design's 80, so the case reports a convention
    // error rather than a one-decibel drift in the Kaiser estimate. The
    // designed depth is asserted by the planner's own figure above.
    CHECK(rejection_db < -60.0);
}

TEST_CASE("each demodulator recovers its own modulation at the stated level",
          "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The audio convention, asserted rather than described: a unit-amplitude
    // signal fully modulating its own mode produces audio that swings to
    // exactly +/-1. These cases hand the demodulator the complex baseband the
    // fine stage would have produced, so what is under test is the detector
    // and its gain and nothing upstream of them.
    //
    // The baseband is built periodic in the ring, so the detector's history
    // reads are continuous wherever they wrap and the filters have no
    // start-up transient to skip past.
    //
    // The modulation sits near 1 kHz for every mode, which matters for AM:
    // its DC-removal window corners at 200 Hz, so a modulation below that
    // would be measuring the highpass rather than the envelope detector.
    constexpr double kTargetModulationHz = 1000.0;

    struct Case {
        engine::Demod mode;
        dsp::Hertz bandwidth;
        const dsp::GridParams* grid;
        dsp::Hertz centre;
        std::uint32_t count;
    };

    const Case cases[] = {
        {engine::Demod::Am, 10'000, &kGrid, 196'500, 2048},
        {engine::Demod::Nfm, 12'000, &kGrid, 196'500, 2048},
        {engine::Demod::Wfm, 200'000, &kWideGrid, 160'000, 512},
        {engine::Demod::Usb, 3'000, &kGrid, 196'500, 2048},
        {engine::Demod::Lsb, 3'000, &kGrid, 196'500, 2048},
        {engine::Demod::Dsb, 6'000, &kGrid, 196'500, 2048},
        {engine::Demod::Cw, 500, &kGrid, 196'500, 2048},
    };

    for (const auto& item : cases) {
        // Every case here runs with the de-emphasis curve OFF, including
        // WFM, whose default is 75 us from 2026-09-20. That is not the
        // curve being excused: this case measures the DETECTOR's gain
        // convention, and a receiver that also runs a curve reads 0.907 at
        // a kilohertz because the curve is doing exactly what it is for.
        // Asserting 1.0 with the curve on would mean asserting a number
        // that is a property of the audio frequency the case happened to
        // pick. The curve has its own case below, which asserts the shape
        // of the response rather than one point on it.
        //
        // Mono for the same reason: this measures one detector's output
        // level, and a stereo receiver writes two interleaved channels,
        // which is a different shape and has its own cases.
        const auto plan = make_plan(item.mode, item.centre, item.bandwidth, *item.grid,
                                    engine::Deemphasis::None, 0, false);
        REQUIRE(plan.demod.channels == ((item.mode == engine::Demod::Raw) ? 2U : 1U));
        const std::uint32_t count = demod_count(plan.demod, kFineCapacity, item.count);

        const auto demod_rate = static_cast<double>(plan.demod_rate);
        const auto audio_rate = static_cast<double>(plan.output_rate);

        // A whole number of periods across the ring, so the ring is seamless
        // and every wrapped history read is continuous.
        const auto periods = std::max(
            1.0, std::round(kTargetModulationHz * static_cast<double>(kFineCapacity) /
                            demod_rate));
        const double modulation_hz =
            demod_rate * periods / static_cast<double>(kFineCapacity);

        std::vector<dsp::Complex32> fine_ring(kFineCapacity);
        for (std::size_t m = 0; m < fine_ring.size(); ++m) {
            const double turns =
                std::fmod(modulation_hz * static_cast<double>(m) / demod_rate, 1.0);
            const double angle = test::kTwoPi * turns;

            if (item.mode == engine::Demod::Am) {
                // A unit carrier at 100 percent modulation. Its envelope is
                // 1 + cos, which touches zero at the trough, so this is the
                // deepest modulation the mode has.
                const auto envelope = static_cast<float>(1.0 + std::cos(angle));
                fine_ring[m] = dsp::Complex32{envelope, 0.0F};
            } else if (item.mode == engine::Demod::Nfm || item.mode == engine::Demod::Wfm) {
                // Sinusoidal frequency modulation at exactly the peak
                // deviation the mode's channel plan implies, so correct audio
                // is +/-1 by definition of the gain.
                const double beta =
                    static_cast<double>(plan.deviation) / modulation_hz;
                const double phase = beta * std::sin(angle);
                fine_ring[m] = dsp::Complex32{static_cast<float>(std::cos(phase)),
                                              static_cast<float>(std::sin(phase))};
            } else {
                // A unit-amplitude tone in the passband. The product detector
                // takes its real part, so correct audio is a cosine of
                // amplitude one.
                fine_ring[m] = dsp::Complex32{static_cast<float>(std::cos(angle)),
                                              static_cast<float>(std::sin(angle))};
            }
        }

        const auto params = demod_params(plan, 1024, count);

        INFO("mode " << engine::demod_name(item.mode) << ", demod rate " << plan.demod_rate
                     << ", audio rate " << plan.output_rate << ", decimation "
                     << plan.demod.decimation << ", gain " << plan.demod_gain
                     << ", modulation " << modulation_hz << " Hz, deviation "
                     << plan.deviation << " Hz");

        const auto audio =
            run_demod_on_gpu(plan.demod, params, fine_ring, plan.demod_weights, 64);

        // A margin rather than a settling time. Nothing here has a transient,
        // because the ring is periodic and every filter reads valid signal
        // from its first tap; the margin exists so a case that acquires one
        // reports an amplitude error rather than hiding it in an average.
        const std::size_t skip = audio.size() / 8;
        REQUIRE(audio.size() > skip);
        const std::span<const float> settled(audio.data() + skip, audio.size() - skip);

        // At the audio rate for every mode but the raw tap, and the audio
        // frequency is the modulation frequency: decimation moves the rate,
        // not the tone.
        const auto fit = test::measure_audio_tone(settled, audio_rate, modulation_hz);
        INFO("recovered amplitude " << fit.amplitude << ", purity " << fit.purity);

        CHECK(fit.amplitude == Approx(1.0).margin(0.02));
        CHECK(fit.purity > 0.95);
    }
}

TEST_CASE("the raw tap hands back exactly what it was given", "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The raw tap is not a demodulator. Its gain is one and its job is to hand
    // out the complex baseband unchanged, so the right assertion is equality
    // and not a tolerance.
    constexpr std::uint64_t kSeed = 0x5652580000000009ULL;

    const auto plan = make_plan(engine::Demod::Raw, 196'500, 12'000, kGrid);
    REQUIRE(plan.demod.mode == dsp::kDemodRaw);
    REQUIRE(plan.demod.decimation == 1U);
    REQUIRE(plan.demod_gain == 1.0F);

    test::SeededInput input(kSeed);
    const auto fine_ring = input.complexes(kFineCapacity);

    constexpr std::uint32_t kCount = 1024;
    const auto params = demod_params(plan, 900, kCount);

    const auto audio = run_demod_on_gpu(plan.demod, params, fine_ring, plan.demod_weights, 64);
    REQUIRE(audio.size() == 2U * kCount);

    for (std::uint32_t i = 0; i < kCount; ++i) {
        const auto& expected = fine_ring[(params.in_offset + i) & kFineMask];
        INFO("sample " << i);
        CHECK(audio[2U * i] == expected.real());
        CHECK(audio[2U * i + 1U] == expected.imag());
    }
}

// ---------------------------------------------------------------------------
// De-emphasis
// ---------------------------------------------------------------------------
//
// THE TEST THAT SHOULD HAVE CAUGHT THIS AND DID NOT.
// tests/decode/test_rds_bits.cpp renders a broadcast station, demodulates it
// and decodes RDS, and it passed on every commit while no de-emphasis
// existed anywhere in the tree. It passed because RDS rides 57 kHz and an
// audio curve never touches it. So every case here asserts on the AUDIO
// BAND, which is the band that was wrong, and the end-to-end one renders
// with pre-emphasis and requires the recovered audio to match the signal
// that went IN to the pre-emphasis rather than the one that came out of it.

namespace {

// A larger fine ring for the curve cases. The folded audio filter is around
// 880 taps at 48 kHz of audio, which leaves too few outputs in a 4096-sample
// ring to measure a 500 Hz tone against.
constexpr std::uint32_t kCurveFineCapacity = 1U << 14;
constexpr std::uint32_t kCurveFineMask = kCurveFineCapacity - 1U;

// The one-pole's magnitude in the continuous-time prototype the transmitter
// pre-emphasised with: |1 / (1 + j*2*pi*f*tau)|.
//
// This is what the receiver is trying to be and NOT what it is. The kernel
// runs the impulse-invariant discrete pole, exp(-1/(tau*Fa)), whose response
// is this one aliased; the gap is zero at DC and grows with frequency, and
// the cases below measure it rather than assuming it away.
[[nodiscard]] double analog_deemphasis_gain(engine::Deemphasis curve, double frequency_hz) {
    const double tau = engine::deemphasis_seconds(curve);
    if (tau <= 0.0) {
        return 1.0;
    }
    const double omega_tau = test::kTwoPi * frequency_hz * tau;
    return 1.0 / std::sqrt(1.0 + omega_tau * omega_tau);
}

[[nodiscard]] double to_db(double ratio) { return 20.0 * std::log10(ratio); }

// A ring of sinusoidal FM at exactly the plan's peak deviation, periodic in
// the ring so every history read wraps onto continuous signal. Correct audio
// with no curve is +/-1 by the definition of the mode's gain, so what the
// curve does is read straight off the amplitude.
[[nodiscard]] std::vector<dsp::Complex32> fm_ring_at(const dsp::VrxPlan& plan,
                                                     std::uint32_t capacity,
                                                     double modulation_hz) {
    const auto demod_rate = static_cast<double>(plan.demod_rate);
    const double beta = static_cast<double>(plan.deviation) / modulation_hz;

    std::vector<dsp::Complex32> ring(capacity);
    for (std::size_t m = 0; m < ring.size(); ++m) {
        const double turns =
            std::fmod(modulation_hz * static_cast<double>(m) / demod_rate, 1.0);
        const double phase = beta * std::sin(test::kTwoPi * turns);
        ring[m] = dsp::Complex32{static_cast<float>(std::cos(phase)),
                                 static_cast<float>(std::sin(phase))};
    }
    return ring;
}

// The nearest modulation frequency that fits a whole number of periods in
// the ring, so the ring is seamless.
[[nodiscard]] double seamless_hz(const dsp::VrxPlan& plan, std::uint32_t capacity,
                                 double wanted_hz) {
    const auto demod_rate = static_cast<double>(plan.demod_rate);
    const double periods =
        std::max(1.0, std::round(wanted_hz * static_cast<double>(capacity) / demod_rate));
    return demod_rate * periods / static_cast<double>(capacity);
}

// The recovered audio amplitude at one modulation frequency, through the
// GPU kernel, for a plan whose ring is kCurveFineCapacity wide.
[[nodiscard]] test::AudioFit curve_response(const dsp::VrxPlan& plan, double modulation_hz) {
    REQUIRE(plan.demod.channels == 1U);
    const auto ring = fm_ring_at(plan, kCurveFineCapacity, modulation_hz);

    const std::uint32_t count =
        demod_count(plan.demod, kCurveFineCapacity, kCurveFineCapacity);
    REQUIRE(count > 64);

    dsp::VrxDemodParams params;
    params.in_mask = kCurveFineMask;
    params.in_offset = 4096;
    params.count = count;
    params.gain = plan.demod_gain;

    const auto audio = run_demod_on_gpu(plan.demod, params, ring, plan.demod_weights, 64);

    // No settling to skip: the ring is periodic and every tap of every
    // filter reads valid signal. The eighth taken off the front is the same
    // margin the audio-convention case uses, so a transient that did appear
    // would show as an amplitude error rather than being averaged away.
    const std::size_t skip = audio.size() / 8;
    REQUIRE(audio.size() > skip);
    const std::span<const float> settled(audio.data() + skip, audio.size() - skip);
    return test::measure_audio_tone(settled, static_cast<double>(plan.output_rate),
                                    modulation_hz);
}

}  // namespace

TEST_CASE("the de-emphasis curve resolves per mode and never reaches a composite tap",
          "[vrx][m1]") {
    // No GPU. This is the resolution rule, which is where the defect that
    // matters most would live: a curve on the 171000 S/s composite tap would
    // pull the 57 kHz data band down 28.6 dB, and tools/cli --rds decoded a
    // real station through exactly that path on 2026-09-20.

    SECTION("broadcast FM at an ordinary audio rate takes the North American curve") {
        const auto plan = make_plan(engine::Demod::Wfm, 160'000, 200'000, kWideGrid);
        CHECK(plan.deemphasis == engine::Deemphasis::Us75);
        CHECK(plan.deemphasis_taps > 1);
        CHECK_FALSE(plan.deemphasis_truncated_early);

        // Folded into the decimation filter rather than run beside it, so
        // the kernel still applies exactly one FIR.
        CHECK(plan.demod.audio_taps ==
              plan.audio_decimation_taps +
                  (plan.deemphasis_taps - 1U) * plan.demod.decimation);

        // And the audio band is the channel plan's 15 kHz with its stopband
        // starting before the 19 kHz pilot, not 0.45 of the audio rate.
        CHECK(plan.audio_pass_hz == Approx(15'000.0));
        CHECK(plan.audio_stop_hz == Approx(19'000.0));

        const std::string words = dsp::describe_audio_chain(plan);
        INFO(words);
        CHECK(words.find("75us de-emphasis") != std::string::npos);
    }

    SECTION("the composite tap takes no curve and keeps its wide audio filter") {
        // A WFM receiver at 171000 S/s of audio is the RDS composite tap:
        // three times the 57 kHz subcarrier and 144 times the bit rate.
        const auto plan =
            make_plan(engine::Demod::Wfm, 160'000, 200'000, kWideGrid,
                      engine::Deemphasis::Default, 171'000);
        REQUIRE(plan.audio_rate == 171'000);
        CHECK(plan.deemphasis == engine::Deemphasis::None);
        CHECK(plan.deemphasis_taps == 1);

        // The 15 kHz ceiling must not reach this receiver either. Its
        // passband edge is 0.4 of the audio rate, which is 68400 Hz, and the
        // composite reaches 59375.
        CHECK(plan.audio_pass_hz == Approx(0.4 * 171'000.0));
        CHECK(plan.demod.audio_taps == plan.audio_decimation_taps);

        const std::string words = dsp::describe_audio_chain(plan);
        INFO(words);
        CHECK(words.find("NO DE-EMPHASIS") != std::string::npos);
    }

    SECTION("every other mode defaults to none and the raw tap refuses one outright") {
        for (const engine::Demod mode :
             {engine::Demod::Am, engine::Demod::Nfm, engine::Demod::Usb, engine::Demod::Lsb,
              engine::Demod::Dsb, engine::Demod::Cw}) {
            INFO("mode " << engine::demod_name(mode));
            CHECK(engine::resolve_deemphasis(mode, engine::Deemphasis::Default, 48'000) ==
                  engine::Deemphasis::None);
        }

        // Raw is complex baseband and is not audio, so a curve asked for
        // explicitly is not applied rather than being an error.
        CHECK(engine::resolve_deemphasis(engine::Demod::Raw, engine::Deemphasis::Us75,
                                         48'000) == engine::Deemphasis::None);
        const auto raw = make_plan(engine::Demod::Raw, 196'500, 12'000, kGrid,
                                   engine::Deemphasis::Us75);
        CHECK(raw.deemphasis == engine::Deemphasis::None);
        CHECK(raw.demod.audio_taps == 1U);
    }

    SECTION("an explicit curve is honoured on the modes that can carry one") {
        const auto eu = make_plan(engine::Demod::Wfm, 160'000, 200'000, kWideGrid,
                                  engine::Deemphasis::Eu50);
        CHECK(eu.deemphasis == engine::Deemphasis::Eu50);

        const auto nfm =
            make_plan(engine::Demod::Nfm, 196'500, 12'000, kGrid, engine::Deemphasis::Us75);
        CHECK(nfm.deemphasis == engine::Deemphasis::Us75);

        // VrxStatus answers the same question from the same function, so a
        // status and a plan cannot disagree about which curve is running.
        engine::VrxStatus status;
        status.params.demod = engine::Demod::Wfm;
        status.params.deemphasis = engine::Deemphasis::Default;
        status.params.audio_rate = 48'000;
        status.resolved_audio_rate = 48'000;
        CHECK(status.applied_deemphasis() == engine::Deemphasis::Us75);
        status.params.audio_rate = 171'000;
        status.resolved_audio_rate = 171'000;
        CHECK(status.applied_deemphasis() == engine::Deemphasis::None);
    }

    SECTION("a receiver that took the engine's default is resolved against what it got") {
        // VrxParams::audio_rate of zero is "the engine's default" and the
        // echo on a status keeps it as zero, deliberately, so that feeding a
        // status back into set_vrx_params does not pin the receiver to
        // whatever the default happened to be. Both answers below therefore
        // have to come off resolved_audio_rate, which the graph fills with
        // what with_audio_rate decided.
        //
        // EngineConfig::audio_rate is a field, so "the default is programme
        // audio" is a property of one configuration rather than of the
        // engine. An engine built at the composite rate hands a receiver
        // that named no rate a multiplex tap, and resolving against the echo
        // reported Us75 and stereo for it.
        engine::VrxStatus composite;
        composite.params.demod = engine::Demod::Wfm;
        composite.params.deemphasis = engine::Deemphasis::Default;
        composite.params.stereo = true;
        composite.params.audio_rate = 0;
        composite.resolved_audio_rate = 171'000;
        CHECK(composite.applied_deemphasis() == engine::Deemphasis::None);
        CHECK_FALSE(composite.decoding_stereo());

        // The ordinary configuration, same echo, and it still answers the
        // way it always did.
        engine::VrxStatus programme = composite;
        programme.resolved_audio_rate = 48'000;
        CHECK(programme.applied_deemphasis() == engine::Deemphasis::Us75);
        CHECK(programme.decoding_stereo());
    }

    SECTION("a flat curve leaves the audio filter bit-identical to no curve at all") {
        const auto off = make_plan(engine::Demod::Wfm, 160'000, 200'000, kWideGrid,
                                   engine::Deemphasis::None);
        REQUIRE(off.demod.audio_taps == off.audio_decimation_taps);

        // fold_deemphasis short-circuits the identity rather than
        // convolving with it, so "the curve is off" and "there is no curve"
        // are the same table and not two that differ in the last place.
        auto identity = dsp::design_deemphasis_taps(engine::Deemphasis::None, 48'000);
        REQUIRE(identity.has_value());
        REQUIRE(identity->taps.size() == 1);
        CHECK(identity->taps[0] == 1.0F);

        auto folded = dsp::fold_deemphasis(dsp::ConstRealSpan(off.demod_weights)
                                               .first(off.audio_decimation_taps),
                                           dsp::ConstRealSpan(identity->taps), 7);
        REQUIRE(folded.has_value());
        for (std::size_t i = 0; i < folded->size(); ++i) {
            INFO("tap " << i);
            REQUIRE((*folded)[i] == off.demod_weights[i]);
        }
    }
}

TEST_CASE("a de-emphasised receiver follows the one-pole across the audio band",
          "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The curve, measured rather than described. A receiver with no curve
    // recovers +/-1 at every modulation frequency, because that is what the
    // discriminator's gain is defined to do, so the amplitude a de-emphasised
    // receiver recovers IS the curve's magnitude response and needs no
    // reference measurement to divide by.
    //
    // The bar is the CONTINUOUS-time prototype, because that is what the
    // transmitter pre-emphasised with. The kernel runs the impulse-invariant
    // discrete pole instead, which is the standard one-pole form and is
    // aliased rather than exact; the gap is reported per point and bounded
    // once, so a future change that swapped the pole mapping would move a
    // number here rather than passing silently.
    constexpr double kWarpBudgetDb = 0.5;

    const double wanted[] = {500.0, 1000.0, 2122.0, 5000.0, 8000.0};

    for (const engine::Deemphasis curve :
         {engine::Deemphasis::Us75, engine::Deemphasis::Eu50}) {
        // Mono, so the recovered amplitude is one number and not an
        // interleaved pair. The curve is applied to L and R after the
        // matrix, which for a linear matrix is the same filter on the sum
        // and the difference, so measuring it on the sum channel alone
        // measures all of it.
        const auto plan =
            make_plan(engine::Demod::Wfm, 160'000, 200'000, kWideGrid, curve, 0, false);
        REQUIRE(plan.deemphasis == curve);

        const auto flat = make_plan(engine::Demod::Wfm, 160'000, 200'000, kWideGrid,
                                    engine::Deemphasis::None, 0, false);

        for (const double target : wanted) {
            const double modulation_hz = seamless_hz(plan, kCurveFineCapacity, target);

            const auto with_curve = curve_response(plan, modulation_hz);
            const auto without = curve_response(flat, modulation_hz);

            const double expected = analog_deemphasis_gain(curve, modulation_hz);
            const double error_db = to_db(with_curve.amplitude / expected);

            INFO(engine::deemphasis_name(curve)
                 << " at " << modulation_hz << " Hz: recovered " << with_curve.amplitude
                 << " (" << to_db(with_curve.amplitude) << " dB), the continuous curve wants "
                 << expected << " (" << to_db(expected) << " dB), gap " << error_db
                 << " dB. With no curve the same receiver recovers " << without.amplitude
                 << ", purity " << with_curve.purity);

            // The reference leg: no curve is still the mode's +/-1.
            CHECK(without.amplitude == Approx(1.0).margin(0.02));

            CHECK(std::abs(error_db) < kWarpBudgetDb);
            CHECK(with_curve.purity > 0.95);
        }
    }
}

TEST_CASE("a pre-emphasised station comes back at the level it had before pre-emphasis",
          "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The whole loop, and the case tests/decode/test_rds_bits.cpp could not
    // be: a real broadcast station is rendered WITH pre-emphasis by
    // core/dsp/synth/wfm_mod.cpp, demodulated by the kernel, and the
    // recovered audio is required to match the programme level that went IN
    // to the pre-emphasis. With no de-emphasis it comes back at the
    // pre-emphasised level instead, which at 6 kHz is 9.5 dB too loud, and
    // that is the defect an operator heard on air as harshness.
    constexpr std::uint64_t kSeed = 0x565258000000000CULL;
    constexpr dsp::SampleRate kStationRate = 336'000;
    constexpr std::size_t kStationSamples = 40'000;
    constexpr std::uint32_t kRingCapacity = 1U << 16;
    constexpr std::uint32_t kRingMask = kRingCapacity - 1U;
    constexpr std::uint32_t kRingOrigin = 1024;

    // 15000 Hz of deviation is 0.2 of the 75 kHz full scale, which leaves
    // room for the 6 kHz tone's own pre-emphasis gain of 3.9 plus the pilot
    // and the data without the station over deviating.
    constexpr dsp::Hertz kAudioDeviationHz = 15'000;
    const double expected_level =
        static_cast<double>(kAudioDeviationHz) / static_cast<double>(siggen::kCompositePeakDeviationHz);

    struct Case {
        dsp::Hertz tone_hz;
        engine::Deemphasis curve;
        siggen::Preemphasis transmitted;
        double budget_db;
    };

    const Case cases[] = {
        // At a kilohertz the discrete pole and the continuous curve agree to
        // six thousandths of a decibel, so this one is held tight.
        {1'000, engine::Deemphasis::Us75, siggen::Preemphasis::Us75, 0.1},
        {1'000, engine::Deemphasis::Eu50, siggen::Preemphasis::Eu50, 0.1},
        // At six the impulse-invariant pole is 0.22 dB above the curve it is
        // approximating. Held to 0.4 and reported, rather than widened to
        // whatever passes.
        {6'000, engine::Deemphasis::Us75, siggen::Preemphasis::Us75, 0.4},
    };

    for (const auto& item : cases) {
        siggen::WfmSpec spec;
        spec.rate = kStationRate;
        spec.programme.stereo = false;
        spec.programme.left_tone_hz = item.tone_hz;
        spec.programme.right_tone_hz = item.tone_hz;
        spec.programme.audio_deviation_hz = kAudioDeviationHz;
        spec.programme.preemphasis = item.transmitted;
        spec.rds.bits = siggen::random_bits(256, kSeed);
        spec.rds.rds_deviation_hz = 2'000;

        auto station = siggen::generate_wfm(spec, kStationSamples);
        INFO(test::message_of(station));
        REQUIRE(station.has_value());
        CHECK_FALSE(station->over_deviated);

        const auto plan =
            make_plan(engine::Demod::Wfm, 160'000, 200'000, kWideGrid, item.curve, 0, false);
        REQUIRE(plan.demod_rate == kStationRate);
        REQUIRE(plan.deemphasis == item.curve);
        REQUIRE(plan.demod.channels == 1U);

        // Straight into the fine ring: the station is already at the
        // demodulation rate and on the receiver's own centre, so nothing
        // here runs the channelizer or the fine stage. What is under test is
        // the detector, its audio filter and the curve folded into it.
        std::vector<dsp::Complex32> ring(kRingCapacity, dsp::Complex32{});
        std::copy(station->samples.begin(), station->samples.end(),
                  ring.begin() + kRingOrigin);

        const std::uint32_t history = plan.demod.audio_taps + 1U;
        const std::uint32_t first = kRingOrigin + history;
        const auto count = static_cast<std::uint32_t>(
            (kStationSamples - history - 8U) / plan.demod.decimation);
        REQUIRE(count > 1024);

        dsp::VrxDemodParams params;
        params.in_mask = kRingMask;
        params.in_offset = first;
        params.count = count;
        params.gain = plan.demod_gain;

        const auto audio = run_demod_on_gpu(plan.demod, params, ring, plan.demod_weights, 64);
        const auto fit = test::measure_audio_tone(audio, static_cast<double>(plan.output_rate),
                                                  static_cast<double>(item.tone_hz));

        const double transmitted_gain =
            siggen::preemphasis_gain(item.transmitted, item.tone_hz);
        const double uncorrected = expected_level * transmitted_gain;
        const double error_db = to_db(fit.amplitude / expected_level);

        INFO(engine::deemphasis_name(item.curve)
             << " against a " << siggen::preemphasis_name(item.transmitted)
             << " transmitter at " << item.tone_hz << " Hz: recovered " << fit.amplitude
             << ", the programme level before pre-emphasis is " << expected_level
             << ", after it is " << uncorrected << ". Error against the un-emphasised level "
             << error_db << " dB; a receiver with no curve would read "
             << to_db(uncorrected / expected_level) << " dB high. Purity " << fit.purity);

        CHECK(std::abs(error_db) < item.budget_db);

        // And the thing that fails without the fix, stated as its own
        // assertion rather than left implied: the curve accounts for nearly
        // all of the boost the transmitter applied, so a receiver that had
        // none would land at the other end of this gap.
        //
        // Scaled by the boost rather than fixed, because the boost is what
        // the case is measuring and it runs from 0.4 dB at 50 us and a
        // kilohertz to 9.5 dB at 75 us and six. A fixed floor would either
        // be unreachable for the first or free for the last.
        const double boost_db = std::abs(to_db(transmitted_gain));
        CHECK(std::abs(to_db(fit.amplitude / uncorrected)) > 0.8 * boost_db);

        // The 19 kHz pilot and the 57 kHz data are both in this composite
        // and neither belongs in the audio. The old 0.45-of-the-audio-rate
        // filter passed the pilot at 19.2 kHz; this is what notices.
        CHECK(fit.purity > 0.98);
    }
}

// ---------------------------------------------------------------------------
// FM stereo
// ---------------------------------------------------------------------------
//
// Every one of these renders a real station through
// core/dsp/synth/wfm_mod.cpp and reads the two channels back out. Nothing
// here asserts that the decoder is "good": separation is stated in decibels
// and the mono fallback is asserted as bit equality, both of which a reader
// can argue with.

namespace {

constexpr dsp::SampleRate kStereoStationRate = 336'000;
constexpr std::size_t kStereoStationSamples = 40'000;
constexpr std::uint32_t kStereoRingCapacity = 1U << 16;
constexpr std::uint32_t kStereoRingMask = kStereoRingCapacity - 1U;
constexpr std::uint32_t kStereoRingOrigin = 1024;

struct StereoAudio {
    std::vector<float> left;
    std::vector<float> right;
    std::size_t identical_frames = 0;
};

// Renders the station, runs it through the demodulator on the GPU and splits
// the interleaved result.
//
// The station goes straight into the fine ring: it is already at the
// receiver's demodulation rate and on its centre, so nothing here runs the
// channelizer. What is under test is the detector, the pilot recovery and
// the matrix.
[[nodiscard]] StereoAudio receive_station(const siggen::WfmSpec& spec,
                                          const dsp::VrxPlan& plan) {
    auto station = siggen::generate_wfm(spec, kStereoStationSamples);
    INFO(test::message_of(station));
    REQUIRE(station.has_value());
    REQUIRE_FALSE(station->over_deviated);
    REQUIRE(plan.demod_rate == spec.rate);
    REQUIRE(plan.demod.channels == 2U);

    std::vector<dsp::Complex32> ring(kStereoRingCapacity, dsp::Complex32{});
    std::copy(station->samples.begin(), station->samples.end(),
              ring.begin() + kStereoRingOrigin);

    const std::uint32_t history = dsp::demod_fine_history(plan.demod) + 1U;
    const auto count = static_cast<std::uint32_t>(
        (kStereoStationSamples - history - 8U) / plan.demod.decimation);
    REQUIRE(count > 1024);

    dsp::VrxDemodParams params;
    params.in_mask = kStereoRingMask;
    params.in_offset = kStereoRingOrigin + history;
    params.count = count;
    params.gain = plan.demod_gain;

    const auto audio = run_demod_on_gpu(plan.demod, params, ring, plan.demod_weights, 64);
    REQUIRE(audio.size() == 2U * static_cast<std::size_t>(count));

    StereoAudio out;
    out.left.resize(count);
    out.right.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        out.left[i] = audio[2U * i];
        out.right[i] = audio[2U * i + 1U];
        if (out.left[i] == out.right[i]) {
            ++out.identical_frames;
        }
    }
    return out;
}

[[nodiscard]] siggen::WfmSpec stereo_station(dsp::Hertz left_hz, dsp::Hertz right_hz,
                                             std::uint64_t seed) {
    siggen::WfmSpec spec;
    spec.rate = kStereoStationRate;
    spec.programme.stereo = true;
    spec.programme.left_tone_hz = left_hz;
    spec.programme.right_tone_hz = right_hz;
    // 0.4 of full scale before pre-emphasis. The highest tone below takes a
    // 75 us gain of 1.73, so the audio peaks at 0.69 and the pilot and the
    // data fit above it without the station over deviating.
    spec.programme.audio_deviation_hz = 30'000;
    spec.programme.preemphasis = siggen::Preemphasis::Us75;
    spec.rds.bits = siggen::random_bits(256, seed);
    spec.rds.rds_deviation_hz = 2'000;
    return spec;
}

}  // namespace

TEST_CASE("a stereo station separates into two channels, in decibels",
          "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // Two different tones, one per channel, which is the only arrangement
    // that can tell a working matrix from a receiver that wrote the sum
    // channel into both outputs. Equal tones cannot: (L-R)/2 is zero, the
    // difference channel carries nothing, and a decoder that did nothing at
    // all would look right.
    constexpr std::uint64_t kSeed = 0x565258000000000DULL;
    constexpr dsp::Hertz kLeftHz = 1'000;
    constexpr dsp::Hertz kRightHz = 3'000;

    // What the operator gets back is the programme level before
    // pre-emphasis, because the receiver's curve undoes the transmitter's.
    const double expected = 30'000.0 / static_cast<double>(siggen::kCompositePeakDeviationHz);

    const auto spec = stereo_station(kLeftHz, kRightHz, kSeed);
    const auto plan = make_plan(engine::Demod::Wfm, 160'000, 200'000, kWideGrid);
    REQUIRE(plan.stereo);
    REQUIRE(plan.deemphasis == engine::Deemphasis::Us75);

    const auto audio = receive_station(spec, plan);
    const auto rate = static_cast<double>(plan.output_rate);

    const auto left_wanted = test::measure_audio_tone(audio.left, rate, kLeftHz);
    const auto left_leaked = test::measure_audio_tone(audio.left, rate, kRightHz);
    const auto right_wanted = test::measure_audio_tone(audio.right, rate, kRightHz);
    const auto right_leaked = test::measure_audio_tone(audio.right, rate, kLeftHz);

    const double left_separation_db = to_db(left_wanted.amplitude / left_leaked.amplitude);
    const double right_separation_db = to_db(right_wanted.amplitude / right_leaked.amplitude);

    INFO("left channel: " << kLeftHz << " Hz at " << left_wanted.amplitude << ", " << kRightHz
                          << " Hz leaked at " << left_leaked.amplitude << ", separation "
                          << left_separation_db << " dB");
    INFO("right channel: " << kRightHz << " Hz at " << right_wanted.amplitude << ", "
                           << kLeftHz << " Hz leaked at " << right_leaked.amplitude
                           << ", separation " << right_separation_db << " dB");
    INFO("the programme level before pre-emphasis is " << expected << " in both channels");

    // Each channel carries its own tone at the level it was transmitted at.
    // A receiver that handed out the sum channel twice would read half of
    // this in each, which is what mono sounded like.
    CHECK(left_wanted.amplitude == Approx(expected).epsilon(0.05));
    CHECK(right_wanted.amplitude == Approx(expected).epsilon(0.05));

    // MEASURED 40.5 dB in both channels on an RTX 4090 at 1 and 3 kHz.
    // 30 is the bar, and the measurement is printed above, so a change that
    // halves the separation moves a number a reader can see rather than
    // passing at the same threshold. The ceiling is not the matrix: it is
    // the pilot filter's 70 dB rejection of the sum channel and the
    // transmitter's clock offset acting over half the audio filter's
    // window, both stated at core/dsp/vrx_reference.h's stereo constants.
    CHECK(left_separation_db > 30.0);
    CHECK(right_separation_db > 30.0);

    // A stereo programme never produces two identical channels for long. A
    // receiver whose pilot gate was stuck shut would produce nothing else,
    // and would still pass an amplitude check on a mono programme.
    INFO(audio.identical_frames << " of " << audio.left.size()
                                << " frames came back with L equal to R");
    CHECK(audio.identical_frames * 20 < audio.left.size());
}

TEST_CASE("a station with no pilot comes back as two identical channels",
          "[gpu][vrx][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The fallback, and how an operator is told about it.
    //
    // A kernel cannot report anything, so the difference channel is
    // multiplied by zero rather than faded, and L and R come back BIT
    // IDENTICAL. That is a fact a consumer tests for exactly. Fading it
    // instead would leave a consumer thresholding a level to guess at
    // something the receiver already knows, which is the shape of defect
    // this whole round is about.
    constexpr std::uint64_t kSeed = 0x565258000000000EULL;

    auto spec = stereo_station(1'000, 1'000, kSeed);
    spec.programme.stereo = false;
    spec.rds.pilot_enabled = false;

    const auto plan = make_plan(engine::Demod::Wfm, 160'000, 200'000, kWideGrid);
    REQUIRE(plan.stereo);

    const auto audio = receive_station(spec, plan);

    INFO(audio.identical_frames << " of " << audio.left.size()
                                << " frames came back with L equal to R");
    CHECK(audio.identical_frames == audio.left.size());

    // And the sum channel is still there at the right level, so the
    // fallback is mono and not silence.
    const auto fit = test::measure_audio_tone(audio.left, static_cast<double>(plan.output_rate),
                                              1'000.0);
    const double expected = 30'000.0 / static_cast<double>(siggen::kCompositePeakDeviationHz);
    INFO("recovered " << fit.amplitude << " against " << expected << ", purity "
                      << fit.purity);
    CHECK(fit.amplitude == Approx(expected).epsilon(0.05));
}

TEST_CASE("the stereo decoder is off where two channels would destroy the signal",
          "[vrx][m1]") {
    // No GPU. The plan-level rule, which is the same predicate the
    // de-emphasis curve uses and is stated once in engine::resolve_stereo.

    SECTION("a broadcast receiver at an ordinary audio rate decodes stereo") {
        const auto plan = make_plan(engine::Demod::Wfm, 160'000, 200'000, kWideGrid);
        CHECK(plan.stereo);
        CHECK(plan.demod.channels == 2U);
        CHECK(plan.demod.pilot_taps > 16);

        const std::string words = dsp::describe_audio_chain(plan);
        INFO(words);
        CHECK(words.find("stereo from a") != std::string::npos);

        // The pilot filter reaches below the middle of the audio window and
        // further down than the window's own edge, and the block the engine
        // dispatches has to say so: core/engine/vrx_stage.cpp compares
        // oldest_fine against its ring capacity, and a figure that left the
        // pilot out would let a dispatch read slots a later fine dispatch
        // had already written. Silent, and it would sound like a click.
        constexpr std::uint32_t kFirstAudio = 4096;
        auto block = dsp::demod_block(plan, kFineMask, kFirstAudio, 64);
        INFO(test::message_of(block));
        REQUIRE(block.has_value());

        const auto pilot_reach = static_cast<dsp::SampleIndex>(
            plan.demod.audio_taps / 2U + plan.demod.pilot_taps - 1U + 1U);
        const auto audio_reach = static_cast<dsp::SampleIndex>(plan.demod.audio_taps - 1U + 1U);
        INFO("the pilot reaches " << pilot_reach << " below the first output and the audio "
                                  << "filter " << audio_reach);
        REQUIRE(pilot_reach > audio_reach);
        CHECK(block->first_fine - block->oldest_fine >= pilot_reach);
    }

    SECTION("the composite tap stays mono however it is asked") {
        const auto plan = make_plan(engine::Demod::Wfm, 160'000, 200'000, kWideGrid,
                                    engine::Deemphasis::Default, 171'000, true);
        CHECK_FALSE(plan.stereo);
        CHECK(plan.demod.channels == 1U);
        CHECK(plan.demod.pilot_taps == 0);
    }

    SECTION("no other mode carries a pilot-tone multiplex") {
        for (const engine::Demod mode :
             {engine::Demod::Am, engine::Demod::Nfm, engine::Demod::Usb, engine::Demod::Lsb,
              engine::Demod::Dsb, engine::Demod::Cw}) {
            INFO("mode " << engine::demod_name(mode));
            CHECK_FALSE(engine::resolve_stereo(mode, true, 48'000));
        }

        // The raw tap is two channels for a different reason and has always
        // been: it writes I then Q, not L then R.
        const auto raw = make_plan(engine::Demod::Raw, 196'500, 12'000, kGrid);
        CHECK_FALSE(raw.stereo);
        CHECK(raw.demod.channels == 2U);
        CHECK(raw.demod.pilot_taps == 0);
    }

    SECTION("a config that claims stereo without a pilot filter is refused") {
        // The pair that must not drift: two channels on a detector mode
        // with no pilot bandpass would hand the sum channel out twice and
        // report stereo the receiver never decoded.
        dsp::VrxDemodConfig config;
        config.mode = dsp::kDemodWfm;
        config.channels = 2;
        config.pilot_taps = 0;
        dsp::VrxDemodParams params;
        params.in_mask = kFineMask;
        params.count = 16;
        const auto refused = dsp::validate(config, params);
        INFO(test::message_of(refused));
        CHECK_FALSE(refused.has_value());

        // And the converse: a pilot filter on a mode that has no pilot.
        dsp::VrxDemodConfig wrong_mode;
        wrong_mode.mode = dsp::kDemodNfm;
        wrong_mode.channels = 2;
        wrong_mode.pilot_taps = 129;
        const auto also_refused = dsp::validate(wrong_mode, params);
        INFO(test::message_of(also_refused));
        CHECK_FALSE(also_refused.has_value());
    }
}
