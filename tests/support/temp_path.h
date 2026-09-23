// A path in the system temp directory that no other test process can pick.
//
// %TEMP% is one directory for every checkout of this tree on the machine, and
// several checkouts run their suites at once. A fixed name there is shared
// state between processes that know nothing of each other. On 2026-09-23,
// with parallel checkouts on one machine, the RDS end-to-end case and "a
// burst of retunes does not leave the fence permanently armed" failed; both
// wrote their capture to a name fixed by the case, which every checkout's run
// of that case writes, reads and removes.
//
// The process id makes a name unique among processes alive at the same time,
// and the counter among calls inside one process. A clock is not used: two
// processes can read the same tick, and nothing about a tick is unique.
//
// The stem goes first so a file left behind by a crashed run still says which
// case wrote it.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string_view>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace revenant::test {

[[nodiscard]] inline std::uint64_t process_id() {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

// `extension` includes its dot, ".cf32", or is empty for a directory.
[[nodiscard]] inline std::filesystem::path unique_temp_path(std::string_view stem,
                                                            std::string_view extension = {}) {
    static std::atomic<std::uint64_t> counter{0};
    return std::filesystem::temp_directory_path() /
           std::format("{}-{}-{}{}", stem, process_id(), counter.fetch_add(1), extension);
}

}  // namespace revenant::test
