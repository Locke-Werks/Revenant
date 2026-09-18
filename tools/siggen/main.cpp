// siggen: the command line front end to the modulators and the wideband
// synthesizer.
//
// Everything is streamed. A mode is generated block by block straight to the
// file, so `siggen qpsk --duration 3600` writes an hour without ever holding
// an hour, and the wideband subcommand does the same thing through
// stream_scene. That is not an optimisation, it is the case the synthesizer
// exists for: an hour of 20 MS/s is 288 gigabytes and there is no version of
// this that buffers it.
//
// This file deliberately does not include channel.h. The channel simulator is
// being written in parallel and its header is a moving target while this one
// is compiled and checked, so the two are kept independent until integration.
// Adding a `channel` subcommand later is a header include and one more branch
// in run().

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ios>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"
#include "tools/siggen/modulators.h"
#include "tools/siggen/wideband.h"

namespace {

using revenant::Expected;
using revenant::Status;
using revenant::fail;
using revenant::with_context;

namespace dsp = revenant::dsp;
namespace siggen = revenant::siggen;

using dsp::Complex32;
using dsp::ConstComplexSpan;
using dsp::Hertz;
using dsp::SampleIndex;
using dsp::SampleRate;

// ---------------------------------------------------------------------------
// Output formats
// ---------------------------------------------------------------------------

enum class Format {
    // Interleaved float32, the project's native on-disk form. Complex32 is
    // layout compatible with float[2] by guarantee, so the buffer is written
    // straight out with no repacking.
    Cf32,

    // Interleaved signed 8-bit. Full scale maps to +/-127 rather than to -128
    // and +127, so the two polarities scale identically and a DC offset is not
    // manufactured by the quantiser.
    Cs8,

    // Interleaved unsigned 8-bit, offset by 127. This is what an RTL-SDR
    // actually puts on the wire, so it is the format to use when standing in
    // for one.
    Cu8,
};

[[nodiscard]] Expected<Format> format_from_name(std::string_view name)
{
    if (name == "cf32") {
        return Format::Cf32;
    }
    if (name == "cs8") {
        return Format::Cs8;
    }
    if (name == "cu8") {
        return Format::Cu8;
    }
    return fail(std::format("unknown format '{}', expected cf32, cs8 or cu8", name));
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

class Options {
public:
    [[nodiscard]] static Expected<Options> parse(int argc, char** argv);

    [[nodiscard]] const std::string& command() const { return command_; }

    [[nodiscard]] bool flag(std::string_view name);
    [[nodiscard]] Expected<std::int64_t> integer(std::string_view name, std::int64_t fallback);
    [[nodiscard]] Expected<double> real(std::string_view name, double fallback);
    [[nodiscard]] Expected<std::string> text(std::string_view name, std::string fallback);

    // An option nobody read is almost always a typo, and silently ignoring it
    // produces a file that looks right and is not.
    [[nodiscard]] Status reject_unused() const;

private:
    struct Entry {
        std::string name;
        std::string value;
        bool has_value = false;
        bool used = false;
    };

    [[nodiscard]] Entry* find(std::string_view name);

    std::string command_;
    std::vector<Entry> entries_;
};

Expected<Options> Options::parse(int argc, char** argv)
{
    Options options;
    if (argc < 2) {
        return fail("no subcommand");
    }
    options.command_ = argv[1];

    for (int i = 2; i < argc; ++i) {
        const std::string_view token = argv[i];
        if (!token.starts_with("--")) {
            return fail(std::format("unexpected argument '{}', options start with --", token));
        }

        Entry entry;
        entry.name = std::string(token.substr(2));
        if (entry.name.empty()) {
            return fail("empty option name");
        }

        // A lone '-' starts a negative number, not another option, so a
        // negative frequency offset parses the way anyone would expect.
        if (i + 1 < argc && !std::string_view(argv[i + 1]).starts_with("--")) {
            entry.value = argv[i + 1];
            entry.has_value = true;
            ++i;
        }
        options.entries_.push_back(std::move(entry));
    }
    return options;
}

Options::Entry* Options::find(std::string_view name)
{
    for (Entry& entry : entries_) {
        if (entry.name == name) {
            entry.used = true;
            return &entry;
        }
    }
    return nullptr;
}

bool Options::flag(std::string_view name)
{
    return find(name) != nullptr;
}

Expected<std::int64_t> Options::integer(std::string_view name, std::int64_t fallback)
{
    const Entry* entry = find(name);
    if (entry == nullptr) {
        return fallback;
    }
    if (!entry->has_value) {
        return fail(std::format("--{} needs a value", name));
    }

    std::int64_t parsed = 0;
    const char* begin = entry->value.data();
    const char* end = begin + entry->value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        return fail(std::format("--{} '{}' is not an integer", name, entry->value));
    }
    return parsed;
}

Expected<double> Options::real(std::string_view name, double fallback)
{
    const Entry* entry = find(name);
    if (entry == nullptr) {
        return fallback;
    }
    if (!entry->has_value) {
        return fail(std::format("--{} needs a value", name));
    }

    double parsed = 0.0;
    const char* begin = entry->value.data();
    const char* end = begin + entry->value.size();
    // from_chars rather than strtod: strtod honours the C locale, so on a
    // machine set to a comma decimal separator "0.35" would parse as 0.
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        return fail(std::format("--{} '{}' is not a number", name, entry->value));
    }
    return parsed;
}

Expected<std::string> Options::text(std::string_view name, std::string fallback)
{
    const Entry* entry = find(name);
    if (entry == nullptr) {
        return fallback;
    }
    if (!entry->has_value) {
        return fail(std::format("--{} needs a value", name));
    }
    return entry->value;
}

Status Options::reject_unused() const
{
    std::string unused;
    for (const Entry& entry : entries_) {
        if (entry.used) {
            continue;
        }
        if (!unused.empty()) {
            unused += ", ";
        }
        unused += "--" + entry.name;
    }
    if (!unused.empty()) {
        return fail(std::format("unrecognised option(s): {}", unused));
    }
    return {};
}

// ---------------------------------------------------------------------------
// Writing samples
// ---------------------------------------------------------------------------

class Writer {
public:
    Writer(std::ostream& stream, Format format) : stream_(stream), format_(format) {}

    [[nodiscard]] Status consume(ConstComplexSpan samples);

    [[nodiscard]] double mean_power() const
    {
        return (count_ > 0) ? power_sum_ / static_cast<double>(count_) : 0.0;
    }
    [[nodiscard]] double peak() const { return std::sqrt(peak_square_); }
    [[nodiscard]] std::uint64_t clipped() const { return clipped_; }
    [[nodiscard]] std::uint64_t written() const { return count_; }

private:
    [[nodiscard]] std::int32_t quantise(float value);

    std::ostream& stream_;
    Format format_;
    std::vector<char> scratch_{};
    double power_sum_ = 0.0;
    double peak_square_ = 0.0;
    std::uint64_t count_ = 0;
    std::uint64_t clipped_ = 0;
};

std::int32_t Writer::quantise(float value)
{
    const auto scaled = static_cast<std::int32_t>(std::lround(static_cast<double>(value) * 127.0));
    if (scaled > 127) {
        ++clipped_;
        return 127;
    }
    if (scaled < -127) {
        ++clipped_;
        return -127;
    }
    return scaled;
}

Status Writer::consume(ConstComplexSpan samples)
{
    for (const Complex32& sample : samples) {
        const auto re = static_cast<double>(sample.real());
        const auto im = static_cast<double>(sample.imag());
        const double square = re * re + im * im;
        power_sum_ += square;
        peak_square_ = std::max(peak_square_, square);
    }
    count_ += samples.size();

    if (format_ == Format::Cf32) {
        // Complex32 is two packed floats by static_assert in core/dsp/types.h,
        // so the interleaved file layout is already what is in memory.
        const auto* bytes = reinterpret_cast<const char*>(samples.data());
        stream_.write(bytes, static_cast<std::streamsize>(samples.size_bytes()));
    } else {
        scratch_.resize(samples.size() * 2);
        std::size_t at = 0;
        for (const Complex32& sample : samples) {
            const std::int32_t re = quantise(sample.real());
            const std::int32_t im = quantise(sample.imag());
            if (format_ == Format::Cs8) {
                scratch_[at++] = static_cast<char>(static_cast<std::int8_t>(re));
                scratch_[at++] = static_cast<char>(static_cast<std::int8_t>(im));
            } else {
                scratch_[at++] = static_cast<char>(static_cast<std::uint8_t>(re + 127));
                scratch_[at++] = static_cast<char>(static_cast<std::uint8_t>(im + 127));
            }
        }
        stream_.write(scratch_.data(), static_cast<std::streamsize>(scratch_.size()));
    }

    if (!stream_) {
        return fail("write failed, the output stream is no longer good");
    }
    return {};
}

// ---------------------------------------------------------------------------
// Shared options
// ---------------------------------------------------------------------------

struct CommonOptions {
    SampleRate rate = 48000;
    Hertz offset = 0;
    double amplitude = 1.0;
    double phase = 0.0;
    std::uint64_t seed = 0;
    SampleIndex samples = 0;
    Format format = Format::Cf32;
    std::string out_path;
    std::size_t block = 262144;
};

[[nodiscard]] Expected<CommonOptions> read_common(Options& options, SampleRate default_rate)
{
    CommonOptions common;

    auto rate = options.integer("rate", default_rate);
    if (!rate) {
        return std::unexpected(rate.error());
    }
    common.rate = *rate;

    auto offset = options.integer("offset", 0);
    if (!offset) {
        return std::unexpected(offset.error());
    }
    common.offset = *offset;

    auto amplitude = options.real("amplitude", 1.0);
    if (!amplitude) {
        return std::unexpected(amplitude.error());
    }
    common.amplitude = *amplitude;

    auto phase = options.real("phase", 0.0);
    if (!phase) {
        return std::unexpected(phase.error());
    }
    common.phase = *phase;

    auto seed = options.integer("seed", 0);
    if (!seed) {
        return std::unexpected(seed.error());
    }
    common.seed = static_cast<std::uint64_t>(*seed);

    auto duration = options.real("duration", 1.0);
    if (!duration) {
        return std::unexpected(duration.error());
    }
    auto samples = options.integer("samples", 0);
    if (!samples) {
        return std::unexpected(samples.error());
    }

    if (*samples > 0) {
        common.samples = static_cast<SampleIndex>(*samples);
    } else {
        if (!std::isfinite(*duration) || *duration <= 0.0) {
            return fail("--duration must be positive");
        }
        if (common.rate <= 0) {
            return fail("--rate must be positive");
        }
        common.samples =
            static_cast<SampleIndex>(std::llround(*duration * static_cast<double>(common.rate)));
    }
    if (common.samples == 0) {
        return fail("nothing to generate: zero samples requested");
    }

    auto format = options.text("format", "cf32");
    if (!format) {
        return std::unexpected(format.error());
    }
    auto parsed_format = format_from_name(*format);
    if (!parsed_format) {
        return std::unexpected(parsed_format.error());
    }
    common.format = *parsed_format;

    auto out = options.text("out", "");
    if (!out) {
        return std::unexpected(out.error());
    }
    common.out_path = *out;
    if (common.out_path.empty()) {
        return fail("--out is required");
    }

    auto block = options.integer("block", 262144);
    if (!block) {
        return std::unexpected(block.error());
    }
    if (*block <= 0) {
        return fail("--block must be positive");
    }
    common.block = static_cast<std::size_t>(*block);

    return common;
}

[[nodiscard]] Expected<std::ofstream> open_output(const std::string& path)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return fail(std::format("cannot open '{}' for writing", path));
    }
    return stream;
}

void report_buffer(const Writer& writer, double nominal_power)
{
    std::print("  samples written   {}\n", writer.written());
    std::print("  nominal power     {:.6g}\n", nominal_power);
    std::print("  measured power    {:.6g}\n", writer.mean_power());
    std::print("  peak magnitude    {:.6g}\n", writer.peak());
    if (writer.clipped() > 0) {
        std::print("  CLIPPED           {} of {} components hit the quantiser limit\n",
                   writer.clipped(), writer.written() * 2);
    }
}

// ---------------------------------------------------------------------------
// Single mode
// ---------------------------------------------------------------------------

[[nodiscard]] Status run_modulator(siggen::Modulation kind, Options& options)
{
    auto common = read_common(options, 48000);
    if (!common) {
        return std::unexpected(common.error());
    }

    siggen::ModulatorSpec spec;
    spec.kind = kind;
    spec.common.rate = common->rate;
    spec.common.carrier_offset = common->offset;
    spec.common.amplitude = common->amplitude;
    spec.common.initial_phase = common->phase;
    spec.common.seed = common->seed;

    switch (kind) {
    case siggen::Modulation::Cw: {
        auto wpm = options.real("wpm", spec.cw.words_per_minute);
        auto rise = options.real("rise-ms", spec.cw.rise_fall_ms);
        auto text = options.text("text", spec.cw.text);
        if (!wpm) { return std::unexpected(wpm.error()); }
        if (!rise) { return std::unexpected(rise.error()); }
        if (!text) { return std::unexpected(text.error()); }
        spec.cw.words_per_minute = *wpm;
        spec.cw.rise_fall_ms = *rise;
        spec.cw.text = *text;
        break;
    }

    case siggen::Modulation::Am: {
        auto index = options.real("index", spec.am.modulation_index);
        auto tone = options.integer("tone", spec.am.tone_hz);
        if (!index) { return std::unexpected(index.error()); }
        if (!tone) { return std::unexpected(tone.error()); }
        spec.am.modulation_index = *index;
        spec.am.tone_hz = *tone;
        break;
    }

    case siggen::Modulation::Nfm: {
        auto deviation = options.integer("deviation", spec.nfm.deviation);
        auto tone = options.integer("tone", spec.nfm.tone_hz);
        if (!deviation) { return std::unexpected(deviation.error()); }
        if (!tone) { return std::unexpected(tone.error()); }
        spec.nfm.deviation = *deviation;
        spec.nfm.tone_hz = *tone;
        break;
    }

    case siggen::Modulation::Usb:
    case siggen::Modulation::Lsb: {
        auto tone = options.integer("tone", spec.ssb.tone_hz);
        auto tone2 = options.integer("tone2", spec.ssb.tone2_hz);
        auto low = options.integer("audio-low", spec.ssb.audio_low_hz);
        auto high = options.integer("audio-high", spec.ssb.audio_high_hz);
        auto taps = options.integer("hilbert-taps",
                                    static_cast<std::int64_t>(spec.ssb.hilbert_taps));
        if (!tone) { return std::unexpected(tone.error()); }
        if (!tone2) { return std::unexpected(tone2.error()); }
        if (!low) { return std::unexpected(low.error()); }
        if (!high) { return std::unexpected(high.error()); }
        if (!taps) { return std::unexpected(taps.error()); }
        if (*taps <= 0) { return fail("--hilbert-taps must be positive"); }
        spec.ssb.tone_hz = *tone;
        spec.ssb.tone2_hz = *tone2;
        spec.ssb.audio_low_hz = *low;
        spec.ssb.audio_high_hz = *high;
        spec.ssb.hilbert_taps = static_cast<std::size_t>(*taps);
        break;
    }

    case siggen::Modulation::Fsk2: {
        auto symbol_rate = options.real("symbol-rate", spec.fsk.symbol_rate);
        auto deviation = options.integer("deviation", spec.fsk.deviation);
        auto symbols = options.integer("symbols",
                                       static_cast<std::int64_t>(spec.fsk.symbol_count));
        if (!symbol_rate) { return std::unexpected(symbol_rate.error()); }
        if (!deviation) { return std::unexpected(deviation.error()); }
        if (!symbols) { return std::unexpected(symbols.error()); }
        if (*symbols <= 0) { return fail("--symbols must be positive"); }
        spec.fsk.symbol_rate = *symbol_rate;
        spec.fsk.deviation = *deviation;
        spec.fsk.symbol_count = static_cast<std::size_t>(*symbols);
        break;
    }

    case siggen::Modulation::Bpsk:
    case siggen::Modulation::Qpsk: {
        auto symbol_rate = options.real("symbol-rate", spec.psk.symbol_rate);
        auto rolloff = options.real("rolloff", spec.psk.rolloff);
        auto symbols = options.integer("symbols",
                                       static_cast<std::int64_t>(spec.psk.symbol_count));
        auto span = options.integer("span", static_cast<std::int64_t>(spec.psk.span_symbols));
        if (!symbol_rate) { return std::unexpected(symbol_rate.error()); }
        if (!rolloff) { return std::unexpected(rolloff.error()); }
        if (!symbols) { return std::unexpected(symbols.error()); }
        if (!span) { return std::unexpected(span.error()); }
        if (*symbols <= 0) { return fail("--symbols must be positive"); }
        if (*span <= 0) { return fail("--span must be positive"); }
        spec.psk.symbol_rate = *symbol_rate;
        spec.psk.rolloff = *rolloff;
        spec.psk.symbol_count = static_cast<std::size_t>(*symbols);
        spec.psk.span_symbols = static_cast<std::size_t>(*span);
        break;
    }
    }

    if (auto clean = options.reject_unused(); !clean) {
        return clean;
    }

    auto modulator = siggen::Modulator::create(spec);
    if (!modulator) {
        return std::unexpected(modulator.error());
    }

    auto stream = open_output(common->out_path);
    if (!stream) {
        return std::unexpected(stream.error());
    }

    Writer writer(*stream, common->format);
    std::vector<Complex32> buffer(common->block);

    SampleIndex produced = 0;
    while (produced < common->samples) {
        const auto length = static_cast<std::size_t>(std::min<SampleIndex>(
            static_cast<SampleIndex>(common->block), common->samples - produced));
        modulator->render(produced, dsp::ComplexSpan(buffer.data(), length));
        if (auto written = writer.consume(ConstComplexSpan(buffer.data(), length)); !written) {
            return written;
        }
        produced += length;
    }

    stream->flush();
    if (!*stream) {
        return fail(std::format("failed to flush '{}'", common->out_path));
    }

    const siggen::SpectralExtent extent = modulator->occupied_extent();
    std::print("{} at {} S/s\n", siggen::modulation_name(kind), common->rate);
    std::print("  carrier offset    {} Hz\n", common->offset);
    std::print("  occupied band     {} to {} Hz, centre {} Hz, width {} Hz\n",
               extent.low_hz, extent.high_hz, extent.center_hz(), extent.bandwidth_hz());
    if (modulator->effective_symbol_rate() > 0.0) {
        std::print("  symbol rate       {:.6g} baud over a {} sample payload cycle\n",
                   modulator->effective_symbol_rate(), modulator->cycle_samples());
    }
    if (!modulator->payload_bits().empty()) {
        std::print("  payload           {} bits, repeating, from seed {}\n",
                   modulator->payload_bits().size(), common->seed);
    }
    if (kind == siggen::Modulation::Cw) {
        std::print("  keying            '{}' over a {} sample cycle\n",
                   modulator->text(), modulator->cycle_samples());
    }
    report_buffer(writer, modulator->nominal_mean_power());
    std::print("  wrote             {}\n", common->out_path);
    return {};
}

// ---------------------------------------------------------------------------
// Wideband
// ---------------------------------------------------------------------------

[[nodiscard]] Expected<std::vector<siggen::Modulation>> parse_mode_list(std::string_view list)
{
    std::vector<siggen::Modulation> modes;
    std::size_t at = 0;
    while (at <= list.size()) {
        const std::size_t comma = list.find(',', at);
        const std::string_view piece =
            list.substr(at, (comma == std::string_view::npos) ? std::string_view::npos
                                                              : comma - at);
        if (!piece.empty()) {
            auto kind = siggen::modulation_from_name(piece);
            if (!kind) {
                return std::unexpected(kind.error());
            }
            modes.push_back(*kind);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        at = comma + 1;
    }
    if (modes.empty()) {
        return fail("--modes listed no modes");
    }
    return modes;
}

[[nodiscard]] Status run_wideband(Options& options)
{
    auto common = read_common(options, 20'000'000);
    if (!common) {
        return std::unexpected(common.error());
    }

    siggen::SceneSpec spec;
    spec.rate = common->rate;
    spec.duration_samples = common->samples;
    spec.seed = common->seed;

    auto center = options.integer("center", 0);
    auto emitters = options.integer("emitters", 32);
    auto bursts = options.integer("bursts", 1);
    auto span_low = options.integer("span-low", -common->rate * 9 / 20);
    auto span_high = options.integer("span-high", common->rate * 9 / 20);
    auto noise_dbfs = options.real("noise-dbfs", spec.noise_power_full_band_dbfs);
    auto snr_min = options.real("snr-min", spec.random.snr_in_occupied_bandwidth_db_min);
    auto snr_max = options.real("snr-max", spec.random.snr_in_occupied_bandwidth_db_max);
    auto min_burst = options.real("min-burst", spec.random.min_burst_seconds);
    auto max_burst = options.real("max-burst", spec.random.max_burst_seconds);
    auto threads = options.integer("threads", 0);
    auto epoch = options.integer("epoch-ns", 0);
    auto modes = options.text("modes", "");
    auto truth_path = options.text("truth", "");
    const bool no_noise = options.flag("no-noise");

    if (!center) { return std::unexpected(center.error()); }
    if (!emitters) { return std::unexpected(emitters.error()); }
    if (!bursts) { return std::unexpected(bursts.error()); }
    if (!span_low) { return std::unexpected(span_low.error()); }
    if (!span_high) { return std::unexpected(span_high.error()); }
    if (!noise_dbfs) { return std::unexpected(noise_dbfs.error()); }
    if (!snr_min) { return std::unexpected(snr_min.error()); }
    if (!snr_max) { return std::unexpected(snr_max.error()); }
    if (!min_burst) { return std::unexpected(min_burst.error()); }
    if (!max_burst) { return std::unexpected(max_burst.error()); }
    if (!threads) { return std::unexpected(threads.error()); }
    if (!epoch) { return std::unexpected(epoch.error()); }
    if (!modes) { return std::unexpected(modes.error()); }
    if (!truth_path) { return std::unexpected(truth_path.error()); }

    if (*emitters < 0) { return fail("--emitters must not be negative"); }
    if (*bursts <= 0) { return fail("--bursts must be positive"); }
    if (*threads < 0) { return fail("--threads must not be negative"); }

    spec.center_hz = *center;
    spec.epoch_anchor_ns = *epoch;
    spec.add_noise = !no_noise;
    spec.noise_power_full_band_dbfs = *noise_dbfs;
    spec.worker_threads = static_cast<std::size_t>(*threads);

    spec.random.emitter_count = static_cast<std::size_t>(*emitters);
    spec.random.bursts_per_emitter = static_cast<std::size_t>(*bursts);
    spec.random.span_low_hz = *span_low;
    spec.random.span_high_hz = *span_high;
    spec.random.snr_in_occupied_bandwidth_db_min = *snr_min;
    spec.random.snr_in_occupied_bandwidth_db_max = *snr_max;
    spec.random.min_burst_seconds = *min_burst;
    spec.random.max_burst_seconds = *max_burst;

    if (!modes->empty()) {
        auto palette = parse_mode_list(*modes);
        if (!palette) {
            return std::unexpected(palette.error());
        }
        spec.random.palette = *palette;
    }

    if (auto clean = options.reject_unused(); !clean) {
        return clean;
    }

    auto scene = siggen::Scene::create(spec);
    if (!scene) {
        return std::unexpected(with_context(scene.error(), "siggen wideband"));
    }

    auto stream = open_output(common->out_path);
    if (!stream) {
        return std::unexpected(stream.error());
    }

    Writer writer(*stream, common->format);
    auto streamed = siggen::stream_scene(
        *scene, 0, common->samples, common->block,
        [&writer](const dsp::BlockTimestamp&, ConstComplexSpan block) {
            return writer.consume(block);
        });
    if (!streamed) {
        return streamed;
    }

    stream->flush();
    if (!*stream) {
        return fail(std::format("failed to flush '{}'", common->out_path));
    }

    if (!truth_path->empty()) {
        std::ofstream truth(*truth_path, std::ios::trunc);
        if (!truth) {
            return fail(std::format("cannot open '{}' for writing", *truth_path));
        }
        const std::string csv = siggen::truth_csv(*scene);
        truth.write(csv.data(), static_cast<std::streamsize>(csv.size()));
        truth.flush();
        if (!truth) {
            return fail(std::format("failed to write '{}'", *truth_path));
        }
    }

    const double seconds =
        static_cast<double>(common->samples) / static_cast<double>(common->rate);
    std::print("wideband scene at {} S/s, centre {} Hz\n", common->rate, spec.center_hz);
    std::print("  duration          {} samples, {:.3f} s\n", common->samples, seconds);
    std::print("  emitters          {} transmissions from {} slots\n",
               scene->truth().size(), spec.random.emitter_count);
    std::print("  placement span    {} to {} Hz\n", *span_low, *span_high);
    std::print("  noise floor       {}\n",
               spec.add_noise ? std::format("{:.2f} dBFS full band, power {:.6g}",
                                            spec.noise_power_full_band_dbfs,
                                            scene->noise_power_full_band())
                              : std::string("disabled"));
    report_buffer(writer, scene->noise_power_full_band());
    std::print("  wrote             {}\n", common->out_path);
    if (!truth_path->empty()) {
        std::print("  truth             {}\n", *truth_path);
    }
    return {};
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

void print_usage()
{
    std::print(
        "siggen: Revenant signal generator\n"
        "\n"
        "Usage: siggen <mode> [options]\n"
        "\n"
        "Modes:\n"
        "  cw am nfm usb lsb fsk2 bpsk qpsk   one emitter, streamed to a file\n"
        "  wideband                           a populated scene with ground truth\n"
        "  modes                              list the mode names\n"
        "\n"
        "Common options:\n"
        "  --rate N          sample rate in Hz (48000, or 20000000 for wideband)\n"
        "  --offset N        carrier offset from baseband DC in Hz\n"
        "  --duration S      seconds to generate (1.0)\n"
        "  --samples N       sample count, overriding --duration\n"
        "  --amplitude X     peak for constant-envelope modes, RMS for shaped ones\n"
        "  --phase X         initial carrier phase in radians\n"
        "  --seed N          payload and scene seed\n"
        "  --format F        cf32 (default), cs8, or cu8 for RTL-SDR native\n"
        "  --block N         streaming block size in samples (262144)\n"
        "  --out PATH        output file, required\n"
        "\n"
        "  cw                --wpm X --rise-ms X --text \"CQ DE REVENANT\"\n"
        "  am                --index X --tone N\n"
        "  nfm               --deviation N --tone N\n"
        "  usb lsb           --tone N --tone2 N --audio-low N --audio-high N\n"
        "                    --hilbert-taps N\n"
        "  fsk2              --symbol-rate X --deviation N --symbols N\n"
        "  bpsk qpsk         --symbol-rate X --rolloff X --symbols N --span N\n"
        "\n"
        "  wideband          --emitters N --bursts N --span-low N --span-high N\n"
        "                    --noise-dbfs X --no-noise --snr-min X --snr-max X\n"
        "                    --min-burst S --max-burst S --modes a,b,c\n"
        "                    --center N --epoch-ns N --threads N --truth PATH\n"
        "\n"
        "Every mode is deterministic from its seed, and a file generated in one\n"
        "block is byte for byte the same as the same file generated in many.\n");
}

[[nodiscard]] Status run(int argc, char** argv)
{
    auto options = Options::parse(argc, argv);
    if (!options) {
        return std::unexpected(options.error());
    }

    const std::string& command = options->command();
    if (command == "modes") {
        for (siggen::Modulation kind : siggen::all_modulations()) {
            std::print("{}\n", siggen::modulation_name(kind));
        }
        return {};
    }
    if (command == "wideband") {
        return run_wideband(*options);
    }

    auto kind = siggen::modulation_from_name(command);
    if (!kind) {
        return std::unexpected(kind.error());
    }
    return run_modulator(*kind, *options);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }
    const std::string_view first = argv[1];
    if (first == "-h" || first == "--help" || first == "help") {
        print_usage();
        return 0;
    }

    if (auto status = run(argc, argv); !status) {
        std::print(stderr, "siggen: {}\n", status.error().message);
        return 1;
    }
    return 0;
}
