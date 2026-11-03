// Serving the web app's files from a directory on disk.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace revenant::webgate {

struct StaticResponse {
    int status = 200;
    std::string content_type;
    std::string body;
    std::string cache_control;
};

// Maps a request path (query string and fragment ignored, percent escapes
// decoded) to a file under `root`. A directory serves its index.html.
//
// Nothing outside `root` is ever returned: the decoded path is resolved,
// symlinks included, and refused with 404 unless it lands under the resolved
// root. A refusal and a missing file look the same on purpose.
[[nodiscard]] StaticResponse serve_static(const std::filesystem::path& root,
                                          std::string_view url_path);

}  // namespace revenant::webgate
