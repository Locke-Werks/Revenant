// The probe pool, through the engine, on emitters whose modulation is known.
//
// WHAT THIS FILE IS THE RECORD OF
//
// core/engine/probe.h places receivers of the engine's own on detections and
// hands their baseband to core/characterise. docs/detection.md asked, before
// any of it was built, that anything feeding the characteriser a tighter
// extract show that a channel with nothing in it stays "unknown", because a
// narrowed extract of noise was measured coming back PSK at 0.98. The noise
// slot below is that check, run through the real receiver rather than through
// a low pass on the host.
//
// THE SCENE
//
// One emitter of each family tests/detect's family survey uses, plus an OFDM
// burst from tests/characterise/signal_lab.h and one empty slot, 50 kHz apart
// on a 600 kS/s source over 16 coarse channels. That grid runs its channels at
// 75000 S/s, so every bucket up to 48000 fits, and it keeps the capture file
// to 58 MB for twelve seconds. Everything is written to a cf32 file and read
// back through the engine's file source, so the path is the real one from the
// ring upward.
//
// WHAT "RIGHT" MEANS FOR EACH ROW, which is this file's choice and is stated
// rather than left to the assertion:
//
//   cw, am       Unmodulated. The characteriser has no AM family and calls a
//                carrier-dominated signal a carrier; a keyed carrier is one
//                too, which its own header says ("Keyed or not").
//   nfm          AnalogueFm.
//   usb, lsb     Unknown only. No family the characteriser names is single
//                sideband, so any name at all is wrong. AND BOTH COME BACK
//                WRONG: see kTwoToneCall below.
//   fsk2         Fsk.
//   bpsk, qpsk   Psk.
//   ofdm         Ofdm.
//   noise        Unknown only.
//
// Every row may also come back Unknown and still pass, because docs/
// detection.md makes unknown a real answer: a probe that declines is not
// wrong. The hidden survey at the end counts how often each row declines.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <numbers>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/characterise/catalogue.h"
#include "core/detect/detector.h"
#include "core/detect/tier_two.h"
#include "core/dsp/synth/wideband.h"
#include "core/engine/engine.h"
#include "core/engine/probe.h"
#include "tests/characterise/signal_lab.h"
#include "tests/reference/reference_diff.h"
#include "tests/support/temp_path.h"

using namespace revenant;
using characterise::ModulationFamily;

namespace {

constexpr dsp::SampleRate kRate = 600'000;
constexpr std::uint32_t kChannels = 16;
constexpr dsp::Hertz kSpacing = 50'000;
constexpr dsp::Hertz kFirst = -225'000;
constexpr dsp::Hertz kCentre = 145'000'000;

// The OFDM burst's own rate, and what that makes it: 100 of 256 subcarriers
// at 46.875 Hz apart is 4687.5 Hz occupied, and a useful symbol of 21.3 ms.
constexpr dsp::SampleRate kOfdmRate = 12'000;
constexpr std::size_t kOfdmUseful = 256;
constexpr std::size_t kOfdmPrefix = 32;
constexpr std::size_t kOfdmCarriers = 100;
constexpr dsp::Hertz kOfdmOccupied = static_cast<dsp::Hertz>(
    kOfdmCarriers * static_cast<std::size_t>(kOfdmRate) / kOfdmUseful);

// Where the first probe receiver's id sits; see core/engine/engine.cpp.
constexpr engine::VrxId kFirstProbeId{0x8000'0000U};

enum class Kind { Modulated, Ofdm, Noise };

struct Family {
    const char* name;
    Kind kind;
    siggen::Modulation modulation;
    ModulationFamily right;
};

constexpr Family kFamilies[] = {
    {"cw", Kind::Modulated, siggen::Modulation::Cw, ModulationFamily::Unmodulated},
    {"am", Kind::Modulated, siggen::Modulation::Am, ModulationFamily::Unmodulated},
    {"nfm", Kind::Modulated, siggen::Modulation::Nfm, ModulationFamily::AnalogueFm},
    {"usb", Kind::Modulated, siggen::Modulation::Usb, ModulationFamily::Unknown},
    {"lsb", Kind::Modulated, siggen::Modulation::Lsb, ModulationFamily::Unknown},
    {"fsk2", Kind::Modulated, siggen::Modulation::Fsk2, ModulationFamily::Fsk},
    {"bpsk", Kind::Modulated, siggen::Modulation::Bpsk, ModulationFamily::Psk},
    {"qpsk", Kind::Modulated, siggen::Modulation::Qpsk, ModulationFamily::Psk},
    {"ofdm", Kind::Ofdm, siggen::Modulation::Cw, ModulationFamily::Ofdm},
    {"noise", Kind::Noise, siggen::Modulation::Cw, ModulationFamily::Unknown},
};
constexpr std::size_t kSlots = std::size(kFamilies);

// What the characteriser says about the two sideband emitters, which is wrong
// and is pinned here as wrong rather than hidden.
//
// Measured 2026-09-23 through a 12000 S/s probe at 30 dB in the occupied band:
// usb and lsb both come back PSK of order 2 at 1200.1 baud and confidence
// 1.00, which characterise::may_drive_detection accepts. 1200 Hz is the gap
// between the scene's two tones, 700 and 1900 Hz. Two tones make a squared
// envelope with one pure line at their difference, which the symbol-rate
// detector reads as a symbol clock, and squaring them lights the M-th power
// law at exponent two. Everything it checks is satisfied and none of it is
// PSK. Its own spectral concentration says so, 0.50 against the 0.06 real
// BPSK and QPSK read in the same run, and nothing in the PSK branch reads it.
//
// So the assertion for these two rows is "unknown, or this exact wrong call",
// and the case prints which. The rule that would refuse it is a change to
// core/characterise that docs/detection.md leaves open, with the options.
constexpr ModulationFamily kTwoToneCall = ModulationFamily::Psk;

[[nodiscard]] bool is_sideband(std::string_view name) { return name == "usb" || name == "lsb"; }

[[nodiscard]] constexpr dsp::Hertz slot_offset(std::size_t slot) {
    return kFirst + static_cast<dsp::Hertz>(slot) * kSpacing;
}

// ---------------------------------------------------------------------------
// The capture
// ---------------------------------------------------------------------------

// Abramowitz and Stegun 9.6.12, for the interpolator's Kaiser window. The
// same interpolator as tests/engine/test_engine_dv.cpp, for the same reason:
// the OFDM burst is rendered at its own rate and has to be lifted onto the
// wideband source without putting an image anywhere a probe looks.
[[nodiscard]] double bessel_i0(double x) {
    const double quarter_square = 0.25 * x * x;
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k < 256; ++k) {
        term *= quarter_square / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1e-18 * sum) {
            break;
        }
    }
    return sum;
}

[[nodiscard]] std::vector<dsp::Complex32> interpolate(std::span<const dsp::Complex32> in,
                                                      std::uint32_t factor, double cutoff_hz,
                                                      dsp::SampleRate out_rate) {
    constexpr std::size_t kTapsPerPhase = 24;
    constexpr double kBeta = 8.0;

    const std::size_t length = kTapsPerPhase * factor;
    const double centre = (static_cast<double>(length) - 1.0) / 2.0;
    const double cutoff = cutoff_hz / static_cast<double>(out_rate);
    const double i0_beta = bessel_i0(kBeta);

    std::vector<double> taps(length, 0.0);
    double sum = 0.0;
    for (std::size_t i = 0; i < length; ++i) {
        const double position = static_cast<double>(i) - centre;
        const double ratio = 2.0 * static_cast<double>(i) / static_cast<double>(length - 1) - 1.0;
        const double window =
            bessel_i0(kBeta * std::sqrt(std::max(0.0, 1.0 - ratio * ratio))) / i0_beta;
        const double argument = 2.0 * cutoff * position;
        const double sinc = (argument == 0.0)
                                ? 1.0
                                : std::sin(std::numbers::pi * argument) /
                                      (std::numbers::pi * argument);
        taps[i] = 2.0 * cutoff * sinc * window;
        sum += taps[i];
    }
    const double scale = static_cast<double>(factor) / sum;

    std::vector<dsp::Complex32> out(in.size() * factor);
    for (std::size_t m = 0; m < out.size(); ++m) {
        std::complex<double> accumulated{0.0, 0.0};
        for (std::size_t k = m / factor + 1U; k-- > 0U;) {
            const std::size_t offset = m - k * factor;
            if (offset >= length) {
                break;
            }
            accumulated += taps[offset] * std::complex<double>(in[k]);
        }
        accumulated *= scale;
        out[m] = dsp::Complex32{static_cast<float>(accumulated.real()),
                                static_cast<float>(accumulated.imag())};
    }
    return out;
}

struct Truth {
    std::string name;
    dsp::Hertz low = 0;
    dsp::Hertz high = 0;
    ModulationFamily right = ModulationFamily::Unknown;

    [[nodiscard]] dsp::Hertz center() const { return (low + high) / 2; }
    [[nodiscard]] dsp::Hertz occupied() const { return high - low; }
};

// A cf32 file of the scene, removed when this goes.
struct Capture {
    std::filesystem::path path;
    std::vector<Truth> truth;
    double seconds = 0.0;

    Capture() = default;
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    ~Capture() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    [[nodiscard]] std::string uri() const {
        return "file:///" + path.generic_string() + "?rate=" + std::to_string(kRate) +
               "&format=cf32&center=" + std::to_string(kCentre);
    }
};

[[nodiscard]] std::unique_ptr<Capture> make_capture(double seconds, double snr_db,
                                                    std::uint64_t seed) {
    auto capture = std::make_unique<Capture>();
    capture->seconds = seconds;
    const auto samples =
        static_cast<dsp::SampleIndex>(std::llround(seconds * static_cast<double>(kRate)));

    siggen::SceneSpec spec;
    spec.rate = kRate;
    spec.center_hz = kCentre;
    spec.duration_samples = samples;
    spec.seed = seed;
    spec.noise_power_full_band_dbfs = -60.0;
    spec.worker_threads = 1;

    for (std::size_t slot = 0; slot < kSlots; ++slot) {
        if (kFamilies[slot].kind != Kind::Modulated) {
            continue;
        }
        siggen::ModulatorSpec modulator;
        modulator.kind = kFamilies[slot].modulation;
        modulator.common.rate = kRate;
        modulator.common.carrier_offset = slot_offset(slot);
        modulator.common.seed = seed + 17 * (slot + 1);

        // The same two-tone SSB tests/detect/test_front_end.cpp uses, so a
        // sideband emitter is two things rather than one carrier.
        modulator.ssb.tone_hz = 700;
        modulator.ssb.tone2_hz = 1900;

        // Narrowband refarming deviation, so Carson's 2 * (2500 + 3000) is
        // 11 kHz and fits the 48000 S/s bucket's quarter. At the default 5 kHz
        // the emitter is 16 kHz wide and too wide for this grid's probes.
        modulator.nfm.deviation = 2500;

        siggen::EmitterPlacement placement;
        placement.modulator = modulator;
        placement.use_snr = true;
        placement.snr_in_occupied_bandwidth_db = snr_db;
        placement.start_sample = 0;
        placement.end_sample = samples;
        spec.emitters.push_back(placement);
    }

    auto scene = siggen::Scene::create(spec);
    INFO(test::message_of(scene));
    REQUIRE(scene.has_value());

    std::vector<dsp::Complex32> buffer(static_cast<std::size_t>(samples));
    scene->render(0, buffer);

    for (std::size_t slot = 0; slot < kSlots; ++slot) {
        Truth truth;
        truth.name = kFamilies[slot].name;
        truth.right = kFamilies[slot].right;
        const dsp::Hertz offset = slot_offset(slot);

        if (kFamilies[slot].kind == Kind::Modulated) {
            const auto found = std::find_if(
                scene->truth().begin(), scene->truth().end(),
                [&](const siggen::EmitterTruth& row) { return row.carrier_offset_hz == offset; });
            REQUIRE(found != scene->truth().end());
            truth.low = found->extent.low_hz;
            truth.high = found->extent.high_hz;
        } else if (kFamilies[slot].kind == Kind::Ofdm) {
            truth.low = offset - kOfdmOccupied / 2;
            truth.high = offset + kOfdmOccupied / 2;
        } else {
            // The empty slot, asked about at the width of a narrow signal.
            truth.low = offset - 1'500;
            truth.high = offset + 1'500;
        }
        capture->truth.push_back(truth);
    }

    // The OFDM burst: rendered at its own rate, lifted by an integer factor,
    // put at its slot and set to the same SNR in its own occupied bandwidth
    // the scene gives every other emitter.
    {
        constexpr auto kFactor = static_cast<std::uint32_t>(kRate / kOfdmRate);
        characterise_test::OfdmSpec ofdm;
        ofdm.useful_samples = kOfdmUseful;
        ofdm.prefix_samples = kOfdmPrefix;
        ofdm.used_carriers = kOfdmCarriers;
        ofdm.symbol_count =
            static_cast<std::size_t>(samples / kFactor) / (kOfdmUseful + kOfdmPrefix) + 2;
        ofdm.seed = seed + 991;
        const auto native = characterise_test::ofdm_signal(ofdm);
        REQUIRE(native.size() == ofdm.symbol_count * (kOfdmUseful + kOfdmPrefix));

        auto lifted =
            interpolate(native, kFactor, 0.4 * static_cast<double>(kOfdmRate), kRate);
        REQUIRE(lifted.size() >= buffer.size());
        lifted.resize(buffer.size());

        double power = 0.0;
        for (const dsp::Complex32 sample : lifted) {
            power += static_cast<double>(std::norm(sample));
        }
        power /= static_cast<double>(lifted.size());

        const double noise_in_band = 1e-6 * static_cast<double>(kOfdmOccupied) /
                                     static_cast<double>(kRate);
        const double wanted = std::pow(10.0, snr_db / 10.0) * noise_in_band;
        const double gain = std::sqrt(wanted / power);

        std::size_t ofdm_slot = 0;
        while (kFamilies[ofdm_slot].kind != Kind::Ofdm) {
            ++ofdm_slot;
        }
        const dsp::Hertz offset = slot_offset(ofdm_slot);
        for (std::size_t n = 0; n < buffer.size(); ++n) {
            std::int64_t turns = (offset * static_cast<std::int64_t>(n)) % kRate;
            if (turns < 0) {
                turns += kRate;
            }
            const double angle =
                2.0 * std::numbers::pi * static_cast<double>(turns) / static_cast<double>(kRate);
            const std::complex<double> moved =
                gain * std::complex<double>(lifted[n]) * std::polar(1.0, angle);
            buffer[n] += dsp::Complex32{static_cast<float>(moved.real()),
                                        static_cast<float>(moved.imag())};
        }
    }

    capture->path = test::unique_temp_path("revenant_test_probe", ".cf32");
    std::FILE* file = std::fopen(capture->path.string().c_str(), "wb");
    REQUIRE(file != nullptr);
    const std::size_t wrote = std::fwrite(buffer.data(), sizeof(dsp::Complex32), buffer.size(), file);
    std::fclose(file);
    REQUIRE(wrote == buffer.size());
    return capture;
}

[[nodiscard]] engine::EngineConfig probe_config(std::uint32_t probes, std::uint32_t transform) {
    engine::EngineConfig config;
    config.channels = kChannels;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = 32'768;
    config.gpu_index = -1;
    config.probe_receivers = probes;
    config.spectrum_transform = transform;

    // PACED, AND IT HAS TO BE. The pool's worker places a probe on its own
    // thread and notices a full capture on a 10 ms poll, both on the wall
    // clock, while the capture itself is counted in stream samples. An
    // unthrottled file source moved this whole scene through the GPU in
    // 20 ms on the RTX 4090, so every probe was placed after the stream it
    // was meant to read had gone and not one filled. Four times realtime is
    // 40 ms of stream per poll, against a two second dwell.
    config.pace = 4.0;
    return config;
}

// ---------------------------------------------------------------------------
// One run of the pool with the requests handed over up front
// ---------------------------------------------------------------------------

struct PoolRun {
    std::vector<engine::ProbeOutcome> outcomes;
    engine::ProbeStats stats;
    std::uint32_t most_receivers = 0;
    std::uint32_t most_busy = 0;

    // Polls while the engine ran, and what vrx_ids answered at each. Every
    // one has to be exactly the ordinary receiver the run added.
    std::size_t polls = 0;
    std::size_t polls_listing_other_ids = 0;
    bool probe_id_refused = true;
    double wall_seconds = 0.0;
};

[[nodiscard]] PoolRun run_pool(const Capture& capture, std::uint32_t pool,
                               std::span<const engine::ProbeRequest> requests) {
    auto created = engine::Engine::create(probe_config(pool, 0));
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;

    const auto opened = eng.open_source(capture.uri());
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());
    REQUIRE(eng.info().channel_rate == 2 * kRate / kChannels);

    // One ordinary receiver beside the probes, which is what vrx_ids should
    // list and all it should list.
    engine::VrxParams ordinary;
    ordinary.center = 280'000;
    ordinary.demod = engine::Demod::Nfm;
    ordinary.bandwidth = 0;
    const auto ordinary_id = eng.add_vrx(ordinary);
    INFO(test::message_of(ordinary_id));
    REQUIRE(ordinary_id.has_value());

    for (const engine::ProbeRequest& request : requests) {
        const auto submitted = eng.submit_probe(request);
        INFO(test::message_of(submitted));
        REQUIRE(submitted.has_value());
    }

    PoolRun out;
    std::atomic<bool> finished{false};
    Status ran;
    const auto began = std::chrono::steady_clock::now();
    std::thread runner([&] {
        ran = eng.run();
        finished.store(true, std::memory_order_release);
    });

    std::vector<engine::ProbeOutcome> scratch(64);
    const auto take = [&] {
        const std::size_t got = eng.take_probe_outcomes(scratch);
        out.outcomes.insert(out.outcomes.end(), scratch.begin(),
                            scratch.begin() + static_cast<std::ptrdiff_t>(got));
    };

    while (!finished.load(std::memory_order_acquire)) {
        const std::vector<engine::VrxId> listed = eng.vrx_ids();
        ++out.polls;
        if (listed.size() != 1 || listed.front() != *ordinary_id) {
            ++out.polls_listing_other_ids;
        }
        if (eng.vrx_status(kFirstProbeId).has_value() || eng.remove_vrx(kFirstProbeId)) {
            out.probe_id_refused = false;
        }
        const engine::ProbeStats now = eng.probe_stats();
        out.most_receivers = std::max(out.most_receivers, now.receivers);
        out.most_busy = std::max(out.most_busy, now.busy);
        take();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    runner.join();
    out.wall_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());

    // The worker notices a full capture on its own poll, so the last outcomes
    // can land a moment after the stream has ended.
    for (int i = 0; i < 200 && out.outcomes.size() < requests.size(); ++i) {
        take();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    out.stats = eng.probe_stats();
    return out;
}

[[nodiscard]] std::vector<engine::ProbeRequest> requests_for(const Capture& capture) {
    std::vector<engine::ProbeRequest> requests;
    for (std::size_t slot = 0; slot < capture.truth.size(); ++slot) {
        engine::ProbeRequest request{};
        request.tag = slot + 1;
        request.center = capture.truth[slot].center();
        request.occupied_hz = capture.truth[slot].occupied();
        requests.push_back(request);
    }
    return requests;
}

[[nodiscard]] std::string describe(const engine::ProbeOutcome& outcome) {
    return std::format("{} {} conf {:.2f} rate {:.1f} Bd order {} tones {} conc {:.3f} drive {} "
                       "flag {} at {} S/s, {} samples, slot {}{}, {:.1f} ms",
                       engine::probe_status_name(outcome.status),
                       characterise::modulation_family_name(outcome.family), outcome.confidence,
                       outcome.symbol_rate_hz, outcome.order, outcome.tone_count,
                       outcome.concentration, outcome.may_drive_detection,
                       outcome.psk_without_symbol_rate, outcome.rate, outcome.samples,
                       outcome.slot, outcome.built ? " built" : "", outcome.characterise_ms);
}

}  // namespace

TEST_CASE("a probe's bucket and dwell follow the detection and the grid", "[engine][probe-pool]") {
    SECTION("narrow detections take the floor bucket and the stated dwell") {
        const auto shape = engine::probe_shape(10, 75'000);
        REQUIRE(shape.has_value());
        CHECK(shape->rate == engine::kProbeFloorRate);
        CHECK(shape->bandwidth == engine::kProbeFloorRate / 2);
        CHECK(shape->samples == 24'000);
        CHECK(shape->seconds == 2.0);
    }
    SECTION("the bucket is at least four times the occupied bandwidth") {
        const auto shape = engine::probe_shape(4'688, 75'000);
        REQUIRE(shape.has_value());
        CHECK(shape->rate == 24'000);
        CHECK(shape->samples == 48'000);
    }
    SECTION("nothing above the channel rate") {
        CHECK(engine::probe_shape(12'000, 75'000)->rate == 48'000);
        CHECK_FALSE(engine::probe_shape(12'001, 75'000).has_value());
    }
    SECTION("a grid under the floor stretches the dwell to the sample floor") {
        // 96 kS/s over the 64 channels revenant-cli pins is 3000 S/s channels.
        const auto shape = engine::probe_shape(15, 3'000);
        REQUIRE(shape.has_value());
        CHECK(shape->rate == 3'000);
        CHECK(shape->samples == characterise::kMinCharacteriseSamples);
        CHECK(shape->seconds > 5.4);
        CHECK_FALSE(engine::probe_shape(751, 3'000).has_value());
    }
    SECTION("every bucket the grid allows is a retune within itself") {
        // The half-width is a quarter of the bucket and the residual never
        // exceeds a quarter of the channel rate, so place() never clamps and
        // the shape cannot move with the centre. Walked across a whole
        // channel spacing at every bucket.
        const dsp::GridParams grid{.channels = kChannels,
                                   .taps_per_branch = 17,
                                   .decimation = kChannels / 2};
        for (const dsp::SampleRate rate : engine::kProbeRates) {
            if (rate > 2 * kRate / kChannels) {
                continue;
            }
            for (dsp::Hertz centre = 100'000; centre <= 100'000 + kRate / kChannels;
                 centre += 997) {
                engine::VrxParams params;
                params.center = centre;
                params.demod = engine::Demod::Raw;
                params.bandwidth = rate / 2;
                params.audio_rate = rate;
                const auto placed = engine::place(grid, kRate, params);
                REQUIRE(placed.has_value());
                INFO("rate " << rate << " centre " << centre);
                CHECK_FALSE(placed->bandwidth_clamped);
            }
        }
    }
}

TEST_CASE("a probe names each synthetic family or says unknown, and noise stays unknown",
          "[engine][probe-pool][gpu]") {
    constexpr std::uint64_t kSeed = 20'260'923;
    std::println("test_engine_probe families: seed {}", kSeed);

    const auto capture = make_capture(12.0, 30.0, kSeed);
    const auto requests = requests_for(*capture);
    const PoolRun run = run_pool(*capture, 4, requests);

    std::println("  {} outcomes in {:.2f} s wall, {} receivers built, {} retunes, "
                 "characterise {:.1f} ms in total",
                 run.outcomes.size(), run.wall_seconds, run.stats.builds, run.stats.retunes,
                 run.stats.characterise_ms_total);

    REQUIRE(run.outcomes.size() == requests.size());
    for (const engine::ProbeOutcome& outcome : run.outcomes) {
        REQUIRE(outcome.tag >= 1);
        REQUIRE(outcome.tag <= capture->truth.size());
        const Truth& truth = capture->truth[outcome.tag - 1];
        std::println("  {:<6} {}", truth.name, describe(outcome));

        INFO(truth.name << ": " << describe(outcome));
        CHECK(outcome.status == engine::ProbeStatus::Characterised);
        if (is_sideband(truth.name)) {
            CHECK((outcome.family == ModulationFamily::Unknown || outcome.family == kTwoToneCall));
            continue;
        }
        CHECK((outcome.family == truth.right || outcome.family == ModulationFamily::Unknown));
    }

    // The empty slot, on its own, because it is the one docs/detection.md
    // asked for by name.
    const auto noise = std::find_if(run.outcomes.begin(), run.outcomes.end(),
                                    [&](const engine::ProbeOutcome& outcome) {
                                        return capture->truth[outcome.tag - 1].name == "noise";
                                    });
    REQUIRE(noise != run.outcomes.end());
    CHECK(noise->family == ModulationFamily::Unknown);
    CHECK_FALSE(noise->may_drive_detection);
}

TEST_CASE("the pool never holds more receivers than its size and never lists one",
          "[engine][probe-pool][gpu]") {
    constexpr std::uint64_t kSeed = 20'260'924;
    const auto capture = make_capture(12.0, 30.0, kSeed);

    // Twice as many requests as receivers, in two buckets, so the pool has to
    // queue, retune in place and rebuild across a bucket.
    std::vector<engine::ProbeRequest> requests = requests_for(*capture);
    requests.resize(4);
    for (std::size_t i = 0; i < 4; ++i) {
        engine::ProbeRequest wide = requests[i];
        wide.tag = 100 + i;
        wide.occupied_hz = 10'000;
        requests.push_back(wide);
    }

    constexpr std::uint32_t kPool = 2;
    const PoolRun run = run_pool(*capture, kPool, requests);

    std::println("test_engine_probe pool: {} polls, most receivers {}, most busy {}, {} built, "
                 "{} retuned",
                 run.polls, run.most_receivers, run.most_busy, run.stats.builds,
                 run.stats.retunes);

    CHECK(run.polls > 0);
    CHECK(run.most_receivers <= kPool);
    CHECK(run.most_busy <= kPool);
    CHECK(run.stats.receivers <= kPool);
    CHECK(run.polls_listing_other_ids == 0);
    CHECK(run.probe_id_refused);

    REQUIRE(run.outcomes.size() == requests.size());
    CHECK(run.stats.characterised == requests.size());
    CHECK(run.stats.builds + run.stats.retunes == requests.size());
    CHECK(run.stats.retunes > 0);
}

TEST_CASE("a probe wider than the grid's buckets is refused without a receiver",
          "[engine][probe-pool][gpu]") {
    const auto capture = make_capture(1.0, 30.0, 7);
    engine::ProbeRequest wide{};
    wide.tag = 1;
    wide.center = 0;
    wide.occupied_hz = 40'000;
    const PoolRun run = run_pool(*capture, 1, std::span<const engine::ProbeRequest>(&wide, 1));
    REQUIRE(run.outcomes.size() == 1);
    CHECK(run.outcomes.front().status == engine::ProbeStatus::TooWide);
    CHECK(run.stats.builds == 0);
    CHECK(run.most_receivers == 0);
}

// ---------------------------------------------------------------------------
// The two surveys docs/detection.md records. Hidden, because each renders nine
// twelve-second scenes.
//
// The first puts one probe on each emitter at the emitter's own centre and
// occupied width, which is what the pool and the characteriser make of a
// signal when the detection handed to them is the whole signal. The second
// runs the detector and lets tier two probe its tracks, which is what they
// make of what the detector actually publishes, and the difference between
// the two tables is the finding.
// ---------------------------------------------------------------------------

TEST_CASE("probe survey: one probe per emitter at its own centre and width",
          "[.probe-survey]") {
    const std::uint64_t seeds[] = {101, 202, 303};
    const double levels[] = {30.0, 20.0, 10.0, 5.0};

    struct Tally {
        std::size_t correct = 0;
        std::size_t wrong = 0;
        std::size_t unknown = 0;
    };
    std::vector<std::vector<Tally>> tallies(std::size(levels), std::vector<Tally>(kSlots));
    std::vector<std::string> wrongs;

    for (std::size_t l = 0; l < std::size(levels); ++l) {
        for (const std::uint64_t seed : seeds) {
            const auto capture = make_capture(12.0, levels[l], seed);
            const auto requests = requests_for(*capture);
            const PoolRun run = run_pool(*capture, 4, requests);
            REQUIRE(run.outcomes.size() == requests.size());
            for (const engine::ProbeOutcome& outcome : run.outcomes) {
                const std::size_t slot = outcome.tag - 1;
                Tally& tally = tallies[l][slot];
                const ModulationFamily said =
                    outcome.may_drive_detection ? outcome.family : ModulationFamily::Unknown;
                if (said == ModulationFamily::Unknown) {
                    ++tally.unknown;
                } else if (said == capture->truth[slot].right) {
                    ++tally.correct;
                } else {
                    ++tally.wrong;
                    wrongs.push_back(std::format("{} at {:.0f} dB seed {}: {}",
                                                 capture->truth[slot].name, levels[l], seed,
                                                 describe(outcome)));
                }
            }
        }
    }

    std::println("probe survey at the emitter, {} seeds a level; correct/wrong/unknown, where a "
                 "call may_drive_detection refuses counts as unknown:",
                 std::size(seeds));
    std::string header = std::format("  {:<6}", "family");
    for (const double level : levels) {
        header += std::format("  {:>8}", std::format("{:.0f} dB", level));
    }
    std::println("{}", header);
    for (std::size_t slot = 0; slot < kSlots; ++slot) {
        std::string line = std::format("  {:<6}", kFamilies[slot].name);
        for (std::size_t l = 0; l < std::size(levels); ++l) {
            const Tally& tally = tallies[l][slot];
            line += std::format("  {:>8}", std::format("{}/{}/{}", tally.correct, tally.wrong,
                                                       tally.unknown));
        }
        std::println("{}", line);
    }
    for (const std::string& line : wrongs) {
        std::println("  wrong: {}", line);
    }
}

namespace {

struct FamilyScore {
    std::size_t tracks = 0;
    std::size_t probed = 0;
    std::size_t correct = 0;
    std::size_t wrong = 0;
    std::size_t unknown = 0;
    std::size_t refused = 0;

    // Seconds from the scene's start to the first accepted family on any of
    // this emitter's tracks, summed over the runs that got one.
    std::size_t emitters_named = 0;
    double first_seconds_total = 0.0;
    double first_seconds_worst = 0.0;
};

struct SurveyState {
    std::unique_ptr<detect::Detector> detector;
    std::unique_ptr<detect::TierTwo> tier_two;
};

}  // namespace

TEST_CASE("probe survey: tier two across the synthetic families", "[.probe-survey]") {
    const std::uint64_t seeds[] = {101, 202, 303};
    const double levels[] = {30.0, 20.0, 10.0};

    std::vector<FamilyScore> scores(kSlots);
    std::vector<std::string> wrongs;
    double characterise_ms = 0.0;
    std::uint64_t characterised = 0;

    for (const double level : levels) {
        for (const std::uint64_t seed : seeds) {
            const auto capture = make_capture(12.0, level, seed);

            auto created = engine::Engine::create(probe_config(4, 2048));
            REQUIRE(created.has_value());
            auto& eng = **created;
            REQUIRE(eng.open_source(capture->uri()).has_value());

            detect::DetectorConfig config;
            config.source_rate = kRate;
            config.source_center = eng.info().source_center;
            config.grid_channels = kChannels;
            auto detector = detect::Detector::create(config, eng.info().spectrum);
            REQUIRE(detector.has_value());
            auto tier_two = detect::TierTwo::create(detect::TierTwoConfig{.source_rate = kRate});
            REQUIRE(tier_two.has_value());

            SurveyState state;
            state.detector = std::make_unique<detect::Detector>(std::move(*detector));
            state.tier_two = std::make_unique<detect::TierTwo>(std::move(*tier_two));

            dsp::SampleIndex last = 0;
            const auto sink = [&](const engine::SpectrumFrame& frame) -> Status {
                if (auto fed = state.detector->consume(frame); !fed) {
                    return fed;
                }
                if (state.detector->last_decision() == last) {
                    return {};
                }
                last = state.detector->last_decision();
                return state.tier_two->step(*state.detector, eng);
            };
            REQUIRE(eng.set_spectrum_sink(sink).has_value());

            REQUIRE(eng.run().has_value());

            const engine::ProbeStats pool = eng.probe_stats();
            characterise_ms += pool.characterise_ms_total;
            characterised += pool.characterised;

            // Score the tracks alive at the end of the scene. A track is an
            // emitter's when its centre is inside that emitter's occupied
            // band widened by a kilohertz either side.
            std::vector<bool> emitter_named(kSlots, false);
            std::vector<double> emitter_first(kSlots, 0.0);
            for (const detect::Track& track : state.detector->tracks()) {
                const dsp::Hertz offset = track.center - kCentre;
                std::size_t slot = kSlots;
                for (std::size_t i = 0; i < kSlots; ++i) {
                    if (offset >= capture->truth[i].low - 1'000 &&
                        offset <= capture->truth[i].high + 1'000) {
                        slot = i;
                    }
                }
                if (slot == kSlots) {
                    continue;
                }
                FamilyScore& score = scores[slot];
                ++score.tracks;
                if (track.probes == 0) {
                    continue;
                }
                ++score.probed;
                const ModulationFamily right = capture->truth[slot].right;
                if (track.classification == detect::Classification::Unknown) {
                    ++score.unknown;
                    if (track.last_probe.family != detect::Classification::Unknown) {
                        ++score.refused;
                    }
                } else if (track.classification == detect::classification_of(right)) {
                    ++score.correct;
                } else {
                    ++score.wrong;
                    wrongs.push_back(std::format(
                        "{} at {:.0f} dB seed {}: track {} {} Hz wide called {} at {:.2f}, "
                        "{:.1f} Bd",
                        capture->truth[slot].name, level, seed, track.id, track.bandwidth,
                        detect::classification_name(track.classification),
                        track.classification_confidence, track.symbol_rate_hz));
                }
                if (track.classification != detect::Classification::Unknown) {
                    const double seconds = static_cast<double>(track.classified_at) /
                                           static_cast<double>(kRate);
                    if (!emitter_named[slot] || seconds < emitter_first[slot]) {
                        emitter_first[slot] = seconds;
                    }
                    emitter_named[slot] = true;
                }
            }
            for (std::size_t i = 0; i < kSlots; ++i) {
                if (emitter_named[i]) {
                    ++scores[i].emitters_named;
                    scores[i].first_seconds_total += emitter_first[i];
                    scores[i].first_seconds_worst =
                        std::max(scores[i].first_seconds_worst, emitter_first[i]);
                }
            }

            const detect::TierTwoStats& stats = state.tier_two->stats();
            std::println("  {:.0f} dB seed {}: {} submitted, {} recorded, {} accepted, {} "
                         "refused by characterise, {} orphaned, {} too wide, first "
                         "classification {:.2f} s mean over {} tracks ({:.2f} to {:.2f})",
                         level, seed, stats.submitted, stats.recorded, stats.accepted,
                         stats.refused_by_characterise, stats.orphaned, stats.too_wide,
                         stats.first_classifications == 0
                             ? 0.0
                             : stats.first_classification_seconds_total /
                                   static_cast<double>(stats.first_classifications),
                         stats.first_classifications, stats.first_classification_seconds_min,
                         stats.first_classification_seconds_max);
        }
    }

    std::println("probe survey, {} runs, tracks alive at the end of each scene:",
                 std::size(seeds) * std::size(levels));
    std::println("  {:<6} {:>6} {:>6} {:>7} {:>5} {:>7} {:>7}  {}", "family", "tracks", "probed",
                 "correct", "wrong", "unknown", "refused", "first named, mean and worst");
    for (std::size_t i = 0; i < kSlots; ++i) {
        const FamilyScore& score = scores[i];
        std::println("  {:<6} {:>6} {:>6} {:>7} {:>5} {:>7} {:>7}  {} of {} emitters, {:.2f} s, "
                     "{:.2f} s",
                     kFamilies[i].name, score.tracks, score.probed, score.correct, score.wrong,
                     score.unknown, score.refused, score.emitters_named,
                     std::size(seeds) * std::size(levels),
                     score.emitters_named == 0
                         ? 0.0
                         : score.first_seconds_total / static_cast<double>(score.emitters_named),
                     score.first_seconds_worst);
    }
    for (const std::string& line : wrongs) {
        std::println("  wrong: {}", line);
    }
    std::println("  characterise: {} extracts, {:.1f} ms each on average", characterised,
                 characterised == 0 ? 0.0 : characterise_ms / static_cast<double>(characterised));
}
