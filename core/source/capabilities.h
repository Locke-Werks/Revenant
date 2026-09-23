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

    // signed 24-bit I/Q, packed three bytes a component with no padding. What
    // HF recorders write as 24-bit PCM WAV. Appended rather than placed by
    // width, because the ordinal is on the wire.
    Cs24,
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
        case SampleFormat::Cs24:
            return 6;
    }
    return 0;
}

[[nodiscard]] constexpr const char* format_name(SampleFormat format) {
    switch (format) {
        case SampleFormat::Cu8: return "cu8";
        case SampleFormat::Cs8: return "cs8";
        case SampleFormat::Cs16: return "cs16";
        case SampleFormat::Cf32: return "cf32";
        case SampleFormat::Cs24: return "cs24";
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

// How fine the spectrum has to be before what it says about a signal is worth
// more than "something is there".
//
// A detector finds a signal that is one bin wide. Placing its centre and
// measuring its width, which is what naming a mode rests on, takes several
// bins across it. The shipped geometry is 2.4 MS/s over 64 coarse channels
// with a 2048-point transform per channel, which is 65536 bins at 36.6 Hz,
// and it was chosen for an RTL-SDR watching broadcast FM where the narrowest
// thing on the band is 12.5 kHz. Point the same geometry at HF and FT8 is
// 1.4 bins wide and PSK31 is under one. The detector still fires. What it
// publishes is a centre it cannot place inside the signal and a width that is
// the bin's width rather than the signal's, which is worse than a miss
// because it reads as a working detector.
//
// So a source states the scale of what it carries and something above it
// chooses a geometry that meets it. This is the stating half.
struct ResolutionRequest {
    // The narrowest signal worth telling apart from its neighbours anywhere
    // in this source's span.
    //
    // Zero means the source has not said, which is not a claim that nothing
    // narrow is here. A caller that knows better overwrites it.
    dsp::Hertz narrowest_signal_hz = 0;

    // Bins wanted across that signal. Four is what the callers here ask for:
    // one bin cannot distinguish a centre from an edge, two cannot show a
    // shape, and four puts the centre within a quarter of the signal's own
    // width.
    std::uint32_t bins_across_narrowest = 0;

    // Why, in a phrase, so a caller that cannot meet the request can quote it
    // back instead of reporting two bare numbers.
    std::string basis;

    [[nodiscard]] bool stated() const {
        return narrowest_signal_hz > 0 && bins_across_narrowest > 0;
    }

    // Whether bins of numerator/denominator hertz are fine enough.
    //
    // A rational rather than a rounded integer because the engine's bin width
    // is 2 * rate / (channels * transform) hertz and is not a whole number of
    // hertz in general. Rounding it here would put the sizing decision one
    // bin either side of the truth for no reason, and the integer-hertz
    // convention exists to stop exactly that kind of quiet rounding.
    //
    // An unstated request is met by anything, because a source that said
    // nothing cannot refuse what a caller picked.
    [[nodiscard]] bool met_by(std::int64_t bin_width_numerator,
                              std::int64_t bin_width_denominator) const;
};

// The narrowest signal an operator is likely to want identified in a span,
// from where that span sits.
//
// THE MODE WIDTHS, AND WHERE EACH NUMBER COMES FROM
//
//   FT8     50 Hz   WSJT-X User Guide, protocol specifications table: 8-FSK,
//                   6.25 Hz tone spacing, 50 Hz occupied bandwidth, 15 s T/R
//                   period.
//   PSK31   31 Hz   Peter Martinez G3PLX, "PSK31: A New Radio-Teletype Mode",
//                   RadCom, December 1998: 31.25 baud BPSK.
//   CW      50 to   ITU-R SM.1138, necessary bandwidth for class A1A, Bn = BK
//           150 Hz  with K = 5 on a fading circuit. 10 to 30 baud, which is
//                   roughly 12 to 36 words per minute, gives 50 to 150 Hz.
//   RTTY    261 Hz  45.45 baud, 170 Hz shift, by Carson's rule
//                   shift + 2 * baud. The amateur HF parameters.
//
// PSK31 is the narrowest of those, so a span that reaches HF asks for 31 Hz
// across four bins, a ceiling of 7.75 Hz per bin.
//
// WHAT THAT COSTS, WORKED THROUGH, because the point of HF is that it is
// cheap. Bin width and frame rate are the same number in this geometry: a
// coarse channel's stream runs at 2 * rate / channels and a transform of N
// points over it produces one frame per N samples, so both come out at
// 2 * rate / (channels * N). N is capped at dsp::kMaxSpectrumTransform, 2048,
// so the request is met by raising the channel count, which Engine::open_source
// does when the caller named none. A 2 MS/s capture at 7.1 MHz goes from the
// 8 channels engine::default_channel_count gives it to 256, 7.63 Hz per bin at
// 7.63 frames per second, which is 114 frames inside a 15-second FT8
// transmission. A 96 kS/s recording goes from 2 channels to 16, 5.86 Hz.
//
// WHAT THIS PARAGRAPH USED TO SAY: "On a 1 MS/s HF recording over 64 channels
// a 16384-point transform is 1.9 Hz per bin at 1.9 frames per second, which is
// 28 frames inside a 15-second FT8 transmission. The request above is already
// met at 4096 points; 16384 is what the rate can afford." No transform above
// 2048 points exists in this engine, so neither the 16384 nor the 4096 is a
// grid anything here can build.
//
// THE BOUNDARY is 30 MHz, the top of ITU-R V.431-8 band 7. The test is the
// span's low edge rather than its centre, so a capture straddling the top of
// HF asks for the finer grid rather than the coarser one.
//
// Above it the request is 12.5 kHz across four bins, from the narrowest
// analogue channel plan in common use. The shipped 36.6 Hz meets that by a
// factor of 85, so nothing about a VHF or UHF session changes. It is stated
// rather than left at zero so that the number is on the record and a later
// geometry cannot get coarser than it without something noticing.
//
// A centre at or below zero returns an unstated request. A recording at DC
// has not said where it was taken, and guessing HF from that would put the
// finest grid in the project onto every synthesised baseband scene.
//
// WHAT THIS DOES NOT KNOW. A weak-signal operator running FT8 on 2 metres
// wants the HF figure, and nothing reachable from a tuning alone can tell
// that apart from a repeater listener on the same frequency. The request is a
// default from the band, and a caller who knows the session overwrites it.
[[nodiscard]] ResolutionRequest resolution_for_span(dsp::Hertz center, dsp::SampleRate rate);

struct SourceCapabilities {
    // The URI this source was opened from, so a session can be reproduced.
    std::string uri;

    // "file", "synthetic", "rtlsdr".
    std::string backend;

    std::string display_name;

    // The physical device's own serial string, where it has one and it could
    // be read. Empty for a file and a synthetic scene, and for a dongle that
    // was described without being opened.
    //
    // It is what a calibration is keyed by, because an index is only an
    // enumeration order and two dongles swap places across a replug. See
    // core/source/calibration.h for why it is still not a unique key.
    std::string serial;

    // True when the device itself was told a frequency correction when it was
    // opened, which an rtlsdr URI with ppm= does through librtlsdr. The
    // engine then applies no stored correction of its own on top, since
    // two corrections of one crystal is a crystal corrected twice.
    bool device_corrects_frequency = false;

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

    // Conditions an operator would want to know about that did not stop the
    // source opening.
    //
    // A WAV whose auxi chunk carries no centre frequency, a SigMF sidecar
    // with an empty captures array, a recording playing across capture
    // segments that happen to agree. Each of those is the answer to "why is
    // this at the wrong frequency" an hour later, and a source that reported
    // only its failures would have said nothing about any of them. A decoder
    // that is not locked has to look different from a signal that is not
    // there, and the same holds one layer down: a recording whose centre
    // frequency nobody supplied has to look different from one recorded at
    // DC.
    std::vector<std::string> notes;

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

    // How fine a spectrum this source's content needs. See ResolutionRequest.
    //
    // Engine::open_source reads it. A stated request that the caller's
    // EngineConfig::spectrum_transform does not meet doubles the transform
    // until it does or until the device's ceiling stops it, and either outcome
    // is reported through the same clamp sentence a reduced channel count uses.
    // It is RAISED AND NEVER LOWERED: a request for a coarser grid than the
    // caller chose would be a request to throw a measurement away, and nothing
    // in this struct asks for less.
    //
    // WHAT THIS PARAGRAPH USED TO SAY: "NOTHING READS THIS YET, AND THIS IS THE
    // HALF THAT CANNOT DECIDE IT. The decision belongs to engine::EngineConfig,
    // which already chooses one side of the same geometry:
    // engine::default_channel_count picks the coarse channel count from the
    // source's rate, and EngineConfig::spectrum_transform is the per-channel
    // transform that is still a caller-supplied constant. Meeting a request
    // means picking that transform so that 2 * rate / (channels * transform)
    // satisfies ResolutionRequest::met_by, then reporting what was settled on in
    // EngineInfo::spectrum the way the clamped channel count already is." That
    // is a description of what was built, down to where the reporting goes. The
    // sentence after it, that the wiring was left out so a source would not
    // reach up into the graph, is why the decision sits in the engine rather
    // than here; that part still holds and the engine's own note repeats it.
    //
    // The tunable backends leave this unstated on purpose. A dongle's centre
    // moves under tune() while its capability description does not, so a
    // stated request there would be right at open and quietly wrong one
    // retune later. Wiring it up means the engine re-asking on retune, which
    // is the same lane as the choosing.
    //
    // ONE CONSTANT DOWNSTREAM DOES NOT FOLLOW THE GRID, and a finer grid is
    // what makes that reachable: detect::DetectorConfig::split_gap_bins is
    // eight bins whose justification is a frequency, so on an HF grid at 7.6 Hz
    // per bin, RTTY's 170 Hz shift is 22 bins and eight is a gap its two tones
    // clear. Its own note carries the arithmetic and why neither a bin count
    // nor a frequency is right for both bands.
    //
    // WHAT THIS PARAGRAPH USED TO SAY: "a finer transform is what makes that
    // reachable ... on an HF grid at 1.9 Hz per bin it splits every RTTY
    // signal into its two tones". The grid gets finer through the channel
    // count, and 1.9 Hz was the 16384-point transform that does not exist.
    ResolutionRequest resolution;

    [[nodiscard]] bool supports_rate(dsp::SampleRate rate) const;
    [[nodiscard]] bool can_tune(dsp::Hertz frequency) const;
};

}  // namespace revenant::source
