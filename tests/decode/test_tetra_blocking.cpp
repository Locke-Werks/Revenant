// TETRA V+D: what comes out must not depend on how the input was blocked.
//
// The property tests/decode/test_p25p1_blocking.cpp holds P25 to. One
// capture of synchronisation bursts the way a real channel arrives, a carrier
// a few hundred hertz off, baseband DC and noise, decoded whole, in fixed
// blocks from 1024 to 65536 samples, in random lengths and one sample a call,
// and every blocking must give the same bursts, field for field and bit for
// bit, as the whole-capture call.
//
// WHAT IT WAS
//
// Tetra::process restarted its matched filter from zeros at every call, and
// numbered a burst from the start of a buffer it trims. Measured on this
// file's capture, 36 synchronisation bursts, 2026-09-23, before the fix:
// bursts found of 36, and of those, how many had a BSCH that verified.
//
//     blocking      found   verified
//     whole         36      36
//     1024          12      4
//     2731          33      32
//     4096          30      28
//     16384         34      34
//     65536         36      36
//     random        36      35
//     one sample    32      31
//
// The filter is 65 taps, 16 symbols at 72000 S/s, so a block boundary
// anywhere near the training sequence or the BSCH cost the burst. Now every
// blocking finds and verifies all 36, identically.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <print>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/decode/tetra.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dv_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 72'000;

constexpr dsp::Hertz kCarrierOffsetHz = 350;
constexpr dsp::Complex32 kResidualDc{0.05F, -0.03F};
constexpr double kSnrFullBandDb = 20.0;
constexpr std::uint64_t kNoiseSeed = 0xB10C'7E7A'0000'0001ULL;
constexpr std::uint64_t kBlockingSeed = 0xB10C'7E7A'0000'0002ULL;

constexpr double kQuietSeconds = 0.25;
constexpr std::size_t kBursts = 36;

decode::TetraSyncPdu pdu(std::size_t burst) {
    decode::TetraSyncPdu out;
    out.system_code = 0b0011;
    out.colour_code = 37;
    out.timeslot = static_cast<std::uint8_t>(burst % 4);
    out.frame_number = static_cast<std::uint8_t>(1 + (burst / 4) % 18);
    out.multiframe_number = 42;
    out.uplane_dtx_allowed = true;
    out.mobile_country_code = 234;
    out.mobile_network_code = 1'234;
    out.neighbour_cell_broadcast = 1;
    out.cell_load = 2;
    out.late_entry_supported = true;
    return out;
}

std::vector<std::uint8_t> sent_bits() {
    std::vector<std::uint8_t> stream;
    for (std::size_t burst = 0; burst < kBursts; ++burst) {
        auto bits = siggen::tetra_sync_burst_bits(pdu(burst), 0xB10C'7E7A'0000'0100ULL + burst);
        REQUIRE(bits.has_value());
        stream.insert(stream.end(), bits->begin(), bits->end());
    }
    return stream;
}

struct Condition {
    dsp::Hertz offset_hz = kCarrierOffsetHz;
    dsp::Complex32 dc = kResidualDc;
    bool noise = true;
};

std::vector<dsp::Complex32> capture(const Condition& condition = {}) {
    siggen::TetraModConfig mod;
    mod.rate = kRate;
    auto signal = siggen::tetra_render_bits(mod, sent_bits());
    REQUIRE(signal.has_value());

    const auto quiet = static_cast<std::size_t>(kQuietSeconds * static_cast<double>(kRate));
    std::vector<dsp::Complex32> all(quiet, dsp::Complex32{});
    all.insert(all.end(), signal->begin(), signal->end());
    all.insert(all.end(), quiet, dsp::Complex32{});

    siggen::FrequencyConfig offset;
    offset.doppler_shift_hz = condition.offset_hz;
    REQUIRE(siggen::apply_frequency_offset(all, offset, kRate, 0).has_value());
    for (dsp::Complex32& sample : all) {
        sample += condition.dc;
    }
    if (condition.noise) {
        const double noise_power = std::pow(10.0, -kSnrFullBandDb / 10.0);
        REQUIRE(siggen::add_awgn_at_power(all, noise_power, kNoiseSeed).has_value());
    }
    return all;
}

std::string digest(const decode::TetraBurst& burst) {
    std::string out = std::format("at {} score {:.17g} verified {}", burst.first_symbol,
                                  burst.sync_score, burst.block_code_verified);
    if (burst.sync) {
        const decode::TetraSyncPdu& p = *burst.sync;
        out += std::format(" | mcc {} mnc {} cc {} tn {} fn {} mn {}", p.mobile_country_code,
                           p.mobile_network_code, p.colour_code, p.timeslot, p.frame_number,
                           p.multiframe_number);
    }
    std::uint64_t hash = 0xCBF2'9CE4'8422'2325ULL;
    for (const std::uint8_t bit : burst.bits) {
        hash = (hash ^ bit) * 0x0000'0100'0000'01B3ULL;
    }
    out += std::format(" | bits {:016x}", hash);
    return out;
}

struct Result {
    std::vector<std::string> bursts;
    std::size_t verified = 0;
    std::size_t pdus_correct = 0;
    std::size_t bits_compared = 0;
    std::size_t bit_errors = 0;
};

template <typename NextLength>
Result decode_blocked(std::span<const dsp::Complex32> samples, NextLength next) {
    decode::TetraConfig config;
    config.rate = kRate;
    auto decoder = decode::Tetra::create(config);
    REQUIRE(decoder.has_value());

    std::vector<decode::TetraBurst> bursts;
    std::size_t at = 0;
    while (at < samples.size()) {
        const std::size_t length = std::min<std::size_t>(next(), samples.size() - at);
        REQUIRE(decoder->process(samples.subspan(at, length), bursts).has_value());
        at += length;
    }

    // Bursts are matched to what was sent by their order among those whose
    // PDU decoded: each carries its own timeslot and frame number.
    const std::vector<std::uint8_t> sent = sent_bits();
    Result result;
    for (const decode::TetraBurst& burst : bursts) {
        result.bursts.push_back(digest(burst));
        result.verified += burst.block_code_verified ? 1U : 0U;
        if (!burst.sync) {
            continue;
        }
        const std::size_t index =
            static_cast<std::size_t>(burst.sync->frame_number - 1) * 4 + burst.sync->timeslot;
        if (index >= kBursts) {
            continue;
        }
        const decode::TetraSyncPdu want = pdu(index);
        const bool correct = burst.sync->mobile_country_code == want.mobile_country_code &&
                             burst.sync->mobile_network_code == want.mobile_network_code &&
                             burst.sync->colour_code == want.colour_code;
        result.pdus_correct += correct ? 1U : 0U;
        for (std::size_t i = 0; i < decode::kTetraBurstBits; ++i) {
            ++result.bits_compared;
            result.bit_errors +=
                (burst.bits[i] != sent[index * decode::kTetraBurstBits + i]) ? 1U : 0U;
        }
    }
    return result;
}

void report(std::string_view blocking, const Result& result) {
    std::println(
        "test_tetra_blocking {}: {} bursts of {}, {} verified, {} PDUs correct, {} bit errors in "
        "{}",
        blocking, result.bursts.size(), kBursts, result.verified, result.pdus_correct,
        result.bit_errors, result.bits_compared);
}

}  // namespace

TEST_CASE("the streaming complex filter matches the one-call one bit for bit",
          "[decode][tetra]") {
    // core/decode/dv_phy.h claims it: ComplexFir gives a stream the output
    // filter_complex gives it in one call, however the stream is split.
    // Float equality, in 777-sample pieces, which divide nothing here.
    const std::vector<dsp::Complex32> samples = capture();
    const std::span<const dsp::Complex32> all(samples);

    auto taps = decode::design_rrc(kRate, decode::kTetraSymbolRate, decode::kTetraRollOff,
                                   decode::TetraConfig{}.filter_taps);
    REQUIRE(taps.has_value());
    std::vector<dsp::Complex32> reference(samples.size());
    REQUIRE(decode::filter_complex(all, *taps, reference).has_value());

    auto filter = decode::ComplexFir::create(*taps);
    REQUIRE(filter.has_value());
    constexpr std::size_t kPiece = 777;
    std::vector<dsp::Complex32> streamed(samples.size());
    for (std::size_t at = 0; at < samples.size(); at += kPiece) {
        const std::size_t count = std::min(kPiece, samples.size() - at);
        REQUIRE(filter->process(all.subspan(at, count), std::span(streamed).subspan(at, count))
                    .has_value());
    }
    CHECK(streamed == reference);
}

TEST_CASE("TETRA decodes the same capture identically however it is blocked",
          "[decode][tetra]") {
    const std::vector<dsp::Complex32> samples = capture();
    const std::span<const dsp::Complex32> all(samples);

    const Result whole = decode_blocked(all, [&] { return samples.size(); });
    report("whole", whole);
    CHECK(whole.pdus_correct == kBursts);

    struct Blocking {
        std::string name;
        std::size_t fixed;
    };
    const Blocking blockings[] = {
        {"1024", 1'024},   {"2731", 2'731},   {"4096", 4'096},   {"16384", 16'384},
        {"65536", 65'536}, {"random", 0},     {"one sample", 1},
    };

    std::println("test_tetra_blocking random lengths from seed {:#x}", kBlockingSeed);
    for (const Blocking& blocking : blockings) {
        std::mt19937_64 random(kBlockingSeed);
        std::uniform_int_distribution<std::size_t> length(1, 20'000);
        const Result blocked = decode_blocked(all, [&] {
            return blocking.fixed != 0 ? blocking.fixed : length(random);
        });
        report(blocking.name, blocked);

        INFO("blocking " << blocking.name);
        CHECK(blocked.bursts.size() == whole.bursts.size());
        for (std::size_t i = 0; i < std::min(whole.bursts.size(), blocked.bursts.size()); ++i) {
            INFO("burst " << i << "\n  whole:   " << whole.bursts[i] << "\n  blocked: "
                          << blocked.bursts[i]);
            CHECK(blocked.bursts[i] == whole.bursts[i]);
        }
    }
}
