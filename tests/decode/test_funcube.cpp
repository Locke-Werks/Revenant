// FUNcube telemetry, core/decode/funcube.h, against the AO-40 coding
// proposal and the FUNcube-1 downlink document, and through the transmitter
// in core/dsp/synth/funcube_mod.h over BPSK with noise and Doppler.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "core/decode/funcube.h"
#include "core/decode/psk_demod.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/funcube_mod.h"
#include "core/dsp/synth/psk_mod.h"

using namespace revenant;
using namespace revenant::decode;

namespace {

FuncubeRealTime sample_real_time(unsigned seed) {
    FuncubeRealTime t;
    t.photo_voltage = {static_cast<std::uint16_t>(1000 + seed), 2000, 3000};
    t.battery_voltage = 8123;
    t.reboot_count = 77;
    t.boost_converter_temp = {10, 20, 30};
    t.bob = {1, 2, 3, 4, 5, 6, 7, 8, 9, 1023};
    t.rf = {11, 12, 13, 14, 15, 16};
    t.pa = {21, 22, 23, 24};
    t.antenna_deployed = {true, false, true, true};
    t.sequence_number = 0xABCDE0U + seed;
    t.dtmf_command_count = 63;
    t.dtmf_last_command = 17;
    t.dtmf_command_success = true;
    t.data_valid = {true, true, false, true, true, true, false};
    t.in_eclipse = true;
    t.deployment_wait = true;
    return t;
}

std::vector<std::vector<std::uint8_t>> sample_frames(std::size_t n) {
    std::vector<std::vector<std::uint8_t>> frames;
    for (std::size_t i = 0; i < n; ++i) {
        std::string text = "FITTER MESSAGE " + std::to_string(i);
        std::vector<std::uint8_t> payload(text.begin(), text.end());
        payload.resize(200, ' ');
        frames.push_back(siggen::funcube_build_frame(2, static_cast<std::uint8_t>(14 + i % 3),
                                                     sample_real_time(static_cast<unsigned>(i)), payload));
    }
    return frames;
}

}  // namespace

TEST_CASE("FUNcube sync vector is the 65 bits AO40 prints", "[decode][funcube]") {
    const std::string printed = "11111110000111011110010110010010000001000100110001011101011011000";
    std::string got;
    for (const auto b : funcube_sync_vector()) {
        got.push_back(b != 0U ? '1' : '0');
    }
    CHECK(got == printed);
    // AO40: (2048 + 512 + 6) * 2 = 5132 coded symbols, 3 spare of 5200.
    CHECK(kFuncubeCodedSymbols + kFuncubeColumns + 3 == kFuncubeBlockSymbols);
}

TEST_CASE("FUNcube frame fields round trip through the FC section 4 layout", "[decode][funcube]") {
    const auto frames = sample_frames(1);
    const auto f = funcube_parse(std::span<const std::uint8_t, 256>(frames[0].data(), 256));
    CHECK(f.satellite_id == 2);
    CHECK(f.frame_type == 14);
    CHECK(f.content == FuncubeContent::FitterMessage);
    const auto want = sample_real_time(0);
    CHECK(f.real_time.photo_voltage == want.photo_voltage);
    CHECK(f.real_time.battery_voltage == 8123);
    CHECK(f.real_time.reboot_count == 77);
    CHECK(f.real_time.bob == want.bob);
    CHECK(f.real_time.rf == want.rf);
    CHECK(f.real_time.pa == want.pa);
    CHECK(f.real_time.antenna_deployed == want.antenna_deployed);
    CHECK(f.real_time.sequence_number == want.sequence_number);
    CHECK(f.real_time.dtmf_last_command == 17);
    CHECK(f.real_time.data_valid == want.data_valid);
    CHECK(f.real_time.in_eclipse);
    CHECK_FALSE(f.real_time.in_safe_mode);
    CHECK(f.real_time.deployment_wait);
    CHECK(std::string(f.payload.begin(), f.payload.begin() + 16) == "FITTER MESSAGE 0");
    CHECK(funcube_content(1) == FuncubeContent::WholeOrbit);
    CHECK(funcube_content(13) == FuncubeContent::HighResolution);
    CHECK(funcube_content(21) == FuncubeContent::HighResolution);
    CHECK(funcube_content(24) == FuncubeContent::FitterMessage);
    CHECK(funcube_content(0) == FuncubeContent::Unknown);
}

TEST_CASE("FUNcube decoder recovers blocks with symbol errors, in any blocking", "[decode][funcube]") {
    const auto frames = sample_frames(4);
    for (const bool differential : {true, false}) {
        const auto symbols = siggen::funcube_symbols(frames, differential);
        std::mt19937_64 rng(5);
        std::vector<float> soft(300, 0.3F);  // idle before the first block
        std::size_t flipped = 0;
        for (const auto s : symbols) {
            float v = s != 0U ? -1.0F : 1.0F;
            if (rng() % 100 < 2) {  // 2% hard symbol errors
                v = -v;
                ++flipped;
            }
            soft.push_back(differential ? v : -v);  // coherent case sent inverted
        }
        for (const std::size_t chunk : {soft.size(), std::size_t{997}, std::size_t{1}}) {
            auto dec = FuncubeDecoder::create({differential, 0.55}).value();
            std::vector<FuncubeFrame> out;
            for (std::size_t i = 0; i < soft.size(); i += chunk) {
                dec.push(std::span(soft).subspan(i, std::min(chunk, soft.size() - i)), out);
            }
            INFO("differential " << differential << " chunk " << chunk);
            REQUIRE(out.size() == frames.size());
            for (std::size_t i = 0; i < out.size(); ++i) {
                CHECK(std::equal(frames[i].begin(), frames[i].end(), out[i].octets.begin()));
                CHECK(out[i].symbol_position == 300 + i * kFuncubeBlockSymbols);
            }
        }
        std::printf("FUNcube %s: %zu of %zu symbols flipped, all frames decoded\n",
                    differential ? "DBPSK" : "BPSK", flipped, symbols.size());
    }
}

TEST_CASE("FUNcube over 1200 Bd BPSK with noise, Doppler and drift", "[decode][funcube]") {
    constexpr dsp::SampleRate kRate = 48000;
    const auto frames = sample_frames(4);
    auto symbols = siggen::funcube_symbols(frames, true);
    std::vector<std::uint8_t> lead(600);
    for (std::size_t i = 0; i < lead.size(); ++i) {
        lead[i] = static_cast<std::uint8_t>((i * 2654435761U >> 9U) & 1U);
    }
    symbols.insert(symbols.begin(), lead.begin(), lead.end());
    // Information rate: 2048 bits per 5200 symbols.
    const double info_rate = kFuncubeSymbolRate * 2048.0 / 5200.0;

    for (const double eb_n0_db : {5.0, 7.0, 10.0}) {
        siggen::PskModConfig mod;
        mod.rate = kRate;
        mod.symbol_rate = kFuncubeSymbolRate;
        mod.modulation = PskModulation::Bpsk;
        mod.initial_phase = 2.0;
        mod.timing_offset_samples = 7.3;
        mod.clock_error = 30e-6;
        auto samples = siggen::psk_render(mod, symbols).value();
        // A VHF pass at 145.935 MHz, tuned to within 150 Hz, drifting 5 Hz/s.
        siggen::FrequencyConfig fc;
        fc.doppler_shift_hz = 150;
        fc.drift_hz_per_second = 5.0;
        REQUIRE(siggen::apply_frequency_offset(samples, fc, kRate, 3).has_value());
        REQUIRE(siggen::add_awgn(samples, siggen::NoiseLevel::eb_over_n0_db(eb_n0_db, info_rate), kRate,
                                 static_cast<std::uint64_t>(eb_n0_db * 7))
                    .has_value());

        PskDemodConfig dc;
        dc.rate = kRate;
        dc.symbol_rate = kFuncubeSymbolRate;
        dc.modulation = PskModulation::Bpsk;
        auto demod = PskDemod::create(dc).value();
        auto dec = FuncubeDecoder::create().value();
        std::vector<FuncubeFrame> out;
        std::vector<float> soft;
        for (std::size_t i = 0; i < samples.size(); i += 2400) {
            soft.clear();
            for (const auto& s : demod.process(std::span(samples).subspan(i, std::min<std::size_t>(2400, samples.size() - i)))) {
                soft.push_back(s.soft[0]);
            }
            dec.push(soft, out);
        }
        std::size_t good = 0;
        std::size_t corrected = 0;
        for (const auto& f : out) {
            good += std::any_of(frames.begin(), frames.end(),
                                [&](const auto& fr) { return std::equal(fr.begin(), fr.end(), f.octets.begin()); })
                        ? 1
                        : 0;
            corrected += f.corrected[0] + f.corrected[1];
        }
        std::printf("FUNcube DBPSK Eb/N0 %.1f dB (Es/N0 %.1f dB), 150 Hz + 5 Hz/s: %zu/%zu frames, "
                    "%zu RS symbols corrected, %zu RS failures\n",
                    eb_n0_db, eb_n0_db + 10.0 * std::log10(info_rate / kFuncubeSymbolRate), good,
                    frames.size(), corrected, dec.failed());
        CHECK(good == out.size());
        if (eb_n0_db >= 7.0) {
            CHECK(good == frames.size());
        }
    }
}
