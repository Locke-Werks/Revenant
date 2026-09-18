// A recorded IQ file, presented as a source.
//
// Demand flow control, so the sink is allowed to block and blocking is the
// backpressure: the file advances at exactly the rate its consumer retires
// work. Unthrottled that is several times realtime on any modern disk, and
// there is no code path that makes it so. Faster than realtime is what
// happens when nothing is holding a stopwatch.
//
// This is the backend the milestone exit criterion runs against, and the
// reason is in the design: a synthesised scene dense enough for a fifty
// receiver test cannot be generated in realtime on this machine, so the scene
// is rendered once to a file and replayed from here.
//
// The file is raw interleaved IQ with no header. The rate, the format and the
// centre frequency are not in the bytes, so they come from the URI, and a
// wrong one is a capture whose metadata is wrong with nothing to catch it.
// That is why the registry rejects an unknown query key rather than ignoring
// it.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "core/dsp/types.h"
#include "core/error.h"
#include "core/source/capabilities.h"
#include "core/source/source.h"

namespace revenant::source {

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

    dsp::SampleRate rate = 0;
    SampleFormat format = SampleFormat::Cf32;
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
};

// Maps "cu8", "cs8", "cs16", "cf32" onto the enum. Lives here rather than in
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
