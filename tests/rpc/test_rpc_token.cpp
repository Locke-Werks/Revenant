// The token file: minting it, loading it, and refusing the shapes that are
// not safe to load.
//
// NO GPU AND NO ENGINE. These are about a file on disk, and every case runs
// against a directory of its own under the system temp path so that several
// of them at once cannot collide and none of them touches a real profile.
// The wire cases in test_rpc_auth.cpp go the other way and never touch a file
// at all, because ServerOptions::token is bytes for exactly that reason.
//
// THE SHAPES, ENUMERATED BEFORE THE BAR WAS WRITTEN
//
// The natural bar here is "mint one, load it back, compare", and that
// certifies one reachable shape out of a dozen while reading as settled. So:
//
//   T1  No file. mint writes one, the ACL is this account's and SYSTEM's,
//       and a second start loads the same bytes.
//   T2  Two engines racing the same path. CREATE_NEW loses on the second,
//       which then loads the winner's file. Both serve the same token and
//       neither fails. Sequential, so it reaches the disposition and not
//       the interleaving; T13 is the interleaving.
//   T3  mint over a file that already exists. Refused, and refused
//       distinguishably from every other failure, because that is the one
//       refusal load_or_mint_token turns into a load.
//   T4  Unreadable. The open fails and the message names the path. It must
//       not silently mint a replacement, which would hand out a token the
//       operator's client is not holding.
//   T5  Readable by Everyone. Refused, naming the principal.
//   T6  Readable by Users. Refused as well. T5 alone would pass a check
//       written against the word "Everyone".
//   T7  Readable by Administrators. ACCEPTED. An administrator can take
//       ownership whatever the ACL says, so refusing that group would be a
//       statement about wishes, and the operator is probably in it. T5 and
//       T6 without this one are satisfied by a check that refuses
//       everything.
//   T8  Malformed and readable, five ways: empty, non-hex, one short, one
//       long, and correct hex with a CRLF line ending. The last is what an
//       operator who opened the file in Notepad produces and is the one of
//       these that will actually happen, so it has to LOAD rather than be
//       refused.
//   T9  The path is a directory.
//   T10 The parent directory does not exist.
//   T11 Uppercase hex loads. Writing is lowercase.
//   T12 rotate replaces the bytes, and the file is still private afterwards.
//   T13 The winner's exclusive handle is STILL OPEN when the loser looks.
//       The loser waits it out instead of failing the sharing violation,
//       which is the real shape of T2 and the one that was broken.
//   T14 A holder that never lets go. The wait is bounded and the sharing
//       violation arrives as itself.
//
// The comparison itself is not a file shape and is tested separately at the
// bottom: equal, one bit different in the first byte, one bit different in
// the last, and both length mismatches.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/rpc/token.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

namespace {

// A directory of its own per case, removed when the case ends however it
// ends. Cases here deliberately create files nothing else can read, so
// leaving one behind would leave litter an ordinary clean-up cannot remove.
class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                std::format("revenant-token-{}-{}", GetCurrentProcessId(),
                            counter.fetch_add(1));
        std::filesystem::create_directories(path_);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    ~TempDir() {
        // A file this account cannot read can still be deleted, because
        // delete is a permission on the containing directory. Ownership also
        // carries WRITE_DAC, so the ACL is put back first for the one case
        // that removed this account's own access.
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(path_, ignored)) {
            static_cast<void>(SetNamedSecurityInfoW(
                const_cast<LPWSTR>(entry.path().wstring().c_str()), SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr));
        }
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] std::string file(std::string_view name) const {
        return (path_ / name).string();
    }

    [[nodiscard]] std::string sub(std::string_view name) const {
        return (path_ / name).string();
    }

private:
    std::filesystem::path path_;
};

// The current process's user SID, in a buffer the caller owns.
[[nodiscard]] std::vector<unsigned char> my_sid() {
    HANDLE token = nullptr;
    REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != 0);
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<unsigned char> buffer(needed);
    const BOOL ok = GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed);
    CloseHandle(token);
    REQUIRE(ok != 0);
    return buffer;
}

[[nodiscard]] std::vector<unsigned char> well_known(WELL_KNOWN_SID_TYPE which) {
    std::vector<unsigned char> buffer(SECURITY_MAX_SID_SIZE);
    DWORD length = static_cast<DWORD>(buffer.size());
    REQUIRE(CreateWellKnownSid(which, nullptr, buffer.data(), &length) != 0);
    buffer.resize(length);
    return buffer;
}

// Writes a file with a protected DACL of the caller's choosing.
//
// owner_access of zero leaves this account out of the list entirely, which is
// how T4 makes a file this process cannot open. Ownership still carries
// READ_CONTROL and WRITE_DAC, so TempDir can still clean it up; it does not
// carry FILE_READ_DATA, which is what the open needs.
void write_with_dacl(const std::string& path, std::string_view content, DWORD owner_access,
                     const std::vector<WELL_KNOWN_SID_TYPE>& readers) {
    std::vector<unsigned char> owner = my_sid();
    std::vector<std::vector<unsigned char>> extra;
    extra.reserve(readers.size());
    for (WELL_KNOWN_SID_TYPE which : readers) {
        extra.push_back(well_known(which));
    }

    std::vector<EXPLICIT_ACCESS_W> entries;
    if (owner_access != 0) {
        EXPLICIT_ACCESS_W one{};
        one.grfAccessPermissions = owner_access;
        one.grfAccessMode = SET_ACCESS;
        one.grfInheritance = NO_INHERITANCE;
        one.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        one.Trustee.TrusteeType = TRUSTEE_IS_USER;
        one.Trustee.ptstrName =
            static_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(owner.data())->User.Sid);
        entries.push_back(one);
    }
    // Always at least one ACE, or SetEntriesInAcl makes an empty DACL and the
    // file is unopenable by anything, including the clean-up.
    {
        EXPLICIT_ACCESS_W system{};
        static std::vector<unsigned char> system_sid;
        system_sid = well_known(WinLocalSystemSid);
        system.grfAccessPermissions = FILE_ALL_ACCESS;
        system.grfAccessMode = SET_ACCESS;
        system.grfInheritance = NO_INHERITANCE;
        system.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        system.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        system.Trustee.ptstrName = static_cast<LPWSTR>(static_cast<void*>(system_sid.data()));
        entries.push_back(system);
    }
    for (auto& sid : extra) {
        EXPLICIT_ACCESS_W one{};
        one.grfAccessPermissions = FILE_GENERIC_READ;
        one.grfAccessMode = SET_ACCESS;
        one.grfInheritance = NO_INHERITANCE;
        one.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        one.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        one.Trustee.ptstrName = static_cast<LPWSTR>(static_cast<void*>(sid.data()));
        entries.push_back(one);
    }

    PACL acl = nullptr;
    REQUIRE(SetEntriesInAclW(static_cast<ULONG>(entries.size()), entries.data(), nullptr,
                             &acl) == ERROR_SUCCESS);

    SECURITY_DESCRIPTOR descriptor{};
    REQUIRE(InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) != 0);
    REQUIRE(SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) != 0);
    REQUIRE(SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED) !=
            0);

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = &descriptor;

    const std::wstring wide = std::filesystem::path(path).wstring();
    const HANDLE file = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD opened = GetLastError();
    INFO("CreateFileW on " << path << " reported " << opened);
    REQUIRE(file != INVALID_HANDLE_VALUE);

    DWORD written = 0;
    const BOOL ok = WriteFile(file, content.data(), static_cast<DWORD>(content.size()),
                              &written, nullptr);
    CloseHandle(file);
    LocalFree(acl);
    REQUIRE(ok != 0);
    REQUIRE(written == content.size());
}

// Every principal the file's DACL grants any read to, named.
[[nodiscard]] std::vector<std::string> readers_of(const std::string& path) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const std::wstring wide = std::filesystem::path(path).wstring();
    REQUIRE(GetNamedSecurityInfoW(wide.c_str(), SE_FILE_OBJECT,
                                  OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                  &owner, nullptr, &dacl, nullptr,
                                  &descriptor) == ERROR_SUCCESS);
    REQUIRE(dacl != nullptr);

    ACL_SIZE_INFORMATION size{};
    REQUIRE(GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation) != 0);

    std::vector<std::string> out;
    for (DWORD i = 0; i < size.AceCount; ++i) {
        LPVOID raw = nullptr;
        REQUIRE(GetAce(dacl, i, &raw) != 0);
        auto* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            continue;
        }
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
        if ((ace->Mask & (FILE_READ_DATA | GENERIC_READ | GENERIC_ALL)) == 0) {
            continue;
        }
        LPWSTR text = nullptr;
        auto* sid = static_cast<PSID>(static_cast<void*>(&ace->SidStart));
        REQUIRE(ConvertSidToStringSidW(sid, &text) != 0);
        std::array<char, 128> narrow{};
        WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow.data(),
                            static_cast<int>(narrow.size()), nullptr, nullptr);
        out.emplace_back(narrow.data());
        LocalFree(text);
    }
    LocalFree(descriptor);
    return out;
}

[[nodiscard]] std::string sid_string(PSID sid) {
    LPWSTR text = nullptr;
    REQUIRE(ConvertSidToStringSidW(sid, &text) != 0);
    std::array<char, 128> narrow{};
    WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow.data(), static_cast<int>(narrow.size()),
                        nullptr, nullptr);
    LocalFree(text);
    return {narrow.data()};
}

// my_sid hands back a TOKEN_USER and not a bare SID, so the SID is one
// indirection in. Getting that wrong produced a ConvertSidToStringSid that
// failed rather than a wrong answer, which is the good direction for a
// mistake to fail in.
[[nodiscard]] std::string my_sid_string() {
    std::vector<unsigned char> buffer = my_sid();
    return sid_string(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid);
}

[[nodiscard]] std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("a missing token file is minted, and the next start loads the same bytes",
          "[rpc][token]") {
    const TempDir dir;
    const std::string path = dir.file("rpc-token");

    auto first = rpc::load_or_mint_token(path);
    INFO(test::message_of(first));
    REQUIRE(first.has_value());

    // T1. On disk as 64 lowercase hex characters and one newline, which is
    // what makes it something a person can select and paste.
    const std::string text = read_all(path);
    CHECK(text.size() == (rpc::kTokenBytes * 2) + 1);
    CHECK(text.back() == '\n');
    CHECK(text.substr(0, rpc::kTokenBytes * 2) == rpc::token_to_hex(*first));

    // The ACL names this account and SYSTEM and nothing else. Read off the
    // file rather than assumed, because the whole point of building the
    // descriptor before CreateFileW is that the directory's inheritable ACEs
    // do not get merged in.
    const std::vector<std::string> readers = readers_of(path);
    const std::string mine = my_sid_string();
    std::vector<unsigned char> system_sid = well_known(WinLocalSystemSid);
    const std::string system = sid_string(system_sid.data());
    CHECK(readers.size() == 2);
    CHECK(std::ranges::find(readers, mine) != readers.end());
    CHECK(std::ranges::find(readers, system) != readers.end());

    auto second = rpc::load_or_mint_token(path);
    INFO(test::message_of(second));
    REQUIRE(second.has_value());
    CHECK(*second == *first);
}

TEST_CASE("two engines racing one token path both serve the winner's token", "[rpc][token]") {
    // T2 and T3. mint is CREATE_NEW, so the loser is told the file exists
    // rather than clobbering a token the winner has already handed out.
    //
    // THIS CASE IS NAMED FOR A RACE AND CONTAINS NONE. Both mints here run
    // to completion before the next call starts, so the winner's handle is
    // shut by the time the loser looks, and what is certified is the
    // disposition of CREATE_NEW: a shape that was never in doubt. The
    // interleaving the name promises is T13 below, and load_or_mint_token
    // failed it until 2026-09-20.
    const TempDir dir;
    const std::string path = dir.file("rpc-token");

    auto winner = rpc::mint_token(path);
    INFO(test::message_of(winner));
    REQUIRE(winner.has_value());

    auto loser = rpc::mint_token(path);
    REQUIRE_FALSE(loser.has_value());
    INFO(loser.error().message);
    CHECK(loser.error().code == ERROR_FILE_EXISTS);

    // And that exact failure, and only that one, is what load_or_mint turns
    // into a load.
    auto through = rpc::load_or_mint_token(path);
    INFO(test::message_of(through));
    REQUIRE(through.has_value());
    CHECK(*through == *winner);
}

TEST_CASE("a token file still held open by the engine minting it is waited for",
          "[rpc][token]") {
    // T13, THE RACE T2 IS NAMED FOR AND DOES NOT REACH.
    //
    // write_new_token_file opens CREATE_NEW with a share mode of zero, so
    // between the winner's CreateFileW and its CloseHandle the file exists
    // and nothing else may open it at all. The loser's CREATE_NEW is told
    // ERROR_FILE_EXISTS, which is the invitation to load, and its load then
    // failed ERROR_SHARING_VIOLATION. Every engine but the first refused to
    // start, on exactly the interleaving load_or_mint_token exists for.
    //
    // Reproduced by minting properly and then RE-OPENING the finished file
    // exclusively. That puts the loader in the state it is in mid-mint and
    // leaves the ACL the one a real mint wrote, so the case tests the
    // sharing window and not the permission check T4 covers.
    const TempDir dir;
    const std::string path = dir.file("rpc-token");

    auto winner = rpc::mint_token(path);
    INFO(test::message_of(winner));
    REQUIRE(winner.has_value());

    const std::wstring wide = std::filesystem::path(path).wstring();
    const HANDLE held = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);

    // Longer than the first three waits together, so the retry is what
    // carries this rather than the first attempt getting lucky, and far
    // inside the 255 ms the loop is bounded at.
    std::thread releaser([held]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        CloseHandle(held);
    });

    auto loser = rpc::load_or_mint_token(path);
    releaser.join();

    INFO(test::message_of(loser));
    REQUIRE(loser.has_value());
    CHECK(*loser == *winner);
}

TEST_CASE("a token file held open for good is reported rather than retried forever",
          "[rpc][token]") {
    // T14, the control for T13. A sharing violation that is NOT the mint
    // window is a real failure with a real cause, an operator or a backup
    // agent holding the file, and it has to arrive as itself. Without this
    // case an unbounded retry would satisfy T13 and hang a start-up instead
    // of failing it.
    const TempDir dir;
    const std::string path = dir.file("rpc-token");
    REQUIRE(rpc::mint_token(path).has_value());

    const std::wstring wide = std::filesystem::path(path).wstring();
    const HANDLE held = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);

    auto blocked = rpc::load_or_mint_token(path);
    CloseHandle(held);

    REQUIRE_FALSE(blocked.has_value());
    INFO(blocked.error().message);
    CHECK(blocked.error().code == ERROR_SHARING_VIOLATION);
    CHECK(blocked.error().message.find(path) != std::string::npos);
}

TEST_CASE("a token file this account cannot read is refused rather than replaced",
          "[rpc][token]") {
    // T4. Silently minting a replacement here would be the worst outcome
    // available: the engine would come up serving a token the operator's
    // client is not holding, and nothing would say why every login failed.
    const TempDir dir;
    const std::string path = dir.file("rpc-token");
    write_with_dacl(path, std::string(rpc::kTokenBytes * 2, 'a') + "\n", 0, {});

    auto loaded = rpc::load_token(path);
    REQUIRE_FALSE(loaded.has_value());
    INFO(loaded.error().message);
    CHECK(loaded.error().message.find(path) != std::string::npos);
    CHECK(loaded.error().code == ERROR_ACCESS_DENIED);

    // Still there, and still the same bytes. load_or_mint must report this
    // rather than route around it.
    auto through = rpc::load_or_mint_token(path);
    REQUIRE_FALSE(through.has_value());
    CHECK(through.error().code == ERROR_ACCESS_DENIED);
}

TEST_CASE("a token file other principals can read is refused, and names which one",
          "[rpc][token]") {
    const std::string good = std::string(rpc::kTokenBytes * 2, 'a') + "\n";

    SECTION("Everyone") {
        // T5.
        const TempDir dir;
        const std::string path = dir.file("rpc-token");
        write_with_dacl(path, good, FILE_ALL_ACCESS, {WinWorldSid});

        auto loaded = rpc::load_token(path);
        REQUIRE_FALSE(loaded.has_value());
        INFO(loaded.error().message);
        CHECK(loaded.error().message.find("can be read by") != std::string::npos);
        CHECK(loaded.error().message.find(path) != std::string::npos);

        // The recovery is in the refusal itself, because the operator reading
        // it is looking at a headless process that would not start.
        CHECK(loaded.error().message.find("--token-file") != std::string::npos);
    }

    SECTION("the Users group, which a check written against Everyone would miss") {
        // T6. This is the one that catches a bar naming one principal.
        const TempDir dir;
        const std::string path = dir.file("rpc-token");
        write_with_dacl(path, good, FILE_ALL_ACCESS, {WinBuiltinUsersSid});

        auto loaded = rpc::load_token(path);
        REQUIRE_FALSE(loaded.has_value());
        INFO(loaded.error().message);
        CHECK(loaded.error().message.find("can be read by") != std::string::npos);
    }

    SECTION("Authenticated Users") {
        const TempDir dir;
        const std::string path = dir.file("rpc-token");
        write_with_dacl(path, good, FILE_ALL_ACCESS, {WinAuthenticatedUserSid});

        auto loaded = rpc::load_token(path);
        REQUIRE_FALSE(loaded.has_value());
        INFO(loaded.error().message);
        CHECK(loaded.error().message.find("can be read by") != std::string::npos);
    }

    SECTION("Administrators, which is allowed") {
        // T7, and without it the three above are satisfied by a check that
        // refuses every file with more than one ACE. An administrator can
        // take ownership of this file whatever its ACL says, so denying that
        // group would be a statement about wishes rather than a control.
        const TempDir dir;
        const std::string path = dir.file("rpc-token");
        write_with_dacl(path, good, FILE_ALL_ACCESS, {WinBuiltinAdministratorsSid});

        auto loaded = rpc::load_token(path);
        INFO(test::message_of(loaded));
        CHECK(loaded.has_value());
    }
}

TEST_CASE("a readable token file that is not a token says which way it is wrong",
          "[rpc][token]") {
    const TempDir dir;

    // T8. Each of these opens cleanly and gets past the ACL check, so what is
    // being tested is the parse and nothing else.
    struct Case {
        const char* name;
        std::string content;
        const char* expect;
    };

    const std::vector<Case> cases{
        {"empty", "", "empty"},
        {"one character short", std::string((rpc::kTokenBytes * 2) - 1, 'a') + "\n",
         "characters and it has to be exactly"},
        {"one character long", std::string((rpc::kTokenBytes * 2) + 1, 'a') + "\n",
         "characters and it has to be exactly"},
        {"a character that is not hexadecimal",
         std::string((rpc::kTokenBytes * 2) - 1, 'a') + "z\n", "not hexadecimal"},
    };

    for (const Case& one : cases) {
        INFO(one.name);
        const std::string path = dir.file(std::format("rpc-token-{}", one.name));
        write_with_dacl(path, one.content, FILE_ALL_ACCESS, {});

        auto loaded = rpc::load_token(path);
        REQUIRE_FALSE(loaded.has_value());
        INFO(loaded.error().message);
        CHECK(loaded.error().message.find(one.expect) != std::string::npos);

        // The path is in every one of them, because the operator has to know
        // which file to fix and may have named it with --token-file.
        CHECK(loaded.error().message.find(path) != std::string::npos);

        // AND THE CATEGORY SURVIVES with_context, which is what puts the path
        // into the message above. token_from_hex sets Unauthenticated, load_token
        // wraps it with the filename, and a with_context that rebuilt the Error
        // without copying the category would leave every one of these reading as
        // Unclassified. That failure would look exactly like the category never
        // having been set at the source, which is the harder thing to find.
        CHECK(loaded.error().category == ErrorCategory::Unauthenticated);
    }
}

TEST_CASE("a token file that is not there yet is waited for, not refused",
          "[rpc][token]") {
    // THE ONE FAILURE IN THIS FILE THAT IS NOT ABOUT A CREDENTIAL
    //
    // The engine mints the token file on its first run, so on a machine where
    // no engine has ever run there is no file and nothing is wrong. A client
    // started before its engine has to keep asking, and that is the whole
    // difference between this case and the one above it: everything else here
    // is a file an operator must go and fix.
    //
    // Unreachable rather than Unclassified, because Unclassified is retried at
    // the ordinary rate by accident rather than on purpose, and this one is
    // retried at the ordinary rate deliberately.
    const TempDir dir;
    const std::string missing = dir.file("rpc-token-that-was-never-minted");

    auto loaded = rpc::load_token(missing);
    REQUIRE_FALSE(loaded.has_value());
    INFO(loaded.error().message);
    CHECK(loaded.error().category == ErrorCategory::Unreachable);
    CHECK(loaded.error().category != ErrorCategory::Unauthenticated);

    // The Win32 code is reported as well, because error.h reserves that field
    // for the originating API's own number and this failure has one.
    CHECK(loaded.error().code == static_cast<long long>(ERROR_FILE_NOT_FOUND));
}

TEST_CASE("a token file saved by Notepad loads", "[rpc][token]") {
    // T8's last row and the only one of them that will actually happen: the
    // file is 64 correct hex characters and a CRLF. Refusing it would be a
    // support question, so the trailing whitespace comes off rather than
    // being counted.
    const TempDir dir;
    const std::string path = dir.file("rpc-token");
    const std::string hex(rpc::kTokenBytes * 2, 'b');
    write_with_dacl(path, hex + "\r\n", FILE_ALL_ACCESS, {});

    auto loaded = rpc::load_token(path);
    INFO(test::message_of(loaded));
    REQUIRE(loaded.has_value());
    CHECK(rpc::token_to_hex(*loaded) == hex);
}

TEST_CASE("uppercase hex loads and is written back lowercase", "[rpc][token]") {
    // T11. Accepting either case costs nothing and removes the support
    // question an operator creates by reformatting the file.
    auto parsed = rpc::token_from_hex(std::string(rpc::kTokenBytes * 2, 'D'));
    INFO(test::message_of(parsed));
    REQUIRE(parsed.has_value());
    CHECK(rpc::token_to_hex(*parsed) == std::string(rpc::kTokenBytes * 2, 'd'));
}

TEST_CASE("a token path that is not a file says so", "[rpc][token]") {
    const TempDir dir;

    SECTION("the path is a directory") {
        // T9.
        const std::string path = dir.file("a-directory");
        std::filesystem::create_directory(path);

        auto loaded = rpc::load_token(path);
        REQUIRE_FALSE(loaded.has_value());
        INFO(loaded.error().message);
        CHECK(loaded.error().message.find(path) != std::string::npos);
    }

    SECTION("the grandparent does not exist") {
        // T10. mint creates one level of parent, deliberately, so this is
        // reported by CreateFileW naming the path rather than by this
        // building an arbitrary directory tree somebody mistyped.
        const std::string path = dir.sub("missing\\also-missing\\rpc-token");

        auto minted = rpc::mint_token(path);
        REQUIRE_FALSE(minted.has_value());
        INFO(minted.error().message);
        CHECK(minted.error().message.find(path) != std::string::npos);
        CHECK(minted.error().code == ERROR_PATH_NOT_FOUND);
    }
}

TEST_CASE("rotating replaces the token and leaves the file private", "[rpc][token]") {
    // T12.
    const TempDir dir;
    const std::string path = dir.file("rpc-token");

    auto first = rpc::mint_token(path);
    REQUIRE(first.has_value());

    auto second = rpc::rotate_token(path);
    INFO(test::message_of(second));
    REQUIRE(second.has_value());
    CHECK(*second != *first);

    CHECK(readers_of(path).size() == 2);

    auto reloaded = rpc::load_token(path);
    REQUIRE(reloaded.has_value());
    CHECK(*reloaded == *second);
}

TEST_CASE("two random tokens are not the same token", "[rpc][token]") {
    // Not a statistical claim and not pretending to be one. It is here
    // because a stub that returned a zeroed array would satisfy every other
    // case in this file, including the round trips.
    auto one = rpc::random_token();
    auto two = rpc::random_token();
    REQUIRE(one.has_value());
    REQUIRE(two.has_value());
    CHECK(*one != *two);
    CHECK(*one != rpc::Token{});
}

TEST_CASE("the token comparison reaches the end of both tokens", "[rpc][token]") {
    rpc::Token base{};
    for (std::size_t i = 0; i < base.size(); ++i) {
        base[i] = static_cast<std::uint8_t>(i);
    }

    CHECK(rpc::tokens_equal(base, base));

    // The first byte and the LAST byte. A compare written against a prefix
    // passes the first of these and fails the second, which is the mistake
    // worth catching: an early-exit memcmp is correct and leaks the prefix,
    // where a compare over too few bytes is outright wrong.
    rpc::Token first_differs = base;
    first_differs.front() = static_cast<std::uint8_t>(first_differs.front() ^ 0x01U);
    CHECK_FALSE(rpc::tokens_equal(base, first_differs));

    rpc::Token last_differs = base;
    last_differs.back() = static_cast<std::uint8_t>(last_differs.back() ^ 0x01U);
    CHECK_FALSE(rpc::tokens_equal(base, last_differs));

    // Both length mismatches, which take the branch rather than the loop.
    // Nothing is leaked by branching on a length that is published in the
    // schema, in the docs and in the size of the file.
    const std::span<const std::uint8_t> whole{base};
    CHECK_FALSE(rpc::tokens_equal(whole, whole.first(rpc::kTokenBytes - 1)));
    CHECK_FALSE(rpc::tokens_equal(whole.first(rpc::kTokenBytes - 1), whole));
    CHECK_FALSE(rpc::tokens_equal(whole, {}));
}
