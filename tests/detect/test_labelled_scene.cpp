// Every emitter of the labelled scene ends with its own label, over several
// seeds and on two grids.
//
// WHY THIS EXISTS. tools/siggen/labelled.h's scene is one emitter of each kind
// the span's labels name, and docs/detection.md's "The labelled scene, end to
// end" measured it once by hand. Then "Label a talker on AM or FM by its
// emitter, and stop calling SSB voice digital" changed the characteriser's
// branch order and nothing ran the scene again: on the grid revenant-cli
// opens it on, keyed CW at -450 kHz went from CW to no label, because a coarse
// grid measures a line 1311 Hz wide and the single sideband rule took it for a
// talker. And RTTY ended as CW or NFM: its two tones read as a carrier or as a
// talker on FM, and the carrier's family kept core/identify from trying its
// framing at all. This holds each emitter to its label so the next change to
// the classifier is measured against all twelve.
//
// THE PATH IS THE WIRE'S, as tests/detect/test_voice_survey.cpp's is: the
// scene written to a cf32 file, read back through the engine's file source,
// the detector on the engine's own frames, tier two through the engine's
// probe pool, and detect::label_track on every track, which is what
// core/rpc/convert.cpp publishes.
//
// TWO GRIDS. Sixteen channels, 131.8 Hz a bin, is the grid docs/detection.md
// measured the scene on. The other is the one revenant-cli picks for itself
// when --channels is not given, EngineConfig::channels_yield_to_source: eight
// channels and 263.7 Hz a bin on this source.
//
// WHAT IS ASSERTED, per emitter at the last decision: a Live or Held track of
// it, one of them carrying its own label, and none carrying another. A track
// with no label is not a wrong one, because Unknown is a real answer. What
// happened before the end is printed rather than asserted, as the first label
// other than the one held: RTTY carries its family, 2FSK, until the long
// identification dwell verifies its framing, and that is the probe's order of
// work rather than a wrong answer.
//
// RTTY ON THE DEFAULT GRID IS HELD TO ITS FAMILY, 2FSK. Its detection there is
// 1318 Hz wide, over the kilohertz core/identify/identify.cpp's RTTY row and
// the long identification dwell (engine::kProbeIdentifyNarrowHz) admit, so its
// framing is never tried. It read NFM or CW there before the change named
// above as well; docs/detection.md, "The labelled scene, held", says what
// reaching the protocol would take.
//
// THIRTY SECONDS A SCENE. With twenty, RTTY's first probe came back at 11 to
// 12 s of stream and its long dwell did not land before the end on two seeds
// of three on 16 channels; with thirty it verified on all three.

#include <catch2/catch_test_macros.hpp>

#include <complex>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "core/detect/detector.h"
#include "core/detect/label.h"
#include "core/detect/tier_two.h"
#include "core/dsp/spectrum_reference.h"
#include "core/engine/engine.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/support/temp_path.h"
#include "tools/siggen/labelled.h"

using namespace revenant;

namespace {

constexpr dsp::Hertz kCentre = 150'000'000;

// Every emitter's first probe, the long identification dwell a narrow track
// gets after it, and a re-probe of what came back refused. See the header.
constexpr double kSceneSeconds = 30.0;

template <typename T>
[[nodiscard]] std::string message_of(const T& result) {
    return result.has_value() ? std::string() : result.error().message;
}

// The scene's emitters, from tools/siggen/labelled.h, and the label each
// should end with.
struct SceneEmitter {
    std::string_view name;
    dsp::Hertz offset_hz;
    std::string_view label;

    // What it is held to on the default grid where that differs: RTTY's
    // family, because its protocol is out of reach there. See the header.
    std::string_view coarse_label = {};
};

constexpr SceneEmitter kEmitters[] = {
    {"AM", -800'000, "AM"},
    {"NFM", -600'000, "NFM"},
    {"CW", -450'000, "CW"},
    {"BPSK", -300'000, "BPSK"},
    {"P25", -150'000, "P25"},
    {"D-STAR", 100'000, "D-STAR"},
    {"TETRA", 250'000, "TETRA"},
    {"M17", 400'000, "M17"},
    {"AX.25", 550'000, "AX.25"},
    {"RTTY", 700'000, "RTTY", "2FSK"},
    {"DMR", -950'000, "DMR"},
    {"USB", 850'000, "USB"},
};

[[nodiscard]] std::string_view wanted(const SceneEmitter& emitter, bool default_grid) {
    return default_grid && !emitter.coarse_label.empty() ? emitter.coarse_label : emitter.label;
}

// A track is an emitter's when its centre is within this of the emitter's
// offset. The nearest two emitters are 150 kHz apart and the widest, TETRA,
// measures about 21 kHz.
constexpr dsp::Hertz kAttributionHz = 20'000;

[[nodiscard]] std::size_t emitter_of(const detect::Track& track) {
    const dsp::Hertz offset = track.center - kCentre;
    for (std::size_t i = 0; i < std::size(kEmitters); ++i) {
        const dsp::Hertz distance = offset - kEmitters[i].offset_hz;
        if (distance >= -kAttributionHz && distance <= kAttributionHz) {
            return i;
        }
    }
    return std::size(kEmitters);
}

struct Capture {
    std::filesystem::path path;

    Capture() = default;
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    ~Capture() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    [[nodiscard]] std::string uri() const {
        return "file:///" + path.generic_string() +
               "?rate=" + std::to_string(siggen_labelled::kLabelledRate) +
               "&format=cf32&center=" + std::to_string(kCentre);
    }
};

struct Grid {
    const char* name;
    std::uint32_t channels;
    bool yield_to_source;
};

struct Record {
    std::vector<std::string> at_end;
    std::string first_other;
};

// One run of the scene through the engine on one grid: each emitter's labels
// at the last decision, and the first label any of its tracks carried other
// than the one it is held to.
[[nodiscard]] std::vector<Record> run_scene(const Capture& capture, const Grid& grid,
                                            double* bin_width_hz) {
    engine::EngineConfig config;
    config.gpu_index = -1;
    config.channels = grid.channels;
    config.channels_yield_to_source = grid.yield_to_source;
    config.probe_receivers = 4;
    config.spectrum_transform = dsp::kDefaultSpectrumTransform;

    // Paced for the reason tests/engine/test_engine_probe.cpp gives: the pool
    // places probes on the wall clock and an unthrottled file is gone before
    // the first one lands. Twice realtime and not the voice survey's four:
    // the pool runs below normal priority under a CPU budget, and in a full
    // ctest run on a machine other builds were loading, four times realtime
    // left the NFM, AX.25 and RTTY emitters of one seed unprobed at the end.
    config.pace = 2.0;

    auto created = engine::Engine::create(config);
    INFO(message_of(created));
    REQUIRE(created.has_value());
    auto& eng = **created;
    const auto opened = eng.open_source(capture.uri());
    INFO(message_of(opened));
    REQUIRE(opened.has_value());
    *bin_width_hz = eng.info().spectrum.bin_width_hz();

    detect::DetectorConfig detect_config;
    detect_config.source_rate = eng.info().source_rate;
    detect_config.source_center = eng.info().source_center;
    detect_config.grid_channels = eng.info().grid.channels;
    auto detector = detect::Detector::create(detect_config, eng.info().spectrum);
    INFO(message_of(detector));
    REQUIRE(detector.has_value());
    auto tier_two = detect::TierTwo::create(
        detect::TierTwoConfig{.source_rate = eng.info().source_rate});
    INFO(message_of(tier_two));
    REQUIRE(tier_two.has_value());
    auto detector_ptr = std::make_unique<detect::Detector>(std::move(*detector));
    auto tier_two_ptr = std::make_unique<detect::TierTwo>(std::move(*tier_two));

    std::vector<Record> records(std::size(kEmitters));
    dsp::SampleIndex last = 0;
    const double rate = static_cast<double>(eng.info().source_rate);
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
            const std::size_t which = emitter_of(track);
            if (which == records.size() || !records[which].first_other.empty()) {
                continue;
            }
            const detect::TrackLabel label = detect::label_track(track);
            if (label.kind != detect::LabelKind::Unknown &&
                label.name != wanted(kEmitters[which], grid.yield_to_source)) {
                records[which].first_other =
                    std::format("{} at {:.1f} s", label.name, static_cast<double>(last) / rate);
            }
        }
        return stepped;
    };
    REQUIRE(eng.set_spectrum_sink(sink).has_value());
    const auto ran = eng.run();
    INFO(message_of(ran));
    REQUIRE(ran.has_value());

    for (const detect::Track& track : detector_ptr->tracks()) {
        const std::size_t which = emitter_of(track);
        if (which == records.size() || (track.state != detect::TrackState::Live &&
                                         track.state != detect::TrackState::Held)) {
            continue;
        }
        const detect::TrackLabel label = detect::label_track(track);
        records[which].at_end.push_back(label.kind == detect::LabelKind::Unknown
                                            ? std::string()
                                            : std::string(label.name));
    }
    return records;
}

}  // namespace

// REJECTS: a change to the characteriser, the identifier or tier two that
// moves any emitter of the labelled scene off its label, which is what the
// keyed carrier's did unnoticed.
TEST_CASE("each emitter of the labelled scene ends with its own label",
          "[gpu][detect][labelled]") {
    REVENANT_NEEDS_GPU();

    const std::uint64_t seeds[] = {1, 2, 3};
    const Grid grids[] = {
        {"16 channels", 16, false},
        {"the CLI's own grid", 64, true},
    };
    for (const std::uint64_t seed : seeds) {
        Capture capture;
        capture.path = test::unique_temp_path("revenant_labelled_scene", ".cf32");
        siggen_labelled::LabelledSceneSpec spec;
        spec.out_path = capture.path.string();
        spec.seconds = kSceneSeconds;
        spec.seed = seed;
        const auto rendered = siggen_labelled::render_labelled_scene(spec);
        INFO(message_of(rendered));
        REQUIRE(rendered.has_value());

        for (const Grid& grid : grids) {
            double bin_width_hz = 0.0;
            const std::vector<Record> records = run_scene(capture, grid, &bin_width_hz);
            const bool default_grid = grid.yield_to_source;

            std::println("labelled scene, seed {}, {} at {:.1f} Hz a bin:", seed, grid.name,
                         bin_width_hz);
            for (std::size_t i = 0; i < records.size(); ++i) {
                const SceneEmitter& emitter = kEmitters[i];
                const Record& record = records[i];
                std::string ends;
                for (const std::string& one : record.at_end) {
                    ends += (ends.empty() ? "" : ", ") + (one.empty() ? std::string("none") : one);
                }
                std::println("  {:<7} {:<30} first other label {}", emitter.name,
                             ends.empty() ? std::string("no track") : ends,
                             record.first_other.empty() ? std::string("never")
                                                        : record.first_other);

                const std::string_view label = wanted(emitter, default_grid);
                INFO(std::format("{} on seed {}, {}: {}", emitter.name, seed, grid.name,
                                 ends.empty() ? std::string("no track") : ends));
                CHECK_FALSE(record.at_end.empty());
                bool right = false;
                for (const std::string& one : record.at_end) {
                    right = right || one == label;
                    CHECK((one.empty() || one == label));
                }
                if (!label.empty()) {
                    CHECK(right);
                }
            }
        }
    }
}
