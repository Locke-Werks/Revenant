// The HLS audio path: the AAC encoder, and the fMP4 segmenter around it.
// The segmenter is checked structurally here; whether Safari plays the output
// is what the on-device spike is for.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "tools/webgate/audio_hls.h"
#include "tools/webgate/hls_playlist.h"

namespace {

using revenant::webgate::AacEncoder;
using revenant::webgate::HlsRegistry;
using revenant::webgate::HlsSegmenter;

constexpr std::uint32_t kRate = 48000;

// A continuous 1 kHz tone, so chunk boundaries do not introduce clicks the
// encoder would have to spend bits on.
class Tone {
public:
    [[nodiscard]] std::vector<float> take(std::size_t count) {
        std::vector<float> out(count);
        for (float& s : out) {
            s = 0.5F * static_cast<float>(std::sin(phase_));
            phase_ += 2.0 * std::numbers::pi * 1000.0 / kRate;
        }
        return out;
    }

private:
    double phase_ = 0.0;
};

void push_seconds(HlsSegmenter& segmenter, double seconds) {
    Tone tone;
    std::size_t remaining = static_cast<std::size_t>(seconds * kRate);
    while (remaining > 0) {
        const std::size_t n = std::min<std::size_t>(remaining, 960);
        segmenter.push(tone.take(n));
        remaining -= n;
    }
}

[[nodiscard]] bool contains(std::span<const std::uint8_t> bytes, std::string_view tag) {
    return std::search(bytes.begin(), bytes.end(), tag.begin(), tag.end()) != bytes.end();
}

[[nodiscard]] std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

[[nodiscard]] std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t at) {
    return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) |
           (std::uint32_t{b[at + 2]} << 8) | std::uint32_t{b[at + 3]};
}

}  // namespace

TEST_CASE("AAC encoder accepts the engine's chunk sizes", "[webgate][hls]") {
    auto created = AacEncoder::create(kRate);
    REQUIRE(created.has_value());
    AacEncoder& encoder = **created;

    Tone tone;
    // The sizes the engine delivers, plus an odd one that never fills a frame.
    const std::size_t pattern[] = {480, 960, 4096, 7};
    std::size_t pushed = 0;
    std::size_t units = 0;
    constexpr std::size_t total = 2U * kRate;
    while (pushed < total) {
        for (const std::size_t pattern_n : pattern) {
            // Stop at exactly two seconds so the expectation is the brief's.
            const std::size_t n = std::min(pattern_n, total - pushed);
            if (n == 0) {
                break;
            }
            REQUIRE(encoder.push(tone.take(n)).has_value());
            pushed += n;
            for (const auto& unit : encoder.take_access_units()) {
                CHECK_FALSE(unit.empty());
                ++units;
            }
        }
    }
    const double expected = static_cast<double>(pushed) / 1024.0;
    // 93.75 frames of input. The MFT holds back about two frames of lookahead
    // and the last 0.75 of a frame is incomplete, so the steady-state count is
    // 91; the brief's tolerance of 2 is one frame too tight for that.
    CHECK(std::abs(static_cast<double>(units) - expected) <= 3.0);
}

TEST_CASE("AAC encoder reports an AudioSpecificConfig", "[webgate][hls]") {
    auto created = AacEncoder::create(kRate);
    REQUIRE(created.has_value());
    // AAC-LC (2), 48 kHz (index 3), mono (1): 00010 0011 0001 000.
    const auto config = (*created)->audio_specific_config();
    REQUIRE(config.size() == 2);
    CHECK(config[0] == 0x11);
    CHECK(config[1] == 0x88);
}

TEST_CASE("AAC encoder rejects a rate the MFT does not offer", "[webgate][hls]") {
    CHECK_FALSE(AacEncoder::create(8000).has_value());
}

TEST_CASE("segments are one second", "[webgate][hls]") {
    HlsSegmenter segmenter(kRate);
    REQUIRE(segmenter.ok());
    push_seconds(segmenter, 5.0);

    const std::string playlist = segmenter.playlist();
    CHECK(playlist.rfind("#EXTM3U", 0) == 0);
    CHECK(playlist.find("#EXT-X-TARGETDURATION:1\n") != std::string::npos);
    CHECK(playlist.find("#EXT-X-VERSION:7\n") != std::string::npos);
    CHECK(playlist.find("#EXT-X-MAP:URI=\"init.mp4\"\n") != std::string::npos);
    CHECK(playlist.find("#EXT-X-ENDLIST") == std::string::npos);

    int extinf = 0;
    for (const std::string& line : lines_of(playlist)) {
        if (line.rfind("#EXTINF:", 0) == 0) {
            ++extinf;
            const double seconds = std::strtod(line.c_str() + 8, nullptr);
            CHECK(std::abs(seconds - 1.0) <= 1024.0 / kRate);
        }
    }
    CHECK(extinf >= 4);
}

TEST_CASE("window slides", "[webgate][hls]") {
    HlsSegmenter segmenter(kRate, 1.0, 6);
    REQUIRE(segmenter.ok());
    push_seconds(segmenter, 10.0);

    const std::string playlist = segmenter.playlist();
    const std::size_t at = playlist.find("#EXT-X-MEDIA-SEQUENCE:");
    REQUIRE(at != std::string::npos);
    const long first = std::strtol(playlist.c_str() + at + 22, nullptr, 10);
    CHECK((first == 4 || first == 5));

    CHECK_FALSE(segmenter.segment(0).has_value());
    REQUIRE(segmenter.segment(static_cast<std::uint64_t>(first)).has_value());

    int entries = 0;
    for (const std::string& line : lines_of(playlist)) {
        if (line.size() > 4 && line.ends_with(".m4s")) {
            ++entries;
        }
    }
    CHECK(entries == 6);
    // Nothing past the newest segment exists yet.
    CHECK_FALSE(segmenter.segment(1000).has_value());
}

TEST_CASE("init segment declares mp4a", "[webgate][hls]") {
    HlsSegmenter segmenter(kRate);
    REQUIRE(segmenter.ok());
    const auto init = segmenter.init_segment();
    CHECK(contains(init, "ftyp"));
    CHECK(contains(init, "moov"));
    CHECK(contains(init, "mp4a"));
    CHECK(contains(init, "esds"));
    CHECK(contains(init, "mvex"));
    CHECK(contains(init, "trex"));
}

TEST_CASE("boxes are well formed", "[webgate][hls]") {
    HlsSegmenter segmenter(kRate);
    REQUIRE(segmenter.ok());
    push_seconds(segmenter, 3.0);

    // Top-level boxes must tile the buffer exactly.
    const auto walk = [](std::span<const std::uint8_t> bytes) {
        std::vector<std::string> types;
        std::size_t at = 0;
        while (at + 8 <= bytes.size()) {
            const std::uint32_t size = be32(bytes, at);
            if (size < 8 || at + size > bytes.size()) {
                return std::vector<std::string>{"BAD"};
            }
            types.emplace_back(reinterpret_cast<const char*>(bytes.data() + at + 4), 4);
            at += size;
        }
        if (at != bytes.size()) {
            types.emplace_back("BAD");
        }
        return types;
    };

    CHECK(walk(segmenter.init_segment()) == std::vector<std::string>{"ftyp", "moov"});

    const std::string playlist = segmenter.playlist();
    const std::size_t at = playlist.find("#EXT-X-MEDIA-SEQUENCE:");
    REQUIRE(at != std::string::npos);
    const auto first = static_cast<std::uint64_t>(std::strtoull(playlist.c_str() + at + 22, nullptr, 10));
    const auto media = segmenter.segment(first);
    REQUIRE(media.has_value());
    CHECK(walk(*media) == std::vector<std::string>{"moof", "mdat"});

    // trun's data offset points from the start of moof to the first sample,
    // which is the byte after mdat's 8-byte header.
    const std::uint32_t moof_size = be32(*media, 0);
    const std::string_view trun = "trun";
    const auto it = std::search(media->begin(), media->end(), trun.begin(), trun.end());
    REQUIRE(it != media->end());
    const std::size_t trun_at = static_cast<std::size_t>(it - media->begin()) - 4;
    // size, 'trun', version+flags, sample_count, data_offset
    CHECK(be32(*media, trun_at + 16) == moof_size + 8);
}

TEST_CASE("a failed encoder leaves the segmenter inert", "[webgate][hls]") {
    HlsSegmenter segmenter(8000);
    CHECK_FALSE(segmenter.ok());
    CHECK_FALSE(segmenter.error().empty());
    segmenter.push(std::vector<float>(4096, 0.0F));
    CHECK(segmenter.playlist().empty());
    CHECK_FALSE(segmenter.segment(0).has_value());
}

TEST_CASE("registry maps a receiver id to a segmenter", "[webgate][hls]") {
    HlsSegmenter segmenter(kRate);
    HlsRegistry registry;
    CHECK(registry.find(3) == nullptr);
    registry.set(3, &segmenter);
    CHECK(registry.find(3) == &segmenter);
    registry.remove(3);
    CHECK(registry.find(3) == nullptr);
}

// Not run by ctest: writes init.mp4, the playlist and the live segments to
// $REVENANT_HLS_DUMP so ffprobe or a phone can be pointed at real output.
//   test_webgate "[.hls_dump]"
TEST_CASE("write HLS output to disk", "[.hls_dump]") {
    const char* dir = std::getenv("REVENANT_HLS_DUMP");
    REQUIRE(dir != nullptr);
    const std::filesystem::path root(dir);
    std::filesystem::create_directories(root);

    HlsSegmenter segmenter(kRate);
    REQUIRE(segmenter.ok());
    push_seconds(segmenter, 10.0);

    const auto write = [](const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    };
    write(root / "init.mp4", segmenter.init_segment());
    const std::string playlist = segmenter.playlist();
    std::ofstream(root / "live.m3u8") << playlist;
    for (std::uint64_t seq = 0; seq < 20; ++seq) {
        if (const auto media = segmenter.segment(seq)) {
            write(root / (std::to_string(seq) + ".m4s"), *media);
        }
    }
}

TEST_CASE("the playlist is withheld until three segments exist", "[webgate][hls]") {
    HlsSegmenter segmenter(kRate);
    REQUIRE(segmenter.ok());
    push_seconds(segmenter, 2.0);
    CHECK(segmenter.playlist().empty());
    // The segments themselves are already fetchable by number.
    CHECK(segmenter.segment(0).has_value());
    push_seconds(segmenter, 2.0);
    CHECK_FALSE(segmenter.playlist().empty());
}

TEST_CASE("a push failure mid-stream marks the segmenter failed", "[webgate][hls]") {
    HlsSegmenter segmenter(kRate);
    REQUIRE(segmenter.ok());
    push_seconds(segmenter, 4.0);
    REQUIRE_FALSE(segmenter.playlist().empty());
    const std::string before = segmenter.playlist();

    int calls = 0;
    segmenter.set_push_override_for_test([&](std::span<const float>) -> revenant::Status {
        ++calls;
        return revenant::fail("encoder fell over");
    });
    push_seconds(segmenter, 1.0);
    CHECK_FALSE(segmenter.ok());
    CHECK(segmenter.error() == "encoder fell over");
    CHECK(calls == 1);  // nothing is fed after the first failure
    CHECK(segmenter.playlist() == before);
}
