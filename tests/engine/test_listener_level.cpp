// The receiver AGC's arithmetic, core/engine/listener_level.h, on its own.
//
// No GPU. What can be wrong in a peak follower is which coefficient moves it,
// what it does across a block edge, and what the gate, the switch and a
// retune do to its state, and all of that is host arithmetic over a span.
// tests/engine/test_engine_agc.cpp runs the same stage on demodulated audio
// through a real engine.
//
// THE SIGNAL MOST OF THESE USE IS A SQUARE WAVE, on purpose. Its magnitude is
// the same on every frame, so the follower is always on one side of it and
// moves by exactly one coefficient: a step in level then moves the envelope
// by 1 - 1/e of the way in exactly one time constant, which is the figure the
// operator set and the only reading of "attack" and "decay" that can be
// checked to a part in a million. On a sinusoid the attack acts near the
// crests only; the engine test measures that case.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/engine/listener_level.h"
#include "core/engine/vrx.h"

using namespace revenant;
using engine::AgcSettings;
using engine::Demod;
using engine::kAgcCeilingDb;
using engine::kHeardTarget;
using engine::Levelling;
using engine::ListenerAgc;

namespace {

constexpr dsp::SampleRate kRate = 48'000;

// Two levels 20 dB apart that a float holds exactly, 2^-10 and ten times it,
// so an envelope that has settled on one reads it to the last bit.
constexpr double kExactLow = 0.0009765625;
constexpr double kExactHigh = 0.009765625;

[[nodiscard]] std::vector<float> square(std::size_t frames, double amplitude) {
    std::vector<float> out(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        out[i] = static_cast<float>(((i / 24U) % 2U == 0U) ? amplitude : -amplitude);
    }
    return out;
}

[[nodiscard]] std::vector<float> sine(std::size_t frames, double amplitude, double hz,
                                      std::size_t from = 0) {
    std::vector<float> out(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i + from) / static_cast<double>(kRate);
        out[i] = static_cast<float>(amplitude * std::sin(2.0 * 3.14159265358979323846 * hz * t));
    }
    return out;
}

// Runs `in` through `agc` in chunks of `chunk` frames and returns the output.
[[nodiscard]] std::vector<float> run(ListenerAgc& agc, std::span<const float> in,
                                     std::size_t chunk, const AgcSettings& settings,
                                     bool open = true, std::uint32_t channels = 1) {
    std::vector<float> out(in.size());
    const std::size_t step = chunk * channels;
    for (std::size_t at = 0; at < in.size(); at += step) {
        const std::size_t count = std::min(step, in.size() - at);
        agc.process(in.subspan(at, count), std::span<float>(out).subspan(at, count), channels,
                    kRate, open, settings);
    }
    return out;
}

[[nodiscard]] double peak(std::span<const float> samples) {
    double out = 0.0;
    for (const float s : samples) {
        out = std::max(out, std::abs(static_cast<double>(s)));
    }
    return out;
}

[[nodiscard]] double db(double ratio) { return 20.0 * std::log10(ratio); }

}  // namespace

TEST_CASE("the AGC runs on the amplitude-detected modes, FM takes a fixed gain, taps nothing",
          "[engine][agc]") {
    for (const Demod mode : {Demod::Am, Demod::Usb, Demod::Lsb, Demod::Dsb, Demod::Cw}) {
        INFO(engine::demod_name(mode));
        CHECK(engine::levelling_for(mode) == Levelling::Agc);
    }
    for (const Demod mode : {Demod::Nfm, Demod::Wfm}) {
        INFO(engine::demod_name(mode));
        CHECK(engine::levelling_for(mode) == Levelling::Fixed);
    }
    for (const Demod mode : {Demod::Raw, Demod::P25p1, Demod::Dstar, Demod::Tetra, Demod::Dmr}) {
        INFO(engine::demod_name(mode));
        CHECK(engine::levelling_for(mode) == Levelling::None);
    }
    CHECK(static_cast<double>(engine::kFmHeardGain) == kHeardTarget);
}

TEST_CASE("a steady level is held at the target whatever it came in at", "[engine][agc]") {
    // REJECTS: no AGC, which hands a -90 dBFS square out at -90 dBFS, and a
    // gain that is not the target over the envelope.
    for (const double amplitude : {1.0, kExactHigh, 3.162e-4}) {
        INFO("input " << db(amplitude) << " dBFS");
        ListenerAgc agc;
        const auto in = square(kRate, amplitude);
        const auto out = run(agc, in, 328, AgcSettings{});
        const double settled = peak(std::span<const float>(out).last(kRate / 4));
        CHECK(std::abs(settled - kHeardTarget) < 1e-6);
    }
}

TEST_CASE("a step up moves the envelope 1 - 1/e of the way in exactly the attack time",
          "[engine][agc]") {
    for (const double attack_ms : {10.0, 40.0}) {
        INFO("attack " << attack_ms << " ms");
        const AgcSettings settings{true, attack_ms, 500.0};
        ListenerAgc agc;
        constexpr double kLow = kExactLow;
        constexpr double kHigh = kExactHigh;
        static_cast<void>(run(agc, square(kRate / 2, kLow), 328, settings));
        REQUIRE(std::abs(agc.envelope() - kLow) < 1e-12);

        const auto frames = static_cast<std::size_t>(attack_ms / 1000.0 * kRate);
        static_cast<void>(run(agc, square(frames, kHigh), frames, settings));
        const double fraction = (agc.envelope() - kLow) / (kHigh - kLow);
        INFO("moved " << fraction << " of the way");
        CHECK(std::abs(fraction - (1.0 - std::exp(-1.0))) < 1e-6);
    }
}

TEST_CASE("a step down moves the envelope 1 - 1/e of the way in exactly the decay time",
          "[engine][agc]") {
    for (const double decay_ms : {500.0, 2000.0}) {
        INFO("decay " << decay_ms << " ms");
        const AgcSettings settings{true, 10.0, decay_ms};
        ListenerAgc agc;
        constexpr double kLow = kExactLow;
        constexpr double kHigh = kExactHigh;
        static_cast<void>(run(agc, square(kRate / 2, kHigh), 328, settings));
        REQUIRE(std::abs(agc.envelope() - kHigh) < 1e-12);

        const auto frames = static_cast<std::size_t>(decay_ms / 1000.0 * kRate);
        static_cast<void>(run(agc, square(frames, kLow), 328, settings));
        const double fraction = (kHigh - agc.envelope()) / (kHigh - kLow);
        INFO("moved " << fraction << " of the way");
        CHECK(std::abs(fraction - (1.0 - std::exp(-1.0))) < 1e-6);
    }
}

TEST_CASE("a sine 30 dB weaker comes out at the same level", "[engine][agc]") {
    // The case the stopgap in the client existed for: an SSB station and one
    // 30 dB down must both be heard, at the same level.
    ListenerAgc strong;
    ListenerAgc weak;
    const auto loud = sine(2 * kRate, 3.0e-2, 1000.0);
    const auto quiet = sine(2 * kRate, 3.0e-2 / std::pow(10.0, 30.0 / 20.0), 1000.0);
    const auto a = run(strong, loud, 328, AgcSettings{});
    const auto b = run(weak, quiet, 328, AgcSettings{});
    const double pa = peak(std::span<const float>(a).last(kRate / 2));
    const double pb = peak(std::span<const float>(b).last(kRate / 2));
    WARN("settled peak of a 1 kHz sine: " << db(pa) << " dBFS from -30.5 dBFS in, " << db(pb)
                                          << " dBFS from -60.5 dBFS in; target "
                                          << db(kHeardTarget));
    CHECK(std::abs(db(pa) - db(pb)) < 0.01);
    CHECK(std::abs(db(pa) - db(kHeardTarget)) < 1.0);
}

TEST_CASE("the envelope crosses a block edge exactly as it crosses a frame", "[engine][agc]") {
    // REJECTS: state reset or recomputed per chunk, which is a click at every
    // block edge and what "carries its envelope across blocks" rules out.
    // The first chunk is the same in both runs because it primes the
    // envelope from its own first attack time.
    const auto in = sine(kRate, 1.0e-3, 700.0);
    ListenerAgc whole;
    std::vector<float> one(in.size());
    whole.process(in, one, 1, kRate, true, AgcSettings{});

    ListenerAgc pieces;
    std::vector<float> many(in.size());
    const std::size_t sizes[] = {600, 97, 331, 1, 4096, 17};
    std::size_t at = 0;
    std::size_t which = 0;
    while (at < in.size()) {
        const std::size_t count = std::min(sizes[which % std::size(sizes)], in.size() - at);
        pieces.process(std::span<const float>(in).subspan(at, count),
                       std::span<float>(many).subspan(at, count), 1, kRate, true,
                       AgcSettings{});
        at += count;
        ++which;
    }
    CHECK(one == many);
}

TEST_CASE("a closed squelch passes zeros and leaves the envelope where it was",
          "[engine][agc]") {
    // REJECTS: following the zeros the gate writes, which walks the gain up to
    // its ceiling and puts the first syllable after the gate 70 dB up.
    ListenerAgc agc;
    constexpr double kLevel = kExactLow;
    static_cast<void>(run(agc, square(kRate / 2, kLevel), 328, AgcSettings{}));
    const double before = agc.envelope();

    const std::vector<float> zeros(2 * kRate, 0.0F);
    const auto shut = run(agc, zeros, 328, AgcSettings{}, false);
    CHECK(peak(shut) == 0.0);
    CHECK(agc.envelope() == before);

    const auto reopened = run(agc, square(328, kLevel), 328, AgcSettings{});
    CHECK(std::abs(peak(reopened) - kHeardTarget) < 1e-6);
}

TEST_CASE("off holds the gain it had, and on again returns to the target with a fade",
          "[engine][agc]") {
    ListenerAgc agc;
    constexpr double kLow = kExactLow;
    constexpr double kHigh = kExactHigh;
    const AgcSettings on{};
    AgcSettings off{};
    off.enabled = false;

    static_cast<void>(run(agc, square(kRate / 2, kLow), 328, on));
    const double held = agc.gain();

    // Twenty dB up with the AGC off comes out twenty dB up: the gain did not
    // move to meet it.
    const auto louder = run(agc, square(kRate / 2, kHigh), 328, off);
    CHECK(agc.holding());
    CHECK(agc.gain() == held);
    CHECK(std::abs(db(peak(louder) / kHeardTarget) - 20.0) < 1e-4);

    // Back on: the chunk primes the envelope from its own level and the gain
    // fades there over kSwitchRampMs rather than stepping at the edge.
    const auto back = run(agc, square(kRate / 10, kHigh), 328, on);
    CHECK_FALSE(agc.holding());
    const auto ramp = static_cast<std::size_t>(ListenerAgc::kSwitchRampMs / 1000.0 * kRate);
    CHECK(std::abs(std::abs(back.front()) / kHigh - held) / held < 1e-3);
    double worst = 0.0;
    for (std::size_t i = 1; i < ramp; ++i) {
        worst = std::max(worst, std::abs(std::abs(static_cast<double>(back[i])) -
                                         std::abs(static_cast<double>(back[i - 1]))));
    }
    const double step = (held * kHigh - kHeardTarget) / static_cast<double>(ramp);
    CHECK(worst <= step * 1.01);
    CHECK(std::abs(peak(std::span<const float>(back).last(kRate / 20)) - kHeardTarget) < 1e-6);
}

TEST_CASE("a receiver that starts with its AGC off holds the gain its first signal calls for",
          "[engine][agc]") {
    ListenerAgc agc;
    AgcSettings off{};
    off.enabled = false;

    // Silence first: a block with nothing in it is no guide.
    const std::vector<float> zeros(1000, 0.0F);
    CHECK(peak(run(agc, zeros, 328, off)) == 0.0);

    const auto first = run(agc, square(kRate / 4, kExactLow), 328, off);
    CHECK(std::abs(peak(first) - kHeardTarget) < 1e-6);
    const auto louder = run(agc, square(kRate / 4, kExactHigh), 328, off);
    CHECK(std::abs(db(peak(louder) / kHeardTarget) - 20.0) < 1e-4);
}

TEST_CASE("a restart puts the first block of a weaker station at the target", "[engine][agc]") {
    // REJECTS: carrying the envelope across a retune, which leaves a station
    // 30 dB weaker than the last one 30 dB low for the length of a decay.
    // The control arm is that case, measured.
    constexpr double kStrong = 3.0e-2;
    const double weak = kStrong / std::pow(10.0, 30.0 / 20.0);
    for (const bool restart : {true, false}) {
        INFO((restart ? "restarted" : "carried over"));
        ListenerAgc agc;
        static_cast<void>(run(agc, square(kRate / 2, kStrong), 328, AgcSettings{}));
        if (restart) {
            agc.restart();
        }
        const auto first = run(agc, square(328, weak), 328, AgcSettings{});
        const double level = db(peak(first) / kHeardTarget);
        if (restart) {
            CHECK(std::abs(level) < 1e-4);
        } else {
            CHECK(level < -29.0);
        }
    }
}

TEST_CASE("the gain never passes its ceiling", "[engine][agc]") {
    ListenerAgc agc;
    const auto faint = square(kRate, 1.0e-7);
    const auto out = run(agc, faint, 328, AgcSettings{});
    const double ceiling = std::pow(10.0, kAgcCeilingDb / 20.0);
    CHECK(agc.gain() <= ceiling * (1.0 + 1e-9));
    CHECK(peak(out) <= 1.0e-7 * ceiling * (1.0 + 1e-6));
}

TEST_CASE("one gain for both channels of a frame", "[engine][agc]") {
    ListenerAgc agc;
    std::vector<float> stereo(2 * kRate);
    const auto left = sine(kRate, 1.0e-3, 440.0);
    for (std::size_t i = 0; i < left.size(); ++i) {
        stereo[2 * i] = left[i];
        stereo[2 * i + 1] = 0.5F * left[i];
    }
    const auto out = run(agc, stereo, 328, AgcSettings{}, true, 2);
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (out[2 * i] != 0.0F) {
            REQUIRE(std::abs(out[2 * i + 1] / out[2 * i] - 0.5F) < 1e-6);
        }
    }
}

TEST_CASE("the time constants are refused outside their range while the AGC is on",
          "[engine][agc]") {
    engine::VrxParams params;
    CHECK(engine::validate_agc_request(params).has_value());

    params.agc_attack_ms = 0.0;
    CHECK_FALSE(engine::validate_agc_request(params).has_value());
    params.agc_enabled = false;
    CHECK(engine::validate_agc_request(params).has_value());

    params = engine::VrxParams{};
    params.agc_decay_ms = 1.0e5;
    CHECK_FALSE(engine::validate_agc_request(params).has_value());
    params.agc_decay_ms = std::nan("");
    CHECK_FALSE(engine::validate_agc_request(params).has_value());

    params = engine::VrxParams{};
    params.agc_attack_ms = engine::kAgcAttackMinMs;
    params.agc_decay_ms = engine::kAgcDecayMaxMs;
    CHECK(engine::validate_agc_request(params).has_value());
}
