// HttpServer over real sockets on loopback, with a kj-tls client that trusts
// only the gateway's own CA, so a certificate that did not chain to it would
// fail the handshake here as it would on the phone.

// No <windows.h> here: its DELETE macro breaks kj's HttpMethod enum.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/compat/tls.h>

#include "tools/webgate/certs.h"
#include "tools/webgate/http.h"

namespace {

using revenant::webgate::CertPaths;
using revenant::webgate::HostNames;
using revenant::webgate::HttpServer;
using revenant::webgate::ServerConfig;

class Fixture {
public:
    Fixture() {
        static std::atomic<unsigned> counter{0};
        base_ = std::filesystem::temp_directory_path() /
                ("revenant-webgate-http-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(counter.fetch_add(1)));
        std::filesystem::remove_all(base_);
        std::filesystem::create_directories(base_ / "web");
        std::ofstream(base_ / "web" / "index.html", std::ios::binary) << "<html>app</html>";
        certs_ = CertPaths{base_ / "certs"};
        REQUIRE(revenant::webgate::ensure_certificates(certs_, HostNames{"radio", {"127.0.0.1"}})
                    .has_value());
    }
    ~Fixture() {
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    [[nodiscard]] ServerConfig config() const {
        ServerConfig cfg;
        cfg.https_port = 0;
        cfg.http_port = 0;
        cfg.web_root = base_ / "web";
        cfg.certs = certs_;
        return cfg;
    }
    [[nodiscard]] const CertPaths& certs() const { return certs_; }

private:
    std::filesystem::path base_;
    CertPaths certs_;
};

[[nodiscard]] std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

struct Reply {
    unsigned status = 0;
    std::string content_type;
    std::string location;
    std::string body;
};

// Fixed bytes: a test client has no use for real masking entropy.
class FixedEntropy final : public kj::EntropySource {
public:
    void generate(kj::ArrayPtr<kj::byte> buffer) override {
        for (auto& b : buffer) {
            b = 0x5a;
        }
    }
};

[[nodiscard]] kj::Promise<Reply> fetch(kj::HttpClient& client, kj::HttpHeaderTable& table,
                                       kj::StringPtr path) {
    kj::HttpHeaders headers(table);
    headers.set(kj::HttpHeaderId::HOST, "radio");
    auto request = client.request(kj::HttpMethod::GET, path, headers);
    return request.response.then([](kj::HttpClient::Response&& response) {
        Reply reply;
        reply.status = response.statusCode;
        KJ_IF_MAYBE(type, response.headers->get(kj::HttpHeaderId::CONTENT_TYPE)) {
            reply.content_type = type->cStr();
        }
        KJ_IF_MAYBE(location, response.headers->get(kj::HttpHeaderId::LOCATION)) {
            reply.location = location->cStr();
        }
        auto body = response.body->readAllText();
        return body.then([reply = kj::mv(reply)](kj::String text) mutable {
            reply.body.assign(text.begin(), text.size());
            return kj::mv(reply);
        }).attach(kj::mv(response.body));
    });
}

}  // namespace

TEST_CASE("serves over TLS and redirects plain HTTP", "[webgate][http]") {
    Fixture fixture;
    kj::AsyncIoContext io = kj::setupAsyncIo();
    HttpServer server(io, fixture.config());
    REQUIRE(server.listen().has_value());
    REQUIRE(server.https_port() != 0);
    REQUIRE(server.http_port() != 0);
    auto running = server.run().eagerlyEvaluate([](kj::Exception&&) {});

    kj::HttpHeaderTable table;
    auto& network = io.provider->getNetwork();

    SECTION("HTTPS GET / returns the app") {
        const std::string ca_pem = slurp(fixture.certs().ca_crt());
        kj::TlsContext::Options options;
        options.useSystemTrustStore = false;
        auto builder = kj::heapArrayBuilder<kj::TlsCertificate>(1);
        builder.add(kj::StringPtr(ca_pem.c_str()));
        kj::Array<kj::TlsCertificate> trusted = builder.finish();
        options.trustedCertificates = trusted;
        kj::TlsContext tls(kj::mv(options));

        auto address =
            network.parseAddress("127.0.0.1", server.https_port()).wait(io.waitScope);
        auto stream = address->connect().wait(io.waitScope);
        auto secure = tls.wrapClient(kj::mv(stream), "radio").wait(io.waitScope);
        auto client = kj::newHttpClient(table, *secure);

        const Reply reply = fetch(*client, table, "/").wait(io.waitScope);
        CHECK(reply.status == 200);
        CHECK(reply.content_type == "text/html; charset=utf-8");
        CHECK(reply.body == "<html>app</html>");
    }

    SECTION("plain HTTP redirects to HTTPS") {
        auto address =
            network.parseAddress("127.0.0.1", server.http_port()).wait(io.waitScope);
        auto stream = address->connect().wait(io.waitScope);
        auto client = kj::newHttpClient(table, *stream);

        const Reply reply = fetch(*client, table, "/x").wait(io.waitScope);
        CHECK(reply.status == 301);
        CHECK(reply.location ==
              "https://radio:" + std::to_string(server.https_port()) + "/x");
    }

    SECTION("a hostile Host header is not reflected into Location") {
        auto address =
            network.parseAddress("127.0.0.1", server.http_port()).wait(io.waitScope);
        auto stream = address->connect().wait(io.waitScope);
        const kj::StringPtr request =
            "GET /x HTTP/1.1\r\nHost: evil.com/ x\r\nConnection: close\r\n\r\n";
        stream->write(request.begin(), request.size()).wait(io.waitScope);
        std::string text;
        char buffer[512];
        for (;;) {
            const size_t got = stream->tryRead(buffer, 1, sizeof(buffer)).wait(io.waitScope);
            if (got == 0) {
                break;
            }
            text.append(buffer, got);
        }
        CHECK(text.rfind("HTTP/1.1 400", 0) == 0);
        CHECK(text.find("Location") == std::string::npos);
        CHECK(text.find("evil.com") == std::string::npos);
    }

    SECTION("/ca returns the profile") {
        auto address =
            network.parseAddress("127.0.0.1", server.http_port()).wait(io.waitScope);
        auto stream = address->connect().wait(io.waitScope);
        auto client = kj::newHttpClient(table, *stream);

        const Reply reply = fetch(*client, table, "/ca").wait(io.waitScope);
        CHECK(reply.status == 200);
        CHECK(reply.content_type == "application/x-apple-aspen-config");
        CHECK(reply.body.find("<plist") != std::string::npos);
    }
}

TEST_CASE("destroying the server with connections open is safe", "[webgate][http]") {
    Fixture fixture;
    kj::AsyncIoContext io = kj::setupAsyncIo();
    kj::HttpHeaderTable table;
    auto& network = io.provider->getNetwork();

    // Declared before the server so they are still open when it is destroyed.
    kj::Own<kj::AsyncIoStream> keepalive_stream;
    kj::Own<kj::AsyncIoStream> silent;
    kj::Own<kj::HttpClient> keepalive;

    {
        HttpServer server(io, fixture.config());
        REQUIRE(server.listen().has_value());
        auto running = server.run().eagerlyEvaluate([](kj::Exception&&) {});

        auto address =
            network.parseAddress("127.0.0.1", server.http_port()).wait(io.waitScope);
        keepalive_stream = address->connect().wait(io.waitScope);
        keepalive = kj::newHttpClient(table, *keepalive_stream);
        CHECK(fetch(*keepalive, table, "/").wait(io.waitScope).status == 200);

        // A client stalled before its TLS hello holds an accept task open too.
        auto silent_address =
            network.parseAddress("127.0.0.1", server.https_port()).wait(io.waitScope);
        silent = silent_address->connect().wait(io.waitScope);
        io.waitScope.poll();
    }
    SUCCEED("server destroyed with a keep-alive connection and a pending handshake");
}

TEST_CASE("the ws endpoint refuses a foreign Origin", "[webgate][http]") {
    Fixture fixture;
    kj::AsyncIoContext io = kj::setupAsyncIo();
    HttpServer server(io, fixture.config());
    server.on_websocket = [](kj::Own<kj::WebSocket> ws) {
        // Stay open until the client goes away, or the reply races the close.
        auto* raw = ws.get();
        return raw->send(kj::StringPtr("hello"))
            .then([raw]() { return raw->receive(); })
            .then([](kj::WebSocket::Message&&) {})
            .catch_([](kj::Exception&&) {})
            .attach(kj::mv(ws));
    };
    REQUIRE(server.listen().has_value());
    auto running = server.run().eagerlyEvaluate([](kj::Exception&&) {});

    const std::string ca_pem = slurp(fixture.certs().ca_crt());
    kj::TlsContext::Options options;
    options.useSystemTrustStore = false;
    auto builder = kj::heapArrayBuilder<kj::TlsCertificate>(1);
    builder.add(kj::StringPtr(ca_pem.c_str()));
    kj::Array<kj::TlsCertificate> trusted = builder.finish();
    options.trustedCertificates = trusted;
    kj::TlsContext tls(kj::mv(options));

    kj::HttpHeaderTable table;
    auto& network = io.provider->getNetwork();

    FixedEntropy entropy;
    const auto open = [&](const char* origin) {
        auto address =
            network.parseAddress("127.0.0.1", server.https_port()).wait(io.waitScope);
        auto stream = address->connect().wait(io.waitScope);
        auto secure = tls.wrapClient(kj::mv(stream), "radio").wait(io.waitScope);
        kj::HttpClientSettings client_settings;
        client_settings.entropySource = entropy;
        auto client = kj::newHttpClient(table, *secure, client_settings);
        kj::HttpHeaders headers(table);
        headers.set(kj::HttpHeaderId::HOST, "radio");
        if (origin != nullptr) {
            headers.add("Origin", origin);
        }
        auto response = client->openWebSocket("/ws", headers).wait(io.waitScope);
        struct Result {
            unsigned status;
            std::string first_message;
        } result{response.statusCode, {}};
        if (response.webSocketOrBody.is<kj::Own<kj::WebSocket>>()) {
            auto& ws = response.webSocketOrBody.get<kj::Own<kj::WebSocket>>();
            auto message = ws->receive().wait(io.waitScope);
            if (message.is<kj::String>()) {
                result.first_message = message.get<kj::String>().cStr();
            }
        }
        return result;
    };

    const std::string own =
        "https://radio:" + std::to_string(server.https_port());
    const auto matching = open(own.c_str());
    CHECK(matching.status == 101);
    CHECK(matching.first_message == "hello");

    const auto none = open(nullptr);
    CHECK(none.status == 101);

    CHECK(open("https://evil.example").status == 403);
    CHECK(open("http://radio").status == 403);
    CHECK(open(("https://radio:" + std::to_string(server.https_port() + 1)).c_str()).status ==
          403);
    CHECK(open("null").status == 403);
}

TEST_CASE("serves HLS from the registry", "[webgate][http][hls]") {
    Fixture fixture;
    kj::AsyncIoContext io = kj::setupAsyncIo();
    HttpServer server(io, fixture.config());
    revenant::webgate::HlsSegmenter segmenter(48000);
    REQUIRE(segmenter.ok());
    for (int i = 0; i < 4 * 48; ++i) {
        segmenter.push(std::vector<float>(1000, 0.1F));
    }
    server.hls.set(7, &segmenter);
    REQUIRE(server.listen().has_value());
    auto running = server.run().eagerlyEvaluate([](kj::Exception&&) {});

    const std::string ca_pem = slurp(fixture.certs().ca_crt());
    kj::TlsContext::Options options;
    options.useSystemTrustStore = false;
    auto builder = kj::heapArrayBuilder<kj::TlsCertificate>(1);
    builder.add(kj::StringPtr(ca_pem.c_str()));
    kj::Array<kj::TlsCertificate> trusted = builder.finish();
    options.trustedCertificates = trusted;
    kj::TlsContext tls(kj::mv(options));

    kj::HttpHeaderTable table;
    auto address =
        io.provider->getNetwork().parseAddress("127.0.0.1", server.https_port()).wait(io.waitScope);
    auto stream = address->connect().wait(io.waitScope);
    auto secure = tls.wrapClient(kj::mv(stream), "radio").wait(io.waitScope);
    auto client = kj::newHttpClient(table, *secure);
    const auto get = [&](kj::StringPtr path) { return fetch(*client, table, path).wait(io.waitScope); };

    const Reply playlist = get("/hls/7/live.m3u8");
    CHECK(playlist.status == 200);
    CHECK(playlist.content_type == "application/vnd.apple.mpegurl");
    CHECK(playlist.body.rfind("#EXTM3U", 0) == 0);

    const Reply init = get("/hls/7/init.mp4");
    CHECK(init.status == 200);
    CHECK(init.content_type == "video/mp4");
    CHECK(init.body.find("ftyp") == 4);

    const Reply media = get("/hls/7/0.m4s");
    CHECK(media.status == 200);
    CHECK(media.content_type == "video/mp4");
    CHECK(media.body.find("moof") == 4);

    CHECK(get("/hls/8/live.m3u8").status == 404);
    CHECK(get("/hls/7/999.m4s").status == 404);
    CHECK(get("/hls/7/-1.m4s").status == 404);
    CHECK(get("/hls/7/../x").status == 404);
    CHECK(get("/hls/7/live.m3u8x").status == 404);
}
