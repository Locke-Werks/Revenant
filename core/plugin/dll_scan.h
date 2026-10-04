// The parts of finding plugin DLLs that do not depend on which ABI they speak.
//
// Two loaders use these: core/decode/vocoder_plugin.cpp and
// core/plugin/engine_plugin.cpp. Each owns its own contract checks, its own
// refusals and its own report; what they share is where the folder is and
// what in it counts as a candidate, which must not come to differ between
// them.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace revenant::plugin {

// `name` beside the running executable. Empty when the executable's own path
// cannot be determined, or on a platform with no loader yet, which the caller
// reports rather than guessing a path relative to the working directory.
[[nodiscard]] std::filesystem::path directory_beside_executable(std::string_view name);

// Every regular file in `directory` with a .dll extension, any case, sorted so
// that two machines scan in the same order. `directory` should be absolute,
// because LoadLibraryExW's search flags require a fully qualified path.
[[nodiscard]] std::vector<std::filesystem::path> dll_candidates(
    const std::filesystem::path& directory);

// Win32's sentence for a GetLastError code.
[[nodiscard]] std::string os_error_text(long long code);

}  // namespace revenant::plugin
