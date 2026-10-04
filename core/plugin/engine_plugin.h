// Loading engine plugins and running them.
//
// core/plugin/engine_plugin_abi.h is the contract a plugin implements. This
// header is the host side, in two halves.
//
// SCANNING follows core/decode/vocoder_plugin.h exactly, on its reasoning:
// scanning cannot fail, every candidate file gets a report whether it loaded
// or not, and status_line() is never empty. The folder is "plugins" beside the
// executable, and it is as trusted as the executable: LoadLibrary runs a DLL's
// entry point before any byte of it can be checked, so nothing here is a
// sandbox.
//
// RUNNING is EnginePluginRunner: one per loaded plugin, each with a thread of
// its own that makes every call into the plugin, and a bounded queue in front
// of it. Events are posted from whatever thread produced them and never wait
// for the plugin. Commands the plugin issues go out through a CommandSink,
// which the RPC server implements by queueing them for its event loop, because
// that loop is the only thread that changes the engine. Nothing in this file
// knows the server exists.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace revenant::plugin {

// One loaded DLL. Opaque so that <windows.h> stays in the .cpp.
struct EnginePluginModule;

inline constexpr std::string_view kEnginePluginDirectoryName = "plugins";

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

// Why a candidate did not become a plugin, one cause each so the operator is
// sent to the right fix: a dependency, the plugin's author, or its own
// configuration.
enum class EnginePluginRefusal : std::uint8_t {
    None,
    LoadFailed,
    MissingEntryPoint,
    AbiVersionMismatch,
    DescribeFailed,
    NotSupportedOnThisPlatform,
};

[[nodiscard]] std::string_view engine_plugin_refusal_name(EnginePluginRefusal refusal) noexcept;

struct EnginePluginReport {
    std::filesystem::path path;
    bool loaded = false;
    EnginePluginRefusal refusal = EnginePluginRefusal::None;
    std::uint32_t abi_version = 0;
    long long os_error = 0;

    // What the plugin called itself, when it got as far as describe.
    std::string name;
    std::string version;
    std::uint32_t interests = 0;

    // One sentence naming the cause and the fix. Always set.
    std::string detail;
};

struct EnginePluginScanOptions {
    // Empty means "plugins" beside the executable.
    std::optional<std::filesystem::path> directory;

    // Scan nothing and say so, for --no-plugins.
    bool disabled = false;
};

// Every module that loaded, and a report for every file that was looked at.
// Owns the modules; a runner holds its own reference, so a plugin's code stays
// mapped while its thread can still be inside it.
class EnginePluginSet {
public:
    EnginePluginSet() = default;
    ~EnginePluginSet();

    EnginePluginSet(const EnginePluginSet&) = delete;
    EnginePluginSet& operator=(const EnginePluginSet&) = delete;
    EnginePluginSet(EnginePluginSet&&) noexcept;
    EnginePluginSet& operator=(EnginePluginSet&&) noexcept;

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
    [[nodiscard]] bool directory_present() const noexcept { return directory_present_; }
    [[nodiscard]] bool disabled() const noexcept { return disabled_; }
    [[nodiscard]] std::span<const EnginePluginReport> reports() const noexcept { return reports_; }
    [[nodiscard]] std::span<const std::shared_ptr<EnginePluginModule>> modules() const noexcept
    {
        return modules_;
    }

    // The line an operator reads. Never empty.
    [[nodiscard]] std::string status_line() const;

private:
    friend EnginePluginSet scan_engine_plugins(const EnginePluginScanOptions& options);

    std::filesystem::path directory_;
    bool directory_present_ = false;
    bool disabled_ = false;
    std::vector<EnginePluginReport> reports_;
    std::vector<std::shared_ptr<EnginePluginModule>> modules_;
};

[[nodiscard]] EnginePluginSet scan_engine_plugins(const EnginePluginScanOptions& options);

[[nodiscard]] std::filesystem::path default_engine_plugin_directory();

// The file name a module was loaded from, for logs and refusals.
[[nodiscard]] std::string module_file_name(const EnginePluginModule& module);

// ---------------------------------------------------------------------------
// Events, as the host builds them
// ---------------------------------------------------------------------------

// The C++ side of rv_engine_event: owning, so it can sit in a queue, and
// turned into the borrowed C struct on the plugin's own thread just before the
// call. `type` and the code constants are the ABI's own numbers.

struct EventField {
    std::string key;
    std::uint32_t type = 0;
    std::int64_t int_value = 0;
    double double_value = 0.0;
    bool bool_value = false;
    std::string text;                 // RV_ENGINE_FIELD_TEXT
    std::vector<std::uint8_t> bytes;  // RV_ENGINE_FIELD_BYTES
};

struct EventRemoved {
    std::uint32_t vrx = 0;
    std::uint32_t cause = 0;
    std::int64_t frequency_hz = 0;
};

struct Event {
    std::uint32_t type = 0;

    std::int64_t center_hz = 0;
    std::uint64_t epoch = 0;
    std::vector<EventRemoved> removed;

    std::uint32_t vrx = 0;
    std::uint32_t owner = 0;
    std::uint32_t bandwidth_hz = 0;
    std::int64_t vrx_center_hz = 0;
    std::string demod;

    std::string decoder;
    std::string kind;
    std::uint64_t start_sample = 0;
    std::uint64_t end_sample = 0;
    std::uint64_t sequence = 0;
    std::uint64_t dropped_before = 0;
    std::uint32_t sample_rate = 0;
    std::vector<EventField> fields;

    std::uint32_t request_tag = 0;
    std::int32_t result_code = 0;

    std::string text;
};

// ---------------------------------------------------------------------------
// Commands, as a plugin issues them
// ---------------------------------------------------------------------------

struct AddVrxCommand {
    std::int64_t center_hz = 0;
    std::uint32_t bandwidth_hz = 0;
    std::string demod;
};
struct RemoveVrxCommand {
    std::uint32_t vrx = 0;
};
struct SetVrxCenterCommand {
    std::uint32_t vrx = 0;
    std::int64_t center_hz = 0;
};
struct SetSourceCenterCommand {
    std::int64_t center_hz = 0;
};
struct SubscribeDecodedCommand {
    std::uint32_t vrx = 0;
    std::string decoder;
};
struct UnsubscribeDecodedCommand {
    std::uint32_t vrx = 0;
    std::string decoder;
};

using CommandBody = std::variant<AddVrxCommand, RemoveVrxCommand, SetVrxCenterCommand,
                                 SetSourceCenterCommand, SubscribeDecodedCommand,
                                 UnsubscribeDecodedCommand>;

struct Command {
    // Which runner it came from, as the sink was told in attach order.
    std::size_t plugin = 0;
    std::uint32_t tag = 0;
    CommandBody body;
};

// Where a runner sends what its plugin asks for. Called from any thread,
// including the plugin's own and any thread it started; must not block on the
// engine and must not call back into the runner. Returns an ABI code: OK when
// queued, ERR_QUEUE_FULL or ERR_STOPPED otherwise.
class CommandSink {
public:
    virtual ~CommandSink() = default;
    virtual std::int32_t post_command(Command command) = 0;
    virtual void plugin_log(std::size_t plugin, std::uint32_t level, std::string message) = 0;
};

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

// One plugin, live. Constructed on any thread; create() runs on the runner's
// own thread, so the constructor returns before the plugin has necessarily
// started. stop() is idempotent and the destructor runs it.
class EnginePluginRunner {
public:
    // How many events wait for a slow plugin before the oldest go.
    static constexpr std::size_t kQueueDepth = 1024;

    EnginePluginRunner(std::shared_ptr<EnginePluginModule> module, std::size_t index,
                       CommandSink& sink);
    ~EnginePluginRunner();

    EnginePluginRunner(const EnginePluginRunner&) = delete;
    EnginePluginRunner& operator=(const EnginePluginRunner&) = delete;

    // Any thread. Never blocks on the plugin. Dropped when the plugin did not
    // ask for this kind of event, or has stopped.
    void post(Event event);

    // Calls destroy on the plugin's thread and joins it. Requests the plugin
    // makes from here on are refused with ERR_STOPPED.
    void stop();

    [[nodiscard]] std::uint32_t interests() const noexcept { return interests_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] std::size_t index() const noexcept { return index_; }

    // Whether create returned a handle. False until the thread has run it, and
    // false for ever after a plugin that declined.
    [[nodiscard]] bool running() const;

    // Events this runner's queue has lost, in total.
    [[nodiscard]] std::uint64_t events_dropped() const;

    // Blocks until everything posted so far has been delivered. For tests.
    void wait_idle();

    // The C host table's entry points reach the runner through this; public
    // only so the trampolines in the .cpp can name it.
    struct Host;

private:
    void run();

    std::shared_ptr<EnginePluginModule> module_;
    const std::size_t index_;
    CommandSink& sink_;
    std::uint32_t interests_ = 0;
    std::string name_;

    std::unique_ptr<Host> host_;

    mutable std::mutex lock_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<Event> queue_;
    std::uint64_t dropped_pending_ = 0;
    std::uint64_t dropped_total_ = 0;
    bool stopping_ = false;
    bool started_ = false;
    bool running_ = false;
    bool busy_ = false;

    std::thread thread_;
};

}  // namespace revenant::plugin
