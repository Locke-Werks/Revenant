#include "core/thread_role.h"

#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace revenant {

void name_this_thread(std::wstring_view name)
{
#if defined(_WIN32)
    // SetThreadDescription wants a terminated string, and a view is not one.
    const std::wstring terminated(name);
    static_cast<void>(SetThreadDescription(GetCurrentThread(), terminated.c_str()));
#else
    static_cast<void>(name);
#endif
}

std::uint64_t this_thread_cpu_ns()
{
#if defined(_WIN32)
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) == FALSE) {
        return 0;
    }
    const auto as_100ns = [](const FILETIME& time) {
        return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
    };
    return (as_100ns(kernel) + as_100ns(user)) * 100U;
#else
    return 0;
#endif
}

bool set_this_thread_class(ThreadClass role)
{
#if defined(_WIN32)
    int priority = THREAD_PRIORITY_NORMAL;
    switch (role) {
        case ThreadClass::Listening: priority = THREAD_PRIORITY_ABOVE_NORMAL; break;
        case ThreadClass::Display: priority = THREAD_PRIORITY_NORMAL; break;
        case ThreadClass::SigId: priority = THREAD_PRIORITY_BELOW_NORMAL; break;
    }
    return SetThreadPriority(GetCurrentThread(), priority) != 0;
#else
    static_cast<void>(role);
    return false;
#endif
}

}  // namespace revenant
