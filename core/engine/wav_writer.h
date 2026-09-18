// A WAV writer for recorded receiver audio.
//
// Smaller than it looks, with three traps in it that are worth stating before
// the interface, because each one is a decision rather than an implementation
// detail and a reader has to be able to see which way it went.
//
// SIZE. The classic RIFF header states every size in 32 bits, so a recording
// cannot pass 4 GiB in it. At 48 kHz stereo float32 that is six and a half
// hours, which a scanner left running reaches on a weekend. The three answers
// are refusing, rolling to a second file, and RF64. This writer does RF64
// (EBU Tech 3306), automatically and without moving a byte of audio: a JUNK
// chunk of exactly the size of a ds64 chunk is reserved as the first chunk of
// every file, and the moment the next write would carry the file past the
// 32-bit ceiling the RIFF tag becomes RF64, the JUNK becomes ds64 carrying the
// real 64-bit sizes, and the 32-bit size fields are set to 0xFFFFFFFF as the
// specification requires. A file that never reaches the ceiling is an ordinary
// RIFF/WAVE file that has 36 bytes of padding in it and nothing else unusual,
// so nothing has to understand RF64 to read the common case. Rolling was
// rejected because a recording split across files is a recording someone has
// to reassemble, and the seam lands wherever the clock happened to be.
//
// TRUNCATION. The two sizes in the header are not known until the file is
// closed, so the usual writer leaves an unreadable file behind when the
// process dies. This one refreshes both sizes in place every
// WavWriteOptions::size_refresh_bytes of audio and on every flush(), so a file
// whose writer was killed is readable up to the last refresh and the audio
// past that point is what is lost rather than the whole recording. The header
// is written with sizes covering zero bytes at creation, which means even a
// file that dies before the first refresh parses as a valid empty recording
// instead of as garbage. The alternative, pre-sizing the file and truncating
// on close, needs the length up front, and a live receiver does not have one.
//
// QUANTISATION. Audio arrives here as float and IEEE float WAV is a real
// format (WAVE_FORMAT_IEEE_FLOAT, tag 3), so the default is Float32: the
// writer's job is to not be the stage that loses information, and an SDR
// recording is frequently the input to something else rather than the end of
// the line. Plenty of tools read only 16-bit PCM, Python's standard library
// wave module among them, so Pcm16 is one option away and the command line
// exposes it. Nothing here dithers: a dither needs a seeded generator to stay
// reproducible under the purity rule in docs/conventions.md, and demodulated
// audio already sits on a noise floor tens of dB above the sixteenth bit.
//
// The writer does not read a clock. Per docs/conventions.md the DSP path is a
// pure function of its arguments, and a recording of a capture replayed
// faster than realtime must carry the capture's timestamps rather than the
// replay's, so the creation date and the epoch anchor are supplied by the
// caller and never derived here.

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::engine {

// How the samples are stored in the data chunk.
enum class WavSampleFormat : std::uint8_t {
    // WAVE_FORMAT_IEEE_FLOAT, 32 bits. The samples are written through
    // unchanged, so a recording round-trips bit for bit.
    Float32,

    // WAVE_FORMAT_PCM, 16 bits, clamped and rounded. Lossy, and the reason to
    // pick it is that something downstream cannot read anything else.
    Pcm16,
};

[[nodiscard]] constexpr const char* wav_format_name(WavSampleFormat format)
{
    switch (format) {
        case WavSampleFormat::Float32: return "float32";
        case WavSampleFormat::Pcm16: return "pcm16";
    }
    return "unknown";
}

[[nodiscard]] Expected<WavSampleFormat> wav_format_from_name(std::string_view name);

// The largest file the classic 32-bit RIFF header can describe. Past this the
// writer upgrades the file to RF64 rather than writing a size field that has
// wrapped, which is the failure mode that produces a file every tool reads as
// a few seconds long.
inline constexpr std::uint64_t kWavClassicFileCeiling = 0xFFFF'FFFFULL;

// Channel counts above stereo need WAVE_FORMAT_EXTENSIBLE and a channel mask,
// which is not implemented because nothing in the engine produces more than a
// stereo pair. The writer refuses rather than emitting a header that names a
// layout it has not described.
inline constexpr std::uint32_t kWavMaxChannels = 2;

// What a recording says about itself.
//
// The point of these fields is that a file found on disk in six months can be
// traced back to what produced it without the filename being the only
// evidence. They are written twice on purpose: as RIFF LIST/INFO text, which
// is where a person and every tag-reading tool look, and as a fixed-layout
// 'revn' chunk, which is what a script can parse without pulling a sentence
// apart. Readers that do not know either chunk skip both, which is ordinary
// RIFF behaviour and not a compatibility risk.
struct WavMetadata {
    // LIST/INFO INAM. Empty takes a generated name naming the receiver.
    std::string name;

    // LIST/INFO ICMT. Empty takes a generated one-line summary of the fields
    // below, which is the form a person reading the file's tags wants.
    std::string comment;

    // LIST/INFO ISFT. Empty writes "Revenant".
    std::string software;

    // LIST/INFO ICRD, an ISO 8601 date or timestamp. Supplied by the caller
    // rather than read from a clock here: see the header comment.
    std::string created;

    // LIST/INFO ISRC, which in RIFF INFO means the source of the material and
    // not a recording code. The source URI belongs here.
    std::string source;

    // LIST/INFO IENG.
    std::string engineer;

    // The receiver this came off. All of these also go in the 'revn' chunk,
    // whose layout is documented at kWavRevnChunkBytes below.
    std::uint32_t vrx = 0;
    dsp::Hertz center = 0;
    std::string demod;

    // Index of the first audio sample in the file, counted in audio samples
    // from the start of the stream, which is what AudioChunk::start carries.
    // Time is a sample index everywhere in this engine and this is the field
    // that keeps a recording attached to that timeline.
    dsp::SampleIndex start = 0;

    // Unix epoch nanosecond at which audio sample 0 of the stream was
    // captured, not at which this file was opened. Zero when the source could
    // not supply one, which is honest and is not the same as the epoch.
    std::int64_t epoch_anchor_ns = 0;
};

// Size of the 'revn' chunk payload. The layout, all little-endian:
//
//   offset  size  field
//   0       4     layout version, currently 1
//   4       4     receiver id
//   8       8     centre frequency in hertz, signed
//   16      8     audio sample rate, signed
//   24      8     first audio sample index, unsigned
//   32      8     epoch anchor nanoseconds of audio sample 0, signed
//   40      4     channel count
//   44      4     reserved, written as zero
//   48      16    demodulator name, NUL padded
//
// Fixed layout and a version field rather than a key-value soup: a reader
// that gets the version it knows can trust every offset, and a reader that
// does not recognise the version skips the chunk like any other.
inline constexpr std::uint32_t kWavRevnChunkBytes = 64;
inline constexpr std::uint32_t kWavRevnVersion = 1;
inline constexpr std::uint32_t kWavRevnDemodBytes = 16;

struct WavWriteOptions {
    WavSampleFormat format = WavSampleFormat::Float32;

    // Frames held in the quantisation staging buffer. Allocated once when the
    // file is opened, so that write() on the egress thread never allocates.
    // Ignored for Float32, which writes the caller's span straight through.
    std::size_t staging_frames = 4096;

    // Audio bytes between in-place refreshes of the header's size fields. See
    // the TRUNCATION note above. Zero refreshes only on flush() and close(),
    // which is what a caller writing a short file from a fixed buffer wants
    // and is wrong for a receiver left running.
    std::uint64_t size_refresh_bytes = 1ULL << 20;

    // File size at which the RF64 upgrade fires. The default is the real
    // 32-bit ceiling; a test lowers it so that the upgrade path runs against a
    // file small enough to check by hand rather than only against a four
    // gigabyte one nobody generates.
    std::uint64_t rf64_threshold_bytes = kWavClassicFileCeiling;

    // When false no JUNK placeholder is reserved and reaching the ceiling
    // fails the write instead of upgrading. For a caller that would rather
    // have a short valid RIFF file than a long RF64 one.
    bool allow_rf64 = true;

    // Bytes of CRT buffering. One large buffer rather than the 4 KiB default
    // keeps the write syscall count down on a recording that runs for hours.
    std::size_t io_buffer_bytes = 1ULL << 20;
};

struct WavStreamSpec {
    // UTF-8. Opened through the wide CRT entry point on Windows so a path
    // with a character outside the active code page opens rather than failing
    // with a message about a file that plainly exists.
    std::string path;

    dsp::SampleRate rate = 0;
    std::uint32_t channels = 1;
    WavMetadata metadata{};
};

// One open file.
//
// THREAD SAFETY. Not internally synchronised, and deliberately so: it is
// owned by one thread at a time and a lock in it would be a lock in the
// egress path. open_wav_writer() and close() are called from the control
// thread; write() and flush() are called from whichever single thread drives
// the sink, which for the file backend in core/engine/audio_egress.h is the
// egress drain thread. The handoff between those two threads is the egress
// layer's slot state machine, not a mutex in here.
//
// write() does not allocate. Everything it needs, the staging buffer and the
// CRT's own buffer, is allocated when the file is opened. It does block, on
// the filesystem, which is what a file sink is for: the backpressure of a
// slow disk shows up as backlog in the receiver's ring and then as counted
// drops, never as a stall in the sample path.
class WavWriter {
public:
    virtual ~WavWriter() = default;

    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;
    WavWriter(WavWriter&&) = delete;
    WavWriter& operator=(WavWriter&&) = delete;

    // Interleaved, and a whole number of frames. A partial frame is rejected
    // rather than padded: half a frame written now puts every later sample in
    // the wrong channel, and a stereo recording that swaps sides part way
    // through is a fault nobody traces back to the writer.
    [[nodiscard]] virtual Status write(std::span<const float> interleaved) = 0;

    // Pushes the CRT buffer to the filesystem and refreshes the header sizes.
    // After this returns the file on disk is complete and readable.
    [[nodiscard]] virtual Status flush() = 0;

    // Finalises the header and closes the file. Idempotent: a second call
    // succeeds and does nothing, so a close on the error path and a close in
    // the destructor cannot conflict.
    [[nodiscard]] virtual Status close() = 0;

    [[nodiscard]] virtual std::uint64_t frames_written() const = 0;
    [[nodiscard]] virtual std::uint64_t data_bytes() const = 0;

    // True once the file has been upgraded, which is a property of this
    // recording and not of the options: a file under the threshold is never
    // RF64.
    [[nodiscard]] virtual bool rf64() const = 0;

    [[nodiscard]] virtual bool is_open() const = 0;
    [[nodiscard]] virtual std::string_view path() const = 0;
    [[nodiscard]] virtual WavSampleFormat format() const = 0;

protected:
    WavWriter() = default;
};

// Creates the file and writes the header.
//
// An existing file is truncated. The caller owns the naming, and a writer
// that refuses an existing path is one that has to be told the name twice;
// uniqueness belongs in whatever composes the path.
[[nodiscard]] Expected<std::unique_ptr<WavWriter>> open_wav_writer(
    const WavStreamSpec& spec, const WavWriteOptions& options = {});

}  // namespace revenant::engine
