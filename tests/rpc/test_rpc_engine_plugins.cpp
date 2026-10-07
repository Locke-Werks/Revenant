// Engine plugins on a live server: the listing Session.enginePlugins gives,
// and a plugin acting on the engine through the host table and hearing what
// the engine does. tests/plugin/engine_test_plugin.cpp is the plugin, and its
// header says what the good build does on each event; the loader and runner
// alone are tests/plugin/test_engine_plugin.cpp.
//
// The plugin reports through its log, one line per event, which the harness
// hands to PluginLog below. Lines are matched by substring, so each check
// names the fields it cares about and nothing else.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/decode/p25p1.h"
#include "core/dsp/synth/dv_mod.h"
#include "core/dsp/types.h"
#include "core/plugin/engine_plugin.h"
#include "core/plugin/engine_plugin_abi.h"
#include "core/rpc/client.h"
#include "core/rpc/decoders.h"
#include "core/rpc/types.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"
#include "tests/support/temp_path.h"

#include "rpc_engine_plugin_paths.h"

using namespace revenant;
using test::Harness;
using test::HarnessOptions;

namespace {

// ---------------------------------------------------------------------------
// The plugin's log
// ---------------------------------------------------------------------------

class PluginLog {
public:
    [[nodiscard]] std::function<void(std::string_view)> sink() {
        return [this](std::string_view line) {
            const std::scoped_lock held(lock_);
            lines_.emplace_back(line);
        };
    }

    [[nodiscard]] std::vector<std::string> lines() const {
        const std::scoped_lock held(lock_);
        return lines_;
    }

    // The first line holding every needle, waiting up to `timeout_ms`.
    [[nodiscard]] std::string find(std::initializer_list<std::string_view> needles,
                                   int timeout_ms = 10'000) const {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            for (const std::string& line : lines()) {
                if (std::ranges::all_of(needles, [&](std::string_view needle) {
                        return line.find(needle) != std::string::npos;
                    })) {
                    return line;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return {};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    [[nodiscard]] std::string dump() const {
        std::string all;
        for (const std::string& line : lines()) {
            all += "\n  " + line;
        }
        return all;
    }

private:
    mutable std::mutex lock_;
    std::vector<std::string> lines_;
};

[[nodiscard]] std::filesystem::path plugin_directory(std::string_view name,
                                                     std::initializer_list<const char*> dlls) {
    std::error_code ec;
    const std::filesystem::path dir =
        test::unique_temp_path(std::format("revenant_rpc_engine_plugins-{}", name));
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    for (const char* dll : dlls) {
        const std::filesystem::path source(dll);
        REQUIRE(std::filesystem::exists(source));
        std::filesystem::copy_file(source, dir / source.filename(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        REQUIRE_FALSE(ec);
    }
    return dir;
}

[[nodiscard]] plugin::EnginePluginSet scan(const std::filesystem::path& dir) {
    plugin::EnginePluginScanOptions options;
    options.directory = dir;
    return plugin::scan_engine_plugins(options);
}

// ---------------------------------------------------------------------------
// A P25 capture, on tests/rpc/test_rpc_decode.cpp's terms
// ---------------------------------------------------------------------------

// The same transmission and file test_rpc_decode.cpp's "a P25 header crosses
// the wire" case runs, cut down to what a plugin needs: clear headers on
// one talkgroup, 5 kHz above the file's centre, which is where the fixture
// plugin opens its receiver.
constexpr dsp::SampleRate kFileRate = 288'000;
constexpr dsp::Hertz kCarrierHz = 5'000;
constexpr std::int64_t kFileCenterHz = 420'000'000;
constexpr double kQuietSeconds = 0.5;
constexpr std::uint16_t kNac = 0x293;
constexpr std::uint16_t kTalkgroup = 0x02A7;
constexpr std::uint32_t kGridChannels = 4;
constexpr std::uint32_t kBlockSamples = 16'384;

[[nodiscard]] Expected<std::vector<dsp::Complex32>> p25_transmission() {
    std::vector<std::uint8_t> dibits;
    for (int round = 0; round < 4; ++round) {
        siggen::P25HeaderMessage message;
        message.network_access_code = kNac;
        message.header.manufacturer_id = 0x00;
        message.header.algorithm_id = decode::kP25AlgidUnencrypted;
        message.header.talkgroup_id = kTalkgroup;
        auto one = siggen::p25_header_message_dibits(message);
        if (!one) {
            return std::unexpected(one.error());
        }
        dibits.insert(dibits.end(), one->begin(), one->end());
    }
    siggen::P25ModConfig mod;
    mod.filter_taps = rpc::decoders_detail::scaled_taps(mod.filter_taps, mod.rate, kFileRate);
    mod.rate = kFileRate;
    return siggen::p25_render_dibits(mod, dibits);
}

class CaptureFile {
public:
    CaptureFile()
        : path_(test::unique_temp_path("revenant-engine-plugin-p25", ".cf32")) {}
    CaptureFile(const CaptureFile&) = delete;
    CaptureFile& operator=(const CaptureFile&) = delete;
    ~CaptureFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] Status write(std::span<const dsp::Complex32> signal) {
        const auto quiet =
            static_cast<std::size_t>(kQuietSeconds * static_cast<double>(kFileRate));
        std::vector<dsp::Complex32> all(quiet, dsp::Complex32{});
        for (std::size_t n = 0; n < signal.size(); ++n) {
            const std::int64_t turns = (kCarrierHz * static_cast<std::int64_t>(n)) % kFileRate;
            const double angle = 2.0 * std::numbers::pi * static_cast<double>(turns) /
                                 static_cast<double>(kFileRate);
            const std::complex<double> moved =
                std::complex<double>(signal[n]) * std::polar(1.0, angle);
            all.emplace_back(static_cast<float>(moved.real()), static_cast<float>(moved.imag()));
        }
        all.insert(all.end(), quiet, dsp::Complex32{});
        samples_ = all.size();
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(all.data()),
                  static_cast<std::streamsize>(all.size() * sizeof(dsp::Complex32)));
        if (!out) {
            return fail(std::format("could not write {}", path_.string()));
        }
        return {};
    }

    [[nodiscard]] std::string uri() const {
        return std::format("file:///{}?rate={}&format=cf32&center={}", path_.generic_string(),
                           kFileRate, kFileCenterHz);
    }
    [[nodiscard]] dsp::SampleIndex samples() const { return samples_; }

private:
    std::filesystem::path path_;
    dsp::SampleIndex samples_ = 0;
};

[[nodiscard]] std::string event_type(std::uint32_t type) {
    return std::format("type={} ", type);
}

}  // namespace

// ---------------------------------------------------------------------------
// The listing
// ---------------------------------------------------------------------------

TEST_CASE("an engine that never scanned for plugins says so", "[rpc][plugin]") {
    Harness harness;
    REQUIRE(harness.open(HarnessOptions{}).has_value());
    const auto plugins = harness.client().engine_plugins();
    INFO((plugins ? std::string() : plugins.error().message));
    REQUIRE(plugins.has_value());
    CHECK_FALSE(plugins->scanned);
    CHECK(plugins->files.empty());
}

TEST_CASE("a running plugin and a refused one both cross", "[rpc][plugin]") {
    const auto dir = plugin_directory("listing", {kRpcTestEnginePluginGood,
                                                  kRpcTestEnginePluginOldAbi});
    const plugin::EnginePluginSet set = scan(dir);
    INFO(set.status_line());

    PluginLog log;
    HarnessOptions options;
    options.engine_plugins = &set;
    options.plugin_log = log.sink();
    Harness harness;
    REQUIRE(harness.open(options).has_value());
    REQUIRE_FALSE(log.find({"created"}).empty());

    const auto plugins = harness.client().engine_plugins();
    REQUIRE(plugins.has_value());
    CHECK(plugins->scanned);
    CHECK(plugins->directory_present);
    CHECK(plugins->status == set.status_line());
    REQUIRE(plugins->files.size() == 2);

    const auto good = std::ranges::find_if(plugins->files, [](const rpc::EnginePluginFile& f) {
        return f.file == std::filesystem::path(kRpcTestEnginePluginGood).filename().string();
    });
    const auto old = std::ranges::find_if(plugins->files, [](const rpc::EnginePluginFile& f) {
        return f.file == std::filesystem::path(kRpcTestEnginePluginOldAbi).filename().string();
    });
    REQUIRE(good != plugins->files.end());
    REQUIRE(old != plugins->files.end());
    CHECK(good->loaded);
    CHECK(good->running);
    CHECK(good->name == "test-tracker");
    CHECK(good->refusal == "none");
    CHECK_FALSE(old->loaded);
    CHECK_FALSE(old->running);
    CHECK(old->refusal == "abi-version-mismatch");
}

// ---------------------------------------------------------------------------
// Acting on the engine
// ---------------------------------------------------------------------------

TEST_CASE("a plugin opens a P25 receiver and hears its headers", "[gpu][rpc][plugin]") {
    REVENANT_NEEDS_GPU();

    auto signal = p25_transmission();
    INFO(test::message_of(signal));
    REQUIRE(signal.has_value());
    CaptureFile file;
    REQUIRE(file.write(*signal).has_value());

    const auto dir = plugin_directory("p25", {kRpcTestEnginePluginGood});
    const plugin::EnginePluginSet set = scan(dir);
    REQUIRE(set.modules().size() == 1);

    PluginLog log;
    HarnessOptions options;
    options.source_uri = file.uri();
    options.channels = kGridChannels;
    options.block_samples = kBlockSamples;
    options.engine_plugins = &set;
    options.plugin_log = log.sink();

    auto harness = std::make_unique<Harness>();
    const auto ready = harness->open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    // The source was open before the server started, so the plugin learns of
    // it from the replay the ABI promises, opens its receiver on the carrier
    // and subscribes. All before the engine has run a sample.
    const std::string opened = log.find(
        {event_type(RV_ENGINE_EVENT_SOURCE_OPENED), std::format("center={}", kFileCenterHz)});
    INFO(log.dump());
    REQUIRE_FALSE(opened.empty());

    const std::string added = log.find({event_type(RV_ENGINE_EVENT_VRX_ADDED),
                                        std::format("owner={}", RV_ENGINE_OWNER_THIS_PLUGIN),
                                        "demod=p25p1",
                                        std::format("vrx_center={}", kFileCenterHz + kCarrierHz)});
    REQUIRE_FALSE(added.empty());
    REQUIRE_FALSE(log.find({event_type(RV_ENGINE_EVENT_COMMAND_RESULT), "tag=1 ", "code=0"})
                      .empty());
    REQUIRE_FALSE(log.find({event_type(RV_ENGINE_EVENT_COMMAND_RESULT), "tag=2 ", "code=0"})
                      .empty());

    // The receiver is real: a client sees it, and sees it is not its own.
    const auto receivers = harness->client().vrx_ids();
    INFO(test::message_of(receivers));
    REQUIRE(receivers.has_value());
    CHECK(receivers->size() == 1);

    // The run. Each clear header carries the talkgroup, and the plugin logs
    // integer fields as key=value.
    REQUIRE(harness->start_engine().has_value());
    const std::uint64_t blocks = (file.samples() + kBlockSamples - 1) / kBlockSamples;
    CHECK(harness->wait_for_blocks(blocks, 60'000) >= blocks);
    const std::string header = log.find({event_type(RV_ENGINE_EVENT_DECODED), "decoder=p25p1",
                                         std::format("talkgroup={}", kTalkgroup)},
                                        30'000);
    CHECK_FALSE(header.empty());
    CHECK(harness->stop_engine().has_value());

    // A receiver a client opens is not the plugin's to take. The fixture
    // tries, with tag 3, and the refusal comes back as NOT_OWNER. Kept, so
    // the session ending with the server below does not take it, which would
    // hide whether the plugin's unloading did.
    auto client_vrx = harness->client().add_vrx(
        rpc::VrxParams{.center = kCarrierHz + 20'000, .bandwidth = 0, .demod = rpc::Demod::Nfm},
        rpc::VrxLifetime::Kept);
    INFO(test::message_of(client_vrx));
    REQUIRE(client_vrx.has_value());
    CHECK_FALSE(log.find({event_type(RV_ENGINE_EVENT_VRX_ADDED),
                          std::format("vrx={} ", *client_vrx),
                          std::format("owner={}", RV_ENGINE_OWNER_CLIENT)})
                    .empty());
    CHECK_FALSE(log.find({event_type(RV_ENGINE_EVENT_COMMAND_RESULT), "tag=3 ",
                          std::format("code={}", RV_ENGINE_PLUGIN_ERR_NOT_OWNER)})
                    .empty());
    const auto still = harness->client().vrx_status(*client_vrx);
    CHECK(still.has_value());

    // Unloading the plugin takes its receiver with it and leaves the client's.
    engine::Engine& engine = harness->engine();
    const std::size_t before = engine.vrx_ids().size();
    CHECK(before == 2);
    harness->server().stop();
    CHECK(engine.vrx_ids().size() == 1);
}

TEST_CASE("a front-end retune reaches a plugin with what it removed", "[gpu][rpc][plugin]") {
    REVENANT_NEEDS_GPU();

    const auto dir = plugin_directory("retune", {kRpcTestEnginePluginGood});
    const plugin::EnginePluginSet set = scan(dir);
    REQUIRE(set.modules().size() == 1);

    PluginLog log;
    HarnessOptions options;
    options.retunable = true;
    options.engine_plugins = &set;
    options.plugin_log = log.sink();
    Harness harness;
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());
    REQUIRE_FALSE(log.find({event_type(RV_ENGINE_EVENT_SOURCE_OPENED)}).empty());

    // A long way: every receiver falls outside the new span.
    const auto landed = harness.client().set_source_center(500'000'000);
    INFO(test::message_of(landed));
    REQUIRE(landed.has_value());

    const std::string retuned = log.find({event_type(RV_ENGINE_EVENT_SOURCE_RETUNED)});
    INFO(log.dump());
    REQUIRE_FALSE(retuned.empty());
    CHECK(retuned.find("removed=") != std::string::npos);
}

// ---------------------------------------------------------------------------
// What a client is told about a plugin's receivers
// ---------------------------------------------------------------------------

TEST_CASE("a client is told of a plugin's receiver as the plugin's, by name",
          "[gpu][rpc][plugin][vrx-events]") {
    REVENANT_NEEDS_GPU();

    // The reason subscribeVrxEvents exists: a client never learned of a
    // receiver a plugin opened, so it could neither show it nor play it.
    // Retunable so the case can also take the receiver away the way a trunk
    // tracker's receivers most often go when nobody removes them, with the
    // front end moving out from under them.
    const auto dir = plugin_directory("vrx-events", {kRpcTestEnginePluginGood});
    const plugin::EnginePluginSet set = scan(dir);
    REQUIRE(set.modules().size() == 1);

    PluginLog log;
    HarnessOptions options;
    options.retunable = true;
    options.engine_plugins = &set;
    options.plugin_log = log.sink();
    Harness harness;
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    // The fixture opens its receiver from the source-opened replay, as tag 1.
    INFO(log.dump());
    REQUIRE_FALSE(log.find({event_type(RV_ENGINE_EVENT_COMMAND_RESULT), "tag=1 ", "code=0"})
                      .empty());
    const auto ids = harness.client().vrx_ids();
    INFO(test::message_of(ids));
    REQUIRE(ids.has_value());
    REQUIRE(ids->size() == 1);
    const std::uint64_t vrx = ids->front();

    // Polled, it is the plugin's.
    const auto status = harness.client().vrx_status(vrx);
    INFO(test::message_of(status));
    REQUIRE(status.has_value());
    CHECK(status->owner_kind == rpc::VrxOwnerKind::Plugin);
    CHECK(status->owner_name == "test-tracker");
    CHECK_FALSE(status->owned_by_caller);
    CHECK(status->params.demod == rpc::Demod::P25p1);

    // Pushed, it is the plugin's too, in the replay.
    std::mutex lock;
    std::vector<rpc::VrxEvent> events;
    const auto subscribed = harness.client().subscribe_vrx_events(
        [&](const rpc::VrxEvent& event) {
            const std::scoped_lock held(lock);
            events.push_back(event);
        },
        [](const std::string&) {});
    INFO(test::message_of(subscribed));
    REQUIRE(subscribed.has_value());

    const auto wait_for = [&](rpc::VrxEventKind kind) -> std::optional<rpc::VrxEvent> {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            {
                const std::scoped_lock held(lock);
                for (const rpc::VrxEvent& event : events) {
                    if (event.kind == kind && event.vrx == vrx) {
                        return event;
                    }
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return std::nullopt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };

    const auto added = wait_for(rpc::VrxEventKind::Added);
    REQUIRE(added.has_value());
    CHECK(added->owner == rpc::VrxOwnerKind::Plugin);
    CHECK(added->owner_name == "test-tracker");
    REQUIRE(added->status.has_value());
    CHECK(added->status->owner_kind == rpc::VrxOwnerKind::Plugin);
    CHECK(added->status->owner_name == "test-tracker");

    // A long way: the plugin's receiver falls outside the new span, and the
    // client is told it went and whose it was.
    const auto landed = harness.client().set_source_center(500'000'000);
    INFO(test::message_of(landed));
    REQUIRE(landed.has_value());
    const auto removed = wait_for(rpc::VrxEventKind::Removed);
    REQUIRE(removed.has_value());
    CHECK(removed->owner == rpc::VrxOwnerKind::Plugin);
    CHECK(removed->owner_name == "test-tracker");
    CHECK_FALSE(removed->reason.empty());

    harness.client().unsubscribe_vrx_events();
}
