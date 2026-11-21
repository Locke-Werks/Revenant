#include "doctest/doctest.h"

#include "lwdsp/lwdsp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <vector>

using namespace lwdsp;

namespace {

constexpr double kFs = 48000.0;

// Dominant positive frequency of a real signal, via a large windowed FFT and
// parabolic interpolation inside measure_passband.
double dominant_hz(const std::vector<float>& audio, double lo = 50.0, double hi = 5000.0) {
    std::vector<cf32> c(audio.begin(), audio.end());
    const auto r = measure_passband(c, kFs, 0.5 * (lo + hi), hi - lo, 32768);
    return r.peak_hz;
}

// Least-squares fit of a sinusoid at f; returns fitted power over residual.
double tone_snr_db(const std::vector<float>& x, double f) {
    double mean = 0.0;
    for (float v : x) mean += v;
    mean /= static_cast<double>(x.size());
    double cc = 0, ss = 0, cs = 0, xc = 0, xs = 0, var = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double a = 2.0 * std::numbers::pi * f * static_cast<double>(i) / kFs;
        const double c = std::cos(a), s = std::sin(a), v = x[i] - mean;
        cc += c * c;
        ss += s * s;
        cs += c * s;
        xc += v * c;
        xs += v * s;
        var += v * v;
    }
    const double det = cc * ss - cs * cs;
    const double A = (xc * ss - xs * cs) / det;
    const double B = (xs * cc - xc * cs) / det;
    double resid = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double a = 2.0 * std::numbers::pi * f * static_cast<double>(i) / kFs;
        const double e = (x[i] - mean) - A * std::cos(a) - B * std::sin(a);
        resid += e * e;
    }
    const double sig = var - resid;
    return 10.0 * std::log10(std::max(sig, 1e-30) / std::max(resid, 1e-30));
}

// Runs band through receiver for seconds, returns audio after settle_s.
std::vector<float> run_chain(Band& band, Receiver& rx, double seconds, double settle_s = 0.3) {
    const std::size_t block = 1024;
    const auto total = static_cast<std::size_t>(seconds * kFs);
    std::vector<cf32> iq(block);
    std::vector<float> audio(block), out;
    for (std::size_t done = 0; done < total; done += block) {
        band.render(iq);
        const std::size_t n = rx.process(iq, audio);
        out.insert(out.end(), audio.begin(), audio.begin() + static_cast<std::ptrdiff_t>(n));
    }
    const auto skip = static_cast<std::ptrdiff_t>(settle_s * kFs);
    return {out.begin() + skip, out.end()};
}

BandConfig quiet_band(std::uint64_t seed = 7) {
    BandConfig b;
    b.sample_rate = kFs;
    b.seed = seed;
    b.noise.noise_floor_db = -60.0f;
    b.noise.crash_rate_hz = 0.0f;
    b.noise.drift_db = 0.0f;
    return b;
}

} // namespace

TEST_CASE("AM modulate and demodulate a tone") {
    Band band(quiet_band());
    auto am = std::make_shared<AmModulator>(5000.0, kFs, 0.8f);
    am->set_feed(make_tone_feed(1000.0, kFs, 1.0f));
    band.add(am, {.level_db = 30.0f});

    ReceiverConfig rc;
    rc.mode = Mode::AM;
    rc.vfo_hz = 5000.0;
    rc.bandwidth_hz = 6000.0f;
    Receiver rx(rc);
    const auto audio = run_chain(band, rx, 1.5);

    CHECK(dominant_hz(audio) == doctest::Approx(1000.0).epsilon(0.005));
    const double snr = tone_snr_db(audio, 1000.0);
    MESSAGE("AM tone SNR " << snr << " dB");
    CHECK(snr > 15.0); // 6 kHz AM noise bandwidth and m = 0.8 cost about 10 dB vs carrier
}

TEST_CASE("USB and LSB modulate and demodulate a tone") {
    for (bool upper : {true, false}) {
        Band band(quiet_band());
        auto ssb = std::make_shared<SsbModulator>(3000.0, kFs, upper);
        ssb->set_feed(make_tone_feed(1000.0, kFs, 0.9f));
        band.add(ssb, {.level_db = 30.0f});

        ReceiverConfig rc;
        rc.mode = upper ? Mode::USB : Mode::LSB;
        rc.vfo_hz = 3000.0;
        Receiver rx(rc);
        const auto audio = run_chain(band, rx, 1.5);

        CHECK(dominant_hz(audio) == doctest::Approx(1000.0).epsilon(0.005));
        const double snr = tone_snr_db(audio, 1000.0);
        MESSAGE((upper ? "USB" : "LSB") << " tone SNR " << snr << " dB");
        CHECK(snr > 20.0);
    }
}

TEST_CASE("SSB in the wrong sideband is rejected") {
    Band band(quiet_band());
    auto ssb = std::make_shared<SsbModulator>(3000.0, kFs, true);
    ssb->set_feed(make_tone_feed(1000.0, kFs, 0.9f));
    band.add(ssb, {.level_db = 30.0f});
    ReceiverConfig rc;
    rc.mode = Mode::LSB;
    rc.vfo_hz = 3000.0;
    rc.agc.enabled = false;
    Receiver rx(rc);
    const auto audio = run_chain(band, rx, 1.0);
    CHECK(tone_snr_db(audio, 1000.0) < 0.0);
}

TEST_CASE("USB mistuned by 300 Hz shifts the tone") {
    Band band(quiet_band());
    auto ssb = std::make_shared<SsbModulator>(3000.0, kFs, true);
    ssb->set_feed(make_tone_feed(1000.0, kFs, 0.9f));
    band.add(ssb, {.level_db = 30.0f});

    ReceiverConfig rc;
    rc.mode = Mode::USB;
    rc.vfo_hz = 3300.0; // tuned 300 Hz high
    Receiver rx(rc);
    const auto audio = run_chain(band, rx, 1.5);

    const double f = dominant_hz(audio);
    MESSAGE("mistuned tone at " << f << " Hz");
    CHECK(f == doctest::Approx(700.0).epsilon(0.01));
    CHECK(tone_snr_db(audio, 700.0) > 20.0);

    // The clarifier pulls it back without moving the dial.
    Band band2(quiet_band());
    auto ssb2 = std::make_shared<SsbModulator>(3000.0, kFs, true);
    ssb2->set_feed(make_tone_feed(1000.0, kFs, 0.9f));
    band2.add(ssb2, {.level_db = 30.0f});
    rc.clarifier_hz = -300.0f;
    Receiver rx2(rc);
    CHECK(dominant_hz(run_chain(band2, rx2, 1.0)) == doctest::Approx(1000.0).epsilon(0.005));
}

TEST_CASE("AGC settles after a level step") {
    AgcConfig cfg;
    cfg.target = 0.3f;
    cfg.attack_ms = 2.0f;
    cfg.decay_ms = 300.0f;
    cfg.hang_ms = 100.0f;
    Agc agc(cfg, kFs);
    Nco tone(800.0, kFs);

    auto run = [&](float amp, double secs) {
        std::vector<float> out(static_cast<std::size_t>(secs * kFs));
        for (float& v : out) v = agc.process(amp * tone.step().real());
        return out;
    };
    auto peak = [](const std::vector<float>& v, std::size_t from, std::size_t to) {
        float p = 0.0f;
        for (std::size_t i = from; i < to; ++i) p = std::max(p, std::fabs(v[i]));
        return p;
    };

    // Weak signal: gain rises until the output reaches target.
    const auto weak = run(0.005f, 2.0);
    CHECK(peak(weak, weak.size() - 4800, weak.size()) == doctest::Approx(0.3).epsilon(0.15));

    // 40 dB step up: the lookahead plus fast attack keeps the overshoot
    // bounded and the level is back at target within 50 ms.
    const auto loud = run(0.5f, 1.0);
    CHECK(peak(loud, 0, loud.size()) < 0.3f * 2.0f);
    const float settled_a = peak(loud, 2400, 4800);
    const float settled_b = peak(loud, loud.size() - 4800, loud.size());
    CHECK(settled_a == doctest::Approx(0.3).epsilon(0.15));
    CHECK(settled_b == doctest::Approx(settled_a).epsilon(0.02));
}

TEST_CASE("Band noise is bit-identical for a fixed seed") {
    auto make = [](std::uint64_t seed) {
        BandConfig b;
        b.sample_rate = kFs;
        b.seed = seed;
        b.noise.crash_rate_hz = 5.0f;
        b.noise.impulse_rate_hz = 20.0f;
        b.noise.tilt_db = 6.0f;
        auto band = std::make_unique<Band>(b);
        auto cw = std::make_shared<CwModulator>(-2000.0, kFs);
        cw->set_text("CQ CQ DE UA1ABC", 22.0f);
        band->add(cw, {.level_db = 15.0f, .qsb = {.depth_db = 12.0f, .rate_hz = 0.5f}});
        return band;
    };
    auto a = make(42), b = make(42), c = make(43);
    std::vector<cf32> ba(4096), bb(4096), bc(4096);
    bool same_ab = true, same_ac = true;
    for (int blk = 0; blk < 30; ++blk) {
        a->render(ba);
        b->render(bb);
        c->render(bc);
        same_ab = same_ab && std::memcmp(ba.data(), bb.data(), ba.size() * sizeof(cf32)) == 0;
        same_ac = same_ac && std::memcmp(ba.data(), bc.data(), ba.size() * sizeof(cf32)) == 0;
    }
    CHECK(same_ab);
    CHECK_FALSE(same_ac);

    // reseed rewinds the noise to the same stream.
    BandConfig nb;
    nb.seed = 9;
    Band n1(nb);
    std::vector<cf32> first(2048), again(2048);
    n1.render_noise(first);
    n1.reseed(9);
    n1.render_noise(again);
    CHECK(std::memcmp(first.data(), again.data(), first.size() * sizeof(cf32)) == 0);
}

TEST_CASE("Notch kills a tone and passes its neighbours") {
    NotchConfig nc;
    nc.enabled = true;
    nc.freq_hz = 1000.0f;
    nc.width_hz = 60.0f;
    auto rms_through = [&](double f) {
        Notch notch(nc, kFs);
        Nco osc(f, kFs);
        std::vector<float> v(static_cast<std::size_t>(kFs));
        for (float& s : v) s = osc.step().real();
        notch.process(v);
        double acc = 0;
        for (std::size_t i = v.size() / 2; i < v.size(); ++i) acc += v[i] * v[i];
        return 10.0 * std::log10(acc / (v.size() / 2) / 0.5);
    };
    const double at = rms_through(1000.0);
    MESSAGE("notch depth at centre " << at << " dB");
    CHECK(at < -60.0);
    CHECK(std::fabs(rms_through(2000.0)) < 1.0);
    CHECK(std::fabs(rms_through(500.0)) < 1.0);

    // Through the whole receiver: a USB tone at 1000 Hz audio disappears.
    auto chain_power = [&](bool notch_on) {
        Band band(quiet_band());
        auto ssb = std::make_shared<SsbModulator>(3000.0, kFs, true);
        ssb->set_feed(make_tone_feed(1000.0, kFs, 0.9f));
        band.add(ssb, {.level_db = 40.0f});
        ReceiverConfig rc;
        rc.vfo_hz = 3000.0;
        rc.agc.enabled = false;
        rc.notch = nc;
        rc.notch.enabled = notch_on;
        Receiver rx(rc);
        const auto a = run_chain(band, rx, 1.0);
        double acc = 0;
        for (float s : a) acc += s * s;
        return 10.0 * std::log10(acc / a.size());
    };
    const double drop = chain_power(false) - chain_power(true);
    MESSAGE("receiver notch drop " << drop << " dB");
    CHECK(drop > 30.0);
}

TEST_CASE("Spectrum reads a full-scale tone at 0 dBFS in the right bin") {
    Spectrum spec({.size = 4096});
    Nco osc(3000.0, kFs);
    std::vector<cf32> iq(4096);
    for (auto& s : iq) s = osc.step();
    const auto db = spec.compute(iq);
    const auto it = std::max_element(db.begin(), db.end());
    const int bin = static_cast<int>(it - db.begin());
    CHECK(spec.bin_hz(bin, kFs) == doctest::Approx(3000.0).epsilon(0.005));
    CHECK(*it == doctest::Approx(0.0).epsilon(0.5));
}

TEST_CASE("Passband SNR tracks the requested level") {
    Band band(quiet_band(3));
    auto am = std::make_shared<AmModulator>(4000.0, kFs, 0.0f); // bare carrier
    band.add(am, {.level_db = 20.0f});
    std::vector<cf32> iq(16384);
    band.render(iq);
    // Carrier power over noise in a 3 kHz reference bandwidth should read
    // close to the 20 dB the band was asked for.
    const auto r = measure_passband(iq, kFs, 4000.0, 3000.0, 16384);
    MESSAGE("passband SNR " << r.snr_db << " dB, peak " << r.peak_hz << " Hz");
    CHECK(r.snr_db == doctest::Approx(20.0).epsilon(0.1));
    CHECK(r.peak_hz == doctest::Approx(4000.0).epsilon(0.001));
}

TEST_CASE("Tune quality falls with offset and rises with SNR") {
    TuneInput in;
    in.signal_mode = Mode::USB;
    in.signal_offset_hz = 3000.0;
    in.rx_mode = Mode::USB;
    in.rx_vfo_hz = 3000.0;
    in.passband_snr_db = 25.0f;
    const auto good = evaluate_tune(in);
    in.rx_vfo_hz = 3300.0;
    const auto off = evaluate_tune(in);
    CHECK(good.quality > 0.95f);
    CHECK(off.offset_error_hz == doctest::Approx(300.0f));
    CHECK(off.quality < 0.05f);
    in.rx_vfo_hz = 3000.0;
    in.passband_snr_db = 3.0f;
    CHECK(evaluate_tune(in).quality < good.quality);
    CHECK(word_reveal(good, 10.0f) > word_reveal(good, 30.0f));
}

TEST_CASE("CW timing follows ITU spacing") {
    CHECK(CwModulator::text_to_timing("CQ") == "111010111010001110111010111");
    CHECK(CwModulator::text_to_timing("E E") == "100000001");
    CwModulator cw(0.0, kFs);
    cw.set_text("E", 20.0f); // one 60 ms dot
    std::vector<cf32> out(static_cast<std::size_t>(0.1 * kFs));
    cw.render(out);
    CHECK(std::abs(out[static_cast<std::size_t>(0.03 * kFs)]) == doctest::Approx(1.0f));
    CHECK(std::abs(out[static_cast<std::size_t>(0.09 * kFs)]) == doctest::Approx(0.0f));
    CHECK(cw.finished());
}

TEST_CASE("CW product detector produces the BFO pitch") {
    Band band(quiet_band());
    auto cw = std::make_shared<CwModulator>(-1500.0, kFs);
    cw->set_timing("1", 10.0); // key down for 10 s
    band.add(cw, {.level_db = 30.0f});
    ReceiverConfig rc;
    rc.mode = Mode::CW;
    rc.vfo_hz = -1500.0;
    rc.bandwidth_hz = 500.0f;
    rc.bfo_hz = 650.0f;
    Receiver rx(rc);
    CHECK(dominant_hz(run_chain(band, rx, 1.0)) == doctest::Approx(650.0).epsilon(0.01));
}

TEST_CASE("FSK encodes ITA2 frames") {
    const auto sym = FskModulator::ita2_encode("RY");
    // LTRS + R + Y, 7 symbols each (start, 5 data, stop).
    REQUIRE(sym.size() == 21);
    // R = 01010 sent LSB first: 0 1 0 1 0.
    CHECK(sym[7] == 0);
    CHECK(sym[8] == 0);
    CHECK(sym[9] == 1);
    CHECK(sym[10] == 0);
    CHECK(sym[11] == 1);
    CHECK(sym[12] == 0);
    CHECK(sym[13] == 2);
}

TEST_CASE("Noise blanker removes impulses") {
    BandConfig b = quiet_band(5);
    b.noise.impulse_rate_hz = 200.0f;
    b.noise.impulse_level_db = 40.0f;
    Band band(b);
    std::vector<cf32> iq(48000);
    band.render(iq);
    auto peak = [](const std::vector<cf32>& v) {
        float p = 0;
        for (std::size_t i = 64; i < v.size(); ++i) p = std::max(p, std::abs(v[i]));
        return p;
    };
    const float before = peak(iq);
    NoiseBlankerConfig nc;
    nc.enabled = true;
    NoiseBlanker nb(nc, kFs);
    nb.process(iq);
    const float after = peak(iq);
    MESSAGE("impulse peak " << lin_to_db(before) << " -> " << lin_to_db(after) << " dBFS, blanked "
                            << nb.blanked_samples());
    CHECK(after < before * 0.1f);
    CHECK(nb.blanked_samples() < 48000 / 20);
}

TEST_CASE("SpscRing moves audio and counts underruns") {
    auto ring = SpscRing<float>::create(1000);
    REQUIRE(ring);
    CHECK(ring->capacity() == 1024);
    std::vector<float> in(600, 1.0f), out(800, 5.0f);
    CHECK(ring->write(in) == 600);
    CHECK(ring->read_or_fill(out) == 600);
    CHECK(out[599] == 1.0f);
    CHECK(out[700] == 0.0f);
    CHECK(ring->underrun_events() == 1);
}
