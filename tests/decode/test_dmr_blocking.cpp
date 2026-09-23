// DMR: what comes out must not depend on how the input was blocked.
//
// The engine hands a receiver's stream to its decoder one block at a time,
// and tests/decode/test_p25p1_blocking.cpp has what it cost P25 when the
// output moved with the block length. This is the same case for DMR, and
// the same rule core/decode/dv_phy.h states: a stream decodes to the same
// bursts, field for field and bit for bit, however it is split.
//
// The capture is built the way a real channel arrives: the carrier a few
// hundred hertz off DC, a DC term in the baseband, noise under everything,
// and quiet either side. It carries a base station channel, a voice call on
// timeslot 1 with a CSBK, a data header and a PI header on timeslot 2 and a
// Short LC in the CACH, then a direct mode call with the carrier off between
// its bursts, so the burst grid, the lanes, the CACH's Short LC assembly and
// the search after a gap are all blocked. Decoded whole, in fixed blocks from
// 1024 to 65536 samples, in random lengths, and one sample per call.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <print>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/decode/dmr.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dmr_mod.h"

using namespace revenant;
using decode::DmrDataType;
using decode::DmrSyncType;

namespace {

constexpr dsp::SampleRate kRate = 48'000;
constexpr std::uint8_t kColourCode = 3;

// test_p25p1_blocking.cpp's channel: well inside what the modulation
// tolerates and well away from zero, so a receiver assuming either is zero
// shows it.
constexpr dsp::Hertz kCarrierOffsetHz = 350;
constexpr dsp::Complex32 kResidualDc{0.05F, -0.03F};
constexpr double kSnrFullBandDb = 20.0;
constexpr std::uint64_t kNoiseSeed = 0xB10C'0D32'0000'0001ULL;
constexpr std::uint64_t kBlockingSeed = 0xB10C'0D32'0000'0002ULL;
constexpr double kQuietSeconds = 0.5;

std::array<std::uint8_t, 9> group_lc(std::uint32_t group, std::uint32_t source) {
    return {0x00,
            0x00,
            0x00,
            static_cast<std::uint8_t>(group >> 16U),
            static_cast<std::uint8_t>(group >> 8U),
            static_cast<std::uint8_t>(group),
            static_cast<std::uint8_t>(source >> 16U),
            static_cast<std::uint8_t>(source >> 8U),
            static_cast<std::uint8_t>(source)};
}

std::vector<siggen::DmrVoiceBits> voice(std::size_t bursts, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    std::vector<siggen::DmrVoiceBits> out(bursts);
    for (auto& frame : out) {
        for (std::uint8_t& bit : frame) {
            bit = static_cast<std::uint8_t>(random() & 1U);
        }
    }
    return out;
}

std::vector<siggen::DmrSlot> transmission() {
    // The base station channel.
    siggen::DmrVoiceCall call;
    call.colour_code = kColourCode;
    call.lc = group_lc(3121, 0x30'3930);
    call.voice = voice(2 * decode::kDmrSuperframeBursts, 0xB10C'0D32'0000'0003ULL);
    auto one = siggen::dmr_voice_call_bursts(call);
    REQUIRE(one.has_value());

    std::vector<siggen::DmrBurstBits> two;
    const std::array<std::uint8_t, 10> preamble = {0xBD, 0x00, 0x00, 0x02, 0x00,
                                                   0x0C, 0x31, 0x30, 0x39, 0x30};
    const std::array<std::uint8_t, 10> header = {0x82, 0x40, 0x00, 0x0C, 0x31,
                                                 0x30, 0x39, 0x30, 0x81, 0x00};
    const std::array<std::uint8_t, 10> pi = {0x21, 0x00, 0x00, 0x00, 0x00,
                                             0x00, 0x00, 0x00, 0x00, 0x00};
    for (int i = 0; i < 2; ++i) {
        auto csbk = siggen::dmr_bptc_burst(DmrSyncType::BsData, kColourCode, DmrDataType::Csbk, preamble);
        REQUIRE(csbk.has_value());
        two.push_back(*csbk);
    }
    for (const auto& [type, octets] : {std::pair{DmrDataType::DataHeader, std::span<const std::uint8_t>(header)},
                                       std::pair{DmrDataType::PiHeader, std::span<const std::uint8_t>(pi)}}) {
        auto burst = siggen::dmr_bptc_burst(DmrSyncType::BsData, kColourCode, type, octets);
        REQUIRE(burst.has_value());
        two.push_back(*burst);
    }

    const siggen::DmrShortLcFragments short_lc =
        siggen::dmr_short_lc_fragments(decode::kDmrSlcoActivityUpdate, 0x8B'0042U);
    std::vector<siggen::DmrSlot> slots;
    const std::size_t frames = one->size() + 4;
    for (std::size_t i = 0; i < 2 * frames; ++i) {
        const std::size_t frame = i / 2;
        const bool first = i % 2 == 0;
        const auto& bursts = first ? *one : two;
        siggen::DmrSlot slot;
        const std::size_t fragment = i % decode::kDmrShortLcFragments;
        slot.cach = siggen::dmr_cach(true, first ? 1 : 2, short_lc.lcss[fragment], short_lc.payload[fragment]);
        slot.burst = (frame >= 2 && frame - 2 < bursts.size())
                         ? bursts[frame - 2]
                         : siggen::dmr_idle_burst(DmrSyncType::BsData, kColourCode);
        slots.push_back(slot);
    }

    // A gap, then a direct mode call on timeslot 2 with nothing between its
    // bursts.
    slots.resize(slots.size() + 6);
    siggen::DmrVoiceCall direct;
    direct.colour_code = kColourCode;
    direct.voice_sync = DmrSyncType::DirectVoiceSlot2;
    direct.lc = group_lc(91, 0x12'3456);
    direct.voice = voice(2 * decode::kDmrSuperframeBursts, 0xB10C'0D32'0000'0004ULL);
    auto bursts = siggen::dmr_voice_call_bursts(direct);
    REQUIRE(bursts.has_value());
    for (const auto& burst : *bursts) {
        slots.emplace_back();
        siggen::DmrSlot on;
        on.burst = burst;
        slots.push_back(on);
    }
    slots.resize(slots.size() + 4);
    return slots;
}

struct Condition {
    dsp::Hertz offset_hz = kCarrierOffsetHz;
    dsp::Complex32 dc = kResidualDc;
};

std::vector<dsp::Complex32> capture(const Condition& condition = {}) {
    auto signal = siggen::dmr_render_slots(siggen::DmrModConfig{}, transmission());
    REQUIRE(signal.has_value());

    siggen::FrequencyConfig offset;
    offset.doppler_shift_hz = condition.offset_hz;
    REQUIRE(siggen::apply_frequency_offset(*signal, offset, kRate, 0).has_value());

    const auto quiet = static_cast<std::size_t>(kQuietSeconds * static_cast<double>(kRate));
    std::vector<dsp::Complex32> all(quiet, dsp::Complex32{});
    all.insert(all.end(), signal->begin(), signal->end());
    all.insert(all.end(), quiet, dsp::Complex32{});
    for (dsp::Complex32& sample : all) {
        sample += condition.dc;
    }
    const double noise_power = std::pow(10.0, -kSnrFullBandDb / 10.0);
    REQUIRE(siggen::add_awgn_at_power(all, noise_power, kNoiseSeed).has_value());
    return all;
}

// Everything a caller can read off a burst, as one comparable line.
std::string digest(const decode::DmrBurst& burst) {
    std::string out = std::format(
        "at {} slot {} sync {} score {:.17g} voice {} cc {} dt {} st+{} failed {} pi {} fit {:.17g} {:.17g}",
        burst.first_sample, burst.slot, burst.sync ? decode::dmr_sync_name(*burst.sync) : "-",
        burst.sync_score, burst.voice_burst, burst.colour_code ? static_cast<int>(*burst.colour_code) : -1,
        burst.data_type ? static_cast<int>(*burst.data_type) : -1, burst.slot_type_corrected,
        burst.payload_failed, burst.pi_header, burst.carrier_offset_hz, burst.deviation_ratio);
    if (burst.cach) {
        out += std::format(" | cach at {} tc {} lcss {} +{}", burst.cach->access_busy, burst.cach->channel,
                           burst.cach->lcss, burst.cach->tact_corrected);
    }
    if (burst.emb) {
        out += std::format(" | emb cc {} pi {} lcss {} +{}", burst.emb->colour_code, burst.emb->pi,
                           burst.emb->lcss, burst.emb->corrected);
    }
    if (burst.full_lc) {
        out += std::format(" | lc {} flco {} dst {} src {} +{}", static_cast<int>(burst.full_lc->carrier),
                           burst.full_lc->flco, burst.full_lc->destination.value_or(0),
                           burst.full_lc->source.value_or(0), burst.full_lc->corrected);
    }
    if (burst.csbk) {
        out += std::format(" | csbk {} tgt {} src {}", burst.csbk->opcode, burst.csbk->target.value_or(0),
                           burst.csbk->source.value_or(0));
    }
    if (burst.data_header) {
        out += std::format(" | header dpf {} sap {} dst {} src {}", burst.data_header->format,
                           burst.data_header->sap, burst.data_header->destination.value_or(0),
                           burst.data_header->source.value_or(0));
    }
    if (burst.short_lc) {
        out += std::format(" | slc {} {:06x}", burst.short_lc->slco, burst.short_lc->data);
    }
    std::uint64_t hash = 0xCBF2'9CE4'8422'2325ULL;  // FNV-1a over the payload
    for (const std::uint8_t bit : burst.payload) {
        hash = (hash ^ bit) * 0x0000'0100'0000'01B3ULL;
    }
    out += std::format(" | {} bits {:016x}", burst.payload.size(), hash);
    return out;
}

struct Result {
    std::vector<std::string> bursts;
    std::size_t full_lcs = 0;
    std::size_t csbks = 0;
    std::size_t data_headers = 0;
    std::size_t pi_headers = 0;
    std::size_t short_lcs = 0;
    std::size_t voice = 0;
    std::vector<double> carrier_offsets_hz;
};

template <typename NextLength>
Result decode_blocked(std::span<const dsp::Complex32> samples, NextLength next) {
    decode::DmrConfig config;
    config.rate = kRate;
    auto decoder = decode::Dmr::create(config);
    REQUIRE(decoder.has_value());

    std::vector<decode::DmrBurst> bursts;
    std::size_t at = 0;
    while (at < samples.size()) {
        const std::size_t length = std::min<std::size_t>(next(), samples.size() - at);
        REQUIRE(decoder->process(samples.subspan(at, length), bursts).has_value());
        at += length;
    }

    Result result;
    for (const decode::DmrBurst& burst : bursts) {
        result.bursts.push_back(digest(burst));
        result.full_lcs += burst.full_lc ? 1U : 0U;
        result.csbks += burst.csbk ? 1U : 0U;
        result.data_headers += burst.data_header ? 1U : 0U;
        result.pi_headers += burst.pi_header ? 1U : 0U;
        result.short_lcs += burst.short_lc ? 1U : 0U;
        result.voice += burst.voice_burst != 0 ? 1U : 0U;
        if (burst.sync) {
            result.carrier_offsets_hz.push_back(burst.carrier_offset_hz);
        }
    }
    return result;
}

void report(std::string_view blocking, const Result& result) {
    std::println("test_dmr_blocking {}: {} bursts, {} voice, {} Full LC, {} CSBK, {} data headers, "
                 "{} PI headers, {} Short LC",
                 blocking, result.bursts.size(), result.voice, result.full_lcs, result.csbks,
                 result.data_headers, result.pi_headers, result.short_lcs);
}

}  // namespace

TEST_CASE("DMR decodes the same capture identically however it is blocked", "[decode][dmr]") {
    const std::vector<dsp::Complex32> samples = capture();
    const std::span<const dsp::Complex32> all(samples);

    const Result whole = decode_blocked(all, [&] { return samples.size(); });
    report("whole", whole);

    // The capture decodes at all, or identical output would be the trivial
    // kind: a header, two embedded LCs and a terminator on each call, and
    // every voice burst of both.
    CHECK(whole.full_lcs == 8);
    CHECK(whole.csbks == 2);
    CHECK(whole.data_headers == 1);
    CHECK(whole.pi_headers == 1);
    CHECK(whole.voice == 24);
    CHECK(whole.short_lcs > 5);

    // 2731 and 10923 are what a 16384 and a 65536-sample engine block at
    // 288000 S/s come to at 48000.
    struct Blocking {
        std::string name;
        std::size_t fixed;  // zero for random lengths
    };
    const Blocking blockings[] = {
        {"1024", 1'024},   {"2731", 2'731},   {"4096", 4'096},   {"10923", 10'923},
        {"16384", 16'384}, {"65536", 65'536}, {"random", 0},     {"one sample", 1},
    };

    std::println("test_dmr_blocking random lengths from seed {:#x}", kBlockingSeed);
    for (const Blocking& blocking : blockings) {
        std::mt19937_64 random(kBlockingSeed);
        std::uniform_int_distribution<std::size_t> length(1, 20'000);
        const Result blocked = decode_blocked(all, [&] {
            return blocking.fixed != 0 ? blocking.fixed : length(random);
        });
        report(blocking.name, blocked);

        INFO("blocking " << blocking.name);
        REQUIRE(blocked.bursts.size() == whole.bursts.size());
        for (std::size_t i = 0; i < whole.bursts.size(); ++i) {
            INFO("burst " << i << "\n  whole:   " << whole.bursts[i] << "\n  blocked: " << blocked.bursts[i]);
            CHECK(blocked.bursts[i] == whole.bursts[i]);
        }
    }
}

TEST_CASE("DMR frames a carrier 1200 Hz off DC from each burst's own sync", "[decode][dmr]") {
    // test_p25p1_blocking.cpp's second case: two symbol units of offset at
    // P25's 600 Hz, 1.85 at DMR's 648, where a transmitter 1.5 parts per
    // million off puts an 800 MHz carrier. No baseband DC.
    constexpr dsp::Hertz kOffsetHz = 1'200;
    const std::vector<dsp::Complex32> samples = capture(Condition{kOffsetHz, dsp::Complex32{}});
    const Result whole = decode_blocked(std::span<const dsp::Complex32>(samples),
                                        [&] { return samples.size(); });
    report("1200 Hz off, whole", whole);
    CHECK(whole.full_lcs == 8);
    CHECK(whole.csbks == 2);
    CHECK(whole.voice == 24);

    REQUIRE_FALSE(whole.carrier_offsets_hz.empty());
    const auto [low, high] = std::ranges::minmax(whole.carrier_offsets_hz);
    std::println("test_dmr_blocking 1200 Hz off: syncs measured {:.1f} to {:.1f} Hz", low, high);
    CHECK(low > static_cast<double>(kOffsetHz) - 60.0);
    CHECK(high < static_cast<double>(kOffsetHz) + 60.0);
}
