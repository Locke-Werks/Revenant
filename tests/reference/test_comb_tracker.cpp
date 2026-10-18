// CombTracker, closed loop, CPU only. A drifting comb is synthesised in double
// precision straight into the projections the project kernel would return for
// each planned block: per line and sub-block, the true tone against the planned
// tone summed in closed form (a geometric series, the line's frequency held at
// its value at the sub-block's centre), plus complex Gaussian noise of the
// variance a sub-block of unit-power white noise would give. Neighbouring
// lines' leakage into a projection is left out: at 14 kHz spacing and 4096
// sample sub-blocks a neighbour sits 24 cycles off and leaks under -37 dB.
// Observations go back to the tracker frames_in_flight blocks late, as the
// graph returns them.
//
// The projections do not depend on the kernels' reference twins, so this file
// tests the tracker alone; the twins' conformance is test_comb_kernels.cpp.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <numbers>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "core/dsp/comb_cancel.h"

using namespace revenant;
using dsp::CombBlockPlan;
using dsp::CombEstimate;
using dsp::CombLock;
using dsp::CombObservation;
using dsp::CombTracker;
using Cd = std::complex<double>;

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr dsp::SampleRate kRate = 2'400'000;
constexpr std::uint32_t kBlock = 65536;
constexpr double kTwo32 = 4294967296.0;

double wrap(double x)
{
    return x - std::floor(x);
}

// The true comb. offset(t) and spacing(t) in hertz at t seconds; per-line
// extra frequency for the non-comb case.
struct Truth {
    std::function<double(double)> offset;
    std::function<double(double)> spacing;
    std::function<double(std::int32_t, double)> extra;  // may be empty
    std::map<std::int32_t, Cd> amplitude;               // per-sample complex amplitude
    std::map<std::int32_t, double> extra_phase;         // integrated extra, cycles
    double phase_c = 0.0;                               // integrated offset, cycles mod 1
    double phase_d = 0.0;                               // integrated spacing, cycles mod 1
};

struct Sim {
    Truth truth;
    std::mt19937_64 rng;
    std::uint32_t frames_in_flight = 3;
    double noise_power = 1.0;  // per sample

    // Running scores, over cancelled strong lines of subtracting blocks.
    double true_before = 0.0;
    double true_after = 0.0;
    std::uint32_t scored_blocks = 0;
    double worst_line_hz = 0.0;  // worst |model - true| frequency over strong lines

    std::deque<std::pair<CombBlockPlan, std::vector<dsp::Complex32>>> in_flight;
    std::uint64_t block = 0;
    bool score = false;
};

double plan_freq_hz(std::uint32_t step)
{
    return static_cast<double>(static_cast<std::int32_t>(step)) / kTwo32 *
           static_cast<double>(kRate);
}

// Which true line a planned line is: the nearest k on the true grid.
std::int32_t line_k(const Truth& truth, double t, std::uint32_t step)
{
    const double f = plan_freq_hz(step);
    return static_cast<std::int32_t>(std::lround((f - truth.offset(t)) / truth.spacing(t)));
}

// Projections of one block against `plan`, advancing the truth by the block.
std::vector<dsp::Complex32> synthesise(Sim& sim, const CombBlockPlan& plan)
{
    Truth& truth = sim.truth;
    const std::uint32_t S = plan.sub_blocks;
    const std::uint32_t L = plan.count / S;
    const double rate = static_cast<double>(kRate);
    const double t_mid = (static_cast<double>(plan.start) + plan.count / 2.0) / rate;
    std::normal_distribution<double> gauss(0.0, std::sqrt(sim.noise_power * L / 2.0));

    std::vector<std::int32_t> ks(plan.lines.size());
    for (std::size_t i = 0; i < plan.lines.size(); ++i) {
        ks[i] = line_k(truth, t_mid, plan.lines[i].step);
    }

    std::vector<dsp::Complex32> out(plan.lines.size() * S);
    std::vector<Cd> clean_sum(plan.lines.size());
    for (std::uint32_t s = 0; s < S; ++s) {
        const double n0 = static_cast<double>(s) * L;
        const double t = (static_cast<double>(plan.start) + n0 + L / 2.0) / rate;
        const double c = truth.offset(t);
        const double d = truth.spacing(t);
        for (std::size_t i = 0; i < plan.lines.size(); ++i) {
            const std::int32_t k = ks[i];
            const auto& pl = plan.lines[i];
            Cd value{};
            auto amp = truth.amplitude.find(k);
            if (amp != truth.amplitude.end()) {
                const double extra = truth.extra ? truth.extra(k, t) : 0.0;
                const double f = c + k * d + extra;
                const double theta =
                    wrap(truth.phase_c + k * truth.phase_d + truth.extra_phase[k]);
                const auto plan_phase0 = static_cast<std::uint32_t>(
                    pl.phase0 + pl.step * static_cast<std::uint32_t>(s * L));
                const double delta =
                    f / rate - static_cast<double>(static_cast<std::int32_t>(pl.step)) / kTwo32;
                const Cd r = std::polar(1.0, kTwoPi * delta);
                Cd sum;
                if (std::abs(delta) < 1e-12) {
                    sum = Cd{static_cast<double>(L), 0.0};
                } else {
                    sum = (1.0 - std::pow(r, static_cast<double>(L))) / (1.0 - r);
                }
                value = amp->second *
                        std::polar(1.0, kTwoPi * (theta - plan_phase0 / kTwo32)) * sum;
            }
            clean_sum[i] += value;
            const Cd noisy = value + Cd{gauss(sim.rng), gauss(sim.rng)};
            out[i * S + s] = dsp::Complex32{static_cast<float>(noisy.real()),
                                            static_cast<float>(noisy.imag())};
        }
        // Advance the truth by the sub-block at its centre frequency.
        truth.phase_c = wrap(truth.phase_c + c * L / rate);
        truth.phase_d = wrap(truth.phase_d + d * L / rate);
        if (truth.extra) {
            for (auto& [k, a] : truth.amplitude) {
                truth.extra_phase[k] = wrap(truth.extra_phase[k] + truth.extra(k, t) * L / rate);
            }
        }
    }

    // True suppression: noise-free line power before, line minus removed after.
    if (sim.score && plan.subtract) {
        bool any = false;
        for (std::size_t i = 0; i < plan.lines.size(); ++i) {
            const Cd amp{plan.lines[i].amplitude.real(), plan.lines[i].amplitude.imag()};
            if (amp == Cd{}) {
                continue;
            }
            const Cd line = clean_sum[i] / static_cast<double>(plan.count);
            sim.true_before += std::norm(line);
            sim.true_after += std::norm(line - amp);
            const double f_true = truth.offset(t_mid) + ks[i] * truth.spacing(t_mid) +
                                  (truth.extra ? truth.extra(ks[i], t_mid) : 0.0);
            sim.worst_line_hz =
                std::max(sim.worst_line_hz, std::abs(plan_freq_hz(plan.lines[i].step) - f_true));
            any = true;
        }
        if (any) {
            ++sim.scored_blocks;
        }
    }
    return out;
}

// Plan one block, synthesise it, and hand back whatever is frames_in_flight
// old.
void step(Sim& sim, CombTracker& tracker)
{
    const dsp::SampleIndex start = sim.block * kBlock;
    ++sim.block;
    CombBlockPlan plan = tracker.plan(start, kBlock);
    std::vector<dsp::Complex32> proj;
    if (!plan.lines.empty()) {
        proj = synthesise(sim, plan);
    } else {
        // Advance the truth anyway, with a dummy sub-block pass.
        const double rate = static_cast<double>(kRate);
        const double t = (static_cast<double>(start) + kBlock / 2.0) / rate;
        sim.truth.phase_c = wrap(sim.truth.phase_c + sim.truth.offset(t) * kBlock / rate);
        sim.truth.phase_d = wrap(sim.truth.phase_d + sim.truth.spacing(t) * kBlock / rate);
    }
    sim.in_flight.emplace_back(std::move(plan), std::move(proj));
    while (sim.in_flight.size() > sim.frames_in_flight) {
        auto& [old_plan, old_proj] = sim.in_flight.front();
        if (!old_plan.lines.empty()) {
            CombObservation obs;
            obs.start = old_plan.start;
            obs.count = old_plan.count;
            obs.sub_blocks = old_plan.sub_blocks;
            obs.projections = old_proj;
            tracker.observe(obs);
        }
        sim.in_flight.pop_front();
    }
}

// Lines k_lo..k_hi with block SNRs spread over [snr_lo_db, snr_hi_db] and
// every `gap_every`-th line absent.
void populate(Sim& sim, std::int32_t k_lo, std::int32_t k_hi, double snr_lo_db,
              double snr_hi_db, std::int32_t gap_every)
{
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    for (std::int32_t k = k_lo; k <= k_hi; ++k) {
        if (gap_every > 0 && (k - k_lo) % gap_every == gap_every - 1) {
            continue;
        }
        const double snr_db = snr_lo_db + (snr_hi_db - snr_lo_db) * uni(sim.rng);
        const double power = std::pow(10.0, snr_db / 10.0) * sim.noise_power / kBlock;
        sim.truth.amplitude[k] = std::polar(std::sqrt(power), kTwoPi * uni(sim.rng));
    }
}

CombEstimate estimate_of(const Truth& truth, std::int32_t k_lo, std::int32_t k_hi,
                         double offset_error_hz, double spacing_error_hz)
{
    CombEstimate e;
    e.offset_hz = truth.offset(0.0) + offset_error_hz;
    e.spacing_hz = truth.spacing(0.0) + spacing_error_hz;
    e.k_lo = k_lo;
    e.k_hi = k_hi;
    e.lines_found = static_cast<std::uint32_t>(k_hi - k_lo + 1);
    e.median_height_db = 10.0F;
    return e;
}

// Runs until locked or `limit` blocks; returns the block it locked at, or -1.
int run_to_lock(Sim& sim, CombTracker& tracker, int limit)
{
    for (int b = 0; b < limit; ++b) {
        step(sim, tracker);
        if (tracker.status().lock == CombLock::kLocked) {
            return b + 1;
        }
    }
    return -1;
}

double true_suppression_db(const Sim& sim)
{
    if (sim.true_after <= 0.0) {
        return 99.0;
    }
    return 10.0 * std::log10(sim.true_before / sim.true_after);
}

}  // namespace

TEST_CASE("comb tracker: acquires from a detection a bin off and tracks a wandering spacing",
          "[comb][tracker]")
{
    for (std::uint32_t fif : {2U, 3U, 4U}) {
        const std::uint64_t seed = 0xC0FFEE00ULL + fif;
        std::printf("comb tracker acquisition: seed %llu, frames in flight %u\n",
                    static_cast<unsigned long long>(seed), fif);
        INFO("seed " << seed << ", frames in flight " << fif);
        Sim sim;
        sim.rng.seed(seed);
        sim.frames_in_flight = fif;

        // Spacing: a 14.1234 kHz supply with a 0.3 Hz, 15 s load wander and a
        // random walk of 0.01 Hz per root second; offset an internal clock
        // product with its own slow drift.
        auto walk = std::make_shared<std::vector<double>>();
        std::normal_distribution<double> g(0.0, 0.01 * std::sqrt(0.1));
        double acc = 0.0;
        for (int i = 0; i < 1000; ++i) {
            walk->push_back(acc);
            acc += g(sim.rng);
        }
        auto rw = [walk](double t) {
            const double x = std::clamp(t / 0.1, 0.0, 998.0);
            const auto i = static_cast<std::size_t>(x);
            return (*walk)[i] + ((*walk)[i + 1] - (*walk)[i]) * (x - static_cast<double>(i));
        };
        sim.truth.spacing = [rw](double t) {
            return 14123.4 + 0.3 * std::sin(kTwoPi * t / 15.0) + rw(t);
        };
        sim.truth.offset = [](double t) { return -301234.5 + 2.0 * std::sin(kTwoPi * t / 23.0); };
        populate(sim, -40, 40, 12.0, 25.0, 7);

        CombTracker tracker(kRate);
        tracker.detected(estimate_of(sim.truth, -40, 40, 90.0, 0.4));
        CHECK(tracker.status().lock == CombLock::kAcquiring);

        const int locked_at = run_to_lock(sim, tracker, 80);
        INFO("locked at block " << locked_at);
        REQUIRE(locked_at > 0);
        CHECK(locked_at <= 60);

        // Let the amplitudes settle, then score 600 blocks, 16 s.
        for (int b = 0; b < 60; ++b) {
            step(sim, tracker);
        }
        sim.score = true;
        for (int b = 0; b < 600; ++b) {
            step(sim, tracker);
            REQUIRE(tracker.status().lock == CombLock::kLocked);
        }
        const auto st = tracker.status();
        const double sup = true_suppression_db(sim);
        std::printf("  locked at %d, true suppression %.1f dB, reported %.1f dB, worst line "
                    "%.3f Hz, drift %.4f Hz/s\n",
                    locked_at, sup, st.suppression_db, sim.worst_line_hz,
                    st.spacing_drift_hz_per_s);
        CHECK(sup > 20.0);
        // The report is a lower bound, limited by each line's SNR in a block.
        CHECK(st.suppression_db > 10.0F);
        CHECK(st.suppression_db < sup + 3.0);
        CHECK(sim.worst_line_hz < 1.0);
        CHECK(std::abs(st.comb.spacing_hz - sim.truth.spacing(sim.block * kBlock / 2.4e6)) <
              0.05);

        // Amplitudes: magnitudes within 10 % of the truth on the lines present,
        // nothing subtracted at the gaps.
        const CombBlockPlan plan = tracker.plan(sim.block * kBlock, kBlock);
        const double t = (static_cast<double>(sim.block) * kBlock + kBlock / 2.0) / 2.4e6;
        std::uint32_t checked = 0;
        for (const auto& line : plan.lines) {
            const std::int32_t k = line_k(sim.truth, t, line.step);
            const double got = std::abs(Cd{line.amplitude.real(), line.amplitude.imag()});
            auto it = sim.truth.amplitude.find(k);
            if (it == sim.truth.amplitude.end()) {
                CHECK(got == 0.0);
            } else {
                CHECK(std::abs(got - std::abs(it->second)) < 0.1 * std::abs(it->second));
                ++checked;
            }
        }
        CHECK(checked == sim.truth.amplitude.size());
    }
}

TEST_CASE("comb tracker: harmonic comb, offset tied to 500 times a wandering fundamental",
          "[comb][tracker]")
{
    struct Case {
        double wander_hz;
        double period_s;
        double min_suppression_db;
    };
    // 0.02 Hz moves the offset 10 Hz; 0.2 Hz moves it 100 Hz at up to 31 Hz/s.
    for (const Case cs : {Case{0.02, 20.0, 20.0}, Case{0.2, 20.0, 15.0}}) {
        const std::uint64_t seed = 0x4A2B0000ULL + static_cast<std::uint64_t>(cs.wander_hz * 1000);
        std::printf("comb tracker harmonic: seed %llu, wander %.2f Hz over %.0f s\n",
                    static_cast<unsigned long long>(seed), cs.wander_hz, cs.period_s);
        INFO("seed " << seed << ", wander " << cs.wander_hz);
        Sim sim;
        sim.rng.seed(seed);
        sim.frames_in_flight = 4;

        constexpr double kF0 = 14000.3;
        constexpr double kN0 = 500.0;
        const double centre = kN0 * kF0 + 12345.0;
        auto fundamental = [cs](double t) {
            return kF0 + cs.wander_hz * std::sin(kTwoPi * t / cs.period_s) +
                   0.002 * std::sin(kTwoPi * t / 3.7);
        };
        sim.truth.spacing = fundamental;
        sim.truth.offset = [fundamental, centre](double t) { return kN0 * fundamental(t) - centre; };
        populate(sim, -60, 60, 15.0, 25.0, 0);

        CombTracker tracker(kRate);
        tracker.detected(estimate_of(sim.truth, -60, 60, -70.0, 0.2));
        const int locked_at = run_to_lock(sim, tracker, 100);
        INFO("locked at block " << locked_at);
        REQUIRE(locked_at > 0);
        for (int b = 0; b < 60; ++b) {
            step(sim, tracker);
        }
        sim.score = true;
        // 30 s: one and a half wander periods.
        std::uint32_t unlocked = 0;
        for (int b = 0; b < 1100; ++b) {
            step(sim, tracker);
            if (tracker.status().lock != CombLock::kLocked) {
                ++unlocked;
            }
        }
        const double sup = true_suppression_db(sim);
        const auto st = tracker.status();
        std::printf("  locked at %d, true suppression %.1f dB, reported %.1f dB, worst line "
                    "%.3f Hz, unlocked blocks %u\n",
                    locked_at, sup, st.suppression_db, sim.worst_line_hz, unlocked);
        CHECK(unlocked == 0);
        CHECK(sup > cs.min_suppression_db);
    }
}

TEST_CASE("comb tracker: lines on a grid that do not move together are not a comb",
          "[comb][tracker]")
{
    const std::uint64_t seed = 0x0DD11E5ULL;
    std::printf("comb tracker non-comb: seed %llu\n", static_cast<unsigned long long>(seed));
    INFO("seed " << seed);
    Sim sim;
    sim.rng.seed(seed);
    sim.truth.spacing = [](double) { return 12500.0; };
    sim.truth.offset = [](double) { return 41000.0; };
    populate(sim, -30, 30, 15.0, 25.0, 0);
    // Each carrier wanders on its own: 3 Hz over a few seconds, random phase.
    auto params = std::make_shared<std::map<std::int32_t, std::pair<double, double>>>();
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    for (std::int32_t k = -30; k <= 30; ++k) {
        (*params)[k] = {2.0 + 4.0 * uni(sim.rng), kTwoPi * uni(sim.rng)};
    }
    sim.truth.extra = [params](std::int32_t k, double t) {
        const auto& [period, phase] = params->at(k);
        return 3.0 * std::sin(kTwoPi * t / period + phase);
    };

    CombTracker tracker(kRate);
    tracker.detected(estimate_of(sim.truth, -30, 30, 0.0, 0.0));
    bool ever_locked_long = false;
    std::uint32_t locked_run = 0;
    for (int b = 0; b < 400; ++b) {
        step(sim, tracker);
        if (tracker.status().lock == CombLock::kLocked) {
            ++locked_run;
            ever_locked_long = ever_locked_long || locked_run > 40;
        } else {
            locked_run = 0;
        }
    }
    CHECK_FALSE(ever_locked_long);
    CHECK(tracker.status().lock == CombLock::kLost);
    CHECK(tracker.plan(sim.block * kBlock, kBlock).lines.empty());
}

TEST_CASE("comb tracker: reset, re-detection and trimming", "[comb][tracker]")
{
    const std::uint64_t seed = 0x5E7ULL;
    std::printf("comb tracker reset: seed %llu\n", static_cast<unsigned long long>(seed));
    INFO("seed " << seed);
    Sim sim;
    sim.rng.seed(seed);
    sim.truth.spacing = [](double) { return 9876.5; };
    sim.truth.offset = [](double) { return 2345.0; };
    populate(sim, -20, 20, 15.0, 25.0, 0);

    CombTracker tracker(kRate);
    CHECK(tracker.status().lock == CombLock::kIdle);
    CHECK(tracker.plan(0, kBlock).lines.empty());
    tracker.detected(estimate_of(sim.truth, -20, 20, 40.0, 0.0));
    REQUIRE(run_to_lock(sim, tracker, 80) > 0);

    // An agreeing detection, numbered from a different origin, is ignored.
    CombEstimate shifted = estimate_of(sim.truth, -25, 15, 5.0, 0.01);
    shifted.offset_hz += 5.0 * sim.truth.spacing(0.0);
    tracker.detected(shifted);
    CHECK(tracker.status().lock == CombLock::kLocked);

    // A disagreeing one restarts acquisition.
    tracker.detected(estimate_of(sim.truth, -20, 20, 0.0, 300.0));
    CHECK(tracker.status().lock == CombLock::kAcquiring);

    tracker.reset();
    CHECK(tracker.status().lock == CombLock::kIdle);
    sim.in_flight.clear();  // a retune discards what was in flight
    for (int b = 0; b < 5; ++b) {
        step(sim, tracker);
    }
    CHECK(tracker.status().lock == CombLock::kIdle);

    tracker.detected(estimate_of(sim.truth, -20, 20, -60.0, 0.0));
    sim.in_flight.clear();
    CHECK(run_to_lock(sim, tracker, 80) > 0);

    // Observations from before a reset are ignored.
    CombTracker stale(kRate);
    stale.detected(estimate_of(sim.truth, -20, 20, 0.0, 0.0));
    const CombBlockPlan old_plan = stale.plan(0, kBlock);
    std::vector<dsp::Complex32> zeros(old_plan.lines.size() * old_plan.sub_blocks);
    stale.reset();
    stale.observe(CombObservation{0, kBlock, old_plan.sub_blocks, zeros});
    CHECK(stale.status().lock == CombLock::kIdle);

    // More lines than the stage can take: the central kCombMaxLines.
    CombTracker wide(kRate);
    CombEstimate many;
    many.offset_hz = -1.0e6;
    many.spacing_hz = 600.0;
    many.k_lo = 0;
    many.k_hi = 3499;
    many.lines_found = 3500;
    wide.detected(many);
    const CombBlockPlan plan = wide.plan(0, kBlock);
    REQUIRE(plan.lines.size() == dsp::kCombMaxLines);
    const auto st = wide.status();
    constexpr auto kMax = static_cast<std::int32_t>(dsp::kCombMaxLines);
    CHECK(st.comb.k_lo == (3500 - kMax) / 2);
    CHECK(st.comb.k_hi == (3500 - kMax) / 2 + kMax - 1);
    CHECK(plan.sub_blocks == 16);
    CHECK_FALSE(plan.subtract);
}

TEST_CASE("comb tracker: deterministic for a seed", "[comb][tracker]")
{
    const std::uint64_t seed = 0xD1CEULL;
    std::printf("comb tracker determinism: seed %llu\n", static_cast<unsigned long long>(seed));
    INFO("seed " << seed);
    auto run = [seed]() {
        Sim sim;
        sim.rng.seed(seed);
        sim.truth.spacing = [](double t) { return 11111.1 + 0.2 * std::sin(t); };
        sim.truth.offset = [](double t) { return 5000.0 + 3.0 * std::sin(0.3 * t); };
        populate(sim, -25, 25, 12.0, 20.0, 5);
        CombTracker tracker(kRate);
        tracker.detected(estimate_of(sim.truth, -25, 25, 70.0, 0.3));
        std::vector<std::uint32_t> trace;
        for (int b = 0; b < 200; ++b) {
            step(sim, tracker);
            const CombBlockPlan p = tracker.plan(sim.block * kBlock, kBlock);
            // Planning the same block twice must not disturb it.
            for (const auto& l : p.lines) {
                trace.push_back(l.phase0);
                trace.push_back(l.step);
                trace.push_back(std::bit_cast<std::uint32_t>(l.amplitude.real()));
            }
            trace.push_back(static_cast<std::uint32_t>(tracker.status().lock));
        }
        return trace;
    };
    const auto a = run();
    const auto b = run();
    CHECK(a == b);

    // A copy is a fork: both continue identically.
    CombTracker one(kRate);
    one.detected(CombEstimate{1000.0, 5000.0, -10, 10, 21, 10.0F});
    (void)one.plan(0, kBlock);
    CombTracker two = one;
    const auto p1 = one.plan(kBlock, kBlock);
    const auto p2 = two.plan(kBlock, kBlock);
    REQUIRE(p1.lines.size() == p2.lines.size());
    for (std::size_t i = 0; i < p1.lines.size(); ++i) {
        CHECK(p1.lines[i].phase0 == p2.lines[i].phase0);
        CHECK(p1.lines[i].step == p2.lines[i].step);
    }
}

TEST_CASE("comb tracker: planned phases are the model's, exact across blocks", "[comb][tracker]")
{
    // With no observations the model is a fixed comb: line k's planned phase
    // at any sample must equal k-th line's exact phase f n / rate mod 1 to the
    // 32-bit rounding, however many blocks have passed.
    CombTracker tracker(kRate);
    const double c = 1234.5;
    const double d = 7000.25;
    tracker.detected(CombEstimate{c, d, -12, 12, 25, 10.0F});
    const CombBlockPlan first = tracker.plan(0, kBlock);
    const std::uint32_t half = kBlock / 2;
    CombBlockPlan later;
    const std::uint64_t blocks = 5000;  // 136 s
    for (std::uint64_t b = 1; b <= blocks; ++b) {
        later = tracker.plan(b * kBlock, kBlock);
    }
    for (std::size_t i = 0; i < first.lines.size(); ++i) {
        const auto k = static_cast<std::int32_t>(i) - 12;
        // The model's frequency is fixed-point; read it from the step and the
        // fixed point of c and d, then predict the later middle's phase.
        const auto p0 = static_cast<std::uint32_t>(first.lines[i].phase0 + first.lines[i].step * half);
        const auto p1 = static_cast<std::uint32_t>(later.lines[i].phase0 + later.lines[i].step * half);
        const double f = (c + k * d) / 2.4e6;
        const double elapsed = static_cast<double>(blocks * kBlock);
        double expected = std::fmod(p0 / kTwo32 + f * elapsed, 1.0);
        double err = p1 / kTwo32 - expected;
        err -= std::round(err);
        INFO("line " << k << " phase error " << err << " cycles");
        // The model's frequency is c/rate rounded to 2^-64, so over 3.3e8
        // samples the drift is under 1e-10 cycles; the bound is the 32-bit
        // rounding of the two anchors plus double error in `expected`.
        CHECK(std::abs(err) < 1.0e-6);
    }
}

namespace {

// Two combs in one capture: each sim synthesises its own comb's share of the
// projections against the bank's whole plan, and the shares add. Noise comes
// from `a` alone, so the floor is one floor.
void bank_step(Sim& a, Sim& b, dsp::CombBank& bank,
               std::deque<std::pair<CombBlockPlan, std::vector<dsp::Complex32>>>& in_flight)
{
    const dsp::SampleIndex start = a.block * kBlock;
    ++a.block;
    ++b.block;
    CombBlockPlan plan = bank.plan(start, kBlock);
    std::vector<dsp::Complex32> proj;
    if (!plan.lines.empty()) {
        proj = synthesise(a, plan);
        const std::vector<dsp::Complex32> other = synthesise(b, plan);
        for (std::size_t i = 0; i < proj.size(); ++i) {
            proj[i] += other[i];
        }
    } else {
        const double rate = static_cast<double>(kRate);
        const double t = (static_cast<double>(start) + kBlock / 2.0) / rate;
        for (Sim* sim : {&a, &b}) {
            sim->truth.phase_c = wrap(sim->truth.phase_c + sim->truth.offset(t) * kBlock / rate);
            sim->truth.phase_d = wrap(sim->truth.phase_d + sim->truth.spacing(t) * kBlock / rate);
        }
    }
    in_flight.emplace_back(std::move(plan), std::move(proj));
    while (in_flight.size() > a.frames_in_flight) {
        auto& [old_plan, old_proj] = in_flight.front();
        if (!old_plan.lines.empty()) {
            CombObservation obs;
            obs.start = old_plan.start;
            obs.count = old_plan.count;
            obs.sub_blocks = old_plan.sub_blocks;
            obs.projections = old_proj;
            bank.observe(obs);
        }
        in_flight.pop_front();
    }
}

}  // namespace

// The owner's HF comb, 2026-10-12: two switchers near 31.58 kHz whose lines
// interleave, spacings a few hertz apart and wandering independently. One
// affine model follows one of them; the bank holds each as its own comb and
// cancels both.
TEST_CASE("comb bank: two interleaved switchers are both locked and cancelled", "[comb][tracker]")
{
    const std::uint64_t seed = 0xB4A2C0ULL;
    std::printf("comb bank: seed %llu\n", static_cast<unsigned long long>(seed));
    INFO("seed " << seed);
    Sim a;
    Sim b;
    a.rng.seed(seed);
    b.rng.seed(seed + 1);
    b.noise_power = 0.0;
    a.truth.spacing = [](double t) { return 31574.0 + 0.2 * std::sin(kTwoPi * t / 13.0); };
    a.truth.offset = [](double t) { return -9876.5 + 1.5 * std::sin(kTwoPi * t / 21.0); };
    b.truth.spacing = [](double t) { return 31579.5 + 0.25 * std::sin(kTwoPi * t / 9.0); };
    b.truth.offset = [](double t) { return -9876.5 + 15790.0 + 2.0 * std::sin(kTwoPi * t / 17.0); };
    populate(a, -30, 30, 14.0, 25.0, 0);
    // b's amplitudes are scaled for a's noise floor.
    b.noise_power = 1.0;
    populate(b, -30, 30, 10.0, 18.0, 0);
    b.noise_power = 0.0;

    dsp::CombBank bank(kRate);
    const std::vector<CombEstimate> found{estimate_of(a.truth, -30, 30, 40.0, 0.1),
                                          estimate_of(b.truth, -30, 30, -30.0, -0.1)};
    bank.detected(found);
    REQUIRE(bank.trackers().size() == 2);

    std::deque<std::pair<CombBlockPlan, std::vector<dsp::Complex32>>> in_flight;
    int locked_at = -1;
    for (int blk = 0; blk < 200 && locked_at < 0; ++blk) {
        bank_step(a, b, bank, in_flight);
        if (bank.trackers()[0].status().lock == CombLock::kLocked &&
            bank.trackers()[1].status().lock == CombLock::kLocked) {
            locked_at = blk + 1;
        }
    }
    INFO("both locked at block " << locked_at);
    REQUIRE(locked_at > 0);

    // Detections a second apart while cancelling hand back what is already
    // held, at its own spacing and at the joint one; none may start a third.
    CombEstimate joint = estimate_of(a.truth, -60, 60, 0.0, 0.0);
    joint.spacing_hz = 0.5 * (a.truth.spacing(0.0) + b.truth.spacing(0.0)) / 2.0;
    for (int blk = 0; blk < 400; ++blk) {
        bank_step(a, b, bank, in_flight);
        if (blk % 36 == 0) {
            bank.detected(found);
            bank.detected(std::span<const CombEstimate>(&joint, 1));
        }
    }
    REQUIRE(bank.trackers().size() == 2);
    const double t = static_cast<double>(a.block) * kBlock / 2.4e6;
    const auto sa = bank.trackers()[0].status();
    const auto sb = bank.trackers()[1].status();
    std::printf("  locked at %d; a %u lines %.1f dB spacing err %.3f Hz; b %u lines %.1f dB spacing "
                "err %.3f Hz\n",
                locked_at, sa.lines_cancelled, sa.suppression_db,
                sa.comb.spacing_hz - a.truth.spacing(t), sb.lines_cancelled, sb.suppression_db,
                sb.comb.spacing_hz - b.truth.spacing(t));
    CHECK(sa.lock == CombLock::kLocked);
    CHECK(sb.lock == CombLock::kLocked);
    CHECK(std::abs(sa.comb.spacing_hz - a.truth.spacing(t)) < 0.05);
    CHECK(std::abs(sb.comb.spacing_hz - b.truth.spacing(t)) < 0.05);
    CHECK(sa.lines_cancelled >= 55);
    CHECK(sb.lines_cancelled >= 55);
    CHECK(sa.suppression_db > 10.0F);
    CHECK(sb.suppression_db > 8.0F);
    const auto all = bank.status();
    CHECK(all.lock == CombLock::kLocked);
    CHECK(all.lines_cancelled == sa.lines_cancelled + sb.lines_cancelled);
}

// A comb whose every line carries ripple sidebands one ripple either side, as
// on the owner's 80 m comb (rungs every 59.5 Hz). The carriers and each
// sideband order are simulated as combs of their own, summed, so each
// projection holds the others' leakage the way the kernel's would. The
// tracker starts half a hertz off the ripple and must follow it and cancel
// the sidebands with the carriers.
TEST_CASE("comb tracker: cancels ripple sidebands with their carriers", "[comb][tracker]")
{
    const std::uint64_t seed = 0x51DE5EEDULL;
    std::printf("comb tracker sidebands: seed %llu\n", static_cast<unsigned long long>(seed));
    INFO("seed " << seed);
    auto ripple = [](double t) { return 59.5 + 0.1 * std::sin(kTwoPi * t / 7.0); };
    auto offset = [](double t) { return 2707.0 + 1.0 * std::sin(kTwoPi * t / 19.0); };
    auto spacing = [](double t) { return 31578.9 + 0.05 * std::sin(kTwoPi * t / 11.0); };

    Sim carrier;
    Sim upper;
    Sim lower;
    carrier.rng.seed(seed);
    upper.rng.seed(seed + 1);
    lower.rng.seed(seed + 2);
    carrier.truth.spacing = spacing;
    carrier.truth.offset = offset;
    upper.truth.spacing = spacing;
    upper.truth.offset = [=](double t) { return offset(t) + ripple(t); };
    lower.truth.spacing = spacing;
    lower.truth.offset = [=](double t) { return offset(t) - ripple(t); };
    populate(carrier, -20, 20, 24.0, 30.0, 0);
    populate(upper, -20, 20, 12.0, 18.0, 0);
    populate(lower, -20, 20, 12.0, 18.0, 0);
    upper.noise_power = 0.0;
    lower.noise_power = 0.0;

    CombTracker tracker(kRate);
    CombEstimate estimate = estimate_of(carrier.truth, -20, 20, 10.0, 0.02);
    estimate.ripple_hz = ripple(0.0) + 0.5;
    estimate.sideband_orders = 1;
    tracker.detected(estimate);
    REQUIRE(tracker.lines() == 41 * 3);

    std::vector<Sim*> sims{&carrier, &upper, &lower};
    std::deque<std::pair<CombBlockPlan, std::vector<dsp::Complex32>>> in_flight;
    auto step_all = [&] {
        const dsp::SampleIndex start = carrier.block * kBlock;
        for (Sim* sim : sims) {
            ++sim->block;
        }
        CombBlockPlan plan = tracker.plan(start, kBlock);
        std::vector<dsp::Complex32> proj;
        if (!plan.lines.empty()) {
            for (Sim* sim : sims) {
                const std::vector<dsp::Complex32> part = synthesise(*sim, plan);
                if (proj.empty()) {
                    proj = part;
                } else {
                    for (std::size_t i = 0; i < proj.size(); ++i) {
                        proj[i] += part[i];
                    }
                }
            }
        }
        in_flight.emplace_back(std::move(plan), std::move(proj));
        while (in_flight.size() > carrier.frames_in_flight) {
            auto& [old_plan, old_proj] = in_flight.front();
            if (!old_plan.lines.empty()) {
                tracker.observe(CombObservation{old_plan.start, old_plan.count,
                                                old_plan.sub_blocks, old_proj});
            }
            in_flight.pop_front();
        }
    };

    int locked_at = -1;
    for (int blk = 0; blk < 200 && locked_at < 0; ++blk) {
        step_all();
        if (tracker.status().lock == CombLock::kLocked) {
            locked_at = blk + 1;
        }
    }
    INFO("locked at block " << locked_at);
    REQUIRE(locked_at > 0);
    for (int blk = 0; blk < 300; ++blk) {
        step_all();
    }
    const double t = static_cast<double>(carrier.block) * kBlock / 2.4e6;
    const auto status = tracker.status();
    std::printf("  locked at %d; %u of %u lines cancelled, ripple %.3f Hz against %.3f\n",
                locked_at, status.lines_cancelled, tracker.lines(), tracker.ripple_hz(),
                ripple(t));
    CHECK(status.lock == CombLock::kLocked);
    CHECK(std::abs(tracker.ripple_hz() - ripple(t)) < 0.2);
    // Carriers and sidebands nearly all cancelled.
    CHECK(status.lines_cancelled >= 41 * 3 * 9 / 10);
}

TEST_CASE("comb tracker: folds finer sub-blocks into the ones it planned", "[comb][tracker]")
{
    // The same block observed at the planned sub-blocks and at four times as
    // many, summed in fours, must leave two forks of the tracker identical.
    Sim sim;
    sim.rng.seed(77);
    sim.truth.spacing = [](double) { return 12000.0; };
    sim.truth.offset = [](double) { return 3000.0; };
    populate(sim, -10, 10, 15.0, 20.0, 0);
    CombTracker tracker(kRate);
    tracker.detected(estimate_of(sim.truth, -10, 10, 0.0, 0.0));
    // Locked, the tracker plans its long sub-blocks, four to a block.
    REQUIRE(run_to_lock(sim, tracker, 120) > 0);
    const dsp::SampleIndex start = sim.block * kBlock;
    const CombBlockPlan plan = tracker.plan(start, kBlock);
    REQUIRE(plan.sub_blocks * 4 <= dsp::kCombMaxSubBlocks);

    CombBlockPlan fine = plan;
    fine.sub_blocks = plan.sub_blocks * 4;
    Sim copy = sim;
    const std::vector<dsp::Complex32> fine_proj = synthesise(copy, fine);
    std::vector<dsp::Complex32> coarse(plan.lines.size() * plan.sub_blocks);
    for (std::size_t i = 0; i < coarse.size(); ++i) {
        for (std::size_t g = 0; g < 4; ++g) {
            coarse[i] += fine_proj[i * 4 + g];
        }
    }
    CombTracker x = tracker;
    CombTracker y = tracker;
    x.observe(CombObservation{start, kBlock, plan.sub_blocks, coarse});
    y.observe(CombObservation{start, kBlock, fine.sub_blocks, fine_proj});
    const CombBlockPlan px = x.plan(start + kBlock, kBlock);
    const CombBlockPlan py = y.plan(start + kBlock, kBlock);
    REQUIRE(px.lines.size() == py.lines.size());
    for (std::size_t i = 0; i < px.lines.size(); ++i) {
        CHECK(px.lines[i].step == py.lines[i].step);
        CHECK(px.lines[i].phase0 == py.lines[i].phase0);
    }
    CHECK(x.status().comb.spacing_hz == y.status().comb.spacing_hz);
}

// A spur, CombEstimate::spur: one line, tracked and subtracted through the
// kernels' twins on real samples, with a keyed CW signal 200 Hz above it and
// noise. The spur is to come down by 30 dB; the CW is to come through as it
// went in, which is checked against the CW and noise synthesised without the
// spur, so the measurement does not have to see past the spur's own leakage.
TEST_CASE("comb tracker: a single-line spur tracker removes the tone and spares CW beside it",
          "[comb][tracker][spur]")
{
    const std::uint64_t seed = 0x5B0B5ULL;
    std::printf("spur tracker: seed %llu\n", static_cast<unsigned long long>(seed));
    INFO("seed " << seed);
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> gauss(0.0, std::sqrt(0.5));

    constexpr double kSpurHz = 41'234.5;
    constexpr double kCwHz = kSpurHz + 200.0;
    constexpr double rate = static_cast<double>(kRate);
    // 40 dB over the noise in a block's resolution, and the CW 10 dB under it.
    // The depth is noise limited, about the spur's own SNR in a block less
    // what the amplitude average buys back, so a 30 dB spur comes down by
    // about as much as it stands up.
    const double spur_amp = std::sqrt(10'000.0 / kBlock);
    const double cw_amp = spur_amp / std::sqrt(10.0);

    CombEstimate spur;
    spur.offset_hz = kSpurHz + 4.0;  // a detection a few hertz off
    spur.k_lo = 0;
    spur.k_hi = 0;
    spur.lines_found = 1;
    spur.spur = true;
    CombTracker tracker(kRate, 1);
    tracker.detected(spur);
    REQUIRE(tracker.active());
    REQUIRE(tracker.spur());
    REQUIRE(tracker.lines() == 1);

    const std::vector<dsp::Complex32> table = dsp::comb_table();
    std::vector<dsp::Complex32> raw(kBlock);
    std::vector<dsp::Complex32> without(kBlock);  // CW and noise, no spur
    std::vector<dsp::Complex32> cleaned(kBlock);
    std::vector<dsp::Complex32> removed(kBlock);
    std::vector<dsp::Complex32> proj(dsp::kCombMaxSubBlocks);
    std::uint64_t n_total = 0;
    const double spur_phase0 = 0.3;

    // A tone at f from sample n0, by recursion from its exact phase there: in
    // double the drift over a block is far under anything measured here.
    auto phasor_at = [&](double f, std::uint64_t n0, double phase0) {
        const double cycles = f * static_cast<double>(n0) / rate + phase0;
        return std::polar(1.0, kTwoPi * (cycles - std::floor(cycles)));
    };
    // Projection of a buffer onto a tone at f over the block starting at n0.
    auto project = [&](const std::vector<dsp::Complex32>& x, double f, std::uint64_t n0) {
        Cd z = std::conj(phasor_at(f, n0, 0.0));
        const Cd r = std::polar(1.0, -kTwoPi * f / rate);
        Cd sum{};
        for (std::uint32_t n = 0; n < kBlock; ++n) {
            sum += Cd{x[n].real(), x[n].imag()} * z;
            z *= r;
        }
        return sum / static_cast<double>(kBlock);
    };

    double spur_before = 0.0;
    double spur_after = 0.0;
    double cw_power = 0.0;
    double cw_error = 0.0;
    int locked_at = -1;
    for (int blk = 0; blk < 240; ++blk) {
        const dsp::SampleIndex start = n_total;
        Cd s_z = phasor_at(kSpurHz, start, spur_phase0) * spur_amp;
        Cd c_z = phasor_at(kCwHz, start, 0.0) * cw_amp;
        const Cd s_r = std::polar(1.0, kTwoPi * kSpurHz / rate);
        const Cd c_r = std::polar(1.0, kTwoPi * kCwHz / rate);
        // Key down for 60 ms in every 120: 20 WPM dits.
        for (std::uint32_t n = 0; n < kBlock; ++n) {
            const double t = static_cast<double>(start + n) / rate;
            const bool key = std::fmod(t, 0.12) < 0.06;
            const Cd noise{gauss(rng), gauss(rng)};
            const Cd s = s_z;
            const Cd c = key ? c_z : Cd{};
            s_z *= s_r;
            c_z *= c_r;
            const Cd w = c + noise;
            without[n] = dsp::Complex32{static_cast<float>(w.real()), static_cast<float>(w.imag())};
            const Cd x = w + s;
            raw[n] = dsp::Complex32{static_cast<float>(x.real()), static_cast<float>(x.imag())};
        }
        n_total += kBlock;

        const CombBlockPlan plan = tracker.plan(start, kBlock);
        REQUIRE(plan.lines.size() == 1);
        dsp::CombParams params;
        params.ring_mask = kBlock - 1;
        params.first = 0;
        params.count = kBlock;
        params.lines = 1;
        params.sub_blocks = plan.sub_blocks;
        params.removed_mask = kBlock - 1;
        REQUIRE(dsp::reference_comb_project(params, raw, plan.lines, table, proj));
        tracker.observe(CombObservation{start, kBlock, plan.sub_blocks,
                                        std::span<const dsp::Complex32>(proj.data(),
                                                                        plan.sub_blocks)});
        if (locked_at < 0 && tracker.status().lock == CombLock::kLocked) {
            locked_at = blk + 1;
        }
        if (!plan.subtract) {
            continue;
        }
        REQUIRE(dsp::reference_comb_subtract(params, raw, plan.lines, table, cleaned, removed));
        if (blk < 140) {
            continue;  // settling
        }
        spur_before += std::norm(project(raw, kSpurHz, start) - project(without, kSpurHz, start));
        spur_after += std::norm(project(cleaned, kSpurHz, start) - project(without, kSpurHz, start));
        const Cd cw_in = project(without, kCwHz, start);
        const Cd cw_out = project(cleaned, kCwHz, start);
        cw_power += std::norm(cw_in);
        cw_error += std::norm(cw_out - cw_in);
    }
    INFO("locked at block " << locked_at);
    REQUIRE(locked_at > 0);
    REQUIRE(spur_after > 0.0);
    const double suppression_db = 10.0 * std::log10(spur_before / spur_after);
    const double cw_change_db = 10.0 * std::log10(cw_power / std::max(cw_error, 1e-30));
    std::printf("  locked at %d; spur down %.1f dB; CW disturbed %.1f dB under itself\n",
                locked_at, suppression_db, cw_change_db);
    CHECK(tracker.status().lock == CombLock::kLocked);
    CHECK(suppression_db >= 30.0);
    // What the subtraction did to the CW is at least 30 dB under the CW.
    CHECK(cw_change_db >= 30.0);
}

// A spur is held in the bank beside a comb, in one slot, and let go when a
// receiver comes to listen to it.
TEST_CASE("comb bank: holds a spur in one slot and drops it for a receiver", "[comb][tracker][spur]")
{
    dsp::CombBank bank(kRate);
    CombEstimate spur;
    spur.offset_hz = 41'234.5;
    spur.k_lo = 0;
    spur.k_hi = 0;
    spur.lines_found = 1;
    spur.spur = true;
    bank.spurs(std::span<const CombEstimate>(&spur, 1));
    REQUIRE(bank.trackers().size() == 1);
    CHECK(bank.trackers()[0].spur());
    CHECK(bank.trackers()[0].lines() == 1);

    // Offered again: already held.
    bank.spurs(std::span<const CombEstimate>(&spur, 1));
    CHECK(bank.trackers().size() == 1);
    // Not a comb, so not in the comb's status.
    CHECK(bank.status().lock == CombLock::kIdle);

    const dsp::CombBank::SpurBand far[] = {{99'000.0, 101'000.0}};
    bank.drop_spurs_in(far);
    CHECK(bank.trackers().size() == 1);
    const dsp::CombBank::SpurBand listened[] = {{40'000.0, 42'000.0}};
    bank.drop_spurs_in(listened);
    CHECK(bank.trackers().empty());

    // Just dropped: offered again at once, it waits out the hold-off.
    bank.spurs(std::span<const CombEstimate>(&spur, 1));
    CHECK(bank.trackers().empty());
    for (int blk = 0; blk < 120; ++blk) {
        (void)bank.plan(static_cast<dsp::SampleIndex>(blk) * kBlock, kBlock);
    }
    bank.spurs(std::span<const CombEstimate>(&spur, 1));
    CHECK(bank.trackers().size() == 1);

    // Withdrawn by the detector while the carrier is still there: let go.
    const double gone[] = {spur.offset_hz + 10.0};
    bank.withdraw_spurs(gone);
    CHECK(bank.trackers().empty());
}

// A spur on a comb's ripple sideband is a line the comb already subtracts.
TEST_CASE("comb bank: a spur on a comb's sideband line is not started", "[comb][tracker][spur]")
{
    dsp::CombBank bank(kRate);
    CombEstimate comb;
    comb.offset_hz = 2'707.0;
    comb.spacing_hz = 31'578.9;
    comb.k_lo = -10;
    comb.k_hi = 10;
    comb.lines_found = 21;
    comb.ripple_hz = 59.5;
    comb.sideband_orders = 2;
    bank.detected(std::span<const CombEstimate>(&comb, 1));
    REQUIRE(bank.trackers().size() == 1);
    REQUIRE(bank.trackers()[0].holds_frequency(comb.offset_hz + 3 * comb.spacing_hz + 2 * 59.5,
                                               1.0));

    CombEstimate spur;
    spur.offset_hz = comb.offset_hz + 3 * comb.spacing_hz + 2 * 59.5 + 5.0;
    spur.k_lo = 0;
    spur.k_hi = 0;
    spur.lines_found = 1;
    spur.spur = true;
    bank.spurs(std::span<const CombEstimate>(&spur, 1));
    CHECK(bank.trackers().size() == 1);

    // Off every line, it is.
    spur.offset_hz = comb.offset_hz + 3.5 * comb.spacing_hz;
    bank.spurs(std::span<const CombEstimate>(&spur, 1));
    CHECK(bank.trackers().size() == 2);
}
