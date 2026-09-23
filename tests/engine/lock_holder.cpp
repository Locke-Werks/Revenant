// A second process for tests/engine/test_device_lock.cpp to contend with.
//
// The lock exists to arbitrate between processes, and a second thread in the
// test's own process would share the test's acquisition by design, so the only
// honest contender is another process. This is it, and it does nothing but
// take and report:
//
//   revenant_lock_holder <name> hold [wait_ms]
//       Takes the lock, waiting up to wait_ms (default 10000). Prints "held"
//       or "held abandoned" on success, then waits for a line or for stdin to
//       close, releases, prints "released" and exits 0. Prints "busy" and
//       exits 3 when the wait ran out. The test kills it mid-hold to stand in
//       for an engine that crashed while streaming.
//
//   revenant_lock_holder <name> try
//       Asks once without waiting. Prints "free" and exits 0 when it got the
//       lock, which it lets go at once, or "busy" and exits 3.
//
// Anything else prints the error and exits 2. It never touches a device: the
// tests hand it a stand-in name, never the RTL-SDR's.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "core/source/device_lock.h"

namespace {

void say(const char* line)
{
    std::fputs(line, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv)
{
    using revenant::source::DeviceLock;

    if (argc < 3) {
        std::fputs("usage: revenant_lock_holder <name> hold [wait_ms] | try\n", stderr);
        return 2;
    }
    const std::string name = argv[1];
    const std::string mode = argv[2];

    if (mode == "try") {
        auto lock = DeviceLock::acquire(name, std::chrono::milliseconds(0));
        if (lock) {
            say("free");
            return 0;
        }
        if (revenant::source::held_elsewhere(lock.error())) {
            say("busy");
            return 3;
        }
        std::fprintf(stderr, "%s\n", lock.error().message.c_str());
        return 2;
    }

    if (mode == "hold") {
        const long wait_ms = argc > 3 ? std::strtol(argv[3], nullptr, 10) : 10'000;
        auto lock = DeviceLock::acquire(name, std::chrono::milliseconds(wait_ms));
        if (!lock) {
            if (revenant::source::held_elsewhere(lock.error())) {
                say("busy");
                return 3;
            }
            std::fprintf(stderr, "%s\n", lock.error().message.c_str());
            return 2;
        }
        say(lock->recovered_from_abandoned() ? "held abandoned" : "held");

        std::string line;
        std::getline(std::cin, line);

        lock->release();
        say("released");
        return 0;
    }

    std::fprintf(stderr, "unknown mode '%s'\n", mode.c_str());
    return 2;
}
