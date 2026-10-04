// The host side of the engine plugin ABI.
//
// The loader is core/decode/vocoder_plugin.cpp's shape on purpose, and its
// three rules carry over: every refusal is a sentence the host writes from
// what it already knows, nothing the plugin returns is trusted to be
// terminated, and the module stays mapped for as long as anything can still
// call into it.
//
// What is new is the runner. A vocoder is called on a decode lane, inline,
// because it has to be: its output is the audio. A plugin's output is
// commands, which can wait, so a plugin gets a thread of its own and a queue
// in front of it, and the threads that produce events only ever append to that
// queue. A plugin that hangs in on_event costs its own events and nobody
// else's.

#include "core/plugin/engine_plugin.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <utility>

#include "core/plugin/dll_scan.h"
#include "core/plugin/engine_plugin_abi.h"
#include "core/thread_role.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace revenant::plugin {

// The layouts are the ABI. A change to any of them without
// RV_ENGINE_PLUGIN_ABI_VERSION moving with it is a silent incompatibility, and
// these are where that becomes impossible. x64 only, which is the only target
// this tree builds.
static_assert(sizeof(void*) != 8 || sizeof(rv_engine_plugin_desc) == 104,
              "rv_engine_plugin_desc is part of the plugin ABI");
static_assert(sizeof(void*) != 8 || sizeof(rv_engine_removed) == 16,
              "rv_engine_removed is part of the plugin ABI");
static_assert(sizeof(void*) != 8 || sizeof(rv_engine_field) == 48,
              "rv_engine_field is part of the plugin ABI");
static_assert(sizeof(void*) != 8 || sizeof(rv_engine_event) == 152,
              "rv_engine_event is part of the plugin ABI");
static_assert(sizeof(void*) != 8 || sizeof(rv_engine_host) == 72,
              "rv_engine_host is part of the plugin ABI");

// ---------------------------------------------------------------------------
// The loaded module
// ---------------------------------------------------------------------------

struct EnginePluginModule {
    std::filesystem::path path;

#ifdef _WIN32
    HMODULE handle = nullptr;
#endif

    rv_engine_plugin_describe_fn describe = nullptr;
    rv_engine_plugin_create_fn create = nullptr;
    rv_engine_plugin_on_event_fn on_event = nullptr;
    rv_engine_plugin_destroy_fn destroy = nullptr;

    std::string name;
    std::string version;
    std::uint32_t interests = 0;

    EnginePluginModule() = default;
    EnginePluginModule(const EnginePluginModule&) = delete;
    EnginePluginModule& operator=(const EnginePluginModule&) = delete;

    ~EnginePluginModule()
    {
#ifdef _WIN32
        if (handle != nullptr) {
            FreeLibrary(handle);
        }
#endif
    }
};

std::string module_file_name(const EnginePluginModule& module)
{
    return module.path.filename().string();
}

namespace {

[[nodiscard]] std::string terminated(const char* first, std::size_t capacity)
{
    const char* const last = first + capacity;
    return std::string(first, std::find(first, last, '\0'));
}

#ifdef _WIN32

struct EntryPoint {
    const char* name;
    void** slot;
};

[[nodiscard]] EnginePluginReport load_one(const std::filesystem::path& path,
                                          std::shared_ptr<EnginePluginModule>& out_module)
{
    EnginePluginReport report;
    report.path = path;
    const std::string file = path.filename().string();

    // The search path pinned to the plugin's own directory and the system's,
    // for vocoder_plugin.cpp's reason: not the working directory.
    const HMODULE handle = LoadLibraryExW(
        path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (handle == nullptr) {
        report.refusal = EnginePluginRefusal::LoadFailed;
        report.os_error = static_cast<long long>(GetLastError());
        report.detail = std::format(
            "{} did not load: Windows error {}, {}. The usual cause is a dependency of the "
            "plugin that is not beside it; put its DLLs in the same directory",
            file, report.os_error, os_error_text(report.os_error));
        return report;
    }

    auto module = std::make_shared<EnginePluginModule>();
    module->path = path;
    module->handle = handle;

    rv_engine_plugin_abi_version_fn version_fn = nullptr;
    const EntryPoint entries[] = {
        {"revenant_engine_plugin_abi_version", reinterpret_cast<void**>(&version_fn)},
        {"revenant_engine_plugin_describe", reinterpret_cast<void**>(&module->describe)},
        {"revenant_engine_plugin_create", reinterpret_cast<void**>(&module->create)},
        {"revenant_engine_plugin_on_event", reinterpret_cast<void**>(&module->on_event)},
        {"revenant_engine_plugin_destroy", reinterpret_cast<void**>(&module->destroy)},
    };

    for (const EntryPoint& entry : entries) {
        FARPROC symbol = GetProcAddress(handle, entry.name);
        if (symbol == nullptr) {
            report.refusal = EnginePluginRefusal::MissingEntryPoint;
            report.os_error = static_cast<long long>(GetLastError());
            report.detail = std::format(
                "{} does not export {}, so it is not an engine plugin of ABI version {}; a "
                "plugin must export all {} entry points in core/plugin/engine_plugin_abi.h",
                file, entry.name, RV_ENGINE_PLUGIN_ABI_VERSION, std::size(entries));
            return report;
        }
        *entry.slot = reinterpret_cast<void*>(symbol);
    }

    report.abi_version = version_fn();
    if (report.abi_version != RV_ENGINE_PLUGIN_ABI_VERSION) {
        report.refusal = EnginePluginRefusal::AbiVersionMismatch;
        report.detail = std::format(
            "{} reports engine plugin ABI version {} and this build speaks {}, so it was not "
            "loaded; rebuild the plugin against core/plugin/engine_plugin_abi.h from this "
            "version of Revenant",
            file, report.abi_version, RV_ENGINE_PLUGIN_ABI_VERSION);
        return report;
    }

    rv_engine_plugin_desc desc{};
    desc.struct_size = static_cast<std::uint32_t>(sizeof(rv_engine_plugin_desc));
    const std::int32_t code = module->describe(&desc);
    if (code != RV_ENGINE_PLUGIN_OK) {
        report.refusal = EnginePluginRefusal::DescribeFailed;
        report.detail = std::format(
            "{} returned {} from describe. This build passes a {}-byte rv_engine_plugin_desc; a "
            "plugin built against an edited copy of the header refuses it even when the ABI "
            "version matches",
            file, code, sizeof(rv_engine_plugin_desc));
        return report;
    }

    module->name = terminated(desc.name, RV_ENGINE_PLUGIN_NAME_CAPACITY);
    module->version = terminated(desc.version, RV_ENGINE_PLUGIN_VERSION_CAPACITY);
    module->interests = desc.interests;
    if (module->name.empty()) {
        module->name = path.stem().string();
    }

    report.name = module->name;
    report.version = module->version;
    report.interests = module->interests;
    report.loaded = true;
    report.detail = std::format("{} loaded from {}, ABI {}{}", module->name, file,
                                report.abi_version,
                                module->version.empty() ? std::string()
                                                        : std::format(", version {}",
                                                                      module->version));
    out_module = std::move(module);
    return report;
}

#endif  // _WIN32

}  // namespace

std::string_view engine_plugin_refusal_name(EnginePluginRefusal refusal) noexcept
{
    switch (refusal) {
        case EnginePluginRefusal::None:
            return "none";
        case EnginePluginRefusal::LoadFailed:
            return "load-failed";
        case EnginePluginRefusal::MissingEntryPoint:
            return "missing-entry-point";
        case EnginePluginRefusal::AbiVersionMismatch:
            return "abi-version-mismatch";
        case EnginePluginRefusal::DescribeFailed:
            return "describe-failed";
        case EnginePluginRefusal::NotSupportedOnThisPlatform:
            return "not-supported-on-this-platform";
    }
    return "invalid";
}

// ---------------------------------------------------------------------------
// EnginePluginSet
// ---------------------------------------------------------------------------

EnginePluginSet::~EnginePluginSet() = default;
EnginePluginSet::EnginePluginSet(EnginePluginSet&&) noexcept = default;
EnginePluginSet& EnginePluginSet::operator=(EnginePluginSet&&) noexcept = default;

std::string EnginePluginSet::status_line() const
{
    const std::string where = directory_.empty() ? std::string("<unknown>") : directory_.string();

    if (disabled_) {
        return "no engine plugins: loading them was switched off with --no-plugins";
    }
    if (!directory_present_) {
        return std::format(
            "no engine plugins: {} does not exist. Create it beside the executable and put an "
            "engine plugin DLL in it",
            where);
    }
    if (reports_.empty()) {
        return std::format("no engine plugins: {} exists and holds no .dll", where);
    }
    if (modules_.empty()) {
        std::string refusals;
        for (const EnginePluginReport& report : reports_) {
            if (!refusals.empty()) {
                refusals += "; ";
            }
            refusals += report.detail;
        }
        return std::format("no engine plugins: {} file(s) in {}, none usable. {}",
                           reports_.size(), where, refusals);
    }

    std::string loaded;
    for (const EnginePluginReport& report : reports_) {
        if (!report.loaded) {
            continue;
        }
        if (!loaded.empty()) {
            loaded += "; ";
        }
        loaded += std::format("{} from {}", report.name, report.path.filename().string());
    }
    std::string line = std::format("{} engine plugin(s) from {}: {}", modules_.size(), where,
                                   loaded);
    if (const std::size_t refused = reports_.size() - modules_.size(); refused != 0) {
        line += std::format(". {} file(s) in the same directory were refused", refused);
    }
    return line;
}

std::filesystem::path default_engine_plugin_directory()
{
    return directory_beside_executable(kEnginePluginDirectoryName);
}

EnginePluginSet scan_engine_plugins(const EnginePluginScanOptions& options)
{
    EnginePluginSet set;
    set.directory_ = options.directory.value_or(default_engine_plugin_directory());
    if (options.disabled) {
        set.disabled_ = true;
        return set;
    }

#ifndef _WIN32
    EnginePluginReport unsupported;
    unsupported.path = set.directory_;
    unsupported.refusal = EnginePluginRefusal::NotSupportedOnThisPlatform;
    unsupported.detail =
        "engine plugins are loaded with LoadLibrary and this build is not for Windows; the "
        "dlopen path is not written yet, so no plugin can be loaded here";
    set.reports_.push_back(std::move(unsupported));
    return set;
#else
    std::error_code ec;
    if (set.directory_.empty() || !std::filesystem::is_directory(set.directory_, ec)) {
        return set;
    }
    const std::filesystem::path root = std::filesystem::absolute(set.directory_, ec);
    if (ec) {
        return set;
    }
    set.directory_ = root;
    set.directory_present_ = true;

    for (const std::filesystem::path& candidate : dll_candidates(root)) {
        std::shared_ptr<EnginePluginModule> module;
        EnginePluginReport report = load_one(candidate, module);
        if (module) {
            set.modules_.push_back(std::move(module));
        }
        set.reports_.push_back(std::move(report));
    }
    return set;
#endif
}

// ---------------------------------------------------------------------------
// The host table
// ---------------------------------------------------------------------------

// What rv_engine_host::ctx points at. The table lives inside it so one
// allocation carries both and the address the plugin holds stays put.
struct EnginePluginRunner::Host {
    rv_engine_host table{};
    EnginePluginRunner* runner = nullptr;

    // Separate from the runner's queue lock, because a plugin calls these
    // from inside on_event, which runs with that lock released, and from
    // threads of its own, which may race stop().
    std::mutex lock;
    bool accepting = true;

    [[nodiscard]] std::int32_t post(std::uint32_t tag, CommandBody body)
    {
        const std::scoped_lock held(lock);
        if (!accepting) {
            return RV_ENGINE_PLUGIN_ERR_STOPPED;
        }
        return runner->sink_.post_command(
            Command{.plugin = runner->index_, .tag = tag, .body = std::move(body)});
    }

    void log(std::uint32_t level, std::string message)
    {
        const std::scoped_lock held(lock);
        if (accepting) {
            runner->sink_.plugin_log(runner->index_, level, std::move(message));
        }
    }
};

namespace {

using Host = EnginePluginRunner::Host;

[[nodiscard]] Host* host_of(void* ctx) noexcept
{
    return static_cast<Host*>(ctx);
}

// Every trampoline catches, because the plugin's frame is above it and an
// exception unwinding into a foreign runtime is the one thing the ABI forbids
// in both directions. A host that cannot allocate a command says so as a code.
std::int32_t RV_ENGINE_PLUGIN_CALL host_add_vrx(void* ctx, std::int64_t center_hz,
                                                std::uint32_t bandwidth_hz, const char* demod,
                                                std::uint32_t tag)
{
    if (ctx == nullptr || demod == nullptr) {
        return RV_ENGINE_PLUGIN_ERR_ARGUMENT;
    }
    try {
        return host_of(ctx)->post(
            tag, AddVrxCommand{.center_hz = center_hz, .bandwidth_hz = bandwidth_hz,
                               .demod = demod});
    } catch (...) {
        return RV_ENGINE_PLUGIN_ERR_INTERNAL;
    }
}

std::int32_t RV_ENGINE_PLUGIN_CALL host_remove_vrx(void* ctx, std::uint32_t vrx,
                                                   std::uint32_t tag)
{
    if (ctx == nullptr) {
        return RV_ENGINE_PLUGIN_ERR_ARGUMENT;
    }
    try {
        return host_of(ctx)->post(tag, RemoveVrxCommand{.vrx = vrx});
    } catch (...) {
        return RV_ENGINE_PLUGIN_ERR_INTERNAL;
    }
}

std::int32_t RV_ENGINE_PLUGIN_CALL host_set_vrx_center(void* ctx, std::uint32_t vrx,
                                                       std::int64_t center_hz, std::uint32_t tag)
{
    if (ctx == nullptr) {
        return RV_ENGINE_PLUGIN_ERR_ARGUMENT;
    }
    try {
        return host_of(ctx)->post(tag, SetVrxCenterCommand{.vrx = vrx, .center_hz = center_hz});
    } catch (...) {
        return RV_ENGINE_PLUGIN_ERR_INTERNAL;
    }
}

std::int32_t RV_ENGINE_PLUGIN_CALL host_set_source_center(void* ctx, std::int64_t center_hz,
                                                          std::uint32_t tag)
{
    if (ctx == nullptr) {
        return RV_ENGINE_PLUGIN_ERR_ARGUMENT;
    }
    try {
        return host_of(ctx)->post(tag, SetSourceCenterCommand{.center_hz = center_hz});
    } catch (...) {
        return RV_ENGINE_PLUGIN_ERR_INTERNAL;
    }
}

std::int32_t RV_ENGINE_PLUGIN_CALL host_subscribe_decoded(void* ctx, std::uint32_t vrx,
                                                          const char* decoder, std::uint32_t tag)
{
    if (ctx == nullptr || decoder == nullptr) {
        return RV_ENGINE_PLUGIN_ERR_ARGUMENT;
    }
    try {
        return host_of(ctx)->post(tag, SubscribeDecodedCommand{.vrx = vrx, .decoder = decoder});
    } catch (...) {
        return RV_ENGINE_PLUGIN_ERR_INTERNAL;
    }
}

std::int32_t RV_ENGINE_PLUGIN_CALL host_unsubscribe_decoded(void* ctx, std::uint32_t vrx,
                                                            const char* decoder,
                                                            std::uint32_t tag)
{
    if (ctx == nullptr || decoder == nullptr) {
        return RV_ENGINE_PLUGIN_ERR_ARGUMENT;
    }
    try {
        return host_of(ctx)->post(tag,
                                  UnsubscribeDecodedCommand{.vrx = vrx, .decoder = decoder});
    } catch (...) {
        return RV_ENGINE_PLUGIN_ERR_INTERNAL;
    }
}

void RV_ENGINE_PLUGIN_CALL host_log(void* ctx, std::uint32_t level, const char* message)
{
    if (ctx == nullptr || message == nullptr) {
        return;
    }
    try {
        host_of(ctx)->log(level, std::string(message));
    } catch (...) {
        // A log line that could not be allocated is not worth a plugin's
        // process.
    }
}

// Whether a plugin that set `interests` hears an event of `type`. Results and
// the end of its own subscriptions always, because the plugin asked for those
// by acting.
[[nodiscard]] bool wanted(std::uint32_t interests, std::uint32_t type) noexcept
{
    switch (type) {
        case RV_ENGINE_EVENT_SOURCE_OPENED:
        case RV_ENGINE_EVENT_SOURCE_CLOSED:
        case RV_ENGINE_EVENT_SOURCE_RETUNED:
            return (interests & RV_ENGINE_INTEREST_SOURCE) != 0;
        case RV_ENGINE_EVENT_VRX_ADDED:
        case RV_ENGINE_EVENT_VRX_REMOVED:
        case RV_ENGINE_EVENT_VRX_CHANGED:
            return (interests & RV_ENGINE_INTEREST_VRX) != 0;
        case RV_ENGINE_EVENT_DECODED:
            return (interests & RV_ENGINE_INTEREST_DECODED) != 0;
        case RV_ENGINE_EVENT_DECODED_ENDED:
        case RV_ENGINE_EVENT_COMMAND_RESULT:
            return true;
        default:
            return false;
    }
}

// The borrowed view of one Event, built on the plugin's thread and alive for
// exactly one on_event call.
struct EventView {
    rv_engine_event event{};
    std::vector<rv_engine_removed> removed;
    std::vector<rv_engine_field> fields;

    EventView(const Event& in, std::uint64_t dropped)
    {
        event.struct_size = static_cast<std::uint32_t>(sizeof(rv_engine_event));
        event.type = in.type;
        event.events_dropped_before = dropped;
        event.center_hz = in.center_hz;
        event.epoch = in.epoch;

        removed.reserve(in.removed.size());
        for (const EventRemoved& gone : in.removed) {
            removed.push_back(rv_engine_removed{
                .vrx = gone.vrx, .cause = gone.cause, .frequency_hz = gone.frequency_hz});
        }
        event.removed = removed.empty() ? nullptr : removed.data();
        event.removed_count = static_cast<std::uint32_t>(removed.size());

        event.vrx = in.vrx;
        event.owner = in.owner;
        event.bandwidth_hz = in.bandwidth_hz;
        event.vrx_center_hz = in.vrx_center_hz;
        event.demod = in.demod.c_str();

        event.decoder = in.decoder.c_str();
        event.kind = in.kind.c_str();
        event.start_sample = in.start_sample;
        event.end_sample = in.end_sample;
        event.sequence = in.sequence;
        event.dropped_before = in.dropped_before;
        event.sample_rate = in.sample_rate;

        fields.reserve(in.fields.size());
        for (const EventField& field : in.fields) {
            rv_engine_field out{};
            out.key = field.key.c_str();
            out.type = field.type;
            out.int_value = field.int_value;
            out.double_value = field.double_value;
            out.bool_value = field.bool_value ? 1u : 0u;
            if (field.type == RV_ENGINE_FIELD_TEXT) {
                out.data = reinterpret_cast<const std::uint8_t*>(field.text.c_str());
                out.size = static_cast<std::uint32_t>(field.text.size());
            } else if (field.type == RV_ENGINE_FIELD_BYTES && !field.bytes.empty()) {
                out.data = field.bytes.data();
                out.size = static_cast<std::uint32_t>(field.bytes.size());
            }
            fields.push_back(out);
        }
        event.fields = fields.empty() ? nullptr : fields.data();
        event.field_count = static_cast<std::uint32_t>(fields.size());

        event.request_tag = in.request_tag;
        event.result_code = in.result_code;
        event.text = in.text.c_str();
    }

    EventView(const EventView&) = delete;
    EventView& operator=(const EventView&) = delete;
};

}  // namespace

// ---------------------------------------------------------------------------
// EnginePluginRunner
// ---------------------------------------------------------------------------

EnginePluginRunner::EnginePluginRunner(std::shared_ptr<EnginePluginModule> module,
                                       std::size_t index, CommandSink& sink)
    : module_(std::move(module)), index_(index), sink_(sink),
      interests_(module_->interests), name_(module_->name), host_(std::make_unique<Host>())
{
    host_->runner = this;
    host_->table.struct_size = static_cast<std::uint32_t>(sizeof(rv_engine_host));
    host_->table.ctx = host_.get();
    host_->table.add_vrx = host_add_vrx;
    host_->table.remove_vrx = host_remove_vrx;
    host_->table.set_vrx_center = host_set_vrx_center;
    host_->table.set_source_center = host_set_source_center;
    host_->table.subscribe_decoded = host_subscribe_decoded;
    host_->table.unsubscribe_decoded = host_unsubscribe_decoded;
    host_->table.log = host_log;

    thread_ = std::thread([this] { run(); });
}

EnginePluginRunner::~EnginePluginRunner()
{
    stop();
}

void EnginePluginRunner::post(Event event)
{
    if (!wanted(interests_, event.type)) {
        return;
    }
    {
        const std::scoped_lock held(lock_);
        if (stopping_) {
            return;
        }
        // From the front: the oldest event is the one a plugin that fell
        // behind is least likely still to want, and front eviction is what
        // makes events_dropped_before exact.
        while (queue_.size() >= kQueueDepth) {
            queue_.pop_front();
            ++dropped_pending_;
            ++dropped_total_;
        }
        queue_.push_back(std::move(event));
    }
    wake_.notify_one();
}

void EnginePluginRunner::stop()
{
    {
        const std::scoped_lock held(lock_);
        if (stopping_ && !thread_.joinable()) {
            return;
        }
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool EnginePluginRunner::running() const
{
    const std::scoped_lock held(lock_);
    return running_;
}

std::uint64_t EnginePluginRunner::events_dropped() const
{
    const std::scoped_lock held(lock_);
    return dropped_total_;
}

void EnginePluginRunner::wait_idle()
{
    std::unique_lock held(lock_);
    idle_.wait(held, [this] { return stopping_ || (started_ && queue_.empty() && !busy_); });
}

void EnginePluginRunner::run()
{
    describe_this_thread(L"revenant engine plugin", ThreadClass::SigId);

    rv_engine_plugin* const self = module_->create(&host_->table);
    {
        const std::scoped_lock held(lock_);
        started_ = true;
        running_ = self != nullptr;
    }
    if (self == nullptr) {
        sink_.plugin_log(index_, RV_ENGINE_LOG_WARN,
                         std::format("{} declined to start: create returned NULL",
                                     module_file_name(*module_)));
        {
            const std::scoped_lock held(host_->lock);
            host_->accepting = false;
        }
        const std::scoped_lock held(lock_);
        stopping_ = true;
        queue_.clear();
        idle_.notify_all();
        return;
    }

    for (;;) {
        Event next;
        std::uint64_t dropped = 0;
        {
            std::unique_lock held(lock_);
            busy_ = false;
            idle_.notify_all();
            wake_.wait(held, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                break;
            }
            next = std::move(queue_.front());
            queue_.pop_front();
            dropped = std::exchange(dropped_pending_, 0);
            busy_ = true;
        }
        const EventView view(next, dropped);
        module_->on_event(self, &view.event);
    }

    // Refused from here on, so a command the plugin sends while it tears down
    // gets ERR_STOPPED rather than reaching a server that is going away.
    {
        const std::scoped_lock held(host_->lock);
        host_->accepting = false;
    }
    module_->destroy(self);

    const std::scoped_lock held(lock_);
    running_ = false;
    busy_ = false;
    queue_.clear();
    idle_.notify_all();
}

}  // namespace revenant::plugin
