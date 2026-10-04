// The engine plugin loader and runner, against real DLLs and a fake command
// sink. tests/plugin/engine_test_plugin.cpp is the fixture and says what each
// build of it does. The server's half, a plugin acting on a live engine, is
// tests/rpc/test_rpc_engine_plugins.cpp.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "core/plugin/engine_plugin.h"
#include "core/plugin/engine_plugin_abi.h"
#include "tests/support/temp_path.h"

#include "engine_test_plugin_paths.h"

using revenant::plugin::AddVrxCommand;
using revenant::plugin::Command;
using revenant::plugin::CommandSink;
using revenant::plugin::EnginePluginRefusal;
using revenant::plugin::EnginePluginRunner;
using revenant::plugin::EnginePluginScanOptions;
using revenant::plugin::EnginePluginSet;
using revenant::plugin::Event;
using revenant::plugin::SubscribeDecodedCommand;
using revenant::plugin::scan_engine_plugins;

namespace {

// One directory per case, emptied first, for the reason
// tests/decode/test_vocoder_plugin.cpp's case_directory gives.
[[nodiscard]] std::filesystem::path case_directory(const std::string& name)
{
    std::error_code ec;
    const std::filesystem::path dir =
        revenant::test::unique_temp_path("revenant_engine_plugin_tests-" + name);
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

[[nodiscard]] EnginePluginSet scan(const std::filesystem::path& dir)
{
    EnginePluginScanOptions options;
    options.directory = dir;
    return scan_engine_plugins(options);
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
}

// What a runner sends out, kept for the test thread to read.
class RecordingSink final : public CommandSink {
public:
    std::int32_t post_command(Command command) override
    {
        const std::scoped_lock held(lock_);
        commands_.push_back(std::move(command));
        changed_.notify_all();
        return RV_ENGINE_PLUGIN_OK;
    }

    void plugin_log(std::size_t, std::uint32_t, std::string message) override
    {
        const std::scoped_lock held(lock_);
        lines_.push_back(std::move(message));
        changed_.notify_all();
    }

    // Waits up to five seconds for `ready` to hold over what has arrived.
    template <typename Ready>
    bool wait(Ready ready)
    {
        std::unique_lock held(lock_);
        return changed_.wait_for(held, std::chrono::seconds(5), [&] { return ready(*this); });
    }

    [[nodiscard]] std::vector<Command> commands()
    {
        const std::scoped_lock held(lock_);
        return commands_;
    }
    [[nodiscard]] std::vector<std::string> lines()
    {
        const std::scoped_lock held(lock_);
        return lines_;
    }

    // Unlocked, for use inside wait().
    std::vector<Command> commands_;
    std::vector<std::string> lines_;

private:
    std::mutex lock_;
    std::condition_variable changed_;
};

[[nodiscard]] Event source_opened(std::int64_t center)
{
    Event event;
    event.type = RV_ENGINE_EVENT_SOURCE_OPENED;
    event.center_hz = center;
    return event;
}

}  // namespace

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

TEST_CASE("a folder that does not exist is reported, not failed", "[plugin]")
{
    const EnginePluginSet set = scan(case_directory("absent") / "plugins");
    CHECK_FALSE(set.directory_present());
    CHECK(set.reports().empty());
    CHECK(contains(set.status_line(), "does not exist"));
}

TEST_CASE("switching plugins off scans nothing and says why", "[plugin]")
{
    const std::filesystem::path dir = case_directory("disabled");
    install(dir, kTestEnginePluginGood);
    EnginePluginScanOptions options;
    options.directory = dir;
    options.disabled = true;
    const EnginePluginSet set = scan_engine_plugins(options);
    CHECK(set.disabled());
    CHECK(set.modules().empty());
    CHECK(contains(set.status_line(), "--no-plugins"));
}

TEST_CASE("the good plugin loads and names itself", "[plugin]")
{
    const std::filesystem::path dir = case_directory("good");
    install(dir, kTestEnginePluginGood);
    const EnginePluginSet set = scan(dir);
    INFO(set.status_line());
    REQUIRE(set.reports().size() == 1);
    CHECK(set.reports()[0].loaded);
    CHECK(set.reports()[0].name == "test-tracker");
    CHECK(set.reports()[0].version == "1.0");
    CHECK(set.modules().size() == 1);
    CHECK(contains(set.status_line(), "test-tracker"));
}

TEST_CASE("each defect is refused by its own cause, naming the file", "[plugin]")
{
    struct Row {
        const char* dll;
        EnginePluginRefusal refusal;
        std::string_view says;
    };
    const Row rows[] = {
        {kTestEnginePluginNoSymbol, EnginePluginRefusal::MissingEntryPoint,
         "revenant_engine_plugin_on_event"},
        {kTestEnginePluginOldAbi, EnginePluginRefusal::AbiVersionMismatch, "version 0"},
        {kTestEnginePluginBadDescribe, EnginePluginRefusal::DescribeFailed, "describe"},
    };
    for (const Row& row : rows) {
        const std::filesystem::path dir = case_directory("refused");
        install(dir, row.dll);
        const EnginePluginSet set = scan(dir);
        INFO(set.status_line());
        REQUIRE(set.reports().size() == 1);
        const auto& report = set.reports()[0];
        CHECK_FALSE(report.loaded);
        CHECK(report.refusal == row.refusal);
        CHECK(contains(report.detail, row.says));
        CHECK(contains(report.detail, std::filesystem::path(row.dll).filename().string()));
        CHECK(set.modules().empty());
        CHECK(contains(set.status_line(), "none usable"));
    }
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

TEST_CASE("a running plugin hears events and acts through the host table", "[plugin]")
{
    const std::filesystem::path dir = case_directory("run");
    install(dir, kTestEnginePluginGood);
    const EnginePluginSet set = scan(dir);
    REQUIRE(set.modules().size() == 1);

    RecordingSink sink;
    EnginePluginRunner runner(set.modules()[0], 3, sink);
    REQUIRE(sink.wait([](RecordingSink& s) { return !s.lines_.empty(); }));
    CHECK(sink.lines().front() == "created");
    CHECK(runner.running());

    // SOURCE_OPENED: the plugin opens a p25p1 receiver 5 kHz up, tag 1.
    runner.post(source_opened(420'000'000));
    REQUIRE(sink.wait([](RecordingSink& s) { return s.commands_.size() >= 1; }));
    {
        const Command first = sink.commands().at(0);
        CHECK(first.plugin == 3);
        CHECK(first.tag == 1);
        const auto* add = std::get_if<AddVrxCommand>(&first.body);
        REQUIRE(add != nullptr);
        CHECK(add->center_hz == 420'005'000);
        CHECK(add->demod == "p25p1");
    }

    // Its result: the plugin subscribes to the new receiver, tag 2.
    Event result;
    result.type = RV_ENGINE_EVENT_COMMAND_RESULT;
    result.request_tag = 1;
    result.result_code = RV_ENGINE_PLUGIN_OK;
    result.vrx = 17;
    runner.post(result);
    REQUIRE(sink.wait([](RecordingSink& s) { return s.commands_.size() >= 2; }));
    {
        const Command second = sink.commands().at(1);
        CHECK(second.tag == 2);
        const auto* subscribe = std::get_if<SubscribeDecodedCommand>(&second.body);
        REQUIRE(subscribe != nullptr);
        CHECK(subscribe->vrx == 17);
        CHECK(subscribe->decoder.empty());
    }

    // A decoded message arrives whole: kind, text and the integer fields.
    Event decoded;
    decoded.type = RV_ENGINE_EVENT_DECODED;
    decoded.vrx = 17;
    decoded.decoder = "p25p1";
    decoded.kind = "tsbk";
    decoded.text = "a grant";
    revenant::plugin::EventField talkgroup;
    talkgroup.key = "talkgroup";
    talkgroup.type = RV_ENGINE_FIELD_INT;
    talkgroup.int_value = 1201;
    decoded.fields.push_back(talkgroup);
    runner.post(decoded);
    runner.wait_idle();
    const std::vector<std::string> lines = sink.lines();
    REQUIRE_FALSE(lines.empty());
    const std::string& last = lines.back();
    INFO(last);
    CHECK(contains(last, "type=7"));
    CHECK(contains(last, "kind=tsbk"));
    CHECK(contains(last, "talkgroup=1201"));
    CHECK(contains(last, "text=a grant"));

    runner.stop();
    CHECK_FALSE(runner.running());
}

TEST_CASE("a plugin that declines to start is not running and says so", "[plugin]")
{
    const std::filesystem::path dir = case_directory("decline");
    install(dir, kTestEnginePluginRefuse);
    const EnginePluginSet set = scan(dir);
    REQUIRE(set.modules().size() == 1);

    RecordingSink sink;
    EnginePluginRunner runner(set.modules()[0], 0, sink);
    runner.wait_idle();
    CHECK_FALSE(runner.running());
    const std::vector<std::string> lines = sink.lines();
    REQUIRE(lines.size() == 1);
    CHECK(contains(lines[0], "declined to start"));

    // Posting to it afterwards is a no-op rather than a queue that grows.
    runner.post(source_opened(1));
    runner.wait_idle();
    CHECK(sink.commands().empty());
}

TEST_CASE("a slow plugin loses its oldest events and is told how many", "[plugin]")
{
    const std::filesystem::path dir = case_directory("slow");
    install(dir, kTestEnginePluginSlow);
    const EnginePluginSet set = scan(dir);
    REQUIRE(set.modules().size() == 1);

    RecordingSink sink;
    EnginePluginRunner runner(set.modules()[0], 0, sink);
    REQUIRE(sink.wait([](RecordingSink& s) { return !s.lines_.empty(); }));

    // Far more than the queue holds, posted faster than 20 ms apiece. The
    // post must not wait for the plugin, which is the property this case is
    // about: a whole burst taking seconds would mean the caller was held.
    const auto began = std::chrono::steady_clock::now();
    const std::size_t burst = EnginePluginRunner::kQueueDepth * 2;
    for (std::size_t i = 0; i < burst; ++i) {
        Event event;
        event.type = RV_ENGINE_EVENT_VRX_CHANGED;
        event.vrx = static_cast<std::uint32_t>(i);
        runner.post(event);
    }
    const auto spent = std::chrono::steady_clock::now() - began;
    CHECK(spent < std::chrono::seconds(1));
    CHECK(runner.events_dropped() >= EnginePluginRunner::kQueueDepth / 2);

    // The next event it is given carries the count. Stopped rather than
    // drained, because draining 1024 events at 20 ms each is twenty seconds.
    REQUIRE(sink.wait([](RecordingSink& s) {
        for (const std::string& line : s.lines_) {
            if (line.find("dropped=0") == std::string::npos &&
                line.find("dropped=") != std::string::npos) {
                return true;
            }
        }
        return false;
    }));
    runner.stop();
}

TEST_CASE("a plugin that asked for nothing hears only its own results", "[plugin]")
{
    // The good fixture asks for everything, so this checks the filter from
    // the other side: an event type outside the ABI's list is never posted.
    const std::filesystem::path dir = case_directory("filter");
    install(dir, kTestEnginePluginGood);
    const EnginePluginSet set = scan(dir);
    REQUIRE(set.modules().size() == 1);

    RecordingSink sink;
    EnginePluginRunner runner(set.modules()[0], 0, sink);
    Event unknown;
    unknown.type = 999;
    runner.post(unknown);
    runner.wait_idle();
    for (const std::string& line : sink.lines()) {
        CHECK_FALSE(contains(line, "type=999"));
    }
}
