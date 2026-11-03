// revenant-webgate: the HTTPS and WebSocket gateway an iPhone web app uses to
// drive the engine over the LAN.
//
// This stage only settles the TLS identity: it parses the command line, makes
// sure the certificate authority and server certificate exist, prints what the
// certificate covers, and exits. The listeners come in later stages.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include <windows.h>

#include "tools/webgate/certs.h"

namespace {

struct Options {
    int port = 17700;
    int http_port = 17701;
    std::string engine = "127.0.0.1:17690";
    std::filesystem::path state_dir;
    std::filesystem::path web_root;
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
        "                        [--state-dir DIR] [--web-root DIR]\n"
        "  --port        HTTPS and WebSocket port (default 17700)\n"
        "  --http-port   plain HTTP port that serves the CA profile (default 17701)\n"
        "  --engine      the engine's Cap'n Proto address (default 127.0.0.1:17690)\n"
        "  --state-dir   certificates live here (default %LOCALAPPDATA%\\Revenant\\webgate)\n"
        "  --web-root    the web app (default: web beside the executable)");
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
    std::printf("https port %d, http port %d, engine %s, web root %s\n", options.port,
                options.http_port, options.engine.c_str(), options.web_root.string().c_str());
    std::fflush(stdout);
    return 0;
}
