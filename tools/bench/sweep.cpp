// Floating-point discipline first, before any function definition in the
// translation unit, per core/dsp/reference_fp.h.
//
// A bench tool is not a GPU reference twin, but it is a referee all the same:
// a stored curve is only evidence of a regression if the same source produces
// the same numbers on a different host compiler. Letting the matched filter
// accumulator fuse its multiply-adds here would move the curve by a fraction
// of a decibel and read as a decoder regression.
#include "core/dsp/reference_fp.h"

#include "tools/bench/sweep.h"

#include <algorithm>
#include <cmath>
#include <future>
#include <limits>
#include <numbers>
#include <random>
#include <thread>

namespace revenant::bench {

static_assert(dsp::kReferenceFpDisciplineApplied,
              "tools/bench/sweep.cpp must include core/dsp/reference_fp.h first");

namespace {

// Everything a point accumulates. Integers only: summation of integers is
// exact and associative, so the order in which worker results are folded in
// cannot change the answer. Accumulating a rate in floating point across
// threads would not have that property.
struct Accumulator {
    std::uint64_t trials = 0;
    std::uint64_t bits_total = 0;
    std::uint64_t bit_errors = 0;
    std::uint64_t decoded = 0;

    void add(const Accumulator& other) {
        trials += other.trials;
        bits_total += other.bits_total;
        bit_errors += other.bit_errors;
        decoded += other.decoded;
    }
};

unsigned resolve_thread_count(unsigned requested) {
    if (requested > 0) {
        return requested;
    }
    const unsigned available = std::thread::hardware_concurrency();
    return available == 0 ? 1u : available;
}

// A 53-bit uniform drawn from the raw generator output rather than from
// std::uniform_real_distribution, whose algorithm is unspecified and therefore
// varies between standard libraries. std::mt19937_64 itself is specified
// exactly, so a stream built this way is portable.
constexpr double kTwoPow53 = 9007199254740992.0;

double uniform_half_open(std::mt19937_64& rng) {
    return static_cast<double>(rng() >> 11) / kTwoPow53;  // [0, 1)
}

double uniform_open_upper(std::mt19937_64& rng) {
    return (static_cast<double>(rng() >> 11) + 1.0) / kTwoPow53;  // (0, 1]
}

struct GaussianPair {
    double i = 0.0;
    double q = 0.0;
};

// Box-Muller, in its direct form. One pair of uniforms yields one complex
// noise sample, so the stream stays aligned with the sample index and a trial
// can be reproduced from its seed alone. The rejection-based polar form would
// consume a variable number of draws and lose that.
GaussianPair gaussian_pair(std::mt19937_64& rng, double sigma) {
    const double u1 = uniform_open_upper(rng);
    const double u2 = uniform_half_open(rng);
    const double radius = sigma * std::sqrt(-2.0 * std::log(u1));
    const double angle = 2.0 * std::numbers::pi * u2;
    return GaussianPair{radius * std::cos(angle), radius * std::sin(angle)};
}

Accumulator run_trial_range(const SweepConfig& config,
                            const Subject& subject,
                            const Generator& generator,
                            std::size_t point_index,
                            std::uint64_t first_trial,
                            std::uint64_t last_trial,
                            unsigned worker_limit) {
    const std::uint64_t span = last_trial - first_trial;
    const std::uint64_t workers = std::min<std::uint64_t>(worker_limit, span);
    const std::uint64_t chunk = (span + workers - 1) / workers;

    // References to the caller's objects are safe in these tasks because every
    // future is waited on before this function returns.
    std::vector<std::future<Accumulator>> pending;
    pending.reserve(static_cast<std::size_t>(workers));

    for (std::uint64_t worker = 0; worker < workers; ++worker) {
        const std::uint64_t begin = first_trial + worker * chunk;
        if (begin >= last_trial) {
            break;
        }
        const std::uint64_t end = std::min<std::uint64_t>(begin + chunk, last_trial);
        pending.push_back(std::async(std::launch::async,
                                     [&config, &subject, &generator, point_index, begin, end] {
                                         Accumulator local;
                                         for (std::uint64_t k = begin; k < end; ++k) {
                                             const TrialResult trial =
                                                 run_trial(config, subject, generator, point_index, k);
                                             local.trials += 1;
                                             local.bits_total += trial.bits_total;
                                             local.bit_errors += trial.bits_wrong;
                                             local.decoded += trial.decoded ? 1ULL : 0ULL;
                                         }
                                         return local;
                                     }));
    }

    Accumulator total;
    for (auto& future : pending) {
        total.add(future.get());
    }
    return total;
}

}  // namespace

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

std::uint64_t trial_seed(std::uint64_t base_seed, std::uint64_t point_index, std::uint64_t trial_index) {
    // Distinct odd multipliers, so swapping the two indices cannot land on the
    // same seed. The +1 keeps index zero from vanishing out of the product.
    const std::uint64_t with_point = splitmix64(base_seed ^ (0xD1B54A32D192ED03ULL * (point_index + 1)));
    return splitmix64(with_point ^ (0xAEF17502108EF2D9ULL * (trial_index + 1)));
}

Status SweepConfig::validate() const {
    if (!(snr_step_db > 0.0)) {
        return fail("snr_step_db must be greater than zero");
    }
    if (snr_stop_db < snr_start_db) {
        return fail("snr_stop_db must not be below snr_start_db");
    }
    if (trials_per_point == 0) {
        return fail("trials_per_point must be at least one");
    }
    if (payload_bytes == 0) {
        return fail("payload_bytes must be at least one");
    }
    if (trial_batch == 0) {
        return fail("trial_batch must be at least one");
    }

    // A step of 1e-9 across a 40 dB range is a typo, not a sweep, and it would
    // otherwise be discovered as an out of memory kill twenty minutes later.
    constexpr std::size_t kMaxPoints = 100000;
    if (point_count() > kMaxPoints) {
        return fail("sweep range and step produce more than 100000 points");
    }
    return {};
}

double SweepConfig::snr_db_at(std::size_t point_index) const {
    return snr_start_db + static_cast<double>(point_index) * snr_step_db;
}

std::size_t SweepConfig::point_count() const {
    if (!(snr_step_db > 0.0) || snr_stop_db < snr_start_db) {
        return 0;
    }
    // The nudge admits a stop value that lands on a step but misses it in
    // binary, which is every sweep whose step is a tenth of a decibel.
    const double steps = (snr_stop_db - snr_start_db) / snr_step_db;
    return static_cast<std::size_t>(std::floor(steps + 1e-9)) + 1;
}

double SweepPoint::ber() const {
    if (bits_total == 0) {
        return 0.0;
    }
    return static_cast<double>(bit_errors) / static_cast<double>(bits_total);
}

double SweepPoint::decode_rate() const {
    if (trials == 0) {
        return 0.0;
    }
    return static_cast<double>(decoded_count) / static_cast<double>(trials);
}

TrialResult run_trial(const SweepConfig& config,
                      const Subject& subject,
                      const Generator& generator,
                      std::size_t point_index,
                      std::uint64_t trial_index) {
    const std::uint64_t seed = trial_seed(config.base_seed, point_index, trial_index);
    const std::vector<std::uint8_t> payload =
        random_payload(config.payload_bytes, splitmix64(seed ^ kPayloadStreamTag));
    const std::vector<dsp::Complex32> waveform =
        generator(payload, config.snr_db_at(point_index), splitmix64(seed ^ kChannelStreamTag));
    return subject(dsp::ConstComplexSpan(waveform), std::span<const std::uint8_t>(payload));
}

Expected<std::vector<SweepPoint>> run_sweep(const SweepConfig& config,
                                            const Subject& subject,
                                            const Generator& generator,
                                            const ProgressFn& progress) {
    if (const Status ok = config.validate(); !ok) {
        return std::unexpected(with_context(ok.error(), "sweep configuration"));
    }
    if (!subject) {
        return fail("sweep requires a subject");
    }
    if (!generator) {
        return fail("sweep requires a generator");
    }

    const std::size_t points = config.point_count();
    const unsigned workers = resolve_thread_count(config.thread_count);
    const std::uint64_t stop_at =
        config.min_bit_errors == 0 ? std::numeric_limits<std::uint64_t>::max() : config.min_bit_errors;

    std::vector<SweepPoint> out;
    out.reserve(points);

    for (std::size_t index = 0; index < points; ++index) {
        Accumulator totals;
        for (std::uint64_t first = 0; first < config.trials_per_point; first += config.trial_batch) {
            const std::uint64_t last = std::min<std::uint64_t>(first + config.trial_batch, config.trials_per_point);
            totals.add(run_trial_range(config, subject, generator, index, first, last, workers));
            if (totals.bit_errors >= stop_at) {
                break;
            }
        }

        SweepPoint point;
        point.snr_db = config.snr_db_at(index);
        point.trials = totals.trials;
        point.bits_total = totals.bits_total;
        point.bit_errors = totals.bit_errors;
        point.decoded_count = totals.decoded;

        if (progress) {
            progress(index, point);
        }
        out.push_back(point);
    }

    return out;
}

bool payload_bit(std::span<const std::uint8_t> payload, std::size_t bit_index) {
    const std::size_t byte_index = bit_index / 8;
    if (byte_index >= payload.size()) {
        return false;
    }
    const unsigned shift = 7u - static_cast<unsigned>(bit_index % 8);
    return ((static_cast<unsigned>(payload[byte_index]) >> shift) & 1u) != 0u;
}

std::vector<std::uint8_t> random_payload(std::size_t bytes, std::uint64_t seed) {
    std::vector<std::uint8_t> payload(bytes);
    std::mt19937_64 rng(seed);
    std::size_t written = 0;
    while (written < bytes) {
        std::uint64_t word = rng();
        for (int byte = 0; byte < 8 && written < bytes; ++byte, ++written) {
            payload[written] = static_cast<std::uint8_t>(word & 0xFFULL);
            word >>= 8;
        }
    }
    return payload;
}

Generator make_bpsk_awgn_generator(ReferenceBpsk options) {
    const std::uint32_t sps = std::max<std::uint32_t>(1u, options.samples_per_symbol);
    const double amplitude = static_cast<double>(options.amplitude);

    return [sps, amplitude](std::span<const std::uint8_t> payload, double snr_db, std::uint64_t seed) {
        const std::size_t bits = payload.size() * 8;
        std::vector<dsp::Complex32> waveform(bits * sps);

        // Eb/N0 in linear terms, then the per-dimension noise deviation.
        // Complex AWGN has variance N0 in total, split evenly across the two
        // dimensions, so each carries N0/2. Detection sees Q(sqrt(2*Eb/N0))
        // out of exactly this calibration.
        const double energy_per_bit = amplitude * amplitude * static_cast<double>(sps);
        const double ebno = std::pow(10.0, snr_db / 10.0);
        const double n0 = energy_per_bit / ebno;
        const double sigma = std::sqrt(n0 / 2.0);

        std::mt19937_64 rng(seed);
        for (std::size_t bit = 0; bit < bits; ++bit) {
            const double symbol = payload_bit(payload, bit) ? amplitude : -amplitude;
            for (std::uint32_t sample = 0; sample < sps; ++sample) {
                const GaussianPair noise = gaussian_pair(rng, sigma);
                const std::size_t at = bit * sps + sample;
                waveform[at] = dsp::Complex32(static_cast<float>(symbol + noise.i),
                                              static_cast<float>(noise.q));
            }
        }
        return waveform;
    };
}

Subject make_bpsk_reference_subject(ReferenceBpsk options) {
    const std::uint32_t sps = std::max<std::uint32_t>(1u, options.samples_per_symbol);

    return [sps](dsp::ConstComplexSpan samples, std::span<const std::uint8_t> payload) {
        TrialResult result;
        const std::size_t bits = payload.size() * 8;
        result.bits_total = bits;

        // A subject handed a capture shorter than the payload it was told to
        // recover has failed, and says so, rather than reading past the end.
        if (samples.size() < bits * sps) {
            result.bits_wrong = bits;
            result.decoded = false;
            return result;
        }

        for (std::size_t bit = 0; bit < bits; ++bit) {
            // Matched filter for a rectangular pulse: sum the symbol's samples.
            // The imaginary part carries only noise for a real constellation,
            // so discarding it is the 3 dB the coherent detector is worth.
            // Accumulated in double because at a high oversampling factor a
            // float accumulator's rounding is comparable to the decision
            // margin at low SNR.
            double correlation = 0.0;
            for (std::uint32_t sample = 0; sample < sps; ++sample) {
                correlation += static_cast<double>(samples[bit * sps + sample].real());
            }
            const bool decided = correlation >= 0.0;
            if (decided != payload_bit(payload, bit)) {
                ++result.bits_wrong;
            }
        }

        result.decoded = result.bits_wrong == 0;
        return result;
    };
}

double q_function(double x) {
    return 0.5 * std::erfc(x / std::numbers::sqrt2);
}

double bpsk_theoretical_ber(double ebno_db) {
    const double ebno = std::pow(10.0, ebno_db / 10.0);
    return 0.5 * std::erfc(std::sqrt(ebno));
}

}  // namespace revenant::bench
