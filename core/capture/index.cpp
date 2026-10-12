#include "core/capture/index.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "core/detect/label.h"
#include "core/detect/tier_two.h"
#include "core/dsp/spectrum_reference.h"
#include "core/engine/engine.h"

namespace revenant::capture {

namespace {

// A file URI the registry decodes back to exactly this path. A capture root
// under a user's profile can hold a space, and a '%', '?' or '#' in a path
// would otherwise be read as an escape, a query or a fragment.
[[nodiscard]] std::string file_uri(const std::string& data_path)
{
    const std::string path = std::filesystem::path(data_path).generic_string();
    std::string out = "file:///";
    for (const char c : path) {
        const auto u = static_cast<unsigned char>(c);
        const bool plain = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                           (u >= '0' && u <= '9') || c == '-' || c == '_' || c == '.' ||
                           c == '~' || c == '/' || c == ':';
        if (plain) {
            out.push_back(c);
        } else {
            out += std::format("%{:02X}", static_cast<unsigned>(u));
        }
    }
    return out;
}

// One spell of one track, open while it stays Live.
struct Spell {
    dsp::SampleIndex start = 0;
    dsp::SampleIndex last = 0;
    dsp::Hertz lower = 0;
    dsp::Hertz upper = 0;
    double confidence = 0.0;
    double snr_db = std::numeric_limits<double>::lowest();
    double peak_db = std::numeric_limits<double>::lowest();
    std::string family;
};

class SpellBook {
public:
    SpellBook(double confidence_bar, dsp::SampleIndex origin)
        : bar_(confidence_bar), origin_(origin) {}

    void observe(const detect::Detector& detector)
    {
        const dsp::SampleIndex decided = detector.last_decision();
        const std::span<const detect::Candidate> candidates = detector.candidates();

        for (auto& [id, spell] : open_) {
            static_cast<void>(spell);
            seen_[id] = false;
        }

        for (const detect::Track& track : detector.tracks()) {
            const auto found = open_.find(track.id);
            if (track.state != detect::TrackState::Live) {
                if (found != open_.end()) {
                    close(found->second);
                    open_.erase(found);
                    seen_.erase(track.id);
                }
                continue;
            }

            Spell& spell = found != open_.end() ? found->second : open_[track.id];
            if (found == open_.end()) {
                spell.start = decided;
            }
            seen_[track.id] = true;
            spell.last = decided;
            spell.lower = track.center - track.bandwidth / 2;
            spell.upper = track.center + (track.bandwidth - track.bandwidth / 2);
            spell.confidence = std::max(spell.confidence, track.confidence);
            spell.snr_db = std::max(spell.snr_db, track.snr_2500_db);

            // The candidate this decision assigned, found by its centre
            // falling inside the track's band. The track carries no peak of
            // its own, and the candidate's is the measured one.
            for (const detect::Candidate& candidate : candidates) {
                if (candidate.center >= spell.lower && candidate.center <= spell.upper) {
                    spell.peak_db = std::max(spell.peak_db, candidate.peak_dbfs);
                }
            }

            const detect::TrackLabel label = detect::label_track(track);
            if (label.kind != detect::LabelKind::Unknown && !label.name.empty()) {
                spell.family = std::string(label.name);
            }
        }

        // A track that left tracks() altogether, dropped after its hold or
        // merged out of existence, ends its spell at its last Live decision.
        for (auto it = open_.begin(); it != open_.end();) {
            if (!seen_[it->first]) {
                close(it->second);
                seen_.erase(it->first);
                it = open_.erase(it);
            } else {
                ++it;
            }
        }
    }

    [[nodiscard]] std::vector<Annotation> finish()
    {
        for (auto& [id, spell] : open_) {
            static_cast<void>(id);
            close(spell);
        }
        open_.clear();
        std::ranges::sort(done_, [](const Annotation& x, const Annotation& y) {
            return std::tie(x.sample_start, x.freq_lower) < std::tie(y.sample_start, y.freq_lower);
        });
        return std::move(done_);
    }

private:
    void close(const Spell& spell)
    {
        if (spell.confidence < bar_) {
            return;
        }
        Annotation annotation;
        annotation.sample_start = spell.start >= origin_ ? spell.start - origin_ : 0;
        annotation.sample_count = spell.last - spell.start;
        annotation.freq_lower = spell.lower;
        annotation.freq_upper = spell.upper;
        annotation.confidence = spell.confidence;
        annotation.snr_db = spell.snr_db;
        // A spell with no candidate inside its band at any decision has no
        // measured peak; NaN is left out of the meta rather than written as a
        // level nobody measured.
        annotation.peak_db = spell.peak_db == std::numeric_limits<double>::lowest()
                                 ? std::numeric_limits<double>::quiet_NaN()
                                 : spell.peak_db;
        annotation.family = spell.family;
        done_.push_back(std::move(annotation));
    }

    double bar_;
    dsp::SampleIndex origin_;
    std::unordered_map<std::uint64_t, Spell> open_;
    std::unordered_map<std::uint64_t, bool> seen_;
    std::vector<Annotation> done_;
};

}  // namespace

Expected<IndexResult> index_segment(const std::string& data_path, const SegmentInfo& info,
                                    const IndexOptions& options)
{
    if (options.probes != 0 && options.pace <= 0.0) {
        return fail("tier two needs a pace: unthrottled, the file is gone before the first "
                    "probe lands");
    }

    const auto began = std::chrono::steady_clock::now();

    engine::EngineConfig config;
    config.gpu_index = options.gpu_index;
    config.channels = options.channels;
    config.channels_yield_to_source = options.channels_yield_to_source;
    config.spectrum_transform = options.spectrum_transform != 0 ? options.spectrum_transform
                                                                : dsp::kDefaultSpectrumTransform;
    config.pace = options.pace;
    config.probe_receivers = options.probes;

    auto made = engine::Engine::create(config);
    if (!made) {
        return std::unexpected(with_context(made.error(), "creating the index engine"));
    }
    engine::Engine& eng = **made;

    if (auto opened = eng.open_source(file_uri(data_path)); !opened) {
        return std::unexpected(with_context(opened.error(), std::format("opening '{}'", data_path)));
    }
    if (info.rate != 0 && eng.info().source_rate != info.rate) {
        return fail(std::format("'{}' opened at {} S/s and its SegmentInfo says {}", data_path,
                                eng.info().source_rate, info.rate));
    }
    if (!eng.info().spectrum.enabled()) {
        return fail("the index engine built no spectrum stage");
    }

    detect::DetectorConfig detect_config = options.detector;
    detect_config.source_rate = eng.info().source_rate;
    detect_config.source_center = eng.info().source_center;
    detect_config.grid_channels = eng.info().grid.channels;
    auto detector = detect::Detector::create(detect_config, eng.info().spectrum);
    if (!detector) {
        return std::unexpected(with_context(detector.error(), "creating the detector"));
    }

    std::optional<detect::TierTwo> tier_two;
    if (options.probes != 0) {
        auto made_tier =
            detect::TierTwo::create(detect::TierTwoConfig{.source_rate = eng.info().source_rate});
        if (!made_tier) {
            return std::unexpected(with_context(made_tier.error(), "tier two"));
        }
        tier_two.emplace(std::move(*made_tier));
    }

    // The file source counts from zero, which is the segment's first sample,
    // so the origin is zero. Named rather than assumed so a source that
    // started counting elsewhere is one line to change.
    constexpr dsp::SampleIndex kOrigin = 0;
    SpellBook book(detect_config.confidence_threshold, kOrigin);
    dsp::SampleIndex published = 0;

    const auto sink = [&](const engine::SpectrumFrame& frame) -> Status {
        if (auto fed = detector->consume(frame); !fed) {
            return fed;
        }
        const dsp::SampleIndex decided = detector->last_decision();
        if (decided == published) {
            return {};
        }
        published = decided;
        if (tier_two.has_value()) {
            // A refused step is a full request ring, which is not a reason to
            // stop indexing; it costs a probe and nothing else.
            static_cast<void>(tier_two->step(*detector, eng));
        }
        book.observe(*detector);
        return {};
    };
    if (auto wired = eng.set_spectrum_sink(sink); !wired) {
        return std::unexpected(with_context(wired.error(), "wiring the spectrum sink"));
    }

    if (auto ran = eng.run(); !ran) {
        return std::unexpected(with_context(ran.error(), std::format("indexing '{}'", data_path)));
    }

    IndexResult result;
    result.annotations = book.finish();
    result.stats = detector->stats();
    const source::SourceStats delivered = eng.source_stats();
    result.capture_seconds = eng.info().source_rate > 0
                                 ? static_cast<double>(delivered.samples_delivered) /
                                       static_cast<double>(eng.info().source_rate)
                                 : 0.0;
    result.wall_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    return result;
}

}  // namespace revenant::capture
