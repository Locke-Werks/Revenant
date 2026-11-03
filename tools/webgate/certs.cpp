#include "tools/webgate/certs.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string_view>
#include <system_error>

#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// winsock2.h has to come before windows.h, which otherwise drags in winsock 1.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <iphlpapi.h>

#include "core/rpc/token.h"

namespace revenant::webgate {
namespace {

// iOS refuses a leaf certificate valid for more than 825 days.
constexpr long kLeafDays = 825;
constexpr long kCaDays = 3650;
constexpr long kBackdateSeconds = 3600;
// A certificate this close to expiry is replaced on the next start, because
// nothing else would: the phone just stops trusting the gateway one day.
constexpr long kRenewWithinDays = 30;
constexpr long kSecondsPerDay = 86400;

struct PkeyDeleter {
    void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
};
struct X509Deleter {
    void operator()(X509* p) const { X509_free(p); }
};
struct BioDeleter {
    void operator()(BIO* p) const { BIO_free(p); }
};
struct BnDeleter {
    void operator()(BIGNUM* p) const { BN_free(p); }
};
struct ExtDeleter {
    void operator()(X509_EXTENSION* p) const { X509_EXTENSION_free(p); }
};
struct NamesDeleter {
    void operator()(GENERAL_NAMES* p) const { GENERAL_NAMES_free(p); }
};

using Pkey = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using Cert = std::unique_ptr<X509, X509Deleter>;
using Bio = std::unique_ptr<BIO, BioDeleter>;
using Bn = std::unique_ptr<BIGNUM, BnDeleter>;
using Ext = std::unique_ptr<X509_EXTENSION, ExtDeleter>;
using Names = std::unique_ptr<GENERAL_NAMES, NamesDeleter>;

// Drains OpenSSL's thread-local error queue into one sentence. Left in place
// it would be attributed to whichever later call checks the queue.
[[nodiscard]] std::string drain_errors() {
    std::string out;
    while (const unsigned long code = ERR_get_error()) {
        std::array<char, 256> text{};
        ERR_error_string_n(code, text.data(), text.size());
        if (!out.empty()) {
            out += "; ";
        }
        out += text.data();
    }
    return out.empty() ? "no detail from OpenSSL" : out;
}

[[nodiscard]] std::unexpected<Error> ssl_fail(std::string_view what) {
    return fail(std::format("{}: {}", what, drain_errors()));
}

[[nodiscard]] std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

[[nodiscard]] std::string canonical_ip(const std::string& text) {
    in_addr v4{};
    if (inet_pton(AF_INET, text.c_str(), &v4) == 1) {
        std::array<char, INET_ADDRSTRLEN> out{};
        if (inet_ntop(AF_INET, &v4, out.data(), out.size()) != nullptr) {
            return out.data();
        }
    }
    in6_addr v6{};
    if (inet_pton(AF_INET6, text.c_str(), &v6) == 1) {
        std::array<char, INET6_ADDRSTRLEN> out{};
        if (inet_ntop(AF_INET6, &v6, out.data(), out.size()) != nullptr) {
            return out.data();
        }
    }
    return {};
}

[[nodiscard]] Expected<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail(std::format("could not open {}", path.string()));
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

[[nodiscard]] Status write_public_file(const std::filesystem::path& path, std::string_view data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return fail(std::format("could not create {}", path.string()));
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) {
        return fail(std::format("could not write {}", path.string()));
    }
    return {};
}

[[nodiscard]] Expected<Cert> read_cert(const std::filesystem::path& path) {
    auto text = read_file(path);
    if (!text) {
        return std::unexpected(text.error());
    }
    Bio bio(BIO_new_mem_buf(text->data(), static_cast<int>(text->size())));
    Cert cert(bio ? PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr) : nullptr);
    if (!cert) {
        return ssl_fail(std::format("could not parse the certificate {}", path.string()));
    }
    return cert;
}

[[nodiscard]] Expected<Pkey> read_key(const std::filesystem::path& path) {
    auto text = read_file(path);
    if (!text) {
        return std::unexpected(text.error());
    }
    Bio bio(BIO_new_mem_buf(text->data(), static_cast<int>(text->size())));
    Pkey key(bio ? PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr) : nullptr);
    if (!key) {
        return ssl_fail(std::format("could not parse the private key {}", path.string()));
    }
    return key;
}

[[nodiscard]] std::string bio_text(BIO* bio) {
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    return length > 0 && data != nullptr ? std::string(data, static_cast<std::size_t>(length))
                                         : std::string();
}

[[nodiscard]] Expected<std::string> key_pem(EVP_PKEY* key) {
    Bio bio(BIO_new(BIO_s_mem()));
    if (!bio || PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) !=
                    1) {
        return ssl_fail("could not encode a private key");
    }
    return bio_text(bio.get());
}

[[nodiscard]] Expected<std::string> cert_pem(X509* cert) {
    Bio bio(BIO_new(BIO_s_mem()));
    if (!bio || PEM_write_bio_X509(bio.get(), cert) != 1) {
        return ssl_fail("could not encode a certificate");
    }
    return bio_text(bio.get());
}

[[nodiscard]] Expected<Pkey> new_key() {
    Pkey key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
    if (!key) {
        return ssl_fail("could not generate an EC P-256 key");
    }
    return key;
}

[[nodiscard]] Status add_extension(X509* subject, X509* issuer, int nid, const char* value) {
    X509V3_CTX ctx;
    X509V3_set_ctx(&ctx, issuer, subject, nullptr, nullptr, 0);
    Ext ext(X509V3_EXT_conf_nid(nullptr, &ctx, nid, value));
    if (!ext || X509_add_ext(subject, ext.get(), -1) != 1) {
        return ssl_fail(std::format("could not add the certificate extension {}", value));
    }
    return {};
}

// A fresh certificate shell: version 3, random serial, subject and validity
// set, public key attached. The caller adds extensions and signs.
[[nodiscard]] Expected<Cert> new_cert(const std::string& common_name, EVP_PKEY* pub,
                                      long not_before_offset, long not_after_offset) {
    Cert cert(X509_new());
    if (!cert) {
        return ssl_fail("could not allocate a certificate");
    }
    if (X509_set_version(cert.get(), 2) != 1) {
        return ssl_fail("could not set the certificate version");
    }

    // 63 bits so the DER integer is never negative-looking.
    Bn serial(BN_new());
    if (!serial || BN_rand(serial.get(), 63, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) != 1 ||
        BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(cert.get())) == nullptr) {
        return ssl_fail("could not generate a certificate serial number");
    }

    X509_NAME* name = X509_get_subject_name(cert.get());
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
                                   reinterpret_cast<const unsigned char*>(common_name.data()),
                                   static_cast<int>(common_name.size()), -1, 0) != 1) {
        return ssl_fail("could not set the certificate subject");
    }

    if (X509_gmtime_adj(X509_getm_notBefore(cert.get()), not_before_offset) == nullptr ||
        X509_gmtime_adj(X509_getm_notAfter(cert.get()), not_after_offset) == nullptr) {
        return ssl_fail("could not set the certificate validity");
    }
    if (X509_set_pubkey(cert.get(), pub) != 1) {
        return ssl_fail("could not attach the public key");
    }
    return cert;
}

struct CaMaterial {
    Pkey key;
    Cert crt;
};

[[nodiscard]] Expected<CaMaterial> make_ca(const std::string& hostname) {
    auto key = new_key();
    if (!key) {
        return std::unexpected(key.error());
    }
    auto crt = new_cert(std::format("Revenant Local CA ({})", hostname), key->get(),
                        -kBackdateSeconds, kCaDays * kSecondsPerDay);
    if (!crt) {
        return std::unexpected(crt.error());
    }
    X509* x = crt->get();
    if (X509_set_issuer_name(x, X509_get_subject_name(x)) != 1) {
        return ssl_fail("could not set the CA issuer");
    }
    for (auto status : {add_extension(x, x, NID_basic_constraints, "critical,CA:TRUE"),
                        add_extension(x, x, NID_key_usage, "critical,keyCertSign,cRLSign"),
                        add_extension(x, x, NID_subject_key_identifier, "hash"),
                        add_extension(x, x, NID_authority_key_identifier, "keyid:always")}) {
        if (!status) {
            return std::unexpected(status.error());
        }
    }
    if (X509_sign(x, key->get(), EVP_sha256()) == 0) {
        return ssl_fail("could not sign the CA certificate");
    }
    return CaMaterial{std::move(*key), std::move(*crt)};
}

// The names the server certificate has to carry, sorted and de-duplicated, in
// the same textual form certificate_sans reports so the two compare directly.
[[nodiscard]] Expected<std::vector<std::string>> wanted_sans(const HostNames& names) {
    std::vector<std::string> out;
    const std::string host = lower(names.hostname);
    if (!host.empty()) {
        out.push_back(host);
        out.push_back(host + ".local");
    }
    for (const std::string& address : names.addresses) {
        const std::string canonical = canonical_ip(address);
        if (canonical.empty()) {
            return fail(std::format("{} is not an IPv4 or IPv6 address", address));
        }
        out.push_back(canonical);
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    if (out.empty()) {
        return fail("the server certificate has no names to carry: no host name and no addresses");
    }
    return out;
}

[[nodiscard]] bool is_ip(const std::string& name) {
    return !canonical_ip(name).empty();
}

[[nodiscard]] Expected<Cert> issue_server(const std::vector<std::string>& sans, X509* ca,
                                          EVP_PKEY* ca_key, EVP_PKEY* server_pub,
                                          const std::string& hostname) {
    // Exactly 825 days from notBefore to notAfter, the iOS ceiling, with the
    // backdate inside it rather than added to it.
    auto crt = new_cert(hostname.empty() ? "revenant-webgate" : hostname, server_pub,
                        -kBackdateSeconds, (kLeafDays * kSecondsPerDay) - kBackdateSeconds);
    if (!crt) {
        return std::unexpected(crt.error());
    }
    X509* x = crt->get();
    if (X509_set_issuer_name(x, X509_get_subject_name(ca)) != 1) {
        return ssl_fail("could not set the server certificate issuer");
    }

    std::string san_text;
    for (const std::string& name : sans) {
        if (!san_text.empty()) {
            san_text += ',';
        }
        san_text += (is_ip(name) ? "IP:" : "DNS:") + name;
    }

    for (auto status : {add_extension(x, ca, NID_basic_constraints, "critical,CA:FALSE"),
                        add_extension(x, ca, NID_key_usage, "critical,digitalSignature"),
                        add_extension(x, ca, NID_ext_key_usage, "serverAuth"),
                        add_extension(x, ca, NID_subject_key_identifier, "hash"),
                        add_extension(x, ca, NID_authority_key_identifier, "keyid:always"),
                        add_extension(x, ca, NID_subject_alt_name, san_text.c_str())}) {
        if (!status) {
            return std::unexpected(status.error());
        }
    }
    if (X509_sign(x, ca_key, EVP_sha256()) == 0) {
        return ssl_fail("could not sign the server certificate");
    }
    return crt;
}

[[nodiscard]] std::string xml_escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c; break;
        }
    }
    return out;
}

[[nodiscard]] std::string uuid_from(const unsigned char* sixteen) {
    std::array<unsigned char, 16> b{};
    std::copy_n(sixteen, 16, b.begin());
    b[6] = static_cast<unsigned char>((b[6] & 0x0FU) | 0x40U);
    b[8] = static_cast<unsigned char>((b[8] & 0x3FU) | 0x80U);
    return std::format("{:02X}{:02X}{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-"
                       "{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
                       b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
                       b[12], b[13], b[14], b[15]);
}

}  // namespace

HostNames current_host_names() {
    HostNames out;

    std::array<wchar_t, 256> host{};
    DWORD host_length = static_cast<DWORD>(host.size());
    if (GetComputerNameExW(ComputerNameDnsHostname, host.data(), &host_length) != 0) {
        const int needed = WideCharToMultiByte(CP_UTF8, 0, host.data(),
                                               static_cast<int>(host_length), nullptr, 0, nullptr,
                                               nullptr);
        if (needed > 0) {
            out.hostname.resize(static_cast<std::size_t>(needed));
            WideCharToMultiByte(CP_UTF8, 0, host.data(), static_cast<int>(host_length),
                                out.hostname.data(), needed, nullptr, nullptr);
        }
    }

    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                            GAA_FLAG_SKIP_DNS_SERVER;
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc != NO_ERROR) {
        return out;
    }

    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp ||
            adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            if (unicast->DadState != IpDadStatePreferred) {
                continue;
            }
            const sockaddr* sa = unicast->Address.lpSockaddr;
            std::array<char, INET6_ADDRSTRLEN> text{};
            if (sa->sa_family == AF_INET) {
                const auto* in = reinterpret_cast<const sockaddr_in*>(sa);
                inet_ntop(AF_INET, &in->sin_addr, text.data(), text.size());
            } else if (sa->sa_family == AF_INET6) {
                const auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
                if (IN6_IS_ADDR_LINKLOCAL(&in6->sin6_addr) ||
                    IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr)) {
                    continue;
                }
                inet_ntop(AF_INET6, &in6->sin6_addr, text.data(), text.size());
            } else {
                continue;
            }
            if (text[0] != '\0') {
                out.addresses.emplace_back(text.data());
            }
        }
    }
    return out;
}

std::vector<std::string> certificate_sans(const std::filesystem::path& crt) {
    std::vector<std::string> out;
    auto cert = read_cert(crt);
    if (!cert) {
        ERR_clear_error();
        return out;
    }
    Names names(static_cast<GENERAL_NAMES*>(
        X509_get_ext_d2i(cert->get(), NID_subject_alt_name, nullptr, nullptr)));
    if (!names) {
        return out;
    }
    for (int i = 0; i < sk_GENERAL_NAME_num(names.get()); ++i) {
        const GENERAL_NAME* gn = sk_GENERAL_NAME_value(names.get(), i);
        if (gn->type == GEN_DNS) {
            const auto* data = ASN1_STRING_get0_data(gn->d.dNSName);
            out.emplace_back(reinterpret_cast<const char*>(data),
                             static_cast<std::size_t>(ASN1_STRING_length(gn->d.dNSName)));
        } else if (gn->type == GEN_IPADD) {
            const auto* data = ASN1_STRING_get0_data(gn->d.iPAddress);
            const int length = ASN1_STRING_length(gn->d.iPAddress);
            std::array<char, INET6_ADDRSTRLEN> text{};
            if (length == 4 && inet_ntop(AF_INET, data, text.data(), text.size()) != nullptr) {
                out.emplace_back(text.data());
            } else if (length == 16 &&
                       inet_ntop(AF_INET6, data, text.data(), text.size()) != nullptr) {
                out.emplace_back(text.data());
            }
        }
    }
    return out;
}

Expected<bool> ensure_certificates(const CertPaths& paths, const HostNames& names) {
    std::error_code ec;
    std::filesystem::create_directories(paths.dir, ec);
    if (ec) {
        return fail(std::format("could not create the certificate directory {}: {}",
                                paths.dir.string(), ec.message()));
    }

    auto wanted = wanted_sans(names);
    if (!wanted) {
        return std::unexpected(wanted.error());
    }

    // Either half missing means the pair cannot be trusted to match, so both
    // are replaced and the server certificate goes with them. Only reachable on
    // first run or after someone deleted a file by hand.
    bool ca_created = false;
    if (!std::filesystem::exists(paths.ca_key()) || !std::filesystem::exists(paths.ca_crt())) {
        auto ca = make_ca(lower(names.hostname));
        if (!ca) {
            return std::unexpected(ca.error());
        }
        auto key_text = key_pem(ca->key.get());
        auto crt_text = cert_pem(ca->crt.get());
        if (!key_text || !crt_text) {
            return std::unexpected(key_text ? crt_text.error() : key_text.error());
        }
        if (auto written = rpc::write_private_file(paths.ca_key().string(), *key_text, true);
            !written) {
            return std::unexpected(written.error());
        }
        if (auto written = write_public_file(paths.ca_crt(), *crt_text); !written) {
            return std::unexpected(written.error());
        }
        ca_created = true;
    }

    auto ca_crt = read_cert(paths.ca_crt());
    if (!ca_crt) {
        return std::unexpected(ca_crt.error());
    }
    auto ca_key = read_key(paths.ca_key());
    if (!ca_key) {
        return std::unexpected(ca_key.error());
    }

    if (!ca_created && std::filesystem::exists(paths.server_crt()) &&
        std::filesystem::exists(paths.server_key())) {
        auto existing = read_cert(paths.server_crt());
        if (existing) {
            std::vector<std::string> have = certificate_sans(paths.server_crt());
            std::ranges::sort(have);
            have.erase(std::ranges::unique(have).begin(), have.end());

            const bool signed_by_ca = X509_verify(existing->get(), X509_get0_pubkey(ca_crt->get())) == 1;
            std::time_t horizon = std::time(nullptr) + (kRenewWithinDays * kSecondsPerDay);
            const bool fresh = X509_cmp_time(X509_get0_notAfter(existing->get()), &horizon) > 0;
            ERR_clear_error();
            if (have == *wanted && signed_by_ca && fresh) {
                return false;
            }
        } else {
            ERR_clear_error();
        }
    }

    auto server_key = new_key();
    if (!server_key) {
        return std::unexpected(server_key.error());
    }
    auto server_crt = issue_server(*wanted, ca_crt->get(), ca_key->get(), server_key->get(),
                                   lower(names.hostname));
    if (!server_crt) {
        return std::unexpected(server_crt.error());
    }
    auto key_text = key_pem(server_key->get());
    auto crt_text = cert_pem(server_crt->get());
    if (!key_text || !crt_text) {
        return std::unexpected(key_text ? crt_text.error() : key_text.error());
    }
    if (auto written = rpc::write_private_file(paths.server_key().string(), *key_text, true);
        !written) {
        return std::unexpected(written.error());
    }
    if (auto written = write_public_file(paths.server_crt(), *crt_text); !written) {
        return std::unexpected(written.error());
    }
    return true;
}

Expected<std::string> mobileconfig(const CertPaths& paths) {
    auto ca = read_cert(paths.ca_crt());
    if (!ca) {
        return std::unexpected(ca.error());
    }

    const int der_length = i2d_X509(ca->get(), nullptr);
    if (der_length <= 0) {
        return ssl_fail("could not encode the CA certificate as DER");
    }
    std::vector<unsigned char> der(static_cast<std::size_t>(der_length));
    unsigned char* cursor = der.data();
    i2d_X509(ca->get(), &cursor);

    std::string base64(((der.size() + 2) / 3 * 4) + 1, '\0');
    const int encoded = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(base64.data()),
                                        der.data(), static_cast<int>(der.size()));
    base64.resize(static_cast<std::size_t>(encoded));

    std::array<char, 256> cn{};
    X509_NAME_get_text_by_NID(X509_get_subject_name(ca->get()), NID_commonName, cn.data(),
                              static_cast<int>(cn.size()));
    const std::string display = xml_escape(cn[0] != '\0' ? cn.data() : "Revenant Local CA");

    // Identifiers come from the certificate's own digest so installing the
    // profile again after a reissue of the same CA replaces it instead of
    // stacking a second copy on the phone.
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0;
    if (X509_digest(ca->get(), EVP_sha256(), digest.data(), &digest_length) != 1 ||
        digest_length < 32) {
        return ssl_fail("could not fingerprint the CA certificate");
    }
    const std::string payload_uuid = uuid_from(digest.data());
    const std::string profile_uuid = uuid_from(digest.data() + 16);

    return std::format(
        R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>PayloadContent</key>
  <array>
    <dict>
      <key>PayloadCertificateFileName</key>
      <string>ca.crt</string>
      <key>PayloadContent</key>
      <data>{}</data>
      <key>PayloadDescription</key>
      <string>Lets this phone trust the Revenant web gateway on your network.</string>
      <key>PayloadDisplayName</key>
      <string>{}</string>
      <key>PayloadIdentifier</key>
      <string>com.revenant.webgate.ca.cert</string>
      <key>PayloadType</key>
      <string>com.apple.security.root</string>
      <key>PayloadUUID</key>
      <string>{}</string>
      <key>PayloadVersion</key>
      <integer>1</integer>
    </dict>
  </array>
  <key>PayloadDisplayName</key>
  <string>{}</string>
  <key>PayloadIdentifier</key>
  <string>com.revenant.webgate.ca</string>
  <key>PayloadRemovalDisallowed</key>
  <false/>
  <key>PayloadType</key>
  <string>Configuration</string>
  <key>PayloadUUID</key>
  <string>{}</string>
  <key>PayloadVersion</key>
  <integer>1</integer>
</dict>
</plist>
)",
        base64, display, payload_uuid, display, profile_uuid);
}

}  // namespace revenant::webgate
