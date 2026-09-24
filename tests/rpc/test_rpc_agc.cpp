// What a client hears: the receiver AGC's level on subscribeAudio, over a real
// socket, from signals 30 dB apart.
//
// tests/engine/test_engine_agc.cpp holds the stage itself: every mode, the
// timing, the decoders' input bit for bit, and the switch and a retune on a
// running receiver. This is the one thing that file cannot say, which is that
// the stream a client subscribes to is the levelled one. Before the engine
// had an AGC these five modes arrived here at 3e-7 to 2e-6, and a client that
// did not level them itself played nothing anyone could hear.
//
// THE CAPTURE is complex baseband rendered here and served through the file
// source, ten emitters at 1.152 MS/s: AM at 80% modulation, a 1 kHz tone on
// USB, LSB and DSB, and a bare carrier CW pitches to 700 Hz, each once at
// -20 dBFS and once at -50 dBFS. Paced at realtime, as every other streaming
// case in this directory is, so the subscriptions keep up without dropping.
//
// WHY -20 AND -50 AND NOT -30 AND -60, which is what the engine case uses.
// DSB's product detector takes the real part of a carrier nothing recovers,
// so its output is the tone times the cosine of wherever the receiver's
// oscillator happens to sit against the emitter's carrier. At the frequency
// the weak DSB emitter lands on here that cosine is small: rendered at
// -60 dBFS it came out below -82 dBFS, the level the AGC's 70 dB ceiling
// brings to the target, and arrived 0.5 dB under it. That is the ceiling
// doing what it says, and it is not what this case is about.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include "core/dsp/types.h"
#include "core/engine/listener_level.h"
#include "core/rpc/client.h"
#include "core/rpc/types.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"
#include "tests/support/temp_path.h"

using namespace revenant;
using test::Harness;
using test::HarnessOptions;

namespace {

constexpr dsp::SampleRate kRate = 1'152'000;
constexpr std::uint32_t kChannels = 16;
constexpr double kSeconds = 2.0;
constexpr double kToneHz = 1'000.0;
constexpr double kAudioRate = 48'000.0;

struct Emitter {
    rpc::Demod demod;
    dsp::Hertz center;
    double dbfs;
};

// Carrier at DC, level `a`, for one mode's test signal.
[[nodiscard]] std::complex<double> baseband(rpc::Demod demod, double t, double a) {
    const double phase = 2.0 * std::numbers::pi * kToneHz * t;
    switch (demod) {
        case rpc::Demod::Am: return {a * (1.0 + 0.8 * std::cos(phase)), 0.0};
        case rpc::Demod::Usb: return std::polar(a, phase);
        case rpc::Demod::Lsb: return std::polar(a, -phase);
        case rpc::Demod::Dsb: return {a * std::cos(phase), 0.0};
        default: return {a, 0.0};
    }
}

// Writes the capture and removes it again when the case is done.
class Capture {
public:
    explicit Capture(const std::vector<Emitter>& emitters)
        : path_(test::unique_temp_path("revenant_test_rpc_agc", ".cf32")) {
        const auto count = static_cast<std::size_t>(kSeconds * static_cast<double>(kRate));
        std::vector<dsp::Complex32> out(count);
        for (std::size_t n = 0; n < count; ++n) {
            const double t = static_cast<double>(n) / static_cast<double>(kRate);
            std::complex<double> sum{0.0, 0.0};
            for (const Emitter& emitter : emitters) {
                std::int64_t turns = (emitter.center * static_cast<std::int64_t>(n)) % kRate;
                if (turns < 0) {
                    turns += kRate;
                }
                const double angle = 2.0 * std::numbers::pi * static_cast<double>(turns) /
                                     static_cast<double>(kRate);
                sum += baseband(emitter.demod, t, std::pow(10.0, emitter.dbfs / 20.0)) *
                       std::polar(1.0, angle);
            }
            out[n] = dsp::Complex32{static_cast<float>(sum.real()),
                                    static_cast<float>(sum.imag())};
        }
        std::ofstream file(path_, std::ios::binary);
        file.write(reinterpret_cast<const char*>(out.data()),
                   static_cast<std::streamsize>(out.size() * sizeof(dsp::Complex32)));
        written_ = static_cast<bool>(file);
    }

    ~Capture() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    [[nodiscard]] bool written() const { return written_; }

    [[nodiscard]] std::string uri() const {
        return "file:///" + path_.generic_string() + "?rate=" + std::to_string(kRate) +
               "&format=cf32&center=7100000";
    }

private:
    std::filesystem::path path_;
    bool written_ = false;
};

// The peak each subscription saw, by the stream time its chunks started at.
// Shared with the client's loop thread through a shared_ptr, for the reason
// tests/rpc/test_rpc_audio.cpp's AudioLog gives.
class PeakLog {
public:
    void record(const rpc::AudioChunk& chunk) {
        const std::lock_guard<std::mutex> held(lock_);
        const double at = static_cast<double>(chunk.sample_index) / kAudioRate;
        if (chunk.sample_index != next_ && chunks_ > 0) {
            gap_ = true;
        }
        next_ = chunk.sample_index + chunk.frames();
        ++chunks_;
        if (at >= 1.0 && at < 1.5) {
            for (const float sample : chunk.samples) {
                settled_peak_ = std::max(settled_peak_, std::abs(static_cast<double>(sample)));
            }
        }
    }

    void end(const std::string&) {
        const std::lock_guard<std::mutex> held(lock_);
        ended_ = true;
    }

    [[nodiscard]] double settled_peak() const {
        const std::lock_guard<std::mutex> held(lock_);
        return settled_peak_;
    }

    [[nodiscard]] std::uint64_t reached() const {
        const std::lock_guard<std::mutex> held(lock_);
        return next_;
    }

    [[nodiscard]] bool gap() const {
        const std::lock_guard<std::mutex> held(lock_);
        return gap_;
    }

    [[nodiscard]] bool ended() const {
        const std::lock_guard<std::mutex> held(lock_);
        return ended_;
    }

private:
    mutable std::mutex lock_;
    double settled_peak_ = 0.0;
    std::uint64_t next_ = 0;
    std::uint64_t chunks_ = 0;
    bool gap_ = false;
    bool ended_ = false;
};

[[nodiscard]] double db(double ratio) { return 20.0 * std::log10(ratio); }

}  // namespace

TEST_CASE("am, usb, lsb, dsb and cw stream at the listening level from inputs 30 dB apart",
          "[gpu][rpc][audio][agc]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    const std::vector<rpc::Demod> modes = {rpc::Demod::Am, rpc::Demod::Usb, rpc::Demod::Lsb,
                                           rpc::Demod::Dsb, rpc::Demod::Cw};
    const std::vector<std::string> names = {"am", "usb", "lsb", "dsb", "cw"};
    std::vector<Emitter> emitters;
    dsp::Hertz center = -450'000;
    for (const rpc::Demod mode : modes) {
        for (const double dbfs : {-20.0, -50.0}) {
            emitters.push_back(Emitter{mode, center, dbfs});
            center += 90'000;
        }
    }
    const Capture capture(emitters);
    REQUIRE(capture.written());

    HarnessOptions options;
    options.source_uri = capture.uri();
    options.channels = kChannels;
    options.pace = 1.0;
    options.block_samples = 16'384;
    Harness harness;
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    std::vector<std::uint64_t> ids;
    std::vector<std::shared_ptr<PeakLog>> logs;
    for (const Emitter& emitter : emitters) {
        rpc::VrxParams params;
        params.center = emitter.center;
        params.demod = emitter.demod;
        params.bandwidth = 0;
        auto vrx = harness.client().add_vrx(params);
        INFO(test::message_of(vrx));
        REQUIRE(vrx.has_value());
        ids.push_back(*vrx);

        auto log = std::make_shared<PeakLog>();
        auto granted = harness.client().subscribe_audio(
            *vrx, 0, [log](const rpc::AudioChunk& chunk) { log->record(chunk); },
            [log](const std::string& reason) { log->end(reason); });
        INFO(test::message_of(granted));
        REQUIRE(granted.has_value());
        logs.push_back(std::move(log));
    }

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());

    // Half way through, the AGC on the last weak receiver is switched off
    // over the wire. Off holds the gain it had, so its level does not move,
    // and the subscription carries on through it: a push to the running
    // receiver, not a remove and an add.
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (logs.back()->reached() < static_cast<std::uint64_t>(0.7 * kAudioRate) &&
           std::chrono::steady_clock::now() < give_up) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(logs.back()->reached() >= static_cast<std::uint64_t>(0.7 * kAudioRate));
    rpc::VrxParams switched;
    switched.center = emitters.back().center;
    switched.demod = emitters.back().demod;
    switched.bandwidth = 0;
    switched.agc_enabled = false;
    const auto off = harness.client().set_vrx_params(ids.back(), switched);
    INFO(test::message_of(off));
    CHECK(off.has_value());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    const auto wanted = static_cast<std::uint64_t>(1.6 * kAudioRate);
    auto all_there = [&] {
        return std::ranges::all_of(logs, [&](const auto& log) { return log->reached() >= wanted; });
    };
    while (!all_there() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(all_there());

    const double target = db(engine::kHeardTarget);
    for (std::size_t m = 0; m < modes.size(); ++m) {
        const double strong = db(logs[2 * m]->settled_peak());
        const double weak = db(logs[2 * m + 1]->settled_peak());
        WARN(std::format("{:>3} on the wire: {:.2f} dBFS from a -20 dBFS signal and {:.2f} dBFS "
                         "from a -50 dBFS one",
                         names[m], strong, weak));
        INFO(names[m]);
        CHECK(std::abs(strong - target) < 1.0);
        CHECK(std::abs(weak - target) < 1.0);
        CHECK(std::abs(strong - weak) < 0.1);
    }
    for (const auto& log : logs) {
        CHECK_FALSE(log->gap());
        CHECK_FALSE(log->ended());
    }

    const auto status = harness.client().vrx_status(ids.back());
    INFO(test::message_of(status));
    REQUIRE(status.has_value());
    CHECK_FALSE(status->params.agc_enabled);

    for (const std::uint64_t vrx : ids) {
        harness.client().unsubscribe_audio(vrx);
    }
}
