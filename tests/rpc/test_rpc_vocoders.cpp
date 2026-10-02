// Session.vocoderPlugins: what the engine process found in its vocoders folder,
// read back over the wire.
//
// The loader is held to account in tests/decode/test_vocoder_plugin.cpp. What
// these cases hold is the crossing: that an unscanned host says so rather than
// reading as an empty folder, and that a loaded plugin's offer and a refused
// plugin's reason both reach the client intact, from real DLLs.
//
// Each case names the wrong implementation it rejects.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "core/decode/vocoder_plugin.h"
#include "core/rpc/types.h"
#include "tests/rpc/rpc_fixture.h"
#include "tests/support/temp_path.h"

// Written by CMake, holding the full path of each fixture DLL. See
// tests/rpc/CMakeLists.txt.
#include "rpc_vocoder_plugin_paths.h"

namespace rpc = revenant::rpc;
namespace decode = revenant::decode;
using revenant::test::Harness;
using revenant::test::HarnessOptions;

namespace {

[[nodiscard]] std::filesystem::path fresh_directory(const std::string& name)
{
    std::error_code ec;
    const std::filesystem::path dir =
        revenant::test::unique_temp_path("revenant_rpc_vocoder_tests-" + name);
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
    std::filesystem::copy_file(source, dir / source.filename(),
                               std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);
}

[[nodiscard]] const rpc::VocoderPluginFile* find_file(const rpc::VocoderPlugins& plugins,
                                                      const char* built_dll)
{
    const std::string name = std::filesystem::path(built_dll).filename().string();
    const auto it = std::find_if(plugins.files.begin(), plugins.files.end(),
                                 [&](const rpc::VocoderPluginFile& f) { return f.file == name; });
    return it == plugins.files.end() ? nullptr : &*it;
}

}  // namespace

TEST_CASE("an engine that never scanned says so", "[rpc][vocoder]")
{
    // Rejects writing an unscanned host as a scan of an empty folder, which
    // would tell an operator their plugin is missing when nothing looked.
    Harness harness;
    REQUIRE(harness.open(HarnessOptions{}).has_value());

    const auto plugins = harness.client().vocoder_plugins();
    INFO((plugins ? std::string() : plugins.error().message));
    REQUIRE(plugins.has_value());
    CHECK_FALSE(plugins->scanned);
    CHECK(plugins->files.empty());
}

TEST_CASE("a folder that does not exist is scanned and reported absent", "[rpc][vocoder]")
{
    const std::filesystem::path dir = fresh_directory("absent") / "vocoders";
    decode::VocoderPluginScanOptions scan;
    scan.directory = dir;
    const decode::VocoderPluginSet set = decode::scan_vocoder_plugins(scan);

    HarnessOptions options;
    options.vocoders = &set;
    Harness harness;
    REQUIRE(harness.open(options).has_value());

    const auto plugins = harness.client().vocoder_plugins();
    REQUIRE(plugins.has_value());
    CHECK(plugins->scanned);
    CHECK_FALSE(plugins->directory_present);
    CHECK(plugins->files.empty());

    // The status line is the loader's own, and it is never empty.
    CHECK(plugins->status == set.status_line());
    CHECK_FALSE(plugins->status.empty());
}

TEST_CASE("a loaded plugin's offer and a refused plugin's reason both cross", "[rpc][vocoder]")
{
    const std::filesystem::path dir = fresh_directory("mixed");
    install(dir, kRpcTestVocoderGood);
    install(dir, kRpcTestVocoderOldAbi);

    decode::VocoderPluginScanOptions scan;
    scan.directory = dir;
    const decode::VocoderPluginSet set = decode::scan_vocoder_plugins(scan);
    INFO(set.status_line());

    HarnessOptions options;
    options.vocoders = &set;
    Harness harness;
    REQUIRE(harness.open(options).has_value());

    const auto plugins = harness.client().vocoder_plugins();
    REQUIRE(plugins.has_value());
    REQUIRE(plugins->scanned);
    CHECK(plugins->directory_present);
    REQUIRE(plugins->files.size() == 2);

    // Rejects a report that lists only what loaded: the refused file is the
    // one an operator is looking for.
    const rpc::VocoderPluginFile* good = find_file(*plugins, kRpcTestVocoderGood);
    const rpc::VocoderPluginFile* old = find_file(*plugins, kRpcTestVocoderOldAbi);
    REQUIRE(good != nullptr);
    REQUIRE(old != nullptr);

    CHECK(good->loaded);
    CHECK(good->refusal == "none");
    REQUIRE(good->offers.size() == 1);
    const rpc::VocoderOfferInfo& offer = good->offers.front();
    // A copy: offers() builds its vector on each call.
    const std::vector<decode::VocoderOffer> offers = set.offers();
    REQUIRE(offers.size() == 1);
    const decode::VocoderOffer& loaded = offers.front();
    CHECK(offer.name == loaded.name);
    CHECK(offer.kind == decode::vocoder_kind_name(loaded.frame.kind));
    CHECK(offer.bit_count == loaded.frame.bit_count);
    CHECK(offer.pcm_frames == loaded.frame.pcm_frames);
    CHECK(offer.sample_rate == loaded.frame.sample_rate);

    // Rejects a refusal that crosses as a bare false: the reason has to come
    // with it, by name and in the loader's words.
    CHECK_FALSE(old->loaded);
    CHECK(old->refusal == "abi-version-mismatch");
    CHECK_FALSE(old->detail.empty());
    CHECK(old->offers.empty());
}
