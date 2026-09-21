// EngineLink's front-end tuning surface: the parse, the write and the
// engine's answer.
//
// WHY THERE IS A SEPARATE FILE. ui/models/engine_link.cpp is the
// connection, the frame path and the rate clock, receiver_link.cpp is the
// pane's receiver and audio_link.cpp is the subscription. This is the
// source itself, which is a fourth thing and the only one that can move
// every frequency on screen at once.
//
// THE WRITE IS ASYNCHRONOUS, LIKE EVERY OTHER WRITE ON THIS OBJECT.
// core/rpc/client.h is explicit that a call blocks for a round trip and
// must not be made from the frame callback, so the Qt thread cannot make
// one either without stalling the window. tuneSource records what is
// wanted and wakes the supervisor; the supervisor sends it, reads the new
// EngineInfo back and hands both over together.
//
// READING THE INFO BACK IS NOT BELT AND BRACES. EngineInfo::sourceCenter
// is what frequencyAtFraction, receiverCenterHz, the axis labels and every
// detection row are derived from. A retune moves it, and it is the only
// field in EngineInfo that moves while a connection stays up, so a client
// that took the granted centre from setSourceCenter's answer and left the
// rest alone would have two sources of truth for one number.

#include "models/engine_link.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

#include <QMetaObject>
#include <QString>

#include "core/rpc/client.h"
#include "models/frequency_entry.h"
#include "models/wire_seam.h"

namespace revenant::ui {

QString EngineLink::previewTune(const QString& text) const
{
    const std::string utf8 = text.toStdString();
    const auto parsed = parse_frequency(utf8);
    if (!parsed.has_value()) {
        return {};
    }
    return QString::fromStdString(format_mhz(parsed->hertz)) + QStringLiteral(" MHz");
}

bool EngineLink::tuneTextValid(const QString& text) const
{
    return parse_frequency(text.toStdString()).has_value();
}

bool EngineLink::tuneSource(const QString& text)
{
    const auto parsed = parse_frequency(text.toStdString());
    if (!parsed.has_value()) {
        // REFUSED HERE AND SAID HERE, rather than sent and refused by the
        // engine. The engine would answer "out of range" for text that is
        // not a number at all, which sends the operator looking at the
        // band plan instead of at their own typing.
        tune_fault_ = QStringLiteral(
            "\"%1\" is not a frequency. Type 95.1, 95.1M, 95100000 or 98.1 MHz; a "
            "bare number under a million is read as megahertz.")
                          .arg(text);
        emit sourceTuningChanged();
        return false;
    }

    tuneSourceHz(static_cast<double>(parsed->hertz));
    return true;
}

void EngineLink::tuneSourceHz(double hertz)
{
    const auto target = static_cast<std::int64_t>(hertz);

    // CHECKED AGAINST THE RANGE HERE AS WELL AS AT THE ENGINE. The engine
    // is the authority and refuses anything outside it, but the refusal
    // costs a round trip and arrives a quarter of a second later, by which
    // time the operator has typed something else. The stop is local so the
    // sentence is immediate, and the engine's own refusal still overwrites
    // it if the two ever disagree.
    if (source_can_retune_ && source_tune_high_ > source_tune_low_ &&
        (target < source_tune_low_ || target > source_tune_high_)) {
        tune_fault_ = QStringLiteral("%1 MHz is outside what this source tunes, "
                                     "which is %2 to %3 MHz.")
                          .arg(QString::fromStdString(format_mhz(target)),
                               QString::fromStdString(format_mhz(source_tune_low_)),
                               QString::fromStdString(format_mhz(source_tune_high_)));
        emit sourceTuningChanged();
        return;
    }

    tune_requested_hz_ = target;
    tune_fault_.clear();
    requested_center_hz_.store(target);
    tune_pending_.store(true);

    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        tune_work_pending_ = true;
    }
    supervisor_wake_.notify_all();

    emit sourceTuningChanged();
}

void EngineLink::probe_source_tuning()
{
    if (client_ == nullptr) {
        return;
    }

    const SourceTuning tuning = seam_source_can_retune(*client_);

    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        handover_has_range_ = true;
        handover_can_retune_ = tuning.can_retune;
        handover_tune_low_ = tuning.low_hz;
        handover_tune_high_ = tuning.high_hz;
        handover_retune_unavailable_ = QString::fromStdString(tuning.refusal);

        // The tune answer is reset with it. A new connection has been
        // asked for nothing yet, and the previous connection's granted
        // centre is about a source that may not even be the same radio.
        handover_has_tune_ = true;
        handover_tune_fault_.clear();
        handover_tune_granted_ = 0;
        handover_tune_answered_ = false;
        has_tuned_info_ = false;
    }
    QMetaObject::invokeMethod(
        this, [this] { adopt_source_tuning(); }, Qt::QueuedConnection);
}

void EngineLink::apply_source_tune()
{
    if (client_ == nullptr) {
        return;
    }

    // THE FLAG IS CLEARED BEFORE THE REQUEST IS TAKEN, which is the order
    // apply_receiver_request already uses and for the same reason. The
    // other way round, a tune posted between the take and the clear sets
    // both, and then this clears the flag the wake depended on: the
    // request is still there, but the supervisor sleeps out the poll
    // interval before it notices. Clearing first can only cost a spurious
    // wake, which costs one pass that finds nothing to do.
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        tune_work_pending_ = false;
    }

    if (!tune_pending_.exchange(false)) {
        return;
    }

    const std::int64_t target = requested_center_hz_.load();
    auto granted = seam_set_source_center(*client_, target);

    // The range fields are deliberately untouched. They belong to the
    // other handover, they are the Qt thread's to hold between
    // connections, and reading them here to copy them back would be a
    // data race on four members this thread does not own.
    const std::lock_guard<std::mutex> lock(source_mutex_);
    handover_has_tune_ = true;

    if (!granted) {
        handover_tune_fault_ = QString::fromStdString(granted.error().message);
        handover_tune_answered_ = false;
        has_tuned_info_ = false;
    } else {
        handover_tune_fault_.clear();
        handover_tune_granted_ = *granted;
        handover_tune_answered_ = true;

        // The geometry the retune produced. A failure here is not a failed
        // retune: the radio moved, and the only cost is that the axis
        // keeps the previous centre until the next pass. Reported through
        // the fault line rather than swallowed, because an axis that is
        // wrong about where the radio is, is the one failure a spectrum
        // display must not have.
        if (auto info = client_->info()) {
            handover_tuned_info_ = *info;
            has_tuned_info_ = true;
        } else {
            has_tuned_info_ = false;
            handover_tune_fault_ =
                QStringLiteral("the source retuned and the engine then refused to say "
                               "what it built, so the frequency axis is still showing "
                               "the previous centre: ") +
                QString::fromStdString(info.error().message);
        }
    }

    QMetaObject::invokeMethod(
        this, [this] { adopt_source_tuning(); }, Qt::QueuedConnection);
}

void EngineLink::poll_source_pacing(bool engine_running)
{
    // A plain `if` and not `if constexpr`: both arms compile either way,
    // and the constant is what stops a round trip a second being spent on
    // an engine whose wire cannot answer. See models/wire_seam.h.
    if (client_ == nullptr || !kSeamHasPacing) {
        return;
    }

    auto info = client_->info();
    if (!info) {
        // NOT REPORTED HERE. An info call that fails on a live connection
        // is the connection going, and the probe above this is what finds
        // that out and says so. A second sentence about the same event is
        // worse than one.
        return;
    }

    PacingSample sample;
    sample.carried = true;
    sample.realtime_factor = seam_realtime_factor(*info).value_or(0.0);
    sample.paced_by = seam_source_paced_by(*info);
    // Passed in rather than read off the handover. The running flag lives
    // under state_mutex_ and belongs to the connection hand-off; the one
    // caller has just been told the answer by the same probe that decided
    // this connection is alive, so taking it as an argument is both the
    // fresher value and one lock fewer.
    sample.engine_running = engine_running;

    // Posted only on a change. It is asked once a second for the life of
    // the window and the answer is the same almost every time, so a
    // metacall per pass would wake the GUI thread every second to tell it
    // nothing. The comparison is exact rather than within a tolerance:
    // smoothing belongs in the verdict, which has its own hysteresis, and
    // doing it twice would make the rule the test drives not the rule the
    // window runs.
    if (sample.realtime_factor == posted_pacing_.realtime_factor &&
        sample.paced_by == posted_pacing_.paced_by &&
        sample.carried == posted_pacing_.carried &&
        sample.engine_running == posted_pacing_.engine_running) {
        return;
    }
    posted_pacing_ = sample;

    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        handover_has_pacing_ = true;
        handover_pacing_ = sample;
    }
    QMetaObject::invokeMethod(this, [this] { adopt_pacing(); }, Qt::QueuedConnection);
}

void EngineLink::adopt_pacing()
{
    PacingSample sample;
    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        if (!handover_has_pacing_) {
            return;
        }
        handover_has_pacing_ = false;
        sample = handover_pacing_;
    }

    // The previous verdict goes in, which is where the hysteresis lives.
    // Held on this thread rather than in the rule, so the rule stays a
    // pure function and a test can drive a whole trajectory through it.
    pacing_ = sample;
    pacing_verdict_ = classify_pacing(sample, pacing_verdict_);
    pacing_text_ = QString::fromStdString(pacing_sentence(pacing_verdict_, sample));

    emit pacingChanged();
}

void EngineLink::adopt_source_tuning()
{
    bool geometry_moved = false;
    bool anything = false;

    {
        const std::lock_guard<std::mutex> lock(source_mutex_);

        if (handover_has_range_) {
            handover_has_range_ = false;
            anything = true;
            source_can_retune_ = handover_can_retune_;
            source_tune_low_ = handover_tune_low_;
            source_tune_high_ = handover_tune_high_;
            source_retune_unavailable_ = handover_retune_unavailable_;
        }

        if (handover_has_tune_) {
            handover_has_tune_ = false;
            anything = true;
            tune_fault_ = handover_tune_fault_;
            tune_answered_ = handover_tune_answered_;
            if (handover_tune_answered_) {
                tune_granted_hz_ = handover_tune_granted_;
            }

            if (has_tuned_info_) {
                has_tuned_info_ = false;
                info_ = handover_tuned_info_;
                geometry_moved = true;
            }
        }
    }

    if (!anything) {
        return;
    }

    emit sourceTuningChanged();

    if (geometry_moved) {
        // AND THE HISTORY GOES WITH IT. The span moved, so every row the
        // waterfall holds was drawn under a frequency axis that no longer
        // applies, and a row left in place would put a signal where it
        // never was. That is the argument connectionChanged already
        // carries, word for word, for a new engine; a retune is the same
        // event for the same reason.
        emit connectionChanged();
    }
}

}  // namespace revenant::ui
