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

}  // namespace revenant
