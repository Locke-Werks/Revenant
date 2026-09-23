// The vocoder seam and the plugin loader, driven against real DLLs.
//
// WHY THERE ARE REAL DLLS HERE RATHER THAN A MOCK
//
// Almost everything this loader does is Win32: LoadLibraryExW with a pinned
// search path, GetProcAddress by name, a version number read out of a module
// that was compiled somewhere else. A fake plugin object injected into the
// loader would exercise the matching and the message writing and would not
// touch any of that, which is the half that fails on somebody's machine. So
// tests/decode/vocoder_test_plugin.cpp is compiled five times into five DLLs,
// each with one defect, and the real loader is pointed at real directories
// holding real files.
//
// WHAT EACH CASE REJECTS
//
// docs/conventions.md asks a test to name the wrong implementation it catches,
// because a case that passes against anything verifies nothing. Each SECTION
// below carries that line. Read them before adding a case.
//
// The recurring one is worth stating once here: the wrong implementation this
// whole file is aimed at is the SILENT one. A loader that finds no plugin and
// returns an empty set, a decode that fails and returns a bare bool, a plugin
// that misbehaves and gets called again anyway. Every one of those compiles,
// passes a happy-path test and leaves an operator staring at a mode that
// produces no audio with nothing on screen to say why. That is the defect this
// project spent a day removing from the RDS path and it is the one being kept
// out of this one.
//
// NO RANDOM INPUT. Nothing here is seeded because nothing here is randomised:
// every frame is a written-out bit pattern and every expected sample is
// exactly representable in binary32, so a failure is a wrong value and not a
// tolerance argument.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "core/decode/vocoder.h"
#include "core/decode/vocoder_abi.h"
#include "core/decode/vocoder_plugin.h"
#include "tests/support/temp_path.h"

// Written by CMake, holding the full path of each fixture DLL. See
// tests/decode/CMakeLists.txt.
#include "vocoder_test_plugin_paths.h"

using revenant::decode::check_vocoder_call;
using revenant::decode::describe_vocoder_frame;
using revenant::decode::PluginRefusal;
using revenant::decode::scan_vocoder_plugins;
using revenant::decode::Vocoder;
using revenant::decode::VocoderFrame;
using revenant::decode::VocoderKind;
using revenant::decode::VocoderPluginScanOptions;
using revenant::decode::VocoderPluginSet;
using revenant::decode::vocoder_kind_name;

namespace {

// What tests/decode/vocoder_test_plugin.cpp enumerates. Not any real codec's
// numbers, deliberately; the fixture's own header says why.
constexpr std::uint32_t kFixtureBits = 12;
constexpr std::uint32_t kFixtureSamples = 24;
constexpr std::uint32_t kFixtureRate = 8000;

[[nodiscard]] VocoderFrame fixture_frame()
{
    VocoderFrame frame;
    frame.kind = VocoderKind::External;
    frame.bit_count = kFixtureBits;
    frame.pcm_frames = kFixtureSamples;
    frame.sample_rate = kFixtureRate;
    return frame;
}

// One directory per case, emptied first, so a case cannot inherit a DLL a
// previous case installed. Named for the case, so a failed run leaves the
// directory behind under a name that says which case made it, and made unique
// by tests/support/temp_path.h, because a second checkout running the same
// case would otherwise empty the directory this one was loading from. Until
// 2026-09-23 it was revenant_vocoder_plugin_tests/<case> for every checkout on
// the machine.
[[nodiscard]] std::filesystem::path case_directory(const std::string& name)
{
    std::error_code ec;
    const std::filesystem::path dir =
        revenant::test::unique_temp_path("revenant_vocoder_plugin_tests-" + name);
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    return dir;
}

void install(const std::filesystem::path& dir, const char* built_dll)
{
    const std::filesystem::path source(built_dll);
    REQUIRE(std::filesystem::exists(source));
    std::error_code ec;
    std::filesystem::copy_file(source,
                               dir / source.filename(),
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    REQUIRE_FALSE(ec);
}

[[nodiscard]] VocoderPluginSet scan(const std::filesystem::path& dir)
{
    VocoderPluginScanOptions options;
    options.directory = dir;
    return scan_vocoder_plugins(options);
}

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Twelve bits, alternating, so the fixture's output alternates too and a
// decoder that ignored the input would have to guess the pattern to pass.
[[nodiscard]] std::vector<std::uint8_t> alternating_bits()
{
    std::vector<std::uint8_t> bits(kFixtureBits, 0);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        bits[i] = static_cast<std::uint8_t>(i % 2);
    }
    return bits;
}

}  // namespace

// ---------------------------------------------------------------------------
// The seam
// ---------------------------------------------------------------------------

TEST_CASE("Every vocoder family has a name and no two share one", "[vocoder]")
{
    // REJECTS: a name table that returns the same placeholder for a family
    // nobody has filled in yet, which turns a log line naming the wrong
    // decoder into a log line naming no decoder.
    const std::vector<VocoderKind> kinds{
        VocoderKind::Imbe,
        VocoderKind::Codec2,
        VocoderKind::External,
    };

    std::vector<std::string> names;
    for (const VocoderKind kind : kinds) {
        const std::string name(vocoder_kind_name(kind));
        REQUIRE_FALSE(name.empty());
        REQUIRE(name != "invalid");
        names.push_back(name);
    }

    std::sort(names.begin(), names.end());
    REQUIRE(std::adjacent_find(names.begin(), names.end()) == names.end());
}

TEST_CASE("A frame description carries all four numbers", "[vocoder]")
{
    // REJECTS: a description that prints the family and the bit count only.
    // Two offers that differ solely in sample rate would then print
    // identically, and the refusal for asking for the wrong one would read as
    // though the right one had been refused.
    VocoderFrame frame;
    frame.kind = VocoderKind::Codec2;
    frame.bit_count = 37;
    frame.pcm_frames = 19;
    frame.sample_rate = 7001;

    const std::string text = describe_vocoder_frame(frame);
    REQUIRE(contains(text, "codec2"));
    REQUIRE(contains(text, "37"));
    REQUIRE(contains(text, "19"));
    REQUIRE(contains(text, "7001"));
}

TEST_CASE("The shared argument check names the number that was wrong", "[vocoder]")
{
    const VocoderFrame shape = fixture_frame();
    std::vector<float> out(kFixtureSamples, 0.0f);

    SECTION("a packed frame is refused by size")
    {
        // REJECTS: a check that accepts any bits.size() and decodes what it
        // was given. A caller that packed 12 bits into 2 bytes would then be
        // told nothing and would get two bits of audio.
        const std::vector<std::uint8_t> packed(2, 0xAA);
        const auto result = check_vocoder_call(shape, packed, out);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(contains(result.error().message, "2"));
        REQUIRE(contains(result.error().message, "12"));
    }

    SECTION("a byte that is not 0 or 1 is refused, with its index")
    {
        // REJECTS: a check that only compares sizes. A caller passing 0x00 and
        // 0xFF per bit passes a size check exactly and hands the decoder
        // values no vocoder has an interpretation for, which comes out as
        // audio rather than as a complaint.
        std::vector<std::uint8_t> bits = alternating_bits();
        bits[7] = 0xFF;
        const auto result = check_vocoder_call(shape, bits, out);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(contains(result.error().message, "255"));
        REQUIRE(contains(result.error().message, "7"));
    }

    SECTION("an output buffer one sample short is refused")
    {
        // REJECTS: a check using > instead of >=, or one that trusts the
        // caller and lets the implementation write the last sample past the
        // end.
        std::vector<float> small(kFixtureSamples - 1, 0.0f);
        const auto result = check_vocoder_call(shape, alternating_bits(), small);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(contains(result.error().message, "23"));
        REQUIRE(contains(result.error().message, "24"));
    }

    SECTION("a well formed call passes")
    {
        REQUIRE(check_vocoder_call(shape, alternating_bits(), out).has_value());
    }
}

// ---------------------------------------------------------------------------
// The directory
// ---------------------------------------------------------------------------

TEST_CASE("The default plugin directory sits beside the executable", "[vocoder][plugin]")
{
    // REJECTS: a default of the literal relative path "vocoders", which
    // resolves against the working directory. That reads correct in a
    // developer's build tree, where the two happen to coincide, and looks up
    // the wrong directory the moment the engine is started from anywhere else,
    // including from a service.
    const std::filesystem::path dir = revenant::decode::default_vocoder_plugin_directory();
    REQUIRE_FALSE(dir.empty());
    REQUIRE(dir.is_absolute());
    REQUIRE(dir.filename() == "vocoders");
}

TEST_CASE("A missing directory is reported and is not an error", "[vocoder][plugin]")
{
    const std::filesystem::path dir = case_directory("absent") / "not-created";

    const VocoderPluginSet set = scan(dir);
    INFO(set.status_line());

    // REJECTS: a scan that returns Expected and fails when the directory is
    // not there. A mode whose voice needs a dongle nobody owns still
    // demodulates, still frames and still reports metadata, and an error here
    // would take all of that down with the audio.
    REQUIRE_FALSE(set.directory_present());
    REQUIRE(set.empty());
    REQUIRE(set.reports().empty());

    // REJECTS: an empty or generic status line. The operator has to be able to
    // read which directory was looked in, because the wrong directory is the
    // commonest cause and the path is the whole answer.
    const std::string line = set.status_line();
    REQUIRE_FALSE(line.empty());
    REQUIRE(contains(line, "no vocoder"));
    REQUIRE(contains(line, dir.string()));

    const auto opened = set.open(fixture_frame());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(contains(opened.error().message, describe_vocoder_frame(fixture_frame())));
}

TEST_CASE("An empty directory reads differently from a missing one", "[vocoder][plugin]")
{
    // REJECTS: a status line that says "no vocoder" and stops. "The directory
    // is not there" sends the operator to create it; "the directory is there
    // and is empty" sends them to find the plugin they thought they installed.
    // One sentence for both is the silent failure wearing a message.
    const std::filesystem::path present = case_directory("empty");
    const std::filesystem::path absent = case_directory("empty-absent") / "not-created";

    const VocoderPluginSet with_dir = scan(present);
    const VocoderPluginSet without_dir = scan(absent);

    REQUIRE(with_dir.directory_present());
    REQUIRE_FALSE(without_dir.directory_present());
    REQUIRE(with_dir.empty());
    REQUIRE(without_dir.empty());
    REQUIRE(with_dir.status_line() != without_dir.status_line());
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

TEST_CASE("A correct plugin loads, decodes and resets", "[vocoder][plugin]")
{
    const std::filesystem::path dir = case_directory("good");
    install(dir, kTestVocoderPluginGood);

    const VocoderPluginSet set = scan(dir);
    INFO(set.status_line());

    REQUIRE(set.directory_present());
    REQUIRE(set.reports().size() == 1);
    REQUIRE(set.reports()[0].loaded);
    REQUIRE(set.reports()[0].refusal == PluginRefusal::None);
    REQUIRE(set.reports()[0].abi_version == RV_VOCODER_ABI_VERSION);
    REQUIRE(set.offer_count() == 1);

    // REJECTS: a loader that reports a plugin as present without carrying what
    // it offers. "A plugin is loaded" and "a plugin that decodes this mode is
    // loaded" are different answers and only the second one is useful.
    REQUIRE(set.offers().size() == 1);
    REQUIRE(set.offers()[0].frame == fixture_frame());
    REQUIRE(set.offers()[0].name == "fixture-a");
    REQUIRE(contains(set.status_line(), "fixture-a"));

    auto opened = set.open(fixture_frame());
    REQUIRE(opened.has_value());
    std::unique_ptr<Vocoder> vocoder = std::move(opened).value();

    REQUIRE(vocoder->shape() == fixture_frame());
    REQUIRE(contains(std::string(vocoder->name()), "fixture-a"));

    const std::vector<std::uint8_t> bits = alternating_bits();
    std::vector<float> out(kFixtureSamples, 99.0f);

    REQUIRE(vocoder->decode(bits, out).has_value());

    // REJECTS: an adapter that returns success without calling the plugin, or
    // one that passes the wrong pointer and gets whatever was in the buffer
    // back. Every sample is checked and every one is exactly representable.
    REQUIRE(out[0] == 0.0f);  // frames decoded since reset, written by the fixture
    for (std::size_t i = 1; i < out.size(); ++i) {
        INFO("sample " << i);
        REQUIRE(out[i] == ((bits[i % bits.size()] != 0) ? 0.25f : -0.25f));
    }

    REQUIRE(vocoder->decode(bits, out).has_value());
    REQUIRE(out[0] == 1.0f);

    // REJECTS: a reset() that is a no-op, or one the adapter drops on the
    // floor. Interframe state carried from the end of one transmission into
    // the start of the next is audible and is not attributable to anything.
    vocoder->reset();
    REQUIRE(vocoder->decode(bits, out).has_value());
    REQUIRE(out[0] == 0.0f);
}

TEST_CASE("A DLL missing an entry point is refused by the symbol's name", "[vocoder][plugin]")
{
    const std::filesystem::path dir = case_directory("nosym");
    install(dir, kTestVocoderPluginNoSymbol);

    const VocoderPluginSet set = scan(dir);
    INFO(set.status_line());

    // REJECTS: a loader that resolves what it finds and leaves the rest null,
    // which turns a plugin built against half the ABI into a null call at the
    // first decode, hours later, in a receiver thread.
    REQUIRE(set.reports().size() == 1);
    REQUIRE_FALSE(set.reports()[0].loaded);
    REQUIRE(set.reports()[0].refusal == PluginRefusal::MissingEntryPoint);

    // REJECTS: "the plugin is not valid". The plugin author needs the name of
    // the symbol they did not export, and it is the one piece of information
    // the loader has and they do not.
    REQUIRE(contains(set.reports()[0].detail, "revenant_vocoder_decode"));
    REQUIRE(contains(set.status_line(), "revenant_vocoder_decode"));
    REQUIRE(set.empty());
}

TEST_CASE("An ABI version mismatch is refused with both numbers", "[vocoder][plugin]")
{
    const std::filesystem::path dir = case_directory("oldabi");
    install(dir, kTestVocoderPluginOldAbi);

    const VocoderPluginSet set = scan(dir);
    INFO(set.status_line());

    // REJECTS: a loader that reads the version and carries on, which is the
    // failure the version exists to prevent. Calling into a signature that
    // changed under you is a crash in a minidump instead of a line of log.
    REQUIRE(set.reports().size() == 1);
    REQUIRE_FALSE(set.reports()[0].loaded);
    REQUIRE(set.reports()[0].refusal == PluginRefusal::AbiVersionMismatch);
    REQUIRE(set.reports()[0].abi_version == 0);
    REQUIRE(set.empty());

    // REJECTS: a message with one number in it. "Wrong ABI version" does not
    // tell the plugin author which direction to move, and the two numbers do.
    const std::string detail = set.reports()[0].detail;
    REQUIRE(contains(detail, "0"));
    REQUIRE(contains(detail, std::to_string(RV_VOCODER_ABI_VERSION)));
    REQUIRE(contains(detail, "vocoder_abi.h"));
}

TEST_CASE("An offer nobody asked for is refused with what is on hand", "[vocoder][plugin]")
{
    const std::filesystem::path dir = case_directory("mismatch");
    install(dir, kTestVocoderPluginGood);

    const VocoderPluginSet set = scan(dir);
    REQUIRE(set.offer_count() == 1);

    // One bit different from what the fixture offers. Nothing about the family
    // or the audio side changes, which is exactly the near miss a loader that
    // matched loosely would accept.
    VocoderFrame want = fixture_frame();
    want.bit_count = kFixtureBits + 1;

    const auto opened = set.open(want);

    // REJECTS: a loader that matches on VocoderKind, or on the family plus the
    // sample rate, and hands back a decoder whose frame size is wrong. That
    // produces confident audio out of misaligned bits: it sounds like a poor
    // signal, not like a configuration mistake, and listening never localises
    // it.
    REQUIRE_FALSE(opened.has_value());

    // REJECTS: "no matching vocoder". The operator needs both halves, what was
    // asked for and what is actually loaded, to see that the numbers are one
    // apart.
    const std::string message = opened.error().message;
    REQUIRE(contains(message, describe_vocoder_frame(want)));
    REQUIRE(contains(message, "fixture-a"));
    REQUIRE(contains(message, describe_vocoder_frame(fixture_frame())));
}

TEST_CASE("A plugin that enumerates and then refuses is reported as refusing",
          "[vocoder][plugin]")
{
    const std::filesystem::path dir = case_directory("declines");
    install(dir, kTestVocoderPluginRefuseCreate);

    const VocoderPluginSet set = scan(dir);
    INFO(set.status_line());

    // The plugin is fine. The device behind it is not. Those are different
    // states and the loaded flag is the one that says so.
    REQUIRE(set.reports().size() == 1);
    REQUIRE(set.reports()[0].loaded);
    REQUIRE(set.offer_count() == 1);

    const auto opened = set.open(fixture_frame());

    // REJECTS: an open() that returns a null unique_ptr on this path, or one
    // whose message is the same as the no-match message. A plugin that
    // enumerates and then declines almost always means the hardware is absent
    // or already open, and that is a different thing to go and check from a
    // frame shape nobody provides.
    REQUIRE_FALSE(opened.has_value());
    const std::string message = opened.error().message;
    REQUIRE(contains(message, "rv_test_vocoder_refuse"));
    REQUIRE(contains(message, "refused"));
}

TEST_CASE("A plugin error code becomes a sentence with the number in it", "[vocoder][plugin]")
{
    const std::filesystem::path dir = case_directory("frame-rejected");
    install(dir, kTestVocoderPluginGood);

    const VocoderPluginSet set = scan(dir);
    auto opened = set.open(fixture_frame());
    REQUIRE(opened.has_value());
    std::unique_ptr<Vocoder> vocoder = std::move(opened).value();

    // The fixture treats an all-zero frame as one its error correction could
    // not repair.
    const std::vector<std::uint8_t> dead(kFixtureBits, 0);
    std::vector<float> out(kFixtureSamples, 0.0f);

    const auto result = vocoder->decode(dead, out);

    // REJECTS: an adapter that swallows a negative return and reports success
    // with a silent buffer, and one that returns a bare failure carrying only
    // the integer. docs/conventions.md: the caller cannot report what it was
    // not told.
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == RV_VOCODER_ERR_FRAME_REJECTED);
    REQUIRE(contains(result.error().message, "-4"));
    REQUIRE(contains(result.error().message, "fixture-a"));
    REQUIRE(contains(result.error().message, "not usable"));

    // REJECTS: an adapter that treats any failure as fatal. A frame the FEC
    // could not repair is the ordinary weak-signal outcome and the next frame
    // must decode.
    REQUIRE(vocoder->decode(alternating_bits(), out).has_value());
}

TEST_CASE("A buffer too small for a frame is refused before the plugin is called",
          "[vocoder][plugin]")
{
    const std::filesystem::path dir = case_directory("short-buffer");
    install(dir, kTestVocoderPluginGood);

    const VocoderPluginSet set = scan(dir);
    auto opened = set.open(fixture_frame());
    REQUIRE(opened.has_value());
    std::unique_ptr<Vocoder> vocoder = std::move(opened).value();

    const std::vector<std::uint8_t> bits = alternating_bits();
    std::vector<float> small(kFixtureSamples - 1, 0.0f);

    const auto result = vocoder->decode(bits, small);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(contains(result.error().message, "23"));
    REQUIRE(contains(result.error().message, "24"));

    // REJECTS: an adapter that forwards the short buffer and lets the plugin
    // decide. Measured, by deleting the check from the loader and rerunning:
    // the fixture refuses the call itself and the adapter then reports "-3,
    // the plugin says the output buffer is smaller than one frame", which
    // carries neither 23 nor 24 and fails the two assertions above. A plugin
    // that did NOT check, which is the plugin this guard is really for, would
    // have written a sample past the end of a heap allocation before anything
    // could notice.
    //
    // The counter below is the second half: it confirms the refused call never
    // reached the plugin at all, rather than reaching it and being turned back.
    std::vector<float> full(kFixtureSamples, 0.0f);
    REQUIRE(vocoder->decode(bits, full).has_value());
    REQUIRE(full[0] == 0.0f);
}

TEST_CASE("A plugin that overstates what it wrote is never called again", "[vocoder][plugin]")
{
    const std::filesystem::path dir = case_directory("overrun");
    install(dir, kTestVocoderPluginOverrun);

    const VocoderPluginSet set = scan(dir);
    auto opened = set.open(fixture_frame());
    REQUIRE(opened.has_value());
    std::unique_ptr<Vocoder> vocoder = std::move(opened).value();

    const std::vector<std::uint8_t> bits = alternating_bits();
    std::vector<float> out(kFixtureSamples, 0.0f);

    const auto first = vocoder->decode(bits, out);

    // REJECTS: an adapter that checks the return code and then trusts
    // out_written. The fixture returns success, so a code-only check passes
    // this call and hands the caller a count that runs past the end of its own
    // buffer, which the caller then uses as a length.
    REQUIRE_FALSE(first.has_value());
    REQUIRE(contains(first.error().message, "32"));
    REQUIRE(contains(first.error().message, "24"));

    // REJECTS, and this clause was added after measuring that the three above
    // did not: an adapter with no capacity check at all still fails this call,
    // because 32 is not one frame either and the "reported success and wrote
    // 32 where one frame is 24" refusal carries both numbers too. Removing the
    // capacity check from the loader left this case green, which is the exact
    // shape of unverifying test this suite is meant not to contain. Naming the
    // consequence is what separates the two messages.
    REQUIRE(contains(first.error().message, "not being called again"));

    // REJECTS: an adapter that reports the fault and carries on. A module that
    // has already demonstrated it does not honour a capacity gets no further
    // chances, and reset() is not a way back: a plugin that ignores a length
    // is not a plugin that recovers.
    //
    // The sentinel is what proves it. A partial-frame refusal without
    // poisoning fails the second call as well, with an identical message, so
    // the return value alone cannot tell the two apart. Only a buffer the
    // plugin never got to touch can.
    constexpr float kSentinel = 7.0f;
    std::fill(out.begin(), out.end(), kSentinel);

    const auto second = vocoder->decode(bits, out);
    REQUIRE_FALSE(second.has_value());
    REQUIRE(second.error().message == first.error().message);
    REQUIRE(std::count(out.begin(), out.end(), kSentinel) == static_cast<std::ptrdiff_t>(out.size()));

    vocoder->reset();
    std::fill(out.begin(), out.end(), kSentinel);
    const auto third = vocoder->decode(bits, out);
    REQUIRE_FALSE(third.has_value());
    REQUIRE(third.error().message == first.error().message);
    REQUIRE(std::count(out.begin(), out.end(), kSentinel) == static_cast<std::ptrdiff_t>(out.size()));
}

TEST_CASE("One refused plugin does not hide a good one", "[vocoder][plugin]")
{
    const std::filesystem::path dir = case_directory("mixed");
    install(dir, kTestVocoderPluginGood);
    install(dir, kTestVocoderPluginOldAbi);

    const VocoderPluginSet set = scan(dir);
    INFO(set.status_line());

    // REJECTS: a scan that stops at the first refusal, and a scan that reports
    // only what loaded. Both are shapes somebody writes on the way to "it
    // works on my machine": the first loses a working dongle to a stale DLL
    // left in the directory, the second leaves the stale DLL invisible so
    // nobody removes it.
    REQUIRE(set.reports().size() == 2);
    REQUIRE(set.offer_count() == 1);
    REQUIRE(set.open(fixture_frame()).has_value());

    const std::string line = set.status_line();
    REQUIRE(contains(line, "fixture-a"));
    REQUIRE(contains(line, "refused"));
}

TEST_CASE("A vocoder keeps its module loaded after the set is gone", "[vocoder][plugin]")
{
    const std::filesystem::path dir = case_directory("lifetime");
    install(dir, kTestVocoderPluginGood);

    std::unique_ptr<Vocoder> vocoder;
    {
        const VocoderPluginSet set = scan(dir);
        auto opened = set.open(fixture_frame());
        REQUIRE(opened.has_value());
        vocoder = std::move(opened).value();
    }

    // REJECTS: a loader whose adapter borrows the module rather than sharing
    // ownership of it. FreeLibrary at the end of that scope unmaps the code
    // this call is about to enter, and the failure is an access violation in a
    // receiver thread with nothing in the stack naming the plugin. It survives
    // review because the happy path, where the set outlives everything, is the
    // path anybody testing by hand takes.
    std::vector<float> out(kFixtureSamples, 0.0f);
    REQUIRE(vocoder->decode(alternating_bits(), out).has_value());
    REQUIRE(out[0] == 0.0f);
}
