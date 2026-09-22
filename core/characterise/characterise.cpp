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

    ProtocolQuery query;
    query.bandwidth_hz = out.band.found ? out.band.bandwidth_hz : 0.0;

    // The order of these branches is the whole classifier and each one is
    // ahead of the next for a reason rather than by convenience.
    if (out.spectral_concentration >= config.carrier_concentration) {
        // Nothing else can be read off a signal whose energy is one line.
        out.family = ModulationFamily::Unmodulated;
        out.family_confidence = std::clamp(out.spectral_concentration, 0.0, 1.0);
        out.summary = std::format(
            "an unmodulated carrier: {:.1f} percent of the extract's power is in three adjacent "
            "bins at {}, and its instantaneous frequency spans {}",
            100.0 * out.spectral_concentration, hertz(out.band.centre_hz),
            hertz(out.tones.frequency_spread_hz));
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
    } else if (out.order.found && !constant_envelope) {
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

}  // namespace revenant::characterise
