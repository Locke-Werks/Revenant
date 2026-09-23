// Decoded messages from a receiver's AUDIO, from a synthetic transmitter
// through the engine and out over a socket.
//
// WHAT THESE CASES CLAIM
//
// tests/decode scores RTTY, AX.25 with APRS, POCSAG, SITOR-B and NAVTEX each
// against its transmitter in core/dsp/synth/fsk_mod.h, audio buffer to
// decoder, with no receiver in the way. What these add is the receiver. Each
// transmitter's audio is put on a radio carrier the way a station would put
// it there, single sideband for the HF text modes and FM for AFSK, POCSAG
// being direct FSK, and written to a file. The file is read back through the
// channelizer, a usb, lsb or nfm receiver demodulates it on the device, the
// decoder is attached over the wire to that receiver's audio, and what it
// recovered is read back off the socket with its typed fields.
//
// So a pass here says the audio-domain path in core/rpc/server.cpp feeds a
// real demodulator's output to the adapters in core/rpc/decoders.h at the
// rate the receiver runs, and that the sideband a receiver is in reaches the
// adapter that needs it.
//
// THE GRID is test_rpc_decode.cpp's, R = 288000 over M = 4, for its reason:
// the fine stage is known to work there. Every carrier sits 5 kHz above
// channel 0's centre, so the fine stage has a residual to mix out.
//
// THE NOISE is stated in 2500 Hz at the radio frequency, docs/snr-convention.md,
// and covers the whole file including the silence either side, so a decoder
// hears noise before the transmitter keys up and after it stops, as it would on
// the air. Two points per decoder: 30 dB, where every message should cross
// intact, and one near where the library test for that decoder says it starts
// to lose characters, where the case asserts little and prints what arrived.
// For SSB the audio SNR in 2500 Hz is close to the RF figure, so the library
// tests' points carry over. For FM they do not: the discriminator has a
// threshold, so the AX.25 and POCSAG points were found by measuring here.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <numbers>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/decode/ax25.h"
#include "core/decode/pocsag.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/fsk_mod.h"
#include "core/dsp/synth/modulators.h"
#include "core/dsp/types.h"
#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/decoders.h"
#include "core/rpc/types.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/decoded_log.h"
#include "tests/rpc/rpc_fixture.h"

using namespace revenant;
using test::ending;
using test::flag_of;
using test::Harness;
using test::HarnessOptions;
using test::integer_of;
using test::into;
using test::MessageLog;
using test::real_of;
using test::text_of;
using test::wait_for;

namespace {

constexpr std::uint32_t kGridChannels = 4;
constexpr dsp::SampleRate kFileRate = 288'000;
constexpr std::uint32_t kBlockSamples = 16'384;
constexpr dsp::Hertz kCarrierHz = 5'000;

// The rate an audio receiver that asks for none resolves to, and the rate
// every audio decoder was written at. Asserted off the wire rather than
// configured, because the adapter builds at whatever the first chunk carries.
constexpr std::uint32_t kAudioRate = 48'000;

// Silence before the transmitter keys up, for the channelizer's filter to
// fill, and after it stops. The trailing figure is longer than
// test_rpc_decode.cpp's because a line of RTTY or SITOR-B that ends without a
// line end is handed out when the channel has been quiet for ten character
// times, 1.65 s of RTTY, and the file has to run that long past it.
constexpr double kLeadSeconds = 0.5;
constexpr double kTailSeconds = 2.5;

constexpr int kRunTimeoutMs = 120'000;

// The high point every decoder is held to exactly.
constexpr double kHighSnrDb = 30.0;

constexpr std::uint64_t kNoiseSeed = 0xA0D10'5EEDULL;

// HF captures say they are at 14 MHz and VHF ones at 145 MHz, which only
// matters to revenant-cli, where a --vrx is an absolute frequency or an offset
// from this.
constexpr dsp::Hertz kHfCentre = 14'000'000;
constexpr dsp::Hertz kVhfCentre = 145'000'000;

// ---------------------------------------------------------------------------
// Putting audio on a carrier
// ---------------------------------------------------------------------------

[[nodiscard]] std::size_t seconds_to_samples(double seconds) {
    return static_cast<std::size_t>(seconds * static_cast<double>(kFileRate));
}

// The transmission with its silence either side, at the file's rate.
[[nodiscard]] std::vector<float> padded(std::span<const float> audio) {
    std::vector<float> out(seconds_to_samples(kLeadSeconds), 0.0F);
    out.insert(out.end(), audio.begin(), audio.end());
    out.insert(out.end(), seconds_to_samples(kTailSeconds), 0.0F);
    return out;
}

// Single sideband by the phasing method, core/dsp/synth/modulators.h's own.
// The Hilbert transformer's usable band starts near rate / taps, 564 Hz at 511
// taps here, which clears every tone these modes put in the audio: RTTY's
// lowest is 1955 Hz on usb and SITOR-B's 1615 Hz.
[[nodiscard]] Expected<std::vector<dsp::Complex32>> sideband(std::vector<float> audio,
                                                             bool upper) {
    siggen::ModulatorConfig common;
    common.rate = kFileRate;
    common.carrier_offset = kCarrierHz;
    common.amplitude = 0.5;
    siggen::SsbParams params;
    params.audio_low_hz = 1'000;
    params.audio_high_hz = 3'000;
    params.hilbert_taps = 511;
    const std::size_t count = audio.size();
    auto made = siggen::generate_ssb(common, params, upper, count, std::move(audio));
    if (!made) {
        return std::unexpected(made.error());
    }
    return std::move(made->samples);
}

// FM with 3 kHz peak deviation for a full-scale tone, the amateur 2 m figure
// for a 12.5 kHz channel.
[[nodiscard]] Expected<std::vector<dsp::Complex32>> frequency_modulated(std::vector<float> audio) {
    siggen::ModulatorConfig common;
    common.rate = kFileRate;
    common.carrier_offset = kCarrierHz;
    common.amplitude = 0.5;
    siggen::NfmParams params;
    params.deviation = 3'000;
    params.audio_bandwidth_hz = 3'000;
    const std::size_t count = audio.size();
    auto made = siggen::generate_nfm(common, params, count, std::move(audio));
    if (!made) {
        return std::unexpected(made.error());
    }
    return std::move(made->samples);
}

// Complex baseband moved up to the carrier, with silence either side. The
// phase is reduced exactly in integers so it does not drift with length.
[[nodiscard]] std::vector<dsp::Complex32> on_the_carrier(std::span<const dsp::Complex32> signal) {
    std::vector<dsp::Complex32> out(seconds_to_samples(kLeadSeconds), dsp::Complex32{});
    out.reserve(out.size() + signal.size() + seconds_to_samples(kTailSeconds));
    for (std::size_t n = 0; n < signal.size(); ++n) {
        const std::int64_t turns = (kCarrierHz * static_cast<std::int64_t>(n)) % kFileRate;
        const double angle = 2.0 * std::numbers::pi * static_cast<double>(turns) /
                             static_cast<double>(kFileRate);
        const std::complex<double> moved =
            0.5 * std::complex<double>(signal[n]) * std::polar(1.0, angle);
        out.emplace_back(static_cast<float>(moved.real()), static_cast<float>(moved.imag()));
    }
    out.insert(out.end(), seconds_to_samples(kTailSeconds), dsp::Complex32{});
    return out;
}

// Noise over the whole file at snr_2500_db against the power of the
// transmission alone, which is the part between the two silences.
[[nodiscard]] Status add_noise(std::vector<dsp::Complex32>& all, double snr_2500_db) {
    const std::size_t lead = seconds_to_samples(kLeadSeconds);
    const std::size_t tail = seconds_to_samples(kTailSeconds);
    const std::span<const dsp::Complex32> transmission(all.data() + lead,
                                                       all.size() - lead - tail);
    const double power = siggen::mean_power(transmission);
    auto noise = siggen::awgn_power_for(siggen::NoiseLevel::snr_in_2500_hz_db(snr_2500_db),
                                        power, kFileRate);
    if (!noise) {
        return std::unexpected(noise.error());
    }
    return siggen::add_awgn_at_power(all, *noise, kNoiseSeed);
}

// ---------------------------------------------------------------------------
// The transmissions
// ---------------------------------------------------------------------------

// What each capture is for: its samples, the receiver that reads it, and the
// name revenant-cli's capture is written under.
struct Capture {
    std::string tag;
    std::vector<dsp::Complex32> samples;
    rpc::Demod demod = rpc::Demod::Usb;
    dsp::Hertz centre = kHfCentre;
};

// Starts and ends with a line end, as an operator's transmission does, so the
// characters a start-stop receiver finds in the noise before the carrier and
// after it are lines of their own rather than the front of the first line.
const std::u32string kRttyText = U"\r\nCQ DE N0CALL\r\nRYRY 0123456789 73\r\n";
const std::vector<std::string> kRttyLines = {"CQ DE N0CALL", "RYRY 0123456789 73"};

// usb puts space below mark in the audio and lsb above it; see the RTTY
// adapter in core/rpc/decoders.h. The transmitter is told the same thing the
// adapter will decide, and the radio frequencies come out the same way round
// in both: mark on the higher one.
[[nodiscard]] Expected<Capture> rtty_capture(bool upper, double snr_2500_db) {
    auto codes = siggen::ita2_encode_text(kRttyText);
    if (!codes) {
        return std::unexpected(codes.error());
    }
    siggen::RttyModConfig mod;
    mod.rate = kFileRate;
    mod.space_above_mark = !upper;
    mod.amplitude = 1.0;
    auto audio = siggen::rtty_render(mod, *codes);
    if (!audio) {
        return std::unexpected(audio.error());
    }
    auto signal = sideband(padded(*audio), upper);
    if (!signal) {
        return std::unexpected(signal.error());
    }
    if (auto noisy = add_noise(*signal, snr_2500_db); !noisy) {
        return std::unexpected(noisy.error());
    }
    return Capture{upper ? "rtty_usb" : "rtty_lsb", std::move(*signal),
                   upper ? rpc::Demod::Usb : rpc::Demod::Lsb, kHfCentre};
}

// M.625-4 clause 4.6.1's line end first, which is what starts SITOR-B printing.
const std::u32string kSitorText = U"CQ CQ DE N0CALL\r\nSITOR B TEST 42\r\n";
const std::vector<std::string> kSitorLines = {"CQ CQ DE N0CALL", "SITOR B TEST 42"};

[[nodiscard]] Expected<Capture> sitor_capture(std::u32string_view text, bool navtex,
                                              double snr_2500_db) {
    auto codes = siggen::ita2_encode_text(text);
    if (!codes) {
        return std::unexpected(codes.error());
    }
    siggen::SitorModConfig mod;
    mod.rate = kFileRate;
    mod.upper_sideband = true;
    mod.amplitude = 1.0;
    // M.540-2 Annex II Figure 1 has "ZCZC" follow phasing with no line end.
    // Sixteen phasing pairs is M.625-4's minimum, and short of the ten
    // seconds Figure 1 asks of a NAVTEX station; the decoder phases on the
    // same signal either way and a shorter capture is a faster case.
    mod.line_end_first = !navtex;
    auto audio = siggen::sitor_b_render(mod, *codes);
    if (!audio) {
        return std::unexpected(audio.error());
    }
    auto signal = sideband(padded(*audio), true);
    if (!signal) {
        return std::unexpected(signal.error());
    }
    if (auto noisy = add_noise(*signal, snr_2500_db); !noisy) {
        return std::unexpected(noisy.error());
    }
    return Capture{navtex ? "navtex_usb" : "sitor_b_usb", std::move(*signal), rpc::Demod::Usb,
                   kHfCentre};
}

const std::u32string kNavtexBody = U"GALE WARNING 042\r\nSW 8 TO 9.\r\n";
constexpr std::string_view kNavtexMessage = "GALE WARNING 042\r\nSW 8 TO 9.";

[[nodiscard]] Expected<std::vector<std::uint8_t>> octets(const siggen::Ax25FrameSpec& spec) {
    return siggen::ax25_frame_octets(spec);
}

[[nodiscard]] decode::Ax25Address address(std::string call, std::uint8_t ssid) {
    decode::Ax25Address out;
    out.callsign = std::move(call);
    out.ssid = ssid;
    return out;
}

[[nodiscard]] std::vector<std::uint8_t> bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

// APRS101's own examples: page 32's position with its comment, page 80's
// status and page 71's message, each from N0CALL-9 through WIDE1-1. Then an
// I frame with no APRS in it, which the decoder reports as a plain frame.
constexpr std::string_view kPositionInfo = "!4903.50N/07201.75W-Test 001234";
constexpr std::string_view kStatusInfo = ">Net Control Center";
constexpr std::string_view kMessageInfo = ":WU2Z     :Testing{003";
constexpr std::string_view kPlainInfo = "hello from revenant";

[[nodiscard]] Expected<Capture> ax25_capture(double snr_2500_db) {
    std::vector<std::vector<std::uint8_t>> frames;
    for (const std::string_view info : {kPositionInfo, kStatusInfo, kMessageInfo}) {
        siggen::Ax25FrameSpec spec;
        spec.destination = address("APRS", 0);
        spec.source = address("N0CALL", 9);
        spec.repeaters = {address("WIDE1", 1)};
        spec.information = bytes(info);
        auto one = octets(spec);
        if (!one) {
            return std::unexpected(one.error());
        }
        frames.push_back(std::move(*one));
    }
    siggen::Ax25FrameSpec plain;
    plain.destination = address("N0CALL", 1);
    plain.source = address("N0CALL", 2);
    plain.control = 0x00;  // Figure 4.1a: an I frame, N(S) and N(R) zero
    plain.information = bytes(kPlainInfo);
    auto last = octets(plain);
    if (!last) {
        return std::unexpected(last.error());
    }
    frames.push_back(std::move(*last));

    siggen::Ax25ModConfig mod;
    mod.rate = kFileRate;
    mod.amplitude = 1.0;
    mod.flags_between = 16;
    auto audio = siggen::ax25_render(mod, frames);
    if (!audio) {
        return std::unexpected(audio.error());
    }
    auto signal = frequency_modulated(padded(*audio));
    if (!signal) {
        return std::unexpected(signal.error());
    }
    if (auto noisy = add_noise(*signal, snr_2500_db); !noisy) {
        return std::unexpected(noisy.error());
    }
    return Capture{"ax25_nfm", std::move(*signal), rpc::Demod::Nfm, kVhfCentre};
}

constexpr std::uint32_t kAlphaRic = 1'234'567;
constexpr std::uint32_t kNumericRic = 200'000;
constexpr std::uint32_t kToneRic = 300'001;
constexpr std::uint32_t kSlowRic = 1'111'111;
constexpr std::string_view kAlphaText = "REVENANT PAGE TEST";
constexpr std::string_view kNumericText = "5551234";
constexpr std::string_view kSlowText = "SLOW PAGE";

// Two transmissions: three pages at 1200 bit/s, then one at 512 after a
// second of silence, so the case says the adapter runs more than one rate.
[[nodiscard]] Expected<Capture> pocsag_capture(double snr_2500_db) {
    auto alpha = siggen::pocsag_alphanumeric_bits(kAlphaText);
    auto numeric = siggen::pocsag_numeric_bits(kNumericText);
    auto slow = siggen::pocsag_alphanumeric_bits(kSlowText);
    if (!alpha || !numeric || !slow) {
        return fail("the POCSAG test pages did not encode");
    }
    const std::vector<siggen::PocsagPageSpec> fast_pages = {
        {kAlphaRic, decode::kPocsagFunctionAlphanumeric, *alpha},
        {kNumericRic, decode::kPocsagFunctionNumeric, *numeric},
        {kToneRic, 0b01, {}},
    };
    const std::vector<siggen::PocsagPageSpec> slow_pages = {
        {kSlowRic, decode::kPocsagFunctionAlphanumeric, *slow},
    };

    siggen::PocsagModConfig mod;
    mod.rate = kFileRate;
    mod.bit_rate = decode::kPocsag1200;
    auto first = siggen::pocsag_render_baseband(mod, siggen::pocsag_bits(fast_pages));
    mod.bit_rate = decode::kPocsag512;
    auto second = siggen::pocsag_render_baseband(mod, siggen::pocsag_bits(slow_pages));
    if (!first) {
        return std::unexpected(first.error());
    }
    if (!second) {
        return std::unexpected(second.error());
    }
    std::vector<dsp::Complex32> both = std::move(*first);
    both.insert(both.end(), seconds_to_samples(1.0), dsp::Complex32{});
    both.insert(both.end(), second->begin(), second->end());

    std::vector<dsp::Complex32> signal = on_the_carrier(both);
    if (auto noisy = add_noise(signal, snr_2500_db); !noisy) {
        return std::unexpected(noisy.error());
    }
    return Capture{"pocsag_nfm", std::move(signal), rpc::Demod::Nfm, kVhfCentre};
}

// ---------------------------------------------------------------------------
// The file and the run
// ---------------------------------------------------------------------------

// cf32, removed when the case ends unless it was asked to keep it. Named for
// the case and for the run, on test_rpc_decode.cpp's argument about %TEMP%.
class CaptureFile {
public:
    CaptureFile(const Capture& capture, std::filesystem::path path, bool keep)
        : path_(std::move(path)), keep_(keep), centre_(capture.centre),
          samples_(capture.samples.size()) {}

    explicit CaptureFile(const Capture& capture)
        : CaptureFile(capture,
                      std::filesystem::temp_directory_path() /
                          std::format("revenant-decode-audio-{}-{}.cf32", capture.tag,
                                      std::chrono::steady_clock::now().time_since_epoch().count()),
                      false) {}

    CaptureFile(const CaptureFile&) = delete;
    CaptureFile& operator=(const CaptureFile&) = delete;

    ~CaptureFile() {
        if (!keep_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    [[nodiscard]] Status write(std::span<const dsp::Complex32> samples) const {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        if (!out) {
            return fail(std::format("could not write {}", path_.string()));
        }
        out.write(reinterpret_cast<const char*>(samples.data()),
                  static_cast<std::streamsize>(samples.size() * sizeof(dsp::Complex32)));
        if (!out) {
            return fail(std::format("writing {} failed part way through", path_.string()));
        }
        return {};
    }

    [[nodiscard]] std::string uri() const {
        return std::format("file:///{}?rate={}&format=cf32&center={}", path_.generic_string(),
                           kFileRate, centre_);
    }
    [[nodiscard]] dsp::SampleIndex samples() const { return samples_; }

private:
    std::filesystem::path path_;
    bool keep_ = false;
    dsp::Hertz centre_ = 0;
    dsp::SampleIndex samples_ = 0;
};

struct Run {
    std::vector<rpc::DecodedMessage> messages;
    rpc::DecodedStats stats;
    std::string resolved;
};

// Writes the capture, runs it through a receiver in its mode with `decoder`
// attached over the wire, and hands back what arrived. `retune` sends the
// receiver's own params back to it after subscribing and before a sample
// moves, which raises the retune fence on the audio path.
[[nodiscard]] Run run_capture(const Capture& capture, std::string_view decoder,
                              bool retune = false) {
    CaptureFile file(capture);
    const auto written = file.write(capture.samples);
    INFO(test::message_of(written));
    REQUIRE(written.has_value());

    Harness harness;
    HarnessOptions options;
    options.source_uri = file.uri();
    options.channels = kGridChannels;
    options.block_samples = kBlockSamples;
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    const rpc::VrxParams params{.center = kCarrierHz, .bandwidth = 0, .demod = capture.demod};
    auto vrx = harness.client().add_vrx(params);
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto log = std::make_shared<MessageLog>();
    auto resolved = harness.client().subscribe_decoded(*vrx, decoder, into(log), ending(log));
    INFO(test::message_of(resolved));
    REQUIRE(resolved.has_value());
    if (retune) {
        REQUIRE(harness.client().set_vrx_params(*vrx, params).has_value());
    }

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());
    const std::uint64_t blocks = (file.samples() + kBlockSamples - 1) / kBlockSamples;
    const std::uint64_t seen = harness.wait_for_blocks(blocks, kRunTimeoutMs);
    INFO(std::format("{} blocks delivered of {}", seen, blocks));
    CHECK(seen >= blocks);
    const auto finished = harness.stop_engine();
    INFO(test::message_of(finished));
    CHECK(finished.has_value());

    // The engine is stopped, so nothing more is coming; what the subscription
    // says it sent is what the log has to reach before it is read.
    auto stats = harness.client().decoded_stats(*vrx, decoder);
    INFO(test::message_of(stats));
    REQUIRE(stats.has_value());
    const std::uint64_t sent = stats->messages_sent + stats->backlog;
    Run run;
    run.messages = wait_for(*log, [&](const auto& got) { return got.size() >= sent; });
    auto settled = harness.client().decoded_stats(*vrx, decoder);
    REQUIRE(settled.has_value());
    run.stats = *settled;
    run.resolved = *resolved;
    INFO(log->reason());
    CHECK_FALSE(log->ended());
    return run;
}

[[nodiscard]] std::string arrived(const Run& run) {
    std::string out;
    for (const rpc::DecodedMessage& message : run.messages) {
        out += std::format("\n  #{} {} {}: {}", message.sequence, message.decoder, message.kind,
                           message.text);
    }
    return out;
}

[[nodiscard]] const rpc::DecodedMessage* line_reading(const Run& run, std::string_view text) {
    for (const rpc::DecodedMessage& message : run.messages) {
        if (message.kind == "line" && message.text == text) {
            return &message;
        }
    }
    return nullptr;
}

// The fewest single-character edits turning `sent` into some stretch of
// `received`: leading and trailing text in `received` is free, which is
// what lets a line of noise before the transmission cost nothing.
[[nodiscard]] std::size_t fitting_distance(std::string_view sent, std::string_view received) {
    std::vector<std::size_t> previous(received.size() + 1, 0);
    std::vector<std::size_t> current(received.size() + 1, 0);
    for (std::size_t i = 1; i <= sent.size(); ++i) {
        current[0] = i;
        for (std::size_t j = 1; j <= received.size(); ++j) {
            const std::size_t substitute =
                previous[j - 1] + (sent[i - 1] == received[j - 1] ? 0U : 1U);
            current[j] = std::min({substitute, previous[j] + 1, current[j - 1] + 1});
        }
        std::swap(previous, current);
    }
    return *std::min_element(previous.begin(), previous.end());
}

// The character error rate of a text decoder's lines against what was sent,
// both joined with line feeds.
[[nodiscard]] double line_error_rate(const Run& run, const std::vector<std::string>& sent_lines) {
    std::string sent;
    for (const std::string& line : sent_lines) {
        sent += (sent.empty() ? "" : "\n") + line;
    }
    std::string received;
    for (const rpc::DecodedMessage& message : run.messages) {
        received += (received.empty() ? "" : "\n") + message.text;
    }
    return static_cast<double>(fitting_distance(sent, received)) /
           static_cast<double>(sent.size());
}

void check_common(const Run& run, std::string_view decoder) {
    for (const rpc::DecodedMessage& message : run.messages) {
        CHECK(message.decoder == decoder);
        CHECK(message.sample_rate == kAudioRate);
        CHECK(message.end_sample > message.start_sample);
    }
    CHECK(run.stats.sample_rate == kAudioRate);
    CHECK(run.stats.messages_dropped == 0);
}

}  // namespace

// ---------------------------------------------------------------------------
// The registry and the refusals
// ---------------------------------------------------------------------------

TEST_CASE("the audio decoders are listed with the input they read", "[gpu][rpc][decode]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    const auto ready = harness.open(HarnessOptions{});
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    auto listed = harness.client().decoders();
    INFO(test::message_of(listed));
    REQUIRE(listed.has_value());

    std::set<std::string> audio;
    for (const rpc::DecoderInfo& info : *listed) {
        if (info.input == rpc::DecoderInput::RealAudio) {
            audio.insert(info.name);
            // DecoderInfo carries no list of modes, so each description has to
            // say them.
            INFO(info.name << ": " << info.description);
            CHECK(info.description.find("Reads a") != std::string::npos);
        }
    }
    CHECK(audio == std::set<std::string>{"rtty", "ax25", "pocsag", "sitor_b", "navtex"});
}

TEST_CASE("an audio decoder on the wrong receiver is refused naming the mode it needs",
          "[gpu][rpc][decode]") {
    REVENANT_NEEDS_GPU();

    // Eight channels rather than the suite's 64, because a wfm receiver needs
    // 200 kHz of passband and the engine refuses one a channel cannot carry.
    Harness harness;
    HarnessOptions options;
    options.channels = 8;
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    const auto add = [&](rpc::Demod mode, std::int64_t bandwidth) {
        auto vrx = harness.client().add_vrx(
            rpc::VrxParams{.center = 131'072, .bandwidth = bandwidth, .demod = mode});
        INFO(test::message_of(vrx));
        REQUIRE(vrx.has_value());
        return *vrx;
    };
    const std::uint64_t wfm = add(rpc::Demod::Wfm, 0);
    const std::uint64_t usb = add(rpc::Demod::Usb, 2'700);
    const std::uint64_t nfm = add(rpc::Demod::Nfm, 12'000);
    const std::uint64_t raw = add(rpc::Demod::Raw, 12'000);

    auto log = std::make_shared<MessageLog>();
    const auto refusal = [&](std::uint64_t vrx, std::string_view name) {
        auto refused = harness.client().subscribe_decoded(vrx, name, into(log), ending(log));
        REQUIRE_FALSE(refused.has_value());
        return refused.error().message;
    };

    // RTTY on broadcast FM, which is the case the mode list exists for.
    const std::string on_wfm = refusal(wfm, "rtty");
    INFO(on_wfm);
    CHECK(on_wfm.find("the rtty decoder reads the audio of a usb or lsb receiver") !=
          std::string::npos);
    CHECK(on_wfm.find(std::format("receiver {} is wfm", wfm)) != std::string::npos);

    // On a raw tap the answer is the mode, and not only that there is no audio.
    const std::string on_raw = refusal(raw, "navtex");
    INFO(on_raw);
    CHECK(on_raw.find("usb or lsb") != std::string::npos);
    CHECK(on_raw.find("is raw") != std::string::npos);

    const std::string ax25_on_usb = refusal(usb, "ax25");
    INFO(ax25_on_usb);
    CHECK(ax25_on_usb.find("reads the audio of a nfm receiver") != std::string::npos);

    const std::string pocsag_on_usb = refusal(usb, "pocsag");
    INFO(pocsag_on_usb);
    CHECK(pocsag_on_usb.find("nfm") != std::string::npos);

    // An empty name names the decoders that would have read it.
    const std::string usb_default = refusal(usb, "");
    INFO(usb_default);
    CHECK(usb_default.find("no decoder is named after that mode") != std::string::npos);
    CHECK(usb_default.find("rtty, sitor_b, navtex") != std::string::npos);

    const std::string nfm_default = refusal(nfm, "");
    INFO(nfm_default);
    CHECK(nfm_default.find("ax25, pocsag") != std::string::npos);

    // A complex decoder on an audio receiver is still refused in its own words.
    const std::string p25_on_usb = refusal(usb, "p25p1");
    INFO(p25_on_usb);
    CHECK(p25_on_usb.find("reads complex baseband") != std::string::npos);

    CHECK_FALSE(log->ended());
}

// ---------------------------------------------------------------------------
// Through the engine
// ---------------------------------------------------------------------------

TEST_CASE("RTTY on a usb receiver crosses the wire as lines of text", "[gpu][rpc][decode]") {
    REVENANT_NEEDS_GPU();

    auto capture = rtty_capture(true, kHighSnrDb);
    INFO(test::message_of(capture));
    REQUIRE(capture.has_value());
    const Run run = run_capture(*capture, "rtty");
    INFO(std::format("{} messages arrived:{}", run.messages.size(), arrived(run)));
    CHECK(run.resolved == "rtty");
    check_common(run, "rtty");

    for (const std::string& expected : kRttyLines) {
        const rpc::DecodedMessage* line = line_reading(run, expected);
        INFO("looking for \"" << expected << "\"");
        REQUIRE(line != nullptr);
        CHECK(text_of(*line, "text") == expected);
        CHECK(integer_of(*line, "characters") == static_cast<std::int64_t>(expected.size()));
        CHECK(text_of(*line, "ended") == "line_end");
        CHECK_FALSE(flag_of(*line, "space_above_mark"));
        CHECK(real_of(*line, "min_margin") > 0.5);
        const std::int64_t began = integer_of(*line, "began_sample");
        CHECK(began > 0);
        CHECK(static_cast<std::uint64_t>(began) < line->start_sample);
    }
    const double cer = line_error_rate(run, kRttyLines);
    WARN(std::format("rtty usb at {} dB/2500 Hz: {} messages, character error rate {:.4f}",
                     kHighSnrDb, run.messages.size(), cer));
    CHECK(cer == 0.0);

    // Where tests/decode/test_rtty.cpp measures a character error rate of 0.14
    // on audio alone: -8 dB in 2500 Hz, 9.4 dB Eb/N0. Through the receiver it
    // measured 0.097 on 2026-09-22; -5 dB gave none and -10 dB 0.45.
    constexpr double kLowSnrDb = -8.0;
    auto noisy = rtty_capture(true, kLowSnrDb);
    REQUIRE(noisy.has_value());
    const Run low = run_capture(*noisy, "rtty");
    INFO(std::format("at {} dB, {} messages arrived:{}", kLowSnrDb, low.messages.size(),
                     arrived(low)));
    check_common(low, "rtty");
    const double low_cer = line_error_rate(low, kRttyLines);
    WARN(std::format("rtty usb at {} dB/2500 Hz: {} messages, character error rate {:.4f}",
                     kLowSnrDb, low.messages.size(), low_cer));
    CHECK(low_cer <= 0.25);
}

TEST_CASE("RTTY on an lsb receiver decodes the other way up", "[gpu][rpc][decode]") {
    REVENANT_NEEDS_GPU();

    auto capture = rtty_capture(false, kHighSnrDb);
    INFO(test::message_of(capture));
    REQUIRE(capture.has_value());
    const Run run = run_capture(*capture, "rtty");
    INFO(std::format("{} messages arrived:{}", run.messages.size(), arrived(run)));
    check_common(run, "rtty");
    for (const std::string& expected : kRttyLines) {
        const rpc::DecodedMessage* line = line_reading(run, expected);
        INFO("looking for \"" << expected << "\"");
        REQUIRE(line != nullptr);
        CHECK(flag_of(*line, "space_above_mark"));
    }
    WARN(std::format("rtty lsb at {} dB/2500 Hz: {} messages, character error rate {:.4f}",
                     kHighSnrDb, run.messages.size(), line_error_rate(run, kRttyLines)));
}

TEST_CASE("SITOR-B on a usb receiver crosses the wire as lines of text", "[gpu][rpc][decode]") {
    REVENANT_NEEDS_GPU();

    auto capture = sitor_capture(kSitorText, false, kHighSnrDb);
    INFO(test::message_of(capture));
    REQUIRE(capture.has_value());
    const Run run = run_capture(*capture, "sitor_b");
    INFO(std::format("{} messages arrived:{}", run.messages.size(), arrived(run)));
    check_common(run, "sitor_b");
    for (const std::string& expected : kSitorLines) {
        const rpc::DecodedMessage* line = line_reading(run, expected);
        INFO("looking for \"" << expected << "\"");
        REQUIRE(line != nullptr);
        CHECK(integer_of(*line, "lost") == 0);
        CHECK(integer_of(*line, "phasing") >= 1);
        CHECK(flag_of(*line, "upper_sideband"));
        CHECK(text_of(*line, "ended") == "line_end");
    }
    const double cer = line_error_rate(run, kSitorLines);
    WARN(std::format("sitor_b at {} dB/2500 Hz: {} messages, character error rate {:.4f}",
                     kHighSnrDb, run.messages.size(), cer));
    CHECK(cer == 0.0);

    // tests/decode/test_sitor_b.cpp: at -5 dB in 2500 Hz one character in
    // eleven loses its DX copy and one in eighty-three is lost in both.
    // Through the receiver on 2026-09-22 the character error rate was 0.097.
    constexpr double kLowSnrDb = -5.0;
    auto noisy = sitor_capture(kSitorText, false, kLowSnrDb);
    REQUIRE(noisy.has_value());
    const Run low = run_capture(*noisy, "sitor_b");
    INFO(std::format("at {} dB, {} messages arrived:{}", kLowSnrDb, low.messages.size(),
                     arrived(low)));
    check_common(low, "sitor_b");
    const double low_cer = line_error_rate(low, kSitorLines);
    WARN(std::format("sitor_b at {} dB/2500 Hz: {} messages, character error rate {:.4f}",
                     kLowSnrDb, low.messages.size(), low_cer));
    CHECK(low_cer <= 0.25);
}

TEST_CASE("a NAVTEX message crosses the wire with its B1 to B4 letters", "[gpu][rpc][decode]") {
    REVENANT_NEEDS_GPU();

    const std::u32string text = siggen::navtex_text('E', 'A', 7, kNavtexBody);
    auto capture = sitor_capture(text, true, kHighSnrDb);
    INFO(test::message_of(capture));
    REQUIRE(capture.has_value());
    const Run run = run_capture(*capture, "navtex");
    INFO(std::format("{} messages arrived:{}", run.messages.size(), arrived(run)));
    check_common(run, "navtex");
    REQUIRE(run.messages.size() == 1);
    const rpc::DecodedMessage& message = run.messages.front();
    CHECK(message.kind == "message");
    CHECK(text_of(message, "area") == "E");
    CHECK(text_of(message, "subject") == "A");
    CHECK(integer_of(message, "serial") == 7);
    CHECK(flag_of(message, "preamble_clean"));
    CHECK(flag_of(message, "complete"));
    CHECK(text_of(message, "message") == kNavtexMessage);
    CHECK(integer_of(message, "mutilated_characters") == 0);
    CHECK(message.text == "ZCZC EA07 GALE WARNING 042 / SW 8 TO 9.");
    WARN(std::format("navtex at {} dB/2500 Hz: {} message, \"{}\"", kHighSnrDb,
                     run.messages.size(), message.text));

    // tests/decode/test_navtex.cpp: two of ten exact at -5 dB in 2500 Hz with
    // every preamble clean. Through the receiver on 2026-09-22 the one message
    // here arrived exact at -2 and -5 dB, arrived with its text damaged at -6
    // and did not arrive at -8.
    constexpr double kLowSnrDb = -5.0;
    auto noisy = sitor_capture(text, true, kLowSnrDb);
    REQUIRE(noisy.has_value());
    const Run low = run_capture(*noisy, "navtex");
    INFO(std::format("at {} dB, {} messages arrived:{}", kLowSnrDb, low.messages.size(),
                     arrived(low)));
    check_common(low, "navtex");
    std::size_t exact = 0;
    std::size_t clean = 0;
    for (const rpc::DecodedMessage& got : low.messages) {
        clean += flag_of(got, "preamble_clean") ? 1U : 0U;
        exact += text_of(got, "message") == kNavtexMessage ? 1U : 0U;
    }
    WARN(std::format("navtex at {} dB/2500 Hz: {} messages, {} with a clean preamble, {} "
                     "exact",
                     kLowSnrDb, low.messages.size(), clean, exact));
    CHECK(clean >= 1);
}

TEST_CASE("AX.25 frames and their APRS fields cross the wire from an nfm receiver",
          "[gpu][rpc][decode]") {
    REVENANT_NEEDS_GPU();

    auto capture = ax25_capture(kHighSnrDb);
    INFO(test::message_of(capture));
    REQUIRE(capture.has_value());
    // With a retune before the first sample, as test_rpc_decode.cpp does for
    // D-STAR: the fence has to come down on the audio path as well, or the
    // whole file is discarded and nothing arrives.
    const Run run = run_capture(*capture, "ax25", true);
    INFO(std::format("{} messages arrived:{}", run.messages.size(), arrived(run)));
    check_common(run, "ax25");

    std::set<std::string> kinds;
    for (const rpc::DecodedMessage& message : run.messages) {
        kinds.insert(message.kind);
        if (message.kind == "frame") {
            CHECK(text_of(message, "source") == "N0CALL-2");
            CHECK(text_of(message, "destination") == "N0CALL-1");
            CHECK(text_of(message, "frame_type") == "I");
            CHECK_FALSE(flag_of(message, "aprs"));
            const rpc::DecodedField* information = message.field("information");
            REQUIRE(information != nullptr);
            REQUIRE(information->bytes() != nullptr);
            CHECK(*information->bytes() == bytes(kPlainInfo));
            continue;
        }
        CHECK(text_of(message, "source") == "N0CALL-9");
        CHECK(text_of(message, "destination") == "APRS");
        CHECK(text_of(message, "path") == "WIDE1-1");
        CHECK(flag_of(message, "ui"));
        CHECK(flag_of(message, "aprs"));
        CHECK(integer_of(message, "pid") == decode::kAx25PidNoLayer3);
        if (message.kind == "aprs_position") {
            // APRS101 pages 23 and 24: 4903.50N 07201.75W.
            CHECK(std::abs(real_of(message, "latitude") - (49.0 + 3.50 / 60.0)) < 1e-6);
            CHECK(std::abs(real_of(message, "longitude") + (72.0 + 1.75 / 60.0)) < 1e-6);
            CHECK(text_of(message, "symbol_code") == "-");
            CHECK(text_of(message, "comment") == "Test 001234");
            CHECK(message.text == "N0CALL-9>APRS,WIDE1-1 49.0583N 72.0292W Test 001234");
        } else if (message.kind == "aprs_status") {
            CHECK(text_of(message, "status") == "Net Control Center");
        } else if (message.kind == "aprs_message") {
            CHECK(text_of(message, "addressee") == "WU2Z");
            CHECK(text_of(message, "message") == "Testing");
            CHECK(text_of(message, "message_id") == "003");
        }
    }
    CHECK(kinds == std::set<std::string>{"aprs_position", "aprs_status", "aprs_message", "frame"});
    CHECK(run.messages.size() == 4);
    WARN(std::format("ax25 at {} dB/2500 Hz: {} of 4 frames; {} chunks discarded by the fence",
                     kHighSnrDb, run.messages.size(), run.stats.chunks_discarded));

    // Measured here rather than carried over from tests/decode/test_ax25.cpp,
    // whose noise is on the audio: this is noise ahead of the discriminator,
    // which has a threshold. Frames of the four arriving on 2026-09-22, in
    // 2500 Hz: none at 10, 12 and 14 dB, 2 at 16, 3 at 17 and 18, 4 at 20.
    constexpr double kLowSnrDb = 16.0;
    auto noisy = ax25_capture(kLowSnrDb);
    REQUIRE(noisy.has_value());
    const Run low = run_capture(*noisy, "ax25");
    INFO(std::format("at {} dB, {} messages arrived:{}", kLowSnrDb, low.messages.size(),
                     arrived(low)));
    check_common(low, "ax25");
    // Whatever arrives passed the FCS, so it is one of the four frames sent
    // and never a frame assembled from noise.
    std::int64_t fcs_failures = 0;
    for (const rpc::DecodedMessage& message : low.messages) {
        fcs_failures = std::max(fcs_failures, integer_of(message, "fcs_failures"));
        const std::string source = text_of(message, "source");
        CHECK((source == "N0CALL-9" || source == "N0CALL-2"));
    }
    WARN(std::format("ax25 at {} dB/2500 Hz: {} of 4 frames, {} FCS failures before the last",
                     kLowSnrDb, low.messages.size(), fcs_failures));
    CHECK(low.messages.size() <= 4);
}

TEST_CASE("POCSAG pages at two rates cross the wire from an nfm receiver",
          "[gpu][rpc][decode]") {
    REVENANT_NEEDS_GPU();

    auto capture = pocsag_capture(kHighSnrDb);
    INFO(test::message_of(capture));
    REQUIRE(capture.has_value());
    const Run run = run_capture(*capture, "pocsag");
    INFO(std::format("{} messages arrived:{}", run.messages.size(), arrived(run)));
    check_common(run, "pocsag");

    bool alpha = false;
    bool numeric = false;
    bool tone = false;
    bool slow = false;
    for (const rpc::DecodedMessage& message : run.messages) {
        const std::int64_t ric = integer_of(message, "address");
        CHECK(integer_of(message, "uncorrectable_codewords") == 0);
        if (ric == kAlphaRic) {
            alpha = true;
            CHECK(message.kind == "alphanumeric");
            CHECK(integer_of(message, "function") == 3);
            CHECK(integer_of(message, "bit_rate") == 1200);
            CHECK(text_of(message, "message") == kAlphaText);
        } else if (ric == kNumericRic) {
            numeric = true;
            CHECK(message.kind == "numeric");
            CHECK(integer_of(message, "function") == 0);
            // Clause 2.1 pads a numeric message with spaces to a whole
            // codeword, and the spaces are part of what was sent.
            CHECK(text_of(message, "message").starts_with(kNumericText));
        } else if (ric == kToneRic) {
            tone = true;
            CHECK(message.kind == "tone");
            CHECK(integer_of(message, "function") == 1);
        } else if (ric == kSlowRic) {
            slow = true;
            CHECK(message.kind == "alphanumeric");
            CHECK(integer_of(message, "bit_rate") == 512);
            CHECK(text_of(message, "message") == kSlowText);
        } else {
            FAIL_CHECK("a page for an address nobody sent: " << message.text);
        }
    }
    CHECK(alpha);
    CHECK(numeric);
    CHECK(tone);
    CHECK(slow);
    WARN(std::format("pocsag at {} dB/2500 Hz: {} of 4 pages", kHighSnrDb, run.messages.size()));

    // Measured here, on the same argument as AX.25's low point. On 2026-09-22,
    // in 2500 Hz: all four pages intact at 10 dB; at 8 dB all four arrived and
    // the numeric one carried an uncorrectable codeword; at 6 and 4 dB pages
    // arrived for addresses nobody sent, several with no uncorrectable
    // codeword, which is the BCH code miscorrecting noise into an address and
    // is core/decode/pocsag.cpp's rather than the seam's; at 2 dB nothing.
    constexpr double kLowSnrDb = 8.0;
    auto noisy = pocsag_capture(kLowSnrDb);
    REQUIRE(noisy.has_value());
    const Run low = run_capture(*noisy, "pocsag");
    INFO(std::format("at {} dB, {} messages arrived:{}", kLowSnrDb, low.messages.size(),
                     arrived(low)));
    check_common(low, "pocsag");
    std::int64_t corrected = 0;
    std::size_t intact = 0;
    std::size_t sent_to = 0;
    for (const rpc::DecodedMessage& message : low.messages) {
        corrected += integer_of(message, "corrected_bits");
        intact += integer_of(message, "uncorrectable_codewords") == 0 ? 1U : 0U;
        const std::int64_t ric = integer_of(message, "address");
        sent_to += (ric == kAlphaRic || ric == kNumericRic || ric == kToneRic || ric == kSlowRic)
                       ? 1U
                       : 0U;
    }
    WARN(std::format("pocsag at {} dB/2500 Hz: {} pages, {} to an address that was sent, {} "
                     "with every codeword corrected, {} bits corrected",
                     kLowSnrDb, low.messages.size(), sent_to, intact, corrected));
    CHECK(sent_to >= 2);
}

// ---------------------------------------------------------------------------
// The captures revenant-cli is run on
// ---------------------------------------------------------------------------

// Hidden, so catch_discover_tests does not register it; tests/rpc/
// CMakeLists.txt runs it as the setup fixture for the revenant-cli --decode
// entries, with REVENANT_DECODE_CAPTURE_DIR naming where the files go. The
// captures are the 30 dB ones the cases above decode over the wire, so the CLI
// and the server are asked about the same samples.
TEST_CASE("the audio decoder captures for revenant-cli", "[.][write-captures]") {
    const char* directory = std::getenv("REVENANT_DECODE_CAPTURE_DIR");
    if (directory == nullptr || *directory == '\0') {
        SKIP("REVENANT_DECODE_CAPTURE_DIR is not set");
    }
    const std::filesystem::path root(directory);
    std::error_code made;
    std::filesystem::create_directories(root, made);
    INFO(made.message());
    REQUIRE_FALSE(made);

    std::vector<Expected<Capture>> captures;
    captures.push_back(rtty_capture(true, kHighSnrDb));
    captures.push_back(rtty_capture(false, kHighSnrDb));
    captures.push_back(sitor_capture(kSitorText, false, kHighSnrDb));
    captures.push_back(sitor_capture(siggen::navtex_text('E', 'A', 7, kNavtexBody), true,
                                     kHighSnrDb));
    captures.push_back(ax25_capture(kHighSnrDb));
    captures.push_back(pocsag_capture(kHighSnrDb));
    for (const Expected<Capture>& capture : captures) {
        INFO(test::message_of(capture));
        REQUIRE(capture.has_value());
        const CaptureFile file(*capture, root / (capture->tag + ".cf32"), true);
        const auto written = file.write(capture->samples);
        INFO(test::message_of(written));
        REQUIRE(written.has_value());
    }
}

