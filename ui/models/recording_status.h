// What the strip under the top bar says while a recording is playing.
//
// Qt-free, so ui/tests can link it; ui/models/recording_link.cpp feeds it
// from EngineLink and the QML draws the strings.
//
// WHAT THE WIRE OFFERS, AND WHAT THIS THEREFORE DOES NOT
//
// Position is SourceStats::samplesDelivered against the open source's
// SourceDescriptor::lengthSamples, both already on the wire. Pace is
// EngineInfo::sourcePacedBy, the pace in force, beside realtimeFactor, what
// it is achieving, and Session.setSourcePace changes it: the strip's 1x, 2x,
// 4x and max. Two things are not on the wire and are not drawn as though
// they were:
//
//   Seek. SourceDescriptor::seekable is true for a file, and its own note in
//   core/rpc/revenant.capnp says "NOTHING SEEKS YET: there is no seek on this
//   wire". The position is a readout and not a handle.
//
//   Loop. The file backend's URI grammar has no loop key and the source ends
//   when the bytes do. The strip says the recording plays once.
//
// WHAT THIS NOTE USED TO SAY, before 2026-09-23, under a third heading, "A
// pace control": "sourcePacedBy is read-only, set on the engine's command
// line, and a file opened from this window plays at whatever that was". A
// file opened from this window plays at realtime now, because the section
// sends pace=1 and openSource adds it when a URI does not.

#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <optional>
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

    // "realtime", "4x realtime", "max, 11.8x", "4x asked, running at 2.1x".
    // Empty once ended.
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

    // THE PACE IN FORCE, and what it reaches only where that says something
    // the setting does not. At max there is no setting to read, so the
    // measurement is the pace. Paced, a reading within a tenth of the setting
    // is the setting and is not drawn; one further below it is a source that
    // cannot keep up, which the setting alone would hide. Nothing measured yet
    // reads as the setting.
    if (sample.paced_by <= 0.0) {
        line.pace = sample.realtime_factor > 0.0
                        ? std::format("max, {:.1f}x", sample.realtime_factor)
                        : std::string("max");
        return line;
    }
    const std::string asked = sample.paced_by == 1.0
                                  ? std::string("realtime")
                                  : std::format("{:g}x realtime", sample.paced_by);
    if (sample.realtime_factor > 0.0 && sample.realtime_factor < 0.9 * sample.paced_by) {
        line.pace = std::format("{} asked, running at {:.1f}x", asked, sample.realtime_factor);
    } else {
        line.pace = asked;
    }
    return line;
}

// ---------------------------------------------------------------------------
// The pace control
// ---------------------------------------------------------------------------

// What the strip offers, in the order it draws them. max is zero on the wire:
// as fast as the engine retires the samples.
inline constexpr std::array<std::string_view, 4> kPaceOptions{"1x", "2x", "4x", "max"};

// The option a pace is, or empty for one the strip does not offer, a pace=3
// typed into a URI, so no segment claims to be in force when none is.
[[nodiscard]] inline std::string pace_option_for(double paced_by)
{
    if (paced_by <= 0.0) {
        return "max";
    }
    for (const double offered : {1.0, 2.0, 4.0}) {
        if (paced_by == offered) {
            return std::format("{:g}x", offered);
        }
    }
    return {};
}

// The pace an option asks for, or nothing for text that is not one.
[[nodiscard]] inline std::optional<double> pace_for_option(std::string_view option)
{
    if (option == "max") {
        return 0.0;
    }
    if (option == "1x") {
        return 1.0;
    }
    if (option == "2x") {
        return 2.0;
    }
    if (option == "4x") {
        return 4.0;
    }
    return std::nullopt;
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
