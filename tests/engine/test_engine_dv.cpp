// The digital voice modes, through the engine, into their decoders.
//
// WHAT THIS FILE IS THE RECORD OF
//
// Until 2026-09-22 a P25p1, Dstar or Tetra receiver was the graph's raw tap:
// one coarse channel copied out of the channel ring, at the channel rate,
// with the carrier wherever the grid's residual left it and nothing filtered
// out. core/decode's three decoders assume the opposite of all three things,
// a carrier at DC, a channel filter and the rate their own filters were
// designed at, and had only ever been fed by their own transmitters at that
// rate in tests/decode. Nothing measured what they made of the tap.
//
// These modes now get the fine stage: the residual mixed to DC, the mode's
// channel filtered, and the result resampled to 48000 S/s for P25 and D-STAR
// and 72000 for TETRA, which is dsp::complex_tap_rate_step and is each
// decoder's own default. This file checks that the engine builds that, and
// then measures both paths on the same capture.
//
// THE ROUND TRIP
//
// Each mode's transmitter in core/dsp/synth/dv_mod.cpp renders a symbol
// stream at its native rate. The test interpolates it to a 2.304 MS/s
// wideband source, puts the carrier 5 kHz off a grid channel's centre, adds
// white noise, writes the result to a cf32 file and opens that through the
// engine's file source, so the path is the real one from the ring upward.
// Two receivers watch the same carrier in the same run: the digital voice
// receiver and a raw tap on the same centre, which is exactly what the old
// path delivered. Each output goes to the mode's decoder configured for that
// stream's rate, and the raw tap is decoded twice: as delivered, and mixed to
// DC on the host by the residual its placement reports, which separates what
// the fine stage's mixer buys from what its filter and resampler buy.
//
// The noise is stated the way tests/decode states it, as the signal to noise
// ratio across the decoder's own sample rate, so the figures here sit beside
// the ones in test_p25p1.cpp, test_dstar.cpp and test_tetra.cpp at the same
// noise density rather than on some other basis.
//
// Every error rate is measured and reported and the assertions are loose, for
// the reason tests/decode/CMakeLists.txt gives: a figure asserted to three
// decimals fails the day somebody improves the receiver.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "core/decode/dstar.h"
#include "core/decode/dv_phy.h"
#include "core/decode/p25p1.h"
#include "core/decode/tetra.h"
#include "core/dsp/pfb.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dv_mod.h"
#include "core/engine/engine.h"
#include "core/engine/vrx.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

namespace {

// ---------------------------------------------------------------------------
// The engine these cases run
// ---------------------------------------------------------------------------

// The canonical test engine, for the cases that only need a receiver built.
constexpr dsp::SampleRate kSceneRate = 2'400'000;

engine::EngineConfig scene_config() {
    engine::EngineConfig config;
    config.channels = 64;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = 32'768;
    config.gpu_index = -1;  // honours REVENANT_GPU_INDEX
    return config;
}

// One NFM emitter in a synthetic scene. What it carries is irrelevant to the
// structural case; it exists so the run has samples to move.
std::string scene_uri(dsp::Hertz offset_hz, dsp::SampleIndex samples) {
    return "synthetic:wideband?rate=" + std::to_string(kSceneRate) +
           "&emitters=1&modes=nfm&seed=616161&noise_dbfs=-120&snr_min=40&snr_max=40" +
           "&samples=" + std::to_string(samples) +
           "&span_low=" + std::to_string(offset_hz - 8000) +
           "&span_high=" + std::to_string(offset_hz + 8000);
}

struct ModeRate {
    engine::Demod mode;
    dsp::SampleRate rate;
};

constexpr ModeRate kDigitalModes[] = {
    {engine::Demod::P25p1, 48'000},
    {engine::Demod::Dstar, 48'000},
    {engine::Demod::Tetra, 72'000},
};

// ---------------------------------------------------------------------------
// The wideband capture the round trips run on
// ---------------------------------------------------------------------------

// 2.304 MS/s over 32 channels puts the channel rate at 144000, three times
// P25's and D-STAR's 48000 and twice TETRA's 72000, and puts each
// transmitter's native rate a whole factor below the source, 48 and 32, so
// the interpolation below is by an integer.
constexpr dsp::SampleRate kWideRate = 2'304'000;
constexpr std::uint32_t kWideChannels = 32;
constexpr dsp::Hertz kWideSpacing = kWideRate / kWideChannels;

// Three channel spacings up and 5 kHz off that channel's centre, so the
// receiver has a residual for its mixer to take out and a raw tap on the same
// channel hands its decoder the carrier 5 kHz off DC.
constexpr dsp::Hertz kResidualHz = 5'000;
constexpr dsp::Hertz kCarrierHz = 3 * kWideSpacing + kResidualHz;

engine::EngineConfig wide_config() {
    engine::EngineConfig config;
    config.channels = kWideChannels;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = 32'768;
    config.gpu_index = -1;
    return config;
}

// Abramowitz and Stegun 9.6.12, for the interpolator's Kaiser window.
[[nodiscard]] double bessel_i0(double x) {
    const double quarter_square = 0.25 * x * x;
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k < 256; ++k) {
        term *= quarter_square / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1e-18 * sum) {
            break;
        }
    }
    return sum;
}

// Band-limited interpolation by a whole factor: zero-stuff, then a Kaiser
// windowed sinc at the output rate, applied polyphase so each output costs
// kTapsPerPhase multiplies. The transmitters render at their decoders' rates
// because their own filters are specified there; this is what lifts one onto
// a wideband source without putting images of it anywhere a receiver looks.
//
// 24 taps per phase at beta 8 is about 80 dB of image rejection with a
// transition of 15 kHz at a factor of 32 and 10 kHz at 48, both far inside
// the gap between the cutoff and the first image.
[[nodiscard]] std::vector<dsp::Complex32> interpolate(std::span<const dsp::Complex32> in,
                                                      std::uint32_t factor, double cutoff_hz,
                                                      dsp::SampleRate out_rate) {
    constexpr std::size_t kTapsPerPhase = 24;
    constexpr double kBeta = 8.0;

    const std::size_t length = kTapsPerPhase * factor;
    const double centre = (static_cast<double>(length) - 1.0) / 2.0;
    const double cutoff = cutoff_hz / static_cast<double>(out_rate);
    const double i0_beta = bessel_i0(kBeta);

    std::vector<double> taps(length, 0.0);
    double sum = 0.0;
    for (std::size_t i = 0; i < length; ++i) {
        const double position = static_cast<double>(i) - centre;
        const double ratio = 2.0 * static_cast<double>(i) / static_cast<double>(length - 1) - 1.0;
        const double window =
            bessel_i0(kBeta * std::sqrt(std::max(0.0, 1.0 - ratio * ratio))) / i0_beta;
        const double argument = 2.0 * cutoff * position;
        const double sinc = (argument == 0.0)
                                ? 1.0
                                : std::sin(std::numbers::pi * argument) /
                                      (std::numbers::pi * argument);
        taps[i] = 2.0 * cutoff * sinc * window;
        sum += taps[i];
    }
    // Unit gain in the passband after zero-stuffing, which divided the
    // signal's level by the factor.
    const double scale = static_cast<double>(factor) / sum;

    std::vector<dsp::Complex32> out(in.size() * factor);
    for (std::size_t m = 0; m < out.size(); ++m) {
        std::complex<double> accumulated{0.0, 0.0};
        for (std::size_t k = m / factor + 1U; k-- > 0U;) {
            const std::size_t offset = m - k * factor;
            if (offset >= length) {
                break;
            }
            accumulated += taps[offset] * std::complex<double>(in[k]);
        }
        accumulated *= scale;
        out[m] = dsp::Complex32{static_cast<float>(accumulated.real()),
                                static_cast<float>(accumulated.imag())};
    }
    return out;
}

// Multiplies by exp(+j*2*pi*f*n/rate), with the phase reduced exactly in
// integers so a long capture does not lose it to a large argument.
void shift_by(std::span<dsp::Complex32> samples, dsp::Hertz frequency, dsp::SampleRate rate) {
    for (std::size_t n = 0; n < samples.size(); ++n) {
        std::int64_t turns = (frequency * static_cast<std::int64_t>(n)) % rate;
        if (turns < 0) {
            turns += rate;
        }
        const double angle =
            2.0 * std::numbers::pi * static_cast<double>(turns) / static_cast<double>(rate);
        const std::complex<double> rotated =
            std::complex<double>(samples[n]) * std::polar(1.0, angle);
        samples[n] = dsp::Complex32{static_cast<float>(rotated.real()),
                                    static_cast<float>(rotated.imag())};
    }
}

// A rendered transmission, lifted to the wideband rate, placed at the carrier
// and buried in noise at the given ratio across `reference_hz`.
[[nodiscard]] std::vector<dsp::Complex32> wideband_capture(
    std::span<const dsp::Complex32> native, dsp::SampleRate native_rate, double snr_db,
    std::uint64_t seed) {
    REQUIRE(kWideRate % native_rate == 0);
    const auto factor = static_cast<std::uint32_t>(kWideRate / native_rate);

    // 0.35 of the native rate: above every mode's occupied half-width, 6 kHz
    // for P25 and 12.15 kHz for TETRA, and below the first image at the
    // native rate less that half-width.
    auto capture = interpolate(native, factor, 0.35 * static_cast<double>(native_rate), kWideRate);
    shift_by(capture, kCarrierHz, kWideRate);

    // Across the decoder's own rate, which is the full-sample-rate basis
    // tests/decode measures at: the same noise density, so the same figure.
    const auto level = siggen::NoiseLevel::snr_in_reference_bandwidth_db(snr_db, native_rate);
    auto report = siggen::add_awgn(capture, level, kWideRate, seed);
    INFO(test::message_of(report));
    REQUIRE(report.has_value());
    return capture;
}

// What one engine run hands back for one capture: the digital voice
// receiver's stream and a raw tap's on the same centre.
struct Captured {
    std::vector<dsp::Complex32> fine;
    dsp::SampleRate fine_rate = 0;
    std::vector<dsp::Complex32> raw;
    dsp::SampleRate raw_rate = 0;

    // Where the raw tap's channel left the carrier, from the placement.
    double raw_residual_hz = 0.0;
};

// `block_samples` overrides wide_config()'s engine block when it is not zero.
[[nodiscard]] Captured through_engine(std::span<const dsp::Complex32> capture,
                                      engine::Demod mode, const std::string& tag,
                                      std::uint32_t block_samples = 0) {
    // Unique per run as well as per case. %TEMP% is shared by every build of
    // this tree on the machine, and two runs writing one fixed name is how
    // tests/rpc/test_rpc_rds.cpp's end-to-end case fails when two checkouts
    // test at once.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        std::format("revenant_test_dv_{}_{}.cf32", tag, stamp);
    {
        std::FILE* file = std::fopen(path.string().c_str(), "wb");
        REQUIRE(file != nullptr);
        const std::size_t wrote =
            std::fwrite(capture.data(), sizeof(dsp::Complex32), capture.size(), file);
        std::fclose(file);
        REQUIRE(wrote == capture.size());
    }
    struct Remove {
        std::filesystem::path path;
        ~Remove() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } remove{path};

    // A UHF centre, so the file source's resolution request is not the HF
    // one that widens the grid; the offsets below are baseband regardless.
    const std::string uri = "file:///" + path.generic_string() +
                            "?rate=" + std::to_string(kWideRate) +
                            "&format=cf32&center=420000000";

    // Declared before the engine so they outlive it: the sinks below hold
    // references to both, and locals are destroyed in reverse order.
    Captured out;
    std::mutex lock;

    engine::EngineConfig config = wide_config();
    if (block_samples != 0) {
        config.block_samples = block_samples;
    }
    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;

    const auto opened = eng.open_source(uri);
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());
    REQUIRE(eng.info().channel_rate == 2 * kWideSpacing);

    engine::VrxParams fine_params;
    fine_params.center = kCarrierHz;
    fine_params.demod = mode;
    fine_params.bandwidth = 0;
    const auto fine_id = eng.add_vrx(fine_params);
    INFO(test::message_of(fine_id));
    REQUIRE(fine_id.has_value());

    engine::VrxParams raw_params;
    raw_params.center = kCarrierHz;
    raw_params.demod = engine::Demod::Raw;
    const auto raw_id = eng.add_vrx(raw_params);
    INFO(test::message_of(raw_id));
    REQUIRE(raw_id.has_value());

    auto collect = [&lock](std::vector<dsp::Complex32>& into, dsp::SampleRate& rate) {
        return [&lock, &into, &rate](const engine::AudioChunk& chunk) -> Status {
            if (chunk.channels != 2U) {
                return fail("a complex tap delivered something other than I/Q pairs");
            }
            const std::lock_guard<std::mutex> guard(lock);
            rate = chunk.rate;
            for (std::size_t i = 0; i + 1U < chunk.samples.size(); i += 2U) {
                into.push_back(dsp::Complex32{chunk.samples[i], chunk.samples[i + 1U]});
            }
            return {};
        };
    };
    REQUIRE(eng.set_audio_sink(*fine_id, collect(out.fine, out.fine_rate)).has_value());
    REQUIRE(eng.set_audio_sink(*raw_id, collect(out.raw, out.raw_rate)).has_value());

    const auto ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    const auto raw_status = eng.vrx_status(*raw_id);
    REQUIRE(raw_status.has_value());
    out.raw_residual_hz = static_cast<double>(raw_status->placement.residual_numerator) /
                          static_cast<double>(raw_status->placement.residual_denominator);

    const std::lock_guard<std::mutex> guard(lock);
    return out;
}

// The raw tap mixed to DC on the host, by the residual its placement names,
// which is the one thing the fine stage does that a decoder could not have
// done for itself from the tap.
[[nodiscard]] std::vector<dsp::Complex32> mixed_to_dc(std::span<const dsp::Complex32> samples,
                                                      dsp::SampleRate rate,
                                                      double residual_hz) {
    std::vector<dsp::Complex32> out(samples.begin(), samples.end());
    for (std::size_t n = 0; n < out.size(); ++n) {
        const double turns =
            std::fmod(residual_hz * static_cast<double>(n) / static_cast<double>(rate), 1.0);
        const std::complex<double> rotated =
            std::complex<double>(out[n]) * std::polar(1.0, -2.0 * std::numbers::pi * turns);
        out[n] = dsp::Complex32{static_cast<float>(rotated.real()),
                                static_cast<float>(rotated.imag())};
    }
    return out;
}

// A decoder's receive filter is a tap count at the rate it was designed for,
// so a stream at another rate gets the same span in time: the raw tap at
// 144000 gets three times P25's 121 taps rather than a filter a third as
// long.
[[nodiscard]] std::size_t taps_for(std::size_t default_taps, dsp::SampleRate default_rate,
                                   dsp::SampleRate rate) {
    auto taps = static_cast<std::size_t>(std::llround(
        static_cast<double>(default_taps) * static_cast<double>(rate) /
        static_cast<double>(default_rate)));
    if (taps % 2U == 0U) {
        ++taps;
    }
    return taps;
}

struct Score {
    bool locked = false;
    std::size_t errors = 0;
    std::size_t compared = 0;
    std::string note;

    [[nodiscard]] double rate() const {
        return compared == 0 ? 1.0 : static_cast<double>(errors) / static_cast<double>(compared);
    }

    [[nodiscard]] std::string describe() const {
        if (!locked) {
            return "no lock, " + note;
        }
        return std::format("{:.4f}, {} of {}{}", rate(), errors, compared,
                           note.empty() ? "" : ", " + note);
    }
};

// test_p25p1.cpp's measurement, unchanged: the stream is found by correlating
// the first 128 sent symbols, and errors are counted after the first 64,
// where the timing estimator's first window has settled.
[[nodiscard]] Score p25_score(std::span<const dsp::Complex32> samples, dsp::SampleRate rate,
                              std::span<const std::uint8_t> sent) {
    Score score;

    decode::P25Config config;
    config.rate = rate;
    config.filter_taps = taps_for(decode::P25Config{}.filter_taps, decode::P25Config{}.rate, rate);
    auto decoder = decode::P25Phase1::create(config);
    INFO(test::message_of(decoder));
    REQUIRE(decoder.has_value());

    std::vector<decode::P25Frame> frames;
    REQUIRE(decoder->process(samples, frames).has_value());

    const std::span<const float> recovered = decoder->last_symbols();
    if (recovered.size() < sent.size() / 2) {
        score.note = std::format("{} symbols recovered of {}", recovered.size(), sent.size());
        return score;
    }

    std::vector<float> reference(sent.size(), 0.0F);
    for (std::size_t i = 0; i < sent.size(); ++i) {
        reference[i] = static_cast<float>(decode::kP25DibitToSymbol[sent[i]]);
    }
    auto hit = decode::correlate_pattern(recovered, std::span<const float>(reference.data(), 128));
    REQUIRE(hit.has_value());
    if (std::abs(hit->score) <= 0.5) {
        score.note = std::format("the sent symbols correlate at {:.2f} at best", hit->score);
        return score;
    }
    score.locked = true;

    constexpr std::size_t kSettled = 64;
    const std::size_t compare = std::min(sent.size(), recovered.size() - hit->offset);
    for (std::size_t i = kSettled; i < compare; ++i) {
        float value = recovered[hit->offset + i];
        if (hit->inverted) {
            value = -value;
        }
        std::uint8_t got = 0;
        if (value >= 2.0F) {
            got = 0b01;
        } else if (value >= 0.0F) {
            got = 0b00;
        } else if (value >= -2.0F) {
            got = 0b10;
        } else {
            got = 0b11;
        }
        ++score.compared;
        score.errors += static_cast<std::size_t>(got != sent[i]);
    }
    return score;
}

// test_dstar.cpp's measurement, unchanged: found by correlating the first
// 128 sent bits, every bit after that compared.
[[nodiscard]] Score dstar_score(std::span<const dsp::Complex32> samples, dsp::SampleRate rate,
                                std::span<const std::uint8_t> sent) {
    Score score;

    decode::DStarConfig config;
    config.rate = rate;
    config.filter_taps =
        taps_for(decode::DStarConfig{}.filter_taps, decode::DStarConfig{}.rate, rate);
    auto decoder = decode::DStar::create(config);
    INFO(test::message_of(decoder));
    REQUIRE(decoder.has_value());

    std::vector<decode::DStarTransmission> transmissions;
    REQUIRE(decoder->process(samples, transmissions).has_value());

    const std::span<const std::uint8_t> got = decoder->last_bits();
    if (got.size() < sent.size() / 2) {
        score.note = std::format("{} bits recovered of {}", got.size(), sent.size());
        return score;
    }

    std::vector<float> recovered(got.size(), 0.0F);
    for (std::size_t i = 0; i < got.size(); ++i) {
        recovered[i] = got[i] ? 1.0F : -1.0F;
    }
    std::vector<float> head(128, 0.0F);
    for (std::size_t i = 0; i < head.size(); ++i) {
        head[i] = sent[i] ? 1.0F : -1.0F;
    }
    auto hit = decode::correlate_pattern(recovered, head);
    REQUIRE(hit.has_value());
    if (std::abs(hit->score) <= 0.5) {
        score.note = std::format("the sent bits correlate at {:.2f} at best", hit->score);
        return score;
    }
    score.locked = true;

    const std::size_t compare = std::min(sent.size(), recovered.size() - hit->offset);
    for (std::size_t i = 0; i < compare; ++i) {
        const std::uint8_t value = hit->inverted
                                       ? static_cast<std::uint8_t>(1U - got[hit->offset + i])
                                       : got[hit->offset + i];
        ++score.compared;
        score.errors += static_cast<std::size_t>(value != sent[i]);
    }
    return score;
}

// test_tetra.cpp's burst, which carries the payload in block 2 of a
// synchronisation burst because a bare pi/4-DQPSK stream has nothing for the
// burst decoder to lock onto.
[[nodiscard]] decode::TetraSyncPdu tetra_pdu(std::size_t burst) {
    decode::TetraSyncPdu pdu;
    pdu.system_code = 0b0011;
    pdu.colour_code = 37;
    pdu.timeslot = 0;
    pdu.frame_number = static_cast<std::uint8_t>(1 + (burst % 18));
    pdu.multiframe_number = 42;
    pdu.sharing_mode = 0;
    pdu.reserved_frames = 0;
    pdu.uplane_dtx_allowed = true;
    pdu.frame18_extension = false;
    pdu.mobile_country_code = 234;
    pdu.mobile_network_code = 1'234;
    pdu.neighbour_cell_broadcast = 1;
    pdu.cell_load = 2;
    pdu.late_entry_supported = true;
    return pdu;
}

// test_tetra.cpp's measurement: each recovered burst matched to the one it
// came from by where it started, and block 2 compared bit for bit. A burst
// whose training sequence never correlated has no bits to be wrong and is
// reported in the note rather than folded into the rate.
[[nodiscard]] Score tetra_score(std::span<const dsp::Complex32> samples, dsp::SampleRate rate,
                                std::span<const std::uint8_t> sent, std::size_t bursts) {
    Score score;
    constexpr std::size_t kPayload = decode::kTetraSyncBurstBlock2.length;

    decode::TetraConfig config;
    config.rate = rate;
    config.filter_taps =
        taps_for(decode::TetraConfig{}.filter_taps, decode::TetraConfig{}.rate, rate);
    auto decoder = decode::Tetra::create(config);
    INFO(test::message_of(decoder));
    REQUIRE(decoder.has_value());

    std::vector<decode::TetraBurst> found;
    REQUIRE(decoder->process(samples, found).has_value());

    std::size_t matched = 0;
    std::vector<bool> seen(bursts, false);
    for (const decode::TetraBurst& burst : found) {
        // Rounded to the nearest burst rather than truncated: the engine's
        // filters move the stream by a few symbols against where the
        // transmitter put it, in either direction.
        const std::size_t index =
            (burst.first_symbol + decode::kTetraBurstSymbols / 2U) / decode::kTetraBurstSymbols;
        if (index >= bursts || seen[index]) {
            continue;
        }
        seen[index] = true;
        ++matched;
        for (std::size_t i = 0; i < kPayload; ++i) {
            ++score.compared;
            score.errors += static_cast<std::size_t>(
                burst.bits[decode::kTetraSyncBurstBlock2.offset + i] != sent[index * kPayload + i]);
        }
    }
    score.locked = matched > 0;
    score.note = std::format("{} of {} bursts found", matched, bursts);
    if (matched > 0 && matched < bursts) {
        std::string missing;
        for (std::size_t i = 0; i < bursts; ++i) {
            if (!seen[i]) {
                missing += (missing.empty() ? "" : " ") + std::to_string(i);
            }
        }
        score.note += ", missing " + missing;
    }
    return score;
}

}  // namespace

// ---------------------------------------------------------------------------
// The receiver the engine builds
// ---------------------------------------------------------------------------

TEST_CASE("a digital voice receiver is a fine stage at its decoder's rate",
          "[gpu][engine][dv]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    auto created = engine::Engine::create(scene_config());
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;

    // Five channel spacings and a bit, so every receiver has a residual for
    // its mixer to take out.
    constexpr dsp::Hertz kCentre = 37'500 * 5 + 4'000;
    REQUIRE(eng.open_source(scene_uri(kCentre, 400'000)).has_value());

    struct Seen {
        std::mutex lock;
        std::uint64_t frames = 0;
        dsp::SampleRate rate = 0;
        std::uint32_t channels = 0;
    };
    std::vector<std::unique_ptr<Seen>> seen;
    std::vector<engine::VrxId> ids;

    for (const ModeRate& want : kDigitalModes) {
        INFO("mode " << engine::demod_name(want.mode));
        engine::VrxParams params;
        params.center = kCentre;
        params.demod = want.mode;
        params.bandwidth = 0;  // the mode's own channel

        const auto added = eng.add_vrx(params);
        INFO(test::message_of(added));
        REQUIRE(added.has_value());
        ids.push_back(*added);

        // The status names the rate the stream will arrive at. Before the
        // planner accepted these modes this was zero: vrx_shape_for refused
        // them, so the graph never learned a shape for the receiver at all.
        const auto status = eng.vrx_status(*added);
        REQUIRE(status.has_value());
        CHECK(status->demod_rate == want.rate);

        // Moving the dial is a push constant and a new tap table, the same
        // as for any receiver. It was refused on every one of these modes,
        // because Graph::set_vrx_params asks the planner whether the retune
        // changes the pipeline's shape and the planner refused the mode.
        engine::VrxParams nudged = params;
        nudged.center = kCentre + 250;
        const auto retuned = eng.set_vrx_params(*added, nudged);
        INFO(test::message_of(retuned));
        CHECK(retuned.has_value());

        auto record = std::make_unique<Seen>();
        Seen* target = record.get();
        seen.push_back(std::move(record));
        REQUIRE(eng.set_audio_sink(*added, [target](const engine::AudioChunk& chunk) -> Status {
                       const std::lock_guard<std::mutex> guard(target->lock);
                       target->rate = chunk.rate;
                       target->channels = chunk.channels;
                       target->frames += chunk.samples.size() / 2U;
                       return {};
                   }).has_value());
    }

    const auto ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    for (std::size_t i = 0; i < ids.size(); ++i) {
        const ModeRate& want = kDigitalModes[i];
        INFO("mode " << engine::demod_name(want.mode));
        const std::lock_guard<std::mutex> guard(seen[i]->lock);

        // Complex pairs at the decoder's rate, and not the channel rate the
        // raw tap would have delivered, which on this grid is 75000.
        CHECK(seen[i]->channels == 2U);
        CHECK(seen[i]->rate == want.rate);

        // 400000 samples at 2.4 MS/s is a sixth of a second, so a sixth of
        // the rate in frames, less the filters' support at the front.
        const double expected = static_cast<double>(want.rate) * 400'000.0 /
                                static_cast<double>(kSceneRate);
        INFO(seen[i]->frames << " frames, " << expected << " expected at most");
        CHECK(static_cast<double>(seen[i]->frames) > 0.8 * expected);
        CHECK(static_cast<double>(seen[i]->frames) <= expected + 1.0);
    }
}

TEST_CASE("a digital voice receiver cut below its own channel is refused", "[engine][dv]") {
    // The three modes joined engine::clamp_breaks_demodulator when they got a
    // fine stage, because a passband that is applied can now take part of
    // the signal away, and a decoder handed a truncated channel reads it as
    // symbol errors at full strength. 256 channels on 2.4 MS/s guarantee
    // 9375 Hz to a receiver placed anywhere, which carries D-STAR's 6 kHz and
    // neither P25's 12.5 nor TETRA's 25.
    constexpr dsp::GridParams kFine{
        .channels = 256,
        .taps_per_branch = 17,
        .decimation = 128,
    };

    struct Case {
        engine::Demod mode;
        bool fits;
    };
    const Case cases[] = {
        {engine::Demod::P25p1, false},
        {engine::Demod::Dstar, true},
        {engine::Demod::Tetra, false},
        // The raw tap is still not a demodulator and is narrowed rather
        // than refused.
        {engine::Demod::Raw, true},
    };

    for (const Case& want : cases) {
        INFO("mode " << engine::demod_name(want.mode));
        engine::VrxParams params;
        // Half a channel spacing off a centre, where one channel carries the
        // least.
        params.center = 9'375 * 7 + 4'687;
        params.demod = want.mode;
        params.bandwidth = 0;
        if (want.mode == engine::Demod::Raw) {
            params.bandwidth = 25'000;
        }

        const auto placed = engine::place(kFine, kSceneRate, params);
        INFO(test::message_of(placed));
        CHECK(placed.has_value() == want.fits);
        if (!want.fits && !placed.has_value()) {
            // Named as a decoder problem rather than as audio, since none of
            // these three ends in audio.
            CHECK(placed.error().message.find("symbols smeared") != std::string::npos);
            CHECK(placed.error().message.find("--channels") != std::string::npos);
        }
    }
}

// ---------------------------------------------------------------------------
// The round trips
// ---------------------------------------------------------------------------
//
// One case per mode, two signal to noise ratios each: 30 dB, where the
// decoders in tests/decode make no errors, and the low point each of those
// tests uses. Every figure goes out in a WARN line so a passing run still
// prints it, and the one assertion per point holds the fine stage to the
// allowance the matching tests/decode case holds the decoder to on its own.
// The raw tap is measured and reported and not asserted: it is what the
// engine used to hand these decoders, and a number for it is the point.

TEST_CASE("P25 Phase 1 through the engine, the fine stage against the raw tap",
          "[gpu][engine][dv]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // test_p25p1.cpp's pseudorandom dibit stream, same seed and length.
    constexpr std::size_t kSymbols = 6'000;
    std::vector<std::uint8_t> sent(kSymbols, 0);
    std::uint64_t state = 0x5EED'1234'ABCD'0001ULL;
    for (std::uint8_t& dibit : sent) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        dibit = static_cast<std::uint8_t>((state >> 33U) & 0x3U);
    }

    constexpr dsp::SampleRate kNative = decode::P25Config{}.rate;

    // Measured on 2026-09-22 on the RTX 4090, symbol errors after the first 64
    // symbols:
    //
    //                   fine stage       raw tap          raw tap mixed
    //                   48000 S/s        144000 S/s       to DC on host
    //   30 dB           0 of 5913        4 of 5911        0 of 5911
    //    4 dB           41 of 5913       4186 of 5913     2442 of 5910
    //                   0.0069           0.708            0.413
    //
    // test_p25p1.cpp measures 0.047 at 4 dB with the decoder fed straight
    // from the transmitter, at the same noise density. The fine stage is
    // seven times better than that because its channel filter takes out the
    // 35 kHz of noise between the channel edge and the decoder's Nyquist
    // before the discriminator sees it, which is where an FM receiver's
    // threshold is decided. The raw tap hands the discriminator 144 kHz of
    // it, three times what the decode test does, and loses the frame.
    struct Point {
        double snr_db;
        double allowed_symbol_error_rate;
    };
    const Point points[] = {{30.0, 0.001}, {4.0, 0.08}};

    for (const Point& point : points) {
        INFO("signal to noise " << point.snr_db << " dB across " << kNative << " Hz");

        siggen::P25ModConfig mod;
        mod.rate = kNative;
        auto native = siggen::p25_render_dibits(mod, sent);
        INFO(test::message_of(native));
        REQUIRE(native.has_value());

        const auto capture =
            wideband_capture(*native, kNative, point.snr_db, 0x9E37'79B9'7F4A'7C15ULL);
        const Captured got = through_engine(capture, engine::Demod::P25p1,
                                            std::format("p25_{}", point.snr_db));
        CHECK(got.fine_rate == kNative);
        CHECK(got.raw_rate == 2 * kWideSpacing);

        const Score fine = p25_score(got.fine, got.fine_rate, sent);
        const Score raw = p25_score(got.raw, got.raw_rate, sent);
        const Score raw_mixed =
            p25_score(mixed_to_dc(got.raw, got.raw_rate, got.raw_residual_hz), got.raw_rate, sent);

        WARN(std::format(
            "p25p1 at {} dB across {} Hz, symbol error rate after the first 64 symbols. Fine "
            "stage at {} S/s: {}. Raw tap at {} S/s as delivered, carrier {} Hz off DC: {}. "
            "Raw tap mixed to DC on the host: {}",
            point.snr_db, kNative, got.fine_rate, fine.describe(), got.raw_rate,
            got.raw_residual_hz, raw.describe(), raw_mixed.describe()));

        REQUIRE(fine.locked);
        CHECK(fine.rate() <= point.allowed_symbol_error_rate);
    }
}

TEST_CASE("D-STAR through the engine, the fine stage against the raw tap",
          "[gpu][engine][dv]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // test_dstar.cpp's pseudorandom bit stream and seed, at half its length:
    // 6000 bits is 1.25 s of air, the same as the P25 case, and the 2.304
    // MS/s capture that carries it is already 23 MB.
    constexpr std::size_t kBits = 6'000;
    std::vector<std::uint8_t> sent(kBits, 0);
    std::uint64_t state = 0xD57A'0F0F'1234'0001ULL;
    for (std::uint8_t& bit : sent) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        bit = static_cast<std::uint8_t>((state >> 37U) & 1ULL);
    }

    constexpr dsp::SampleRate kNative = decode::DStarConfig{}.rate;

    // Measured on 2026-09-23 on the RTX 4090, bit errors:
    //
    //                   fine stage       raw tap          raw tap mixed
    //                   48000 S/s        144000 S/s       to DC on host
    //   30 dB           0 of 5982        0 of 5977        0 of 5978
    //    2 dB           14 of 5982       no lock          no lock
    //                   0.0023
    //
    // test_dstar.cpp measures 0.056 at 2 dB at the same noise density, for
    // the reason the P25 case gives. At 2 dB the raw tap's stream correlates
    // with what was sent at 0.36 at best as delivered and 0.38 mixed to DC:
    // the discriminator is past its threshold on 144 kHz of noise and the
    // loss is not the carrier offset. At 30 dB the 5 kHz offset costs this
    // decoder nothing.
    //
    // WHAT THIS TABLE USED TO READ, measured on 2026-09-22 before "Decode
    // D-STAR the same however its input is blocked": the raw tap at 30 dB was
    // 0 of 5978, and the sentence after it said the raw tap's stream
    // "correlates with what was sent at 0.38 at best, mixed or not". With that
    // change the bits the raw tap yields moved by one and its best
    // correlation at 2 dB by 0.02, and no count of errors moved.
    struct Point {
        double snr_db;
        double allowed_bit_error_rate;
    };
    const Point points[] = {{30.0, 0.002}, {2.0, 0.09}};

    for (const Point& point : points) {
        INFO("signal to noise " << point.snr_db << " dB across " << kNative << " Hz");

        siggen::DStarModConfig mod;
        mod.rate = kNative;
        auto native = siggen::dstar_render_bits(mod, sent);
        INFO(test::message_of(native));
        REQUIRE(native.has_value());

        const auto capture =
            wideband_capture(*native, kNative, point.snr_db, 0x1234'5678'9ABC'DEF0ULL);
        const Captured got = through_engine(capture, engine::Demod::Dstar,
                                            std::format("dstar_{}", point.snr_db));
        CHECK(got.fine_rate == kNative);
        CHECK(got.raw_rate == 2 * kWideSpacing);

        const Score fine = dstar_score(got.fine, got.fine_rate, sent);
        const Score raw = dstar_score(got.raw, got.raw_rate, sent);
        const Score raw_mixed = dstar_score(
            mixed_to_dc(got.raw, got.raw_rate, got.raw_residual_hz), got.raw_rate, sent);

        WARN(std::format(
            "dstar at {} dB across {} Hz, bit error rate. Fine stage at {} S/s: {}. Raw tap at "
            "{} S/s as delivered, carrier {} Hz off DC: {}. Raw tap mixed to DC on the host: {}",
            point.snr_db, kNative, got.fine_rate, fine.describe(), got.raw_rate,
            got.raw_residual_hz, raw.describe(), raw_mixed.describe()));

        REQUIRE(fine.locked);
        CHECK(fine.rate() <= point.allowed_bit_error_rate);
    }
}

TEST_CASE("TETRA through the engine, the fine stage against the raw tap",
          "[gpu][engine][dv]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // test_tetra.cpp's arrangement: 24 synchronisation bursts with a
    // pseudorandom payload in block 2, same seeds.
    constexpr std::size_t kBursts = 24;
    constexpr std::size_t kPayload = decode::kTetraSyncBurstBlock2.length;

    std::vector<std::uint8_t> sent(kBursts * kPayload, 0);
    std::uint64_t state = 0x7E77'A000'1234'0001ULL;
    for (std::uint8_t& bit : sent) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        bit = static_cast<std::uint8_t>((state >> 41U) & 1ULL);
    }

    std::vector<std::uint8_t> stream;
    stream.reserve(kBursts * decode::kTetraBurstBits);
    for (std::size_t burst = 0; burst < kBursts; ++burst) {
        auto bits = siggen::tetra_sync_burst_bits(tetra_pdu(burst), 0xA11CE + burst);
        INFO(test::message_of(bits));
        REQUIRE(bits.has_value());
        for (std::size_t i = 0; i < kPayload; ++i) {
            (*bits)[decode::kTetraSyncBurstBlock2.offset + i] = sent[burst * kPayload + i];
        }
        stream.insert(stream.end(), bits->begin(), bits->end());
    }

    constexpr dsp::SampleRate kNative = decode::TetraConfig{}.rate;

    // Measured on 2026-09-22 on the RTX 4090, block 2 bit errors:
    //
    //                   fine stage       raw tap          raw tap mixed
    //                   72000 S/s        144000 S/s       to DC on host
    //   30 dB           0 of 4968        no lock          0 of 4968
    //                   23 of 24 bursts  0 of 24 bursts   23 of 24 bursts
    //    2 dB           162 of 4320      no lock          158 of 4320
    //                   0.0375           0 of 24 bursts   0.0366
    //                   20 of 24 bursts                   20 of 24 bursts
    //
    // test_tetra.cpp measures 0.028 at 2 dB at the same density, on 18 bursts.
    // Here the whole difference between the two engine paths is the mixer:
    // pi/4-DQPSK is differentially detected symbol to symbol, a 5 kHz offset
    // turns every symbol a further 100 degrees at 18000 symbols per second,
    // and no training sequence correlates. Mixed to DC on the host the raw
    // tap does as well as the fine stage, because the decoder's matched
    // filter already rejects the noise a channel filter would. The burst lost
    // at 30 dB is the last one, number 23, on both paths that lock, so it is
    // the end of the capture and not the fine stage: the chain's filter
    // delays leave the tail of that burst outside the samples delivered.
    // At 2 dB both lose 3, 6, 18 and 23.
    struct Point {
        double snr_db;
        double allowed_bit_error_rate;
    };
    const Point points[] = {{30.0, 0.002}, {2.0, 0.05}};

    for (const Point& point : points) {
        INFO("signal to noise " << point.snr_db << " dB across " << kNative << " Hz");

        siggen::TetraModConfig mod;
        mod.rate = kNative;
        auto native = siggen::tetra_render_bits(mod, stream);
        INFO(test::message_of(native));
        REQUIRE(native.has_value());

        const auto capture =
            wideband_capture(*native, kNative, point.snr_db, 0x0F1E'2D3C'4B5A'6978ULL);
        const Captured got = through_engine(capture, engine::Demod::Tetra,
                                            std::format("tetra_{}", point.snr_db));
        CHECK(got.fine_rate == kNative);
        CHECK(got.raw_rate == 2 * kWideSpacing);

        const Score fine = tetra_score(got.fine, got.fine_rate, sent, kBursts);
        const Score raw = tetra_score(got.raw, got.raw_rate, sent, kBursts);
        const Score raw_mixed = tetra_score(
            mixed_to_dc(got.raw, got.raw_rate, got.raw_residual_hz), got.raw_rate, sent, kBursts);

        WARN(std::format(
            "tetra at {} dB across {} Hz, block 2 bit error rate. Fine stage at {} S/s: {}. Raw "
            "tap at {} S/s as delivered, carrier {} Hz off DC: {}. Raw tap mixed to DC on the "
            "host: {}",
            point.snr_db, kNative, got.fine_rate, fine.describe(), got.raw_rate,
            got.raw_residual_hz, raw.describe(), raw_mixed.describe()));

        REQUIRE(fine.locked);
        CHECK(fine.rate() <= point.allowed_bit_error_rate);
    }
}

TEST_CASE("P25 through the engine decodes the same at 16384 and 65536-sample blocks",
          "[gpu][engine][dv]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // On 2026-09-22 tests/rpc/test_rpc_decode.cpp's six headers came back four
    // at a time through 16384-sample engine blocks and one at a time through
    // 65536. Two things had to hold for that to go away, and this case holds
    // both, one stage apart. The fine stage hands the receiver the same
    // samples whatever the engine's block, compared here sample for sample;
    // and P25Phase1 decodes those samples the same however they are split,
    // here once whole and once in the pieces the engine delivers them in.
    //
    // Measured on the RTX 4090 the day the decoder stopped depending on its
    // blocking: the two streams are identical, 76095 samples each, and every
    // one of the four decodes gives five of the six headers, the same five.
    // The one missing is the first, which starts at the carrier's first
    // symbol. tests/rpc/test_rpc_decode.cpp's capture, the same six headers
    // through a 288000 S/s grid, gives all six; why this one loses the first
    // is not established.
    std::vector<std::uint8_t> dibits;
    for (int round = 0; round < 3; ++round) {
        for (const bool encrypted : {false, true}) {
            siggen::P25HeaderMessage message;
            message.network_access_code = 0x293;
            decode::P25Header header;
            for (std::size_t i = 0; i < header.message_indicator.size(); ++i) {
                header.message_indicator[i] =
                    encrypted ? static_cast<std::uint8_t>(0x11 * (i + 1)) : std::uint8_t{0};
            }
            header.algorithm_id = encrypted ? std::uint8_t{0x84} : decode::kP25AlgidUnencrypted;
            header.key_id = encrypted ? std::uint16_t{0x1234} : std::uint16_t{0};
            header.talkgroup_id = encrypted ? std::uint16_t{0x0100} : std::uint16_t{0x02A7};
            message.header = header;
            auto one = siggen::p25_header_message_dibits(message);
            REQUIRE(one.has_value());
            dibits.insert(dibits.end(), one->begin(), one->end());
        }
    }
    constexpr dsp::SampleRate kNative = decode::P25Config{}.rate;
    siggen::P25ModConfig mod;
    mod.rate = kNative;
    auto native = siggen::p25_render_dibits(mod, dibits);
    INFO(test::message_of(native));
    REQUIRE(native.has_value());

    // Half a second of nothing either side, as the rpc case's file has, so
    // the transmission starts and stops inside the capture.
    std::vector<dsp::Complex32> padded(kNative / 2, dsp::Complex32{});
    padded.insert(padded.end(), native->begin(), native->end());
    padded.insert(padded.end(), kNative / 2, dsp::Complex32{});

    const auto factor = static_cast<std::uint32_t>(kWideRate / kNative);
    auto capture = interpolate(padded, factor, 0.35 * static_cast<double>(kNative), kWideRate);
    shift_by(capture, kCarrierHz, kWideRate);

    // One line per data unit, everything a caller reads off it.
    const auto decode_in_steps = [](std::span<const dsp::Complex32> stream, std::size_t step) {
        auto decoder = decode::P25Phase1::create(decode::P25Config{});
        REQUIRE(decoder.has_value());
        std::vector<decode::P25Frame> frames;
        for (std::size_t at = 0; at < stream.size(); at += step) {
            const std::size_t count = std::min(step, stream.size() - at);
            REQUIRE(decoder->process(stream.subspan(at, count), frames).has_value());
        }
        std::vector<std::string> lines;
        for (const decode::P25Frame& frame : frames) {
            lines.push_back(std::format(
                "{} nac {:03x} at {} score {:.17g}{}", decode::p25_duid_name(frame.nid.duid),
                frame.nid.network_access_code, frame.first_symbol, frame.sync_score,
                frame.header ? std::format(" tg {:04x} algid {:02x}", frame.header->talkgroup_id,
                                           frame.header->algorithm_id)
                             : std::string{}));
        }
        return lines;
    };
    const auto headers_in = [](const std::vector<std::string>& lines) {
        return static_cast<std::size_t>(std::ranges::count_if(
            lines, [](const std::string& line) { return line.find(" tg ") != std::string::npos; }));
    };

    constexpr std::uint32_t kBlocks[] = {16'384, 65'536};
    std::vector<std::vector<dsp::Complex32>> streams;
    std::vector<std::vector<std::string>> decodes;
    for (const std::uint32_t block : kBlocks) {
        const Captured got =
            through_engine(capture, engine::Demod::P25p1, std::format("p25_block_{}", block), block);
        REQUIRE(got.fine_rate == kNative);

        // The engine hands a receiver one block's worth at a time, which at
        // the tap's rate is the block over the decimation.
        const std::size_t chunk = block / factor;
        decodes.push_back(decode_in_steps(got.fine, got.fine.size()));
        decodes.push_back(decode_in_steps(got.fine, chunk));
        WARN(std::format("p25p1 through {}-sample engine blocks: {} samples at {} S/s, {} of 6 "
                         "headers decoded whole and {} in {}-sample pieces",
                         block, got.fine.size(), got.fine_rate, headers_in(decodes[decodes.size() - 2]),
                         headers_in(decodes.back()), chunk));
        streams.push_back(got.fine);
    }

    REQUIRE(streams[0].size() == streams[1].size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < streams[0].size(); ++i) {
        differing += streams[0][i] != streams[1][i] ? 1U : 0U;
    }
    INFO(differing << " of " << streams[0].size() << " samples differ between the two blockings");
    CHECK(differing == 0);

    for (std::size_t i = 1; i < decodes.size(); ++i) {
        INFO("decode " << i << " against decode 0");
        CHECK(decodes[i] == decodes[0]);
    }
    CHECK(headers_in(decodes[0]) >= 5);
}
