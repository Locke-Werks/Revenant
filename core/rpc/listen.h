// Binding the RPC port so that nothing else can bind it as well.
//
// WHY THE SERVER DOES NOT CALL kj::NetworkAddress::listen
//
// kj's listen() sets SO_REUSEADDR on every listening socket, with a comment
// saying it is there so a restarted server does not wait out TIME_WAIT. That
// is what the option means on POSIX. On Windows it means something else: a
// socket carrying it may bind a port another socket is already listening on.
// kj 1.4.0's async-io-win32.c++ sets it anyway, so two servers built this way
// bind one address and port and both report success.
//
// Measured 2026-09-22 on this machine: Get-NetTCPConnection listed a first
// revenant-engine listening on 127.0.0.1:17690, and a second one started then
// on the same port printed "listening on 127.0.0.1:17690", served for its
// whole --duration and exited zero. Which of the two a client reaches in that
// state is up to the stack. While revenant-engine defaulted to an ephemeral
// port this took two operators naming the same number; a fixed default, which
// revenant-ui's 17690 asks for, makes it the ordinary case.
//
// So the socket is made here with SO_EXCLUSIVEADDRUSE, which refuses the bind
// when anything holds the port and stops anything binding it afterwards, and
// is handed to kj already listening. The refusal names the port and says
// another socket has it, which is the true statement and the one an operator
// who started a second engine can act on.
//
// THE RESTART kj WAS PROTECTING DOES NOT NEED SO_REUSEADDR HERE. Measured the
// same day: an engine that exited with a client connected left the server
// side of that connection in TIME_WAIT on 17690, and an engine started 300 ms
// later bound 17690 exclusively and served. The second engine started while
// the first was still listening was refused with WSAEADDRINUSE.
//
// Windows only, like the rest of this tree. SO_EXCLUSIVEADDRUSE has no POSIX
// spelling because POSIX never had the hole.

#pragma once

#include <cstdint>
#include <string_view>

#include <kj/async-io.h>

#include "core/error.h"

namespace revenant::rpc {

// Resolves `address` numerically or by name, binds the first result it gets
// exclusively on `port` (zero for an ephemeral one), listens, and hands the
// socket to `io`, which owns it from then on.
//
// Must run on the thread whose event loop `io` belongs to, after
// kj::setupAsyncIo has initialised Winsock.
[[nodiscard]] Expected<kj::Own<kj::ConnectionReceiver>> listen_exclusive(
    kj::LowLevelAsyncIoProvider& io, std::string_view address, std::uint16_t port);

}  // namespace revenant::rpc
