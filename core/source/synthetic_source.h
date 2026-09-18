// A synthesised wideband scene, presented as a source.
//
// Wraps siggen::Scene, which the tree already has and which the channelizer
// and the capture writer are developed against because no radio on the desk
// can produce twenty megahertz of densely populated spectrum. Scene::render is
// a pure function of the absolute sample index, so this source seeks for free
// and produces the same bytes as the siggen command line at the same seed.
// That last property is what makes it usable as a fixture: a test can state
// its input as a URI instead of shipping a capture.
//
// Demand flow control, like the file source and for the same reason. There is
// no clock in the loop, so the scene advances exactly as fast as its consumer
// retires work.
//
// THE THREADING NOTE, which is the one thing to get right here. The Scene is
// constructed with worker_threads = 1 and that is not a tuning choice. Left to
// itself Scene::render spawns a thread per hardware thread for every block,
// and the engine already owns a pool; two pools sized to the same machine
// fight for the same cores and both lose. Partitioning a block across the
// engine's pool is the follow-up, and Scene supports it directly because
// render takes an absolute range and disjoint ranges need no coordination.
//
// This is also why core/source depends on tools/siggen. See the note in
// synthetic_source.cpp: the dependency runs the wrong way for the current
// CMake targets and the integrator has to resolve it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"
#include "core/source/capabilities.h"
#include "core/source/source.h"

namespace revenant::source {

// The scene, already parsed out of the URI. Field for field this is siggen's
// wideband subcommand, so a scene described here and one described on the
// command line are the same scene.
//
// The palette is carried as text rather than as siggen::Modulation so that
// this header pulls in nothing from tools/. Only the implementation needs
// siggen, and keeping it that way means registry.cpp does not.
struct SyntheticSourceConfig {
    std::string uri;
    std::string display_name;

    dsp::SampleRate rate = 20'000'000;
    dsp::Hertz center_hz = 0;

    // 0 is an unbounded scene, which is legal for continuous emitters and not
    // for bursty ones. Scene::create enforces that.
    dsp::SampleIndex duration_samples = 0;

    std::uint64_t seed = 0;

    // Nanoseconds since the Unix epoch at sample zero. Supplied, never read
    // from a clock: a generator that reads a clock cannot reproduce its own
    // output, and the whole value of this backend is that it can.
    std::int64_t anchor_ns = 0;

    bool add_noise = true;
    double noise_power_full_band_dbfs = -60.0;

    std::size_t emitters = 32;
    std::size_t bursts_per_emitter = 1;

    // Placement window relative to the scene centre. When span_given is false
    // it becomes plus and minus nine twentieths of the rate, which is what
    // the siggen command line defaults to.
    bool span_given = false;
    dsp::Hertz span_low_hz = 0;
    dsp::Hertz span_high_hz = 0;

    double snr_min_db = 5.0;
    double snr_max_db = 35.0;
    double min_burst_seconds = 0.05;
    double max_burst_seconds = 2.0;

    // Mode names as written in the URI: "cw", "am", "nfm", "usb", "lsb",
    // "fsk2", "bpsk", "qpsk". Empty means every mode.
    std::vector<std::string> modes;
};

// Describes the scene without building it. Validates everything that can be
// checked from the numbers alone; a fault that only Scene::create can find,
// such as bursty emitters in an unbounded scene, surfaces at open instead.
// Building a thirty-two emitter scene to answer a device list would cost more
// than the list is worth.
[[nodiscard]] Expected<SourceCapabilities> describe_synthetic_source(
    const SyntheticSourceConfig& config);

[[nodiscard]] Expected<std::unique_ptr<Source>> open_synthetic_source(
    const SyntheticSourceConfig& config);

}  // namespace revenant::source
