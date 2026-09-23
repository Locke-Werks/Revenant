// The rack half of EngineLink: every receiver this window holds, which one
// the pane is on, and the mix.
//
// Split out the way receiver_link.cpp and audio_link.cpp are. Same class,
// same threads, same rules; ui/models/engine_link.h holds the contract and
// models/receiver_rack.h the rules this file applies.
//
// THE SHAPE OF IT. The pane, which is everything receiver_link.cpp drives,
// holds one receiver and is unchanged by there being others. Focusing
// another receiver PARKS the pane's one: it keeps running on the engine
// with its own audio, and the pane lets go of its passband display and its
// decoders. Then the chosen one is moved into the pane, the passband
// subscription follows it, and the decode and RDS reconciles find a new
// receiver on their next pass exactly as they do after a mode change. So
// nothing that reads the pane has to know the rack exists.
//
// THE THREE THREADS, FOR THIS FILE. The Qt thread owns rack_, pane_key_ and
// the held views, and posts operations. The supervisor owns held_ and
// live_pane_key_ and performs them, in the order posted and interleaved with
// the pane's own requests; see RackOp in the header for why that order is
// the whole design. The sound card's thread reads the mix atomics and
// nothing else here.

#include "models/engine_link.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include <QMetaObject>
#include <QString>
#include <QVariantMap>

#include "models/receiver_palette.h"

namespace revenant::ui {
namespace {

// How long after a click its second press still counts as the other half
// of a double click. Longer than any platform's double-click interval, so
// the display's own judgement of what was a double click is the one that
// decides; this only keeps a snapshot from outliving the gesture it was
// taken for.
constexpr qint64 kClickSnapshotMs = 2000;

[[nodiscard]] QString colour_text(std::size_t slot)
{
    return QString::fromStdString(rgb8_hex(receiver_colour(slot)));
}

[[nodiscard]] double rational_hz(const rpc::Rational& value)
{
    return value.denominator == 0 ? 0.0
                                  : static_cast<double>(value.numerator) /
                                        static_cast<double>(value.denominator);
}

}  // namespace

// ---------------------------------------------------------------------------
// What the rack shows, Qt thread
// ---------------------------------------------------------------------------

EngineLink::HeldView* EngineLink::held_view(std::uint64_t key)
{
    for (auto& [held_key, view] : held_views_) {
        if (held_key == key) {
            return &view;
        }
    }
    return nullptr;
}

const EngineLink::HeldView* EngineLink::held_view(std::uint64_t key) const
{
    for (const auto& [held_key, view] : held_views_) {
        if (held_key == key) {
            return &view;
        }
    }
    return nullptr;
}

int EngineLink::focusedSlot() const
{
    const RackEntry* entry = rack_.find(pane_key_);
    return entry == nullptr ? 0 : static_cast<int>(entry->slot);
}

std::vector<RackMarker> EngineLink::rackMarkers() const
{
    std::vector<RackMarker> out;
    if (!connected_) {
        return out;
    }

    RackMarker focused;
    bool have_focused = false;
    for (const RackEntry& entry : rack_.entries()) {
        RackMarker marker;
        marker.key = entry.key;
        marker.slot = entry.slot;
        if (entry.key == pane_key_) {
            // The grant when there is one and the request otherwise, which
            // is the rule build_receiver_marker has always drawn the pane's
            // receiver by: the request follows a drag at the pointer's rate.
            double low = static_cast<double>(receiver_status_.placement.granted_low);
            double high = static_cast<double>(receiver_status_.placement.granted_high);
            if (!(high > low)) {
                low = static_cast<double>(wanted_.passband_low);
                high = static_cast<double>(wanted_.passband_high);
            }
            const double centre = receiverCenterHz();
            marker.band = ReceiverBand{centre + low, centre + high, centre};
            marker.focused = true;
            focused = marker;
            have_focused = true;
            continue;
        }
        const HeldView* view = held_view(entry.key);
        if (view == nullptr) {
            continue;
        }
        double low = static_cast<double>(view->granted_low);
        double high = static_cast<double>(view->granted_high);
        if (!(high > low)) {
            low = static_cast<double>(view->params.passband_low);
            high = static_cast<double>(view->params.passband_high);
        }
        const auto centre = static_cast<double>(view->absolute_hz);
        marker.band = ReceiverBand{centre + low, centre + high, centre};
        out.push_back(marker);
    }

    // Last, so it is drawn over the others where bands overlap.
    if (have_focused && (receiver_id_ != 0 || pane_key_ != 0)) {
        out.push_back(focused);
    }
    return out;
}

QVariantList EngineLink::rackEntries() const
{
    QVariantList out;
    const std::vector<RackMarker> markers = rackMarkers();
    for (const RackEntry& entry : rack_.entries()) {
        const bool focused = entry.key == pane_key_;
        double centre = 0.0;
        double level = -200.0;
        QString mode;
        if (focused) {
            centre = receiverCenterHz();
            level = receiver_status_.level_dbfs;
            mode = receiverDemod();
        } else if (const HeldView* view = held_view(entry.key); view != nullptr) {
            centre = static_cast<double>(view->absolute_hz);
            level = view->level_dbfs;
            mode = demod_name(view->params.demod);
        }

        double low = centre;
        double high = centre;
        for (const RackMarker& marker : markers) {
            if (marker.key == entry.key) {
                low = marker.band.low_hz;
                high = marker.band.high_hz;
            }
        }

        out.append(QVariantMap{
            {QStringLiteral("key"), QVariant::fromValue<qulonglong>(entry.key)},
            {QStringLiteral("slot"), static_cast<int>(entry.slot)},
            {QStringLiteral("colour"), colour_text(entry.slot)},
            {QStringLiteral("label"), QStringLiteral("RX %1").arg(entry.slot + 1)},
            {QStringLiteral("frequencyHz"), centre},
            {QStringLiteral("lowHz"), low},
            {QStringLiteral("highHz"), high},
            {QStringLiteral("mode"), mode},
            {QStringLiteral("levelDbfs"), level},
            {QStringLiteral("focused"), focused},
            {QStringLiteral("pending"), entry.engine_id == 0},
            {QStringLiteral("muted"), entry.muted},
            {QStringLiteral("solo"), entry.solo},
            {QStringLiteral("heard"), rack_.heard(entry.key)},
            {QStringLiteral("gain"), entry.gain},
            {QStringLiteral("gainText"), QString::fromStdString(rack_gain_text(entry.gain))},
        });
    }
    return out;
}

void EngineLink::set_rack_note(const QString& note)
{
    if (note == rack_note_) {
        return;
    }
    rack_note_ = note;
    emit rackChanged();
}

// ---------------------------------------------------------------------------
// Writes, Qt thread
// ---------------------------------------------------------------------------

void EngineLink::post_rack_op(RackOp op)
{
    {
        const std::lock_guard<std::mutex> lock(receiver_mutex_);
        // The pane's outstanding request belongs to the receiver the pane
        // held when it was written, which is the receiver this operation is
        // about to move. So it travels with the operation and is applied
        // first; see RackOp.
        op.outgoing = take_pane_request_locked();
        rack_ops_.push_back(std::move(op));
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        receiver_work_pending_ = true;
    }
    supervisor_wake_.notify_all();
}

bool EngineLink::ensure_pane_entry()
{
    if (pane_key_ != 0) {
        return true;
    }
    const auto key = rack_.add();
    if (!key) {
        set_rack_note(QString::fromUtf8(kRackFullText));
        return false;
    }
    pane_key_ = *key;
    rack_.focus(*key);
    if (RackEntry* entry = rack_.find(*key); entry != nullptr) {
        mix_gain_[entry->slot].store(static_cast<float>(rack_gain_amplitude(entry->gain)),
                                     std::memory_order_relaxed);
    }
    post_audio_wants();
    emit rackChanged();
    return true;
}

void EngineLink::park_pane()
{
    if (pane_key_ == 0) {
        return;
    }

    // THE COMPOSITE RATE COMES OFF FIRST. A receiver raised to 171000 S/s for
    // RDS hands out the multiplex rather than programme audio, and a held
    // receiver is one the operator is listening to without looking at, so
    // it goes back to programme audio on the way out. The rebuild that takes
    // is the pane's request, which the park below carries with it.
    if (carries_composite(wanted_)) {
        lower_receiver_from_composite();
    }

    HeldView view;
    view.params = wanted_;
    view.absolute_hz = receiver_absolute_hz_;
    view.edges_touched = edges_touched_;
    view.demod_touched = demod_touched_;
    view.detection_bandwidth_hz = tuned_detection_bandwidth_;
    view.granted_low = static_cast<int>(receiver_status_.placement.granted_low);
    view.granted_high = static_cast<int>(receiver_status_.placement.granted_high);
    view.edge_limit = receiver_edge_limit_;
    view.level_dbfs = receiver_status_.level_dbfs;
    held_views_.emplace_back(pane_key_, view);

    if (RackEntry* entry = rack_.find(pane_key_); entry != nullptr) {
        entry->engine_id = receiver_id_;
    }

    RackOp op;
    op.kind = RackOp::Kind::Park;
    op.key = pane_key_;
    post_rack_op(std::move(op));

    clear_pane();
    pane_key_ = 0;
}

void EngineLink::focus_entry(std::uint64_t key)
{
    if (key == 0 || key == pane_key_) {
        return;
    }
    const HeldView* found = held_view(key);
    if (found == nullptr) {
        return;
    }
    // A copy, because the park below adds to held_views_ and may move it.
    const HeldView incoming = *found;

    park_pane();
    std::erase_if(held_views_, [key](const auto& held) { return held.first == key; });

    // The pane takes the receiver as it was parked. The centre is rebuilt
    // from the absolute frequency against the source's centre now, because
    // the engine rebased the receiver's offset if the front end moved while
    // it was held, and the offset parked with it is from before.
    wanted_ = incoming.params;
    wanted_.center = incoming.absolute_hz - info_.source_center;
    receiver_absolute_hz_ = incoming.absolute_hz;
    edges_touched_ = incoming.edges_touched;
    demod_touched_ = incoming.demod_touched;
    tuned_detection_bandwidth_ = incoming.detection_bandwidth_hz;
    sent_width_ = static_cast<int>(wanted_.passband_high - wanted_.passband_low);

    // What the rack last heard about it, so the marker and the fit line have
    // something to draw until the pane's own poll answers.
    receiver_status_ = {};
    receiver_status_.params = wanted_;
    receiver_status_.placement.granted_low = incoming.granted_low;
    receiver_status_.placement.granted_high = incoming.granted_high;
    receiver_status_.level_dbfs = incoming.level_dbfs;
    receiver_edge_limit_ = incoming.edge_limit;

    const RackEntry* entry = rack_.find(key);
    receiver_id_ = entry == nullptr ? 0 : entry->engine_id;
    pane_key_ = key;
    rack_.focus(key);

    // The strip's own sentence about the receiver it is about to describe
    // again would be stale.
    if (!receiver_gone_text_.isEmpty()) {
        receiver_gone_text_.clear();
        emit receiverGoneChanged();
    }

    RackOp op;
    op.kind = RackOp::Kind::Focus;
    op.key = key;
    post_rack_op(std::move(op));

    emit receiverChanged();
    emit receiverStatusChanged();
    emit passbandChanged();
    emit audioChanged();
    emit rdsChanged();
    emit aftChanged();
    emit autoFilterChanged();
    note_receiver_bookmarked();
    update_receiver_fit();
    post_audio_wants();
    emit rackChanged();
}

void EngineLink::focusReceiver(qulonglong key)
{
    focus_entry(key);
}

void EngineLink::focusNextReceiver(int step)
{
    focus_entry(rack_.neighbour(step));
}

void EngineLink::add_receiver_at(double absolute_hz, const QString& mode,
                                 double detection_bandwidth_hz)
{
    if (rack_.full()) {
        set_rack_note(QString::fromUtf8(kRackFullText));
        return;
    }

    park_pane();

    // A new receiver takes its mode's default filter, not the edges of the
    // one it was parked beside, and none of that one's composite rate. The
    // mode itself carries over unless one is named or measured, which is
    // what adding a second receiver on the same band wants.
    wanted_.passband_low = 0;
    wanted_.passband_high = 0;
    wanted_.bandwidth = 0;
    wanted_.audio_rate = 0;
    set_rack_note(QString{});

    if (detection_bandwidth_hz > 0.0) {
        tuneReceiverToDetection(absolute_hz, mode, detection_bandwidth_hz);
    } else {
        tuneReceiver(absolute_hz, mode);
    }
}

void EngineLink::addReceiver(double absolute_hz, const QString& mode)
{
    add_receiver_at(absolute_hz, mode, 0.0);
}

void EngineLink::add_held_receiver(std::uint64_t key, double absolute_hz,
                                   const rpc::VrxParams& params)
{
    const auto absolute = static_cast<std::int64_t>(std::llround(absolute_hz));
    rpc::VrxParams placed = params;
    placed.center = absolute - info_.source_center;

    HeldView* view = held_view(key);
    if (view == nullptr) {
        held_views_.emplace_back(key, HeldView{});
        view = &held_views_.back().second;
    }
    view->params = placed;
    view->absolute_hz = absolute;

    RackOp op;
    op.kind = RackOp::Kind::AddHeld;
    op.key = key;
    op.params = placed;
    post_rack_op(std::move(op));
}

void EngineLink::addStartupReceiver(double absolute_hz, const QString& mode)
{
    startup_extra_.emplace_back(absolute_hz, mode);
}

bool EngineLink::spanClick(double pointer_hz, double center_hz, double bandwidth_hz)
{
    std::vector<RackBand> bands;
    for (const RackMarker& marker : rackMarkers()) {
        bands.push_back({marker.key, marker.band.low_hz, marker.band.high_hz});
    }

    SpanClickInput in;
    in.under = receiver_under(bands, pointer_hz, pane_key_);
    in.had_receiver = pane_key_ != 0;
    in.count = rack_.size();
    const SpanClick kind = classify_span_click(in);

    // No default: a new kind of click is a warning the CI build stops on.
    switch (kind) {
        case SpanClick::Focus:
            click_snapshot_.valid = false;
            focus_entry(in.under);
            return false;
        case SpanClick::Retune:
        case SpanClick::Open:
            // The pane as this click found it, so a second click that turns
            // this into a double click can put it back.
            click_snapshot_ = ClickSnapshot{};
            click_snapshot_.valid = true;
            click_snapshot_.at_ms = click_clock_.elapsed();
            click_snapshot_.had_receiver = pane_key_ != 0;
            click_snapshot_.opened = kind == SpanClick::Open;
            click_snapshot_.key = pane_key_;
            click_snapshot_.wanted = wanted_;
            click_snapshot_.absolute_hz = receiver_absolute_hz_;
            click_snapshot_.edges_touched = edges_touched_;
            click_snapshot_.demod_touched = demod_touched_;
            click_snapshot_.detection_bandwidth_hz = tuned_detection_bandwidth_;
            tuneReceiverToDetection(center_hz, QString{}, bandwidth_hz);
            return true;
        case SpanClick::Add:
        case SpanClick::Full:
        case SpanClick::Nothing:
            break;
    }
    return false;
}

bool EngineLink::spanDoubleClick(double pointer_hz, double center_hz, double bandwidth_hz)
{
    std::vector<RackBand> bands;
    for (const RackMarker& marker : rackMarkers()) {
        bands.push_back({marker.key, marker.band.low_hz, marker.band.high_hz});
    }

    const ClickSnapshot first = click_snapshot_;
    click_snapshot_.valid = false;
    const bool recent =
        first.valid && click_clock_.elapsed() - first.at_ms <= kClickSnapshotMs;

    SpanClickInput in;
    in.double_click = true;
    in.under = receiver_under(bands, pointer_hz, pane_key_);
    in.had_receiver = pane_key_ != 0;
    in.first_click_opened = recent && first.opened;
    in.count = rack_.size();

    switch (classify_span_click(in)) {
        case SpanClick::Nothing:
        case SpanClick::Focus:
        case SpanClick::Retune:
        case SpanClick::Open:
            return false;
        case SpanClick::Full:
            set_rack_note(QString::fromUtf8(kRackFullText));
            return false;
        case SpanClick::Add:
            break;
    }

    // THE FIRST CLICK MOVED THE FOCUSED RECEIVER, and a double click means
    // "a new one here", not "move this one here and add another beside it".
    // So it goes back to exactly where the first click found it, filter and
    // mode and all, before the new one opens. A mode the first click chose
    // from a measurement is a rebuild; anything else is a push constant.
    if (recent && first.had_receiver && first.key == pane_key_) {
        aft_yield();
        cancel_auto_filter(AutoFilterOutcome::Idle);
        const bool rebuild = first.wanted.demod != wanted_.demod ||
                             first.wanted.audio_rate != wanted_.audio_rate;
        wanted_ = first.wanted;
        receiver_absolute_hz_ = first.absolute_hz;
        edges_touched_ = first.edges_touched;
        demod_touched_ = first.demod_touched;
        tuned_detection_bandwidth_ = first.detection_bandwidth_hz;
        if (reset_passband_display()) {
            emit passbandChanged();
        }
        post_receiver_request(rebuild);
    }

    add_receiver_at(center_hz, QString{}, bandwidth_hz);
    return true;
}

void EngineLink::setReceiverMuted(qulonglong key, bool muted)
{
    RackEntry* entry = rack_.find(key);
    if (entry == nullptr || entry->muted == muted) {
        return;
    }
    entry->muted = muted;
    post_audio_wants();
    emit rackChanged();
}

void EngineLink::toggleReceiverSolo(qulonglong key)
{
    if (rack_.find(key) == nullptr) {
        return;
    }
    static_cast<void>(rack_.toggle_solo(key));
    post_audio_wants();
    emit rackChanged();
}

void EngineLink::setReceiverGain(qulonglong key, double position)
{
    RackEntry* entry = rack_.find(key);
    if (entry == nullptr) {
        return;
    }
    const double clamped = std::clamp(position, 0.0, 1.0);
    if (clamped == entry->gain) {
        return;
    }
    entry->gain = clamped;

    // Straight to the player: a gain moves nothing on the engine.
    mix_gain_[entry->slot].store(static_cast<float>(rack_gain_amplitude(clamped)),
                                 std::memory_order_relaxed);
    emit rackChanged();
}

void EngineLink::removeRackReceiver(qulonglong key)
{
    if (key == pane_key_) {
        removeReceiver();
        return;
    }
    if (rack_.find(key) == nullptr) {
        return;
    }
    rack_.remove(key);
    std::erase_if(held_views_, [key](const auto& held) { return held.first == key; });

    RackOp op;
    op.kind = RackOp::Kind::RemoveHeld;
    op.key = key;
    post_rack_op(std::move(op));

    post_audio_wants();
    emit rackChanged();
}

void EngineLink::post_audio_wants()
{
    std::vector<AudioWant> wants;
    for (const RackEntry& entry : rack_.entries()) {
        mix_gain_[entry.slot].store(static_cast<float>(rack_gain_amplitude(entry.gain)),
                                    std::memory_order_relaxed);
        if (rack_.heard(entry.key)) {
            wants.push_back(AudioWant{entry.key, entry.slot});
        }
    }
    {
        const std::lock_guard<std::mutex> lock(audio_mutex_);
        requested_audio_wants_ = std::move(wants);
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        audio_work_pending_ = true;
    }
    supervisor_wake_.notify_all();
}

void EngineLink::adopt_held_reports()
{
    std::vector<HeldReport> reports;
    {
        const std::lock_guard<std::mutex> lock(receiver_mutex_);
        reports.swap(handover_held_);
    }

    bool changed = false;
    bool membership = false;
    for (const HeldReport& report : reports) {
        RackEntry* entry = rack_.find(report.key);
        if (entry == nullptr || report.key == pane_key_) {
            continue;
        }

        if (report.gone) {
            // Said in the rack, because the strip that would have carried it
            // is about to go. The engine's own sentence when there is one,
            // and what this window can say for itself when there is not.
            QString note = report.why;
            if (note.isEmpty()) {
                const HeldView* view = held_view(report.key);
                note = QString::fromStdString(receiver_gone_sentence(
                    view == nullptr ? 0 : view->absolute_hz, spanLowHz(), spanHighHz()));
            }
            rack_.remove(report.key);
            std::erase_if(held_views_,
                          [key = report.key](const auto& held) { return held.first == key; });
            rack_note_ = note;
            membership = true;
            continue;
        }

        if (report.id != 0) {
            entry->engine_id = report.id;
        }
        if (report.has_status) {
            if (HeldView* view = held_view(report.key); view != nullptr) {
                view->level_dbfs = report.status.level_dbfs;
                view->granted_low = static_cast<int>(report.status.placement.granted_low);
                view->granted_high = static_cast<int>(report.status.placement.granted_high);

                // The same limit adopt_receiver_status derives, so a focus
                // hands the pane a drag limit before its own poll answers.
                const double limit =
                    0.5 * static_cast<double>(report.status.placement.channel_rate) -
                    std::abs(rational_hz(report.status.placement.residual));
                view->edge_limit = limit > 0.0 ? static_cast<int>(limit) : 0;
            }
        }
        changed = true;
    }

    if (membership) {
        post_audio_wants();
    }
    if (changed || membership) {
        emit rackChanged();
    }
}

// ---------------------------------------------------------------------------
// The supervisor's side
// ---------------------------------------------------------------------------

void EngineLink::post_settled_id(std::uint64_t key, qulonglong id)
{
    {
        const std::lock_guard<std::mutex> lock(receiver_mutex_);
        pending_receiver_ids_.push_back(SettledId{key, id});
    }
    QMetaObject::invokeMethod(
        this, [this] { adopt_receiver_status(); }, Qt::QueuedConnection);
}

void EngineLink::post_held_reports(std::vector<HeldReport> reports)
{
    if (reports.empty()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(receiver_mutex_);
        for (HeldReport& report : reports) {
            handover_held_.push_back(std::move(report));
        }
    }
    QMetaObject::invokeMethod(
        this, [this] { adopt_held_reports(); }, Qt::QueuedConnection);
}

qulonglong EngineLink::id_for_key(std::uint64_t key, rpc::Demod* demod) const
{
    if (key != 0 && key == live_pane_key_) {
        if (demod != nullptr) {
            *demod = live_receiver_demod_;
        }
        return live_receiver_id_;
    }
    for (const HeldVrx& held : held_) {
        if (held.key == key) {
            if (demod != nullptr) {
                *demod = held.params.demod;
            }
            return held.id;
        }
    }
    return 0;
}

void EngineLink::apply_rack_op(const RackOp& op)
{
    // No default: a new operation is a warning the CI build stops on.
    switch (op.kind) {
        case RackOp::Kind::Park: {
            if (live_receiver_id_ == 0 || live_pane_key_ != op.key) {
                // Nothing on the engine to park: the pane's add was refused,
                // or never got as far as the engine. The strip has nothing
                // behind it, so it goes, unless the engine itself has gone,
                // in which case the reconnection puts it back.
                if (live_pane_key_ == op.key) {
                    live_pane_key_ = 0;
                }
                if (client_ != nullptr) {
                    HeldReport gone;
                    gone.key = op.key;
                    gone.gone = true;
                    gone.why = QStringLiteral("the engine never made that receiver");
                    post_held_reports({gone});
                }
                return;
            }

            // The decoders and the passband display were the pane's. The
            // audio was the receiver's, and it goes on playing.
            stop_decoded();
            if (client_ != nullptr) {
                client_->unsubscribe_passband(live_receiver_id_);
            }
            held_.push_back(HeldVrx{op.key, live_receiver_id_, live_receiver_params_});
            post_settled_id(op.key, live_receiver_id_);
            live_receiver_id_ = 0;
            live_pane_key_ = 0;
            note_receiver_fault(QString{});
            return;
        }

        case RackOp::Kind::Focus: {
            const auto it = std::find_if(held_.begin(), held_.end(),
                                         [&op](const HeldVrx& h) { return h.key == op.key; });
            if (it == held_.end()) {
                // Gone before the pane could take it. With no engine that is
                // the connection, and the reconnection rebuilds the pane's
                // receiver from the Qt thread's request; with one, the pane
                // is told it has nothing.
                if (client_ != nullptr) {
                    post_settled_id(op.key, 0);
                }
                return;
            }

            live_receiver_id_ = it->id;
            live_pane_key_ = op.key;
            live_receiver_params_ = it->params;
            live_receiver_demod_ = it->params.demod;
            held_.erase(it);

            // The passband display follows the focus. A refusal is the same
            // non-fatal fault recreate_receiver reports.
            if (client_ != nullptr) {
                if (auto watched = client_->subscribe_passband(
                        live_receiver_id_, 1,
                        [this](const rpc::PassbandFrame& frame) { on_passband_frame(frame); });
                    !watched) {
                    note_receiver_fault(QString::fromStdString(watched.error().message));
                } else {
                    note_receiver_fault(QString{});
                }
            }
            post_settled_id(op.key, live_receiver_id_);
            return;
        }

        case RackOp::Kind::AddHeld: {
            if (client_ == nullptr) {
                return;
            }
            if (std::any_of(held_.begin(), held_.end(),
                            [&op](const HeldVrx& h) { return h.key == op.key; })) {
                return;
            }
            auto added = client_->add_vrx(op.params);
            if (!added) {
                HeldReport gone;
                gone.key = op.key;
                gone.gone = true;
                gone.why = QString::fromStdString(added.error().message);
                post_held_reports({gone});
                return;
            }
            held_.push_back(HeldVrx{op.key, *added, op.params});
            post_settled_id(op.key, *added);
            return;
        }

        case RackOp::Kind::RemoveHeld: {
            const auto it = std::find_if(held_.begin(), held_.end(),
                                         [&op](const HeldVrx& h) { return h.key == op.key; });
            if (it == held_.end()) {
                return;
            }
            // Audio first, for the reason drop_receiver gives.
            stop_audio_for(it->id);
            if (client_ != nullptr) {
                static_cast<void>(client_->remove_vrx(it->id));
            }
            held_.erase(it);
            return;
        }
    }
}

void EngineLink::poll_held_status()
{
    if (client_ == nullptr || held_.empty()) {
        return;
    }

    std::vector<HeldReport> reports;
    std::vector<std::uint64_t> ids;
    bool have_ids = false;

    for (auto it = held_.begin(); it != held_.end();) {
        auto status = client_->vrx_status(it->id);
        if (status) {
            // The engine's params, which carry the offset it rebased the
            // receiver to if the front end moved.
            it->params = status->params;
            HeldReport report;
            report.key = it->key;
            report.id = it->id;
            report.has_status = true;
            report.status = *status;
            reports.push_back(std::move(report));
            ++it;
            continue;
        }

        // The pane's own poll has the argument for asking the inventory
        // rather than reading the refusal; see forget_removed_receiver.
        const ErrorCategory category = status.error().category;
        if (category == ErrorCategory::Unreachable || category == ErrorCategory::Disconnected) {
            return;
        }
        if (!have_ids) {
            auto listed = client_->vrx_ids();
            if (!listed) {
                return;
            }
            ids.assign(listed->begin(), listed->end());
            have_ids = true;
        }
        if (std::find(ids.begin(), ids.end(), static_cast<std::uint64_t>(it->id)) !=
            ids.end()) {
            ++it;
            continue;
        }

        HeldReport gone;
        gone.key = it->key;
        gone.id = it->id;
        gone.gone = true;
        reports.push_back(std::move(gone));
        it = held_.erase(it);
    }

    post_held_reports(std::move(reports));
}

void EngineLink::forget_held(const QString& why)
{
    if (!why.isEmpty()) {
        std::vector<HeldReport> reports;
        for (const HeldVrx& held : held_) {
            HeldReport gone;
            gone.key = held.key;
            gone.id = held.id;
            gone.gone = true;
            gone.why = why;
            reports.push_back(std::move(gone));
        }
        post_held_reports(std::move(reports));
    }
    held_.clear();
}

}  // namespace revenant::ui
