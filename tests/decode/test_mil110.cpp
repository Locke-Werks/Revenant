// MIL-STD-188-110C serial tone: the building blocks against the values the
// document prints, and the receiver against the transmitter through noise, a
// fixed two-path channel, a mistuned receiver and arbitrary chunking.
//
// Error rates are measured and reported in WARN lines with loose assertions,
// for the reason tests/decode/CMakeLists.txt gives.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "core/decode/mil110.h"
#include "core/dsp/synth/mil110_mod.h"

using namespace revenant;
using decode::Mil110Interleave;
using decode::Mil110Mode;
using decode::Mil110Rate;

namespace {

constexpr dsp::SampleRate kRate = 9600;

// Signal-to-noise ratio in a 3 kHz bandwidth, white noise across the
// Nyquist band.
void add_noise(std::vector<float>& x, double snr_db, double amplitude, dsp::SampleRate rate,
               std::uint32_t seed) {
    const double signal = amplitude * amplitude / 2.0;
    const double noise_3k = signal / std::pow(10.0, snr_db / 10.0);
    const double sigma = std::sqrt(noise_3k * (static_cast<double>(rate) / 2.0) / 3000.0);
    std::mt19937 rng(seed);
    std::normal_distribution<double> n(0.0, sigma);
    for (auto& v : x) {
        v = static_cast<float>(v + n(rng));
    }
}

// Fixed two-tap channel: a second path 6 dB down and 2 ms late.
std::vector<float> two_path(const std::vector<float>& x, dsp::SampleRate rate) {
    const auto delay = static_cast<std::size_t>(rate) * 2 / 1000;
    std::vector<float> y(x.size(), 0.0F);
    for (std::size_t i = 0; i < x.size(); ++i) {
        y[i] = x[i] + (i >= delay ? 0.5F * x[i - delay] : 0.0F);
    }
    return y;
}

// A sound card whose clock runs ppm fast: audio rendered at 48000 S/s read
// back at 9600 (1 + ppm) S/s, by cubic interpolation, which is close to exact
// at five times oversampling.
std::vector<float> sound_card(const std::vector<float>& x48k, double ppm) {
    const double step = 5.0 / (1.0 + ppm * 1e-6);
    std::vector<float> y;
    y.reserve(static_cast<std::size_t>(static_cast<double>(x48k.size()) / step) + 1);
    for (double t = 1.0; t + 2.0 < static_cast<double>(x48k.size()); t += step) {
        const auto i = static_cast<std::size_t>(t);
        const double mu = t - static_cast<double>(i);
        const double a = x48k[i - 1];
        const double b = x48k[i];
        const double c = x48k[i + 1];
        const double d = x48k[i + 2];
        y.push_back(static_cast<float>(-mu * (mu - 1) * (mu - 2) / 6 * a +
                                       (mu + 1) * (mu - 1) * (mu - 2) / 2 * b -
                                       (mu + 1) * mu * (mu - 2) / 2 * c +
                                       (mu + 1) * mu * (mu - 1) / 6 * d));
    }
    return y;
}

std::vector<std::uint8_t> random_bits(std::size_t n, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<std::uint8_t> b(n);
    for (auto& v : b) {
        v = static_cast<std::uint8_t>(rng() & 1U);
    }
    return b;
}

std::vector<decode::Mil110Message> run(const std::vector<float>& audio, dsp::SampleRate rate,
                                       std::size_t chunk_seed) {
    decode::Mil110Config cfg;
    cfg.rate = rate;
    auto dec = decode::Mil110Decoder::create(cfg);
    REQUIRE(dec.has_value());
    std::vector<decode::Mil110Message> out;
    if (chunk_seed == 0) {
        dec->process(audio, out);
    } else {
        std::mt19937 rng(static_cast<std::uint32_t>(chunk_seed));
        std::uniform_int_distribution<std::size_t> len(1, 3000);
        std::size_t i = 0;
        while (i < audio.size()) {
            const std::size_t n = std::min(len(rng), audio.size() - i);
            dec->process(std::span<const float>(audio.data() + i, n), out);
            i += n;
        }
    }
    dec->flush(out);
    return out;
}

double ber(const std::vector<std::uint8_t>& sent, const std::vector<std::uint8_t>& got) {
    std::size_t errors = 0;
    for (std::size_t i = 0; i < sent.size(); ++i) {
        if (i >= got.size() || got[i] != sent[i]) {
            ++errors;
        }
    }
    return static_cast<double>(errors) / static_cast<double>(sent.size());
}

const char* rate_name(Mil110Rate r) {
    switch (r) {
        case Mil110Rate::Bps75: return "75";
        case Mil110Rate::Bps150: return "150";
        case Mil110Rate::Bps300: return "300";
        case Mil110Rate::Bps600: return "600";
        case Mil110Rate::Bps1200: return "1200";
        case Mil110Rate::Bps2400: return "2400";
        case Mil110Rate::Bps2400Voice: return "2400V";
        case Mil110Rate::Bps4800: return "4800";
    }
    return "?";
}

struct Trial {
    double snr_db = 99.0;
    bool multipath = false;
    double offset_hz = 0.0;
    std::size_t chunk_seed = 0;
};

// Sends about two seconds of user data and returns the BER, or 1 when the
// message is not found.
double round_trip(Mil110Mode mode, const Trial& t, std::uint32_t seed, bool check = true) {
    const auto rate_bps = static_cast<std::size_t>(decode::mil110_bit_rate(mode.rate));
    const auto bits = random_bits(std::max<std::size_t>(rate_bps * 2, 150), seed);
    siggen::Mil110ModConfig mc;
    mc.rate = kRate;
    mc.amplitude = 0.5;
    mc.offset_hz = t.offset_hz;
    mc.lead_in_samples = 3000 + seed % 977;
    mc.tail_samples = 4000;
    auto audio = siggen::mil110_render_bits(mc, mode, bits);
    REQUIRE(audio.has_value());
    if (t.multipath) {
        *audio = two_path(*audio, kRate);
    }
    add_noise(*audio, t.snr_db, mc.amplitude, kRate, seed + 7);
    const auto msgs = run(*audio, kRate, t.chunk_seed);
    if (msgs.empty()) {
        return 1.0;
    }
    const auto& m = msgs.front();
    if (!check) {
        return m.mode.rate == mode.rate ? ber(bits, m.bits) : 1.0;
    }
    CHECK(m.mode.rate == mode.rate);
    CHECK(m.mode.interleave == mode.interleave);
    CHECK(m.eom);
    CHECK(std::abs(m.offset_hz - t.offset_hz) < 3.0);
    return ber(bits, m.bits);
}

}  // namespace

TEST_CASE("MIL-STD-188-110C serial tone tables and printed examples", "[mil110]") {
    // 5.3.2.3.7.2.1 note: a count of 23 is 5, 5, 7.
    CHECK(decode::mil110_count_symbols(23) == std::array<std::uint8_t, 3>{5, 5, 7});
    CHECK(decode::mil110_count_symbols(0) == std::array<std::uint8_t, 3>{4, 4, 4});

    // 5.3.2.3.1: the first eight EOM bits are 0100 1011.
    CHECK((decode::kMil110Eom >> 24) == 0x4BU);

    // Table XI.
    auto d = decode::mil110_designators({Mil110Rate::Bps2400, Mil110Interleave::Long});
    REQUIRE(d.has_value());
    CHECK(*d == std::array<std::uint8_t, 2>{4, 4});
    d = decode::mil110_designators({Mil110Rate::Bps75, Mil110Interleave::Short});
    REQUIRE(d.has_value());
    CHECK(*d == std::array<std::uint8_t, 2>{7, 5});
    CHECK_FALSE(decode::mil110_designators({Mil110Rate::Bps4800, Mil110Interleave::Long}));
    auto m = decode::mil110_mode_from_designators(5, 4, false);
    REQUIRE(m.has_value());
    CHECK(m->rate == Mil110Rate::Bps150);
    CHECK(m->interleave == Mil110Interleave::Long);
    CHECK_FALSE(decode::mil110_mode_from_designators(5, 6, false));  // reserved

    // 5.3.2.3.4: 2400 bit/s long, loaded down rows 0, 9, 18, 27 of column 0.
    auto l = decode::mil110_layout({Mil110Rate::Bps2400, Mil110Interleave::Long});
    REQUIRE(l.has_value());
    CHECK(l->rows == 40);
    CHECK(l->cols == 576);
    for (std::size_t k = 0; k < 4; ++k) {
        const auto c = decode::mil110_load_cell(*l, k);
        CHECK(c.row == 9 * k);
        CHECK(c.col == 0);
    }
    CHECK(decode::mil110_load_cell(*l, 40).col == 1);
    // 5.3.2.3.5: the second bit fetched is row 1 column 559, the third row 2
    // column 542.
    CHECK(decode::mil110_fetch_cell(*l, 1).row == 1);
    CHECK(decode::mil110_fetch_cell(*l, 1).col == 559);
    CHECK(decode::mil110_fetch_cell(*l, 2).row == 2);
    CHECK(decode::mil110_fetch_cell(*l, 2).col == 542);
    CHECK(decode::mil110_fetch_cell(*l, 40).col == 1);

    // Every interleaver of table VI is a permutation both ways.
    for (const auto r : {Mil110Rate::Bps75, Mil110Rate::Bps150, Mil110Rate::Bps600,
                         Mil110Rate::Bps1200, Mil110Rate::Bps2400}) {
        for (const auto il : {Mil110Interleave::Short, Mil110Interleave::Long}) {
            const auto lay = decode::mil110_layout({r, il});
            REQUIRE(lay.has_value());
            const std::size_t n = lay->rows * lay->cols;
            std::set<std::size_t> load;
            std::set<std::size_t> fetch;
            for (std::size_t k = 0; k < n; ++k) {
                const auto a = decode::mil110_load_cell(*lay, k);
                const auto b = decode::mil110_fetch_cell(*lay, k);
                load.insert(a.row * lay->cols + a.col);
                fetch.insert(b.row * lay->cols + b.col);
            }
            CHECK(load.size() == n);
            CHECK(fetch.size() == n);
        }
    }

    // Table VI against 5.3.2.2.1: one block lasts the interleave delay.
    for (const auto r : {Mil110Rate::Bps150, Mil110Rate::Bps1200, Mil110Rate::Bps2400}) {
        const auto lay = decode::mil110_layout({r, Mil110Interleave::Short});
        const auto coded_per_s = static_cast<std::size_t>(2400 / (lay->unknown + lay->known)) *
                                 lay->unknown * static_cast<std::size_t>(lay->bits_per_symbol);
        CHECK(lay->rows * lay->cols * 10 == coded_per_s * 6);
    }

    // 5.3.2.3.8.2: the preamble opens with channel symbol 0, so its first
    // 32 tribits are the sync scrambler itself, 7 first. Its length is
    // 0.6 s (short) or 4.8 s (long) at 2400 symbols/s.
    const std::vector<std::uint8_t> none;
    auto s = decode::mil110_transmit_symbols({Mil110Rate::Bps600, Mil110Interleave::Short}, none);
    REQUIRE(s.has_value());
    for (std::size_t k = 0; k < 32; ++k) {
        CHECK((*s)[k] == decode::kMil110SyncScrambler[k]);
    }

    // Figure 6: the data randomiser repeats every 160 symbols by reset;
    // check it is not degenerate and takes all eight values.
    const auto scr = decode::mil110_data_scrambler();
    std::set<int> values(scr.begin(), scr.end());
    CHECK(values.size() == 8);
}

TEST_CASE("MIL-STD-188-110C serial tone round trip at every rate", "[mil110]") {
    const std::array<Mil110Rate, 7> rates = {Mil110Rate::Bps75,  Mil110Rate::Bps150,
                                             Mil110Rate::Bps300, Mil110Rate::Bps600,
                                             Mil110Rate::Bps1200, Mil110Rate::Bps2400,
                                             Mil110Rate::Bps4800};
    std::uint32_t seed = 11;
    for (const auto r : rates) {
        const Mil110Mode mode{r, Mil110Interleave::Short};
        const double clean = round_trip(mode, {99.0, false, 0.0, 0}, seed++);
        // SNR in 3 kHz chosen per rate so the coded rates are tested near
        // where they start to work, not where they cannot fail; the hidden
        // sweep below shows the curves these points sit on.
        double snr = 0.0;
        if (r == Mil110Rate::Bps4800) {
            snr = 18.0;
        } else if (r == Mil110Rate::Bps2400) {
            snr = 9.0;
        } else if (r == Mil110Rate::Bps1200) {
            snr = 3.0;
        }
        const double awgn = round_trip(mode, {snr, false, 75.0, 0}, seed++);
        const double mp = round_trip(mode, {snr + 3.0, true, -75.0, 97}, seed++);
        WARN("MIL-110 " << rate_name(r) << " bit/s short: clean BER " << clean << ", AWGN "
                        << snr << " dB +75 Hz BER " << awgn << ", two-path " << snr + 3.0
                        << " dB -75 Hz BER " << mp);
        CHECK(clean == 0.0);
        CHECK(awgn < 0.01);
        CHECK(mp < 0.01);
    }
}

TEST_CASE("MIL-STD-188-110C serial tone long interleave", "[mil110]") {
    for (const auto r : {Mil110Rate::Bps75, Mil110Rate::Bps2400}) {
        const Mil110Mode mode{r, Mil110Interleave::Long};
        const double b = round_trip(mode, {r == Mil110Rate::Bps75 ? 0.0 : 12.0, true, 40.0, 5}, 300);
        WARN("MIL-110 " << rate_name(r) << " bit/s long, two-path: BER " << b);
        CHECK(b < 0.01);
    }
}

TEST_CASE("MIL-STD-188-110C serial tone output is independent of chunking", "[mil110]") {
    const Mil110Mode mode{Mil110Rate::Bps1200, Mil110Interleave::Short};
    const auto bits = random_bits(2000, 77);
    siggen::Mil110ModConfig mc;
    mc.rate = kRate;
    mc.offset_hz = 33.0;
    mc.lead_in_samples = 1234;
    auto audio = siggen::mil110_render_bits(mc, mode, bits);
    REQUIRE(audio.has_value());
    *audio = two_path(*audio, kRate);
    add_noise(*audio, 8.0, mc.amplitude, kRate, 5);
    const auto whole = run(*audio, kRate, 0);
    const auto split = run(*audio, kRate, 31);
    REQUIRE(whole.size() == 1);
    REQUIRE(split.size() == 1);
    CHECK(whole[0].bits == split[0].bits);
    CHECK(whole[0].position == split[0].position);
    CHECK(whole[0].mse == split[0].mse);
}

TEST_CASE("MIL-STD-188-110C serial tone at an 8000 S/s sound card rate", "[mil110]") {
    const Mil110Mode mode{Mil110Rate::Bps600, Mil110Interleave::Short};
    const auto bits = random_bits(1200, 9);
    siggen::Mil110ModConfig mc;
    mc.rate = 48000;
    mc.offset_hz = -20.0;
    mc.lead_in_samples = 6000;
    auto audio = siggen::mil110_render_bits(mc, mode, bits);
    REQUIRE(audio.has_value());
    // Band-limited below 3.1 kHz, so every sixth sample is an 8000 S/s copy.
    std::vector<float> slow;
    for (std::size_t i = 0; i < audio->size(); i += 6) {
        slow.push_back((*audio)[i]);
    }
    add_noise(slow, 10.0, mc.amplitude, 8000, 3);
    const auto msgs = run(slow, 8000, 0);
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].eom);
    const double b = ber(bits, msgs[0].bits);
    WARN("MIL-110 600 bit/s at 8000 S/s, 10 dB: BER " << b);
    CHECK(b < 0.01);
}

// Hidden: the BER against SNR in 3 kHz for every rate, AWGN and two-path.
// Run with "[.mil110-sweep]".
TEST_CASE("MIL-STD-188-110C serial tone BER sweep", "[.mil110-sweep]") {
    const std::array<Mil110Rate, 7> rates = {Mil110Rate::Bps75,  Mil110Rate::Bps150,
                                             Mil110Rate::Bps300, Mil110Rate::Bps600,
                                             Mil110Rate::Bps1200, Mil110Rate::Bps2400,
                                             Mil110Rate::Bps4800};
    for (const auto r : rates) {
        for (const bool mp : {false, true}) {
            std::string line;
            for (int snr = -9; snr <= 24; snr += 3) {
                double sum = 0.0;
                for (std::uint32_t s = 0; s < 3; ++s) {
                    sum += round_trip({r, Mil110Interleave::Short},
                                      {static_cast<double>(snr), mp, 50.0, 0}, 1000 + s, false);
                }
                line += " " + std::to_string(snr) + ":" + std::to_string(sum / 3.0);
            }
            WARN("MIL-110 " << rate_name(r) << (mp ? " two-path" : " AWGN") << line);
        }
    }
}

TEST_CASE("MIL-STD-188-110C serial tone tracks a sound card clock offset", "[mil110]") {
    // 60 s of 2400 bit/s with the receiver's clock 100 ppm off either way:
    // 14 symbols of slip by the end, which an untracked receiver loses
    // within the first few seconds.
    const Mil110Mode mode{Mil110Rate::Bps2400, Mil110Interleave::Short};
    const auto bits = random_bits(2400 * 60, 2024);
    siggen::Mil110ModConfig mc;
    mc.rate = 48000;
    mc.offset_hz = 10.0;
    mc.lead_in_samples = 12000;
    mc.tail_samples = 12000;
    auto audio = siggen::mil110_render_bits(mc, mode, bits);
    REQUIRE(audio.has_value());
    for (const double ppm : {100.0, -100.0}) {
        auto slow = sound_card(*audio, ppm);
        add_noise(slow, 12.0, mc.amplitude, kRate, 17);
        const auto msgs = run(slow, kRate, 0);
        REQUIRE(msgs.size() == 1);
        const double b = ber(bits, msgs[0].bits);
        WARN("MIL-110 2400 bit/s, 60 s, sound card " << ppm << " ppm, 12 dB: BER " << b
                                                     << ", clock read " << msgs[0].clock_ppm
                                                     << " ppm");
        CHECK(msgs[0].eom);
        CHECK(b < 1e-3);
        // A fast receiver clock makes each symbol longer in its samples.
        CHECK(std::abs(msgs[0].clock_ppm - ppm) < 20.0);
    }
}

TEST_CASE("MIL-STD-188-110C serial tone ignores noise", "[mil110]") {
    std::vector<float> audio(kRate * 10, 0.0F);
    add_noise(audio, 0.0, 0.5, kRate, 1);
    const auto msgs = run(audio, kRate, 0);
    CHECK(msgs.empty());
}
