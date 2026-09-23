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

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <QMetaObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "core/rpc/client.h"
#include "models/frequency_entry.h"
#include "models/source_choice.h"
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

double EngineLink::parseHz(const QString& text) const
{
    const auto parsed = parse_frequency(text.toStdString());
    return parsed.has_value() ? static_cast<double>(parsed->hertz) : 0.0;
}

double EngineLink::parseRateHz(const QString& text) const
{
    const auto parsed = parse_frequency(text.toStdString(), BareNumber::Hertz);
    return parsed.has_value() ? static_cast<double>(parsed->hertz) : 0.0;
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

// ---------------------------------------------------------------------------
// The wheel, coalesced for the whole window
// ---------------------------------------------------------------------------

void EngineLink::takeScrollTune(double angle_delta_eighths)
{
    // GATED ON sourceCanRetune HERE, so neither item repeats it. A file and a
    // synthetic scene refuse a retune in their own words and the property says
    // so once per connection, so the wheel does nothing on those rather than
    // posting a request that always fails and writing a fault line the
    // operator did not ask a question to get.
    //
    // The accumulator is cleared at the same time: wheel banked against a
    // recording must not fire at the first dongle opened after it.
    if (!source_can_retune_) {
        scroll_tune_ = ScrollTuneState{};
        arm_scroll_flush(0.0);
        return;
    }

    ScrollTuneRequest request;
    request.angle_delta_eighths = angle_delta_eighths;

    // The span the displays are actually drawing, which is the pair the axis
    // and every overlay are placed from. The step per notch is a fraction of
    // what is on screen, so it has to come off the same two numbers or a notch
    // would mean something other than what it looks like it means.
    request.span_hz = spanHighHz() - spanLowHz();

    // Where the radio landed and not where it was last asked to go. A device
    // with a tuning step rounds, so adding the step to the request would
    // accumulate the rounding error over a sweep.
    request.center_hz = sourceCenterHz();

    request.tune_low_hz = static_cast<double>(source_tune_low_);
    request.tune_high_hz = static_cast<double>(source_tune_high_);
    request.now_ms = static_cast<double>(scroll_clock_.elapsed());

    const ScrollTunePlan plan = plan_scroll_tune(scroll_tune_, request);
    scroll_tune_ = plan.state;
    if (plan.tune) {
        tuneSourceHz(plan.center_hz);
    }
    arm_scroll_flush(plan.wait_ms);
}

void EngineLink::flush_scroll_tune() { takeScrollTune(0.0); }

void EngineLink::arm_scroll_flush(double wait_ms)
{
    if (wait_ms > 0.0) {
        // Rounded up, because a timer that fires a fraction of a millisecond
        // early finds the interval not yet elapsed, does nothing, and re-arms
        // for the remainder. One extra wakeup per burst rather than two.
        scroll_flush_.start(static_cast<int>(std::ceil(wait_ms)));
        return;
    }
    scroll_flush_.stop();
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

        // Ids from the last connection's engine, which name nothing here.
        handover_tune_removed_.clear();
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
    auto granted = seam_retune_source(*client_, target);

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
        handover_tune_granted_ = granted->granted_hz;
        handover_tune_answered_ = true;

        // Appended rather than assigned: two tunes can be answered before the
        // Qt thread adopts either, and a removal the first one reported is
        // still a removal.
        handover_tune_removed_.insert(handover_tune_removed_.end(), granted->removed.begin(),
                                      granted->removed.end());

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

    // ON THE BACK OF THE ROUND TRIP THIS FUNCTION ALREADY PAYS FOR. The epoch
    // lives on EngineInfo and this is the one call that fetches EngineInfo on
    // every pass, so noticing a source change here costs nothing and a poll of
    // its own would cost a round trip a second forever.
    note_source_epoch(*info);

    // On the back of the same round trip, and here rather than in the
    // supervisor's own list, because this is where the epoch is in hand on
    // THIS thread. info_ is the Qt thread's and reading it from the supervisor
    // to get the same number was a data race.
    poll_source_gain_stage(info->source_epoch);
    poll_open_source(info->source_epoch);

    PacingSample sample;
    sample.carried = true;
    sample.realtime_factor = seam_realtime_factor(*info).value_or(0.0);
    sample.window_seconds = seam_realtime_window_seconds(*info).value_or(0.0);
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
        sample.window_seconds == posted_pacing_.window_seconds &&
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

void EngineLink::poll_front_end(bool engine_running)
{
    if (client_ == nullptr) {
        return;
    }

    auto stats = client_->source_stats();
    if (!stats) {
        // NOT REPORTED HERE, on the argument poll_source_pacing makes: a
        // stats call failing on a live connection is the connection going,
        // and the probe above this is what says so.
        return;
    }

    note_samples_delivered(stats->samples_delivered);

    FrontEndSample sample;
    sample.engine_running = engine_running;

    // Ordinal for ordinal with rpc::FrontEndState, and asserted here rather
    // than trusted. core/rpc/convert.h holds the schema against the engine
    // and core/rpc/client.cpp holds it against rpc::FrontEndState; this is
    // the third pair, and it is the one a display would get wrong silently
    // by drawing a gain change as a front end in trouble.
    static_assert(static_cast<std::uint8_t>(rpc::FrontEndState::Unmeasured) ==
                  static_cast<std::uint8_t>(FrontEndSample::State::Unmeasured));
    static_assert(static_cast<std::uint8_t>(rpc::FrontEndState::Steady) ==
                  static_cast<std::uint8_t>(FrontEndSample::State::Steady));
    static_assert(static_cast<std::uint8_t>(rpc::FrontEndState::SpanScales) ==
                  static_cast<std::uint8_t>(FrontEndSample::State::SpanScales));
    static_assert(static_cast<std::uint8_t>(rpc::FrontEndState::FloorFollowsSignal) ==
                  static_cast<std::uint8_t>(FrontEndSample::State::FloorFollowsSignal));
    sample.state = static_cast<FrontEndSample::State>(stats->front_end);
    sample.slope = stats->front_end_slope;
    sample.floor_lift_db = stats->front_end_floor_lift_db;

    // Posted only on a change, the same as the pacing sample and for the
    // same reason. The comparison is exact: both numbers move continuously
    // while the verdict holds, so a tolerance here would freeze the figures
    // inside the sentence while the sentence itself stayed true.
    if (sample.state == posted_front_end_.state &&
        sample.slope == posted_front_end_.slope &&
        sample.floor_lift_db == posted_front_end_.floor_lift_db &&
        sample.engine_running == posted_front_end_.engine_running) {
        return;
    }
    posted_front_end_ = sample;

    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        handover_has_front_end_ = true;
        handover_front_end_ = sample;
    }
    QMetaObject::invokeMethod(this, [this] { adopt_front_end(); }, Qt::QueuedConnection);
}

void EngineLink::adopt_front_end()
{
    FrontEndSample sample;
    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        if (!handover_has_front_end_) {
            return;
        }
        handover_has_front_end_ = false;
        sample = handover_front_end_;
    }

    front_end_ = sample;
    front_end_text_ = QString::fromStdString(front_end_sentence(sample));

    // pacingChanged and not a signal of its own. One status strip reads
    // both, neither repaints a display, and a second signal fired off the
    // same probe pass would wake the GUI thread twice to redraw one line.
    emit pacingChanged();
}

void EngineLink::adopt_source_tuning()
{
    bool geometry_moved = false;
    bool anything = false;

    // A tune came back with an answer of either kind, which is when a bookmark
    // waiting on one gets to stop waiting. Read out of the locked block below
    // rather than off tune_answered_ afterwards, because that member is also true
    // from an earlier tune nobody is waiting on.
    bool answered_a_tune = false;
    std::vector<rpc::RetuneRemoval> removed;

    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        removed.swap(handover_tune_removed_);

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
            answered_a_tune = true;
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

    // THE PANE'S RECEIVER, IF THE ENGINE SAYS THE RETUNE REMOVED IT. The answer
    // names it, the frequency it was on and why, so the sentence states the
    // engine's cause rather than inferring one from the span, and the
    // frequency is the engine's rather than this window's record of the tune.
    // A receiver refused only for its filter shape is offered back at that
    // frequency, which is the add the engine's refusal asks for. Before a
    // recall below places a receiver, so the one torn down is the one that
    // went. The inventory poll's own path, forget_removed_receiver, finds
    // receiver_id_ already moved on and does nothing.
    for (const rpc::RetuneRemoval& gone : removed) {
        if (receiver_id_ != 0 && gone.id == static_cast<std::uint64_t>(receiver_id_)) {
            receiver_gone_text_ = QString::fromStdString(
                receiver_retuned_away_sentence(gone.frequency_hz, gone.cause, gone.reason));
            receiver_comeback_hz_ = receiver_can_come_back(gone.cause) ? gone.frequency_hz : 0;
            emit receiverGoneChanged();
            removeReceiver();
            break;
        }
    }

    // And any held receiver in the rack the retune took, which goes from the
    // rack with a line saying where it was. Its strip is gone, so the line is
    // the rack's.
    for (const rpc::RetuneRemoval& gone : removed) {
        for (const RackEntry& entry : rack_.entries()) {
            if (entry.key != pane_key_ && entry.engine_id != 0 &&
                entry.engine_id == static_cast<qulonglong>(gone.id)) {
                const std::uint64_t key = entry.key;
                set_rack_note(QString::fromStdString(receiver_gone_sentence(
                    gone.frequency_hz, spanLowHz(), spanHighHz())));
                removeRackReceiver(key);
                break;
            }
        }
    }

    // A bookmark waiting on this retune, placed now that source_center holds the
    // centre the radio actually landed on. THIS IS THE ONLY MOMENT IT IS RIGHT:
    // recallBookmark could not do it, because tuneSourceHz had not been answered
    // when it returned, and a receiver placed then would have been offset from
    // the old centre. Before the geometry emits below, so the pane and the
    // history move in one turn.
    if (answered_a_tune) {
        resolve_pending_recall(tune_answered_ && tune_fault_.isEmpty());
    }

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

// ---------------------------------------------------------------------------
// The device picker
// ---------------------------------------------------------------------------

namespace {

// One descriptor as the panel reads it.
//
// THE DERIVED FIELDS ARE COMPUTED HERE AND NOT IN QML, which is the split
// every other computed thing in this client takes: ui/models/source_choice.h
// owns the rules, ui/tests asserts them, and the QML binds names. A tuning
// envelope composed in JavaScript would be a second copy of the rule that an
// inverted range describes nothing, in a language with no test behind it.
[[nodiscard]] QVariantMap describe(const rpc::SourceDescriptor& source)
{
    QVariantMap out;
    out.insert(QStringLiteral("uri"), QString::fromStdString(source.uri));
    out.insert(QStringLiteral("backend"), QString::fromStdString(source.backend));
    out.insert(QStringLiteral("displayName"), QString::fromStdString(source.display_name));
    out.insert(QStringLiteral("unavailable"), QString::fromStdString(source.unavailable));
    out.insert(QStringLiteral("available"), source.available());

    QStringList notes;
    notes.reserve(static_cast<qsizetype>(source.notes.size()));
    for (const std::string& note : source.notes) {
        notes.append(QString::fromStdString(note));
    }
    out.insert(QStringLiteral("notes"), notes);

    const TuneEnvelope envelope = tune_envelope(source);
    out.insert(QStringLiteral("tunable"), envelope.tunable);
    out.insert(QStringLiteral("tuneLowHz"), static_cast<double>(envelope.low_hz));
    out.insert(QStringLiteral("tuneHighHz"), static_cast<double>(envelope.high_hz));

    out.insert(QStringLiteral("minRate"), static_cast<double>(source.min_rate));
    out.insert(QStringLiteral("maxRate"), static_cast<double>(source.max_rate));

    QVariantList rates;
    rates.reserve(static_cast<qsizetype>(source.sample_rates.size()));
    for (const std::uint32_t rate : source.sample_rates) {
        rates.append(static_cast<double>(rate));
    }
    out.insert(QStringLiteral("sampleRates"), rates);

    out.insert(QStringLiteral("format"),
               QString::fromLatin1(rpc::sample_format_name(source.native_format)));
    out.insert(QStringLiteral("bitsPerComponent"), source.bits_per_component);

    // ONE STAGE AND NOT THE LIST, because compose_source_uri emits one gain
    // key: it is singular in every grammar this can reach, and the RTL-SDR has
    // exactly one stage. Publishing three while only the first could be set
    // would offer an operator two controls that do nothing. A device with
    // several needs per-stage keys on the wire first, and the panel grows then.
    out.insert(QStringLiteral("hasGain"), !source.gain_stages.empty());
    if (!source.gain_stages.empty()) {
        const rpc::GainStage& stage = source.gain_stages.front();
        out.insert(QStringLiteral("gainName"), QString::fromStdString(stage.name));
        out.insert(QStringLiteral("gainMinDb"), stage.min_db);
        out.insert(QStringLiteral("gainMaxDb"), stage.max_db);
        out.insert(QStringLiteral("gainHasAuto"), stage.has_auto);
        out.insert(QStringLiteral("gainStepped"), !stage.steps_db.empty());
    }

    out.insert(QStringLiteral("paced"), source.flow == rpc::FlowControl::Paced);
    out.insert(QStringLiteral("seekable"), source.seekable);
    out.insert(QStringLiteral("length"), QString::fromStdString(describe_length(source)));
    return out;
}

}  // namespace

void EngineLink::refreshSources()
{
    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        want_listing_ = true;
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        source_work_pending_ = true;
    }
    supervisor_wake_.notify_all();

    // Set on this thread rather than waiting for the supervisor to say so,
    // because the whole point of the flag is that listSources is slow: it opens
    // every device index to ask, including ones with nothing behind them, and
    // pays a libusb timeout for each absent one. A busy flag that arrived with
    // the answer would light up for no time at all.
    sources_busy_ = true;
    emit sourcesChanged();
}

void EngineLink::openSource(const QString& uri)
{
    if (uri.isEmpty()) {
        source_fault_ =
            QStringLiteral("there is nothing to open: pick a device in the list first.");
        emit sourcesChanged();
        return;
    }

    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        wanted_uri_ = uri;
        want_open_ = true;

        // ALWAYS, AND NOT ONLY WHEN ONE IS OPEN. openSource is refused over a
        // live source, deliberately: a replace that failed on the new URI would
        // have destroyed the working one already, and the ENGINE cannot know
        // whether a caller meant to replace. This window can. An operator who
        // picked a device in the panel meant to change to it, so the close and
        // the open are sequenced here. Closing an engine with nothing open is a
        // success, so asking when there is nothing to close costs one message,
        // and guessing wrong costs a refused open that reads as a broken panel.
        want_close_ = true;
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        source_work_pending_ = true;
    }
    supervisor_wake_.notify_all();

    source_fault_.clear();
    emit sourcesChanged();

    // A refused pace was about the source this open replaces.
    if (!pace_fault_.isEmpty()) {
        pace_fault_.clear();
        emit pacingChanged();
    }
}

void EngineLink::closeSource()
{
    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        want_close_ = true;

        // An open posted and not yet applied is dropped. The operator changed
        // their mind between the two gestures, and applying both would open the
        // device they just asked to close.
        want_open_ = false;
        wanted_uri_.clear();
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        source_work_pending_ = true;
    }
    supervisor_wake_.notify_all();

    source_fault_.clear();
    emit sourcesChanged();
}

QString EngineLink::composeSourceUri(int index, const QString& center, const QString& rate,
                                     const QString& gain_db, bool gain_auto) const
{
    if (index < 0 || static_cast<std::size_t>(index) >= source_rows_.size()) {
        return {};
    }
    const rpc::SourceDescriptor& source = source_rows_[static_cast<std::size_t>(index)];

    SourceChoice choice;

    // A BOX THAT PARSES BECOMES A VALUE AND ONE THAT DOES NOT STAYS ABSENT,
    // which covers empty and covers junk with the same answer: leave the key off
    // and let the backend decide. See SourceChoice for what emitting zero cost.
    //
    // The centre and the rate take DIFFERENT bare-number rules, which is why
    // they are parsed separately rather than through one helper. See
    // ui/models/frequency_entry.h's BareNumber.
    if (const auto parsed = parse_frequency(center.toStdString())) {
        choice.center_hz = parsed->hertz;
    }
    if (const auto parsed = parse_frequency(rate.toStdString(), BareNumber::Hertz)) {
        choice.rate = parsed->hertz;
    }

    if (!source.gain_stages.empty()) {
        GainChoice gain;
        gain.stage = source.gain_stages.front().name;
        gain.automatic = gain_auto;

        // Its own parse rather than parse_frequency: a gain is decibels, it can
        // be negative on some stages, and it is not a frequency. std::from_chars
        // on the whole string, so trailing junk is a refusal and not a prefix
        // quietly accepted.
        const std::string text = gain_db.trimmed().toStdString();
        if (!text.empty()) {
            double value = 0.0;
            const char* first = text.data();
            const char* last = first + text.size();
            const auto result = std::from_chars(first, last, value);
            if (result.ec == std::errc{} && result.ptr == last && std::isfinite(value)) {
                gain.db = value;
            }
        }
        choice.gains.push_back(std::move(gain));
    }
    return QString::fromStdString(compose_source_uri(source, choice));
}

void EngineLink::apply_source_request()
{
    if (client_ == nullptr) {
        return;
    }

    // THE FLAG IS CLEARED BEFORE THE REQUEST IS TAKEN, which is the order
    // apply_source_tune and apply_receiver_request already use and for the
    // reason apply_source_tune states: the other way round, a request posted
    // between the take and the clear sets both, and then this clears the flag
    // the wake depended on.
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        source_work_pending_ = false;
    }

    bool listing = false;
    bool closing = false;
    bool opening = false;
    QString uri;
    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        listing = std::exchange(want_listing_, false);
        closing = std::exchange(want_close_, false);
        opening = std::exchange(want_open_, false);
        uri = wanted_uri_;
        if (opening) {
            wanted_uri_.clear();
        }
    }

    if (!listing && !closing && !opening) {
        return;
    }

    QString fault;
    bool have_fault = false;
    bool have_sources = false;
    std::vector<rpc::SourceDescriptor> listed;

    if (listing) {
        auto answer = client_->list_sources();
        if (answer) {
            have_sources = true;
            listed = std::move(*answer);
        } else {
            have_fault = true;
            fault = QString::fromStdString(answer.error().message);
        }
    }

    // The close, then the open, in that order and never the reverse: the engine
    // refuses an open over a live source, so an open that ran first would be
    // refused and a close after it would then leave nothing running.
    if (closing) {
        if (auto closed = client_->close_source(); !closed) {
            have_fault = true;
            fault = QString::fromStdString(closed.error().message);

            // AND THE OPEN IS ABANDONED. A close that failed left the old
            // source running, so the open would be refused for a reason that
            // says nothing about the device the operator picked, and that
            // second message would replace the first one that explained it.
            opening = false;
        }
    }

    if (opening) {
        if (auto opened = client_->open_source(uri.toStdString()); !opened) {
            have_fault = true;
            fault = QString::fromStdString(opened.error().message);
        } else {
            // The range answer belongs to the source that was just closed. Ask
            // again now rather than waiting for the next connection, or the
            // frequency box keeps the previous radio's stops and refuses
            // locally, in this client's own words, a tune the new one would
            // have taken.
            probe_source_tuning();
        }
    }

    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        handover_sources_busy_ = false;
        if (have_sources) {
            handover_has_sources_ = true;
            handover_sources_ = std::move(listed);
        }
        handover_has_source_fault_ = true;
        handover_source_fault_ = have_fault ? fault : QString();
    }

    QMetaObject::invokeMethod(
        this, [this] { adopt_sources(); }, Qt::QueuedConnection);
}

void EngineLink::note_source_epoch(const rpc::EngineInfo& info)
{
    if (info.source_epoch == seen_source_epoch_) {
        return;
    }

    const bool first = seen_source_epoch_ == 0;
    seen_source_epoch_ = info.source_epoch;
    if (first) {
        // The epoch this connection opened on. Nothing changed under this
        // window; it is only seeing the number for the first time.
        return;
    }

    // A NEW STREAM ON A CONNECTION THAT NEVER DROPPED, WHICH IS A STATE THIS
    // WINDOW COULD NOT BE IN BEFORE closeSource EXISTED
    //
    // Everything below already happens when the ENGINE goes away, because the
    // Client is destroyed and rebuilt. Here the connection is fine and the
    // stream underneath it was replaced, so nothing tears itself down and the
    // window goes on drawing the previous radio: a frozen waterfall under a
    // frequency axis taken from a grid that no longer exists.
    //
    // Measured. Changing radio through the picker left "103 frames sent" on
    // the engine's status line and a waterfall that stopped, because the
    // server ended every subscription with the source and this client had no
    // reason to ask for another.
    live_receiver_id_ = 0;
    live_pane_key_ = 0;
    forget_held(QStringLiteral("the source was replaced, and its receivers went with it"));
    forget_audio();
    forget_decoded();
    clear_rds(QStringLiteral(
        "the source was replaced, so nothing is decoding RDS. The switch stays on and the "
        "decoder is rebuilt on the next receiver."));

    // The detector went with the source and its tracks were measured against
    // the old centre. A held refusal goes too, or the next source inherits a
    // sentence about a band it was never pointed at.
    clear_detection_fault();

    // AND THE DESCRIPTORS WENT WITH IT, WHICH IS THE ONE THING A REPLACED
    // SOURCE USED TO LEAVE STANDING.
    //
    // Every row the picker holds was described against the world as it was
    // before the switch, and source::describe_sources OPENS each device to
    // answer: a dongle this engine is now streaming from lists as available,
    // one the previous source held lists as busy, and the rate bounds, gain
    // stages and tuning envelope compose_source_uri settles a request against
    // are the ones that applied a source ago. Measured 2026-09-21: opening an
    // RTL-SDR over a synthetic scene moved the frequency axis, enabled the tune
    // box and resumed frames, and left every row in the panel describing the
    // devices as they had been before the switch, for as long as the window
    // stayed up. The panel is inline rather than modal on purpose, so it is
    // still on screen and still under the operator's cursor at exactly the
    // moment its rows stop being true.
    //
    // THAT LIST IS THE WHOLE OF WHERE A DESCRIPTOR REACHES THE SCREEN. Nothing
    // under ui/qml names the source that is OPEN: the identity an
    // operator reads after a switch is the row they picked in the panel, and it
    // is a row from the stale listing. So the listing going stale is not a
    // picker detail, it is the window's only statement about what it is
    // receiving from.
    //
    // It looked like a signal that never fired and was not one. The adopt
    // below emits sourcesChanged, so every binding on the list DID re-read; it
    // re-read the same descriptors, because nothing between this function and
    // adopt_sources asks the engine for another listing. probe_source_tuning
    // covers the tuning half on the open path and there was no equivalent for
    // this half anywhere.
    //
    // ASKED FOR RATHER THAN FETCHED HERE, on the flag refreshSources sets.
    // listSources opens every device index to answer and pays a libusb timeout
    // per absent dongle, and this function is in the middle of re-establishing
    // the spectrum subscription: paying that inline would hold the display
    // unfed for as long as the slowest absent device takes to refuse.
    // apply_source_request already owns the call, and it runs first on the pass
    // after this one. No wake is posted, because this IS the supervisor thread
    // and the wait it is about to reach has source_work_pending_ in its
    // predicate.
    //
    // A failed re-subscribe below rewinds seen_source_epoch_ so the next pass
    // comes through here again, which asks for the listing again with it. That
    // is one round trip per pass on a path that is already retrying, and it is
    // cheaper than a second flag kept only to remember that the ask was made.
    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        want_listing_ = true;
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        source_work_pending_ = true;
    }

    // THE OLD SUBSCRIPTION IS DROPPED AND NOT RE-USED. The server ended it
    // with the source, so the capability this client holds is dead; asking for
    // a new one without dropping the old leaves this side thinking it has two.
    client_->unsubscribe_spectrum();

    // BETWEEN THE UNSUBSCRIBE AND THE RESUBSCRIBE, WHICH IS THE ONLY WINDOW
    // THESE FOUR CAN BE WRITTEN IN.
    //
    // on_frame reads all four and runs on the CLIENT's event loop thread, not
    // this one. Writing them with a subscription live is a data race, and the
    // visible half of it is worse than the tearing: first_sequence_ set to zero
    // after a frame had already established it makes the very next frame look
    // like a jump of its whole sequence number, and engine_link.cpp turns a
    // sequence jump into framesDroppedByEngine. The window would report
    // hundreds of thousands of dropped frames on the first frame of every new
    // source.
    //
    // attempt_connect writes the same four on this thread and is safe for the
    // same reason rather than by luck: the Client has just been built and
    // nothing is subscribed yet.
    //
    // Everything the two engine-side counters are derived from, back to zero. A
    // sequence from the stream that just ended has nothing to say about the
    // distance to a sequence from the one that replaced it.
    every_nth_ = requested_every_nth_;
    first_sequence_ = 0;
    delivered_ = 0;
    have_span_ = false;

    {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        handover_info_ = info;
        handover_connected_ = true;
    }

    if (info.spectrum.enabled()) {
        // A failure is not fatal and is not silent either. The source is open
        // and everything but the display works; the next pass tries again,
        // because this function runs whenever the epoch differs from the one
        // this window last drew against and a failed subscribe leaves that
        // difference in place.
        const auto status = client_->subscribe_spectrum(
            requested_every_nth_,
            [this](const rpc::SpectrumFrame& frame) { on_frame(frame); });
        if (!status) {
            seen_source_epoch_ = 0;
            const std::lock_guard<std::mutex> lock(source_mutex_);
            handover_has_source_fault_ = true;
            handover_source_fault_ =
                QStringLiteral("the source opened and the spectrum subscription did not, so "
                               "the display is not being fed: ") +
                QString::fromStdString(status.error().message);
        }
    }

    QMetaObject::invokeMethod(
        this,
        [this] {
            // adopt(), which emits connectionChanged, and that is the point.
            // render/waterfall_item.cpp and render/spectrum_item.cpp read that
            // signal as a new engine: the waterfall fills its ring with
            // background and the trace forgets its bin count. A new source IS
            // a new engine to both of them, because every row they hold was
            // drawn against a grid that has been replaced.
            adopt();
            emit sourcesChanged();
        },
        Qt::QueuedConnection);
}

void EngineLink::adopt_sources()
{
    bool moved = false;
    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        if (handover_has_sources_) {
            handover_has_sources_ = false;
            source_rows_ = std::move(handover_sources_);
            handover_sources_.clear();

            sources_.clear();
            sources_.reserve(static_cast<qsizetype>(source_rows_.size()));
            for (const rpc::SourceDescriptor& source : source_rows_) {
                sources_.append(describe(source));
            }
            moved = true;
        }
        if (handover_has_source_fault_) {
            handover_has_source_fault_ = false;
            if (source_fault_ != handover_source_fault_) {
                source_fault_ = handover_source_fault_;
                moved = true;
            }
        }
        if (sources_busy_ != handover_sources_busy_) {
            sources_busy_ = handover_sources_busy_;
            moved = true;
        }
    }

    if (moved) {
        emit sourcesChanged();
    }
}

// ---------------------------------------------------------------------------
// The front end's gain
// ---------------------------------------------------------------------------

void EngineLink::setSourceGainFraction(double fraction)
{
    // Settled here, on the Qt thread, so the number posted is one the stage
    // actually has and the readout below can be compared against the answer.
    // The alternative is posting a fraction and settling on the supervisor,
    // which would leave the Qt thread unable to say what it asked for.
    const double asked = gain_request_for_fraction(gain_stage_, fraction);

    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        want_gain_ = true;
        want_gain_db_ = asked;
        want_gain_stage_ = gain_stage_.name;
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        source_work_pending_ = true;
    }
    supervisor_wake_.notify_all();

    // THE HANDLE MOVES NOW AND THE READOUT WAITS. A slider that only moved
    // when the engine answered would move at the round-trip rate, and on an
    // RTL-SDR that round trip includes about a third of a second with the
    // transfers stopped, so the control would feel broken. What must not move
    // early is gain_known_: until the device has answered, this window does
    // not know what gain the tuner is on, and showing the request as a reading
    // is the lie this whole pair of properties exists to avoid.
    gain_db_ = asked;
    gain_auto_ = false;
    emit sourceGainChanged();
}

void EngineLink::setSourceGainAuto(bool on)
{
    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        want_gain_auto_ = on;
        want_gain_auto_set_ = true;
        want_gain_stage_ = gain_stage_.name;
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        source_work_pending_ = true;
    }
    supervisor_wake_.notify_all();

    gain_auto_ = on;

    // The device is choosing now, so whatever number this window last had is
    // not what the tuner is on. Saying nothing is the honest state until the
    // operator takes manual control back and gets an answer.
    if (on) {
        gain_known_ = false;
    }
    emit sourceGainChanged();
}

void EngineLink::apply_source_gain()
{
    if (client_ == nullptr) {
        return;
    }

    bool wanted = false;
    double db = 0.0;
    bool auto_set = false;
    bool automatic = false;
    std::string stage;
    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        wanted = std::exchange(want_gain_, false);
        db = want_gain_db_;
        auto_set = std::exchange(want_gain_auto_set_, false);
        automatic = want_gain_auto_;

        // THE STAGE NAME TRAVELS WITH THE REQUEST, under this lock, and is not
        // read off gain_stage_. gain_stage_ belongs to the Qt thread, which
        // replaces it whole whenever a descriptor arrives, and reading its
        // std::string from this thread was a use-after-free that crashed the
        // window: the Qt thread reassigning the name frees the buffer this one
        // is copying out of. The Qt thread knows the name when it posts, so it
        // sends it.
        stage = want_gain_stage_;
    }

    if (!wanted && !auto_set) {
        return;
    }

    // An empty name is a request posted before any descriptor arrived, which
    // has nothing to name and nothing to set.
    if (stage.empty()) {
        return;
    }

    QString fault;
    double granted = db;
    bool granted_known = false;

    // AUTO FIRST WHEN BOTH ARE PENDING, because taking manual control back is
    // expressed as a gain change: set_gain puts the tuner into manual mode on
    // the way to setting a value, so a gain applied after an auto request
    // would undo it, and an operator who dragged the slider while auto was on
    // means to take control.
    if (auto_set) {
        if (auto applied = client_->set_source_gain_auto(stage, automatic); !applied) {
            fault = QString::fromStdString(applied.error().message);
        } else if (automatic) {
            granted_known = false;
        }
    }

    if (wanted && fault.isEmpty()) {
        auto answer = client_->set_source_gain(stage, db);
        if (!answer) {
            fault = QString::fromStdString(answer.error().message);
        } else {
            granted = *answer;
            granted_known = true;
        }
    }

    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        handover_has_gain_ = true;
        handover_gain_db_ = granted;
        handover_gain_auto_ = auto_set ? automatic : false;
        handover_gain_fault_ = fault;
    }
    QMetaObject::invokeMethod(this, [this, granted_known] { adopt_gain(granted_known); },
                              Qt::QueuedConnection);
}

void EngineLink::poll_source_gain_stage(std::uint64_t epoch)
{
    if (client_ == nullptr) {
        return;
    }

    // ONCE PER SOURCE AND NOT ONCE PER PASS. sourceDescriptor touches no
    // device, which is the whole difference between it and listSources, but it
    // is still a round trip and a stage cannot change under a source that has
    // not been reopened. The epoch is what says a source was.
    //
    // THE EPOCH IS HANDED IN AND NOT READ OFF info_. info_ is Qt thread only,
    // this runs on the supervisor, and reading it here was a data race on a
    // struct holding a std::string. poll_source_pacing already fetches an
    // EngineInfo on this thread, so the number comes from there.
    if (gain_stage_read_ && epoch == gain_stage_epoch_) {
        return;
    }

    auto described = client_->source_descriptor();
    if (!described) {
        // Left to the next pass. The liveness probe owns a lost engine, and a
        // missing gain control is not worth a fault of its own.
        return;
    }

    gain_stage_epoch_ = epoch;
    gain_stage_read_ = true;

    rpc::GainStage stage;
    int count = 0;
    if (described->has_value()) {
        const rpc::SourceDescriptor& open = **described;
        count = static_cast<int>(open.gain_stages.size());
        if (count > 0) {
            // The first stage and not a search by name. A device reports its
            // stages in its own order and an R820T reports exactly one; naming
            // "tuner" here would be this client deciding what a device calls
            // its own control, which is what GainStage::name exists to avoid.
            stage = open.gain_stages.front();
        }
    }

    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        handover_has_gain_stage_ = true;
        handover_gain_stage_ = stage;
        handover_gain_stage_count_ = count;
    }
    QMetaObject::invokeMethod(this, [this] { adopt_gain_stage(); }, Qt::QueuedConnection);
}

void EngineLink::adopt_gain_stage()
{
    rpc::GainStage stage;
    int count = 0;
    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        if (!handover_has_gain_stage_) {
            return;
        }
        handover_has_gain_stage_ = false;
        stage = handover_gain_stage_;
        count = handover_gain_stage_count_;
    }

    const bool same = stage.name == gain_stage_.name && stage.min_db == gain_stage_.min_db &&
                      stage.max_db == gain_stage_.max_db && count == gain_stage_count_;
    gain_stage_ = std::move(stage);
    gain_stage_count_ = count;

    // A NEW SOURCE HAS NOT BEEN ASKED FOR A GAIN FROM HERE. Carrying the last
    // source's number over would put a reading on the handle for a device that
    // has never been told anything, which is the one thing sourceGainKnown is
    // for.
    if (!same) {
        gain_known_ = false;
        gain_auto_ = false;
        gain_db_ = gain_stage_.min_db;
        gain_fault_.clear();
    }

    emit sourceGainChanged();
}

void EngineLink::adopt_gain(bool granted_known)
{
    double db = 0.0;
    bool automatic = false;
    QString fault;
    {
        const std::lock_guard<std::mutex> lock(source_mutex_);
        if (!handover_has_gain_) {
            return;
        }
        handover_has_gain_ = false;
        db = handover_gain_db_;
        automatic = handover_gain_auto_;
        fault = handover_gain_fault_;
    }

    gain_fault_ = fault;
    if (fault.isEmpty()) {
        gain_auto_ = automatic;
        if (granted_known) {
            // THE HANDLE GOES WHERE THE DEVICE LANDED. On a stepped stage this
            // is rarely what was asked, so this is the assignment that stops
            // the slider showing a gain the tuner is not on.
            gain_db_ = db;
            gain_known_ = true;
        }
    }
    emit sourceGainChanged();
}

}  // namespace revenant::ui
