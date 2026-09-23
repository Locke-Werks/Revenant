// The recordings opened lately, and which of them are still there.
//
// Qt-free, so ui/tests can link it. Storage is QSettings under
// settings::kRecentRecordings, as one JSON value for the reason
// settings::kBookmarks gives; ui/models/recording_link.cpp does that half.
//
// AN ENTRY IS WHAT WAS OPENED AND NOT ONLY WHERE. A KF4FIC wideband WAV
// states no centre, so reopening one from this list without the centre it was
// last opened at would put the operator back in front of an empty box they
// already filled in once. The centre, rate and format travel with the path,
// as the operator typed them, and go back into the boxes on a pick.

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace revenant::ui {

// The last ten, which the owner asked for on 2026-09-23.
inline constexpr std::size_t kRecentRecordingsKept = 10;

struct RecentRecording {
    std::string path;

    // As typed into the section's boxes, empty where nothing was. Text and not
    // numbers, so a centre typed "7.15" comes back as "7.15" rather than as a
    // number the operator did not write.
    std::string center_text;
    std::string rate_text;
    std::string format_text;
};

// Whether two paths name one file on this filesystem. Windows paths are
// case-insensitive and take either slash, so "C:\x.wav" and "c:/X.WAV" are one
// entry rather than two; the list would otherwise fill with the same file
// spelled by the dialog and by a command line.
[[nodiscard]] inline bool same_recording_path(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x == '\\') {
            x = '/';
        }
        if (y == '\\') {
            y = '/';
        }
        if (x >= 'A' && x <= 'Z') {
            x = static_cast<char>(x - 'A' + 'a');
        }
        if (y >= 'A' && y <= 'Z') {
            y = static_cast<char>(y - 'A' + 'a');
        }
        if (x != y) {
            return false;
        }
    }
    return true;
}

// The list after `opened` was opened: it goes first, an earlier entry for the
// same file goes, and the eleventh falls off.
//
// THE CAP COUNTS ENTRIES WHOSE FILES ARE MISSING TOO. A recording on a drive
// that is unplugged today is still one of the last ten opened, and dropping it
// from storage because it was absent at one launch would lose it for good
// when the drive comes back. It is hidden from the list, not forgotten.
[[nodiscard]] inline std::vector<RecentRecording> remember_recording(
    std::vector<RecentRecording> list, RecentRecording opened)
{
    std::erase_if(list, [&opened](const RecentRecording& entry) {
        return same_recording_path(entry.path, opened.path);
    });
    list.insert(list.begin(), std::move(opened));
    if (list.size() > kRecentRecordingsKept) {
        list.resize(kRecentRecordingsKept);
    }
    return list;
}

// The entries to show: those whose file is still there, in order.
[[nodiscard]] inline std::vector<RecentRecording> existing_recordings(
    const std::vector<RecentRecording>& list, const std::function<bool(const std::string&)>& exists)
{
    std::vector<RecentRecording> out;
    for (const RecentRecording& entry : list) {
        if (!entry.path.empty() && exists(entry.path)) {
            out.push_back(entry);
        }
    }
    return out;
}

}  // namespace revenant::ui
