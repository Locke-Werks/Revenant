#include "tools/webgate/static_files.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

namespace revenant::webgate {
namespace {

[[nodiscard]] int hex_value(char c) {
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

// nullopt for a malformed escape. The query and fragment are cut off first so
// a '?' inside one cannot be mistaken for the end of the path.
[[nodiscard]] std::optional<std::string> decode_path(std::string_view url) {
    url = url.substr(0, url.find_first_of("?#"));
    std::string out;
    out.reserve(url.size());
    for (std::size_t i = 0; i < url.size(); ++i) {
        if (url[i] != '%') {
            out += url[i];
            continue;
        }
        if (i + 2 >= url.size()) {
            return std::nullopt;
        }
        const int high = hex_value(url[i + 1]);
        const int low = hex_value(url[i + 2]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        out += static_cast<char>((high << 4) | low);
        i += 2;
    }
    return out;
}

[[nodiscard]] std::string lower_extension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

[[nodiscard]] std::string content_type_for(const std::string& ext) {
    struct Entry {
        std::string_view ext;
        std::string_view type;
    };
    static constexpr Entry kTypes[] = {
        {".html", "text/html; charset=utf-8"},
        {".js", "text/javascript"},
        {".mjs", "text/javascript"},
        {".css", "text/css"},
        {".json", "application/json"},
        {".webmanifest", "application/manifest+json"},
        {".wasm", "application/wasm"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".webp", "image/webp"},
        {".svg", "image/svg+xml"},
        {".ico", "image/x-icon"},
        {".woff2", "font/woff2"},
        {".txt", "text/plain; charset=utf-8"},
    };
    for (const Entry& entry : kTypes) {
        if (entry.ext == ext) {
            return std::string(entry.type);
        }
    }
    return "application/octet-stream";
}

[[nodiscard]] StaticResponse not_found() {
    return StaticResponse{404, "text/plain; charset=utf-8", "not found\n", "no-cache"};
}

}  // namespace

StaticResponse serve_static(const std::filesystem::path& root, std::string_view url_path) {
    const auto decoded = decode_path(url_path);
    if (!decoded || decoded->empty() || decoded->front() != '/') {
        return not_found();
    }
    // Characters that mean something to the Windows path layer and nothing to
    // a URL: a backslash is a separator, a colon names a drive or an
    // alternate data stream, and NUL ends the string early.
    if (decoded->find_first_of(std::string_view("\\:\0", 3)) != std::string::npos) {
        return not_found();
    }

    std::error_code ec;
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        return not_found();
    }
    std::filesystem::path target =
        std::filesystem::weakly_canonical(canonical_root / decoded->substr(1), ec);
    if (ec) {
        return not_found();
    }

    const std::filesystem::path relative = target.lexically_relative(canonical_root);
    if (relative.empty() || *relative.begin() == "..") {
        return not_found();
    }

    if (std::filesystem::is_directory(target, ec)) {
        target /= "index.html";
    }
    if (!std::filesystem::is_regular_file(target, ec)) {
        return not_found();
    }

    std::ifstream in(target, std::ios::binary);
    if (!in) {
        return not_found();
    }
    StaticResponse response;
    response.body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());

    const std::string ext = lower_extension(target);
    response.content_type = content_type_for(ext);
    // The worker script and the shell must be re-checked on every load or an
    // installed app keeps running yesterday's code. Hashed assets are not
    // listed here, so they get a short cache.
    const bool revalidate = target.filename() == "sw.js" || ext == ".html" || ext == ".webmanifest";
    response.cache_control = revalidate ? "no-cache" : "public, max-age=3600";
    return response;
}

}  // namespace revenant::webgate
