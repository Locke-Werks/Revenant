#include "tools/webgate/http.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <kj/compat/tls.h>

#include "core/rpc/listen.h"
#include "tools/webgate/static_files.h"

namespace revenant::webgate {
namespace {

constexpr std::string_view kSetupPage =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Revenant setup</title></head><body>"
    "<h1>Revenant</h1>"
    "<p>1. <a href=\"/ca\">Download the certificate profile</a> and allow it.</p>"
    "<p>2. Settings, General, VPN &amp; Device Management: install the profile.</p>"
    "<p>3. Settings, General, About, Certificate Trust Settings: turn on full trust.</p>"
    "<p>4. Reload this address; it moves to HTTPS.</p>"
    "</body></html>";

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

[[nodiscard]] kj::StringPtr status_text(unsigned code) {
    switch (code) {
        case 200: return "OK";
        case 301: return "Moved Permanently";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 503: return "Service Unavailable";
        default: return "Error";
    }
}

[[nodiscard]] std::string_view path_of(kj::StringPtr url) {
    std::string_view view(url.cStr(), url.size());
    return view.substr(0, view.find_first_of("?#"));
}

kj::Promise<void> respond(kj::HttpService::Response& response, const kj::HttpHeaderTable& table,
                          unsigned status, std::string_view content_type, std::string body,
                          std::string_view cache_control = {},
                          std::string_view location = {}) {
    kj::HttpHeaders headers(table);
    if (!content_type.empty()) {
        headers.set(kj::HttpHeaderId::CONTENT_TYPE, kj::heapString(content_type.data(), content_type.size()));
    }
    if (!cache_control.empty()) {
        headers.add("Cache-Control", kj::heapString(cache_control.data(), cache_control.size()));
    }
    if (!location.empty()) {
        headers.set(kj::HttpHeaderId::LOCATION, kj::heapString(location.data(), location.size()));
    }
    auto held = kj::heap<std::string>(std::move(body));
    auto stream = response.send(status, status_text(status), headers, held->size());
    auto written = stream->write(held->data(), held->size());
    return written.attach(kj::mv(stream), kj::mv(held));
}

// The host part of a Host header, without its port: "radio.local:17701" and
// "[name]:17701" both reduce to what a Location needs before its own port.
// Empty for anything that is not a plausible host, which keeps a crafted header
// out of the redirect.
[[nodiscard]] std::string host_only(std::string_view host) {
    const std::size_t colon = host.find(':');
    if (colon != std::string_view::npos) {
        host = host.substr(0, colon);
    }
    if (host.empty()) {
        return {};
    }
    for (const char c : host) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (!ok) {
            return {};
        }
    }
    return std::string(host);
}

}  // namespace

struct HttpServer::Impl final : kj::TaskSet::ErrorHandler {
    Impl(kj::AsyncIoContext& io_context, ServerConfig cfg, HttpServer& owner_ref)
        : io(io_context), config(std::move(cfg)), owner(owner_ref), tasks(*this) {}

    // A failed handshake is a phone that does not trust the certificate yet, or
    // a scanner. Neither is the server's problem.
    void taskFailed(kj::Exception&&) override {}

    // Serves https.
    class SecureService final : public kj::HttpService {
    public:
        explicit SecureService(Impl& impl_ref) : impl(impl_ref) {}

        kj::Promise<void> request(kj::HttpMethod method, kj::StringPtr url,
                                  const kj::HttpHeaders& headers,
                                  kj::AsyncInputStream&, Response& response) override {
            const std::string_view path = path_of(url);
            if (path == "/ws") {
                if (!headers.isWebSocket()) {
                    return respond(response, impl.table, 400, "text/plain; charset=utf-8",
                                   "websocket required\n");
                }
                if (!impl.owner.on_websocket) {
                    return respond(response, impl.table, 503, "text/plain; charset=utf-8",
                                   "no session handler\n");
                }
                if (!impl.origin_allowed(headers)) {
                    return respond(response, impl.table, 403, "text/plain; charset=utf-8",
                                   "origin not allowed\n");
                }
                kj::HttpHeaders out(impl.table);
                return impl.owner.on_websocket(response.acceptWebSocket(out));
            }
            if (method != kj::HttpMethod::GET && method != kj::HttpMethod::HEAD) {
                return respond(response, impl.table, 405, "text/plain; charset=utf-8",
                               "method not allowed\n");
            }
            // Never falls through to the file server, found or not.
            if (path.rfind("/hls/", 0) == 0) {
                return serve_hls(path.substr(5), response);
            }
            StaticResponse file = serve_static(impl.config.web_root, path);
            return respond(response, impl.table, static_cast<unsigned>(file.status),
                           file.content_type, std::move(file.body), file.cache_control);
        }

    private:
        // `rest` is what follows "/hls/": "<vrx>/live.m3u8", "<vrx>/init.mp4"
        // or "<vrx>/<seq>.m4s". The numbers are parsed strictly so nothing
        // else, a "..", a sign, a stray suffix, reaches the lookup.
        kj::Promise<void> serve_hls(std::string_view rest, Response& response) {
            const auto not_found = [&] {
                return respond(response, impl.table, 404, "text/plain; charset=utf-8",
                               "not found\n", "no-cache");
            };
            const std::size_t slash = rest.find('/');
            if (slash == std::string_view::npos) {
                return not_found();
            }
            const auto vrx = parse_number(rest.substr(0, slash));
            const std::string_view name = rest.substr(slash + 1);
            if (!vrx) {
                return not_found();
            }
            HlsSegmenter* segmenter = impl.owner.hls.find(*vrx);
            if (segmenter == nullptr) {
                return not_found();
            }

            // Playlist and segments change under the player's feet in a live
            // stream, so none of it may be cached.
            if (name == "live.m3u8") {
                std::string text = segmenter->playlist();
                if (text.empty()) {
                    return not_found();
                }
                return respond(response, impl.table, 200, "application/vnd.apple.mpegurl",
                               std::move(text), "no-cache");
            }
            if (name == "init.mp4") {
                if (!segmenter->ok()) {
                    return not_found();
                }
                const auto init = segmenter->init_segment();
                return respond(response, impl.table, 200, "video/mp4",
                               std::string(reinterpret_cast<const char*>(init.data()), init.size()),
                               "no-cache");
            }
            constexpr std::string_view suffix = ".m4s";
            if (name.size() > suffix.size() && name.ends_with(suffix)) {
                const auto sequence = parse_number(name.substr(0, name.size() - suffix.size()));
                if (sequence) {
                    if (const auto segment = segmenter->segment(*sequence)) {
                        return respond(
                            response, impl.table, 200, "video/mp4",
                            std::string(reinterpret_cast<const char*>(segment->data()),
                                        segment->size()),
                            "no-cache");
                    }
                }
            }
            return not_found();
        }

        [[nodiscard]] static std::optional<std::uint64_t> parse_number(std::string_view text) {
            std::uint64_t value = 0;
            if (text.empty() || text.size() > 19) {
                return std::nullopt;
            }
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (ec != std::errc{} || end != text.data() + text.size()) {
                return std::nullopt;
            }
            return value;
        }

        Impl& impl;
    };

    // Serves plain http.
    class PlainService final : public kj::HttpService {
    public:
        explicit PlainService(Impl& impl_ref) : impl(impl_ref) {}

        kj::Promise<void> request(kj::HttpMethod method, kj::StringPtr url,
                                  const kj::HttpHeaders& headers,
                                  kj::AsyncInputStream&, Response& response) override {
            if (method != kj::HttpMethod::GET && method != kj::HttpMethod::HEAD) {
                return respond(response, impl.table, 405, "text/plain; charset=utf-8",
                               "method not allowed\n");
            }
            const std::string_view path = path_of(url);
            if (path == "/ca") {
                auto profile = mobileconfig(impl.config.certs);
                if (!profile) {
                    return respond(response, impl.table, 503, "text/plain; charset=utf-8",
                                   profile.error().message + "\n");
                }
                return respond(response, impl.table, 200, "application/x-apple-aspen-config",
                               std::move(*profile), "no-cache");
            }
            if (path == "/") {
                return respond(response, impl.table, 200, "text/html; charset=utf-8",
                               std::string(kSetupPage), "no-cache");
            }

            std::string host;
            KJ_IF_MAYBE(value, headers.get(kj::HttpHeaderId::HOST)) {
                host = host_only(std::string_view(value->cStr(), value->size()));
            }
            if (host.empty() || path.empty() || path.front() != '/') {
                return respond(response, impl.table, 400, "text/plain; charset=utf-8",
                               "bad request\n");
            }
            return respond(response, impl.table, 301, "text/plain; charset=utf-8", "",
                           "no-cache",
                           std::format("https://{}:{}{}", host, impl.https_port,
                                       std::string_view(url.cStr(), url.size())));
        }

    private:
        Impl& impl;
    };

    // A browser sends Origin on every WebSocket upgrade and a script cannot
    // forge it, so this is what stops an arbitrary page in a LAN browser from
    // opening a session. Absent means not a browser (curl, a native client).
    // Present must be https, on this server's port, at a name the certificate
    // carries.
    [[nodiscard]] bool origin_allowed(const kj::HttpHeaders& headers) const {
        std::optional<std::string> origin;
        headers.forEach([&](kj::StringPtr name, kj::StringPtr value) {
            std::string key(name.cStr(), name.size());
            std::ranges::transform(key, key.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (key == "origin") {
                origin = std::string(value.cStr(), value.size());
            }
        });
        if (!origin) {
            return true;
        }
        constexpr std::string_view scheme = "https://";
        if (origin->rfind(scheme, 0) != 0) {
            return false;
        }
        std::string authority = origin->substr(scheme.size());
        unsigned port = 443;
        if (const std::size_t colon = authority.find(':'); colon != std::string::npos) {
            const std::string digits = authority.substr(colon + 1);
            if (digits.empty() || digits.size() > 5 ||
                !std::ranges::all_of(digits, [](unsigned char c) { return std::isdigit(c); })) {
                return false;
            }
            port = static_cast<unsigned>(std::stoul(digits));
            authority.resize(colon);
        }
        std::ranges::transform(authority, authority.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return port == https_port &&
               std::ranges::find(origin_hosts, authority) != origin_hosts.end();
    }

    kj::Promise<void> accept_secure() {
        return secure_receiver->accept().then([this](kj::Own<kj::AsyncIoStream> stream) {
            tasks.add(tls->wrapServer(kj::mv(stream))
                          .then([this](kj::Own<kj::AsyncIoStream> secure) {
                              return secure_server->listenHttp(kj::mv(secure));
                          }));
            return accept_secure();
        });
    }

    kj::Promise<void> accept_plain() {
        return plain_receiver->accept().then([this](kj::Own<kj::AsyncIoStream> stream) {
            tasks.add(plain_server->listenHttp(kj::mv(stream)));
            return accept_plain();
        });
    }

    kj::AsyncIoContext& io;
    ServerConfig config;
    HttpServer& owner;
    kj::HttpHeaderTable table;
    bool listening = false;
    // Lowercased SAN names the certificate answers to; an Origin must be one.
    std::vector<std::string> origin_hosts;
    std::uint16_t https_port = 0;
    std::uint16_t http_port = 0;

    // Members are destroyed in reverse declaration order, so this list runs
    // from what outlives everything to what goes first. `tasks` is last on
    // purpose: live connections and handshakes touch the servers, receivers
    // and TLS context on teardown, so they have to be gone before those are.
    // The key material sits first because the TLS context points at it.
    kj::Own<kj::TlsKeypair> keypair;
    kj::Own<kj::TlsContext> tls;
    kj::Own<kj::ConnectionReceiver> secure_receiver;
    kj::Own<kj::ConnectionReceiver> plain_receiver;
    SecureService secure_service{*this};
    PlainService plain_service{*this};
    kj::Own<kj::HttpServer> secure_server;
    kj::Own<kj::HttpServer> plain_server;
    kj::TaskSet tasks;
};

HttpServer::HttpServer(kj::AsyncIoContext& io, ServerConfig config)
    : impl_(std::make_unique<Impl>(io, std::move(config), *this)) {}

HttpServer::~HttpServer() = default;

Status HttpServer::listen() {
    Impl& s = *impl_;
    if (s.listening) {
        return {};
    }

    const std::string key_pem = read_text(s.config.certs.server_key());
    const std::string crt_pem = read_text(s.config.certs.server_crt());
    if (key_pem.empty() || crt_pem.empty()) {
        return fail(std::format("the TLS identity is missing: {} and {} must both exist",
                                s.config.certs.server_key().string(),
                                s.config.certs.server_crt().string()));
    }
    try {
        s.keypair = kj::heap<kj::TlsKeypair>(
            kj::TlsKeypair{kj::TlsPrivateKey(kj::StringPtr(key_pem.c_str())),
                           kj::TlsCertificate(kj::StringPtr(crt_pem.c_str()))});
        kj::TlsContext::Options options;
        options.defaultKeypair = *s.keypair;
        options.minVersion = kj::TlsVersion::TLS_1_2;
        // A client that connects and says nothing must not hold a slot.
        options.timer = s.io.provider->getTimer();
        options.acceptTimeout = 10 * kj::SECONDS;
        s.tls = kj::heap<kj::TlsContext>(kj::mv(options));
    } catch (const kj::Exception& e) {
        return fail(std::format("could not load the TLS identity: {}", e.getDescription().cStr()));
    }

    auto secure = rpc::listen_exclusive(*s.io.lowLevelProvider, s.config.bind_address,
                                        s.config.https_port);
    if (!secure) {
        return std::unexpected(secure.error());
    }
    auto plain = rpc::listen_exclusive(*s.io.lowLevelProvider, s.config.bind_address,
                                       s.config.http_port);
    if (!plain) {
        return std::unexpected(plain.error());
    }
    s.secure_receiver = kj::mv(*secure);
    s.plain_receiver = kj::mv(*plain);
    s.https_port = static_cast<std::uint16_t>(s.secure_receiver->getPort());
    s.http_port = static_cast<std::uint16_t>(s.plain_receiver->getPort());

    kj::HttpServerSettings settings;
    settings.webSocketCompressionMode = kj::HttpServerSettings::NO_COMPRESSION;
    s.secure_server = kj::heap<kj::HttpServer>(s.io.provider->getTimer(), s.table,
                                               s.secure_service, settings);
    s.plain_server = kj::heap<kj::HttpServer>(s.io.provider->getTimer(), s.table,
                                              s.plain_service, kj::HttpServerSettings{});
    s.origin_hosts = certificate_sans(s.config.certs.server_crt());
    for (std::string& host : s.origin_hosts) {
        std::ranges::transform(host, host.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    }
    s.listening = true;
    return {};
}

std::uint16_t HttpServer::https_port() const {
    return impl_->https_port;
}

std::uint16_t HttpServer::http_port() const {
    return impl_->http_port;
}

kj::Promise<void> HttpServer::run() {
    if (auto status = listen(); !status) {
        KJ_FAIL_REQUIRE(status.error().message.c_str());
    }
    return impl_->accept_secure().exclusiveJoin(impl_->accept_plain());
}

}  // namespace revenant::webgate
