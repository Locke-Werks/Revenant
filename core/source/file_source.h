// A recorded IQ file, presented as a source.
//
// Demand flow control, so the sink is allowed to block and blocking is the
// backpressure: the file advances at exactly the rate its consumer retires
// work. Unthrottled that is several times realtime on any modern disk, and
// there is no code path that makes it so. Faster than realtime is what
// happens when nothing is holding a stopwatch.
//
// A pace holds one: pace= in the URI, or StreamOptions::pace when the URI
// states none, and set_pace while it plays. The next block after a change is
// timed from where the stream is, so a change never makes it catch up.
//
// This is the backend the milestone exit criterion runs against, and the
// reason is in the design: a synthesised scene dense enough for a fifty
// receiver test cannot be generated in realtime on this machine, so the scene
// is rendered once to a file and replayed from here.
//
// A raw file is interleaved IQ with no header. The rate, the format and the
// centre frequency are not in the bytes, so they come from the URI, and a
// wrong one is a capture whose metadata is wrong with nothing to catch it.
// That is why the registry rejects an unknown query key rather than ignoring
// it.
//
// A recording that arrived from somewhere else usually does carry that
// metadata, in one of two containers, and reading it beats retyping it:
//
//   SigMF, a JSON sidecar beside a raw data file. tests/corpus/README.md
//   already names it as the corpus format of record, so it is the one this
//   backend supports in full rather than in the parts that were convenient.
//
//   RIFF WAV with an auxi chunk, which is what SDR#, HDSDR and SDRuno write,
//   plus the RF64 and BW64 extensions that carry a recording past the 4 GB a
//   32-bit RIFF size field can count. At 2 MS/s cs16 that ceiling arrives
//   about eight and a half minutes in, so a long HF capture is either RF64 or
//   a pile of segments.
//
// THE RULE BOTH READERS FOLLOW. Where the container and the URI both state
// something and the two disagree, that is an error naming both values, not a
// precedence rule. Where the container states something the URI did not, the
// container wins and the capability description records where the number came
// from. Where neither states it, the open fails naming the key to add.
//
// Metadata that disagrees with the file's own length is the common real
// failure, because a capture interrupted by a full disk leaves a sidecar
// describing the recording somebody meant to make. Both readers check the
// declared extent against the bytes on disk and refuse with both numbers.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"
#include "core/source/capabilities.h"
#include "core/source/source.h"

namespace revenant::source {

// Which container the bytes are in.
enum class Container : std::uint8_t {
    // Sniff: a SigMF sidecar beside the path, or a RIFF/RF64/BW64 signature
    // in the first twelve bytes, or raw. The signature is read from the file
    // rather than guessed from the extension, because a .dat written by a
    // WAV recorder is still a WAV and a .wav holding raw IQ is not.
    Auto,

    // Headerless interleaved IQ. What this backend read before containers,
    // and still the fastest thing to write from a synthesiser.
    Raw,

    // A raw dataset plus a .sigmf-meta JSON sidecar.
    Sigmf,

    // RIFF, RF64 or BW64.
    Wav,
};

[[nodiscard]] Expected<Container> container_from_name(std::string_view name);
[[nodiscard]] const char* container_name(Container container);

// One entry of a SigMF captures array: where the receiver was pointed from
// this sample onward.
//
// A recording that retunes mid-file is an ordinary thing for SigMF to
// express and the thing a naive reader gets silently wrong, because the
// obvious implementation reads global metadata, takes captures[0], and plays
// the whole file at that centre frequency. Everything after the retune then
// lands at a frequency nobody transmitted on, and the spectrum still looks
// like a spectrum.
struct CaptureSegment {
    dsp::SampleIndex sample_start = 0;

    // Samples from sample_start to the next segment, or to the end.
    dsp::SampleIndex sample_count = 0;

    bool has_center = false;
    dsp::Hertz center_hz = 0;

    // From core:datetime, nanoseconds since the Unix epoch.
    bool has_anchor = false;
    std::int64_t anchor_ns = 0;
};

// What a container said. Every field is optional because the containers
// differ in what they carry, and a field nobody set has to be visibly unset
// rather than defaulted to a plausible number.
struct RecordingMetadata {
    Container container = Container::Raw;

    // The file the samples are in. For SigMF this is the dataset beside the
    // sidecar, which core:dataset may name explicitly.
    std::string data_path;

    // Where the samples start in that file and how many bytes of them there
    // are. A WAV puts them after its chunk headers; a SigMF dataset starts at
    // zero and may declare trailing bytes that are not samples.
    std::uint64_t data_offset = 0;
    std::uint64_t data_bytes = 0;

    bool has_rate = false;
    dsp::SampleRate rate = 0;

    bool has_format = false;
    SampleFormat format = SampleFormat::Cf32;

    bool has_center = false;
    dsp::Hertz center_hz = 0;

    bool has_anchor = false;
    std::int64_t anchor_ns = 0;
    std::int64_t anchor_accuracy_ns = 0;

    // SigMF only, and always at least one entry when it is SigMF.
    std::vector<CaptureSegment> segments;

    // Conditions an operator would want to know about that are not failures:
    // a WAV with no auxi chunk, an auxi rate that agrees with the fmt chunk,
    // a sidecar whose datetime was rounded. Empty is the quiet case, and the
    // capability description carries whatever is here so it reaches a log
    // rather than being dropped on the floor.
    std::vector<std::string> notes;
};

// Reads a SigMF sidecar and the dataset it points at.
//
// `meta_path` is the .sigmf-meta file. Exposed rather than kept private
// because it is the half with all the parsing in it, and a test that has to
// spin up a whole Source to find out whether a datatype string was read
// correctly is a test nobody writes the awkward cases for.
[[nodiscard]] Expected<RecordingMetadata> read_sigmf_metadata(const std::string& meta_path);

// Reads the chunk headers of a RIFF, RF64 or BW64 file and stops at the data
// chunk without reading a sample.
[[nodiscard]] Expected<RecordingMetadata> read_wav_metadata(const std::string& path);

// The .sigmf-meta path a dataset path implies. `x.sigmf-data` gives
// `x.sigmf-meta`; anything else gets `.sigmf-meta` appended, which is what
// the spec's own non-conforming-dataset case produces.
[[nodiscard]] std::string sigmf_meta_path_for(std::string_view data_path);

// Everything the backend needs, already parsed. The URI grammar and the
// spelling of every key live in registry.cpp, which owns the text layer; this
// struct is what survives it.
struct FileSourceConfig {
    // Echoed into SourceCapabilities::uri so a session can be reproduced from
    // what was written down.
    std::string uri;

    // Decoded filesystem path. Percent escapes are already gone.
    std::string path;

    // Shown in a device list. Empty takes the file's own name.
    std::string display_name;

    // Which container to read, and where its metadata is.
    Container container = Container::Auto;

    // An explicit SigMF sidecar. Empty derives it from the data path, which
    // is the ordinary case; a caller names one when the sidecar was renamed
    // or lives beside a dataset whose name the spec calls non-conforming.
    std::string meta_path;

    // Which SigMF capture segment to play, when the recording retunes.
    //
    // Unset on a recording whose captures all sit at one frequency plays the
    // whole file, which is every ordinary recording. Unset on one that
    // retunes is refused, because playing across a retune at one declared
    // centre frequency is the silent failure this field exists to prevent,
    // and the refusal lists each segment with its index, its first sample and
    // its frequency so the choice can be made from the message.
    bool segment_given = false;
    std::uint32_t segment = 0;

    // The three the URI can state, each with whether it actually did.
    //
    // The flags are what makes a disagreement with the container detectable.
    // Without them a rate of zero is indistinguishable from a rate nobody
    // typed, and the reader would have to treat "the URI said 2000000 and the
    // sidecar says 2400000" as a defaulting question rather than as the
    // contradiction it is.
    bool rate_given = false;
    dsp::SampleRate rate = 0;

    bool format_given = false;
    SampleFormat format = SampleFormat::Cf32;

    bool center_given = false;
    dsp::Hertz center_hz = 0;

    // When false the anchor is derived from the file's modification time and
    // carries the much larger accuracy that deserves. See the note in
    // file_source.cpp: a derived anchor with a confident accuracy is worse
    // than no anchor at all.
    bool anchor_given = false;
    std::int64_t anchor_ns = 0;

    // Overrides the derived accuracy. A caller who recorded the file against
    // a disciplined reference knows better than anything derivable here.
    bool anchor_accuracy_given = false;
    std::int64_t anchor_accuracy_ns = 0;

    // Fractional frequency uncertainty of the clock that took these samples,
    // parts per million, one sigma. Zero declares the file's rate to be the
    // definition of its timebase, which is the right answer for a synthesised
    // capture and a lie for one taken off a dongle. A caller that knows the
    // recorder's tolerance states it and the anchor accuracy then grows with
    // elapsed samples rather than sitting still.
    double ppm_uncertainty = 0.0;

    // How fast to play, as a multiple of realtime, zero for unthrottled.
    //
    // Given, it is the source's own and StreamOptions::pace is ignored; not
    // given, the caller's StreamOptions::pace applies, which is what every
    // URI written before pace= existed still gets. Source::own_pace has why a
    // file carries this rather than leaving it to the host.
    bool pace_given = false;
    double pace = 0.0;
};

// Maps "cu8", "cs8", "cs16", "cs24", "cf32" onto the enum. Lives here rather than in
// the frozen capabilities.h because the file backend is the only thing that
// takes a sample format from text.
[[nodiscard]] Expected<SampleFormat> sample_format_from_name(std::string_view name);

// Infers the format from a path's extension, for the common case of a file
// written by siggen. Fails naming the extensions it knows when the path has
// one it does not, so a .iq or a .bin has to state its format explicitly
// rather than being guessed at.
[[nodiscard]] Expected<SampleFormat> sample_format_from_extension(std::string_view path);

// Describes the file without opening a stream on it: the size is a stat and
// the length in samples falls out of it. Used by describe_sources().
[[nodiscard]] Expected<SourceCapabilities> describe_file_source(const FileSourceConfig& config);

[[nodiscard]] Expected<std::unique_ptr<Source>> open_file_source(const FileSourceConfig& config);

}  // namespace revenant::source
