#include "core/dsp/synth/cw_mod.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <format>
#include <numbers>
#include <random>

namespace revenant::siggen {
namespace {

constexpr double kPi = std::numbers::pi;

// Half-amplitude-centred raised cosine: 0 before -width/2, 1 after +width/2.
double ramp(double x, double width) {
    if (width <= 0.0) {
        return x >= 0.0 ? 1.0 : 0.0;
    }
    if (x <= -0.5 * width) {
        return 0.0;
    }
    if (x >= 0.5 * width) {
        return 1.0;
    }
    return 0.5 * (1.0 + std::sin(kPi * x / width));
}

}  // namespace

Expected<std::vector<CwKeyRun>> cw_key_runs(const CwModConfig& config, std::string_view text) {
    if (!(config.wpm > 0.0)) {
        return fail(std::format("cw_key_runs needs a positive speed; got {}", config.wpm));
    }
    const double overall = (config.overall_wpm > 0.0) ? config.overall_wpm : config.wpm;
    auto spacing = decode::morse_spacing(config.wpm, overall);
    if (!spacing) {
        return std::unexpected(spacing.error());
    }
    auto tokens = decode::morse_tokens(text);
    if (!tokens) {
        return std::unexpected(with_context(tokens.error(), "keying CW text"));
    }

    std::mt19937_64 random(config.seed);
    std::normal_distribution<double> normal(0.0, 1.0);
    const auto stretch = [&](double seconds) {
        if (config.jitter <= 0.0) {
            return seconds;
        }
        return seconds * std::max(0.3, 1.0 + config.jitter * normal(random));
    };

    const double unit = spacing->unit_s;
    std::vector<CwKeyRun> runs;
    runs.push_back(CwKeyRun{false, config.lead_in_seconds});

    bool pending_space = false;
    bool pending_word = false;
    for (const std::string& token : *tokens) {
        if (token == " ") {
            pending_word = pending_space || pending_word;
            continue;
        }
        if (pending_space) {
            // Clause 2.3 or 2.4, or their Farnsworth stretch.
            runs.push_back(CwKeyRun{
                false, stretch(pending_word ? spacing->word_space_s : spacing->letter_space_s)});
        }
        auto code = decode::morse_code_for(token);
        if (!code) {
            return std::unexpected(code.error());
        }
        for (std::size_t i = 0; i < code->size(); ++i) {
            if (i > 0) {
                // Clause 2.2: one dot between the elements of a letter.
                runs.push_back(CwKeyRun{false, stretch(decode::kMorseElementSpaceDots * unit)});
            }
            // Clause 2.1: a dash is three dots.
            const double length = ((*code)[i] == '-') ? decode::kMorseDashDots * unit : unit;
            runs.push_back(CwKeyRun{true, stretch(length)});
        }
        pending_space = true;
        pending_word = false;
    }
    runs.push_back(CwKeyRun{false, config.tail_seconds});
    return runs;
}

Expected<std::vector<Complex32>> cw_render_analytic(const CwModConfig& config,
                                                    std::string_view text) {
    if (config.rate <= 0 || config.tone_hz <= 0 || 2 * config.tone_hz >= config.rate) {
        return fail(std::format(
            "cw_render_analytic needs a tone inside the audio band; got {} Hz at {} Hz",
            config.tone_hz, config.rate));
    }
    auto runs = cw_key_runs(config, text);
    if (!runs) {
        return std::unexpected(runs.error());
    }

    // Key-down intervals in seconds.
    struct Interval {
        double begin;
        double end;
    };
    std::vector<Interval> down;
    double t = 0.0;
    for (const CwKeyRun& run : *runs) {
        if (run.key_down) {
            down.push_back(Interval{t, t + run.seconds});
        }
        t += run.seconds;
    }

    const double rate = static_cast<double>(config.rate);
    const auto samples = static_cast<std::size_t>(std::ceil(t * rate));
    std::vector<Complex32> out(samples);
    const auto rate_u = static_cast<std::uint64_t>(config.rate);
    const auto tone_u = static_cast<std::uint64_t>(config.tone_hz);
    const double width = config.edge_seconds;

    std::size_t first = 0;
    for (std::size_t n = 0; n < samples; ++n) {
        const double now = static_cast<double>(n) / rate;
        while (first < down.size() && down[first].end + width < now) {
            ++first;
        }
        double envelope = 0.0;
        for (std::size_t k = first; k < down.size() && down[k].begin - width <= now; ++k) {
            const double value =
                std::min(ramp(now - down[k].begin, width), ramp(down[k].end - now, width));
            envelope = std::max(envelope, value);
        }
        const std::uint64_t cycle = (static_cast<std::uint64_t>(n) % rate_u) * tone_u % rate_u;
        const std::complex<double> value =
            std::polar(config.amplitude * envelope, 2.0 * kPi * static_cast<double>(cycle) / rate);
        out[n] = Complex32{static_cast<float>(value.real()), static_cast<float>(value.imag())};
    }
    return out;
}

}  // namespace revenant::siggen
