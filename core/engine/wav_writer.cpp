// The WAV writer.
//
// The three decisions this file implements are argued in wav_writer.h: RF64
// for the 4 GiB ceiling, in-place size refreshes for the truncation problem,
// and float32 by default with 16-bit PCM one option away.
//
// The chunk order is deliberate and is the other thing worth knowing before
// the code:
//
//   RIFF/WAVE
//   JUNK   28 bytes, reserved so a ds64 chunk can replace it in place. It has
//          to be the first chunk after WAVE, because EBU Tech 3306 requires
//          ds64 to sit there, and a chunk cannot be inserted into a file that
//          already holds hours of audio.
//   fmt
//   fact   float only. WAVE_FORMAT_IEEE_FLOAT is a non-PCM format and the
//          Microsoft specification requires a fact chunk carrying the frame
//          count for those.
//   LIST   INFO tags, for a person and for anything that reads tags.
//   revn   the same facts in a fixed binary layout, for a script.
//   data
//
// Everything but the audio is therefore in front of the audio, so a recording
// that was cut off still says what it is.

#include "core/engine/wav_writer.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <share.h>
#endif

namespace revenant::engine {
namespace {

// The format is little-endian by definition. Everything below writes native
// integers straight out, so a big-endian port has to add byte swaps rather
// than discovering the problem as a file no tool will open.
static_assert(std::endian::native == std::endian::little,
              "the WAV writer emits native integers and assumes a little-endian host");

// WAVE_FORMAT_PCM and WAVE_FORMAT_IEEE_FLOAT from mmreg.h, written here
// rather than included because this file has no business pulling in the
// Windows multimedia headers to learn two constants.
constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kFormatFloat = 3;

// Payload of a ds64 chunk with no sample-count table: three 64-bit sizes and
// the table length. EBU Tech 3306.
constexpr std::uint32_t kDs64PayloadBytes = 28;

// What a 32-bit size field holds once the real size lives in ds64.
constexpr std::uint32_t kSizeLivesInDs64 = 0xFFFF'FFFFU;

// A tag that says nothing and is skipped by every reader, which is what the
// placeholder has to be until the file outgrows a 32-bit header.
constexpr std::string_view kJunkTag = "JUNK";
constexpr std::string_view kDs64Tag = "ds64";

// Metadata text is written by whatever names the recording, so it is bounded
// here rather than trusted. A header is read into memory by everything that
// opens the file and nobody benefits from a megabyte of it.
constexpr std::size_t kMaxInfoTextBytes = 512;

struct FileCloser {
    void operator()(std::FILE* file) const noexcept
    {
        if (file != nullptr) {
            static_cast<void>(std::fclose(file));
        }
    }
};

using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

// A path arriving as UTF-8 has to be told so. filesystem::path's narrow
// constructor reads a std::string in the active code page instead, so a
// recording under a name with an accent in it would be written somewhere
// nobody asked for. Same reasoning and same shape as path_of() in
// core/source/file_source.cpp.
[[nodiscard]] std::filesystem::path path_of(const std::string& utf8)
{
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

// _wfsopen and not _wfopen_s, and the difference matters for a recorder.
//
// The _s variants open with exclusive access, so nothing else on the machine
// can read a recording while it is being made: no playing back the last
// minute of a scanner that is still running, no copying it, and no reading it
// with the engine's own file source. _SH_DENYWR keeps a second writer out,
// which is the thing that would actually corrupt the file, and lets readers
// in, which is the whole reason the header sizes are refreshed as the file
// grows.
[[nodiscard]] Expected<FilePtr> create_binary(const std::string& path)
{
    std::FILE* raw = nullptr;
#if defined(_MSC_VER)
    raw = _wfsopen(path_of(path).c_str(), L"wb", _SH_DENYWR);
    if (raw == nullptr) {
        return fail(std::format("could not create '{}' for writing", path), errno);
    }
#else
    raw = std::fopen(path.c_str(), "wb");
    if (raw == nullptr) {
        return fail(std::format("could not create '{}' for writing", path), errno);
    }
#endif
    return FilePtr(raw);
}

// std::fseek takes a long, which is 32 bits on Windows and would cap the
// writer at 2 GiB, which is half the problem RF64 exists to solve.
[[nodiscard]] bool seek_absolute(std::FILE* file, std::int64_t offset)
{
#if defined(_MSC_VER)
    return _fseeki64(file, offset, SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

[[nodiscard]] bool seek_to_end(std::FILE* file)
{
#if defined(_MSC_VER)
    return _fseeki64(file, 0, SEEK_END) == 0;
#else
    return fseeko(file, 0, SEEK_END) == 0;
#endif
}

// Full scale is 32767 and not 32768.
//
// Both conventions are in use. This one maps +1.0 and -1.0 to equal and
// opposite integers, so a symmetric waveform stays symmetric and nothing
// clips at exactly full scale; the alternative gains half a least significant
// bit of range and makes +1.0 the one value that has to be clamped. Half an
// LSB is 90 dB below anything a receiver produces.
constexpr double kPcm16FullScale = 32767.0;

[[nodiscard]] std::int16_t quantize_pcm16(float sample)
{
    const double scaled = static_cast<double>(sample) * kPcm16FullScale;

    // Written as a negated comparison so that NaN takes this branch: NaN
    // compares false against everything, and std::lround of a NaN is
    // unspecified. A NaN in the audio path is a fault upstream, and it must
    // not become an arbitrary integer in a recording.
    if (!(scaled >= -32768.0)) {
        return std::isnan(scaled) ? static_cast<std::int16_t>(0)
                                  : static_cast<std::int16_t>(-32768);
    }
    if (scaled > 32767.0) {
        return static_cast<std::int16_t>(32767);
    }

    // std::lround rounds half away from zero regardless of the current
    // rounding mode, so the result does not depend on what a caller left in
    // the FPU control word.
    return static_cast<std::int16_t>(std::lround(scaled));
}

// Builds a header in memory so it can be written in one call and its offsets
// recorded as it goes.
class HeaderBytes {
public:
    void tag(std::string_view text)
    {
        // Every RIFF identifier is exactly four characters. A shorter one
        // would silently shift every later offset.
        for (std::size_t i = 0; i < 4; ++i) {
            bytes_.push_back(i < text.size() ? static_cast<std::uint8_t>(text[i])
                                             : static_cast<std::uint8_t>(' '));
        }
    }

    void u8(std::uint8_t value) { bytes_.push_back(value); }

    void u16(std::uint16_t value)
    {
        bytes_.push_back(static_cast<std::uint8_t>(value & 0xFFU));
        bytes_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
    }

    void u32(std::uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8) {
            bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
        }
    }

    void u64(std::uint64_t value)
    {
        for (int shift = 0; shift < 64; shift += 8) {
            bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
        }
    }

    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

    void text(std::string_view value)
    {
        for (const char character : value) {
            bytes_.push_back(static_cast<std::uint8_t>(character));
        }
    }

    // Fixed-width, NUL padded, truncated if it does not fit. Used by the
    // 'revn' chunk, whose whole point is that every field sits at a known
    // offset.
    void text_fixed(std::string_view value, std::size_t width)
    {
        const std::size_t copied = std::min(value.size(), width);
        text(value.substr(0, copied));
        zeros(width - copied);
    }

    void zeros(std::size_t count)
    {
        bytes_.insert(bytes_.end(), count, std::uint8_t{0});
    }

    void append(const HeaderBytes& other)
    {
        bytes_.insert(bytes_.end(), other.bytes_.begin(), other.bytes_.end());
    }

    void patch_u32(std::size_t offset, std::uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8) {
            bytes_[offset + static_cast<std::size_t>(shift / 8)] =
                static_cast<std::uint8_t>((value >> shift) & 0xFFU);
        }
    }

    [[nodiscard]] std::size_t size() const { return bytes_.size(); }
    [[nodiscard]] const std::uint8_t* data() const { return bytes_.data(); }

private:
    std::vector<std::uint8_t> bytes_;
};

// One LIST/INFO field. The size includes the terminating NUL, and the chunk
// is padded to an even length like every other RIFF chunk. Both are places
// where a writer that guesses produces a file that reads fine in one tool and
// not in the next.
void info_field(HeaderBytes& out, std::string_view id, std::string_view value)
{
    if (value.empty()) {
        return;
    }
    const std::string_view text = value.substr(0, std::min(value.size(), kMaxInfoTextBytes));
    const std::size_t payload = text.size() + 1;
    out.tag(id);
    out.u32(static_cast<std::uint32_t>(payload));
    out.text(text);
    out.u8(0);
    if (payload % 2 != 0) {
        out.u8(0);
    }
}

[[nodiscard]] std::string default_name(const WavStreamSpec& spec)
{
    return std::format("Revenant receiver {} at {} Hz", spec.metadata.vrx, spec.metadata.center);
}

[[nodiscard]] std::string default_comment(const WavStreamSpec& spec)
{
    const std::string_view demod =
        spec.metadata.demod.empty() ? std::string_view("unknown") : spec.metadata.demod;
    return std::format(
        "Revenant receiver {}, centre {} Hz, {} demodulation, {} Hz audio, {} channel(s), "
        "first audio sample {}, epoch anchor {} ns",
        spec.metadata.vrx, spec.metadata.center, demod, spec.rate, spec.channels,
        spec.metadata.start, spec.metadata.epoch_anchor_ns);
}

class WavWriterImpl final : public WavWriter {
public:
    WavWriterImpl(FilePtr file, const WavStreamSpec& spec, const WavWriteOptions& options)
        : path_(spec.path),
          options_(options),
          rate_(spec.rate),
          channels_(spec.channels),
          file_(std::move(file))
    {
    }

    ~WavWriterImpl() override { static_cast<void>(close_impl()); }

    Status write(std::span<const float> interleaved) override
    {
        if (closed_) {
            return fail(std::format("write to '{}' after it was closed", path_));
        }
        if (interleaved.empty()) {
            return {};
        }
        if (interleaved.size() % channels_ != 0) {
            return fail(std::format(
                "a write of {} samples is not a whole number of {}-channel frames for '{}'",
                interleaved.size(), channels_, path_));
        }

        const auto frames = static_cast<std::uint64_t>(interleaved.size() / channels_);
        const std::uint64_t incoming =
            static_cast<std::uint64_t>(interleaved.size()) * bytes_per_sample_;

        if (auto room = ensure_room(incoming); !room) {
            return room;
        }

        if (options_.format == WavSampleFormat::Float32) {
            // Straight through. A float32 recording is the samples the
            // demodulator produced, byte for byte, which is the whole reason
            // it is the default.
            if (auto wrote = write_all(interleaved.data(), static_cast<std::size_t>(incoming));
                !wrote) {
                return wrote;
            }
        } else {
            std::size_t offset = 0;
            while (offset < interleaved.size()) {
                const std::size_t count = std::min(staging_.size(), interleaved.size() - offset);
                for (std::size_t i = 0; i < count; ++i) {
                    staging_[i] = quantize_pcm16(interleaved[offset + i]);
                }
                if (auto wrote = write_all(staging_.data(), count * sizeof(std::int16_t)); !wrote) {
                    return wrote;
                }
                offset += count;
            }
        }

        data_bytes_ += incoming;
        frames_ += frames;
        bytes_since_refresh_ += incoming;

        if (options_.size_refresh_bytes != 0 &&
            bytes_since_refresh_ >= options_.size_refresh_bytes) {
            if (auto refreshed = refresh_sizes(); !refreshed) {
                return refreshed;
            }
        }
        return {};
    }

    Status flush() override
    {
        if (closed_) {
            return {};
        }
        if (auto refreshed = refresh_sizes(); !refreshed) {
            return refreshed;
        }
        if (std::fflush(file_.get()) != 0) {
            return fail(std::format("could not flush '{}'", path_), errno);
        }
        return {};
    }

    Status close() override { return close_impl(); }

    [[nodiscard]] std::uint64_t frames_written() const override { return frames_; }
    [[nodiscard]] std::uint64_t data_bytes() const override { return data_bytes_; }
    [[nodiscard]] bool rf64() const override { return rf64_; }
    [[nodiscard]] bool is_open() const override { return !closed_; }
    [[nodiscard]] std::string_view path() const override { return path_; }
    [[nodiscard]] WavSampleFormat format() const override { return options_.format; }

    // Writes the header and takes down the offsets that later have to be
    // patched. Called once, by open_wav_writer, before anything else.
    Status begin(const WavStreamSpec& spec)
    {
        bytes_per_sample_ = options_.format == WavSampleFormat::Float32 ? 4U : 2U;

        if (options_.staging_frames == 0) {
            options_.staging_frames = 4096;
        }
        if (options_.format == WavSampleFormat::Pcm16) {
            // Allocated here, on the control thread, so that write() on the
            // egress thread allocates nothing.
            staging_.assign(options_.staging_frames * channels_, std::int16_t{0});
        }
        if (options_.io_buffer_bytes >= 64) {
            io_buffer_.assign(options_.io_buffer_bytes, '\0');
            if (std::setvbuf(file_.get(), io_buffer_.data(), _IOFBF, io_buffer_.size()) != 0) {
                // Not fatal. The CRT's own buffer is smaller and the file is
                // still correct, so this costs syscalls rather than data.
                io_buffer_.clear();
                io_buffer_.shrink_to_fit();
            }
        }

        const auto block_align = static_cast<std::uint16_t>(channels_ * bytes_per_sample_);
        const auto avg_bytes = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(rate_) * block_align);
        const bool is_float = options_.format == WavSampleFormat::Float32;

        HeaderBytes header;
        header.tag("RIFF");
        header.u32(0);  // patched by refresh_sizes
        header.tag("WAVE");

        if (options_.allow_rf64) {
            junk_offset_ = static_cast<std::int64_t>(header.size());
            header.tag(kJunkTag);
            header.u32(kDs64PayloadBytes);
            header.zeros(kDs64PayloadBytes);
        }

        // 18 bytes with an explicit cbSize for the float case: the Microsoft
        // specification requires the extended form for every non-PCM format,
        // and some readers check the fmt chunk's length before trusting the
        // tag.
        header.tag("fmt ");
        header.u32(is_float ? 18U : 16U);
        header.u16(is_float ? kFormatFloat : kFormatPcm);
        header.u16(static_cast<std::uint16_t>(channels_));
        header.u32(static_cast<std::uint32_t>(rate_));
        header.u32(avg_bytes);
        header.u16(block_align);
        header.u16(static_cast<std::uint16_t>(bytes_per_sample_ * 8U));
        if (is_float) {
            header.u16(0);  // cbSize, no extension follows
            header.tag("fact");
            header.u32(4);
            fact_value_offset_ = static_cast<std::int64_t>(header.size());
            header.u32(0);  // frame count, patched by refresh_sizes
        }

        HeaderBytes info;
        info_field(info, "INAM",
                   spec.metadata.name.empty() ? default_name(spec) : spec.metadata.name);
        info_field(info, "ICMT",
                   spec.metadata.comment.empty() ? default_comment(spec) : spec.metadata.comment);
        info_field(info, "ISFT",
                   spec.metadata.software.empty() ? std::string("Revenant")
                                                  : spec.metadata.software);
        info_field(info, "ICRD", spec.metadata.created);
        info_field(info, "ISRC", spec.metadata.source);
        info_field(info, "IENG", spec.metadata.engineer);

        header.tag("LIST");
        header.u32(static_cast<std::uint32_t>(4 + info.size()));
        header.tag("INFO");
        header.append(info);

        header.tag("revn");
        header.u32(kWavRevnChunkBytes);
        header.u32(kWavRevnVersion);
        header.u32(spec.metadata.vrx);
        header.i64(spec.metadata.center);
        header.i64(rate_);
        header.u64(spec.metadata.start);
        header.i64(spec.metadata.epoch_anchor_ns);
        header.u32(channels_);
        header.u32(0);  // reserved
        header.text_fixed(spec.metadata.demod, kWavRevnDemodBytes);

        header.tag("data");
        data_size_offset_ = static_cast<std::int64_t>(header.size());
        header.u32(0);  // patched by refresh_sizes
        data_start_offset_ = static_cast<std::int64_t>(header.size());

        if (auto wrote = write_all(header.data(), header.size()); !wrote) {
            return wrote;
        }

        // The sizes are correct for an empty recording from this moment, so a
        // file that dies before a single sample is written still parses.
        return refresh_sizes();
    }

private:
    Status write_all(const void* data, std::size_t bytes)
    {
        if (bytes == 0) {
            return {};
        }
        const std::size_t wrote = std::fwrite(data, 1, bytes, file_.get());
        if (wrote != bytes) {
            return fail(
                std::format("wrote {} of {} bytes to '{}'", wrote, bytes, path_), errno);
        }
        return {};
    }

    Status patch_u32_at(std::int64_t offset, std::uint32_t value)
    {
        std::uint8_t encoded[4];
        for (int shift = 0; shift < 32; shift += 8) {
            encoded[shift / 8] = static_cast<std::uint8_t>((value >> shift) & 0xFFU);
        }
        return patch_bytes(offset, encoded, sizeof(encoded));
    }

    Status patch_u64_at(std::int64_t offset, std::uint64_t value)
    {
        std::uint8_t encoded[8];
        for (int shift = 0; shift < 64; shift += 8) {
            encoded[shift / 8] = static_cast<std::uint8_t>((value >> shift) & 0xFFU);
        }
        return patch_bytes(offset, encoded, sizeof(encoded));
    }

    Status patch_bytes(std::int64_t offset, const void* data, std::size_t bytes)
    {
        if (!seek_absolute(file_.get(), offset)) {
            return fail(std::format("could not seek '{}' to offset {}", path_, offset), errno);
        }
        if (auto wrote = write_all(data, bytes); !wrote) {
            return wrote;
        }
        // Back to the end, because every other write appends. Doing this here
        // rather than at each call site means no path can leave the stream
        // positioned in the header.
        if (!seek_to_end(file_.get())) {
            return fail(std::format("could not seek '{}' back to the end", path_), errno);
        }
        return {};
    }

    // Total bytes the file occupies once the pad byte is counted. The pad is
    // unreachable at 2 and 4 bytes per sample and is computed anyway, because
    // the next format someone adds might not be.
    [[nodiscard]] std::uint64_t file_bytes() const
    {
        return static_cast<std::uint64_t>(data_start_offset_) + data_bytes_ + (data_bytes_ & 1U);
    }

    Status ensure_room(std::uint64_t incoming)
    {
        if (rf64_) {
            return {};
        }
        const std::uint64_t projected = file_bytes() + incoming;
        if (projected <= options_.rf64_threshold_bytes) {
            return {};
        }
        if (!options_.allow_rf64 || junk_offset_ == 0) {
            return fail(std::format(
                "'{}' would reach {} bytes, past the {} byte limit of a 32-bit RIFF header, and "
                "the RF64 upgrade is disabled",
                path_, projected, options_.rf64_threshold_bytes));
        }
        return upgrade_to_rf64();
    }

    // Turns the file into an RF64 one without moving any audio: the JUNK
    // chunk reserved at creation becomes the ds64 chunk, and the 32-bit size
    // fields are set to the sentinel that says the real size lives there.
    Status upgrade_to_rf64()
    {
        if (auto patched = patch_bytes(0, "RF64", 4); !patched) {
            return patched;
        }
        if (auto patched = patch_u32_at(4, kSizeLivesInDs64); !patched) {
            return patched;
        }
        if (auto patched = patch_bytes(junk_offset_, kDs64Tag.data(), 4); !patched) {
            return patched;
        }
        if (auto patched = patch_u32_at(data_size_offset_, kSizeLivesInDs64); !patched) {
            return patched;
        }
        if (fact_value_offset_ != 0) {
            if (auto patched = patch_u32_at(fact_value_offset_, kSizeLivesInDs64); !patched) {
                return patched;
            }
        }
        rf64_ = true;
        return refresh_sizes();
    }

    // Writes the current sizes into the header. Called on an interval, on
    // every flush and at close, which is what makes a killed process leave a
    // readable file behind.
    Status refresh_sizes()
    {
        const std::uint64_t total = file_bytes();
        const std::uint64_t riff_size = total - 8;

        if (rf64_) {
            // ds64 payload: riffSize, dataSize, sampleCount, tableLength. The
            // table length stays zero; it exists for files with more than one
            // oversized chunk and this format has exactly one.
            const std::int64_t body = junk_offset_ + 8;
            if (auto patched = patch_u64_at(body, riff_size); !patched) {
                return patched;
            }
            if (auto patched = patch_u64_at(body + 8, data_bytes_); !patched) {
                return patched;
            }
            if (auto patched = patch_u64_at(body + 16, frames_); !patched) {
                return patched;
            }
            return patch_u32_at(body + 24, 0);
        }

        if (auto patched = patch_u32_at(4, static_cast<std::uint32_t>(riff_size)); !patched) {
            return patched;
        }
        if (auto patched =
                patch_u32_at(data_size_offset_, static_cast<std::uint32_t>(data_bytes_));
            !patched) {
            return patched;
        }
        if (fact_value_offset_ != 0) {
            return patch_u32_at(fact_value_offset_, static_cast<std::uint32_t>(frames_));
        }
        return {};
    }

    // Non-virtual, so the destructor is not calling through the vtable of a
    // class that is part way through being destroyed.
    Status close_impl()
    {
        if (closed_) {
            return {};
        }
        closed_ = true;
        if (file_ == nullptr) {
            return {};
        }

        Status result;
        if ((data_bytes_ & 1U) != 0) {
            const std::uint8_t pad = 0;
            result = write_all(&pad, 1);
        }
        if (result) {
            result = refresh_sizes();
        }
        if (result && std::fflush(file_.get()) != 0) {
            result = fail(std::format("could not flush '{}'", path_), errno);
        }

        // Closed whatever happened. A writer that leaves a descriptor open
        // because the last patch failed turns one bad recording into a
        // process that runs out of handles.
        file_.reset();
        return result;
    }

    std::string path_;
    WavWriteOptions options_;
    dsp::SampleRate rate_ = 0;
    std::uint32_t channels_ = 1;
    std::uint32_t bytes_per_sample_ = 4;

    // Declared before file_ so that it outlives the FILE even on a path that
    // does not go through close_impl(): setvbuf hands the CRT a pointer into
    // this and the CRT uses it until fclose.
    std::vector<char> io_buffer_;
    std::vector<std::int16_t> staging_;

    FilePtr file_;

    std::uint64_t data_bytes_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t bytes_since_refresh_ = 0;

    std::int64_t junk_offset_ = 0;
    std::int64_t fact_value_offset_ = 0;
    std::int64_t data_size_offset_ = 0;
    std::int64_t data_start_offset_ = 0;

    bool rf64_ = false;
    bool closed_ = false;
};

}  // namespace

Expected<WavSampleFormat> wav_format_from_name(std::string_view name)
{
    if (name == "float32" || name == "f32" || name == "float") {
        return WavSampleFormat::Float32;
    }
    if (name == "pcm16" || name == "s16" || name == "int16") {
        return WavSampleFormat::Pcm16;
    }
    return fail(std::format("'{}' is not a WAV sample format; expected float32 or pcm16", name));
}

Expected<std::unique_ptr<WavWriter>> open_wav_writer(const WavStreamSpec& spec,
                                                     const WavWriteOptions& options)
{
    if (spec.path.empty()) {
        return fail("open_wav_writer was given an empty path");
    }
    if (spec.rate <= 0) {
        return fail(std::format("'{}' was given a sample rate of {}, which is not a rate",
                                spec.path, spec.rate));
    }
    if (spec.rate > static_cast<dsp::SampleRate>(0xFFFF'FFFFLL)) {
        // nSamplesPerSec is 32 bits. Nothing produces audio at 4 GHz, so this
        // is an arithmetic mistake upstream rather than a limit anyone meets.
        return fail(std::format("'{}' was given a sample rate of {}, past what a WAV header can "
                                "state",
                                spec.path, spec.rate));
    }
    if (spec.channels == 0 || spec.channels > kWavMaxChannels) {
        return fail(std::format(
            "'{}' asked for {} channels; this writer emits 1 or 2, because more needs "
            "WAVE_FORMAT_EXTENSIBLE and a channel mask and nothing in the engine produces more "
            "than a stereo pair",
            spec.path, spec.channels));
    }

    WavWriteOptions effective = options;
    if (effective.rf64_threshold_bytes == 0 ||
        effective.rf64_threshold_bytes > kWavClassicFileCeiling) {
        effective.rf64_threshold_bytes = kWavClassicFileCeiling;
    }

    auto file = create_binary(spec.path);
    if (!file) {
        return std::unexpected(file.error());
    }

    auto writer = std::make_unique<WavWriterImpl>(std::move(*file), spec, effective);
    if (auto started = writer->begin(spec); !started) {
        return std::unexpected(with_context(started.error(),
                                            std::format("writing the header of '{}'", spec.path)));
    }
    return writer;
}

}  // namespace revenant::engine
