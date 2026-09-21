// EngineLink's RDS surface: the poll, the region, and the station as a
// pane shows it.
//
// Split from ui/models/engine_link.cpp on the same grounds audio_link.cpp
// and receiver_link.cpp are: same class, same threads, same rules, and
// ui/models/engine_link.h holds the contract.
//
// THE WIRE HAS CARRIED THIS ALL ALONG. core/rpc/client.h's rds_station has
// been served since the decoder landed, tests/rpc/test_rpc_rds.cpp drives
// it through a real engine, and on 2026-09-20 a real station answered with
// a call sign, a PS, a RadioText and a PTY while the window showed none of
// it. The whole of this file is reading what was already there.
//
// WHY THE FIRST CALL IS A DECISION AND NOT A POLL. core/rpc/client.h is
// explicit that the first rds_station call BUILDS the decoder on that
// receiver, the way the first detections call builds the detector. So this
// is behind a switch: an always-on poll would stand a composite decoder up
// behind every receiver anybody ever tuned, on an engine shared with other
// clients.
//
// WHY A REFUSAL IS THE USEFUL ANSWER AND NOT A FAILURE. Four conditions
// have to hold before a receiver can carry a composite, and the binding
// one on the shipped grid is the audio rate: 57 kHz has to survive the
// decimation, which means about 171000 samples a second, and a 64-channel
// grid over 2.4 MS/s has channels a fraction of that wide. The engine
// refuses and names WHICH condition failed. That sentence is the thing the
// operator needs, so it goes on screen verbatim rather than being turned
// into "RDS unavailable".

#include "models/engine_link.h"

#include <mutex>
#include <string>
#include <utility>

#include <QMetaObject>
#include <QString>

#include "core/rpc/client.h"
#include "core/rpc/types.h"
#include "models/rds_view.h"

namespace revenant::ui {

void EngineLink::setRdsWanted(bool wanted)
{
    if (rds_wanted_.load() == wanted) {
        return;
    }
    rds_wanted_.store(wanted);

    if (!wanted) {
        // Cleared here and not left for the supervisor. The operator has
        // just said they are not looking at this, and a station frozen on
        // screen behind a switch that is off is a reading nothing stands
        // behind.
        rds_answered_ = false;
        rds_station_ = {};
        rds_status_.clear();
        rds_identity_.clear();
        rds_ps_.clear();
        rds_radio_text_.clear();
        rds_programme_type_.clear();
        rds_fault_.clear();
        rds_is_fault_ = false;
        rds_decoding_ = false;
        rds_ps_segments_ = 0;
        rds_ps_segments_total_ = 0;
        rds_rt_segments_ = 0;
        rds_rt_segments_total_ = 0;
        rds_block_error_rate_ = -1.0;
    }

    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        rds_work_pending_ = true;
    }
    supervisor_wake_.notify_all();

    emit rdsChanged();
}

QString EngineLink::rdsRegion() const
{
    return rds_region_rbds_.load() ? QStringLiteral("rbds") : QStringLiteral("rds");
}

void EngineLink::setRdsRegion(const QString& region)
{
    // Two spellings and nothing else. An unrecognised one is ignored
    // rather than defaulted, because defaulting is how a client ends up
    // silently reading a US station under the European PTY table.
    bool rbds = false;
    if (region == QStringLiteral("rbds")) {
        rbds = true;
    } else if (region != QStringLiteral("rds")) {
        return;
    }

    if (rds_region_rbds_.load() == rbds) {
        return;
    }
    rds_region_rbds_.store(rbds);

    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        rds_work_pending_ = true;
    }
    supervisor_wake_.notify_all();

    emit rdsChanged();
}

void EngineLink::clear_rds()
{
    if (rds_polled_vrx_ == 0 && !rds_region_written_) {
        return;
    }
    rds_polled_vrx_ = 0;
    rds_region_written_ = false;

    {
        const std::lock_guard<std::mutex> lock(rds_mutex_);
        has_rds_handover_ = true;
        handover_rds_answered_ = false;
        handover_rds_station_ = {};
        handover_rds_fault_.clear();
    }
    QMetaObject::invokeMethod(this, [this] { adopt_rds(); }, Qt::QueuedConnection);
}

void EngineLink::poll_rds()
{
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        rds_work_pending_ = false;
    }

    const qulonglong vrx = live_receiver_id_;
    if (client_ == nullptr || !rds_wanted_.load() || vrx == 0) {
        clear_rds();
        return;
    }

    // THE RECEIVER CHANGING IS A DIFFERENT DECODER. A rebuild issues a new
    // id and the decoder hangs off the receiver, so anything accumulated
    // belongs to the one that went and the region has to be written again.
    if (vrx != rds_polled_vrx_) {
        rds_polled_vrx_ = vrx;
        rds_region_written_ = false;
    }

    const bool want_rbds = rds_region_rbds_.load();

    // WRITTEN ONLY WHEN IT HAS TO BE. core/rpc/client.h: this call rebuilds
    // the decoder and clears everything accumulated, and a client that set
    // the region it already had would throw away the station the operator
    // was reading, once a second, forever. It is also settable BEFORE the
    // first poll, which is why it goes first: otherwise the first answer
    // comes back under the wrong region and is then discarded by the write
    // that corrects it.
    if (!rds_region_written_ || want_rbds != rds_posted_region_rbds_) {
        const auto region = want_rbds ? rpc::RdsRegion::Rbds : rpc::RdsRegion::Rds;
        if (auto set = client_->set_rds_region(vrx, region); !set) {
            const std::lock_guard<std::mutex> lock(rds_mutex_);
            has_rds_handover_ = true;
            handover_rds_answered_ = false;
            handover_rds_station_ = {};
            handover_rds_fault_ = QString::fromStdString(set.error().message);
            QMetaObject::invokeMethod(
                this, [this] { adopt_rds(); }, Qt::QueuedConnection);
            return;
        }
        rds_region_written_ = true;
        rds_posted_region_rbds_ = want_rbds;
    }

    auto station = client_->rds_station(vrx);

    // POSTED ON EVERY PASS, unlike the detection and audio surfaces. The
    // health counters move continuously on a live station, so a
    // suppression comparison would have to diff the whole struct to answer
    // "did anything change" and would find that it had, every time. One
    // metacall a second carrying a struct nobody redraws from is cheaper
    // than that comparison.
    {
        const std::lock_guard<std::mutex> lock(rds_mutex_);
        has_rds_handover_ = true;
        if (station) {
            handover_rds_answered_ = true;
            handover_rds_station_ = std::move(*station);
            handover_rds_fault_.clear();
        } else {
            handover_rds_answered_ = false;
            handover_rds_station_ = {};
            handover_rds_fault_ = QString::fromStdString(station.error().message);
        }
    }
    QMetaObject::invokeMethod(this, [this] { adopt_rds(); }, Qt::QueuedConnection);
}

void EngineLink::adopt_rds()
{
    rpc::RdsStation station;
    bool answered = false;
    QString fault;

    {
        const std::lock_guard<std::mutex> lock(rds_mutex_);
        if (!has_rds_handover_) {
            return;
        }
        has_rds_handover_ = false;
        station = std::move(handover_rds_station_);
        answered = handover_rds_answered_;
        fault = handover_rds_fault_;
    }

    rds_station_ = std::move(station);
    rds_answered_ = answered;
    rds_fault_ = fault;

    // The whole derivation, on the Qt thread and through the one pure
    // function ui/tests drives. Nothing here decides what an empty station
    // means; models/rds_view.h does, in the order core/rpc/types.h asks
    // for, and this converts the result to QString through fromStdString,
    // which is the UTF-8 conversion. Never fromLatin1: see
    // models/rds_text.h for the byte that makes that mistake invisible.
    const RdsView view = make_rds_view(rds_station_, answered);

    rds_status_ = QString::fromStdString(view.status);
    rds_is_fault_ = view.is_fault;
    rds_decoding_ = view.state == RdsState::Decoding;
    rds_identity_ = QString::fromStdString(view.identity);
    rds_ps_ = QString::fromStdString(view.ps.text);
    rds_radio_text_ = QString::fromStdString(view.radio_text.text);
    rds_programme_type_ = QString::fromStdString(view.programme_type);
    rds_ps_segments_ = view.ps.segments_received;
    rds_ps_segments_total_ = view.ps.segments_total;
    rds_rt_segments_ = view.radio_text.segments_received;
    rds_rt_segments_total_ = view.radio_text.segments_total;
    rds_block_error_rate_ = view.block_error_rate;

    // A refusal outranks the view's own sentence. make_rds_view was handed
    // answered=false and says "not asking for RDS on this receiver", which
    // is the wrong sentence for a poll the engine turned down: the window
    // IS asking, and the engine's own words name which of the four
    // conditions the receiver failed.
    if (!fault.isEmpty()) {
        rds_is_fault_ = true;
        rds_decoding_ = false;
        rds_status_ = fault;
    }

    emit rdsChanged();
}

}  // namespace revenant::ui
