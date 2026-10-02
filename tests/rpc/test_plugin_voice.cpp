// D-STAR and DMR voice through a vocoder plugin: core/rpc/plugin_voice.h.
//
// The plugin is a test double here, a decode::Vocoder that records every unit
// it is handed and answers each with a constant, so a case can say exactly
// which bits reached the plugin and that its audio came out. The DLL side of
// the seam is held in tests/decode/test_vocoder_plugin.cpp and the wire in
// test_rpc_vocoders.cpp.
//
// Each case names the wrong implementation it rejects.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/decode/dmr.h"
#include "core/decode/dstar.h"
#include "core/decode/vocoder.h"
#include "core/decode/vocoder_plugin.h"
#include "core/decode/dmr_codes.h"
#include "core/dsp/synth/dmr_mod.h"
#include "core/dsp/synth/dv_mod.h"
#include "core/dsp/types.h"
#include "core/rpc/plugin_voice.h"

using namespace revenant;
using rpc::PluginVoiceMode;
using rpc::PluginVoiceStream;

namespace {

constexpr dsp::SampleRate kRate = 48'000;

// What the double decodes to, and how many samples a unit makes. Not any
// codec's numbers: the stream takes the shape the plugin declares.
constexpr float kVoiceLevel = 0.25F;
constexpr std::uint32_t kPcmPerUnit = 160;
constexpr std::uint32_t kPluginRate = 8000;

class RecordingVocoder final : public decode::Vocoder {
public:
    explicit RecordingVocoder(std::uint32_t bits, std::vector<std::vector<std::uint8_t>>& seen)
        : bits_(bits), seen_(seen) {}

    [[nodiscard]] decode::VocoderFrame shape() const noexcept override {
        return {decode::VocoderKind::External, bits_, kPcmPerUnit, kPluginRate};
    }

    [[nodiscard]] Status decode(std::span<const std::uint8_t> bits,
                                std::span<float> out) override {
        if (auto checked = decode::check_vocoder_call(shape(), bits, out); !checked) {
            return checked;
        }
        seen_.emplace_back(bits.begin(), bits.end());
        std::fill_n(out.begin(), kPcmPerUnit, kVoiceLevel);
        return {};
    }

    void reset() override {}
    [[nodiscard]] std::string_view name() const noexcept override { return "recording-double"; }

private:
    std::uint32_t bits_;
    std::vector<std::vector<std::uint8_t>>& seen_;
};

struct Played {
    std::size_t voiced_samples = 0;
    std::size_t total_samples = 0;
    std::uint64_t next_start = 0;
    bool contiguous = true;
};

// Feeds the capture through in 20 ms chunks, as a receiver would hand it over,
// and counts what came out.
[[nodiscard]] Played play(PluginVoiceStream& stream, std::span<const dsp::Complex32> iq) {
    const std::size_t chunk = static_cast<std::size_t>(kRate / 50);
    Played played;
    rpc::VoiceChunk out;
    std::vector<float> interleaved;
    for (std::size_t at = 0; at < iq.size(); at += chunk) {
        const std::size_t n = std::min(chunk, iq.size() - at);
        interleaved.resize(2 * n);
        for (std::size_t i = 0; i < n; ++i) {
            interleaved[2 * i] = iq[at + i].real();
            interleaved[2 * i + 1] = iq[at + i].imag();
        }
        const rpc::DecoderChunk in{
            .samples = interleaved, .channels = 2, .rate = kRate, .start = at};
        REQUIRE(stream.process(in, false, out).has_value());
        if (out.start != played.next_start) {
            played.contiguous = false;
        }
        played.next_start = out.start + out.samples.size();
        played.total_samples += out.samples.size();
        played.voiced_samples += static_cast<std::size_t>(
            std::count(out.samples.begin(), out.samples.end(), kVoiceLevel));
    }
    return played;
}

[[nodiscard]] decode::DStarHeader dstar_header() {
    decode::DStarHeader header;
    header.flag1 = 0b0100'0000;
    header.destination_repeater = "JP1YIU A";
    header.departure_repeater = "JP1YIU G";
    header.companion = "CQCQCQ";
    header.own_callsign = "JA1RL";
    header.own_suffix = "MOBL";
    return header;
}

[[nodiscard]] std::vector<dsp::Complex32> dstar_capture(
    const std::vector<std::array<std::uint8_t, decode::kDStarVoiceBits>>& voice) {
    siggen::DStarMessage message;
    message.header = dstar_header();
    message.voice_frames = voice;
    siggen::DStarModConfig mod;
    mod.rate = kRate;
    auto samples = siggen::dstar_render(mod, message);
    REQUIRE(samples.has_value());
    // A second of quiet after, so the held audio plays out.
    samples->insert(samples->end(), static_cast<std::size_t>(kRate), dsp::Complex32{});
    return std::move(*samples);
}

constexpr std::uint8_t kColourCode = 7;
constexpr std::uint8_t kPrivacy = 0x40;  // TS 102 361-2 Table 7.11, Service Options

[[nodiscard]] std::array<std::uint8_t, 9> group_lc(std::uint8_t options) {
    return {0x00, 0x00, options, 0x00, 0x0C, 0x31, 0x30, 0x39, 0x30};
}

template <typename Bits>
[[nodiscard]] std::vector<Bits> random_units(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    std::vector<Bits> out(count);
    for (Bits& unit : out) {
        for (std::uint8_t& bit : unit) {
            bit = static_cast<std::uint8_t>(random() & 1U);
        }
    }
    return out;
}

// One call on timeslot 1 of a base station channel with its CACH, idle on 2,
// with idle slots either side for the receiver to settle on.
[[nodiscard]] std::vector<dsp::Complex32> dmr_capture(const siggen::DmrVoiceCall& call) {
    auto bursts = siggen::dmr_voice_call_bursts(call);
    REQUIRE(bursts.has_value());
    const std::size_t lead = 8;
    const siggen::DmrShortLcFragments short_lc =
        siggen::dmr_short_lc_fragments(decode::kDmrSlcoActivityUpdate, 0x88'1234U);
    std::vector<siggen::DmrSlot> slots;
    const std::size_t length = bursts->size() + 2 * lead;
    for (std::size_t i = 0; i < 2 * length; ++i) {
        const std::size_t frame = i / 2;
        const bool first = i % 2 == 0;
        siggen::DmrSlot slot;
        const std::size_t fragment = i % decode::kDmrShortLcFragments;
        slot.cach = siggen::dmr_cach(true, first ? 1 : 2, short_lc.lcss[fragment],
                                     short_lc.payload[fragment]);
        if (first && frame >= lead && frame - lead < bursts->size()) {
            slot.burst = (*bursts)[frame - lead];
        } else {
            slot.burst = siggen::dmr_idle_burst(decode::DmrSyncType::BsData, kColourCode);
        }
        slots.push_back(slot);
    }
    auto samples = siggen::dmr_render_slots(siggen::DmrModConfig{}, slots);
    REQUIRE(samples.has_value());
    samples->insert(samples->end(), static_cast<std::size_t>(kRate), dsp::Complex32{});
    return std::move(*samples);
}

}  // namespace

TEST_CASE("a plugin serves a mode by its name and nothing that only starts like it",
          "[rpc][vocoder][voice]") {
    // Rejects a bare prefix match, which would route DMR to "dmrx" and to a
    // plugin for some other mode whose name happens to begin with "dmr".
    CHECK(decode::vocoder_name_serves("dmr", "dmr"));
    CHECK(decode::vocoder_name_serves("dmr-acme", "dmr"));
    CHECK(decode::vocoder_name_serves("dstar:fw2", "dstar"));
    CHECK(decode::vocoder_name_serves("dstar_x", "dstar"));
    CHECK_FALSE(decode::vocoder_name_serves("dmrx", "dmr"));
    CHECK_FALSE(decode::vocoder_name_serves("acme-dmr", "dmr"));
    CHECK_FALSE(decode::vocoder_name_serves("dmr", ""));
}

TEST_CASE("D-STAR voice reaches the plugin frame for frame and plays", "[rpc][vocoder][voice]") {
    const auto voice = random_units<std::array<std::uint8_t, decode::kDStarVoiceBits>>(
        25, 0xD57A'7001ULL);
    const std::vector<dsp::Complex32> iq = dstar_capture(voice);

    std::vector<std::vector<std::uint8_t>> seen;
    auto stream = PluginVoiceStream::create(
        PluginVoiceMode::Dstar, kRate,
        std::make_unique<RecordingVocoder>(static_cast<std::uint32_t>(decode::kDStarVoiceBits), seen));
    REQUIRE(stream.has_value());
    CHECK(stream->out_rate() == kPluginRate);

    const Played played = play(*stream, iq);

    // Rejects a stream that drops frames at chunk boundaries, or hands the
    // plugin anything but the 72 bits clause 4.1.2 b puts in the voice slot.
    REQUIRE(seen.size() == voice.size());
    for (std::size_t i = 0; i < voice.size(); ++i) {
        CHECK(std::equal(seen[i].begin(), seen[i].end(), voice[i].begin(), voice[i].end()));
    }
    CHECK(played.contiguous);
    CHECK(played.voiced_samples == voice.size() * kPcmPerUnit);
}

TEST_CASE("without a plugin a voice mode is silence, not a refusal", "[rpc][vocoder][voice]") {
    // The owner's call: no plugin is quiet, so the stream exists, keeps time
    // and carries nothing.
    const auto voice = random_units<std::array<std::uint8_t, decode::kDStarVoiceBits>>(
        10, 0xD57A'7002ULL);
    const std::vector<dsp::Complex32> iq = dstar_capture(voice);

    auto stream = PluginVoiceStream::create(PluginVoiceMode::Dstar, kRate, nullptr);
    REQUIRE(stream.has_value());
    CHECK_FALSE(stream->has_vocoder());
    CHECK(stream->out_rate() == rpc::kPluginVoiceSilentRateHz);

    const Played played = play(*stream, iq);
    CHECK(played.contiguous);
    CHECK(played.voiced_samples == 0);
    CHECK(played.total_samples ==
          (iq.size() * rpc::kPluginVoiceSilentRateHz) / static_cast<std::size_t>(kRate));
}

TEST_CASE("DMR voice bursts reach the plugin whole, VS(215) first", "[rpc][vocoder][voice]") {
    siggen::DmrVoiceCall call;
    call.colour_code = kColourCode;
    call.lc = group_lc(0x00);
    call.voice = random_units<siggen::DmrVoiceBits>(2 * decode::kDmrSuperframeBursts,
                                                    0xD312'7001ULL);
    const std::vector<dsp::Complex32> iq = dmr_capture(call);

    std::vector<std::vector<std::uint8_t>> seen;
    auto stream = PluginVoiceStream::create(
        PluginVoiceMode::Dmr, kRate,
        std::make_unique<RecordingVocoder>(static_cast<std::uint32_t>(decode::kDmrVoiceBits), seen));
    REQUIRE(stream.has_value());

    const Played played = play(*stream, iq);

    // Rejects splitting the vocoder socket into codec frames here, which would
    // put an AMBE+2 constant in this tree, and rejects losing bursts.
    REQUIRE(seen.size() == call.voice.size());
    for (std::size_t i = 0; i < call.voice.size(); ++i) {
        CHECK(std::equal(seen[i].begin(), seen[i].end(), call.voice[i].begin(),
                         call.voice[i].end()));
    }
    CHECK(played.voiced_samples == call.voice.size() * kPcmPerUnit);
}

TEST_CASE("a private DMR call never reaches the plugin", "[rpc][vocoder][voice]") {
    // Rejects handing encrypted voice to a vocoder: the project decodes what is
    // sent in the clear and does not decrypt, and that line holds here too.
    siggen::DmrVoiceCall call;
    call.colour_code = kColourCode;
    call.lc = group_lc(kPrivacy);
    call.voice = random_units<siggen::DmrVoiceBits>(2 * decode::kDmrSuperframeBursts,
                                                    0xD312'7002ULL);
    const std::vector<dsp::Complex32> iq = dmr_capture(call);

    std::vector<std::vector<std::uint8_t>> seen;
    auto stream = PluginVoiceStream::create(
        PluginVoiceMode::Dmr, kRate,
        std::make_unique<RecordingVocoder>(static_cast<std::uint32_t>(decode::kDmrVoiceBits), seen));
    REQUIRE(stream.has_value());

    const Played played = play(*stream, iq);
    CHECK(seen.empty());
    CHECK(played.voiced_samples == 0);
}

TEST_CASE("a plugin that takes the wrong unit is refused by name", "[rpc][vocoder][voice]") {
    // Rejects feeding 216-bit bursts to a 72-bit vocoder, which would make
    // confident noise out of misaligned bits.
    std::vector<std::vector<std::uint8_t>> seen;
    auto stream = PluginVoiceStream::create(
        PluginVoiceMode::Dmr, kRate,
        std::make_unique<RecordingVocoder>(static_cast<std::uint32_t>(decode::kDStarVoiceBits), seen));
    REQUIRE_FALSE(stream.has_value());
    CHECK(stream.error().message.find("216") != std::string::npos);
}
