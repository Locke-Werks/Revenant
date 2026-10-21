// EngineLink's side of the burst timeline: the raw decoded feed and where a
// receiver is. The timeline itself is models/burst_timeline_model.cpp.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "models/engine_link.h"

namespace revenant::ui {

namespace {

// Enough for a busy trunk's minute of grants; the timeline drains it on every
// signal, so this only bounds a window with no timeline open.
constexpr std::size_t kBurstDecodedKept = 512;

}  // namespace

void EngineLink::note_burst_decoded(const std::vector<rpc::DecodedMessage>& messages,
                                    const std::vector<std::int64_t>& arrived_ms)
{
    if (messages.empty()) {
        return;
    }
    for (std::size_t i = 0; i < messages.size(); ++i) {
        BurstDecodedEntry entry;
        entry.serial = ++burst_decoded_serial_;
        entry.arrived_ms = i < arrived_ms.size() ? arrived_ms[i] : 0;
        entry.message = messages[i];
        burst_decoded_feed_.push_back(std::move(entry));
    }
    while (burst_decoded_feed_.size() > kBurstDecodedKept) {
        burst_decoded_feed_.pop_front();
    }
    emit burstDecodedArrived();
}

double EngineLink::burstReceiverHz(std::uint64_t engine_id) const
{
    if (engine_id == 0) {
        return 0.0;
    }
    for (const RackEntry& entry : rack_.entries()) {
        if (entry.engine_id != engine_id) {
            continue;
        }
        if (entry.key == pane_key_) {
            return receiverCenterHz();
        }
        if (const HeldView* view = held_view(entry.key); view != nullptr) {
            return static_cast<double>(view->absolute_hz);
        }
    }
    return 0.0;
}

}  // namespace revenant::ui
