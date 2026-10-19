#include "tools/bench/corpus.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <print>
#include <sstream>
#include <utility>

#include "core/detect/detector.h"
#include "core/detect/label.h"
#include "core/detect/tier_two.h"
#include "core/dsp/spectrum_reference.h"
#include "core/engine/engine.h"
#include "core/source/json.h"

namespace revenant::bench {

namespace {

using source::JsonValue;

[[nodiscard]] Expected<dsp::Hertz> edge_of(const JsonValue& row, std::string_view key,
                                           std::string_view where, std::size_t index) {
    const JsonValue* value = row.member(key);
    if (value == nullptr || value->kind != JsonValue::Kind::Number) {
        return fail(std::format("corpus entry '{}': corpus:truth[{}] has no {} number", where,
                                index, key));
    }
    return static_cast<dsp::Hertz>(std::llround(value->number));
}

}  // namespace

Expected<CorpusEntry> parse_corpus_entry(std::string_view text, std::string_view name,
                                         std::string_view meta_path) {
    auto parsed = source::parse_json(text);
    if (!parsed) {
        return fail(std::format("corpus entry '{}': {}", meta_path, parsed.error().message));
    }
    const JsonValue* global = parsed->member("global");
    if (global == nullptr || global->kind != JsonValue::Kind::Object) {
        return fail(std::format("corpus entry '{}' has no global object", meta_path));
    }

    CorpusEntry entry;
    entry.name = name;
    entry.meta_path = meta_path;

    const JsonValue* sha = global->member("core:sha512");
    if (sha == nullptr || sha->kind != JsonValue::Kind::String || sha->text.empty()) {
        return fail(std::format("corpus entry '{}' has no core:sha512 string", meta_path));
    }
    entry.sha512 = sha->text;

    const JsonValue* url = global->member("corpus:url");
    if (url == nullptr || url->kind != JsonValue::Kind::String || url->text.empty()) {
        return fail(std::format("corpus entry '{}' has no corpus:url string", meta_path));
    }
    entry.url = url->text;

    // An entry with no truth would pass every run, which is a test of nothing.
    const JsonValue* truth = global->member("corpus:truth");
    if (truth == nullptr || truth->kind != JsonValue::Kind::Array || truth->elements.empty()) {
        return fail(std::format("corpus entry '{}' has no corpus:truth array, or an empty one",
                                meta_path));
    }
    for (std::size_t i = 0; i < truth->elements.size(); ++i) {
        const JsonValue& row = truth->elements[i];
        if (row.kind != JsonValue::Kind::Object) {
            return fail(std::format("corpus entry '{}': corpus:truth[{}] is not an object",
                                    meta_path, i));
        }
        auto lower = edge_of(row, "core:freq_lower_edge", meta_path, i);
        if (!lower) {
            return std::unexpected(lower.error());
        }
        auto upper = edge_of(row, "core:freq_upper_edge", meta_path, i);
        if (!upper) {
            return std::unexpected(upper.error());
        }
        if (*upper <= *lower) {
            return fail(std::format(
                "corpus entry '{}': corpus:truth[{}] has an upper edge {} Hz not above its lower "
                "edge {} Hz",
                meta_path, i, *upper, *lower));
        }
        CorpusTruth out{.lower_hz = *lower, .upper_hz = *upper};
        if (const JsonValue* label = row.member("core:label"); label != nullptr) {
            if (label->kind != JsonValue::Kind::String) {
                return fail(std::format("corpus entry '{}': corpus:truth[{}] core:label is not a "
                                        "string",
                                        meta_path, i));
            }
            out.label = label->text;
        }
        entry.truth.push_back(std::move(out));
    }

    // Required rather than defaulted: the threshold is the regression line,
    // and one nobody chose is one nobody will defend.
    const JsonValue* recall = global->member("corpus:min_recall");
    if (recall == nullptr || recall->kind != JsonValue::Kind::Number || recall->number < 0.0 ||
        recall->number > 1.0) {
        return fail(std::format("corpus entry '{}' has no corpus:min_recall between 0 and 1",
                                meta_path));
    }
    entry.min_recall = recall->number;

    if (const JsonValue* alarms = global->member("corpus:max_false_alarms"); alarms != nullptr) {
        if (alarms->kind != JsonValue::Kind::Number || !alarms->unsigned_integral ||
            alarms->uinteger > 0xFFFF'FFFFULL) {
            return fail(std::format(
                "corpus entry '{}' has a corpus:max_false_alarms that is not a non-negative "
                "integer",
                meta_path));
        }
        entry.judge_false_alarms = true;
        entry.max_false_alarms = static_cast<std::uint32_t>(alarms->uinteger);
    }
    return entry;
}

Expected<std::vector<CorpusEntry>> load_corpus(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code error;
    if (!fs::is_directory(dir, error)) {
        return fail(std::format("corpus manifest directory '{}' does not exist", dir));
    }
    std::vector<fs::path> metas;
    for (const auto& item : fs::directory_iterator(dir, error)) {
        if (item.is_regular_file() && item.path().extension() == ".sigmf-meta") {
            metas.push_back(item.path());
        }
    }
    if (error) {
        return fail(std::format("corpus manifest directory '{}': {}", dir, error.message()));
    }
    std::sort(metas.begin(), metas.end());

    std::vector<CorpusEntry> entries;
    for (const fs::path& meta : metas) {
        std::ifstream in(meta, std::ios::binary);
        if (!in) {
            return fail(std::format("corpus entry '{}' cannot be read", meta.string()));
        }
        std::ostringstream text;
        text << in.rdbuf();
        auto entry = parse_corpus_entry(text.str(), meta.stem().string(), meta.string());
        if (!entry) {
            return std::unexpected(entry.error());
        }
        entries.push_back(std::move(*entry));
    }
    return entries;
}

std::string corpus_data_path(const CorpusEntry& entry, const std::string& data_dir) {
    return (std::filesystem::path(data_dir) / (entry.name + ".sigmf-data")).string();
}

EntryScore score_corpus_entry(const CorpusEntry& entry, const std::vector<ObservedTrack>& tracks) {
    EntryScore score;
    score.truth_rows = static_cast<std::uint32_t>(entry.truth.size());

    const auto inside = [](const CorpusTruth& row, dsp::Hertz centre) {
        return centre >= row.lower_hz && centre <= row.upper_hz;
    };

    for (const CorpusTruth& row : entry.truth) {
        bool detected = false;
        bool labelled = false;
        for (const ObservedTrack& track : tracks) {
            const bool hit = std::any_of(track.centres.begin(), track.centres.end(),
                                         [&](dsp::Hertz c) { return inside(row, c); });
            if (!hit) {
                continue;
            }
            detected = true;
            labelled = labelled || row.label.empty() || track.label == row.label;
        }
        if (labelled) {
            ++score.found;
        } else if (detected) {
            ++score.mislabelled;
        }
    }

    for (const ObservedTrack& track : tracks) {
        const bool matched = std::any_of(entry.truth.begin(), entry.truth.end(),
                                         [&](const CorpusTruth& row) {
                                             return std::any_of(
                                                 track.centres.begin(), track.centres.end(),
                                                 [&](dsp::Hertz c) { return inside(row, c); });
                                         });
        if (!matched && !track.centres.empty()) {
            ++score.false_alarms;
        }
    }

    score.recall = score.truth_rows == 0
                       ? 0.0
                       : static_cast<double>(score.found) / static_cast<double>(score.truth_rows);
    if (score.recall < entry.min_recall) {
        score.reasons.push_back(
            std::format("recall {:.3f} below corpus:min_recall {:.3f}", score.recall,
                        entry.min_recall));
    }
    if (entry.judge_false_alarms && score.false_alarms > entry.max_false_alarms) {
        score.reasons.push_back(std::format("{} false alarms over corpus:max_false_alarms {}",
                                            score.false_alarms, entry.max_false_alarms));
    }
    score.passed = score.reasons.empty();
    return score;
}

Expected<std::vector<ObservedTrack>> replay_corpus_entry(const CorpusEntry& entry,
                                                         const std::string& data_path,
                                                         const CorpusRunOptions& options) {
    engine::EngineConfig config;
    config.gpu_index = options.gpu;
    config.probe_receivers = 4;
    config.spectrum_transform = dsp::kDefaultSpectrumTransform;
    config.pace = options.pace;

    auto created = engine::Engine::create(config);
    if (!created) {
        return std::unexpected(created.error());
    }
    auto& eng = **created;

    // The file source takes the dataset's path from its sidecar's own
    // location, so naming the tree's copy with meta= would look for the data
    // in tests/corpus. The dataset is played beside the sidecar it was
    // extracted with instead; see corpus.h.
    const std::string uri =
        "file:///" + std::filesystem::path(data_path).generic_string() + "?container=sigmf";
    if (auto opened = eng.open_source(uri); !opened) {
        return fail(std::format("corpus entry '{}' did not open: {}", entry.name,
                                opened.error().message));
    }

    detect::DetectorConfig detect_config;
    detect_config.source_rate = eng.info().source_rate;
    detect_config.source_center = eng.info().source_center;
    detect_config.grid_channels = eng.info().grid.channels;
    auto detector = detect::Detector::create(detect_config, eng.info().spectrum);
    if (!detector) {
        return std::unexpected(detector.error());
    }
    auto tier_two =
        detect::TierTwo::create(detect::TierTwoConfig{.source_rate = eng.info().source_rate});
    if (!tier_two) {
        return std::unexpected(tier_two.error());
    }
    auto detector_ptr = std::make_unique<detect::Detector>(std::move(*detector));
    auto tier_two_ptr = std::make_unique<detect::TierTwo>(std::move(*tier_two));

    std::map<std::uint64_t, ObservedTrack> seen;
    dsp::SampleIndex last = 0;
    const auto sink = [&](const engine::SpectrumFrame& frame) -> Status {
        if (auto fed = detector_ptr->consume(frame); !fed) {
            return fed;
        }
        if (detector_ptr->last_decision() == last) {
            return {};
        }
        last = detector_ptr->last_decision();
        auto stepped = tier_two_ptr->step(*detector_ptr, eng);
        for (const detect::Track& track : detector_ptr->tracks()) {
            if (track.state != detect::TrackState::Live) {
                continue;
            }
            ObservedTrack& out = seen[track.id];
            out.id = track.id;
            out.centres.push_back(track.center);

            // The latest label it carried, kept here because a track that
            // ends before the stream does is gone from tracks() at the end.
            const detect::TrackLabel label = detect::label_track(track);
            if (label.kind != detect::LabelKind::Unknown) {
                out.label = std::string(label.name);
            }
        }
        return stepped;
    };
    if (auto set = eng.set_spectrum_sink(sink); !set) {
        return std::unexpected(set.error());
    }
    if (auto ran = eng.run(); !ran) {
        return std::unexpected(ran.error());
    }

    std::vector<ObservedTrack> out;
    out.reserve(seen.size());
    for (auto& [id, track] : seen) {
        out.push_back(std::move(track));
    }
    return out;
}

CorpusReport run_corpus(const std::vector<CorpusEntry>& entries, const std::string& data_dir,
                        const CorpusReplay& replay, std::FILE* out) {
    CorpusReport report;
    for (const CorpusEntry& entry : entries) {
        const std::string data_path = corpus_data_path(entry, data_dir);
        std::error_code error;
        if (data_dir.empty() || !std::filesystem::is_regular_file(data_path, error)) {
            report.skipped.push_back(std::format("{}: no {} (fetch from {})", entry.name,
                                                 data_dir.empty() ? std::string("data directory")
                                                                  : data_path,
                                                 entry.url));
            std::println(out, "SKIP  {}  dataset not present", entry.name);
            continue;
        }

        auto tracks = replay(entry, data_path);
        if (!tracks) {
            report.failed.push_back(entry.name);
            std::println(out, "FAIL  {}  replay failed: {}", entry.name, tracks.error().message);
            continue;
        }
        const EntryScore score = score_corpus_entry(entry, *tracks);
        std::println(out,
                     "{}  {}  found {}/{} (recall {:.3f}, needs {:.3f}), {} mislabelled, {} false "
                     "alarms",
                     score.passed ? "PASS" : "FAIL", entry.name, score.found, score.truth_rows,
                     score.recall, entry.min_recall, score.mislabelled, score.false_alarms);
        for (const std::string& reason : score.reasons) {
            std::println(out, "      {}", reason);
        }
        (score.passed ? report.passed : report.failed).push_back(entry.name);
    }

    std::println(out, "corpus: {} entries, {} passed, {} failed, {} skipped", entries.size(),
                 report.passed.size(), report.failed.size(), report.skipped.size());
    // Loud on purpose: a green run that tested nothing has to say so.
    if (!report.skipped.empty()) {
        std::println(out, "UNTESTED, dataset not present locally:");
        for (const std::string& line : report.skipped) {
            std::println(out, "  {}", line);
        }
    }
    return report;
}

}  // namespace revenant::bench
