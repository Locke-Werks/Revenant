// core/transcribe/model_store.h without a network.
//
// Every case builds its own small "model" in a directory of its own and a
// ModelSpec describing it, so the size and hash logic runs over three bytes
// rather than 1.6 GB, and nothing here touches the real model directory except
// the one case that asks where it is.
//
// The one case that downloads the real model is hidden, [.download], and run
// by name: it is how the model gets onto a machine for the smoke test in
// test_whisper.cpp, and it is 1.6 GB.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/transcribe/model_store.h"
#include "tests/support/temp_path.h"

using namespace revenant;
using namespace revenant::transcribe;
namespace fs = std::filesystem;

namespace {

// SHA-256 of "abc", the one-block example in FIPS 180-2 Appendix B.1.
constexpr std::string_view kAbcHash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

// A directory that removes itself.
struct ScratchDirectory {
    fs::path path = test::unique_temp_path("revenant-model-store");
    ScratchDirectory() { fs::create_directories(path); }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    ~ScratchDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void write_file(const fs::path& path, std::string_view bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// A spec for three bytes. The URL is never fetched by the cases that use it.
constexpr ModelSpec kAbc{"tiny.bin", "https://127.0.0.1:1/tiny.bin", kAbcHash, 3};

}  // namespace

TEST_CASE("the model directory is LocalAppData's Revenant models folder", "[transcribe][model_store]")
{
    const fs::path directory = model_directory();
    CHECK(directory.filename() == "models");
    CHECK(directory.parent_path().filename() == "Revenant");
    CHECK(fs::is_directory(directory));

    // Beside the variable the shell sets, which names the same known folder.
    if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr) {
        CHECK(fs::equivalent(directory.parent_path().parent_path(), fs::path(local)));
    }
    CHECK(model_path(kDefaultModel) == directory / "ggml-large-v3-turbo.bin");
}

TEST_CASE("the default model is pinned to a commit, a size and a hash", "[transcribe][model_store]")
{
    // A URL on main would let the bytes change under the hash; the hash would
    // catch it, as a download that fails for everyone until somebody updates
    // the pin. A commit cannot move.
    CHECK(kDefaultModel.url.find("/resolve/main/") == std::string_view::npos);
    CHECK(kDefaultModel.url.find("/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/") != std::string_view::npos);
    CHECK(kDefaultModel.url.ends_with(kDefaultModel.name));
    CHECK(kDefaultModel.size == 1'624'555'275);
    CHECK(kDefaultModel.sha256 == "1fc70f774d38eb169993ac391eea357ef47c88757ef72ee5943879b7e8e2bc69");
}

TEST_CASE("SHA-256 of a file matches the published test vector", "[transcribe][model_store]")
{
    const ScratchDirectory scratch;
    const fs::path file = scratch.path / "abc";
    write_file(file, "abc");
    const auto digest = sha256_file(file);
    REQUIRE(digest);
    CHECK(*digest == kAbcHash);
}

TEST_CASE("a model that is absent or the wrong size is not ready", "[transcribe][model_store]")
{
    const ScratchDirectory scratch;
    CHECK(model_path(kAbc, scratch.path) == scratch.path / "tiny.bin");

    auto ready = model_ready(kAbc, scratch.path);
    REQUIRE(ready);
    CHECK_FALSE(*ready);

    write_file(scratch.path / "tiny.bin", "ab");
    ready = model_ready(kAbc, scratch.path);
    REQUIRE(ready);
    CHECK_FALSE(*ready);
    CHECK_FALSE(fs::exists(verified_path(scratch.path / "tiny.bin")));
}

TEST_CASE("a model with the right bytes is verified once and remembered", "[transcribe][model_store]")
{
    const ScratchDirectory scratch;
    const fs::path model = scratch.path / "tiny.bin";
    write_file(model, "abc");

    auto ready = model_ready(kAbc, scratch.path);
    REQUIRE(ready);
    CHECK(*ready);
    REQUIRE(fs::exists(verified_path(model)));

    // The sidecar is trusted while it describes the file. Same size, other
    // bytes, the modification time put back: a check that hashed every time
    // would refuse this, and that it does not is the proof that startup does
    // not read 1.6 GB.
    const auto when = fs::last_write_time(model);
    write_file(model, "abd");
    fs::last_write_time(model, when);
    ready = model_ready(kAbc, scratch.path);
    REQUIRE(ready);
    CHECK(*ready);

    // Anything that rewrites the file moves its time, and then it is hashed
    // again and refused, and the sidecar that no longer describes it goes.
    fs::last_write_time(model, when + std::chrono::seconds(5));
    ready = model_ready(kAbc, scratch.path);
    REQUIRE(ready);
    CHECK_FALSE(*ready);
    CHECK_FALSE(fs::exists(verified_path(model)));
}

TEST_CASE("a model with the wrong bytes is refused and left for the download to replace",
          "[transcribe][model_store]")
{
    const ScratchDirectory scratch;
    const fs::path model = scratch.path / "tiny.bin";
    write_file(model, "abd");

    const auto ready = model_ready(kAbc, scratch.path);
    REQUIRE(ready);
    CHECK_FALSE(*ready);
    CHECK(fs::exists(model));
    CHECK_FALSE(fs::exists(verified_path(model)));
}

TEST_CASE("a sidecar for another hash does not vouch for the file", "[transcribe][model_store]")
{
    const ScratchDirectory scratch;
    const fs::path model = scratch.path / "tiny.bin";
    write_file(model, "abd");
    const auto when = fs::last_write_time(model).time_since_epoch().count();
    write_file(verified_path(model), std::format("revenant-model-verified 1\nsha256 {}\nsize 3\nmtime {}\n",
                                                 std::string(64, '0'), when));

    const auto ready = model_ready(kAbc, scratch.path);
    REQUIRE(ready);
    CHECK_FALSE(*ready);
}

TEST_CASE("a complete download whose hash is wrong is deleted, naming both hashes", "[transcribe][model_store]")
{
    // A .part already at full size is verified without a request, so this is
    // the mismatch path end to end with no network: the bytes are refused,
    // the .part is gone, nothing was renamed into place, and the message says
    // what was expected and what arrived.
    const ScratchDirectory scratch;
    const fs::path part = scratch.path / "tiny.bin.part";
    write_file(part, "abd");
    const auto arrived = sha256_file(part);
    REQUIRE(arrived);

    const Status downloaded = download_model(kAbc, {}, scratch.path);
    REQUIRE_FALSE(downloaded);
    INFO(downloaded.error().message);
    CHECK(downloaded.error().message.find(std::string(kAbcHash)) != std::string::npos);
    CHECK(downloaded.error().message.find(*arrived) != std::string::npos);
    CHECK_FALSE(fs::exists(part));
    CHECK_FALSE(fs::exists(scratch.path / "tiny.bin"));
}

TEST_CASE("a complete download whose hash is right is moved into place and verified",
          "[transcribe][model_store]")
{
    const ScratchDirectory scratch;
    write_file(scratch.path / "tiny.bin.part", "abc");

    const Status downloaded = download_model(kAbc, {}, scratch.path);
    REQUIRE(downloaded);
    CHECK_FALSE(fs::exists(scratch.path / "tiny.bin.part"));
    CHECK(fs::exists(scratch.path / "tiny.bin"));
    CHECK(fs::exists(verified_path(scratch.path / "tiny.bin")));
    const auto ready = model_ready(kAbc, scratch.path);
    REQUIRE(ready);
    CHECK(*ready);
}

TEST_CASE("a download nobody answers fails in words, and keeps nothing", "[transcribe][model_store]")
{
    // Port 1 on loopback: refused at once on any machine, network or none.
    const ScratchDirectory scratch;
    const Status downloaded = download_model(kAbc, {}, scratch.path);
    REQUIRE_FALSE(downloaded);
    INFO(downloaded.error().message);
    CHECK(downloaded.error().message.find("could not connect to 127.0.0.1") != std::string::npos);
    CHECK_FALSE(fs::exists(scratch.path / "tiny.bin"));
}

TEST_CASE("a spec that is not https or not a hash is refused before any request", "[transcribe][model_store]")
{
    const ScratchDirectory scratch;
    constexpr ModelSpec kPlainHttp{"tiny.bin", "http://example.invalid/tiny.bin", kAbcHash, 3};
    const Status plain = download_model(kPlainHttp, {}, scratch.path);
    REQUIRE_FALSE(plain);
    CHECK(plain.error().message.find("https") != std::string::npos);

    constexpr ModelSpec kBadHash{"tiny.bin", "https://example.invalid/tiny.bin", "ABC", 3};
    const auto ready = model_ready(kBadHash, scratch.path);
    REQUIRE_FALSE(ready);
    CHECK(ready.error().message.find("SHA-256") != std::string::npos);

    constexpr ModelSpec kPathName{"..\\tiny.bin", "https://example.invalid/tiny.bin", kAbcHash, 3};
    CHECK_FALSE(download_model(kPathName, {}, scratch.path));
}

// Run by name: revenant_transcribe_tests "[.download]". Fetches the real model
// into model_directory(), resuming a .part if one is there, and prints its
// progress every 5%.
TEST_CASE("download the default Whisper model", "[.download]")
{
    int last_step = -1;
    const auto started = std::chrono::steady_clock::now();
    const Status downloaded = download_model(kDefaultModel, [&](const DownloadProgress& progress) {
        const int step = static_cast<int>(progress.done * 20 / std::max<std::uint64_t>(progress.total, 1));
        if (step != last_step) {
            last_step = step;
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::println("{:3}%  {:.2f} GB in {:.0f} s", step * 5, static_cast<double>(progress.done) / 1e9,
                         seconds);
        }
        return true;
    });
    INFO((downloaded ? std::string("ok") : downloaded.error().message));
    REQUIRE(downloaded);
    const auto ready = model_ready(kDefaultModel);
    REQUIRE(ready);
    CHECK(*ready);
    std::println("model at {}", model_path(kDefaultModel).string());
}

// Run by name with the one above. Resumes against the real server: the first
// 400 MB of the model already on this machine go into a scratch directory as
// a .part, and the download has to ask for the rest with a Range request,
// follow the redirect to the CDN with it, and hash the two halves into the
// pinned digest. Skips when the model is not here to cut from.
TEST_CASE("resume a partial download of the default Whisper model", "[.download]")
{
    const fs::path source = model_path(kDefaultModel);
    if (!fs::exists(source)) {
        SKIP("the model is not at " << source.string() << " to cut a partial download from");
    }
    const ScratchDirectory scratch;
    constexpr std::uint64_t kHead = 400'000'000;
    {
        std::ifstream in(source, std::ios::binary);
        std::ofstream out(scratch.path / "ggml-large-v3-turbo.bin.part", std::ios::binary);
        std::vector<char> buffer(1U << 20);
        std::uint64_t left = kHead;
        while (left > 0) {
            const auto take = static_cast<std::streamsize>(std::min<std::uint64_t>(left, buffer.size()));
            in.read(buffer.data(), take);
            out.write(buffer.data(), take);
            left -= static_cast<std::uint64_t>(take);
        }
    }

    std::uint64_t first = 0;
    std::uint64_t calls = 0;
    const Status downloaded = download_model(
        kDefaultModel,
        [&](const DownloadProgress& progress) {
            if (first == 0) {
                first = progress.done;
            }
            ++calls;
            return true;
        },
        scratch.path);
    INFO((downloaded ? std::string("ok") : downloaded.error().message));
    REQUIRE(downloaded);
    // The first report is the first chunk after the head, not one from zero.
    CHECK(first > kHead);
    CHECK(first <= kHead + (2U << 20));
    // Reported in megabyte steps, not once per read.
    CHECK(calls <= (kDefaultModel.size - kHead) / (1U << 20) + 2);
    const auto ready = model_ready(kDefaultModel, scratch.path);
    REQUIRE(ready);
    CHECK(*ready);
    std::println("resumed from {} bytes, first progress at {}, {} progress calls", kHead, first, calls);
}
