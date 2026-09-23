// Noise mitigation through the engine: the three stages on real receivers,
// what their fields do on the wire of a retune, and what each one buys on a
// synthetic SSB voice buried in the three things it exists for.
//
// THE MEASUREMENT
//
// A voice-like USB transmission, a harmonic series on a gliding pitch with
// three formants and a syllabic envelope with pauses in it, rendered as
// complex baseband straight at a 1.152 MS/s source rate, 60 dB under full
// scale, and placed 9 kHz off a grid channel's centre. Five captures of it go
// through the engine's file source:
//
//   clean       the voice alone, which is the reference every figure is
//               measured against
//   noisy       the voice and white noise at 15 dB SNR in 2500 Hz
//   whistle     the voice and a steady carrier 1300 Hz above its carrier at
//               the voice's own RMS, which is a 1300 Hz whistle in the audio
//   impulsive   the voice and impulses
//   dirty       all three at once, the same samples of each
//
// The impulses are 100 a second at random instants, three source samples
// long, a thousand times the voice's RMS with a random phase, which puts
// them at full scale: ignition noise on a weak station.
//
// OUTPUT SNR is the clean receiver's audio against each receiver's: the best
// alignment within the stages' latency, a least-squares gain, and the ratio
// of what that explains to what it leaves. Everything a stage does to the
// voice counts against it, so a noise reduction that ate the voice would
// score badly rather than well. The first 0.75 s is left out, which is the
// time the automatic notch and the noise floor take to settle.
//
// IMPULSE ENERGY REMOVED is measured where the blanker works, on the coarse
// channel: a raw tap on the voice's centre in the clean and impulsive runs,
// the difference between the two taps is the impulses as the channelizer
// delivered them, and the blanker's twin, which the conformance suite holds
// bit-identical to the kernel, is run over the impulsive tap. The figure is
// that difference's energy over what is left of it after blanking, and the
// same twin over the clean tap says how much voice it cut when there was
// nothing to cut.
//
// Every figure is printed. The assertions are loose, for the reason
// tests/decode/CMakeLists.txt gives: a figure asserted to a decimal fails
// the day somebody improves the stage. Nothing here is compared with any
// other program.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <mutex>
#include <numbers>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "core/dsp/noise_reference.h"
#include "core/dsp/synth/channel.h"
#include "core/engine/engine.h"
#include "core/engine/vrx.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/support/temp_path.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 1'152'000;
constexpr std::uint32_t kChannels = 16;
constexpr dsp::Hertz kSpacing = kRate / kChannels;
constexpr dsp::Hertz kCarrierHz = 2 * kSpacing + 9'000;
constexpr dsp::Hertz kWhistleHz = 1'300;
constexpr double kSeconds = 3.0;
constexpr double kSettleSeconds = 0.75;
constexpr std::uint64_t kSeed = 0x4E4F495345454E47ULL;

// The voice's RMS, impulses a second, and their height as a multiple of the
// voice's RMS.
constexpr double kVoiceRms = 1.0e-3;
constexpr double kImpulsesPerSecond = 100.0;
constexpr double kImpulseHeight = 1000.0;

engine::EngineConfig noise_config() {
    engine::EngineConfig config;
    config.channels = kChannels;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = 32'768;
    config.gpu_index = -1;
    return config;
}

// The voice, at complex baseband with its carrier at DC, at the source rate.
// A USB transmission's baseband is the analytic signal of its audio, so each
// harmonic is one positive-frequency phasor and there is nothing to filter.
[[nodiscard]] std::vector<std::complex<double>> render_voice(std::size_t count) {
    std::vector<std::complex<double>> out(count);
    const auto rate = static_cast<double>(kRate);
    double theta = 0.0;
    for (std::size_t n = 0; n < count; ++n) {
        const double t = static_cast<double>(n) / rate;
        const double f0 = 130.0 + 25.0 * std::sin(2.0 * std::numbers::pi * 2.3 * t) +
                          10.0 * std::sin(2.0 * std::numbers::pi * 0.9 * t + 1.0);
        theta = std::fmod(theta + 2.0 * std::numbers::pi * f0 / rate, 2.0 * std::numbers::pi);

        // Syllables about three a second, with a silence between each, and a
        // slower swell across a phrase.
        const double syllable = std::sin(2.0 * std::numbers::pi * 3.1 * t);
        const double envelope =
            (syllable > 0.0 ? std::sqrt(syllable) : 0.0) *
            (0.6 + 0.4 * std::sin(2.0 * std::numbers::pi * 0.37 * t));

        const std::complex<double> step = std::polar(1.0, theta);
        std::complex<double> phasor = step;
        std::complex<double> sum{0.0, 0.0};
        for (int h = 1; h <= 24; ++h) {
            const double f = static_cast<double>(h) * f0;
            if (f > 300.0 && f < 2'700.0) {
                auto formant = [f](double centre, double width) {
                    const double x = (f - centre) / width;
                    return 1.0 / (1.0 + x * x);
                };
                const double amplitude = formant(600.0, 150.0) + 0.7 * formant(1'400.0, 200.0) +
                                         0.4 * formant(2'400.0, 250.0) + 0.05;
                sum += amplitude * phasor;
            }
            phasor *= step;
        }
        out[n] = envelope * sum;
    }
    return out;
}

// Multiplies by exp(+j*2*pi*f*n/rate), with the phase reduced exactly in
// integers so a long capture does not lose it to a large argument.
void shift_by(std::span<std::complex<double>> samples, dsp::Hertz frequency) {
    for (std::size_t n = 0; n < samples.size(); ++n) {
        std::int64_t turns = (frequency * static_cast<std::int64_t>(n)) % kRate;
        if (turns < 0) {
            turns += kRate;
        }
        const double angle =
            2.0 * std::numbers::pi * static_cast<double>(turns) / static_cast<double>(kRate);
        samples[n] *= std::polar(1.0, angle);
    }
}

[[nodiscard]] double rms(std::span<const std::complex<double>> samples) {
    double sum = 0.0;
    for (const auto& s : samples) {
        sum += std::norm(s);
    }
    return std::sqrt(sum / static_cast<double>(samples.size()));
}

[[nodiscard]] std::vector<dsp::Complex32> to_float(std::span<const std::complex<double>> in) {
    std::vector<dsp::Complex32> out(in.size());
    for (std::size_t n = 0; n < in.size(); ++n) {
        out[n] = dsp::Complex32{static_cast<float>(in[n].real()), static_cast<float>(in[n].imag())};
    }
    return out;
}

// The voice alone, each impairment added to it alone, and all three
// together. The impairments are the same samples wherever they appear, so a
// stage measured on its own impairment and on the dirty capture is measured
// against the same noise.
struct Captures {
    std::vector<dsp::Complex32> clean;
    std::vector<dsp::Complex32> noisy;
    std::vector<dsp::Complex32> whistle;
    std::vector<dsp::Complex32> impulsive;
    std::vector<dsp::Complex32> dirty;
};

[[nodiscard]] Captures render_captures() {
    const auto count = static_cast<std::size_t>(kSeconds * static_cast<double>(kRate));
    auto voice = render_voice(count);
    shift_by(voice, kCarrierHz);

    // A weak station, 60 dB under full scale, so an impulse at kImpulseHeight
    // times it reaches full scale, which is where ignition noise on a real
    // front end ends up.
    const double scale = kVoiceRms / rms(voice);
    for (auto& s : voice) {
        s *= scale;
    }
    const double voice_rms = rms(voice);

    Captures out;
    out.clean = to_float(voice);

    std::vector<std::complex<double>> impulses(count, {0.0, 0.0});
    std::mt19937_64 engine(kSeed);
    std::uniform_int_distribution<std::size_t> where(0, count - 4U);
    std::uniform_real_distribution<double> phase(0.0, 2.0 * std::numbers::pi);
    const auto impulse_count = static_cast<std::size_t>(kImpulsesPerSecond * kSeconds);
    for (std::size_t k = 0; k < impulse_count; ++k) {
        const std::size_t at = where(engine);
        const std::complex<double> value = std::polar(kImpulseHeight * voice_rms, phase(engine));
        for (std::size_t w = 0; w < 3; ++w) {
            impulses[at + w] += value;
        }
    }

    std::vector<std::complex<double>> whistle(count);
    for (std::size_t n = 0; n < count; ++n) {
        whistle[n] = std::complex<double>(voice_rms, 0.0);
    }
    shift_by(whistle, kCarrierHz + kWhistleHz);

    // White noise at 15 dB in 2500 Hz, calibrated on the voice alone as
    // add_awgn requires. What it added is kept, so the dirty capture carries
    // the same noise samples.
    out.noisy = out.clean;
    const auto level = siggen::NoiseLevel::snr_in_2500_hz_db(15.0);
    auto report = siggen::add_awgn(out.noisy, level, kRate, kSeed + 1U);
    INFO(test::message_of(report));
    REQUIRE(report.has_value());
    WARN(std::format("noise, input: voice RMS {:.4g}, white noise {:.2f} dB under it in 2500 Hz, "
                     "whistle at the voice's RMS, {} impulses at 40 times it",
                     voice_rms, report->snr_in_reference_bandwidth_db, impulse_count));

    auto combine = [&](bool noise, bool tone, bool clicks) {
        std::vector<dsp::Complex32> made(count);
        for (std::size_t n = 0; n < count; ++n) {
            std::complex<double> total = noise ? std::complex<double>(out.noisy[n]) : voice[n];
            if (tone) {
                total += whistle[n];
            }
            if (clicks) {
                total += impulses[n];
            }
            made[n] = dsp::Complex32{static_cast<float>(total.real()),
                                     static_cast<float>(total.imag())};
        }
        return made;
    };
    out.whistle = combine(false, true, false);
    out.impulsive = combine(false, false, true);
    out.dirty = combine(true, true, true);
    return out;
}

// One receiver the engine runs, and what came out of it.
struct Listener {
    std::string name;
    engine::VrxParams params;
    std::vector<float> audio;
    std::vector<dsp::Complex32> iq;
};

[[nodiscard]] engine::VrxParams usb() {
    engine::VrxParams params;
    params.center = kCarrierHz;
    params.demod = engine::Demod::Usb;
    params.bandwidth = 0;
    params.agc_enabled = false;
    return params;
}

// Runs one capture through the engine with every listener attached.
void run_capture(std::span<const dsp::Complex32> capture, std::vector<Listener>& listeners,
                 const std::string& tag) {
    const std::filesystem::path path =
        test::unique_temp_path(std::format("revenant_test_noise_{}", tag), ".cf32");
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

    const std::string uri = "file:///" + path.generic_string() +
                            "?rate=" + std::to_string(kRate) +
                            "&format=cf32&center=7100000";

    std::mutex lock;
    auto created = engine::Engine::create(noise_config());
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;
    const auto opened = eng.open_source(uri);
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    for (auto& listener : listeners) {
        INFO(listener.name);
        const auto id = eng.add_vrx(listener.params);
        INFO(test::message_of(id));
        REQUIRE(id.has_value());
        const bool complex = engine::is_complex_tap(listener.params.demod);
        Listener* const into = &listener;
        REQUIRE(eng.set_audio_sink(*id,
                                   [&lock, into, complex](const engine::AudioChunk& chunk)
                                       -> Status {
                                       const std::lock_guard<std::mutex> guard(lock);
                                       if (complex) {
                                           for (std::size_t i = 0; i + 1U < chunk.samples.size();
                                                i += 2U) {
                                               into->iq.push_back(dsp::Complex32{
                                                   chunk.samples[i], chunk.samples[i + 1U]});
                                           }
                                       } else {
                                           into->audio.insert(into->audio.end(),
                                                              chunk.samples.begin(),
                                                              chunk.samples.end());
                                       }
                                       return {};
                                   })
                    .has_value());
    }

    const auto ran = eng.run();
    INFO(test::message_of(ran));
    REQUIRE(ran.has_value());
}

struct Snr {
    double db = 0.0;
    std::ptrdiff_t lag = 0;
};

// The reference's power over what a least-squares copy of it leaves
// unexplained in `out`, at the best lag in the two windows the stages can
// introduce: a few dozen samples either side, which is where the blanker's
// settling puts a receiver started with it on, and one noise reduction frame
// later.
[[nodiscard]] Snr output_snr(std::span<const float> reference, std::span<const float> out,
                             std::size_t skip) {
    auto score = [&](std::ptrdiff_t lag, double& gain) {
        double cross = 0.0;
        double self = 0.0;
        const auto length = static_cast<std::ptrdiff_t>(std::min(reference.size(), out.size()));
        for (std::ptrdiff_t n = static_cast<std::ptrdiff_t>(skip); n + 1100 < length; ++n) {
            const std::ptrdiff_t m = n + lag;
            if (m < 0 || m >= static_cast<std::ptrdiff_t>(out.size())) {
                continue;
            }
            cross += static_cast<double>(out[static_cast<std::size_t>(m)]) *
                     reference[static_cast<std::size_t>(n)];
            self += static_cast<double>(reference[static_cast<std::size_t>(n)]) *
                    reference[static_cast<std::size_t>(n)];
        }
        gain = self > 0.0 ? cross / self : 0.0;
        return cross;
    };

    Snr best;
    double best_cross = -1.0;
    double best_gain = 0.0;
    auto consider = [&](std::ptrdiff_t lag) {
        double gain = 0.0;
        const double cross = score(lag, gain);
        if (std::abs(cross) > best_cross) {
            best_cross = std::abs(cross);
            best.lag = lag;
            best_gain = gain;
        }
    };
    for (std::ptrdiff_t lag = -80; lag <= 80; ++lag) {
        consider(lag);
    }
    for (std::ptrdiff_t lag = 512 - 80; lag <= 512 + 80; ++lag) {
        consider(lag);
    }

    double signal = 0.0;
    double residual = 0.0;
    const auto length = static_cast<std::ptrdiff_t>(std::min(reference.size(), out.size()));
    for (std::ptrdiff_t n = static_cast<std::ptrdiff_t>(skip); n + 1100 < length; ++n) {
        const std::ptrdiff_t m = n + best.lag;
        if (m < 0 || m >= static_cast<std::ptrdiff_t>(out.size())) {
            continue;
        }
        const double r = best_gain * reference[static_cast<std::size_t>(n)];
        const double e = out[static_cast<std::size_t>(m)] - r;
        signal += r * r;
        residual += e * e;
    }
    best.db = 10.0 * std::log10(signal / residual);
    return best;
}

// The blanker's twin over a linear stream, as though the stream were one
// channel of a ring long enough to hold it.
[[nodiscard]] std::vector<dsp::Complex32> blank_stream(std::span<const dsp::Complex32> stream,
                                                       dsp::SampleRate channel_rate,
                                                       std::size_t& first_valid) {
    dsp::BlankerConfig config = dsp::design_blanker(channel_rate);
    const std::uint32_t reach = dsp::blanker_reach_below(config);
    const std::size_t ring = std::bit_ceil(stream.size() + reach + config.lead + 64U);
    std::vector<dsp::Complex32> channel(ring, dsp::Complex32{});
    std::copy(stream.begin(), stream.end(), channel.begin());

    dsp::BlankerParams params;
    params.chan_mask = static_cast<std::uint32_t>(ring - 1U);
    params.out_mask = params.chan_mask;
    params.threshold = dsp::blanker_threshold(engine::VrxParams{}.nb_threshold_db);

    std::vector<float> flags(ring, 0.0F);
    params.chan_first = reach;
    params.out_first = reach;
    params.count = static_cast<std::uint32_t>(stream.size() - reach);
    config.pass = dsp::kBlankDetect;
    REQUIRE(dsp::reference_blank_detect(config, params, channel, flags).has_value());

    std::vector<dsp::Complex32> blanked(ring, dsp::Complex32{});
    first_valid = reach + config.hang;
    params.chan_first = static_cast<std::uint32_t>(first_valid);
    params.out_first = params.chan_first;
    params.count = static_cast<std::uint32_t>(stream.size() - first_valid - config.lead);
    config.pass = dsp::kBlankApply;
    REQUIRE(dsp::reference_blank_apply(config, params, channel, flags, blanked).has_value());
    blanked.resize(stream.size() - config.lead);
    return blanked;
}

// Energy of a channel-rate stream inside the voice's audio band, which sits
// at the residual plus 300 to 2700 Hz in the raw tap's channel. A complex
// bandpass, a Hamming-windowed lowpass of 1200 Hz half-width moved to the
// band's centre.
[[nodiscard]] double in_band_energy(std::span<const std::complex<double>> stream,
                                    dsp::SampleRate channel_rate, double residual_hz,
                                    std::size_t first, std::size_t end) {
    constexpr std::size_t kTaps = 511;
    const double fs = static_cast<double>(channel_rate);
    const double centre = residual_hz + 1'500.0;
    const double cutoff = 1'200.0 / fs;
    std::vector<std::complex<double>> taps(kTaps);
    const double mid = (kTaps - 1) / 2.0;
    for (std::size_t i = 0; i < kTaps; ++i) {
        const double x = static_cast<double>(i) - mid;
        const double sinc = x == 0.0 ? 2.0 * cutoff
                                     : std::sin(2.0 * std::numbers::pi * cutoff * x) /
                                           (std::numbers::pi * x);
        const double window =
            0.54 - 0.46 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                   static_cast<double>(kTaps - 1));
        taps[i] = sinc * window * std::polar(1.0, 2.0 * std::numbers::pi * centre * x / fs);
    }
    double energy = 0.0;
    for (std::size_t n = first + kTaps; n < end; ++n) {
        std::complex<double> acc{0.0, 0.0};
        for (std::size_t k = 0; k < kTaps; ++k) {
            acc += taps[k] * stream[n - k];
        }
        energy += std::norm(acc);
    }
    return energy;
}

}  // namespace

TEST_CASE("each noise stage measured on a synthetic SSB voice", "[gpu][engine][noise]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    const Captures captures = render_captures();

    // --- the reference, and the raw tap for the impulse figure -------------
    engine::VrxParams raw;
    raw.center = kCarrierHz;
    raw.demod = engine::Demod::Raw;

    // Each stage on the clean capture as well, which is what it costs the
    // voice when there is nothing for it to remove.
    std::vector<Listener> clean{{"reference", usb(), {}, {}}, {"raw tap", raw, {}, {}}};
    {
        auto p = usb();
        p.nb_enabled = true;
        clean.push_back({"noise blanker", p, {}, {}});
        p = usb();
        p.auto_notch_enabled = true;
        clean.push_back({"automatic notch", p, {}, {}});
        p = usb();
        p.nr_enabled = true;
        clean.push_back({"noise reduction 0.5", p, {}, {}});
    }
    run_capture(captures.clean, clean, "clean");
    const std::vector<float>& reference = clean[0].audio;
    REQUIRE(reference.size() > 96'000U);
    const std::size_t skip = static_cast<std::size_t>(kSettleSeconds * 48'000.0);
    std::vector<Snr> clean_scores;
    for (std::size_t i = 2; i < clean.size(); ++i) {
        const Snr snr = output_snr(reference, clean[i].audio, skip);
        clean_scores.push_back(snr);
        WARN(std::format("noise, {:<9} capture, {:<44} output SNR {:6.2f} dB, lag {}", "clean",
                         clean[i].name, snr.db, snr.lag));
    }
    // The blanker finds nothing in a clean capture and must leave it alone.
    CHECK(clean_scores[0].db > 40.0);

    // --- each impairment against the stage that exists for it ---------------
    //
    // One engine run per capture, one receiver per configuration, all on the
    // same carrier, so every receiver in a run heard the same samples.
    struct Config {
        const char* name;
        bool nb = false;
        bool notch = false;
        bool auto_notch = false;
        bool nr = false;
        double strength = 0.5;
    };
    auto listeners_for = [](std::span<const Config> configs) {
        std::vector<Listener> out;
        for (const Config& c : configs) {
            auto p = usb();
            p.nb_enabled = c.nb;
            p.notch_enabled = c.notch;
            p.notch_hz = kWhistleHz;
            p.auto_notch_enabled = c.auto_notch;
            p.nr_enabled = c.nr;
            p.nr_strength = c.strength;
            out.push_back({c.name, p, {}, {}});
        }
        return out;
    };
    auto measure = [&](std::span<const dsp::Complex32> capture, std::span<const Config> configs,
                       const char* tag, bool raw_tap, std::vector<Listener>* keep) {
        std::vector<Listener> listeners = listeners_for(configs);
        if (raw_tap) {
            listeners.push_back({"raw tap", raw, {}, {}});
        }
        run_capture(capture, listeners, tag);
        std::vector<Snr> scores;
        for (std::size_t i = 0; i < configs.size(); ++i) {
            const Snr snr = output_snr(reference, listeners[i].audio, skip);
            scores.push_back(snr);
            WARN(std::format("noise, {:<9} capture, {:<44} output SNR {:6.2f} dB, lag {}", tag,
                             listeners[i].name, snr.db, snr.lag));
        }
        if (keep != nullptr) {
            *keep = std::move(listeners);
        }
        return scores;
    };

    const Config noisy_configs[] = {
        {"all off"},
        {"noise reduction 0", false, false, false, true, 0.0},
        {"noise reduction 0.5", false, false, false, true, 0.5},
        {"noise reduction 1", false, false, false, true, 1.0},
    };
    std::vector<Listener> noisy_kept;
    const auto noisy = measure(captures.noisy, noisy_configs, "noisy", false, &noisy_kept);

    // What the output SNR does not show: how far the noise drops where there
    // is no voice to distort. The pauses are where the clean reference's
    // 10 ms envelope is under a hundredth of its RMS, and each receiver's
    // output there is compared with the untouched receiver's.
    {
        constexpr std::size_t kWindow = 480;
        double total = 0.0;
        for (const float v : reference) {
            total += static_cast<double>(v) * v;
        }
        const double floor = 1.0e-4 * total / static_cast<double>(reference.size());
        std::vector<bool> pause(reference.size(), false);
        double running = 0.0;
        for (std::size_t n = 0; n < reference.size(); ++n) {
            running += static_cast<double>(reference[n]) * reference[n];
            if (n >= kWindow) {
                running -= static_cast<double>(reference[n - kWindow]) * reference[n - kWindow];
            }
            if (n >= kWindow && running / kWindow < floor) {
                for (std::size_t k = n - kWindow; k <= n; ++k) {
                    pause[k] = true;
                }
            }
        }
        auto pause_power = [&](const Listener& listener, std::ptrdiff_t lag) {
            double sum = 0.0;
            std::size_t count = 0;
            for (std::size_t n = skip; n < reference.size(); ++n) {
                const std::ptrdiff_t m = static_cast<std::ptrdiff_t>(n) + lag;
                if (!pause[n] || m < 0 ||
                    m >= static_cast<std::ptrdiff_t>(listener.audio.size())) {
                    continue;
                }
                sum += static_cast<double>(listener.audio[static_cast<std::size_t>(m)]) *
                       listener.audio[static_cast<std::size_t>(m)];
                ++count;
            }
            return count == 0 ? 0.0 : sum / static_cast<double>(count);
        };
        const double off_power = pause_power(noisy_kept[0], noisy[0].lag);
        for (std::size_t i = 1; i < noisy_kept.size(); ++i) {
            const double drop =
                10.0 * std::log10(pause_power(noisy_kept[i], noisy[i].lag) / off_power);
            WARN(std::format("noise, noisy     capture, {:<44} noise in the pauses {:6.2f} dB",
                             noisy_kept[i].name, drop));
            CHECK(drop < -3.0);
        }
    }

    const Config whistle_configs[] = {
        {"all off"},
        {"manual notch at the whistle", false, true, false, false},
        {"automatic notch", false, false, true, false},
        {"both notches", false, true, true, false},
    };
    const auto whistle = measure(captures.whistle, whistle_configs, "whistle", false, nullptr);

    const Config impulsive_configs[] = {
        {"all off"},
        {"noise blanker", true, false, false, false},
    };
    std::vector<Listener> impulsive_kept;
    const auto impulsive =
        measure(captures.impulsive, impulsive_configs, "impulsive", true, &impulsive_kept);

    const Config dirty_configs[] = {
        {"all off"},
        {"noise blanker", true, false, false, false},
        {"manual notch at the whistle", false, true, false, false},
        {"automatic notch", false, false, true, false},
        {"noise reduction 0.5", false, false, false, true},
        {"blanker, manual notch, noise reduction", true, true, false, true},
        {"blanker, automatic notch, noise reduction", true, false, true, true},
    };
    const auto dirty = measure(captures.dirty, dirty_configs, "dirty", false, nullptr);

    // --- impulse energy removed, on the coarse channel --------------------
    const auto& tap_clean = clean[1].iq;
    const auto& tap_impulsive = impulsive_kept.back().iq;
    const std::size_t taps = std::min(tap_clean.size(), tap_impulsive.size());
    REQUIRE(taps > 100'000U);
    const dsp::SampleRate channel_rate = 2 * kSpacing;

    std::size_t first_valid = 0;
    const auto blanked = blank_stream(std::span(tap_impulsive).first(taps), channel_rate,
                                      first_valid);
    std::size_t clean_first = 0;
    const auto clean_blanked =
        blank_stream(std::span(tap_clean).first(taps), channel_rate, clean_first);

    // Twice: across the whole channel, and inside the voice's band, which is
    // the part a receiver hears. In the band, what is left after blanking
    // counts the voice the blanker cut along with the impulses it missed,
    // because both are the difference from the clean tap.
    const std::size_t end = std::min(blanked.size(), clean_blanked.size());
    std::vector<std::complex<double>> impulse_in(end, {0.0, 0.0});
    std::vector<std::complex<double>> impulse_left(end, {0.0, 0.0});
    std::vector<std::complex<double>> voice(end, {0.0, 0.0});
    std::vector<std::complex<double>> voice_cut(end, {0.0, 0.0});
    std::size_t impulsive_zeroed = 0;
    for (std::size_t n = first_valid; n < end; ++n) {
        const std::complex<double> reference_sample(tap_clean[n]);
        impulse_in[n] = std::complex<double>(tap_impulsive[n]) - reference_sample;
        impulse_left[n] = std::complex<double>(blanked[n]) - reference_sample;
        voice[n] = reference_sample;
        voice_cut[n] = std::complex<double>(clean_blanked[n]) - reference_sample;
        impulsive_zeroed += (blanked[n] == dsp::Complex32{}) ? 1U : 0U;
    }
    auto energy = [&](const std::vector<std::complex<double>>& x) {
        double sum = 0.0;
        for (std::size_t n = first_valid; n < end; ++n) {
            sum += std::norm(x[n]);
        }
        return sum;
    };
    const double removed_db = 10.0 * std::log10(energy(impulse_in) / energy(impulse_left));
    // The raw tap does not mix, so the voice's carrier sits where the grid
    // left it: 9 kHz above the centre of channel 2.
    const auto in_band = [&](const std::vector<std::complex<double>>& x) {
        return in_band_energy(x, channel_rate, 9'000.0, first_valid, end);
    };
    const double band_voice = in_band(voice);
    const double band_before_db = 10.0 * std::log10(band_voice / in_band(impulse_in));
    const double band_after_db = 10.0 * std::log10(band_voice / in_band(impulse_left));
    const double voice_cut_db =
        10.0 * std::log10(std::max(in_band(voice_cut), 1.0e-30) / band_voice);
    WARN(std::format("noise, blanker across the coarse channel: {:.2f} dB of the impulse energy "
                     "removed, {} of {} samples zeroed",
                     removed_db, impulsive_zeroed, end - first_valid));
    WARN(std::format("noise, blanker in the voice's band: voice to impulse {:.2f} dB before, "
                     "{:.2f} dB after, {:.2f} dB removed",
                     band_before_db, band_after_db, band_after_db - band_before_db));
    WARN(std::format("noise, blanker on the clean channel: it cut the voice's band by {:.1f} dB",
                     voice_cut_db));

    // Loose, on purpose. Each stage has to help against what it is for, and
    // none may make the dirty capture worse.
    CHECK(noisy[2].db > noisy[0].db);
    CHECK(whistle[1].db > whistle[0].db + 10.0);
    CHECK(whistle[2].db > whistle[0].db + 10.0);
    CHECK(impulsive[1].db > impulsive[0].db + 10.0);
    CHECK(removed_db > 10.0);
    CHECK(band_after_db > band_before_db + 20.0);
    CHECK(voice_cut_db < -30.0);
    for (std::size_t i = 1; i < dirty.size(); ++i) {
        INFO(dirty_configs[i].name);
        CHECK(dirty[i].db > dirty[0].db);
    }

    // The noise reduction's frame is a delay and the notches add none.
    CHECK(std::abs(noisy[2].lag - 512) < 80);
    CHECK(std::abs(whistle[1].lag) < 80);
}

TEST_CASE("the noise fields ride a retune and a mode change and come back on the status",
          "[gpu][engine][noise]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    auto created = engine::Engine::create(noise_config());
    REQUIRE(created.has_value());
    auto& eng = **created;
    const std::string uri = "synthetic:wideband?rate=" + std::to_string(kRate) +
                            "&emitters=1&modes=nfm&seed=515151&samples=400000";
    const auto opened = eng.open_source(uri);
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());

    engine::VrxParams params = usb();
    const auto id = eng.add_vrx(params);
    INFO(test::message_of(id));
    REQUIRE(id.has_value());

    // Every stage on, every figure moved off its default: a retune in place,
    // because noise fields are tuning and never shape.
    params.nb_enabled = true;
    params.nb_threshold_db = 18.0;
    params.notch_enabled = true;
    params.notch_hz = 1'234;
    params.notch_depth_db = 30.0;
    params.notch_width_hz = 80;
    params.auto_notch_enabled = true;
    params.nr_enabled = true;
    params.nr_strength = 0.8;
    const auto tuned = eng.set_vrx_params(*id, params);
    INFO(test::message_of(tuned));
    REQUIRE(tuned.has_value());

    auto same_noise = [](const engine::VrxParams& a, const engine::VrxParams& b) {
        return a.nb_enabled == b.nb_enabled && a.nb_threshold_db == b.nb_threshold_db &&
               a.notch_enabled == b.notch_enabled && a.notch_hz == b.notch_hz &&
               a.notch_depth_db == b.notch_depth_db && a.notch_width_hz == b.notch_width_hz &&
               a.auto_notch_enabled == b.auto_notch_enabled && a.nr_enabled == b.nr_enabled &&
               a.nr_strength == b.nr_strength;
    };
    auto status = eng.vrx_status(*id);
    REQUIRE(status.has_value());
    CHECK(same_noise(status->params, params));

    // Moving the dial carries them.
    params.center += 400;
    REQUIRE(eng.set_vrx_params(*id, params).has_value());
    status = eng.vrx_status(*id);
    REQUIRE(status.has_value());
    CHECK(status->params.center == params.center);
    CHECK(same_noise(status->params, params));

    // A mode change is a remove and an add, and the fields go with the
    // request: AM takes every one of them.
    REQUIRE(eng.remove_vrx(*id).has_value());
    engine::VrxParams am = params;
    am.demod = engine::Demod::Am;
    am.notch_hz = -1'234;
    const auto am_id = eng.add_vrx(am);
    INFO(test::message_of(am_id));
    REQUIRE(am_id.has_value());
    status = eng.vrx_status(*am_id);
    REQUIRE(status.has_value());
    CHECK(same_noise(status->params, am));

    // CW refuses the automatic notch by name rather than cancelling the
    // tone the operator is copying.
    engine::VrxParams cw = am;
    cw.demod = engine::Demod::Cw;
    cw.notch_hz = 0;
    const auto refused = eng.add_vrx(cw);
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);
    CHECK(refused.error().message.find("cw") != std::string::npos);
    CHECK(refused.error().message.find("automatic notch") != std::string::npos);

    // And takes the rest.
    cw.auto_notch_enabled = false;
    const auto cw_id = eng.add_vrx(cw);
    INFO(test::message_of(cw_id));
    REQUIRE(cw_id.has_value());
    status = eng.vrx_status(*cw_id);
    REQUIRE(status.has_value());
    CHECK(same_noise(status->params, cw));

    // A figure out of range is refused on the call, not dropped on the
    // recording thread.
    engine::VrxParams wild = cw;
    wild.nr_strength = 1.5;
    const auto wild_status = eng.set_vrx_params(*cw_id, wild);
    REQUIRE_FALSE(wild_status.has_value());
    CHECK(wild_status.error().message.find("strength") != std::string::npos);

    // Audio still comes out of a receiver running all of it.
    std::mutex lock;
    std::size_t frames = 0;
    std::size_t nonzero = 0;
    REQUIRE(eng.set_audio_sink(*am_id,
                               [&](const engine::AudioChunk& chunk) -> Status {
                                   const std::lock_guard<std::mutex> guard(lock);
                                   frames += chunk.samples.size();
                                   for (const float v : chunk.samples) {
                                       nonzero += (v != 0.0F) ? 1U : 0U;
                                   }
                                   return {};
                               })
                .has_value());
    REQUIRE(eng.run().has_value());
    INFO(frames << " frames, " << nonzero << " not zero");
    CHECK(frames > 10'000U);
    CHECK(nonzero > frames / 2U);
    CHECK(eng.graph_conditions().vrx_retune_refusals == 0U);
}

TEST_CASE("a stage switched on mid-stream starts from rest and keeps the audio flowing",
          "[gpu][engine][noise]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    auto created = engine::Engine::create(noise_config());
    REQUIRE(created.has_value());
    auto& eng = **created;
    const std::string uri = "synthetic:wideband?rate=" + std::to_string(kRate) +
                            "&emitters=1&modes=nfm&seed=525252&samples=1200000";
    REQUIRE(eng.open_source(uri).has_value());

    engine::VrxParams params = usb();
    const auto id = eng.add_vrx(params);
    REQUIRE(id.has_value());

    // Switches every stage on after the first quarter second of audio and
    // off again after the next, from the sink, the way a click in the
    // client arrives while the stream runs.
    std::mutex lock;
    std::size_t frames = 0;
    std::size_t switched = 0;
    Status first_switch;
    Status second_switch;
    REQUIRE(eng.set_audio_sink(*id, [&](const engine::AudioChunk& chunk) -> Status {
                 const std::lock_guard<std::mutex> guard(lock);
                 frames += chunk.samples.size();
                 if (switched == 0 && frames > 12'000U) {
                     engine::VrxParams on = params;
                     on.nb_enabled = true;
                     on.auto_notch_enabled = true;
                     on.notch_enabled = true;
                     on.nr_enabled = true;
                     first_switch = eng.set_vrx_params(*id, on);
                     switched = 1;
                 } else if (switched == 1 && frames > 24'000U) {
                     second_switch = eng.set_vrx_params(*id, params);
                     switched = 2;
                 }
                 return {};
             }).has_value());
    REQUIRE(eng.run().has_value());

    INFO(test::message_of(first_switch));
    CHECK(first_switch.has_value());
    INFO(test::message_of(second_switch));
    CHECK(second_switch.has_value());
    CHECK(switched == 2U);
    CHECK(frames > 40'000U);
    const auto status = eng.vrx_status(*id);
    REQUIRE(status.has_value());
    CHECK(status->reanchors == 0U);
    CHECK(eng.graph_conditions().vrx_retune_refusals == 0U);
}
