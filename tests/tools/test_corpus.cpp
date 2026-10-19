// tools/bench/corpus.h: the manifest a corpus entry is, the scoring, and the
// rule that an entry whose dataset is absent is skipped and listed rather
// than failed. No engine: the replay is a parameter of run_corpus, and these
// cases hand it one that records whether it was called.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "tests/support/temp_path.h"
#include "tools/bench/corpus.h"

using namespace revenant;
using bench::CorpusEntry;
using bench::ObservedTrack;

namespace {

constexpr const char* kEntry = R"({
  "global": {
    "core:datatype": "cf32_le",
    "core:sample_rate": 2400000,
    "core:version": "1.0.0",
    "core:sha512": "abc123",
    "corpus:url": "https://example.invalid/pager.sigmf-data",
    "corpus:min_recall": 0.5,
    "corpus:max_false_alarms": 2,
    "corpus:truth": [
      {"core:freq_lower_edge": 152000000, "core:freq_upper_edge": 152020000, "core:label": "POCSAG"},
      {"core:freq_lower_edge": 152500000, "core:freq_upper_edge": 152510000}
    ]
  },
  "captures": [{"core:sample_start": 0, "core:frequency": 152400000}],
  "annotations": []
})";

[[nodiscard]] std::string without(std::string text, const std::string& line) {
    const std::size_t at = text.find(line);
    REQUIRE(at != std::string::npos);
    text.erase(at, line.size());
    return text;
}

[[nodiscard]] CorpusEntry parsed_entry() {
    auto entry = bench::parse_corpus_entry(kEntry, "pager", "pager.sigmf-meta");
    INFO((entry ? std::string() : entry.error().message));
    REQUIRE(entry.has_value());
    return *entry;
}

// Discards the report's text: the cases assert on the returned report.
struct NullFile {
    std::FILE* file = nullptr;
    NullFile() {
#ifdef _WIN32
        file = std::fopen("NUL", "w");
#else
        file = std::fopen("/dev/null", "w");
#endif
        REQUIRE(file != nullptr);
    }
    NullFile(const NullFile&) = delete;
    NullFile& operator=(const NullFile&) = delete;
    ~NullFile() { std::fclose(file); }
};

}  // namespace

TEST_CASE("a corpus entry reads its checksum, URL, truth and thresholds", "[corpus]") {
    const CorpusEntry entry = parsed_entry();
    CHECK(entry.name == "pager");
    CHECK(entry.sha512 == "abc123");
    CHECK(entry.url == "https://example.invalid/pager.sigmf-data");
    CHECK(entry.min_recall == 0.5);
    CHECK(entry.judge_false_alarms);
    CHECK(entry.max_false_alarms == 2);
    REQUIRE(entry.truth.size() == 2);
    CHECK(entry.truth[0].lower_hz == 152'000'000);
    CHECK(entry.truth[0].upper_hz == 152'020'000);
    CHECK(entry.truth[0].label == "POCSAG");
    CHECK(entry.truth[1].label.empty());
}

TEST_CASE("a corpus entry without max_false_alarms does not judge them", "[corpus]") {
    const std::string text = without(kEntry, R"("corpus:max_false_alarms": 2,)");
    auto entry = bench::parse_corpus_entry(text, "pager", "pager.sigmf-meta");
    REQUIRE(entry.has_value());
    CHECK_FALSE(entry->judge_false_alarms);
}

TEST_CASE("a corpus entry missing a required key is refused by name", "[corpus]") {
    const struct {
        const char* line;
        const char* key;
    } cases[] = {
        {R"("core:sha512": "abc123",)", "core:sha512"},
        {R"("corpus:url": "https://example.invalid/pager.sigmf-data",)", "corpus:url"},
        {R"("corpus:min_recall": 0.5,)", "corpus:min_recall"},
    };
    for (const auto& one : cases) {
        INFO(one.key);
        auto entry = bench::parse_corpus_entry(without(kEntry, one.line), "pager", "p");
        REQUIRE_FALSE(entry.has_value());
        CHECK(entry.error().message.find(one.key) != std::string::npos);
    }
}

TEST_CASE("a corpus entry with no truth or a bad truth row is refused", "[corpus]") {
    const std::string empty =
        R"({"global": {"core:sha512": "a", "corpus:url": "u", "corpus:min_recall": 1,
            "corpus:truth": []}})";
    CHECK_FALSE(bench::parse_corpus_entry(empty, "e", "e").has_value());

    const std::string inverted =
        R"({"global": {"core:sha512": "a", "corpus:url": "u", "corpus:min_recall": 1,
            "corpus:truth": [{"core:freq_lower_edge": 10, "core:freq_upper_edge": 5}]}})";
    CHECK_FALSE(bench::parse_corpus_entry(inverted, "e", "e").has_value());

    const std::string recall =
        R"({"global": {"core:sha512": "a", "corpus:url": "u", "corpus:min_recall": 1.5,
            "corpus:truth": [{"core:freq_lower_edge": 1, "core:freq_upper_edge": 5}]}})";
    CHECK_FALSE(bench::parse_corpus_entry(recall, "e", "e").has_value());

    CHECK_FALSE(bench::parse_corpus_entry("not json", "e", "e").has_value());
}

TEST_CASE("corpus scoring counts found, mislabelled and false alarms", "[corpus]") {
    const CorpusEntry entry = parsed_entry();

    // Row one found with its label, row two missed, one stray track.
    std::vector<ObservedTrack> tracks = {
        {.id = 1, .centres = {152'010'000}, .label = "POCSAG"},
        {.id = 2, .centres = {153'000'000}, .label = ""},
    };
    bench::EntryScore score = bench::score_corpus_entry(entry, tracks);
    CHECK(score.found == 1);
    CHECK(score.mislabelled == 0);
    CHECK(score.false_alarms == 1);
    CHECK(score.recall == 0.5);
    CHECK(score.passed);

    // Row one detected under another label is not found.
    tracks[0].label = "NFM";
    score = bench::score_corpus_entry(entry, tracks);
    CHECK(score.found == 0);
    CHECK(score.mislabelled == 1);
    CHECK_FALSE(score.passed);

    // Too many strays fail on their own threshold.
    tracks[0].label = "POCSAG";
    tracks.push_back({.id = 3, .centres = {154'000'000}});
    tracks.push_back({.id = 4, .centres = {155'000'000}});
    score = bench::score_corpus_entry(entry, tracks);
    CHECK(score.false_alarms == 3);
    CHECK_FALSE(score.passed);
    CHECK(score.reasons.size() == 1);
}

TEST_CASE("corpus entries whose dataset is absent are skipped, not replayed", "[corpus]") {
    const std::filesystem::path dir = test::unique_temp_path("revenant_corpus", "");
    std::filesystem::create_directories(dir);
    {
        std::ofstream meta(dir / "pager.sigmf-meta", std::ios::binary);
        meta << kEntry;
        std::ofstream other(dir / "absent.sigmf-meta", std::ios::binary);
        other << kEntry;
        std::ofstream data(dir / "pager.sigmf-data", std::ios::binary);
        data << "x";
    }

    auto entries = bench::load_corpus(dir.string());
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 2);
    CHECK((*entries)[0].name == "absent");
    CHECK((*entries)[1].name == "pager");

    std::vector<std::string> replayed;
    const bench::CorpusReplay replay = [&](const CorpusEntry& entry, const std::string&)
        -> Expected<std::vector<ObservedTrack>> {
        replayed.push_back(entry.name);
        return std::vector<ObservedTrack>{{.id = 1, .centres = {152'010'000}, .label = "POCSAG"},
                                          {.id = 2, .centres = {152'505'000}}};
    };

    NullFile sink;
    bench::CorpusReport report = bench::run_corpus(*entries, dir.string(), replay, sink.file);
    CHECK(replayed == std::vector<std::string>{"pager"});
    CHECK(report.passed == std::vector<std::string>{"pager"});
    REQUIRE(report.skipped.size() == 1);
    CHECK(report.skipped[0].find("absent") != std::string::npos);
    CHECK(report.skipped[0].find("https://example.invalid") != std::string::npos);
    CHECK_FALSE(report.regressed());

    // No data directory at all skips everything and is still not a regression.
    replayed.clear();
    report = bench::run_corpus(*entries, "", replay, sink.file);
    CHECK(replayed.empty());
    CHECK(report.skipped.size() == 2);
    CHECK_FALSE(report.regressed());

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

TEST_CASE("a corpus replay that errors fails its entry", "[corpus]") {
    const CorpusEntry entry = parsed_entry();
    const std::filesystem::path dir = test::unique_temp_path("revenant_corpus_err", "");
    std::filesystem::create_directories(dir);
    {
        std::ofstream data(dir / "pager.sigmf-data", std::ios::binary);
        data << "x";
    }
    const bench::CorpusReplay replay = [](const CorpusEntry&, const std::string&)
        -> Expected<std::vector<ObservedTrack>> { return fail("no device"); };
    NullFile sink;
    const bench::CorpusReport report = bench::run_corpus({entry}, dir.string(), replay, sink.file);
    CHECK(report.failed == std::vector<std::string>{"pager"});
    CHECK(report.regressed());
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

TEST_CASE("the repository's corpus manifests all parse", "[corpus]") {
    const std::filesystem::path dir =
        std::filesystem::path(REVENANT_SOURCE_DIR) / "tests" / "corpus";
    auto entries = bench::load_corpus(dir.string());
    INFO((entries ? std::string() : entries.error().message));
    CHECK(entries.has_value());
}
