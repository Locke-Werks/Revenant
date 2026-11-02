// The generic MFSK demodulator against its transmitter: symbol error rate
// against noise for M from 2 to 64, with a frequency offset and a symbol
// boundary that falls between samples, drift, frequency search, GFSK, the IFK
// map, the raster view, and chunk independence.
//
// Error rates are reported in WARN lines and asserted loosely, for the reason
// tests/decode/CMakeLists.txt gives. The loose bound is the union bound for
// non-coherent orthogonal MFSK, (M-1)/2 exp(-Es/2N0), taken 3 dB down, which
// is textbook (Proakis) and leaves room for real-input timing and AFC loss.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "core/decode/mfsk_demod.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/mfsk_mod.h"
#include "core/dsp/synth/rds_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 8000;
constexpr double kBaud = 15.625;  // 512 samples per symbol at 8 kHz

std::vector<int> random_tones(int m, std::size_t count, std::uint64_t seed) {
    std::mt19937_64 engine(seed);
    std::vector<int> out(count);
    for (auto& t : out) {
        t = static_cast<int>(engine() % static_cast<std::uint64_t>(m));
    }
    return out;
}

siggen::MfskModConfig mod_config(int m) {
    siggen::MfskModConfig mod;
    mod.rate = kRate;
    mod.tones = m;
    mod.tone_spacing_hz = kBaud;
    mod.symbol_rate = kBaud;
    mod.base_hz = 503.0;  // 3 Hz from what the demodulator is told
    mod.lead_in_samples = 137.3;
    mod.tail_samples = 2048.0;
    return mod;
}

decode::MfskDemodConfig demod_config(int m) {
    decode::MfskDemodConfig cfg;
    cfg.rate = kRate;
    cfg.tones = m;
    cfg.tone_spacing_hz = kBaud;
    cfg.symbol_rate = kBaud;
    cfg.base_hz = 500.0;
    return cfg;
}

std::vector<decode::MfskSymbol> demodulate(const decode::MfskDemodConfig& cfg,
                                           const std::vector<float>& audio) {
    auto demod = decode::MfskDemodulator::create(cfg);
    REQUIRE(demod.has_value());
    std::vector<decode::MfskSymbol> out;
    demod->process(audio, out);
    return out;
}

// Each received symbol is matched to the transmitted one whose period its
// window starts nearest; a transmitted symbol with no match counts as an error.
double symbol_error_rate(const std::vector<decode::MfskSymbol>& rx, const std::vector<int>& tx,
                         double lead, double period) {
    std::vector<int> got(tx.size(), -1);
    for (const auto& s : rx) {
        const auto i = std::llround((static_cast<double>(s.start) - lead) / period);
        if (i >= 0 && i < static_cast<long long>(tx.size())) {
            got[static_cast<std::size_t>(i)] = s.tone;
        }
    }
    std::size_t errors = 0;
    for (std::size_t i = 0; i < tx.size(); ++i) {
        errors += got[i] == tx[i] ? 0U : 1U;
    }
    return static_cast<double>(errors) / static_cast<double>(tx.size());
}

void add_noise(std::vector<float>& audio, double amplitude, double snr_2500_db, std::uint64_t seed) {
    REQUIRE(siggen::add_real_awgn(audio, amplitude * amplitude / 2.0,
                                  siggen::NoiseLevel::snr_in_2500_hz_db(snr_2500_db), kRate, seed)
                .has_value());
}

// Es/N0 for a real signal of this power against one-sided noise density N0,
// given the SNR in 2500 Hz: Es/N0 = SNR * 2500 / symbol rate.
double snr_2500_for_es_n0(double es_n0_db) {
    return es_n0_db - 10.0 * std::log10(2500.0 / kBaud);
}

double union_bound(int m, double es_n0_db) {
    const double es_n0 = std::pow(10.0, es_n0_db / 10.0);
    return std::min(1.0, (m - 1) / 2.0 * std::exp(-es_n0 / 2.0));
}

}  // namespace

TEST_CASE("MFSK symbol error rate against noise, measured", "[decode][mfsk]") {
    for (const int m : {2, 4, 8, 16, 32, 64}) {
        const auto tx = random_tones(m, 300, 0x3F5CULL + static_cast<std::uint64_t>(m));
        const auto mod = mod_config(m);
        const auto clean = siggen::mfsk_render(mod, tx);
        REQUIRE(clean.has_value());
        double previous = 1.0;
        for (const double es_n0 : {6.0, 10.0, 14.0, 18.0}) {
            auto audio = *clean;
            add_noise(audio, mod.amplitude, snr_2500_for_es_n0(es_n0), 0xA11CEULL + static_cast<std::uint64_t>(es_n0));
            const auto rx = demodulate(demod_config(m), audio);
            const double ser = symbol_error_rate(rx, tx, mod.lead_in_samples, kRate / kBaud);
            WARN("MFSK M=" << m << " Es/N0 " << es_n0 << " dB (" << snr_2500_for_es_n0(es_n0)
                           << " dB in 2500 Hz): SER " << ser << ", union bound "
                           << union_bound(m, es_n0));
            CHECK(ser <= union_bound(m, es_n0 - 3.0) + 0.02);
            CHECK(ser <= previous + 0.02);
            previous = ser;
        }
    }
}

TEST_CASE("MFSK clean decode with offset and fractional timing for every M", "[decode][mfsk]") {
    for (const int m : {2, 4, 8, 16, 32, 64}) {
        const auto tx = random_tones(m, 120, 77U + static_cast<std::uint64_t>(m));
        const auto mod = mod_config(m);
        const auto audio = siggen::mfsk_render(mod, tx);
        REQUIRE(audio.has_value());
        const auto rx = demodulate(demod_config(m), *audio);
        CHECK(symbol_error_rate(rx, tx, mod.lead_in_samples, kRate / kBaud) == 0.0);
        REQUIRE(!rx.empty());
        CHECK(std::abs(rx.back().tone0_hz - 503.0) < 0.5);
    }
}

TEST_CASE("MFSK follows drift", "[decode][mfsk]") {
    const int m = 16;
    const auto tx = random_tones(m, 400, 0xD21F7ULL);
    auto mod = mod_config(m);
    mod.drift_hz_per_s = 0.5;  // 12.8 Hz over the transmission, most of a spacing
    auto audio = siggen::mfsk_render(mod, tx);
    REQUIRE(audio.has_value());
    add_noise(*audio, mod.amplitude, snr_2500_for_es_n0(16.0), 5U);
    const auto rx = demodulate(demod_config(m), *audio);
    const double ser = symbol_error_rate(rx, tx, mod.lead_in_samples, kRate / kBaud);
    const double expected_end = 503.0 + 0.5 * 400.0 / kBaud;
    WARN("MFSK drift 0.5 Hz/s, M=16, Es/N0 16 dB: SER " << ser << ", final tone 0 at "
                                                         << rx.back().tone0_hz << " Hz against "
                                                         << expected_end);
    CHECK(ser <= 0.01);
    CHECK(std::abs(rx.back().tone0_hz - expected_end) < 1.5);
}

TEST_CASE("MFSK frequency search finds the signal", "[decode][mfsk]") {
    for (const int m : {2, 8, 16}) {
        const auto tx = random_tones(m, 150, 0x5EA4ULL + static_cast<std::uint64_t>(m));
        auto mod = mod_config(m);
        mod.base_hz = 1234.5;
        auto audio = siggen::mfsk_render(mod, tx);
        REQUIRE(audio.has_value());
        add_noise(*audio, mod.amplitude, snr_2500_for_es_n0(18.0), 9U);
        auto cfg = demod_config(m);
        cfg.search = true;
        cfg.base_hz = 0.0;
        const auto rx = demodulate(cfg, *audio);
        REQUIRE(!rx.empty());
        const double ser = symbol_error_rate(rx, tx, mod.lead_in_samples, kRate / kBaud);
        WARN("MFSK search M=" << m << ": tone 0 found at " << rx.front().tone0_hz
                              << " Hz against 1234.5, SER " << ser);
        CHECK(std::abs(rx.front().tone0_hz - 1234.5) < 2.0);
        CHECK(ser <= 0.01);
    }
}

TEST_CASE("MFSK with Gaussian shaping", "[decode][mfsk]") {
    const int m = 8;
    const auto tx = random_tones(m, 150, 0x6F5ULL);
    auto mod = mod_config(m);
    mod.gaussian_bt = 1.0;
    auto audio = siggen::mfsk_render(mod, tx);
    REQUIRE(audio.has_value());
    add_noise(*audio, mod.amplitude, snr_2500_for_es_n0(16.0), 11U);
    const auto rx = demodulate(demod_config(m), *audio);
    const double ser = symbol_error_rate(rx, tx, mod.lead_in_samples, kRate / kBaud);
    WARN("MFSK GFSK BT 1.0, M=8, Es/N0 16 dB: SER " << ser);
    CHECK(ser <= 0.01);
}

TEST_CASE("IFK maps round trip", "[decode][mfsk]") {
    for (const int m : {2, 4, 8, 16, 32, 64, 256}) {
        for (const int offset : {0, 1, 2}) {
            const auto symbols = random_tones(m, 500, static_cast<std::uint64_t>(m * 7 + offset));
            const auto tones = decode::ifk_encode(symbols, m, offset, 3 % m);
            CHECK(decode::ifk_decode(tones, m, offset, 3 % m) == symbols);
            if (offset > 0 && offset < m) {
                for (std::size_t i = 1; i < tones.size(); ++i) {
                    if (symbols[i] == 0) {
                        CHECK(tones[i] != tones[i - 1]);
                    }
                }
            }
        }
    }
}

TEST_CASE("IFK over the air, hard and soft, through a frequency offset", "[decode][mfsk]") {
    const int m = 16;
    const int offset = 2;
    const auto symbols = random_tones(m, 200, 0x1F4ULL);
    const auto tones = decode::ifk_encode(symbols, m, offset);
    auto mod = mod_config(m);
    auto audio = siggen::mfsk_render(mod, tones);
    REQUIRE(audio.has_value());
    add_noise(*audio, mod.amplitude, snr_2500_for_es_n0(18.0), 21U);
    const auto rx = demodulate(demod_config(m), *audio);
    REQUIRE(symbol_error_rate(rx, tones, mod.lead_in_samples, kRate / kBaud) <= 0.01);

    // The first received symbol is transmitted symbol 0, whose predecessor is
    // the encoder's initial tone.
    std::vector<int> rx_tones;
    for (const auto& s : rx) {
        rx_tones.push_back(s.tone);
    }
    rx_tones.resize(std::min(rx_tones.size(), tones.size()));
    const auto decoded = decode::ifk_decode(rx_tones, m, offset);
    std::size_t errors = 0;
    std::size_t soft_errors = 0;
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        errors += decoded[i] == symbols[i] ? 0U : 1U;
        const int prev = i == 0 ? 0 : rx_tones[i - 1];
        const auto soft = decode::ifk_soft(rx[i].energies, prev, offset);
        const auto best = static_cast<int>(
            std::distance(soft.begin(), std::max_element(soft.begin(), soft.end())));
        soft_errors += best == decoded[i] ? 0U : 1U;
    }
    CHECK(errors <= 4);
    CHECK(soft_errors == 0);
}

TEST_CASE("MFSK output does not depend on chunking", "[decode][mfsk]") {
    for (const int m : {4, 32}) {
        const auto tx = random_tones(m, 120, 0xC4ULL + static_cast<std::uint64_t>(m));
        auto mod = mod_config(m);
        mod.drift_hz_per_s = 0.3;
        auto audio = siggen::mfsk_render(mod, tx);
        REQUIRE(audio.has_value());
        add_noise(*audio, mod.amplitude, snr_2500_for_es_n0(12.0), 3U);

        auto cfg = demod_config(m);
        const auto whole = demodulate(cfg, *audio);

        auto demod = decode::MfskDemodulator::create(cfg);
        REQUIRE(demod.has_value());
        std::vector<decode::MfskSymbol> pieces;
        std::mt19937_64 engine(42);
        std::size_t at = 0;
        while (at < audio->size()) {
            const std::size_t n = std::min<std::size_t>(1 + engine() % 1500, audio->size() - at);
            demod->process(dsp::ConstRealSpan(audio->data() + at, n), pieces);
            at += n;
        }
        REQUIRE(pieces.size() == whole.size());
        for (std::size_t i = 0; i < whole.size(); ++i) {
            CHECK(pieces[i].start == whole[i].start);
            CHECK(pieces[i].tone == whole[i].tone);
            CHECK(pieces[i].tone0_hz == whole[i].tone0_hz);
            CHECK(pieces[i].energies == whole[i].energies);
        }
    }
}

TEST_CASE("MFSK raster shows the tone in the right bin, chunk independent", "[decode][mfsk]") {
    const int m = 8;
    const auto tx = random_tones(m, 40, 0x7A57ULL);
    auto mod = mod_config(m);
    mod.base_hz = 500.0;
    mod.lead_in_samples = 0.0;
    const auto audio = siggen::mfsk_render(mod, tx);
    REQUIRE(audio.has_value());

    decode::MfskRasterConfig cfg;
    cfg.rate = kRate;
    cfg.rows_per_second = kBaud;
    cfg.low_hz = 500.0 - 4 * kBaud;
    cfg.bin_spacing_hz = kBaud;
    cfg.bins = 16;
    auto raster = decode::MfskRaster::create(cfg);
    REQUIRE(raster.has_value());
    std::vector<std::vector<float>> rows;
    raster->process(*audio, rows);
    REQUIRE(rows.size() >= tx.size());
    for (std::size_t i = 0; i < tx.size(); ++i) {
        const auto best = std::distance(rows[i].begin(), std::max_element(rows[i].begin(), rows[i].end()));
        CHECK(best == tx[i] + 4);
    }

    auto again = decode::MfskRaster::create(cfg);
    REQUIRE(again.has_value());
    std::vector<std::vector<float>> chunked;
    for (std::size_t at = 0; at < audio->size(); at += 333) {
        const std::size_t n = std::min<std::size_t>(333, audio->size() - at);
        again->process(dsp::ConstRealSpan(audio->data() + at, n), chunked);
    }
    CHECK(chunked == rows);
}

TEST_CASE("MFSK configuration is validated", "[decode][mfsk]") {
    auto cfg = demod_config(16);
    cfg.tones = 1;
    CHECK_FALSE(decode::MfskDemodulator::create(cfg).has_value());
    cfg.tones = 257;
    CHECK_FALSE(decode::MfskDemodulator::create(cfg).has_value());
    cfg.tones = 256;
    cfg.tone_spacing_hz = 10.0;
    cfg.base_hz = 300.0;
    CHECK(decode::MfskDemodulator::create(cfg).has_value());
    cfg.base_hz = 3000.0;
    CHECK_FALSE(decode::MfskDemodulator::create(cfg).has_value());
}
