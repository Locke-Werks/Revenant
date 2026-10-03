// See model_store.h for what is pinned and why the hash is recorded.

#include "core/transcribe/model_store.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h, which they assume.
#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <winhttp.h>
#endif

// The User-Agent names the version, which core/transcribe/CMakeLists.txt
// passes in from cmake/RevenantVersion.cmake, the one place it is written.
#ifndef REVENANT_TRANSCRIBE_VERSION
#define REVENANT_TRANSCRIBE_VERSION "unknown"
#endif

namespace revenant::transcribe {
namespace {

namespace fs = std::filesystem;

// Read and written in pieces of this size, and the step a download reports
// progress in. WinHTTP hands a read over in whatever the socket has, 8 KB at a
// time from Hugging Face's CDN on 2026-10-03, which would be two hundred
// thousand progress calls for one model and a caller forwarding each one over
// the wire. A megabyte is a fraction of a second on any connection worth
// fetching 1.6 GB over, so a cancel still lands promptly.
constexpr std::size_t kChunk = 1U << 20;

// Hops followed before giving up. Hugging Face answers a resolve URL with one
// 302 to its CDN; ten is room for a proxy or a mirror and still stops a loop.
constexpr int kMaxRedirects = 10;

constexpr std::string_view kSidecarMagic = "revenant-model-verified 1";

[[nodiscard]] std::string display(const fs::path& path)
{
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

[[nodiscard]] std::string gigabytes(std::uint64_t bytes)
{
    return std::format("{:.2f} GB", static_cast<double>(bytes) / 1e9);
}

[[nodiscard]] bool is_hex_digest(std::string_view text)
{
    return text.size() == 64 && std::ranges::all_of(text, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

[[nodiscard]] Status validate(const ModelSpec& spec)
{
    if (spec.name.empty() || spec.name.find_first_of("/\\:") != std::string_view::npos) {
        return fail(std::format("'{}' is not a plain file name", spec.name));
    }
    if (!spec.url.starts_with("https://")) {
        return fail(std::format("the model URL must be https, and is '{}'", spec.url));
    }
    if (!is_hex_digest(spec.sha256)) {
        return fail(std::format("'{}' is not a lowercase hexadecimal SHA-256", spec.sha256));
    }
    if (spec.size == 0) {
        return fail("the model's size is zero");
    }
    return {};
}

#if defined(_WIN32)

[[nodiscard]] std::string windows_message(DWORD code, HMODULE module = nullptr)
{
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS |
                        (module != nullptr ? FORMAT_MESSAGE_FROM_HMODULE : FORMAT_MESSAGE_FROM_SYSTEM);
    const DWORD length = FormatMessageW(flags, module, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string out;
    if (length != 0 && buffer != nullptr) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        out.resize(static_cast<std::size_t>(bytes));
        WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), out.data(), bytes, nullptr, nullptr);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '.')) {
            out.pop_back();
        }
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return out.empty() ? std::format("error {}", code) : out;
}

[[nodiscard]] std::wstring widen(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

[[nodiscard]] std::string narrow(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    const int count =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count, nullptr, nullptr);
    return out;
}

// SHA-256 through CNG. BCRYPT_SHA256_ALG_HANDLE is the pseudo-handle Windows 10
// added, which needs no provider opened or closed, and CNG uses the CPU's SHA
// extensions where it has them, so hashing as bytes arrive costs nothing a
// download would notice.
class Sha256 {
public:
    [[nodiscard]] static Expected<Sha256> create()
    {
        Sha256 out;
        const NTSTATUS status = BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &out.hash_, nullptr, 0, nullptr, 0, 0);
        if (!BCRYPT_SUCCESS(status)) {
            return fail("Windows could not start a SHA-256 hash", static_cast<long long>(status));
        }
        return out;
    }

    Sha256() = default;
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    Sha256(Sha256&& other) noexcept : hash_(std::exchange(other.hash_, nullptr)) {}
    Sha256& operator=(Sha256&& other) noexcept
    {
        if (this != &other) {
            release();
            hash_ = std::exchange(other.hash_, nullptr);
        }
        return *this;
    }
    ~Sha256() { release(); }

    [[nodiscard]] Status update(std::span<const std::byte> bytes)
    {
        // ULONG lengths, so a span past 4 GiB goes in pieces. kChunk keeps
        // every caller far below that; the loop is for the contract.
        while (!bytes.empty()) {
            const std::size_t take = std::min<std::size_t>(bytes.size(), 1U << 30);
            const NTSTATUS status = BCryptHashData(
                hash_, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())), static_cast<ULONG>(take), 0);
            if (!BCRYPT_SUCCESS(status)) {
                return fail("Windows failed while hashing", static_cast<long long>(status));
            }
            bytes = bytes.subspan(take);
        }
        return {};
    }

    [[nodiscard]] Expected<std::string> finish()
    {
        std::array<unsigned char, 32> digest{};
        const NTSTATUS status = BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0);
        if (!BCRYPT_SUCCESS(status)) {
            return fail("Windows could not finish a SHA-256 hash", static_cast<long long>(status));
        }
        std::string hex;
        hex.reserve(64);
        for (const unsigned char byte : digest) {
            hex += std::format("{:02x}", byte);
        }
        return hex;
    }

private:
    void release() noexcept
    {
        if (hash_ != nullptr) {
            BCryptDestroyHash(hash_);
            hash_ = nullptr;
        }
    }

    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

// Feeds a file's bytes from offset 0 up to length into a hash.
[[nodiscard]] Status hash_file_into(Sha256& hash, const fs::path& path, std::uint64_t length)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail(std::format("could not open {} to hash it", display(path)));
    }
    std::vector<std::byte> buffer(kChunk);
    std::uint64_t left = length;
    while (left > 0) {
        const auto want = static_cast<std::streamsize>(std::min<std::uint64_t>(left, buffer.size()));
        in.read(reinterpret_cast<char*>(buffer.data()), want);
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            return fail(std::format("{} ended {} bytes before it should have while being hashed", display(path), left));
        }
        if (auto fed = hash.update(std::span(buffer).first(static_cast<std::size_t>(got))); !fed) {
            return fed;
        }
        left -= static_cast<std::uint64_t>(got);
    }
    return {};
}

// A Win32 file handle that closes itself.
class File {
public:
    File() = default;
    explicit File(HANDLE handle) : handle_(handle) {}
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    File& operator=(File&& other) noexcept
    {
        if (this != &other) {
            close();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    ~File() { close(); }

    [[nodiscard]] HANDLE get() const { return handle_; }
    [[nodiscard]] bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
    void close() noexcept
    {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// An HINTERNET that closes itself.
class Internet {
public:
    Internet() = default;
    explicit Internet(HINTERNET handle) : handle_(handle) {}
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
    Internet(Internet&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Internet& operator=(Internet&& other) noexcept
    {
        if (this != &other) {
            if (handle_ != nullptr) {
                WinHttpCloseHandle(handle_);
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    ~Internet()
    {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }

    [[nodiscard]] HINTERNET get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    HINTERNET handle_ = nullptr;
};

// A WinHTTP failure in words a person can act on. The codes that mean "no
// network" get said as that, because the alternative is "error 12007" in a
// client's status line and an operator searching for it.
[[nodiscard]] Error http_error(std::string_view step, std::string_view host)
{
    const DWORD code = GetLastError();
    std::string why;
    switch (code) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            why = std::format("could not look up {}. This machine has no network connection, or no "
                              "working DNS",
                              host);
            break;
        case ERROR_WINHTTP_CANNOT_CONNECT:
            why = std::format("could not connect to {}. This machine has no network connection, or a "
                              "firewall or proxy is blocking it",
                              host);
            break;
        case ERROR_WINHTTP_TIMEOUT:
            why = std::format("{} stopped answering and the request timed out. Try again; a partial "
                              "download resumes",
                              host);
            break;
        case ERROR_WINHTTP_CONNECTION_ERROR:
            why = std::format("the connection to {} was dropped. Try again; a partial download resumes", host);
            break;
        case ERROR_WINHTTP_SECURE_FAILURE:
            why = std::format("the secure connection to {} could not be established. The system clock "
                              "may be wrong, or a proxy or security product is intercepting HTTPS",
                              host);
            break;
        default:
            why = std::format("{} failed talking to {}: {}", step, host,
                              windows_message(code, GetModuleHandleW(L"winhttp.dll")));
            break;
    }
    return Error{std::move(why), static_cast<long long>(code)};
}

[[nodiscard]] std::optional<std::wstring> query_header_string(HINTERNET request, DWORD info)
{
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &bytes,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &bytes,
                            WINHTTP_NO_HEADER_INDEX) == FALSE) {
        return std::nullopt;
    }
    value.resize(bytes / sizeof(wchar_t));
    return value;
}

[[nodiscard]] std::optional<std::uint64_t> parse_u64(std::string_view text)
{
    std::uint64_t value = 0;
    const auto* end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return std::nullopt;
    }
    return value;
}

// "bytes START-END/TOTAL", RFC 9110 section 14.4.
struct ContentRange {
    std::uint64_t start = 0;
    std::uint64_t total = 0;
};

[[nodiscard]] std::optional<ContentRange> parse_content_range(std::string_view text)
{
    if (!text.starts_with("bytes ")) {
        return std::nullopt;
    }
    text.remove_prefix(6);
    const auto dash = text.find('-');
    const auto slash = text.find('/');
    if (dash == std::string_view::npos || slash == std::string_view::npos || slash < dash) {
        return std::nullopt;
    }
    const auto start = parse_u64(text.substr(0, dash));
    const auto total = parse_u64(text.substr(slash + 1));
    if (!start || !total) {
        return std::nullopt;
    }
    return ContentRange{*start, *total};
}

struct Url {
    bool https = false;
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring path;
};

[[nodiscard]] Expected<Url> crack(const std::wstring& url)
{
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    if (WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) == FALSE) {
        return fail(std::format("'{}' is not a URL WinHTTP can read", narrow(url)));
    }
    Url out;
    out.https = parts.nScheme == INTERNET_SCHEME_HTTPS;
    out.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    out.port = parts.nPort;
    out.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo != nullptr) {
        out.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (out.path.empty()) {
        out.path = L"/";
    }
    return out;
}

[[nodiscard]] Status write_all(const File& file, std::span<const std::byte> bytes, const fs::path& path)
{
    while (!bytes.empty()) {
        DWORD wrote = 0;
        const auto take = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), kChunk));
        if (WriteFile(file.get(), bytes.data(), take, &wrote, nullptr) == FALSE) {
            const DWORD code = GetLastError();
            if (code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL) {
                return fail(std::format("the disk holding {} is full. Free some space and try again; the "
                                        "download resumes where it stopped",
                                        display(path.parent_path())),
                            static_cast<long long>(code));
            }
            return fail(std::format("could not write {}: {}", display(path), windows_message(code)),
                        static_cast<long long>(code));
        }
        bytes = bytes.subspan(wrote);
    }
    return {};
}

// Opens the .part for appending, or truncated when fresh.
[[nodiscard]] Expected<File> open_part(const fs::path& part, bool fresh)
{
    HANDLE handle = CreateFileW(part.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                fresh ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        return fail(std::format("could not open {} for writing: {}", display(part), windows_message(code)),
                    static_cast<long long>(code));
    }
    File file(handle);
    LARGE_INTEGER zero{};
    if (SetFilePointerEx(file.get(), zero, nullptr, FILE_END) == FALSE) {
        const DWORD code = GetLastError();
        return fail(std::format("could not seek to the end of {}: {}", display(part), windows_message(code)),
                    static_cast<long long>(code));
    }
    return file;
}

// What one attempt at the transfer came to.
enum class Fetched {
    Complete,
    // The server refused the Range, 416, so the .part is not something it can
    // continue. Start again from nothing.
    RestartFromZero,
};

// GETs spec.url into part from byte `offset`, following redirects by hand so
// the Range header demonstrably goes to the host that serves the bytes and a
// redirect to plain http is refused rather than followed.
[[nodiscard]] Expected<Fetched> fetch(const ModelSpec& spec, const fs::path& part, std::uint64_t& offset,
                                      Sha256& hash,
                                      const std::function<bool(const DownloadProgress&)>& progress)
{
    const std::wstring agent = widen(std::format("Revenant/{} (model download)", REVENANT_TRANSCRIBE_VERSION));
    Internet session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        return std::unexpected(http_error("opening a WinHTTP session", "the network"));
    }
    // Resolve, connect and send at 30 s; a receive at 60 s, because a CDN
    // edge fetching a 1.6 GB object from origin can sit quiet that long before
    // the first byte.
    WinHttpSetTimeouts(session.get(), 30'000, 30'000, 30'000, 60'000);

    std::wstring url = widen(spec.url);
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        auto cracked = crack(url);
        if (!cracked) {
            return std::unexpected(cracked.error());
        }
        const std::string host = narrow(cracked->host);
        if (!cracked->https) {
            return fail(std::format("the download was redirected to {}, which is not https, and was not "
                                    "followed",
                                    narrow(url)));
        }

        Internet connection(WinHttpConnect(session.get(), cracked->host.c_str(), cracked->port, 0));
        if (!connection) {
            return std::unexpected(http_error("connecting", host));
        }
        Internet request(WinHttpOpenRequest(connection.get(), L"GET", cracked->path.c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE));
        if (!request) {
            return std::unexpected(http_error("opening the request", host));
        }
        DWORD no_redirects = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &no_redirects, sizeof(no_redirects));

        std::wstring headers;
        if (offset > 0) {
            headers = widen(std::format("Range: bytes={}-\r\n", offset));
        }
        if (WinHttpSendRequest(request.get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                               headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0,
                               0) == FALSE) {
            return std::unexpected(http_error("sending the request", host));
        }
        if (WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
            return std::unexpected(http_error("waiting for the answer", host));
        }

        DWORD status = 0;
        DWORD status_bytes = sizeof(status);
        if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_bytes,
                                WINHTTP_NO_HEADER_INDEX) == FALSE) {
            return std::unexpected(http_error("reading the status", host));
        }

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            auto location = query_header_string(request.get(), WINHTTP_QUERY_LOCATION);
            if (!location || location->empty()) {
                return fail(std::format("{} redirected without saying where to (HTTP {})", host, status));
            }
            if (location->front() == L'/') {
                *location = L"https://" + cracked->host + *location;
            }
            url = std::move(*location);
            continue;
        }

        bool fresh = false;
        if (status == 416) {
            return Fetched::RestartFromZero;
        }
        if (status == 200) {
            // The whole file. Either nothing was asked for, or the server
            // ignored the Range, and in both cases the bytes start at zero.
            if (offset > 0) {
                offset = 0;
                auto restarted = Sha256::create();
                if (!restarted) {
                    return std::unexpected(restarted.error());
                }
                hash = std::move(*restarted);
            }
            fresh = true;
            if (auto length = query_header_string(request.get(), WINHTTP_QUERY_CONTENT_LENGTH)) {
                const auto bytes = parse_u64(narrow(*length));
                if (bytes && *bytes != spec.size) {
                    return fail(std::format("{} offered {} bytes for {}, and the pinned size is {}. "
                                            "Nothing was written",
                                            host, *bytes, spec.name, spec.size));
                }
            }
        } else if (status == 206) {
            auto range_text = query_header_string(request.get(), WINHTTP_QUERY_CONTENT_RANGE);
            const auto range = range_text ? parse_content_range(narrow(*range_text)) : std::nullopt;
            if (!range || range->start != offset || range->total != spec.size) {
                return fail(std::format("{} answered the resume with a range that does not continue the "
                                        "partial download ({}). It was left as it is",
                                        host, range_text ? narrow(*range_text) : std::string("none")));
            }
        } else {
            return fail(std::format("{} answered HTTP {} for {}", host, status, spec.name),
                        static_cast<long long>(status));
        }

        auto file = open_part(part, fresh);
        if (!file) {
            return std::unexpected(file.error());
        }

        std::vector<std::byte> buffer(kChunk);
        std::uint64_t reported = offset;
        for (;;) {
            DWORD available = 0;
            if (WinHttpQueryDataAvailable(request.get(), &available) == FALSE) {
                return std::unexpected(http_error("receiving", host));
            }
            if (available == 0) {
                break;
            }
            DWORD read = 0;
            const auto want = static_cast<DWORD>(std::min<std::size_t>(available, buffer.size()));
            if (WinHttpReadData(request.get(), buffer.data(), want, &read) == FALSE) {
                return std::unexpected(http_error("receiving", host));
            }
            if (read == 0) {
                break;
            }
            if (offset + read > spec.size) {
                return fail(std::format("{} sent more than the pinned {} bytes of {}", host, spec.size, spec.name));
            }
            const auto bytes = std::span(buffer).first(read);
            if (auto wrote = write_all(*file, bytes, part); !wrote) {
                return std::unexpected(wrote.error());
            }
            if (auto fed = hash.update(bytes); !fed) {
                return std::unexpected(fed.error());
            }
            offset += read;
            const bool step = offset - reported >= kChunk || offset == spec.size;
            if (step) {
                reported = offset;
            }
            if (step && progress && !progress(DownloadProgress{offset, spec.size})) {
                FlushFileBuffers(file->get());
                return fail(std::format("the download was cancelled with {} of {} fetched; {} keeps it and "
                                        "the next attempt resumes from there",
                                        gigabytes(offset), gigabytes(spec.size), display(part)));
            }
        }
        if (FlushFileBuffers(file->get()) == FALSE) {
            const DWORD code = GetLastError();
            return fail(std::format("could not flush {}: {}", display(part), windows_message(code)),
                        static_cast<long long>(code));
        }
        return Fetched::Complete;
    }
    return fail(std::format("the download of {} was redirected more than {} times", spec.name, kMaxRedirects));
}

#endif  // _WIN32

struct Sidecar {
    std::string sha256;
    std::uint64_t size = 0;
    long long mtime = 0;
};

[[nodiscard]] std::optional<Sidecar> read_sidecar(const fs::path& path)
{
    std::ifstream in(path);
    if (!in) {
        return std::nullopt;
    }
    std::string magic;
    std::getline(in, magic);
    if (magic != kSidecarMagic) {
        return std::nullopt;
    }
    Sidecar out;
    std::string key;
    bool have_hash = false;
    bool have_size = false;
    bool have_mtime = false;
    while (in >> key) {
        if (key == "sha256") {
            have_hash = static_cast<bool>(in >> out.sha256);
        } else if (key == "size") {
            have_size = static_cast<bool>(in >> out.size);
        } else if (key == "mtime") {
            have_mtime = static_cast<bool>(in >> out.mtime);
        } else {
            return std::nullopt;
        }
    }
    if (!have_hash || !have_size || !have_mtime) {
        return std::nullopt;
    }
    return out;
}

// Written beside the model and renamed into place, so a sidecar that exists
// is a whole one. A failure here costs a rehash at the next start and nothing
// else, so the caller treats it as advisory.
[[nodiscard]] Status write_sidecar(const fs::path& model, const ModelSpec& spec)
{
    std::error_code status;
    const auto mtime = fs::last_write_time(model, status);
    if (status) {
        return fail(std::format("could not read the time of {}: {}", display(model), status.message()));
    }
    const fs::path sidecar = verified_path(model);
    fs::path temporary = sidecar;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        out << kSidecarMagic << '\n'
            << "sha256 " << spec.sha256 << '\n'
            << "size " << spec.size << '\n'
            << "mtime " << mtime.time_since_epoch().count() << '\n';
        if (!out) {
            return fail(std::format("could not write {}", display(temporary)));
        }
    }
    fs::rename(temporary, sidecar, status);
    if (status) {
        fs::remove(temporary, status);
        return fail(std::format("could not put {} in place", display(sidecar)));
    }
    return {};
}

}  // namespace

fs::path model_directory()
{
    fs::path base;
#if defined(_WIN32)
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &known)) &&
        known != nullptr) {
        base = known;
    }
    CoTaskMemFree(known);
#endif
    if (base.empty()) {
        base = fs::temp_directory_path();
    }
    fs::path directory = base / "Revenant" / "models";
    std::error_code ignored;
    fs::create_directories(directory, ignored);
    return directory;
}

fs::path model_path(const ModelSpec& spec)
{
    return model_path(spec, model_directory());
}

fs::path model_path(const ModelSpec& spec, const fs::path& directory)
{
    return directory / fs::path(std::u8string(spec.name.begin(), spec.name.end()));
}

fs::path verified_path(const fs::path& model)
{
    fs::path out = model;
    out += ".verified";
    return out;
}

Expected<std::string> sha256_file(const fs::path& path)
{
#if defined(_WIN32)
    std::error_code status;
    const std::uint64_t size = fs::file_size(path, status);
    if (status) {
        return fail(std::format("could not read the size of {}: {}", display(path), status.message()));
    }
    auto hash = Sha256::create();
    if (!hash) {
        return std::unexpected(hash.error());
    }
    if (auto fed = hash_file_into(*hash, path, size); !fed) {
        return std::unexpected(fed.error());
    }
    return hash->finish();
#else
    return fail(std::format("hashing {} needs Windows' CNG, and this is not Windows", display(path)));
#endif
}

Expected<bool> model_ready(const ModelSpec& spec)
{
    return model_ready(spec, model_directory());
}

Expected<bool> model_ready(const ModelSpec& spec, const fs::path& directory)
{
    if (auto valid = validate(spec); !valid) {
        return std::unexpected(valid.error());
    }
    const fs::path path = model_path(spec, directory);
    std::error_code status;
    const fs::file_status kind = fs::status(path, status);
    if (kind.type() == fs::file_type::not_found) {
        return false;
    }
    if (status) {
        return fail(std::format("could not look at {}: {}", display(path), status.message()));
    }
    if (kind.type() != fs::file_type::regular) {
        return fail(std::format("{} exists and is not a file", display(path)));
    }
    const std::uint64_t size = fs::file_size(path, status);
    if (status) {
        return fail(std::format("could not read the size of {}: {}", display(path), status.message()));
    }
    if (size != spec.size) {
        return false;
    }
    const auto mtime = fs::last_write_time(path, status);
    if (status) {
        return fail(std::format("could not read the time of {}: {}", display(path), status.message()));
    }

    if (const auto sidecar = read_sidecar(verified_path(path));
        sidecar && sidecar->sha256 == spec.sha256 && sidecar->size == spec.size &&
        sidecar->mtime == mtime.time_since_epoch().count()) {
        return true;
    }

    // No sidecar, or one describing another file or another time: hash it
    // now, once, and record the answer if it is the right one.
    auto digest = sha256_file(path);
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (*digest != spec.sha256) {
        fs::remove(verified_path(path), status);
        return false;
    }
    (void)write_sidecar(path, spec);
    return true;
}

Status download_model(const ModelSpec& spec, const std::function<bool(const DownloadProgress&)>& progress)
{
    return download_model(spec, progress, model_directory());
}

Status download_model(const ModelSpec& spec, const std::function<bool(const DownloadProgress&)>& progress,
                      const fs::path& directory)
{
#if defined(_WIN32)
    if (auto valid = validate(spec); !valid) {
        return valid;
    }
    std::error_code status;
    fs::create_directories(directory, status);
    if (status) {
        return fail(std::format("could not create the model directory {}: {}", display(directory),
                                status.message()));
    }

    auto ready = model_ready(spec, directory);
    if (!ready) {
        return std::unexpected(ready.error());
    }
    if (*ready) {
        return {};
    }

    const fs::path target = model_path(spec, directory);
    fs::path part = target;
    part += ".part";

    std::uint64_t offset = 0;
    if (fs::exists(part, status)) {
        offset = fs::file_size(part, status);
        if (status || offset > spec.size) {
            fs::remove(part, status);
            offset = 0;
        }
    }

    // Said before a byte moves, because running out at 1.5 GB of 1.6 is the
    // same failure an hour later.
    const fs::space_info space = fs::space(directory, status);
    if (!status && space.available < spec.size - offset) {
        return fail(std::format("the disk holding {} has {} free and the model needs {} more. Free some "
                                "space and try again",
                                display(directory), gigabytes(space.available), gigabytes(spec.size - offset)));
    }

    auto hash = Sha256::create();
    if (!hash) {
        return std::unexpected(hash.error());
    }
    if (offset > 0) {
        if (auto fed = hash_file_into(*hash, part, offset); !fed) {
            return fed;
        }
    }

    // One restart from nothing at most, for a server that refuses the range a
    // .part asks for.
    for (int attempt = 0; offset < spec.size; ++attempt) {
        auto fetched = fetch(spec, part, offset, *hash, progress);
        if (!fetched) {
            return std::unexpected(with_context(fetched.error(), std::format("downloading {}", spec.name)));
        }
        if (*fetched == Fetched::Complete) {
            break;
        }
        if (attempt > 0) {
            return fail(std::format("the server refused to resume {} twice", spec.name));
        }
        fs::remove(part, status);
        offset = 0;
        auto restarted = Sha256::create();
        if (!restarted) {
            return std::unexpected(restarted.error());
        }
        *hash = std::move(*restarted);
    }

    const std::uint64_t size = fs::file_size(part, status);
    if (status || size != spec.size) {
        fs::remove(part, status);
        return fail(std::format("the download of {} ended at {} bytes and the pinned size is {}. It was "
                                "deleted; try again",
                                spec.name, size, spec.size));
    }
    auto digest = hash->finish();
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (*digest != spec.sha256) {
        fs::remove(part, status);
        return fail(std::format("the download of {} does not match its pinned hash: expected {}, got {}. "
                                "It was deleted; try again, and if it happens twice the file at the "
                                "source has changed",
                                spec.name, spec.sha256, *digest));
    }

    fs::remove(verified_path(target), status);
    if (MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        const DWORD code = GetLastError();
        return fail(std::format("could not move {} into place as {}: {}", display(part), display(target),
                                windows_message(code)),
                    static_cast<long long>(code));
    }
    (void)write_sidecar(target, spec);
    return {};
#else
    (void)spec;
    (void)progress;
    return fail(std::format("downloading into {} needs WinHTTP, and this is not Windows", display(directory)));
#endif
}

}  // namespace revenant::transcribe
