// The decode section of the receiver window: which decoders a receiver is
// offered, what one line of the log says, and how many lines are kept.
//
// Qt-free so ui/tests can hold it, on the rule models/rds_view.h follows. It
// includes core/rpc/types.h and nothing else of the tree. models/decoded_link.cpp
// is the wire half and models/decoded_model.cpp puts this in front of a
// ListView.
//
// WHAT THE ENGINE OFFERS AND WHAT THIS WINDOW OFFERS
//
// Session.decoders lists what the engine can attach, and since 2026-09-23 each
// row carries the receiver modes it reads. The window offers exactly the rows
// that read the focused receiver's mode, in the engine's order, so a decoder
// added to the engine appears here without this file knowing its name.
//
// "auto" is this window's, and means what `revenant-cli --decode auto` means:
// the decoder named after the receiver's mode when there is one, and every
// audio decoder that reads the mode when there is not. The wire has no auto;
// an empty name to subscribeDecoded is the named-after half alone and is
// refused on a usb or nfm receiver, so the window resolves auto to names here
// and subscribes each one. A raw tap gets no auto, as the CLI's does not:
// p25p1, dstar, tetra and m17 all read one, and the operator is the one who
// knows which it carries.
//
// TIME IS WHERE THE MESSAGE ENDED IN THE RECEIVER'S OWN STREAM
//
// docs/conventions.md: the sample index is authoritative and wall clock is
// derived at the edges. A DecodedMessage carries [start_sample, end_sample)
// at sample_rate, counted from the start of the receiver's stream, and
// nothing on the wire anchors that count to a wall clock. So the time column
// is end_sample / sample_rate, which is the same reading for a live radio and
// a replayed capture, and the wall clock the line reached this window at is a
// field in the expansion, named as an arrival time rather than as when the
// message was sent.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "core/rpc/types.h"

namespace revenant::ui {

// ---------------------------------------------------------------------------
// Which decoders a receiver is offered
// ---------------------------------------------------------------------------

inline constexpr std::string_view kAutoDecoder = "auto";

// The four modes whose output is complex baseband, as the schema's
// DecoderInput names them. Only used for an engine older than
// DecoderInfo::modes, which cannot say more than the input kind.
[[nodiscard]] inline bool mode_is_complex_tap(std::string_view mode)
{
    return mode == "raw" || mode == "p25p1" || mode == "dstar" || mode == "tetra";
}

// Whether `info` reads a receiver in `mode`.
//
// AN EMPTY LIST IS AN ENGINE THAT DID NOT SAY, not a decoder that reads
// nothing, because convert.cpp always writes the whole list. Such an engine
// is answered by input kind alone, which is as far as it can be trusted; a
// pairing that is still wrong is refused by subscribeDecoded in words, and
// the refusal shows as a chip.
[[nodiscard]] inline bool decoder_reads(const rpc::DecoderInfo& info, std::string_view mode)
{
    if (!info.modes.empty()) {
        return std::ranges::find(info.modes, mode) != info.modes.end();
    }
    return (info.input == rpc::DecoderInput::ComplexBaseband) == mode_is_complex_tap(mode);
}

// What auto attaches on a receiver in `mode`, in the engine's order.
//
// A decoder that reads the mode only by the input-kind fallback above is not
// attached by auto: on an old engine that would put all nine audio decoders
// on a wfm receiver, which is the pairing the modes list exists to refuse.
[[nodiscard]] inline std::vector<std::string> auto_decoders(
    const std::vector<rpc::DecoderInfo>& infos, std::string_view mode)
{
    for (const rpc::DecoderInfo& info : infos) {
        if (info.name == mode && !info.modes.empty() && decoder_reads(info, mode)) {
            return {info.name};
        }
    }
    std::vector<std::string> out;
    for (const rpc::DecoderInfo& info : infos) {
        if (info.input == rpc::DecoderInput::RealAudio && !info.modes.empty() &&
            decoder_reads(info, mode)) {
            out.push_back(info.name);
        }
    }
    return out;
}

// The menu for a receiver in `mode`: auto first when it would attach
// anything, then every decoder that reads the mode. Empty means the section
// is not shown, which is am, dsb and wfm today.
[[nodiscard]] inline std::vector<std::string> decoder_choices(
    const std::vector<rpc::DecoderInfo>& infos, std::string_view mode)
{
    std::vector<std::string> out;
    if (!auto_decoders(infos, mode).empty()) {
        out.emplace_back(kAutoDecoder);
    }
    for (const rpc::DecoderInfo& info : infos) {
        if (decoder_reads(info, mode)) {
            out.push_back(info.name);
        }
    }
    return out;
}

// The choice in force: the operator's when this receiver offers it, and the
// head of the menu when it does not. A choice is sticky across receivers, so
// rtty picked on a usb receiver and then a change to nfm lands here, and the
// menu has to show what will actually run rather than a name it does not
// list. Empty when the menu is.
[[nodiscard]] inline std::string effective_decoder_choice(std::string_view choice,
                                                          const std::vector<std::string>& menu)
{
    if (menu.empty()) {
        return {};
    }
    if (std::ranges::find(menu, choice) != menu.end()) {
        return std::string(choice);
    }
    return menu.front();
}

// The decoder names a choice subscribes, on a receiver in `mode`.
[[nodiscard]] inline std::vector<std::string> resolve_decoder_choice(
    std::string_view choice, const std::vector<rpc::DecoderInfo>& infos, std::string_view mode)
{
    const std::string effective = effective_decoder_choice(choice, decoder_choices(infos, mode));
    if (effective.empty()) {
        return {};
    }
    if (effective == kAutoDecoder) {
        return auto_decoders(infos, mode);
    }
    return {effective};
}

// ---------------------------------------------------------------------------
// Streams the engine ended
// ---------------------------------------------------------------------------

// Which decoders' streams the engine has ended, on which receiver and why,
// so the reconcile leaves those off and keeps the rest running.
//
// PER DECODER, NOT PER RECEIVER. auto on a usb receiver is seven
// subscriptions, and one of them ending, a decoder that refused a chunk
// among the ways it can, says nothing about the other six. This used to be
// one receiver id, set by any ended(): the next pass then resolved nothing
// for that receiver and cancelled every decoder still running on it, and
// decoding stayed off until the switch went round.
//
// An ended stream is not tried again until the switch goes round, which is
// the operator asking again; a removed receiver then answers in the engine's
// words, as a refusal.
struct EndedDecoder {
    std::uint64_t vrx = 0;
    std::string decoder;
    std::string reason;
};

class EndedDecoders {
public:
    // Recorded once per decoder and receiver; a second ended() for the same
    // pair keeps the newer reason.
    void record(std::uint64_t vrx, std::string decoder, std::string reason)
    {
        for (EndedDecoder& known : ended_) {
            if (known.vrx == vrx && known.decoder == decoder) {
                known.reason = std::move(reason);
                return;
            }
        }
        ended_.push_back(EndedDecoder{vrx, std::move(decoder), std::move(reason)});
    }

    void clear() { ended_.clear(); }

    [[nodiscard]] bool has(std::uint64_t vrx, std::string_view decoder) const
    {
        return std::ranges::any_of(ended_, [&](const EndedDecoder& known) {
            return known.vrx == vrx && known.decoder == decoder;
        });
    }

    // `names` without the decoders whose stream ended on `vrx`, in order.
    [[nodiscard]] std::vector<std::string> still_wanted(std::uint64_t vrx,
                                                        std::vector<std::string> names) const
    {
        std::erase_if(names, [&](const std::string& name) { return has(vrx, name); });
        return names;
    }

    // The ones on `vrx`, in the order they ended, for the chip.
    [[nodiscard]] std::vector<EndedDecoder> on(std::uint64_t vrx) const
    {
        std::vector<EndedDecoder> out;
        for (const EndedDecoder& known : ended_) {
            if (known.vrx == vrx) {
                out.push_back(known);
            }
        }
        return out;
    }

private:
    std::vector<EndedDecoder> ended_;
};

// The chip for the streams that ended on the pane's receiver: its label,
// "rtty ended" for one and "2 ended" for more, and each decoder's reason in
// the engine's words. Both empty when none has.
struct EndedChip {
    std::string label;
    std::string detail;
};

[[nodiscard]] inline EndedChip ended_chip(const std::vector<EndedDecoder>& ended)
{
    EndedChip out;
    if (ended.empty()) {
        return out;
    }
    out.label = ended.size() == 1 ? ended.front().decoder + " ended"
                                  : std::format("{} ended", ended.size());
    for (const EndedDecoder& gone : ended) {
        if (!out.detail.empty()) {
            out.detail += "\n\n";
        }
        out.detail += gone.decoder + ": " +
                      (gone.reason.empty() ? std::string("the engine ended the stream and gave "
                                                         "no reason")
                                           : gone.reason);
    }
    return out;
}

// ---------------------------------------------------------------------------
// One line of the log
// ---------------------------------------------------------------------------

// How many lines the log holds before it lets the oldest go. Two thousand is
// about an hour of RTTY at a line every two seconds, and small enough that a
// copy of the whole log is a paste rather than a file.
inline constexpr std::size_t kDecodedLogCapacity = 2000;

// The widest decoder name the column is sized for. "sitor_b" is seven; one
// more keeps a name added later from touching the text.
inline constexpr std::size_t kDecoderColumn = 8;

struct DecodedFieldText {
    std::string key;
    std::string value;

    friend bool operator==(const DecodedFieldText&, const DecodedFieldText&) = default;
};

struct DecodedLine {
    // Counted by the log and never reused, so a view can key a row on it
    // across evictions.
    std::uint64_t serial = 0;

    std::uint64_t vrx = 0;
    std::string time;
    std::string decoder;
    std::string kind;
    std::string text;

    // A quiet note beside the text, empty when there is none. Today only
    // "encrypted", with the talkgroup when the header carried one.
    std::string chip;

    // Everything the message carried, for the expansion: its kind, where in
    // the stream it completed, its sequence, then its own fields in order.
    std::vector<DecodedFieldText> fields;

    // Messages the engine's queue let go ahead of this one.
    std::uint64_t dropped_before = 0;

    // Shown open in the view. View state, and held here rather than in the
    // delegate because a ListView recycles delegates and would open a
    // different line when one scrolled into another's place.
    bool expanded = false;
};

// Seconds into the receiver's stream to a tenth, as hh:mm:ss.t, always ten
// characters until a receiver has run for a hundred hours. Integer arithmetic,
// so the column is exact at any index and a test compares the string.
// No rate is no time: "--:--:--.-", not zero.
[[nodiscard]] inline std::string stream_time_text(std::uint64_t sample, std::uint32_t rate)
{
    if (rate == 0) {
        return "--:--:--.-";
    }
    const std::uint64_t seconds = sample / rate;
    const std::uint64_t tenths = (sample % rate) * 10 / rate;
    return std::format("{:02}:{:02}:{:02}.{}", seconds / 3600, (seconds / 60) % 60,
                       seconds % 60, tenths);
}

// Text that came off the air, made safe for one line: a control character
// becomes a space, so a stray carriage return in a page cannot start a new
// row in the view or in a copy. Bytes at and above 0x80 pass, because the
// decoders emit UTF-8.
[[nodiscard]] inline std::string one_line(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            c = ' ';
        }
    }
    return out;
}

// The P25 identifiers the standard writes in hexadecimal, and the adapter's
// own text line writes that way, so the expansion agrees with the line above
// it. TIA-102.BAAA-A clause 8.5 for the NAC, TIA-102.BAAC for the rest.
[[nodiscard]] inline bool field_reads_as_hex(std::string_view key)
{
    return key == "nac" || key == "algorithm_id" || key == "key_id" ||
           key == "manufacturer_id";
}

[[nodiscard]] inline std::string field_value_text(std::string_view key,
                                                  const rpc::DecodedValue& value)
{
    return std::visit(
        [key](const auto& held) -> std::string {
            using Held = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<Held, std::int64_t>) {
                if (field_reads_as_hex(key) && held >= 0) {
                    return std::format("0x{:X} ({})", held, held);
                }
                return std::to_string(held);
            } else if constexpr (std::is_same_v<Held, double>) {
                return std::format("{:.6g}", held);
            } else if constexpr (std::is_same_v<Held, bool>) {
                return held ? "yes" : "no";
            } else if constexpr (std::is_same_v<Held, std::string>) {
                return one_line(held);
            } else {
                // Bytes are what a transmitter sent where the standard does
                // not promise ASCII, so they are shown as bytes and never
                // guessed at as text.
                if (held.empty()) {
                    return "(none)";
                }
                std::string out;
                for (std::size_t i = 0; i < held.size(); ++i) {
                    if (i > 0) {
                        out += ' ';
                    }
                    out += std::format("{:02X}", held[i]);
                }
                return out;
            }
        },
        value);
}

// "encrypted: talkgroup 1201" for a P25 header on an encrypted call, and
// "encrypted" for anything else that sets the flag, M17 among them. Empty for
// a message in clear or one that does not say.
//
// A CHIP AND NEVER AN ERROR. An encrypted call is the decoder working: it
// read the header and reports what the header says. Nothing here decrypts or
// tries to, per docs/modes.md, so the line says so quietly and moves on.
[[nodiscard]] inline std::string encrypted_chip(const rpc::DecodedMessage& message)
{
    const rpc::DecodedField* flag = message.field("encrypted");
    if (flag == nullptr || flag->flag() == nullptr || !*flag->flag()) {
        return {};
    }
    if (const rpc::DecodedField* group = message.field("talkgroup");
        group != nullptr && group->integer() != nullptr) {
        return std::format("encrypted: talkgroup {}", *group->integer());
    }
    return "encrypted";
}

// One message as a line of the log. `arrived` is the wall clock it reached
// this window at, formatted by the caller, or empty.
[[nodiscard]] inline DecodedLine make_decoded_line(const rpc::DecodedMessage& message,
                                                   std::uint64_t serial,
                                                   std::string_view arrived)
{
    DecodedLine line;
    line.serial = serial;
    line.vrx = message.vrx;
    line.time = stream_time_text(message.end_sample, message.sample_rate);
    line.decoder = one_line(message.decoder);
    line.kind = one_line(message.kind);

    // Every adapter writes a text line today. One that does not still gets
    // a row saying what it was, rather than an empty one.
    line.text = message.text.empty() ? line.kind : one_line(message.text);
    line.chip = encrypted_chip(message);
    line.dropped_before = message.dropped_before;

    line.fields.push_back({"kind", line.kind});
    line.fields.push_back({"receiver", std::to_string(message.vrx)});
    line.fields.push_back(
        {"samples", std::format("{} to {} at {} S/s", message.start_sample, message.end_sample,
                                message.sample_rate)});
    line.fields.push_back({"sequence", std::to_string(message.sequence)});
    if (!arrived.empty()) {
        line.fields.push_back({"arrived", std::string(arrived)});
    }
    for (const rpc::DecodedField& field : message.fields) {
        line.fields.push_back({one_line(field.key), field_value_text(field.key, field.value)});
    }
    return line;
}

// The line as it is copied: time, decoder padded to its column, then the
// chip in brackets when there is one, then the text.
[[nodiscard]] inline std::string decoded_line_text(const DecodedLine& line)
{
    std::string out = line.time;
    out += "  ";
    out += line.decoder;
    if (line.decoder.size() < kDecoderColumn) {
        out.append(kDecoderColumn - line.decoder.size(), ' ');
    }
    out += ' ';
    if (!line.chip.empty()) {
        out += '[';
        out += line.chip;
        out += "] ";
    }
    out += line.text;
    return out;
}

// One line with its fields under it, for copying a single line: the reader of
// a pasted page wants the RIC and the function as well as the text.
[[nodiscard]] inline std::string decoded_line_detail(const DecodedLine& line)
{
    std::string out = decoded_line_text(line);
    for (const DecodedFieldText& field : line.fields) {
        out += "\n    ";
        out += field.key;
        out += ": ";
        out += field.value;
    }
    return out;
}

// Whether a view scrolled to `content_y` is at the newest line, give or take
// a few pixels, so the log keeps following. A view that fits its content is
// always at the tail.
[[nodiscard]] inline bool view_at_tail(double content_y, double view_height,
                                       double content_height)
{
    constexpr double kSlackPx = 4.0;
    if (content_height <= view_height) {
        return true;
    }
    return content_y + view_height >= content_height - kSlackPx;
}

// ---------------------------------------------------------------------------
// The log
// ---------------------------------------------------------------------------

// Lines oldest first, capped, with a count of what the cap and the wire each
// cost.
//
// THE APPEND IS TWO STEPS so a Qt model can wrap it: a view must be told
// which rows are about to go before they go, and which are about to arrive
// before they arrive. plan_append says both, evict_front and push_batch do
// them.
class DecodedLog {
public:
    struct AppendPlan {
        // Lines at the head of an incoming batch that would be evicted by the
        // same batch's tail, so they are never added at all.
        std::size_t skip_incoming = 0;

        // Existing lines to let go first.
        std::size_t evict_front = 0;
    };

    explicit DecodedLog(std::size_t capacity = kDecodedLogCapacity)
        : capacity_(capacity == 0 ? 1 : capacity)
    {
    }

    [[nodiscard]] AppendPlan plan_append(std::size_t incoming) const
    {
        AppendPlan plan;
        plan.skip_incoming = incoming > capacity_ ? incoming - capacity_ : 0;
        const std::size_t added = incoming - plan.skip_incoming;
        const std::size_t total = lines_.size() + added;
        plan.evict_front = total > capacity_ ? total - capacity_ : 0;
        return plan;
    }

    void evict_front(std::size_t count)
    {
        const std::size_t n = std::min(count, lines_.size());
        lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(n));
        evicted_ += n;
    }

    // Adds a batch, skipping the first `skip` of it as plan_append said. The
    // skipped lines count as evicted: they arrived and the cap let them go.
    // Their wire losses still count, since those happened.
    void push_batch(std::vector<DecodedLine> batch, std::size_t skip)
    {
        for (std::size_t i = 0; i < batch.size(); ++i) {
            lost_on_wire_ += batch[i].dropped_before;
            if (i < skip) {
                ++evicted_;
                continue;
            }
            lines_.push_back(std::move(batch[i]));
        }
    }

    // Lines that arrived and were let go before they reached the log, because
    // the hand-off in front of it was full. Counted with the cap's, which is
    // the same loss one step earlier.
    void count_unkept(std::uint64_t count) { evicted_ += count; }

    // plan_append, evict_front and push_batch in one, for a caller with no
    // view to tell.
    void append(std::vector<DecodedLine> batch)
    {
        const AppendPlan plan = plan_append(batch.size());
        evict_front(plan.evict_front);
        push_batch(std::move(batch), plan.skip_incoming);
    }

    void clear()
    {
        lines_.clear();
        evicted_ = 0;
        lost_on_wire_ = 0;
    }

    [[nodiscard]] const std::deque<DecodedLine>& lines() const { return lines_; }
    [[nodiscard]] std::deque<DecodedLine>& lines() { return lines_; }
    [[nodiscard]] std::size_t size() const { return lines_.size(); }
    [[nodiscard]] std::size_t capacity() const { return capacity_; }
    [[nodiscard]] std::uint64_t evicted() const { return evicted_; }
    [[nodiscard]] std::uint64_t lost_on_wire() const { return lost_on_wire_; }

    // A word or two for the chip, empty when nothing has been lost.
    [[nodiscard]] std::string dropped_label() const
    {
        const std::uint64_t total = evicted_ + lost_on_wire_;
        if (total == 0) {
            return {};
        }
        return std::format("{} dropped", total);
    }

    // The chip's sentence. Two causes and two remedies, so both are named:
    // the cap is this window's and is the log working, and a wire loss is
    // this window falling behind the engine's queue, which is worth knowing.
    [[nodiscard]] std::string dropped_detail() const
    {
        std::string out;
        if (evicted_ > 0) {
            out += std::format(
                "{} older line{} left the log, which keeps the newest {}. Copy the log before "
                "clearing it to keep a longer record.",
                evicted_, evicted_ == 1 ? "" : "s", capacity_);
        }
        if (lost_on_wire_ > 0) {
            if (!out.empty()) {
                out += ' ';
            }
            out += std::format(
                "{} message{} never reached this window: the engine holds 256 per decoder for "
                "a client and let the oldest go when this one fell behind.",
                lost_on_wire_, lost_on_wire_ == 1 ? "" : "s");
        }
        return out;
    }

    // The whole log as text, one line each, oldest first, with a first line
    // saying what is missing from the top when anything is.
    [[nodiscard]] std::string copy_all() const
    {
        std::string out;
        if (evicted_ > 0) {
            out += std::format("({} older line{} not kept)\n", evicted_,
                               evicted_ == 1 ? "" : "s");
        }
        for (const DecodedLine& line : lines_) {
            out += decoded_line_text(line);
            out += '\n';
        }
        return out;
    }

private:
    std::size_t capacity_;
    std::deque<DecodedLine> lines_;
    std::uint64_t evicted_ = 0;
    std::uint64_t lost_on_wire_ = 0;
};

}  // namespace revenant::ui
