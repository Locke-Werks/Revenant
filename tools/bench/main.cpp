// The bench CLI. This is what CI calls.
//
// Three commands:
//
//   sweep     run a sweep and write the curve
//   compare   diff two curve files, exit nonzero on a regression
//   validate  run the reference BPSK subject and check the measured curve
//             against Q(sqrt(2*Eb/N0))
//
// validate is the one that keeps the other two honest. There is no decoder to
// sweep yet, so the only evidence that the harness, the SNR calibration and
// the generator agree is that the reference subject lands on the closed form.

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/error.h"
#include "tools/bench/curve.h"
#include "tools/bench/sweep.h"

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

    if (const Status ok = parsed.config.validate(); !ok) {
        return std::unexpected(ok.error());
    }

    parsed.mode = std::string(args.text("mode", "bpsk"));
    parsed.subject = std::string(args.text(
        "subject", std::format("reference-bpsk-coherent/sps{}", parsed.reference.samples_per_symbol)));
    parsed.commit = std::string(args.text("commit", "unknown"));
    parsed.out_path = std::string(args.text("out", ""));
    parsed.quiet = args.has("quiet");
    return parsed;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

void print_point_header() {
    std::println("{:>9}  {:>9}  {:>13}  {:>12}  {:>14}  {:>8}", "SNR dB", "trials", "bits", "errors", "BER",
                 "decoded");
}

void print_point(const bench::SweepPoint& point) {
    std::println("{:>9.3f}  {:>9}  {:>13}  {:>12}  {:>14.6e}  {:>7.1f}%", point.snr_db, point.trials,
                 point.bits_total, point.bit_errors, point.ber(), 100.0 * point.decode_rate());
}

void print_sweep_banner(const SweepArgs& parsed) {
    std::println("seed {}  trials/point {}  payload {} bytes ({} bits)  batch {}  early stop at {} bit errors",
                 parsed.config.base_seed, parsed.config.trials_per_point, parsed.config.payload_bytes,
                 parsed.config.payload_bytes * 8, parsed.config.trial_batch, parsed.config.min_bit_errors);
    std::println("sweep {:.3f} to {:.3f} dB Eb/N0 in {:.3f} dB steps, {} points", parsed.config.snr_start_db,
                 parsed.config.snr_stop_db, parsed.config.snr_step_db, parsed.config.point_count());
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
        print_point_header();
    }

    const bench::Subject subject = bench::make_bpsk_reference_subject(parsed->reference);
    const bench::Generator generator = bench::make_bpsk_awgn_generator(parsed->reference);
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

    const bench::RegressionOptions defaults;
    const double sensitivity = bench::sensitivity_db(curve.points, defaults.ber_threshold);
    if (!parsed->quiet) {
        if (std::isnan(sensitivity)) {
            std::println("sensitivity: the curve does not cross a BER of {:g} inside this range",
                         defaults.ber_threshold);
        } else {
            std::println("sensitivity: {:.3f} dB at a BER of {:g}", sensitivity, defaults.ber_threshold);
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
            std::println("sensitivity at a BER of {:g}: reference {:.3f} dB, candidate {:.3f} dB, delta {:+.3f} dB",
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

constexpr std::string_view kUsage = R"(revenant bench: BER/SNR sweep harness

usage:
  bench sweep    [options] [--out FILE]
  bench compare  REFERENCE.json CANDIDATE.json [options]
  bench validate [options]
  bench help

sweep options (validate takes the same set):
  --snr-start DB          first Eb/N0 point            (default -4)
  --snr-stop DB           last Eb/N0 point             (default 10)
  --snr-step DB           step between points          (default 1)
  --trials N              trials per point             (default 1000)
  --payload-bytes N       payload per trial            (default 64)
  --seed N                base seed                    (default 0)
  --min-bit-errors N      stop a point after N errors  (default 100, 0 disables)
  --batch N               trials between stop checks   (default 64)
  --threads N             worker threads               (default: hardware)
  --sps N                 samples per symbol           (default 1)
  --mode NAME             recorded in the curve        (default bpsk)
  --subject NAME          recorded in the curve
  --commit SHA            recorded in the curve        (default unknown)
  --out FILE              write the curve here, otherwise stdout
  --quiet                 suppress the per-point table

validate also takes:
  --min-check-errors N    skip points with fewer errors than this (default 20)
  --sigma N               width of the band around theory         (default 4)

compare options:
  --ber-threshold X            BER at which sensitivity is measured (default 0.01)
  --ber-ratio X                per-point worsening factor           (default 1.5)
  --sensitivity-tolerance-db X allowed rightward shift              (default 0.2)
  --quiet                      suppress the per-point table

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

    std::print(stderr, "bench: unknown command '{}'\n", command);
    std::print(stderr, "{}", kUsage);
    return kExitUsage;
}
