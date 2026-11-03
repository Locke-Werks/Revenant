// The gateway's two listeners: HTTPS for the web app and its WebSocket, and a
// plain HTTP port whose only jobs are handing out the CA profile and sending
// everything else to HTTPS.
//
// Both listeners are made with core/rpc/listen.cpp's exclusive bind rather
// than kj's listen(). On Windows kj sets SO_REUSEADDR, which lets a second
// gateway bind a port the first is already serving and report success; the
// reasoning is in core/rpc/listen.h. kj::HttpServer takes any
// kj::ConnectionReceiver, so a receiver wrapped around that socket works as is.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

// Do not include <windows.h> before this header: its DELETE macro breaks
// kj::HttpMethod.
#include <kj/async-io.h>
#include <kj/compat/http.h>

#include "core/error.h"
#include "tools/webgate/certs.h"

namespace revenant::webgate {

struct ServerConfig {
    // Zero asks for any free port, which tests use and production does not.
    std::uint16_t https_port = 17700;
    std::uint16_t http_port = 17701;
    std::filesystem::path web_root;
    CertPaths certs;
    // IPv4 only: webgate does not use IPv6.
    std::string bind_address = "0.0.0.0";
};

class HttpServer {
public:
    HttpServer(kj::AsyncIoContext& io, ServerConfig config);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Loads the TLS identity and binds both ports. Separate from the
    // constructor because either can fail for reasons an operator acts on (a
    // port in use, a certificate missing) and a constructor cannot return that.
    // Idempotent once it has succeeded.
    [[nodiscard]] Status listen();

    // Valid after listen() has succeeded; zero before.
    [[nodiscard]] std::uint16_t https_port() const;
    [[nodiscard]] std::uint16_t http_port() const;

    // Accepts connections until cancelled or a listener fails. Calls listen()
    // first if that has not been done, and throws if it fails.
    [[nodiscard]] kj::Promise<void> run();

    // Handles one accepted WebSocket on /ws; the connection lives until the
    // returned promise settles. Set before run(). Without it /ws answers 503.
    std::function<kj::Promise<void>(kj::Own<kj::WebSocket>)> on_websocket;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace revenant::webgate
