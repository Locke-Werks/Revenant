// Turns decoded messages into capture annotations that say who was on the air.
//
// One annotation per message would bloat the metas: a P25 control channel
// alone hands over several TSBKs a second. So each identifier a message
// carries opens a span keyed by (receiver, decoder, key, value), every later
// message carrying the same identifier extends it, and the span goes to the
// writer once the identifier has been quiet for `gap_samples`, its receiver
// retunes or goes away, or the capture stops.
//
// PURE AND CLOCK-FREE. Time is the source stream index the caller stamps on
// each sighting, so the gap is measured in samples and a test drives it with
// plain numbers. Nothing here locks; the caller serialises calls.
//
// WHAT IS INDEXED. indexed() is an allowlist of the identifier keys each
// decoder already publishes (core/rpc/decoders.h). Free text, measurements
// and counters are never indexed: they are not identities, and a key whose
// value changes every message would open a span per message.
//
// PLACEMENT. A decode's own sample span is local to its receiver's stream, so
// the caller stamps end_index with the writer's source index at handover and
// gives the message's duration in source samples. The start is therefore
// right to within one decode chunk, which docs/capture.md states.

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "core/capture/format.h"
#include "core/dsp/types.h"

namespace revenant::capture {

class DecodeAnnotator {
public:
    struct Sighting {
        std::uint32_t vrx = 0;
        std::string decoder;
        std::vector<std::pair<std::string, std::string>> fields;
        // Absolute source stream index when the message was handed over.
        std::uint64_t end_index = 0;
        // The message's duration in source samples.
        std::uint64_t span_samples = 0;
        double freq_lower_hz = 0.0;
        double freq_upper_hz = 0.0;
    };

    using Emit = std::function<void(std::uint64_t absolute_start, const Annotation&)>;

    explicit DecodeAnnotator(std::uint64_t gap_samples, Emit emit)
        : gap_(gap_samples), emit_(std::move(emit))
    {
    }

    // Whether `key` on a `decoder` message is an identifier worth searching
    // by. The names are decoders.h's, unchanged.
    [[nodiscard]] static bool indexed(std::string_view decoder, std::string_view key)
    {
        if (decoder == "p25p1" || decoder == "dmr") {
            return key == "talkgroup" || key == "source" || key == "destination";
        }
        if (decoder == "ais") {
            return key == "mmsi" || key == "callsign";
        }
        if (decoder == "ax25" || decoder == "m17") {
            return key == "source";
        }
        if (decoder == "pocsag") {
            return key == "address";
        }
        if (decoder == "dstar") {
            // "my" is the transmitting station's own callsign.
            return key == "my";
        }
        if (decoder == "tetra") {
            // A sync burst carries no subscriber identity; the network's
            // country and network codes are the only identifiers it has.
            return key == "mcc" || key == "mnc";
        }
        return false;
    }

    // The name a key is filed under. A P25 control channel publishes its
    // talkgroups as "group" and "group_2" (the TSBK grants), the voice
    // channel as "talkgroup"; both are filed as talkgroup so one search
    // finds a talkgroup whichever channel heard it. Found in the live check
    // of 2026-10-18, where a control channel indexed no talkgroup at all.
    [[nodiscard]] static std::string_view filed_as(std::string_view decoder, std::string_view key)
    {
        if (decoder == "p25p1" && (key == "group" || key == "group_2")) {
            return "talkgroup";
        }
        return key;
    }

    void observe(const Sighting& sighting)
    {
        advance(sighting.end_index);
        const std::uint64_t start =
            sighting.end_index - std::min(sighting.span_samples, sighting.end_index);
        for (const auto& [raw_key, value] : sighting.fields) {
            const std::string_view key = filed_as(sighting.decoder, raw_key);
            if (value.empty() || !indexed(sighting.decoder, key)) {
                continue;
            }
            auto [it, opened] = open_.try_emplace(
                Key{sighting.vrx, sighting.decoder, std::string(key), value});
            Open& span = it->second;
            if (opened) {
                span.start = start;
                span.last_end = sighting.end_index;
                span.freq_lower_hz = sighting.freq_lower_hz;
                span.freq_upper_hz = sighting.freq_upper_hz;
            } else {
                span.start = std::min(span.start, start);
                span.last_end = std::max(span.last_end, sighting.end_index);
            }
            ++span.messages;
        }
    }

    // Closes every span quiet for longer than the gap as of `now_index`.
    void advance(std::uint64_t now_index)
    {
        close_if([&](const Key&, const Open& span) { return span.last_end + gap_ < now_index; });
    }

    // A retune or removal: what the receiver hears next is a different
    // channel, so nothing it had open can be extended by it.
    void close_vrx(std::uint32_t vrx)
    {
        close_if([&](const Key& key, const Open&) { return std::get<0>(key) == vrx; });
    }

    void finish()
    {
        close_if([](const Key&, const Open&) { return true; });
    }

    [[nodiscard]] std::size_t open_spans() const { return open_.size(); }
    [[nodiscard]] std::uint64_t sent() const { return sent_; }

private:
    using Key = std::tuple<std::uint32_t, std::string, std::string, std::string>;

    struct Open {
        std::uint64_t start = 0;
        std::uint64_t last_end = 0;
        std::uint32_t messages = 0;
        double freq_lower_hz = 0.0;
        double freq_upper_hz = 0.0;
    };

    // std::map so spans closing together go out in a fixed order.
    template <class Pred>
    void close_if(Pred pred)
    {
        for (auto it = open_.begin(); it != open_.end();) {
            if (pred(it->first, it->second)) {
                send(it->first, it->second);
                it = open_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void send(const Key& key, const Open& span)
    {
        const auto& [vrx, decoder, name, value] = key;
        Annotation out;
        out.sample_count = span.last_end - span.start;
        out.freq_lower = static_cast<dsp::Hertz>(span.freq_lower_hz);
        out.freq_upper = static_cast<dsp::Hertz>(span.freq_upper_hz);
        out.confidence = 1.0;
        out.decoder = decoder;
        out.decoded = {{name, value}};
        out.messages = span.messages;
        ++sent_;
        emit_(span.start, out);
    }

    std::uint64_t gap_;
    Emit emit_;
    std::map<Key, Open> open_;
    std::uint64_t sent_ = 0;
};

}  // namespace revenant::capture
