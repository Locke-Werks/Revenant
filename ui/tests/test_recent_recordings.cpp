// remember_recording and existing_recordings: the recent list's order, its
// cap and what it shows.
//
// EVERY TEST NAMES THE WRONG IMPLEMENTATION IT REJECTS. The ones that matter:
// a list that holds the same file twice because the dialog and a command line
// spelled it differently; one that forgets a recording on an unplugged drive
// the first time it launches without it; and one that remembers where a file
// was but not the centre it needed, which puts the operator back in front of
// an empty box on every KF4FIC recording.

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

#include "models/recent_recordings.h"

using revenant::ui::existing_recordings;
using revenant::ui::kRecentRecordingsKept;
using revenant::ui::RecentRecording;
using revenant::ui::remember_recording;
using revenant::ui::same_recording_path;

TEST_CASE("the newest goes first and an earlier entry for it goes", "[recording]")
{
    // Rejects: appending, and keeping a duplicate.
    std::vector<RecentRecording> list;
    list = remember_recording(list, {"C:/a.wav", "7.15", "", ""});
    list = remember_recording(list, {"C:/b.wav", "14.175", "", ""});
    list = remember_recording(list, {"C:/a.wav", "7.1", "", ""});
    REQUIRE(list.size() == 2);
    CHECK(list[0].path == "C:/a.wav");
    CHECK(list[0].center_text == "7.1");
    CHECK(list[1].path == "C:/b.wav");
}

TEST_CASE("one file spelled two ways is one entry", "[recording]")
{
    // Rejects: an exact string compare. The dialog hands back forward slashes
    // and a command line typed backslashes, and Windows ignores case.
    CHECK(same_recording_path("C:\\SDR Recordings\\X.wav", "c:/sdr recordings/x.WAV"));
    CHECK_FALSE(same_recording_path("C:/a.wav", "C:/a.wa"));
    CHECK_FALSE(same_recording_path("C:/a.wav", "C:/b.wav"));

    std::vector<RecentRecording> list;
    list = remember_recording(list, {"C:\\r\\x.wav", "", "", ""});
    list = remember_recording(list, {"C:/r/X.wav", "", "", ""});
    CHECK(list.size() == 1);
}

TEST_CASE("the list keeps ten and the eleventh falls off", "[recording]")
{
    // Rejects: an unbounded list, which is a registry value that grows for as
    // long as the program is used.
    std::vector<RecentRecording> list;
    for (int i = 0; i < 12; ++i) {
        list = remember_recording(list, {"C:/r/" + std::to_string(i) + ".wav", "", "", ""});
    }
    REQUIRE(list.size() == kRecentRecordingsKept);
    CHECK(kRecentRecordingsKept == 10);
    CHECK(list.front().path == "C:/r/11.wav");
    CHECK(list.back().path == "C:/r/2.wav");
}

TEST_CASE("only files that are still there are shown, and the rest are kept", "[recording]")
{
    // Rejects: dropping a missing file from storage. A recording on a drive
    // that is unplugged today is still one of the last ten opened.
    const std::vector<RecentRecording> list = {
        {"C:/r/here.wav", "7.15", "", ""},
        {"E:/usb/gone.wav", "", "", ""},
        {"C:/r/also.cs16", "98.1", "2400000", "cs16"},
    };
    const std::set<std::string> present = {"C:/r/here.wav", "C:/r/also.cs16"};
    const auto shown = existing_recordings(
        list, [&present](const std::string& path) { return present.contains(path); });
    REQUIRE(shown.size() == 2);
    CHECK(shown[0].path == "C:/r/here.wav");
    CHECK(shown[1].path == "C:/r/also.cs16");
    CHECK(shown[1].rate_text == "2400000");
    CHECK(shown[1].format_text == "cs16");
    CHECK(list.size() == 3);
}
