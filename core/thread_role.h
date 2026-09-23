// What each of the engine's threads is for, said to the operating system.
//
// TWO THINGS, BOTH ADDED ON 2026-09-23 FOR THE CONTENTION MEASURED UNDER
// "Threading" IN docs/rpc.md
//
// A name. Every thread this process starts used to show as an anonymous
// std::thread in a debugger, in Process Explorer and in the per-thread CPU
// table tools/loadtest prints, so "the process is using two cores" could not
// be turned into "the completion thread is using one of them". The name is
// SetThreadDescription's, which every one of those tools reads.
//
// A priority, from the owner's order of precedence after the playtest of that
// day: the receivers somebody is listening to come first, the display second,
// and signal identification gets what is left. Windows schedules by priority
// before anything else, so on a machine with every core busy a thread one
// level up runs and one level down waits. That is the whole of the mechanism,
// and it only matters when the machine is oversubscribed, which is exactly
// when the order has to hold. On an idle machine every level runs.
//
//   Listening  THREAD_PRIORITY_ABOVE_NORMAL. The source's delivery, the pool
//              workers that copy its blocks into the upload buffer, the
//              completion thread that retires every frame, and the server's
//              decode lanes that run the decoders and the P25 voice stream. A
//              stall in any of them is a hole in what somebody is hearing, and
//              on a radio a hole in the source is samples that are gone.
//   Display    THREAD_PRIORITY_NORMAL. The RPC loop, which fans the spectrum
//              and the audio out to clients.
//   SigId      THREAD_PRIORITY_BELOW_NORMAL. The server's detector lane and
//              the probe worker's characteriser. Both shed work under a budget
//              of their own as well; the priority is what stops either taking
//              a core from the two above while the budget has not caught up.
//
// Nothing here is TIME_CRITICAL or REALTIME, and nothing should be: the
// engine runs beside a GUI and a sound card on the same machine, and a thread
// at those levels that spins takes the machine with it. MMCSS stays where it
// is, on the WASAPI render loop in core/engine/audio_wasapi.cpp, which runs on
// a device clock.

#pragma once

#include <cstdint>
#include <string_view>

namespace revenant {

enum class ThreadClass : unsigned char {
    Listening,
    Display,
    SigId,
};

// Names the calling thread. Best effort: a platform or a Windows build
// without SetThreadDescription leaves the thread unnamed and says nothing,
// because a name is a diagnostic and never a reason to fail.
void name_this_thread(std::wstring_view name);

// Sets the calling thread's priority for its class. Best effort for the same
// reason, and it reports whether the call took, so a test can say which.
bool set_this_thread_class(ThreadClass role);

// Both, which is what every thread start in the engine wants.
inline void describe_this_thread(std::wstring_view name, ThreadClass role)
{
    name_this_thread(name);
    static_cast<void>(set_this_thread_class(role));
}

// The calling thread's CPU time so far, user and kernel, in nanoseconds.
//
// What a budget has to be charged in. A below-normal thread on a busy machine
// can take a second of wall time to do ten milliseconds of work, and charging
// it the second would have it shed work twice over for having been made to
// wait once. The operating system counts it at the scheduler's tick, which on
// Windows is about 15.6 ms, so a single short job reads as zero or one tick;
// summed over many jobs the total is right, which is the only way
// core/engine/cpu_budget.h uses it. Zero where the platform will not say.
[[nodiscard]] std::uint64_t this_thread_cpu_ns();

}  // namespace revenant
