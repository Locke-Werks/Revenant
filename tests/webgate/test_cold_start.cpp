// The phone-first cold start: the gateway starts at sign-in holding nothing,
// starts an engine when a phone claims, opens the desktop's last radio on an
// engine it started, and stops that engine once the lease has read free for
// the grace. The engine here is the in-process one of tests/rpc/rpc_fixture.h
// standing in for revenant-engine.exe behind EngineLinkConfig::make_process.
//
// Also the two pieces of running from sign-in: one gateway per session, and a
// size-capped log.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "core/rpc/client.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"
#include "tools/webgate/engine_link.h"
#include "tools/webgate/host.h"

namespace {

namespace rpc = revenant::rpc;
using namespace std::chrono_literals;
using revenant::webgate::EngineLink;
using revenant::webgate::EngineLinkConfig;
using revenant::webgate::EngineProcess;
using revenant::webgate::IdleStop;
using revenant::webgate::LinkState;
using Clock = std::chrono::steady_clock;

[[nodiscard]] std::uint16_t unused_port()
{
    WSADATA data{};
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    const SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(probe != INVALID_SOCKET);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(probe, reinterpret_cast<const sockaddr*>(&at), sizeof at) == 0);
    int length = sizeof at;
    REQUIRE(getsockname(probe, reinterpret_cast<sockaddr*>(&at), &length) == 0);
    const std::uint16_t port = ntohs(at.sin_port);
    closesocket(probe);
    WSACleanup();
    return port;
}

[[nodiscard]] revenant::test::HarnessOptions engine_options(std::uint16_t port)
{
    revenant::test::HarnessOptions options;
    options.center_hz = 100'000'000;
    options.connect_client = false;
    options.port = port;
    return options;
}

// Counts what the link does to the engine it believes it started.
struct ProcessLog {
    std::atomic<int> starts{0};
    std::atomic<int> stops{0};
    std::atomic<bool> running{false};
};

class HarnessProcess final : public EngineProcess {
public:
    explicit HarnessProcess(ProcessLog& log) : log_(log) {}
    ~HarnessProcess() override { stop(); }

    std::string start(const std::filesystem::path&, std::uint16_t port,
                      const std::filesystem::path&, const revenant::launch::CaptureChoice&) override
    {
        const std::lock_guard lock(mutex_);
        harness_ = std::make_unique<revenant::test::Harness>();
        if (const auto opened = harness_->open(engine_options(port)); !opened) {
            harness_.reset();
            return opened.error().message;
        }
        ++log_.starts;
        log_.running = true;
        return {};
    }

    void stop() override
    {
        const std::lock_guard lock(mutex_);
        if (harness_) {
            harness_.reset();
            ++log_.stops;
            log_.running = false;
        }
    }

    [[nodiscard]] std::optional<revenant::launch::EngineLauncher::Exit> exited() const override
    {
        // As EngineLauncher: empty while it runs and after stop(), which
        // clears what the last engine left. This engine never dies of itself.
        return std::nullopt;
    }

private:
    ProcessLog& log_;
    mutable std::mutex mutex_;
    std::unique_ptr<revenant::test::Harness> harness_;
};

// A file that exists, so the launch rules find "an engine beside" it. The
// process seam means it is never run.
struct FakeEngineExe {
    std::filesystem::path path;
    FakeEngineExe()
    {
        path = std::filesystem::temp_directory_path() /
               ("revenant-cold-start-" + std::to_string(GetCurrentProcessId()) + ".exe");
        std::ofstream(path) << "not run";
    }
    ~FakeEngineExe()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
};

struct ColdRig {
    FakeEngineExe exe;
    ProcessLog process;
    std::atomic<int> reopens{0};
    std::uint16_t port;
    std::unique_ptr<EngineLink> link;

    explicit ColdRig(std::chrono::milliseconds grace, std::uint16_t at = unused_port()) : port(at)
    {
        EngineLinkConfig config;
        config.port = port;
        config.token = revenant::test::test_token();
        config.poll_interval = 50ms;
        config.restart_interval = 200ms;
        config.engine_exe = exe.path;
        config.on_demand = true;
        config.idle_grace = grace;
        config.reopen = [this](rpc::Client&) { ++reopens; };
        config.make_process = [this] { return std::make_unique<HarnessProcess>(process); };
        link = std::make_unique<EngineLink>(config);
        link->start();
    }

    ~ColdRig() { link.reset(); }

    [[nodiscard]] std::unique_ptr<rpc::Client> claim(const std::string& holder)
    {
        auto client = link->phone_session(rpc::LoginMode::Claim, holder);
        INFO(revenant::test::message_of(client));
        REQUIRE(client.has_value());
        return std::move(*client);
    }
};

[[nodiscard]] bool wait_until(const std::function<bool()>& done,
                              std::chrono::milliseconds limit = 20s)
{
    const auto until = Clock::now() + limit;
    while (Clock::now() < until) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return done();
}

}  // namespace

TEST_CASE("idle stop waits out the grace on a free lease", "[webgate][coldstart]")
{
    IdleStop idle;
    idle.grace = 10s;
    const auto t0 = Clock::now();
    CHECK_FALSE(idle.observe(false, t0, std::nullopt));
    CHECK_FALSE(idle.observe(false, t0 + 9s, std::nullopt));
    CHECK(idle.observe(false, t0 + 10s, std::nullopt));
}

TEST_CASE("idle stop starts over when the lease is held", "[webgate][coldstart]")
{
    IdleStop idle;
    idle.grace = 10s;
    const auto t0 = Clock::now();
    CHECK_FALSE(idle.observe(false, t0, std::nullopt));
    CHECK_FALSE(idle.observe(true, t0 + 5s, std::nullopt));
    CHECK_FALSE(idle.observe(false, t0 + 11s, std::nullopt));
    CHECK_FALSE(idle.observe(false, t0 + 20s, std::nullopt));
    CHECK(idle.observe(false, t0 + 21s, std::nullopt));
}

TEST_CASE("idle stop is held off by a phone asking during the grace", "[webgate][coldstart]")
{
    IdleStop idle;
    idle.grace = 10s;
    const auto t0 = Clock::now();
    CHECK_FALSE(idle.observe(false, t0, std::nullopt));
    // A phone asked at 8 s and has not claimed yet: its claim is on the way.
    CHECK_FALSE(idle.observe(false, t0 + 12s, t0 + 8s));
    CHECK(idle.observe(false, t0 + 18s, t0 + 8s));
}

TEST_CASE("the log keeps one older file and caps the current one", "[webgate][coldstart]")
{
    const auto dir = std::filesystem::temp_directory_path() /
                     ("revenant-log-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(dir);
    revenant::webgate::RotatingLog log(dir / "webgate.log", 100);
    const std::string line(40, 'a');
    log.append(line + "\n");  // 41
    log.append(line + "\n");  // 82
    CHECK_FALSE(std::filesystem::exists(log.previous_path()));
    log.append(line + "\n");  // would be 123: rotates first
    CHECK(std::filesystem::exists(log.previous_path()));
    CHECK(std::filesystem::file_size(log.previous_path()) == 82);
    CHECK(std::filesystem::file_size(log.path()) == 41);

    for (int i = 0; i < 10; ++i) {
        log.append(line + "\n");
    }
    CHECK(std::filesystem::file_size(log.path()) <= 100);
    CHECK(std::filesystem::file_size(log.previous_path()) <= 100);
    // Exactly two files: the current one and one older.
    int files = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(dir)) {
        ++files;
    }
    CHECK(files == 2);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a second gateway on the same port in one session does not start",
          "[webgate][coldstart]")
{
    const std::string name =
        "Local\\Revenant-webgate-test-" + std::to_string(GetCurrentProcessId());
    auto first = revenant::webgate::SingleInstance::acquire(name);
    REQUIRE(first != nullptr);
    CHECK(revenant::webgate::SingleInstance::acquire(name) == nullptr);
    first.reset();
    CHECK(revenant::webgate::SingleInstance::acquire(name) != nullptr);

    CHECK(revenant::webgate::instance_name(17700) != revenant::webgate::instance_name(17710));
}

TEST_CASE("no engine is started until a phone claims", "[gpu][webgate][coldstart]")
{
    REVENANT_NEEDS_GPU();
    ColdRig rig(10s);

    // Plenty of polls go by with nothing on the port.
    std::this_thread::sleep_for(600ms);
    CHECK(rig.process.starts == 0);
    // Starting or Waiting: a refused loopback connect takes Windows about two
    // seconds, so the first attempt may still be in flight.
    CHECK(rig.link->state() != LinkState::Up);

    auto phone = rig.claim("phone 10.1.10.57 via webgate");
    CHECK(rig.process.starts == 1);
    CHECK(rig.link->owns_engine());
    // The engine came up with no phone yet, so the phone that asked opens
    // the desktop's radio, once.
    CHECK(rig.reopens == 1);

    auto second = rig.link->phone_session(rpc::LoginMode::Observe, "a look");
    REQUIRE(second.has_value());
    CHECK(rig.reopens == 1);
    CHECK(rig.process.starts == 1);
}

TEST_CASE("an engine the gateway started stops after the grace once the lease is free",
          "[gpu][webgate][coldstart]")
{
    REVENANT_NEEDS_GPU();
    ColdRig rig(400ms);
    auto phone = rig.claim("phone 10.1.10.57 via webgate");
    REQUIRE(rig.process.running);

    // Held: well past the grace and still running.
    std::this_thread::sleep_for(1s);
    CHECK(rig.process.running);
    CHECK(rig.process.stops == 0);

    // The phone's linger ending is its session going, which ends the lease.
    phone.reset();
    REQUIRE(wait_until([&] { return rig.process.stops == 1; }));
    CHECK_FALSE(rig.process.running);
    CHECK(rig.link->idle_stops() == 1);
    CHECK_FALSE(rig.link->owns_engine());

    // Idle, not gone: the next phone starts it again and reopens the radio.
    std::this_thread::sleep_for(300ms);
    CHECK(rig.process.starts == 1);
    auto again = rig.claim("phone 10.1.10.58 via webgate");
    CHECK(rig.process.starts == 2);
    CHECK(rig.reopens == 2);
}

TEST_CASE("a phone arriving during the grace cancels the stop", "[gpu][webgate][coldstart]")
{
    REVENANT_NEEDS_GPU();
    ColdRig rig(1500ms);
    auto phone = rig.claim("phone 10.1.10.57 via webgate");
    phone.reset();

    std::this_thread::sleep_for(500ms);
    auto next = rig.claim("phone 10.1.10.58 via webgate");
    // An engine already running is not a fresh one.
    CHECK(rig.reopens == 1);

    std::this_thread::sleep_for(3s);
    CHECK(rig.process.stops == 0);
    CHECK(rig.process.running);
    CHECK(rig.process.starts == 1);
}

TEST_CASE("an engine the gateway did not start is never stopped", "[gpu][webgate][coldstart]")
{
    REVENANT_NEEDS_GPU();
    const std::uint16_t port = unused_port();
    revenant::test::Harness desktop_engine;
    const auto opened = desktop_engine.open(engine_options(port));
    INFO(revenant::test::message_of(opened));
    REQUIRE(opened.has_value());

    ColdRig rig(300ms, port);
    auto phone = rig.claim("phone 10.1.10.57 via webgate");
    CHECK(rig.process.starts == 0);
    // Whatever the desktop had open stays: nothing is reopened over it.
    CHECK(rig.reopens == 0);
    phone.reset();

    std::this_thread::sleep_for(1500ms);
    CHECK(rig.process.stops == 0);
    CHECK(rig.link->idle_stops() == 0);
    CHECK(rig.link->state() == LinkState::Up);
    auto still = desktop_engine.connect_as(rpc::LoginMode::Observe, "check");
    INFO(revenant::test::message_of(still));
    CHECK(still.has_value());
}

TEST_CASE("the hub's rebuild path asks for the engine without waiting for it",
          "[gpu][webgate][coldstart]")
{
    REVENANT_NEEDS_GPU();
    ColdRig rig(10s);
    // The loop calls this to rebuild sessions. With no engine it must come
    // back with the connect's failure, not wait the start out; a refused
    // loopback connect costs Windows about two seconds of that.
    const auto asked = Clock::now();
    auto now = rig.link->phone_session_now(rpc::LoginMode::Claim, "phone 10.1.10.57 via webgate");
    CHECK(Clock::now() - asked < 5s);
    if (!now) {
        // It did ask: the engine comes up for the next tick to find.
        CHECK(wait_until([&] { return rig.process.starts == 1; }));
        CHECK(wait_until([&] {
            return rig.link->phone_session_now(rpc::LoginMode::Claim, "phone 10.1.10.57 via webgate")
                .has_value();
        }));
    }
    CHECK(rig.process.starts == 1);
}

TEST_CASE("a claim at the end of the grace either cancels the stop or follows it",
          "[gpu][webgate][coldstart]")
{
    REVENANT_NEEDS_GPU();
    ColdRig rig(300ms);
    // Each round lets the lease go free and claims again around the moment
    // the grace runs out, from just before to just after. Whichever side of
    // the decision a claim lands on, it must get an engine: never one that is
    // being ended under it.
    for (int round = 0; round < 8; ++round) {
        auto phone = rig.claim("phone 10.1.10.57 via webgate");
        auto status = phone->lease_status();
        REQUIRE(status.has_value());
        CHECK(status->held);
        phone.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(250 + 25 * round));
    }
    auto last = rig.claim("phone 10.1.10.57 via webgate");
    const auto info = last->info();
    INFO(revenant::test::message_of(info));
    CHECK(info.has_value());
    // Starts and stops pair up: never two engines at once.
    CHECK(rig.process.starts - rig.process.stops == 1);
}

namespace {

// A prefix of the case's own, so nothing here reaches a real gateway's event.
[[nodiscard]] std::string test_prefix(std::string_view which)
{
    return "Revenant-webgate-stop-test-" + std::string(which) + "-" +
           std::to_string(GetCurrentProcessId()) + "-";
}

}  // namespace

TEST_CASE("each gateway's stop event is named for its process", "[webgate][coldstart]")
{
    CHECK(revenant::webgate::stop_event_name(revenant::webgate::kStopEventPrefix, 4242) ==
          "Revenant-webgate-stop-4242");
    CHECK(revenant::webgate::stop_event_name("p-", 1) != revenant::webgate::stop_event_name("p-", 2));
}

TEST_CASE("the stop event reaches its gateway through the session namespace",
          "[webgate][coldstart]")
{
    const std::string prefix = test_prefix("session");
    std::atomic<bool> stopped{false};
    revenant::webgate::StopWatch watch([&] { stopped = true; }, prefix);
    REQUIRE(watch.watching());
    // Opened as --stop opens another session's: Session\<n>\..., owner checked.
    CHECK(revenant::webgate::signal_gateway(GetCurrentProcessId(), prefix) ==
          revenant::webgate::StopSignal::Set);
    CHECK(wait_until([&] { return stopped.load(); }, 5s));
}

TEST_CASE("a stop event that already exists is not watched", "[webgate][coldstart]")
{
    const std::string prefix = test_prefix("squat");
    // Somebody created this gateway's name first.
    const std::wstring name = L"Local\\" + [&] {
        const std::string n = revenant::webgate::stop_event_name(prefix, GetCurrentProcessId());
        return std::wstring(n.begin(), n.end());
    }();
    HANDLE squatter = CreateEventW(nullptr, TRUE, TRUE, name.c_str());
    REQUIRE(squatter != nullptr);
    std::atomic<bool> stopped{false};
    {
        revenant::webgate::StopWatch watch([&] { stopped = true; }, prefix);
        CHECK_FALSE(watch.watching());
    }
    CHECK_FALSE(stopped);
    CloseHandle(squatter);
}

TEST_CASE("the stop event's owner must be the gateway's account or an administrator",
          "[webgate][coldstart]")
{
    const auto sid = [](WELL_KNOWN_SID_TYPE type) {
        std::vector<std::uint8_t> out(SECURITY_MAX_SID_SIZE);
        DWORD size = static_cast<DWORD>(out.size());
        REQUIRE(CreateWellKnownSid(type, nullptr, out.data(), &size) != 0);
        return out;
    };
    auto everyone = sid(WinWorldSid);
    auto admins = sid(WinBuiltinAdministratorsSid);
    auto system = sid(WinLocalSystemSid);
    auto user = sid(WinLocalServiceSid);  // stands in for the gateway's account
    auto other = sid(WinNetworkServiceSid);

    using revenant::webgate::stop_event_owner_acceptable;
    CHECK(stop_event_owner_acceptable(user.data(), user.data()));
    CHECK(stop_event_owner_acceptable(admins.data(), user.data()));
    CHECK(stop_event_owner_acceptable(system.data(), user.data()));
    CHECK_FALSE(stop_event_owner_acceptable(other.data(), user.data()));
    CHECK_FALSE(stop_event_owner_acceptable(everyone.data(), user.data()));
    CHECK_FALSE(stop_event_owner_acceptable(nullptr, user.data()));
    CHECK_FALSE(stop_event_owner_acceptable(other.data(), nullptr));
}

namespace {

[[nodiscard]] std::filesystem::path this_exe()
{
    std::wstring buffer(32768, L'\0');
    buffer.resize(GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())));
    return std::filesystem::path(buffer);
}

[[nodiscard]] bool running_elevated()
{
    HANDLE token = nullptr;
    REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != 0);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool yes = GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation,
                                         &size) != 0 &&
                     elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return yes;
}

// A stand-in gateway with an image name of its own, so a case can never
// touch a real one: ping, copied into dir, run long, with no stop event.
struct FakeGateway {
    std::wstring image;
    std::filesystem::path path;
    PROCESS_INFORMATION child{};

    FakeGateway(const std::filesystem::path& dir, std::wstring_view tag)
    {
        image = L"revenant-stop-fake-" + std::wstring(tag) + L"-" +
                std::to_wstring(GetCurrentProcessId()) + L".exe";
        path = dir / image;
        std::filesystem::copy_file("C:\\Windows\\System32\\PING.EXE", path,
                                   std::filesystem::copy_options::overwrite_existing);
        std::wstring command = L"\"" + path.wstring() + L"\" -n 120 127.0.0.1";
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != 0);
        CloseHandle(child.hThread);
    }

    ~FakeGateway()
    {
        if (WaitForSingleObject(child.hProcess, 0) != WAIT_OBJECT_0) {
            TerminateProcess(child.hProcess, 1);
            WaitForSingleObject(child.hProcess, 5'000);
        }
        CloseHandle(child.hProcess);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    [[nodiscard]] bool gone() const { return WaitForSingleObject(child.hProcess, 0) == WAIT_OBJECT_0; }
};

}  // namespace

TEST_CASE("--stop terminates a gateway that does not answer its event", "[webgate][coldstart]")
{
    // Beside this executable, which stands for the installation --stop runs
    // from, so it is terminated whether or not the case runs elevated.
    FakeGateway fake(this_exe().parent_path(), L"here");
    CHECK(revenant::webgate::stop_running_gateways(300ms, test_prefix("fallback"), fake.image) ==
          0);
    CHECK(fake.gone());
}

TEST_CASE("an elevated --stop leaves another installation's gateway running",
          "[webgate][coldstart]")
{
    if (!running_elevated()) {
        SKIP("the directory rule applies only to an elevated --stop");
    }
    FakeGateway fake(std::filesystem::temp_directory_path(), L"elsewhere");
    CHECK(revenant::webgate::stop_running_gateways(300ms, test_prefix("elsewhere"), fake.image) ==
          0);
    CHECK_FALSE(fake.gone());
}

TEST_CASE("a watcher ended normally does not report a stop", "[webgate][coldstart]")
{
    std::atomic<bool> fired{false};
    {
        revenant::webgate::StopWatch watch([&] { fired = true; }, test_prefix("quiet"));
        REQUIRE(watch.watching());
    }
    CHECK_FALSE(fired);
}
