// The bench CLI. This is what CI calls.
//
// Four commands:
//
//   sweep       run a sweep and write the curve, for the reference BPSK
//               subject, the RDS decoder with --mode rds, or any decoder in
//               tools/bench/mode_subjects.h by its name
//   compare     diff two curve files, exit nonzero on a regression
//   validate    run the reference BPSK subject and check the measured curve
//               against Q(sqrt(2*Eb/N0))
//   throughput  measure receivers against wall clock and the channelizer
//               against memory bandwidth
//
// validate is the one that keeps sweep and compare honest. A decoder's curve
// has no closed form to land on, so the evidence that the harness, the SNR
// calibration and the generator agree is still that the reference subject
// lands on Q(sqrt(2*Eb/N0)). This paragraph used to say there is no decoder
// to sweep yet; the RDS decoder is one, in tools/bench/rds_subject.h.
//
// throughput answers a different kind of question and reports rather than
// judges. It has no pass or fail and no threshold, because every number it
// produces depends on what else the machine is doing; a timing assertion here
// would be a test that fails for reasons that have nothing to do with the
// code. What it does is put measured numbers against three claims this
// repository has been making without them. See tools/bench/throughput.h.

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/engine/vrx.h"
#include "core/error.h"
#include "tools/bench/curve.h"
#include "tools/bench/mode_subjects.h"
#include "tools/bench/rds_subject.h"
#include "tools/bench/sweep.h"
#include "tools/bench/throughput.h"

namespace {

namespace bench = revenant::bench;

using revenant::Expected;
using revenant::Status;
using revenant::fail;

constexpr int kExitOk = 0;
constexpr int kExitRegression = 1;
constexpr int kExitUsage = 2;
constexpr int kExitError = 3;

// ---------------------------------------------------------------------------
// Argument handling
// ---------------------------------------------------------------------------

class ArgMap {
public:
    static Expected<ArgMap> parse(std::span<const std::string_view> args) {
        ArgMap out;
        for (std::size_t index = 0; index < args.size(); ++index) {
            const std::string_view arg = args[index];
            if (!arg.starts_with("--")) {
                out.positional_.push_back(arg);
                continue;
            }
            const std::string_view body = arg.substr(2);
            if (body.empty()) {
                return fail("'--' is not an option");
            }

            const std::size_t equals = body.find('=');
            if (equals != std::string_view::npos) {
                out.named_.push_back(Entry{body.substr(0, equals), body.substr(equals + 1)});
                continue;
            }

            // A negative number is a value, not an option, which is why the
            // test is for a leading "--" and not for a leading '-'.
            if (index + 1 < args.size() && !args[index + 1].starts_with("--")) {
                out.named_.push_back(Entry{body, args[index + 1]});
                ++index;
            } else {
                out.named_.push_back(Entry{body, std::string_view{}});
            }
        }
        return out;
    }

    // A mistyped option that silently falls back to its default is how a CI
    // sweep ends up measuring something nobody asked for.
    [[nodiscard]] Status require_known(std::span<const std::string_view> allowed) const {
        for (const Entry& entry : named_) {
            bool known = false;
            for (const std::string_view candidate : allowed) {
                if (entry.key == candidate) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                return fail(std::format("unknown option '--{}'", entry.key));
            }
        }
        return {};
    }

    [[nodiscard]] bool has(std::string_view key) const { return find(key) != nullptr; }

    [[nodiscard]] std::string_view text(std::string_view key, std::string_view fallback) const {
        const Entry* entry = find(key);
        return entry == nullptr || entry->value.empty() ? fallback : entry->value;
    }

    [[nodiscard]] Expected<double> number(std::string_view key, double fallback) const {
        const Entry* entry = find(key);
        if (entry == nullptr) {
            return fallback;
        }
        double value = 0.0;
        const auto result =
            std::from_chars(entry->value.data(), entry->value.data() + entry->value.size(), value);
        if (result.ec != std::errc{} || result.ptr != entry->value.data() + entry->value.size()) {
            return fail(std::format("option '--{}' needs a number, found '{}'", key, entry->value));
        }
        return value;
    }

    [[nodiscard]] Expected<std::uint64_t> integer(std::string_view key, std::uint64_t fallback) const {
        const Entry* entry = find(key);
        if (entry == nullptr) {
            return fallback;
        }
        std::uint64_t value = 0;
        const auto result =
            std::from_chars(entry->value.data(), entry->value.data() + entry->value.size(), value);
        if (result.ec != std::errc{} || result.ptr != entry->value.data() + entry->value.size()) {
            return fail(
                std::format("option '--{}' needs a non-negative integer, found '{}'", key, entry->value));
        }
        return value;
    }

    [[nodiscard]] const std::vector<std::string_view>& positional() const { return positional_; }

private:
    struct Entry {
        std::string_view key;
        std::string_view value;
    };

    std::vector<Entry> named_;
    std::vector<std::string_view> positional_;

    [[nodiscard]] const Entry* find(std::string_view key) const {
        for (const Entry& entry : named_) {
            if (entry.key == key) {
                return &entry;
            }
        }
        return nullptr;
    }
};

struct SweepArgs {
    bench::SweepConfig config;
    bench::ReferenceBpsk reference;
    std::string mode;
    std::string subject;
    std::string commit;
    std::string out_path;
    bool quiet = false;

    // Set for every mode but bpsk and rds, which predate the table in
    // tools/bench/mode_subjects.h and keep their own handling here.
    std::optional<bench::ModeSubject> decoder;

    // What the report calls the axis and the error rate, and where the
    // sensitivity is read. Eb/N0 and bits for the two older modes.
    std::string axis = "Eb/N0";
    std::string unit = "bit";
    double threshold = 0.01;
};

constexpr std::string_view kSweepOptions[] = {
    "snr-start", "snr-stop", "snr-step", "trials",  "payload-bytes", "seed", "min-bit-errors",
    "batch",     "threads",  "sps",      "mode",    "subject",       "commit", "out",
    "quiet",
};

Expected<SweepArgs> parse_sweep_args(const ArgMap& args) {
    SweepArgs parsed;
    const bench::SweepConfig defaults;

    const auto read_double = [&args](std::string_view key, double fallback) { return args.number(key, fallback); };
    const auto read_uint = [&args](std::string_view key, std::uint64_t fallback) {
        return args.integer(key, fallback);
    };

    Expected<double> start = read_double("snr-start", defaults.snr_start_db);
    if (!start) {
        return std::unexpected(start.error());
    }
    Expected<double> stop = read_double("snr-stop", defaults.snr_stop_db);
    if (!stop) {
        return std::unexpected(stop.error());
    }
    Expected<double> step = read_double("snr-step", defaults.snr_step_db);
    if (!step) {
        return std::unexpected(step.error());
    }
    Expected<std::uint64_t> trials = read_uint("trials", defaults.trials_per_point);
    if (!trials) {
        return std::unexpected(trials.error());
    }
    Expected<std::uint64_t> payload = read_uint("payload-bytes", defaults.payload_bytes);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    Expected<std::uint64_t> seed = read_uint("seed", defaults.base_seed);
    if (!seed) {
        return std::unexpected(seed.error());
    }
    Expected<std::uint64_t> min_errors = read_uint("min-bit-errors", defaults.min_bit_errors);
    if (!min_errors) {
        return std::unexpected(min_errors.error());
    }
    Expected<std::uint64_t> batch = read_uint("batch", defaults.trial_batch);
    if (!batch) {
        return std::unexpected(batch.error());
    }
    Expected<std::uint64_t> threads = read_uint("threads", 0);
    if (!threads) {
        return std::unexpected(threads.error());
    }
    Expected<std::uint64_t> sps = read_uint("sps", 1);
    if (!sps) {
        return std::unexpected(sps.error());
    }
    if (*sps == 0 || *sps > 4096) {
        return fail("option '--sps' must be between 1 and 4096");
    }

    parsed.config.snr_start_db = *start;
    parsed.config.snr_stop_db = *stop;
    parsed.config.snr_step_db = *step;
    parsed.config.trials_per_point = *trials;
    parsed.config.payload_bytes = static_cast<std::size_t>(*payload);
    parsed.config.base_seed = *seed;
    parsed.config.min_bit_errors = *min_errors;
    parsed.config.trial_batch = *batch;
    parsed.config.thread_count = static_cast<unsigned>(*threads);
    parsed.reference.samples_per_symbol = static_cast<std::uint32_t>(*sps);

    parsed.mode = std::string(args.text("mode", "bpsk"));
    if (parsed.mode != "bpsk" && parsed.mode != "rds") {
        Expected<bench::ModeSubject> decoder = bench::make_mode_subject(parsed.mode);
        if (!decoder) {
            return fail(std::format("{}; or bpsk or rds", decoder.error().message));
        }
        if (args.has("sps")) {
            return fail(std::format("option '--sps' belongs to the bpsk mode; the {} mode fixes its own rate",
                                    parsed.mode));
        }
        // The mode's own grid, trial count and payload unless an option says
        // otherwise, so `bench sweep --mode NAME` alone reproduces the curve
        // the nightly compares against its baseline.
        if (!args.has("snr-start")) {
            parsed.config.snr_start_db = decoder->snr_start_db;
        }
        if (!args.has("snr-stop")) {
            parsed.config.snr_stop_db = decoder->snr_stop_db;
        }
        if (!args.has("snr-step")) {
            parsed.config.snr_step_db = decoder->snr_step_db;
        }
        if (!args.has("trials")) {
            parsed.config.trials_per_point = decoder->trials;
        }
        if (!args.has("min-bit-errors")) {
            parsed.config.min_bit_errors = decoder->min_errors;
        }
        if (!args.has("payload-bytes")) {
            parsed.config.payload_bytes = decoder->default_payload_bytes;
        }
        const std::size_t bytes = parsed.config.payload_bytes;
        if (bytes < decoder->minimum_payload_bytes || bytes % decoder->payload_multiple != 0) {
            return fail(std::format("the {} mode needs a payload of at least {} bytes in whole units of {}",
                                    parsed.mode, decoder->minimum_payload_bytes, decoder->payload_multiple));
        }
        parsed.axis = decoder->axis;
        parsed.unit = decoder->unit;
        parsed.threshold = decoder->threshold;
        parsed.subject = std::string(args.text("subject", decoder->subject));
        parsed.decoder = std::move(*decoder);
    } else if (parsed.mode == "rds") {
        // The RDS rate is fixed by the subject, so a samples-per-symbol figure
        // would be accepted and do nothing, which is the silent fallback
        // require_known exists to prevent.
        if (args.has("sps")) {
            return fail("option '--sps' belongs to the bpsk mode; the rds mode runs at 144 samples per bit");
        }
        // A trial much shorter than the decoder's acquisition is mostly
        // acquisition, so the rds mode has its own default and floor.
        if (!args.has("payload-bytes")) {
            parsed.config.payload_bytes = 512;
        } else if (parsed.config.payload_bytes < bench::kRdsMinimumPayloadBytes) {
            return fail(std::format("the rds mode needs at least {} payload bytes to lock and still score",
                                    bench::kRdsMinimumPayloadBytes));
        }
    }
    if (!parsed.decoder) {
        parsed.subject = std::string(args.text(
            "subject", parsed.mode == "rds"
                           ? std::string("rds-bits/171000")
                           : std::format("reference-bpsk-coherent/sps{}", parsed.reference.samples_per_symbol)));
    }

    if (const Status ok = parsed.config.validate(); !ok) {
        return std::unexpected(ok.error());
    }
    parsed.commit = std::string(args.text("commit", "unknown"));
    parsed.out_path = std::string(args.text("out", ""));
    parsed.quiet = args.has("quiet");
    return parsed;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

void print_point_header(const SweepArgs& parsed) {
    std::println("{:>9}  {:>9}  {:>13}  {:>12}  {:>14}  {:>8}", "SNR dB", "trials", parsed.unit + "s", "errors",
                 "error rate", "decoded");
}

void print_point(const bench::SweepPoint& point) {
    std::println("{:>9.3f}  {:>9}  {:>13}  {:>12}  {:>14.6e}  {:>7.1f}%", point.snr_db, point.trials,
                 point.bits_total, point.bit_errors, point.ber(), 100.0 * point.decode_rate());
}

void print_sweep_banner(const SweepArgs& parsed) {
    std::println("seed {}  trials/point {}  payload {} bytes ({} bits)  batch {}  early stop at {} {} errors",
                 parsed.config.base_seed, parsed.config.trials_per_point, parsed.config.payload_bytes,
                 parsed.config.payload_bytes * 8, parsed.config.trial_batch, parsed.config.min_bit_errors,
                 parsed.unit);
    std::println("subject: {}", parsed.subject);
    std::println("sweep {:.3f} to {:.3f} dB {} in {:.3f} dB steps, {} points", parsed.config.snr_start_db,
                 parsed.config.snr_stop_db, parsed.axis, parsed.config.snr_step_db, parsed.config.point_count());
    std::println("error rate: wrong {}s over {}s sent", parsed.unit, parsed.unit);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int command_sweep(const ArgMap& args) {
    if (const Status ok = args.require_known(kSweepOptions); !ok) {
        std::print(stderr, "bench sweep: {}\n", ok.error().message);
        return kExitUsage;
    }

    Expected<SweepArgs> parsed = parse_sweep_args(args);
    if (!parsed) {
        std::print(stderr, "bench sweep: {}\n", parsed.error().message);
        return kExitUsage;
    }

    if (!parsed->quiet) {
        print_sweep_banner(*parsed);
        print_point_header(*parsed);
    }

    const bool rds = parsed->mode == "rds";
    bench::Subject subject;
    bench::Generator generator;
    if (parsed->decoder) {
        subject = parsed->decoder->score;
        generator = parsed->decoder->generator;
    } else if (rds) {
        subject = bench::make_rds_subject();
        generator = bench::make_rds_generator();
    } else {
        subject = bench::make_bpsk_reference_subject(parsed->reference);
        generator = bench::make_bpsk_awgn_generator(parsed->reference);
    }
    const bench::ProgressFn progress =
        parsed->quiet ? bench::ProgressFn{}
                      : bench::ProgressFn{[](std::size_t, const bench::SweepPoint& point) { print_point(point); }};

    Expected<std::vector<bench::SweepPoint>> points =
        bench::run_sweep(parsed->config, subject, generator, progress);
    if (!points) {
        std::print(stderr, "bench sweep: {}\n", points.error().message);
        return kExitError;
    }

    const bench::Curve curve =
        bench::make_curve(parsed->mode, parsed->subject, parsed->commit, parsed->config, *points);

    const double sensitivity = bench::sensitivity_db(curve.points, parsed->threshold);
    if (!parsed->quiet) {
        if (std::isnan(sensitivity)) {
            std::println("sensitivity: the curve does not cross a {} error rate of {:g} inside this range",
                         parsed->unit, parsed->threshold);
        } else {
            std::println("sensitivity: {:.3f} dB {} at a {} error rate of {:g}", sensitivity, parsed->axis,
                         parsed->unit, parsed->threshold);
        }
    }

    if (!parsed->out_path.empty()) {
        if (const Status ok = bench::write_curve_file(parsed->out_path, curve); !ok) {
            std::print(stderr, "bench sweep: {}\n", ok.error().message);
            return kExitError;
        }
        if (!parsed->quiet) {
            std::println("wrote {}", parsed->out_path);
        }
    } else {
        std::print("{}", bench::curve_to_json(curve));
    }

    return kExitOk;
}

constexpr std::string_view kCompareOptions[] = {
    "ber-threshold",
    "ber-ratio",
    "sensitivity-tolerance-db",
    "quiet",
};

int command_compare(const ArgMap& args) {
    if (const Status ok = args.require_known(kCompareOptions); !ok) {
        std::print(stderr, "bench compare: {}\n", ok.error().message);
        return kExitUsage;
    }
    if (args.positional().size() != 2) {
        std::print(stderr, "bench compare: expected two curve files, a reference and a candidate\n");
        return kExitUsage;
    }

    const bench::RegressionOptions defaults;
    Expected<double> threshold = args.number("ber-threshold", defaults.ber_threshold);
    if (!threshold) {
        std::print(stderr, "bench compare: {}\n", threshold.error().message);
        return kExitUsage;
    }
    Expected<double> ratio = args.number("ber-ratio", defaults.ber_ratio_tolerance);
    if (!ratio) {
        std::print(stderr, "bench compare: {}\n", ratio.error().message);
        return kExitUsage;
    }
    Expected<double> tolerance = args.number("sensitivity-tolerance-db", defaults.sensitivity_tolerance_db);
    if (!tolerance) {
        std::print(stderr, "bench compare: {}\n", tolerance.error().message);
        return kExitUsage;
    }

    bench::RegressionOptions options;
    options.ber_threshold = *threshold;
    options.ber_ratio_tolerance = *ratio;
    options.sensitivity_tolerance_db = *tolerance;

    Expected<bench::Curve> reference = bench::read_curve_file(args.positional()[0]);
    if (!reference) {
        std::print(stderr, "bench compare: {}\n", reference.error().message);
        return kExitError;
    }

    // A decoder curve is read at its own mode's error rate unless told
    // otherwise, so a caller comparing CW, which is read at 0.05, cannot
    // judge it at 0.01 by leaving the option off.
    if (!args.has("ber-threshold")) {
        if (Expected<bench::ModeSubject> mode = bench::make_mode_subject(reference->mode); mode) {
            options.ber_threshold = mode->threshold;
        }
    }
    Expected<bench::Curve> candidate = bench::read_curve_file(args.positional()[1]);
    if (!candidate) {
        std::print(stderr, "bench compare: {}\n", candidate.error().message);
        return kExitError;
    }

    Expected<bench::CurveComparison> comparison = bench::compare_curves(*reference, *candidate, options);
    if (!comparison) {
        std::print(stderr, "bench compare: {}\n", comparison.error().message);
        return kExitError;
    }

    const bool quiet = args.has("quiet");
    if (!quiet) {
        std::println("{:>9}  {:>14}  {:>14}  {:>10}  {}", "SNR dB", "reference BER", "candidate BER", "ratio",
                     "verdict");
        for (const bench::PointComparison& point : comparison->points) {
            const double point_ratio =
                point.reference_ber > 0.0 ? point.candidate_ber / point.reference_ber : 0.0;
            std::println("{:>9.3f}  {:>14.6e}  {:>14.6e}  {:>10.3f}  {}", point.snr_db, point.reference_ber,
                         point.candidate_ber, point_ratio, point.worsened ? "worse" : "ok");
        }
        if (comparison->has_sensitivity_delta) {
            std::println("sensitivity at an error rate of {:g}: reference {:.3f} dB, candidate {:.3f} dB, "
                         "delta {:+.3f} dB",
                         options.ber_threshold, comparison->reference_sensitivity_db,
                         comparison->candidate_sensitivity_db, comparison->sensitivity_delta_db);
        }
    }

    // The notes carry the only explanation of a failure that is not a number:
    // a range that does not overlap, a curve that never crosses. CI runs quiet
    // and still has to be able to say why it stopped, so these are not
    // suppressed with the table.
    if (!quiet || comparison->regressed) {
        for (const std::string& note : comparison->notes) {
            std::println("note: {}", note);
        }
    }

    if (comparison->regressed) {
        if (quiet) {
            for (const bench::PointComparison& point : comparison->points) {
                if (point.worsened) {
                    std::println("worse at {:.3f} dB: reference BER {:.6e}, candidate BER {:.6e}", point.snr_db,
                                 point.reference_ber, point.candidate_ber);
                }
            }
        }
        std::println("REGRESSION: {} point(s) worse, sensitivity delta {}", comparison->worsened_points,
                     comparison->has_sensitivity_delta
                         ? std::format("{:+.3f} dB", comparison->sensitivity_delta_db)
                         : std::string("not comparable"));
        return kExitRegression;
    }

    std::println("OK: no regression against the reference curve");
    return kExitOk;
}

constexpr std::string_view kValidateOptions[] = {
    "snr-start", "snr-stop", "snr-step", "trials", "payload-bytes", "seed",   "min-bit-errors",
    "batch",     "threads",  "sps",      "mode",   "subject",       "commit", "out",
    "quiet",     "min-check-errors",     "sigma",
};

int command_validate(const ArgMap& args) {
    if (const Status ok = args.require_known(kValidateOptions); !ok) {
        std::print(stderr, "bench validate: {}\n", ok.error().message);
        return kExitUsage;
    }

    Expected<SweepArgs> parsed = parse_sweep_args(args);
    if (!parsed) {
        std::print(stderr, "bench validate: {}\n", parsed.error().message);
        return kExitUsage;
    }
    if (parsed->mode != "bpsk") {
        std::print(stderr, "bench validate: only the bpsk mode has a closed form to validate against\n");
        return kExitUsage;
    }
    Expected<std::uint64_t> min_check_errors = args.integer("min-check-errors", 20);
    if (!min_check_errors) {
        std::print(stderr, "bench validate: {}\n", min_check_errors.error().message);
        return kExitUsage;
    }
    Expected<double> sigma = args.number("sigma", 4.0);
    if (!sigma) {
        std::print(stderr, "bench validate: {}\n", sigma.error().message);
        return kExitUsage;
    }
    if (!(*sigma > 0.0)) {
        std::print(stderr, "bench validate: '--sigma' must be greater than zero\n");
        return kExitUsage;
    }

    if (!parsed->quiet) {
        print_sweep_banner(*parsed);
    }

    const bench::Subject subject = bench::make_bpsk_reference_subject(parsed->reference);
    const bench::Generator generator = bench::make_bpsk_awgn_generator(parsed->reference);
    Expected<std::vector<bench::SweepPoint>> points =
        bench::run_sweep(parsed->config, subject, generator);
    if (!points) {
        std::print(stderr, "bench validate: {}\n", points.error().message);
        return kExitError;
    }

    if (!parsed->quiet) {
        std::println("{:>9}  {:>14}  {:>14}  {:>10}  {:>10}  {}", "SNR dB", "measured BER", "theory BER",
                     "ratio", "errors", "verdict");
    }

    std::size_t checked = 0;
    std::size_t failed = 0;
    for (const bench::SweepPoint& point : *points) {
        const double measured = point.ber();
        const double theory = bench::bpsk_theoretical_ber(point.snr_db);

        if (point.bit_errors < *min_check_errors || !(theory > 0.0)) {
            if (!parsed->quiet) {
                std::println("{:>9.3f}  {:>14.6e}  {:>14.6e}  {:>10}  {:>10}  {}", point.snr_db, measured,
                             theory, "-", point.bit_errors, "too few errors to judge");
            }
            continue;
        }

        // A BER built from k observed errors has a relative standard deviation
        // of 1/sqrt(k), the binomial counting error. Allow `sigma` of those in
        // the log domain, so the band is symmetric in ratio and a passing check
        // does not flake from run to run.
        const double allowed = std::exp(*sigma / std::sqrt(static_cast<double>(point.bit_errors)));
        const double ratio = measured / theory;
        const bool ok = ratio <= allowed && ratio >= 1.0 / allowed;
        ++checked;
        if (!ok) {
            ++failed;
        }
        if (!parsed->quiet) {
            std::println("{:>9.3f}  {:>14.6e}  {:>14.6e}  {:>10.4f}  {:>10}  {}", point.snr_db, measured,
                         theory, ratio, point.bit_errors,
                         ok ? std::format("ok (within x{:.3f})", allowed)
                            : std::format("OUT OF BAND (x{:.3f})", allowed));
        }
    }

    if (!parsed->out_path.empty()) {
        const bench::Curve curve =
            bench::make_curve(parsed->mode, parsed->subject, parsed->commit, parsed->config, *points);
        if (const Status ok = bench::write_curve_file(parsed->out_path, curve); !ok) {
            std::print(stderr, "bench validate: {}\n", ok.error().message);
            return kExitError;
        }
        std::println("wrote {}", parsed->out_path);
    }

    if (checked < 3) {
        std::println("INCONCLUSIVE: only {} point(s) had enough errors to judge. Widen the sweep downwards "
                     "or raise --trials.",
                     checked);
        return kExitRegression;
    }
    if (failed > 0) {
        std::println("FAILED: {} of {} checked points are outside the {} sigma band around theory", failed,
                     checked, *sigma);
        return kExitRegression;
    }

    std::println("OK: {} checked points track Q(sqrt(2*Eb/N0)) within {} sigma", checked, *sigma);
    return kExitOk;
}

// ---------------------------------------------------------------------------
// throughput
// ---------------------------------------------------------------------------

// A comma-separated list of counts. Rejected rather than skipped on a bad
// entry, for the same reason require_known rejects a mistyped option: a list
// that silently drops what it could not read measures something nobody asked
// for.
[[nodiscard]] Expected<std::vector<std::uint32_t>> parse_count_list(std::string_view text,
                                                                    std::string_view option) {
    std::vector<std::uint32_t> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string_view item =
            text.substr(start, comma == std::string_view::npos ? std::string_view::npos
                                                               : comma - start);
        if (item.empty()) {
            return fail(std::format("option '--{}' has an empty entry in '{}'", option, text));
        }
        std::uint32_t value = 0;
        const auto result =
            std::from_chars(item.data(), item.data() + item.size(), value);
        if (result.ec != std::errc{} || result.ptr != item.data() + item.size()) {
            return fail(std::format("option '--{}' needs a comma-separated list of counts, and "
                                    "'{}' is not one",
                                    option, item));
        }
        out.push_back(value);
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    if (out.empty()) {
        return fail(std::format("option '--{}' is empty", option));
    }
    return out;
}

// The device index alone is signed, because -1 is what hands the choice to
// REVENANT_GPU_INDEX and that is how every other binary here is aimed.
[[nodiscard]] Expected<int> parse_device_index(std::string_view text) {
    int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return fail(std::format("option '--gpu' needs a device index, found '{}'", text));
    }
    return value;
}

constexpr std::string_view kThroughputOptions[] = {
    "gpu",           "rate",       "channels",    "block-samples",  "audio-rate",
    "seconds",       "seed",       "emitters",    "noise",          "demod",
    "bandwidth",     "receivers",  "iterations",  "warmup",         "no-device",
    "no-bandwidth",  "transform-bytes", "transform-sizes", "bandwidth-iterations",
    "json",          "out",        "quiet",
};

int command_throughput(const ArgMap& args) {
    if (const Status ok = args.require_known(kThroughputOptions); !ok) {
        std::print(stderr, "bench throughput: {}\n", ok.error().message);
        return kExitUsage;
    }

    bench::ThroughputConfig config;

    if (args.has("gpu")) {
        Expected<int> gpu = parse_device_index(args.text("gpu", ""));
        if (!gpu) {
            std::print(stderr, "bench throughput: {}\n", gpu.error().message);
            return kExitUsage;
        }
        config.gpu_index = *gpu;
    }

    const auto read_uint = [&args](std::string_view key, std::uint64_t fallback) {
        return args.integer(key, fallback);
    };

    Expected<std::uint64_t> rate = read_uint("rate", static_cast<std::uint64_t>(config.rate));
    if (!rate) {
        std::print(stderr, "bench throughput: {}\n", rate.error().message);
        return kExitUsage;
    }
    Expected<std::uint64_t> channels = read_uint("channels", config.channels);
    if (!channels) {
        std::print(stderr, "bench throughput: {}\n", channels.error().message);
        return kExitUsage;
    }
    Expected<std::uint64_t> block_samples = read_uint("block-samples", config.block_samples);
    if (!block_samples) {
        std::print(stderr, "bench throughput: {}\n", block_samples.error().message);
        return kExitUsage;
    }
    Expected<std::uint64_t> audio_rate =
        read_uint("audio-rate", static_cast<std::uint64_t>(config.audio_rate));
    if (!audio_rate) {
        std::print(stderr, "bench throughput: {}\n", audio_rate.error().message);
        return kExitUsage;
    }
    Expected<double> seconds = args.number("seconds", config.seconds);
    if (!seconds) {
        std::print(stderr, "bench throughput: {}\n", seconds.error().message);
        return kExitUsage;
    }
    Expected<std::uint64_t> seed = read_uint("seed", config.seed);
    if (!seed) {
        std::print(stderr, "bench throughput: {}\n", seed.error().message);
        return kExitUsage;
    }
    Expected<std::uint64_t> emitters = read_uint("emitters", config.emitters);
    if (!emitters) {
        std::print(stderr, "bench throughput: {}\n", emitters.error().message);
        return kExitUsage;
    }
    Expected<std::uint64_t> bandwidth =
        read_uint("bandwidth", static_cast<std::uint64_t>(config.bandwidth));
    if (!bandwidth) {
        std::print(stderr, "bench throughput: {}\n", bandwidth.error().message);
        return kExitUsage;
    }
    Expected<std::uint64_t> iterations = read_uint("iterations", config.iterations);
    if (!iterations) {
        std::print(stderr, "bench throughput: {}\n", iterations.error().message);
        return kExitUsage;
    }
    Expected<std::uint64_t> warmup = read_uint("warmup", config.warmup);
    if (!warmup) {
        std::print(stderr, "bench throughput: {}\n", warmup.error().message);
        return kExitUsage;
    }
    Expected<std::uint64_t> transform_bytes = read_uint("transform-bytes", config.transform_bytes);
    if (!transform_bytes) {
        std::print(stderr, "bench throughput: {}\n", transform_bytes.error().message);
        return kExitUsage;
    }
    Expected<std::uint64_t> bandwidth_iterations =
        read_uint("bandwidth-iterations", config.bandwidth_iterations);
    if (!bandwidth_iterations) {
        std::print(stderr, "bench throughput: {}\n", bandwidth_iterations.error().message);
        return kExitUsage;
    }

    auto demod = revenant::engine::demod_from_name(
        args.text("demod", revenant::engine::demod_name(config.demod)));
    if (!demod) {
        std::print(stderr, "bench throughput: {}\n", demod.error().message);
        return kExitUsage;
    }

    config.rate = static_cast<revenant::dsp::SampleRate>(*rate);
    config.channels = static_cast<std::uint32_t>(*channels);
    config.block_samples = static_cast<std::size_t>(*block_samples);
    config.audio_rate = static_cast<revenant::dsp::SampleRate>(*audio_rate);
    config.seconds = *seconds;
    config.seed = *seed;
    config.emitters = static_cast<std::size_t>(*emitters);
    config.noise = args.has("noise");
    config.demod = *demod;
    config.bandwidth = static_cast<revenant::dsp::Hertz>(*bandwidth);
    config.iterations = static_cast<std::uint32_t>(*iterations);
    config.warmup = static_cast<std::uint32_t>(*warmup);
    config.device_rig = !args.has("no-device");
    config.bandwidth_pass = !args.has("no-bandwidth");
    config.transform_bytes = *transform_bytes;
    config.bandwidth_iterations = static_cast<std::uint32_t>(*bandwidth_iterations);

    if (args.has("receivers")) {
        auto counts = parse_count_list(args.text("receivers", ""), "receivers");
        if (!counts) {
            std::print(stderr, "bench throughput: {}\n", counts.error().message);
            return kExitUsage;
        }
        config.receiver_counts = std::move(*counts);
    }
    if (args.has("transform-sizes")) {
        auto sizes = parse_count_list(args.text("transform-sizes", ""), "transform-sizes");
        if (!sizes) {
            std::print(stderr, "bench throughput: {}\n", sizes.error().message);
            return kExitUsage;
        }
        config.transform_sizes = std::move(*sizes);
    }

    if (const Status ok = config.validate(); !ok) {
        std::print(stderr, "bench throughput: {}\n", ok.error().message);
        return kExitUsage;
    }

    const bool json_only = args.has("json");
    const bool quiet = args.has("quiet");
    const std::string out_path(args.text("out", ""));

    Expected<bench::ThroughputReport> report = bench::run_throughput(config);
    if (!report) {
        std::print(stderr, "bench throughput: {}\n", report.error().message);
        return kExitError;
    }

    if (json_only) {
        std::print("{}", bench::throughput_to_json(*report));
    } else if (!quiet) {
        bench::print_throughput(*report);
    }

    if (!out_path.empty()) {
        std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::print(stderr, "bench throughput: cannot open '{}' for writing\n", out_path);
            return kExitError;
        }
        const std::string text = bench::throughput_to_json(*report);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            std::print(stderr, "bench throughput: failed while writing '{}'\n", out_path);
            return kExitError;
        }
        if (!json_only && !quiet) {
            std::println("wrote {}", out_path);
        }
    }

    return kExitOk;
}

constexpr std::string_view kUsage = R"(revenant bench: BER/SNR sweep harness and throughput measurement

usage:
  bench sweep      [options] [--out FILE]
  bench compare    REFERENCE.json CANDIDATE.json [options]
  bench validate   [options]
  bench throughput [options]
  bench help

sweep options (validate takes the same set):
  --snr-start DB          first point                  (default -4)
  --snr-stop DB           last point                   (default 10)
  --snr-step DB           step between points          (default 1)
  --trials N              trials per point             (default 1000)
  --payload-bytes N       payload per trial            (default 64)
  --seed N                base seed                    (default 0)
  --min-bit-errors N      stop a point after N errors  (default 100, 0 disables)
  --batch N               trials between stop checks   (default 64)
  --threads N             worker threads               (default: hardware)
  --sps N                 samples per symbol, bpsk only (default 1)
  --mode NAME             bpsk, the reference detector, or rds, the RDS
                          decoder against its transmitter (default bpsk).
                          Recorded in the curve. rds defaults
                          --payload-bytes to 512 and needs at least 128.
                          Both sweep Eb/N0 and count bits.
                          Or a decoder against its transmitter, swept in SNR
                          in 2500 Hz and counted in its own unit:
                            rtty sitor-b psk31 psk63 qpsk31 cw   characters
                            ax25 m17                             frames
                            pocsag-512 pocsag-1200 pocsag-2400   pages
                            navtex                               messages
                            p25p1 dstar tetra                    bits
                          Each has its own grid, trial count and payload,
                          the ones its nightly baseline was made with, and
                          no early stop, used unless the options above say
                          otherwise
  --subject NAME          recorded in the curve
  --commit SHA            recorded in the curve        (default unknown)
  --out FILE              write the curve here, otherwise stdout
  --quiet                 suppress the per-point table

validate also takes:
  --min-check-errors N    skip points with fewer errors than this (default 20)
  --sigma N               width of the band around theory         (default 4)

compare options:
  --ber-threshold X            error rate at which sensitivity is measured
                               (default 0.01, or the reference curve's mode's
                               own rate for a decoder mode: 0.05 for cw)
  --ber-ratio X                per-point worsening factor           (default 1.5)
  --sensitivity-tolerance-db X allowed rightward shift              (default 0.2)
  --quiet                      suppress the per-point table

throughput options:
  --gpu N                 device index, -1 honours REVENANT_GPU_INDEX
  --rate HZ               source rate                  (default 20000000)
  --channels N            channelizer channels         (default 64)
  --block-samples N       samples per source block     (default 65536)
  --audio-rate HZ         receiver audio rate          (default 48000)
  --seconds X             source time per point        (default 4)
  --seed N                scene seed                   (default 20260918)
  --emitters N            emitters in the scene        (default 0)
  --noise                 add the scene's noise floor  (default off)
  --demod NAME            am nfm wfm usb lsb dsb cw    (default nfm)
  --bandwidth HZ          receiver bandwidth           (default 16000)
  --receivers LIST        receiver counts to measure   (default 0,1,2,8,16,32,50,100)
  --iterations N          device rig iterations        (default 32)
  --warmup N              device rig warmup passes     (default 4)
  --no-device             skip the per-stage GPU timing
  --no-bandwidth          skip the channelizer bandwidth pass
  --transform-bytes N     transform buffer size, read once and written once,
                          so the traffic is twice this  (default 134217728,
                          which is VkFFT's own test size in docs/fft.md)
  --transform-sizes LIST  channel counts to measure    (default 64,128,256,1024,2048)
  --bandwidth-iterations N                             (default 16)
  --json                  print JSON instead of the tables
  --out FILE              also write the JSON here
  --quiet                 print nothing but what --out or --json asked for

  The scene is silent by default. The synthetic generator is single threaded
  and a populated scene at 20 MS/s is an order of magnitude slower than the
  engine, which would make every receiver count report the generator's rate
  rather than the engine's. The measured source ceiling is printed first so
  the headroom is visible rather than assumed.

  throughput reports and never judges: it has no threshold and always exits 0
  on a completed run.

exit codes:
  0  ok
  1  regression, or validation outside the band
  2  bad usage
  3  an error while running
)";

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
        args.emplace_back(argv[index]);
    }

    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        std::print("{}", kUsage);
        return args.empty() ? kExitUsage : kExitOk;
    }

    const std::string_view command = args[0];
    const std::span<const std::string_view> rest(args.begin() + 1, args.end());

    Expected<ArgMap> parsed = ArgMap::parse(rest);
    if (!parsed) {
        std::print(stderr, "bench: {}\n", parsed.error().message);
        return kExitUsage;
    }

    if (command == "sweep") {
        return command_sweep(*parsed);
    }
    if (command == "compare") {
        return command_compare(*parsed);
    }
    if (command == "validate") {
        return command_validate(*parsed);
    }
    if (command == "throughput") {
        return command_throughput(*parsed);
    }

    std::print(stderr, "bench: unknown command '{}'\n", command);
    std::print(stderr, "{}", kUsage);
    return kExitUsage;
}
