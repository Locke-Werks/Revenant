// What the span's labels say about voice, end to end.
//
// THE QUESTION. The owner, after the live playtest of 2026-09-23: "I also want
// to reasonably ident AM/FM phone." So: does the bracket on an AM voice signal
// read AM, on an FM voice signal NFM, and on a sideband voice signal USB or
// LSB rather than a digital mode, and how many brackets does one talker put up?
//
// THE PATH IS THE WIRE'S. tools/siggen/voice.h's scene, written to a cf32 file,
// read back through the engine's file source, the detector on the engine's own
// spectrum frames, tier two probing its tracks through the engine's probe pool,
// and detect::label_track on every track at every decision, which is what
// core/rpc/convert.cpp puts on the wire. So this needs the GPU, which is why it
// is hidden and why tests/detect/CMakeLists.txt's "no GPU" is about the cases
// ctest runs: nothing here is registered with it.
//
//     revenant_detect_tests.exe "[.voice-survey]"
//
// WHAT IS COUNTED, per emitter and per level. A track is an emitter's when its
// centre is inside the emitter's nominal band widened by a kilohertz either
// side, the same rule tests/engine/test_engine_probe.cpp's survey scores by.
//
//   ids         distinct track ids over the run
//   at once     the most Live or Held tracks at one decision
//   at end      the labels on its Live and Held tracks at the last decision
//   right       the first decision any of its tracks carried the right label,
//               in seconds from the start of the scene
//   wrong       the first decision any carried a label that is not the right
//               one, which is the number that has to stay empty
//
//   units       the most of tier two's emitters at one decision, a group
//               counting once: what the span would show with one detection
//               per emitter on the wire
//   hole        the widest gap between two of its lines at one decision,
//               which TierTwoConfig::emitter_gap_hz has to clear
//
// Two seeds at each level, and every line is printed rather than asserted:
// this is the record docs/detection.md quotes, not a gate. Three variables
// narrow or vary a run: REVENANT_VOICE_SURVEY_LEVEL runs one level, and
// REVENANT_VOICE_SURVEY_GAP and REVENANT_VOICE_SURVEY_FILL set
// TierTwoConfig::emitter_gap_hz and emitter_min_fill.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <print>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "core/detect/detector.h"
#include "core/detect/label.h"
#include "core/detect/tier_two.h"
#include "core/engine/engine.h"
#include "tests/support/temp_path.h"
#include "tools/siggen/voice.h"

using namespace revenant;

namespace {

constexpr std::uint32_t kChannels = 32;
constexpr dsp::Hertz kCentre = 146'000'000;

template <typename T>
[[nodiscard]] std::string message_of(const T& result) {
    return result.has_value() ? std::string() : result.error().message;
}

struct Capture {
    std::filesystem::path path;
    std::vector<siggen_voice::VoiceTruth> truth;

    Capture() = default;
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    ~Capture() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    [[nodiscard]] std::string uri() const {
        return "file:///" + path.generic_string() +
               "?rate=" + std::to_string(siggen_voice::kVoiceSceneRate) +
               "&format=cf32&center=" + std::to_string(kCentre);
    }
};

[[nodiscard]] std::unique_ptr<Capture> make_capture(const siggen_voice::VoiceSceneSpec& spec) {
    auto capture = std::make_unique<Capture>();
    auto scene = siggen_voice::build_voice_scene(spec);
    INFO(message_of(scene));
    REQUIRE(scene.has_value());
    capture->truth = scene->truth;
    capture->path = test::unique_temp_path("revenant_voice_survey", ".cf32");
    std::FILE* file = std::fopen(capture->path.string().c_str(), "wb");
    REQUIRE(file != nullptr);
    const auto written =
        siggen_voice::render_voice_scene(spec, [&](std::span<const std::complex<float>> block) -> Status {
            if (std::fwrite(block.data(), sizeof(std::complex<float>), block.size(), file) !=
                block.size()) {
                return fail("short write");
            }
            return {};
        });
    std::fclose(file);
    INFO(message_of(written));
    REQUIRE(written.has_value());
    return capture;
}

[[nodiscard]] engine::EngineConfig survey_config() {
    engine::EngineConfig config;
    config.channels = kChannels;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = 32'768;
    config.gpu_index = -1;
    config.probe_receivers = 4;
    config.spectrum_transform = 2048;

    // Paced for the reason tests/engine/test_engine_probe.cpp gives: the
    // pool places probes on the wall clock and an unthrottled file is gone
    // before the first one lands.
    config.pace = 4.0;
    return config;
}

struct EmitterRecord {
    std::set<std::uint64_t> ids;
    std::size_t most_at_once = 0;

    // The same count taken over tier two's emitters, a group counting once
    // and a line in no group counting as itself: what the span would show if
    // the wire published one detection per emitter.
    std::size_t most_units_at_once = 0;

    // The widest hole between two of its lines at one decision, from one
    // line's upper edge to the next one's lower edge in frequency order,
    // which is the number TierTwoConfig::emitter_gap_hz has to clear.
    dsp::Hertz widest_hole = 0;
    bool right_seen = false;
    double right_seconds = 0.0;
    bool wrong_seen = false;
    double wrong_seconds = 0.0;
    std::string wrong_name;
    std::vector<std::string> at_end;
    std::vector<std::string> probe_notes;
};

[[nodiscard]] std::size_t emitter_of(const std::vector<siggen_voice::VoiceTruth>& truth,
                                     const detect::Track& track) {
    const dsp::Hertz offset = track.center - kCentre;
    for (std::size_t i = 0; i < truth.size(); ++i) {
        if (offset >= truth[i].low_hz - 1'000 && offset <= truth[i].high_hz + 1'000) {
            return i;
        }
    }
    return truth.size();
}

}  // namespace

TEST_CASE("voice survey: labels on speech over every analogue modulation", "[.voice-survey]") {
    const double levels[] = {30.0, 20.0, 15.0, 10.0};
    const std::uint64_t seeds[] = {20260924, 20260925};
    const double seconds = 20.0;

    // REVENANT_VOICE_SURVEY_LEVEL runs one level, for looking at one case.
    const char* only = std::getenv("REVENANT_VOICE_SURVEY_LEVEL");
    for (const double level : levels) {
        if (only != nullptr && std::atof(only) != level) {
            continue;
        }
        for (const std::uint64_t seed : seeds) {
            siggen_voice::VoiceSceneSpec spec;
            spec.seconds = seconds;
            spec.seed = seed;
            spec.snr_2500_db = level;
            const auto capture = make_capture(spec);

            auto created = engine::Engine::create(survey_config());
            INFO(message_of(created));
            REQUIRE(created.has_value());
            auto& eng = **created;
            REQUIRE(eng.open_source(capture->uri()).has_value());
            const dsp::SampleRate rate = siggen_voice::kVoiceSceneRate;

            detect::DetectorConfig config;
            config.source_rate = rate;
            config.source_center = eng.info().source_center;
            config.grid_channels = kChannels;
            auto detector = detect::Detector::create(config, eng.info().spectrum);
            REQUIRE(detector.has_value());
            detect::TierTwoConfig tier_config{.source_rate = rate};
            if (const char* gap = std::getenv("REVENANT_VOICE_SURVEY_GAP"); gap != nullptr) {
                tier_config.emitter_gap_hz = std::atoll(gap);
            }
            if (const char* fill = std::getenv("REVENANT_VOICE_SURVEY_FILL"); fill != nullptr) {
                tier_config.emitter_min_fill = std::atof(fill);
            }
            auto tier_two = detect::TierTwo::create(tier_config);
            REQUIRE(tier_two.has_value());
            auto detector_ptr = std::make_unique<detect::Detector>(std::move(*detector));
            auto tier_two_ptr = std::make_unique<detect::TierTwo>(std::move(*tier_two));

            std::vector<EmitterRecord> records(capture->truth.size());
            dsp::SampleIndex last = 0;
            const auto observe = [&] {
                const double now =
                    static_cast<double>(detector_ptr->last_decision()) / static_cast<double>(rate);
                std::vector<std::size_t> at_once(records.size(), 0);
                for (const detect::Track& track : detector_ptr->tracks()) {
                    const std::size_t which = emitter_of(capture->truth, track);
                    if (which == records.size()) {
                        continue;
                    }
                    EmitterRecord& record = records[which];
                    record.ids.insert(track.id);
                    if (track.state == detect::TrackState::Live ||
                        track.state == detect::TrackState::Held) {
                        ++at_once[which];
                    }
                    const detect::TrackLabel label = detect::label_track(track);
                    if (label.kind == detect::LabelKind::Unknown) {
                        continue;
                    }
                    if (label.name == capture->truth[which].label) {
                        if (!record.right_seen) {
                            record.right_seen = true;
                            record.right_seconds = now;
                        }
                    } else if (!record.wrong_seen) {
                        record.wrong_seen = true;
                        record.wrong_seconds = now;
                        record.wrong_name = std::string(label.name);
                    }
                }
                for (std::size_t i = 0; i < records.size(); ++i) {
                    records[i].most_at_once = std::max(records[i].most_at_once, at_once[i]);
                }

                std::vector<std::size_t> units(records.size(), 0);
                std::set<std::uint64_t> grouped;
                for (const detect::TierTwoEmitter& emitter : tier_two_ptr->emitters()) {
                    std::set<std::size_t> hit;
                    for (const std::uint64_t id : emitter.tracks) {
                        grouped.insert(id);
                        for (const detect::Track& track : detector_ptr->tracks()) {
                            if (track.id == id) {
                                hit.insert(emitter_of(capture->truth, track));
                            }
                        }
                    }
                    for (const std::size_t which : hit) {
                        if (which < units.size()) {
                            ++units[which];
                        }
                    }
                }
                for (const detect::Track& track : detector_ptr->tracks()) {
                    const std::size_t which = emitter_of(capture->truth, track);
                    if (which < units.size() && !grouped.contains(track.id) &&
                        (track.state == detect::TrackState::Live ||
                         track.state == detect::TrackState::Held)) {
                        ++units[which];
                    }
                }
                for (std::size_t i = 0; i < records.size(); ++i) {
                    records[i].most_units_at_once =
                        std::max(records[i].most_units_at_once, units[i]);
                }

                // tracks() is ascending in frequency, so a hole is between one
                // of an emitter's lines and the next of the same emitter.
                std::vector<dsp::Hertz> reach(records.size(), 0);
                std::vector<bool> seen(records.size(), false);
                for (const detect::Track& track : detector_ptr->tracks()) {
                    const std::size_t which = emitter_of(capture->truth, track);
                    if (which == records.size() || (track.state != detect::TrackState::Live &&
                                                     track.state != detect::TrackState::Held)) {
                        continue;
                    }
                    const dsp::Hertz low = track.center - track.bandwidth / 2;
                    const dsp::Hertz high = track.center + (track.bandwidth - track.bandwidth / 2);
                    if (seen[which]) {
                        records[which].widest_hole =
                            std::max(records[which].widest_hole, low - reach[which]);
                        reach[which] = std::max(reach[which], high);
                    } else {
                        seen[which] = true;
                        reach[which] = high;
                    }
                }
            };
            const auto sink = [&](const engine::SpectrumFrame& frame) -> Status {
                if (auto fed = detector_ptr->consume(frame); !fed) {
                    return fed;
                }
                if (detector_ptr->last_decision() == last) {
                    return {};
                }
                last = detector_ptr->last_decision();
                auto stepped = tier_two_ptr->step(*detector_ptr, eng);
                observe();
                return stepped;
            };
            REQUIRE(eng.set_spectrum_sink(sink).has_value());
            const auto ran = eng.run();
            INFO(message_of(ran));
            REQUIRE(ran.has_value());

            for (const detect::Track& track : detector_ptr->tracks()) {
                const std::size_t which = emitter_of(capture->truth, track);
                if (which == records.size() || (track.state != detect::TrackState::Live &&
                                                 track.state != detect::TrackState::Held)) {
                    continue;
                }
                const detect::TrackLabel label = detect::label_track(track);
                records[which].at_end.push_back(
                    label.kind == detect::LabelKind::Unknown
                        ? std::format("-({})", track.probes)
                        : std::format("{} {:.2f}", label.name, label.confidence));
                records[which].probe_notes.push_back(std::format(
                    "#{} {:+.0f} Hz {} Hz: {} probes, last {} {:.2f} {:.0f} Bd drive {} dsb {}",
                    track.id, static_cast<double>(track.center - kCentre - capture->truth[which].offset_hz),
                    track.bandwidth, track.probes,
                    detect::classification_name(track.last_probe.family),
                    track.last_probe.confidence, track.last_probe.symbol_rate_hz,
                    track.last_probe.may_drive_detection, track.last_probe.double_sideband));
            }

            std::println("voice survey, {:.0f} dB in 2500 Hz, seed {}:", level, seed);
            std::println("  {:<7} {:>4} {:>7} {:>7} {:>6}  {:<34} {:>7} {:>13}", "emitter",
                         "ids", "at once", "units", "hole", "labels at end", "right", "wrong");
            for (std::size_t i = 0; i < records.size(); ++i) {
                const EmitterRecord& record = records[i];
                std::string ends;
                for (const std::string& one : record.at_end) {
                    ends += (ends.empty() ? "" : ", ") + one;
                }
                std::println("  {:<7} {:>4} {:>7} {:>7} {:>6}  {:<34} {:>7} {:>13}",
                             capture->truth[i].name, record.ids.size(), record.most_at_once,
                             record.most_units_at_once, record.widest_hole,
                             ends.empty() ? "none" : ends,
                             record.right_seen ? std::format("{:.1f} s", record.right_seconds)
                                               : std::string("never"),
                             record.wrong_seen ? std::format("{} {:.1f} s", record.wrong_name,
                                                             record.wrong_seconds)
                                               : std::string("never"));
                for (const std::string& note : record.probe_notes) {
                    std::println("      {}", note);
                }
            }
        }
    }
}
