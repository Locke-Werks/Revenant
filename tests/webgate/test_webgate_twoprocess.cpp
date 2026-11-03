// revenant-webgate end to end: a real revenant-engine on a synthetic source,
// a real revenant-webgate in front of it, and a phone played by a kj-tls
// WebSocket client that trusts only the gateway's own CA.
//
// Every path is a temporary directory of the case's own: the engine's token
// file, the gateway's certificates and its web root. Nothing here reads or
// writes the user's real token or state.
//
// The source is one CW emitter keying "CQ DE REVENANT" 100 kHz above the
// centre (core/dsp/synth/wideband.cpp), placed in a 100 Hz window so a cw
// receiver at 100000 Hz is on it. The keyer leaves only a word space between
// repeats, so the CW decoder ends a line at its 80 character limit, not on
// idle. Seed 4 keys at 30 WPM, which puts that line about 26 s into the
// source; the engine runs at eight times realtime, so it arrives about 3.5 s
// after audio starts (measured 6.5 s at four times), well inside the 30 s
// the case allows. At 12 WPM (seed 7) the same line takes 64 s of source.
//
// Every wait is bounded. The engine and gateway are in a job object that
// kills them if this process dies, so a failed case never leaves a gateway
// holding a port.

// kj before <windows.h>: windows.h defines DELETE, which breaks
// kj::HttpMethod.
#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/compat/tls.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "core/rpc/client.h"
#include "core/rpc/token.h"
#include "tools/webgate/certs.h"

namespace {

namespace rpc = revenant::rpc;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kScene =
    "synthetic:wideband?rate=2400032&emitters=1&modes=cw&span_low=99950&span_high=100050&seed=4";
constexpr std::int64_t kCarrierHz = 100'000;

// Process startup includes a Vulkan device and, for the gateway, issuing a
// certificate; a loaded CI box can take a while over either.
constexpr auto kStartup = 60s;

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
}

[[nodiscard]] std::string slurp(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

[[nodiscard]] std::wstring widen(std::string_view text)
{
    return {text.begin(), text.end()};
}

[[nodiscard]] std::filesystem::path fresh_directory()
{
    static std::atomic<unsigned> counter{0};
    const auto dir = std::filesystem::temp_directory_path() /
                     ("revenant-webgate-e2e-" + std::to_string(GetCurrentProcessId()) + "-" +
                      std::to_string(Clock::now().time_since_epoch().count()) + "-" +
                      std::to_string(counter.fetch_add(1)));
    std::filesystem::create_directories(dir);
    return dir;
}

// One kill-on-close job for every child of this test process.
[[nodiscard]] HANDLE job()
{
    static const HANDLE handle = [] {
        HANDLE created = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(created, JobObjectExtendedLimitInformation, &limits,
                                sizeof(limits));
        return created;
    }();
    return handle;
}

// A child process with stdout and stderr in files, killed on destruction.
class Child {
public:
    Child(const std::filesystem::path& exe, const std::vector<std::string>& args,
          const std::filesystem::path& out, const std::filesystem::path& err)
        : out_(out), err_(err)
    {
        std::wstring command = L"\"" + exe.wstring() + L"\"";
        for (const auto& arg : args) {
            command += L" \"" + widen(arg) + L"\"";
        }
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        const auto open = [&](const std::filesystem::path& path) {
            return CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        };
        HANDLE out_handle = open(out);
        HANDLE err_handle = open(err);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = out_handle;
        startup.hStdError = err_handle;
        PROCESS_INFORMATION info{};
        const BOOL created =
            CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &info);
        CloseHandle(out_handle);
        CloseHandle(err_handle);
        INFO("CreateProcessW " << exe.string() << " failed: " << GetLastError());
        REQUIRE(created != 0);
        AssignProcessToJobObject(job(), info.hProcess);
        ResumeThread(info.hThread);
        CloseHandle(info.hThread);
        process_ = info.hProcess;
    }

    ~Child() { kill(); }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    void kill()
    {
        if (process_ != nullptr) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 10'000);
            CloseHandle(process_);
            process_ = nullptr;
        }
    }

    [[nodiscard]] bool running() const
    {
        return process_ != nullptr && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
    }

    [[nodiscard]] std::string out() const { return slurp(out_); }
    [[nodiscard]] std::string err() const { return slurp(err_); }

    // The first capture group of pattern in stdout, once it appears.
    [[nodiscard]] std::optional<std::string> wait_for(const std::regex& pattern,
                                                      Clock::duration timeout) const
    {
        const auto deadline = Clock::now() + timeout;
        for (;;) {
            const auto text = out();
            std::smatch match;
            if (std::regex_search(text, match, pattern)) {
                return match[1].str();
            }
            if (!running() || Clock::now() > deadline) {
                return std::nullopt;
            }
            Sleep(20);
        }
    }

private:
    HANDLE process_ = nullptr;
    std::filesystem::path out_;
    std::filesystem::path err_;
};

[[nodiscard]] std::filesystem::path engine_exe() { return REVENANT_ENGINE_EXE; }
[[nodiscard]] std::filesystem::path webgate_exe() { return REVENANT_WEBGATE_EXE; }

// Fixed bytes: a test client has no use for real masking entropy.
class FixedEntropy final : public kj::EntropySource {
public:
    void generate(kj::ArrayPtr<kj::byte> buffer) override
    {
        for (auto& b : buffer) {
            b = 0x5a;
        }
    }
};

// A phone: one TLS WebSocket, its messages collected by a receive loop that
// runs whenever the test waits on the event loop. It answers "ping" with
// "pong" as the app must, or the gateway closes it after 45 s.
class Phone {
public:
    Phone(kj::AsyncIoContext& io, kj::TlsContext& tls, kj::HttpHeaderTable& table,
          const std::string& host, std::uint16_t port)
    {
        auto address = io.provider->getNetwork().parseAddress("127.0.0.1", port).wait(io.waitScope);
        auto stream = address->connect().wait(io.waitScope);
        secure_ = tls.wrapClient(kj::mv(stream), host).wait(io.waitScope);
        kj::HttpClientSettings settings;
        settings.entropySource = entropy_;
        http_ = kj::newHttpClient(table, *secure_, settings);
        kj::HttpHeaders headers(table);
        headers.set(kj::HttpHeaderId::HOST, host);
        // HttpHeaders keeps pointers, not copies, so the value has to outlive
        // the request.
        const std::string origin = "https://" + host + ":" + std::to_string(port);
        headers.add("Origin", origin);
        auto response = http_->openWebSocket("/ws", headers).wait(io.waitScope);
        status = response.statusCode;
        if (response.webSocketOrBody.is<kj::Own<kj::WebSocket>>()) {
            ws_ = kj::mv(response.webSocketOrBody.get<kj::Own<kj::WebSocket>>());
            loop_ = receive_loop().eagerlyEvaluate([this](kj::Exception&&) { closed = true; });
        } else {
            closed = true;
        }
    }

    Phone(const Phone&) = delete;
    Phone& operator=(const Phone&) = delete;

    void send(std::string text)
    {
        auto held = kj::heap<std::string>(std::move(text));
        sending_ = sending_
                       .then([this, held = kj::mv(held)]() mutable {
                           auto sent = ws_->send(kj::ArrayPtr<const char>(held->data(), held->size()));
                           return sent.attach(kj::mv(held));
                       })
                       .eagerlyEvaluate([this](kj::Exception&&) { closed = true; });
    }

    void command(std::uint64_t id, std::string_view op, std::string_view args)
    {
        send("{\"id\":" + std::to_string(id) + ",\"op\":\"" + std::string(op) +
             "\",\"args\":" + std::string(args) + "}");
    }

    // The reply to command id, or empty.
    [[nodiscard]] std::string reply(std::uint64_t id) const
    {
        const std::string prefix = "{\"id\":" + std::to_string(id) + ",";
        for (const auto& text : texts) {
            if (text.rfind(prefix, 0) == 0) {
                return text;
            }
        }
        return {};
    }

    // Texts from index `from` on containing needle.
    [[nodiscard]] std::vector<std::string> with(std::string_view needle, std::size_t from = 0) const
    {
        std::vector<std::string> out;
        for (std::size_t i = from; i < texts.size(); ++i) {
            if (contains(texts[i], needle)) {
                out.push_back(texts[i]);
            }
        }
        return out;
    }

    unsigned status = 0;
    bool closed = false;
    std::vector<std::string> texts;
    std::size_t spectrum = 0;
    std::size_t opus = 0;

private:
    kj::Promise<void> receive_loop()
    {
        return ws_->receive().then([this](kj::WebSocket::Message&& message) -> kj::Promise<void> {
            if (message.is<kj::WebSocket::Close>()) {
                closed = true;
                return kj::READY_NOW;
            }
            if (message.is<kj::String>()) {
                std::string text(message.get<kj::String>().cStr());
                if (contains(text, R"("event":"ping")")) {
                    send(R"({"id":0,"op":"pong"})");
                }
                texts.push_back(std::move(text));
            } else {
                const auto& bytes = message.get<kj::Array<kj::byte>>();
                if (bytes.size() > 0 && bytes[0] == 1) {
                    ++spectrum;
                } else if (bytes.size() > 0 && bytes[0] == 2) {
                    ++opus;
                }
            }
            return receive_loop();
        });
    }

    // Destroyed in reverse: the loops first, then the socket they use.
    FixedEntropy entropy_;
    kj::Own<kj::AsyncIoStream> secure_;
    kj::Own<kj::HttpClient> http_;
    kj::Own<kj::WebSocket> ws_;
    kj::Promise<void> sending_ = kj::READY_NOW;
    kj::Promise<void> loop_ = nullptr;
};

[[nodiscard]] std::uint64_t vrx_of(const std::string& reply)
{
    const auto at = reply.find("\"vrx\":");
    REQUIRE(at != std::string::npos);
    return std::stoull(reply.substr(at + 6));
}

// Removes the directory when it goes. Declared first in Rig so a REQUIRE
// that fails in Rig's constructor, after which ~Rig never runs, still
// cleans up through this member's destructor.
class TempDir {
public:
    TempDir() : path_(fresh_directory()) {}
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// An engine and a gateway in a directory of their own.
class Rig {
public:
    Rig() : dir_(temp_.path()), io_(kj::setupAsyncIo())
    {
        std::filesystem::create_directories(dir_ / "web");
        start_engine(0);
        start_gateway();

        const std::string ca_pem = slurp(revenant::webgate::CertPaths{dir_ / "state" / "certs"}.ca_crt());
        REQUIRE_FALSE(ca_pem.empty());
        kj::TlsContext::Options options;
        options.useSystemTrustStore = false;
        auto builder = kj::heapArrayBuilder<kj::TlsCertificate>(1);
        builder.add(kj::StringPtr(ca_pem.c_str()));
        trusted_ = builder.finish();
        options.trustedCertificates = trusted_;
        tls_ = kj::heap<kj::TlsContext>(kj::mv(options));
    }

    ~Rig()
    {
        // The children hold files in the directory; they go before it does.
        gateway_.reset();
        engine_.reset();
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    // Starts the engine on port (0 for any). A restart on the port the last
    // one held is retried for a while, because the address can take a moment
    // to be free after the old process is gone.
    void start_engine(std::uint16_t port)
    {
        const auto deadline = Clock::now() + kStartup;
        for (int attempt = 0;; ++attempt) {
            const auto stem = "engine-" + std::to_string(engines_++);
            engine_ = std::make_unique<Child>(
                engine_exe(),
                std::vector<std::string>{std::string(kScene), "--gpu", "-1", "--block-samples",
                                         "16384", "--pace", "8", "--quiet", "--port",
                                         std::to_string(port), "--token-file",
                                         (dir_ / "token").string()},
                dir_ / (stem + ".out"), dir_ / (stem + ".err"));
            const auto got = engine_->wait_for(std::regex(R"(listening on 127\.0\.0\.1:(\d+))"),
                                               kStartup);
            if (got) {
                engine_port_ = static_cast<std::uint16_t>(std::stoul(*got));
                return;
            }
            INFO("engine stdout: " << engine_->out());
            INFO("engine stderr: " << engine_->err());
            REQUIRE((port != 0 && Clock::now() < deadline));
            Sleep(250);
        }
    }

    void start_gateway()
    {
        gateway_ = std::make_unique<Child>(
            webgate_exe(),
            std::vector<std::string>{"--port", "0", "--http-port", "0", "--engine",
                                     "127.0.0.1:" + std::to_string(engine_port_), "--token-file",
                                     (dir_ / "token").string(), "--state-dir",
                                     (dir_ / "state").string(), "--web-root",
                                     (dir_ / "web").string(), "--session-linger-ms", "500"},
            dir_ / "webgate.out", dir_ / "webgate.err");
        const auto port = gateway_->wait_for(std::regex(R"(https port (\d+))"), kStartup);
        INFO("webgate stdout: " << gateway_->out());
        INFO("webgate stderr: " << gateway_->err());
        REQUIRE(port.has_value());
        https_port_ = static_cast<std::uint16_t>(std::stoul(*port));

        // A name the certificate covers, not an address: the TLS client
        // checks it against a DNS SAN.
        const auto text = gateway_->out();
        const std::regex covers(R"(covers ([^\r\n]+))");
        for (auto it = std::sregex_iterator(text.begin(), text.end(), covers);
             it != std::sregex_iterator(); ++it) {
            const auto name = (*it)[1].str();
            if (std::ranges::any_of(name, [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }) &&
                !contains(name, ":")) {
                host_ = name;
                break;
            }
        }
        REQUIRE_FALSE(host_.empty());
    }

    // A phone with its hello in hand. The gateway answers a socket with an
    // error and a close until its link has a session to give, so this retries
    // until the engine has come up behind it.
    [[nodiscard]] std::unique_ptr<Phone> phone()
    {
        const auto deadline = Clock::now() + kStartup;
        for (;;) {
            auto phone = std::make_unique<Phone>(io_, *tls_, table_, host_, https_port_);
            INFO("host " << host_ << " port " << https_port_);
            REQUIRE(phone->status == 101);
            const bool greeted = wait([&] { return !phone->with(R"("event":"hello")").empty() || phone->closed; }, 10s);
            if (greeted && !phone->with(R"("event":"hello")").empty()) {
                return phone;
            }
            REQUIRE(Clock::now() < deadline);
            pause(250ms);
        }
    }

    // Runs the event loop, so every phone's receive loop, until pred holds.
    template <class Pred>
    [[nodiscard]] bool wait(Pred pred, Clock::duration timeout)
    {
        const auto deadline = Clock::now() + timeout;
        while (!pred()) {
            if (Clock::now() > deadline) {
                return false;
            }
            pause(20ms);
        }
        return true;
    }

    void pause(std::chrono::milliseconds ms)
    {
        io_.provider->getTimer()
            .afterDelay(static_cast<std::int64_t>(ms.count()) * kj::MILLISECONDS)
            .wait(io_.waitScope);
    }

    // A client of the rig's engine, for asking it what receivers exist.
    [[nodiscard]] std::unique_ptr<rpc::Client> engine_client()
    {
        const auto token = rpc::load_token((dir_ / "token").string());
        INFO((token ? std::string() : token.error().message));
        REQUIRE(token.has_value());
        auto client = rpc::Client::connect("127.0.0.1", engine_port_, *token);
        INFO((client ? std::string() : client.error().message));
        REQUIRE(client.has_value());
        return std::move(*client);
    }

    [[nodiscard]] Child& engine() { return *engine_; }
    [[nodiscard]] std::uint16_t engine_port() const { return engine_port_; }

private:
    TempDir temp_;
    std::filesystem::path dir_;
    kj::AsyncIoContext io_;
    kj::HttpHeaderTable table_;
    kj::Array<kj::TlsCertificate> trusted_;
    kj::Own<kj::TlsContext> tls_;
    std::unique_ptr<Child> engine_;
    std::unique_ptr<Child> gateway_;
    std::uint16_t engine_port_ = 0;
    std::uint16_t https_port_ = 0;
    std::string host_;
    int engines_ = 0;
};

// addVrx on the CW carrier, returning its id.
[[nodiscard]] std::uint64_t add_cw(Rig& rig, Phone& phone, std::uint64_t id)
{
    phone.command(id, "addVrx",
                  R"({"hz":)" + std::to_string(kCarrierHz) + R"(,"mode":"cw"})");
    REQUIRE(rig.wait([&] { return !phone.reply(id).empty(); }, 10s));
    const auto reply = phone.reply(id);
    INFO(reply);
    REQUIRE(contains(reply, R"("ok":true)"));
    return vrx_of(reply);
}

[[nodiscard]] std::vector<std::uint64_t> engine_ids(rpc::Client& client)
{
    const auto ids = client.vrx_ids();
    INFO((ids ? std::string() : ids.error().message));
    REQUIRE(ids.has_value());
    return *ids;
}

}  // namespace

TEST_CASE("phone round trip", "[webgate][twoprocess]")
{
    Rig rig;
    auto phone = rig.phone();
    auto engine = rig.engine_client();

    phone->command(1, "setView", R"({"centerHz":0,"spanHz":2400000,"widthPx":400})");
    const auto vrx = add_cw(rig, *phone, 2);
    phone->command(3, "audio", R"({"vrx":)" + std::to_string(vrx) + "}");

    const auto decoded = [&] { return !phone->with(R"("event":"decoded")").empty(); };
    const bool streaming =
        rig.wait([&] { return phone->spectrum >= 1 && phone->opus >= 10; }, 10s);
    INFO("spectrum frames " << phone->spectrum << ", opus frames " << phone->opus);
    CHECK(streaming);
    // A window of its own: the line normally lands about 3.5 s after audio
    // starts (header), and a loaded box slows the engine below its pace.
    CHECK(rig.wait(decoded, 30s));
    CHECK(contains(phone->reply(3), R"("ok":true)"));
    if (decoded()) {
        INFO(phone->with(R"("event":"decoded")").front());
        CHECK(contains(phone->with(R"("event":"decoded")").front(), R"("decoder":"cw")"));
    }
    {
        const auto ids = engine_ids(*engine);
        CHECK(std::ranges::find(ids, vrx) != ids.end());
    }

    // Gone from the engine once the session's linger (500 ms here) is over.
    phone.reset();
    const bool removed = rig.wait(
        [&] {
            const auto ids = engine_ids(*engine);
            return std::ranges::find(ids, vrx) == ids.end();
        },
        10s);
    CHECK(removed);
}

TEST_CASE("two phones keep their own receivers", "[webgate][twoprocess]")
{
    Rig rig;
    auto engine = rig.engine_client();
    auto first = rig.phone();
    auto second = rig.phone();
    const auto mine = add_cw(rig, *first, 1);
    const auto theirs = add_cw(rig, *second, 1);
    REQUIRE(mine != theirs);

    first.reset();
    const bool only = rig.wait([&] { return engine_ids(*engine) == std::vector{theirs}; }, 10s);
    INFO("engine has " << engine_ids(*engine).size() << " receivers");
    CHECK(only);

    // And the second phone still drives its own.
    second->command(2, "audio", R"({"vrx":)" + std::to_string(theirs) + "}");
    CHECK(rig.wait([&] { return second->opus >= 10; }, 10s));
    CHECK(engine_ids(*engine) == std::vector{theirs});
}

TEST_CASE("engine restart recreates the phone's receivers", "[webgate][twoprocess]")
{
    Rig rig;
    auto phone = rig.phone();
    const auto vrx = add_cw(rig, *phone, 1);
    phone->command(2, "audio", R"({"vrx":)" + std::to_string(vrx) + "}");
    REQUIRE(rig.wait([&] { return phone->opus >= 1; }, 10s));

    const auto mark = phone->texts.size();
    const auto port = rig.engine_port();
    rig.engine().kill();
    const bool waiting =
        rig.wait([&] { return !phone->with(R"("state":"waiting")", mark).empty(); }, 15s);
    REQUIRE(waiting);

    rig.start_engine(port);
    REQUIRE(rig.engine_port() == port);
    const bool up = rig.wait([&] { return !phone->with(R"("state":"up")", mark).empty(); }, 30s);
    REQUIRE(up);
    const auto event = phone->with(R"("state":"up")", mark).front();
    INFO(event);
    CHECK(contains(event, R"("recreated":[{"old":)" + std::to_string(vrx) + R"(,"new":)"));
    CHECK(contains(event, R"("lost":[])"));

    // Waiting came before Up.
    const auto at = [&](std::string_view needle) {
        for (std::size_t i = mark; i < phone->texts.size(); ++i) {
            if (contains(phone->texts[i], needle)) {
                return i;
            }
        }
        return phone->texts.size();
    };
    CHECK(at(R"("state":"waiting")") < at(R"("state":"up")"));

    // The new engine holds exactly one receiver, at the same frequency and in
    // the same mode, and its audio reaches the phone again.
    auto engine = rig.engine_client();
    const auto ids = engine_ids(*engine);
    REQUIRE(ids.size() == 1);
    const auto status = engine->vrx_status(ids.front());
    REQUIRE(status.has_value());
    CHECK(status->params.center == kCarrierHz);
    CHECK(status->params.demod == rpc::Demod::Cw);
    CHECK(contains(event, R"("new":)" + std::to_string(ids.front()) + "}"));

    const auto opus_before = phone->opus;
    CHECK(rig.wait([&] { return phone->opus >= opus_before + 10; }, 10s));
}
