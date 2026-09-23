// What each of the engine's threads is for, said to the operating system.
//
// Every thread this process starts used to show as an anonymous std::thread
// in a debugger, in Process Explorer and in the per-thread CPU table
// tools/loadtest prints, so "the process is using two cores" could not be
// turned into "the completion thread is using one of them". The name is
// SetThreadDescription's, which every one of those tools reads. Added
// 2026-09-23 for the contention measurement under "Threading" in docs/rpc.md.

#pragma once

#include <string_view>

namespace revenant {

// Names the calling thread. Best effort: a platform or a Windows build
// without SetThreadDescription leaves the thread unnamed and says nothing,
// because a name is a diagnostic and never a reason to fail.
void name_this_thread(std::wstring_view name);

}  // namespace revenant
