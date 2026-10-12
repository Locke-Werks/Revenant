// Turns the live detector's tracks into capture annotations as they end.
//
// The detector has no "track ended" event: a track is simply absent from the
// next decision's tracks(). So this keeps a copy of every track that has
// cleared the confidence bar, notices when one goes missing, and hands the
// last copy to the writer then. Whatever is still alive when the stream stops
// goes out in finish(). Cost per decision is one pass over tracks() and one
// over the held copies, at most DetectorConfig::max_tracks each, at the
// decision rate rather than the frame rate.
//
// WHAT THE FIELDS ARE. first_seen and last_seen are already absolute stream
// indices, which is what SegmentWriter::add_annotation keys on. The edges are
// the centre plus and minus half the bandwidth. peak_db is the highest
// snr_2500_db the track reached and snr_db the last one, both in the
// detector's 2500 Hz reference bandwidth, because a Track carries no absolute
// level. family is the verified protocol when there is one, otherwise the
// accepted classification, otherwise empty.

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <utility>

#include "core/capture/format.h"
#include "core/detect/detector.h"
#include "core/dsp/types.h"
#include "core/identify/identify.h"

namespace revenant::capture {

class TrackAnnotator {
public:
    using Emit = std::function<void(dsp::SampleIndex absolute_start, const Annotation&,
                                    std::uint64_t track_id)>;

    explicit TrackAnnotator(Emit emit) : emit_(std::move(emit)) {}

    // At each new detector decision, on the thread that owns the detector.
    void observe(std::span<const detect::Track> tracks, double confidence_bar)
    {
        ++generation_;
        for (const detect::Track& track : tracks) {
            auto found = held_.find(track.id);
            if (found == held_.end()) {
                if (track.confidence < confidence_bar) {
                    continue;
                }
                found = held_.emplace(track.id, Held{}).first;
                found->second.peak_db = track.snr_2500_db;
            }
            Held& held = found->second;
            held.track = track;
            held.peak_db = std::max(held.peak_db, track.snr_2500_db);
            held.confidence = std::max(held.confidence, track.confidence);
            held.generation = generation_;
        }
        for (auto it = held_.begin(); it != held_.end();) {
            if (it->second.generation != generation_) {
                send(it->second);
                it = held_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Every track still held, as though it had ended now.
    void finish()
    {
        for (const auto& [id, held] : held_) {
            send(held);
        }
        held_.clear();
    }

    [[nodiscard]] std::uint64_t sent() const { return sent_; }

private:
    struct Held {
        detect::Track track;
        double peak_db = 0.0;
        double confidence = 0.0;
        std::uint64_t generation = 0;
    };

    void send(const Held& held)
    {
        const detect::Track& track = held.track;
        Annotation out;
        out.sample_count = track.last_seen >= track.first_seen
                               ? track.last_seen - track.first_seen
                               : 0;
        out.freq_lower = track.center - track.bandwidth / 2;
        out.freq_upper = track.center + track.bandwidth / 2;
        out.confidence = held.confidence;
        out.peak_db = held.peak_db;
        out.snr_db = track.snr_2500_db;
        if (track.protocol != identify::Protocol::None) {
            out.family = identify::protocol_name(track.protocol);
        } else if (track.classification != detect::Classification::Unknown) {
            out.family = detect::classification_name(track.classification);
        }
        ++sent_;
        emit_(track.first_seen, out, track.id);
    }

    Emit emit_;
    std::unordered_map<std::uint64_t, Held> held_;
    std::uint64_t generation_ = 0;
    std::uint64_t sent_ = 0;
};

}  // namespace revenant::capture
