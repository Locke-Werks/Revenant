// A lock on a piece of hardware that every process on the machine honours.
//
// WHY IT EXISTS. Valkyrie is both the CI runner and the owner's development
// machine, and on the night of 2026-09-22 CI's dongle tests failed over and
// over with "usb_open error -3" while a local process held the RTL-SDR: a
// test, a revenant-cli on rtlsdr://, or a client whose device picker called
// listSources, which opens each radio to describe it. Until this file the only
// arbitration between those processes was librtlsdr refusing the second
// opener, which arrives as a failure after the fact and says nothing about who
// holds the device or whether waiting would help. A lock every opener takes
// first turns that into a wait with a bound, and a refusal that names what
// was waited for.
//
// A NAMED MUTEX, BECAUSE A DEAD HOLDER MUST NOT KEEP THE DEVICE. Windows
// releases a mutex whose owning thread ends without releasing it and tells the
// next waiter so with WAIT_ABANDONED. That is the property wanted: an engine
// that crashes mid-stream frees the dongle for the next process instead of
// locking it out until a reboot. A semaphore or an event would be simpler to
// hold across threads and would stay taken forever after a crash. An
// abandoned mutex counts as acquired here, and recovered_from_abandoned()
// reports it, because the device itself is fine: librtlsdr's handle died with
// the process that held it.
//
// That report needs the mutex to outlive its holder, which it does only while
// some other process has it open, typically by already waiting on it. With
// nobody else holding a handle the object goes with the dead process and the
// next opener creates a fresh one, unowned and unabandoned. Both routes end
// with the next process holding the lock, which is the property that matters;
// only the first can say a crash happened.
//
// HELD BY A THREAD OF ITS OWN. A mutex belongs to a thread, not to a process,
// and the thread that opens a source is rarely the one that destroys it: the
// RPC loop opens, a worker lists, whichever thread drops the last reference
// closes. ReleaseMutex from the wrong thread fails, and worse, a mutex whose
// owning thread exits is abandoned while its process is still streaming, so
// the next process would walk straight in. So each held name gets one thread
// that takes the mutex, waits to be told to let go, and releases it. It does
// nothing else and never touches a sample.
//
// SHARED WITHIN A PROCESS. The lock arbitrates between processes. Two holders
// in one process share a single acquisition and the mutex is released when the
// last of them goes. Within a process the arbitration that already existed is
// kept on purpose: a second open of a dongle this process is streaming from
// still reaches rtlsdr_open and is refused there, by index and by call, which
// tests/engine/test_rtlsdr_source.cpp pins. Making a thread of the same engine
// wait on its own stream would turn that refusal into a stall.
//
// THE GLOBAL NAMESPACE, NOT Local. The CI runner is a service in session 0 and
// the owner's engine runs in their desktop session. Local\ is per session, so a
// Local\ name would give each of them its own lock and serialise nothing that
// matters. Global\ is one namespace for the machine. Creating a mutex there
// needs no privilege, unlike a file mapping, but its security descriptor has to
// let the other account in: see device_lock.cpp.

#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "core/error.h"

namespace revenant::source {

// The Error::code acquire carries when another process held the lock for the
// whole wait. It is WAIT_TIMEOUT, which is what WaitForSingleObject returned,
// so the code is the originating API's as error.h asks. held_elsewhere() is the
// way to test for it.
inline constexpr long long kDeviceLockHeldElsewhere = 0x102;

[[nodiscard]] inline bool held_elsewhere(const Error& error)
{
    return error.code == kDeviceLockHeldElsewhere;
}

class DeviceLock {
public:
    // Holds nothing.
    DeviceLock() = default;
    ~DeviceLock() { release(); }

    DeviceLock(DeviceLock&& other) noexcept
        : name_(std::exchange(other.name_, {})),
          abandoned_(std::exchange(other.abandoned_, false))
    {
    }

    DeviceLock& operator=(DeviceLock&& other) noexcept
    {
        if (this != &other) {
            release();
            name_ = std::exchange(other.name_, {});
            abandoned_ = std::exchange(other.abandoned_, false);
        }
        return *this;
    }

    DeviceLock(const DeviceLock&) = delete;
    DeviceLock& operator=(const DeviceLock&) = delete;

    // Takes the lock called `name`, waiting up to `wait` for another process
    // to let go of it. A zero wait asks once and does not block.
    //
    // When this process already holds the name the call shares that
    // acquisition and returns at once. When another thread of this process is
    // waiting for the same name right now, this one waits behind it within its
    // own deadline and then shares or tries in turn.
    //
    // Fails with held_elsewhere() true when the wait ran out, and with the
    // system's own error for anything else, such as a name the security
    // descriptor will not let this account open.
    [[nodiscard]] static Expected<DeviceLock> acquire(std::string_view name,
                                                      std::chrono::milliseconds wait);

    [[nodiscard]] bool held() const { return !name_.empty(); }
    [[nodiscard]] const std::string& name() const { return name_; }

    // True when the acquisition that this holder is part of took the mutex
    // over from a process that died holding it. False after a crash that left
    // nobody else with the mutex open; see the header comment.
    [[nodiscard]] bool recovered_from_abandoned() const { return abandoned_; }

    // Lets go early. The destructor does the same.
    void release();

private:
    DeviceLock(std::string name, bool abandoned) : name_(std::move(name)), abandoned_(abandoned) {}

    std::string name_{};
    bool abandoned_ = false;
};

// Whether any holder in this process has `name` right now. For diagnostics and
// for tests that need to see a lock outlive the handle it licenses.
[[nodiscard]] bool held_by_this_process(std::string_view name);

// What a device backend does about its lock, kept apart from the device calls
// so the policy can be exercised with a stand-in lock and a stand-in open.
struct DeviceLockPolicy {
    // The kernel object's name, namespace prefix included.
    std::string name;

    // The device as a sentence says it, "the RTL-SDR".
    std::string device;

    // How long an open waits for another process to let go.
    std::chrono::milliseconds open_wait{0};
};

// For a path that is about to use the device: waits up to policy.open_wait
// and then fails with an Error naming the lock and saying another process
// holds the device. held_elsewhere() stays true on that error.
[[nodiscard]] Expected<DeviceLock> lock_for_open(const DeviceLockPolicy& policy);

// For a path that only describes the device: does not wait at all. Fails with
// a message that opens with kProbeHeldElsewhere and then names the lock, so a
// listing shows the plain reason first and the detail after it.
[[nodiscard]] Expected<DeviceLock> lock_for_probe(const DeviceLockPolicy& policy);

// The sentence a probe reports when another process holds the device. A
// constant so a listing and its tests say the same words.
inline constexpr std::string_view kProbeHeldElsewhere = "in use by another Revenant process";

// A device handle and the lock that licenses it. The lock is declared first so
// it is destroyed last: the handle closes, and only then may another process
// open the device.
template <class Handle>
struct Locked {
    DeviceLock lock;
    Handle handle;
};

template <class Open>
using OpenedHandle = typename std::invoke_result_t<Open&>::value_type;

// Takes the lock for an open, then calls `open`, which returns
// Expected<Handle>. `open` is not called at all unless the lock is held, so a
// device another process is using is never touched.
template <class Open>
[[nodiscard]] Expected<Locked<OpenedHandle<Open>>> open_under_lock(const DeviceLockPolicy& policy,
                                                                   Open&& open)
{
    auto lock = lock_for_open(policy);
    if (!lock) {
        return std::unexpected(std::move(lock.error()));
    }
    auto handle = std::invoke(open);
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }
    return Locked<OpenedHandle<Open>>{std::move(*lock), std::move(*handle)};
}

// The same for a probe, which does not wait.
template <class Open>
[[nodiscard]] Expected<Locked<OpenedHandle<Open>>> probe_under_lock(const DeviceLockPolicy& policy,
                                                                    Open&& open)
{
    auto lock = lock_for_probe(policy);
    if (!lock) {
        return std::unexpected(std::move(lock.error()));
    }
    auto handle = std::invoke(open);
    if (!handle) {
        return std::unexpected(std::move(handle.error()));
    }
    return Locked<OpenedHandle<Open>>{std::move(*lock), std::move(*handle)};
}

}  // namespace revenant::source
