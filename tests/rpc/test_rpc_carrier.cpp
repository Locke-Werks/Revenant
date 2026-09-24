// sam and dsb over a real socket at the listening level, from the layout
// tests/rpc/test_rpc_agc.cpp streams, rendered at -30 and -60 dBFS.
//
// That case was rendered at -20 and -50 because DSB could not reach the
// target from -60: its product detector read the real part of a carrier
// nothing recovered, the weak DSB emitter came out below -82 dBFS, and the
// AGC's 70 dB ceiling stopped short of it. The Costas loop in
// core/shaders/vrx_carrier.comp recovers that carrier now, so this renders
// the same ten emitters 10 dB lower and asks for every stream at the target,
// and adds two sam receivers on the two AM emitters.
//
// Two retunes ride along, because the loop restarts on the AGC's rule and
// only the engine applies that rule to a running receiver. A dsb receiver starts
// on the strong DSB emitter and is moved 90 kHz to the weak one, which is
// further than its passband and restarts the loop; a sam receiver starts
// 200 Hz below the weak AM emitter and is moved onto it, which is inside its
// passband and carries the loop. Both are measured where every other stream
// is, 1.0 to 1.5 s of audio, which is after both retunes.

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
#include <utility>
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

// Carrier at DC, level `a`, for one mode's test signal. The same signals as
// tests/rpc/test_rpc_agc.cpp.
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

class Capture {
public:
    explicit Capture(const std::vector<Emitter>& emitters)
        : path_(test::unique_temp_path("revenant_test_rpc_carrier", ".cf32")) {
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

// The peak each subscription saw over 1.0 to 1.5 s of audio, and whether its
// sample index ever skipped.
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

struct Receiver {
    std::string name;
    rpc::Demod demod;
    dsp::Hertz center;
    std::uint64_t id = 0;
    std::shared_ptr<PeakLog> log;
};

}  // namespace

TEST_CASE("sam and dsb stream at the listening level from -30 and -60 dBFS",
          "[gpu][rpc][audio][agc][carrier]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // test_rpc_agc.cpp's ten emitters, 90 kHz apart from -450 kHz, strong
    // then weak for each mode, at -30 and -60 dBFS.
    const std::vector<rpc::Demod> modes = {rpc::Demod::Am, rpc::Demod::Usb, rpc::Demod::Lsb,
                                           rpc::Demod::Dsb, rpc::Demod::Cw};
    const std::vector<std::string> names = {"am", "usb", "lsb", "dsb", "cw"};
    std::vector<Emitter> emitters;
    dsp::Hertz center = -450'000;
    for (const rpc::Demod mode : modes) {
        for (const double dbfs : {-30.0, -60.0}) {
            emitters.push_back(Emitter{mode, center, dbfs});
            center += 90'000;
        }
    }
    const Capture capture(emitters);
    REQUIRE(capture.written());

    std::vector<Receiver> receivers;
    for (std::size_t i = 0; i < emitters.size(); ++i) {
        receivers.push_back(Receiver{std::format("{} {}", names[i / 2], i % 2 == 0 ? "-30" : "-60"),
                                     emitters[i].demod, emitters[i].center});
    }
    // sam on both AM emitters.
    receivers.push_back(Receiver{"sam -30", rpc::Demod::Sam, emitters[0].center});
    receivers.push_back(Receiver{"sam -60", rpc::Demod::Sam, emitters[1].center});

    // The two retuned receivers, and where each is moved to.
    const dsp::Hertz dsb_strong = emitters[6].center;
    const dsp::Hertz dsb_weak = emitters[7].center;
    const dsp::Hertz am_weak = emitters[1].center;
    receivers.push_back(Receiver{"dsb moved -30 to -60", rpc::Demod::Dsb, dsb_strong});
    receivers.push_back(Receiver{"sam moved 200 Hz onto -60", rpc::Demod::Sam, am_weak - 200});
    const std::size_t moved_dsb = receivers.size() - 2;
    const std::size_t moved_sam = receivers.size() - 1;

    HarnessOptions options;
    options.source_uri = capture.uri();
    options.channels = kChannels;
    options.pace = 1.0;
    options.block_samples = 16'384;
    Harness harness;
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    for (Receiver& receiver : receivers) {
        rpc::VrxParams params;
        params.center = receiver.center;
        params.demod = receiver.demod;
        params.bandwidth = 0;
        auto vrx = harness.client().add_vrx(params);
        INFO(receiver.name << ": " << test::message_of(vrx));
        REQUIRE(vrx.has_value());
        receiver.id = *vrx;

        auto log = std::make_shared<PeakLog>();
        auto granted = harness.client().subscribe_audio(
            *vrx, 0, [log](const rpc::AudioChunk& chunk) { log->record(chunk); },
            [log](const std::string& reason) { log->end(reason); });
        INFO(receiver.name << ": " << test::message_of(granted));
        REQUIRE(granted.has_value());
        receiver.log = std::move(log);
    }

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());

    // The two retunes, a little over half a second in.
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (receivers[moved_sam].log->reached() < static_cast<std::uint64_t>(0.6 * kAudioRate) &&
           std::chrono::steady_clock::now() < give_up) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(receivers[moved_sam].log->reached() >= static_cast<std::uint64_t>(0.6 * kAudioRate));
    for (const auto& [index, to] : {std::pair{moved_dsb, dsb_weak}, std::pair{moved_sam, am_weak}}) {
        rpc::VrxParams params;
        params.center = to;
        params.demod = receivers[index].demod;
        params.bandwidth = 0;
        const auto moved = harness.client().set_vrx_params(receivers[index].id, params);
        INFO(receivers[index].name << ": " << test::message_of(moved));
        CHECK(moved.has_value());
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    const auto wanted = static_cast<std::uint64_t>(1.6 * kAudioRate);
    auto all_there = [&] {
        return std::ranges::all_of(receivers,
                                   [&](const Receiver& r) { return r.log->reached() >= wanted; });
    };
    while (!all_there() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(all_there());

    const double target = db(engine::kHeardTarget);
    std::string table = std::format("on the wire, peak over 1.0 to 1.5 s, target {:.2f} dBFS:\n",
                                    target);
    for (const Receiver& receiver : receivers) {
        const double heard = db(receiver.log->settled_peak());
        table += std::format("  {:<28} {:7.2f} dBFS\n", receiver.name, heard);
        INFO(receiver.name);
        // At or above the target, not merely near it. The AGC settles 0.43
        // dB over it on a tone (docs/rpc.md), and the defect this case is
        // about arrived 0.5 dB under it, with the 70 dB ceiling spent: a
        // window of a decibel either side would have passed that.
        CHECK(heard >= target);
        CHECK(heard - target < 1.0);
        CHECK_FALSE(receiver.log->gap());
        CHECK_FALSE(receiver.log->ended());
    }
    WARN(table);

    for (const Receiver& receiver : receivers) {
        harness.client().unsubscribe_audio(receiver.id);
    }
}
