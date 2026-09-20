#include "tests/rpc/rpc_fixture.h"

#include <format>
#include <utility>

#include "core/engine/vrx.h"

namespace revenant::test {
namespace {

// Six emitters well above a quiet floor, so the frame has structure in it.
// A frame of noise alone would let a truncated or reordered bin array pass
// the integrity case, because noise looks like noise however it is shuffled.
constexpr const char* kSceneShape =
    "&emitters=6&seed=606060&noise_dbfs=-100&snr_min=30&snr_max=40";

}  // namespace

std::string scene_uri(dsp::SampleIndex samples, dsp::Hertz center_hz) {
    return std::format("synthetic:wideband?rate={}{}&samples={}&center={}", kSourceRate,
                       kSceneShape, samples, center_hz);
}

Harness::~Harness() { shutdown(); }

Status Harness::open(const HarnessOptions& options) {
    engine::EngineConfig config;
    config.gpu_index = -1;  // honours REVENANT_GPU_INDEX, like every other binary here
    config.channels = kChannels;
    config.taps_per_branch = 17;
    config.ring_seconds = options.ring_seconds;
    config.block_samples = options.block_samples;
    config.pace = options.pace;
    config.spectrum_transform = options.spectrum_transform;

    auto created = engine::Engine::create(config);
    if (!created) {
        return std::unexpected(with_context(created.error(), "building the engine"));
    }
    engine_ = std::move(*created);

    if (auto opened = engine_->open_source(scene_uri(options.samples, options.center_hz));
        !opened) {
        return std::unexpected(with_context(opened.error(), "opening the scene"));
    }

    // Ephemeral, so two of these running at once on one machine do not
    // collide. ServerOptions::port defaults to zero for that reason and
    // Server::port() reports what was bound.
    rpc::ServerOptions server_options;
    auto served = rpc::Server::create(*engine_, server_options);
    if (!served) {
        return std::unexpected(with_context(served.error(), "starting the server"));
    }
    server_ = std::move(*served);
    port_ = server_->port();

    if (!options.connect_client) {
        return {};
    }

    auto connected = rpc::Client::connect("127.0.0.1", port_);
    if (!connected) {
        return std::unexpected(with_context(connected.error(), "connecting the client"));
    }
    client_ = std::move(*connected);
    return {};
}

Expected<std::unique_ptr<rpc::Client>> Harness::connect_another() {
    return rpc::Client::connect("127.0.0.1", port_);
}

Status Harness::start_engine() {
    if (running_) {
        return fail("the harness engine is already running");
    }
    running_ = true;
    stopped_ = false;
    runner_ = std::thread([this] { run_outcome_ = engine_->run(); });
    return {};
}

Status Harness::stop_engine() {
    if (!running_) {
        return run_outcome_;
    }
    if (!stopped_) {
        stopped_ = true;
        static_cast<void>(engine_->stop());
    }
    if (runner_.joinable()) {
        runner_.join();
    }
    running_ = false;

    // A stop this asked for and got is a clean finish, which is the same
    // distinction tools/cli/main.cpp draws and for the same reason: run()
    // still stops the source, flushes the graph and checks the scheduler
    // after cancellation, and any of those can fail for a real reason.
    // Swallowing everything behind the flag would hide a device lost mid-run
    // behind "we asked it to stop".
    if (!run_outcome_ &&
        run_outcome_.error().message.find("the engine was stopped") != std::string::npos) {
        run_outcome_ = Status{};
    }
    return run_outcome_;
}

std::uint64_t Harness::wait_for_blocks(std::uint64_t blocks, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::uint64_t seen = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        seen = engine_->source_stats().blocks_delivered;
        if (seen >= blocks) {
            return seen;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return engine_->source_stats().blocks_delivered;
}

void Harness::shutdown() {
    // The engine first: run() has to return before anything it feeds is
    // destroyed, and the runner thread has to be joined before this frame
    // unwinds whatever it captured.
    static_cast<void>(stop_engine());

    // Then in the order the headers require. The client holds a connection to
    // the server; the server holds the engine's spectrum sink and a thread
    // that fans frames out to subscribers.
    client_.reset();
    server_.reset();
    engine_.reset();
}

// ---------------------------------------------------------------------------
// FrameLog
// ---------------------------------------------------------------------------

void FrameLog::record(const rpc::SpectrumFrame& frame) {
    std::chrono::milliseconds delay{0};
    {
        const std::lock_guard<std::mutex> held(lock_);
        if (inside_) {
            reentered_ = true;
            return;
        }
        inside_ = true;
        delay = delay_;

        FrameRecord record;
        record.sequence = frame.sequence;
        record.start = frame.start;
        record.count = frame.count;
        record.bins = frame.power_db.size();
        record.geometry = frame.geometry;
        record.floor_db = frame.floor_db;
        record.ceiling_db = frame.ceiling_db;
        record.percentile_low_db = frame.percentile_low_db;
        record.percentile_high_db = frame.percentile_high_db;

        if (frames_.size() < kKeptFrames) {
            record.power_db = frame.power_db;
        }
        frames_.push_back(std::move(record));
    }

    // Outside the lock, so the test thread reading frames() is not blocked
    // for the length of a deliberately slow callback. The sleep is what makes
    // this subscriber one the engine cannot keep up with; the backpressure
    // case is the only one that sets it.
    if (delay.count() > 0) {
        std::this_thread::sleep_for(delay);
    }

    const std::lock_guard<std::mutex> held(lock_);
    inside_ = false;
}

std::vector<FrameRecord> FrameLog::frames() const {
    const std::lock_guard<std::mutex> held(lock_);
    return frames_;
}

std::size_t FrameLog::size() const {
    const std::lock_guard<std::mutex> held(lock_);
    return frames_.size();
}

bool FrameLog::reentered() const {
    const std::lock_guard<std::mutex> held(lock_);
    return reentered_;
}

void FrameLog::set_callback_delay(std::chrono::milliseconds delay) {
    const std::lock_guard<std::mutex> held(lock_);
    delay_ = delay;
}

}  // namespace revenant::test
