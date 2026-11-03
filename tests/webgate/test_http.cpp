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
            reply.body = text.cStr();
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
        CHECK(reply.location.rfind("https://", 0) == 0);
        CHECK(reply.location.find("/x") != std::string::npos);
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
