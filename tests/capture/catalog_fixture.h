// Segments on disk for the catalog and replay tests, written by hand.
//
// Written here rather than through core/capture/writer.h on purpose. The
// catalog's tests are the check that the catalog reads what format.h says,
// and a fixture that went through the writer would pass whenever the writer
// and the catalog agreed with each other, including when both were wrong in
// the same way. So this follows format.h directly and nothing else.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "core/capture/format.h"
#include "core/dsp/types.h"
#include "tests/support/temp_path.h"

namespace revenant::test {

// 2026-10-12T00:00:00Z, a round anchor so expected instants read plainly.
inline constexpr std::int64_t kCaptureAnchorNs = 1'791'763'200'000'000'000;

// Above the signed 64-bit ceiling, so a reader that went through int64 or a
// double would get it wrong.
inline constexpr std::uint64_t kCaptureEpoch = 0xfedcba9876543210ull;

// A capture root in temp that removes itself.
class CaptureRoot {
public:
    CaptureRoot() : path_(unique_temp_path("revenant-capture-catalog"))
    {
        std::filesystem::create_directories(path_);
    }
    ~CaptureRoot()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    CaptureRoot(const CaptureRoot&) = delete;
    CaptureRoot& operator=(const CaptureRoot&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] std::string utf8() const
    {
        const std::u8string text = path_.generic_u8string();
        return std::string(reinterpret_cast<const char*>(text.data()), text.size());
    }

private:
    std::filesystem::path path_;
};

// The sample bytes a cu8 segment holds at stream index `index`: a pattern
// that never repeats inside any test here, so a block from the wrong place
// cannot compare equal by accident.
[[nodiscard]] inline std::byte capture_byte(dsp::SampleIndex index, std::size_t component)
{
    return static_cast<std::byte>((index * 7 + component * 3 + index / 251) & 0xFF);
}

[[nodiscard]] inline std::vector<std::byte> capture_bytes(dsp::SampleIndex first,
                                                          std::uint64_t count)
{
    std::vector<std::byte> out;
    out.reserve(count * 2);
    for (std::uint64_t i = 0; i < count; ++i) {
        out.push_back(capture_byte(first + i, 0));
        out.push_back(capture_byte(first + i, 1));
    }
    return out;
}

struct SegmentSpec {
    std::uint64_t epoch = kCaptureEpoch;
    dsp::SampleIndex first_index = 0;
    std::uint64_t sample_count = 0;
    std::int64_t anchor_ns = kCaptureAnchorNs;
    dsp::SampleRate rate = 1000;
    dsp::Hertz center = 100'000'000;
    std::uint64_t dropped_before = 0;
    std::string close_reason = "size";

    // Raw JSON for the annotations array, without the brackets.
    std::string annotations;

    // Leave the data file under its .open name and write no meta.
    bool open = false;
    // Write the data file under its closed name and no meta.
    bool orphan = false;
};

[[nodiscard]] inline std::int64_t start_ns_of(const SegmentSpec& spec)
{
    return dsp::BlockTimestamp{spec.first_index, spec.anchor_ns, spec.rate}.wall_clock_ns();
}

struct WrittenSegment {
    std::filesystem::path data;
    std::filesystem::path meta;
};

// format.h's layout and stem, cu8 data.
inline WrittenSegment write_segment(const std::filesystem::path& root, const SegmentSpec& spec)
{
    using namespace std::chrono;
    const std::int64_t start = start_ns_of(spec);
    const sys_time<milliseconds> when{milliseconds{start / 1'000'000}};
    const auto day = floor<days>(when);
    const year_month_day ymd{day};
    const hh_mm_ss<milliseconds> tod{when - day};

    const std::string day_name = std::format("{:04}-{:02}-{:02}", static_cast<int>(ymd.year()),
                                             static_cast<unsigned>(ymd.month()),
                                             static_cast<unsigned>(ymd.day()));
    const std::string stamp = std::format(
        "{:04}{:02}{:02}T{:02}{:02}{:02}.{:03}Z", static_cast<int>(ymd.year()),
        static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()), tod.hours().count(),
        tod.minutes().count(), tod.seconds().count(), tod.subseconds().count());
    const std::string iso = std::format(
        "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", static_cast<int>(ymd.year()),
        static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()), tod.hours().count(),
        tod.minutes().count(), tod.seconds().count(), tod.subseconds().count());
    const std::string stem =
        std::format("rev-{}-{}-{:016x}-{}", stamp, spec.center, spec.epoch, spec.first_index);

    const std::filesystem::path dir = root / day_name;
    std::filesystem::create_directories(dir);

    WrittenSegment out;
    const std::string data_name =
        stem + (spec.open ? std::string(capture::kOpenSuffix) : std::string()) +
        std::string(capture::kDataExtension);
    out.data = dir / data_name;
    out.meta = dir / (stem + std::string(capture::kMetaExtension));

    {
        const std::vector<std::byte> bytes = capture_bytes(spec.first_index, spec.sample_count);
        std::ofstream data(out.data, std::ios::binary);
        data.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (spec.open || spec.orphan) {
        return out;
    }

    std::string meta;
    meta += "{\n  \"global\": {\n";
    meta += "    \"core:datatype\": \"cu8\",\n";
    meta += std::format("    \"core:sample_rate\": {},\n", spec.rate);
    meta += "    \"core:version\": \"1.0.0\",\n";
    meta += "    \"core:recorder\": \"Revenant test\",\n";
    meta += "    \"core:extensions\": [{\"name\": \"revenant\", \"version\": \"1.0.0\", "
            "\"optional\": true}],\n";
    meta += std::format("    \"revenant:stream_epoch\": {},\n", spec.epoch);
    meta += std::format("    \"revenant:first_index\": {},\n", spec.first_index);
    meta += std::format("    \"revenant:epoch_anchor_ns\": {},\n", spec.anchor_ns);
    meta += std::format("    \"revenant:dropped_before\": {},\n", spec.dropped_before);
    meta += std::format("    \"revenant:close_reason\": \"{}\"\n", spec.close_reason);
    meta += "  },\n";
    meta += std::format("  \"captures\": [{{\"core:sample_start\": 0, \"core:frequency\": {}, "
                        "\"core:datetime\": \"{}\"}}],\n",
                        spec.center, iso);
    meta += "  \"annotations\": [" + spec.annotations + "]\n}\n";

    std::ofstream file(out.meta, std::ios::binary);
    file << meta;
    return out;
}

}  // namespace revenant::test
