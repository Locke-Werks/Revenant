#include "core/detect/tier_two.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <tuple>

namespace revenant::detect {

Expected<TierTwo> TierTwo::create(const TierTwoConfig& config) {
    if (config.source_rate <= 0) {
        return fail("TierTwo needs the source rate to turn sample indices into seconds");
    }
    if (!std::isfinite(config.reprobe_seconds) || config.reprobe_seconds < 0.0) {
        return fail(std::format("reprobe_seconds must be zero or more, got {}",
                                config.reprobe_seconds));
    }
    TierTwo out;
    out.config_ = config;
    return out;
}

std::vector<std::uint64_t> TierTwo::pick(
    std::span<const Track> tracks,
    const std::unordered_map<std::uint64_t, dsp::SampleIndex>& attempts,
    std::span<const std::uint64_t> in_flight, std::size_t free, dsp::SampleIndex now,
    dsp::SampleRate source_rate, double reprobe_seconds) {
    const auto reprobe_samples =
        static_cast<dsp::SampleIndex>(reprobe_seconds * static_cast<double>(source_rate));

    // (never probed first, then the key, then the id so the order is total).
    // The key is the birth for a track never probed and the last submission
    // for one that has been, and oldest goes first in both.
    std::vector<std::tuple<int, dsp::SampleIndex, std::uint64_t>> eligible;
    for (const Track& track : tracks) {
        if (track.state != TrackState::Live) {
            continue;
        }
        if (track.classification != Classification::Unknown) {
            continue;
        }
        if (std::find(in_flight.begin(), in_flight.end(), track.id) != in_flight.end()) {
            continue;
        }
        const auto tried = attempts.find(track.id);
        if (tried == attempts.end()) {
            eligible.emplace_back(0, track.first_seen, track.id);
            continue;
        }
        if (now < tried->second + reprobe_samples) {
            continue;
        }
        eligible.emplace_back(1, tried->second, track.id);
    }

    std::sort(eligible.begin(), eligible.end());
    std::vector<std::uint64_t> out;
    for (const auto& entry : eligible) {
        if (out.size() >= free) {
            break;
        }
        out.push_back(std::get<2>(entry));
    }
    return out;
}

void TierTwo::take(Detector& detector, engine::Engine& engine) {
    if (outcomes_.size() < 32) {
        outcomes_.resize(32);
    }
    for (;;) {
        const std::size_t count = engine.take_probe_outcomes(outcomes_);
        for (std::size_t i = 0; i < count; ++i) {
            const engine::ProbeOutcome& outcome = outcomes_[i];
            std::erase(in_flight_, outcome.tag);
            statuses_[outcome.tag] = outcome.status;

            switch (outcome.status) {
                case engine::ProbeStatus::Characterised: break;
                case engine::ProbeStatus::TooWide: ++stats_.too_wide; continue;
                case engine::ProbeStatus::Unplaced: ++stats_.unplaced; continue;
                case engine::ProbeStatus::Cancelled: ++stats_.cancelled; continue;
                case engine::ProbeStatus::Failed: ++stats_.failed; continue;
            }

            ProbeFinding finding;
            finding.family = classification_of(outcome.family);
            finding.confidence = outcome.confidence;
            finding.symbol_rate_hz = outcome.symbol_rate_hz;
            finding.order = outcome.order;
            finding.tone_count = outcome.tone_count;
            finding.concentration = outcome.concentration;
            finding.may_drive_detection = outcome.may_drive_detection;
            finding.psk_without_symbol_rate = outcome.psk_without_symbol_rate;

            // Whether this is the track's first family, read before the
            // detector writes it, so the time to first classification is
            // taken once per track and not once per accepted probe.
            bool first = false;
            dsp::SampleIndex born = 0;
            for (const Track& track : detector.tracks()) {
                if (track.id == outcome.tag) {
                    first = track.classification == Classification::Unknown;
                    born = track.first_seen;
                    break;
                }
            }

            if (!detector.record_probe(outcome.tag, finding)) {
                ++stats_.orphaned;
                continue;
            }
            ++stats_.recorded;
            const auto family = static_cast<std::size_t>(finding.family);
            ++stats_.named[family];
            if (!finding.may_drive_detection) {
                ++stats_.refused_by_characterise;
                continue;
            }
            ++stats_.accepted;
            ++stats_.accepted_as[family];
            if (first) {
                const double seconds = static_cast<double>(detector.last_decision() - born) /
                                       static_cast<double>(config_.source_rate);
                stats_.first_classification_seconds_min =
                    stats_.first_classifications == 0
                        ? seconds
                        : std::min(stats_.first_classification_seconds_min, seconds);
                stats_.first_classification_seconds_max =
                    std::max(stats_.first_classification_seconds_max, seconds);
                stats_.first_classification_seconds_total += seconds;
                ++stats_.first_classifications;
            }
        }
        if (count < outcomes_.size()) {
            break;
        }
    }
}

std::optional<engine::ProbeStatus> TierTwo::last_status(std::uint64_t track_id) const {
    const auto found = statuses_.find(track_id);
    if (found == statuses_.end()) {
        return std::nullopt;
    }
    return found->second;
}

Status TierTwo::step(Detector& detector, engine::Engine& engine) {
    take(detector, engine);

    const std::span<const Track> tracks = detector.tracks();

    // Forget attempts on tracks that have gone, so the maps are bounded by the
    // track list rather than by the length of the run.
    const auto gone = [&](const auto& entry) {
        return std::none_of(tracks.begin(), tracks.end(),
                            [&](const Track& track) { return track.id == entry.first; });
    };
    std::erase_if(attempts_, gone);
    std::erase_if(statuses_, gone);

    const engine::ProbeStats pool = engine.probe_stats();
    if (pool.size == 0 || in_flight_.size() >= pool.size) {
        return {};
    }
    const std::size_t free = pool.size - in_flight_.size();

    const dsp::SampleIndex now = detector.last_decision();
    const std::vector<std::uint64_t> chosen = pick(tracks, attempts_, in_flight_, free, now,
                                                   config_.source_rate, config_.reprobe_seconds);
    if (chosen.empty()) {
        return {};
    }

    const dsp::Hertz source_center = engine.info().source_center;
    for (const std::uint64_t id : chosen) {
        const auto found = std::find_if(tracks.begin(), tracks.end(),
                                        [id](const Track& track) { return track.id == id; });
        if (found == tracks.end()) {
            continue;
        }

        engine::ProbeRequest request{};
        request.tag = id;
        request.center = found->center - source_center;
        request.occupied_hz = found->bandwidth;

        if (auto submitted = engine.submit_probe(request); !submitted) {
            ++stats_.refused;
            return std::unexpected(with_context(submitted.error(), "tier two"));
        }
        in_flight_.push_back(id);
        attempts_[id] = now;
        ++stats_.submitted;
    }
    return {};
}

}  // namespace revenant::detect
