// What the strip under the top bar says while a recording is playing.
//
// Qt-free, so ui/tests can link it; ui/models/recording_link.cpp feeds it
// from EngineLink and the QML draws the strings.
//
// WHAT THE WIRE OFFERS, AND WHAT THIS THEREFORE DOES NOT
//
// Position is SourceStats::samplesDelivered against the open source's
// SourceDescriptor::lengthSamples, both already on the wire. Pace is
// EngineInfo::sourcePacedBy, the --pace the engine was STARTED with, beside
// realtimeFactor, what it is achieving. Three things are not on the wire and
// are not drawn as though they were:
//
//   Seek. SourceDescriptor::seekable is true for a file, and its own note in
//   core/rpc/revenant.capnp says "NOTHING SEEKS YET: there is no seek on this
//   wire". The position is a readout and not a handle.
//
//   A pace control. sourcePacedBy is read-only, set on the engine's command
//   line, and a file opened from this window plays at whatever that was: an
//   engine started for a dongle with the default --pace 0 plays a recording
//   as fast as the GPU retires it. The strip says which rather than offering
//   a control nothing could apply.
//
//   Loop. The file backend's URI grammar has no loop key and the source ends
//   when the bytes do. The strip says the recording plays once.

#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#include "models/frequency_entry.h"
#include "models/recording_header.h"

namespace revenant::ui {

// Whole seconds as a clock: "0:07", "12:34", "1:02:06". Hours appear only
// when there are some, so a short recording does not carry a "0:" it never
// reaches.
[[nodiscard]] inline std::string format_clock(std::uint64_t seconds)
{
    const std::uint64_t hours = seconds / 3600;
    const std::uint64_t minutes = (seconds / 60) % 60;
    const std::uint64_t rest = seconds % 60;
    if (hours > 0) {
        return std::format("{}:{:02}:{:02}", hours, minutes, rest);
    }
    return std::format("{}:{:02}", minutes, rest);
}

// A length in samples at a rate as a duration a person reads, or empty when
// either is unknown. Samples with no rate are a count and not a duration.
[[nodiscard]] inline std::string format_recording_length(std::uint64_t samples, std::int64_t rate)
{
    if (samples == 0 || rate <= 0) {
        return {};
    }
    return format_clock(samples / static_cast<std::uint64_t>(rate));
}

struct PlaybackSample {
    std::uint64_t delivered = 0;
    std::uint64_t length = 0;
    std::int64_t rate = 0;

    // EngineInfo::sourcePacedBy and realtimeFactor.
    double paced_by = 0.0;
    double realtime_factor = 0.0;

    // EngineLink::engineRunning. An engine goes on answering after its source
    // ends, and says it is not running.
    bool running = false;
};

struct PlaybackLine {
    // "12:34 / 1:02:06", or "ended at 1:02:06". Empty with no rate.
    std::string position;

    // "realtime", "0.5x realtime", "unthrottled, 11.8x". Empty once ended.
    std::string pace;

    bool ended = false;

    // Played so far, zero to one, for a bar under the position.
    double fraction = 0.0;
};

[[nodiscard]] inline PlaybackLine describe_playback(const PlaybackSample& sample)
{
    PlaybackLine line;
    if (sample.rate <= 0 || sample.length == 0) {
        return line;
    }

    const std::uint64_t at = sample.delivered < sample.length ? sample.delivered : sample.length;
    line.fraction = static_cast<double>(at) / static_cast<double>(sample.length);

    // ENDED IS EITHER HALF. Every sample delivered is the ordinary end. An
    // engine that stopped running with samples delivered is also finished
    // with this stream: a demand source's last block can land short of the
    // count by the part that did not fill a block, and waiting for a number
    // that will never arrive would leave the strip claiming playback forever.
    line.ended = sample.delivered >= sample.length || (!sample.running && sample.delivered > 0);

    const auto rate = static_cast<std::uint64_t>(sample.rate);
    if (line.ended) {
        line.position = "ended at " + format_clock(at / rate);
        return line;
    }
    line.position = format_clock(at / rate) + " / " + format_clock(sample.length / rate);

    if (sample.paced_by <= 0.0) {
        line.pace = sample.realtime_factor > 0.0
                        ? std::format("unthrottled, {:.1f}x", sample.realtime_factor)
                        : std::string("unthrottled");
    } else if (sample.paced_by == 1.0) {
        line.pace = "realtime";
    } else {
        line.pace = std::format("{:g}x realtime", sample.paced_by);
    }
    return line;
}

// What the engine opened against what the preview said it would, as a sentence
// when they differ and empty when they agree.
//
// THIS IS WHERE A REMOTE ENGINE SHOWS UP. The preview read a file on this
// machine and the engine opened a path on its own; on one machine they are
// the same bytes and this is always empty. It is compared after every open
// rather than assumed, because the one case where it is not empty is exactly
// the case where the preview was describing a different file.
struct OpenedRecording {
    std::uint64_t length_samples = 0;
    std::int64_t rate = 0;
    std::string format;
    std::int64_t center_hz = 0;
};

[[nodiscard]] inline std::string compare_opened_recording(const OpenedRecording& planned,
                                                          const OpenedRecording& engine)
{
    std::string out;
    const auto add = [&out](const std::string& part) {
        out += out.empty() ? part : ", " + part;
    };
    if (planned.length_samples != engine.length_samples) {
        add(std::format("{} samples where the preview read {}", engine.length_samples,
                        planned.length_samples));
    }
    if (planned.rate != engine.rate) {
        add(std::format("{} S/s where the preview read {}", engine.rate, planned.rate));
    }
    if (!planned.format.empty() && !engine.format.empty() && planned.format != engine.format) {
        add(std::format("{} where the preview read {}", engine.format, planned.format));
    }
    if (planned.center_hz != engine.center_hz) {
        add(std::format("a centre of {} MHz where the preview planned {}",
                        format_mhz(engine.center_hz), format_mhz(planned.center_hz)));
    }
    if (out.empty()) {
        return out;
    }
    return "the engine opened something other than the file this window read: " + out +
           ". It resolves the path on its own machine.";
}

}  // namespace revenant::ui
