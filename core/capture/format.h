// The rolling capture's on-disk contract.
//
// Everything that writes, lists, replays or searches a capture agrees on what
// is in this file and on nothing else, so it is the one place the layout is
// stated. writer.h, catalog.h, capture_source.h and search.h all follow it.
//
// A SEGMENT IS A SIGMF RECORDING. One .sigmf-data file of raw samples in the
// radio's own format and one .sigmf-meta JSON sidecar beside it, both named
// after the segment's stem. The file source already reads SigMF in full, so a
// segment plays through the identical graph a live radio does, extraction is a
// copy of two files, and anything else that reads SigMF opens one without
// Revenant.
//
// A SEGMENT IS HOMOGENEOUS AND GAPLESS. One centre frequency, one sample rate,
// one format, and no lost samples inside it. A retune, a rate change, a format
// change or a block arriving with dropped_before > 0 closes the open segment
// and starts a new one. That is what keeps the ring trivial (delete the oldest
// whole segment), keeps seeking a lookup rather than a scan, and means a gap
// in a capture is always visible as two segments whose indices do not meet
// rather than as samples that look continuous and are not.
//
// THE META IS WRITTEN WHEN THE SEGMENT CLOSES. Until then the segment is the
// data file alone, with the stem ending in kOpenSuffix, and the catalog skips
// it. A segment whose writer died is therefore a data file with no meta, which
// the catalog reports as orphaned rather than guessing its metadata, and a
// recovery pass can finalise it from the extent plus <stem>.open.revenant, a
// few key=value lines the writer puts beside the data before its first sample.
// The stem alone cannot carry it: rate, format, the exact anchor, the gain and
// core:hw are not in it. The side file is deleted on close, so a closed
// segment is the two SigMF files and nothing else.
//
// Cs24 is written as ci24_le, which the file source does not read yet, so a
// 24-bit capture stores but does not replay until it does.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/dsp/types.h"
#include "core/source/capabilities.h"

namespace revenant::capture {

// Data and meta extensions, the SigMF specification's own.
inline constexpr std::string_view kDataExtension = ".sigmf-data";
inline constexpr std::string_view kMetaExtension = ".sigmf-meta";

// A data file still being written is <stem>.open.sigmf-data. Renamed to
// <stem>.sigmf-data in the same step that writes the meta.
inline constexpr std::string_view kOpenSuffix = ".open";

// Layout under the capture root:
//
//   <root>/<YYYY-MM-DD>/<stem>.sigmf-data
//   <root>/<YYYY-MM-DD>/<stem>.sigmf-meta
//
// The day directory is the UTC date of the segment's first sample, so a
// directory listing is a coarse time index and a whole day is removable with
// one rmdir.
//
// stem = "rev-" <UTC yyyymmddThhmmss.fffZ of the first sample>
//        "-" <centre frequency in Hz, decimal>
//        "-" <stream_epoch, 16 lowercase hex digits>
//        "-" <first sample index, decimal>
//
// The stem sorts by time within a day, and stream_epoch plus first index make
// it unique even when two segments start in the same millisecond.

// The SigMF extension namespace. Declared in the meta's core:extensions with
// this name and version, optional = true, so a reader that does not know it
// still reads the recording.
inline constexpr std::string_view kExtensionName = "revenant";
inline constexpr std::string_view kExtensionVersion = "1.0.0";

// Global keys, beyond the core ones every segment carries
// (core:datatype, core:sample_rate, core:version, core:recorder = "Revenant
// <version>", core:hw = the source's description).
//
//   revenant:stream_epoch     u64. Identifies one unbroken run of the source.
//                             Sample indices are comparable only within an
//                             epoch; a reopen, a radio change or an engine
//                             restart starts a new one. Random at stream start.
//   revenant:first_index      u64. stream index of sample 0 of this segment.
//   revenant:epoch_anchor_ns  i64. BlockTimestamp::epoch_anchor_ns of the
//                             stream, so wall clock for any sample is exact
//                             rather than re-derived from core:datetime.
//   revenant:dropped_before   u64. Samples lost between the previous segment
//                             of this epoch and this one. Zero when the
//                             segment started because of a retune.
//   revenant:close_reason     string. "retune", "rate", "format", "loss",
//                             "size", "stop", "recovered".
//   revenant:gain_db          number, optional. The gain in force, when the
//                             source reports one.
//   revenant:indexed          array, optional. One object per offline pass
//                             that has run over the segment, keyed by
//                             generator: {"generator": string, "at": ISO 8601
//                             UTC, "settings": string}. Written whether or
//                             not the pass found anything, since a quiet
//                             segment leaves no annotation to say it was
//                             looked at. A rerun replaces its own entry.
//
// One core:captures entry, sample_start 0, with core:frequency and
// core:datetime (ISO 8601 UTC, the wall clock of the first sample).
//
// The top-level annotations array (SigMF has no core:annotations key) carries what the detector saw while the segment was being
// written, and what an offline index pass added later. See Annotation.

// Keys on an annotations entry. Core keys used:
// core:sample_start, core:sample_count (relative to the segment),
// core:freq_lower_edge, core:freq_upper_edge (absolute Hz), core:label
// (the family name when one was given, otherwise "signal"),
// core:generator ("revenant-detect" live, "revenant-index" offline).
//
//   revenant:confidence   number, 0..1, the detector's confidence.
//   revenant:peak_db      number, peak level in the detector's dB scale.
//   revenant:snr_db       number, against the local noise floor.
//   revenant:family       string, optional, the identified family.
//   revenant:track_id     u64, optional, the live detector's track id.

// What the writer needs to know about the stream it is writing. Also what the
// catalog reconstructs from a meta file.
struct SegmentInfo {
    std::uint64_t stream_epoch = 0;
    dsp::SampleIndex first_index = 0;
    std::uint64_t sample_count = 0;
    std::int64_t epoch_anchor_ns = 0;
    dsp::SampleRate rate = 0;
    dsp::Hertz center = 0;
    source::SampleFormat format = source::SampleFormat::Cu8;
    std::uint64_t dropped_before = 0;

    // Wall clock of the first sample, derived from the anchor exactly as
    // BlockTimestamp::wall_clock_ns does.
    [[nodiscard]] constexpr std::int64_t start_ns() const {
        return dsp::BlockTimestamp{first_index, epoch_anchor_ns, rate}.wall_clock_ns();
    }
    [[nodiscard]] constexpr dsp::SampleIndex end_index() const {
        return first_index + sample_count;
    }
};

struct Annotation {
    // Relative to the segment's first sample.
    std::uint64_t sample_start = 0;
    std::uint64_t sample_count = 0;
    dsp::Hertz freq_lower = 0;
    dsp::Hertz freq_upper = 0;
    double confidence = 0.0;
    double peak_db = 0.0;
    double snr_db = 0.0;
    // Empty when the signal was not identified.
    std::string family;
};

}  // namespace revenant::capture
