// A capture root replayed as a source.
//
// capture://<root>?from=...&to=...&pace=... plays the rolling capture back
// through the identical graph a live radio feeds, which is the point of
// writing segments as SigMF in the first place: the time machine is the file
// source's replay with the catalog choosing which files, rather than a second
// engine path that drifts from the first.
//
// Demand flow control, as FileSource has, for the same reason: blocking in
// the sink is the backpressure, and unthrottled replay runs at whatever rate
// the engine retires work. pace= or StreamOptions::pace holds a stopwatch to
// it, exactly as for a file.
//
// WHAT ONE REPLAY COVERS. A run: consecutive segments of one stream_epoch at
// one centre frequency, one sample rate and one format, starting with the
// segment that holds `from` and ending at `to` or at the newest segment of the
// run when the source was opened. The stream's own sample indices are kept,
// so every block's stamp is the index it had when it was captured and its
// wall clock is exact rather than re-derived.
//
// A GAP INSIDE A RUN is two segments whose indices do not meet: samples the
// writer lost. The first block after it carries dropped_before equal to the
// samples missing and a stamp that has jumped by the same amount, which is
// what a live radio's overrun looks like to the engine, so a replay of a lossy
// capture is a faithful replay of the loss rather than a splice that pretends
// the stream was whole.
//
// THE REPLAY STOPS AT A RETUNE, A RATE CHANGE OR A NEW EPOCH. The format
// closes a segment on each, and the engine cannot follow any of them from a
// source: EngineInfo::source_center is read from Source::center() once at
// open and moved only by Engine::set_source_center, SourceBlock carries no
// centre at all, and every receiver's frequency is an offset from that
// constant. A source that changed center() mid-stream would have the engine
// keep demodulating at the old frequency with every label on the waterfall
// wrong and nothing failing. A new epoch is worse: its indices start again,
// and a stamp that goes backwards is something no consumer here expects. So
// a run ends at the first such boundary and the capability notes say where
// and why; the next run is another open with from= at its start. Following a
// retune properly needs a centre on SourceBlock and the graph re-placing its
// receivers on it, which is engine work this source should not fake.
//
// Seeking is by sample index within the epoch, the same indices the stamps
// carry. A seek into a gap lands on the first sample after it, and is a seek,
// so it reports no loss.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/dsp/types.h"
#include "core/error.h"
#include "core/source/capabilities.h"
#include "core/source/source.h"

namespace revenant::capture {

struct CaptureSourceConfig {
    // Echoed into SourceCapabilities::uri.
    std::string uri;

    // UTF-8 path of the capture root, percent escapes already decoded.
    std::string root;

    // Shown in a device list. Empty names the root.
    std::string display_name;

    // Wall clock to start from, nanoseconds since the Unix epoch. Unset
    // starts at the oldest segment on disk. An instant inside a gap, or before
    // the first segment, starts at the next segment that exists.
    bool from_given = false;
    std::int64_t from_ns = 0;

    // Wall clock to stop before. Unset plays to the end of the run as it
    // stood at open; segments closed after that are a later open's.
    bool to_given = false;
    std::int64_t to_ns = 0;

    // As FileSourceConfig::pace: given, it is the source's own and
    // StreamOptions::pace is ignored.
    bool pace_given = false;
    double pace = 0.0;
};

// Describes the run an open would play, from the catalog alone.
[[nodiscard]] Expected<source::SourceCapabilities> describe_capture_source(
    const CaptureSourceConfig& config);

[[nodiscard]] Expected<std::unique_ptr<source::Source>> open_capture_source(
    const CaptureSourceConfig& config);

}  // namespace revenant::capture
