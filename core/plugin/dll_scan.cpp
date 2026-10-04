#include "core/plugin/dll_scan.h"

#include <algorithm>
#include <cctype>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revenant::plugin {

std::filesystem::path directory_beside_executable(std::string_view name)
{
#ifdef _WIN32
    // Grown rather than assumed, because MAX_PATH stopped being the limit and
    // a truncated executable path would resolve to a directory that exists
    // somewhere else.
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(nullptr,
                                                 buffer.data(),
                                                 static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            break;
        }
        if (buffer.size() >= 32768) {
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }

    const std::filesystem::path executable(buffer);
    return executable.parent_path() / std::filesystem::path(name);
#else
    static_cast<void>(name);
    return {};
#endif
}

std::vector<std::filesystem::path> dll_candidates(const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path> candidates;
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::string text = entry.path().extension().string();
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (text == ".dll") {
            candidates.push_back(entry.path());
        }
    }

    // Directory order is whatever the filesystem feels like, and a report, or
    // the order offers are matched in, has to be the same on two machines for
    // a bug report about either to mean anything.
    std::sort(candidates.begin(), candidates.end());
    return candidates;
}

std::string os_error_text(long long code)
{
    // std::system_category rather than FormatMessage by hand, because the
    // standard library already owns the Win32 mapping and the result is the
    // sentence the rest of the tree prints for a Win32 failure.
    return std::system_category().message(static_cast<int>(code));
}

}  // namespace revenant::plugin
