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

// What the server certificate has to answer to. addresses are textual IPv4;
// the gateway does not use IPv6, so an IPv6 address given here is ignored.
struct HostNames {
    std::string hostname;
    std::vector<std::string> addresses;
};

// The machine's DNS host name and the addresses of every adapter that is up and
// is not loopback. IPv4 only. Tentative addresses are left out: one is gone
// again before the certificate is used, so it only makes the certificate
// reissue for nothing.
[[nodiscard]] HostNames current_host_names();

// Creates the CA when it is missing and issues a server certificate when there
// is none, when its names differ from `names`, or when it was not signed by
// the CA now on disk. Returns true when a new server certificate was issued.
//
// The CA carries critical name constraints: DNS names under the host name and
// under .local, and the private IPv4 ranges 10/8, 172.16/12 and 192.168/16.
// Addresses outside those ranges are left out of the server certificate. The
// CA is otherwise kept while both its files are present, since replacing it
// makes every phone that installed the profile distrust the gateway; one
// without those exact constraints (an older CA, or a renamed machine) is
// replaced all the same, and the phones install the profile again.
[[nodiscard]] Expected<bool> ensure_certificates(const CertPaths& paths, const HostNames& names);

// The .mobileconfig an iPhone installs to trust ca.crt.
[[nodiscard]] Expected<std::string> mobileconfig(const CertPaths& paths);

// Subject alternative names of a PEM certificate: DNS names as written and IP
// addresses in canonical text form.
[[nodiscard]] std::vector<std::string> certificate_sans(const std::filesystem::path& crt);

}  // namespace revenant::webgate
