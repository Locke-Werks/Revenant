// GPS L2C and L5 CNAV: codes against IS-GPS-200N Table 3-IIa and IS-GPS-705J
// Table 3-Ia, the FEC and framing round trip, and synthetic constellations.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <numbers>
#include <random>
#include <set>
#include <vector>

#include "core/decode/gps_cnav.h"
#include "core/dsp/synth/gnss_mod.h"
#include "core/dsp/synth/gps_cnav_mod.h"

using namespace revenant;
using namespace revenant::decode::gnss;

namespace {

std::vector<std::uint8_t> random_bits(std::size_t n, std::mt19937& rng) {
    std::vector<std::uint8_t> v(n);
    for (auto& b : v) {
        b = static_cast<std::uint8_t>(rng() & 1U);
    }
    return v;
}

struct Sent {
    std::vector<std::vector<std::uint8_t>> messages;
    std::vector<std::uint8_t> symbols;
};

// Messages of type 10 then 11 alternating, TOW counting up by `tow_step`.
Sent cnav_stream(unsigned prn, unsigned count, unsigned tow0, unsigned tow_step,
                 std::mt19937& rng) {
    Sent s;
    std::vector<std::uint8_t> bits;
    for (unsigned k = 0; k < count; ++k) {
        const auto fill = random_bits(238, rng);
        const unsigned type = k % 2 == 0 ? 10U : 11U;
        const auto payload =
            type == 10 ? siggen::cnav_type10_payload(2300, 100 + prn, 200 + prn, fill) : fill;
        auto m = siggen::cnav_message(prn, type, tow0 + k * tow_step, false, payload);
        bits.insert(bits.end(), m.begin(), m.end());
        s.messages.push_back(std::move(m));
    }
    s.symbols = siggen::cnav_encode(bits);
    return s;
}

}  // namespace

TEST_CASE("L2 CM and CL reproduce Table 3-IIa end states", "[gnss][l2c]") {
    for (unsigned prn = 1; prn <= 37; ++prn) {
        INFO("PRN " << prn);
        CHECK(l2c_register_end(l2c_initial_state(prn, false), 10230) ==
              l2c_table_end_state(prn, false));
        CHECK(l2c_register_end(l2c_initial_state(prn, true), 767250) ==
              l2c_table_end_state(prn, true));
        CHECK(l2cm_code(prn).size() == 10230);
    }
}

TEST_CASE("L5 codes reproduce Table 3-Ia initial XB states", "[gnss][l5]") {
    for (unsigned prn = 1; prn <= 37; ++prn) {
        for (bool q5 : {false, true}) {
            INFO("PRN " << prn << (q5 ? " Q5" : " I5"));
            const auto code = l5_code(prn, q5);
            REQUIRE(code.size() == 10230);
            // XA starts all ones, so the first 13 chips are the complement
            // of the printed XB state, read from its rightmost bit.
            const unsigned state = l5_table_xb_state(prn, q5);
            for (unsigned k = 0; k < 13; ++k) {
                const unsigned chip = code[k] < 0 ? 1U : 0U;
                CHECK(chip == (1U ^ ((state >> k) & 1U)));
            }
        }
    }
    CHECK(l5_nh10().size() == 10);
    CHECK(l5_nh20().size() == 20);
}

TEST_CASE("CNAV framing finds messages through FEC, noise and inversion", "[gnss][l2c][l5]") {
    std::mt19937 rng(3);
    const auto sent = cnav_stream(7, 6, 1000, 2, rng);
    std::normal_distribution<float> noise(0.0F, 0.5F);  // Es/N0 3 dB, Eb/N0 6 dB
    for (bool invert : {false, true}) {
        for (std::size_t skip : {0U, 1U}) {
            CnavFramer f(7);
            std::vector<CnavMessage> out;
            for (std::size_t i = skip; i < sent.symbols.size(); ++i) {
                float v = sent.symbols[i] != 0 ? -1.0F : 1.0F;
                v = (invert ? -v : v) + noise(rng);
                f.push(v, i, out);
            }
            INFO("invert " << invert << " skip " << skip);
            // The last message is still inside the traceback window.
            REQUIRE(out.size() >= 4);
            for (const auto& m : out) {
                const auto k = static_cast<std::size_t>((m.tow - 1000) / 2);
                REQUIRE(k < sent.messages.size());
                CHECK(std::equal(m.bits.begin(), m.bits.end(), sent.messages[k].begin()));
                CHECK(m.msg_prn == 7);
                CHECK(m.sample == 600 * k);
            }
        }
    }
}

TEST_CASE("L2 CM acquisition threshold clears noise", "[gnss][l2c]") {
    constexpr dsp::SampleRate kRate = 2'048'000;
    auto synth = siggen::GnssSynth::create(kRate, {}, 11);
    REQUIRE(synth);
    std::vector<dsp::Complex32> block(siggen::gnss_samples(kRate, 0.0615));
    const auto fc = cnav_front_end(CnavBand::L2C, kRate);
    double worst = 0.0;
    for (unsigned prn : {1U, 9U, 30U}) {
        synth->generate(block);
        SignalSpec s;
        s.id = prn;
        s.code = l2cm_replica(prn);
        s.carrier_hz = 1227.60e6;
        const auto ev = acquire(block, kRate, s, fc.acquisition);
        worst = std::max(worst, ev.peak_ratio);
        CHECK_FALSE(ev.found);
    }
    WARN("L2 CM noise-only peak ratio, worst of 3: " << worst << ", threshold "
                                                     << fc.acquisition.threshold);
}

TEST_CASE("L2C decodes six satellites", "[gnss][l2c]") {
    constexpr dsp::SampleRate kRate = 2'048'000;
    std::mt19937 rng(1227);
    const std::array<unsigned, 6> prns = {2, 6, 11, 17, 24, 30};
    std::vector<siggen::GnssSatelliteSpec> specs;
    std::map<unsigned, Sent> sent;
    for (std::size_t i = 0; i < prns.size(); ++i) {
        siggen::CnavSatellite sat;
        sat.prn = prns[i];
        sat.code_phase_chips = 1000.5 + 3011.0 * static_cast<double>(i);
        sat.doppler_hz = 2400.0 - 900.0 * static_cast<double>(i);
        sat.cn0_dbhz = 35.0 + 2.0 * static_cast<double>(i);
        sent[prns[i]] = cnav_stream(prns[i], 3, 5000, 2, rng);
        // Start part-way into the first message so a whole one follows soon.
        auto sym = sent[prns[i]].symbols;
        sat.symbols.assign(sym.begin() + 400 + 20 * static_cast<std::ptrdiff_t>(i), sym.end());
        for (auto& sp : siggen::l2c_satellite(sat)) {
            specs.push_back(std::move(sp));
        }
    }
    auto synth = siggen::GnssSynth::create(kRate, specs, 77);
    REQUIRE(synth);
    GpsCnavConfig cfg;
    cfg.band = CnavBand::L2C;
    cfg.sample_rate = kRate;
    cfg.prns = {2, 6, 11, 17, 24, 30, 5, 20};
    auto dec = GpsCnavDecoder::create(cfg);
    REQUIRE(dec);
    GpsCnavOutput out;
    std::vector<dsp::Complex32> chunk(kRate / 4);
    const double seconds = 24.0;
    double cpu = 0.0;
    for (std::size_t b = 0; b < static_cast<std::size_t>(seconds * 4.0); ++b) {
        synth->generate(chunk);
        const auto t0 = std::chrono::steady_clock::now();
        dec->feed(chunk, out);
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    WARN("GPS L2C decoder: " << cpu / seconds << " s CPU per s of input at 2.048 MS/s, 8 PRNs searched over +-5 kHz, 6 tracked");
    std::map<unsigned, CnavStatus> status;
    for (const auto& st : out.status) {
        status[st.prn] = st;
    }
    std::map<unsigned, unsigned> got;
    for (const auto& m : out.messages) {
        INFO("PRN " << m.prn << " TOW " << m.tow);
        const auto& s = sent.at(m.prn);
        const auto k = static_cast<std::size_t>((m.tow - 5000) / 2);
        REQUIRE(k < s.messages.size());
        CHECK(std::equal(m.bits.begin(), m.bits.end(), s.messages[k].begin()));
        if (m.type == 10) {
            CHECK(m.wn == 2300);
            CHECK(m.toe == 200 + m.prn);
        }
        ++got[m.prn];
    }
    for (std::size_t i = 0; i < prns.size(); ++i) {
        INFO("PRN " << prns[i]);
        WARN("L2C PRN " << prns[i] << " sent " << 35.0 + 2.0 * static_cast<double>(i)
                        << " dB-Hz, estimated " << status[prns[i]].cn0_dbhz << ", messages "
                        << got[prns[i]]);
        CHECK(got[prns[i]] >= 1);
        CHECK(std::abs(status[prns[i]].doppler_hz - (2400.0 - 900.0 * static_cast<double>(i))) < 5.0);
    }
    CHECK(status.count(5) == 0);
    CHECK(status.count(20) == 0);
}

namespace {

// Windowed-sinc low-pass and decimate, streaming: what a tap narrower than
// the signal does to it.
class Decimator {
public:
    Decimator(unsigned factor, double cutoff_fraction, std::size_t taps) : m_(factor) {
        h_.resize(taps);
        const double mid = static_cast<double>(taps - 1) / 2.0;
        double sum = 0.0;
        for (std::size_t i = 0; i < taps; ++i) {
            const double x = static_cast<double>(i) - mid;
            const double sinc = x == 0.0 ? 2.0 * cutoff_fraction
                                         : std::sin(2.0 * std::numbers::pi * cutoff_fraction * x) /
                                               (std::numbers::pi * x);
            const double w = 0.54 - 0.46 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                                    static_cast<double>(taps - 1));
            h_[i] = sinc * w;
            sum += h_[i];
        }
        for (auto& v : h_) {
            v /= sum;
        }
        hist_.assign(taps, {});
    }
    void run(std::span<const dsp::Complex32> in, std::vector<dsp::Complex32>& out) {
        for (const auto& x : in) {
            hist_[pos_] = x;
            pos_ = (pos_ + 1) % hist_.size();
            if (++phase_ == m_) {
                phase_ = 0;
                std::complex<double> acc{};
                for (std::size_t k = 0; k < h_.size(); ++k) {
                    const auto& v = hist_[(pos_ + k) % hist_.size()];
                    acc += h_[k] * std::complex<double>(v.real(), v.imag());
                }
                // Noise power drops by the decimation factor through the
                // filter; scale back to unit variance per output sample so the
                // C/N0 convention of gnss_mod.h still holds.
                acc *= std::sqrt(static_cast<double>(m_));
                out.emplace_back(static_cast<float>(acc.real()), static_cast<float>(acc.imag()));
            }
        }
    }

private:
    unsigned m_;
    std::vector<double> h_;
    std::vector<dsp::Complex32> hist_;
    std::size_t pos_ = 0;
    unsigned phase_ = 0;
};

}  // namespace

TEST_CASE("L5 decodes through a 4 MS/s tap", "[gnss][l5]") {
    // Synthesised at 20.46 MS/s, the full main lobe, then filtered to
    // +-1.8 MHz and decimated by 5: the loss against the sent C/N0 is what a
    // dongle-width tap costs. I5 is sent at 44 to 50 dB-Hz over the full
    // band, which the tap turns into roughly 35 to 41.
    constexpr dsp::SampleRate kWide = 20'460'000;
    constexpr dsp::SampleRate kRate = 4'092'000;
    std::mt19937 rng(1176);
    const std::array<unsigned, 4> prns = {1, 9, 22, 32};
    std::vector<siggen::GnssSatelliteSpec> specs;
    std::map<unsigned, Sent> sent;
    for (std::size_t i = 0; i < prns.size(); ++i) {
        siggen::CnavSatellite sat;
        sat.prn = prns[i];
        sat.code_phase_chips = 777.0 + 2003.0 * static_cast<double>(i);
        sat.doppler_hz = 1800.0 - 1100.0 * static_cast<double>(i);
        sat.cn0_dbhz = 44.0 + 2.0 * static_cast<double>(i);
        sent[prns[i]] = cnav_stream(prns[i], 4, 7000, 1, rng);
        auto sym = sent[prns[i]].symbols;
        sat.symbols.assign(sym.begin() + 300 + 20 * static_cast<std::ptrdiff_t>(i), sym.end());
        for (auto& sp : siggen::l5_satellite(sat)) {
            specs.push_back(std::move(sp));
        }
    }
    auto synth = siggen::GnssSynth::create(kWide, specs, 5);
    REQUIRE(synth);
    Decimator dec5(5, 1.8e6 / static_cast<double>(kWide), 81);
    GpsCnavConfig cfg;
    cfg.band = CnavBand::L5;
    cfg.sample_rate = kRate;
    cfg.prns = {1, 9, 22, 32, 4, 15};
    auto dec = GpsCnavDecoder::create(cfg);
    REQUIRE(dec);
    GpsCnavOutput out;
    std::vector<dsp::Complex32> wide(kWide / 10);
    std::vector<dsp::Complex32> narrow;
    const double seconds = 15.0;
    double cpu = 0.0;
    for (std::size_t b = 0; b < static_cast<std::size_t>(seconds * 10.0); ++b) {
        synth->generate(wide);
        narrow.clear();
        dec5.run(wide, narrow);
        const auto t0 = std::chrono::steady_clock::now();
        dec->feed(narrow, out);
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    WARN("GPS L5 decoder: " << cpu / seconds << " s CPU per s of input at 4.092 MS/s, 6 PRNs searched, 4 tracked");
    std::map<unsigned, CnavStatus> status;
    for (const auto& st : out.status) {
        status[st.prn] = st;
    }
    std::map<unsigned, unsigned> got;
    for (const auto& m : out.messages) {
        INFO("PRN " << m.prn << " TOW " << m.tow);
        const auto& s = sent.at(m.prn);
        const auto k = static_cast<std::size_t>(m.tow - 7000);
        REQUIRE(k < s.messages.size());
        CHECK(std::equal(m.bits.begin(), m.bits.end(), s.messages[k].begin()));
        ++got[m.prn];
    }
    for (std::size_t i = 0; i < prns.size(); ++i) {
        INFO("PRN " << prns[i]);
        const double sent_cn0 = 44.0 + 2.0 * static_cast<double>(i);
        WARN("L5 PRN " << prns[i] << " I5 sent " << sent_cn0 << " dB-Hz, estimated "
                       << status[prns[i]].cn0_dbhz << " through the 4.092 MS/s tap, messages "
                       << got[prns[i]]);
        CHECK(got[prns[i]] >= 1);
        CHECK(std::abs(status[prns[i]].doppler_hz - (1800.0 - 1100.0 * static_cast<double>(i))) < 5.0);
    }
}

TEST_CASE("L5 tracking and symbol sync do not depend on chunking", "[gnss][l5]") {
    constexpr dsp::SampleRate kRate = 4'092'000;
    std::mt19937 rng(9);
    siggen::CnavSatellite sat;
    sat.prn = 9;
    sat.code_phase_chips = 4321.0;
    sat.doppler_hz = -700.0;
    sat.cn0_dbhz = 42.0;
    sat.symbols = cnav_stream(9, 2, 10, 1, rng).symbols;
    auto synth = siggen::GnssSynth::create(kRate, siggen::l5_satellite(sat), 1);
    std::vector<dsp::Complex32> all(siggen::gnss_samples(kRate, 1.5));
    synth->generate(all);
    auto run = [&](std::size_t chunk) {
        GpsCnavConfig cfg;
        cfg.band = CnavBand::L5;
        cfg.sample_rate = kRate;
        cfg.prns = {9};
        cfg.status_periods = 50;
        auto dec = GpsCnavDecoder::create(cfg);
        GpsCnavOutput out;
        for (std::size_t i = 0; i < all.size(); i += chunk) {
            const auto n = std::min(chunk, all.size() - i);
            dec->feed(std::span<const dsp::Complex32>(all).subspan(i, n), out);
        }
        return out;
    };
    const auto a = run(40920);
    const auto b = run(997);
    REQUIRE(!a.status.empty());
    REQUIRE(a.status.size() == b.status.size());
    for (std::size_t i = 0; i < a.status.size(); ++i) {
        CHECK(a.status[i].doppler_hz == b.status[i].doppler_hz);
        CHECK(a.status[i].symbol_synced == b.status[i].symbol_synced);
    }
    CHECK(a.status.back().symbol_synced);
}
