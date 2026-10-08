// EngineLink's side of the engine this window started, and of the radio used
// last.
//
// WHY THERE IS A SEPARATE FILE. engine_link.cpp is the connection, the frame
// path and the rate clock; source_link.cpp is the picker and the front end.
// This is what the window says and does because it started the engine itself,
// which is neither: main() starts the process (models/engine_launcher.h), and
// this object only hears that it did and what became of it. The rules it acts
// on are in models/engine_start.h with their cases in ui/tests.

#include "models/engine_link.h"

#include <chrono>
#include <cstdint>

#include <QSettings>
#include <QString>

#include "models/engine_start.h"
#include "models/settings.h"

namespace revenant::ui {
namespace {

// How long a start may take before the window says it is taking long. The
// first start on a machine is the slow one, Vulkan finding the GPU and the
// engine minting its token, and was a few seconds on the development machine;
// thirty is far enough past that that a start still going is stuck rather
// than slow.
//
// WHAT HAPPENS AT IT IS ONLY A CHANGE OF WORDS. The engine is still alive,
// still in this window's job and still being tried once a second; the
// sentence gains whatever the connection attempts are failing on, so a start
// that is stuck on something this side can see, a token that does not match,
// says what rather than "starting" for ever.
constexpr std::chrono::seconds kEngineStartPatience{30};

}  // namespace

void EngineLink::setEngineStarted(std::uint32_t pid)
{
    engine_start_ = EngineStart::Starting;
    engine_pid_ = pid;
    engine_start_slow_ = false;
    engine_start_text_.clear();

    engine_start_patience_.setSingleShot(true);
    engine_start_patience_.setInterval(kEngineStartPatience);
    connect(&engine_start_patience_, &QTimer::timeout, this, [this] {
        if (engine_start_ == EngineStart::Starting) {
            engine_start_slow_ = true;
            emit engineStartChanged();
        }
    });
    engine_start_patience_.start();

    // Connected here and not in the constructor, so a window that started
    // nothing does not run it at all: every failed reconnect emits
    // connectionChanged, once a second for as long as an outage lasts, and
    // each of those calls would decide nothing.
    connect(this, &EngineLink::connectionChanged, this, &EngineLink::consider_last_source);

    emit engineStartChanged();
}

void EngineLink::setEngineStartFailed(const QString& sentence)
{
    if (engine_start_ == EngineStart::Failed) {
        return;
    }
    engine_start_ = EngineStart::Failed;
    engine_start_text_ = sentence;
    engine_start_patience_.stop();
    emit engineStartChanged();
}

void EngineLink::setRememberLastSource(bool remember) { remember_last_source_ = remember; }

bool EngineLink::engineStarting() const
{
    return engine_start_ == EngineStart::Starting && !connected_;
}

bool EngineLink::engineStartFailed() const { return engine_start_ == EngineStart::Failed; }

QString EngineLink::engineStartText() const
{
    if (engine_start_ == EngineStart::Failed) {
        return engine_start_text_;
    }
    if (engine_start_ != EngineStart::Starting) {
        return {};
    }
    if (!engine_start_slow_) {
        return QStringLiteral("starting the engine at %1").arg(endpoint_);
    }

    QString text = QStringLiteral("the engine this window started (process %1) is running and "
                                  "has not answered at %2 in %3 s")
                       .arg(engine_pid_)
                       .arg(endpoint_)
                       .arg(static_cast<long long>(kEngineStartPatience.count()));

    // The connection's own reason, unless it is the sentence describe_failure
    // in engine_link.cpp gives an unreachable port, which is this same fact in
    // fewer words and would read twice.
    if (!error_text_.isEmpty() &&
        error_text_ != QStringLiteral("waiting for an engine at %1").arg(endpoint_)) {
        text += QStringLiteral(": ") + error_text_;
    }
    return text;
}

// WHICH SOURCELESS ENGINES GET THE RADIO USED LAST: ONLY THE ONE THIS WINDOW
// STARTED, AND ONLY ON ITS FIRST CONNECTION.
//
// The brief left this open. The wider rule, any engine with no source, reads
// as more helpful and is wrong three ways:
//
//   An engine somebody else started with no source may be that way on
//   purpose. A headless engine waiting for its own controlling client, or one
//   whose operator just closed the source from another window, is somebody's
//   decision, and the owner's rule is that an engine somebody started
//   themselves is never touched.
//
//   The remembered URI names a device on THIS machine. rtlsdr://0 on a remote
//   engine is that machine's first dongle, which is a different radio under
//   the same words; opening it would claim a band nobody chose.
//
//   A reconnect is a connection too. Reopening on every connection to a
//   sourceless engine would reopen the radio an operator had just closed, the
//   next time the link blinked.
//
// An engine this window started with --no-source has none of those problems:
// nobody else has had it, it is on this machine, and the first connection is
// the moment it is known to have no source because it was started without
// one. Its source_open is still read rather than assumed, in case a second
// window raced this one to it.
void EngineLink::consider_last_source()
{
    // errorText is part of the slow sentence and moves on connectionChanged.
    if (engine_start_ == EngineStart::Starting && engine_start_slow_) {
        emit engineStartChanged();
    }

    if (!connected_ || last_source_considered_ || engine_start_ != EngineStart::Starting) {
        return;
    }
    last_source_considered_ = true;
    engine_start_ = EngineStart::Reached;
    engine_start_patience_.stop();
    emit engineStartChanged();

    const QString remembered =
        remember_last_source_ ? QSettings().value(settings::kLastSource).toString() : QString();

    LastSourceFacts facts;
    facts.engine_ours = true;
    facts.source_open = sourceOpen();
    facts.open_already_asked = open_asked_;
    facts.have_remembered = !remembered.isEmpty();

    switch (plan_last_source(facts)) {
        case LastSourceAction::Nothing:
            return;
        case LastSourceAction::Reopen:
            // The radio comes back as it was left, not as it was opened.
            reopening_uri_ = restored_last_source(remembered);
            openSource(reopening_uri_);
            return;
        case LastSourceAction::ShowPicker:
            emit sourcePickerWanted();
            return;
    }
}

void EngineLink::note_open_answer(const QString& uri, bool ok)
{
    const bool was_reopen = !reopening_uri_.isEmpty() && uri == reopening_uri_;
    if (was_reopen) {
        reopening_uri_.clear();
    }

    if (ok) {
        // Every open that worked, from the picker or from the reopen, and
        // never a recording; see remembers_as_last_radio.
        if (remember_last_source_ && remembers_as_last_radio(uri.toStdString())) {
            QSettings().setValue(settings::kLastSource, uri);
        }
        return;
    }

    // A REFUSED REOPEN CLEARS NOTHING. The radio may be unplugged today and
    // back tomorrow, and forgetting it on one refusal would make the person
    // pick it again for no reason. The picker opens so they can pick another
    // now; the engine's refusal is already in sourceFault, on that panel.
    if (was_reopen) {
        emit sourcePickerWanted();
    }
}

}  // namespace revenant::ui
