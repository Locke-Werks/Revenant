// core/thread_role.h and core/engine/cpu_budget.h: the two halves of the
// order of precedence the owner set on 2026-09-23, receivers first, display
// second, signal identification last. Neither needs a GPU.
//
// WHAT THESE CASES CLAIM
//
// That each thread class lands on the Windows priority core/thread_role.h
// says it does, since a class that silently stayed at normal would leave the
// order to chance on a busy machine; that a thread's CPU meter counts CPU
// rather than wall time, which is the whole reason a budget is charged in it;
// and that a budget holds a thread to its share over a long run, lets an idle
// stretch buy no more than its burst, and starts nothing while it is empty.

#include <chrono>
#include <cstdint>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "core/engine/cpu_budget.h"
#include "core/thread_role.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace revenant;

#if defined(_WIN32)
TEST_CASE("each thread class runs at the priority its order of precedence gives it",
          "[engine][threads]") {
    const auto priority_for = [](ThreadClass role) {
        int seen = 0;
        std::thread probe([&] {
            REQUIRE(set_this_thread_class(role));
            seen = GetThreadPriority(GetCurrentThread());
        });
        probe.join();
        return seen;
    };
    CHECK(priority_for(ThreadClass::Listening) == THREAD_PRIORITY_ABOVE_NORMAL);
    CHECK(priority_for(ThreadClass::Display) == THREAD_PRIORITY_NORMAL);
    CHECK(priority_for(ThreadClass::SigId) == THREAD_PRIORITY_BELOW_NORMAL);
}

TEST_CASE("a thread's CPU meter counts its CPU and not the time it slept", "[engine][threads]") {
    const std::uint64_t before = this_thread_cpu_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const std::uint64_t slept = this_thread_cpu_ns() - before;

    // Half a second of arithmetic, timed on the wall clock.
    const auto began = std::chrono::steady_clock::now();
    volatile double sink = 1.0;
    while (std::chrono::steady_clock::now() - began < std::chrono::milliseconds(500)) {
        for (int i = 0; i < 10'000; ++i) {
            sink = sink * 1.0000001 + 1e-9;
        }
    }
    const std::uint64_t worked = this_thread_cpu_ns() - before - slept;

    // The meter's tick is about 15.6 ms, so the sleep reads as at most a tick
    // or two. The work reads as most of its half second: not all of it,
    // because this runs beside the rest of the suite and a busy machine
    // takes some of the half second away, which is exactly what a wall-time
    // meter would have failed to see.
    INFO("slept " << slept << " ns of CPU, worked " << worked << " ns");
    CHECK(slept <= 50'000'000);
    CHECK(worked >= 250'000'000);
    CHECK(worked <= 600'000'000);
}
#endif

TEST_CASE("a CPU budget holds a thread to its share and spends an idle stretch only once",
          "[engine][threads]") {
    constexpr std::uint64_t kMs = 1'000'000;

    // A quarter of a core with 100 ms of burst.
    engine::CpuBudget budget(0.25, 100 * kMs);

    // The first question fills the bucket, so the first job may start.
    std::uint64_t now = 1'000 * kMs;
    REQUIRE(budget.may_start(now));

    // A job that takes 60 ms of CPU leaves 40 ms of credit, so another may
    // start at once; the second one's 60 ms takes it to -20.
    budget.charge(60 * kMs, now);
    CHECK(budget.may_start(now));
    budget.charge(60 * kMs, now);
    CHECK_FALSE(budget.may_start(now));

    // Credit comes back at a quarter of wall time: 20 ms of it takes 80 ms.
    CHECK_FALSE(budget.may_start(now + 79 * kMs));
    CHECK(budget.may_start(now + 81 * kMs));

    // Over a long run of jobs that each take 10 ms, started whenever the
    // budget allows, the CPU charged is a quarter of the wall time spent,
    // within the one burst the bucket began with.
    engine::CpuBudget steady(0.25, 100 * kMs);
    std::uint64_t clock = 0;
    std::uint64_t charged = 0;
    for (int step = 0; step < 100'000; ++step) {
        clock += kMs;
        if (steady.may_start(clock)) {
            steady.charge(10 * kMs, clock);
            charged += 10 * kMs;
        }
    }
    const double share = static_cast<double>(charged) / static_cast<double>(clock);
    INFO("charged " << charged << " ns over " << clock << " ns of wall time");
    CHECK(share >= 0.249);
    CHECK(share <= 0.252);

    // An idle hour buys the burst and not an hour's worth of work.
    engine::CpuBudget idle(0.25, 100 * kMs);
    REQUIRE(idle.may_start(0));
    idle.charge(0, 0);
    const std::uint64_t later = 3'600'000 * kMs;
    std::uint64_t started = 0;
    while (idle.may_start(later)) {
        idle.charge(10 * kMs, later);
        ++started;
    }
    CHECK(started == 10);
}
