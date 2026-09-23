// Holding the RTL-SDR for the length of a test case that opens it.
//
// Every case tagged [dongle] starts with hold_the_dongle(). It takes the same
// machine-wide lock the engine takes, core/source/rtlsdr_lock.h, and keeps it
// until the case ends, so the case's own opens share it at once and nothing
// outside the process can open the dongle between two of them.
//
// A HELD DONGLE IS AN ENVIRONMENT FACT, NOT A TEST FAILURE. Valkyrie is the CI
// runner and the owner's desk at once, and on 2026-09-22 CI went red all night
// because the owner's engine had the radio. So a case that cannot get the lock
// skips with that reason, which ctest reports as Skipped rather than Failed,
// and the suite stays an honest statement about the code.
//
// HOW LONG IT WAITS. Long enough to let a short holder finish: a picker
// describing the dongle, a revenant-cli run somebody started, a case from a
// second checkout's suite. Not long enough to outlast a stream, which can run
// for hours, so a case against a streaming engine waits out its bound and
// skips.
//
// That bound is paid per case, and roughly twenty cases open the dongle, so a
// streaming engine would add twenty minutes to a run at a flat minute each. The
// first case to time out therefore leaves a marker in the temp directory, and
// for ten minutes after it later cases wait two seconds instead of sixty: long
// enough to take a lock that has come free, and short enough that a run
// against a busy radio finishes in the time it would have taken anyway.
// REVENANT_DONGLE_WAIT_S overrides both, in whole seconds, for a developer who
// wants to wait longer or not at all.

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include "core/source/device_lock.h"
#include "core/source/rtlsdr_lock.h"
#include "core/source/rtlsdr_source.h"

namespace revenant::test {

inline constexpr std::chrono::seconds kDongleWait{60};
inline constexpr std::chrono::seconds kDongleWaitAfterATimeout{2};
inline constexpr std::chrono::minutes kDongleTimeoutMemory{10};

[[nodiscard]] inline std::filesystem::path dongle_held_marker()
{
    std::error_code error;
    const std::filesystem::path temp = std::filesystem::temp_directory_path(error);
    return (error ? std::filesystem::path(".") : temp) / "revenant-dongle-held";
}

[[nodiscard]] inline std::chrono::seconds dongle_wait()
{
    if (const char* stated = std::getenv("REVENANT_DONGLE_WAIT_S"); stated != nullptr) {
        return std::chrono::seconds(std::max(0L, std::strtol(stated, nullptr, 10)));
    }

    std::error_code error;
    const auto written = std::filesystem::last_write_time(dongle_held_marker(), error);
    if (!error) {
        const auto age = std::filesystem::file_time_type::clock::now() - written;
        if (age < kDongleTimeoutMemory) {
            return kDongleWaitAfterATimeout;
        }
    }
    return kDongleWait;
}

// Skips when no dongle is attached or when another process holds it past the
// wait, and fails only when the lock itself cannot be taken for some other
// reason, which is a fault rather than a busy machine.
[[nodiscard]] inline source::DeviceLock hold_the_dongle()
{
    auto attached = source::enumerate_rtlsdr_devices();
    if (!attached.has_value() || attached->empty()) {
        SKIP("no RTL-SDR is attached to this machine");
    }

    const std::chrono::seconds wait = dongle_wait();
    auto lock = source::DeviceLock::acquire(source::kRtlSdrLockName, wait);
    if (!lock) {
        if (source::held_elsewhere(lock.error())) {
            std::ofstream(dongle_held_marker()) << "the RTL-SDR lock was held elsewhere\n";
            SKIP("the RTL-SDR is held by another Revenant process, which is the machine being in "
                 "use and not a test failure: waited "
                 << wait.count() << " s for " << source::kRtlSdrLockName << ". "
                 << lock.error().message);
        }
        FAIL("the RTL-SDR lock could not be taken: " << lock.error().message);
    }

    std::error_code ignored;
    std::filesystem::remove(dongle_held_marker(), ignored);
    return std::move(*lock);
}

}  // namespace revenant::test
