// The gateway's TLS identity: a local certificate authority and a server
// certificate it signs.
//
// iOS will not run a web app over plain HTTP from another machine (service
// workers and the install prompt both need a secure context), and a
// self-signed server certificate cannot be trusted per device without also
// being a CA. So the gateway is its own CA: the phone installs ca.crt once as
// a profile, and every server certificate issued later chains to it without
// the phone being asked again.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/error.h"

namespace revenant::webgate {

struct CertPaths {
    std::filesystem::path dir;

    [[nodiscard]] std::filesystem::path ca_key() const { return dir / "ca.key"; }
    [[nodiscard]] std::filesystem::path ca_crt() const { return dir / "ca.crt"; }
    [[nodiscard]] std::filesystem::path server_key() const { return dir / "server.key"; }
    [[nodiscard]] std::filesystem::path server_crt() const { return dir / "server.crt"; }
};

// What the server certificate has to answer to. addresses are textual IPv4 or
// IPv6.
struct HostNames {
    std::string hostname;
    std::vector<std::string> addresses;
};

// The machine's DNS host name and the addresses of every adapter that is up and
// is not loopback. IPv6 link-local and tentative addresses are left out: a
// phone cannot dial a link-local literal without a scope id, and a tentative
// one is gone again before the certificate is used, so each only makes the
// certificate reissue for nothing.
[[nodiscard]] HostNames current_host_names();

// Creates the CA when it is missing and issues a server certificate when there
// is none, when its names differ from `names`, or when it was not signed by
// the CA now on disk. Returns true when a new server certificate was issued.
//
// The CA is never replaced while both its files are present: replacing it
// would make every phone that installed the profile distrust the gateway.
[[nodiscard]] Expected<bool> ensure_certificates(const CertPaths& paths, const HostNames& names);

// The .mobileconfig an iPhone installs to trust ca.crt.
[[nodiscard]] Expected<std::string> mobileconfig(const CertPaths& paths);

// Subject alternative names of a PEM certificate: DNS names as written and IP
// addresses in canonical text form.
[[nodiscard]] std::vector<std::string> certificate_sans(const std::filesystem::path& crt);

}  // namespace revenant::webgate
