// An engine, a server and a client, over a real socket.
//
// Nothing here is a stub. The cases in this directory bind an ephemeral port
// on loopback, connect a real Cap'n Proto client to it and drive the engine
// through the wire, because the failures the RPC layer can have are all in
// the seams: a field dropped in conversion, a rational rounded, a frame
// queued instead of dropped, an event loop touched from the wrong thread.
// None of them is visible to a test that calls the conversion functions
// directly.
//
// WHY THE ORDER THESE THREE ARE TORN DOWN IN IS SPELLED OUT
//
// server.h requires the engine to outlive the server, because the server
// installs a spectrum sink on it and fans frames out on its own thread.
// client.h owns an event loop thread of its own, and a client left connected
// while the server goes is the ordinary case rather than the exception, but
// a client destroyed after the engine would be one calling into a torn-down
// service. Member order already gives the right sequence by reverse
// destruction; Harness::shutdown does it explicitly so that reordering the
// members cannot silently change it.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/dsp/types.h"
#include "core/engine/engine.h"
#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/server.h"

namespace revenant::test {

// A source rate the channel count does not divide, chosen for that reason
// and constrained by one rule that is not obvious.
//
// 2400000 over 64 channels is 37500 hertz on the nose, and a suite built on
// it would pass with every rational in the schema rounded to an integer on
// the way out. docs/conventions.md says k*rate/M is usually not a whole
// number and that rounding it anywhere puts an unsourceable offset into the
// display, so testing at the one rate where that cannot show up tests
// nothing.
//
// The obvious answer, an odd rate, is refused: engine::place rejects any
// rate the grid's decimation does not divide, because the channel stream's
// rate has to be a whole number of samples per second. So the rate has to be
// a multiple of D and not of M, and with the 2x-oversampled grid this
// project uses, D is M/2 and that is exactly one bit of freedom.
//
// 2400032 is 32 * 75001. The decimation, 32, divides it and the channel rate
// is 75001 S/s exactly. The channel count, 64, does not: every odd channel
// sits at a half hertz. The spectrum's 8192 points per frame do not either,
// so a bin is 75001/256 hertz wide and bin zero is -4875065/4.
inline constexpr dsp::SampleRate kSourceRate = 2'400'032;

// Grid and geometry the cases share.
//
// 256 points per channel rather than the engine's 2048: the frame is then
// 8192 bins and 32 KiB, which is enough to prove a frame crosses intact and
// small enough that a case streaming three hundred of them over loopback
// costs nothing worth measuring.
inline constexpr std::uint32_t kChannels = 64;
inline constexpr std::uint32_t kSpectrumTransform = 256;

// A scene with something in it. The bins have to carry structure for a case
// to be able to say they arrived intact, and a frame of pure noise would let
// a truncated copy pass.
//
// center_hz is the real radio frequency baseband DC is a label for, and it
// reaches EngineInfo::source_center. Zero for every case that does not care,
// which is most of them; the detection cases pass a real one because a
// detection is documented as carrying ABSOLUTE frequency with source_center
// already added, and at a centre of zero absolute and baseband are the same
// number and the case would prove nothing.
[[nodiscard]] std::string scene_uri(dsp::SampleIndex samples, dsp::Hertz center_hz = 0);

struct HarnessOptions {
    // Zero builds no spectrum stage, which is what the session cases want:
    // they never subscribe and the stage costs a pipeline and a readback per
    // frame in flight.
    std::uint32_t spectrum_transform = 0;

    // Zero builds no passband stage, on the same terms and for the same
    // reason: a receiver's passband is per-receiver opt in and costs a
    // pipeline and a readback per frame in flight once anything subscribes.
    std::uint32_t passband_transform = 0;

    // Always bounded. A synthetic source with no sample count runs forever,
    // and a test that hangs reports less than one that fails.
    dsp::SampleIndex samples = 2'400'032;

    // Multiple of realtime. Zero is unthrottled, which is right for a case
    // that never runs the engine and wrong for one that wants frames to
    // arrive over a window it can act inside.
    double pace = 0.0;

    std::uint32_t block_samples = 16'384;

    // Capture history the ring is asked for, in seconds.
    //
    // The default is not neutral and the clamp cases in test_rpc_session.cpp
    // depend on knowing which way it leans. Half a second at kSourceRate is
    // 1200016 samples, which is not a power of two, so plan_ring_geometry
    // rounds it down and reports the ring as clamped: the engine hands back
    // 1048576 samples, read off the wire on 2026-09-19. Every case that
    // leaves this alone therefore runs against an engine whose EngineInfo
    // already carries a clamp reason.
    //
    // Asking for LESS than the sizing floor is what produces an unclamped
    // engine: Engine::open_source substitutes its own floor, which is a
    // bit_ceil and so already a power of two, and a ring that got at least
    // what was asked for is not a clamp. kUnclampedRingSeconds is that
    // request, and the clamp case's control arm is the only proof that this
    // suite's clamp assertions are not asserting a field that is always set.
    double ring_seconds = 0.5;

    // What baseband DC is a label for, in real radio frequency. See
    // scene_uri: zero everywhere except the detection cases, which need
    // absolute and baseband to be different numbers.
    dsp::Hertz center_hz = 0;

    // False leaves the client unconnected, for the cases that are about
    // connecting.
    bool connect_client = true;
};

// A ring request the engine satisfies in full, so nothing is clamped.
//
// Below the floor Engine::open_source computes from the filter support and
// the block size, which for this fixture's 64-channel grid, 17 taps a branch
// and 16384-sample blocks is bit_ceil(4 * (1088 + 16384)). Any request under
// that is replaced by the floor itself, and the floor is a bit_ceil, so the
// engine reports the ring as unclamped: 131072 samples and 0.0546 s
// retained, read off the wire on 2026-09-19.
inline constexpr double kUnclampedRingSeconds = 0.02;

// A spectrum transform larger than either device in the conformance matrix
// will run, so the engine reduces it and says so.
//
// The ceiling is min(max_fft_transform_size(shared memory), 2048), and the
// second term is the one that binds on both devices: a 4096-point transform
// wants 32768 bytes of workgroup shared memory, and on 2026-09-19 the RTX
// 4090 reported 49152 bytes and the integrated AMD part 32768, so shared
// memory is not what refuses it. core/dsp/spectrum_reference.h caps the
// transform at the 2048 points build_twiddles will produce a circle for.
// The clamped geometry that comes back is 2048 points on either device.
inline constexpr std::uint32_t kOverLargeSpectrumTransform = 4096;

class Harness {
public:
    Harness() = default;
    ~Harness();

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;
    Harness(Harness&&) = delete;
    Harness& operator=(Harness&&) = delete;

    // Engine, source, server, client, in that order. Returns the first
    // failure with the layer that produced it named, so a case that REQUIREs
    // this prints which seam did not come up rather than a bare false.
    [[nodiscard]] Status open(const HarnessOptions& options);

    // Valid after open() succeeded.
    [[nodiscard]] engine::Engine& engine() { return *engine_; }
    [[nodiscard]] rpc::Server& server() { return *server_; }
    [[nodiscard]] rpc::Client& client() { return *client_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }

    // A second client on the same server, for the cases that need one. Owned
    // by the caller and destroyed before the harness, which the caller gets
    // for free by declaring it after.
    [[nodiscard]] Expected<std::unique_ptr<rpc::Client>> connect_another();

    // Runs the engine on its own thread. Every spectrum case needs this,
    // because run() blocks until the source ends and the client has to be
    // driven while it does.
    [[nodiscard]] Status start_engine();

    // Stops the engine, joins the thread and hands back what run() returned,
    // with the graph's own cancellation treated as the clean finish it is.
    // Idempotent, and also run by the destructor.
    [[nodiscard]] Status stop_engine();

    // Blocks until the source has delivered this many blocks, or the deadline
    // passes. Returns what it saw, so a case can assert on it rather than
    // assuming the wait succeeded.
    [[nodiscard]] std::uint64_t wait_for_blocks(std::uint64_t blocks, int timeout_ms);

private:
    void shutdown();

    // Declared in the order they are built and destroyed in reverse, which is
    // the order the headers require. See the note at the top.
    std::unique_ptr<engine::Engine> engine_;
    std::unique_ptr<rpc::Server> server_;
    std::unique_ptr<rpc::Client> client_;

    std::thread runner_;
    Status run_outcome_;
    bool running_ = false;
    bool stopped_ = false;
    std::uint16_t port_ = 0;
};

// Frames whose bins are kept whole rather than counted.
//
// The callback runs on the client's event loop thread and client.h requires
// it to copy anything it keeps. At 8192 bins a frame is 32 KiB, so keeping
// every one of a three-second run would be tens of megabytes nobody reads.
// Keeping the first few dozen is enough for the integrity case to ask its
// question of more than one frame, which is what tells an intermittent
// device fault apart from a systematic copy fault.
inline constexpr std::size_t kKeptFrames = 48;

// One received frame, reduced to what a case asserts on.
struct FrameRecord {
    std::uint64_t sequence = 0;
    std::uint64_t start = 0;
    std::uint64_t count = 0;
    std::size_t bins = 0;
    rpc::SpectrumGeometry geometry;
    float floor_db = 0.0F;
    float ceiling_db = 0.0F;
    float percentile_low_db = 0.0F;
    float percentile_high_db = 0.0F;

    // Empty past kKeptFrames. Non-empty means these are the bins that
    // arrived with the percentiles above, which is the pairing the integrity
    // case rests on.
    std::vector<float> power_db;
};

// A subscriber's record of what arrived, safe to read from the test thread
// while the callback is still running on the loop thread.
//
// Held through a shared_ptr the callback captures by value, so its lifetime
// is not a question about which local a case declared first. The callback
// outlives the statement that installed it and is dropped by the client's
// event loop thread, which is a worse place to discover a dangling reference
// than a compile error would have been.
class FrameLog {
public:
    // Event loop thread.
    void record(const rpc::SpectrumFrame& frame);

    [[nodiscard]] std::vector<FrameRecord> frames() const;
    [[nodiscard]] std::size_t size() const;

    // Set when a frame arrived while the previous callback had not returned.
    // client.h describes the count this would feed; see the note in the
    // backpressure case about why it stays false.
    [[nodiscard]] bool reentered() const;

    // Slows every callback by this much, which is how the backpressure case
    // makes a subscriber the engine cannot keep up with.
    void set_callback_delay(std::chrono::milliseconds delay);

private:
    mutable std::mutex lock_;
    std::vector<FrameRecord> frames_;
    std::chrono::milliseconds delay_{0};
    bool inside_ = false;
    bool reentered_ = false;
};

}  // namespace revenant::test
