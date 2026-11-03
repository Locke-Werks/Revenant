// What flows to one phone without being asked for each time: spectrum rows,
// Opus audio, the HLS segmenter behind "keep listening", decoded messages,
// transcripts, and the events that say a receiver or the source changed.
//
// PhoneStreams sits between a PhoneSession (the engine side) and a
// PhoneSocket (the WebSocket side). Commands go in through on_text, which
// dispatches them and then brings the engine subscriptions into line with
// what the session now records.
//
// THREADING
//
// Two kinds of thread touch this object.
//
//   The strand. Every member function except deliver_spectrum and
//   deliver_audio runs on it, and so does every PhoneSocket call and every
//   HlsRegistry or HlsSegmenter call. In the gateway the strand is the kj
//   event loop thread (see KjStrand); in tests it is the test thread pumping
//   a manual queue.
//
//   The rpc::Client's own event loop thread, where the engine's callbacks
//   arrive. Those callbacks never touch this object. They copy what they were
//   given into an Inbox under its lock and post one wake-up to the strand,
//   latched so a burst of frames costs one post. The Inbox outlives this
//   object (the callbacks hold it by shared_ptr) and is closed by the
//   destructor, after which a late callback posts nothing.
//
// Engine calls (dispatch, subscribe, info) are made from the strand and
// block it for a loopback round trip. That stalls every phone's sends for
// that long, which at loopback latency is accepted rather than worth a
// thread per phone.
//
// BACKPRESSURE. One message is handed to the socket at a time. Queued behind
// it, in this order of priority:
//   JSON replies and events: never dropped.
//   Opus packets: at most max_queued_opus; past that the oldest is dropped
//     and counted, and the sequence number shows the gap to the phone.
//   Spectrum: one pending frame, kept as the engine's raw bins. A newer frame
//     replaces it and the replaced one is counted as skipped. It is reduced
//     when it is actually sent, with the view in force at that moment, so a
//     setView that lands while a frame waits is honoured by that frame.

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/rpc/types.h"
#include "tools/webgate/audio_opus.h"
#include "tools/webgate/hls_playlist.h"
#include "tools/webgate/session.h"

namespace revenant::webgate {

// Runs tasks one at a time on a single thread. post() is callable from any
// thread; the tasks run later, never inside post().
class Strand {
public:
    virtual ~Strand() = default;
    virtual void post(std::function<void()> task) = 0;
};

struct OutgoingMessage {
    enum class Kind : std::uint8_t { Text, Spectrum, Opus };
    Kind kind = Kind::Text;
    std::string text;                  // Kind::Text
    std::vector<std::uint8_t> binary;  // the other two
};

// The phone's WebSocket, as the stream layer sees it. send() is only called
// again after the previous message's done has run. done runs on the strand,
// possibly before send() returns; it also runs when the send failed, since the
// socket being gone is reported by the receive side, not here.
class PhoneSocket {
public:
    virtual ~PhoneSocket() = default;
    virtual void send(OutgoingMessage message, std::function<void()> done) = 0;
};

struct StreamsConfig {
    // Where /hls/<vrx>/ finds this phone's segmenters. Null disables keep
    // listening (the reply still succeeds and no hls event is sent).
    HlsRegistry* hls = nullptr;

    // Empty means std::chrono::steady_clock::now. Tests drive the HLS linger
    // with their own.
    std::function<std::chrono::steady_clock::time_point()> clock;

    // How long a segmenter outlives keepListening going off, so a quick lock
    // and unlock does not rebuild the stream.
    std::chrono::milliseconds hls_linger{30'000};

    // 25 packets is half a second of 20 ms frames: enough to ride out a short
    // stall, short enough that what plays after it is still live.
    std::size_t max_queued_opus = 25;

    double target_fps = 20.0;
};

struct StreamCounters {
    std::uint64_t spectrum_sent = 0;
    std::uint64_t spectrum_skipped = 0;
    std::uint64_t opus_sent = 0;
    std::uint64_t opus_dropped = 0;
};

class PhoneStreams {
public:
    PhoneStreams(PhoneSession& session, PhoneSocket& socket, Strand& strand,
                 StreamsConfig config = {});

    // Ends every subscription and removes this phone's segmenters from the
    // registry. Does not send anything.
    ~PhoneStreams();

    PhoneStreams(const PhoneStreams&) = delete;
    PhoneStreams& operator=(const PhoneStreams&) = delete;
    PhoneStreams(PhoneStreams&&) = delete;
    PhoneStreams& operator=(PhoneStreams&&) = delete;

    // Subscribes to receiver events and transcripts, reads the source, and
    // sends the first "source" event. Spectrum starts once the source is open.
    void start();

    // One text frame from the phone. The reply is queued before any event
    // the command caused.
    void on_text(std::string_view text);

    // Periodic work: notices a source opened, closed or retuned by someone
    // else, and ends segmenters whose linger has run out. The gateway calls
    // it a few times a second; tests call it after moving their clock.
    void tick();

    // A finished JSON event from outside, such as the engine link's state.
    void send_event(std::string json);

    // Any thread. The rpc callbacks call these; tests call them directly to
    // feed synthetic frames through the same path.
    void deliver_spectrum(const rpc::SpectrumFrame& frame);
    void deliver_audio(std::uint64_t vrx, const rpc::AudioChunk& chunk);

    [[nodiscard]] StreamCounters counters() const { return counters_; }
    [[nodiscard]] std::uint32_t every_nth() const { return every_nth_; }

    // The segmenter serving this vrx, null when there is none.
    [[nodiscard]] HlsSegmenter* segmenter(std::uint64_t vrx) const;

private:
    struct Inbox;
    struct Hls {
        std::unique_ptr<HlsSegmenter> segmenter;
        std::optional<std::chrono::steady_clock::time_point> ends_at;
    };
    struct AudioPath {
        std::unique_ptr<LiveOpusEncoder> opus;
        std::uint32_t next_seq = 0;
        bool reported_error = false;
    };

    // The callbacks' half: copy into the inbox and post one wake-up. Static
    // because a callback must not reach this object, only the inbox.
    static void put_spectrum(const std::shared_ptr<Inbox>& inbox, const rpc::SpectrumFrame& frame);
    static void put_audio(const std::shared_ptr<Inbox>& inbox, std::uint64_t vrx,
                          const rpc::AudioChunk& chunk);
    static void wake_locked(const std::shared_ptr<Inbox>& inbox);
    static void put_item(const std::shared_ptr<Inbox>& inbox,
                         std::function<void(PhoneStreams&)> item);

    [[nodiscard]] std::chrono::steady_clock::time_point now() const;

    void drain_inbox();
    void pump();
    void queue_text(std::string json);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> cut_spectrum(
        const rpc::SpectrumFrame& frame) const;

    void refresh_source();
    void subscribe_spectrum(std::uint32_t every_nth);
    void settle_every_nth(const rpc::SpectrumFrame& frame);
    void stop_spectrum();
    void reconcile();
    void reconcile_hls();
    void reconcile_audio();
    void reconcile_decoded();
    void handle_audio(std::uint64_t vrx, const rpc::AudioChunk& chunk);
    void handle_vrx_event(const rpc::VrxEvent& event);
    void drop_hls(std::uint64_t vrx);
    void subscribe_vrx_events();

    PhoneSession& session_;
    PhoneSocket& socket_;
    Strand& strand_;
    StreamsConfig config_;
    std::shared_ptr<Inbox> inbox_;
    std::shared_ptr<bool> alive_;

    // Send side.
    bool in_flight_ = false;
    bool pumping_ = false;
    std::deque<std::string> texts_;
    std::deque<std::vector<std::uint8_t>> opus_;
    std::optional<rpc::SpectrumFrame> pending_spectrum_;
    StreamCounters counters_;

    // Source as last read from info().
    bool source_open_ = false;
    bool source_known_ = false;
    std::uint32_t source_rate_ = 0;
    std::int64_t source_center_ = 0;
    std::uint64_t source_epoch_ = 0;
    bool spectrum_enabled_ = false;
    std::optional<std::chrono::steady_clock::time_point> last_refresh_;

    // Spectrum subscription.
    bool spectrum_subscribed_ = false;
    std::uint32_t every_nth_ = 0;
    bool every_nth_checked_ = false;
    struct FrameMark {
        std::uint64_t sequence = 0;
        std::uint64_t start = 0;
    };
    std::optional<FrameMark> first_seen_;

    // Receivers this phone has owned and not yet reported gone.
    std::set<std::uint64_t> known_owned_;

    std::set<std::uint64_t> audio_subscribed_;
    std::map<std::uint64_t, AudioPath> audio_;
    std::map<std::uint64_t, Hls> hls_;
    std::optional<std::uint64_t> announced_hls_;
    std::set<std::uint64_t> decoded_tried_;
    bool vrx_events_resubscribed_ = false;
};

}  // namespace revenant::webgate
