// The generic coherent M-PSK demodulator in core/decode/psk_demod.h against
// the transmitter in core/dsp/synth/psk_mod.h.
//
// Each modulation is run with a carrier offset of a few percent of the symbol
// rate, a fractional timing offset and a small clock error, and the symbol
// error rate is compared with the textbook figure for coherent detection in
// white noise (Proakis, Digital Communications, 5th ed., section 4.3). The
// phase ambiguity the demodulator reports is resolved here the way a caller's
// sync word would, by trying every rotation.
//
// Error rates are measured and printed and the assertions are loose: the
// demodulator should sit within about a decibel of theory, not on it.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <random>
#include <span>
#include <vector>

#include "core/decode/psk_demod.h"
#include "core/dsp/synth/psk_mod.h"

using namespace revenant;
using decode::PskModulation;

namespace {

constexpr dsp::SampleRate kRate = 48000;

const char* name(PskModulation m) {
    switch (m) {
        case PskModulation::Bpsk: return "BPSK";
        case PskModulation::Qpsk: return "QPSK";
        case PskModulation::Oqpsk: return "OQPSK";
        case PskModulation::Psk8: return "8PSK";
    }
    return "?";
}

std::vector<std::uint8_t> random_symbols(std::size_t count, unsigned order, std::uint64_t seed) {
    std::vector<std::uint8_t> out(count);
    std::uint64_t state = seed;
    for (auto& s : out) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        s = static_cast<std::uint8_t>((state >> 40U) % order);
    }
    return out;
}

double q_function(double x) { return 0.5 * std::erfc(x / std::numbers::sqrt2); }

// Symbol error probability for coherent detection at Es/N0 (linear). Exact
// for BPSK and QPSK, the usual nearest-neighbour bound for 8PSK.
double theory_ser(PskModulation m, double es_n0_db) {
    const double es_n0 = std::pow(10.0, es_n0_db / 10.0);
    switch (decode::psk_order(m)) {
        case 2: return q_function(std::sqrt(2.0 * es_n0));
        case 4: {
            const double q = q_function(std::sqrt(es_n0));
            return 2.0 * q - q * q;
        }
        default:
            return 2.0 * q_function(std::sqrt(2.0 * es_n0) * std::sin(std::numbers::pi / 8.0));
    }
}

// White Gaussian noise at Es/N0, the signal's energy per symbol measured from
// the middle of the clean rendering.
void add_noise(std::vector<dsp::Complex32>& x, double sps, double es_n0_db, std::uint64_t seed) {
    double power = 0.0;
    const std::size_t a = x.size() / 4;
    const std::size_t b = 3 * x.size() / 4;
    for (std::size_t i = a; i < b; ++i) {
        power += std::norm(x[i]);
    }
    power /= static_cast<double>(b - a);
    const double n0 = power * sps / std::pow(10.0, es_n0_db / 10.0);
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> g(0.0, std::sqrt(n0 / 2.0));
    for (auto& s : x) {
        s += dsp::Complex32(static_cast<float>(g(rng)), static_cast<float>(g(rng)));
    }
}

struct Scenario {
    PskModulation modulation = PskModulation::Qpsk;
    double symbol_rate = 4800.0;
    double offset_fraction = 0.03;  // of the symbol rate
    double timing_offset = 0.37;
    double clock_error = 40e-6;
    double es_n0_db = 100.0;
    std::size_t symbols = 6000;
    std::uint64_t seed = 1;
};

struct Run {
    std::vector<dsp::Complex32> samples;
    std::vector<std::uint8_t> sent;
};

Run render(const Scenario& s) {
    const unsigned order = decode::psk_order(s.modulation);
    Run run;
    run.sent = random_symbols(s.symbols, order, 0x5053'4B00ULL + s.seed);
    siggen::PskModConfig mod;
    mod.rate = kRate;
    mod.symbol_rate = s.symbol_rate;
    mod.modulation = s.modulation;
    mod.carrier_offset_hz = s.offset_fraction * s.symbol_rate;
    mod.initial_phase = 0.7;
    mod.timing_offset_samples = s.timing_offset;
    mod.clock_error = s.clock_error;
    run.samples = siggen::psk_render(mod, run.sent).value();
    if (s.es_n0_db < 99.0) {
        add_noise(run.samples, static_cast<double>(kRate) / s.symbol_rate, s.es_n0_db,
                  0xA5A5ULL + s.seed);
    }
    return run;
}

decode::PskDemodConfig demod_config(const Scenario& s) {
    decode::PskDemodConfig c;
    c.rate = kRate;
    c.symbol_rate = s.symbol_rate;
    c.modulation = s.modulation;
    c.carrier_bandwidth = 0.01;
    c.timing_bandwidth = 0.005;
    return c;
}

std::vector<decode::PskSymbol> demodulate(decode::PskDemod& demod,
                                          const std::vector<dsp::Complex32>& samples,
                                          std::size_t chunk, std::uint64_t jitter_seed = 0) {
    std::vector<decode::PskSymbol> out;
    std::uint64_t state = jitter_seed;
    std::size_t i = 0;
    while (i < samples.size()) {
        std::size_t n = chunk;
        if (jitter_seed != 0) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            n = 1 + static_cast<std::size_t>((state >> 33U) % (2 * chunk));
        }
        n = std::min(n, samples.size() - i);
        const auto got = demod.process(std::span(samples).subspan(i, n));
        out.insert(out.end(), got.begin(), got.end());
        i += n;
    }
    return out;
}

struct Score {
    std::size_t errors = 0;
    std::size_t compared = 0;
    double ser() const { return compared ? static_cast<double>(errors) / compared : 1.0; }
};

// Best alignment and rotation, skipping the first `settle` sent symbols while
// the loops pull in.
Score score(const std::vector<std::uint8_t>& sent, const std::vector<decode::PskSymbol>& got,
            unsigned order, std::size_t settle) {
    Score best{~std::size_t{0}, 0};
    for (std::size_t lag = 0; lag < 80; ++lag) {
        for (unsigned rot = 0; rot < order; ++rot) {
            Score s;
            for (std::size_t i = settle; i < sent.size() && i + lag < got.size(); ++i) {
                ++s.compared;
                if ((sent[i] + rot) % order != got[i + lag].index) {
                    ++s.errors;
                }
            }
            if (s.compared > sent.size() / 2 && s.errors < best.errors) {
                best = s;
            }
        }
    }
    return best;
}

// OQPSK's quarter-turn ambiguity is not a relabelling: a rotated OQPSK signal
// is a valid one with its rails swapped and re-paired half a symbol over. So
// score the interleaved rail stream I0 Q0 I1 Q1 ..., where a quarter turn is
// a shift by one rail plus a polarity per rail, and count a symbol wrong when
// either of its rails is.
Score score_rails(const std::vector<std::uint8_t>& sent, const std::vector<decode::PskSymbol>& got,
                  std::size_t settle) {
    const auto rails = [](auto count, auto point) {
        std::vector<std::uint8_t> r;
        for (std::size_t k = 0; k < count; ++k) {
            const dsp::Complex32 p = point(k);
            r.push_back(p.real() > 0.0F ? 1 : 0);
            r.push_back(p.imag() > 0.0F ? 1 : 0);
        }
        return r;
    };
    const auto t = rails(sent.size(), [&](std::size_t k) {
        return decode::psk_point(PskModulation::Oqpsk, sent[k]);
    });
    const auto r = rails(got.size(), [&](std::size_t k) {
        return decode::psk_point(PskModulation::Oqpsk, got[k].index);
    });
    Score best{~std::size_t{0}, 0};
    for (std::size_t lag = 0; lag < 160; ++lag) {
        for (unsigned flips = 0; flips < 4; ++flips) {
            Score s;
            for (std::size_t k = settle; 2 * k + 1 < t.size() && 2 * k + 1 + lag < r.size(); ++k) {
                bool wrong = false;
                for (std::size_t j = 2 * k; j <= 2 * k + 1; ++j) {
                    const std::uint8_t flip = (flips >> (j & 1U)) & 1U;
                    wrong = wrong || ((t[j] ^ flip) != r[j + lag]);
                }
                ++s.compared;
                s.errors += wrong ? 1 : 0;
            }
            if (s.compared > sent.size() / 2 && s.errors < best.errors) {
                best = s;
            }
        }
    }
    return best;
}

Score score_any(PskModulation m, const std::vector<std::uint8_t>& sent,
                const std::vector<decode::PskSymbol>& got, std::size_t settle) {
    return m == PskModulation::Oqpsk ? score_rails(sent, got, settle)
                                     : score(sent, got, decode::psk_order(m), settle);
}

constexpr PskModulation kAll[] = {PskModulation::Bpsk, PskModulation::Qpsk, PskModulation::Oqpsk,
                                  PskModulation::Psk8};

}  // namespace

TEST_CASE("PSK demodulator locks and decodes cleanly with carrier and timing offsets",
          "[decode][psk]") {
    for (const PskModulation m : kAll) {
        for (const double offset : {0.03, -0.05}) {
            for (const double symbol_rate : {4800.0, 10983.0}) {
                Scenario s;
                s.modulation = m;
                s.offset_fraction = offset;
                s.symbol_rate = symbol_rate;
                s.symbols = 3000;
                const Run run = render(s);
                auto demod = decode::PskDemod::create(demod_config(s)).value();
                const auto got = demodulate(demod, run.samples, 4096);
                const Score sc = score_any(m, run.sent, got, 600);
                std::printf("clean %-5s offset %+.2f Rs, sps %.2f: coarse %.1f Hz, final %.1f Hz "
                            "(true %.1f), lock %.3f, %zu/%zu errors\n",
                            name(m), offset, kRate / symbol_rate, demod.coarse_offset_hz(),
                            demod.frequency_offset_hz(), offset * symbol_rate, demod.lock_metric(),
                            sc.errors, sc.compared);
                CHECK(demod.ambiguity() == decode::psk_order(m));
                CHECK(demod.locked());
                CHECK(std::abs(demod.frequency_offset_hz() - offset * symbol_rate) <
                      0.002 * symbol_rate);
                CHECK(sc.errors == 0);
            }
        }
    }
}

TEST_CASE("PSK demodulator symbol error rate tracks coherent theory in white noise",
          "[decode][psk]") {
    struct Point {
        PskModulation m;
        double es_n0_db;
    };
    const Point points[] = {
        {PskModulation::Bpsk, 3.0},  {PskModulation::Bpsk, 5.0},  {PskModulation::Bpsk, 7.0},
        {PskModulation::Qpsk, 6.0},  {PskModulation::Qpsk, 8.0},  {PskModulation::Qpsk, 10.0},
        {PskModulation::Oqpsk, 6.0}, {PskModulation::Oqpsk, 8.0}, {PskModulation::Oqpsk, 10.0},
        {PskModulation::Psk8, 12.0}, {PskModulation::Psk8, 14.0}, {PskModulation::Psk8, 16.0},
    };
    for (const Point& p : points) {
        Scenario s;
        s.modulation = p.m;
        s.es_n0_db = p.es_n0_db;
        s.symbols = 20000;
        s.seed = static_cast<std::uint64_t>(p.es_n0_db * 10.0) + static_cast<std::uint64_t>(p.m);
        const Run run = render(s);
        auto demod = decode::PskDemod::create(demod_config(s)).value();
        const auto got = demodulate(demod, run.samples, 4096);
        const Score sc = score_any(p.m, run.sent, got, 1000);
        const double theory = theory_ser(p.m, p.es_n0_db);
        const double one_db_worse = theory_ser(p.m, p.es_n0_db - 1.0);
        std::printf("%-5s Es/N0 %4.1f dB: SER %.2e (theory %.2e, 1 dB worse %.2e), lock %.3f, "
                    "coarse %.1f Hz\n",
                    name(p.m), p.es_n0_db, sc.ser(), theory, one_db_worse, demod.lock_metric(),
                    demod.coarse_offset_hz());
        CHECK(demod.locked());
        CHECK(sc.ser() <= 1.2 * one_db_worse + 5e-4);
        CHECK(sc.ser() >= 0.3 * theory);
    }
}

TEST_CASE("PSK demodulator output does not depend on how the input is split",
          "[decode][psk]") {
    for (const PskModulation m : kAll) {
        Scenario s;
        s.modulation = m;
        s.es_n0_db = 12.0;
        s.symbols = 2000;
        s.symbol_rate = 10983.0;
        const Run run = render(s);
        auto whole = decode::PskDemod::create(demod_config(s)).value();
        const auto reference = demodulate(whole, run.samples, run.samples.size());
        for (const std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{513}}) {
            auto demod = decode::PskDemod::create(demod_config(s)).value();
            const auto got = demodulate(demod, run.samples, chunk, chunk == 7 ? 99 : 0);
            REQUIRE(got.size() == reference.size());
            bool same = true;
            for (std::size_t i = 0; i < got.size(); ++i) {
                same = same && got[i].symbol == reference[i].symbol &&
                       got[i].index == reference[i].index &&
                       got[i].position == reference[i].position;
            }
            CHECK(same);
        }
        // reset() returns it to a fresh demodulator.
        whole.reset();
        const auto again = demodulate(whole, run.samples, 1000);
        REQUIRE(again.size() == reference.size());
        CHECK(again.back().symbol == reference.back().symbol);
    }
}

TEST_CASE("PSK demodulator soft bits agree with the labelling of the hard decision",
          "[decode][psk]") {
    for (const PskModulation m : kAll) {
        Scenario s;
        s.modulation = m;
        s.es_n0_db = 15.0;
        s.symbols = 1500;
        const Run run = render(s);
        auto config = demod_config(s);
        // A non-Gray labelling, to show the map is honoured.
        for (unsigned k = 0; k < 8; ++k) {
            config.bit_map[k] = static_cast<std::uint8_t>(k % decode::psk_order(m));
        }
        auto demod = decode::PskDemod::create(config).value();
        const auto got = demodulate(demod, run.samples, 1024);
        const unsigned bits = decode::psk_bits_per_symbol(m);
        std::size_t disagreements = 0;
        for (const auto& sym : got) {
            const unsigned label = config.bit_map[sym.index];
            for (unsigned b = 0; b < bits; ++b) {
                const bool one = ((label >> (bits - 1U - b)) & 1U) != 0;
                if ((sym.soft[b] > 0.0F) != one) {
                    ++disagreements;
                }
            }
        }
        CHECK(disagreements == 0);
    }
}

TEST_CASE("PSK demodulator rejects configurations it cannot run", "[decode][psk]") {
    decode::PskDemodConfig c;
    c.rate = kRate;
    c.symbol_rate = 30000.0;
    CHECK_FALSE(decode::PskDemod::create(c).has_value());
    c.symbol_rate = 4800.0;
    c.bit_map[0] = 7;
    CHECK_FALSE(decode::PskDemod::create(c).has_value());
}
