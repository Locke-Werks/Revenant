// revenant-cli: the program a person runs to hear a radio.
//
// Everything below this file already worked: a source's bytes cross the bus
// once, the channelizer runs on the GPU, and each receiver's audio comes back
// through core/engine/audio_egress.h. What did not exist was a way to point
// that at a frequency and get a WAV file or a loudspeaker out of it. This is
// that, and it is deliberately thin: no DSP, no threading of its own beyond
// one status thread, and no knowledge of any stage's internals.
//
// Three decisions in here are not obvious from the option list.
//
// WHAT GETS PRINTED BEFORE THE RUN. The placement block is the whole reason
// this tool is pleasant to use. A receiver that is one channel off, or whose
// bandwidth was clamped to what a grid channel can carry, sounds exactly like
// a dead band, and the only way to tell them apart afterwards is to have been
// shown the placement beforehand. So the grid, the channel each receiver
// landed on, its exact channel centre and its residual offset are printed
// before a single sample moves.
//
// ONE RECEIVER, TWO DESTINATIONS. AudioEgress carries one backend per
// receiver id and refuses a second registration for the same id, so
// --record and --play on the same receiver cannot both be one egress slot.
// Rather than dropping one silently, the monitor is registered as its own
// egress stream under an id the engine never issues, and the receiver's sink
// publishes the same chunk to both. The two streams count separately, which
// is honest: a monitor that is dropping audio because the source is running
// faster than realtime says nothing about the recording, and the summary
// shows them apart.
//
// CTRL-C IS A CLEAN STOP. A recording killed by an unhandled Ctrl-C is a WAV
// file whose header describes however much of it had been refreshed, and the
// device is left open. The console control handler calls Engine::stop(),
// which unblocks run() on the main thread, and the ordinary teardown path
// finalises the files and closes the endpoint.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <complex>
#include <mutex>
#include <numbers>
#include <optional>
#include <print>
#include <utility>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "core/dsp/spectrum_levels_reference.h"
#include "core/decode/rds_bits.h"
#include "core/decode/rds_groups.h"
#include "core/characterise/characterise.h"
#include "core/detect/detector.h"
#include "core/detect/front_end.h"
#include "core/detect/groups.h"
#include "core/detect/tier_two.h"
#include "core/dsp/spectrum_reference.h"
#include "core/dsp/types.h"
#include "core/dsp/vrx_reference.h"
#include "core/engine/audio_egress.h"
#include "core/engine/audio_wasapi.h"
#include "core/engine/engine.h"
#include "core/engine/spectrum_scale.h"
#include "core/engine/spsc_ring.h"
#include "core/engine/vrx.h"
#include "core/engine/wav_writer.h"
#include "core/error.h"
#include "core/identify/identify.h"
#include "core/rpc/decoders.h"
#include "core/source/capabilities.h"
#include "core/source/registry.h"

namespace {

using revenant::Error;
using revenant::Expected;
using revenant::Status;
using revenant::fail;
using revenant::with_context;

namespace characterise = revenant::characterise;
namespace decode = revenant::decode;
namespace detect = revenant::detect;
namespace dsp = revenant::dsp;
namespace engine = revenant::engine;
namespace identify = revenant::identify;
namespace rpc = revenant::rpc;
namespace source = revenant::source;

using dsp::Hertz;
using dsp::SampleRate;
using engine::Demod;

// ---------------------------------------------------------------------------
// Numbers in and numbers out
// ---------------------------------------------------------------------------

// 7100000, 7.1M, 162.550M, 14074k, 500 into exact integer hertz.
//
// The grammar lives in core/source/registry.h, which owns the text layer of a
// source, because a URI's freq= has to mean exactly what --vrx means. Two
// copies of it would drift and a number would then depend on where it was
// typed.
using source::parse_frequency;

[[nodiscard]] Expected<double> parse_real(std::string_view text, std::string_view what)
{
    double value = 0.0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    // from_chars rather than strtod: strtod honours the C locale, so on a
    // machine set to a comma decimal separator "0.5" would parse as 0.
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return fail(std::format("{} '{}' is not a number", what, text));
    }
    return value;
}

[[nodiscard]] Expected<std::int64_t> parse_integer(std::string_view text, std::string_view what)
{
    std::int64_t value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return fail(std::format("{} '{}' is not a whole number", what, text));
    }
    return value;
}

// Display only. Every decision is made on the integer.
[[nodiscard]] std::string format_hz(Hertz hertz)
{
    const double value = static_cast<double>(hertz);
    if (std::abs(hertz) >= 1'000'000) {
        return std::format("{:.6f} MHz", value / 1e6);
    }
    if (std::abs(hertz) >= 1'000) {
        return std::format("{:.3f} kHz", value / 1e3);
    }
    return std::format("{} Hz", hertz);
}

[[nodiscard]] std::string format_bytes(std::uintmax_t bytes)
{
    if (bytes >= (1ULL << 20)) {
        return std::format("{:.2f} MiB", static_cast<double>(bytes) / static_cast<double>(1ULL << 20));
    }
    if (bytes >= (1ULL << 10)) {
        return std::format("{:.2f} KiB", static_cast<double>(bytes) / 1024.0);
    }
    return std::format("{} B", bytes);
}

[[nodiscard]] const char* clock_source_name(source::ClockSource clock)
{
    switch (clock) {
        case source::ClockSource::Internal: return "internal";
        case source::ClockSource::Tcxo: return "tcxo";
        case source::ClockSource::ExternalReference: return "external reference";
        case source::ClockSource::Gps: return "gps";
        case source::ClockSource::Pps: return "pps";
    }
    return "unknown";
}

[[nodiscard]] const char* flow_name(source::FlowControl flow)
{
    return flow == source::FlowControl::Paced ? "paced" : "demand";
}

// Flow control is the property everything else falls out of, so the block
// printed before a run spells out what it means rather than naming it.
[[nodiscard]] const char* flow_explained(source::FlowControl flow)
{
    return flow == source::FlowControl::Paced ? "paced, the device's clock sets the rate"
                                              : "demand, the consumer sets the rate";
}

// ---------------------------------------------------------------------------
// Receiver specifications
// ---------------------------------------------------------------------------

// What one mode needs to sound right when the operator did not say.
//
// This used to be a table of eight widths here, private to the CLI, and it
// has moved to dsp::default_passband. A default that lives in one client is
// a default the next client invents differently, and the widths are now
// edges: USB's default is the carrier plus 300 to plus 2700 rather than
// 3 kHz of something centred on nothing in particular.

struct VrxSpec {
    // As typed. Absolute radio frequency unless relative is set, in which
    // case it is an offset from the source's own centre.
    Hertz center = 0;
    bool relative = false;

    Demod demod = Demod::Nfm;

    // The passband, as signed hertz from center. Always resolved here rather
    // than left as a bandwidth for the engine to expand: the shorthand
    // exists for wire compatibility and a client that has a mode in its hand
    // has no reason to use it.
    dsp::Passband passband{};

    // center resolved against the source's centre, which is the frame
    // VrxParams is in. Filled once the source is open, because nothing knows
    // the centre before then.
    Hertz baseband = 0;
};

// freq[:mode[:bandwidth]] or freq:mode:low:high
[[nodiscard]] Expected<VrxSpec> parse_vrx_spec(std::string_view text)
{
    if (text.empty()) {
        return fail("--vrx needs a spec, such as 162.550M:nfm:16k");
    }

    std::string_view rest = text;
    const auto first = rest.find(':');
    const std::string_view frequency = rest.substr(0, first);
    rest = (first == std::string_view::npos) ? std::string_view{} : rest.substr(first + 1);

    const auto second = rest.find(':');
    const std::string_view mode = rest.substr(0, second);
    rest = (second == std::string_view::npos) ? std::string_view{} : rest.substr(second + 1);

    // The third field is either one width or the low edge of a pair, and
    // which it is depends on whether a fourth follows. A pair is how an
    // asymmetric passband is named, so `7.1M:usb:300:2700` is the carrier
    // plus 300 to plus 2700 hertz and `7.1M:usb:2.4k` is the same 2.4 kHz
    // the mode's shorthand would have produced.
    const auto third = rest.find(':');
    const std::string_view width = rest.substr(0, third);
    const std::string_view upper =
        (third == std::string_view::npos) ? std::string_view{} : rest.substr(third + 1);

    if (!upper.empty() && upper.find(':') != std::string_view::npos) {
        return fail(std::format(
            "--vrx '{}' has too many fields. The form is freq[:mode[:bandwidth]] or "
            "freq:mode:low:high, where low and high are signed hertz from freq",
            text));
    }

    VrxSpec spec;

    // A leading sign means "this far from where the source is tuned", which
    // is the only way to name a frequency in a capture whose centre the
    // operator does not remember. Everything else is absolute radio
    // frequency, because that is what is printed on a repeater list. The two
    // coincide for a source that declares no centre, which is every
    // synthetic scene, so nothing about the common case changes.
    spec.relative = frequency.starts_with('+') || frequency.starts_with('-');

    auto center = parse_frequency(frequency, "--vrx frequency");
    if (!center) {
        return std::unexpected(center.error());
    }
    spec.center = *center;
    if (!spec.relative && spec.center < 0) {
        return fail(std::format(
            "--vrx '{}' is a negative absolute frequency. Write an offset from the source's "
            "centre as -150k with the sign attached to the number",
            text));
    }

    if (!mode.empty()) {
        auto demod = engine::demod_from_name(mode);
        if (!demod) {
            return std::unexpected(with_context(demod.error(), std::format("--vrx '{}'", text)));
        }
        spec.demod = *demod;
    }

    if (width.empty()) {
        spec.passband = dsp::default_passband(spec.demod);
    } else if (upper.empty()) {
        auto parsed = parse_frequency(width, "--vrx bandwidth");
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        if (*parsed <= 0) {
            return fail(std::format("--vrx '{}' asks for a bandwidth of {} Hz", text, *parsed));
        }

        // Expanded here rather than sent as a bandwidth, so the CLI and the
        // engine agree by calling the same function instead of by both
        // knowing the same rule.
        engine::VrxParams shorthand;
        shorthand.demod = spec.demod;
        shorthand.bandwidth = *parsed;
        auto resolved = dsp::resolve_passband(shorthand);
        if (!resolved) {
            return std::unexpected(
                with_context(resolved.error(), std::format("--vrx '{}'", text)));
        }
        spec.passband = *resolved;
    } else {
        auto low = parse_frequency(width, "--vrx passband low edge");
        if (!low) {
            return std::unexpected(low.error());
        }
        auto high = parse_frequency(upper, "--vrx passband high edge");
        if (!high) {
            return std::unexpected(high.error());
        }
        if (*low >= *high) {
            return fail(std::format(
                "--vrx '{}' asks for a passband from {} Hz to {} Hz, which is empty or "
                "inverted. Both are signed hertz from the receiver's frequency and the low "
                "edge is the smaller number, so USB is 300:2700 and LSB is -2700:-300",
                text, *low, *high));
        }
        spec.passband = dsp::Passband{*low, *high};
    }

    return spec;
}

// ---------------------------------------------------------------------------
// RDS
// ---------------------------------------------------------------------------

// The top of the FM composite: the 57 kHz subcarrier plus the 2375 Hz of
// shaping EN 50067 clause 1.7 puts either side of it. A receiver whose
// granted passband does not reach this far has the data band outside its
// filter and will never decode a bit, however strong the station is.
constexpr Hertz kCompositeTopHz = 59'375;

// WHY AN RDS RECEIVER IS A SECOND RECEIVER AND NOT THE ONE YOU ARE LISTENING
// TO.
//
// RDS lives at 57 kHz on the FM composite, which exists only on the far side
// of the discriminator. core/dsp/vrx_reference.h puts the audio decimation
// filter's passband edge at 0.4 of the audio rate, so at the 48 kHz a
// loudspeaker wants the edge is 19.2 kHz and the subcarrier is buried in the
// stopband before any sink sees it. At 171000 the edge is 68.4 kHz, the whole
// composite up to 59375 Hz survives, and the ordinary AudioSink is carrying
// the multiplex rather than audio.
//
// 171000 is not a round number picked for headroom. It is 3 x 57000 and
// 144 x 1187.5, both exact, so the subcarrier and the bit clock both land on
// a whole number of samples and neither loop starts with a rate error to
// absorb. It is also decode::RdsBitsConfig::rate's default, which is where
// this takes it from rather than repeating the literal.
//
// One consequence for the operator: this receiver's audio is a 171 kHz
// multiplex, not sound. It is not offered to --record or --play, because
// what would come out of either is a screech.
struct RdsSpec {
    // As typed, and the same rule --vrx follows: absolute radio frequency
    // unless a leading sign made it an offset from the source's centre.
    Hertz center = 0;
    bool relative = false;

    // center resolved against the source's centre, filled once it is known.
    Hertz baseband = 0;
};

[[nodiscard]] Expected<RdsSpec> parse_rds_spec(std::string_view text)
{
    if (text.empty()) {
        return fail("--rds needs a broadcast FM frequency, such as --rds 98.5M");
    }
    if (text.find(':') != std::string_view::npos) {
        return fail(std::format(
            "--rds '{}' takes a frequency and nothing else. The mode, the passband and the "
            "audio rate are all fixed by what an RDS decode needs; --rds-region chooses "
            "between the RDS and RBDS group tables",
            text));
    }

    RdsSpec spec;
    spec.relative = text.starts_with('+') || text.starts_with('-');

    auto center = parse_frequency(text, "--rds frequency");
    if (!center) {
        return std::unexpected(center.error());
    }
    spec.center = *center;
    if (!spec.relative && spec.center < 0) {
        return fail(std::format(
            "--rds '{}' is a negative absolute frequency. Write an offset from the source's "
            "centre as -150k with the sign attached to the number",
            text));
    }
    return spec;
}

// ---------------------------------------------------------------------------
// The command line
// ---------------------------------------------------------------------------

struct Options {
    std::string uri;
    std::vector<VrxSpec> receivers;

    // 1-based receiver numbers, as the operator counted them on the command
    // line. Repeatable, because two receivers on the same sound card is what
    // a shared-mode stream is for.
    std::vector<std::size_t> play;

    std::string audio_device;
    double volume = 1.0;

    std::string record;
    engine::WavSampleFormat record_format = engine::WavSampleFormat::Float32;

    // Seconds of source time. Zero runs until the source ends or Ctrl-C.
    double duration = 0.0;

    // Multiple of realtime a Demand source is asked to deliver at. Zero is
    // unthrottled. Negative means nothing was asked for, so --play can supply
    // the default a loudspeaker needs without overriding an explicit choice.
    double pace = -1.0;

    std::uint32_t channels = 64;

    // Whether --channels was given. When it was not, 64 is a default that
    // gives way to the engine's own choice on a source that states the
    // resolution it needs, which is HF: EngineConfig::channels_yield_to_source.
    bool channels_given = false;

    SampleRate audio_rate = 48'000;
    int gpu = -1;

    // Draw the full-span waterfall. Points per coarse channel, or zero for
    // the engine's default; the flag on its own takes the default.
    bool spectrum = false;
    std::uint32_t spectrum_points = 0;

    // Either end of the colour map, held still in dBFS. Empty is automatic,
    // which is the default docs/ui-spectrum.md wants.
    std::optional<float> spectrum_floor_db;
    std::optional<float> spectrum_ceiling_db;

    // Run the wideband detector and print its track list. Needs the spectrum
    // stage, and turns it on by itself if --spectrum did not.
    //
    // Both thresholds are the operator's, per docs/detection.md, which is
    // why they are flags rather than constants. The detection one is in dB
    // of SNR in the 2500 Hz reference bandwidth, which is the only unit a
    // single number can mean the same thing in across a span carrying a
    // 50 Hz carrier and a 200 kHz broadcast.
    bool detect = false;
    double detect_threshold_db = 6.0;
    double detect_confidence = 0.5;

    // Zero means leave DetectorConfig's own default alone, which is what
    // anybody not asking this question wants. It is a count of bins and not a
    // frequency on purpose; see the flag's help text.
    std::uint32_t detect_split_gap = 0;

    // How far apart two tracks' centres can sit and still be followed as one
    // group by detect::LineGrouper, or zero for not asked.
    //
    // A GROUPING AND NOT A CLASSIFICATION. No field on the wire carries it,
    // and the distance is the operator's for the same reason both thresholds
    // are: no measurement has chosen one.
    //
    // WHAT THIS USED TO SAY: "Nothing on the engine knows about it and no
    // field carries it; this arranges rows on a terminal". The arranging moved
    // into core/detect/groups.h, where a group has an id that survives between
    // decisions; the second half still holds.
    Hertz detect_groups = 0;

    // The ITU occupied-power fraction the reported bandwidth holds, or zero to
    // leave DetectorConfig's own default alone. Exposed for the same reason
    // split_gap_bins was: a constant nobody can sweep is a constant nobody can
    // check, and a claim about what sets a bandwidth is worth no more than the
    // sweep that survives it.
    double detect_occupied = 0.0;

    // Probe receivers for tier two, which --detect turns on: the engine
    // places them on live tracks, characterises what they collect, and the
    // table prints the answer per track. Zero switches tier two off and costs
    // nothing; see core/engine/probe.h for what four cost.
    std::uint32_t detect_probes = 4;

    // Absolute hertz to characterise. A raw receiver is placed there and its
    // complex baseband is handed to core/characterise.
    //
    // ZERO IS A FREQUENCY AND NOT A SENTINEL. On a file source
    // EngineInfo::source_center is zero, so an offset of zero is the middle of
    // the span, which is exactly where a strong carrier or an LO artefact
    // sits. Asking about it used to reach "nothing to do" and exit 2, which
    // reads as the whole invocation being malformed rather than as one
    // argument being ignored. Whether it was asked lives in its own flag.
    Hertz characterise_hz = 0;
    bool characterise_asked = false;

    // Width to filter the extract to before characterising, or zero to hand
    // over the whole coarse channel.
    Hertz characterise_width = 0;

    // Seconds of baseband to collect, or zero for just over the stage's own
    // floor. THE ANSWER DEPENDS ON THIS, which is why it is a knob and not a
    // constant; see the help text.
    double characterise_seconds = 0.0;

    // How far a carrier may drift and still read as concentrated, in hertz.
    // The analysis segment is derived from it so the answer stops depending on
    // how much was collected.
    double characterise_drift = 5.0;

    // Broadcast FM stations to decode RDS from, repeatable. Each one gets a
    // receiver of its own; see RdsSpec for why it cannot share one with a
    // receiver somebody is listening to.
    std::vector<RdsSpec> rds;

    // RBDS by default because this radio is in the United States and the two
    // tables disagree about almost every programme type. Reading a US
    // station with the European table is not a near miss: PTY 15 is "Other
    // Music" there and "Classic Rock" here.
    decode::Region rds_region = decode::Region::kRbds;

    // Decoders to attach to the --vrx receivers, by registry name, repeatable.
    // "auto" is the decoder named after each receiver's mode, or every decoder
    // that reads a receiver no decoder is named after. These are the
    // same adapters revenant-engine serves over subscribeDecoded, from
    // core/rpc/decoders.h, run here in-process.
    std::vector<std::string> decode;

    bool list = false;
    bool list_audio = false;
    bool quiet = false;
    bool help = false;
    long status_ms = 500;
};

void print_usage()
{
    std::print(
        "revenant-cli: tune receivers at a source and record or listen to them\n"
        "\n"
        "Usage: revenant-cli <source-uri> [options]\n"
        "\n"
        "Receivers:\n"
        "  --vrx <spec>        A receiver, repeatable. freq[:mode[:bandwidth]]\n"
        "                      or freq:mode:low:high for an asymmetric passband.\n"
        "                      freq and bandwidth take 7100000, 7.1M, 162.550M, 14074k.\n"
        "                      mode is raw am nfm wfm usb lsb dsb cw, default nfm.\n"
        "                      Default passband per mode: am +/-5k, nfm +/-8k,\n"
        "                      wfm +/-100k, usb 300..2700, lsb -2700..-300,\n"
        "                      dsb +/-3k, cw +/-250, raw +/-6k.\n"
        "                      low and high are signed hertz from freq, so a\n"
        "                      sideband filter is named where it sits and can be\n"
        "                      pulled as wide as the channel allows.\n"
        "                      freq is absolute radio frequency. A leading + or -\n"
        "                      makes it an offset from wherever the source is\n"
        "                      tuned, which is what a capture wants when you do\n"
        "                      not remember its centre.\n"
        "                      Examples: --vrx 162.550M:nfm:16k  --vrx 7.1M:lsb:2.8k\n"
        "                                --vrx 7.074M:usb:300:2700  --vrx +12.5k:nfm\n"
        "                                --vrx 3.9M:usb:50:6000\n"
        "\n"
        "Listening:\n"
        "  --play [<n>]        Send receiver n to the speakers, 1-based, default 1.\n"
        "                      Repeatable, once per receiver you want to hear.\n"
        "  --audio-device <id> Render endpoint id from --list-audio.\n"
        "  --volume <x>        Monitor gain, default 1.0. Never touches the recording.\n"
        "\n"
        "Recording:\n"
        "  --record <path>     Write WAV. One receiver: a file path. Several: a\n"
        "                      directory, and each receiver gets\n"
        "                      <dir>/vrx<N>-<freq>-<mode>.wav. Created if missing.\n"
        "  --record-format <f> float32 (default) or pcm16.\n"
        "\n"
        "Decoding:\n"
        "  --rds <freq>        Decode RDS from the broadcast FM station at freq,\n"
        "                      repeatable. Adds a receiver of its own: wfm, +/-100k,\n"
        "                      and 171000 S/s of audio, which is what carries the FM\n"
        "                      composite the 57 kHz subcarrier lives on. That receiver\n"
        "                      is not offered to --record or --play, because what comes\n"
        "                      out of it is a multiplex rather than sound. Listen to\n"
        "                      the same station at the same time by adding an ordinary\n"
        "                      --vrx <freq>:wfm --play beside it.\n"
        "                      The composite reaches 59375 Hz either side, so the grid\n"
        "                      has to have channels wide enough to grant that: at a\n"
        "                      2.4 MS/s source --channels 8 does and the default 64\n"
        "                      does not. The refusal names the numbers.\n"
        "  --rds-region <r>    rbds (default, North America) or rds (Europe and the\n"
        "                      rest of ITU regions 1 and 3). It chooses the programme\n"
        "                      type table and whether a PI code is read back as a call\n"
        "                      sign. The two PTY tables agree on four of thirty-two\n"
        "                      entries, so the wrong one mislabels almost everything.\n"
        "  --decode <name>     Attach a decoder to every --vrx receiver whose output it\n"
        "                      reads, and print each message it recovers as it arrives:\n"
        "                      the time in the receiver's stream, the receiver number,\n"
        "                      the decoder, the kind of message and one line of text.\n"
        "                      Repeatable. p25p1, dstar, tetra and dmr read a --vrx in the\n"
        "                      mode of the same name, such as --vrx 453.1M:p25p1, and\n"
        "                      m17 a p25p1 or raw one. rtty, sitor_b, navtex, psk31,\n"
        "                      psk63 and qpsk31 read a usb or lsb --vrx, psk with the\n"
        "                      tone at 1000 Hz; cw reads a cw one, or usb or lsb with\n"
        "                      the tone at 700 Hz; ax25 (with APRS) and pocsag read an\n"
        "                      nfm one. auto attaches the decoder named after each\n"
        "                      receiver's mode, and on a usb, lsb or nfm receiver\n"
        "                      every decoder that reads it.\n"
        "                      Nothing is decrypted: an encrypted P25 call is\n"
        "                      reported as encrypted.\n"
        "\n"
        "Watching:\n"
        "  --spectrum[=<n>]    Draw an ASCII waterfall of the whole span, one row per\n"
        "                      status interval, peak held in between. n is points per\n"
        "                      coarse channel, a power of two, default 2048; the frame\n"
        "                      is then channels*n/2 bins wide. Needs no --vrx.\n"
        "  --spectrum-floor <dbfs>\n"
        "  --spectrum-ceiling <dbfs>\n"
        "                      Hold one or both ends of the colour map still, in dBFS.\n"
        "                      Both ends track the signal automatically otherwise, which\n"
        "                      is right almost always; pin them when two captures have\n"
        "                      to be compared, because a scale that moves is a scale\n"
        "                      that lies about which signal was stronger. The run's own\n"
        "                      summary prints the automatic ends, which is where the\n"
        "                      numbers to pin come from.\n"
        "  --detect            Run the wideband detector and print the live track list:\n"
        "                      centre, bandwidth, SNR, confidence, margin and age, one\n"
        "                      line per track. Confidence is how long a track has been\n"
        "                      there and margin is how far it stood above the threshold;\n"
        "                      neither says a track is real.\n"
        "                      Turns the spectrum stage on by itself, so it needs\n"
        "                      neither --vrx nor --spectrum.\n"
        "                      The last column is tier two: the engine puts a probe\n"
        "                      receiver on each live track, oldest unclassified first,\n"
        "                      collects two seconds of its baseband and characterises it.\n"
        "                      It reads family, symbol rate and confidence, or \"unknown\"\n"
        "                      when a probe ran and named nothing, or \"-\" before one has.\n"
        "                      A family the characteriser itself refuses to let drive\n"
        "                      detection is shown in brackets and changes nothing.\n"
        "                      A PROBE COUNTS STREAM SECONDS AND IS PLACED ON WALL ONES, so\n"
        "                      an unthrottled file outruns it; give a file --pace or\n"
        "                      --realtime when the last column matters.\n"
        "  --detect-probes <n> Probe receivers for tier two, default 4, 0 for none.\n"
        "                      Each is a receiver the GPU runs on every block, whether it\n"
        "                      is collecting or not.\n"
        "  --characterise <hz> Ask what modulation is at this frequency. Places a raw\n"
        "                      receiver there, collects one coarse channel of complex\n"
        "                      baseband and hands it to core/characterise, which answers\n"
        "                      with a family, a symbol rate where it found one, and its\n"
        "                      own refusal where it did not.\n"
        "                      THE COARSE CHANNEL IS THE EXTRACT, so this is honest on a\n"
        "                      slow source and wasteful on a fast one: at 96 kS/s on a\n"
        "                      64-channel grid a channel is 1.5 kHz, which suits an HF\n"
        "                      signal, and at 2.4 MS/s it is 37.5 kHz, which is far wider\n"
        "                      than anything being asked about.\n"
        "                      It needs 16384 samples, which is 5.5 s at a 3 kS/s channel\n"
        "                      rate, so give --duration enough to collect them.\n"
        "                      A PSK CALL ON A WEAK SIGNAL IS PROBABLY A CARRIER. Squaring a\n"
        "                      tone gives a tone, so a carrier lights the same M-th power line\n"
        "                      BPSK does, and what separates them is an envelope the noise\n"
        "                      takes first. Measured: one carrier walked down through noise is\n"
        "                      named correctly at 3 dB SNR with 0.66 confidence and called PSK\n"
        "                      at -3 dB with 0.98, so the confidence column rises as the answer\n"
        "                      gets worse. Read the concentration instead: it tracked the SNR\n"
        "                      across every step. Ten of thirty-one channels swept across 20 m\n"
        "                      came back PSK, order 2 every time, with symbol rates from 0 to\n"
        "                      738 baud, mostly where the detector found nothing.\n"
        "  --characterise-drift <hz>\n"
        "                      How far a carrier may drift and still read as concentrated,\n"
        "                      default 5. The analysis segment is derived from it, three\n"
        "                      bins wide, and handed to the stage so the answer stops\n"
        "                      depending on how much was collected.\n"
        "                      Without it the window is a sixteenth of whatever arrived:\n"
        "                      4.4 Hz over eleven seconds of a 3 kS/s channel and 1.1 Hz\n"
        "                      over sixty, and a real HF carrier crosses the threshold\n"
        "                      between the two for no reason but the length.\n"
        "  --characterise-seconds <s>\n"
        "                      How much baseband to collect, default just over the 16384\n"
        "                      samples the stage needs.\n"
        "                      THE ANSWER DEPENDS ON THIS AND NOTHING WARNS YOU.\n"
        "                      analysis_segment picks a transform length from the sample\n"
        "                      count, so a longer extract gives finer bins, and\n"
        "                      spectral_concentration is three of those bins: at a 3 kS/s\n"
        "                      channel rate that window is 4.4 Hz over 11 seconds and\n"
        "                      1.1 Hz over 60. Measured on a real 40 m carrier, the short\n"
        "                      extract reads unmodulated carrier at 0.53 and the long one\n"
        "                      reads unknown at 0.18, because the carrier drifts further\n"
        "                      than 1.1 Hz in a minute. Longer is not better here.\n"
        "  --characterise-width <hz>\n"
        "                      Filter the extract to this width, centred on the frequency\n"
        "                      asked about, before characterising it. Without it the whole\n"
        "                      coarse channel goes in, and everything else living in that\n"
        "                      channel goes in with it.\n"
        "                      It decimates as well as filtering, to about three times the\n"
        "                      width asked for, which keeps the noise nearer white than\n"
        "                      filtering alone does: empty 40 m low passed to 800 Hz at the\n"
        "                      full channel rate came back PSK at 0.98 confidence.\n"
        "                      IT DOES NOT MAKE NARROWING SAFE. The paragraph below is the\n"
        "                      measurement of what it still gets wrong.\n"
        "                      THE STAGE'S SAMPLE FLOOR CAPS HOW FAR IT CAN GO. 16384\n"
        "                      samples have to survive, so twenty seconds of a 3 kS/s\n"
        "                      channel can only decimate by three whatever width is asked\n"
        "                      for. It says so when that binds, and says what\n"
        "                      --characterise-seconds would lift it to.\n"
        "                      NARROWING THIS COLOURS THE NOISE AND THE STAGE WILL SAY SO\n"
        "                      CONFIDENTLY, AND DECIMATING DOES NOT STOP IT. Measured on a\n"
        "                      channel of 20 m holding nothing at all, concentration 0.007,\n"
        "                      over 55 seconds so the decimation was never capped: unfiltered\n"
        "                      it answers unknown, correctly; at 800 Hz it answers 2-PSK at\n"
        "                      0.67; at 300 Hz, decimated by three, it answers 2-PSK at 0.81;\n"
        "                      at 100 Hz it answers unknown again.\n"
        "                      Neither false call carried a symbol rate, and both named a\n"
        "                      carrier just past the cutoff: -422.6 Hz against 400, and\n"
        "                      +172.1 Hz against 150. That is the filter's own edge being read\n"
        "                      as a signal, and a SHARPER filter makes it worse rather than\n"
        "                      better: at enough taps to put the transition inside a tenth of\n"
        "                      the cutoff, both rose to 0.94.\n"
        "                      SO THE TEST TO RUN IS A CHANNEL WITH NOTHING IN IT. A family\n"
        "                      that moves when this does is an artefact of this, and a PSK\n"
        "                      call carrying no symbol rate is the shape of one. Two real\n"
        "                      carriers were named correctly at every width in the same run,\n"
        "                      so this costs the answers about noise rather than the answers\n"
        "                      about signals.\n"
        "  --detect-groups <hz>\n"
        "                      Follow tracks whose centres chain within this of each other\n"
        "                      as groups, through core/detect/groups.h, and print each under\n"
        "                      the table: its id, which survives from one table to the next,\n"
        "                      how long its current lines have all been in it, how far apart\n"
        "                      their births were, and where each line sits relative to the\n"
        "                      strongest, marked with an asterisk, with its own age beside it.\n"
        "                      READ THE TWO GROUP AGES FIRST. Lines one emitter produces are\n"
        "                      born together and stay together; an old carrier with a line\n"
        "                      a few seconds old beside it is two things that sit close. On\n"
        "                      real HF that is mostly what these are.\n"
        "                      Every live and held track is grouped, not only the ones over\n"
        "                      the confidence bar, so a group can name a line the table does\n"
        "                      not list.\n"
        "                      WHAT IT IS FOR. Five of the eight families measured in\n"
        "                      tests/detect are reported as separate spectral lines, one\n"
        "                      detection each: an AM station is three rows in this table\n"
        "                      and a narrowband FM one is fifteen. The set of lines is what\n"
        "                      tells them apart and no single row can. On the scene, cw is\n"
        "                      one line, am is a carrier with a matched pair either side,\n"
        "                      usb and lsb are two lines 1200 Hz apart, and nfm is a comb at\n"
        "                      the modulation frequency.\n"
        "                      IT DECIDES NOTHING and no field on the wire carries it. The\n"
        "                      gap is an argument because no measurement has chosen one, and\n"
        "                      a number chosen here would be a classification smuggled in as\n"
        "                      a grouping rule.\n"
        "  --detect-occupied <fraction>\n"
        "                      The share of a detection's excess power its reported\n"
        "                      bandwidth holds, default 0.99, which is the ITU occupied\n"
        "                      bandwidth. Exposed so it can be swept.\n"
        "                      WHAT A SWEEP OF IT ANSWERS: HOW A BAND HOLDS ITS POWER, which no\n"
        "                      single width can say. A band that is one filled thing shrinks\n"
        "                      smoothly as the fraction comes down. A band that is a carrier with\n"
        "                      sidebands holds still and then steps, because there is nothing\n"
        "                      between the core and the lines.\n"
        "                      Measured on 20 m, a 489 Hz detection reading concentration 0.82:\n"
        "                      3 Hz at 0.50, 3 at 0.55, 3 at 0.60, 5 at 0.65, 4 at 0.70 and then\n"
        "                      473 at 0.80. Seventy percent of it is in five hertz and the next\n"
        "                      tenth is a pair about 235 Hz either side. A real carrier over the\n"
        "                      same sweep goes 4, 7, 8, 10, 10 and an 11.7 kHz patch of noise floor\n"
        "                      shrinks smoothly and never steps.\n"
        "  --detect-split-gap <bins>\n"
        "                      How many consecutive bins at the noise floor separate two\n"
        "                      detections rather than one, default 8. IN BINS AND NOT IN\n"
        "                      HERTZ, which matters because a bin is not the same width on\n"
        "                      every grid: 8 bins is 293 Hz on a 2.4 MS/s VHF span and\n"
        "                      11.7 Hz on a 96 kS/s HF one. Raise it to stop one signal with\n"
        "                      interior nulls reading as several, lower it to tell two close\n"
        "                      signals apart.\n"
        "  --detect-threshold <db>\n"
        "                      Detection threshold, default 6. In dB of SNR in the\n"
        "                      2500 Hz reference bandwidth, which is what makes one\n"
        "                      number mean the same thing for a 50 Hz carrier and a\n"
        "                      200 kHz broadcast. Lower finds more and invents more;\n"
        "                      where it belongs depends on the band and the antenna.\n"
        "  --detect-confidence <x>\n"
        "                      Confidence a track needs before it is listed, from 0 up to\n"
        "                      but not including 1, default 0.5. A track earns confidence\n"
        "                      by being detected repeatedly and loses it while it is held\n"
        "                      through a gap, so it approaches 1 without reaching it and a\n"
        "                      bar of exactly 1 would list nothing at all.\n"
        "\n"
        "Run:\n"
        "  --duration <sec>    Stop after this many seconds of source time. Accepts a\n"
        "                      fraction. Omitted runs to the end of the source or\n"
        "                      until Ctrl-C.\n"
        "  --realtime          Deliver at one times realtime. Implied by --play,\n"
        "                      which is the only consumer that needs a clock.\n"
        "  --pace <x>          Deliver at x times realtime. 0 is unthrottled, which\n"
        "                      is the default without --play and is what makes an\n"
        "                      hour of capture take a minute. A live radio sets its\n"
        "                      own rate and ignores both of these.\n"
        "  --channels <n>      Channelizer channel count, default 64 above 30 MHz and\n"
        "                      the engine's own choice below it. Zero lets the engine\n"
        "                      size the grid from the source everywhere: a 2 MS/s\n"
        "                      capture at 7.1 MHz opens on 256 channels and 7.6 Hz\n"
        "                      bins, a 96 kS/s one on 16 channels and 5.9 Hz. A number\n"
        "                      pins the count on any band. More channels is a narrower\n"
        "                      widest receiver, and the two trade directly.\n"
        "  --audio-rate <hz>   Default 48000.\n"
        "  --gpu <n>           Device index, default -1, which honours\n"
        "                      REVENANT_GPU_INDEX. See revenant-devices.\n"
        "  --quiet             No periodic status line.\n"
        "  --status-ms <n>     Status interval, default 500.\n"
        "  --list              List sources and exit.\n"
        "  --list-audio        List render endpoints and exit.\n"
        "  -h, --help          This.\n"
        "\n"
        "A receiver can have both --record and --play. The egress layer carries one\n"
        "backend per receiver, so the monitor is registered as a second stream fed\n"
        "from the same audio and is counted separately in the summary.\n"
        "\n"
        "A file or synthetic source is not paced: it runs as fast as the GPU retires\n"
        "work, which is what makes an hour of capture take a minute. A monitor\n"
        "consumes at the sound card's rate, so listening to one of those will drop\n"
        "audio, and the summary says how much. Recording does not.\n"
        "\n"
        "Exit codes: 0 finished cleanly, 1 an error, 2 bad usage.\n"
        "\n"
        "Examples:\n"
        "  revenant-cli \"synthetic:wideband?rate=2400000&emitters=8&seed=4242\" \\\n"
        "      --spectrum --duration 5\n"
        "  revenant-cli \"synthetic:wideband?rate=2400000&emitters=8&seed=4242\" \\\n"
        "      --vrx 150k:nfm:16k --record out.wav --duration 5\n"
        "  revenant-cli \"file:///C:/captures/hf.cf32?rate=2400000&format=cf32\" \\\n"
        "      --vrx 7.1M:lsb --vrx 7.074M:usb --record C:/out --play 2\n"
        "  revenant-cli \"rtlsdr://0?freq=98.5M&rate=2400000&gain=20\" \\\n"
        "      --channels 8 --rds 98.5M --vrx 98.5M:wfm --play\n");
}

[[nodiscard]] Expected<Options> parse_options(int argc, char** argv)
{
    Options options;

    const auto value_of = [&](int& i, std::string_view name,
                              std::string_view inline_value,
                              bool has_inline) -> Expected<std::string> {
        if (has_inline) {
            return std::string(inline_value);
        }
        if (i + 1 >= argc) {
            return fail(std::format("{} needs a value", name));
        }
        ++i;
        return std::string(argv[i]);
    };

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};

        std::string_view inline_value;
        bool has_inline = false;
        if (arg.starts_with("--")) {
            if (const auto equals = arg.find('='); equals != std::string_view::npos) {
                inline_value = arg.substr(equals + 1);
                has_inline = true;
                arg = arg.substr(0, equals);
            }
        }

        if (arg == "-h" || arg == "--help") {
            options.help = true;
            return options;
        }
        if (arg == "--list") {
            options.list = true;
            continue;
        }
        if (arg == "--list-audio") {
            options.list_audio = true;
            continue;
        }
        if (arg == "--quiet") {
            options.quiet = true;
            continue;
        }

        if (arg == "--spectrum") {
            options.spectrum = true;
            if (has_inline) {
                auto points = parse_integer(inline_value, "--spectrum");
                if (!points) {
                    return std::unexpected(points.error());
                }
                if (*points < 4 || *points > dsp::kMaxSpectrumTransform ||
                    (*points & (*points - 1)) != 0) {
                    return fail(std::format(
                        "--spectrum takes a power of two between 4 and {} points per coarse "
                        "channel",
                        dsp::kMaxSpectrumTransform));
                }
                options.spectrum_points = static_cast<std::uint32_t>(*points);
            }
            continue;
        }

        if (arg == "--detect") {
            options.detect = true;
            continue;
        }

        if (arg == "--characterise" || arg == "--characterize") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto hz = parse_frequency(*text, arg);
            if (!hz) {
                return std::unexpected(hz.error());
            }
            options.characterise_hz = *hz;
            options.characterise_asked = true;
            continue;
        }

        if (arg == "--characterise-drift") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_real(*text, arg);
            if (!number) {
                return std::unexpected(number.error());
            }
            if (!std::isfinite(*number) || *number <= 0.0) {
                return fail("--characterise-drift takes a positive width in hertz");
            }
            options.characterise_drift = *number;
            continue;
        }

        if (arg == "--characterise-seconds") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_real(*text, arg);
            if (!number) {
                return std::unexpected(number.error());
            }
            if (!std::isfinite(*number) || *number <= 0.0) {
                return fail("--characterise-seconds takes a positive number of seconds");
            }
            options.characterise_seconds = *number;
            continue;
        }

        if (arg == "--characterise-width") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto hz = parse_frequency(*text, arg);
            if (!hz) {
                return std::unexpected(hz.error());
            }
            if (*hz <= 0) {
                return fail("--characterise-width takes a positive width, such as 500");
            }
            options.characterise_width = *hz;
            continue;
        }

        if (arg == "--detect-groups") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto hz = parse_frequency(*text, arg);
            if (!hz) {
                return std::unexpected(hz.error());
            }
            if (*hz <= 0) {
                return fail("--detect-groups takes a positive gap, such as 2k");
            }
            options.detect_groups = *hz;
            options.detect = true;
            continue;
        }

        if (arg == "--detect-occupied") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_real(*text, arg);
            if (!number) {
                return std::unexpected(number.error());
            }
            if (!(*number >= 0.01) || !(*number <= 1.0)) {
                return fail("--detect-occupied takes a power fraction from 0.01 to 1.0, "
                            "such as 0.99");
            }
            options.detect_occupied = *number;
            options.detect = true;
            continue;
        }

        if (arg == "--detect-split-gap") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto bins = parse_integer(*text, arg);
            if (!bins) {
                return std::unexpected(bins.error());
            }
            if (*bins < 1 || *bins > 65535) {
                return fail("--detect-split-gap takes a whole number of bins from 1 to 65535");
            }
            options.detect_split_gap = static_cast<std::uint32_t>(*bins);
            options.detect = true;
            continue;
        }

        if (arg == "--detect-probes") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto count = parse_integer(*text, arg);
            if (!count) {
                return std::unexpected(count.error());
            }
            if (*count < 0 || *count > static_cast<std::int64_t>(engine::kMaxProbeReceivers)) {
                return fail(std::format("--detect-probes takes 0 to {} receivers",
                                        engine::kMaxProbeReceivers));
            }
            options.detect_probes = static_cast<std::uint32_t>(*count);
            options.detect = true;
            continue;
        }

        if (arg == "--detect-threshold" || arg == "--detect-confidence") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_real(*text, arg);
            if (!number) {
                return std::unexpected(number.error());
            }
            if (arg == "--detect-threshold") {
                if (!std::isfinite(*number) || *number < -60.0 || *number > 120.0) {
                    return fail("--detect-threshold takes a level between -60 and 120 dB");
                }
                options.detect_threshold_db = *number;
            } else {
                // The open upper end is the whole point of validating this
                // rather than clamping it. A track's confidence rises by a
                // fraction of its remaining distance to one, so it approaches
                // one and never arrives, and a bar of exactly one is a bar
                // nothing crosses. It fails as an empty list, which is also
                // what a dead band looks like: measured against the RTL-SDR
                // at 98.1 MHz, --detect-confidence 1 listed nothing at any of
                // the four intervals of a five second run in which 88 tracks
                // were born.
                if (!std::isfinite(*number) || *number < 0.0 || *number >= 1.0) {
                    return fail("--detect-confidence takes a value from 0 up to but not "
                                "including 1. A track's confidence approaches 1 without ever "
                                "reaching it, so a bar of 1 lists nothing however strong the "
                                "signal is");
                }
                options.detect_confidence = *number;
            }
            options.detect = true;
            continue;
        }

        if (arg == "--spectrum-floor" || arg == "--spectrum-ceiling") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_real(*text, arg);
            if (!number) {
                return std::unexpected(number.error());
            }
            if (!std::isfinite(*number)) {
                return fail(std::format("{} takes a level in dBFS", arg));
            }
            // Below the spectrum stage's own silence floor there is nothing
            // to see, and above full scale by more than the levels
            // histogram's range there is nothing to measure.
            if (*number < static_cast<double>(dsp::kSpectrumLevelsRangeFloorDb) ||
                *number > static_cast<double>(dsp::kSpectrumLevelsRangeCeilingDb)) {
                return fail(std::format("{} takes a level between {} and {} dBFS", arg,
                                        dsp::kSpectrumLevelsRangeFloorDb,
                                        dsp::kSpectrumLevelsRangeCeilingDb));
            }
            if (arg == "--spectrum-floor") {
                options.spectrum_floor_db = static_cast<float>(*number);
            } else {
                options.spectrum_ceiling_db = static_cast<float>(*number);
            }
            continue;
        }

        if (arg == "--play") {
            // The count is optional, so a bare --play must not swallow the
            // next option or a positional URI. Only a run of digits is taken.
            std::string text = "1";
            if (has_inline) {
                text = std::string(inline_value);
            } else if (i + 1 < argc) {
                const std::string_view peek{argv[i + 1]};
                const bool digits =
                    !peek.empty() &&
                    std::all_of(peek.begin(), peek.end(),
                                [](char c) { return c >= '0' && c <= '9'; });
                if (digits) {
                    text = std::string(peek);
                    ++i;
                }
            }
            auto which = parse_integer(text, "--play");
            if (!which) {
                return std::unexpected(which.error());
            }
            if (*which < 1) {
                return fail("--play counts receivers from 1");
            }
            options.play.push_back(static_cast<std::size_t>(*which));
            continue;
        }

        if (arg == "--vrx") {
            auto text = value_of(i, "--vrx", inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto spec = parse_vrx_spec(*text);
            if (!spec) {
                return std::unexpected(spec.error());
            }
            options.receivers.push_back(*spec);
            continue;
        }

        if (arg == "--rds") {
            auto text = value_of(i, "--rds", inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto spec = parse_rds_spec(*text);
            if (!spec) {
                return std::unexpected(spec.error());
            }
            options.rds.push_back(*spec);
            continue;
        }

        if (arg == "--decode") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            if (*text != "auto" && rpc::find_decoder(*text) == nullptr) {
                return fail(std::format("--decode '{}' names no decoder. There are {}, and auto "
                                        "picks the one named after each receiver's mode",
                                        *text, rpc::decoder_names()));
            }
            options.decode.push_back(*text);
            continue;
        }

        if (arg == "--rds-region") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            if (*text == "rds") {
                options.rds_region = decode::Region::kRds;
            } else if (*text == "rbds") {
                options.rds_region = decode::Region::kRbds;
            } else {
                return fail(std::format(
                    "--rds-region '{}' is neither rds nor rbds. rbds is North America and "
                    "is the default; rds is everywhere else",
                    *text));
            }
            continue;
        }

        if (arg == "--audio-device" || arg == "--record") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            if (arg == "--record") {
                options.record = *text;
            } else {
                options.audio_device = *text;
            }
            continue;
        }

        if (arg == "--record-format") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto format = engine::wav_format_from_name(*text);
            if (!format) {
                return std::unexpected(with_context(format.error(), "--record-format"));
            }
            options.record_format = *format;
            continue;
        }

        if (arg == "--pace") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_real(*text, arg);
            if (!number) {
                return std::unexpected(number.error());
            }
            if (*number < 0.0 || !std::isfinite(*number)) {
                return fail("--pace must be zero for unthrottled or a positive multiple of "
                            "realtime");
            }
            options.pace = *number;
            continue;
        }

        if (arg == "--realtime") {
            options.pace = 1.0;
            continue;
        }

        if (arg == "--volume" || arg == "--duration") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_real(*text, arg);
            if (!number) {
                return std::unexpected(number.error());
            }
            if (arg == "--volume") {
                if (*number < 0.0) {
                    return fail("--volume must not be negative");
                }
                options.volume = *number;
            } else {
                if (*number <= 0.0) {
                    return fail("--duration must be positive");
                }
                options.duration = *number;
            }
            continue;
        }

        if (arg == "--channels" || arg == "--gpu" || arg == "--status-ms") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_integer(*text, arg);
            if (!number) {
                return std::unexpected(number.error());
            }
            if (arg == "--channels") {
                // ZERO IS ACCEPTED AND MEANS "LET THE ENGINE CHOOSE", which is
                // what revenant-engine has always done with it and what this
                // program refused outright.
                //
                // It is not a convenience. Engine::open_source sizes the grid
                // from the source's own source::ResolutionRequest when the
                // caller named no count, and that is the only path to the fine
                // grid HF needs: 2 MS/s at 7.1 MHz opens on 256 channels and
                // 7.6 Hz bins, where the 64 below gives 30.5 Hz and a detector
                // that cannot place PSK31's centre inside PSK31. Naming a count
                // pins it, deliberately, so without a zero there was no way to
                // ask from here.
                //
                // The DEFAULT stays 64 on VHF, where docs/detection.md's
                // measurements were all taken at 64 channels over 2.4 MS/s.
                // Below 30 MHz it gives way to the engine's own choice unless
                // a count was given here; Options::channels_given.
                //
                // WHAT THIS PARAGRAPH USED TO SAY: "The DEFAULT stays 64.
                // docs/detection.md's measurements are all taken at 64
                // channels over 2.4 MS/s, and moving the default would
                // invalidate a table rather than add an option." True of VHF
                // still. On HF the pin cost tier two 57 of 126 answers.
                if (*number != 0 && (*number < 2 || *number > 65'536)) {
                    return fail("--channels is a power of two between 2 and 65536, or 0 to let "
                                "the engine size the grid from the source");
                }
                options.channels = static_cast<std::uint32_t>(*number);
                options.channels_given = true;
            } else if (arg == "--gpu") {
                if (*number < -1 || *number > 1'000) {
                    return fail("--gpu is a device index, or -1 to choose");
                }
                options.gpu = static_cast<int>(*number);
            } else {
                if (*number < 10) {
                    return fail("--status-ms must be at least 10");
                }
                options.status_ms = static_cast<long>(*number);
            }
            continue;
        }

        if (arg == "--audio-rate") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto rate = parse_frequency(*text, "--audio-rate");
            if (!rate) {
                return std::unexpected(rate.error());
            }
            if (*rate < 4'000) {
                return fail(std::format("--audio-rate {} is below anything audible", *rate));
            }
            options.audio_rate = *rate;
            continue;
        }

        if (arg.starts_with("-")) {
            return fail(std::format("unknown option '{}'. Try --help.", arg));
        }

        if (!options.uri.empty()) {
            return fail(std::format(
                "'{}' is a second source URI. One source per run; add receivers with --vrx.",
                arg));
        }
        options.uri = std::string(arg);
    }

    return options;
}

// ---------------------------------------------------------------------------
// Ctrl-C
// ---------------------------------------------------------------------------

// The handler runs on a thread the OS injects, so the engine is reached under
// a lock and nothing else is touched. Engine::stop() is safe from any thread:
// it sets a flag, cancels the graph and wakes run().
//
// THE LOCK IS NOT DECORATION. Unregistering a handler does not wait for one
// that is already running, so without it the injected thread can load the
// engine pointer, be descheduled while the main thread finishes its summary
// and destroys the engine, and then make a virtual call through a freed
// vtable. The window is however long the summary takes to print, which is
// ordinary scheduling latency for a thread that has just been created. The
// main thread takes the same lock to clear the pointer, so either the handler
// gets a live engine or it gets nothing.
std::mutex g_engine_lock;
engine::Engine* g_engine = nullptr;
std::atomic<bool> g_interrupted{false};

// Set whenever this process asked the engine to stop, by Ctrl-C or by
// --duration. The graph reports its own cancellation as a failed run, which is
// right for it and wrong here: a stop we asked for and got is a clean finish,
// and reporting it as an error would mean every timed recording exits 1.
std::atomic<bool> g_stop_requested{false};

// Signalled once the files are closed and the device is released.
//
// CTRL_C_EVENT and CTRL_BREAK_EVENT let the process keep running, so the
// handler returns and the main thread unwinds normally. The other three do
// not: Windows terminates the process the moment a close, logoff or shutdown
// handler returns. A handler that returned straight away there took the whole
// recording with it, because WavWriter only refreshes its size fields every
// megabyte and anything since the last refresh is not in the header. So those
// three block here until teardown says it is finished. Windows allows about
// five seconds for a close before killing the process anyway, and the wait is
// bounded below that: a recording finalised late is still better than one
// whose data chunk says zero.
HANDLE g_teardown_done = nullptr;

constexpr DWORD kTeardownWaitMs = 4000;

BOOL WINAPI console_handler(DWORD event)
{
    bool terminating = false;
    switch (event) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT: break;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT: terminating = true; break;
        default: return FALSE;
    }

    {
        const std::lock_guard<std::mutex> guard(g_engine_lock);
        if (g_engine == nullptr) {
            return FALSE;
        }
        g_interrupted.store(true, std::memory_order_release);
        g_stop_requested.store(true, std::memory_order_release);
        static_cast<void>(g_engine->stop());
    }

    if (terminating && g_teardown_done != nullptr) {
        static_cast<void>(WaitForSingleObject(g_teardown_done, kTeardownWaitMs));
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// The status line
// ---------------------------------------------------------------------------

[[nodiscard]] std::size_t console_width()
{
    CONSOLE_SCREEN_BUFFER_INFO info{};
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(out, &info) != 0) {
        const int width = info.srWindow.Right - info.srWindow.Left + 1;
        if (width > 20) {
            return static_cast<std::size_t>(width);
        }
    }
    // Redirected to a file or a pipe. One line is still one line.
    return 100;
}

// One terminal line that rewrites itself. Truncated rather than wrapped: a
// wrapped status line scrolls the placement block off the screen, which is
// the one thing the operator wants to keep looking at.
class StatusLine {
public:
    explicit StatusLine(bool enabled) : enabled_(enabled) {}

    void draw(const std::string& text)
    {
        if (!enabled_) {
            return;
        }
        const std::size_t width = console_width();
        std::string line = text;
        if (line.size() > width - 1) {
            line.resize(width - 1);
        }
        std::string painted = line;
        if (painted.size() < drawn_) {
            painted.append(drawn_ - painted.size(), ' ');
        }
        drawn_ = line.size();
        std::print("\r{}", painted);
        std::fflush(stdout);
    }

    void erase()
    {
        if (!enabled_ || drawn_ == 0) {
            return;
        }
        std::print("\r{}\r", std::string(drawn_, ' '));
        std::fflush(stdout);
        drawn_ = 0;
    }

private:
    bool enabled_;
    std::size_t drawn_ = 0;
};

// ---------------------------------------------------------------------------
// The ASCII waterfall
// ---------------------------------------------------------------------------
//
// One row per drawn interval, one character per column, oldest at the top.
//
// THREE THINGS THAT ARE NOT OBVIOUS FROM THE OUTPUT.
//
// The frame arrives on the engine's completion thread and the row is drawn on
// the status thread, and the two are joined by a lock-free ring rather than by
// a mutex. A sink that blocks blocks the thread that is bringing every
// receiver's audio home, so the rule in docs/conventions.md about no mutex in
// the sample path reaches this far out. A full ring drops the row and counts
// it rather than waiting.
//
// Frames arrive far faster than a terminal can be read: at 2.4 MS/s in 65536
// sample blocks that is thirty-six a second, where a row every few hundred
// milliseconds is what a person can follow. The rows in between are not
// discarded, they are peak-held into the next one, so a signal that keyed up
// for a tenth of a second still paints. A waterfall that sampled one frame in
// twenty would miss most of what a detector is for.
//
// The scale is the engine's. SpectrumFrame::floor_db and ceiling_db are the
// two ends docs/ui-spectrum.md specifies: percentiles measured on the device,
// smoothed with a fast attack and a thirty second decay, identical for every
// consumer of the frame. This display does not compute a scale of its own and
// two of its rows can be compared for absolute level, which the per-row
// version it replaces could not.
//
// WHAT IT DOES ADD, AND WHY IT IS NOT A SECOND SCALE
//
// One correction, to the floor only, for this display's own reduction.
//
// A column here covers hundreds of bins and is drawn as the largest of them,
// because a narrow carrier in one bin of seven hundred is invisible in a mean
// and is the whole point of looking. The largest of K samples is not
// distributed like one sample. On an empty band the bin powers are
// exponential, so the expected largest of K is the K-th harmonic number times
// the mean, while the frame's fifth percentile sits at -ln(0.95) times it.
// The gap between the two is 10*log10(H_K / -ln(1-p)), which at 720 bins per
// column is 21.4 dB.
//
// Ignore it and every column of an empty band draws above the ceiling and the
// display is a solid wall. It is a property of the reduction rather than of
// the signal, which is why it is here and not in the engine: a display
// drawing one bin per pixel has a different K and a GUI zoomed into a hundred
// kilohertz has another.
//
// It moves the floor and not the ceiling. A column containing a real signal
// draws that signal's own bin, and a maximum over K does not inflate a value
// that was already the largest, so the top of the map stays where the device
// put it.
//
// A pinned end takes no correction and is never moved, including by the
// minimum-span rule. A pin is an instruction in dBFS about where the map
// should end, not a measured percentile, so it is obeyed as written. Pin a
// ceiling below where the corrected floor lands and the floor is what gives
// way, because the alternative is drawing against a top the operator did not
// ask for while the header line says otherwise.
//
// What it assumes, stated because it is an assumption: that an empty column
// is noise. A frame in which every bin is literally identical, which is a
// silent source rather than a quiet band, has a column maximum equal to the
// bin value and the correction over-reports by the whole 21 dB, so the
// display draws nothing. Drawing nothing for a silent source is the right
// answer arrived at by the wrong route, and it is the only case where the
// two diverge: a disconnected antenna is thermal noise and the model holds.
class SpectrumView {
public:
    // Darkest to brightest. ASCII only, because this goes to a Windows
    // console that may be in any code page.
    static constexpr std::string_view kRamp = " .:-=+*#%@";

    // Width of the time column each row starts with, which the axis lines
    // have to match.
    static constexpr std::size_t kPrefix = 8;

    // A published row is the two ends of the colour map followed by one value
    // per column. They travel through the same ring as the row rather than
    // through a pair of atomics, so a row is drawn against the scale that
    // arrived with it.
    static constexpr std::size_t kRowHeader = 2;

    [[nodiscard]] static Expected<std::unique_ptr<SpectrumView>> create(
        const engine::SpectrumGeometry& geometry, Hertz source_center, std::size_t columns,
        bool floor_pinned, bool ceiling_pinned)
    {
        if (!geometry.enabled() || columns == 0) {
            return fail("the waterfall needs a spectrum geometry and at least one column");
        }

        std::unique_ptr<SpectrumView> view(new (std::nothrow) SpectrumView());
        if (view == nullptr) {
            return fail("could not allocate the waterfall");
        }

        const std::size_t stride = columns + kRowHeader;

        // Eight rows of headroom. The drawing thread polls every ten
        // milliseconds and frames arrive every twenty-seven at 2.4 MS/s, so
        // this only fills if the terminal itself stalls.
        auto ring = engine::SpscRing<float>::create(stride * 8);
        if (!ring) {
            return std::unexpected(with_context(ring.error(), "the waterfall's frame ring"));
        }

        view->geometry_ = geometry;
        view->source_center_ = source_center;
        view->columns_ = columns;
        view->stride_ = stride;
        view->floor_pinned_ = floor_pinned;
        view->ceiling_pinned_ = ceiling_pinned;
        view->ring_ = std::move(*ring);
        view->produced_.assign(stride, dsp::kSpectrumFloorDb);
        view->consumed_.assign(stride, dsp::kSpectrumFloorDb);
        view->held_.assign(columns, dsp::kSpectrumFloorDb);

        const std::size_t bins_per_column =
            std::max<std::size_t>(1, static_cast<std::size_t>(geometry.bins) / columns);
        view->reduction_headroom_db_ =
            floor_pinned ? 0.0F : peak_reduction_headroom_db(bins_per_column);
        return view;
    }

    // The engine's completion thread. Reduces a frame to one row and
    // publishes it. Never blocks, never allocates.
    [[nodiscard]] Status publish(const engine::SpectrumFrame& frame)
    {
        const std::size_t bins = frame.power_db.size();
        if (bins == 0) {
            return {};
        }

        produced_[0] = frame.floor_db;
        produced_[1] = frame.ceiling_db;

        // Peak within a column rather than a mean. A narrow carrier occupying
        // one bin of the seven hundred a column covers is invisible in the
        // mean and is the whole point of looking. The header comment says
        // what that costs the floor and how it is paid back.
        for (std::size_t c = 0; c < columns_; ++c) {
            const std::size_t begin = bins * c / columns_;
            std::size_t end = bins * (c + 1) / columns_;
            if (end <= begin) {
                end = begin + 1;
            }
            float peak = frame.power_db[begin];
            for (std::size_t i = begin + 1; i < end && i < bins; ++i) {
                peak = std::max(peak, frame.power_db[i]);
            }
            produced_[kRowHeader + c] = peak;
        }

        frames_.fetch_add(1, std::memory_order_relaxed);

        // Only the writer shrinks the free count, so a check here is still
        // true at the write below and the row goes in whole. A torn row would
        // shift every later row by a column.
        if (ring_->writable() < stride_) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        static_cast<void>(ring_->write(std::span<const float>(produced_)));
        return {};
    }

    // The drawing thread. Folds everything published since the last call into
    // the held row and says whether there was anything.
    [[nodiscard]] bool collect()
    {
        bool any = false;
        while (ring_->read(std::span<float>(consumed_)) == stride_) {
            // The latest frame's ends win rather than the oldest's. Several
            // frames are peak held into one row and the scale moved across
            // them, so the newest is the one the row is closest to.
            held_floor_db_ = consumed_[0];
            held_ceiling_db_ = consumed_[1];
            for (std::size_t c = 0; c < columns_; ++c) {
                held_[c] = std::max(held_[c], consumed_[kRowHeader + c]);
            }
            any = true;
        }
        return any;
    }

    // The drawing thread. Renders the held row and clears it.
    [[nodiscard]] std::string take_row(double seconds)
    {
        const auto [floor, top] = map_ends();
        const float span = top - floor;

        std::string row = std::format("{:6.2f}s ", seconds);
        row.reserve(kPrefix + columns_);
        for (std::size_t c = 0; c < columns_; ++c) {
            const float level = std::clamp((held_[c] - floor) / span, 0.0F, 1.0F);
            const auto step = static_cast<std::size_t>(
                std::lround(static_cast<double>(level) * static_cast<double>(kRamp.size() - 1)));
            row += kRamp[std::min(step, kRamp.size() - 1)];
        }

        std::fill(held_.begin(), held_.end(), dsp::kSpectrumFloorDb);
        rows_.fetch_add(1, std::memory_order_relaxed);
        return row;
    }

    // The two ends the next row will be drawn against, for the status line.
    // The same arithmetic take_row uses, so what is printed is what is drawn.
    [[nodiscard]] std::pair<float, float> map_ends() const
    {
        float floor = held_floor_db_ + reduction_headroom_db_;
        float top = held_ceiling_db_;

        // The engine already holds its own two ends apart, and the correction
        // above has just eaten into that gap, so the display re-applies the
        // same bound to what it actually draws against.
        //
        // It takes the span out of whichever end is not pinned. Moving a
        // pinned end is the one thing this must never do: a pin is the
        // operator saying where the map ends, and the feature it exists for
        // is comparing two captures, which needs the same map both times.
        // Restoring the span by raising a pinned ceiling put the drawn top
        // as much as 21.4 dB above the requested one while the header line
        // went on claiming the pin had been obeyed.
        if (top - floor < engine::kSpectrumMinimumSpanDb) {
            if (ceiling_pinned_ && !floor_pinned_) {
                // The operator has asked for a ceiling at or below where the
                // corrected floor lands, so the floor is what gives way.
                floor = top - engine::kSpectrumMinimumSpanDb;
            } else if (!ceiling_pinned_) {
                top = floor + engine::kSpectrumMinimumSpanDb;
            }
            // Both pinned: the operator has said exactly what they want,
            // including a narrow span, and gets it.
        }
        return {floor, top};
    }

    [[nodiscard]] float reduction_headroom_db() const { return reduction_headroom_db_; }

    // Where the ticks go and what they say. Two lines, aligned with a row.
    [[nodiscard]] std::pair<std::string, std::string> axis() const
    {
        constexpr std::size_t kTicks = 5;

        std::string labels(kPrefix, ' ');
        std::string ticks(kPrefix, ' ');
        labels.append(columns_, ' ');
        ticks.append(columns_, ' ');

        for (std::size_t t = 0; t < kTicks; ++t) {
            const std::size_t column = (columns_ - 1) * t / (kTicks - 1);
            ticks[kPrefix + column] = '|';

            const std::string text = std::format("{:.3f}", frequency_of(column) / 1e6);
            // Centred on the tick, then pulled back inside the line rather
            // than clipped, so the first and last labels stay readable.
            std::size_t start = kPrefix + column;
            start = (text.size() / 2 > start) ? 0 : start - text.size() / 2;
            start = std::min(start, kPrefix + columns_ - text.size());
            labels.replace(start, text.size(), text);
        }

        labels.append(" MHz");
        return {labels, ticks};
    }

    [[nodiscard]] Hertz low_edge() const { return static_cast<Hertz>(std::llround(edge(0))); }
    [[nodiscard]] Hertz high_edge() const
    {
        return static_cast<Hertz>(std::llround(edge(geometry_.bins)));
    }

    [[nodiscard]] std::uint64_t frames() const
    {
        return frames_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t dropped() const
    {
        return dropped_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t rows() const { return rows_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t columns() const { return columns_; }

private:
    SpectrumView() = default;

    // How far above the frame's low percentile a noise-only column draws,
    // when the column is the largest of `bins` bins.
    //
    // On an empty band a bin's power is the squared magnitude of complex
    // Gaussian noise, which is exponential. The expected largest of K
    // independent exponentials is the K-th harmonic number times their mean,
    // and the p-th percentile of one of them is -ln(1-p) times it, so the gap
    // between the two is the ratio of those, in decibels.
    //
    // H_K from the Euler expansion, which is better than a ten-thousandth
    // from K = 2 upwards. K = 1 is the exact answer rather than the limit,
    // and it is not a degenerate case: a display with one bin per column
    // still draws a value a mean above the fifth percentile.
    [[nodiscard]] static float peak_reduction_headroom_db(std::size_t bins)
    {
        constexpr double kEulerMascheroni = 0.577215664901532861;
        const double count = static_cast<double>(bins);
        const double harmonic =
            bins <= 1 ? 1.0 : std::log(count) + kEulerMascheroni + 0.5 / count;

        const double fraction =
            static_cast<double>(dsp::kSpectrumLowPermille) / 1000.0;
        const double percentile_of_mean = -std::log(1.0 - fraction);

        return static_cast<float>(10.0 * std::log10(harmonic / percentile_of_mean));
    }

    // Absolute frequency at the centre of a display column.
    [[nodiscard]] double frequency_of(std::size_t column) const
    {
        const double bin = (static_cast<double>(geometry_.bins) *
                            (static_cast<double>(column) + 0.5)) /
                           static_cast<double>(columns_);
        return static_cast<double>(source_center_) + geometry_.bin_zero_hz() +
               bin * geometry_.bin_width_hz();
    }

    [[nodiscard]] double edge(std::size_t bin) const
    {
        return static_cast<double>(source_center_) + geometry_.bin_zero_hz() +
               (static_cast<double>(bin) - 0.5) * geometry_.bin_width_hz();
    }

    engine::SpectrumGeometry geometry_{};
    Hertz source_center_ = 0;
    std::size_t columns_ = 0;
    std::size_t stride_ = 0;
    bool floor_pinned_ = false;
    bool ceiling_pinned_ = false;
    float reduction_headroom_db_ = 0.0F;

    std::unique_ptr<engine::SpscRing<float>> ring_;

    std::vector<float> produced_;  // completion thread only
    std::vector<float> consumed_;  // drawing thread only
    std::vector<float> held_;      // drawing thread only

    // Drawing thread only. The last published frame's ends, which is what the
    // next row is drawn against.
    float held_floor_db_ = dsp::kSpectrumFloorDb;
    float held_ceiling_db_ = dsp::kSpectrumFloorDb + engine::kSpectrumMinimumSpanDb;

    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> rows_{0};
};

// The wideband detector's live track list.
//
// Shaped like SpectrumView above and for the same reason. The detector runs
// where the frame arrives, on the engine's completion thread, and the table
// is printed by the thread that owns the terminal. A snapshot crosses between
// them through a ring rather than a lock, because the alternative is the
// display's scheduling deciding how long a spectrum callback takes.
//
// Snapshots are fixed-size blocks so a torn read is not expressible: a block
// is written whole or not at all, and the live count travels inside it, since
// track id zero is never issued.
//
// It also times the detector, which is the one number that decides whether
// this belongs on the host at all. core/detect/detector.h budgets it against
// 65536 bins at 305 frames a second; the summary prints what it actually
// cost, so the budget is checked rather than believed.
class DetectView {
public:
    static constexpr std::size_t kRows = 64;

    // No default member initialisers: SpscRing zero-fills its storage rather
    // than running constructors, so its element type has to be trivially
    // default constructible.
    struct Row {
        std::uint64_t id;
        double center_hz;
        double bandwidth_hz;
        double snr_db;
        double confidence;

        // How far this stood above the detection threshold, zero to one.
        //
        // BESIDE confidence AND NOT INSTEAD OF IT. They answer different
        // questions and the pair is the point: confidence counts consecutive
        // detections and reads no signal quality, so a column of it is nearly
        // constant and says how long each track has been there. This reads the
        // measurement and nothing about time. On 2026-09-20 at 95.1 MHz three
        // intermodulation products sat in this table at confidence 1.00 and
        // the column could not be used to sort them from the stations.
        //
        // It does not say a track is real either. A strong product stands well
        // above the noise and reads high, correctly. The front-end verdict
        // below is what speaks to that.
        double margin;

        // From detect::BandShape, straight off the track.
        //
        // concentration is the band's excess in its strongest three bins over
        // its excess in total, which is the same unit
        // characterise::spectral_concentration answers in, so the two tiers
        // can be read side by side.
        //
        // IT IS NOT peak_to_mean, WHICH WAS TRIED HERE FIRST. On 20 m at 1603
        // UT the narrow detections read peak_to_mean 2.23 to 5.34 and the wide
        // ones 250 and 405: it rises with how much of the band is EMPTY, so a
        // correctly sized narrow band scores like noise by construction. It
        // stays on BandShape, where its own header explains it; it is not a
        // column an operator can act on.
        //
        // balance is BandShape::lower_fraction: exactly 0.5 when the band is
        // symmetric about its own centre. It was MEANT to read near zero or
        // one for a sideband mode with a suppressed carrier and has never been
        // shown to; core/detect/shape.h has why the one measurement of it
        // looks like mirrored sub-bin placement rather than sideband
        // structure. Read it as a symmetry number and nothing more.
        double concentration;
        double balance;
        bool shape_measured;

        double age_seconds;
        double silent_seconds;
        std::uint32_t state;
        std::uint32_t channel;

        // Tier two, off the track: the family the detector reports with its
        // symbol rate and confidence, how many probes have answered, and the
        // most recent answer whether or not it was allowed to drive anything.
        // Families as detect::Classification's value, so the block stays
        // trivially copyable for the ring.
        std::uint32_t tier_family;
        double tier_confidence;
        double tier_symbol_rate;
        std::uint32_t tier_probes;
        std::uint32_t tier_last_family;
        double tier_last_confidence;
        double tier_last_symbol_rate;

        // The protocol a probe verified, as identify::Protocol's value, zero
        // for none.
        std::uint32_t tier_protocol;

        // Where the pool is with this track when no answer is on it yet:
        // kTierIdle, kTierProbing, or one plus the engine::ProbeStatus the
        // last probe ended with.
        std::uint32_t tier_status;

        // How many tracks cleared the confidence bar at the decision this
        // block came from, which is not the number of rows in it once more
        // clear it than a block can carry. Written identically into every
        // slot rather than sent beside the block, so the count and the rows
        // it describes are the same write and cannot disagree.
        std::uint32_t over_bar;

        // What core/detect/front_end.h said about the front end at that same
        // decision, carried the same way and for the same reason.
        //
        // IN THIS TABLE BECAUSE THIS IS WHERE THE PHANTOMS WERE SEEN.
        // Measured on air 2026-09-20 with revenant-cli --detect at 95.1 MHz:
        // three intermodulation products in this list at confidence 1.00,
        // indistinguishable from stations. The verdict is the only thing on
        // screen that can tell an operator the list is describing their own
        // receiver.
        std::uint32_t front_end;
        double front_end_slope;
        double front_end_lift_db;
    };

    // One member of one group from detect::LineGrouper, flattened so a
    // group is the run of consecutive rows sharing an id. The group's own
    // numbers are repeated on each of its rows for the same reason over_bar
    // is: one write, so they cannot disagree with the members they describe.
    static constexpr std::size_t kGroupRows = 128;
    struct GroupRow {
        std::uint64_t group;
        std::uint64_t track;
        double offset_hz;
        double anchor_hz;
        double span_hz;

        // The member's own age, which is what the table beside it prints.
        double age_seconds;

        // The group's: how long its current members have all been in it,
        // and how far apart their births were.
        double together_seconds;
        double births_seconds;

        std::uint32_t members;
        std::uint32_t state;
    };

    // What crosses the ring: the table and the groups of ONE decision. One
    // element rather than two rings, so a drawing thread cannot print one
    // decision's table under the next one's groups.
    struct Snapshot {
        std::array<Row, kRows> rows;
        std::array<GroupRow, kGroupRows> groups;

        // Groups at that decision, and how many were whole enough to fit
        // above. A group that does not fit is left out entirely rather than
        // cut, because a group with members missing is a different pattern.
        std::uint32_t groups_total;
        std::uint32_t groups_listed;
    };

    // group_gap is zero for no grouping, which is every run that did not ask
    // for --detect-groups.
    // probes is the engine whose pool tier two submits to, or null for no
    // tier two, which is every run with --detect-probes 0.
    [[nodiscard]] static Expected<std::unique_ptr<DetectView>> create(
        const detect::DetectorConfig& config, const engine::SpectrumGeometry& geometry,
        Hertz group_gap, engine::Engine* probes)
    {
        auto detector = detect::Detector::create(config, geometry);
        if (!detector) {
            return std::unexpected(with_context(detector.error(), "creating the detector"));
        }

        std::unique_ptr<DetectView> view(new (std::nothrow) DetectView());
        if (view == nullptr) {
            return fail("could not allocate the track list");
        }

        if (probes != nullptr) {
            auto tier_two =
                detect::TierTwo::create(detect::TierTwoConfig{.source_rate = config.source_rate});
            if (!tier_two) {
                return std::unexpected(with_context(tier_two.error(), "tier two"));
            }
            view->tier_two_ = std::move(*tier_two);
            view->probes_ = probes;
        }

        if (group_gap > 0) {
            auto grouper = detect::LineGrouper::create(detect::LineGroupConfig{.gap_hz = group_gap});
            if (!grouper) {
                return std::unexpected(with_context(grouper.error(), "--detect-groups"));
            }
            view->grouper_ = std::move(*grouper);
        }

        auto ring = engine::SpscRing<Snapshot>::create(8);
        if (!ring) {
            return std::unexpected(with_context(ring.error(), "the track list's snapshot ring"));
        }

        view->detector_ = std::move(*detector);
        view->ring_ = std::move(*ring);
        view->rate_ = config.source_rate;
        return view;
    }

    // The engine's completion thread.
    [[nodiscard]] Status publish(const engine::SpectrumFrame& frame)
    {
        const auto began = std::chrono::steady_clock::now();
        Status fed = detector_->consume(frame);
        const auto cost = std::chrono::steady_clock::now() - began;

        consume_ns_.fetch_add(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(cost).count()),
            std::memory_order_relaxed);
        frames_.fetch_add(1, std::memory_order_relaxed);
        if (!fed) {
            return fed;
        }

        // A frame that only fed the average has nothing new to say. Most
        // frames are that: the decision runs at a tenth of the frame rate.
        const dsp::SampleIndex decided = detector_->last_decision();
        if (decided == published_) {
            return {};
        }

        // Before published_ moves, because the interval is what the slope is
        // fitted against. The first decision has nothing behind it, so it
        // only sets the mark. A refusal is dropped: the monitor rejects
        // geometries the detector cannot produce, so there is no reachable
        // fault here that is not already a detector fault, and this must not
        // be what ends a run.
        if (published_ != 0 && rate_ > 0) {
            const double elapsed =
                static_cast<double>(decided - published_) / static_cast<double>(rate_);
            static_cast<void>(front_end_.observe(detector_->averaged_power(),
                                                 detector_->noise_floor(), elapsed));
        }

        published_ = decided;
        decisions_.fetch_add(1, std::memory_order_relaxed);
        const detect::FrontEndObservation front_end = front_end_.observation();

        // Tier two, here and nowhere else, because the pool wants one thread
        // submitting and taking and this is the thread that owns the tracks.
        // A refusal is counted rather than returned: a sink error ends the
        // run, and a full request ring is not a reason to stop listening.
        if (tier_two_.has_value()) {
            if (auto stepped = tier_two_->step(*detector_, *probes_); !stepped) {
                tier_two_errors_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // The bar is counted past the point the block fills up. A full block
        // is the common case on a broadcast band: the RTL-SDR at 98.1 MHz put
        // 64 rows, which is kRows exactly, in every table of a five second
        // run in which 86 tracks were born. A count that stopped at the cap
        // would report the cap as the answer and make two different
        // thresholds look the same. The loop is over at most max_tracks
        // entries, so counting the rest costs nothing worth measuring.
        const double confidence_bar = detector_->config().confidence_threshold;
        std::size_t used = 0;
        std::uint32_t over_bar = 0;
        for (const detect::Track& track : detector_->tracks()) {
            if (track.confidence < confidence_bar) {
                continue;
            }
            ++over_bar;
            if (used == kRows) {
                continue;
            }
            produced_.rows[used] = Row{
                .id = track.id,
                .center_hz = static_cast<double>(track.center),
                .bandwidth_hz = static_cast<double>(track.bandwidth),
                .snr_db = track.snr_2500_db,
                .confidence = track.confidence,
                .margin = track.margin_confidence,
                .concentration = track.shape.concentration,
                .balance = track.shape.lower_fraction,
                .shape_measured = track.shape.measured,
                .age_seconds = seconds_of(track.age_samples()),
                .silent_seconds = seconds_of(track.silent_samples()),
                .state = static_cast<std::uint32_t>(track.state),
                .channel = track.channel_valid ? track.channel : 0xFFFF'FFFFU,
                .tier_family = static_cast<std::uint32_t>(track.classification),
                .tier_confidence = track.classification_confidence,
                .tier_symbol_rate = track.symbol_rate_hz,
                .tier_probes = track.probes,
                .tier_last_family = static_cast<std::uint32_t>(track.last_probe.family),
                .tier_last_confidence = track.last_probe.confidence,
                .tier_last_symbol_rate = track.last_probe.symbol_rate_hz,
                .tier_protocol = static_cast<std::uint32_t>(track.protocol),
                .tier_status = tier_status(track.id),
            };
            ++used;
        }
        for (std::size_t i = used; i < kRows; ++i) {
            produced_.rows[i].id = 0;
        }
        // Every slot, including the zeroed tail, so slot zero carries the
        // count even at a decision where nothing cleared the bar and there
        // are no rows to hang it off.
        for (std::size_t i = 0; i < kRows; ++i) {
            produced_.rows[i].over_bar = over_bar;
            produced_.rows[i].front_end = static_cast<std::uint32_t>(front_end.verdict);
            produced_.rows[i].front_end_slope = front_end.slope;
            produced_.rows[i].front_end_lift_db = front_end.floor_lift_db;
        }

        publish_groups(decided);

        if (ring_->writable() < 1) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        static_cast<void>(ring_->write(std::span<const Snapshot>(&produced_, 1)));
        return {};
    }

    // The drawing thread. Takes the newest snapshot and discards any behind
    // it: a table is a state, not a history, so an older one is of no use.
    [[nodiscard]] bool collect()
    {
        bool any = false;
        while (ring_->read(std::span<Snapshot>(&consumed_, 1)) == 1) {
            held_ = consumed_;
            any = true;
        }
        return any;
    }

    [[nodiscard]] std::span<const Row> rows() const
    {
        std::size_t count = 0;
        while (count < kRows && held_.rows[count].id != 0) {
            ++count;
        }
        return std::span<const Row>(held_.rows.data(), count);
    }

    // The groups at the decision the held snapshot came from, one row per
    // member. Empty when grouping was not asked for.
    [[nodiscard]] std::span<const GroupRow> group_rows() const
    {
        std::size_t count = 0;
        while (count < kGroupRows && held_.groups[count].group != 0) {
            ++count;
        }
        return std::span<const GroupRow>(held_.groups.data(), count);
    }

    [[nodiscard]] std::uint32_t groups_total() const { return held_.groups_total; }
    [[nodiscard]] std::uint32_t groups_listed() const { return held_.groups_listed; }
    [[nodiscard]] bool grouping() const { return grouper_.has_value(); }

    // How many tracks cleared the confidence bar at the decision the held
    // snapshot came from. Never below rows().size() and above it whenever the
    // block filled, which is the only way an operator can tell a short list
    // from a truncated one.
    [[nodiscard]] std::uint32_t over_bar() const { return held_.rows[0].over_bar; }

    // What the front end monitor said at the decision the held snapshot came
    // from. Reconstructed from the block rather than read off the monitor,
    // because the monitor lives on the completion thread.
    [[nodiscard]] detect::FrontEndObservation front_end() const
    {
        detect::FrontEndObservation out;
        out.verdict = static_cast<detect::FrontEndVerdict>(held_.rows[0].front_end);
        out.slope = held_.rows[0].front_end_slope;
        out.floor_lift_db = held_.rows[0].front_end_lift_db;
        return out;
    }

    [[nodiscard]] const detect::DetectorStats& stats() const { return detector_->stats(); }

    static constexpr std::uint32_t kTierIdle = 0;
    static constexpr std::uint32_t kTierProbing = 0xFFFF'FFFFU;

    // Tier two's own counters, or null when it is off. Read after the engine
    // has stopped, the same as stats() above.
    [[nodiscard]] const detect::TierTwoStats* tier_two_stats() const
    {
        return tier_two_.has_value() ? &tier_two_->stats() : nullptr;
    }
    [[nodiscard]] bool tier_two() const { return tier_two_.has_value(); }
    [[nodiscard]] std::uint64_t tier_two_errors() const
    {
        return tier_two_errors_.load(std::memory_order_relaxed);
    }

    // The live tracks at the end of the run, with everything tier two left
    // on them. After the engine has stopped only.
    [[nodiscard]] std::span<const detect::Track> final_tracks() const
    {
        return detector_->tracks();
    }

    // Row::tier_status for one track. The completion thread while running,
    // any thread once the engine has stopped.
    [[nodiscard]] std::uint32_t tier_status(std::uint64_t track_id) const
    {
        if (!tier_two_.has_value()) {
            return kTierIdle;
        }
        if (tier_two_->probing(track_id)) {
            return kTierProbing;
        }
        const auto last = tier_two_->last_status(track_id);
        return last ? static_cast<std::uint32_t>(*last) + 1U : kTierIdle;
    }

    // Read after the engine has stopped, the same as stats() above.
    [[nodiscard]] const detect::LineGrouper* grouper() const
    {
        return grouper_.has_value() ? &*grouper_ : nullptr;
    }

    [[nodiscard]] std::uint64_t frames() const
    {
        return frames_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t decisions() const
    {
        return decisions_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t dropped() const
    {
        return dropped_.load(std::memory_order_relaxed);
    }

    // Mean wall time one frame cost the detector, in microseconds.
    [[nodiscard]] double microseconds_per_frame() const
    {
        const std::uint64_t seen = frames();
        if (seen == 0) {
            return 0.0;
        }
        return static_cast<double>(consume_ns_.load(std::memory_order_relaxed)) /
               (static_cast<double>(seen) * 1000.0);
    }

private:
    DetectView() = default;

    [[nodiscard]] double seconds_of(dsp::SampleIndex samples) const
    {
        return rate_ > 0 ? static_cast<double>(samples) / static_cast<double>(rate_) : 0.0;
    }

    // The completion thread. Feeds the grouper this decision's tracks and
    // flattens what it says into the snapshot, whole groups only.
    //
    // THE GROUPER SEES EVERY TRACK and not only those over the confidence
    // bar. The bar is the operator's filter on a display; which lines sit
    // together is a question about the band, and a line that has faded under
    // the bar while its neighbours hold is exactly the case a group exists to
    // follow. So a group can name a track the table above it does not list.
    void publish_groups(dsp::SampleIndex decided)
    {
        produced_.groups_total = 0;
        produced_.groups_listed = 0;
        std::size_t used = 0;
        if (grouper_.has_value() && grouper_->observe(detector_->tracks(), decided)) {
            const auto groups = grouper_->groups();
            produced_.groups_total = static_cast<std::uint32_t>(groups.size());
            for (const detect::LineGroup& group : groups) {
                const auto members = grouper_->members(group);
                if (used + members.size() > kGroupRows) {
                    continue;
                }
                for (const detect::GroupMember& member : members) {
                    produced_.groups[used++] = GroupRow{
                        .group = group.id,
                        .track = member.track,
                        .offset_hz = static_cast<double>(member.offset),
                        .anchor_hz = static_cast<double>(group.anchor_center),
                        .span_hz = static_cast<double>(group.span),
                        .age_seconds = seconds_of(decided - member.first_seen),
                        .together_seconds = seconds_of(decided - group.together_since),
                        .births_seconds = seconds_of(group.birth_spread),
                        .members = group.member_count,
                        .state = static_cast<std::uint32_t>(member.state),
                    };
                }
                ++produced_.groups_listed;
            }
        }
        for (std::size_t i = used; i < kGroupRows; ++i) {
            produced_.groups[i].group = 0;
        }
    }

    std::optional<detect::Detector> detector_;

    // Completion thread only, beside the detector whose arrays they read.
    detect::FrontEndMonitor front_end_;
    std::optional<detect::LineGrouper> grouper_;
    std::optional<detect::TierTwo> tier_two_;
    engine::Engine* probes_ = nullptr;
    std::atomic<std::uint64_t> tier_two_errors_{0};
    std::unique_ptr<engine::SpscRing<Snapshot>> ring_;
    SampleRate rate_ = 0;

    Snapshot produced_{};  // completion thread only
    Snapshot consumed_{};  // drawing thread only
    Snapshot held_{};      // drawing thread only

    dsp::SampleIndex published_ = 0;  // completion thread only

    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> decisions_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> consume_ns_{0};
};

// Collects one coarse channel of complex baseband off a raw receiver, so
// core/characterise has something to read.
//
// WHY THE RAW TAP AND NOT THE RECEIVER'S OWN BASEBAND. The fine stage already
// produces exactly what a characteriser wants, mixed to DC and filtered to the
// passband, and it produces it on the device where nothing copies it back.
// core/engine/graph.h says so at StageFineOutput. The raw tap is the only
// complex baseband that reaches the host at all, so it is what there is.
//
// WHAT THAT COSTS, AND WHY IT IS ACCEPTABLE HERE. The tap is one coarse
// channel: unmixed, so the signal sits at an arbitrary offset, and unfiltered
// beyond the channelizer's own prototype, so everything else in that channel
// comes too. On the shipped 2.4 MS/s VHF grid a channel is 37.5 kHz and that
// is most of a band away from what was asked about. On a 96 kS/s HF source it
// is 1.5 kHz, which is narrower than an SSB signal, and the estimators are
// then being asked a fair question.
//
// So this is a tool for slow sources. The help text says so rather than the
// flag refusing, because a wide channel gives a worse answer rather than a
// wrong one, and seeing it is how somebody learns the difference.
struct CharacteriseCollector {
    std::mutex lock;
    std::vector<dsp::Complex32> samples;
    dsp::SampleRate rate = 0;
    std::uint64_t seen = 0;

    // How many to keep. Set before the run from --characterise-seconds, or
    // left at twice the stage's floor.
    //
    // A KNOB AND NOT A CONSTANT, because the answer depends on it.
    // analysis_segment picks a transform length from the sample count, so a
    // longer extract gives finer bins, and spectral_concentration is three of
    // them. The same 40 m carrier reads 0.53 over eleven seconds and 0.18 over
    // sixty, because three bins is 4.4 Hz in the first case and 1.1 Hz in the
    // second and the carrier drifts further than that in a minute.
    std::size_t wanted = 2 * characterise::kMinCharacteriseSamples;

    void take(const engine::AudioChunk& chunk)
    {
        const std::lock_guard<std::mutex> held(lock);
        rate = chunk.rate;
        seen += chunk.samples.size() / 2;
        if (samples.size() >= wanted || chunk.channels != 2) {
            return;
        }

        // Interleaved I and Q, which is what the raw tap puts in an
        // AudioChunk. core/engine/engine.h still says a chunk is real and
        // never complex; core/engine/graph.cpp builds this one anyway, and a
        // consumer tells the two apart by the receiver's demodulator.
        for (std::size_t i = 0; i + 1 < chunk.samples.size(); i += 2) {
            samples.emplace_back(chunk.samples[i], chunk.samples[i + 1]);
            if (samples.size() >= wanted) {
                break;
            }
        }
    }
};

// One track, as a line. Frequencies to the hertz, because a detection an
// operator is about to click is a frequency they may have to type somewhere
// else.
// A tier-two family with what came with it: "psk 1200 Bd 0.99", "carrier
// 0.87". The symbol rate only when one was found, because a zero there would
// read as a measurement of zero.
[[nodiscard]] std::string family_text(std::uint32_t family, double symbol_rate_hz,
                                      double confidence)
{
    std::string text = detect::classification_name(static_cast<detect::Classification>(family));
    if (symbol_rate_hz > 0.0) {
        text += std::format(" {:.0f} Bd", symbol_rate_hz);
    }
    text += std::format(" {:.2f}", confidence);
    return text;
}

// The tier-two column. Three states that look alike in a lazier table and
// are not alike: "-" is a track no probe has reached yet, "unknown" is one a
// probe reached and could name nothing on, and a family in brackets is one
// the characteriser named and refused to let drive anything, which the
// detector kept and did not report. Before any answer, "probing" is one a
// receiver is collecting for now, and "too wide" is one wider than any probe
// this grid's channels can carry, which on the 3000 S/s channels of a 96 kS/s
// source over 64 is anything past 750 Hz.
[[nodiscard]] std::string tier_two_text(const DetectView::Row& row)
{
    if (row.tier_probes == 0) {
        if (row.tier_status == DetectView::kTierProbing) {
            return "probing";
        }
        if (row.tier_status != DetectView::kTierIdle) {
            return engine::probe_status_name(
                static_cast<engine::ProbeStatus>(row.tier_status - 1U));
        }
        return "-";
    }
    // A verified protocol leads, since it stands on a checked sync and the
    // family beside it does not.
    const std::string protocol =
        row.tier_protocol != 0
            ? std::string(identify::protocol_name(static_cast<identify::Protocol>(row.tier_protocol))) +
                  " "
            : std::string();
    if (row.tier_family != 0) {
        return protocol + family_text(row.tier_family, row.tier_symbol_rate, row.tier_confidence);
    }
    if (!protocol.empty()) {
        return protocol + "(family unknown)";
    }
    if (row.tier_last_family != 0) {
        return "(" +
               family_text(row.tier_last_family, row.tier_last_symbol_rate,
                           row.tier_last_confidence) +
               ", refused)";
    }
    return "unknown";
}

[[nodiscard]] std::string track_line(const DetectView::Row& row, bool tier_two)
{
    std::string channel = "  -";
    if (row.channel != 0xFFFF'FFFFU) {
        channel = std::format("{:3}", row.channel);
    }
    std::string held;
    if (row.silent_seconds > 0.0) {
        held = std::format("+{:.1f}s", row.silent_seconds);
    }
    const std::string concentration =
        row.shape_measured ? std::format("{:>5.2f}", row.concentration) : std::string("    -");
    const std::string balance =
        row.shape_measured ? std::format("{:>5.2f}", row.balance) : std::string("    -");
    return std::format(
        "  #{:<4} {:<7}{:>16}  {:>11}  {:>7.1f} dB  {:>5.2f}  {:>6.2f}  {}  {}  {:>6.1f}s  ch {}  "
        "{:<6}  {}",
        row.id, detect::track_state_name(static_cast<detect::TrackState>(row.state)),
        format_hz(static_cast<Hertz>(std::llround(row.center_hz))),
        format_hz(static_cast<Hertz>(std::llround(row.bandwidth_hz))), row.snr_db, row.confidence,
        row.margin, concentration, balance, row.age_seconds, channel, held,
        tier_two ? tier_two_text(row) : std::string{});
}

// The groups core/detect/groups.h is following, one line per group.
//
// WHY A GROUPING IS WORTH ANYTHING HERE. Five of the eight families in
// tests/detect/test_front_end.cpp are reported as separate spectral lines, one
// detection each, about five bins wide whatever the grid: an AM station is
// three rows in this table and a narrowband FM one is fifteen. Measured on the
// scene, the SET of lines is what tells them apart and no single row can:
// cw is one line, am is a carrier with a matched pair either side, usb and lsb
// are two lines 1200 Hz apart, nfm is a comb. tests/detect/test_groups.cpp
// pins that against the grouper this prints.
//
// IT DECIDES NOTHING. The grouper chains tracks whose centres sit within the
// operator's gap, gives the chain an id that survives between decisions, and
// this prints where each line sits relative to the strongest of them.
//
// WHAT THIS USED TO BE: a bracket drawn round rows of one snapshot, here in
// the display, with no identity from one table to the next. The HF corpus
// showed what that cost: two runs over the same file grouped differently,
// because the instant the table was sampled was not the same twice. The group
// id and the "together" age are what a snapshot could not carry.
[[nodiscard]] std::vector<std::string> track_groups(
    const std::span<const DetectView::GroupRow> rows)
{
    std::vector<std::string> out;
    std::size_t first = 0;
    while (first < rows.size()) {
        std::size_t last = first;
        while (last + 1 < rows.size() && rows[last + 1].group == rows[first].group) {
            ++last;
        }
        const DetectView::GroupRow& head = rows[first];

        // AGE BESIDE EVERY OFFSET, and the group's own two ages in front of
        // them, because those are what separate a structure from a
        // coincidence: lines one emitter produces are born together and stay
        // together, and an old carrier with a line a few seconds old beside it
        // is the track list moving under the reader.
        std::string offsets;
        for (std::size_t i = first; i <= last; ++i) {
            const DetectView::GroupRow& member = rows[i];
            // One decimal, which is what the table above uses. Rounding
            // to whole seconds here printed "0s" beside a row reading
            // "0.4s", and two numbers for one quantity disagreeing on the
            // same screen is the kind of thing a reader stops to check.
            const bool held = member.state == static_cast<std::uint32_t>(detect::TrackState::Held);
            // A plus on the lines above the anchor, because which side a line
            // sits is half of what a group says and a bare number reads as a
            // distance.
            const char* mark = member.offset_hz == 0.0 ? "*" : (member.offset_hz > 0.0 ? "+" : "");
            offsets += std::format(
                " {}{}({:.1f}s{})", mark,
                format_hz(static_cast<Hertz>(std::llround(member.offset_hz))),
                member.age_seconds, held ? " held" : "");
        }
        out.push_back(std::format(
            "  group #{}: {} lines across {}, from {}, together {:.1f}s, born {:.1f}s apart:{}",
            head.group, last - first + 1,
            format_hz(static_cast<Hertz>(std::llround(head.span_hz))),
            format_hz(static_cast<Hertz>(std::llround(head.anchor_hz))), head.together_seconds,
            head.births_seconds, offsets));
        first = last + 1;
    }
    return out;
}

// The column names, on the same widths track_line uses.
//
// A HEADER BECAUSE TWO OF THE COLUMNS ARE NOW BOTH A NUMBER BETWEEN ZERO AND
// ONE, and they mean opposite things: confidence is how long a track has been
// there and margin is how far it stood above the threshold. Unlabelled, the
// pair is unreadable, and guessing wrong is worse than either alone.
//
// Printed with each table rather than once at startup, because the table
// refreshes in place and a legend that scrolled away an hour ago is not a
// legend.
[[nodiscard]] std::string track_header(bool tier_two)
{
    return std::format(
        "  {:<5}{:<7}{:>16}  {:>11}  {:>10}  {:>5}  {:>6}  {:>6}  {:>5}  {:>7}  {:<6}  {:<6}  {}",
        "#id", "state", "centre", "bandwidth", "snr", "conf", "margin", "conc", "bal", "age", "ch",
        "held", tier_two ? "tier two" : "");
}

[[nodiscard]] std::string level_bar(double dbfs)
{
    constexpr int kCells = 8;
    constexpr double kFloorDb = -60.0;
    const double fraction = std::clamp((dbfs - kFloorDb) / -kFloorDb, 0.0, 1.0);
    const int filled = static_cast<int>(std::lround(fraction * kCells));
    std::string bar(static_cast<std::size_t>(kCells), ' ');
    for (int i = 0; i < filled; ++i) {
        bar[static_cast<std::size_t>(i)] = '#';
    }
    return bar;
}

// ---------------------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------------------

// Everything the run needs to know about one receiver, kept together so the
// summary does not have to reconstruct any of it.
struct Receiver {
    engine::VrxId id;
    engine::VrxId monitor;  // invalid when this receiver is not being played
    VrxSpec spec;
    SampleRate rate = 0;
    std::uint32_t channels = 1;
    std::string record_path;

    // Whether the egress has a path for this receiver. A receiver with no
    // --record and no --play has none, which --decode makes an ordinary case:
    // its output goes to a decoder and nowhere else, and asking the egress to
    // close a path it never opened fails the run on the way out.
    bool has_egress = false;
};

// Monitor ids come from above anything the graph issues, which allocates from
// 1 upwards. AudioEgress keys its slot table on the id alone and never asks
// the engine whether one exists, so a monitor is an ordinary egress stream
// that simply has no receiver behind it.
constexpr std::uint32_t kMonitorIdBase = 0x4000'0000U;

// ---------------------------------------------------------------------------
// One station's RDS decode
// ---------------------------------------------------------------------------

// PS, RadioText and PTYN are EN 50067 Annex E bytes. That repertoire is ASCII
// only over 0x20 to 0x7E; above that it is a set of accented, Greek and
// symbol characters whose numbering is the standard's own and matches neither
// Latin-1 nor UTF-8. Writing one of those bytes to the console prints
// whatever the active code page makes of it, which is a different character
// on a different machine, so every byte outside the shared subset is escaped
// here rather than passed through.
//
// 0x00 is not in Annex E at all. It is what StationState leaves where a
// RadioText segment has not arrived yet, so it is rendered as a tilde to keep
// the shape of the line, and a genuine tilde escapes as \x7e so the two can
// never be confused.
struct RdsText {
    std::string text;
    std::size_t escaped = 0;  // bytes outside printable ASCII, shown as \xNN
    std::size_t missing = 0;  // 0x00, a character no segment has delivered
};

[[nodiscard]] RdsText render_rds_text(std::string_view raw)
{
    RdsText out;
    out.text.reserve(raw.size());
    for (const char c : raw) {
        const auto byte = static_cast<std::uint8_t>(c);
        if (byte == 0x00) {
            out.text.push_back('~');
            ++out.missing;
        } else if (byte >= 0x20 && byte < 0x7E) {
            out.text.push_back(static_cast<char>(byte));
        } else {
            out.text += std::format("\\x{:02x}", byte);
            ++out.escaped;
        }
    }
    return out;
}

// Which segments of a segmented field have landed, one character each. PS is
// four two-character segments and RadioText is sixteen of four, and both
// arrive in whatever order the transmitter cycles them, so "which of them do
// I have" is the thing to show while a name is still filling in.
[[nodiscard]] std::string segment_mask(std::uint32_t received, int segments)
{
    std::string mask(static_cast<std::size_t>(segments), '.');
    for (int i = 0; i < segments; ++i) {
        if (((received >> i) & 1U) != 0U) {
            mask[static_cast<std::size_t>(i)] = '#';
        }
    }
    return mask;
}

[[nodiscard]] std::string_view sync_state_name(decode::SyncState state)
{
    switch (state) {
        case decode::SyncState::kHunting:
            return "hunting";
        case decode::SyncState::kPreSync:
            return "confirming";
        case decode::SyncState::kSynced:
            return "synced";
    }
    return "unknown";
}

// Everything the run needs to decode one station, kept together because the
// sink callable co-owns it.
//
// TWO THREADS. The engine's completion thread runs the sink and pushes
// samples in; the status thread reads the state out. Both decoders are
// ordinary mutable objects with no locking of their own, so everything below
// the mutex is taken under it. The fields above it are written before the
// engine runs and never again, so the sink and the printer read them free.
struct RdsStation {
    RdsStation(engine::VrxId which, SampleRate rate, decode::Region what,
               decode::RdsBitSync sync)
        : vrx(which), composite_rate(rate), region(what), bits(std::move(sync)), groups(what)
    {
    }

    engine::VrxId vrx;
    SampleRate composite_rate = 0;
    decode::Region region = decode::Region::kRbds;
    Hertz center = 0;

    // The receiver number the placement block printed, so "rds 3" and
    // "vrx 3" name the same receiver rather than each counting its own kind.
    std::size_t number = 0;

    std::mutex lock;
    decode::RdsBitSync bits;
    decode::RdsDecoder groups;

    // Set once and never cleared. A chunk that is not the shape the decoder
    // was built for stops the decode rather than failing the sink: a
    // refusing sink fails the dispatch and ends the run, and losing the
    // radio over a decoder is the wrong trade.
    std::string fault;
};

// What one print takes off a station, under its lock. Copied rather than
// referenced because the completion thread may be back inside the decoder
// the instant the lock is released, and StationState carries three vectors
// that would be reallocated under the reader.
struct RdsSnapshot {
    decode::StationState state;
    decode::RdsBitsStatus bits;
    decode::SyncState sync = decode::SyncState::kHunting;
    std::uint64_t groups_decoded = 0;
    std::uint64_t blocks_good = 0;
    std::uint64_t blocks_corrected = 0;
    std::uint64_t blocks_dropped = 0;
    std::uint64_t sync_losses = 0;
    std::string fault;
};

[[nodiscard]] RdsSnapshot snapshot_rds(RdsStation& station)
{
    const std::lock_guard<std::mutex> held(station.lock);
    RdsSnapshot snap;
    snap.state = station.groups.state();
    snap.bits = station.bits.status();
    snap.sync = station.groups.sync_state();
    snap.groups_decoded = station.groups.groups_decoded();
    snap.blocks_good = station.groups.blocks_good();
    snap.blocks_corrected = station.groups.blocks_corrected();
    snap.blocks_dropped = station.groups.blocks_dropped();
    snap.sync_losses = station.groups.sync_losses();
    snap.fault = station.fault;
    return snap;
}

// The engine's completion thread, with the station's lock already held.
void decode_rds_chunk(RdsStation& station, const engine::AudioChunk& chunk)
{
    if (!station.fault.empty()) {
        return;
    }

    // The two things a chunk can be that this decoder was not built for.
    // Neither is reachable from the command line as it stands, because --rds
    // builds its own receiver at a rate it chose and wfm is mono. They are
    // checked anyway: reading an interleaved pair as consecutive samples
    // decodes a signal that does not exist, and loops sized for one rate
    // running at another report a subcarrier offset that is an artefact of
    // the mismatch. Both would look like a weak station.
    if (chunk.channels != 1) {
        station.fault = std::format(
            "the receiver delivered {} interleaved channels and the decoder was built for a "
            "real mono composite",
            chunk.channels);
        return;
    }
    if (chunk.rate != station.composite_rate) {
        station.fault = std::format(
            "the receiver delivered audio at {} S/s and the decoder was built for {}",
            chunk.rate, station.composite_rate);
        return;
    }

    // A muted chunk is fed like any other. core/engine/graph.cpp writes zeros
    // into the readback buffer when the squelch is shut, and those zeros are
    // what the receiver produced: skipping them would take the composite
    // timeline out of step with the decoder's own sample count. A gated
    // receiver simply loses lock, which is the truth about what reached it.
    //
    // One std::function built per chunk and not per sample. It captures one
    // pointer, which MSVC's small-object buffer holds inline, so the
    // per-chunk cost is a construction and no allocation.
    station.bits.process(chunk.samples,
                         [&station](bool bit) { station.groups.feed(bit); });
}

// The block the status cadence prints, one per station.
//
// PS AND RADIOTEXT ARE PRINTED BEFORE THEY ARE COMPLETE, WHICH IS THE POINT.
// PS is eight characters in four segments and RadioText is up to 64 in
// sixteen, both cycled in whatever order the transmitter likes. Waiting for a
// complete field shows nothing at all for several seconds on PS and often
// tens of seconds on RadioText, which reads as a dead decoder. The blanks and
// the segment mask beside them say exactly how much has landed.
void print_rds(double source_seconds, const RdsStation& station, const RdsSnapshot& snap)
{
    const decode::StationState& state = snap.state;

    std::println("{:8.2f}s  rds {}  {}  {}", source_seconds, station.number,
                 format_hz(station.center),
                 station.region == decode::Region::kRbds ? "RBDS" : "RDS");

    if (!snap.fault.empty()) {
        std::println("  FAULTED         {}", snap.fault);
    }

    if (state.pi_valid) {
        const auto call = decode::callsign_from_pi(station.region, state.pi);
        std::println("  pi              0x{:04X}{}", state.pi,
                     call ? std::format("  {}", *call) : std::string{});
    } else {
        std::println("  pi              not yet received");
    }

    const RdsText ps = render_rds_text(state.ps_text());
    std::println("  ps              [{}]  segments {}", ps.text,
                 segment_mask(state.ps_received, 4));

    const RdsText rt = render_rds_text(state.rt_text());
    if (state.rt_length == 0) {
        std::println("  radiotext       not yet received");
    } else {
        std::println("  radiotext       [{}]", rt.text);
        std::println("                  {} of 64 characters, {}, segments {}",
                     state.rt_length, state.rt_version_b ? "type 2B" : "type 2A",
                     segment_mask(state.rt_received, 16));
    }

    const std::size_t escaped = ps.escaped + rt.escaped;
    if (escaped != 0) {
        std::println("                  {} byte{} above 0x7F shown as \\xNN: EN 50067 Annex E "
                     "is not ASCII above that and is not UTF-8 at all",
                     escaped, escaped == 1 ? "" : "s");
    }
    if (rt.missing != 0) {
        std::println("                  ~ is a character no RadioText segment has delivered "
                     "yet");
    }

    if (state.pty_valid) {
        // The prose name and not either display form. The 8- and
        // 16-character forms are padded with the standard's own underscores
        // for a fixed-width receiver display, which on a terminal line reads
        // as a typo: RBDS code 6 is "Classic_Rock" long and "Classic Rock" in
        // prose.
        std::println("  pty             {}  {}", state.pty,
                     decode::pty_entry(station.region, state.pty).name);
    } else {
        std::println("  pty             not yet received");
    }

    std::println("  flags           TP {}  TA {}  {}",
                 state.tp_valid ? (state.tp ? "yes" : "no") : "?",
                 state.ta_valid ? (state.ta ? "YES" : "no") : "?",
                 state.music_valid ? (state.music ? "music" : "speech") : "music/speech ?");

    // The physical layer, then the block layer. Two different failures look
    // the same from a distance: no carrier lock is the wrong frequency or a
    // passband that does not reach the subcarrier, and carrier lock with no
    // block sync is a station that carries no RDS.
    std::println("  carrier         {}, quality {:.2f}, coherence {:.2f}, offset {:+.1f} Hz, "
                 "{:.2f} bit/s, pilot {}",
                 decode::lock_name(snap.bits.lock), snap.bits.quality,
                 snap.bits.carrier_coherence, snap.bits.carrier_offset_hz,
                 snap.bits.bit_rate_hz, snap.bits.pilot_locked ? "locked" : "absent");

    const std::uint64_t blocks =
        snap.blocks_good + snap.blocks_corrected + snap.blocks_dropped;
    // CORRECTED BLOCKS COUNT AS ERRORS, which is the window's definition in
    // ui/models/rds_view.h and is now the only one in the tree.
    //
    // WHAT THIS USED TO COMPUTE. It was blocks_dropped over blocks, so a
    // corrected block counted as a clean one. That made two numbers with one
    // name: an operator comparing this line against the window's percentage
    // saw them disagree, with nothing anywhere saying which was which, and on
    // the 2026-09-20 KKFM capture the gap was 3.7 against 12.7.
    //
    // The window's is the one that survives, on the argument it already wrote
    // down and that core/rpc/types.h backs at blocks_corrected: a corrected
    // block had a burst repaired rather than being received clean and is
    // trusted less than a good one. What an operator wants from "block error
    // rate" is how much of the bitstream needed help, and counting only
    // dropped blocks reports a fading station with a working error corrector
    // as perfect.
    //
    // The three counts are printed beside it either way, so nothing is lost:
    // a reader who wants the drop-only figure can still see it.
    const double bler = blocks == 0 ? 0.0
                                    : 100.0 *
                                          static_cast<double>(snap.blocks_corrected +
                                                              snap.blocks_dropped) /
                                          static_cast<double>(blocks);
    std::println("  blocks          {}, {} group{}, {} block{}: {} clean, {} corrected, "
                 "{} dropped ({:.1f}% BLER), {} resync{}",
                 sync_state_name(snap.sync), snap.groups_decoded,
                 snap.groups_decoded == 1 ? "" : "s", blocks, blocks == 1 ? "" : "s",
                 snap.blocks_good, snap.blocks_corrected, snap.blocks_dropped, bler,
                 snap.sync_losses, snap.sync_losses == 1 ? "" : "s");
}

// ---------------------------------------------------------------------------
// Event decoders on --vrx receivers
// ---------------------------------------------------------------------------

// Messages one decoder may hold between two status intervals before it drops
// the oldest. The printing thread drains them every interval, 500 ms by
// default, and P25 produces a data unit every 180 ms during a call, so this is
// minutes of margin; the count of any dropped is printed rather than hidden.
constexpr std::size_t kDecodePending = 1'024;

// One decoder from core/rpc/decoders.h attached to one receiver.
//
// The sink runs on the engine's completion thread and the printing happens on
// the main thread, which is the arrangement RdsStation has and for its reason:
// a terminal write inside the sink would put console latency in front of
// every other consumer of the completion thread. The sink decodes and queues;
// the main thread prints.
struct DecodeTap {
    std::size_t number = 0;
    const rpc::DecoderSpec* spec = nullptr;

    // The receiver's demodulator, engine::demod_name's spelling, for the
    // decoders whose polarity the sideband decides.
    std::string_view mode;

    std::mutex lock;
    std::unique_ptr<rpc::ChunkDecoder> decoder;
    std::vector<rpc::DecodedMessage> pending;
    std::uint64_t produced = 0;
    std::uint64_t dropped = 0;
    std::uint32_t rate = 0;

    // Why the decoder stopped, empty while it has not. Terminal, on the
    // argument core/rpc/decoders.h makes for every adapter refusal.
    std::string fault;
    bool fault_printed = false;
};

// tap.lock held. Numbers what a decoder produced and queues it for printing.
void queue_decoded(DecodeTap& tap, std::vector<rpc::DecodedMessage>& recovered)
{
    for (rpc::DecodedMessage& message : recovered) {
        message.sequence = tap.produced++;
        if (tap.pending.size() >= kDecodePending) {
            tap.pending.erase(tap.pending.begin());
            ++tap.dropped;
        }
        tap.pending.push_back(std::move(message));
    }
}

// Engine completion thread, with tap.lock held.
void decode_tap_chunk(DecodeTap& tap, const engine::AudioChunk& chunk)
{
    if (!tap.fault.empty()) {
        return;
    }
    const rpc::DecoderChunk in{
        .samples = chunk.samples,
        .channels = chunk.channels,
        .rate = chunk.rate,
        .start = chunk.start,
    };
    if (in.frames() == 0) {
        return;
    }

    // Built at the rate the first chunk carries rather than at a rate taken
    // from the placement, because a digital voice receiver's fine stage and a
    // raw tap deliver at different rates and only the chunk says which.
    if (tap.decoder == nullptr) {
        auto made = tap.spec->make(rpc::DecoderBuild{.rate = chunk.rate, .mode = tap.mode});
        if (!made) {
            tap.fault = made.error().message;
            return;
        }
        tap.decoder = std::move(*made);
        tap.rate = static_cast<std::uint32_t>(chunk.rate);
    }

    std::vector<rpc::DecodedMessage> recovered;
    if (auto consumed = tap.decoder->consume(in, recovered); !consumed) {
        tap.fault = consumed.error().message;
        return;
    }
    queue_decoded(tap, recovered);
}

// Main thread, once the engine has stopped: the end of the stream, which a
// decoder holding a message open needs to be told. Not after a fault.
void flush_decode_tap(DecodeTap& tap)
{
    const std::lock_guard<std::mutex> held(tap.lock);
    if (tap.decoder == nullptr || !tap.fault.empty()) {
        return;
    }
    std::vector<rpc::DecodedMessage> recovered;
    tap.decoder->flush(recovered);
    queue_decoded(tap, recovered);
}

// Main thread. Prints and clears what the decoder queued.
//
// The time is the message's own, where its receiver's stream says it
// completed: DecodedMessage::end_sample over its rate. That stream starts with
// the receiver, which on this command line is when the run starts, so it reads
// as time into the run and is exact rather than the interval it was printed
// in. Returns whether the decoder has faulted, so the summary can say so.
bool print_decoded(DecodeTap& tap)
{
    std::vector<rpc::DecodedMessage> ready;
    std::string fault;
    bool newly_faulted = false;
    {
        const std::lock_guard<std::mutex> held(tap.lock);
        ready.swap(tap.pending);
        fault = tap.fault;
        if (!tap.fault.empty() && !tap.fault_printed) {
            tap.fault_printed = true;
            newly_faulted = true;
        }
    }

    for (const rpc::DecodedMessage& message : ready) {
        const double seconds =
            message.sample_rate == 0
                ? 0.0
                : static_cast<double>(message.end_sample) / static_cast<double>(message.sample_rate);
        std::println("{:8.2f}s  vrx {}  {} {}  {}", seconds, tap.number, message.decoder,
                     message.kind, message.text);
    }
    if (newly_faulted) {
        std::println("          vrx {}  {} STOPPED: {}", tap.number, tap.spec->name, fault);
    }
    return !fault.empty();
}

[[nodiscard]] std::string iso8601_now()
{
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
}

void print_source(const source::SourceCapabilities& caps)
{
    std::println("source    {}", caps.uri);
    std::println("  backend         {}{}", caps.backend,
                 caps.display_name.empty() ? std::string{}
                                           : std::format("  ({})", caps.display_name));
    std::println("  sample rate     {} S/s", caps.min_rate == caps.max_rate
                                                 ? std::to_string(caps.max_rate)
                                                 : std::format("{} to {}", caps.min_rate,
                                                               caps.max_rate));
    std::println("  format          {}, {} bits per component, {} bytes per sample",
                 source::format_name(caps.native_format), caps.bits_per_component,
                 source::bytes_per_sample(caps.native_format));
    std::println("  flow control    {}", flow_explained(caps.flow));

    std::string clocks;
    for (const source::ClockSource clock : caps.clock_sources) {
        if (!clocks.empty()) {
            clocks += ", ";
        }
        clocks += clock_source_name(clock);
    }
    if (clocks.empty()) {
        clocks = "none declared";
    }
    // The live discipline residual lives on the Source and the engine owns
    // that, so what a caller can see is what the source said it can do.
    std::println("  clock           {}, sample zero placed to +/- {} ns", clocks,
                 caps.timestamp_accuracy_ns);

    if (caps.length_samples > 0) {
        const double seconds = caps.max_rate > 0 ? static_cast<double>(caps.length_samples) /
                                                       static_cast<double>(caps.max_rate)
                                                 : 0.0;
        std::println("  length          {} samples, {:.3f} s", caps.length_samples, seconds);
    } else {
        std::println("  length          unbounded");
    }
    std::println("  seekable        {}", caps.seekable ? "yes" : "no");
}

void print_grid(const engine::EngineInfo& info)
{
    std::println("");
    std::println("gpu       {}", info.device.describe());
    std::println("  ring            {:.3f} s, {} samples, {}{}", info.ring.seconds_retained,
                 info.ring.capacity_samples, format_bytes(info.ring.bytes),
                 info.ring.clamped ? std::format("  (clamped: {})", info.ring.clamp_reason)
                                   : std::string{});
    std::println("");
    std::println("grid      M {}  D {}  {} taps per branch", info.grid.channels,
                 info.grid.decimation, info.grid.taps_per_branch);
    std::println("  channel rate    {} S/s", info.channel_rate);
    std::println("  channel spacing {} ({} Hz)", format_hz(info.channel_spacing),
                 info.channel_spacing);
    std::println("  source rate     {} S/s", info.source_rate);
}

void print_placement(std::size_t number, const engine::VrxStatus& status,
                     std::uint32_t grid_channels, Hertz absolute)
{
    const engine::VrxPlacement& placement = status.placement;
    const double residual = placement.residual_denominator == 0
                                ? 0.0
                                : static_cast<double>(placement.residual_numerator) /
                                      static_cast<double>(placement.residual_denominator);

    // The absolute frequency first, because that is the number the operator
    // typed and the one on the repeater list. The baseband offset follows it
    // only when the two differ, which is when the source declares a centre.
    //
    // The passband is printed as its two edges and its width, because a
    // width alone cannot tell a USB filter on the carrier apart from one 300
    // hertz above it and those are different receivers.
    const std::int64_t granted_width = placement.granted_high - placement.granted_low;
    std::println("vrx {}     {} {}  passband {:+} to {:+} Hz ({}){}", number,
                 format_hz(absolute), engine::demod_name(status.params.demod),
                 placement.granted_low, placement.granted_high, format_hz(granted_width),
                 placement.bandwidth_clamped
                     ? "  CLAMPED: wider than one grid channel can carry"
                     : "");
    if (placement.bandwidth_clamped) {
        std::println("  asked for       {:+} to {:+} Hz", status.params.passband_low,
                     status.params.passband_high);
    }
    std::println("  demod rate      {} S/s", status.demod_rate);
    if (absolute != status.params.center) {
        std::println("  baseband        {:+} Hz from the source's centre",
                     status.params.center);
    }
    std::println("  channel         {} of {}, centre {:.3f} Hz{}", placement.channel,
                 grid_channels, placement.channel_centre.hertz(),
                 placement.channel_centre.is_integral()
                     ? ""
                     : "  (not a whole hertz, carried as a rational)");
    std::println("  residual        {:+.3f} Hz mixed out by the fine stage", residual);
    std::println("  channel rate    {} S/s", placement.channel_rate);
}

// Where one receiver's recording goes. A single receiver names a file; more
// than one names a directory, because two receivers cannot share a WAV.
[[nodiscard]] Expected<std::string> record_path_for(const Options& options, std::size_t number,
                                                    const VrxSpec& spec)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    const fs::path given = fs::path(options.record);
    const bool as_directory = options.receivers.size() > 1 || fs::is_directory(given, ec);

    fs::path path;
    if (as_directory) {
        fs::create_directories(given, ec);
        if (ec) {
            return fail(std::format("could not create the recording directory '{}': {}",
                                    options.record, ec.message()),
                        ec.value());
        }
        path = given / std::format("vrx{}-{}-{}.wav", number, spec.center,
                                   engine::demod_name(spec.demod));
    } else {
        path = given;
        const fs::path parent = path.parent_path();
        if (!parent.empty()) {
            fs::create_directories(parent, ec);
            if (ec) {
                return fail(std::format("could not create '{}': {}", parent.string(),
                                        ec.message()),
                            ec.value());
            }
        }
    }

    return path.string();
}

// ---------------------------------------------------------------------------
// The listings
// ---------------------------------------------------------------------------

[[nodiscard]] Status list_sources()
{
    auto described = source::describe_sources();
    if (!described) {
        return std::unexpected(with_context(described.error(), "listing sources"));
    }

    if (described->empty()) {
        std::println("no sources are attached or constructible");
        return {};
    }

    for (const source::SourceCapabilities& caps : *described) {
        std::println("{}", caps.uri);
        std::println("  {}  ({} backend)", caps.display_name, caps.backend);

        // A device that is attached but could not be opened still belongs in
        // the list. Saying why beats leaving it out, because "my dongle is
        // missing" and "my dongle is busy" send a person looking in very
        // different places.
        if (!caps.available()) {
            std::println("  UNAVAILABLE     {}", caps.unavailable);
            std::println("");
            continue;
        }

        std::println("  {} to {} S/s, {}, {}", caps.min_rate, caps.max_rate,
                     source::format_name(caps.native_format), flow_name(caps.flow));
        std::println("");
    }

    std::println("A file source is named rather than discovered, so it never appears here:");
    std::println("  file:///C:/captures/hf.cf32?rate=2400000&format=cf32&center=7100000");
    return {};
}

[[nodiscard]] Status list_audio_devices()
{
    auto devices = engine::enumerate_audio_devices();
    if (!devices) {
        return std::unexpected(with_context(devices.error(), "listing audio render endpoints"));
    }

    if (devices->empty()) {
        std::println("no audio render endpoints are active");
        return {};
    }

    for (const engine::AudioDeviceInfo& device : *devices) {
        std::println("{}{}", device.name, device.is_default ? "  (default)" : "");
        std::println("  id      {}", device.id);
        std::println("  mixing  {} S/s, {} channels", device.mix_rate, device.mix_channels);
        std::println("");
    }
    std::println("Pass an id to --audio-device.");
    return {};
}

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

[[nodiscard]] Status run_receivers(const Options& options)
{
    const std::string created = iso8601_now();

    engine::EngineConfig config;
    config.gpu_index = options.gpu;
    config.channels = options.channels;
    config.channels_yield_to_source = !options.channels_given;
    config.audio_rate = options.audio_rate;

    // Chosen before the engine is created, because the spectrum stage is part
    // of the coarse chain and is built once when the source is opened.
    // --detect needs the same stage, so it turns it on rather than failing
    // later with a message about a flag the operator did not pass.
    if (options.spectrum || options.detect) {
        config.spectrum_transform = options.spectrum_points != 0 ? options.spectrum_points
                                                                 : dsp::kDefaultSpectrumTransform;
        config.spectrum_floor_db = options.spectrum_floor_db;
        config.spectrum_ceiling_db = options.spectrum_ceiling_db;
    }

    // A loudspeaker is the one consumer that cannot take audio faster than it
    // can play it. A Demand source with nothing holding a stopwatch delivers
    // as fast as the GPU retires work, so a monitor fed from one hears its
    // own backlog being trimmed rather than the signal. --play therefore
    // asks for realtime unless the operator asked for something else, and a
    // Paced source ignores the request because its hardware already sets the
    // rate.
    config.pace = options.pace >= 0.0 ? options.pace : (options.play.empty() ? 0.0 : 1.0);

    // Before the engine is created, because the pool is built with the graph.
    config.probe_receivers = options.detect ? options.detect_probes : 0;

    auto made = engine::Engine::create(config);
    if (!made) {
        return std::unexpected(with_context(made.error(), "creating the engine"));
    }
    engine::Engine& eng = **made;

    if (auto opened = eng.open_source(options.uri); !opened) {
        return std::unexpected(
            with_context(opened.error(), std::format("opening source '{}'", options.uri)));
    }

    print_source(eng.source_capabilities());
    print_grid(eng.info());

    // The waterfall, built before the receivers so its geometry is printed
    // with the rest of what the engine settled on rather than after a page of
    // placements.
    std::unique_ptr<SpectrumView> waterfall;
    if (options.spectrum) {
        const engine::SpectrumGeometry& geometry = eng.info().spectrum;
        if (!geometry.enabled()) {
            return fail("--spectrum was asked for and the engine built no spectrum stage");
        }

        const std::size_t width = console_width();
        const std::size_t columns =
            std::clamp(width - SpectrumView::kPrefix - 1, std::size_t{16}, std::size_t{512});

        auto view = SpectrumView::create(geometry, eng.info().source_center, columns,
                                         options.spectrum_floor_db.has_value(),
                                         options.spectrum_ceiling_db.has_value());
        if (!view) {
            return std::unexpected(view.error());
        }
        waterfall = std::move(*view);

        std::println("");
        std::println("spectrum  {} bins of {:.3f} Hz, {} points per channel",
                     geometry.bins, geometry.bin_width_hz(), geometry.transform);
        std::println("  span            {} to {}", format_hz(waterfall->low_edge()),
                     format_hz(waterfall->high_edge()));
        std::println("  display         {} columns, one row every {} ms, peak held between rows",
                     waterfall->columns(), options.status_ms);
        std::println("  scale           floor {}, ceiling {}",
                     options.spectrum_floor_db
                         ? std::format("pinned at {:.1f} dBFS", *options.spectrum_floor_db)
                         : std::format("automatic, {:.1f} dB above the 5th percentile for a "
                                       "{}-bin column peak",
                                       waterfall->reduction_headroom_db(),
                                       geometry.bins / waterfall->columns()),
                     options.spectrum_ceiling_db
                         ? std::format("pinned at {:.1f} dBFS", *options.spectrum_ceiling_db)
                         : std::string{"automatic, the 99th percentile"});
        if (!options.spectrum_floor_db || !options.spectrum_ceiling_db) {
            std::println("                  automatic ends expand in a frame or two and "
                         "contract over 30 s");
        }
    }

    // The detector, on the same frames the waterfall draws.
    std::unique_ptr<DetectView> detector;
    if (options.detect) {
        const engine::SpectrumGeometry& geometry = eng.info().spectrum;
        if (!geometry.enabled()) {
            return fail("--detect was asked for and the engine built no spectrum stage");
        }

        detect::DetectorConfig detect_config;
        detect_config.source_rate = eng.info().source_rate;
        detect_config.source_center = eng.info().source_center;
        detect_config.grid_channels = eng.info().grid.channels;
        detect_config.detection_threshold_db = options.detect_threshold_db;
        detect_config.confidence_threshold = options.detect_confidence;
        if (options.detect_split_gap != 0) {
            detect_config.split_gap_bins = options.detect_split_gap;
        }
        if (options.detect_occupied != 0.0) {
            detect_config.occupied_power_fraction = options.detect_occupied;
        }

        auto view = DetectView::create(detect_config, geometry, options.detect_groups,
                                       options.detect_probes > 0 ? &eng : nullptr);
        if (!view) {
            return std::unexpected(view.error());
        }
        detector = std::move(*view);

        std::println("");
        // The confidence bar is printed at whatever precision it was given
        // rather than to two places, because a bar of 0.995 is legal, a bar
        // of 1 is refused at parse time, and rounding the first to "1.00"
        // shows the operator the one value they cannot have asked for.
        std::println("detector  threshold {:.1f} dB SNR in {:.0f} Hz, confidence {:g}",
                     detect_config.detection_threshold_db, detect::kReferenceBandwidthHz,
                     detect_config.confidence_threshold);
        std::println("  integration     {:.2f} s, deciding every {:.0f} ms, {} consecutive "
                     "decisions to be born",
                     detect_config.average_seconds,
                     detect_config.decision_interval_seconds * 1000.0, detect_config.birth_hits);
        std::println("  hold            {:.1f} s with no evidence, confidence halving every "
                     "{:.1f} s",
                     detect_config.bootstrap_hold_seconds,
                     detect_config.confidence_half_life_seconds);
        std::println("  first answer    after {:.2f} s of source time, which is one integration; "
                     "a threshold calibrated against an average cannot be applied before there "
                     "is one",
                     detect_config.average_seconds);
        if (options.detect_probes == 0) {
            std::println("  tier two        off, --detect-probes 0");
        } else {
            const auto floor = engine::probe_shape(1, eng.info().channel_rate);
            std::println("  tier two        {} probe receivers, oldest unclassified live track "
                         "first; {}",
                         options.detect_probes,
                         floor ? std::format(
                                     "{:.1f} s of baseband at {} S/s or more per probe, {:.1f} s "
                                     "for a detection under {} Hz so a protocol can be identified",
                                     static_cast<double>(floor->characterise_samples) /
                                         static_cast<double>(floor->rate),
                                     floor->rate, floor->seconds, engine::kProbeIdentifyNarrowHz)
                               : floor.error().message);
            if (eng.source_pacing().paced_by == 0.0 &&
                eng.source_capabilities().flow != source::FlowControl::Paced) {
                std::println("                  this source is unthrottled and will outrun the "
                             "probes; --pace 1 or --realtime lets them keep up");
            }
        }
    }

    std::println("");

    // Receivers, then their placements, before anything is recorded. A
    // receiver on the wrong channel is indistinguishable from a dead band
    // once the audio starts, and this is the last moment it is visible.
    std::vector<Receiver> receivers;
    receivers.reserve(options.receivers.size());
    for (std::size_t i = 0; i < options.receivers.size(); ++i) {
        VrxSpec spec = options.receivers[i];

        // VrxParams is in the source's baseband frame, because that is the
        // only frame the grid has. An absolute frequency has to be resolved
        // against where the source is tuned, and a source that declares no
        // centre puts the two in the same place.
        const Hertz source_center = eng.info().source_center;
        if (spec.relative) {
            spec.baseband = spec.center;
            // Normalised to absolute here and not later, so the filename, the
            // WAV metadata and the status line all name the frequency a
            // person would tune a second radio to, whichever way it was
            // typed.
            spec.center = source_center + spec.baseband;
        } else {
            spec.baseband = spec.center - source_center;
        }

        const Hertz reach = eng.info().source_rate / 2;
        if (spec.baseband > reach || spec.baseband < -reach) {
            return fail(std::format(
                "receiver {} at {} is {} from the source's centre of {}, and the source only "
                "carries +/-{}. Either retune the source or name an offset with a leading "
                "sign, such as +150k",
                i + 1, format_hz(spec.center), format_hz(spec.baseband),
                format_hz(source_center), format_hz(reach)));
        }

        engine::VrxParams params;
        params.center = spec.baseband;
        params.passband_low = spec.passband.low;
        params.passband_high = spec.passband.high;
        params.bandwidth = spec.passband.width();
        params.demod = spec.demod;
        params.audio_rate = options.audio_rate;

        auto added = eng.add_vrx(params);
        if (!added) {
            return std::unexpected(with_context(
                added.error(), std::format("adding receiver {} at {}", i + 1,
                                           format_hz(spec.center))));
        }

        auto status = eng.vrx_status(*added);
        if (!status) {
            return std::unexpected(with_context(
                status.error(), std::format("reading back receiver {}", i + 1)));
        }
        print_placement(i + 1, *status, eng.info().grid.channels, spec.center);

        Receiver receiver;
        receiver.id = *added;
        receiver.spec = spec;
        // The raw tap is not a demodulator: it hands back one grid channel as
        // interleaved complex at the channel rate, so its stream is two
        // channels and is not at the audio rate. The three digital voice
        // modes are two channels of complex too, but out of the fine stage,
        // at the rate their decoder wants, which VrxStatus::demod_rate
        // carries. Everything else is mono audio.
        //
        // WHAT THIS USED TO SAY: "The three digital voice modes are taps on
        // the same terms", and it took their rate from the placement. They
        // stopped being raw taps on 2026-09-22, and a recording labelled at
        // the channel rate would play their 48000 or 72000 at the wrong speed.
        if (spec.demod == Demod::Raw) {
            receiver.rate = status->placement.channel_rate;
            receiver.channels = 2;
        } else if (is_complex_tap(spec.demod)) {
            receiver.rate = status->demod_rate;
            receiver.channels = 2;
        } else {
            receiver.rate = options.audio_rate;
            receiver.channels = 1;
        }
        receivers.push_back(receiver);
    }

    // The RDS receivers, after the ordinary ones so every placement is in one
    // block, and each with a decoder attached before a sample moves.
    //
    // The rate is decode::RdsBitsConfig's default rather than a literal, so a
    // change on that side cannot leave this receiver at a rate the decoder
    // would then refuse. The bound it has to clear is not the decoder's own
    // 125000 but the receiver's: core/dsp/vrx_reference.h puts the audio
    // decimation filter's passband edge at 0.4 of the audio rate, so the top
    // of the composite is inside the passband only from 148438 S/s up.
    constexpr SampleRate kCompositeRate = decode::RdsBitsConfig{}.rate;
    static_assert((kCompositeRate * 2) / 5 >= kCompositeTopHz,
                  "the RDS composite rate has to put the audio decimation filter's passband "
                  "edge, which is 0.4 of it, above the top of the FM composite");

    std::vector<std::shared_ptr<RdsStation>> stations;
    stations.reserve(options.rds.size());
    for (std::size_t i = 0; i < options.rds.size(); ++i) {
        RdsSpec spec = options.rds[i];

        const Hertz source_center = eng.info().source_center;
        if (spec.relative) {
            spec.baseband = spec.center;
            spec.center = source_center + spec.baseband;
        } else {
            spec.baseband = spec.center - source_center;
        }

        const Hertz reach = eng.info().source_rate / 2;
        if (spec.baseband > reach || spec.baseband < -reach) {
            return fail(std::format(
                "--rds {} is {} from the source's centre of {}, and the source only carries "
                "+/-{}. Either retune the source or name an offset with a leading sign",
                format_hz(spec.center), format_hz(spec.baseband), format_hz(source_center),
                format_hz(reach)));
        }

        // THE GRID, BEFORE THE RECEIVER, because add_vrx refuses first and
        // refuses about the wrong thing. A receiver cannot be given more than
        // half a grid channel either side of where it sits, so a channel
        // narrower than the composite is a grid problem and not a passband
        // problem, and the engine's own refusal says "no transition band
        // between its own edge and the fold" rather than "lower --channels".
        // An operator reading that goes looking at the receiver, which is
        // the one place the answer is not.
        const SampleRate channel_rate = eng.info().channel_rate;
        if (channel_rate / 2 < kCompositeTopHz) {
            return fail(std::format(
                "--rds {} needs a receiver reaching {} Hz either side of the station, "
                "because that is where the top of the FM composite is and the 57 kHz "
                "subcarrier is under it. One grid channel is {} S/s here and a receiver "
                "cannot be given more than half of that either side of where it sits, so "
                "the widest receiver this grid can carry is +/-{} Hz. The grid decimates by "
                "half the channel count, so each halving of --channels doubles the channel "
                "rate: --channels {} on this source would grant +/-{} Hz",
                format_hz(spec.center), kCompositeTopHz, channel_rate, channel_rate / 2,
                eng.info().grid.channels / 2, channel_rate));
        }

        engine::VrxParams params;
        params.center = spec.baseband;
        // The wfm default, 200 kHz wide. The composite only needs +/-59375,
        // and a receiver at that bare minimum decodes worse rather than not
        // at all: Carson for a multiplex deviating 75 kHz and reaching
        // 59375 Hz is about 269 kHz, so even 200 kHz is already truncating
        // the sidebands.
        const dsp::Passband passband = dsp::default_passband(Demod::Wfm);
        params.passband_low = passband.low;
        params.passband_high = passband.high;
        params.bandwidth = passband.width();
        params.demod = Demod::Wfm;
        params.audio_rate = kCompositeRate;

        auto added = eng.add_vrx(params);
        if (!added) {
            return std::unexpected(with_context(
                added.error(),
                std::format("adding the RDS receiver for {}", format_hz(spec.center))));
        }

        auto status = eng.vrx_status(*added);
        if (!status) {
            return std::unexpected(with_context(
                status.error(),
                std::format("reading back the RDS receiver for {}", format_hz(spec.center))));
        }
        print_placement(receivers.size() + i + 1, *status, eng.info().grid.channels,
                        spec.center);

        // And again against what was actually GRANTED, because the grid
        // check above is necessary and not sufficient: each edge is fitted
        // on its own against the fold, so a wide enough channel can still
        // hand back a narrow passband. Without this the symptom is a decoder
        // that simply never locks on a station that is plainly there.
        const engine::VrxPlacement& placement = status->placement;
        if (placement.granted_low > -kCompositeTopHz ||
            placement.granted_high < kCompositeTopHz) {
            return fail(std::format(
                "the RDS receiver for {} was granted {:+} to {:+} Hz about its centre and "
                "the FM composite reaches {} Hz either side, so the 57 kHz subcarrier is "
                "outside the filter and no amount of signal will decode. {}The channel it "
                "landed on runs at {} S/s",
                format_hz(spec.center), placement.granted_low, placement.granted_high,
                kCompositeTopHz,
                placement.bandwidth_clamped
                    ? "The request was clamped to what one grid channel can carry. "
                    : "",
                placement.channel_rate));
        }

        decode::RdsBitsConfig bits_config;
        bits_config.rate = kCompositeRate;
        auto sync = decode::RdsBitSync::create(bits_config);
        if (!sync) {
            return std::unexpected(with_context(
                sync.error(),
                std::format("building the RDS decoder for {}", format_hz(spec.center))));
        }

        auto station = std::make_shared<RdsStation>(*added, kCompositeRate,
                                                    options.rds_region, std::move(*sync));
        station->center = spec.center;
        station->number = receivers.size() + i + 1;

        // attach_audio_sink and not set_audio_sink. Nothing else is on this
        // receiver today, so the two would behave identically, and that is
        // exactly the reason to use the one that stays correct: the slot
        // set_audio_sink writes holds one sink, so the first person to give
        // this receiver a second consumer would silently take the decoder's
        // samples away. Nothing detaches, because these live for the run.
        if (auto wired = eng.attach_audio_sink(
                *added,
                [station](const engine::AudioChunk& chunk) -> Status {
                    const std::lock_guard<std::mutex> held(station->lock);
                    decode_rds_chunk(*station, chunk);
                    return {};
                });
            !wired) {
            return std::unexpected(with_context(
                wired.error(),
                std::format("wiring the RDS decoder for {}", format_hz(spec.center))));
        }

        std::println("  rds             {} decode at {} S/s of composite, bit rate 1187.5, "
                     "first groups after carrier and block sync",
                     options.rds_region == decode::Region::kRbds ? "RBDS" : "RDS",
                     kCompositeRate);
        stations.push_back(std::move(station));
    }

    // The event decoders, on the --vrx receivers, each attached before a
    // sample moves. A name attaches to every receiver whose output it reads;
    // auto attaches the decoder named after each receiver's mode, and on a
    // receiver no decoder is named after, every decoder that reads its audio:
    // a usb receiver gets rtty, sitor_b, navtex, the three PSK decoders and
    // cw, an nfm one ax25 and pocsag. That is the right default here and not
    // on the wire, because an operator watching a terminal wants to see which
    // of them the channel is carrying and a client subscribing names what it
    // wants. A request that matches no receiver is refused, because a run that
    // prints nothing looks exactly like a band with nothing on it.
    std::vector<std::shared_ptr<DecodeTap>> decode_taps;
    for (const std::string& name : options.decode) {
        std::size_t attached = 0;
        for (std::size_t i = 0; i < receivers.size(); ++i) {
            const Demod mode = receivers[i].spec.demod;
            const std::string_view mode_name = engine::demod_name(mode);
            std::vector<const rpc::DecoderSpec*> specs;
            if (name != "auto") {
                specs.push_back(rpc::find_decoder(name));
            } else if (const rpc::DecoderSpec* named = rpc::find_decoder(mode_name)) {
                specs.push_back(named);
            } else {
                // Audio decoders only: a raw tap stays as it was, with nothing
                // attached by auto, because p25p1, dstar, tetra, dmr and m17 all read
                // one and the operator is the one who knows which it carries.
                for (const rpc::DecoderSpec& candidate : rpc::decoder_registry()) {
                    if (candidate.input == rpc::DecoderInput::RealAudio &&
                        !candidate.modes.empty() && rpc::decoder_accepts(candidate, mode_name)) {
                        specs.push_back(&candidate);
                    }
                }
            }

            for (const rpc::DecoderSpec* spec : specs) {
                if (spec == nullptr || !rpc::decoder_accepts(*spec, mode_name)) {
                    continue;
                }
                const bool complex_tap = engine::is_complex_tap(mode);
                if ((spec->input == rpc::DecoderInput::ComplexBaseband) != complex_tap) {
                    continue;
                }

                auto tap = std::make_shared<DecodeTap>();
                tap->number = i + 1;
                tap->spec = spec;
                tap->mode = mode_name;

                // attach and not set, for the reason the RDS decoder gives
                // above: a recording or a loudspeaker on the same receiver
                // keeps its samples. Nothing detaches, because these live for
                // the run.
                if (auto wired = eng.attach_audio_sink(
                        receivers[i].id,
                        [tap](const engine::AudioChunk& chunk) -> Status {
                            const std::lock_guard<std::mutex> held(tap->lock);
                            decode_tap_chunk(*tap, chunk);
                            return {};
                        });
                    !wired) {
                    return std::unexpected(with_context(
                        wired.error(),
                        std::format("attaching the {} decoder to receiver {}", spec->name, i + 1)));
                }
                std::println("  decode          {} on receiver {}", spec->name, i + 1);
                decode_taps.push_back(std::move(tap));
                ++attached;
            }
        }
        if (attached == 0) {
            std::string why;
            if (name == "auto") {
                why = std::format("No --vrx is in a mode a decoder is named after or reads; the "
                                  "decoders are {}.",
                                  rpc::decoder_names());
            } else {
                // Every registry row names the modes it reads, the complex
                // decoders included since each was held to its own mode or a
                // raw tap, so the modes are the whole answer. A branch for a
                // decoder with no modes, which told complex from audio by
                // input alone, went when the last such row did.
                const rpc::DecoderSpec* spec = rpc::find_decoder(name);
                why = std::format("The {} decoder reads {}, and no --vrx is one.", name,
                                  rpc::decoder_needs_text(*spec));
            }
            return fail(std::format("--decode {} matched no receiver. {}", name, why));
        }
    }

    if (!options.record.empty()) {
        for (std::size_t i = 0; i < receivers.size(); ++i) {
            auto path = record_path_for(options, i + 1, receivers[i].spec);
            if (!path) {
                return std::unexpected(path.error());
            }
            receivers[i].record_path = *path;
        }
    }

    engine::AudioEgressConfig egress_config;
    // Four seconds rather than the default two. A file or synthetic source
    // runs as fast as the GPU retires work, so the drain thread can be several
    // seconds of audio behind while being nowhere near slow in wall time.
    egress_config.ring_seconds = 4.0;
    // Zero never trims, which is what a recording wants: late audio is still
    // audio. The monitor caps its own backlog from the device side, in
    // WasapiOptions::max_backlog_ms.
    egress_config.max_backlog_seconds = 0.0;

    auto egress_made = engine::AudioEgress::create(egress_config);
    if (!egress_made) {
        return std::unexpected(with_context(egress_made.error(), "creating the audio egress"));
    }
    engine::AudioEgress& egress = **egress_made;

    std::println("");
    for (std::size_t i = 0; i < receivers.size(); ++i) {
        Receiver& receiver = receivers[i];
        const bool playing =
            std::find(options.play.begin(), options.play.end(), i + 1) != options.play.end();

        engine::AudioStreamInfo info;
        info.vrx = receiver.id;
        info.rate = receiver.rate;
        info.channels = receiver.channels;
        info.center = receiver.spec.center;
        info.demod = receiver.spec.demod;
        info.start = 0;
        info.label = std::format("vrx{} {} {}", i + 1, format_hz(receiver.spec.center),
                                 engine::demod_name(receiver.spec.demod));
        info.source_uri = options.uri;
        // Read once, in one place, and handed down. Nothing in the DSP path
        // reads a clock, so a capture replayed faster than realtime still
        // records the time the command was given rather than a time derived
        // from however fast it ran.
        info.created = created;

        std::unique_ptr<engine::AudioBackend> file_backend;
        if (!receiver.record_path.empty()) {
            engine::WavWriteOptions wav;
            wav.format = options.record_format;
            auto backend = engine::make_wav_file_backend(receiver.record_path, wav);
            if (!backend) {
                return std::unexpected(with_context(
                    backend.error(),
                    std::format("opening '{}' for receiver {}", receiver.record_path, i + 1)));
            }
            file_backend = std::move(*backend);
        }

        std::unique_ptr<engine::AudioBackend> device_backend;
        if (playing) {
            engine::WasapiOptions wasapi;
            wasapi.device_id = options.audio_device;
            wasapi.gain = static_cast<float>(options.volume);
            auto backend = engine::make_wasapi_backend(wasapi);
            if (!backend) {
                return std::unexpected(with_context(
                    backend.error(),
                    std::format("opening the sound device for receiver {}", i + 1)));
            }
            device_backend = std::move(*backend);
        }

        if (file_backend == nullptr && device_backend == nullptr) {
            std::println("vrx {}     no destination: neither --record nor --play names it",
                         i + 1);
            continue;
        }

        // The file keeps the receiver's own id so its stats line up with the
        // receiver everywhere else. The monitor, when there is one alongside a
        // recording, takes an id of its own.
        engine::VrxId primary = receiver.id;
        std::unique_ptr<engine::AudioBackend> primary_backend;
        std::unique_ptr<engine::AudioBackend> secondary_backend;
        engine::VrxId secondary{};

        if (file_backend != nullptr) {
            primary_backend = std::move(file_backend);
            if (device_backend != nullptr) {
                secondary = engine::VrxId{kMonitorIdBase + receiver.id.value};
                secondary_backend = std::move(device_backend);
            }
        } else {
            primary_backend = std::move(device_backend);
        }

        if (auto added = egress.add_receiver(info, std::move(primary_backend)); !added) {
            return std::unexpected(with_context(
                added.error(), std::format("registering egress for receiver {}", i + 1)));
        }
        receiver.has_egress = true;

        if (secondary_backend != nullptr) {
            engine::AudioStreamInfo monitor_info = info;
            monitor_info.vrx = secondary;
            monitor_info.label += " monitor";
            if (auto added = egress.add_receiver(monitor_info, std::move(secondary_backend));
                !added) {
                return std::unexpected(with_context(
                    added.error(),
                    std::format("registering the monitor for receiver {}", i + 1)));
            }
            receiver.monitor = secondary;
        }

        auto primary_sink = egress.sink_for(primary);
        if (!primary_sink) {
            return std::unexpected(with_context(
                primary_sink.error(), std::format("sink for receiver {}", i + 1)));
        }

        // attach_audio_sink AND NOT set_audio_sink, WHICH IS THE WHOLE POINT
        //
        // This used to call set_audio_sink and, when there was a monitor
        // alongside a recording, hand it a lambda that called the two sinks
        // itself. core/engine/engine.h named that lambda as the shape of the
        // problem AudioFanout was built to end, and it was still here after
        // the fan-out landed. The slot holds one sink: whatever filled it
        // last wins, silently, so this process and any wire subscriber were
        // one call away from taking each other's audio with nothing said.
        //
        // Attaching leaves the ordering and the failure rule in one place
        // rather than two. The hand-rolled version returned at the first
        // refusal and the monitor never saw that chunk; the fan-out calls
        // every consumer and hands back the first error, so a disk that
        // filled up no longer silences the loudspeaker on the way out.
        //
        // Nothing detaches. These live for the run and the engine goes with
        // the process; detaching on the way out would be the same file
        // operations in reverse for no reader.
        if (auto wired = eng.attach_audio_sink(receiver.id, std::move(*primary_sink));
            !wired) {
            return std::unexpected(with_context(
                wired.error(), std::format("wiring receiver {}", i + 1)));
        }

        if (receiver.monitor.valid()) {
            auto monitor_sink = egress.sink_for(receiver.monitor);
            if (!monitor_sink) {
                return std::unexpected(with_context(
                    monitor_sink.error(), std::format("monitor sink for receiver {}", i + 1)));
            }

            // The monitor is registered with the egress under an id of its
            // own, so the chunk it is handed has to carry that id. The
            // samples are not copied: the span is borrowed and publish()
            // does its own copy into the ring.
            const engine::VrxId monitor_id = receiver.monitor;
            engine::AudioSink monitored = std::move(*monitor_sink);
            if (auto wired = eng.attach_audio_sink(
                    receiver.id,
                    [monitored, monitor_id](const engine::AudioChunk& chunk) -> Status {
                        engine::AudioChunk copy = chunk;
                        copy.vrx = monitor_id;
                        return monitored(copy);
                    });
                !wired) {
                return std::unexpected(with_context(
                    wired.error(), std::format("wiring the monitor for receiver {}", i + 1)));
            }
        }

        std::string destinations;
        if (!receiver.record_path.empty()) {
            destinations = std::format("recording to {}, {}", receiver.record_path,
                                       engine::wav_format_name(options.record_format));
        }
        if (playing) {
            if (!destinations.empty()) {
                destinations += " and ";
            }
            destinations += std::format(
                "playing to {}", options.audio_device.empty() ? std::string("the default device")
                                                              : options.audio_device);
            if (options.volume != 1.0) {
                destinations += std::format(" at gain {:.3g}", options.volume);
            }
        }
        std::println("vrx {}     {}", i + 1, destinations);
    }

    // One sink, because there is one span. When both the waterfall and the
    // detector want the frame it fans out here rather than either of them
    // keeping a copy: the buffer is valid for the call and both consumers
    // finish inside it.
    if (waterfall != nullptr || detector != nullptr) {
        SpectrumView* view = waterfall.get();
        DetectView* detect_view = detector.get();
        if (auto wired = eng.set_spectrum_sink(
                [view, detect_view](const engine::SpectrumFrame& frame) -> Status {
                    if (view != nullptr) {
                        if (auto drawn = view->publish(frame); !drawn) {
                            return drawn;
                        }
                    }
                    if (detect_view != nullptr) {
                        return detect_view->publish(frame);
                    }
                    return {};
                });
            !wired) {
            return std::unexpected(with_context(wired.error(), "wiring the spectrum sink"));
        }
    }

    if (auto started = egress.start(); !started) {
        return std::unexpected(with_context(started.error(), "starting the audio egress"));
    }

    // From here on a failure still has to tear the egress down, so the file
    // headers are finalised and the device is released. Everything below
    // records its outcome rather than returning early.
    Status outcome;

    std::atomic<bool> finished{false};
    const auto wall_start = std::chrono::steady_clock::now();
    StatusLine status_line(!options.quiet);

    // Manual reset, so a handler that arrives after teardown returns at once
    // rather than being told to wait for something that already happened.
    g_teardown_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    {
        const std::lock_guard<std::mutex> guard(g_engine_lock);
        g_engine = &eng;
    }
    if (SetConsoleCtrlHandler(console_handler, TRUE) == 0) {
        std::println(stderr,
                     "warning: the Ctrl-C handler could not be installed, so an interrupt will "
                     "leave the recording unfinalised");
    }

    // One thread for both jobs. --duration is enforced against the source's
    // own delivered sample count rather than a wall clock, because a capture
    // replayed at forty times realtime has to stop after the requested amount
    // of capture, not of an afternoon.
    if (waterfall != nullptr) {
        const auto [labels, ticks] = waterfall->axis();
        std::println("");
        std::println("{}", labels);
        std::println("{}", ticks);
    }

    std::thread monitor([&] {
        constexpr auto kPoll = std::chrono::milliseconds(10);
        auto last_draw = std::chrono::steady_clock::now();
        const auto interval = std::chrono::milliseconds(options.status_ms);
        bool pending_row = false;
        bool pending_tracks = false;

        while (!finished.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(kPoll);

            // Drained every poll rather than once per row, so the ring never
            // fills and every frame between two rows is peak held into the
            // next one.
            if (waterfall != nullptr && waterfall->collect()) {
                pending_row = true;
            }
            if (detector != nullptr && detector->collect()) {
                pending_tracks = true;
            }

            const source::SourceStats source_stats = eng.source_stats();
            const SampleRate rate = eng.info().source_rate;
            const double source_seconds =
                rate > 0 ? static_cast<double>(source_stats.samples_delivered) /
                               static_cast<double>(rate)
                         : 0.0;

            if (options.duration > 0.0 && source_seconds >= options.duration &&
                !g_stop_requested.exchange(true, std::memory_order_acq_rel)) {
                static_cast<void>(eng.stop());
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - last_draw < interval) {
                continue;
            }
            last_draw = now;

            // The waterfall owns the terminal for the length of a row: the
            // status line is erased, the row is printed, and the status line
            // is redrawn under it. One thread does all three, which is the
            // whole reason the frame came here through a ring instead of
            // being printed where it arrived.
            if (pending_row) {
                status_line.erase();
                std::println("{}", waterfall->take_row(source_seconds));
                pending_row = false;
            }

            // The track list, under the same rule: the thread holding the
            // terminal prints it. A whole table each interval rather than a
            // diff, because a table read at a glance is the point and a
            // stream of "track 12 changed" is not.
            if (pending_tracks) {
                const std::span<const DetectView::Row> rows = detector->rows();
                // Both thresholds, because both are filtering this list and
                // naming one of them reads as though the other were not in
                // force. A run with the confidence bar at one printed
                // "0 tracks over 6.0 dB" while 88 tracks were born, which
                // blames the dB threshold for what the confidence bar did.
                const std::uint32_t over_bar = detector->over_bar();
                std::string capped;
                if (over_bar > rows.size()) {
                    capped = std::format(", {} of them listed", rows.size());
                }
                status_line.erase();
                std::println("{:8.2f}s  {} track{} over {:.1f} dB and {:g} confidence{}",
                             source_seconds, over_bar, over_bar == 1 ? "" : "s",
                             options.detect_threshold_db, options.detect_confidence, capped);
                if (!rows.empty()) {
                    std::println("{}", track_header(detector->tier_two()));
                }
                for (const DetectView::Row& row : rows) {
                    std::println("{}", track_line(row, detector->tier_two()));
                }
                if (detector->grouping()) {
                    const auto groups = track_groups(detector->group_rows());
                    if (detector->groups_total() == 0) {
                        std::println("  no two tracks within {}",
                                     format_hz(options.detect_groups));
                    }
                    for (const std::string& group : groups) {
                        std::println("{}", group);
                    }
                    // Said rather than dropped, for the reason over_bar is
                    // counted past the block: a short list and a truncated
                    // one must not look the same.
                    if (detector->groups_listed() < detector->groups_total()) {
                        std::println("  {} more group{} than the snapshot holds",
                                     detector->groups_total() - detector->groups_listed(),
                                     detector->groups_total() - detector->groups_listed() == 1
                                         ? ""
                                         : "s");
                    }
                }

                // Under the table, because it is about the list rather than
                // about any row in it. Printed only when there is something
                // to say, on the argument ui/models/source_pacing.h makes: a
                // line that is always there is a line nobody reads.
                const detect::FrontEndObservation front_end = detector->front_end();
                if (front_end.verdict == detect::FrontEndVerdict::SpanScales) {
                    std::println(
                        "          front end: the whole span is moving with the strongest "
                        "signal at {:.1f} dB per dB, which is a gain control changing. Set "
                        "gain to a number if it is on auto.",
                        front_end.slope);
                } else if (front_end.verdict == detect::FrontEndVerdict::FloorFollowsSignal) {
                    std::println(
                        "          front end: the noise floor is rising {:.1f} dB for every "
                        "dB the strongest signal rises and sits {:.1f} dB above its quietest. "
                        "Being driven too hard does that, and so does a broadband interferer. "
                        "Tracks in this list may be products.",
                        front_end.slope, front_end.floor_lift_db);
                }
                pending_tracks = false;
            }

            // The stations, under the same rule as the track list: the
            // thread holding the terminal prints them. Printed on every
            // interval rather than on a change, because the point of the
            // display is watching a name and a RadioText fill in character
            // by character, and a diff of that is unreadable.
            //
            // Printed even under --quiet, which suppresses the one-line
            // status and not a decode that was explicitly asked for, the
            // same way --detect's table is.
            for (const std::shared_ptr<RdsStation>& station : stations) {
                status_line.erase();
                print_rds(source_seconds, *station, snapshot_rds(*station));
            }

            // Decoded messages, one line each as they arrived since the last
            // interval, under the same rule and also printed under --quiet.
            for (const std::shared_ptr<DecodeTap>& tap : decode_taps) {
                status_line.erase();
                static_cast<void>(print_decoded(*tap));
            }

            if (options.quiet) {
                continue;
            }

            const double wall_seconds =
                std::chrono::duration<double>(now - wall_start).count();
            std::string line = std::format(
                "{:8.2f}s  x{:6.2f}", source_seconds,
                wall_seconds > 0.0 ? source_seconds / wall_seconds : 0.0);

            // The map's ends, printed because a moving scale is otherwise
            // invisible: a row that darkened because the band went quiet and
            // one that darkened because the ceiling rose look identical.
            if (waterfall != nullptr) {
                const auto [floor, top] = waterfall->map_ends();
                line += std::format(" | map {:6.1f} to {:6.1f} dB", floor, top);
            }

            for (std::size_t i = 0; i < receivers.size(); ++i) {
                auto status = eng.vrx_status(receivers[i].id);
                if (!status) {
                    continue;
                }
                line += std::format(" | v{} {:6.1f}dB [{}] {}", i + 1, status->level_dbfs,
                                    level_bar(status->level_dbfs),
                                    status->squelch_open ? "open" : "shut");
            }

            std::uint64_t dropped = 0;
            std::uint64_t underruns = 0;
            for (const engine::AudioEgressStats& stats : egress.all_stats()) {
                dropped += stats.frames_dropped;
                underruns += stats.underrun_frames;
            }
            if (source_stats.samples_lost != 0) {
                line += std::format(" | lost {}", source_stats.samples_lost);
            }
            if (dropped != 0) {
                line += std::format(" | dropped {}", dropped);
            }
            if (underruns != 0) {
                line += std::format(" | underrun {}", underruns);
            }

            status_line.draw(line);
        }
    });

    // The characterise receiver, placed last so it cannot disturb the ones the
    // operator asked for and so a refusal here names itself.
    auto collector = std::make_shared<CharacteriseCollector>();
    engine::VrxId characterise_vrx{};

    // How far the asked-for frequency sits from the coarse channel's own
    // centre. The raw tap is not mixed, so this is what has to come off the
    // extract to put the signal at DC.
    double characterise_residual_hz = 0.0;
    if (options.characterise_asked) {
        engine::VrxParams params;
        params.center = options.characterise_hz - eng.info().source_center;
        params.demod = engine::Demod::Raw;

        auto added = eng.add_vrx(params);
        if (!added) {
            return std::unexpected(with_context(
                added.error(),
                std::format("placing a raw receiver at {} to characterise",
                            format_hz(options.characterise_hz))));
        }
        characterise_vrx = *added;

        if (auto wired = eng.attach_audio_sink(
                characterise_vrx,
                [collector](const engine::AudioChunk& chunk) -> Status {
                    collector->take(chunk);
                    return {};
                });
            !wired) {
            return std::unexpected(with_context(
                wired.error(), std::format("wiring the characteriser at {}",
                                           format_hz(options.characterise_hz))));
        }

        auto status = eng.vrx_status(characterise_vrx);
        const double channel_rate =
            status ? static_cast<double>(status->placement.channel_rate) : 0.0;
        if (status && status->placement.residual_denominator != 0) {
            characterise_residual_hz =
                static_cast<double>(status->placement.residual_numerator) /
                static_cast<double>(status->placement.residual_denominator);
        }
        std::println("characterise  raw tap at {}, channel {} at {:g} S/s",
                     format_hz(options.characterise_hz),
                     status ? status->placement.channel : 0, channel_rate);
        if (channel_rate > 0.0) {
            if (options.characterise_seconds > 0.0) {
                const auto asked = static_cast<std::size_t>(
                    options.characterise_seconds * channel_rate);
                const std::lock_guard<std::mutex> held(collector->lock);
                collector->wanted =
                    std::max(asked, characterise::kMinCharacteriseSamples);
            }
            std::size_t wanted = 0;
            {
                const std::lock_guard<std::mutex> held(collector->lock);
                wanted = collector->wanted;
            }
            std::println("  collecting {} samples, {:.1f} s at this channel rate; the stage "
                         "needs {}",
                         wanted, static_cast<double>(wanted) / channel_rate,
                         characterise::kMinCharacteriseSamples);
        }
    }

    Status ran = eng.run();

    finished.store(true, std::memory_order_release);
    monitor.join();
    status_line.erase();

    // Unregistering does not wait for a handler already inside stop(), so the
    // lock is what makes the engine safe to destroy after this returns.
    static_cast<void>(SetConsoleCtrlHandler(console_handler, FALSE));
    {
        const std::lock_guard<std::mutex> guard(g_engine_lock);
        g_engine = nullptr;
    }

    // A stop we asked for and got is a clean finish. Anything else is not,
    // and the difference is the exact message the graph reports when it
    // cancels itself: run() still stops the source, flushes the graph and
    // checks the scheduler after cancellation, and any of those can fail for
    // a real reason. Swallowing every error behind the latch turned a device
    // lost to a driver reset during a timed recording into "stopped at the
    // requested 30 s" and exit 0.
    const bool asked_to_stop = g_stop_requested.load(std::memory_order_acquire);
    const bool is_cancellation =
        !ran && ran.error().message.find("the engine was stopped") != std::string::npos;
    if (!ran && !(asked_to_stop && is_cancellation)) {
        outcome = std::unexpected(with_context(ran.error(), "running the engine"));
    }

    const double wall_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();

    const source::SourceStats source_stats = eng.source_stats();

    if (auto stopped = egress.stop(); !stopped && outcome) {
        outcome = std::unexpected(with_context(stopped.error(), "stopping the audio egress"));
    }

    // After stop(), which moves the last of every ring into its backend, and
    // before remove_receiver, which frees the slot and its counters with it.
    // Reading earlier would report a written count short by whatever the drain
    // thread had not reached yet.
    const std::vector<engine::AudioEgressStats> egress_stats = egress.all_stats();

    // stop() makes every file complete and readable but leaves it open.
    // Removing the receiver is what closes it, and the file size below is only
    // the truth once it has.
    for (const Receiver& receiver : receivers) {
        if (!receiver.has_egress) {
            continue;
        }
        if (auto removed = egress.remove_receiver(receiver.id); !removed && outcome) {
            outcome = std::unexpected(
                with_context(removed.error(), "closing a receiver's audio destination"));
        }
        if (receiver.monitor.valid()) {
            if (auto removed = egress.remove_receiver(receiver.monitor); !removed && outcome) {
                outcome = std::unexpected(with_context(removed.error(), "closing a monitor"));
            }
        }
    }

    // -----------------------------------------------------------------------
    // The summary
    // -----------------------------------------------------------------------

    const SampleRate rate = eng.info().source_rate;
    const double source_seconds =
        rate > 0 ? static_cast<double>(source_stats.samples_delivered) / static_cast<double>(rate)
                 : 0.0;

    std::println("");
    if (g_interrupted.load(std::memory_order_acquire)) {
        std::println("interrupted, stopped cleanly");
    } else if (asked_to_stop) {
        std::println("stopped at the requested {:g} s of source time", options.duration);
    }
    std::println("source    {} samples in, {} blocks, {:.3f} s of capture in {:.3f} s wall "
                 "({:.2f}x realtime)",
                 source_stats.samples_delivered, source_stats.blocks_delivered, source_seconds,
                 wall_seconds, wall_seconds > 0.0 ? source_seconds / wall_seconds : 0.0);

    if (characterise_vrx.valid()) {
        std::vector<dsp::Complex32> extract;
        dsp::SampleRate extract_rate = 0;
        std::uint64_t extract_seen = 0;
        {
            const std::lock_guard<std::mutex> held(collector->lock);
            extract.swap(collector->samples);
            extract_rate = collector->rate;
            extract_seen = collector->seen;
        }

        std::println("");
        std::println("characterise  {} samples collected of {} seen at {} S/s",
                     extract.size(), extract_seen, extract_rate);

        if (extract.size() < characterise::kMinCharacteriseSamples) {
            // Said as an arithmetic shortfall rather than as a failure,
            // because the fix is a longer --duration and the number to give
            // it is right here.
            const double needed =
                extract_rate > 0
                    ? static_cast<double>(characterise::kMinCharacteriseSamples -
                                          extract.size()) /
                          static_cast<double>(extract_rate)
                    : 0.0;
            std::println("  short of the {} the stage needs; run {:.1f} s longer",
                         characterise::kMinCharacteriseSamples, needed);
        } else {
            // MIXED TO DC HERE, ON THE HOST, WHICH THE ENGINE WOULD NOT DO.
            //
            // The raw tap is one coarse channel straight out of the channel
            // ring, so the signal sits wherever it sits inside that channel.
            // Every geometric answer characterise() gives is relative to the
            // extract's own DC, and the M-th power law folds a carrier offset
            // modulo rate over the order, so an unmixed extract reports a
            // carrier position that is ambiguous rather than wrong.
            //
            // A rotation is the whole of it: one complex multiply per sample
            // over thirty-two thousand samples, in a command line tool that
            // has already stopped the engine. It is NOT filtering, and the
            // extract still carries everything else in the channel.
            if (characterise_residual_hz != 0.0 && extract_rate > 0) {
                const double step = -2.0 * std::numbers::pi * characterise_residual_hz /
                                    static_cast<double>(extract_rate);
                for (std::size_t i = 0; i < extract.size(); ++i) {
                    const double phase = step * static_cast<double>(i);
                    const auto rotation = std::complex<double>(std::cos(phase),
                                                               std::sin(phase));
                    const auto value = std::complex<double>(extract[i].real(),
                                                            extract[i].imag()) * rotation;
                    extract[i] = dsp::Complex32(static_cast<float>(value.real()),
                                                static_cast<float>(value.imag()));
                }
                std::println("  mixed           {:+.1f} Hz to put the asked-for frequency at "
                             "DC; still unfiltered",
                             -characterise_residual_hz);
            }

            // FILTERED AFTER THE MIX, which is the only order that works: the
            // low pass is about DC and the mix is what put the signal there.
            //
            // A windowed sinc applied by direct convolution. Thirty-two
            // thousand samples against a hundred and one taps is three million
            // multiplies in a tool that has already stopped the engine, and
            // the alternative is a transform that would have to be written.
            //
            // WHAT THIS PARAGRAPH USED TO SAY
            //
            // It read: "IT DOES NOT DECIMATE. characterise() wants at least
            // 16384 samples and the extract is twice that, so throwing away
            // seven of every eight to narrow the rate would take the count
            // under the floor and refuse. Filtering without decimating removes
            // the noise power and keeps the samples, which is what this is
            // for."
            //
            // That is now false and the block below says why: filtering
            // without decimating leaves the noise correlated and a
            // cyclostationary detector reads correlation as a symbol rate. The
            // decimation went in on 2026-09-22 and this paragraph was left
            // sitting directly above it, contradicting the code it introduces.
            if (options.characterise_width > 0 && extract_rate > 0) {
                // DECIMATE AS WELL AS FILTER, AND THE FIRST ATTEMPT DID NOT.
                //
                // Filtering without decimating leaves the noise heavily
                // oversampled, which is to say correlated, and a
                // cyclostationary detector reads correlation as a symbol rate.
                // Measured 2026-09-22: empty 40 m low passed to 800 Hz at the
                // full 3 kS/s channel rate came back PSK at 0.98 confidence.
                // Dropping the rate with the bandwidth is what keeps the noise
                // white, and white noise is what every threshold in
                // core/characterise was stated against.
                //
                // N is chosen so the surviving rate is about three times the
                // requested width: enough margin that the filter's own
                // transition is not folded back in, and low enough that the
                // estimators are not looking for a symbol rate a thousandth of
                // the way along their axis.
                const auto width = static_cast<double>(options.characterise_width);
                auto decimation = static_cast<std::size_t>(
                    std::floor(static_cast<double>(extract_rate) / (3.0 * width)));
                decimation = std::max<std::size_t>(decimation, 1);

                // Not so far that what is left is under the floor.
                const std::size_t wanted_decimation = decimation;
                while (decimation > 1 &&
                       extract.size() / decimation < characterise::kMinCharacteriseSamples) {
                    --decimation;
                }

                // AND SAY SO WHEN IT BINDS, because it used to bind silently
                // and that is what made this flag look like it was doing
                // nothing. The extract is what was collected, the floor is the
                // stage's, and the ratio of the two caps the decimation
                // however narrow a width is asked for: at the default twenty
                // seconds of a 3 kS/s channel the cap is 3, so every width
                // from about 350 Hz down produced the same answer. The seconds
                // that would lift it are arithmetic and the operator should
                // not have to do it.
                if (decimation < wanted_decimation) {
                    const double needed_samples =
                        static_cast<double>(wanted_decimation) *
                        static_cast<double>(characterise::kMinCharacteriseSamples);
                    std::println("  narrowing       capped: {} Hz wants to decimate by {} and "
                                 "{} samples only allow {}. The stage needs {} after "
                                 "decimating, so pass --characterise-seconds {:.0f} to get "
                                 "the width asked for",
                                 options.characterise_width, wanted_decimation,
                                 extract.size(), decimation,
                                 characterise::kMinCharacteriseSamples,
                                 std::ceil(needed_samples /
                                           static_cast<double>(extract_rate)));
                }

                const double survives =
                    static_cast<double>(extract_rate) / static_cast<double>(decimation);

                // HALF THE WIDTH, BECAUSE THE ARGUMENT IS A WIDTH. The
                // passband runs either side of DC, so a caller asking for
                // 800 Hz is asking for plus and minus 400.
                //
                // THIS READ 0.45 * survives UNTIL 2026-09-22, which is the
                // surviving Nyquist and not the width at all. With the
                // decimation above at N, the surviving rate is about three
                // times the requested width, so the old line cut at 1.35
                // times it: asking for 800 Hz filtered to 2.7 kHz and asking
                // for 100 Hz filtered to 450. The flag never once filtered to
                // its own argument, and because the number it did use came
                // from the decimation, two different widths landing on one
                // decimation gave bit-identical answers. Measured on 20 m:
                // 300 Hz and 100 Hz agreed to three places on every field,
                // which is what sent somebody looking.
                //
                // The Nyquist term stays as a CAP and not as the value. When
                // the decimation is clamped below what the width wanted, the
                // surviving rate is lower than three times the width and half
                // the width would then be above the new Nyquist, which is the
                // one case where the caller's number cannot be honoured.
                const auto requested_cutoff = 0.5 * width;
                const double nyquist_cap = 0.45 * survives;
                const double cutoff = std::min(requested_cutoff, nyquist_cap);
                const double normalised = cutoff / static_cast<double>(extract_rate);
                if (normalised >= 0.5) {
                    std::println("  filter          skipped, {} Hz is wider than the {} S/s "
                                 "extract can carry",
                                 options.characterise_width, extract_rate);
                } else {
                    constexpr int kHalf = 50;
                    std::vector<double> taps(2 * kHalf + 1, 0.0);
                    double sum = 0.0;
                    for (int n = -kHalf; n <= kHalf; ++n) {
                        const double x = 2.0 * normalised * static_cast<double>(n);
                        const double sinc =
                            n == 0 ? 1.0 : std::sin(std::numbers::pi * x) /
                                               (std::numbers::pi * x);
                        // Hann, which is what core/dsp reaches for when it
                        // wants a window with no argument about it.
                        const double window =
                            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi *
                                                 static_cast<double>(n + kHalf) /
                                                 static_cast<double>(2 * kHalf));
                        taps[static_cast<std::size_t>(n + kHalf)] = sinc * window;
                        sum += sinc * window;
                    }
                    for (double& tap : taps) {
                        tap /= sum;
                    }

                    std::vector<dsp::Complex32> filtered(extract.size());
                    for (std::size_t i = 0; i < extract.size(); ++i) {
                        double re = 0.0;
                        double im = 0.0;
                        for (int n = -kHalf; n <= kHalf; ++n) {
                            const auto j = static_cast<std::ptrdiff_t>(i) + n;
                            if (j < 0 || j >= static_cast<std::ptrdiff_t>(extract.size())) {
                                continue;
                            }
                            const double tap = taps[static_cast<std::size_t>(n + kHalf)];
                            re += tap * extract[static_cast<std::size_t>(j)].real();
                            im += tap * extract[static_cast<std::size_t>(j)].imag();
                        }
                        filtered[i] = dsp::Complex32(static_cast<float>(re),
                                                     static_cast<float>(im));
                    }
                    if (decimation > 1) {
                        std::vector<dsp::Complex32> kept;
                        kept.reserve(filtered.size() / decimation + 1);
                        for (std::size_t i = 0; i < filtered.size(); i += decimation) {
                            kept.push_back(filtered[i]);
                        }
                        filtered.swap(kept);
                        extract_rate = static_cast<dsp::SampleRate>(
                            std::llround(survives));
                    }

                    extract.swap(filtered);
                    std::println("  filtered        {:.0f} Hz cutoff, {} taps, decimated by "
                                 "{} to {} S/s, {} samples left",
                                 cutoff, 2 * kHalf + 1, decimation, extract_rate,
                                 extract.size());
                }
            }

            characterise::CharacteriseConfig how;
            how.rate = extract_rate;

            // THE SEGMENT IS STATED RATHER THAN INHERITED. Three bins is what
            // spectral_concentration measures over, so a segment of
            // 3 * rate / drift makes that window the drift allowance asked
            // for. Rounded to a power of two because the transform wants one,
            // and floored at 512 which is where analysis_segment floors.
            {
                const double ideal =
                    3.0 * static_cast<double>(extract_rate) / options.characterise_drift;
                auto segment = static_cast<std::size_t>(std::llround(ideal));
                segment = std::bit_floor(std::max<std::size_t>(segment, 512));
                segment = std::min<std::size_t>(segment, extract.size() / 4);
                segment = std::bit_floor(std::max<std::size_t>(segment, 512));
                how.segment = segment;
                std::println("  segment         {} points, so three bins span {:.2f} Hz at "
                             "{} S/s",
                             segment,
                             3.0 * static_cast<double>(extract_rate) /
                                 static_cast<double>(segment),
                             extract_rate);
            }

            auto answer = characterise::characterise(
                dsp::ConstComplexSpan{extract.data(), extract.size()}, how);
            if (!answer) {
                std::println("  refused: {}", answer.error().message);
            } else {
                std::println("  family          {} at {:.2f} confidence",
                             characterise::modulation_family_name(answer->family),
                             answer->family_confidence);
                std::println("  summary         {}", answer->summary);
                if (answer->symbol_rate.found) {
                    std::println("  symbol rate     {:.2f} baud",
                                 answer->symbol_rate.symbol_rate_hz);
                }
                std::println("  occupied        {:.0f} Hz wide, centred {:.0f} Hz from the "
                             "extract's own DC",
                             answer->band.bandwidth_hz, answer->band.centre_hz);
                std::println("  envelope        power variance {:.3f}, peak to average "
                             "{:.1f} dB, concentration {:.3f}",
                             answer->envelope.normalised_power_variance,
                             answer->envelope.peak_to_average_db,
                             answer->spectral_concentration);
                // Found or not, because the OFDM guard bar is a multiple of
                // the searched lags' median and the ratio is the number that
                // says how near a refusal came to being an OFDM call.
                std::println("  cyclic prefix   {:.4f} against a median of {:.4f} over the "
                             "searched lags, {:.1f} times it{}",
                             answer->ofdm.correlation, answer->ofdm.floor_ratio,
                             answer->ofdm.floor_ratio > 0.0
                                 ? answer->ofdm.correlation / answer->ofdm.floor_ratio
                                 : 0.0,
                             answer->ofdm.found ? ", taken as a guard interval" : "");
                if (answer->psk_without_symbol_rate) {
                    std::println("  flagged         PSK with no symbol rate: confidence held to "
                                 "{:.2f}, and it may not drive detection",
                                 characterise::kPskWithoutRateConfidence);
                }
                if (answer->psk_carrier_outside_band) {
                    std::println("  set aside       the power law's carrier sat on {:.2f} of the "
                                 "occupied band's median power, where the band has fallen "
                                 "away, so no PSK call was made",
                                 answer->psk_carrier_level);
                }
                if (!answer->refusal.empty()) {
                    // Printed even when a family WAS found, because the
                    // refusals are how a reader checks the decision rather
                    // than taking it.
                    std::println("  what refused    {}", answer->refusal);
                }
                if (!answer->candidates.empty()) {
                    std::println("  consistent with {}",
                                 characterise::summarise_candidates(answer->candidates));
                }
            }
        }
    }

    if (waterfall != nullptr) {
        const auto [floor, top] = waterfall->map_ends();
        std::println("spectrum  {} frames in, {} rows drawn", waterfall->frames(),
                     waterfall->rows());
        // Where the scale ended up, which is what to hand to
        // --spectrum-floor and --spectrum-ceiling on the next capture if the
        // two are going to be compared.
        std::println("  colour map      {:.1f} to {:.1f} dBFS at the end of the run", floor, top);
    }

    if (detector != nullptr) {
        const detect::DetectorStats& detect_stats = detector->stats();
        std::println("detector  {} frames in, {} decisions, {} tracks born, {} dropped",
                     detector->frames(), detect_stats.decisions, detect_stats.tracks_born,
                     detect_stats.tracks_dropped);
        std::println("  merge and split {} merges, {} splits", detect_stats.merges,
                     detect_stats.splits);
        // The run's answer to whether any set of lines persisted, which no
        // one table can give: a table is one decision, and the tables above
        // are sampled at whatever instant the display thread woke.
        if (const detect::LineGrouper* grouper = detector->grouper(); grouper != nullptr) {
            const detect::LineGroupStats& groups = grouper->stats();
            const SampleRate group_rate = eng.info().source_rate;
            std::string longest = "no group outlasted the decision it formed at";
            if (groups.longest_together_group != 0 && group_rate > 0) {
                longest = std::format("the longest any group held all its lines was {:.1f} s, "
                                      "group #{}",
                                      static_cast<double>(groups.longest_together) /
                                          static_cast<double>(group_rate),
                                      groups.longest_together_group);
            }
            std::println("  groups          {} formed, {} ended, {} joins, {} leaves; {}",
                         groups.groups_formed, groups.groups_ended, groups.joins, groups.leaves,
                         longest);
        }
        // The number core/detect/detector.h justifies running this on the
        // host at all, measured rather than asserted. It covers the whole
        // consume path, so the decisions are amortised into it.
        std::println("  cost            {:.3f} ms per frame across {} bins, {:.1f}% of one core "
                     "at this frame rate",
                     detector->microseconds_per_frame() / 1000.0, eng.info().spectrum.bins,
                     source_seconds > 0.0
                         ? 100.0 * detector->microseconds_per_frame() * 1.0e-6 *
                               static_cast<double>(detector->frames()) / source_seconds
                         : 0.0);

        // Tier two's run, in three parts: what the pool did, what came of
        // it, and what the tracks alive at the end were left saying.
        if (const detect::TierTwoStats* tier = detector->tier_two_stats(); tier != nullptr) {
            const engine::ProbeStats pool = eng.probe_stats();
            std::println("tier two  {} probes submitted, {} characterised, {} too wide, {} "
                         "unplaced, {} cancelled, {} failed",
                         tier->submitted, pool.characterised, pool.too_wide, pool.unplaced,
                         pool.cancelled, pool.failed);
            std::println("  receivers       {} of {} built, {} probes built one and {} retuned "
                         "one in place, characterise {:.1f} ms per extract",
                         pool.receivers, pool.size, pool.builds, pool.retunes,
                         pool.characterised == 0
                             ? 0.0
                             : pool.characterise_ms_total /
                                   static_cast<double>(pool.characterised));
            std::println("  answers         {} recorded on a track, {} named a family the "
                         "detector reports, {} refused by characterise::may_drive_detection, "
                         "{} came back for a track that had gone",
                         tier->recorded, tier->accepted, tier->refused_by_characterise,
                         tier->orphaned);
            // Every answer over the run by what it named, and in brackets how
            // many of those the detector was allowed to report, because the
            // tracks alive at the end are a small and late part of it.
            std::string by_family;
            for (std::size_t i = 0; i < detect::kClassificationCount; ++i) {
                if (tier->named[i] == 0) {
                    continue;
                }
                by_family += std::format(
                    "{}{} {}", by_family.empty() ? "" : ", ",
                    detect::classification_name(static_cast<detect::Classification>(i)),
                    tier->named[i]);
                if (i != 0 && tier->accepted_as[i] != tier->named[i]) {
                    by_family += std::format(" ({} reported)", tier->accepted_as[i]);
                }
            }
            if (!by_family.empty()) {
                std::println("  named           {}", by_family);
            }
            std::string by_protocol;
            for (std::size_t i = 1; i < identify::kProtocolCount; ++i) {
                if (tier->protocols[i] != 0) {
                    by_protocol += std::format(
                        "{}{} {}", by_protocol.empty() ? "" : ", ",
                        identify::protocol_name(static_cast<identify::Protocol>(i)),
                        tier->protocols[i]);
                }
            }
            std::println("  protocols       {}",
                         by_protocol.empty() ? std::string("none verified") : by_protocol);
            if (tier->first_classifications > 0) {
                std::println("  first family    {} tracks, {:.2f} s after birth on average, "
                             "{:.2f} s at best and {:.2f} s at worst",
                             tier->first_classifications,
                             tier->first_classification_seconds_total /
                                 static_cast<double>(tier->first_classifications),
                             tier->first_classification_seconds_min,
                             tier->first_classification_seconds_max);
            }
            if (detector->tier_two_errors() != 0) {
                std::println("  REFUSED         {} submissions the pool had no room for",
                             detector->tier_two_errors());
            }

            // What the tracks alive at the end say, one line each for the
            // ones a probe reached. The table above samples an instant the
            // display thread picked; this is the state the run ended in.
            std::size_t unprobed = 0;
            std::size_t alive = 0;
            for (const detect::Track& track : detector->final_tracks()) {
                ++alive;
                if (track.probes == 0) {
                    ++unprobed;
                }
            }
            std::println("  at the end      {} tracks, {} never reached by a probe", alive,
                         unprobed);
            for (const detect::Track& track : detector->final_tracks()) {
                DetectView::Row row{};
                row.tier_status = detector->tier_status(track.id);
                row.tier_family = static_cast<std::uint32_t>(track.classification);
                row.tier_confidence = track.classification_confidence;
                row.tier_symbol_rate = track.symbol_rate_hz;
                row.tier_probes = track.probes;
                row.tier_last_family = static_cast<std::uint32_t>(track.last_probe.family);
                row.tier_last_confidence = track.last_probe.confidence;
                row.tier_last_symbol_rate = track.last_probe.symbol_rate_hz;
                row.tier_protocol = static_cast<std::uint32_t>(track.protocol);
                std::println("    #{:<5} {:>16}  {:>11}  {:>6.1f} dB  {} probe{}  {}", track.id,
                             format_hz(track.center), format_hz(track.bandwidth),
                             track.snr_2500_db, track.probes, track.probes == 1 ? " " : "s",
                             tier_two_text(row));
            }
        }
    }

    bool any_counter = false;
    if (source_stats.overrun_events != 0 || source_stats.samples_lost != 0) {
        any_counter = true;
        std::println("  LOST            {} samples in {} overruns, last at index {}",
                     source_stats.samples_lost, source_stats.overrun_events,
                     source_stats.last_loss_index);
    }
    if (waterfall != nullptr && waterfall->dropped() != 0) {
        any_counter = true;
        std::println("  DROPPED         {} spectrum frames the display could not keep up with",
                     waterfall->dropped());
    }
    if (detector != nullptr && detector->dropped() != 0) {
        any_counter = true;
        std::println("  DROPPED         {} track snapshots the display could not keep up with",
                     detector->dropped());
    }
    if (detector != nullptr && detector->stats().frames_rejected != 0) {
        any_counter = true;
        std::println("  REJECTED        {} frames whose geometry was not the detector's",
                     detector->stats().frames_rejected);
    }

    for (std::size_t i = 0; i < receivers.size(); ++i) {
        const Receiver& receiver = receivers[i];
        auto status = eng.vrx_status(receiver.id);
        if (status) {
            std::println("vrx {}     {} {}  {} frames out at {} S/s, {} channel{}", i + 1,
                         format_hz(receiver.spec.center),
                         engine::demod_name(receiver.spec.demod), status->audio_samples,
                         receiver.rate, receiver.channels, receiver.channels == 1 ? "" : "s");
            if (status->audio_dropped != 0) {
                any_counter = true;
                std::println("  DROPPED         {} frames the engine could not place",
                             status->audio_dropped);
            }
        }

        for (const engine::AudioEgressStats& stats : egress_stats) {
            const bool mine = stats.vrx == receiver.id || (receiver.monitor.valid() &&
                                                           stats.vrx == receiver.monitor);
            if (!mine) {
                continue;
            }

            // A Pull backend is read through its tap by the device's own
            // thread, so frames_written is structurally zero for it and
            // printing it would read as a fault. The device stream is either
            // the monitor id, or the receiver's own when there is no file.
            const bool to_device =
                stats.vrx != receiver.id || receiver.record_path.empty();
            if (to_device) {
                std::println("  monitor         {} frames published to the sound device",
                             stats.frames_published);
            } else {
                std::println("  recording       {} frames published, {} written",
                             stats.frames_published, stats.frames_written);
            }
            if (stats.frames_dropped != 0) {
                any_counter = true;
                std::println("  DROPPED         {} frames in {} events on the {}",
                             stats.frames_dropped, stats.drop_events,
                             to_device ? "monitor" : "recording");
            }
            if (stats.frames_filled != 0) {
                any_counter = true;
                std::println("  FILLED          {} frames of silence over {} gaps",
                             stats.frames_filled, stats.gap_events);
            }
            if (stats.discontinuities != 0) {
                any_counter = true;
                std::println("  DISCONTINUITY   {} jumps too large to fill",
                             stats.discontinuities);
            }
            if (stats.frames_trimmed != 0) {
                any_counter = true;
                std::println("  TRIMMED         {} frames discarded to cap the backlog",
                             stats.frames_trimmed);
            }
            if (stats.underrun_frames != 0) {
                any_counter = true;
                std::println("  UNDERRUN        {} frames in {} events the device found missing",
                             stats.underrun_frames, stats.underrun_events);
            }
            if (stats.faulted) {
                any_counter = true;
                std::println("  FAULTED         {}{}", stats.fault,
                             stats.fault_code != 0 ? std::format(" (code {})", stats.fault_code)
                                                   : std::string{});
            }
        }

        if (!receiver.record_path.empty()) {
            std::error_code ec;
            const auto size = std::filesystem::file_size(receiver.record_path, ec);
            if (ec) {
                any_counter = true;
                std::println("  FILE            {} could not be measured: {}",
                             receiver.record_path, ec.message());
            } else {
                std::println("  file            {}  {}", receiver.record_path,
                             format_bytes(size));
            }
        }
    }

    // The stations last, because the final state of one is the answer the
    // run was for and it belongs where somebody scrolling back will find it.
    // The engine has stopped by now, so no lock is contended and the
    // snapshot is the whole decode rather than a moment in it.
    for (const std::shared_ptr<RdsStation>& station : stations) {
        const RdsSnapshot snap = snapshot_rds(*station);
        std::println("");
        print_rds(source_seconds, *station, snap);
        if (!snap.fault.empty()) {
            any_counter = true;
        }
    }

    // The decoders' last messages, which the final flush may have produced
    // after the last interval printed, and one line each saying how many there
    // were in all. The engine has stopped, so no lock is contended.
    //
    // The stream has ended, so a decoder holding a message open is told so
    // first and hands over what it recovered of it: a D-STAR transmission
    // the file stopped inside of would otherwise lose every frame since its
    // last superframe.
    for (const std::shared_ptr<DecodeTap>& tap : decode_taps) {
        flush_decode_tap(*tap);
    }
    for (const std::shared_ptr<DecodeTap>& tap : decode_taps) {
        const bool faulted = print_decoded(*tap);
        std::uint64_t produced = 0;
        std::uint64_t dropped = 0;
        std::uint32_t decoded_rate = 0;
        {
            const std::lock_guard<std::mutex> held(tap->lock);
            produced = tap->produced;
            dropped = tap->dropped;
            decoded_rate = tap->rate;
        }
        std::println("");
        std::println("decode {} on vrx {}  {} message{} at {} S/s", tap->spec->name, tap->number,
                     produced, produced == 1 ? "" : "s", decoded_rate);
        if (dropped != 0) {
            any_counter = true;
            std::println("  DROPPED         {} messages not printed because the queue was full",
                         dropped);
        }
        if (faulted) {
            any_counter = true;
        }
    }

    std::println("");
    if (any_counter) {
        std::println("Counters above in capitals are not zero. Audio was lost, filled or "
                     "faulted; the run is not clean.");
    } else {
        std::println("Every drop, fill, underrun and fault counter is zero.");
    }

    return outcome;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        print_usage();
        return 2;
    }

    auto options = parse_options(argc, argv);
    if (!options) {
        std::println(stderr, "revenant-cli: {}", options.error().message);
        std::println(stderr, "Try revenant-cli --help.");
        return 2;
    }

    if (options->help) {
        print_usage();
        return 0;
    }

    if (options->list) {
        if (auto listed = list_sources(); !listed) {
            std::println(stderr, "revenant-cli: {}", listed.error().message);
            return 1;
        }
        return 0;
    }

    if (options->list_audio) {
        if (auto listed = list_audio_devices(); !listed) {
            std::println(stderr, "revenant-cli: {}", listed.error().message);
            return 1;
        }
        return 0;
    }

    if (options->uri.empty()) {
        std::println(stderr, "revenant-cli: no source URI. Run --list to see what is available.");
        return 2;
    }
    // --spectrum, --detect and --rds are each a destination of their own:
    // the first two watch the whole span and need no receiver at all, and
    // the third brings its own.
    if (options->receivers.empty() && options->rds.empty() && !options->spectrum &&
        !options->detect && !options->characterise_asked) {
        std::println(stderr,
                     "revenant-cli: nothing to do. Add a receiver with --vrx, such as "
                     "--vrx 162.550M:nfm:16k, watch the span with --spectrum, look for "
                     "signals with --detect, or decode a station with --rds 98.5M.");
        return 2;
    }
    if (!options->spectrum &&
        (options->spectrum_floor_db.has_value() || options->spectrum_ceiling_db.has_value())) {
        std::println(stderr,
                     "revenant-cli: --spectrum-floor and --spectrum-ceiling set the colour map "
                     "of a waterfall that was not asked for. Add --spectrum.");
        return 2;
    }
    if (options->spectrum_floor_db.has_value() && options->spectrum_ceiling_db.has_value() &&
        *options->spectrum_ceiling_db <= *options->spectrum_floor_db) {
        std::println(stderr,
                     "revenant-cli: --spectrum-ceiling {:g} is not above --spectrum-floor {:g}, "
                     "so the colour map has no range.",
                     *options->spectrum_ceiling_db, *options->spectrum_floor_db);
        return 2;
    }
    // --decode is a destination for a receiver's output as much as a file or
    // a loudspeaker is: a P25 receiver decoded and printed is the whole of
    // what somebody running it wanted.
    if (!options->receivers.empty() && options->record.empty() && options->play.empty() &&
        !options->spectrum && options->decode.empty()) {
        std::println(stderr,
                     "revenant-cli: nothing to do with the audio. Pass --record, --play, "
                     "--decode, or any of them together.");
        return 2;
    }
    for (const std::size_t which : options->play) {
        if (which > options->receivers.size()) {
            std::println(stderr,
                         "revenant-cli: --play {} names a receiver that was never added; "
                         "there {} --vrx.",
                         which,
                         options->receivers.size() == 1
                             ? std::string("is 1")
                             : std::format("are {}", options->receivers.size()));
            return 2;
        }
    }

    if (auto ran = run_receivers(*options); !ran) {
        std::println(stderr, "");
        std::println(stderr, "revenant-cli: {}", ran.error().message);
        if (ran.error().code != 0) {
            std::println(stderr, "  code {}", ran.error().code);
        }
        return 1;
    }

    return 0;
}
