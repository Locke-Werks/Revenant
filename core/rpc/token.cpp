#include "core/rpc/token.h"

#include <array>
#include <cstring>
#include <format>
#include <vector>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <sddl.h>
#include <shlobj.h>

#endif  // _WIN32

namespace revenant::rpc {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

#ifdef _WIN32

// FormatMessage with the trailing newline taken off, because these are
// embedded mid-sentence in messages an operator reads.
[[nodiscard]] std::string win32_message(DWORD code) {
    LPSTR buffer = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        if (buffer != nullptr) {
            LocalFree(buffer);
        }
        return std::format("Windows error {}", code);
    }
    std::string text(buffer, length);
    LocalFree(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

[[nodiscard]] Expected<std::wstring> widen(const std::string& utf8) {
    if (utf8.empty()) {
        return fail("the token file path is empty");
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return fail(std::format("the token file path is not valid UTF-8: {}",
                                win32_message(GetLastError())),
                    GetLastError());
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                        needed);
    return wide;
}

[[nodiscard]] std::string narrow(const wchar_t* wide) {
    if (wide == nullptr) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed - 1, nullptr, nullptr);
    return out;
}

// Whatever the operator sees in a refusal. A SID with no account behind it
// still has to be nameable, or the message says nothing actionable.
[[nodiscard]] std::string describe_sid(PSID sid) {
    std::array<wchar_t, 256> name{};
    std::array<wchar_t, 256> domain{};
    DWORD name_length = static_cast<DWORD>(name.size());
    DWORD domain_length = static_cast<DWORD>(domain.size());
    SID_NAME_USE use{};
    if (LookupAccountSidW(nullptr, sid, name.data(), &name_length, domain.data(),
                          &domain_length, &use) != 0) {
        const std::string account = narrow(name.data());
        const std::string where = narrow(domain.data());
        return where.empty() ? account : where + "\\" + account;
    }

    LPWSTR text = nullptr;
    if (ConvertSidToStringSidW(sid, &text) != 0 && text != nullptr) {
        const std::string out = narrow(text);
        LocalFree(text);
        return out;
    }
    return "an unnameable principal";
}

// The current process's user SID, in a buffer the caller owns.
[[nodiscard]] Expected<std::vector<unsigned char>> current_user_sid() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        const DWORD code = GetLastError();
        return fail(std::format("could not open this process's token to find out who to grant "
                                "the token file to: {}",
                                win32_message(code)),
                    code);
    }

    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        const DWORD code = GetLastError();
        CloseHandle(token);
        return fail(std::format("could not size this process's user SID: {}",
                                win32_message(code)),
                    code);
    }

    std::vector<unsigned char> buffer(needed);
    if (GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed) == 0) {
        const DWORD code = GetLastError();
        CloseHandle(token);
        return fail(std::format("could not read this process's user SID: {}",
                                win32_message(code)),
                    code);
    }
    CloseHandle(token);
    return buffer;
}

// The principals a token file may name and still be private enough.
//
// THE ACCOUNT THIS PROCESS RUNS AS IS ON THE LIST, AND LEAVING IT OFF WAS A
// REAL BUG RATHER THAN A THEORETICAL ONE
//
// The first version of this function compared an ACE only against the file's
// OWNER, on the reasoning that mint_token grants the current user and so the
// current user is the owner. That is false whenever the process is elevated:
// a token whose default owner is the Administrators group makes every file it
// creates owned by BUILTIN\Administrators, so the ACE naming the operator is
// not the owner's, and the engine refused to load the file it had itself
// minted one line earlier. tests/rpc/test_rpc_token.cpp found it.
//
// So the question this asks is the one it always meant to: can anything
// OTHER than this process and the operating system read the file. An ACE
// naming the account this process runs as grants nothing, because this
// process is about to hold the token in memory anyway.
//
// The owner stays on the list for a different reason, and it is not
// politeness. Ownership carries WRITE_DAC implicitly, so an owner who is not
// us can grant themselves read at any moment; refusing their ACE would refuse
// a file they can read either way. Administrators is on the list on the same
// terms: an administrator can take ownership whatever the ACL says, so
// denying that group would be a statement about wishes rather than a control,
// and the operator is probably in it.
//
// Users, Everyone and Authenticated Users are what this is here to catch, and
// they are caught by not being on the list rather than by being enumerated: a
// check written against Everyone alone certifies one shape.
[[nodiscard]] bool sid_is_permitted(PSID sid, PSID owner, PSID self) {
    if (self != nullptr && EqualSid(sid, self) != 0) {
        return true;
    }
    if (owner != nullptr && EqualSid(sid, owner) != 0) {
        return true;
    }
    return IsWellKnownSid(sid, WinLocalSystemSid) != 0 ||
           IsWellKnownSid(sid, WinBuiltinAdministratorsSid) != 0 ||
           IsWellKnownSid(sid, WinCreatorOwnerSid) != 0;
}

// Read access in any of the spellings an ACE can carry it in. READ_CONTROL is
// deliberately absent: it grants reading the ACL, not the bytes.
constexpr DWORD kReadRights = FILE_READ_DATA | GENERIC_READ | GENERIC_ALL;

[[nodiscard]] Status check_token_file_acl(HANDLE file, const std::string& path) {
    // Not fatal if it cannot be had: the owner and the three well-known SIDs
    // still answer the question for every ordinary file, and refusing to
    // start because this process could not look up its own SID would be a
    // worse failure than the one being prevented.
    std::vector<unsigned char> self_buffer;
    PSID self = nullptr;
    if (auto found = current_user_sid(); found) {
        self_buffer = std::move(*found);
        self = reinterpret_cast<TOKEN_USER*>(self_buffer.data())->User.Sid;
    }

    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD rc =
        GetSecurityInfo(file, SE_FILE_OBJECT,
                        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                        nullptr, &dacl, nullptr, &descriptor);
    if (rc != ERROR_SUCCESS) {
        return fail(std::format("could not read the permissions on the token file {}: {}", path,
                                win32_message(rc)),
                    rc);
    }

    // A present-but-null DACL is not "no permissions", it is everyone with
    // everything, and it is what a caller passing an empty SECURITY_DESCRIPTOR
    // produces by accident.
    if (dacl == nullptr) {
        LocalFree(descriptor);
        return fail(std::format(
            "the token file {} has no access control list at all, which grants every account "
            "on this machine full control of it. Anything that can read this file can drive "
            "the radio. Delete it and a fresh one will be minted with the right permissions, "
            "or point --token-file somewhere else",
            path));
    }

    ACL_SIZE_INFORMATION size{};
    if (GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation) == 0) {
        const DWORD code = GetLastError();
        LocalFree(descriptor);
        return fail(std::format("could not walk the permissions on the token file {}: {}", path,
                                win32_message(code)),
                    code);
    }

    for (DWORD i = 0; i < size.AceCount; ++i) {
        LPVOID raw = nullptr;
        if (GetAce(dacl, i, &raw) == 0) {
            continue;
        }
        auto* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            continue;
        }
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
        if ((ace->Mask & kReadRights) == 0) {
            continue;
        }
        auto* sid = static_cast<PSID>(static_cast<void*>(&ace->SidStart));
        if (sid_is_permitted(sid, owner, self)) {
            continue;
        }

        const std::string who = describe_sid(sid);
        LocalFree(descriptor);
        return fail(std::format(
            "the token file {} can be read by {}, so anything running as that principal can "
            "drive the radio. Delete the file and a fresh one will be minted readable only by "
            "you and SYSTEM, or point --token-file at a path only you can read",
            path, who));
    }

    LocalFree(descriptor);
    return {};
}

// The DACL a freshly minted token file gets: this user and SYSTEM, protected
// so nothing is inherited from the directory.
class MintedAcl {
public:
    MintedAcl() = default;

    MintedAcl(const MintedAcl&) = delete;
    MintedAcl& operator=(const MintedAcl&) = delete;
    MintedAcl(MintedAcl&&) = delete;
    MintedAcl& operator=(MintedAcl&&) = delete;

    ~MintedAcl() {
        if (acl_ != nullptr) {
            LocalFree(acl_);
        }
    }

    [[nodiscard]] Status build() {
        auto user = current_user_sid();
        if (!user) {
            return std::unexpected(user.error());
        }
        user_ = std::move(*user);

        system_.resize(SECURITY_MAX_SID_SIZE);
        DWORD length = static_cast<DWORD>(system_.size());
        if (CreateWellKnownSid(WinLocalSystemSid, nullptr, system_.data(), &length) == 0) {
            const DWORD code = GetLastError();
            return fail(std::format("could not build the SYSTEM SID for the token file's "
                                    "permissions: {}",
                                    win32_message(code)),
                        code);
        }

        auto* token_user = reinterpret_cast<TOKEN_USER*>(user_.data());

        std::array<EXPLICIT_ACCESS_W, 2> entries{};
        entries[0].grfAccessPermissions = FILE_ALL_ACCESS;
        entries[0].grfAccessMode = SET_ACCESS;
        entries[0].grfInheritance = NO_INHERITANCE;
        entries[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entries[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
        entries[0].Trustee.ptstrName = static_cast<LPWSTR>(token_user->User.Sid);

        entries[1].grfAccessPermissions = FILE_ALL_ACCESS;
        entries[1].grfAccessMode = SET_ACCESS;
        entries[1].grfInheritance = NO_INHERITANCE;
        entries[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entries[1].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        entries[1].Trustee.ptstrName = static_cast<LPWSTR>(static_cast<void*>(system_.data()));

        const DWORD rc = SetEntriesInAclW(static_cast<ULONG>(entries.size()), entries.data(),
                                          nullptr, &acl_);
        if (rc != ERROR_SUCCESS || acl_ == nullptr) {
            return fail(std::format("could not build the token file's access control list: {}",
                                    win32_message(rc)),
                        rc);
        }

        if (InitializeSecurityDescriptor(&descriptor_, SECURITY_DESCRIPTOR_REVISION) == 0 ||
            SetSecurityDescriptorDacl(&descriptor_, TRUE, acl_, FALSE) == 0) {
            const DWORD code = GetLastError();
            return fail(std::format("could not build the token file's security descriptor: {}",
                                    win32_message(code)),
                        code);
        }

        // Without this the directory's inheritable ACEs are merged in, which
        // on a profile directory is exactly the grant being avoided.
        if (SetSecurityDescriptorControl(&descriptor_, SE_DACL_PROTECTED, SE_DACL_PROTECTED) ==
            0) {
            const DWORD code = GetLastError();
            return fail(std::format("could not mark the token file's permissions as not "
                                    "inherited: {}",
                                    win32_message(code)),
                        code);
        }
        return {};
    }

    [[nodiscard]] SECURITY_ATTRIBUTES attributes() {
        SECURITY_ATTRIBUTES out{};
        out.nLength = sizeof(out);
        out.lpSecurityDescriptor = &descriptor_;
        out.bInheritHandle = FALSE;
        return out;
    }

private:
    std::vector<unsigned char> user_;
    std::vector<unsigned char> system_;
    PACL acl_ = nullptr;
    SECURITY_DESCRIPTOR descriptor_{};
};

// One level only. A missing grandparent is left to CreateFileW, which reports
// ERROR_PATH_NOT_FOUND and names the path, and that is a different problem
// from the ordinary one this handles: %LOCALAPPDATA%\Revenant does not exist
// until something makes it.
void ensure_parent_directory(const std::wstring& path) {
    const std::size_t cut = path.find_last_of(L"\\/");
    if (cut == std::wstring::npos || cut == 0) {
        return;
    }
    const std::wstring parent = path.substr(0, cut);
    static_cast<void>(CreateDirectoryW(parent.c_str(), nullptr));
}

[[nodiscard]] Expected<Token> write_new_token_file(const std::wstring& wide,
                                                   const std::string& path) {
    auto minted = random_token();
    if (!minted) {
        return std::unexpected(minted.error());
    }

    MintedAcl acl;
    if (auto built = acl.build(); !built) {
        return std::unexpected(built.error());
    }
    SECURITY_ATTRIBUTES attributes = acl.attributes();

    ensure_parent_directory(wide);

    const HANDLE file = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        return fail(std::format("could not create the token file {}: {}", path,
                                win32_message(code)),
                    code);
    }

    const std::string text = token_to_hex(*minted) + "\n";
    DWORD written = 0;
    const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written,
                              nullptr);
    const DWORD code = GetLastError();
    CloseHandle(file);

    if (ok == 0 || written != text.size()) {
        return fail(std::format("could not write the token file {}: {}", path,
                                win32_message(code)),
                    code);
    }
    return *minted;
}

#endif  // _WIN32

}  // namespace

bool tokens_equal(std::span<const std::uint8_t> left,
                  std::span<const std::uint8_t> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    // volatile so the compiler cannot decide it may stop once this is nonzero,
    // which is the whole of what the loop is for.
    volatile unsigned char difference = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        difference = static_cast<unsigned char>(difference | (left[i] ^ right[i]));
    }
    return difference == 0;
}

std::string token_to_hex(const Token& token) {
    std::string out;
    out.resize(token.size() * 2);
    for (std::size_t i = 0; i < token.size(); ++i) {
        out[2 * i] = kHexDigits[token[i] >> 4U];
        out[(2 * i) + 1] = kHexDigits[token[i] & 0x0FU];
    }
    return out;
}

Expected<Token> token_from_hex(std::string_view text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' ||
                             text.back() == '\t')) {
        text.remove_suffix(1);
    }
    // EVERY REFUSAL IN THIS FUNCTION IS Unauthenticated, and the category is
    // about what a caller can do rather than about what is wrong. A token file
    // holding the wrong thing does not start holding the right thing because
    // somebody read it again: a supervisor that retries writes one line a
    // second until an operator happens to look at the window. What a client
    // does with that is ui/models/engine_link.cpp's business and it now has
    // something to branch on.
    if (text.empty()) {
        return fail(std::format("the token is empty, and it has to be {} hexadecimal characters",
                                kTokenBytes * 2),
                    ErrorCategory::Unauthenticated);
    }
    if (text.size() != kTokenBytes * 2) {
        return fail(std::format("the token is {} characters and it has to be exactly {}, which is "
                                "{} bytes written in hexadecimal",
                                text.size(), kTokenBytes * 2, kTokenBytes),
                    ErrorCategory::Unauthenticated);
    }

    Token out{};
    for (std::size_t i = 0; i < kTokenBytes; ++i) {
        const int high = hex_value(text[2 * i]);
        const int low = hex_value(text[(2 * i) + 1]);
        if (high < 0 || low < 0) {
            return fail(std::format("the token has a character that is not hexadecimal at "
                                    "position {}. It has to be {} characters from 0-9 and a-f",
                                    high < 0 ? 2 * i : (2 * i) + 1, kTokenBytes * 2),
                        ErrorCategory::Unauthenticated);
        }
        out[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return out;
}

#ifdef _WIN32

Expected<Token> random_token() {
    Token out{};
    const NTSTATUS status = BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()),
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        return fail(std::format("BCryptGenRandom could not produce {} random bytes for the RPC "
                                "token (status 0x{:08x})",
                                kTokenBytes, static_cast<unsigned long>(status)),
                    status);
    }
    return out;
}

Expected<std::string> default_token_path() {
    PWSTR folder = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr,
                                            &folder);
    if (FAILED(hr) || folder == nullptr) {
        if (folder != nullptr) {
            CoTaskMemFree(folder);
        }
        return fail(std::format("could not find this account's local application data folder, "
                                "so there is nowhere to keep the RPC token (HRESULT 0x{:08x}). "
                                "Pass --token-file to say where it should live",
                                static_cast<unsigned long>(hr)),
                    hr);
    }

    std::string out = narrow(folder);
    CoTaskMemFree(folder);
    if (out.empty()) {
        return fail("this account's local application data folder came back as an empty path");
    }
    out += "\\Revenant\\rpc-token";
    return out;
}

Expected<Token> load_token(const std::string& path) {
    auto wide = widen(path);
    if (!wide) {
        return std::unexpected(wide.error());
    }

    const HANDLE file = CreateFileW(wide->c_str(), GENERIC_READ | READ_CONTROL,
                                    FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();

        // A MISSING FILE IS THE ORDINARY STATE OF A CLIENT STARTED FIRST, and
        // it is the one failure here that is not about a credential. The engine
        // mints this file on its first run, so on a machine where no engine has
        // ever run there is nothing to read and nothing wrong. Unreachable says
        // wait; Unauthenticated would tell a supervisor to stop, which would
        // leave the window refusing to connect to an engine that then started.
        //
        // Anything else is a file that exists and cannot be read, which is a
        // permission an operator has to change.
        const bool absent = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
        return fail(std::format("could not open the token file {}: {}", path,
                                win32_message(code)),
                    absent ? ErrorCategory::Unreachable : ErrorCategory::Unauthenticated,
                    code);
    }

    // Before the bytes, not after: a file anything can read has already leaked
    // whatever is in it, and reading it first would be this process agreeing.
    //
    // The category is set here rather than inside check_token_file_acl, which
    // fails for two different reasons: a permission that is wrong, and a Win32
    // call that would not answer. Both leave this process unable to present a
    // credential and neither is fixed by asking again, so one category covers
    // them and the message says which happened.
    if (auto permitted = check_token_file_acl(file, path); !permitted) {
        CloseHandle(file);
        Error refused = permitted.error();
        refused.category = ErrorCategory::Unauthenticated;
        return std::unexpected(std::move(refused));
    }

    // Larger than any file this should accept, so one carrying more than a
    // token is refused by token_from_hex rather than silently truncated to
    // the right length here.
    //
    // WHAT THIS COMMENT USED TO SAY: "One byte more than the file should
    // hold". The buffer is seventy-two bytes and a minted file is
    // sixty-five, the sixty-four hex characters and a newline, so the slack
    // is seven bytes and not one. Eight are reserved because the line ending
    // an operator's editor leaves is not known here and only a run of
    // trailing whitespace is stripped; the point of the slack is that a
    // sixty-five byte read and a seventy-two byte read are distinguishable,
    // and any size past the longest acceptable file does that. The size was
    // right and the sentence describing it was not, which is the worse way
    // round: a reader trimming the buffer to match the sentence would have
    // cut it to sixty-six.
    std::array<char, (kTokenBytes * 2) + 8> buffer{};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                             nullptr);
    const DWORD code = GetLastError();
    CloseHandle(file);

    if (ok == 0) {
        return fail(std::format("could not read the token file {}: {}", path,
                                win32_message(code)),
                    ErrorCategory::Unauthenticated, code);
    }

    auto parsed = token_from_hex(std::string_view(buffer.data(), read));
    if (!parsed) {
        return std::unexpected(with_context(parsed.error(), std::format("in {}", path)));
    }
    return *parsed;
}

Expected<Token> mint_token(const std::string& path) {
    auto wide = widen(path);
    if (!wide) {
        return std::unexpected(wide.error());
    }
    return write_new_token_file(*wide, path);
}

Expected<Token> load_or_mint_token(const std::string& path) {
    auto wide = widen(path);
    if (!wide) {
        return std::unexpected(wide.error());
    }

    auto minted = write_new_token_file(*wide, path);
    if (minted) {
        return minted;
    }
    // Any other failure is reported as itself. Only "it is already there" is
    // an invitation to read it, which is also what the loser of a start-up
    // race between two engines gets.
    if (minted.error().code != ERROR_FILE_EXISTS &&
        minted.error().code != ERROR_ALREADY_EXISTS) {
        return minted;
    }

    // AND UNTIL 2026-09-20 THE LOSER OF THAT RACE COULD NOT READ IT, WHICH
    // IS THE ONE INTERLEAVING THIS FALL-THROUGH EXISTS FOR.
    //
    // write_new_token_file opens with CREATE_NEW and a share mode of zero,
    // so from engine A's CreateFileW until its CloseHandle the file exists
    // and nothing else may open it at all. Engine B arriving inside that
    // window is told ERROR_FILE_EXISTS by its own CREATE_NEW, falls through
    // to load_token, and load_token's open fails ERROR_SHARING_VIOLATION. B
    // then refused to start, on exactly the race the comment above says it
    // handles. The window is one WriteFile of sixty-five bytes wide, narrow
    // enough that every case in tests/rpc/test_rpc_token.cpp minted
    // sequentially and never reached it, and wide enough that two services
    // set to start at logon will.
    //
    // THE FIX IS NOT FILE_SHARE_READ ON THE MINT HANDLE. That lets B open
    // the file in the same window and read what is in it, which between
    // CreateFileW and WriteFile is nothing: B comes up refusing "the token
    // is empty" on a file that was a millisecond from being correct, and
    // the failure moves from loud to silent. The exclusive handle is what
    // makes the sharing violation the honest answer, so the answer is to
    // wait for it.
    //
    // Bounded, because a sharing violation that is not this race is a real
    // failure that has to be reported: a backup agent, an antivirus, or an
    // operator with the file open. Eight attempts with the wait doubling
    // from a millisecond is 255 ms of waiting in total, hundreds of times
    // the window and still inside a startup a person is watching.
    constexpr unsigned kSharingAttempts = 8;
    auto loaded = load_token(path);
    for (unsigned attempt = 0; attempt < kSharingAttempts && !loaded &&
                               loaded.error().code == ERROR_SHARING_VIOLATION;
         ++attempt) {
        Sleep(1U << attempt);
        loaded = load_token(path);
    }
    return loaded;
}

Expected<Token> rotate_token(const std::string& path) {
    auto wide = widen(path);
    if (!wide) {
        return std::unexpected(wide.error());
    }
    if (DeleteFileW(wide->c_str()) == 0) {
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
            return fail(std::format("could not remove the old token file {}: {}", path,
                                    win32_message(code)),
                        code);
        }
    }
    return write_new_token_file(*wide, path);
}

#else  // _WIN32

// The same arrangement core/engine/audio_wasapi.cpp uses for the platforms
// whose backend has not been written: the tree still builds, and the failure
// says which piece is missing rather than pretending there is a token.
namespace {
[[nodiscard]] Error not_supported() {
    return Error{"the RPC token store is implemented for Windows only so far. It needs a "
                 "platform source of random bytes and a file mode that keeps the token to one "
                 "account, and neither has been written for this platform",
                 ErrorCategory::Unimplemented};
}
}  // namespace

Expected<Token> random_token() { return std::unexpected(not_supported()); }
Expected<std::string> default_token_path() { return std::unexpected(not_supported()); }
Expected<Token> load_token(const std::string&) { return std::unexpected(not_supported()); }
Expected<Token> mint_token(const std::string&) { return std::unexpected(not_supported()); }
Expected<Token> load_or_mint_token(const std::string&) {
    return std::unexpected(not_supported());
}
Expected<Token> rotate_token(const std::string&) { return std::unexpected(not_supported()); }

#endif  // _WIN32

}  // namespace revenant::rpc
