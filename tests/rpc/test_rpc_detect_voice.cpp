// A talker over the wire: one detection per AM or NFM talker.
//
// tools/siggen/voice.h's scene, written to a cf32 file and opened by an engine
// with four probe receivers, the voice survey's engine, behind a server a
// client polls the way revenant-ui does. The detector reports a talker on AM
// or FM as several spectral lines; docs/detection.md's voice survey counted
// five on AM and up to eleven on NFM at 30 dB. Tier two probes them as one
// emitter, and the server now publishes one detection for it,
// detect::fold_emitters. This is that, counted where the client counts it.
//
// Its own file because it links tools/siggen's scene library, which the rest
// of the RPC suite does not need, and is built only where the tools are.
//
// WHAT IS ASSERTED AND WHAT IS PRINTED. A talker counts the detections whose
// centre is inside its nominal band widened by a kilohertz, Live or Held, at
// every poll. The case prints the most at once for each emitter and the count
// at the last decision. It asserts that at the last decision each AM and NFM
// talker is exactly one detection and carries its own label, AM or NFM; the
// most at once over the run is printed and not asserted, because tier two
// groups at a 400 Hz edge gap and docs/detection.md measured that splitting
// NFM into two or three emitters at some decisions at 30 dB.
//
// REVENANT_RPC_VOICE_PROBES=0 runs the same scene with no probe receivers,
// so no tier two and nothing folded: the line count the wire carried before,
// for comparison. That run skips the assertions.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <print>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/engine/engine.h"
#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/server.h"
#include "core/rpc/types.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/rpc/rpc_fixture.h"
#include "tests/support/temp_path.h"
#include "tools/siggen/voice.h"

using namespace revenant;

namespace {

constexpr dsp::Hertz kCentre = 146'000'000;

template <typename T>
[[nodiscard]] std::string message_of(const T& result) {
    return result.has_value() ? std::string() : result.error().message;
}

struct Talker {
    std::string name;
    std::string label;
    dsp::Hertz low = 0;
    dsp::Hertz high = 0;
    std::size_t most = 0;
    std::size_t at_end = 0;
    std::vector<std::string> end_labels;
};

}  // namespace

TEST_CASE("a talker on AM or NFM is one detection on the wire", "[gpu][rpc][detect][m2]") {
    REVENANT_NEEDS_GPU();

    std::uint32_t probes = 4;
    if (const char* asked = std::getenv("REVENANT_RPC_VOICE_PROBES"); asked != nullptr) {
        probes = static_cast<std::uint32_t>(std::atoi(asked));
    }

    siggen_voice::VoiceSceneSpec spec;
    spec.seconds = 16.0;
    spec.seed = 20260924;
    spec.snr_2500_db = 30.0;
    auto scene = siggen_voice::build_voice_scene(spec);
    INFO(message_of(scene));
    REQUIRE(scene.has_value());

    const std::filesystem::path path = test::unique_temp_path("revenant-rpc-voice", ".cf32");
    struct Remove {
        std::filesystem::path path;
        ~Remove() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } const remove{path};
    {
        std::FILE* file = std::fopen(path.string().c_str(), "wb");
        REQUIRE(file != nullptr);
        const auto written = siggen_voice::render_voice_scene(
            spec, [&](std::span<const std::complex<float>> block) -> Status {
                if (std::fwrite(block.data(), sizeof(std::complex<float>), block.size(), file) !=
                    block.size()) {
                    return fail("short write");
                }
                return {};
            });
        std::fclose(file);
        INFO(message_of(written));
        REQUIRE(written.has_value());
    }

    std::vector<Talker> talkers;
    for (const siggen_voice::VoiceTruth& truth : scene->truth) {
        if (truth.label.empty()) {
            continue;
        }
        talkers.push_back(Talker{.name = truth.name,
                                 .label = truth.label,
                                 .low = kCentre + truth.low_hz - 1'000,
                                 .high = kCentre + truth.high_hz + 1'000});
    }

    // The voice survey's engine: 32 channels at 1.2 MS/s and a 2048-point
    // second stage, the owner's 36.6 Hz bins, paced at four times realtime
    // because the pool places probes on the wall clock.
    engine::EngineConfig config;
    config.gpu_index = -1;
    config.channels = 32;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.block_samples = 32'768;
    config.probe_receivers = probes;
    config.spectrum_transform = 2048;
    config.pace = 4.0;
    auto created = engine::Engine::create(config);
    INFO(message_of(created));
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    const std::string uri = "file:///" + path.generic_string() +
                            "?rate=" + std::to_string(siggen_voice::kVoiceSceneRate) +
                            "&format=cf32&center=" + std::to_string(kCentre);
    const auto opened = eng.open_source(uri);
    INFO(message_of(opened));
    REQUIRE(opened.has_value());

    rpc::ServerOptions server_options;
    const rpc::Token token = test::test_token();
    server_options.token.assign(token.begin(), token.end());
    auto served = rpc::Server::create(eng, server_options);
    INFO(message_of(served));
    REQUIRE(served.has_value());
    auto connected = rpc::Client::connect("127.0.0.1", (*served)->port(), token);
    INFO(message_of(connected));
    REQUIRE(connected.has_value());
    rpc::Client& client = **connected;

    // The first poll builds the detector, before the engine runs.
    REQUIRE(client.detections(0.0, 0.0).has_value());

    std::atomic<bool> finished{false};
    Status ran;
    std::thread runner([&] {
        ran = eng.run();
        finished.store(true);
    });

    const auto count = [&](const rpc::DetectionList& list, bool at_end) {
        for (Talker& talker : talkers) {
            std::size_t here = 0;
            for (const rpc::Detection& detection : list.detections) {
                if (detection.center_hz < talker.low || detection.center_hz > talker.high ||
                    (detection.state != rpc::TrackState::Live &&
                     detection.state != rpc::TrackState::Held)) {
                    continue;
                }
                ++here;
                if (at_end) {
                    talker.end_labels.push_back(
                        detection.label.name.empty()
                            ? std::format("none {} Hz", detection.bandwidth_hz)
                            : std::format("{} {} Hz", detection.label.name,
                                          detection.bandwidth_hz));
                }
            }
            talker.most = std::max(talker.most, here);
            if (at_end) {
                talker.at_end = here;
            }
        }
    };

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!finished.load() && std::chrono::steady_clock::now() < deadline) {
        auto answered = client.detections(0.0, 0.0);
        REQUIRE(answered.has_value());
        count(*answered, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!finished.load()) {
        static_cast<void>(eng.stop());
    }
    runner.join();
    INFO(message_of(ran));
    REQUIRE(ran.has_value());

    // The engine has stopped, so the detector is frozen at its last decision.
    auto last = client.detections(0.0, 0.0);
    REQUIRE(last.has_value());
    count(*last, true);

    std::println("rpc voice, 30 dB, seed {}, {} probe receivers, decision {}:", spec.seed,
                 probes, last->last_decision);
    for (const Talker& talker : talkers) {
        std::string labels;
        for (const std::string& one : talker.end_labels) {
            labels += (labels.empty() ? "" : ", ") + one;
        }
        std::println("  {:<7} most at once {:>2}  at the end {:>2}  {}", talker.name, talker.most,
                     talker.at_end, labels);
    }

    if (probes == 0) {
        return;
    }
    for (const Talker& talker : talkers) {
        if (talker.label != "AM" && talker.label != "NFM") {
            continue;
        }
        INFO(std::format("{}: {} at the end, most {} at once", talker.name, talker.at_end,
                         talker.most));
        CHECK(talker.at_end == 1);
        for (const std::string& one : talker.end_labels) {
            CHECK(one.starts_with(talker.label + " "));
        }
    }
}
