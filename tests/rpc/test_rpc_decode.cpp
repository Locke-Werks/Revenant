// Decoded messages, from a synthetic transmitter through the engine and out
// over a socket.
//
// WHAT THESE CASES CLAIM
//
// tests/decode scores each of P25 Phase 1, D-STAR and TETRA against the
// transmitter in core/dsp/synth/dv_mod.h, sample buffer to decoder, with no
// engine in the way. What nothing before this could say is that a receiver in
// one of those modes, placed by the engine on a real channelizer on a real
// device, hands its complex baseband to the decoder seam and that what the decoder
// recovers arrives at a client as a DecodedMessage with the right values in
// the right typed fields. Each case renders a transmission into a file, reads
// it back through the channelizer, attaches a decoder over the wire, runs the
// engine to the end of the file and reads the messages that came back.
//
// THE GRID, AND WHY IT IS NOT THE SUITE'S
//
// Since 2026-09-22 the three digital voice modes go through the fine stage:
// the residual mixed to DC, the passband filtered to the mode's channel, and
// the result resampled to 48000 S/s for P25 and D-STAR and 72000 for TETRA.
// The adapters in core/rpc/decoders.h read the rate off each chunk, so these
// cases assert the rate that arrived rather than configure one.
//
// One grid carries all three: R = 288000 over M = 4 puts the channel rate
// 2R/M at 144000, three times 48000 and twice 72000, which is the arrangement
// tests/engine/test_engine_dv.cpp measured the fine stage on. It is also wide
// enough for TETRA's prototype: core/dsp/pfb_design.cpp puts the passband edge
// at 0.25 R/M, 18000 Hz here, and pi/4-DQPSK at 18000 symbols a second with
// alpha 0.35 reaches 12150 Hz either side.
//
// Each transmission sits 5 kHz above channel 0's centre and so does its
// receiver, so the fine stage has a residual to mix out. A case with the
// receiver on the centre would pass with the mixer switched off.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <numbers>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/decode/dstar.h"
#include "core/decode/p25p1.h"
#include "core/decode/tetra.h"
#include "core/dsp/synth/dv_mod.h"
#include "core/dsp/types.h"
#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/decoders.h"
#include "core/rpc/types.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/decoded_log.h"
#include "tests/rpc/rpc_fixture.h"
#include "tests/support/temp_path.h"

using namespace revenant;
using test::Harness;
using test::HarnessOptions;
using test::ending;
using test::flag_of;
using test::integer_of;
using test::real_of;
using test::into;
using test::MessageLog;
using test::text_of;
using test::wait_for;

namespace {

constexpr std::uint32_t kGridChannels = 4;
constexpr dsp::SampleRate kFileRate = 288'000;

// The P25 case below was where the P25 decoder's dependence on its blocking
// was found. WHAT THIS PARAGRAPH USED TO SAY, until later on 2026-09-22: "THE
// BLOCK SIZE CHANGES WHAT THE P25 DECODER RECOVERS. The same capture through
// the same engine and adapter gave 4 of its 6 headers at 16384-sample blocks
// and 1 of 6 at 65536 ... P25Phase1::process subtracts each call's mean as its
// carrier offset". It restarted its discriminator and its receive filter at
// every call as well, and the filter was the larger cost;
// tests/decode/test_p25p1_blocking.cpp has the three measured apart. With the
// two carrying their state and the mean gone, the case gives all 6 headers
// at 16384 and all 6 at 65536, measured on the RTX 4090, and
// tests/engine/test_engine_dv.cpp holds the fine stage's samples identical
// across the two.
constexpr std::uint32_t kBlockSamples = 16'384;

// Where every transmitter and every receiver in this file sits: 5 kHz above
// channel 0's centre, which is baseband DC.
constexpr dsp::Hertz kCarrierHz = 5'000;

// The rates the fine stage hands these three modes out at, which are their
// decoders' own. Read here from the decoders' configs rather than written as
// literals, so a change on that side moves the expectation with it.
constexpr dsp::SampleRate kP25TapRate = decode::P25Config{}.rate;
constexpr dsp::SampleRate kDStarTapRate = decode::DStarConfig{}.rate;
constexpr dsp::SampleRate kTetraTapRate = decode::TetraConfig{}.rate;

// Silence either side of the transmission. The leading half second lets the
// channelizer's filter fill and gives the decoder something to throw away
// before the carrier; the trailing half second carries the last data unit
// past the decoder's own filter delay and timing window, which
// core/dsp/synth/dv_mod.h says a test has to supply.
constexpr double kQuietSeconds = 0.5;

// Long enough for a loaded machine to finish a file of about a second
// unthrottled. The wait for the last message to cross the socket is
// tests/rpc/decoded_log.h's.
constexpr int kRunTimeoutMs = 60'000;

// ---------------------------------------------------------------------------
// The transmissions
// ---------------------------------------------------------------------------

// A P25 header as tests/decode/test_p25p1.cpp builds one, in clear or not.
[[nodiscard]] decode::P25Header p25_header(bool encrypted, std::uint16_t talkgroup) {
    decode::P25Header header;
    for (std::size_t i = 0; i < header.message_indicator.size(); ++i) {
        header.message_indicator[i] =
            encrypted ? static_cast<std::uint8_t>(0x11 * (i + 1)) : std::uint8_t{0};
    }
    header.manufacturer_id = 0x00;
    // TIA-102.BAAC clause 2.8: 0x80 is unencrypted, and 0x84 is AES.
    header.algorithm_id = encrypted ? std::uint8_t{0x84} : decode::kP25AlgidUnencrypted;
    header.key_id = encrypted ? std::uint16_t{0x1234} : std::uint16_t{0};
    header.talkgroup_id = talkgroup;
    return header;
}

constexpr std::uint16_t kNac = 0x293;
constexpr std::uint16_t kClearTalkgroup = 0x02A7;
constexpr std::uint16_t kSecureTalkgroup = 0x0100;

// A clear header and an encrypted one, alternating, three of each, rendered
// as one continuous C4FM transmission at the file's rate.
[[nodiscard]] Expected<std::vector<dsp::Complex32>> p25_transmission() {
    std::vector<std::uint8_t> dibits;
    for (int round = 0; round < 3; ++round) {
        for (const bool encrypted : {false, true}) {
            siggen::P25HeaderMessage message;
            message.network_access_code = kNac;
            message.header = p25_header(encrypted,
                                        encrypted ? kSecureTalkgroup : kClearTalkgroup);
            auto one = siggen::p25_header_message_dibits(message);
            if (!one) {
                return std::unexpected(one.error());
            }
            dibits.insert(dibits.end(), one->begin(), one->end());
        }
    }

    // Rendered straight at the file's rate, with the transmitter's filter span
    // held as the rate rises, for the reason core/rpc/decoders.h scales a
    // receiver's.
    siggen::P25ModConfig mod;
    mod.filter_taps = rpc::decoders_detail::scaled_taps(mod.filter_taps, mod.rate, kFileRate);
    mod.rate = kFileRate;
    return siggen::p25_render_dibits(mod, dibits);
}

[[nodiscard]] decode::DStarHeader dstar_header() {
    decode::DStarHeader header;
    // Clause 4.1.1 c: voice, addressed to a repeater, null response.
    header.flag1 = 0b0100'0000;
    header.destination_repeater = "JP1YIU A";
    header.departure_repeater = "JP1YIU G";
    header.companion = "CQCQCQ";
    header.own_callsign = "JA1RL";
    header.own_suffix = "MOBL";
    return header;
}

[[nodiscard]] Expected<std::vector<dsp::Complex32>> dstar_transmission() {
    siggen::DStarMessage message;
    message.header = dstar_header();
    // A second of voice, the AMBE slots holding a fixed pattern: the payload
    // is never rendered and only has to be there.
    message.voice_frames.resize(50);
    for (std::size_t frame = 0; frame < message.voice_frames.size(); ++frame) {
        for (std::size_t bit = 0; bit < decode::kDStarVoiceBits; ++bit) {
            message.voice_frames[frame][bit] = static_cast<std::uint8_t>((frame + bit) & 1U);
        }
    }

    siggen::DStarModConfig mod;
    mod.filter_taps = rpc::decoders_detail::scaled_taps(mod.filter_taps, mod.rate, kFileRate);
    mod.rate = kFileRate;
    return siggen::dstar_render(mod, message);
}

[[nodiscard]] decode::TetraSyncPdu tetra_pdu(std::uint8_t frame) {
    decode::TetraSyncPdu pdu;
    // tests/decode/test_tetra.cpp's cell: table 21.76 system code 0011, colour
    // code 37, timeslot 00 which is timeslot 1; table 18.17 MCC 234.
    pdu.system_code = 0b0011;
    pdu.colour_code = 37;
    pdu.timeslot = 0;
    pdu.frame_number = frame;
    pdu.multiframe_number = 42;
    pdu.uplane_dtx_allowed = true;
    pdu.mobile_country_code = 234;
    pdu.mobile_network_code = 1'234;
    pdu.neighbour_cell_broadcast = 1;
    pdu.cell_load = 2;
    pdu.late_entry_supported = true;
    return pdu;
}

constexpr std::size_t kTetraBursts = 18;

[[nodiscard]] Expected<std::vector<dsp::Complex32>> tetra_transmission() {
    std::vector<std::uint8_t> bits;
    for (std::size_t burst = 0; burst < kTetraBursts; ++burst) {
        // Clause 9 numbers frames 1 to 18; walking the field is what lets the
        // case say which bursts arrived.
        auto one = siggen::tetra_sync_burst_bits(
            tetra_pdu(static_cast<std::uint8_t>(1 + burst)), 0xA11CE + burst);
        if (!one) {
            return std::unexpected(one.error());
        }
        bits.insert(bits.end(), one->begin(), one->end());
    }

    siggen::TetraModConfig mod;
    mod.filter_taps = rpc::decoders_detail::scaled_taps(mod.filter_taps, mod.rate, kFileRate);
    mod.rate = kFileRate;
    return siggen::tetra_render_bits(mod, bits);
}

// ---------------------------------------------------------------------------
// The file
// ---------------------------------------------------------------------------

// A cf32 file the case owns and removes. Named for the case and for the run,
// for the reason tests/support/temp_path.h gives.
class CaptureFile {
public:
    explicit CaptureFile(std::string_view tag)
        : path_(test::unique_temp_path(std::format("revenant-decode-{}", tag), ".cf32")) {}

    CaptureFile(const CaptureFile&) = delete;
    CaptureFile& operator=(const CaptureFile&) = delete;

    ~CaptureFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    // Quiet, then the transmission moved up to kCarrierHz, then quiet unless
    // the case wants the file to stop inside the transmission.
    [[nodiscard]] Status write(std::span<const dsp::Complex32> signal,
                               bool trailing_quiet = true) {
        const auto quiet =
            static_cast<std::size_t>(kQuietSeconds * static_cast<double>(kFileRate));
        std::vector<dsp::Complex32> all(quiet, dsp::Complex32{});
        all.reserve(quiet * 2 + signal.size());
        for (std::size_t n = 0; n < signal.size(); ++n) {
            // The phase reduced exactly in integers, so it does not drift
            // with the length of the capture.
            const std::int64_t turns =
                (kCarrierHz * static_cast<std::int64_t>(n)) % kFileRate;
            const double angle = 2.0 * std::numbers::pi * static_cast<double>(turns) /
                                 static_cast<double>(kFileRate);
            const std::complex<double> moved =
                std::complex<double>(signal[n]) * std::polar(1.0, angle);
            all.emplace_back(static_cast<float>(moved.real()), static_cast<float>(moved.imag()));
        }
        if (trailing_quiet) {
            all.insert(all.end(), quiet, dsp::Complex32{});
        }
        samples_ = all.size();

        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        if (!out) {
            return fail(std::format("could not write {}", path_.string()));
        }
        out.write(reinterpret_cast<const char*>(all.data()),
                  static_cast<std::streamsize>(all.size() * sizeof(dsp::Complex32)));
        if (!out) {
            return fail(std::format("writing {} failed part way through", path_.string()));
        }
        return {};
    }

    // A UHF centre, as tests/engine/test_engine_dv.cpp gives its file, so the
    // source's resolution request is not the HF one. Receivers are placed in
    // baseband and do not see it.
    [[nodiscard]] std::string uri() const {
        return std::format("file:///{}?rate={}&format=cf32&center=420000000",
                           path_.generic_string(), kFileRate);
    }
    [[nodiscard]] dsp::SampleIndex samples() const { return samples_; }

private:
    std::filesystem::path path_;
    dsp::SampleIndex samples_ = 0;
};

void bring_up(Harness& harness, const CaptureFile& file) {
    HarnessOptions options;
    options.source_uri = file.uri();
    options.channels = kGridChannels;
    options.block_samples = kBlockSamples;
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());
}

// Runs the file to its end and joins, which flushes the last blocks through
// the sink before it returns.
void run_to_completion(Harness& harness, const CaptureFile& file) {
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
}

[[nodiscard]] rpc::VrxParams on_the_carrier(rpc::Demod mode) {
    // The mode's own default passband, from zero bandwidth, on the carrier.
    return rpc::VrxParams{.center = kCarrierHz, .bandwidth = 0, .demod = mode};
}

}  // namespace

// ---------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------

TEST_CASE("the decoder registry crosses the wire", "[gpu][rpc][decode]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    const auto ready = harness.open(HarnessOptions{});
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    auto listed = harness.client().decoders();
    INFO(test::message_of(listed));
    REQUIRE(listed.has_value());

    // Against the registry itself rather than a list written here, so a
    // decoder added to core/rpc/decoders.h is checked by this case without
    // anybody remembering to extend it.
    const std::span<const rpc::DecoderSpec> registry = rpc::decoder_registry();
    REQUIRE(listed->size() == registry.size());
    for (std::size_t i = 0; i < registry.size(); ++i) {
        CHECK((*listed)[i].name == registry[i].name);
        CHECK((*listed)[i].input == registry[i].input);
        CHECK((*listed)[i].description == registry[i].description);
    }

    std::set<std::string> names;
    for (const rpc::DecoderInfo& info : *listed) {
        names.insert(info.name);
    }
    CHECK(names.contains("p25p1"));
    CHECK(names.contains("dstar"));
    CHECK(names.contains("tetra"));
}

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------

TEST_CASE("a decoder the receiver cannot feed is refused in words", "[gpu][rpc][decode]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    const auto ready = harness.open(HarnessOptions{});
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    auto log = std::make_shared<MessageLog>();

    // No such receiver: the engine's own words.
    auto absent = harness.client().subscribe_decoded(9'999, "p25p1", into(log), ending(log));
    REQUIRE_FALSE(absent.has_value());
    INFO(absent.error().message);
    CHECK(absent.error().message.find("9999") != std::string::npos);

    auto audio = harness.client().add_vrx(
        rpc::VrxParams{.center = 131'072, .bandwidth = 12'000, .demod = rpc::Demod::Nfm});
    INFO(test::message_of(audio));
    REQUIRE(audio.has_value());

    // A complex decoder on an audio receiver, naming both halves.
    auto wrong_input = harness.client().subscribe_decoded(*audio, "p25p1", into(log), ending(log));
    REQUIRE_FALSE(wrong_input.has_value());
    INFO(wrong_input.error().message);
    CHECK(wrong_input.error().message.find("reads complex baseband") != std::string::npos);
    CHECK(wrong_input.error().message.find("nfm") != std::string::npos);

    // A name this engine does not have, listing the ones it does.
    auto unknown = harness.client().subscribe_decoded(*audio, "morse", into(log), ending(log));
    REQUIRE_FALSE(unknown.has_value());
    INFO(unknown.error().message);
    CHECK(unknown.error().message.find("no decoder named 'morse'") != std::string::npos);
    CHECK(unknown.error().message.find("p25p1") != std::string::npos);

    // The default on a mode nothing is named after.
    auto by_mode = harness.client().subscribe_decoded(*audio, "", into(log), ending(log));
    REQUIRE_FALSE(by_mode.has_value());
    INFO(by_mode.error().message);
    CHECK(by_mode.error().message.find("no decoder is named after that mode") !=
          std::string::npos);

    // None of those left anything to hold.
    auto stats = harness.client().decoded_stats(*audio, "p25p1");
    CHECK_FALSE(stats.has_value());
    CHECK_FALSE(log->ended());
}

TEST_CASE("removing the receiver ends its decoded stream with the reason",
          "[gpu][rpc][decode]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    const auto ready = harness.open(HarnessOptions{});
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    auto vrx = harness.client().add_vrx(
        rpc::VrxParams{.center = 131'072, .bandwidth = 12'500, .demod = rpc::Demod::P25p1});
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto log = std::make_shared<MessageLog>();
    auto subscribed = harness.client().subscribe_decoded(*vrx, "", into(log), ending(log));
    INFO(test::message_of(subscribed));
    REQUIRE(subscribed.has_value());
    CHECK(*subscribed == "p25p1");

    auto live = harness.client().decoded_stats(*vrx, "");
    INFO(test::message_of(live));
    REQUIRE(live.has_value());
    CHECK(live->messages_sent == 0);

    REQUIRE(harness.client().remove_vrx(*vrx).has_value());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!log->ended() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    INFO(log->reason());
    REQUIRE(log->ended());
    CHECK(log->reason() == "the receiver was removed");

    // Forgotten on this side once ended() arrived, so stats has nothing to ask.
    auto after = harness.client().decoded_stats(*vrx, "");
    CHECK_FALSE(after.has_value());
}

// ---------------------------------------------------------------------------
// Through the engine
// ---------------------------------------------------------------------------

TEST_CASE("a P25 header crosses the wire as a decoded message", "[gpu][rpc][decode]") {
    REVENANT_NEEDS_GPU();

    auto signal = p25_transmission();
    INFO(test::message_of(signal));
    REQUIRE(signal.has_value());
    CaptureFile file("p25p1");
    REQUIRE(file.write(*signal).has_value());

    Harness harness;
    bring_up(harness, file);

    auto vrx = harness.client().add_vrx(on_the_carrier(rpc::Demod::P25p1));
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    // Two subscribers on two connections, sharing one decoder. Both see the
    // same messages under the same sequence numbers, which is what "per
    // decoder and not per subscription" means on the wire.
    auto log = std::make_shared<MessageLog>();
    auto resolved = harness.client().subscribe_decoded(*vrx, "", into(log), ending(log));
    INFO(test::message_of(resolved));
    REQUIRE(resolved.has_value());
    CHECK(*resolved == "p25p1");

    auto other = harness.connect_another();
    INFO(test::message_of(other));
    REQUIRE(other.has_value());
    auto other_log = std::make_shared<MessageLog>();
    auto second =
        (*other)->subscribe_decoded(*vrx, "p25p1", into(other_log), ending(other_log));
    INFO(test::message_of(second));
    REQUIRE(second.has_value());

    run_to_completion(harness, file);

    const auto has_both = [](const std::vector<rpc::DecodedMessage>& seen) {
        bool clear = false;
        bool secure = false;
        for (const rpc::DecodedMessage& message : seen) {
            const rpc::DecodedField* tg = message.field("talkgroup");
            if (tg == nullptr || tg->integer() == nullptr) {
                continue;
            }
            clear = clear || *tg->integer() == kClearTalkgroup;
            secure = secure || *tg->integer() == kSecureTalkgroup;
        }
        return clear && secure;
    };
    const auto seen = wait_for(*log, has_both);
    std::string arrived;
    for (const rpc::DecodedMessage& message : seen) {
        arrived += std::format("\n  #{} [{}, {}) {}", message.sequence, message.start_sample,
                               message.end_sample, message.text);
    }
    INFO(std::format("{} messages arrived:{}", seen.size(), arrived));
    REQUIRE(has_both(seen));

    std::size_t headers = 0;
    std::size_t trusted = 0;
    double worst_offset = 0.0;
    double narrowest = 2.0;
    double widest = 0.0;
    for (const rpc::DecodedMessage& message : seen) {
        CHECK(message.vrx == *vrx);
        CHECK(message.decoder == "p25p1");
        CHECK(message.sample_rate == kP25TapRate);
        CHECK(message.end_sample > message.start_sample);

        // THE NAC IS HELD TO THE TRANSMITTED ONE ONLY WHERE THE CODE VOUCHES
        // FOR IT. core/decode/p25p1.h: the NID's BCH code guarantees up to 11
        // corrections, and above that it answers with the nearest code word.
        // The tail of this capture, where the transmission stops and the
        // filters ring down, is where such a NID turns up; measured on
        // 2026-09-22 it read a terminator with NAC 0x1B3. The field is on the
        // wire so a client can make exactly this call.
        if (integer_of(message, "nid_corrected_bits") > 11) {
            continue;
        }
        ++trusted;
        CHECK(integer_of(message, "nac") == kNac);

        // The receiver sits on the carrier and the fine stage mixes it to
        // DC, so the sync word's fit should find it there. The deviation
        // reads under nominal, as tests/decode/test_p25p1_blocking.cpp
        // measures without the engine: the outer symbols land inside +/-3.
        // Loose, and printed so the figures through the engine are on record.
        const double offset = real_of(message, "carrier_offset_hz");
        const double deviation = real_of(message, "deviation_ratio");
        worst_offset = std::max(worst_offset, std::abs(offset));
        narrowest = std::min(narrowest, deviation);
        widest = std::max(widest, deviation);
        CHECK(std::abs(offset) < 50.0);
        CHECK(deviation > 0.75);
        CHECK(deviation < 1.1);

        if (message.kind != "hdu") {
            continue;
        }
        ++headers;
        const bool encrypted = flag_of(message, "encrypted");
        const std::int64_t talkgroup = integer_of(message, "talkgroup");
        if (encrypted) {
            CHECK(talkgroup == kSecureTalkgroup);
            CHECK(integer_of(message, "algorithm_id") == 0x84);
            CHECK(integer_of(message, "key_id") == 0x1234);
            const rpc::DecodedField* mi = message.field("message_indicator");
            REQUIRE(mi != nullptr);
            REQUIRE(mi->bytes() != nullptr);
            CHECK(mi->bytes()->size() == 9);
            CHECK(mi->bytes()->front() == 0x11);
            CHECK(message.text.find("encrypted") != std::string::npos);
        } else {
            CHECK(talkgroup == kClearTalkgroup);
            CHECK(integer_of(message, "algorithm_id") == 0x80);
            CHECK(message.text.find("clear") != std::string::npos);
        }
        CHECK(message.text.find("NAC 0x293 hdu") == 0);
    }
    INFO(std::format("{} headers among {} messages, {} with a NID the code vouches for",
                     headers, seen.size(), trusted));
    CHECK(headers == 6);
    CHECK(trusted + 1 >= seen.size());
    WARN(std::format("p25p1: carrier offset within {:.2f} Hz of DC and deviation {:.4f} to "
                     "{:.4f} of nominal over {} trusted data units",
                     worst_offset, narrowest, widest, trusted));

    // The sequence numbers are the decoder's and not the subscription's.
    const auto theirs =
        wait_for(*other_log, [&](const auto& got) { return got.size() >= seen.size(); });
    std::set<std::uint64_t> mine_seq;
    std::set<std::uint64_t> their_seq;
    for (const auto& message : seen) {
        mine_seq.insert(message.sequence);
    }
    for (const auto& message : theirs) {
        their_seq.insert(message.sequence);
    }
    CHECK(mine_seq == their_seq);
    CHECK_FALSE(log->ended());

    auto stats = harness.client().decoded_stats(*vrx, "");
    INFO(test::message_of(stats));
    REQUIRE(stats.has_value());
    CHECK(stats->messages_sent == seen.size());
    CHECK(stats->messages_dropped == 0);
    CHECK(stats->sample_rate == kP25TapRate);

    // What the adapter costs, measured on this machine rather than asserted:
    // the same transmission fed to the adapter directly, off the engine, in
    // chunks of one engine block's worth at the tap's rate, 16384 source
    // samples at 288000 being 2731 at 48000. Printed for docs/rpc.md.
    auto decoder =
        rpc::P25p1Decoder::make(rpc::DecoderBuild{.rate = kP25TapRate, .mode = "p25p1"});
    REQUIRE(decoder.has_value());
    constexpr std::size_t kStep = static_cast<std::size_t>(kFileRate / kP25TapRate);
    std::vector<float> interleaved;
    for (std::size_t i = 0; i < signal->size(); i += kStep) {
        // Every sixth sample, which is the tap's rate from the file's. No
        // anti-alias filter, which a cost figure does not care about.
        interleaved.push_back((*signal)[i].real());
        interleaved.push_back((*signal)[i].imag());
    }
    const std::size_t frames = interleaved.size() / 2;
    constexpr std::size_t kChunk = 2'731;
    std::vector<rpc::DecodedMessage> scratch;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t at = 0; at < frames; at += kChunk) {
        const std::size_t count = std::min(kChunk, frames - at);
        const rpc::DecoderChunk chunk{
            .samples = std::span<const float>(interleaved).subspan(at * 2, count * 2),
            .channels = 2,
            .rate = kP25TapRate,
            .start = at,
        };
        REQUIRE((*decoder)->consume(chunk, scratch).has_value());
    }
    const double seconds_of_signal =
        static_cast<double>(frames) / static_cast<double>(kP25TapRate);
    const double spent_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    WARN(std::format("p25p1 adapter: {:.2f} ms of one core per second of {} S/s tap, over "
                     "{:.3f} s of signal in {}-sample chunks; {} messages",
                     spent_ms / seconds_of_signal, kP25TapRate, seconds_of_signal, kChunk,
                     scratch.size()));
}

TEST_CASE("a D-STAR header crosses the wire as a decoded message", "[gpu][rpc][decode]") {
    REVENANT_NEEDS_GPU();

    auto signal = dstar_transmission();
    INFO(test::message_of(signal));
    REQUIRE(signal.has_value());
    CaptureFile file("dstar");
    REQUIRE(file.write(*signal).has_value());

    Harness harness;
    bring_up(harness, file);

    const rpc::VrxParams params = on_the_carrier(rpc::Demod::Dstar);
    auto vrx = harness.client().add_vrx(params);
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto log = std::make_shared<MessageLog>();
    auto resolved = harness.client().subscribe_decoded(*vrx, "dstar", into(log), ending(log));
    INFO(test::message_of(resolved));
    REQUIRE(resolved.has_value());

    // A retune after the decoder is attached and before a chunk has moved.
    // The fence is raised to the receiver's new epoch and has to come down
    // again on the first chunk that carries it; a fence that stuck would
    // discard the whole file and this case would see no header at all.
    REQUIRE(harness.client().set_vrx_params(*vrx, params).has_value());

    run_to_completion(harness, file);

    // THE WHOLE TRANSMISSION, a superframe at a time. 50 voice frames with the
    // resynchronisation signal at 0, 21 and 42 are three pieces of 21, 21 and
    // 8, the last closed by the clause 4.1.2 h last frame. Until 2026-09-23
    // the adapter reported the header's piece and dropped the other two.
    const auto seen = wait_for(*log, [](const auto& got) { return got.size() >= 3; });
    std::string arrived;
    for (const rpc::DecodedMessage& message : seen) {
        arrived += std::format("\n  #{} {} {}", message.sequence, message.kind, message.text);
    }
    INFO(std::format("{} messages arrived:{}", seen.size(), arrived));
    REQUIRE(seen.size() == 3);

    constexpr std::array<std::int64_t, 3> kFrames = {21, 21, 8};
    std::int64_t total = 0;
    for (std::size_t i = 0; i < seen.size(); ++i) {
        const rpc::DecodedMessage& piece = seen[i];
        total += kFrames[i];
        CHECK(piece.kind == (i == 0 ? "header" : "superframe"));
        CHECK(integer_of(piece, "superframe") == static_cast<std::int64_t>(i));
        CHECK(integer_of(piece, "voice_frames") == kFrames[i]);
        CHECK(integer_of(piece, "voice_frames_total") == total);
        CHECK(flag_of(piece, "ended") == (i + 1 == seen.size()));
        CHECK_FALSE(flag_of(piece, "flushed"));
        CHECK(text_of(piece, "my") == "JA1RL");
        CHECK(text_of(piece, "ur") == "CQCQCQ");

        // The first frame sync's fit, on every piece. The receiver is on the
        // carrier and the transmitter at GMSK's nominal deviation, and the
        // fit reads both low for the reason tests/decode/test_dstar_blocking.cpp
        // gives: at BT 0.5 a lone bit falls short of full deviation, and the
        // frame sync is mostly lone bits. Measured there without the engine,
        // 0.646 to 0.693 of nominal and 40 to 80 Hz under the carrier.
        CHECK(real_of(piece, "carrier_offset_hz") == real_of(seen.front(), "carrier_offset_hz"));
        CHECK(std::abs(real_of(piece, "carrier_offset_hz")) < 120.0);
        CHECK(real_of(piece, "deviation_ratio") > 0.55);
        CHECK(real_of(piece, "deviation_ratio") < 0.8);
    }
    WARN(std::format("dstar: carrier offset {:.2f} Hz, deviation ratio {:.4f}",
                     real_of(seen.front(), "carrier_offset_hz"),
                     real_of(seen.front(), "deviation_ratio")));

    const rpc::DecodedMessage& header = seen.front();
    CHECK(header.decoder == "dstar");
    CHECK(header.kind == "header");
    CHECK(header.sample_rate == kDStarTapRate);
    CHECK(flag_of(header, "fcs_valid"));
    CHECK(text_of(header, "my") == "JA1RL");
    CHECK(text_of(header, "my_suffix") == "MOBL");
    CHECK(text_of(header, "ur") == "CQCQCQ");
    CHECK(text_of(header, "rpt1") == "JP1YIU G");
    CHECK(text_of(header, "rpt2") == "JP1YIU A");
    CHECK(flag_of(header, "via_repeater"));
    CHECK_FALSE(flag_of(header, "data"));
    CHECK(integer_of(header, "flag1") == 0b0100'0000);
    INFO(header.text);
    CHECK(header.text.find("MY JA1RL/MOBL UR CQCQCQ") == 0);

    auto stats = harness.client().decoded_stats(*vrx, "dstar");
    INFO(test::message_of(stats));
    REQUIRE(stats.has_value());
    WARN(std::format("dstar: {} chunks discarded by the retune fence before the first chunk of "
                     "the new tuning",
                     stats->chunks_discarded));
}

TEST_CASE("a D-STAR transmission still open when its receiver goes is reported before ended",
          "[gpu][rpc][decode]") {
    REVENANT_NEEDS_GPU();

    // The same transmission cut off about two thirds of the way through, with
    // no quiet after it, so the file stops inside the second superframe and
    // neither the last frame nor a missing resynchronisation signal closes it.
    auto signal = dstar_transmission();
    INFO(test::message_of(signal));
    REQUIRE(signal.has_value());
    const std::size_t cut = signal->size() * 2 / 3;
    CaptureFile file("dstar-cut");
    REQUIRE(file.write(std::span<const dsp::Complex32>(*signal).first(cut), false).has_value());

    Harness harness;
    bring_up(harness, file);

    auto vrx = harness.client().add_vrx(on_the_carrier(rpc::Demod::Dstar));
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto log = std::make_shared<MessageLog>();
    REQUIRE(harness.client().subscribe_decoded(*vrx, "dstar", into(log), ending(log)).has_value());

    run_to_completion(harness, file);

    // The header's superframe, and nothing after it: the frames since are
    // held by the decoder, waiting for a boundary that is not coming.
    const auto before = wait_for(*log, [](const auto& got) { return !got.empty(); });
    REQUIRE(before.size() == 1);
    CHECK(before.front().kind == "header");
    CHECK(integer_of(before.front(), "voice_frames") == 21);
    CHECK_FALSE(flag_of(before.front(), "ended"));

    REQUIRE(harness.client().remove_vrx(*vrx).has_value());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!log->ended() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    INFO(log->reason());
    REQUIRE(log->ended());
    CHECK(log->reason() == "the receiver was removed");

    // DELIVERED AHEAD OF ended(), which the client forgets the subscription
    // on, so a message that arrived after it would not be in the log at all.
    const auto after = log->messages();
    std::string arrived;
    for (const rpc::DecodedMessage& message : after) {
        arrived += std::format("\n  #{} {} {}", message.sequence, message.kind, message.text);
    }
    INFO(std::format("{} messages arrived:{}", after.size(), arrived));
    REQUIRE(after.size() == 2);
    const rpc::DecodedMessage& tail = after.back();
    CHECK(tail.kind == "superframe");
    CHECK(integer_of(tail, "superframe") == 1);
    CHECK(flag_of(tail, "flushed"));
    CHECK_FALSE(flag_of(tail, "ended"));
    CHECK(text_of(tail, "my") == "JA1RL");
    const std::int64_t held = integer_of(tail, "voice_frames");
    WARN(std::format("dstar: {} voice frames after the header's superframe handed over by flush",
                     held));
    CHECK(held > 0);
    CHECK(held < 21);
    CHECK(integer_of(tail, "voice_frames_total") == 21 + held);
}

TEST_CASE("TETRA synchronisation bursts cross the wire as decoded messages",
          "[gpu][rpc][decode]") {
    REVENANT_NEEDS_GPU();

    auto signal = tetra_transmission();
    INFO(test::message_of(signal));
    REQUIRE(signal.has_value());
    CaptureFile file("tetra");
    REQUIRE(file.write(*signal).has_value());

    Harness harness;
    bring_up(harness, file);

    auto vrx = harness.client().add_vrx(on_the_carrier(rpc::Demod::Tetra));
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto log = std::make_shared<MessageLog>();
    auto resolved = harness.client().subscribe_decoded(*vrx, "", into(log), ending(log));
    INFO(test::message_of(resolved));
    REQUIRE(resolved.has_value());
    CHECK(*resolved == "tetra");

    run_to_completion(harness, file);

    // Most of the eighteen, not all: the first burst sits inside the
    // receiver's own transient and the last inside its filter delay, which
    // tests/decode/test_tetra.cpp measures on its own terms.
    constexpr std::size_t kEnough = kTetraBursts / 2;
    const auto count_sync = [](const std::vector<rpc::DecodedMessage>& got) {
        std::size_t n = 0;
        for (const auto& message : got) {
            n += message.kind == "sync" ? 1U : 0U;
        }
        return n;
    };
    const auto seen =
        wait_for(*log, [&](const auto& got) { return count_sync(got) >= kEnough; });
    INFO(std::format("{} messages arrived, {} of them sync", seen.size(), count_sync(seen)));
    REQUIRE(count_sync(seen) >= kEnough);

    std::set<std::int64_t> frames;
    for (const rpc::DecodedMessage& message : seen) {
        CHECK(message.decoder == "tetra");
        CHECK(message.sample_rate == kTetraTapRate);
        if (message.kind != "sync") {
            continue;
        }
        CHECK(flag_of(message, "block_code_verified"));
        CHECK(integer_of(message, "mcc") == 234);
        CHECK(integer_of(message, "mnc") == 1'234);
        CHECK(integer_of(message, "colour_code") == 37);
        CHECK(integer_of(message, "timeslot") == 1);
        CHECK(integer_of(message, "multiframe_number") == 42);
        const std::int64_t frame = integer_of(message, "frame_number");
        CHECK(frame >= 1);
        CHECK(frame <= static_cast<std::int64_t>(kTetraBursts));
        frames.insert(frame);
        CHECK(message.text.find("MCC 234 MNC 1234 CC 37 TN 1") == 0);
    }

    // Distinct bursts rather than one burst reported many times.
    CHECK(frames.size() >= kEnough);
}
