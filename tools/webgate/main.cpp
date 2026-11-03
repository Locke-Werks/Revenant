// revenant-webgate: the HTTPS and WebSocket gateway an iPhone web app uses to
// drive the engine over the LAN.
//
// Settles the TLS identity, binds the two listeners, connects to the engine
// (starting revenant-engine.exe from beside this executable when nothing
// answers), and serves phones until the process is ended.

// http.h (through gateway.h) before <windows.h>: windows.h defines DELETE,
// which breaks kj::HttpMethod.
#include "tools/webgate/gateway.h"
#include "tools/webgate/http.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

#include "tools/webgate/certs.h"
#include "tools/webgate/engine_link.h"

namespace {

struct Options {
    int port = 17700;
    int http_port = 17701;
    std::string engine = "127.0.0.1:17690";
    std::filesystem::path state_dir;
    std::filesystem::path web_root;
    std::filesystem::path token_file;
    std::optional<long> session_linger_ms;
};

[[nodiscard]] std::filesystem::path exe_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::filesystem::current_path();
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

[[nodiscard]] std::filesystem::path default_state_dir() {
    const char* local = std::getenv("LOCALAPPDATA");
    const std::filesystem::path base = local != nullptr ? local : ".";
    return base / "Revenant" / "webgate";
}

void usage() {
    std::puts(
        "usage: revenant-webgate [--port N] [--http-port N] [--engine HOST:PORT]\n"
        "                        [--state-dir DIR] [--web-root DIR] [--token-file PATH]\n"
        "                        [--session-linger-ms N]\n"
        "  --port        HTTPS and WebSocket port (default 17700)\n"
        "  --http-port   plain HTTP port that serves the CA profile (default 17701)\n"
        "  --engine      the engine's Cap'n Proto address (default 127.0.0.1:17690)\n"
        "  --state-dir   certificates live here (default %LOCALAPPDATA%\\Revenant\\webgate)\n"
        "  --web-root    the web app (default: web beside the executable)\n"
        "  --token-file  the engine's token (default: the engine's own default path)\n"
        "  --session-linger-ms N\n"
        "                how long a phone's session outlives its dropped socket\n"
        "                (default 600000, 10 min); tests shorten it");
}

[[nodiscard]] bool parse_port(std::string_view text, int& out) {
    char* end = nullptr;
    const std::string copy(text);
    const long value = std::strtol(copy.c_str(), &end, 10);
    if (end == copy.c_str() || *end != '\0' || value < 0 || value > 65535) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

// "HOST:PORT", split at the last colon.
[[nodiscard]] bool parse_engine(std::string_view text, std::string& host, std::uint16_t& port) {
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0) {
        return false;
    }
    int value = 0;
    if (!parse_port(text.substr(colon + 1), value) || value == 0) {
        return false;
    }
    host = std::string(text.substr(0, colon));
    port = static_cast<std::uint16_t>(value);
    return true;
}

// Owns the three long-lived parts and tears them down in the one safe order,
// on every path out of main including an exception: the link first, so no
// state callback lands on a gateway being destroyed; then the gateway's
// sessions, while the server's HLS registry they borrow still exists; then
// the server, whose connections call back into the gateway as they go; then
// the gateway itself.
struct Runtime {
    std::unique_ptr<revenant::webgate::HttpServer> server;
    std::unique_ptr<revenant::webgate::EngineLink> link;
    std::unique_ptr<revenant::webgate::Gateway> gateway;

    Runtime() = default;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ~Runtime() {
        link.reset();
        if (gateway) {
            gateway->shutdown();
        }
        server.reset();
        gateway.reset();
    }
};

}  // namespace

int main(int argc, char** argv) {
    Options options;
    options.state_dir = default_state_dir();
    options.web_root = exe_directory() / "web";

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        }
        if (!has_value) {
            std::fprintf(stderr, "revenant-webgate: %s needs a value\n", argv[i]);
            return 2;
        }
        const std::string_view value = argv[++i];
        if (arg == "--port") {
            if (!parse_port(value, options.port)) {
                std::fprintf(stderr, "revenant-webgate: --port is not a port number\n");
                return 2;
            }
        } else if (arg == "--http-port") {
            if (!parse_port(value, options.http_port)) {
                std::fprintf(stderr, "revenant-webgate: --http-port is not a port number\n");
                return 2;
            }
        } else if (arg == "--engine") {
            options.engine = std::string(value);
        } else if (arg == "--state-dir") {
            options.state_dir = std::filesystem::path(std::string(value));
        } else if (arg == "--web-root") {
            options.web_root = std::filesystem::path(std::string(value));
        } else if (arg == "--token-file") {
            options.token_file = std::filesystem::path(std::string(value));
        } else if (arg == "--session-linger-ms") {
            char* end = nullptr;
            const std::string copy(value);
            const long ms = std::strtol(copy.c_str(), &end, 10);
            if (end == copy.c_str() || *end != '\0' || ms < 0) {
                std::fprintf(stderr, "revenant-webgate: --session-linger-ms is not a count\n");
                return 2;
            }
            options.session_linger_ms = ms;
        } else {
            std::fprintf(stderr, "revenant-webgate: unknown option %s\n", argv[i - 1]);
            usage();
            return 2;
        }
    }

    const revenant::webgate::CertPaths paths{options.state_dir / "certs"};
    const revenant::webgate::HostNames names = revenant::webgate::current_host_names();

    const auto issued = revenant::webgate::ensure_certificates(paths, names);
    if (!issued) {
        std::fprintf(stderr, "revenant-webgate: %s\n", issued.error().message.c_str());
        return 1;
    }

    std::printf("certificates: %s (%s)\n", paths.dir.string().c_str(),
                *issued ? "issued a new server certificate" : "unchanged");
    for (const std::string& san : revenant::webgate::certificate_sans(paths.server_crt())) {
        std::printf("  covers %s\n", san.c_str());
    }
    revenant::webgate::EngineLinkConfig link_config;
    if (!parse_engine(options.engine, link_config.address, link_config.port)) {
        std::fprintf(stderr, "revenant-webgate: --engine must be HOST:PORT\n");
        return 2;
    }
    // The spec has the gateway start its own engine when none is running.
    // EngineLink decides whether the address is one such an engine would be
    // reachable at; this only says where the executable is.
    link_config.engine_exe = exe_directory() / "revenant-engine.exe";
    link_config.token_path = options.token_file;

    revenant::webgate::HubTiming timing;
    if (options.session_linger_ms) {
        timing.detached_linger = std::chrono::milliseconds(*options.session_linger_ms);
    }

    try {
        auto io = kj::setupAsyncIo();

        revenant::webgate::ServerConfig server_config;
        server_config.https_port = static_cast<std::uint16_t>(options.port);
        server_config.http_port = static_cast<std::uint16_t>(options.http_port);
        server_config.web_root = options.web_root;
        server_config.certs = paths;
        Runtime rt;
        rt.server = std::make_unique<revenant::webgate::HttpServer>(io, server_config);
        if (const auto listening = rt.server->listen(); !listening) {
            std::fprintf(stderr, "revenant-webgate: %s\n", listening.error().message.c_str());
            return 1;
        }

        rt.link = std::make_unique<revenant::webgate::EngineLink>(link_config);
        auto* link = rt.link.get();
        rt.gateway = std::make_unique<revenant::webgate::Gateway>(
            io, [link] { return link->new_session(); }, rt.server->hls, timing);
        auto* gateway = rt.gateway.get();
        link->on_state([gateway](revenant::webgate::LinkState state, std::string detail) {
            gateway->on_link_state(state, std::move(detail));
        });
        rt.server->on_websocket = [gateway](kj::Own<kj::WebSocket> socket) {
            return gateway->accept(kj::mv(socket));
        };
        link->start();

        std::printf("https port %u, http port %u, engine %s, web root %s\n",
                    static_cast<unsigned>(rt.server->https_port()),
                    static_cast<unsigned>(rt.server->http_port()), options.engine.c_str(),
                    options.web_root.string().c_str());
        std::fflush(stdout);

        rt.server->run().exclusiveJoin(gateway->run()).wait(io.waitScope);
    } catch (const kj::Exception& e) {
        std::fprintf(stderr, "revenant-webgate: %s\n", e.getDescription().cStr());
        return 1;
    }
    return 0;
}
