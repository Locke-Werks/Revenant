// The generic OFDM receiver in core/decode/ofdm.h against the transmitter in
// core/dsp/synth/ofdm_mod.h.
//
// Two synthetic geometries, neither of them any standard's: a 256-point
// symbol with a quarter prefix and a pilot on every fourth carrier, and a
// 1024-point one at 48 kHz shaped like DRM robustness mode B (21.33 ms
// useful, 5.33 ms guard, 207 carriers) with a pilot grid of this file's own
// invention. The pilot values come from a hash, not from any table.
//
// EVM and SNR figures are measured and printed; the assertions around them
// are loose, as everywhere in this directory.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <vector>

#include "core/decode/ofdm.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/ofdm_mod.h"

using namespace revenant;
using dsp::Complex32;

namespace {

constexpr dsp::SampleRate kRate = 48000;

std::uint64_t mix(std::uint64_t x)
{
    x ^= x >> 33U;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33U;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33U;
    return x;
}

Complex32 pilot_value(int carrier, std::size_t row)
{
    const auto h = mix(static_cast<std::uint64_t>(carrier + 4096) * 131U + row);
    const double phase = 2.0 * std::numbers::pi * static_cast<double>(h & 0xFFFFU) / 65536.0;
    return std::polar(static_cast<float>(std::numbers::sqrt2), static_cast<float>(phase));
}

// carriers -half..half without DC; a pilot on every `spacing`-th carrier,
// stepping by `step` carriers per symbol over `period` symbols.
decode::OfdmConfig make_config(std::size_t fft, std::size_t cp, int half, int spacing, int step,
                               std::size_t period)
{
    decode::OfdmConfig c;
    c.sample_rate = kRate;
    c.fft_size = fft;
    c.cp_length = cp;
    for (int k = -half; k <= half; ++k) {
        if (k != 0) {
            c.carriers.push_back(k);
        }
    }
    c.pilot_table.resize(period);
    for (std::size_t s = 0; s < period; ++s) {
        for (int k : c.carriers) {
            if ((k + half) % spacing == static_cast<int>(s) * step % spacing) {
                c.pilot_table[s].push_back({k, pilot_value(k, s)});
            }
        }
    }
    c.max_integer_cfo = 8;
    return c;
}

decode::OfdmConfig config_256() { return make_config(256, 64, 100, 4, 1, 4); }
decode::OfdmConfig config_drm_b() { return make_config(1024, 256, 103, 6, 2, 3); }

std::vector<std::vector<Complex32>> qpsk_grid(const decode::OfdmConfig& c, std::size_t symbols,
                                              std::uint64_t seed)
{
    std::vector<std::vector<Complex32>> g(symbols, std::vector<Complex32>(c.carriers.size()));
    std::uint64_t state = seed;
    const float a = static_cast<float>(std::numbers::sqrt2 / 2.0);
    for (auto& row : g) {
        for (auto& v : row) {
            state = mix(state + 1);
            v = Complex32((state & 1U) != 0 ? a : -a, (state & 2U) != 0 ? a : -a);
        }
    }
    return g;
}

struct Scenario {
    std::size_t lead = 0;
    double cfo_carriers = 0.0;
    double echo_delay_samples = 0.0;
    double echo_db = 0.0;
    double snr_full_band_db = 200.0;
    std::size_t symbols = 60;
    std::size_t first_phase = 0;
};

struct Built {
    std::vector<Complex32> samples;
    siggen::OfdmTransmission tx;
};

Built build(const decode::OfdmConfig& c, const Scenario& sc, std::uint64_t seed)
{
    Built b;
    b.tx = siggen::ofdm_modulate(c, qpsk_grid(c, sc.symbols, seed), sc.first_phase).value();
    const std::size_t len = c.fft_size + c.cp_length;
    std::vector<Complex32> s(sc.lead, Complex32{});
    s.insert(s.end(), b.tx.samples.begin(), b.tx.samples.end());
    s.insert(s.end(), 2 * len, Complex32{});

    if (sc.echo_delay_samples > 0.0) {
        siggen::MultipathProfile mp;
        mp.taps.push_back({0.0, 0.0, 0.0});
        mp.taps.push_back({sc.echo_delay_samples / kRate, sc.echo_db, 1.1});
        s = siggen::apply_multipath(s, mp, kRate).value();
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        const double ph = 2.0 * std::numbers::pi * sc.cfo_carriers * static_cast<double>(i) /
                          static_cast<double>(c.fft_size);
        s[i] *= Complex32(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
    }
    if (sc.snr_full_band_db < 100.0) {
        const double p = siggen::mean_power(b.tx.samples);
        REQUIRE(siggen::add_awgn_at_power(s, p / std::pow(10.0, sc.snr_full_band_db / 10.0),
                                          seed ^ 0xA5A5U)
                    .has_value());
    }
    b.samples = std::move(s);
    return b;
}

struct Measured {
    std::size_t symbols = 0;
    double evm_db = 0.0;
    double mean_snr_db = 0.0;
    bool phases_right = true;
};

Measured measure(const decode::OfdmConfig& c, const Scenario& sc, const Built& b,
                 const std::vector<decode::OfdmSymbol>& out)
{
    const std::size_t len = c.fft_size + c.cp_length;
    Measured m;
    double err = 0.0;
    double ref = 0.0;
    double snr = 0.0;
    std::size_t snr_n = 0;
    for (const auto& sym : out) {
        const double pos = static_cast<double>(sym.start_sample - static_cast<std::int64_t>(sc.lead)) /
                           static_cast<double>(len);
        const auto t = static_cast<long long>(std::llround(pos));
        if (t < 0 || static_cast<std::size_t>(t) + c.pilot_table.size() >= sc.symbols) {
            continue;
        }
        if ((static_cast<std::size_t>(t) + sc.first_phase) % c.pilot_table.size() != sym.pilot_phase) {
            m.phases_right = false;
        }
        ++m.symbols;
        const auto& row = b.tx.grid[static_cast<std::size_t>(t)];
        for (std::size_t i = 0; i < row.size(); ++i) {
            err += std::norm(sym.equalised[i] - row[i]);
            ref += std::norm(row[i]);
            snr += sym.snr[i];
            ++snr_n;
        }
    }
    m.evm_db = 10.0 * std::log10(std::max(err, 1e-30) / std::max(ref, 1e-30));
    m.mean_snr_db = 10.0 * std::log10(std::max(snr / static_cast<double>(std::max<std::size_t>(snr_n, 1)), 1e-30));
    return m;
}

Measured run(const decode::OfdmConfig& c, const Scenario& sc, std::uint64_t seed, const char* label)
{
    const Built b = build(c, sc, seed);
    auto rx = decode::OfdmReceiver::create(c).value();
    std::vector<decode::OfdmSymbol> out;
    rx.push(b.samples, out);
    REQUIRE(rx.sync().locked);
    const Measured m = measure(c, sc, b, out);
    std::printf("ofdm %s: cfo %.4f (true %.4f) carriers, start %lld (lead %zu), timing %.3f pilot %.3f, "
                "%zu symbols, EVM %.1f dB, mean est SNR %.1f dB\n",
                label, rx.sync().cfo_carriers, sc.cfo_carriers,
                static_cast<long long>(rx.sync().first_symbol_sample), sc.lead,
                rx.sync().timing_metric, rx.sync().pilot_metric, m.symbols, m.evm_db,
                m.mean_snr_db);
    CHECK(std::abs(rx.sync().cfo_carriers - sc.cfo_carriers) < 0.02);
    CHECK(m.phases_right);
    CHECK(m.symbols + 8 + c.pilot_table.size() >= sc.symbols);
    return m;
}

}  // namespace

TEST_CASE("ofdm: clean 256-point round trip is exact to float precision", "[decode][ofdm]")
{
    const auto c = config_256();
    Scenario sc;
    sc.lead = 1000;
    sc.first_phase = 2;
    const auto m = run(c, sc, 1, "256 clean");
    CHECK(m.evm_db < -60.0);
}

TEST_CASE("ofdm: 256-point with offset, timing, two-tap multipath and noise", "[decode][ofdm]")
{
    const auto c = config_256();
    Scenario sc;
    sc.lead = 1337;
    sc.cfo_carriers = 3.27;
    sc.echo_delay_samples = 9.4;
    sc.echo_db = -6.0;
    sc.snr_full_band_db = 25.0;
    sc.first_phase = 1;
    const auto m = run(c, sc, 2, "256 impaired");
    // Noise is 25 dB below the signal across the band; the occupied carriers
    // see it about 1 dB better, the echo's notches and the estimator worse.
    // Measured 2026-11-02: EVM -23.9 dB, mean estimated SNR 24.6 dB.
    CHECK(m.evm_db < -21.0);
    CHECK(m.mean_snr_db > 20.0);
    CHECK(m.mean_snr_db < 35.0);
}

TEST_CASE("ofdm: negative integer offset beyond one carrier", "[decode][ofdm]")
{
    const auto c = config_256();
    Scenario sc;
    sc.lead = 300;
    sc.cfo_carriers = -6.71;
    sc.snr_full_band_db = 30.0;
    const auto m = run(c, sc, 3, "256 cfo -6.71");
    // Measured 2026-11-02: EVM -31.2 dB.
    CHECK(m.evm_db < -27.0);
}

TEST_CASE("ofdm: DRM-mode-B-like geometry with a 1 ms echo", "[decode][ofdm]")
{
    const auto c = config_drm_b();
    Scenario sc;
    sc.lead = 5000;
    sc.cfo_carriers = 4.6;  // 215.6 Hz at 46.875 Hz spacing
    sc.echo_delay_samples = 48.0;
    sc.echo_db = -3.0;
    sc.snr_full_band_db = 25.0;
    sc.symbols = 45;
    sc.first_phase = 1;
    const auto m = run(c, sc, 4, "drm-b-like");
    // Measured 2026-11-02: EVM -25.8 dB, mean estimated SNR 29.5 dB.
    CHECK(m.evm_db < -22.0);
    CHECK(m.mean_snr_db > 22.0);
    CHECK(m.mean_snr_db < 36.0);
}

TEST_CASE("ofdm: output does not depend on how the stream is split", "[decode][ofdm]")
{
    const auto c = config_256();
    Scenario sc;
    sc.lead = 777;
    sc.cfo_carriers = 1.4;
    sc.echo_delay_samples = 5.0;
    sc.echo_db = -8.0;
    sc.snr_full_band_db = 20.0;
    const Built b = build(c, sc, 5);

    auto whole = decode::OfdmReceiver::create(c).value();
    std::vector<decode::OfdmSymbol> a;
    whole.push(b.samples, a);

    for (const std::size_t seed : {11U, 12U, 13U}) {
        auto split = decode::OfdmReceiver::create(c).value();
        std::vector<decode::OfdmSymbol> o;
        std::size_t at = 0;
        std::uint64_t state = seed;
        while (at < b.samples.size()) {
            state = mix(state);
            const std::size_t n = std::min<std::size_t>(1 + state % 700, b.samples.size() - at);
            split.push(std::span<const Complex32>(b.samples.data() + at, n), o);
            at += n;
        }
        REQUIRE(o.size() == a.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            REQUIRE(o[i].start_sample == a[i].start_sample);
            REQUIRE(o[i].pilot_phase == a[i].pilot_phase);
            REQUIRE(o[i].equalised == a[i].equalised);
            REQUIRE(o[i].snr == a[i].snr);
        }
    }
    CHECK(a.size() > 50);
}

TEST_CASE("ofdm: noise alone does not lock", "[decode][ofdm]")
{
    const auto c = config_256();
    std::vector<Complex32> s(40000, Complex32{});
    REQUIRE(siggen::add_awgn_at_power(s, 1.0, 99).has_value());
    auto rx = decode::OfdmReceiver::create(c).value();
    std::vector<decode::OfdmSymbol> out;
    rx.push(s, out);
    CHECK_FALSE(rx.sync().locked);
    CHECK(out.empty());
}

TEST_CASE("ofdm: configuration is validated", "[decode][ofdm]")
{
    auto c = config_256();
    c.fft_size = 200;
    CHECK_FALSE(decode::validate(c).has_value());
    c = config_256();
    c.pilot_table[0].push_back({127, Complex32{1.0F, 0.0F}});
    CHECK_FALSE(decode::validate(c).has_value());
    c = config_256();
    c.carriers.push_back(5);
    CHECK_FALSE(decode::validate(c).has_value());
}
