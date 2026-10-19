// The real-signal corpus harness: tests/corpus/README.md's design, as code.
//
// AN ENTRY IS A SIGMF SIDECAR. tests/corpus/<name>.sigmf-meta, the one
// revenant-capture extract wrote, with four keys added to its global object:
//
//   core:sha512              the dataset's checksum. A SigMF core key, so a
//                            sidecar that already carries one needs nothing.
//   corpus:url               where the dataset is retrieved from.
//   corpus:truth             the ground truth, an array of objects each with
//                            core:freq_lower_edge and core:freq_upper_edge in
//                            absolute Hz and an optional core:label.
//   corpus:min_recall        0..1, the share of truth rows that must be found.
//   corpus:max_false_alarms  optional, how many tracks matching no truth row
//                            are tolerated. Absent is not judged.
//
// The truth is its own key rather than the sidecar's annotations array,
// because extract already fills that array with what the live detector saw,
// and a harness that scored the detector against its own output would always
// pass.
//
// The dataset is <data dir>/<name>.sigmf-data, outside the repository for the
// reason the README gives, with the sidecar extract wrote beside it: the file
// source finds a SigMF dataset through the sidecar next to it, and extract
// writes the two as a pair anyway. An entry whose dataset is absent is skipped
// and listed, never failed: a developer without the corpus still gets a green
// run.
//
// SCORING. A truth row is found when some track that went Live had its centre
// inside the row's edges at a decision and, when the row names a label, last
// carried that label from detect::label_track. A false alarm is a track that went
// Live and was never inside any row. Scoring is a pure function of what was
// observed, so the unit tests reach it without an engine.

#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::bench {

struct CorpusTruth {
    dsp::Hertz lower_hz = 0;
    dsp::Hertz upper_hz = 0;

    // Empty when the row asks only for a detection.
    std::string label;
};

struct CorpusEntry {
    std::string name;
    std::string meta_path;
    std::string sha512;
    std::string url;
    std::vector<CorpusTruth> truth;
    double min_recall = 1.0;
    bool judge_false_alarms = false;
    std::uint32_t max_false_alarms = 0;
};

// Parses one sidecar's text. `name` is the entry's stem and `meta_path` where
// the text came from, both carried into the entry and into every message.
[[nodiscard]] Expected<CorpusEntry> parse_corpus_entry(std::string_view text,
                                                       std::string_view name,
                                                       std::string_view meta_path);

// Every *.sigmf-meta in `dir`, sorted by name. A malformed one fails the load
// rather than being skipped: a typo in a manifest is not a missing dataset.
[[nodiscard]] Expected<std::vector<CorpusEntry>> load_corpus(const std::string& dir);

// <data_dir>/<name>.sigmf-data.
[[nodiscard]] std::string corpus_data_path(const CorpusEntry& entry, const std::string& data_dir);

// One track as the harness saw it over the whole replay.
struct ObservedTrack {
    std::uint64_t id = 0;

    // Every centre the track had at a decision while Live or Held.
    std::vector<dsp::Hertz> centres;

    // detect::label_track's name at the last decision it was Live with a
    // label; empty when it never had one.
    std::string label;
};

struct EntryScore {
    std::uint32_t truth_rows = 0;
    std::uint32_t found = 0;

    // Rows that were detected but ended with another label than the row's.
    std::uint32_t mislabelled = 0;
    std::uint32_t false_alarms = 0;
    double recall = 0.0;
    bool passed = false;

    // One line per failed criterion, for the report.
    std::vector<std::string> reasons;
};

[[nodiscard]] EntryScore score_corpus_entry(const CorpusEntry& entry,
                                            const std::vector<ObservedTrack>& tracks);

struct CorpusRunOptions {
    // Multiple of realtime. Tier two places probes on the wall clock, so an
    // unthrottled file ends before the first label lands.
    double pace = 2.0;

    // -1 honours REVENANT_GPU_INDEX, as everywhere else in the bench.
    int gpu = -1;
};

// Replays the entry's dataset through the engine, the detector and tier two,
// the path tests/detect/test_labelled_scene.cpp takes.
[[nodiscard]] Expected<std::vector<ObservedTrack>> replay_corpus_entry(
    const CorpusEntry& entry, const std::string& data_path, const CorpusRunOptions& options);

// What one whole run came to.
struct CorpusReport {
    std::vector<std::string> passed;
    std::vector<std::string> failed;

    // Entries whose dataset was not on disk, each with the path looked at and
    // the URL to fetch it from.
    std::vector<std::string> skipped;

    [[nodiscard]] bool regressed() const { return !failed.empty(); }
};

using CorpusReplay = std::function<Expected<std::vector<ObservedTrack>>(
    const CorpusEntry& entry, const std::string& data_path)>;

// Runs every entry through `replay` and scores it, writing one line per entry
// and a total to `out`. A replay that errors fails its entry. The replay is a
// parameter so the skip rule is testable without an engine.
[[nodiscard]] CorpusReport run_corpus(const std::vector<CorpusEntry>& entries,
                                      const std::string& data_dir, const CorpusReplay& replay,
                                      std::FILE* out);

}  // namespace revenant::bench
