// The pre-shared token the RPC login bootstrap is built on.
//
// WHY THIS LIVES IN THE CLIENT LIBRARY AND NOT THE SERVER'S
//
// Both halves need it. The engine mints the file and loads it; the Qt client
// loads it. core/rpc/CMakeLists.txt states that nothing in revenant_rpc_client
// may depend on core/engine, core/dsp, core/detect or core/source, and nothing
// here does: core/error.h is header-only and the rest is the Windows SDK. So
// the writer and the reader of one file format stay in one translation unit,
// which is the only arrangement in which they cannot drift apart.
//
// WHERE THE FILE LIVES, AND WHY THAT IS A DECISION RATHER THAN A LOOKUP
//
// There is no config file anywhere in this tree. Every binary is configured by
// command-line arguments and one environment variable, so "beside the config"
// resolved to a directory that did not exist. default_token_path picks
// %LOCALAPPDATA%\Revenant\rpc-token, resolved with SHGetKnownFolderPath rather
// than getenv: the environment variable is absent under a service and lies
// under impersonation. Local and not Roaming, because a token that identifies
// one machine's engine must not follow a domain profile to another machine.
//
// Not %ProgramData%: its default DACL grants read to Users, so it would need
// an explicit ACL anyway, and a machine-wide token means every account on the
// box can drive the radio.
//
// Under LocalSystem, FOLDERID_LocalAppData resolves inside
// C:\Windows\System32\config\systemprofile and an operator's client cannot
// read it. That case is answered by revenant-engine's --token-file rather than
// by a special case in the resolver.
//
// WHAT THE FILE HOLDS
//
// 64 lowercase hex characters and one newline, 65 bytes. Hex so that a person
// can select and paste it; the wire carries the raw bytes, because two
// encodings of one secret would make "wrong token" and "wrong encoding" the
// same failure at the one place that must not be ambiguous. Reading accepts
// either case and any trailing whitespace, including the CRLF an operator who
// opened it in Notepad produces. Writing is always lowercase with one \n.
//
// THE ACL IS ATTACHED WHEN THE FILE IS CREATED, NOT AFTERWARDS
//
// mint_token builds the security descriptor first and hands it to CreateFileW
// in SECURITY_ATTRIBUTES with CREATE_NEW. Applying the ACL after writing would
// leave the token on disk under an inherited ACL for the length of the write,
// which passes any test that only reads the final ACL and is a real window.
//
// CREATE_NEW and not CREATE_ALWAYS, for two reasons that are not caution. Two
// engines starting at once: the loser gets ERROR_FILE_EXISTS, loads what the
// winner wrote and serves the same token, rather than clobbering a token the
// winner has already handed to a client. And CREATE_NEW will not follow a
// symlink or hardlink somebody pre-placed at that path.
//
// load_token REFUSES A FILE ANYTHING ELSE CAN READ
//
// Refusal and not a warning: a warning from a headless process lands in a log
// nobody reads, and a world-readable token is the "anything on the box can
// drive the radio" failure the whole design exists to stop, with one extra
// step. The refusal names the principal and states the two recoveries, because
// the operator reading it has no UI to look anything up in.
//
// The check distinguishes Administrators, which is fine and which the operator
// is probably in, from Users, Everyone and Authenticated Users, which are not.
// An administrator can take ownership of the file regardless, so denying that
// group would be a statement about wishes rather than a control. A check
// written against Everyone alone certifies one shape and passes a file the
// Users group can read.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "core/error.h"

namespace revenant::rpc {

// Thirty-two bytes, and the length is not a secret. It is stated in the
// schema, in docs/rpc.md, and in the size of the file on disk.
inline constexpr std::size_t kTokenBytes = 32;

using Token = std::array<std::uint8_t, kTokenBytes>;

// Constant-time over the whole of both spans, after a length check.
//
// The length branch leaks nothing, because the length is published. What the
// loop protects is the PREFIX: a compare that returned at the first wrong byte
// turns a 2^256 search into a 32 x 256 one, guessing one byte position at a
// time against the response time. Whether that is measurable across loopback
// under Windows scheduler jitter is a separate question that docs/rpc.md
// answers honestly. The loop costs three lines on a once-per-connection call
// and removes the argument permanently.
[[nodiscard]] bool tokens_equal(std::span<const std::uint8_t> left,
                                std::span<const std::uint8_t> right) noexcept;

// 64 lowercase hex characters, no newline. The caller adds the newline when it
// is writing a file.
[[nodiscard]] std::string token_to_hex(const Token& token);

// Trailing whitespace, including \r, is stripped. Either case is accepted.
// Anything else is refused with a message naming what was wrong, because an
// operator who reformatted the file by hand needs to be told which way.
[[nodiscard]] Expected<Token> token_from_hex(std::string_view text);

// BCryptGenRandom against the system-preferred RNG.
//
// bcrypt.lib is a Windows SDK import library for an OS component present on
// every supported Windows, on the same terms core/CMakeLists.txt already links
// ole32 and avrt. It adds no vcpkg dependency and the x64-windows-static
// triplet and the one-file signed binary are unaffected.
//
// rand_s would need no link entry at all, and was not taken: it yields four
// bytes a call, so 32 bytes is a loop with eight failure points, and it needs
// _CRT_RAND_S defined before <cstdlib> in a header other code includes.
// std::random_device is out outright, because the standard permits it to be
// deterministic and MSVC's behaviour is a documentation statement rather than
// a contract.
[[nodiscard]] Expected<Token> random_token();

// %LOCALAPPDATA%\Revenant\rpc-token, as UTF-8. See the note at the top.
[[nodiscard]] Expected<std::string> default_token_path();

// Reads and validates, including the ACL check the note at the top describes.
[[nodiscard]] Expected<Token> load_token(const std::string& path);

// Creates the file, or fails because it is already there. The immediate parent
// directory is created if it is missing; a missing grandparent is left to
// CreateFileW, which names the path in its own error.
[[nodiscard]] Expected<Token> mint_token(const std::string& path);

// mint_token, falling back to load_token when the file already exists. This is
// what an engine calls at startup.
[[nodiscard]] Expected<Token> load_or_mint_token(const std::string& path);

// Deletes whatever is there and mints a fresh one.
//
// ROTATION DOES NOT DISCONNECT ANYBODY, which will surprise people and is in
// revenant-engine's help text and in docs/rpc.md for that reason. A Session
// already granted is a capability and capabilities do not re-check, so an
// operator rotating because the token leaked has to restart the engine to kill
// the sessions the leak already bought.
[[nodiscard]] Expected<Token> rotate_token(const std::string& path);

}  // namespace revenant::rpc
