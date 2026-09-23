// The egress layer.
//
// The threading contract is stated in audio_egress.h and this file is the
// thing that has to actually hold it up. Three parts of it are not obvious
// from the interface.
//
// THE SLOT TABLE is a fixed array of slots with an atomic state each, the
// same shape as ConsumerTable in core/engine/ring_consumer.h and for the same
// reason: publish() runs on the readback thread and must not walk a container
// that another thread can reallocate underneath it. A separate compact array
// of receiver ids sits beside the slots so the lookup touches four cache
// lines rather than the whole table.
//
// THE IN-FLIGHT GUARD is what makes removing a receiver safe while the
// producer is still running. A producer entering publish() increments the
// slot's publisher count, then checks the slot is still active; a control
// thread closing the slot marks it closing, then waits for the count to reach
// zero. Both pairs are sequentially consistent, and that is not
// over-ordering: with release and acquire alone this is the store buffer
// pattern, and the control thread is allowed to observe a count of zero while
// the producer observes an active slot, which is exactly the interleaving
// that frees the ring out from under a memcpy. On x86 the producer's cost is
// a lock-prefixed add it was going to pay anyway.
//
// THE FAULT PATH allocates nothing on the drain thread. A backend's error is
// copied into a fixed buffer in the slot, once, and the slot then stops
// draining, so a disk that has filled up costs one allocation inside the
// backend that produced the message and nothing after that. The receiver
// keeps counting drops, which is what makes the failure visible from the
// outside instead of the recording simply stopping.

#include "core/engine/audio_egress.h"
#include "core/thread_role.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <format>
#include <mutex>
#include <span>
#include <system_error>
#include <thread>
#include <utility>

namespace revenant::engine {
namespace {

// Slot lifecycle. Free is the only state a slot can be claimed from, and
// every transition below is made by exactly one thread.
//
//   Free    -> Busy      control, by CAS, claiming the slot
//   Busy    -> Active    control, once the ring and the backend are installed
//   Busy    -> Free      control, if opening the backend failed
//   Active  -> Closing   control, at remove_receiver or teardown
//   Closing -> Closed    the drain thread, acknowledging that it has let go
//   Closed  -> Free      control, after the last producer has left
constexpr std::uint32_t kSlotFree = 0;
constexpr std::uint32_t kSlotBusy = 1;
constexpr std::uint32_t kSlotActive = 2;
constexpr std::uint32_t kSlotClosing = 3;
constexpr std::uint32_t kSlotClosed = 4;

// A fault message is copied into the slot rather than stored as a string, so
// that recording one costs no allocation on the drain thread. Long enough for
// the chained messages core/error.h produces.
constexpr std::size_t kFaultMessageBytes = 256;

// Silence, for covering a hole in the sample index from the producer thread.
// constexpr so it is in the image rather than built at startup, and shared
// because nothing writes to it.
constexpr std::size_t kSilenceSamples = 512;

// The message for a backend that threw, built where a second failure cannot
// escape.
//
// std::format and the string inside Error both allocate, and this runs in a
// catch block on a thread where an escaping exception is std::terminate. An
// Error with no message is a poor report and an enormous improvement on the
// process ending without one. Same shape and same reasoning as
// engine::sink_threw in core/engine/engine.h.
[[nodiscard]] Status backend_threw(const char* method, const char* detail) noexcept
{
    try {
        return fail(detail != nullptr
                        ? std::format("the audio backend's {}() threw an exception: {}", method,
                                      detail)
                        : std::format("the audio backend's {}() threw an exception that is not "
                                      "a std::exception",
                                      method));
    } catch (...) {
    }
    return std::unexpected(Error{});
}

// Calls AudioBackend::write and turns a throw into the ordinary error the
// fault path already records.
//
// WHY THE CATCH IS HERE AND NOT SOMEWHERE FURTHER OUT. drain_loop runs on a
// std::thread this class spawned, so an exception leaving write() is
// std::terminate: the process is gone, with no error of its own, on the one
// path whose whole design is that one receiver's disk filling up does not
// stop the other forty-nine. AudioBackend is an interface a caller
// implements, so what it throws is not this file's to enumerate.
//
// core/engine/engine.h gave the completion thread exactly this catch, in
// call_sink, and said why at length. The sweep that added it stopped at the
// engine's own sinks and did not reach the thread one layer above them. The
// other caller, final_drain, is on the control thread where a throw would
// merely unwind through a Status-returning API rather than end the process;
// it goes through here too, because a caller that gets a Status back for a
// disk error and an exception for a bug in the same backend has to handle
// both to handle either.
//
// The success path allocates nothing, which is what the drain thread's
// contract in core/engine/audio_egress.h requires. The failure path
// allocates a message, once per slot, exactly as a returned Error already
// does: record_fault copies it into the slot's fixed buffer and the slot
// stops draining, so there is no second one.
[[nodiscard]] Status write_to_backend(AudioBackend& backend, std::span<const float> samples)
{
    try {
        return backend.write(samples);
    } catch (const std::exception& thrown) {
        return backend_threw("write", thrown.what());
    } catch (...) {
        return backend_threw("write", nullptr);
    }
}
constexpr std::array<float, kSilenceSamples> kSilence{};

// How long the control thread sleeps while waiting for the drain thread to
// acknowledge a close. Short enough not to add a visible pause to removing a
// receiver, long enough not to spin a core while the drain thread is inside a
// filesystem call.
constexpr std::chrono::microseconds kAcknowledgePoll{100};

class WavFileBackend final : public AudioBackend {
public:
    WavFileBackend(std::string path, const WavWriteOptions& options)
        : path_(std::move(path)), options_(options)
    {
    }

    [[nodiscard]] AudioBackendKind kind() const override { return AudioBackendKind::Push; }

    Status open(const AudioStreamInfo& info) override
    {
        WavStreamSpec spec;
        spec.path = path_;
        spec.rate = info.rate;
        spec.channels = info.channels;
        spec.metadata.name = info.label;
        spec.metadata.created = info.created;
        spec.metadata.source = info.source_uri;
        spec.metadata.vrx = info.vrx.value;
        spec.metadata.center = info.center;
        spec.metadata.demod = demod_name(info.demod);
        spec.metadata.start = info.start;
        spec.metadata.epoch_anchor_ns = info.epoch_anchor_ns;

        auto writer = open_wav_writer(spec, options_);
        if (!writer) {
            return std::unexpected(writer.error());
        }
        writer_ = std::move(*writer);
        return {};
    }

    Status write(std::span<const float> interleaved) override
    {
        if (writer_ == nullptr) {
            return fail(std::format("write to '{}' before it was opened", path_));
        }
        return writer_->write(interleaved);
    }

    Status flush() override
    {
        return writer_ == nullptr ? Status{} : writer_->flush();
    }

    Status close() override
    {
        if (writer_ == nullptr) {
            return {};
        }
        Status result = writer_->close();
        writer_.reset();
        return result;
    }

private:
    std::string path_;
    WavWriteOptions options_;
    std::unique_ptr<WavWriter> writer_;
};

// One receiver's egress path.
//
// Not cache-line aligned, deliberately. Alignment that pads a structure is
// C4324 and therefore an error under /WX, and the contended data here is the
// ring's own cursors, which are padded inside SpscRing. Two slots sharing a
// line costs a coherence bounce per block, not per sample.
struct Slot {
    std::atomic<std::uint32_t> state{kSlotFree};

    // Producers currently inside publish() for this slot. See the in-flight
    // guard in the file comment.
    std::atomic<std::uint32_t> publishers{0};

    // Installed while the slot is Busy and read only while it is Active or
    // Closing, so no synchronisation of its own is needed: the release store
    // of state publishes it and the matching acquire load consumes it.
    std::unique_ptr<SpscRing<float>> ring;
    std::unique_ptr<AudioBackend> backend;
    AudioStreamInfo info;
    AudioBackendKind kind = AudioBackendKind::Push;
    std::uint32_t channels = 1;
    dsp::SampleRate rate = 0;
    std::size_t max_backlog_frames = 0;
    std::uint64_t max_gap_fill_frames = 0;

    // Touched only by the producer thread, and only between its in-flight
    // increment and decrement, so it needs no atomicity.
    dsp::SampleIndex next_start = 0;
    bool have_next_start = false;

    std::atomic<std::uint64_t> frames_published{0};
    std::atomic<std::uint64_t> frames_dropped{0};
    std::atomic<std::uint64_t> drop_events{0};
    std::atomic<std::uint64_t> frames_filled{0};
    std::atomic<std::uint64_t> gap_events{0};
    std::atomic<std::uint64_t> discontinuities{0};
    std::atomic<std::uint64_t> frames_trimmed{0};
    std::atomic<std::uint64_t> frames_written{0};

    std::atomic<bool> fault_set{false};
    std::atomic<long long> fault_code{0};
    char fault_message[kFaultMessageBytes]{};
};

class AudioEgressImpl final : public AudioEgress {
public:
    explicit AudioEgressImpl(const AudioEgressConfig& config) : config_(config) {}

    ~AudioEgressImpl() override
    {
        static_cast<void>(stop());

        const std::lock_guard<std::mutex> lock(control_);
        for (std::uint32_t index = 0; index < kMaxAudioReceivers; ++index) {
            if (slots_[index].state.load(std::memory_order_acquire) == kSlotActive) {
                static_cast<void>(close_slot(index));
            }
        }
    }

    // --- the control thread -------------------------------------------------

    Status add_receiver(const AudioStreamInfo& info,
                        std::unique_ptr<AudioBackend> backend) override
    {
        if (backend == nullptr) {
            return fail("add_receiver was given no backend");
        }
        if (!info.vrx.valid()) {
            return fail("add_receiver was given receiver id 0, which is not a receiver");
        }
        if (info.rate <= 0) {
            return fail(std::format("receiver {} was registered with an audio rate of {}",
                                    info.vrx.value, info.rate));
        }
        if (info.channels == 0 || info.channels > kMaxAudioChannels) {
            return fail(std::format(
                "receiver {} asked for {} audio channels; the egress path carries 1 or 2",
                info.vrx.value, info.channels));
        }

        const auto capacity = ring_capacity(info);
        if (!capacity) {
            return std::unexpected(capacity.error());
        }

        const std::lock_guard<std::mutex> lock(control_);

        if (find_index(info.vrx) >= 0) {
            return fail(std::format("receiver {} already has an egress path", info.vrx.value));
        }

        const int claimed = claim_slot();
        if (claimed < 0) {
            return fail(std::format("all {} egress slots are in use", kMaxAudioReceivers));
        }
        const auto index = static_cast<std::uint32_t>(claimed);
        Slot& slot = slots_[index];

        auto ring = SpscRing<float>::create(*capacity);
        if (!ring) {
            slot.state.store(kSlotFree, std::memory_order_release);
            return std::unexpected(
                with_context(ring.error(),
                             std::format("sizing the audio ring for receiver {}",
                                         info.vrx.value)));
        }
        slot.ring = std::move(*ring);

        if (auto opened = backend->open(info); !opened) {
            slot.ring.reset();
            slot.state.store(kSlotFree, std::memory_order_release);
            return std::unexpected(with_context(
                opened.error(), std::format("opening the audio backend for receiver {}",
                                            info.vrx.value)));
        }

        slot.kind = backend->kind();
        if (slot.kind == AudioBackendKind::Pull) {
            // The tap points at the ring, so it is installed first: a backend
            // that starts its device thread inside attach() may read through
            // the tap before attach() has even returned.
            const AudioTap tap(slot.ring.get(), info.rate, info.channels);
            if (auto attached = backend->attach(tap); !attached) {
                static_cast<void>(backend->close());
                slot.ring.reset();
                slot.state.store(kSlotFree, std::memory_order_release);
                return std::unexpected(with_context(
                    attached.error(),
                    std::format("attaching the audio device for receiver {}", info.vrx.value)));
            }
        }

        slot.backend = std::move(backend);
        slot.info = info;
        slot.channels = info.channels;
        slot.rate = info.rate;
        slot.max_backlog_frames = backlog_frames(info);
        slot.max_gap_fill_frames = gap_fill_frames(info);

        // The first chunk sets the baseline rather than being measured
        // against info.start. A caller that leaves start at zero on a stream
        // that begins elsewhere would otherwise have its recording open with
        // a gap it never had.
        slot.next_start = info.start;
        slot.have_next_start = false;

        reset_counters(slot);

        ids_[index].store(info.vrx.value, std::memory_order_release);

        // Release: everything above must be visible to a producer that
        // observes this slot as active, and to the drain thread.
        slot.state.store(kSlotActive, std::memory_order_release);
        return {};
    }

    Status remove_receiver(VrxId id) override
    {
        const std::lock_guard<std::mutex> lock(control_);
        const int index = find_index(id);
        if (index < 0) {
            return fail(std::format("no egress path is registered for receiver {}", id.value));
        }
        return close_slot(static_cast<std::uint32_t>(index));
    }

    Status start() override
    {
        const std::lock_guard<std::mutex> lock(control_);
        if (thread_.joinable()) {
            return fail("the audio egress thread is already running");
        }

        // Allocated here, on the control thread, so that the drain thread
        // allocates nothing at all, including on its first pass.
        const std::size_t frames = config_.drain_frames == 0 ? 4096 : config_.drain_frames;
        scratch_.assign(frames * kMaxAudioChannels, 0.0F);

        stop_requested_.store(false, std::memory_order_relaxed);
        try {
            thread_ = std::thread([this] { drain_loop(); });
        } catch (const std::system_error& error) {
            return fail(std::format("could not start the audio egress thread: {}", error.what()),
                        error.code().value());
        }
        running_.store(true, std::memory_order_release);
        return {};
    }

    Status stop() override
    {
        const std::lock_guard<std::mutex> lock(control_);
        return stop_locked();
    }

    [[nodiscard]] bool running() const override
    {
        return running_.load(std::memory_order_acquire);
    }

    Expected<AudioSink> sink_for(VrxId id) override
    {
        const std::lock_guard<std::mutex> lock(control_);
        if (find_index(id) < 0) {
            return fail(std::format("no egress path is registered for receiver {}", id.value));
        }

        // Resolves the receiver on every call rather than capturing a slot
        // index. A cached index goes stale the moment the receiver is removed
        // and re-added, and the failure that produces is audio delivered to
        // the wrong recording, which nothing downstream can detect. The scan
        // it avoids is 64 relaxed loads over 256 bytes, once per block.
        return AudioSink([this](const AudioChunk& chunk) { return publish(chunk); });
    }

    Expected<AudioEgressStats> stats(VrxId id) const override
    {
        const std::lock_guard<std::mutex> lock(control_);
        const int index = find_index(id);
        if (index < 0) {
            return fail(std::format("no egress path is registered for receiver {}", id.value));
        }
        return snapshot(slots_[static_cast<std::uint32_t>(index)]);
    }

    std::vector<AudioEgressStats> all_stats() const override
    {
        const std::lock_guard<std::mutex> lock(control_);
        std::vector<AudioEgressStats> out;
        for (const Slot& slot : slots_) {
            const std::uint32_t state = slot.state.load(std::memory_order_acquire);
            if (state == kSlotActive || state == kSlotClosing) {
                out.push_back(snapshot(slot));
            }
        }
        return out;
    }

    // --- the producer thread ------------------------------------------------

    Status publish(const AudioChunk& chunk) override
    {
        if (!chunk.vrx.valid()) {
            return fail("an audio chunk was published with no receiver id");
        }
        const int index = find_index(chunk.vrx);
        if (index < 0) {
            return fail(std::format("no egress path is registered for receiver {}",
                                    chunk.vrx.value));
        }
        Slot& slot = slots_[static_cast<std::uint32_t>(index)];

        // Sequentially consistent, and the file comment says why: acquire and
        // release alone leave the interleaving where the closer sees no
        // publishers while the publisher sees an active slot.
        slot.publishers.fetch_add(1, std::memory_order_seq_cst);
        const bool live = slot.state.load(std::memory_order_seq_cst) == kSlotActive &&
                          ids_[static_cast<std::uint32_t>(index)].load(
                              std::memory_order_acquire) == chunk.vrx.value;
        if (!live) {
            // The slot is part way into being registered or part way out of
            // being torn down. Not an error and not counted: in the first
            // case the receiver's counters do not exist yet and in the
            // second they are about to be discarded.
            slot.publishers.fetch_sub(1, std::memory_order_release);
            return {};
        }

        Status result = deliver(slot, chunk);
        slot.publishers.fetch_sub(1, std::memory_order_release);
        return result;
    }

private:
    // --- sizing, all on the control thread ----------------------------------

    [[nodiscard]] Expected<std::size_t> ring_capacity(const AudioStreamInfo& info) const
    {
        const double wanted = config_.ring_seconds * static_cast<double>(info.rate) *
                              static_cast<double>(info.channels);
        if (!(wanted >= 0.0) || wanted > static_cast<double>(kMaxSpscCapacity)) {
            return fail(std::format(
                "an audio ring of {} seconds at {} Hz and {} channels is not a buffer anyone "
                "wants",
                config_.ring_seconds, info.rate, info.channels));
        }

        // At least two drain passes, so a single pass is never bounded by the
        // ring rather than by the scratch buffer.
        const std::size_t drain = (config_.drain_frames == 0 ? 4096 : config_.drain_frames) *
                                  info.channels * 2;
        return std::max(static_cast<std::size_t>(wanted), drain);
    }

    [[nodiscard]] std::size_t backlog_frames(const AudioStreamInfo& info) const
    {
        if (!(config_.max_backlog_seconds > 0.0)) {
            return 0;
        }
        const double frames = config_.max_backlog_seconds * static_cast<double>(info.rate);
        return static_cast<std::size_t>(std::min(frames, static_cast<double>(kMaxSpscCapacity)));
    }

    [[nodiscard]] std::uint64_t gap_fill_frames(const AudioStreamInfo& info) const
    {
        if (!(config_.max_gap_fill_seconds > 0.0)) {
            return 0;
        }
        const double frames = config_.max_gap_fill_seconds * static_cast<double>(info.rate);
        return static_cast<std::uint64_t>(std::min(frames, static_cast<double>(kMaxSpscCapacity)));
    }

    [[nodiscard]] int claim_slot()
    {
        for (std::uint32_t index = 0; index < kMaxAudioReceivers; ++index) {
            std::uint32_t expected = kSlotFree;
            if (slots_[index].state.compare_exchange_strong(expected, kSlotBusy,
                                                            std::memory_order_acq_rel,
                                                            std::memory_order_relaxed)) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    static void reset_counters(Slot& slot)
    {
        slot.frames_published.store(0, std::memory_order_relaxed);
        slot.frames_dropped.store(0, std::memory_order_relaxed);
        slot.drop_events.store(0, std::memory_order_relaxed);
        slot.frames_filled.store(0, std::memory_order_relaxed);
        slot.gap_events.store(0, std::memory_order_relaxed);
        slot.discontinuities.store(0, std::memory_order_relaxed);
        slot.frames_trimmed.store(0, std::memory_order_relaxed);
        slot.frames_written.store(0, std::memory_order_relaxed);
        slot.fault_code.store(0, std::memory_order_relaxed);
        slot.fault_message[0] = '\0';
        slot.fault_set.store(false, std::memory_order_release);
    }

    // Any thread. The id array is compact so this touches four cache lines
    // rather than the whole slot table.
    [[nodiscard]] int find_index(VrxId id) const
    {
        if (!id.valid()) {
            return -1;
        }
        for (std::uint32_t index = 0; index < kMaxAudioReceivers; ++index) {
            if (ids_[index].load(std::memory_order_acquire) == id.value) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    [[nodiscard]] AudioEgressStats snapshot(const Slot& slot) const
    {
        AudioEgressStats out;
        out.vrx = slot.info.vrx;
        out.active = slot.state.load(std::memory_order_acquire) == kSlotActive;
        out.rate = slot.rate;
        out.channels = slot.channels;
        out.frames_published = slot.frames_published.load(std::memory_order_relaxed);
        out.frames_dropped = slot.frames_dropped.load(std::memory_order_relaxed);
        out.drop_events = slot.drop_events.load(std::memory_order_relaxed);
        out.frames_filled = slot.frames_filled.load(std::memory_order_relaxed);
        out.gap_events = slot.gap_events.load(std::memory_order_relaxed);
        out.discontinuities = slot.discontinuities.load(std::memory_order_relaxed);
        out.frames_trimmed = slot.frames_trimmed.load(std::memory_order_relaxed);
        out.frames_written = slot.frames_written.load(std::memory_order_relaxed);
        if (slot.ring != nullptr && slot.channels != 0) {
            out.backlog_frames = slot.ring->readable() / slot.channels;
            out.underrun_frames = slot.ring->underrun_samples() / slot.channels;
            out.underrun_events = slot.ring->underrun_events();
        }
        if (slot.fault_set.load(std::memory_order_acquire)) {
            out.faulted = true;
            out.fault = slot.fault_message;
            out.fault_code = slot.fault_code.load(std::memory_order_relaxed);
        }
        return out;
    }

    // --- teardown, control thread with the mutex held -----------------------

    Status close_slot(std::uint32_t index)
    {
        Slot& slot = slots_[index];
        if (slot.state.load(std::memory_order_acquire) != kSlotActive) {
            return fail(std::format("egress slot {} is not open", index));
        }

        // No producer gets past its liveness check from here on.
        slot.state.store(kSlotClosing, std::memory_order_seq_cst);

        if (thread_.joinable()) {
            // The drain thread acknowledges at the top of its next pass, so
            // this waits at most one pass plus whatever filesystem call it is
            // already inside.
            while (slot.state.load(std::memory_order_acquire) != kSlotClosed) {
                std::this_thread::sleep_for(kAcknowledgePoll);
            }
        } else {
            slot.state.store(kSlotClosed, std::memory_order_release);
        }

        // And now wait out any producer that was already inside publish()
        // when the state changed. Bounded by one memcpy.
        while (slot.publishers.load(std::memory_order_seq_cst) != 0) {
            std::this_thread::yield();
        }

        // Nothing else can reach the ring or the backend, so the last of the
        // audio can be moved across from this thread.
        Status result = final_drain(slot);
        if (slot.backend != nullptr) {
            if (auto closed = slot.backend->close(); !closed && result) {
                result = std::unexpected(closed.error());
            }
        }

        slot.backend.reset();
        slot.ring.reset();
        slot.have_next_start = false;
        ids_[index].store(0, std::memory_order_release);
        slot.state.store(kSlotFree, std::memory_order_release);
        return result;
    }

    Status stop_locked()
    {
        stop_requested_.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
        running_.store(false, std::memory_order_release);

        // With the drain thread gone this thread is the only reader, so it
        // finishes the job: everything still in a ring goes to its backend
        // and every backend is made durable. A recording is complete and
        // playable when stop() returns, without having been closed.
        Status result;
        for (Slot& slot : slots_) {
            if (slot.state.load(std::memory_order_acquire) != kSlotActive) {
                continue;
            }
            if (auto drained = final_drain(slot); !drained && result) {
                result = std::unexpected(drained.error());
            }
            if (slot.backend != nullptr) {
                if (auto flushed = slot.backend->flush(); !flushed && result) {
                    result = std::unexpected(flushed.error());
                }
            }
        }
        return result;
    }

    // Control thread, and only when no other thread can be reading the ring.
    // Allocates, which is why it is not the drain thread's business.
    Status final_drain(Slot& slot)
    {
        if (slot.kind != AudioBackendKind::Push || slot.ring == nullptr ||
            slot.backend == nullptr || slot.channels == 0) {
            return {};
        }
        if (slot.fault_set.load(std::memory_order_acquire)) {
            return {};
        }

        const std::size_t frames = config_.drain_frames == 0 ? 4096 : config_.drain_frames;
        std::vector<float> buffer(frames * slot.channels, 0.0F);

        for (;;) {
            const std::size_t ready = slot.ring->readable();
            if (ready < slot.channels) {
                return {};
            }
            const std::size_t aligned =
                (std::min(ready, buffer.size()) / slot.channels) * slot.channels;
            if (aligned == 0) {
                return {};
            }
            const std::size_t got = slot.ring->read(std::span<float>(buffer.data(), aligned));
            if (got == 0) {
                return {};
            }
            if (auto written =
                    write_to_backend(*slot.backend, std::span<const float>(buffer.data(), got));
                !written) {
                record_fault(slot, written.error());
                return std::unexpected(written.error());
            }
            slot.frames_written.fetch_add(got / slot.channels, std::memory_order_relaxed);
        }
    }

    // --- the drain thread ---------------------------------------------------

    void drain_loop()
    {
        name_this_thread(L"revenant egress");
        while (!stop_requested_.load(std::memory_order_acquire)) {
            if (!drain_pass()) {
                std::this_thread::sleep_for(config_.idle_poll);
            }
        }

        // One last pass, so a close requested at the same moment as the stop
        // is acknowledged rather than waited on by a control thread that has
        // not joined yet.
        static_cast<void>(drain_pass());
    }

    bool drain_pass()
    {
        bool moved = false;
        for (Slot& slot : slots_) {
            const std::uint32_t state = slot.state.load(std::memory_order_acquire);
            if (state == kSlotClosing) {
                // Release: this thread is done with the slot's ring and
                // backend, and the control thread is waiting to see exactly
                // that before it frees them.
                slot.state.store(kSlotClosed, std::memory_order_release);
                continue;
            }
            if (state != kSlotActive || slot.kind != AudioBackendKind::Push) {
                continue;
            }
            if (slot.fault_set.load(std::memory_order_acquire)) {
                // A faulted backend is not called again. The ring fills, the
                // producer counts drops, and the fault and the drops are both
                // visible in the statistics.
                continue;
            }
            moved = drain_slot(slot) || moved;
        }
        return moved;
    }

    bool drain_slot(Slot& slot)
    {
        SpscRing<float>& ring = *slot.ring;

        if (slot.max_backlog_frames != 0) {
            const std::size_t trimmed = ring.trim_to(slot.max_backlog_frames * slot.channels);
            if (trimmed != 0) {
                slot.frames_trimmed.fetch_add(trimmed / slot.channels,
                                              std::memory_order_relaxed);
            }
        }

        const std::size_t ready = ring.readable();
        if (ready < slot.channels) {
            return false;
        }
        const std::size_t aligned =
            (std::min(ready, scratch_.size()) / slot.channels) * slot.channels;
        if (aligned == 0) {
            return false;
        }

        const std::size_t got = ring.read(std::span<float>(scratch_.data(), aligned));
        if (got == 0) {
            return false;
        }
        if (auto written =
                write_to_backend(*slot.backend, std::span<const float>(scratch_.data(), got));
            !written) {
            record_fault(slot, written.error());
            return false;
        }
        slot.frames_written.fetch_add(got / slot.channels, std::memory_order_relaxed);
        return true;
    }

    // Drain thread. Copies into a fixed buffer, so this allocates nothing;
    // the only allocation on the whole fault path is whatever the backend
    // spent building the message, once.
    static void record_fault(Slot& slot, const Error& error)
    {
        if (slot.fault_set.load(std::memory_order_acquire)) {
            return;
        }
        const std::size_t count = std::min(error.message.size(), kFaultMessageBytes - 1);
        if (count != 0) {
            std::memcpy(slot.fault_message, error.message.data(), count);
        }
        slot.fault_message[count] = '\0';
        slot.fault_code.store(error.code, std::memory_order_relaxed);

        // Release, so a reader that sees the flag sees the message.
        slot.fault_set.store(true, std::memory_order_release);
    }

    // --- the producer thread ------------------------------------------------

    static void note_drop(Slot& slot, std::uint64_t frames)
    {
        if (frames == 0) {
            return;
        }
        slot.frames_dropped.fetch_add(frames, std::memory_order_relaxed);
        slot.drop_events.fetch_add(1, std::memory_order_relaxed);
    }

    // Covers a hole in the sample index with silence so that the recording
    // stays aligned with the stream's timeline. A recording whose length no
    // longer matches the index it started at cannot be correlated with
    // anything else, and the sample index is the authority the whole engine
    // is built on. Writes from a shared constant buffer, so it allocates
    // nothing.
    static void fill_silence(Slot& slot, std::uint64_t frames)
    {
        const std::size_t channels = slot.channels;
        const std::size_t per_pass = kSilenceSamples / channels;
        std::uint64_t remaining = frames;
        std::uint64_t filled = 0;

        while (remaining != 0) {
            const std::size_t room = slot.ring->writable() / channels;
            if (room == 0) {
                break;
            }
            const auto want = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, std::min(room, per_pass)));
            if (want == 0) {
                break;
            }
            const std::size_t wrote =
                slot.ring->write(std::span<const float>(kSilence.data(), want * channels));
            if (wrote == 0) {
                break;
            }
            const std::uint64_t wrote_frames = wrote / channels;
            filled += wrote_frames;
            remaining -= wrote_frames;
        }

        if (filled != 0) {
            slot.frames_filled.fetch_add(filled, std::memory_order_relaxed);
            slot.gap_events.fetch_add(1, std::memory_order_relaxed);
        }
        note_drop(slot, remaining);
    }

    Status deliver(Slot& slot, const AudioChunk& chunk) const
    {
        if (chunk.channels != slot.channels) {
            return fail(std::format(
                "receiver {} is registered for {} audio channels and published {}",
                slot.info.vrx.value, slot.channels, chunk.channels));
        }
        if (chunk.rate != slot.rate) {
            return fail(std::format("receiver {} is registered at {} Hz and published at {} Hz",
                                    slot.info.vrx.value, slot.rate, chunk.rate));
        }
        if (chunk.samples.empty()) {
            return {};
        }
        if (chunk.samples.size() % slot.channels != 0) {
            return fail(std::format(
                "receiver {} published {} samples, which is not a whole number of {}-channel "
                "frames",
                slot.info.vrx.value, chunk.samples.size(), slot.channels));
        }

        const auto frames = static_cast<std::uint64_t>(chunk.samples.size() / slot.channels);

        if (slot.have_next_start && chunk.start != slot.next_start) {
            if (chunk.start > slot.next_start) {
                const std::uint64_t gap = chunk.start - slot.next_start;
                if (slot.max_gap_fill_frames != 0 && gap <= slot.max_gap_fill_frames) {
                    fill_silence(slot, gap);
                } else {
                    // Too large to be a loss worth papering over. A seek or a
                    // retune, so the recording simply carries on from here.
                    slot.discontinuities.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                // Backwards, so a seek into history. Nothing to fill.
                slot.discontinuities.fetch_add(1, std::memory_order_relaxed);
            }
        }
        slot.next_start = chunk.start + frames;
        slot.have_next_start = true;

        // write() with a frame-aligned span rather than write_or_drop().
        //
        // write_or_drop() would take whatever fits, and what fits is a sample
        // count, not a frame count. Stopping half way through a stereo frame
        // puts every sample after it in the wrong channel for the rest of the
        // recording, which is a fault nobody traces back to a full ring. So
        // the writer trims to whole frames and counts the remainder itself,
        // which is also why frames_dropped here and the ring's own
        // dropped_samples() are not the same number.
        const std::size_t room = slot.ring->writable() / slot.channels;
        const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(frames, room));

        std::uint64_t accepted = 0;
        if (take != 0) {
            const std::size_t wrote =
                slot.ring->write(chunk.samples.first(take * slot.channels));
            accepted = wrote / slot.channels;
            slot.frames_published.fetch_add(accepted, std::memory_order_relaxed);
        }
        if (accepted < frames) {
            note_drop(slot, frames - accepted);
        }
        return {};
    }

    AudioEgressConfig config_;

    // Guards every control-plane operation against every other one. Never
    // taken by publish(), by the drain thread, or by a device callback.
    mutable std::mutex control_;

    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};

    // The drain thread's only buffer, allocated by start().
    std::vector<float> scratch_;

    std::array<Slot, kMaxAudioReceivers> slots_{};

    // Receiver id per slot, or zero. Separate from the slots so that the
    // lookup on the producer thread reads 256 contiguous bytes instead of
    // striding the whole table.
    std::array<std::atomic<std::uint32_t>, kMaxAudioReceivers> ids_{};
};

}  // namespace

Expected<std::unique_ptr<AudioBackend>> make_wav_file_backend(std::string path,
                                                               const WavWriteOptions& options)
{
    if (path.empty()) {
        return fail("make_wav_file_backend was given an empty path");
    }
    return std::make_unique<WavFileBackend>(std::move(path), options);
}

Expected<std::unique_ptr<AudioEgress>> AudioEgress::create(const AudioEgressConfig& config)
{
    if (!(config.ring_seconds > 0.0)) {
        return fail(std::format("an audio ring of {} seconds is not a ring", config.ring_seconds));
    }
    if (config.drain_frames == 0) {
        return fail("the audio egress drain size is zero");
    }
    if (config.idle_poll.count() < 0) {
        return fail("the audio egress idle poll interval is negative");
    }
    return std::make_unique<AudioEgressImpl>(config);
}

}  // namespace revenant::engine
