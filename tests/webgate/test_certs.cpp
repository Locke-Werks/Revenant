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
#include <openssl/x509v3.h>

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

[[nodiscard]] EVP_PKEY* load_key(const std::filesystem::path& path) {
    const std::string text = slurp(path);
    BIO* bio = BIO_new_mem_buf(text.data(), static_cast<int>(text.size()));
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return key;
}

void write_pem(const std::filesystem::path& path, X509* cert, EVP_PKEY* key) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (cert != nullptr) {
        PEM_write_bio_X509(bio, cert);
    } else {
        PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    }
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data, length);
    BIO_free(bio);
}

void add_ext(X509* subject, X509* issuer, int nid, const char* value) {
    X509V3_CTX ctx;
    X509V3_set_ctx(&ctx, issuer, subject, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    REQUIRE(ext != nullptr);
    X509_add_ext(subject, ext, -1);
    X509_EXTENSION_free(ext);
}

// A certificate signed by issuer_key, self-signed when issuer is null.
[[nodiscard]] X509* make_cert(EVP_PKEY* key, X509* issuer, EVP_PKEY* issuer_key, bool ca,
                              const char* san) {
    X509* cert = X509_new();
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 7);
    X509_NAME_add_entry_by_txt(X509_get_subject_name(cert), "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("test"), -1, -1, 0);
    X509_set_issuer_name(cert, X509_get_subject_name(issuer != nullptr ? issuer : cert));
    X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert), 86400);
    X509_set_pubkey(cert, key);
    X509* const signer = issuer != nullptr ? issuer : cert;
    if (ca) {
        add_ext(cert, signer, NID_basic_constraints, "critical,CA:TRUE");
        add_ext(cert, signer, NID_key_usage, "critical,keyCertSign,cRLSign");
    } else {
        add_ext(cert, signer, NID_basic_constraints, "critical,CA:FALSE");
        add_ext(cert, signer, NID_ext_key_usage, "serverAuth");
        add_ext(cert, signer, NID_subject_alt_name, san);
    }
    X509_sign(cert, issuer_key, EVP_sha256());
    return cert;
}

// X509_verify_cert of leaf against ca alone; returns the verify error.
[[nodiscard]] int verify_against(X509* ca, X509* leaf) {
    X509_STORE* store = X509_STORE_new();
    X509_STORE_add_cert(store, ca);
    X509_STORE_CTX* ctx = X509_STORE_CTX_new();
    X509_STORE_CTX_init(ctx, store, leaf, nullptr);
    const int ok = X509_verify_cert(ctx);
    const int error = ok == 1 ? X509_V_OK : X509_STORE_CTX_get_error(ctx);
    X509_STORE_CTX_free(ctx);
    X509_STORE_free(store);
    return error;
}

[[nodiscard]] std::vector<std::string> ca_subtrees(X509* ca, bool& critical) {
    std::vector<std::string> out;
    const int index = X509_get_ext_by_NID(ca, NID_name_constraints, -1);
    critical = index >= 0 && X509_EXTENSION_get_critical(X509_get_ext(ca, index)) != 0;
    auto* nc = static_cast<NAME_CONSTRAINTS*>(
        X509_get_ext_d2i(ca, NID_name_constraints, nullptr, nullptr));
    if (nc == nullptr) {
        return out;
    }
    CHECK(sk_GENERAL_SUBTREE_num(nc->excludedSubtrees) <= 0);
    for (int i = 0; i < sk_GENERAL_SUBTREE_num(nc->permittedSubtrees); ++i) {
        const GENERAL_NAME* gn = sk_GENERAL_SUBTREE_value(nc->permittedSubtrees, i)->base;
        const auto* data = gn->type == GEN_DNS ? ASN1_STRING_get0_data(gn->d.dNSName)
                                               : ASN1_STRING_get0_data(gn->d.iPAddress);
        const int length = gn->type == GEN_DNS ? ASN1_STRING_length(gn->d.dNSName)
                                               : ASN1_STRING_length(gn->d.iPAddress);
        if (gn->type == GEN_DNS) {
            out.push_back("DNS:" + std::string(reinterpret_cast<const char*>(data),
                                               static_cast<std::size_t>(length)));
        } else {
            std::string text = "IP:";
            for (int b = 0; b < length; ++b) {
                text += std::to_string(data[b]) + (b == 3 ? "/" : (b == length - 1 ? "" : "."));
            }
            out.push_back(text);
        }
    }
    NAME_CONSTRAINTS_free(nc);
    return sorted(out);
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
                paths, HostNames{"radio", {"192.168.1.20", "10.0.0.5"}})
                .has_value());

    const auto sans = sorted(revenant::webgate::certificate_sans(paths.server_crt()));
    const auto expected = sorted({"radio", "radio.local", "192.168.1.20", "10.0.0.5"});
    CHECK(sans == expected);
}

TEST_CASE("an IPv6 address is never put in the certificate", "[webgate][certs]") {
    TempDir dir;
    const CertPaths paths = dir.paths();

    REQUIRE(revenant::webgate::ensure_certificates(
                paths, HostNames{"radio", {"192.168.1.20", "fe80::1", "2001:db8::1"}})
                .has_value());
    CHECK(sorted(revenant::webgate::certificate_sans(paths.server_crt())) ==
          sorted({"radio", "radio.local", "192.168.1.20"}));

    for (const std::string& name : revenant::webgate::current_host_names().addresses) {
        CHECK(name.find(':') == std::string::npos);
    }
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

TEST_CASE("the CA is name-constrained to this machine and the LAN", "[webgate][certs]") {
    TempDir dir;
    const CertPaths paths = dir.paths();
    REQUIRE(revenant::webgate::ensure_certificates(paths, HostNames{"Radio", {"192.168.1.20"}})
                .has_value());

    X509* ca = load_cert(paths.ca_crt());
    REQUIRE(ca != nullptr);
    bool critical = false;
    CHECK(ca_subtrees(ca, critical) ==
          sorted({"DNS:radio", "DNS:local", "IP:10.0.0.0/255.0.0.0",
                  "IP:172.16.0.0/255.240.0.0", "IP:192.168.0.0/255.255.0.0"}));
    CHECK(critical);

    // The server certificate verifies with the constraints enforced, which
    // X509_verify_cert does by default.
    X509* server = load_cert(paths.server_crt());
    REQUIRE(server != nullptr);
    CHECK(verify_against(ca, server) == X509_V_OK);

    // Signed by the same key, a certificate for a name outside the
    // constraints does not verify.
    EVP_PKEY* ca_key = load_key(paths.ca_key());
    REQUIRE(ca_key != nullptr);
    EVP_PKEY* leaf_key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    for (const char* san : {"DNS:example.com", "IP:8.8.8.8", "DNS:radio.local,IP:100.64.0.1"}) {
        INFO(san);
        X509* outside = make_cert(leaf_key, ca, ca_key, false, san);
        CHECK(verify_against(ca, outside) != X509_V_OK);
        X509_free(outside);
    }
    X509* inside = make_cert(leaf_key, ca, ca_key, false, "DNS:other.local,IP:10.1.2.3");
    CHECK(verify_against(ca, inside) == X509_V_OK);
    X509_free(inside);

    EVP_PKEY_free(leaf_key);
    EVP_PKEY_free(ca_key);
    X509_free(server);
    X509_free(ca);
}

TEST_CASE("addresses outside the private ranges stay out of the certificate",
          "[webgate][certs]") {
    TempDir dir;
    const CertPaths paths = dir.paths();
    REQUIRE(revenant::webgate::ensure_certificates(
                paths, HostNames{"radio", {"192.168.1.20", "100.64.1.2", "8.8.8.8", "172.22.0.9"}})
                .has_value());
    CHECK(sorted(revenant::webgate::certificate_sans(paths.server_crt())) ==
          sorted({"radio", "radio.local", "192.168.1.20", "172.22.0.9"}));
}

TEST_CASE("an unconstrained CA is replaced and the server certificate reissued",
          "[webgate][certs]") {
    TempDir dir;
    const CertPaths paths = dir.paths();
    std::filesystem::create_directories(paths.dir);

    // A CA as the gateway made them before name constraints.
    EVP_PKEY* old_key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    X509* old_ca = make_cert(old_key, nullptr, old_key, true, nullptr);
    write_pem(paths.ca_crt(), old_ca, nullptr);
    write_pem(paths.ca_key(), nullptr, old_key);
    const std::string old_text = slurp(paths.ca_crt());
    X509_free(old_ca);
    EVP_PKEY_free(old_key);

    const auto issued =
        revenant::webgate::ensure_certificates(paths, HostNames{"radio", {"192.168.1.20"}});
    REQUIRE(issued.has_value());
    CHECK(*issued);
    CHECK(slurp(paths.ca_crt()) != old_text);

    X509* ca = load_cert(paths.ca_crt());
    X509* server = load_cert(paths.server_crt());
    REQUIRE(ca != nullptr);
    REQUIRE(server != nullptr);
    bool critical = false;
    CHECK_FALSE(ca_subtrees(ca, critical).empty());
    CHECK(critical);
    CHECK(verify_against(ca, server) == X509_V_OK);
    X509_free(server);
    X509_free(ca);

    // Kept on the next start.
    const std::string kept = slurp(paths.ca_crt());
    const auto again =
        revenant::webgate::ensure_certificates(paths, HostNames{"radio", {"192.168.1.20"}});
    REQUIRE(again.has_value());
    CHECK_FALSE(*again);
    CHECK(slurp(paths.ca_crt()) == kept);
}

TEST_CASE("a CA constrained to another host name is replaced", "[webgate][certs]") {
    TempDir dir;
    const CertPaths paths = dir.paths();
    REQUIRE(revenant::webgate::ensure_certificates(paths, HostNames{"radio", {}}).has_value());
    const std::string before = slurp(paths.ca_crt());

    REQUIRE(revenant::webgate::ensure_certificates(paths, HostNames{"scanner", {}}).value());
    CHECK(slurp(paths.ca_crt()) != before);
    X509* ca = load_cert(paths.ca_crt());
    REQUIRE(ca != nullptr);
    bool critical = false;
    const auto subtrees = ca_subtrees(ca, critical);
    CHECK(std::ranges::find(subtrees, std::string("DNS:scanner")) != subtrees.end());
    X509_free(ca);

    const auto profile = revenant::webgate::mobileconfig(paths);
    REQUIRE(profile.has_value());
    CHECK_THAT(*profile, Catch::Matchers::ContainsSubstring("Revenant Local CA (scanner)"));
}
