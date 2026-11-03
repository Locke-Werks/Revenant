// serve_static: path containment, directory index, content types, caching.
//
// Each case builds a web root in a directory of its own under the system temp
// path, with a file next to the root that the root must never give away.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "tools/webgate/static_files.h"

namespace {

using revenant::webgate::serve_static;

class WebRoot {
public:
    WebRoot() {
        static std::atomic<unsigned> counter{0};
        base_ = std::filesystem::temp_directory_path() /
                ("revenant-webgate-static-" + std::to_string(GetCurrentProcessId()) + "-" +
                 std::to_string(counter.fetch_add(1)));
        std::filesystem::remove_all(base_);
        std::filesystem::create_directories(base_ / "root" / "assets");
        write(base_ / "secret", "outside the root");
        write(base_ / "root" / "index.html", "<html>home</html>");
        write(base_ / "root" / "sw.js", "// worker");
        write(base_ / "root" / "app.js", "// app");
        write(base_ / "root" / "assets" / "decode.wasm", "wasm");
        write(base_ / "root" / "assets" / "app.webmanifest", "{}");
        write(base_ / "root" / "assets" / "icon.png", "png");
    }
    ~WebRoot() {
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
    }
    WebRoot(const WebRoot&) = delete;
    WebRoot& operator=(const WebRoot&) = delete;

    [[nodiscard]] std::filesystem::path root() const { return base_ / "root"; }

private:
    static void write(const std::filesystem::path& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary);
        out << text;
    }

    std::filesystem::path base_;
};

}  // namespace

TEST_CASE(".. cannot escape the root", "[webgate][static]") {
    WebRoot web;
    CHECK(serve_static(web.root(), "/../secret").status == 404);
    CHECK(serve_static(web.root(), "/%2e%2e/secret").status == 404);
    CHECK(serve_static(web.root(), "/assets/../../secret").status == 404);
    CHECK(serve_static(web.root(), "/..%5csecret").status == 404);
    CHECK(serve_static(web.root(), "/%2E%2E%2Fsecret").status == 404);
    CHECK(serve_static(web.root(), "/index.html::$DATA").status == 404);
}

TEST_CASE("directory serves index.html", "[webgate][static]") {
    WebRoot web;
    const auto response = serve_static(web.root(), "/");
    CHECK(response.status == 200);
    CHECK(response.content_type == "text/html; charset=utf-8");
    CHECK(response.body == "<html>home</html>");

    CHECK(serve_static(web.root(), "/?x=1").status == 200);
}

TEST_CASE("a missing file is 404", "[webgate][static]") {
    WebRoot web;
    CHECK(serve_static(web.root(), "/nope.js").status == 404);
}

TEST_CASE("content types", "[webgate][static]") {
    WebRoot web;
    CHECK(serve_static(web.root(), "/app.js").content_type == "text/javascript");
    CHECK(serve_static(web.root(), "/assets/decode.wasm").content_type == "application/wasm");
    CHECK(serve_static(web.root(), "/assets/app.webmanifest").content_type ==
          "application/manifest+json");
    CHECK(serve_static(web.root(), "/assets/icon.png").content_type == "image/png");
}

TEST_CASE("service worker is never cached by the browser", "[webgate][static]") {
    WebRoot web;
    CHECK(serve_static(web.root(), "/sw.js").cache_control == "no-cache");
}
