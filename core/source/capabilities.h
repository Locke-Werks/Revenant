// What a source can do, described before it is opened.
//
// Users judge an SDR application in the first five minutes on whether their
// radio works properly, and the way that goes wrong is a common abstraction
// that flattens every device into the same shape. Gain stages, clock sources,
// bias-T and direct-sampling modes genuinely differ per device, so this
// describes them rather than reducing them to string keys and hoping.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/dsp/types.h"

namespace revenant::source {

// What the device puts on the bus. Conversion to the engine's canonical
// Complex32 happens on the GPU during upload, so native width crosses the bus
// exactly once and the host never makes a pass over every sample.
enum class SampleFormat : std::uint8_t {
    Cu8,   // unsigned 8-bit I/Q, offset binary. The RTL-SDR's native format.
    Cs8,   // signed 8-bit I/Q
    Cs16,  // signed 16-bit I/Q
    Cf32,  // float32 I/Q, the engine's canonical form
};

[[nodiscard]] constexpr std::size_t bytes_per_sample(SampleFormat format) {
    switch (format) {
        case SampleFormat::Cu8:
        case SampleFormat::Cs8:
            return 2;
        case SampleFormat::Cs16:
            return 4;
        case SampleFormat::Cf32:
            return 8;
    }
    return 0;
}

[[nodiscard]] constexpr const char* format_name(SampleFormat format) {
    switch (format) {
        case SampleFormat::Cu8: return "cu8";
        case SampleFormat::Cs8: return "cs8";
        case SampleFormat::Cs16: return "cs16";
        case SampleFormat::Cf32: return "cf32";
    }
    return "unknown";
}

// Who sets the pace, which is the single most consequential property a source
// has and the one everything else falls out of.
//
// Paced: the device's own clock. The sink must return promptly and must never
// block, because there is nowhere to put samples that keep arriving. A sink
// that cannot keep up causes an overrun, which is a counted correctness event
// and not a log line.
//
// Demand: the consumer's clock. The sink may block, and blocking is the
// backpressure: the source advances at exactly the rate its consumer retires
// work. This is the whole mechanism behind faster than realtime. It is not a
// mode and there is no code path for it anywhere: it is what happens when
// nothing is holding a stopwatch.
enum class FlowControl : std::uint8_t { Paced, Demand };

enum class ClockSource : std::uint8_t { Internal, Tcxo, ExternalReference, Gps, Pps };

struct TuneRange {
    dsp::Hertz low = 0;
    dsp::Hertz high = 0;

    // 0 means continuous within whatever the synthesiser can resolve.
    dsp::Hertz step = 0;

    [[nodiscard]] constexpr bool contains(dsp::Hertz frequency) const {
        return frequency >= low && frequency <= high;
    }
};

struct GainStage {
    // The device's own name for it, lowercase: "lna", "mixer", "vga", "if".
    std::string name;

    double min_db = 0.0;
    double max_db = 0.0;

    // Empty means continuous. Populated means the stage only takes these
    // values, and a request lands on the nearest one.
    std::vector<double> steps_db;

    bool has_auto = false;
};

// How well a source can place sample zero on the wall clock, and how well it
// is holding that placement now.
//
// An anchor with no stated accuracy is a lie, so the accuracy travels with the
// anchor everywhere it goes. FT8 and the other slotted modes need UTC within
// about a second; a disciplined reference does far better and the difference
// should be visible rather than assumed.
struct ClockQuality {
    ClockSource source = ClockSource::Internal;

    // One-sigma uncertainty in placing sample zero on the wall clock.
    std::int64_t accuracy_ns = 0;

    // Fractional frequency error, parts per million. Positive means the
    // device's sample clock runs fast.
    double ppm_error = 0.0;

    // True when a reference is present and the anchor is being corrected
    // against it rather than free-running from its value at stream start.
    bool disciplined = false;

    // Residual against the reference at the last correction, which is the
    // number worth watching: a discipline loop that is locked reports a small
    // and stable residual, and one that has lost its reference does not.
    std::int64_t residual_ns = 0;
};

struct SourceCapabilities {
    // The URI this source was opened from, so a session can be reproduced.
    std::string uri;

    // "file", "synthetic", "rtlsdr".
    std::string backend;

    std::string display_name;

    // Empty when this description is complete. Otherwise why it is not: the
    // device is attached and enumerable but could not be opened to be asked,
    // most often because something else is already streaming from it.
    //
    // A field rather than an error return, because describing a list of
    // sources is not an all-or-nothing operation. A dongle held by another
    // process must not be able to hide the synthetic and file backends, which
    // cannot fail and are exactly what somebody reaches for when the radio is
    // busy. The entry stays in the list, says what is wrong with it, and
    // carries whatever enumeration alone could establish.
    std::string unavailable;

    [[nodiscard]] bool available() const { return unavailable.empty(); }

    std::vector<TuneRange> tune_ranges;

    // Discrete rates the device supports. Empty means continuous between
    // min_rate and max_rate.
    std::vector<dsp::SampleRate> sample_rates;
    dsp::SampleRate min_rate = 0;
    dsp::SampleRate max_rate = 0;

    SampleFormat native_format = SampleFormat::Cf32;
    std::uint8_t bits_per_component = 32;

    std::vector<GainStage> gain_stages;
    std::vector<ClockSource> clock_sources;

    FlowControl flow = FlowControl::Demand;

    bool seekable = false;

    // 0 means unbounded, which is every live device.
    dsp::SampleIndex length_samples = 0;

    // The block size this source would rather deliver. A caller may ask for
    // another and the source will honour it where it can, but a USB backend
    // has a transfer size and fighting it costs throughput.
    std::size_t preferred_block_samples = 0;

    std::int64_t timestamp_accuracy_ns = 0;

    [[nodiscard]] bool supports_rate(dsp::SampleRate rate) const;
    [[nodiscard]] bool can_tune(dsp::Hertz frequency) const;
};

}  // namespace revenant::source
