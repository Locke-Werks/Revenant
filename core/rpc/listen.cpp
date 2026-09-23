// Winsock is included here and in no other file of this directory, so the
// windows.h-before-winsock2.h ordering trap stays inside one translation unit.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "core/rpc/listen.h"

#include <format>
#include <string>

namespace revenant::rpc {
namespace {

[[nodiscard]] std::string winsock_error(int code) {
    // The two a port conflict produces, named, because an operator reading
    // "10048" has to look it up and one reading its name does not.
    switch (code) {
        case WSAEADDRINUSE: return "WSAEADDRINUSE: another socket is already bound to this port";
        case WSAEACCES:
            return "WSAEACCES: another socket holds this port, or the port is reserved";
        case WSAEADDRNOTAVAIL: return "WSAEADDRNOTAVAIL: that address is not on this machine";
        default: return std::format("Winsock error {}", code);
    }
}

// Closes on every path that does not hand the socket over.
class SocketGuard {
public:
    explicit SocketGuard(SOCKET socket) : socket_(socket) {}
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;
    ~SocketGuard() {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
        }
    }
    [[nodiscard]] SOCKET get() const { return socket_; }
    [[nodiscard]] SOCKET release() {
        const SOCKET out = socket_;
        socket_ = INVALID_SOCKET;
        return out;
    }

private:
    SOCKET socket_;
};

}  // namespace

Expected<kj::Own<kj::ConnectionReceiver>> listen_exclusive(kj::LowLevelAsyncIoProvider& io,
                                                           std::string_view address,
                                                           std::uint16_t port) {
    const std::string host(address);
    const std::string service = std::to_string(port);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* found = nullptr;
    if (const int resolved = getaddrinfo(host.c_str(), service.c_str(), &hints, &found);
        resolved != 0 || found == nullptr) {
        return fail(std::format("{} does not resolve to an address this machine can listen on: {}",
                                host, winsock_error(resolved)));
    }

    // The first result, which is what kj's own listen() takes and warns
    // about. A numeric address resolves to exactly one.
    const int family = found->ai_family;
    SocketGuard socket(WSASocketW(found->ai_family, found->ai_socktype, found->ai_protocol,
                                  nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT));
    if (socket.get() == INVALID_SOCKET) {
        const int code = WSAGetLastError();
        freeaddrinfo(found);
        return fail(std::format("could not create a socket for {}: {}", host, winsock_error(code)));
    }

    // The whole point of this file. Set before the bind, which is the only
    // moment it is honoured.
    BOOL exclusive = TRUE;
    if (setsockopt(socket.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0) {
        const int code = WSAGetLastError();
        freeaddrinfo(found);
        return fail(std::format("could not ask for exclusive use of {}:{}: {}", host, port,
                                winsock_error(code)));
    }

    // What kj sets on every stream socket it makes, and for its reason: Nagle
    // holds back the small messages Cap'n Proto's RPC protocol is made of.
    // Set on the listener so every accepted connection inherits it.
    BOOL no_delay = TRUE;
    static_cast<void>(setsockopt(socket.get(), IPPROTO_TCP, TCP_NODELAY,
                                 reinterpret_cast<const char*>(&no_delay), sizeof(no_delay)));

    // An IPv6 wildcard takes IPv4 as well, which is what kj's listen() does
    // for a wildcard and what a caller binding "::" means.
    if (family == AF_INET6) {
        const auto* six = reinterpret_cast<const sockaddr_in6*>(found->ai_addr);
        if (IN6_IS_ADDR_UNSPECIFIED(&six->sin6_addr)) {
            DWORD only = 0;
            static_cast<void>(setsockopt(socket.get(), IPPROTO_IPV6, IPV6_V6ONLY,
                                         reinterpret_cast<const char*>(&only), sizeof(only)));
        }
    }

    const int bound = bind(socket.get(), found->ai_addr, static_cast<int>(found->ai_addrlen));
    const int bind_error = bound == 0 ? 0 : WSAGetLastError();
    freeaddrinfo(found);
    if (bound != 0) {
        return fail(std::format("could not bind {}:{}: {}", host, port, winsock_error(bind_error)));
    }

    if (listen(socket.get(), SOMAXCONN) != 0) {
        const int code = WSAGetLastError();
        return fail(std::format("could not listen on {}:{}: {}", host, port, winsock_error(code)));
    }

    // TAKE_OWNERSHIP: kj closes it with the receiver. The guard lets go only
    // now, so every refusal above closed what it had opened.
    return io.wrapListenSocketFd(static_cast<kj::LowLevelAsyncIoProvider::Fd>(socket.release()),
                                 kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
}

}  // namespace revenant::rpc
