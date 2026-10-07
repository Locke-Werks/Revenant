// Who is talking: digital voice calls, folded from the decoded stream of every
// digital voice receiver into one row per transmission and one row per
// talkgroup or station.
//
// Pure C++ and core/rpc/types.h, so ui/tests/test_call_log.cpp asserts every
// rule here without Qt. models/call_model.{h,cpp} puts the two lists in front
// of a view; models/decoded_link.cpp feeds them.
//
// ONE ROW SHAPE FOR FOUR PROTOCOLS. call_frames() is the adapter: it reads the
// field keys core/rpc/decoders.h documents beside each decoder and says, for
// each message, whether a call started or carried on, ended, or was granted
// on a control channel. CallLog never looks at a decoder's field names.
//
// WHEN A CALL STARTS AND ENDS.
//   P25    hdu, ldu1 and ldu2 start or refresh; tdu and tdulc end. A tsbk
//          group voice grant opens a "granted" row that a voice call on the
//          same talkgroup takes over, so a grant no receiver followed stays
//          visible as granted and not followed.
//   DMR    voice_header, embedded_lc and pi_header start or refresh,
//          terminator ends, per timeslot: the two slots of one channel are
//          two calls at once.
//   D-STAR a header always starts a new call, keyed on MY and UR; a
//          superframe refreshes, and one that closes the transmission ends it.
//   M17    lsf starts or refreshes; stream_end and eot end.
// And in all four, silence: no frame for kCallTimeoutMs ends the call, since
// a terminator lost to fading is the ordinary case and not the exception.
//
// A refresh whose talkgroup or source differs from the call it would refresh
// is a new call. Two transmissions back to back with the end of the first
// lost would otherwise be one row naming the wrong talker.
//
// ENCRYPTED MARKS THE ROW AND NOTHING ELSE. Nothing here, or anywhere in the
// client, decrypts or tries to; docs/modes.md.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/rpc/types.h"

namespace revenant::ui {

inline constexpr std::size_t kCallLogCapacity = 500;
inline constexpr std::size_t kCallGroupCapacity = 500;

// About three superframes of any of the four. Long enough to ride out a
// fade, short enough that the row stops reading as live before the next
// talker's header arrives.
inline constexpr std::int64_t kCallTimeoutMs = 1500;

// A control channel repeats an active grant as updates about once a second,
// so a grant is live for a little over two of those.
inline constexpr std::int64_t kGrantTimeoutMs = 3000;

// One message, normalised.
struct CallFrame {
    enum class Event : std::uint8_t {
        Voice,  // a call started or carried on
        End,    // the call on this key ended, after taking this frame's fields
        Grant,  // a control channel granted a talkgroup a voice channel
    };
    Event event = Event::Voice;

    std::string protocol;  // "P25", "DMR", "D-STAR", "M17"
    std::uint64_t vrx = 0;
    int slot = 0;  // the DMR timeslot, 0 elsewhere

    // NAC, colour code, RPT1. Empty when this frame does not say.
    std::string system;

    // Talkgroup, UR or destination, and source unit or MY. Empty when this
    // frame does not say, which keeps what an earlier frame said.
    std::string target;
    std::string source;
    bool group = true;

    // D-STAR's header: a new transmission whatever the last one said.
    bool fresh = false;

    std::optional<bool> encrypted;
    bool emergency = false;

    std::int64_t frequency_hz = 0;
    std::string owner;
    std::int64_t at_ms = 0;
};

struct CallRow {
    std::uint64_t serial = 0;
    std::string protocol;
    std::string system;
    std::string target;
    std::string source;
    bool group = true;
    bool encrypted = false;
    bool emergency = false;
    std::int64_t frequency_hz = 0;
    std::uint64_t vrx = 0;
    int slot = 0;
    std::string owner;
    std::int64_t first_ms = 0;
    std::int64_t last_ms = 0;
    bool active = false;

    // Opened by a grant and never taken over by a voice call.
    bool granted = false;

    // Frames heard on a call row; calls on a group row.
    std::uint64_t count = 0;

    // Which group row this call counts towards, 0 for none yet.
    std::uint64_t group_serial = 0;
};

// What a call or a group is filed under in the groups view: the talkgroup
// for a group call on P25 and DMR, the calling station otherwise.
[[nodiscard]] inline std::string call_group_key(const CallRow& row)
{
    const bool by_group = row.group && (row.protocol == "P25" || row.protocol == "DMR");
    const std::string& who = by_group ? row.target : row.source;
    if (who.empty()) {
        return {};
    }
    return row.protocol + '\x1f' + row.system + '\x1f' + (by_group ? "g" : "s") + who;
}

class CallLog {
public:
    // Newest first.
    [[nodiscard]] const std::deque<CallRow>& calls() const { return calls_; }

    // Newest first by first heard. Each row's target is the talkgroup and
    // its source empty, or its source the station and its target empty.
    [[nodiscard]] const std::deque<CallRow>& groups() const { return groups_; }

    [[nodiscard]] bool any_active() const
    {
        return std::ranges::any_of(calls_, [](const CallRow& row) { return row.active; });
    }

    void clear()
    {
        calls_.clear();
        groups_.clear();
    }

    void apply(const CallFrame& frame)
    {
        expire(frame.at_ms);
        switch (frame.event) {
            case CallFrame::Event::Grant: grant(frame); break;
            case CallFrame::Event::Voice: voice(frame); break;
            case CallFrame::Event::End:
                if (CallRow* row = live(frame)) {
                    absorb(*row, frame);
                    finish(*row);
                }
                break;
        }
    }

    // Ends every call silent for longer than its timeout. Returns whether
    // anything changed.
    bool expire(std::int64_t now_ms)
    {
        bool changed = false;
        for (CallRow& row : calls_) {
            if (!row.active) {
                continue;
            }
            const std::int64_t limit = row.granted ? kGrantTimeoutMs : kCallTimeoutMs;
            if (now_ms - row.last_ms > limit) {
                finish(row);
                changed = true;
            }
        }
        return changed;
    }

private:
    [[nodiscard]] static bool same_key(const CallRow& row, const CallFrame& frame)
    {
        return !row.granted && row.vrx == frame.vrx && row.protocol == frame.protocol &&
               row.slot == frame.slot;
    }

    [[nodiscard]] CallRow* live(const CallFrame& frame)
    {
        for (CallRow& row : calls_) {
            if (row.active && same_key(row, frame)) {
                return &row;
            }
        }
        return nullptr;
    }

    [[nodiscard]] static bool differs(const std::string& known, const std::string& said)
    {
        return !known.empty() && !said.empty() && known != said;
    }

    static void absorb(CallRow& row, const CallFrame& frame)
    {
        if (!frame.system.empty()) {
            row.system = frame.system;
        }
        if (!frame.target.empty()) {
            row.target = frame.target;
            row.group = frame.group;
        }
        if (!frame.source.empty()) {
            row.source = frame.source;
        }
        // Sticky for the call: an LDU2 that says clear does not unsay an
        // encrypted header.
        if (frame.encrypted.value_or(false)) {
            row.encrypted = true;
        }
        row.emergency = row.emergency || frame.emergency;
        if (frame.frequency_hz != 0) {
            row.frequency_hz = frame.frequency_hz;
        }
        if (!frame.owner.empty()) {
            row.owner = frame.owner;
        }
        row.last_ms = std::max(row.last_ms, frame.at_ms);
        ++row.count;
    }

    void voice(const CallFrame& frame)
    {
        if (CallRow* row = live(frame)) {
            if (!frame.fresh && !differs(row->target, frame.target) &&
                !differs(row->source, frame.source)) {
                absorb(*row, frame);
                file(*row);
                return;
            }
            finish(*row);
        }

        // A grant for this talkgroup that nothing has taken yet becomes this
        // call, so the grant and the call it led to are one row.
        if (frame.protocol == "P25" && !frame.target.empty()) {
            for (CallRow& row : calls_) {
                if (row.active && row.granted && row.target == frame.target &&
                    (frame.system.empty() || row.system.empty() || row.system == frame.system)) {
                    const std::int64_t granted_hz = row.frequency_hz;
                    row.granted = false;
                    row.vrx = frame.vrx;
                    row.slot = frame.slot;
                    row.owner.clear();
                    row.count = 0;
                    absorb(row, frame);
                    if (row.frequency_hz == 0) {
                        row.frequency_hz = granted_hz;
                    }
                    file(row);
                    return;
                }
            }
        }

        CallRow row;
        row.serial = next_serial_++;
        row.protocol = frame.protocol;
        row.vrx = frame.vrx;
        row.slot = frame.slot;
        row.first_ms = frame.at_ms;
        row.last_ms = frame.at_ms;
        row.active = true;
        absorb(row, frame);
        push(std::move(row));
    }

    void grant(const CallFrame& frame)
    {
        // Followed already: the voice call is the row, and says more.
        for (CallRow& row : calls_) {
            if (row.active && !row.granted && row.protocol == frame.protocol &&
                row.target == frame.target &&
                (row.system.empty() || frame.system.empty() || row.system == frame.system)) {
                return;
            }
        }
        for (CallRow& row : calls_) {
            if (row.active && row.granted && row.target == frame.target &&
                row.system == frame.system) {
                absorb(row, frame);
                file(row);
                return;
            }
        }
        CallRow row;
        row.serial = next_serial_++;
        row.protocol = frame.protocol;
        row.vrx = frame.vrx;
        row.first_ms = frame.at_ms;
        row.last_ms = frame.at_ms;
        row.active = true;
        row.granted = true;
        absorb(row, frame);
        push(std::move(row));
    }

    void push(CallRow row)
    {
        calls_.push_front(std::move(row));
        file(calls_.front());
        while (calls_.size() > kCallLogCapacity) {
            calls_.pop_back();
        }
    }

    void finish(CallRow& row)
    {
        row.active = false;
        if (CallRow* group = group_of(row.group_serial)) {
            group->last_ms = std::max(group->last_ms, row.last_ms);
            group->active = std::ranges::any_of(calls_, [&](const CallRow& other) {
                return other.active && other.group_serial == group->serial;
            });
        }
    }

    [[nodiscard]] CallRow* group_of(std::uint64_t serial)
    {
        if (serial == 0) {
            return nullptr;
        }
        for (CallRow& group : groups_) {
            if (group.serial == serial) {
                return &group;
            }
        }
        return nullptr;
    }

    // Counts the call towards its talkgroup or station, the first time it
    // can be told which, and carries what it says onto that row.
    void file(CallRow& row)
    {
        CallRow* group = group_of(row.group_serial);
        if (group == nullptr) {
            const std::string key = call_group_key(row);
            if (key.empty()) {
                return;
            }
            for (CallRow& known : groups_) {
                if (call_group_key(known) == key) {
                    group = &known;
                    break;
                }
            }
            if (group == nullptr) {
                CallRow made;
                made.serial = next_serial_++;
                made.protocol = row.protocol;
                made.system = row.system;
                made.group = row.group && (row.protocol == "P25" || row.protocol == "DMR");
                if (made.group) {
                    made.target = row.target;
                } else {
                    made.source = row.source;
                }
                made.first_ms = row.first_ms;
                groups_.push_front(std::move(made));
                group = &groups_.front();
                while (groups_.size() > kCallGroupCapacity) {
                    groups_.pop_back();
                }
            }
            row.group_serial = group->serial;
            ++group->count;
        }
        group->encrypted = row.encrypted;
        group->emergency = row.emergency;
        group->granted = row.granted;
        group->frequency_hz = row.frequency_hz;
        group->vrx = row.vrx;
        group->slot = row.slot;
        group->owner = row.owner;
        group->last_ms = std::max(group->last_ms, row.last_ms);
        group->active = group->active || row.active;
        if (group->group && !row.source.empty()) {
            // The last unit heard on the talkgroup, which is the question a
            // groups view is asked.
            group->source = row.source;
        }
        if (!group->group && !row.target.empty()) {
            group->target = row.target;
        }
    }

    std::deque<CallRow> calls_;
    std::deque<CallRow> groups_;
    std::uint64_t next_serial_ = 1;
};

// ---------------------------------------------------------------------------
// The adapter
// ---------------------------------------------------------------------------

// Whether a decoder is one call_frames reads, so a feed can skip the rest
// without copying them.
[[nodiscard]] inline bool is_call_decoder(std::string_view decoder)
{
    return decoder == "p25p1" || decoder == "dmr" || decoder == "dstar" || decoder == "m17";
}

namespace call_detail {

[[nodiscard]] inline std::optional<std::int64_t> integer_of(const rpc::DecodedMessage& message,
                                                            std::string_view key)
{
    const rpc::DecodedField* field = message.field(key);
    if (field == nullptr || field->integer() == nullptr) {
        return std::nullopt;
    }
    return *field->integer();
}

[[nodiscard]] inline std::optional<bool> flag_of(const rpc::DecodedMessage& message,
                                                 std::string_view key)
{
    const rpc::DecodedField* field = message.field(key);
    if (field == nullptr || field->flag() == nullptr) {
        return std::nullopt;
    }
    return *field->flag();
}

[[nodiscard]] inline std::string text_of(const rpc::DecodedMessage& message, std::string_view key)
{
    const rpc::DecodedField* field = message.field(key);
    if (field == nullptr || field->text() == nullptr) {
        return {};
    }
    // Padding the decoder left, and anything that would break a row onto two.
    std::string out;
    for (const char c : *field->text()) {
        out.push_back(c == '\n' || c == '\r' || c == '\t' ? ' ' : c);
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

[[nodiscard]] inline std::string number_of(const rpc::DecodedMessage& message,
                                           std::string_view key)
{
    const auto value = integer_of(message, key);
    return value ? std::to_string(*value) : std::string{};
}

}  // namespace call_detail

// What a message says about calls: none, one, or two for a grant update
// naming two talkgroups. `frequency_hz` and `owner` are the receiver's, which
// the message does not carry; `at_ms` is when it arrived.
[[nodiscard]] inline std::vector<CallFrame> call_frames(const rpc::DecodedMessage& message,
                                                        std::int64_t at_ms,
                                                        std::int64_t frequency_hz,
                                                        std::string_view owner)
{
    using namespace call_detail;
    using Event = CallFrame::Event;

    std::vector<CallFrame> out;
    CallFrame frame;
    frame.vrx = message.vrx;
    frame.at_ms = at_ms;
    frame.frequency_hz = frequency_hz;
    frame.owner = std::string(owner);
    frame.encrypted = flag_of(message, "encrypted");
    frame.emergency = flag_of(message, "emergency").value_or(false);

    const std::string& kind = message.kind;
    if (message.decoder == "p25p1") {
        frame.protocol = "P25";
        if (const auto nac = integer_of(message, "nac")) {
            frame.system = std::format("NAC {:03X}", *nac);
        }
        if (kind == "tsbk") {
            // Only the group voice grants carry `group` as an integer.
            if (!integer_of(message, "group")) {
                return out;
            }
            frame.event = Event::Grant;
            frame.vrx = message.vrx;
            frame.owner.clear();
            frame.encrypted = flag_of(message, "encrypted_call");
            frame.target = number_of(message, "group");
            frame.source = number_of(message, "source");
            frame.frequency_hz = integer_of(message, "channel_frequency_hz").value_or(0);
            out.push_back(frame);
            if (const auto second = integer_of(message, "group_2")) {
                CallFrame other = frame;
                other.target = std::to_string(*second);
                other.source.clear();
                other.frequency_hz = integer_of(message, "channel_2_frequency_hz").value_or(0);
                out.push_back(std::move(other));
            }
            return out;
        }
        if (kind == "hdu" || kind == "ldu1" || kind == "ldu2") {
            frame.event = Event::Voice;
            frame.target = number_of(message, "talkgroup");
            if (frame.target.empty()) {
                if (const auto destination = integer_of(message, "destination")) {
                    frame.target = std::to_string(*destination);
                    frame.group = false;
                }
            }
            frame.source = number_of(message, "source");
        } else if (kind == "tdu" || kind == "tdulc") {
            frame.event = Event::End;
        } else {
            return out;
        }
        out.push_back(std::move(frame));
        return out;
    }

    if (message.decoder == "dmr") {
        frame.protocol = "DMR";
        frame.slot = static_cast<int>(integer_of(message, "slot").value_or(0));
        if (const auto cc = integer_of(message, "colour_code")) {
            frame.system = frame.slot > 0 ? std::format("CC {} TS {}", *cc, frame.slot)
                                          : std::format("CC {}", *cc);
        }
        if (kind == "voice_header" || kind == "embedded_lc" || kind == "pi_header") {
            frame.event = Event::Voice;
        } else if (kind == "terminator") {
            frame.event = Event::End;
        } else {
            return out;
        }
        if (kind == "pi_header") {
            frame.encrypted = true;
        }
        const auto group = flag_of(message, "group");
        if (group.value_or(true)) {
            frame.target = number_of(message, "talkgroup");
        } else {
            frame.target = number_of(message, "destination");
            frame.group = false;
        }
        frame.source = number_of(message, "source");
        if (flag_of(message, "privacy").value_or(false)) {
            frame.encrypted = true;
        }
        out.push_back(std::move(frame));
        return out;
    }

    if (message.decoder == "dstar") {
        frame.protocol = "D-STAR";
        frame.system = text_of(message, "rpt1");
        frame.target = text_of(message, "ur");
        frame.source = text_of(message, "my");
        // MY alone and not MY/suffix: a superframe repeats MY without the
        // suffix, and the two must read as the same talker.
        frame.group = false;
        if (kind == "header") {
            frame.fresh = true;
        } else if (kind != "superframe") {
            return out;
        }
        const bool closed = flag_of(message, "ended").value_or(false) ||
                            flag_of(message, "flushed").value_or(false) ||
                            integer_of(message, "voice_frames").value_or(21) < 21;
        frame.event = closed ? Event::End : Event::Voice;
        if (closed && frame.fresh) {
            // A transmission that opened and closed inside one piece: the
            // call is started here and ended at once.
            CallFrame end = frame;
            end.fresh = false;
            frame.event = Event::Voice;
            out.push_back(std::move(frame));
            out.push_back(std::move(end));
            return out;
        }
        out.push_back(std::move(frame));
        return out;
    }

    if (message.decoder == "m17") {
        frame.protocol = "M17";
        frame.group = false;
        if (kind == "lsf") {
            frame.event = Event::Voice;
            frame.target = text_of(message, "destination");
            frame.source = text_of(message, "source");
        } else if (kind == "stream_end" || kind == "eot") {
            frame.event = Event::End;
        } else {
            return out;
        }
        out.push_back(std::move(frame));
        return out;
    }
    return out;
}

}  // namespace revenant::ui
