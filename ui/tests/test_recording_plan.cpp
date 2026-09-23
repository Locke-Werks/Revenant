// plan_recording_open, file_uri_for_path, guess_center_from_name and
// split_recording_argument: what the recording section sends, and when.
//
// EVERY TEST NAMES THE WRONG IMPLEMENTATION IT REJECTS. The ones that matter
// here send a request the engine refuses for a reason the section could have
// said first: restating a container's rate or centre differently, which the
// engine refuses naming both; leaving a raw file's format to the engine's
// case-sensitive inference; and a path whose '%' or '?' the registry decodes
// into another file. And one that does something worse than a refusal: a
// guess that fills the centre box on its own, which stops being read as a
// guess.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>

#include "models/recording_header.h"
#include "models/recording_plan.h"

using revenant::ui::file_uri_for_path;
using revenant::ui::guess_center_from_name;
using revenant::ui::plan_recording_open;
using revenant::ui::recording_file_filters;
using revenant::ui::RecordingChoice;
using revenant::ui::RecordingFormat;
using revenant::ui::RecordingHeader;
using revenant::ui::RecordingKind;
using revenant::ui::split_recording_argument;

namespace {

// One of the six KF4FIC recordings as read_recording_header describes it.
[[nodiscard]] RecordingHeader kf4fic()
{
    RecordingHeader header;
    header.kind = RecordingKind::Wav;
    header.container = "RIFF WAV";
    header.path = "C:/Users/vexam/projects/SDR Recordings/"
                  "KF4FIC_wideband_7000_7300kHz_20170821_1359UT.wav";
    header.data_path = header.path;
    header.data_bytes = 2'146'437'120u;
    header.format = RecordingFormat::Cs24;
    header.has_rate = true;
    header.rate = 96000;
    header.channels = "2, I and Q";
    return header;
}

[[nodiscard]] RecordingHeader raw_cs16()
{
    RecordingHeader header;
    header.kind = RecordingKind::Raw;
    header.container = "raw";
    header.path = "D:/caps/x.CS16";
    header.data_path = header.path;
    header.data_bytes = 4 * 1000;
    header.format = RecordingFormat::Cs16;
    return header;
}

[[nodiscard]] RecordingChoice centre(std::int64_t hz)
{
    RecordingChoice choice;
    choice.center_hz = hz;
    choice.center_typed = true;
    return choice;
}

}  // namespace

TEST_CASE("a WAV with no centre waits for one and then sends only center=", "[recording]")
{
    // Rejects: opening at 0 Hz, which the engine allows and the owner does
    // not want, and restating the container's rate, which is harmless until
    // somebody types a different one.
    const RecordingHeader header = kf4fic();
    const auto empty = plan_recording_open(header, {});
    CHECK_FALSE(empty.ready);
    CHECK(empty.uri.empty());
    CHECK(empty.blocker.find("no centre") != std::string::npos);

    // Waiting only for its centre, it already knows how long it is, which the
    // preview shows while the box is empty.
    CHECK(empty.length_samples == 357'739'520u);
    CHECK(empty.rate == 96000);

    const auto planned = plan_recording_open(header, centre(7'150'000));
    REQUIRE(planned.ready);
    CHECK(planned.uri ==
          "file:///C:/Users/vexam/projects/SDR Recordings/"
          "KF4FIC_wideband_7000_7300kHz_20170821_1359UT.wav?center=7150000");
    CHECK(planned.rate == 96000);
    CHECK(planned.format == RecordingFormat::Cs24);
    CHECK(planned.length_samples == 357'739'520u);
    CHECK_FALSE(planned.center_from_file);
}

TEST_CASE("a centre the recording states is not restated, and a different one is refused",
          "[recording]")
{
    // Rejects: sending center= over a container's own. The engine refuses a
    // disagreement naming both numbers, so the section says it first.
    RecordingHeader header = kf4fic();
    header.has_center = true;
    header.center_hz = 7'100'000;

    const auto plain = plan_recording_open(header, {});
    REQUIRE(plain.ready);
    CHECK(plain.uri.find("center=") == std::string::npos);
    CHECK(plain.center_hz == 7'100'000);
    CHECK(plain.center_from_file);

    CHECK(plan_recording_open(header, centre(7'100'000)).ready);

    const auto clash = plan_recording_open(header, centre(7'150'000));
    CHECK_FALSE(clash.ready);
    CHECK(clash.blocker.find("7.100") != std::string::npos);
}

TEST_CASE("a rate typed over a container's own is refused when it differs", "[recording]")
{
    // Rejects: letting a typed rate win. Every frequency derived from the
    // wrong rate is off by the ratio and nothing downstream can tell.
    RecordingChoice choice = centre(7'150'000);
    choice.rate = 48000;
    choice.rate_typed = true;
    const auto clash = plan_recording_open(kf4fic(), choice);
    CHECK_FALSE(clash.ready);
    CHECK(clash.blocker.find("96000") != std::string::npos);
}

TEST_CASE("a raw file needs a rate and a centre, and always states its format", "[recording]")
{
    // Rejects: leaving format= off because the extension named it. The
    // engine's extension match is case-sensitive, so "x.CS16" would be
    // refused by the engine after this preview called it cs16.
    const RecordingHeader header = raw_cs16();
    CHECK(plan_recording_open(header, centre(98'100'000)).blocker.find("rate") !=
          std::string::npos);

    RecordingChoice choice = centre(98'100'000);
    choice.rate = 2'400'000;
    choice.rate_typed = true;
    const auto planned = plan_recording_open(header, choice);
    REQUIRE(planned.ready);
    CHECK(planned.uri == "file:///D:/caps/x.CS16?rate=2400000&format=cs16&center=98100000");
    CHECK(planned.length_samples == 1000);

    // The operator's format wins over the extension's for a raw file, and the
    // length follows it.
    choice.format = RecordingFormat::Cf32;
    const auto wider = plan_recording_open(header, choice);
    REQUIRE(wider.ready);
    CHECK(wider.uri.find("format=cf32") != std::string::npos);
    CHECK(wider.length_samples == 500);
}

TEST_CASE("a raw file whose name names no format asks for one", "[recording]")
{
    // Rejects: defaulting to cf32, which the engine's URI parser does for an
    // unstated format= and which reinterprets every byte of a cs16 file.
    RecordingHeader header = raw_cs16();
    header.path = "D:/caps/x.iq";
    header.format = RecordingFormat::Unknown;
    RecordingChoice choice = centre(98'100'000);
    choice.rate = 2'400'000;
    const auto planned = plan_recording_open(header, choice);
    CHECK_FALSE(planned.ready);
    CHECK(planned.blocker.find("format") != std::string::npos);
}

TEST_CASE("a partial trailing sample is refused as the engine refuses it", "[recording]")
{
    // Rejects: rounding the length down. Either the format is wrong or the
    // file was cut, and both are worth stopping for.
    RecordingHeader header = raw_cs16();
    header.data_bytes = 4 * 1000 + 2;
    RecordingChoice choice = centre(98'100'000);
    choice.rate = 2'400'000;
    CHECK(plan_recording_open(header, choice).blocker.find("whole number") != std::string::npos);
}

TEST_CASE("junk in a box is its own sentence, not an empty box", "[recording]")
{
    // Rejects: a junk centre treated as none typed, which would show "type
    // the centre" to somebody who just typed one.
    RecordingChoice choice;
    choice.center_typed = true;
    CHECK(plan_recording_open(kf4fic(), choice).blocker.find("does not hold a frequency") !=
          std::string::npos);
}

TEST_CASE("a refused file is not planned", "[recording]")
{
    // Rejects: composing a URI for a file the preview already knows the
    // engine will refuse.
    RecordingHeader header = kf4fic();
    header.refusal = "it declares 1 channels, and an IQ recording is two, I and Q";
    const auto planned = plan_recording_open(header, centre(7'150'000));
    CHECK_FALSE(planned.ready);
    CHECK(planned.blocker.find("1 channels") != std::string::npos);
}

TEST_CASE("a path becomes the URI the registry reads back as the same path", "[recording]")
{
    // Rejects: an unescaped '%' or '?', which the registry decodes or splits
    // into a different path, and a Windows path written with one slash after
    // the scheme, which the registry reads as a host.
    CHECK(file_uri_for_path("C:\\Users\\vexam\\x.wav") == "file:///C:/Users/vexam/x.wav");
    CHECK(file_uri_for_path("C:/SDR Recordings/a b.wav") == "file:///C:/SDR Recordings/a b.wav");
    CHECK(file_uri_for_path("C:/caps/100%?.cs16") == "file:///C:/caps/100%25%3F.cs16");
    CHECK(file_uri_for_path("C:/caps/#1.cu8") == "file:///C:/caps/%231.cu8");
    CHECK(file_uri_for_path("\\\\nas\\iq\\x.wav") == "file://nas/iq/x.wav");
    CHECK(file_uri_for_path("/home/op/x.cf32") == "file:///home/op/x.cf32");
}

TEST_CASE("a filename's band gives its midpoint, labelled as the band", "[recording]")
{
    // Rejects: reading a date or a time as a frequency, and presenting the
    // band's midpoint as the tuning. docs/recordings.md used exactly these
    // two midpoints and calls them an assumption.
    const auto forty = guess_center_from_name(
        "C:/SDR Recordings/KF4FIC_wideband_7000_7300kHz_20170821_1359UT.wav");
    REQUIRE(forty.has_value());
    CHECK(forty->hz == 7'150'000);
    CHECK(forty->why.find("midpoint of 7000 to 7300 kHz") != std::string::npos);
    CHECK(forty->why.find("not necessarily the tuning") != std::string::npos);

    const auto twenty =
        guess_center_from_name("KF4FIC_wideband_14000_14350kHz_20170821_1603UT.wav");
    REQUIRE(twenty.has_value());
    CHECK(twenty->hz == 14'175'000);
}

TEST_CASE("a filename's single frequency is taken with its unit", "[recording]")
{
    // Rejects: a bare number read as a frequency. The SDR# name below holds a
    // date and a time before its frequency; the gqrx one holds a frequency
    // and a rate with no units at all and gets no guess.
    const auto hdsdr = guess_center_from_name("HDSDR_20260921_140307Z_7100kHz_RF.wav");
    REQUIRE(hdsdr.has_value());
    CHECK(hdsdr->hz == 7'100'000);

    const auto sdrsharp = guess_center_from_name("SDRSharp_20170821_135900Z_7100000Hz_IQ.wav");
    REQUIRE(sdrsharp.has_value());
    CHECK(sdrsharp->hz == 7'100'000);

    const auto dotted = guess_center_from_name("ft8_14.074MHz.cs16");
    REQUIRE(dotted.has_value());
    CHECK(dotted->hz == 14'074'000);

    CHECK_FALSE(guess_center_from_name("gqrx_20170821_135900_7100000_2400000_fc.raw").has_value());
    CHECK_FALSE(guess_center_from_name("capture.wav").has_value());
}

// Not named for the option: a test name starting "--" reaches Catch2's command
// line as an option when ctest runs it by name, and fails as an unknown one.
TEST_CASE("a startup recording's centre is split off its last colon only", "[recording]")
{
    // Rejects: splitting at the first colon, which cuts every Windows path at
    // its drive letter.
    const auto plain = split_recording_argument("C:\\SDR Recordings\\x.wav");
    CHECK(plain.path == "C:\\SDR Recordings\\x.wav");
    CHECK(plain.center_text.empty());

    const auto with = split_recording_argument("C:\\SDR Recordings\\x.wav:7150000");
    CHECK(with.path == "C:\\SDR Recordings\\x.wav");
    CHECK(with.center_text == "7150000");

    const auto relative = split_recording_argument("x.wav:7.15M");
    CHECK(relative.path == "x.wav");
    CHECK(relative.center_text == "7.15M");

    const auto odd = split_recording_argument("C:/a:b/x.wav");
    CHECK(odd.path == "C:/a:b/x.wav");
    CHECK(odd.center_text.empty());
}

TEST_CASE("only a loopback engine is taken to share this disk", "[recording]")
{
    // Rejects: treating any engine as local. A preview read here and opened
    // there is a preview of the wrong file, so anything not loopback gets the
    // caveat, including a LAN address that may well be this machine.
    CHECK(revenant::ui::engine_is_local("127.0.0.1"));
    CHECK(revenant::ui::engine_is_local("127.1.2.3"));
    CHECK(revenant::ui::engine_is_local("localhost"));
    CHECK(revenant::ui::engine_is_local("::1"));
    CHECK(revenant::ui::engine_is_local("[::1]"));
    CHECK_FALSE(revenant::ui::engine_is_local("192.168.1.20"));
    CHECK_FALSE(revenant::ui::engine_is_local("172.22.77.44"));
    CHECK_FALSE(revenant::ui::engine_is_local(""));
}

TEST_CASE("the dialog's first filter covers every type the engine opens", "[recording]")
{
    // Rejects: a first filter narrower than the engine, which hides a file
    // the operator can open behind a filter they have to go and change.
    const auto filters = recording_file_filters();
    REQUIRE_FALSE(filters.empty());
    for (const char* pattern : {"*.wav", "*.rf64", "*.bw64", "*.sigmf-meta", "*.sigmf-data",
                                "*.cu8", "*.cs8", "*.cs16", "*.cs24", "*.cf32"}) {
        CHECK(filters.front().find(pattern) != std::string::npos);
    }
    CHECK(filters.back() == "All files (*)");
}
