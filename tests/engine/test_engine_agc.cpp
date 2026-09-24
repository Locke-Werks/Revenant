// The receiver AGC through a real engine: what a person hears from each mode,
// what the decoders read beside it, and what the switch and a retune do on a
// running receiver.
//
// THE CAPTURES are complex baseband rendered here and played through the
// engine's file source, so every level is known to the decibel. Each
// amplitude-detected mode carries a 1 kHz tone, CW a bare carrier its
// receiver pitches to 700 Hz, AM at 80% modulation, and the FM modes a 1 kHz
// tone at the full deviation dsp::fm_deviation gives the receiver's own
// filter. "Level" is the carrier's amplitude, or the tone's for the
// suppressed-carrier modes, in dBFS.
//
// WHAT IS MEASURED, and printed so docs/rpc.md can carry it:
//
//   heard       AudioChunk::heard, what subscribeAudio sends and a
//               loudspeaker plays
//   samples     AudioChunk::samples, what the decoders, the RDS route and a
//               recording read
//
// A settled level is the peak over half a second of audio after a second of
// settling.
//
// Every figure is printed and the assertions are loose where a stage upstream
// could move them, for the reason tests/decode/CMakeLists.txt gives: a figure
// asserted to a decimal fails the day somebody improves the filter.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "core/dsp/vrx_reference.h"
#include "core/engine/engine.h"
#include "core/engine/listener_level.h"
#include "core/engine/vrx.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/support/temp_path.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 1'152'000;
constexpr double kToneHz = 1'000.0;
constexpr double kAudioRate = 48'000.0;

// The peak of a 1 kHz tone, and of the carrier CW pitches up, in dBFS of
// input. Thirty decibels apart, which is the spread the owner's rack sees
// between a local station and a distant one on one band.
constexpr double kStrongDbfs = -30.0;
constexpr double kWeakDbfs = -60.0;

[[nodiscard]] double amplitude_of(double dbfs) { return std::pow(10.0, dbfs / 20.0); }
[[nodiscard]] double db(double ratio) { return 20.0 * std::log10(ratio); }

// A signal at complex baseband with its carrier at DC, as a function of the
// sample index and the level.
using Render = std::function<std::complex<double>(std::size_t, double)>;

struct Emitter {
    dsp::Hertz center = 0;
    Render render;

    // Where the level changes, in seconds of capture, and what it becomes.
    // Empty is one level throughout.
    std::vector<std::pair<double, double>> steps;
};

[[nodiscard]] double seconds_at(std::size_t n) {
    return static_cast<double>(n) / static_cast<double>(kRate);
}

[[nodiscard]] Render tone_above() {
    return [](std::size_t n, double a) {
        return std::polar(a, 2.0 * std::numbers::pi * kToneHz * seconds_at(n));
    };
}

[[nodiscard]] Render tone_below() {
    return [](std::size_t n, double a) {
        return std::polar(a, -2.0 * std::numbers::pi * kToneHz * seconds_at(n));
    };
}

[[nodiscard]] Render am() {
    return [](std::size_t n, double a) {
        return std::complex<double>(
            a * (1.0 + 0.8 * std::cos(2.0 * std::numbers::pi * kToneHz * seconds_at(n))), 0.0);
    };
}

[[nodiscard]] Render dsb() {
    return [](std::size_t n, double a) {
        return std::complex<double>(a * std::cos(2.0 * std::numbers::pi * kToneHz * seconds_at(n)),
                                    0.0);
    };
}

[[nodiscard]] Render carrier() {
    return [](std::size_t, double a) { return std::complex<double>(a, 0.0); };
}

[[nodiscard]] Render fm(dsp::Hertz deviation) {
    return [deviation](std::size_t n, double a) {
        const double index = static_cast<double>(deviation) / kToneHz;
        return std::polar(a, index * std::sin(2.0 * std::numbers::pi * kToneHz * seconds_at(n)));
    };
}

// The whole capture: every emitter at its level, shifted to its centre with
// the phase reduced exactly in integers.
[[nodiscard]] std::vector<dsp::Complex32> render(std::span<const Emitter> emitters,
                                                 double level_dbfs, double seconds) {
    const auto count = static_cast<std::size_t>(seconds * static_cast<double>(kRate));
    std::vector<std::complex<double>> sum(count, {0.0, 0.0});
    for (const Emitter& emitter : emitters) {
        for (std::size_t n = 0; n < count; ++n) {
            double level = level_dbfs;
            for (const auto& [at, to] : emitter.steps) {
                if (seconds_at(n) >= at) {
                    level = to;
                }
            }
            std::int64_t turns = (emitter.center * static_cast<std::int64_t>(n)) % kRate;
            if (turns < 0) {
                turns += kRate;
            }
            const double angle =
                2.0 * std::numbers::pi * static_cast<double>(turns) / static_cast<double>(kRate);
            sum[n] += emitter.render(n, amplitude_of(level)) * std::polar(1.0, angle);
        }
    }
    std::vector<dsp::Complex32> out(count);
    for (std::size_t n = 0; n < count; ++n) {
        out[n] = dsp::Complex32{static_cast<float>(sum[n].real()),
                                static_cast<float>(sum[n].imag())};
    }
    return out;
}

// What one receiver handed out.
struct Collected {
    std::vector<float> samples;
    std::vector<float> heard;
    std::uint32_t channels = 1;
    bool mismatched = false;
    bool gap = false;
    dsp::SampleIndex next = 0;

    // The first frame each tuning epoch was seen at, indexed by epoch.
    std::vector<std::size_t> epoch_frame;

    [[nodiscard]] std::size_t frames() const { return samples.size() / channels; }
};

// Called under the collection lock, once per chunk, so a case can retune the
// receiver it is listening to from the stream the way a client's click
// arrives.
using OnChunk = std::function<void(std::size_t receiver, const engine::AudioChunk&,
                                   engine::Engine&, engine::VrxId)>;

struct Run {
    std::vector<Collected> out;
    std::vector<engine::VrxStatus> status;
    std::uint64_t retune_refusals = 0;
};

[[nodiscard]] Run run_capture(std::span<const dsp::Complex32> capture, std::uint32_t channels,
                              std::span<const engine::VrxParams> receivers,
                              const std::string& tag, const OnChunk& on_chunk = {}) {
    const std::filesystem::path path =
        test::unique_temp_path(std::format("revenant_test_agc_{}", tag), ".cf32");
    {
        std::FILE* file = std::fopen(path.string().c_str(), "wb");
        REQUIRE(file != nullptr);
        const std::size_t wrote =
            std::fwrite(capture.data(), sizeof(dsp::Complex32), capture.size(), file);
        std::fclose(file);
        REQUIRE(wrote == capture.size());
    }
    struct Remove {
        std::filesystem::path path;
        ~Remove() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } remove{path};

    engine::EngineConfig config;
    config.channels = channels;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = 32'768;
    config.gpu_index = -1;

    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;
    const std::string uri = "file:///" + path.generic_string() + "?rate=" +
                            std::to_string(kRate) + "&format=cf32&center=7100000";
    const auto opened = eng.open_source(uri);
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    Run run;
    run.out.resize(receivers.size());
    std::vector<engine::VrxId> ids;
    std::mutex lock;
    for (std::size_t i = 0; i < receivers.size(); ++i) {
        const auto id = eng.add_vrx(receivers[i]);
        INFO("receiver " << i << ": " << test::message_of(id));
        REQUIRE(id.has_value());
        ids.push_back(*id);
    }
    for (std::size_t i = 0; i < receivers.size(); ++i) {
        Collected* const into = &run.out[i];
        const engine::VrxId id = ids[i];
        REQUIRE(eng.attach_audio_sink(id, [&, into, i, id](const engine::AudioChunk& chunk)
                                              -> Status {
                       const std::lock_guard<std::mutex> guard(lock);
                       into->channels = std::max<std::uint32_t>(chunk.channels, 1);
                       if (chunk.heard.size() != chunk.samples.size()) {
                           into->mismatched = true;
                       }
                       if (chunk.start != into->next) {
                           into->gap = true;
                       }
                       const std::size_t frames = chunk.samples.size() / into->channels;
                       into->next = chunk.start + frames;
                       while (into->epoch_frame.size() <= chunk.tuning_epoch) {
                           into->epoch_frame.push_back(into->frames());
                       }
                       into->samples.insert(into->samples.end(), chunk.samples.begin(),
                                            chunk.samples.end());
                       into->heard.insert(into->heard.end(), chunk.heard.begin(),
                                          chunk.heard.end());
                       if (on_chunk) {
                           on_chunk(i, chunk, eng, id);
                       }
                       return {};
                   })
                    .has_value());
    }

    const auto ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    for (const engine::VrxId id : ids) {
        const auto status = eng.vrx_status(id);
        REQUIRE(status.has_value());
        run.status.push_back(*status);
    }
    run.retune_refusals = eng.graph_conditions().vrx_retune_refusals;
    return run;
}

// The peak of the first channel over [from, to) seconds of audio.
[[nodiscard]] double peak_over(std::span<const float> audio, std::uint32_t channels,
                               double from, double to) {
    const auto first = static_cast<std::size_t>(from * kAudioRate);
    const auto last = std::min(static_cast<std::size_t>(to * kAudioRate), audio.size() / channels);
    double out = 0.0;
    for (std::size_t i = first; i < last; ++i) {
        out = std::max(out, std::abs(static_cast<double>(audio[i * channels])));
    }
    return out;
}

// The peak of each millisecond, in dB relative to `reference`.
[[nodiscard]] std::vector<double> level_track(std::span<const float> audio, double reference) {
    constexpr std::size_t kWindow = 48;
    std::vector<double> out;
    for (std::size_t at = 0; at + kWindow <= audio.size(); at += kWindow) {
        double peak = 0.0;
        for (std::size_t i = at; i < at + kWindow; ++i) {
            peak = std::max(peak, std::abs(static_cast<double>(audio[i])));
        }
        out.push_back(peak > 0.0 ? db(peak / reference) : -300.0);
    }
    return out;
}

[[nodiscard]] engine::VrxParams receiver(engine::Demod mode, dsp::Hertz center) {
    engine::VrxParams params;
    params.center = center;
    params.demod = mode;
    params.bandwidth = 0;
    return params;
}

// The FM deviation a receiver's own filter implies, which the capture is
// rendered at so the discriminator swings the audio to +/-1.
[[nodiscard]] dsp::Hertz full_deviation(engine::Demod mode) {
    const auto band = dsp::default_passband(mode);
    return dsp::fm_deviation(static_cast<std::uint32_t>(mode), band.high - band.low);
}

}  // namespace

TEST_CASE("every mode reaches its listening level from inputs 30 dB apart",
          "[gpu][engine][agc]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // Six narrow emitters on a 16-channel grid, 100 kHz apart, and WFM on a
    // grid of its own below, because a broadcast channel does not fit a 72
    // kHz spacing.
    struct Mode {
        engine::Demod demod;
        dsp::Hertz center;
        Render render;
    };
    const std::vector<Mode> modes = {
        {engine::Demod::Am, -300'000, am()},
        {engine::Demod::Usb, -200'000, tone_above()},
        {engine::Demod::Lsb, -100'000, tone_below()},
        {engine::Demod::Dsb, 100'000, dsb()},
        {engine::Demod::Cw, 200'000, carrier()},
        {engine::Demod::Nfm, 300'000, fm(full_deviation(engine::Demod::Nfm))},
    };
    std::vector<Emitter> emitters;
    std::vector<engine::VrxParams> receivers;
    for (const Mode& mode : modes) {
        emitters.push_back(Emitter{mode.center, mode.render, {}});
        receivers.push_back(receiver(mode.demod, mode.center));
    }

    const std::vector<Emitter> broadcast = {
        Emitter{0, fm(full_deviation(engine::Demod::Wfm)), {}}};
    const std::vector<engine::VrxParams> wfm = {receiver(engine::Demod::Wfm, 0)};

    constexpr double kSeconds = 2.0;
    const Run strong = run_capture(render(emitters, kStrongDbfs, kSeconds), 16, receivers,
                                   "strong");
    const Run weak = run_capture(render(emitters, kWeakDbfs, kSeconds), 16, receivers, "weak");
    const Run wfm_strong =
        run_capture(render(broadcast, kStrongDbfs, kSeconds), 4, wfm, "wfm_strong");
    const Run wfm_weak = run_capture(render(broadcast, kWeakDbfs, kSeconds), 4, wfm, "wfm_weak");

    auto report = [](engine::Demod mode, const Collected& loud, const Collected& quiet) {
        const double heard_loud = db(peak_over(loud.heard, loud.channels, 1.0, 1.5));
        const double heard_quiet = db(peak_over(quiet.heard, quiet.channels, 1.0, 1.5));
        const double out_loud = db(peak_over(loud.samples, loud.channels, 1.0, 1.5));
        const double out_quiet = db(peak_over(quiet.samples, quiet.channels, 1.0, 1.5));
        WARN(std::format("{:>4}: heard {:7.2f} and {:7.2f} dBFS from {} and {} dBFS in; the "
                         "receiver's own output {:7.2f} and {:7.2f} dBFS",
                         engine::demod_name(mode), heard_loud, heard_quiet, kStrongDbfs,
                         kWeakDbfs, out_loud, out_quiet));
        CHECK_FALSE(loud.mismatched);
        CHECK_FALSE(quiet.mismatched);
        CHECK(loud.frames() > static_cast<std::size_t>(1.5 * kAudioRate));
        CHECK(quiet.frames() > static_cast<std::size_t>(1.5 * kAudioRate));

        if (engine::levelling_for(mode) == engine::Levelling::Agc) {
            // The receiver's output is 30 dB apart and what is heard is not.
            CHECK(std::abs((out_loud - out_quiet) - 30.0) < 0.5);
            CHECK(std::abs(heard_loud - heard_quiet) < 0.1);
            CHECK(std::abs(heard_loud - db(engine::kHeardTarget)) < 1.0);
            CHECK(std::abs(heard_quiet - db(engine::kHeardTarget)) < 1.0);
        } else {
            // The discriminator's output does not know the input level, and
            // what is heard is it times the fixed gain, to the bit: a
            // multiply by a power of two.
            CHECK(std::abs(out_loud - out_quiet) < 0.05);
            for (const Collected* run : {&loud, &quiet}) {
                bool exact = run->heard.size() == run->samples.size();
                for (std::size_t i = 0; exact && i < run->samples.size(); ++i) {
                    exact = run->heard[i] == run->samples[i] * engine::kFmHeardGain;
                }
                CHECK(exact);
            }
            CHECK(std::abs(heard_loud - db(engine::kHeardTarget)) < 1.5);
        }
    };

    for (std::size_t i = 0; i < modes.size(); ++i) {
        INFO(engine::demod_name(modes[i].demod));
        report(modes[i].demod, strong.out[i], weak.out[i]);
    }
    report(engine::Demod::Wfm, wfm_strong.out[0], wfm_weak.out[0]);
}

TEST_CASE("the heard level follows the attack and decay, and the decoders' input does not move",
          "[gpu][engine][agc]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // One USB tone: -50 dBFS, up 20 dB at 0.5 s, down again at 1.5 s, heard
    // to 3.5 s. Four receivers on it, identical but for the AGC: the default
    // constants, four times both, the default switched off, and the default
    // again. The first two are the timing; all four are the decoders' input,
    // which must be the same bits whatever the AGC is doing.
    constexpr dsp::Hertz kCenter = 2 * 72'000 + 9'000;
    const std::vector<Emitter> emitters = {
        Emitter{kCenter, tone_above(), {{0.5, -30.0}, {1.5, -50.0}}}};

    engine::VrxParams fast = receiver(engine::Demod::Usb, kCenter);
    engine::VrxParams slow = fast;
    slow.agc_attack_ms = 40.0;
    slow.agc_decay_ms = 2'000.0;
    engine::VrxParams off = fast;
    off.agc_enabled = false;
    const std::vector<engine::VrxParams> receivers = {fast, slow, off, fast};

    const Run run = run_capture(render(emitters, -50.0, 3.5), 16, receivers, "timing");
    for (const Collected& out : run.out) {
        REQUIRE_FALSE(out.mismatched);
        REQUIRE(out.frames() > static_cast<std::size_t>(3.2 * kAudioRate));
    }

    // THE DECODERS' INPUT, BIT FOR BIT. Four receivers whose AGCs did four
    // different things, and one stream of samples between them.
    for (std::size_t i = 1; i < run.out.size(); ++i) {
        INFO("receiver " << i);
        REQUIRE(run.out[i].samples.size() == run.out[0].samples.size());
        CHECK(std::memcmp(run.out[i].samples.data(), run.out[0].samples.data(),
                          run.out[0].samples.size() * sizeof(float)) == 0);
    }
    CHECK(run.out[0].heard != run.out[1].heard);
    CHECK(run.out[0].heard == run.out[3].heard);

    // Where the steps landed in the audio, read off the receiver's own
    // output, which moves 20 dB with the input and nothing else.
    const auto output = level_track(run.out[0].samples, 1.0);
    const double low = output[400];
    std::size_t up = 0;
    while (up < output.size() && output[up] < low + 10.0) {
        ++up;
    }
    std::size_t down = up + 200;
    while (down < output.size() && output[down] > low + 10.0) {
        ++down;
    }
    REQUIRE(up < output.size());
    REQUIRE(down < output.size());

    struct Timing {
        double overshoot_db = 0.0;
        double settle_ms = 0.0;
        double dip_db = 0.0;
        double recover_ms = 0.0;
    };
    auto time_it = [&](const Collected& out) {
        const auto heard = level_track(out.heard, engine::kHeardTarget);
        Timing t;
        const double high = heard[down - 50];
        t.overshoot_db = *std::max_element(heard.begin() + static_cast<std::ptrdiff_t>(up),
                                           heard.begin() + static_cast<std::ptrdiff_t>(up + 20));
        // Stopping short of the fall, whose first windows straddle it.
        std::size_t settled = up;
        for (std::size_t i = up; i + 20 < down; ++i) {
            if (std::abs(heard[i] - high) > 1.0) {
                settled = i;
            }
        }
        t.settle_ms = static_cast<double>(settled + 1 - up);

        // The level the low input settles at, from before the rise: the
        // slow receiver has not finished recovering by the end of the run.
        const double floor_level = heard[up - 50];
        t.dip_db = *std::min_element(heard.begin() + static_cast<std::ptrdiff_t>(down),
                                     heard.begin() + static_cast<std::ptrdiff_t>(down + 20)) -
                   floor_level;
        std::size_t recovered = down;
        while (recovered < heard.size() && heard[recovered] < floor_level - 14.0) {
            ++recovered;
        }
        t.recover_ms = static_cast<double>(recovered - down);
        return t;
    };
    const Timing a = time_it(run.out[0]);
    const Timing b = time_it(run.out[1]);
    WARN(std::format("usb, 20 dB step up then down: attack 10 ms settles within 1 dB in {} ms "
                     "after a {:.1f} dB overshoot, attack 40 ms in {} ms after {:.1f} dB; decay "
                     "500 ms climbs out of a {:.1f} dB dip to 14 dB below in {} ms, decay "
                     "2000 ms out of {:.1f} dB in {} ms",
                     a.settle_ms, a.overshoot_db, b.settle_ms, b.overshoot_db, a.dip_db,
                     a.recover_ms, b.dip_db, b.recover_ms));

    // The step itself is the whole 20 dB before either constant can act.
    CHECK(a.overshoot_db > 15.0);
    CHECK(a.dip_db < -15.0);

    // Four times the constant is about four times the time. A tone's crests
    // are where the attack acts, so a rise takes a few attack times; a fall
    // is the decay alone and scales with it exactly.
    const double attack_ratio = b.settle_ms / a.settle_ms;
    const double decay_ratio = b.recover_ms / a.recover_ms;
    INFO("attack ratio " << attack_ratio << ", decay ratio " << decay_ratio);
    CHECK(attack_ratio > 3.0);
    CHECK(attack_ratio < 5.0);
    CHECK(decay_ratio > 3.5);
    CHECK(decay_ratio < 4.5);

    // Off from the start holds the gain its first block called for, so the
    // step up comes out 20 dB up and stays there.
    const auto held = level_track(run.out[2].heard, engine::kHeardTarget);
    const double before = held[up - 50];
    const double after = held[down - 50];
    INFO("held " << before << " dB before the step and " << after << " after");
    CHECK(std::abs(before) < 1.0);
    CHECK(std::abs((after - before) - 20.0) < 0.5);
}

TEST_CASE("switching the AGC off and on again is a retune in place on a running receiver",
          "[gpu][engine][agc]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // -50 dBFS, up 20 dB at 1.0 s and down again at 2.0 s. The receiver is
    // switched off at 0.6 s of audio, holds through the rise, and is
    // switched on again at 1.6 s. A second receiver on the same signal is
    // never touched, and its samples are the first one's, bit for bit.
    constexpr dsp::Hertz kCenter = 2 * 72'000 + 9'000;
    const std::vector<Emitter> emitters = {
        Emitter{kCenter, tone_above(), {{1.0, -30.0}, {2.0, -50.0}}}};
    const engine::VrxParams params = receiver(engine::Demod::Usb, kCenter);
    const std::vector<engine::VrxParams> receivers = {params, params};

    Status switched_off;
    Status switched_on;
    int stage = 0;
    const OnChunk toggle = [&](std::size_t which, const engine::AudioChunk& chunk,
                               engine::Engine& eng, engine::VrxId id) {
        if (which != 0) {
            return;
        }
        const double at = static_cast<double>(chunk.start) / kAudioRate;
        if (stage == 0 && at >= 0.6) {
            engine::VrxParams next = params;
            next.agc_enabled = false;
            switched_off = eng.set_vrx_params(id, next);
            stage = 1;
        } else if (stage == 1 && at >= 1.6) {
            switched_on = eng.set_vrx_params(id, params);
            stage = 2;
        }
    };
    const Run run = run_capture(render(emitters, -50.0, 3.0), 16, receivers, "switch", toggle);

    INFO(test::message_of(switched_off));
    CHECK(switched_off.has_value());
    INFO(test::message_of(switched_on));
    CHECK(switched_on.has_value());
    REQUIRE(stage == 2);

    const Collected& toggled = run.out[0];
    const Collected& control = run.out[1];

    // Tuning, not shape: no remove and add, no hole in the stream, nothing
    // refused, and the receiver's own output untouched.
    CHECK_FALSE(toggled.gap);
    CHECK_FALSE(toggled.mismatched);
    CHECK(run.retune_refusals == 0U);
    CHECK(run.status[0].reanchors == 0U);
    CHECK(run.status[0].tuning_epoch == 2U);
    CHECK(run.status[0].params.agc_enabled);
    REQUIRE(toggled.samples.size() == control.samples.size());
    CHECK(std::memcmp(toggled.samples.data(), control.samples.data(),
                      control.samples.size() * sizeof(float)) == 0);

    // Where the two switches landed, off the chunks' own epochs.
    REQUIRE(toggled.epoch_frame.size() == 3U);
    const double off_at = static_cast<double>(toggled.epoch_frame[1]) / kAudioRate;
    const double on_at = static_cast<double>(toggled.epoch_frame[2]) / kAudioRate;
    const double target = db(engine::kHeardTarget);
    const double before = db(peak_over(toggled.heard, 1, 0.3, off_at));
    const double held = db(peak_over(toggled.heard, 1, 1.2, on_at));
    const double back = db(peak_over(toggled.heard, 1, on_at + 0.05, 1.95));
    const double control_high = db(peak_over(control.heard, 1, 1.2, 1.95));
    WARN(std::format("usb, AGC switched off at {:.3f} s and on at {:.3f} s: {:.2f} dBFS before, "
                     "{:.2f} dBFS held through a 20 dB rise, {:.2f} dBFS once back on; the "
                     "untouched receiver {:.2f} dBFS through the same rise",
                     off_at, on_at, before, held, back, control_high));
    CHECK(off_at < 1.0);
    CHECK(on_at > 1.2);
    CHECK(on_at < 1.9);
    CHECK(std::abs(before - target) < 1.0);
    CHECK(std::abs((held - before) - 20.0) < 1.0);
    CHECK(std::abs(back - target) < 1.0);
    CHECK(std::abs(control_high - target) < 1.0);

    // Back on is a fade and not a step. A 1 kHz tone at the held level moves
    // at most 2*pi*1000/48000 of its peak between two samples, and a gain
    // stepping from the held level to the target at the switch would move it
    // by up to nine tenths of it; the fade adds under a percent.
    const auto first = toggled.epoch_frame[2];
    double worst = 0.0;
    for (std::size_t i = first; i < first + 240 && i < toggled.heard.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(toggled.heard[i]) -
                                         static_cast<double>(toggled.heard[i - 1])));
    }
    const double slope = 2.0 * std::numbers::pi * kToneHz / kAudioRate * std::pow(10.0, held / 20.0);
    INFO("largest sample-to-sample move in the fade " << worst << ", the tone's own " << slope);
    CHECK(worst < 1.2 * slope);
}

TEST_CASE("a retune onto a station 30 dB weaker is heard at the level from its first block",
          "[gpu][engine][agc]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // Two USB tones 20 kHz apart in one grid channel, the first at -30 dBFS
    // and the second at -60. The receiver starts on the strong one and is
    // moved to the weak one at 1.0 s of audio, further than its own 2.4 kHz
    // passband, which restarts the AGC. Carried over instead, the envelope
    // would leave the weak station 30 dB down until the 500 ms decay had
    // brought it up, about two seconds.
    constexpr dsp::Hertz kStrong = 2 * 72'000 + 5'000;
    constexpr dsp::Hertz kWeak = kStrong + 20'000;
    const std::vector<Emitter> emitters = {
        Emitter{kStrong, tone_above(), {}},
        Emitter{kWeak, tone_above(), {{0.0, kWeakDbfs}}},
    };
    const engine::VrxParams params = receiver(engine::Demod::Usb, kStrong);
    const std::vector<engine::VrxParams> receivers = {params};

    Status moved;
    bool done = false;
    const OnChunk retune = [&](std::size_t, const engine::AudioChunk& chunk,
                               engine::Engine& eng, engine::VrxId id) {
        if (!done && static_cast<double>(chunk.start) / kAudioRate >= 1.0) {
            engine::VrxParams next = params;
            next.center = kWeak;
            moved = eng.set_vrx_params(id, next);
            done = true;
        }
    };
    const Run run =
        run_capture(render(emitters, kStrongDbfs, 2.0), 16, receivers, "retune", retune);
    INFO(test::message_of(moved));
    REQUIRE(moved.has_value());
    const Collected& out = run.out[0];
    REQUIRE(out.epoch_frame.size() == 2U);
    CHECK(run.retune_refusals == 0U);

    const std::size_t at = out.epoch_frame[1];
    const double seconds = static_cast<double>(at) / kAudioRate;
    const double target = db(engine::kHeardTarget);
    const double before = db(peak_over(out.heard, 1, 0.5, seconds));
    const double first = db(peak_over(out.heard, 1, seconds + 0.005, seconds + 0.025));
    const double loudest = db(peak_over(out.heard, 1, seconds, seconds + 0.2));
    const double later = db(peak_over(out.heard, 1, seconds + 0.2, seconds + 0.6));
    const double output_step = db(peak_over(out.samples, 1, seconds + 0.2, seconds + 0.6) /
                                  peak_over(out.samples, 1, 0.5, seconds));
    WARN(std::format("usb retuned 20 kHz from a {} dBFS tone to a {} dBFS one at {:.3f} s: "
                     "{:.2f} dBFS before, {:.2f} dBFS over 5 to 25 ms after, at most {:.2f} "
                     "dBFS in the first 200 ms, {:.2f} dBFS after; the receiver's own output "
                     "moved {:.2f} dB",
                     kStrongDbfs, kWeakDbfs, seconds, before, first, loudest, later,
                     output_step));
    CHECK(std::abs(output_step + 30.0) < 1.0);
    CHECK(std::abs(before - target) < 1.0);
    CHECK(std::abs(first - target) < 1.5);
    CHECK(loudest < target + 3.0);
    CHECK(std::abs(later - target) < 1.0);
}
