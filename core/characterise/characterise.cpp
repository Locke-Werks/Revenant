#include "core/characterise/characterise.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace revenant::characterise {
namespace {

[[nodiscard]] std::string hertz(double value) {
    // Anything under a twentieth of a hertz is zero for this stage's
    // purposes and printing it as 1.137e-13 Hz, which a centred tone set
    // produces, reads as a measurement rather than as the rounding it is.
    if (std::abs(value) < 0.05) {
        return "0 Hz";
    }
    if (std::abs(value) >= 1000.0) {
        return std::format("{:.5g} kHz", value / 1000.0);
    }
    return std::format("{:.5g} Hz", value);
}

[[nodiscard]] std::string seconds(double value) {
    if (value < 1.0e-3) {
        return std::format("{:.3g} us", value * 1.0e6);
    }
    return std::format("{:.4g} ms", value * 1000.0);
}

// The clause that names what the numbers are consistent with, or says
// plainly that nothing in the catalogue matches. The second half is a
// finding rather than a gap, and phrasing it as one is the point: a clean
// measurement of a system nobody has a document for is a correct result.
[[nodiscard]] std::string candidate_clause(const std::vector<ProtocolCandidate>& candidates) {
    if (candidates.empty()) {
        return "; consistent with nothing in the catalogue, which is a result rather than a "
               "gap: docs/modes.md's out-of-scope section is full of systems with no obtainable "
               "document";
    }
    return "; consistent with " + summarise_candidates(candidates);
}

// The extract's own power at the power law's carrier, over the median power
// across the occupied band, taken at whichever of the carrier's M-fold
// aliases stands highest: the law reads a line at M times the carrier, so the
// carrier it reports is only known modulo rate/M. Three bins around each
// alias, so one bin's variance does not decide it. Negative when there is no
// band or no order to measure against.
[[nodiscard]] double carrier_level(const ModulationOrder& order, const OccupiedBand& band,
                                   const PowerSpectrum& spectrum)
{
    if (!order.found || order.order <= 0 || !band.found || spectrum.bins.empty()) {
        return -1.0;
    }
    std::vector<double> inside;
    for (std::size_t k = 0; k < spectrum.bins.size(); ++k) {
        const double f = spectrum.frequency_at(static_cast<double>(k));
        if (f >= band.low_hz && f <= band.high_hz) {
            inside.push_back(spectrum.bins[k]);
        }
    }
    if (inside.empty()) {
        return -1.0;
    }
    const std::size_t middle = inside.size() / 2;
    std::nth_element(inside.begin(), inside.begin() + static_cast<std::ptrdiff_t>(middle),
                     inside.end());
    const double typical = inside[middle];
    if (!(typical > 0.0)) {
        return -1.0;
    }

    const std::size_t count = spectrum.bins.size();
    const double alias = static_cast<double>(spectrum.rate) / static_cast<double>(order.order);
    double best = 0.0;
    for (int k = -order.order; k <= order.order; ++k) {
        const std::size_t at =
            spectrum.bin_at(order.carrier_offset_hz + static_cast<double>(k) * alias);
        if (at >= count) {
            continue;
        }
        double sum = 0.0;
        for (std::size_t d = 0; d < 3; ++d) {
            sum += spectrum.bins[(at + count - 1 + d) % count];
        }
        best = std::max(best, sum / 3.0);
    }
    return best / typical;
}

// The two strongest lines and the third, for the tone-pair rule on
// CharacteriseConfig::tone_pair_fraction. Each line is a three-bin window of
// excess power over the band's floor, wrapping on a two-sided spectrum, and
// each later window is taken at least four bins from every earlier one so a
// line's own skirt is not counted as its neighbour.
struct TonePair {
    double share = -1.0;
    double third = -1.0;
    double spacing_hz = 0.0;
};

[[nodiscard]] TonePair tone_pair(const PowerSpectrum& spectrum, const OccupiedBand& band)
{
    TonePair out;
    const std::size_t size = spectrum.bins.size();
    if (size < 16) {
        return out;
    }
    const double floor = band.found ? band.noise_floor : 0.0;
    std::vector<double> excess(size);
    double total = 0.0;
    for (std::size_t k = 0; k < size; ++k) {
        excess[k] = std::max(spectrum.bins[k] - floor, 0.0);
        total += excess[k];
    }
    if (!(total > 0.0)) {
        return out;
    }

    const auto window = [&](std::size_t k) {
        return excess[(k + size - 1) % size] + excess[k] + excess[(k + 1) % size];
    };
    const auto apart = [size](std::size_t a, std::size_t b) {
        const std::size_t d = a > b ? a - b : b - a;
        return std::min(d, size - d) >= 4;
    };

    std::size_t picked[3] = {size, size, size};
    double power[3] = {0.0, 0.0, 0.0};
    for (std::size_t line = 0; line < 3; ++line) {
        for (std::size_t k = 0; k < size; ++k) {
            bool clear = true;
            for (std::size_t earlier = 0; earlier < line; ++earlier) {
                clear = clear && apart(k, picked[earlier]);
            }
            if (!clear) {
                continue;
            }
            const double here = window(k);
            if (picked[line] == size || here > power[line]) {
                picked[line] = k;
                power[line] = here;
            }
        }
    }
    if (picked[1] == size) {
        return out;
    }
    out.share = (power[0] + power[1]) / total;
    out.third = power[1] > 0.0 ? power[2] / power[1] : 1.0;
    out.spacing_hz = std::abs(spectrum.frequency_at(static_cast<double>(picked[0])) -
                              spectrum.frequency_at(static_cast<double>(picked[1])));
    return out;
}

// The double-sideband reading on CharacteriseConfig::am_sideband_share. The
// carrier is the strongest three-bin window; each bin at least the minimum
// offset from it, out to the occupied band's farther edge, is paired with its
// mirror.
struct Sidebands {
    double share = -1.0;
    double symmetry = -1.0;
};

[[nodiscard]] Sidebands sidebands(const PowerSpectrum& spectrum, const OccupiedBand& band,
                                  double min_offset_hz)
{
    Sidebands out;
    const std::size_t size = spectrum.bins.size();
    if (size < 16 || !band.found || !(spectrum.bin_width_hz > 0.0)) {
        return out;
    }
    std::vector<double> excess(size);
    double total = 0.0;
    for (std::size_t k = 0; k < size; ++k) {
        excess[k] = std::max(spectrum.bins[k] - band.noise_floor, 0.0);
        total += excess[k];
    }
    if (!(total > 0.0)) {
        return out;
    }
    std::size_t carrier = 0;
    double loudest = -1.0;
    for (std::size_t k = 0; k < size; ++k) {
        const double here =
            excess[(k + size - 1) % size] + excess[k] + excess[(k + 1) % size];
        if (here > loudest) {
            loudest = here;
            carrier = k;
        }
    }
    const double carrier_hz = spectrum.frequency_at(static_cast<double>(carrier));
    const double reach_hz =
        std::max(std::abs(band.high_hz - carrier_hz), std::abs(carrier_hz - band.low_hz));
    const auto first = static_cast<std::size_t>(std::ceil(min_offset_hz / spectrum.bin_width_hz));
    const auto last = std::min(static_cast<std::size_t>(reach_hz / spectrum.bin_width_hz),
                               size / 2 - 1);
    double side = 0.0;
    double smaller = 0.0;
    double larger = 0.0;
    for (std::size_t d = first; d <= last; ++d) {
        const double upper = excess[(carrier + d) % size];
        const double lower = excess[(carrier + size - d) % size];
        side += upper + lower;
        smaller += std::min(upper, lower);
        larger += std::max(upper, lower);
    }
    out.share = side / total;
    out.symmetry = larger > 0.0 ? smaller / larger : 0.0;
    return out;
}

}  // namespace

Expected<Characterisation> characterise(dsp::ConstComplexSpan samples,
                                        const CharacteriseConfig& config)
{
    if (config.rate <= 0) {
        return fail(std::format(
            "characterise: CharacteriseConfig::rate must be positive, was {}. The stage has no "
            "way to turn a bin or a lag into hertz without it.",
            config.rate));
    }
    if (samples.size() < kMinCharacteriseSamples) {
        return fail(std::format(
            "characterise: {} samples is under the {} this stage needs, which at {} S/s is "
            "{:.3g} seconds. The tone histogram wants 4096 sample pairs past its amplitude "
            "gate, the autocorrelation divides by its own overlap, and the spectral estimate "
            "averages sixteen segments; none of the three produces a usable floor on a buffer "
            "this short. Capture a longer extract.",
            samples.size(), kMinCharacteriseSamples, config.rate,
            static_cast<double>(kMinCharacteriseSamples) / static_cast<double>(config.rate)));
    }

    Characterisation out;
    const std::vector<Complex64> widened = to_analysis(samples);

    out.envelope = envelope_stats(widened);
    if (!(out.envelope.mean_power > 0.0)) {
        return fail("characterise: the extract carries no power at all. Check the receiver "
                    "reached this call and that the buffer is the one that was filled.");
    }

    // The caller's transform length when it named one, so two extracts of
    // different lengths can be compared. See CharacteriseConfig::segment for
    // the measurement that made this worth offering.
    const std::size_t segment =
        config.segment != 0 ? config.segment : analysis_segment(widened.size());

    auto spectrum = welch_spectrum(widened, config.rate, segment);
    if (!spectrum.has_value()) {
        return std::unexpected(with_context(spectrum.error(), "characterise"));
    }
    out.spectral_concentration = spectral_concentration(*spectrum);
    out.band = occupied_band(*spectrum, 0.99);

    auto tone_result = estimate_tone_structure(widened, config.rate, config.tones);
    if (!tone_result.has_value()) {
        return std::unexpected(with_context(tone_result.error(), "characterise"));
    }
    out.tones = std::move(*tone_result);

    auto envelope_rate = estimate_symbol_rate(widened, config.rate,
                                              CyclicDetector::SquaredEnvelope, config.cyclic);
    if (!envelope_rate.has_value()) {
        return std::unexpected(with_context(envelope_rate.error(), "characterise"));
    }
    out.squared_envelope = std::move(*envelope_rate);

    auto transition_rate = estimate_symbol_rate(
        widened, config.rate, CyclicDetector::FrequencyTransition, config.cyclic);
    if (!transition_rate.has_value()) {
        return std::unexpected(with_context(transition_rate.error(), "characterise"));
    }
    out.frequency_transition = std::move(*transition_rate);

    auto order = estimate_modulation_order(widened, config.rate, config.cyclic.threshold_db);
    if (!order.has_value()) {
        return std::unexpected(with_context(order.error(), "characterise"));
    }
    out.order = std::move(*order);

    auto profile = autocorrelation(widened, config.rate);
    if (!profile.has_value()) {
        return std::unexpected(with_context(profile.error(), "characterise"));
    }
    out.ofdm = find_cyclic_prefix(*profile, config.ofdm);
    out.frame = find_frame_period(*profile, config.frame);

    const bool constant_envelope =
        out.envelope.normalised_power_variance < config.constant_envelope_variance;

    // Whether the power law's carrier sits where the extract has no power of
    // its own, which is where band-limited noise puts one. See
    // CharacteriseConfig::psk_carrier_level_fraction for the measurement.
    out.psk_carrier_level = carrier_level(out.order, out.band, *spectrum);
    out.psk_carrier_outside_band =
        out.psk_carrier_level >= 0.0 && out.psk_carrier_level < config.psk_carrier_level_fraction;

    // Whether what the PSK branch would call a symbol clock is the gap between
    // two lines. See CharacteriseConfig::tone_pair_fraction. The rate is the
    // one the branch would report, so the test is against the claim it would
    // make rather than against either detector on its own.
    {
        const TonePair pair = tone_pair(*spectrum, out.band);
        out.tone_pair_share = pair.share;
        out.tone_pair_third = pair.third;
        out.tone_pair_spacing_hz = pair.spacing_hz;
        const SymbolRateEstimate& claimed =
            out.squared_envelope.found ? out.squared_envelope : out.frequency_transition;
        if (claimed.found && pair.share >= config.tone_pair_fraction &&
            pair.third < config.tone_pair_third_fraction) {
            const double tolerance =
                std::max(2.0 * spectrum->bin_width_hz,
                         config.tone_pair_rate_tolerance * claimed.symbol_rate_hz);
            out.psk_tone_pair = std::abs(pair.spacing_hz - claimed.symbol_rate_hz) <= tolerance;
        }
    }

    ProtocolQuery query;
    query.bandwidth_hz = out.band.found ? out.band.bandwidth_hz : 0.0;

    // The order of these branches is the whole classifier and each one is
    // ahead of the next for a reason rather than by convenience.
    if (out.spectral_concentration >= config.carrier_concentration) {
        // Nothing else can be read off a signal whose energy is one line.
        out.family = ModulationFamily::Unmodulated;
        out.family_confidence = std::clamp(out.spectral_concentration, 0.0, 1.0);

        // Whether the carrier has mirrored sidebands, which is what a label
        // reads as AM. See CharacteriseConfig::am_sideband_share.
        const Sidebands sides = sidebands(*spectrum, out.band, config.am_sideband_min_offset_hz);
        out.sideband_share = sides.share;
        out.sideband_symmetry = sides.symmetry;
        const bool mirrored = sides.share >= config.am_sideband_share &&
                              sides.symmetry >= config.am_sideband_symmetry;

        // Mirrored sidebands on a constant envelope are frequency modulation
        // at a low index, not AM: AM's sidebands are its envelope. Voice on
        // narrowband FM keeps most of its power in the carrier, so it
        // reaches the concentration bar above and would otherwise be called a
        // carrier. See CharacteriseConfig::am_sideband_share.
        //
        // The envelope is judged net of what the extract's own noise puts on
        // it, which the plain constant_envelope test is not: a constant
        // envelope of power S in complex Gaussian noise of power N reads a
        // normalised power variance of (2SN + N^2) / (S + N)^2, and the
        // spectrum's floor is N, because a Welch bin reads the per-sample
        // variance of white noise. Without the correction low-index FM at
        // 20 dB in 2500 Hz read 0.089, over the 0.05 bar, and was called AM.
        const double noise = out.band.found ? out.band.noise_floor : 0.0;
        const double signal = std::max(out.envelope.mean_power - noise, 0.0);
        const double total = signal + noise;
        const double from_noise =
            total > 0.0 ? (2.0 * signal * noise + noise * noise) / (total * total) : 0.0;
        out.envelope_variance_net = out.envelope.normalised_power_variance - from_noise;
        const bool flat = out.envelope_variance_net < config.constant_envelope_variance;
        out.double_sideband = mirrored && !flat;
        out.low_index_fm = mirrored && flat;
        out.summary = std::format(
            "an unmodulated carrier: {:.1f} percent of the extract's power is in three adjacent "
            "bins at {}, and its instantaneous frequency spans {}",
            100.0 * out.spectral_concentration, hertz(out.band.centre_hz),
            hertz(out.tones.frequency_spread_hz));
        if (out.double_sideband) {
            out.summary += std::format(
                ", with {:.1f} percent of the band's excess power in sidebands that mirror each "
                "other to {:.2f} about it, which is double sideband",
                100.0 * out.sideband_share, out.sideband_symmetry);
        }
        if (out.low_index_fm) {
            // Held to a half for the reason the AnalogueFm branch below is:
            // an elimination, and a continuous-phase digital mode at a low
            // index would read the same.
            out.family = ModulationFamily::AnalogueFm;
            out.family_confidence = 0.5;
            query.family = ModulationFamily::AnalogueFm;
            out.candidates = match_protocols(query);
            out.summary = std::format(
                "frequency modulation at a low index: {:.1f} percent of the extract's power is "
                "in the carrier at {}, the envelope is constant, and {:.1f} percent sits in "
                "sidebands mirroring to {:.2f} about it, which an unmodulated carrier does not "
                "have and AM would carry on its envelope{}",
                100.0 * out.spectral_concentration, hertz(out.band.centre_hz),
                100.0 * out.sideband_share, out.sideband_symmetry,
                candidate_clause(out.candidates));
        }
    } else if (out.ofdm.found) {
        // Before the tone test, because an OFDM waveform's instantaneous
        // frequency is a mess with no tones in it and would fall through
        // to the analogue branch. A cyclic prefix is a positive finding
        // and the others below are eliminations.
        out.family = ModulationFamily::Ofdm;
        out.family_confidence = out.ofdm.confidence;
        query.family = ModulationFamily::Ofdm;
        query.ofdm_symbol_seconds = out.ofdm.symbol_seconds;
        out.candidates = match_protocols(query);
        out.summary = std::format(
            "OFDM: a {} useful symbol, so {} between subcarriers, with a guard of about {} "
            "samples read off a correlation of {:.3f}; occupying {}{}",
            seconds(out.ofdm.symbol_seconds), hertz(out.ofdm.subcarrier_spacing_hz),
            out.ofdm.prefix_samples, out.ofdm.correlation, hertz(out.band.bandwidth_hz),
            candidate_clause(out.candidates));
    } else if (out.tones.found) {
        out.family = ModulationFamily::Fsk;
        out.family_confidence = out.tones.confidence;
        // The transition detector first: a tone set is constant envelope
        // by construction, which is the case the squared envelope cannot
        // read at all.
        out.symbol_rate = out.frequency_transition.found ? out.frequency_transition
                                                         : out.squared_envelope;
        query.family = ModulationFamily::Fsk;
        query.tone_count = out.tones.tone_count;
        query.symbol_rate_hz = out.symbol_rate.found ? out.symbol_rate.symbol_rate_hz : 0.0;
        out.candidates = match_protocols(query);
        out.summary = std::format(
            "{}-FSK at {}, tones {} apart centred on {}, occupying {}{}", out.tones.tone_count,
            out.symbol_rate.found ? hertz(out.symbol_rate.symbol_rate_hz)
                                  : std::string("an unmeasured symbol rate"),
            hertz(out.tones.spacing_hz), hertz(out.tones.centre_offset_hz),
            hertz(out.band.bandwidth_hz), candidate_clause(out.candidates));
    } else if (constant_envelope && out.band.found &&
               out.tones.frequency_spread_hz >
                   config.analogue_fm_spread_fraction * out.band.bandwidth_hz) {
        // Constant envelope rules out every linear modulation, and the
        // tone test has already said the instantaneous frequency is a
        // continuum rather than a set of values. That is frequency
        // modulation by something continuous.
        out.family = ModulationFamily::AnalogueFm;
        // Half, and no more. This branch is an elimination rather than a
        // positive finding, and it cannot separate analogue FM from a
        // continuous-phase digital mode whose index is low enough to
        // merge its tones. Reporting it above half would claim a
        // discrimination that was not made.
        out.family_confidence = 0.5;
        query.family = ModulationFamily::AnalogueFm;
        out.candidates = match_protocols(query);
        // The cycle frequency is reported and not attributed, which is a
        // different thing from suppressing it. On a broadcast carrier the
        // frequency-transition feature carries lines at the composite's
        // own tones, and on a 4FSK signal too weak for its tones to
        // separate it carries one at the symbol rate. The two are the
        // same measurement and nothing here tells them apart, so the
        // number goes in the summary saying exactly that, rather than
        // being thrown away or being called a symbol rate.
        const std::string cycle =
            out.frequency_transition.found
                ? std::format(
                      ". A cycle frequency of {} stands {:.1f} dB up in the phase-curvature "
                      "feature. On an analogue carrier that is a modulation tone; on a digital "
                      "one it would be the symbol rate. Nothing here decides which, so it is "
                      "not reported as a symbol rate",
                      hertz(out.frequency_transition.symbol_rate_hz),
                      out.frequency_transition.margin_db)
                : std::string();
        out.summary = std::format(
            "constant envelope with a continuously distributed instantaneous frequency spanning "
            "{} inside {}: analogue FM, or a continuous-phase digital mode whose tones this "
            "extract cannot separate{}{}",
            hertz(out.tones.frequency_spread_hz), hertz(out.band.bandwidth_hz), cycle,
            candidate_clause(out.candidates));
    } else if (out.order.found && !constant_envelope && !out.psk_carrier_outside_band &&
               !out.psk_tone_pair) {
        out.family = ModulationFamily::Psk;
        out.family_confidence = out.order.confidence;
        out.symbol_rate =
            out.squared_envelope.found ? out.squared_envelope : out.frequency_transition;
        query.family = ModulationFamily::Psk;
        query.psk_order = out.order.order;
        query.symbol_rate_hz = out.symbol_rate.found ? out.symbol_rate.symbol_rate_hz : 0.0;
        out.candidates = match_protocols(query);
        out.summary = std::format(
            "{}-PSK at {}, carrier {} from the extract's own centre modulo {}, occupying {}{}",
            out.order.order,
            out.symbol_rate.found ? hertz(out.symbol_rate.symbol_rate_hz)
                                  : std::string("an unmeasured symbol rate"),
            hertz(out.order.carrier_offset_hz),
            hertz(static_cast<double>(config.rate) / static_cast<double>(out.order.order)),
            hertz(out.band.bandwidth_hz), candidate_clause(out.candidates));

        // Kept, flagged and capped rather than refused, and the reason is on
        // kPskWithoutRateConfidence: real PSK loses its rate before its order,
        // and a carrier in noise lights the same order line with no rate at
        // all, at confidences the two share.
        if (!out.symbol_rate.found) {
            out.psk_without_symbol_rate = true;
            out.family_confidence = std::min(out.family_confidence, kPskWithoutRateConfidence);
            out.summary += std::format(
                ". No symbol rate was measured, so this rests on the M-th power line alone, "
                "which a bare carrier in noise lights the same way: confidence held to {:.2f} "
                "and not to be used to drive detection",
                kPskWithoutRateConfidence);
        }
    }

    // A signal cannot be keyed faster than it is wide. See
    // CharacteriseConfig::detection_bandwidth_hz for the measurement. Applied
    // to whatever family carried a rate, which is PSK and FSK: the others
    // never report one.
    double refused_rate = 0.0;
    if (config.detection_bandwidth_hz > 0.0 && out.symbol_rate.found &&
        out.symbol_rate.symbol_rate_hz > config.detection_bandwidth_hz &&
        (out.family == ModulationFamily::Psk || out.family == ModulationFamily::Fsk)) {
        refused_rate = out.symbol_rate.symbol_rate_hz;
        out.symbol_rate_exceeds_detection = true;
        out.family = ModulationFamily::Unknown;
        out.family_confidence = 0.0;
        out.psk_without_symbol_rate = false;
        out.candidates.clear();
    }

    if (out.family == ModulationFamily::Unknown) {
        // Nothing carried a family. The frame period is the one finding
        // that can stand without one, because a repeat is a repeat
        // whatever is repeating, so it is reported rather than swallowed.
        out.refused = !out.frame.found;
        out.refusal = std::format(
            "no modulation family was established. The envelope's normalised power variance is "
            "{:.4f}, against {:.2f} for a constant envelope and exactly 1 for pure complex "
            "Gaussian noise. {:.2f} percent of the power is in three adjacent bins, against "
            "the {:.0f} percent an unmodulated carrier reaches. The tone histogram said: {} "
            "The squared envelope said: {} The frequency transition said: {} The cyclic "
            "prefix search said: {}",
            out.envelope.normalised_power_variance, config.constant_envelope_variance,
            100.0 * out.spectral_concentration, 100.0 * config.carrier_concentration,
            out.tones.refusal.empty() ? "it found a tone set." : out.tones.refusal,
            out.squared_envelope.refusal.empty() ? "it found a symbol rate."
                                                 : out.squared_envelope.refusal,
            out.frequency_transition.refusal.empty() ? "it found a symbol rate."
                                                     : out.frequency_transition.refusal,
            out.ofdm.refusal.empty() ? "it found a cyclic prefix." : out.ofdm.refusal);

        if (out.psk_carrier_outside_band) {
            out.refusal += std::format(
                " The M-th power law found an order-{} line, {:.1f} dB up, and put its carrier at "
                "{}, where the extract's own power is {:.2f} of the median across its occupied "
                "band, against the {:.2f} a carrier has to reach. A linear modulation's spectrum "
                "peaks at its carrier; a carrier where the band's power has fallen away is the "
                "corner band-limited noise leaves at its edge, not a line. So no PSK call was "
                "made from it.",
                out.order.order, out.order.margin_db, hertz(out.order.carrier_offset_hz),
                out.psk_carrier_level, config.psk_carrier_level_fraction);
        }

        if (out.psk_tone_pair) {
            out.refusal += std::format(
                " The M-th power law found an order-{} line and a symbol clock was read at {}, "
                "but {:.2f} of the band's excess power is in two lines {} apart, with the next "
                "line at {:.2f} of the weaker. Two tones square to a line at their difference, so "
                "that clock is the gap between two carriers, not a symbol rate, and no PSK call "
                "was made from it.",
                out.order.order,
                hertz(out.squared_envelope.found ? out.squared_envelope.symbol_rate_hz
                                                 : out.frequency_transition.symbol_rate_hz),
                out.tone_pair_share, hertz(out.tone_pair_spacing_hz), out.tone_pair_third);
        }

        if (out.symbol_rate_exceeds_detection) {
            out.refusal += std::format(
                " A family was found at a symbol rate of {}, wider than the {} the detection it "
                "was asked about occupies. A signal cannot be keyed faster than it is wide, so "
                "the rate is a modulating tone or the gap between lines, and the family was "
                "refused.",
                hertz(refused_rate), hertz(config.detection_bandwidth_hz));
        }

        if (out.frame.found) {
            out.summary = std::format(
                "unidentified, but something repeats every {} ({} samples) at a correlation of "
                "{:.3f}. A frame period with no modulation family under it is still worth "
                "recording: it is what a probe or a sync word in the clear produces, and it "
                "does not need the payload",
                seconds(out.frame.period_seconds), out.frame.period_samples,
                out.frame.repeat_fraction);
        } else {
            out.summary = "unidentified: " + out.refusal;
        }
    } else if (out.frame.found) {
        // Two cases where the repeat is already accounted for and saying
        // it again would read as a second, independent finding.
        //
        // On OFDM the guard interval IS the repeat, at the useful symbol
        // length, so the frame reader finds the same lag the cyclic
        // prefix reader did and the summary has already named it.
        //
        // On an analogue carrier the modulating waveform comes round
        // again, and a 1 kHz programme tone at 684 kS/s repeats every 684
        // samples at a correlation of one. That is the modulation's own
        // period and it is not framing; calling it a frame period would
        // put a digital word on an analogue measurement.
        const bool is_the_prefix = out.family == ModulationFamily::Ofdm &&
                                   out.frame.period_samples == out.ofdm.symbol_samples;
        if (out.family == ModulationFamily::AnalogueFm) {
            out.summary += std::format(
                ". The modulating waveform itself repeats every {}, which is its own period "
                "rather than a frame", seconds(out.frame.period_seconds));
        } else if (!is_the_prefix) {
            out.summary += std::format(". Something also repeats every {} ({} samples) at a "
                                       "correlation of {:.3f}",
                                       seconds(out.frame.period_seconds),
                                       out.frame.period_samples, out.frame.repeat_fraction);
        }
    }

    return out;
}

bool may_drive_detection(const Characterisation& result)
{
    return result.family != ModulationFamily::Unknown && !result.psk_without_symbol_rate;
}

}  // namespace revenant::characterise
