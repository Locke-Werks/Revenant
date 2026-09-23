// read_recording_header, which is what the recording section shows before the
// engine is asked to open anything.
//
// EVERY TEST NAMES THE WRONG IMPLEMENTATION IT REJECTS, as
// test_source_choice.cpp does. The ones that matter here are a preview that
// disagrees with core/source/file_source.cpp: a sniff by extension rather than
// by signature, which calls a .wav holding raw IQ a WAV; a reader that takes
// the fmt tag at face value and refuses every WAVE_FORMAT_EXTENSIBLE file,
// which is every KF4FIC recording on this machine; one that reads an auxi
// centre of zero as DC; one that stops at the RF64 sentinel instead of the
// ds64 size; and one that takes captures[0] and calls a retuning SigMF
// recording openable.
//
// Every container is built byte by byte here rather than read off disk, so the
// cases run anywhere and say exactly which bytes they are about.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "models/recording_header.h"

using revenant::ui::read_recording_header;
using revenant::ui::RecordingFiles;
using revenant::ui::RecordingFormat;
using revenant::ui::RecordingHeader;
using revenant::ui::RecordingKind;

namespace {

using Bytes = std::vector<std::uint8_t>;

// A filesystem of named byte strings. Sizes can be declared larger than the
// bytes held, which is how a 2 GiB WAV is described without allocating one:
// the reader asks for headers only and never reads past them.
struct FakeDisk {
    std::map<std::string, Bytes> files;
    std::map<std::string, std::uint64_t> declared_sizes;

    [[nodiscard]] RecordingFiles view() const
    {
        RecordingFiles out;
        out.exists = [this](const std::string& path) { return files.contains(path); };
        out.size = [this](const std::string& path) -> std::optional<std::uint64_t> {
            if (const auto it = declared_sizes.find(path); it != declared_sizes.end()) {
                return it->second;
            }
            if (const auto it = files.find(path); it != files.end()) {
                return it->second.size();
            }
            return std::nullopt;
        };
        out.read = [this](const std::string& path, std::uint64_t offset,
                          std::size_t count) -> Bytes {
            const auto it = files.find(path);
            if (it == files.end() || offset >= it->second.size()) {
                return {};
            }
            const auto begin = it->second.begin() + static_cast<std::ptrdiff_t>(offset);
            const std::size_t available = it->second.size() - static_cast<std::size_t>(offset);
            const std::size_t take = count < available ? count : available;
            return Bytes(begin, begin + static_cast<std::ptrdiff_t>(take));
        };
        return out;
    }
};

void put16(Bytes& b, std::uint16_t v)
{
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(Bytes& b, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

void put64(Bytes& b, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

void put4(Bytes& b, std::string_view id)
{
    for (const char c : id) {
        b.push_back(static_cast<std::uint8_t>(c));
    }
}

struct WavSpec {
    std::string signature = "RIFF";
    std::uint16_t tag = 1;
    bool extensible = false;
    std::uint16_t channels = 2;
    std::uint32_t rate = 96000;
    std::uint16_t bits = 24;
    std::optional<std::uint32_t> auxi_center;
    std::uint32_t auxi_rate = 0;
    std::uint32_t data_bytes = 6 * 1000;
    bool rf64_sentinel = false;
    std::uint64_t ds64_data = 0;
};

// The header bytes of a WAV, up to and including the data chunk's header, and
// the size the whole file would have. The samples themselves are never held.
[[nodiscard]] std::pair<Bytes, std::uint64_t> make_wav(const WavSpec& spec)
{
    Bytes b;
    put4(b, spec.signature);
    put32(b, 0xFFFFFFFFu);
    put4(b, "WAVE");

    if (spec.signature == "RF64" || spec.signature == "BW64") {
        put4(b, "ds64");
        put32(b, 28);
        put64(b, 0);
        put64(b, spec.ds64_data);
        put64(b, 0);
        put32(b, 0);
    }

    put4(b, "fmt ");
    put32(b, spec.extensible ? 40 : 16);
    put16(b, spec.extensible ? 0xFFFE : spec.tag);
    put16(b, spec.channels);
    put32(b, spec.rate);
    put32(b, spec.rate * spec.channels * (spec.bits / 8u));
    put16(b, static_cast<std::uint16_t>(spec.channels * (spec.bits / 8)));
    put16(b, spec.bits);
    if (spec.extensible) {
        put16(b, 22);
        put16(b, spec.bits);
        put32(b, 3);
        // The SubFormat GUID, whose first two bytes are the real tag.
        put16(b, spec.tag);
        for (int i = 0; i < 14; ++i) {
            b.push_back(0);
        }
    }

    if (spec.auxi_center.has_value()) {
        put4(b, "auxi");
        put32(b, 60);
        for (int i = 0; i < 32; ++i) {
            b.push_back(0);
        }
        put32(b, *spec.auxi_center);
        put32(b, spec.auxi_rate);
        for (int i = 0; i < 20; ++i) {
            b.push_back(0);
        }
    }

    put4(b, "data");
    put32(b, spec.rf64_sentinel ? 0xFFFFFFFFu : spec.data_bytes);
    const std::uint64_t data = spec.rf64_sentinel ? spec.ds64_data : spec.data_bytes;
    return {b, b.size() + data};
}

[[nodiscard]] RecordingHeader read_wav(const WavSpec& spec, const std::string& path = "C:/r/x.wav")
{
    FakeDisk disk;
    auto [bytes, size] = make_wav(spec);
    disk.files[path] = bytes;
    disk.declared_sizes[path] = size;
    return read_recording_header(path, disk.view());
}

}  // namespace

TEST_CASE("a KF4FIC wideband WAV reads as 24-bit IQ at 96 kHz with no centre", "[recording]")
{
    // Rejects: a reader that takes the fmt tag and refuses 0xFFFE. Every one
    // of the six recordings on this machine is WAVE_FORMAT_EXTENSIBLE with a
    // PCM SubFormat, and docs/recordings.md has that read off the real header.
    WavSpec spec;
    spec.extensible = true;
    spec.data_bytes = 2'146'437'120u;
    const RecordingHeader header = read_wav(spec);

    REQUIRE(header.readable());
    CHECK(header.kind == RecordingKind::Wav);
    CHECK(header.container == "RIFF WAV");
    CHECK(header.format == RecordingFormat::Cs24);
    CHECK(header.has_rate);
    CHECK(header.rate == 96000);
    CHECK(header.channels == "2, I and Q");
    CHECK_FALSE(header.has_center);
    CHECK(header.length_samples(RecordingFormat::Unknown) == 357'739'520u);
    REQUIRE(header.notes.size() == 1);
    CHECK(header.notes.front().find("no auxi") != std::string::npos);
}

TEST_CASE("an auxi chunk's centre is read, and a zero one is not DC", "[recording]")
{
    // Rejects: a zero centre passed off as a frequency. The recorders write
    // zero when they had nothing to put there, and the engine treats it as
    // absent, so the preview has to ask for a centre exactly when the engine
    // would present the stream at 0 Hz.
    WavSpec spec;
    spec.tag = 1;
    spec.bits = 16;
    spec.rate = 2'000'000;
    spec.auxi_center = 7'100'000u;
    spec.data_bytes = 4 * 500;
    const RecordingHeader stated = read_wav(spec);
    REQUIRE(stated.readable());
    CHECK(stated.format == RecordingFormat::Cs16);
    CHECK(stated.has_center);
    CHECK(stated.center_hz == 7'100'000);

    spec.auxi_center = 0u;
    const RecordingHeader zero = read_wav(spec);
    REQUIRE(zero.readable());
    CHECK_FALSE(zero.has_center);
    REQUIRE(zero.notes.size() == 1);
    CHECK(zero.notes.front().find("zero") != std::string::npos);
}

TEST_CASE("an auxi rate that disagrees with the fmt chunk is refused, as the engine does",
          "[recording]")
{
    // Rejects: a preview that picks one of two rates. The engine refuses the
    // file naming both, and a preview that opened the button would send a
    // request answered by that refusal.
    WavSpec spec;
    spec.bits = 16;
    spec.rate = 2'000'000;
    spec.auxi_center = 7'100'000u;
    spec.auxi_rate = 2'400'000u;
    spec.data_bytes = 4 * 10;
    const RecordingHeader header = read_wav(spec);
    CHECK_FALSE(header.readable());
    CHECK(header.refusal.find("2400000") != std::string::npos);
}

TEST_CASE("RF64 takes its data size from ds64, past the 32-bit sentinel", "[recording]")
{
    // Rejects: stopping at the all-ones sentinel. A recording past 4 GiB is
    // exactly the one written as RF64, and reading its length as 4 GiB would
    // show an hour-long capture as eight minutes.
    WavSpec spec;
    spec.signature = "RF64";
    spec.bits = 16;
    spec.rate = 2'000'000;
    spec.rf64_sentinel = true;
    spec.ds64_data = 8'000'000'000ull;
    const RecordingHeader header = read_wav(spec, "C:/r/long.rf64");
    REQUIRE(header.readable());
    CHECK(header.container == "RF64");
    CHECK(header.data_bytes == 8'000'000'000ull);
    CHECK(header.length_samples(RecordingFormat::Unknown) == 2'000'000'000ull);
}

TEST_CASE("a WAV that is not two channels, or not a width the engine reads, is refused",
          "[recording]")
{
    // Rejects: a preview more permissive than the engine. A mono WAV is an
    // audio recording and a 32-bit PCM one has no upload kernel; both are
    // refused by core/source/file_source.cpp and have to be refused here.
    WavSpec mono;
    mono.channels = 1;
    CHECK(read_wav(mono).refusal.find("1 channels") != std::string::npos);

    WavSpec wide;
    wide.bits = 32;
    CHECK(read_wav(wide).refusal.find("format tag 1 at 32 bits") != std::string::npos);

    WavSpec riff_past_4g;
    riff_past_4g.data_bytes = 0xFFFFFF00u;
    FakeDisk disk;
    auto [bytes, size] = make_wav(riff_past_4g);
    disk.files["C:/r/big.wav"] = bytes;
    disk.declared_sizes["C:/r/big.wav"] = 5'000'000'000ull;
    CHECK(read_recording_header("C:/r/big.wav", disk.view()).refusal.find("4 GiB") !=
          std::string::npos);
}

TEST_CASE("the container is sniffed from the bytes and not the extension", "[recording]")
{
    // Rejects: trusting the name. core/source/file_source.cpp sniffs the
    // signature, so a .wav holding raw IQ is raw to the engine and has to be
    // raw here, and a .dat written by a WAV recorder is a WAV.
    FakeDisk disk;
    disk.files["C:/r/raw.wav"] = Bytes(4000, 0x7F);
    const RecordingHeader raw = read_recording_header("C:/r/raw.wav", disk.view());
    CHECK(raw.kind == RecordingKind::Raw);
    CHECK(raw.format == RecordingFormat::Unknown);
    CHECK(raw.data_bytes == 4000);

    WavSpec spec;
    spec.bits = 16;
    auto [bytes, size] = make_wav(spec);
    disk.files["C:/r/capture.dat"] = bytes;
    disk.declared_sizes["C:/r/capture.dat"] = size;
    CHECK(read_recording_header("C:/r/capture.dat", disk.view()).kind == RecordingKind::Wav);
}

TEST_CASE("a raw file's format comes from its extension and nothing else", "[recording]")
{
    // Rejects: a guessed format. A .cs16 names one; a .iq names nothing and
    // the section has to ask, as the engine would.
    FakeDisk disk;
    disk.files["C:/r/a.cs16"] = Bytes(400, 0);
    disk.files["C:/r/b.iq"] = Bytes(400, 0);
    const RecordingHeader named = read_recording_header("C:/r/a.cs16", disk.view());
    CHECK(named.kind == RecordingKind::Raw);
    CHECK(named.format == RecordingFormat::Cs16);
    CHECK_FALSE(named.has_rate);
    CHECK_FALSE(named.has_center);
    CHECK(named.length_samples(RecordingFormat::Unknown) == 100);

    const RecordingHeader unnamed = read_recording_header("C:/r/b.iq", disk.view());
    CHECK(unnamed.format == RecordingFormat::Unknown);
    CHECK(unnamed.length_samples(RecordingFormat::Cf32) == 50);
}

TEST_CASE("SigMF is read from its sidecar, whichever half was picked", "[recording]")
{
    // Rejects: a reader that only understands the .sigmf-meta half. The
    // dialog offers both, and the engine resolves either to the pair.
    FakeDisk disk;
    const std::string meta = R"({
        "global": {"core:version": "1.0.0", "core:datatype": "ci16_le",
                   "core:sample_rate": 2400000},
        "captures": [{"core:sample_start": 0, "core:frequency": 98100000}],
        "annotations": []
    })";
    disk.files["C:/r/fm.sigmf-meta"] = Bytes(meta.begin(), meta.end());
    disk.files["C:/r/fm.sigmf-data"] = Bytes(4 * 2400, 0);

    for (const std::string& picked : {std::string("C:/r/fm.sigmf-meta"),
                                      std::string("C:/r/fm.sigmf-data")}) {
        const RecordingHeader header = read_recording_header(picked, disk.view());
        REQUIRE(header.readable());
        CHECK(header.kind == RecordingKind::Sigmf);
        CHECK(header.format == RecordingFormat::Cs16);
        CHECK(header.rate == 2'400'000);
        CHECK(header.has_center);
        CHECK(header.center_hz == 98'100'000);
        CHECK(header.channels == "1 complex");
        CHECK(header.data_path == "C:/r/fm.sigmf-data");
        CHECK(header.length_samples(RecordingFormat::Unknown) == 2400);
    }
}

TEST_CASE("a raw file with a sidecar beside it is SigMF, as the engine sniffs it", "[recording]")
{
    // Rejects: the extension winning over a sidecar. core/source/file_source.cpp
    // looks for x.cs16.sigmf-meta before it reads a byte of x.cs16.
    FakeDisk disk;
    const std::string meta = R"({"global": {"core:version": "1.2.0", "core:datatype": "cf32_le",
        "core:dataset": "x.cs16"}, "captures": []})";
    disk.files["C:/r/x.cs16.sigmf-meta"] = Bytes(meta.begin(), meta.end());
    disk.files["C:/r/x.cs16"] = Bytes(80, 0);
    const RecordingHeader header = read_recording_header("C:/r/x.cs16", disk.view());
    REQUIRE(header.readable());
    CHECK(header.kind == RecordingKind::Sigmf);
    CHECK(header.format == RecordingFormat::Cf32);
    CHECK_FALSE(header.has_rate);
    CHECK_FALSE(header.has_center);
    CHECK(header.data_path == "C:/r/x.cs16");
    REQUIRE(header.notes.size() == 1);
}

TEST_CASE("a SigMF recording that retunes is refused, and one that does not is not",
          "[recording]")
{
    // Rejects: taking captures[0] and playing the file through. Everything
    // after the retune would land at a frequency nobody transmitted on; the
    // engine refuses without segment=, which this section does not offer.
    FakeDisk disk;
    const std::string retunes = R"({"global": {"core:version": "1.0.0", "core:datatype": "cu8",
        "core:sample_rate": 1000}, "captures": [
        {"core:sample_start": 0, "core:frequency": 7100000},
        {"core:sample_start": 10, "core:frequency": 14100000}]})";
    disk.files["C:/r/t.sigmf-meta"] = Bytes(retunes.begin(), retunes.end());
    disk.files["C:/r/t.sigmf-data"] = Bytes(40, 0);
    const RecordingHeader moving = read_recording_header("C:/r/t.sigmf-meta", disk.view());
    CHECK_FALSE(moving.readable());
    CHECK(moving.refusal.find("retunes") != std::string::npos);

    const std::string steady = R"({"global": {"core:version": "1.0.0", "core:datatype": "cu8",
        "core:sample_rate": 1000}, "captures": [
        {"core:sample_start": 0, "core:frequency": 7100000},
        {"core:sample_start": 10, "core:frequency": 7100000}]})";
    disk.files["C:/r/s.sigmf-meta"] = Bytes(steady.begin(), steady.end());
    disk.files["C:/r/s.sigmf-data"] = Bytes(40, 0);
    const RecordingHeader still = read_recording_header("C:/r/s.sigmf-meta", disk.view());
    CHECK(still.readable());
    CHECK(still.center_hz == 7'100'000);
}

TEST_CASE("SigMF datatypes the engine does not read are refused in words", "[recording]")
{
    // Rejects: a preview that opens a real or big-endian dataset. The engine
    // refuses both, and a real one read as IQ would be a picture rather than
    // a failure.
    FakeDisk disk;
    const auto sidecar = [&disk](const std::string& type) {
        const std::string meta = R"({"global": {"core:version": "1.0.0", "core:datatype": ")" +
                                 type + R"("}, "captures": []})";
        disk.files["C:/r/d.sigmf-meta"] = Bytes(meta.begin(), meta.end());
        disk.files["C:/r/d.sigmf-data"] = Bytes(8, 0);
        return read_recording_header("C:/r/d.sigmf-meta", disk.view());
    };
    CHECK(sidecar("rf32_le").refusal.find("real") != std::string::npos);
    CHECK(sidecar("ci16_be").refusal.find("big-endian") != std::string::npos);
    CHECK(sidecar("ci32_le").refusal.find("not one the engine reads") != std::string::npos);
    CHECK(sidecar("cu8").readable());
}

TEST_CASE("a missing file or a missing sidecar is said, not previewed", "[recording]")
{
    // Rejects: an empty preview that reads as a file with nothing in it.
    FakeDisk disk;
    CHECK(read_recording_header("C:/r/gone.wav", disk.view()).refusal == "the file is not there");
    disk.files["C:/r/lonely.sigmf-data"] = Bytes(8, 0);
    CHECK(read_recording_header("C:/r/lonely.sigmf-data", disk.view()).refusal.find(
              "lonely.sigmf-meta is not there") != std::string::npos);
}

TEST_CASE("a malformed sidecar is refused rather than half read", "[recording]")
{
    // Rejects: a parser that stops at the first error and returns what it
    // had, which would preview a rate from a file whose datatype it never saw.
    FakeDisk disk;
    const std::string broken = R"({"global": {"core:version": "1.0.0", "core:datatype": "cu8")";
    disk.files["C:/r/b.sigmf-meta"] = Bytes(broken.begin(), broken.end());
    disk.files["C:/r/b.sigmf-data"] = Bytes(8, 0);
    CHECK_FALSE(read_recording_header("C:/r/b.sigmf-meta", disk.view()).readable());

    // Nesting past the bound is a refusal and not a stack overflow.
    std::string deep = R"({"global": )";
    for (int i = 0; i < 200; ++i) {
        deep += "[";
    }
    disk.files["C:/r/b.sigmf-meta"] = Bytes(deep.begin(), deep.end());
    CHECK_FALSE(read_recording_header("C:/r/b.sigmf-meta", disk.view()).readable());
}
