// The machine-wide lock every path that opens an RTL-SDR takes first.
//
// core/source/device_lock.h has the mechanism and the night that asked for it.
// This header is only the RTL-SDR's name for it and how long an open waits, kept
// apart from rtlsdr_source.h so a tool that opens a dongle without the engine,
// tools/devicespike, can apply the same policy without linking the engine.

#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include "core/source/device_lock.h"

namespace revenant::source {

// ONE LOCK FOR EVERY RTL-SDR ON THE MACHINE, not one per dongle. A dongle's
// index is its position in libusb's enumeration and moves when another is
// plugged in, and its serial can only be read by opening it, which is the
// thing the lock has to come before. The machine has one dongle today, so one
// name costs nothing. When a second radio arrives this is the line to revisit,
// because it serialises two processes that could each have had a dongle of
// their own. Within one process nothing changes: holders there share the lock,
// so one engine can still open several dongles.
//
// Global\ because the CI runner is a service in session 0 and the owner's
// engine is not, and Local\ is one namespace per session.
inline constexpr std::string_view kRtlSdrLockName = "Global\\Revenant.RtlSdr";

// How long an open waits for another process to let go before failing.
//
// Five seconds covers what a short holder does: a device picker describing
// every dongle, which opens and closes each in a few hundred milliseconds, or
// the tail of somebody's single command. It does not try to outlast a stream,
// which can hold the dongle for hours, and an open is often answering somebody
// who clicked a button, so a wait past a few seconds is a hang to them rather
// than patience.
inline constexpr std::chrono::milliseconds kRtlSdrOpenWait{5'000};

// The policy the backend applies, as device_lock.h's helpers take it. Public so
// a tool and a test standing in for the device apply exactly the same one.
[[nodiscard]] inline DeviceLockPolicy rtlsdr_lock_policy()
{
    return DeviceLockPolicy{std::string(kRtlSdrLockName), "the RTL-SDR", kRtlSdrOpenWait};
}

}  // namespace revenant::source
