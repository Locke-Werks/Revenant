// Reference floating point discipline, before anything else in the file.
#include "core/dsp/reference_fp.h"

#include "core/dsp/synth/wideband.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <random>
#include <system_error>
#include <thread>
#include <utility>

namespace revenant::siggen {

static_assert(dsp::kReferenceFpDisciplineApplied,
              "wideband.cpp must include core/dsp/reference_fp.h first");

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// 2^-53, the step of a double drawn from the top 53 bits of a 64-bit word.
constexpr double kMantissaStep = 1.0 / 9007199254740992.0;

// Noise is generated in absolutely aligned blocks of this many samples, each
// from its own freshly seeded engine. That is what makes the noise a function
// of the absolute sample index rather than of how many samples happen to have
// been drawn before it.
//
// The size trades two things off. Seeding a Mersenne Twister costs about 600
// word operations, which over 4096 samples disappears. And a render that does
// not start on a block boundary has to discard draws to reach its start, which
// this bounds at 8190 draws, once per call. Block-aligned rendering, which is
// every real caller, discards nothing.
constexpr SampleIndex kNoiseBlockSamples = 4096;

// Domain tags, so adding a stage does not shift another stage's stream and
// stop a recorded scene reproducing.
constexpr std::uint64_t kLayoutDomain = 0x1a'59'01'7e'55'ce'0d'01ull;
constexpr std::uint64_t kNoiseDomain = 0x1a'59'01'7e'55'ce'0d'02ull;

// SplitMix64's finaliser. Used only to derive seeds, never to generate a
// sample: every actual draw goes through std::mt19937_64. Named apart from
// channel.h's derive_seed so the two translation units cannot collide.
[[nodiscard]] std::uint64_t derive_stream_seed(std::uint64_t master, std::uint64_t domain)
{
    std::uint64_t z = master + domain * 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Everything below draws from the engine by hand rather than through
// std::uniform_real_distribution, std::uniform_int_distribution or
// std::normal_distribution.
//
// The standard specifies what those distributions produce but not how they
// consume the engine, so the same seed lays out a different scene on libstdc++
// than it does on the MSVC standard library. A seed printed in a failing test
// log has to reproduce that failure on the machine the fix gets written on.

[[nodiscard]] double uniform_unit(std::mt19937_64& engine)
{
    return static_cast<double>(engine() >> 11) * kMantissaStep;
}

[[nodiscard]] double uniform_range(std::mt19937_64& engine, double low, double high)
{
    return low + (high - low) * uniform_unit(engine);
}

// Uniform over [0, bound), by rejection rather than modulo so the distribution
// is exact.
[[nodiscard]] std::uint64_t uniform_below(std::mt19937_64& engine, std::uint64_t bound)
{
    if (bound <= 1) {
        return 0;
    }
    const std::uint64_t buckets = std::numeric_limits<std::uint64_t>::max() / bound;
    const std::uint64_t ceiling = buckets * bound;
    std::uint64_t draw = engine();
    while (draw >= ceiling) {
        draw = engine();
    }
    return draw / buckets;
}

// Uniform over [low, high], inclusive.
[[nodiscard]] std::int64_t uniform_between(std::mt19937_64& engine,
                                           std::int64_t low,
                                           std::int64_t high)
{
    if (high <= low) {
        return low;
    }
    const auto span = static_cast<std::uint64_t>(high - low);
    return low + static_cast<std::int64_t>(uniform_below(engine, span + 1));
}

template <class T>
[[nodiscard]] T pick(std::mt19937_64& engine, std::initializer_list<T> choices)
{
    const auto index = static_cast<std::size_t>(uniform_below(engine, choices.size()));
    return *(choices.begin() + static_cast<std::ptrdiff_t>(index));
}

[[nodiscard]] double db_to_power(double decibels)
{
    return std::pow(10.0, decibels * 0.1);
}

[[nodiscard]] double power_to_db(double power)
{
    if (power <= 0.0) {
        return -std::numeric_limits<double>::infinity();
    }
    return 10.0 * std::log10(power);
}

// Plausible parameters per mode, so a random scene looks like a band rather
// than like a parameter sweep. The values are the ones that actually appear on
// the air: 1200 and 9600 baud packet, 2.5 and 5 kHz FM deviation, 0.35 rolloff.
void randomise_mode(ModulatorSpec& spec, std::mt19937_64& engine)
{
    switch (spec.kind) {
    case Modulation::Cw:
        spec.cw.words_per_minute = static_cast<double>(pick(engine, {12, 18, 20, 25, 30}));
        spec.cw.rise_fall_ms = 5.0;
        spec.cw.text = "CQ DE REVENANT";
        break;

    case Modulation::Am:
        spec.am.modulation_index = uniform_range(engine, 0.3, 0.9);
        spec.am.tone_hz = pick<Hertz>(engine, {400, 1000, 1500, 2500});
        break;

    case Modulation::Nfm:
        spec.nfm.deviation = pick<Hertz>(engine, {2500, 5000});
        spec.nfm.tone_hz = pick<Hertz>(engine, {400, 1000, 1500});
        break;

    case Modulation::Usb:
    case Modulation::Lsb:
        spec.ssb.tone_hz = pick<Hertz>(engine, {700, 1000, 1500, 2200});
        // Two tones some of the time, which is the standard linearity test
        // signal and has a very different envelope from one tone.
        spec.ssb.tone2_hz = (uniform_unit(engine) < 0.25) ? Hertz{1900} : Hertz{0};
        if (spec.ssb.tone2_hz == spec.ssb.tone_hz) {
            spec.ssb.tone2_hz = 0;
        }
        break;

    case Modulation::Fsk2: {
        const auto baud = pick<Hertz>(engine, {1200, 2400, 4800, 9600});
        spec.fsk.symbol_rate = static_cast<double>(baud);
        // Modulation index 0.5 is MSK, 1.0 is the common packet case, 2.0 is a
        // wide land mobile signal.
        const double index = pick(engine, {0.5, 1.0, 2.0});
        spec.fsk.deviation = static_cast<Hertz>(std::llround(0.5 * index * static_cast<double>(baud)));
        spec.fsk.symbol_count = 256;
        break;
    }

    case Modulation::Bpsk:
    case Modulation::Qpsk: {
        const auto baud = pick<Hertz>(engine, {1200, 2400, 4800, 9600, 19200});
        spec.psk.symbol_rate = static_cast<double>(baud);
        spec.psk.rolloff = pick(engine, {0.2, 0.35, 0.5});
        spec.psk.symbol_count = 256;
        break;
    }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Expected<Scene> Scene::create(const SceneSpec& spec)
{
    if (spec.rate <= 0 || spec.rate > kMaxSampleRate) {
        return fail(std::format("scene sample rate {} is outside 1 to {} S/s",
                                spec.rate, kMaxSampleRate));
    }
    if (!std::isfinite(spec.noise_power_full_band_dbfs)) {
        return fail("noise floor must be finite");
    }

    Scene scene;
    scene.rate_ = spec.rate;
    scene.center_hz_ = spec.center_hz;
    scene.duration_ = spec.duration_samples;
    scene.epoch_anchor_ns_ = spec.epoch_anchor_ns;
    scene.seed_ = spec.seed;
    scene.worker_threads_ = spec.worker_threads;
    scene.add_noise_ = spec.add_noise;
    scene.noise_power_ = spec.add_noise ? db_to_power(spec.noise_power_full_band_dbfs) : 0.0;
    // The total power is split evenly between the real and imaginary parts, so
    // each is Gaussian with variance noise_power/2.
    scene.noise_sigma_ = std::sqrt(scene.noise_power_ * 0.5);

    // The level one emitter is placed at, and the noise it is placed
    // against. Shared by the Modulator path and the broadcast FM path, which
    // differ in what they render and in nothing about how they are levelled.
    struct Level {
        double gain = 1.0;
        double power = 0.0;
        double noise_in_band = 0.0;
    };

    const auto level_for = [&scene](const SpectralExtent& extent, double nominal,
                                    bool use_snr,
                                    double requested_snr_db) -> Expected<Level> {
        // Noise in the emitter's own band, which is the denominator a
        // detector's sensitivity figure is quoted against.
        const double bandwidth = std::max(1.0, static_cast<double>(extent.bandwidth_hz()));

        Level level;
        level.noise_in_band =
            scene.noise_power_ * bandwidth / static_cast<double>(scene.rate_);
        level.power = nominal;

        if (use_snr) {
            if (scene.noise_power_ <= 0.0) {
                return fail("an emitter placed by SNR needs a noise floor to be placed against");
            }
            level.power = level.noise_in_band * db_to_power(requested_snr_db);
            level.gain = (nominal > 0.0) ? std::sqrt(level.power / nominal) : 0.0;
            if (nominal <= 0.0) {
                level.power = 0.0;
            }
        }
        return level;
    };

    // Turns one emitter's truth prototype and its windows into rows, clamped
    // to the scene. Shared for the same reason level_for is: two kinds of
    // emitter, one truth table, and a second copy of this is a second
    // chance for the two to disagree about what a burst is.
    const auto add_bursts =
        [&scene, &spec](const EmitterTruth& prototype, std::size_t index, EmitterKind kind,
                        double gain,
                        const std::vector<std::pair<SampleIndex, SampleIndex>>& windows) {
            for (const auto& window : windows) {
                SampleIndex start = window.first;
                SampleIndex end = window.second;
                if (spec.duration_samples > 0) {
                    if (start >= spec.duration_samples) {
                        continue;  // entirely past the end of the scene
                    }
                    end = std::min(end, spec.duration_samples);
                }
                if (end <= start) {
                    continue;
                }

                EmitterTruth record = prototype;
                record.start_sample = start;
                record.end_sample = end;

                scene.truth_.push_back(record);
                scene.bursts_.push_back(Burst{index, kind, start, end, gain});
            }
        };

    // Adds one slot's modulator and the bursts that run it. Shared by the
    // explicit and the random paths so both produce identical truth records.
    const auto add_slot = [&scene, &level_for, &add_bursts](
                              ModulatorSpec modulator_spec,
                              bool use_snr,
                              double requested_snr_db,
                              std::uint64_t payload_seed,
                              const std::vector<std::pair<SampleIndex, SampleIndex>>& windows)
        -> Status {
        auto modulator = Modulator::create(std::move(modulator_spec));
        if (!modulator) {
            return std::unexpected(modulator.error());
        }

        const SpectralExtent extent = modulator->occupied_extent();
        Expected<Level> level =
            level_for(extent, modulator->nominal_mean_power(), use_snr, requested_snr_db);
        if (!level) {
            return std::unexpected(level.error());
        }

        EmitterTruth prototype;
        prototype.kind = EmitterKind::Modulated;
        prototype.modulation = modulator->kind();
        prototype.carrier_offset_hz = modulator->spec().common.carrier_offset;
        prototype.extent = extent;
        prototype.mean_power = level->power;
        prototype.snr_in_occupied_bandwidth_db =
            (level->noise_in_band > 0.0) ? power_to_db(level->power / level->noise_in_band)
                                         : 0.0;
        prototype.snr_in_full_band_db =
            (scene.noise_power_ > 0.0) ? power_to_db(level->power / scene.noise_power_) : 0.0;
        prototype.symbol_rate_baud = modulator->effective_symbol_rate();
        prototype.payload_seed = payload_seed;

        const std::size_t index = scene.modulators_.size();
        scene.modulators_.push_back(std::move(*modulator));
        add_bursts(prototype, index, EmitterKind::Modulated, level->gain, windows);
        return Status{};
    };

    // And the same for a broadcast FM station. Placed by hand only: see
    // WfmStationPlacement for why there is no random path here.
    const auto add_station = [&scene, &spec, &level_for, &add_bursts](
                                 WfmStationPlacement placement) -> Status {
        if (placement.end_sample <= placement.start_sample) {
            return fail(std::format("FM station window [{}, {}) is empty",
                                    placement.start_sample, placement.end_sample));
        }

        // One stream, one sample rate.
        placement.station.rate = spec.rate;
        auto station = WfmModulator::create(std::move(placement.station));
        if (!station) {
            return std::unexpected(station.error());
        }

        const SpectralExtent extent = station->occupied_extent();
        Expected<Level> level = level_for(extent, station->nominal_mean_power(),
                                          placement.use_snr,
                                          placement.snr_in_occupied_bandwidth_db);
        if (!level) {
            return std::unexpected(level.error());
        }

        EmitterTruth prototype;
        prototype.kind = EmitterKind::BroadcastFm;
        prototype.carrier_offset_hz = station->spec().carrier_offset;
        prototype.extent = extent;
        prototype.mean_power = level->power;
        prototype.snr_in_occupied_bandwidth_db =
            (level->noise_in_band > 0.0) ? power_to_db(level->power / level->noise_in_band)
                                         : 0.0;
        prototype.snr_in_full_band_db =
            (scene.noise_power_ > 0.0) ? power_to_db(level->power / scene.noise_power_) : 0.0;
        prototype.composite_peak_bound = station->composite().peak_bound();
        // modulation, symbol_rate_baud and payload_seed stay at their
        // defaults. EmitterTruth says why each of the three is unreadable on
        // this kind of row.

        const std::vector<std::pair<SampleIndex, SampleIndex>> windows{
            {placement.start_sample, placement.end_sample}};

        const std::size_t index = scene.stations_.size();
        scene.stations_.push_back(std::move(*station));
        add_bursts(prototype, index, EmitterKind::BroadcastFm, level->gain, windows);
        return Status{};
    };

    for (const EmitterPlacement& placement : spec.emitters) {
        if (placement.end_sample <= placement.start_sample) {
            return fail(std::format("emitter window [{}, {}) is empty",
                                    placement.start_sample, placement.end_sample));
        }
        std::vector<std::pair<SampleIndex, SampleIndex>> windows{
            {placement.start_sample, placement.end_sample}};
        auto added = add_slot(placement.modulator, placement.use_snr,
                              placement.snr_in_occupied_bandwidth_db,
                              placement.modulator.common.seed, windows);
        if (!added) {
            return std::unexpected(with_context(added.error(), "siggen scene emitter"));
        }
    }

    for (const WfmStationPlacement& placement : spec.fm_stations) {
        if (Status added = add_station(placement); !added) {
            return std::unexpected(with_context(added.error(), "siggen scene FM station"));
        }
    }

    const RandomPopulation& population = spec.random;
    if (population.emitter_count > 0) {
        if (population.span_high_hz <= population.span_low_hz) {
            return fail("random population needs an increasing placement span");
        }
        if (population.bursts_per_emitter == 0) {
            return fail("random population needs at least one burst per emitter");
        }
        if (population.bursts_per_emitter > 1 && spec.duration_samples == 0) {
            return fail("bursty emitters need a bounded scene: bursts have to be laid out "
                        "across a known duration");
        }
        if (population.snr_in_occupied_bandwidth_db_max <
            population.snr_in_occupied_bandwidth_db_min) {
            return fail("random population SNR range is inverted");
        }

        std::vector<Modulation> palette = population.palette;
        if (palette.empty()) {
            const std::span<const Modulation> every = all_modulations();
            palette.assign(every.begin(), every.end());
        }

        const SampleIndex window_samples =
            (population.bursts_per_emitter > 0)
                ? spec.duration_samples / population.bursts_per_emitter
                : 0;
        if (population.bursts_per_emitter > 1 && window_samples < 2) {
            return fail(std::format("{} bursts do not fit in a {} sample scene",
                                    population.bursts_per_emitter, spec.duration_samples));
        }

        std::mt19937_64 engine(derive_stream_seed(spec.seed, kLayoutDomain));

        for (std::size_t slot = 0; slot < population.emitter_count; ++slot) {
            ModulatorSpec modulator_spec;
            modulator_spec.kind =
                palette[static_cast<std::size_t>(uniform_below(engine, palette.size()))];
            modulator_spec.common.rate = spec.rate;
            modulator_spec.common.amplitude = 1.0;
            modulator_spec.common.initial_phase = uniform_range(engine, 0.0, kTwoPi);
            const std::uint64_t payload_seed = engine();
            modulator_spec.common.seed = payload_seed;
            randomise_mode(modulator_spec, engine);

            // Place by the occupied band, not by the carrier. The band is
            // measured with the carrier at zero and then translated, so an
            // upper sideband signal does not end up with half its energy off
            // the end of the span.
            ModulatorSpec probe = modulator_spec;
            probe.common.carrier_offset = 0;
            auto relative = occupied_extent(probe);
            if (!relative) {
                return std::unexpected(with_context(relative.error(), "siggen scene layout"));
            }

            const Hertz nyquist = spec.rate / 2;
            const Hertz lowest = std::max(population.span_low_hz, -nyquist) - relative->low_hz;
            const Hertz highest = std::min(population.span_high_hz, nyquist) - relative->high_hz;
            if (lowest > highest) {
                return fail(std::format(
                    "a {} emitter is {} Hz wide and will not fit in the {} to {} Hz span",
                    modulation_name(modulator_spec.kind), relative->bandwidth_hz(),
                    population.span_low_hz, population.span_high_hz));
            }
            modulator_spec.common.carrier_offset = uniform_between(engine, lowest, highest);

            std::vector<std::pair<SampleIndex, SampleIndex>> windows;
            double slot_snr_db = 0.0;
            if (population.bursts_per_emitter == 1 && spec.duration_samples == 0) {
                slot_snr_db = uniform_range(engine,
                                            population.snr_in_occupied_bandwidth_db_min,
                                            population.snr_in_occupied_bandwidth_db_max);
                windows.emplace_back(SampleIndex{0}, kAlwaysOn);
            } else if (population.bursts_per_emitter == 1) {
                slot_snr_db = uniform_range(engine,
                                            population.snr_in_occupied_bandwidth_db_min,
                                            population.snr_in_occupied_bandwidth_db_max);
                windows.emplace_back(SampleIndex{0}, spec.duration_samples);
            } else {
                // One burst per window, so bursts of a slot never overlap and
                // the truth table stays unambiguous about which transmission a
                // detection belongs to.
                slot_snr_db = uniform_range(engine,
                                            population.snr_in_occupied_bandwidth_db_min,
                                            population.snr_in_occupied_bandwidth_db_max);
                for (std::size_t burst = 0; burst < population.bursts_per_emitter; ++burst) {
                    const double seconds = uniform_range(engine,
                                                         population.min_burst_seconds,
                                                         population.max_burst_seconds);
                    auto length = static_cast<SampleIndex>(std::max<std::int64_t>(
                        1, std::llround(seconds * static_cast<double>(spec.rate))));
                    const SampleIndex ceiling = std::max<SampleIndex>(1, window_samples * 9 / 10);
                    length = std::min(length, ceiling);

                    const SampleIndex slack = window_samples - length;
                    const auto jitter = static_cast<SampleIndex>(
                        uniform_below(engine, static_cast<std::uint64_t>(slack) + 1));
                    const SampleIndex start =
                        window_samples * static_cast<SampleIndex>(burst) + jitter;
                    windows.emplace_back(start, start + length);
                }
            }

            auto added = add_slot(std::move(modulator_spec), true, slot_snr_db,
                                  payload_seed, windows);
            if (!added) {
                return std::unexpected(with_context(added.error(), "siggen scene emitter"));
            }
        }
    }

    // Sort by start, keeping truth_ in step, so a block render can bound its
    // search instead of scanning every emitter in the scene.
    std::vector<std::size_t> order(scene.bursts_.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(),
                     [&scene](std::size_t left, std::size_t right) {
                         return scene.bursts_[left].start < scene.bursts_[right].start;
                     });

    std::vector<Burst> sorted_bursts;
    std::vector<EmitterTruth> sorted_truth;
    sorted_bursts.reserve(order.size());
    sorted_truth.reserve(order.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        sorted_bursts.push_back(scene.bursts_[order[i]]);
        EmitterTruth record = scene.truth_[order[i]];
        record.id = static_cast<std::uint32_t>(i);
        sorted_truth.push_back(record);
    }
    scene.bursts_ = std::move(sorted_bursts);
    scene.truth_ = std::move(sorted_truth);

    for (const Burst& burst : scene.bursts_) {
        if (burst.end == kAlwaysOn) {
            scene.unbounded_burst_ = true;
        } else {
            scene.longest_burst_ = std::max(scene.longest_burst_, burst.end - burst.start);
        }
    }

    return scene;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

dsp::BlockTimestamp Scene::timestamp_at(SampleIndex start) const
{
    return dsp::BlockTimestamp{start, epoch_anchor_ns_, rate_};
}

void Scene::fill_noise(SampleIndex start, ComplexSpan out) const
{
    if (!add_noise_ || noise_sigma_ <= 0.0) {
        std::fill(out.begin(), out.end(), Complex32(0.0f, 0.0f));
        return;
    }

    const SampleIndex end = start + out.size();
    SampleIndex index = start;

    while (index < end) {
        const SampleIndex block = index / kNoiseBlockSamples;
        const SampleIndex block_start = block * kNoiseBlockSamples;
        const SampleIndex stop = std::min(end, block_start + kNoiseBlockSamples);

        std::mt19937_64 engine(derive_stream_seed(seed_ ^ block, kNoiseDomain));

        // Two draws per complex sample, always, so skipping forward to an
        // unaligned start is exact rather than approximate.
        for (SampleIndex skipped = block_start; skipped < index; ++skipped) {
            static_cast<void>(engine());
            static_cast<void>(engine());
        }

        for (; index < stop; ++index) {
            // Box-Muller, which turns two uniforms into the two independent
            // normals a complex sample needs, with no state carried between
            // samples. std::normal_distribution caches its second value, which
            // would make the noise depend on how many samples were drawn
            // before it and break block independence outright.
            const std::uint64_t first = engine();
            const std::uint64_t second = engine();

            // (0, 1]: the plus one keeps a zero out of the logarithm.
            const double radial =
                static_cast<double>((first >> 11) + 1ull) * kMantissaStep;
            const double angular = static_cast<double>(second >> 11) * kMantissaStep;

            const double magnitude = noise_sigma_ * std::sqrt(-2.0 * std::log(radial));
            const double angle = kTwoPi * angular;
            out[static_cast<std::size_t>(index - start)] =
                Complex32(static_cast<float>(magnitude * std::cos(angle)),
                          static_cast<float>(magnitude * std::sin(angle)));
        }
    }
}

void Scene::render_range(SampleIndex start, ComplexSpan out) const
{
    fill_noise(start, out);
    if (bursts_.empty()) {
        return;
    }

    const SampleIndex end = start + out.size();

    // Bursts are sorted by start, so anything that can still be running at
    // `start` began no earlier than start - longest_burst_. Searching from
    // there is exact and keeps the render a function of the absolute range
    // rather than of a cursor carried between calls, which is what lets a
    // consumer seek.
    SampleIndex window = 0;
    if (!unbounded_burst_ && start > longest_burst_) {
        window = start - longest_burst_;
    }

    const auto first = std::lower_bound(
        bursts_.begin(), bursts_.end(), window,
        [](const Burst& burst, SampleIndex value) { return burst.start < value; });

    for (auto it = first; it != bursts_.end() && it->start < end; ++it) {
        const SampleIndex low = std::max(it->start, start);
        const SampleIndex high = std::min(it->end, end);
        if (low >= high) {
            continue;
        }
        const ComplexSpan slice = out.subspan(static_cast<std::size_t>(low - start),
                                              static_cast<std::size_t>(high - low));

        // No default label: /w14062 makes a third emitter kind a build error
        // here rather than a transmission that renders as silence.
        switch (it->kind) {
            case EmitterKind::Modulated:
                modulators_[it->modulator].accumulate(low - it->start, slice, it->gain);
                break;
            case EmitterKind::BroadcastFm:
                stations_[it->modulator].accumulate(low - it->start, slice, it->gain);
                break;
        }
    }
}

std::size_t Scene::plan_workers(std::size_t count) const
{
    // Below this there is more cost in starting a thread than there is work for
    // it to do.
    //
    // 65536 was the first guess and it was far too coarse. It caps the worker
    // count at block_size/65536, so the default 262144-sample block used four
    // threads on a 32-thread machine and the generator ran at 0.14x realtime at
    // 20 MS/s. The floor is not really a sample count: each sample costs one
    // pass per emitter, so a 16384-sample slice of a 32-emitter scene is over
    // half a million sample operations, which is milliseconds of work against
    // tens of microseconds of thread startup. Measured at 20 MS/s with 32
    // emitters: 0.14x realtime at 65536, 0.55x at 16384.
    constexpr std::size_t kMinimumPerWorker = 16384;

    std::size_t requested = worker_threads_;
    if (requested == 0) {
        requested = std::thread::hardware_concurrency();
        if (requested == 0) {
            requested = 1;
        }
    }
    const std::size_t affordable = std::max<std::size_t>(1, count / kMinimumPerWorker);
    return std::min(requested, affordable);
}

void Scene::render(SampleIndex start, ComplexSpan out) const
{
    if (out.empty()) {
        return;
    }

    const std::size_t count = out.size();
    const std::size_t workers = plan_workers(count);
    if (workers <= 1) {
        render_range(start, out);
        return;
    }

    // Chunks are aligned to the noise block so a worker does not have to
    // discard draws catching up to its own start. The output does not depend on
    // the chunking either way, this only decides how much work is wasted.
    std::size_t chunk = (count + workers - 1) / workers;
    const auto alignment = static_cast<std::size_t>(kNoiseBlockSamples);
    chunk = ((chunk + alignment - 1) / alignment) * alignment;

    std::vector<std::pair<std::size_t, std::size_t>> pieces;
    for (std::size_t offset = 0; offset < count; offset += chunk) {
        pieces.emplace_back(offset, std::min(chunk, count - offset));
    }
    if (pieces.size() <= 1) {
        render_range(start, out);
        return;
    }

    std::vector<std::thread> running;
    std::size_t spawned = 0;
    try {
        running.reserve(pieces.size() - 1);
        for (std::size_t i = 1; i < pieces.size(); ++i) {
            const auto piece = pieces[i];
            running.emplace_back([this, start, out, piece] {
                render_range(start + piece.first, out.subspan(piece.first, piece.second));
            });
            spawned = i;
        }
    } catch (const std::system_error&) {
        // The system would not give us another thread. Finishing the rest here
        // is slower and produces exactly the same samples, which is a better
        // outcome than failing a render over it. This is the standard library
        // throwing, not control flow.
    }

    render_range(start + pieces[0].first, out.subspan(pieces[0].first, pieces[0].second));
    for (std::size_t i = spawned + 1; i < pieces.size(); ++i) {
        render_range(start + pieces[i].first, out.subspan(pieces[i].first, pieces[i].second));
    }

    for (std::thread& worker : running) {
        worker.join();
    }
}

// ---------------------------------------------------------------------------
// Streaming and reporting
// ---------------------------------------------------------------------------

Status stream_scene(const Scene& scene,
                    SampleIndex start,
                    SampleIndex count,
                    std::size_t block_samples,
                    const BlockSink& sink)
{
    if (block_samples == 0) {
        return fail("block size must be positive");
    }
    if (!sink) {
        return fail("stream_scene needs a sink");
    }

    std::vector<Complex32> buffer(block_samples);
    SampleIndex produced = 0;
    while (produced < count) {
        const auto length = static_cast<std::size_t>(
            std::min<SampleIndex>(static_cast<SampleIndex>(block_samples), count - produced));
        const SampleIndex at = start + produced;

        scene.render(at, ComplexSpan(buffer.data(), length));
        if (auto delivered = sink(scene.timestamp_at(at),
                                  ConstComplexSpan(buffer.data(), length));
            !delivered) {
            return delivered;
        }
        produced += length;
    }
    return {};
}

std::string_view emitter_kind_name(EmitterKind kind)
{
    // No default label. A third kind has to be named here before it can be
    // written into a truth file as a blank.
    switch (kind) {
        case EmitterKind::Modulated: return "modulated";
        case EmitterKind::BroadcastFm: return "wfm";
    }
    return "unknown";
}

std::string truth_csv(const Scene& scene)
{
    std::string out;
    out.reserve(128 + scene.truth().size() * 128);
    out +=
        "id,kind,modulation,carrier_offset_hz,low_hz,high_hz,center_hz,bandwidth_hz,"
        "start_sample,end_sample,mean_power,snr_in_occupied_bandwidth_db,"
        "snr_in_full_band_db,symbol_rate_baud,payload_seed,composite_peak_bound\n";

    for (const EmitterTruth& record : scene.truth()) {
        const std::string end =
            record.open_ended() ? std::string{} : std::format("{}", record.end_sample);

        // Empty rather than "cw" on a station's row. EmitterTruth::modulation
        // is meaningless there and a scorer reading a mode name off it would
        // score a 268 kHz broadcast station as a keyed carrier.
        const std::optional<Modulation> readable = record.readable_modulation();
        const std::string_view modulation =
            readable ? modulation_name(*readable) : std::string_view{};

        // Empty rather than 0.0000 on a Modulated row, for the same reason
        // the modulation cell is empty on a station's: a zero in that column
        // would read as a station with no deviation at all.
        const std::string peak_bound =
            (record.kind == EmitterKind::BroadcastFm)
                ? std::format("{:.4f}", record.composite_peak_bound)
                : std::string{};

        out += std::format(
            "{},{},{},{},{},{},{},{},{},{},{:.9g},{:.4f},{:.4f},{:.6g},{},{}\n",
                           record.id,
                           emitter_kind_name(record.kind),
                           modulation,
                           record.carrier_offset_hz,
                           record.extent.low_hz,
                           record.extent.high_hz,
                           record.extent.center_hz(),
                           record.extent.bandwidth_hz(),
                           record.start_sample,
                           end,
                           record.mean_power,
                           record.snr_in_occupied_bandwidth_db,
                           record.snr_in_full_band_db,
                           record.symbol_rate_baud,
                           record.payload_seed,
                           peak_bound);
    }
    return out;
}

}  // namespace revenant::siggen
