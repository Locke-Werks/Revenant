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
#include "tools/webgate/audio_rate.h"
#include "tools/webgate/desktop_settings.h"
#include "tools/webgate/hls_playlist.h"
#include "tools/webgate/phone_mix.h"
#include "tools/webgate/protocol.h"
#include "tools/webgate/display_sync.h"
#include "tools/webgate/dv_feed.h"
#include "tools/webgate/session.h"
#include "tools/webgate/spectrum_reducer.h"
#include "tools/webgate/voice_gain.h"

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
// possibly before send() returns. A failed send never runs done; the socket
// closes itself and the receive side reports the end.
//
// The socket, not the stream, owns whether a send is in flight. A session
// that resumes onto a socket the fresh session was still writing to must wait
// for that write: kj allows one send at a time and throws on a second.
class PhoneSocket {
public:
    virtual ~PhoneSocket() = default;

    void send(OutgoingMessage message, std::function<void()> done)
    {
        busy_ = true;
        do_send(std::move(message), [this, done = std::move(done)] {
            busy_ = false;
            auto idle = std::move(idle_);
            idle_ = nullptr;
            done();
            if (idle) {
                idle();
            }
        });
    }

    [[nodiscard]] bool busy() const { return busy_; }

    // Runs once, after the send in flight completes. Replaces an earlier one.
    void when_idle(std::function<void()> callback) { idle_ = std::move(callback); }

    // Ends the connection. The receive side then sees it end and detaches the
    // session, which is how an overflowing or broken socket is handled.
    virtual void close() = 0;

protected:
    // complete is called when the message has gone; never for a failed send.
    virtual void do_send(OutgoingMessage message, std::function<void()> complete) = 0;

private:
    bool busy_ = false;
    std::function<void()> idle_;
};

struct StreamsConfig {
    // Where /hls/<id>/ finds this phone's segmenter. Null disables keep
    // listening (the reply still succeeds and no hls event is sent).
    HlsRegistry* hls = nullptr;

    // Empty means std::chrono::steady_clock::now. Tests drive the HLS linger
    // with their own.
    std::function<std::chrono::steady_clock::time_point()> clock;

    // Where the speech status is read each poll. Empty reads the engine; tests
    // supply one, since a real engine's model state cannot be steered.
    std::function<Expected<rpc::TranscriptionStatus>()> transcription_status;

    // Where the detections are read each poll. Empty reads the engine with the
    // session's confidence bar; tests supply one, since the real detector's
    // tracks cannot be steered.
    std::function<Expected<rpc::DetectionList>(double, double)> detections;

    // How often detections are polled while the phone has a view set.
    std::chrono::milliseconds detection_poll{500};

    // Where a WFM receiver's RDS is read. Empty reads the engine; tests
    // supply one, since a station's groups cannot be steered.
    std::function<Expected<rpc::RdsStation>(std::uint64_t)> rds_station;
    std::function<Status(std::uint64_t, rpc::RdsRegion)> set_rds_region;

    // How often each WFM receiver's station is read, and how long a refusal
    // waits before the next try. The first read of a WFM receiver makes the
    // engine open a companion receiver at the composite rate, so a refusal
    // (no channel free for it, say) is not retried every second.
    std::chrono::milliseconds rds_poll{1000};
    std::chrono::milliseconds rds_retry{10'000};

    // Stands in for Client::subscribe_decoded, so a test can end a
    // subscription when it likes. Empty subscribes on the engine.
    std::function<Expected<std::string>(std::uint64_t, std::string_view,
                                        std::function<void(const rpc::DecodedMessage&)>,
                                        std::function<void(const std::string&)>)>
        subscribe_decoded;

    // Where the trunk follower's status is read. Empty reads the engine; tests
    // supply one, since a control channel's grants cannot be steered.
    std::function<Expected<rpc::TrunkStatusInfo>()> trunk_status;

    // How often trunk status is read while the DV tab is open or following
    // is on, and how long a refusal waits: an engine without the call would
    // only fill the log.
    std::chrono::milliseconds trunk_poll{1000};
    std::chrono::milliseconds trunk_retry{10'000};

    // Wall clock milliseconds since the epoch, for the call rows. Empty is
    // std::chrono::system_clock.
    std::function<std::int64_t()> wall_clock_ms;

    // How long a segmenter outlives keepListening going off, so a quick lock
    // and unlock does not rebuild the stream.
    std::chrono::milliseconds hls_linger{30'000};

    // 25 packets is half a second of 20 ms frames: enough to ride out a short
    // stall, short enough that what plays after it is still live.
    std::size_t max_queued_opus = 25;

    // The most spectrum rows a second this phone is sent; every row the
    // engine makes when it makes fewer. Zero or less is no cap.
    double max_fps = kDefaultMaxPhoneFps;

    // The desktop's display settings, read on every tick. Empty is the
    // desktop's defaults; the gateway reads the registry through a
    // DisplaySettingsSource, and tests supply their own.
    std::function<ui::DisplaySettings()> display_settings;

    // The desktop's digital voice boost in whole decibels, read per audio
    // chunk. Empty is zero, which leaves decoded voice at the engine's level.
    std::function<double()> voice_gain_db;

    // What the desktop has saved, read on every tick; see desktop_settings.h.
    // Empty is nothing saved: no engine switch is sent and the phone is told
    // nothing about the desktop.
    std::function<DesktopSettings()> desktop_settings;

    // Shared by every phone the hub makes, so the whole-frame half of the
    // zoomed view offsets is measured once a frame and not once a phone. Null
    // measures it per phone.
    std::shared_ptr<FrameLevelsCache> frame_levels;

    // JSON queued for a socket that is not taking it. Past either limit an
    // attached socket is closed (the phone reconnects and resumes); a
    // detached session drops its oldest events instead.
    std::size_t max_queued_text = 2000;
    std::size_t max_queued_text_bytes = 1024 * 1024;
};

struct StreamCounters {
    std::uint64_t spectrum_sent = 0;
    std::uint64_t spectrum_skipped = 0;
    std::uint64_t opus_sent = 0;
    std::uint64_t opus_dropped = 0;
    std::uint64_t text_dropped = 0;
};

// The most receivers one phone mix holds: the desktop's rack limit. Lead
// first, then digital voice, then analogue. The rack cannot hold more, so this
// bounds a gateway bug rather than being a rule anyone should hear.
inline constexpr std::size_t kMaxMixMembers = 64;

class PhoneStreams {
public:
    // socket may be null: a session can exist without a connection.
    PhoneStreams(PhoneSession& session, PhoneSocket* socket, Strand& strand,
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

    // The reply to a command already dispatched elsewhere (the hub runs the
    // slow ones on a worker; see dispatch.h runs_off_loop), and the refreshes
    // the command calls for, exactly as on_text would have done them.
    void on_reply(const Command& command, std::string reply);

    // Periodic work: notices a source opened, closed or retuned by someone
    // else, and ends segmenters whose linger has run out. The gateway calls
    // it a few times a second; tests call it after moving their clock.
    void tick();

    // A finished JSON event from outside, such as the engine link's state.
    void send_event(std::string json);

    // Moves the stream to another socket, or to none. While detached,
    // spectrum and Opus are skipped (a phone that is not there has no use for
    // stale ones), JSON is kept up to the cap, and HLS keeps encoding. A send
    // in flight on the old socket is forgotten: its done is ignored.
    void attach(PhoneSocket& socket);
    void detach();
    [[nodiscard]] bool attached() const { return socket_ != nullptr; }
    [[nodiscard]] bool spectrum_subscribed() const { return spectrum_subscribed_; }

    // Any thread. The rpc callbacks call these; tests call them directly to
    // feed synthetic frames through the same path.
    void deliver_spectrum(const rpc::SpectrumFrame& frame);
    void deliver_audio(std::uint64_t vrx, const rpc::AudioChunk& chunk);

    // For recreating the session on a restarted engine (phone_hub.h).
    // The centre the source had at the last read, when one was open: a
    // receiver's params are relative to it, and the new engine's may differ.
    [[nodiscard]] std::optional<std::int64_t> source_center() const
    {
        return source_known_ && source_open_ ? std::optional(source_center_) : std::nullopt;
    }
    // Takes the JSON not yet sent, so a replacement can send it first.
    [[nodiscard]] std::deque<std::string> take_texts();
    // Brings subscriptions into line with receivers the session already knows
    // without a command having added them.
    void resync();
    // Re-sends the current source, receiver and HLS events, for a socket that
    // resumed this session and has seen none of them.
    void announce_state();

    [[nodiscard]] StreamCounters counters() const { return counters_; }
    [[nodiscard]] std::uint32_t every_nth() const { return every_nth_; }

    // The segmenter behind /hls/<id>/, null when this session has none or the
    // id is another's. One per session: it encodes the mix, whichever
    // receiver the phone is listening to.
    [[nodiscard]] HlsSegmenter* segmenter(std::uint64_t id) const;
    // The id this session's segmenter is registered under, fixed for the life
    // of the object. Not a receiver id.
    [[nodiscard]] std::uint64_t hls_id() const { return hls_id_; }
    // The receivers the mix has a stream for, for tests.
    [[nodiscard]] std::set<std::uint64_t> audio_subscriptions() const { return audio_subscribed_; }
    // The decoders subscribed on a receiver right now, for tests.
    [[nodiscard]] std::set<std::string> decoding(std::uint64_t vrx) const;

private:
    struct Inbox;
    struct Hls {
        std::unique_ptr<HlsSegmenter> segmenter;
        std::optional<std::chrono::steady_clock::time_point> ends_at;
    };
    // One per receiver whose audio is subscribed. The resampler sits ahead of
    // the mix, so every stream enters it at 48 kHz and a receiver re-created
    // under a new id starts a fresh filter.
    struct AudioPath {
        PhoneRateConverter rate;
        bool reported_error = false;
    };

    // A receiver in the mix and whether its audio is decoded voice, which
    // takes the desktop's voice boost; analog enters at the engine's level.
    struct MixMember {
        std::uint64_t vrx = 0;
        bool digital_voice = false;
    };

    // WHO THE PHONE HEARS, in one place. Every receiver on the engine, analog
    // and digital voice, whoever opened it, with the selected one first as the
    // lead, while the phone's audio is on or keep listening has its stream up.
    // A different rule for who is heard changes this function and nothing
    // else: the mixer is told what it is given.
    [[nodiscard]] std::vector<MixMember> mix_members() const;
    // Mixed samples out to the Opus encoder and the HLS segmenter.
    void emit_mix(std::span<const float> mixed, const std::function<void(const std::string&)>& report);

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
        const rpc::SpectrumFrame& frame);

    void refresh_source();
    // Sends a transcriptionStatus event when the engine's speech state has
    // changed since the last one, and the first time it is read.
    void refresh_transcription();
    // Reads the detections and sends the ones near the view. Sends nothing on
    // a failed read, so a detector that is not ready does not reach the phone.
    void refresh_detections();
    void refresh_rds();
    // The DV tab (dv_feed.h): one decoded message into the call log, the
    // calls and talkgroups events when they changed, the trunk status poll,
    // one Auto DV pass over a detection list, and the session's switches put
    // back on the engine when a session starts.
    void fold_call(const rpc::DecodedMessage& message);
    void refresh_calls();
    void refresh_trunk();
    void run_auto_dv(const rpc::DetectionList& list);
    void apply_dv_switches();
    void refresh_trunk_settings();
    void send_dv_state();
    [[nodiscard]] std::int64_t wall_ms() const;
    // Lifts the plugin hold this session set and stops the following it
    // started. Run as the streams end, which is when a session ends.
    void release_dv_switches();
    static void dv_log(const std::string& line);
    // Takes the desktop's settings in and sends a display event when the
    // colours they give are not the ones last sent.
    void refresh_display();
    void send_display_if_changed();
    // The desktop event, sent when what it would say has changed.
    void refresh_desktop();
    // The engine-wide switches the desktop has written, sent the way the
    // desktop sends them on each connection. Once per session start.
    void apply_desktop_switches();
    [[nodiscard]] std::string source_event() const;
    void subscribe_spectrum(std::uint32_t every_nth);
    void settle_every_nth(const rpc::SpectrumFrame& frame);
    void stop_spectrum();
    void reconcile();
    void reconcile_hls();
    void reconcile_audio();
    void reconcile_decoded();
    [[nodiscard]] Status subscribe_decoder(std::uint64_t vrx, const std::string& name);
    void handle_audio(std::uint64_t vrx, const rpc::AudioChunk& chunk);
    void handle_vrx_event(const rpc::VrxEvent& event);
    void drop_hls();
    void subscribe_vrx_events();

    PhoneSession& session_;
    PhoneSocket* socket_;
    std::uint64_t socket_generation_ = 0;
    std::size_t queued_text_bytes_ = 0;
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
    double realtime_factor_ = 0.0;
    double paced_by_ = 0.0;
    // The factor in the last source event, so a small wander sends nothing.
    double sent_realtime_factor_ = 0.0;
    double sent_paced_by_ = 0.0;
    bool spectrum_enabled_ = false;
    // Where the engine's bins really lie, which is not the band: see
    // source_event.
    rpc::SpectrumGeometry spectrum_{};
    std::optional<std::chrono::steady_clock::time_point> last_refresh_;
    // The speech state at the last read, and at the last event sent.
    std::optional<rpc::TranscriptionStatus> transcription_;
    std::optional<rpc::TranscriptionStatus> sent_transcription_;
    std::optional<std::chrono::steady_clock::time_point> last_detection_poll_;

    // Per WFM receiver: when its station is next read, and the last rds
    // event sent for it, so only a change goes out.
    struct RdsPoll {
        std::chrono::steady_clock::time_point next{};
        std::string sent;
        bool region_set = false;
    };
    // The last detections event sent, so an unchanged list is not resent.
    std::string sent_detections_;
    std::map<std::uint64_t, RdsPoll> rds_;

    // Spectrum subscription.
    bool spectrum_subscribed_ = false;
    std::uint32_t every_nth_ = 0;
    bool every_nth_checked_ = false;
    struct FrameMark {
        std::uint64_t sequence = 0;
        std::uint64_t start = 0;
    };
    std::optional<FrameMark> first_seen_;

    // Receivers the phone has been told of and not yet told are gone.
    std::set<std::uint64_t> known_;
    // Each receiver as the engine last described it and who opened it, so a
    // resumed socket can be sent the whole list again.
    struct Described {
        rpc::VrxStatus status;
        std::string opened_by;
    };
    std::map<std::uint64_t, Described> described_;
    void send_receiver(std::uint64_t vrx);
    // The "vrxList" snapshot: every receiver the engine has now.
    void send_receiver_list();

    std::set<std::uint64_t> audio_subscribed_;
    std::map<std::uint64_t, AudioPath> audio_;
    // The one segmenter, present while keep listening is on and for the linger
    // after. Registered under hls_id_, which is unique per session so the
    // playlist URL does not change when the active receiver does.
    std::optional<Hls> hls_;
    std::uint64_t hls_id_ = 0;
    // The receiver named in the last hls event, so a move of the audio to
    // another receiver is announced and a repeat is not.
    std::optional<std::uint64_t> announced_hls_;

    // The sum of every mixed receiver, and the one Opus stream cut from it.
    // It is tagged kPhoneMixStream whichever receiver leads, so a new lead is
    // the same stream; the encoder and sequence start again when the phone's
    // audio is turned off (reconcile_audio).
    PhoneMixer mixer_;
    // The desktop's boost as of the last start() or tick().
    double voice_gain_db_ = 0.0;
    std::unique_ptr<LiveOpusEncoder> mix_opus_;
    std::uint32_t mix_opus_seq_ = 0;
    // Per receiver: the decoder set last applied, and which of it the engine
    // took. A set is applied once; a refusal is reported and not retried
    // until the phone changes the set or the receiver is recreated.
    std::map<std::uint64_t, std::vector<std::string>> decoded_applied_;
    std::map<std::uint64_t, std::set<std::string>> decoded_;
    // The generation of each live decoder subscription. An ended event still
    // queued from one the phone turned off and on again carries the old
    // generation, and must not take the new subscription out of decoded_.
    std::map<std::pair<std::uint64_t, std::string>, std::uint64_t> decoder_generation_;
    std::uint64_t next_decoder_generation_ = 0;
    bool vrx_events_resubscribed_ = false;

    // The DV tab.
    ui::CallLog calls_;
    bool calls_dirty_ = false;
    std::string sent_calls_;
    std::string sent_talkgroups_;
    std::string sent_trunk_;
    std::string sent_dv_;
    TrunkAliases aliases_;
    std::optional<std::string> aliases_text_;
    std::optional<std::chrono::steady_clock::time_point> next_trunk_poll_;

    // The desktop's scale state for this phone, and the display event it was
    // last sent. The view is the one the last row was cut for.
    PhoneDisplay display_;
    std::string sent_display_;
    std::string sent_desktop_;
    std::optional<View> display_view_;
    // A row's working columns, kept so a row allocates none of them.
    std::vector<float> shown_columns_;
    std::vector<float> waterfall_columns_;
    std::vector<float> trace_columns_;
};

}  // namespace revenant::webgate
