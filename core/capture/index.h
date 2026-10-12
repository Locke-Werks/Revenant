// The offline index pass: a closed segment's samples, through the detector,
// into format.h annotations.
//
// THE SAME GRAPH AS LIVE. The segment is opened as a file source in an engine
// of its own, the spectrum stage runs on the device exactly as it does behind
// a radio, and the detector consumes the engine's frames. Nothing here has a
// second implementation of the detector or of the spectrum, so an annotation
// written offline means what one written live means, and a change to the
// detector reaches both.
//
// UNTHROTTLED. A file is a Demand source, and pace zero asks it for samples
// as fast as the device retires them. The detector reads no clock, every
// interval inside it is a difference of sample indices, so its tracks do not
// depend on how fast the file was read.
//
// TIER TWO IS OFF BY DEFAULT, and with it every family. The probe pool places
// its probes on the wall clock and an unthrottled file is gone before the
// first one lands, which tests/detect/test_labelled_scene.cpp says at more
// length. IndexOptions::probes turns it on together with a pace that lets
// them land, at the cost of indexing at that pace.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/capture/format.h"
#include "core/detect/detector.h"
#include "core/error.h"

namespace revenant::capture {

struct IndexOptions {
    // -1 takes REVENANT_GPU_INDEX, then the engine's own choice.
    int gpu_index = -1;

    // The coarse grid and the spectrum. Zero transform takes the spectrum
    // stage's default, which is what revenant-cli --detect runs.
    std::uint32_t channels = 64;
    bool channels_yield_to_source = true;
    std::uint32_t spectrum_transform = 0;

    // The detector's own thresholds and timings. source_rate, source_center
    // and grid_channels are overwritten from the segment and the engine.
    detect::DetectorConfig detector{};

    // Probe receivers for tier two, zero for none. Non-zero needs a pace.
    std::uint32_t probes = 0;

    // Multiple of realtime the file is read at, zero for unthrottled.
    double pace = 0.0;
};

struct IndexResult {
    std::vector<Annotation> annotations;

    // Source seconds the engine actually delivered, and the wall time it took
    // from opening the segment to the last frame, so a caller can report the
    // speed as a multiple of realtime.
    double capture_seconds = 0.0;
    double wall_seconds = 0.0;

    detect::DetectorStats stats{};
};

// Runs the detector over one segment.
//
// One annotation per spell a track was detected: a keyed emitter that comes
// and goes is one track in the detector, since it keeps its id across a hold,
// and several annotations here, one per transmission, because a search for
// "what was on at three o'clock" wants the transmissions. A spell is kept
// only when the track's confidence reached the detector's confidence
// threshold during it, which is the bar the live track list draws at.
//
// sample_start is the first decision the track was Live at and sample_count
// runs to the last, both relative to the segment. Decisions are a tenth of a
// second apart by default and the spectrum integrates over about a second,
// so an annotation starts up to one decision late and ends late by however
// long the integrated average takes to fall under the threshold after the
// signal stops. Neither is corrected for: the lag depends on level, and a
// guess applied to every annotation would be wrong for most of them.
//
// Frequency edges are the track's centre plus and minus half its bandwidth at
// its last detection in the spell. peak_db is the highest candidate peak, in
// dBFS, over the spell; snr_db the highest snr_2500_db; confidence the
// highest. family is detect::label_track's name at the spell's end, empty
// without tier two.
[[nodiscard]] Expected<IndexResult> index_segment(const std::string& data_path,
                                                  const SegmentInfo& info,
                                                  const IndexOptions& options = {});

}  // namespace revenant::capture
