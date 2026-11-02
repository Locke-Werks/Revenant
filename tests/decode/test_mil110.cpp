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

// ---------------------------------------------------------------------------
// Appendix C, 3200 to 12800 bit/s
// ---------------------------------------------------------------------------

namespace {

using decode::Mil110cInterleave;
using decode::Mil110cMode;
using decode::Mil110cRate;

constexpr std::array<Mil110cRate, 6> kCRates = {Mil110cRate::Bps3200, Mil110cRate::Bps4800,
                                                Mil110cRate::Bps6400, Mil110cRate::Bps8000,
                                                Mil110cRate::Bps9600, Mil110cRate::Bps12800};

std::vector<decode::Mil110cMessage> run_c(const std::vector<float>& audio, dsp::SampleRate rate,
                                          std::size_t chunk_seed) {
    decode::Mil110cConfig cfg;
    cfg.rate = rate;
    auto dec = decode::Mil110cDecoder::create(cfg);
    REQUIRE(dec.has_value());
    std::vector<decode::Mil110cMessage> out;
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

// The interleaver for a rate tested at its own short setting; 12800 has only
// the one frame.
Mil110cMode c_mode(Mil110cRate r, Mil110cInterleave il = Mil110cInterleave::Short) {
    return {r, r == Mil110cRate::Bps12800 ? Mil110cInterleave::UltraShort : il};
}

// Table C-XVII: AWGN SNR for a BER of 1e-5 with the 72-frame interleaver.
double c_required_snr(Mil110cRate r) {
    switch (r) {
        case Mil110cRate::Bps3200: return 9.0;
        case Mil110cRate::Bps4800: return 13.0;
        case Mil110cRate::Bps6400: return 16.0;
        case Mil110cRate::Bps8000: return 19.0;
        case Mil110cRate::Bps9600: return 21.0;
        case Mil110cRate::Bps12800: return 27.0;
    }
    return 30.0;
}

// About `seconds` of user data through the channel; the BER, or 1 when the
// message is not found.
double round_trip_c(Mil110cMode mode, const Trial& t, std::uint32_t seed, bool check = true,
                    double seconds = 2.0) {
    const auto rate_bps = static_cast<double>(decode::mil110c_bit_rate(mode.rate));
    const auto bits = random_bits(static_cast<std::size_t>(rate_bps * seconds), seed);
    siggen::Mil110ModConfig mc;
    mc.rate = kRate;
    mc.amplitude = 0.5;
    mc.offset_hz = t.offset_hz;
    mc.lead_in_samples = 3000 + seed % 977;
    mc.tail_samples = 4000;
    auto audio = siggen::mil110c_render_bits(mc, mode, bits);
    REQUIRE(audio.has_value());
    if (t.multipath) {
        *audio = two_path(*audio, kRate);
    }
    add_noise(*audio, t.snr_db, mc.amplitude, kRate, seed + 7);
    const auto msgs = run_c(*audio, kRate, t.chunk_seed);
    if (msgs.empty()) {
        return 1.0;
    }
    const auto& m = msgs.front();
    if (!check) {
        return m.mode.rate == mode.rate ? ber(bits, m.bits) : 1.0;
    }
    CHECK(msgs.size() == 1);
    CHECK(m.mode.rate == mode.rate);
    CHECK(m.mode.interleave == mode.interleave);
    CHECK(m.eom);
    CHECK(std::abs(m.offset_hz - t.offset_hz) < 3.0);
    return ber(bits, m.bits);
}

const char* c_rate_name(Mil110cRate r) {
    switch (r) {
        case Mil110cRate::Bps3200: return "3200";
        case Mil110cRate::Bps4800: return "4800";
        case Mil110cRate::Bps6400: return "6400";
        case Mil110cRate::Bps8000: return "8000";
        case Mil110cRate::Bps9600: return "9600";
        case Mil110cRate::Bps12800: return "12800";
    }
    return "?";
}

}  // namespace

TEST_CASE("MIL-STD-188-110C Appendix C tables and printed examples", "[mil110]") {
    // Table C-IX as printed, rows 3200 to 9600, columns 1 to 72 frames.
    const std::array<std::array<std::array<std::uint8_t, 3>, 6>, 5> printed = {{
        {{{0, 0, 4}, {0, 2, 6}, {0, 2, 4}, {2, 0, 6}, {2, 0, 4}, {2, 2, 6}}},
        {{{0, 6, 2}, {0, 4, 0}, {0, 4, 2}, {2, 6, 0}, {2, 6, 2}, {2, 4, 0}}},
        {{{0, 6, 4}, {0, 4, 6}, {0, 4, 4}, {2, 6, 6}, {2, 6, 4}, {2, 4, 6}}},
        {{{6, 0, 2}, {6, 2, 0}, {6, 2, 2}, {4, 0, 0}, {4, 0, 2}, {4, 2, 0}}},
        {{{6, 0, 4}, {6, 2, 6}, {6, 2, 4}, {4, 0, 6}, {4, 0, 4}, {4, 2, 6}}},
    }};
    for (std::size_t r = 0; r < 5; ++r) {
        for (std::size_t l = 0; l < 6; ++l) {
            const Mil110cMode m{static_cast<Mil110cRate>(r), static_cast<Mil110cInterleave>(l)};
            const auto d = decode::mil110c_designators(m);
            REQUIRE(d.has_value());
            CHECK(*d == printed[r][l]);
            const auto back = decode::mil110c_mode_from_designators(*d);
            REQUIRE(back.has_value());
            CHECK(back->rate == m.rate);
            CHECK(back->interleave == m.interleave);
        }
    }
    const auto d12800 = decode::mil110c_designators({Mil110cRate::Bps12800,
                                                     Mil110cInterleave::UltraShort});
    REQUIRE(d12800.has_value());
    CHECK(*d12800 == std::array<std::uint8_t, 3>{6, 6, 2});
    CHECK_FALSE(decode::mil110c_designators({Mil110cRate::Bps12800, Mil110cInterleave::Short}));

    // Table C-XII as printed: + is 0. 3200 with 1 frame, 9600 with 72,
    // 12800 with 1. Mini-probes 9 to 14 carry them.
    auto six = [](Mil110cMode m) {
        const auto s = decode::mil110c_probe_signs(m);
        std::string out;
        for (std::size_t k = 8; k < 14; ++k) {
            out += s[k] ? '-' : '+';
        }
        return out;
    };
    CHECK(six({Mil110cRate::Bps3200, Mil110cInterleave::UltraShort}) == "++-++-");
    CHECK(six({Mil110cRate::Bps6400, Mil110cInterleave::Short}) == "+--+--");
    CHECK(six({Mil110cRate::Bps6400, Mil110cInterleave::Medium}) == "+---++");
    CHECK(six({Mil110cRate::Bps9600, Mil110cInterleave::VeryLong}) == "-+---+");
    CHECK(six({Mil110cRate::Bps12800, Mil110cInterleave::UltraShort}) == "--+++-");
    // C.5.2.2: seven "-", a "+", ..., the set count (table C-XIII), a "+".
    const auto signs = decode::mil110c_probe_signs({Mil110cRate::Bps4800, Mil110cInterleave::Long});
    for (std::size_t set = 0; set < 4; ++set) {
        for (std::size_t k = 0; k < 7; ++k) {
            CHECK(signs[18 * set + k]);
        }
        CHECK_FALSE(signs[18 * set + 7]);
        CHECK_FALSE(signs[18 * set + 17]);
    }
    CHECK_FALSE(signs[14]);  // set 1: + + -
    CHECK_FALSE(signs[15]);
    CHECK(signs[16]);
    CHECK(signs[68]);  // set 4: - + +
    CHECK_FALSE(signs[69]);
    CHECK_FALSE(signs[70]);

    // C.5.3.3.2: 3200 bit/s, one frame: the first eight load locations.
    const auto l3200 = decode::mil110c_layout({Mil110cRate::Bps3200, Mil110cInterleave::UltraShort});
    REQUIRE(l3200.has_value());
    CHECK(l3200->interleaver == 512);
    CHECK(l3200->increment == 97);
    const std::array<std::size_t, 8> load = {0, 97, 194, 291, 388, 485, 70, 167};
    for (std::size_t n = 0; n < load.size(); ++n) {
        CHECK((n * l3200->increment) % l3200->interleaver == load[n]);
    }
    // Table C-XIV against C-XV, and every interleaver a permutation.
    CHECK(l3200->input_bits == 384);
    const auto l9600 = decode::mil110c_layout({Mil110cRate::Bps9600, Mil110cInterleave::VeryLong});
    REQUIRE(l9600.has_value());
    CHECK(l9600->input_bits == 82944);
    CHECK(l9600->interleaver == 110592);
    for (std::size_t r = 0; r < 5; ++r) {
        for (std::size_t c = 0; c < 6; ++c) {
            const auto lay = decode::mil110c_layout(
                {static_cast<Mil110cRate>(r), static_cast<Mil110cInterleave>(c)});
            REQUIRE(lay.has_value());
            CHECK(lay->interleaver == lay->frames * 256 *
                                          static_cast<std::size_t>(lay->bits_per_symbol));
            std::vector<bool> seen(lay->interleaver, false);
            std::size_t hit = 0;
            for (std::size_t n = 0; n < lay->interleaver; ++n) {
                const std::size_t at = (n * lay->increment) % lay->interleaver;
                hit += seen[at] ? 0 : 1;
                seen[at] = true;
            }
            CHECK(hit == lay->interleaver);
        }
    }

    // C.5.1.3: the generator starts at 00000001 and the first symbol takes
    // its bits from that; x^9 + x^4 + 1 repeats every 511 bits.
    CHECK(decode::mil110c_scrambler(3)[0] == 1);
    CHECK(decode::mil110c_scrambler(6)[0] == 1);
    {
        const auto one = decode::mil110c_scrambler(1);
        const auto three = decode::mil110c_scrambler(3);
        // One bit at a time is the register's rightmost cell; three at a
        // time is the same sequence read three cells at once.
        std::array<std::uint8_t, 256> bit{};
        unsigned s = 1;
        for (auto& b : bit) {
            b = static_cast<std::uint8_t>(s & 1U);
            const unsigned fb = (s ^ (s >> 4)) & 1U;
            s = (s >> 1) | (fb << 8);
        }
        CHECK(one == bit);
        unsigned period = 0;
        s = 1;
        do {
            const unsigned fb = (s ^ (s >> 4)) & 1U;
            s = (s >> 1) | (fb << 8);
            ++period;
        } while (s != 1 && period < 1000);
        CHECK(period == 511);
        CHECK(three[1] == (bit[3] | (bit[4] << 1) | (bit[5] << 2)));
    }

    // C.5.1.2.2: 0111 is 16QAM symbol 7; every constellation has unit peak
    // and distinct points.
    const auto q16 = decode::mil110c_constellation(Mil110cRate::Bps6400);
    CHECK(std::abs(q16[7].real() + 0.258819F) < 1e-6F);
    CHECK(std::abs(q16[7].imag() - 0.258819F) < 1e-6F);
    for (const auto r : kCRates) {
        const auto pts = decode::mil110c_constellation(r);
        float peak = 0.0F;
        float closest = 10.0F;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            peak = std::max(peak, std::abs(pts[i]));
            for (std::size_t j = i + 1; j < pts.size(); ++j) {
                closest = std::min(closest, std::abs(pts[i] - pts[j]));
            }
        }
        CHECK(std::abs(peak - 1.0F) < 1e-5F);
        CHECK(closest > 0.1F);
    }

    // Tables C-VIII and C-XI: 287 symbols, the last 103 the reinserted
    // preamble, which opens with the "+" mini-probe and closes with "-".
    const auto tail = decode::mil110c_reinserted({0, 0, 4});
    CHECK(tail[31] == 2);
    CHECK(tail[32 + 39] == 6);
    for (std::size_t k = 0; k < 31; ++k) {
        CHECK(tail[k] == decode::kMil110cMiniProbe[k]);
        CHECK(tail[72 + k] == (decode::kMil110cMiniProbe[k] + 4) % 8);
    }
    // D2 = 4 on the Barker code 0101001100000.
    CHECK(tail[32 + 26] == 4);
    CHECK(tail[32 + 27] == 0);

    // C.5.3.2: the tail-biting code. Rotating the input by three bits rotates
    // the punctured block by four, and the decoder inverts it.
    const auto in = random_bits(384, 5);
    const auto code = decode::mil110c_encode(in);
    REQUIRE(code.size() == 512);
    std::vector<std::uint8_t> rot(in.begin() + 3, in.end());
    rot.insert(rot.end(), in.begin(), in.begin() + 3);
    const auto code_rot = decode::mil110c_encode(rot);
    for (std::size_t i = 0; i < code.size(); ++i) {
        CHECK(code_rot[i] == code[(i + 4) % code.size()]);
    }
    std::vector<float> soft(code.size());
    for (std::size_t i = 0; i < code.size(); ++i) {
        soft[i] = code[i] != 0 ? -1.0F : 1.0F;
    }
    soft[10] = -soft[10];  // an error the code must correct
    soft[200] = -soft[200];
    CHECK(decode::mil110c_decode(soft) == in);

    // Frame structure (figure C-6): preamble, then 287 a frame, 72 more
    // after every 72nd frame that is not the last.
    const auto bits = random_bits(100, 1);
    auto s = decode::mil110c_transmit_symbols({Mil110cRate::Bps3200, Mil110cInterleave::VeryShort},
                                              bits, 2);
    REQUIRE(s.has_value());
    CHECK(s->size() == 2 * 184 + 287 + 3 * 287);
    CHECK(std::abs((*s)[0] - std::conj((*s)[2 * 184])) < 1e-6F);
}

TEST_CASE("MIL-STD-188-110C Appendix C round trip at every rate", "[mil110]") {
    std::uint32_t seed = 41;
    for (const auto r : kCRates) {
        const auto mode = c_mode(r);
        const double snr = c_required_snr(r);
        const double clean = round_trip_c(mode, {99.0, false, 0.0, 0}, seed++);
        const double awgn = round_trip_c(mode, {snr, false, 75.0, 0}, seed++);
        const double mp = round_trip_c(mode, {snr + 3.0, true, -75.0, 97}, seed++);
        WARN("MIL-110C " << c_rate_name(r) << " bit/s: clean BER " << clean << ", AWGN " << snr
                         << " dB +75 Hz BER " << awgn << ", two-path " << snr + 3.0
                         << " dB -75 Hz BER " << mp);
        CHECK(clean == 0.0);
        CHECK(awgn < 0.01);
        CHECK(mp < 0.01);
    }
}

TEST_CASE("MIL-STD-188-110C Appendix C crosses a reinserted preamble", "[mil110]") {
    // Twelve seconds runs past the first 72-frame group, so the decoder has
    // to step over the reinserted preamble, with the longest interleaver.
    const double b = round_trip_c({Mil110cRate::Bps6400, Mil110cInterleave::Long},
                                  {22.0, true, 20.0, 13}, 77, true, 12.0);
    WARN("MIL-110C 6400 bit/s long, 12 s, two-path 22 dB: BER " << b);
    CHECK(b < 1e-3);
}

TEST_CASE("MIL-STD-188-110C Appendix C output is independent of chunking", "[mil110]") {
    const Mil110cMode mode{Mil110cRate::Bps8000, Mil110cInterleave::VeryShort};
    const auto bits = random_bits(16000, 78);
    siggen::Mil110ModConfig mc;
    mc.rate = kRate;
    mc.offset_hz = -31.0;
    mc.lead_in_samples = 2345;
    auto audio = siggen::mil110c_render_bits(mc, mode, bits, 1);
    REQUIRE(audio.has_value());
    *audio = two_path(*audio, kRate);
    add_noise(*audio, 22.0, mc.amplitude, kRate, 6);
    const auto whole = run_c(*audio, kRate, 0);
    const auto split = run_c(*audio, kRate, 29);
    REQUIRE(whole.size() == 1);
    REQUIRE(split.size() == 1);
    CHECK(whole[0].bits == split[0].bits);
    CHECK(whole[0].position == split[0].position);
    CHECK(whole[0].mse == split[0].mse);
    CHECK(ber(bits, whole[0].bits) < 1e-3);
}

TEST_CASE("MIL-STD-188-110C Appendix C tracks a sound card clock offset", "[mil110]") {
    for (const auto r : {Mil110cRate::Bps3200, Mil110cRate::Bps9600}) {
        const Mil110cMode mode{r, Mil110cInterleave::Long};
        const auto bits =
            random_bits(static_cast<std::size_t>(decode::mil110c_bit_rate(r)) * 60, 4040);
        siggen::Mil110ModConfig mc;
        mc.rate = 48000;
        mc.offset_hz = -12.0;
        mc.lead_in_samples = 12000;
        mc.tail_samples = 12000;
        auto audio = siggen::mil110c_render_bits(mc, mode, bits);
        REQUIRE(audio.has_value());
        for (const double ppm : {100.0, -100.0}) {
            auto slow = sound_card(*audio, ppm);
            add_noise(slow, c_required_snr(r) + 3.0, mc.amplitude, kRate, 19);
            const auto msgs = run_c(slow, kRate, 0);
            REQUIRE(msgs.size() == 1);
            const double b = ber(bits, msgs[0].bits);
            WARN("MIL-110C " << c_rate_name(r) << " bit/s, 60 s, sound card " << ppm << " ppm, "
                             << c_required_snr(r) + 3.0 << " dB: BER " << b << ", clock read "
                             << msgs[0].clock_ppm << " ppm");
            CHECK(msgs[0].eom);
            CHECK(b < 1e-3);
            CHECK(std::abs(msgs[0].clock_ppm - ppm) < 20.0);
        }
    }
}

TEST_CASE("MIL-STD-188-110C Appendix C ignores noise and the 5.3.2 waveform", "[mil110]") {
    std::vector<float> audio(kRate * 10, 0.0F);
    add_noise(audio, 0.0, 0.5, kRate, 2);
    CHECK(run_c(audio, kRate, 0).empty());
    siggen::Mil110ModConfig mc;
    mc.rate = kRate;
    auto other = siggen::mil110_render_bits(mc, {Mil110Rate::Bps2400, Mil110Interleave::Short},
                                            random_bits(4800, 3));
    REQUIRE(other.has_value());
    CHECK(run_c(*other, kRate, 0).empty());
}

// Hidden: BER against SNR in 3 kHz for every rate, AWGN and two-path, short
// interleaver. Run with "[.mil110c-sweep]".
TEST_CASE("MIL-STD-188-110C Appendix C BER sweep", "[.mil110c-sweep]") {
    for (const auto r : kCRates) {
        for (const bool mp : {false, true}) {
            std::string line;
            for (int snr = 3; snr <= 33; snr += 3) {
                double sum = 0.0;
                for (std::uint32_t s = 0; s < 3; ++s) {
                    sum += round_trip_c(c_mode(r), {static_cast<double>(snr), mp, 50.0, 0},
                                        2000 + s, false);
                }
                line += " " + std::to_string(snr) + ":" + std::to_string(sum / 3.0);
            }
            WARN("MIL-110C " << c_rate_name(r) << (mp ? " two-path" : " AWGN") << line);
        }
    }
}
