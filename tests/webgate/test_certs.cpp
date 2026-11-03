// The gateway's certificate authority and the server certificate it issues.
//
// No network and no engine. Every case works in a directory of its own under
// the system temp path, removed afterwards, so cases cannot see each other's
// keys and none of them touches a real profile.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "tools/webgate/certs.h"

namespace {

using revenant::webgate::CertPaths;
using revenant::webgate::HostNames;

class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("revenant-webgate-test-" + std::to_string(GetCurrentProcessId()) + "-" +
                 std::to_string(counter.fetch_add(1)));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] CertPaths paths() const { return CertPaths{path_ / "certs"}; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

[[nodiscard]] X509* load_cert(const std::filesystem::path& path) {
    const std::string text = slurp(path);
    BIO* bio = BIO_new_mem_buf(text.data(), static_cast<int>(text.size()));
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return cert;
}

[[nodiscard]] std::vector<std::string> sorted(std::vector<std::string> v) {
    std::ranges::sort(v);
    return v;
}

}  // namespace

TEST_CASE("first run creates a CA and a server certificate", "[webgate][certs]") {
    TempDir dir;
    const CertPaths paths = dir.paths();

    const auto issued = revenant::webgate::ensure_certificates(paths, HostNames{"radio", {}});
    REQUIRE(issued.has_value());
    CHECK(*issued);
    CHECK(std::filesystem::exists(paths.ca_key()));
    CHECK(std::filesystem::exists(paths.ca_crt()));
    CHECK(std::filesystem::exists(paths.server_key()));
    CHECK(std::filesystem::exists(paths.server_crt()));
}

TEST_CASE("SANs cover hostname, .local and every address", "[webgate][certs]") {
    TempDir dir;
    const CertPaths paths = dir.paths();

    REQUIRE(revenant::webgate::ensure_certificates(
                paths, HostNames{"radio", {"192.168.1.20", "fe80::1"}})
                .has_value());

    const auto sans = sorted(revenant::webgate::certificate_sans(paths.server_crt()));
    const auto expected = sorted({"radio", "radio.local", "192.168.1.20", "fe80::1"});
    CHECK(sans == expected);
}

TEST_CASE("unchanged names do not reissue", "[webgate][certs]") {
    TempDir dir;
    const CertPaths paths = dir.paths();
    const HostNames names{"radio", {"192.168.1.20"}};

    REQUIRE(revenant::webgate::ensure_certificates(paths, names).value());
    const std::string before = slurp(paths.server_crt());

    const auto again = revenant::webgate::ensure_certificates(paths, names);
    REQUIRE(again.has_value());
    CHECK_FALSE(*again);
    CHECK(slurp(paths.server_crt()) == before);
}

TEST_CASE("changed address reissues from the same CA", "[webgate][certs]") {
    TempDir dir;
    const CertPaths paths = dir.paths();

    REQUIRE(revenant::webgate::ensure_certificates(paths, HostNames{"radio", {"192.168.1.20"}})
                .value());
    const std::string ca_before = slurp(paths.ca_crt());
    const std::string server_before = slurp(paths.server_crt());

    const auto again =
        revenant::webgate::ensure_certificates(paths, HostNames{"radio", {"192.168.1.21"}});
    REQUIRE(again.has_value());
    CHECK(*again);
    CHECK(slurp(paths.ca_crt()) == ca_before);
    CHECK(slurp(paths.server_crt()) != server_before);

    X509* ca = load_cert(paths.ca_crt());
    X509* server = load_cert(paths.server_crt());
    REQUIRE(ca != nullptr);
    REQUIRE(server != nullptr);

    X509_STORE* store = X509_STORE_new();
    X509_STORE_add_cert(store, ca);
    X509_STORE_CTX* ctx = X509_STORE_CTX_new();
    X509_STORE_CTX_init(ctx, store, server, nullptr);
    CHECK(X509_verify_cert(ctx) == 1);
    X509_STORE_CTX_free(ctx);
    X509_STORE_free(store);
    X509_free(server);
    X509_free(ca);
}

TEST_CASE("the server certificate is valid for at most 825 days", "[webgate][certs]") {
    TempDir dir;
    const CertPaths paths = dir.paths();
    REQUIRE(revenant::webgate::ensure_certificates(paths, HostNames{"radio", {}}).has_value());

    X509* server = load_cert(paths.server_crt());
    REQUIRE(server != nullptr);
    int days = 0;
    int seconds = 0;
    REQUIRE(ASN1_TIME_diff(&days, &seconds, X509_get0_notBefore(server),
                           X509_get0_notAfter(server)) == 1);
    X509_free(server);
    CHECK(days == 825);
    CHECK(seconds == 0);
}

TEST_CASE("a server key that does not match the certificate is reissued", "[webgate][certs]") {
    TempDir dir;
    const CertPaths paths = dir.paths();
    const HostNames names{"radio", {"192.168.1.20"}};
    REQUIRE(revenant::webgate::ensure_certificates(paths, names).value());

    // The state a crash between writing server.key and server.crt leaves.
    EVP_PKEY* stray = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    REQUIRE(stray != nullptr);
    BIO* bio = BIO_new(BIO_s_mem());
    REQUIRE(PEM_write_bio_PrivateKey(bio, stray, nullptr, nullptr, 0, nullptr, nullptr) == 1);
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    {
        std::ofstream out(paths.server_key(), std::ios::binary | std::ios::trunc);
        out.write(data, length);
    }
    BIO_free(bio);
    EVP_PKEY_free(stray);

    const auto again = revenant::webgate::ensure_certificates(paths, names);
    REQUIRE(again.has_value());
    CHECK(*again);

    X509* server = load_cert(paths.server_crt());
    REQUIRE(server != nullptr);
    const std::string key_text = slurp(paths.server_key());
    BIO* key_bio = BIO_new_mem_buf(key_text.data(), static_cast<int>(key_text.size()));
    EVP_PKEY* key = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr);
    BIO_free(key_bio);
    REQUIRE(key != nullptr);
    CHECK(X509_check_private_key(server, key) == 1);
    EVP_PKEY_free(key);
    X509_free(server);
}

TEST_CASE("key files are readable only by owner and SYSTEM", "[webgate][certs]") {
    TempDir dir;
    const CertPaths paths = dir.paths();
    REQUIRE(revenant::webgate::ensure_certificates(paths, HostNames{"radio", {}}).has_value());

    // The current user's SID, for comparison with the ACE that is not SYSTEM.
    HANDLE token = nullptr;
    REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != 0);
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<unsigned char> user_buffer(needed);
    REQUIRE(GetTokenInformation(token, TokenUser, user_buffer.data(), needed, &needed) != 0);
    CloseHandle(token);
    PSID user = reinterpret_cast<TOKEN_USER*>(user_buffer.data())->User.Sid;

    for (const auto& key : {paths.ca_key(), paths.server_key()}) {
        INFO(key.string());
        PACL dacl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        REQUIRE(GetNamedSecurityInfoW(key.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                      nullptr, nullptr, &dacl, nullptr, &descriptor) ==
                ERROR_SUCCESS);
        REQUIRE(dacl != nullptr);

        ACL_SIZE_INFORMATION size{};
        REQUIRE(GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation) != 0);
        CHECK(size.AceCount == 2);

        bool saw_user = false;
        bool saw_system = false;
        for (DWORD i = 0; i < size.AceCount; ++i) {
            LPVOID raw = nullptr;
            REQUIRE(GetAce(dacl, i, &raw) != 0);
            auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
            CHECK(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE);
            auto* sid = static_cast<PSID>(static_cast<void*>(&ace->SidStart));
            if (IsWellKnownSid(sid, WinLocalSystemSid) != 0) {
                saw_system = true;
            } else if (EqualSid(sid, user) != 0) {
                saw_user = true;
            }
        }
        CHECK(saw_user);
        CHECK(saw_system);
        LocalFree(descriptor);
    }
}

TEST_CASE("mobileconfig carries the CA", "[webgate][certs]") {
    TempDir dir;
    const CertPaths paths = dir.paths();
    REQUIRE(revenant::webgate::ensure_certificates(paths, HostNames{"radio", {}}).has_value());

    const auto profile = revenant::webgate::mobileconfig(paths);
    REQUIRE(profile.has_value());

    X509* ca = load_cert(paths.ca_crt());
    REQUIRE(ca != nullptr);
    const int der_length = i2d_X509(ca, nullptr);
    std::vector<unsigned char> der(static_cast<std::size_t>(der_length));
    unsigned char* cursor = der.data();
    i2d_X509(ca, &cursor);
    X509_free(ca);

    std::string base64(((der.size() + 2) / 3 * 4) + 1, '\0');
    const int encoded = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(base64.data()),
                                        der.data(), static_cast<int>(der.size()));
    base64.resize(static_cast<std::size_t>(encoded));

    using Catch::Matchers::ContainsSubstring;
    CHECK_THAT(*profile, ContainsSubstring("com.apple.security.root"));
    CHECK_THAT(*profile, ContainsSubstring(base64));
}
