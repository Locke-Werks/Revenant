// The bookmark half of EngineLink: saving a place, listing what was saved, and
// getting back to one.
//
// Split from ui/models/engine_link.cpp on the same grounds receiver_link.cpp and
// source_link.cpp are: same class, same threads, same rules, and that file is
// already the longest in the client. ui/models/engine_link.h holds the contract.
//
// ONE THREAD AND NO RPC CALLS. Everything here runs on the Qt thread. A save is a
// registry write; a recall is the same two writes the frequency box and the mode
// buttons already make, so the supervisor picks the work up the way it picks
// theirs up. Nothing here blocks on a round trip, which is what made splitting
// the retune into two turns necessary rather than optional.
//
// WHY A RECALL IS TWO TURNS WHEN THE FRONT END HAS TO MOVE
//
// tuneSourceHz records a target, wakes the supervisor and returns. The radio has
// not moved when it returns, and info_.source_center still holds the old value,
// so placing the receiver on the next line would subtract the wrong centre and
// land the receiver at an offset nothing asked for. It would play; it would play
// the wrong thing.
//
// So a recall that needs a retune parks the bookmark in pending_recall_ and the
// placement happens in resolve_pending_recall, called from the source half's
// adopt at the moment a granted centre reaches the Qt thread. That is also the
// only place the refusal is visible, which is why abandoning the bookmark lives
// there too: a retune the radio declined would otherwise leave an entry waiting
// for a turn that never comes.
//
// WHAT OF THIS THE WINDOW STILL CALLS. Since 2026-09-23 the frequency manager
// (models/frequency_manager.h) holds the list, in its own file, and reaches
// this file only through recallPlace, which is the recall below with the place
// passed in and an option to put it in a new receiver. The registry list, save,
// remove, rename and receiverBookmarked are no longer called from the QML, and
// the registry value is what the manager migrates from once. They are left in
// place so the change that brought the manager in touches this class as little
// as it can; retiring them is its own change.

#include "models/engine_link.h"

#include <cmath>
#include <utility>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include "models/settings.h"

namespace revenant::ui {

namespace {

// The field names in the stored JSON. Spelled once, because a bookmark written
// under one spelling and read under another comes back empty rather than wrong,
// and an empty list reads as "nothing was ever saved".
constexpr QLatin1StringView kName{"name"};
constexpr QLatin1StringView kFreq{"hz"};
constexpr QLatin1StringView kDemod{"demod"};
constexpr QLatin1StringView kLow{"low"};
constexpr QLatin1StringView kHigh{"high"};

[[nodiscard]] QJsonObject to_json(const Bookmark& mark)
{
    QJsonObject object;
    object[kName] = QString::fromStdString(mark.name);
    // A double carries an integer exactly to 2^53, and the highest frequency any
    // of this is about is eleven orders of magnitude below that. JSON has no
    // other number type, so the alternative would be a string.
    object[kFreq] = static_cast<double>(mark.freq_hz);
    object[kDemod] = QString::fromStdString(mark.demod);
    object[kLow] = mark.passband_low;
    object[kHigh] = mark.passband_high;
    return object;
}

// One stored entry, or nothing if it does not describe a bookmark.
//
// A ROW IS DROPPED RATHER THAN REPAIRED. Anything in the registry was written by
// a previous version of this client or edited by hand, and a half-read row filled
// out with defaults is a bookmark on a frequency nobody chose in a mode nobody
// picked. Dropping it loses an entry; keeping it invents one.
[[nodiscard]] std::optional<Bookmark> from_json(const QJsonValue& value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const QJsonObject object = value.toObject();

    Bookmark mark;
    mark.name = object[kName].toString().toStdString();
    mark.freq_hz = static_cast<std::int64_t>(object[kFreq].toDouble(0.0));
    mark.demod = object[kDemod].toString().toStdString();
    mark.passband_low = object[kLow].toInt(0);
    mark.passband_high = object[kHigh].toInt(0);

    if (mark.empty()) {
        return std::nullopt;
    }
    return mark;
}

}  // namespace

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

void EngineLink::load_bookmarks()
{
    const QSettings store;
    const QByteArray raw = store.value(settings::kBookmarks).toString().toUtf8();
    if (raw.isEmpty()) {
        return;
    }

    const QJsonDocument document = QJsonDocument::fromJson(raw);
    if (!document.isArray()) {
        return;
    }

    for (const QJsonValue& value : document.array()) {
        if (auto mark = from_json(value)) {
            bookmarks_.push_back(std::move(*mark));
        }
    }
}

void EngineLink::store_bookmarks() const
{
    QJsonArray array;
    for (const Bookmark& mark : bookmarks_) {
        array.append(to_json(mark));
    }

    // Compact rather than indented: this goes in a registry value, where the only
    // reader is this client and a human looking at regedit reads one line more
    // easily than forty.
    const QJsonDocument document(array);
    QSettings().setValue(settings::kBookmarks,
                         QString::fromUtf8(document.toJson(QJsonDocument::Compact)));
}

// ---------------------------------------------------------------------------
// What a display reads
// ---------------------------------------------------------------------------

QStringList EngineLink::bookmarkLabels() const
{
    QStringList labels;
    labels.reserve(static_cast<qsizetype>(bookmarks_.size()));
    for (const Bookmark& mark : bookmarks_) {
        labels.append(QString::fromStdString(bookmark_label(mark)));
    }
    return labels;
}

bool EngineLink::receiverBookmarked() const
{
    if (receiver_id_ == 0) {
        return false;
    }

    // The receiver's own width as the tolerance, which is the argument
    // bookmark_at's comment makes: two frequencies inside one passband are the
    // same station. Falls back to nothing on a receiver with no width yet, so the
    // question becomes an exact-frequency one rather than a guess.
    const std::int64_t width = wanted_.passband_high - wanted_.passband_low;
    const auto center = static_cast<std::int64_t>(receiverCenterHz());
    return bookmark_at(bookmarks_, center, width / 2) != nullptr;
}

// ---------------------------------------------------------------------------
// Saving and forgetting
// ---------------------------------------------------------------------------

void EngineLink::note_receiver_bookmarked()
{
    const bool now = receiverBookmarked();
    if (now == receiver_bookmarked_) {
        return;
    }
    receiver_bookmarked_ = now;
    emit bookmarksChanged();
}

void EngineLink::saveBookmark(const QString& name)
{
    if (receiver_id_ == 0) {
        bookmark_fault_ = QStringLiteral("no receiver to save");
        emit bookmarkFaultChanged();
        return;
    }

    Bookmark mark;
    mark.name = name.trimmed().toStdString();
    mark.freq_hz = static_cast<std::int64_t>(receiverCenterHz());
    mark.demod = receiverDemod().toStdString();
    mark.passband_low = wanted_.passband_low;
    mark.passband_high = wanted_.passband_high;

    if (mark.empty()) {
        // receiverCenterHz is absolute, so zero means the source is at DC and the
        // receiver is on it, which is a real place on an HF recording and not a
        // reason to refuse. An empty demod is the one that cannot be recalled,
        // because an empty mode means "keep the current one" to tuneReceiver.
        bookmark_fault_ = QStringLiteral("this receiver has no mode to save");
        emit bookmarkFaultChanged();
        return;
    }

    bookmarks_.push_back(std::move(mark));
    store_bookmarks();

    bookmark_fault_.clear();
    emit bookmarkFaultChanged();

    // The cached answer goes with it: saving the receiver's own frequency
    // makes receiverBookmarked true without the receiver having moved, and a
    // stale cache would then swallow the next real change.
    receiver_bookmarked_ = receiverBookmarked();
    emit bookmarksChanged();
}

void EngineLink::removeBookmark(int index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= bookmarks_.size()) {
        return;
    }
    bookmarks_.erase(bookmarks_.begin() + index);
    store_bookmarks();
    receiver_bookmarked_ = receiverBookmarked();
    emit bookmarksChanged();
}

void EngineLink::renameBookmark(int index, const QString& name)
{
    if (index < 0 || static_cast<std::size_t>(index) >= bookmarks_.size()) {
        return;
    }
    bookmarks_[static_cast<std::size_t>(index)].name = name.trimmed().toStdString();
    store_bookmarks();
    receiver_bookmarked_ = receiverBookmarked();
    emit bookmarksChanged();
}

// ---------------------------------------------------------------------------
// Recall
// ---------------------------------------------------------------------------

void EngineLink::recallBookmark(int index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= bookmarks_.size()) {
        return;
    }
    const Bookmark& mark = bookmarks_[static_cast<std::size_t>(index)];
    recallPlace(static_cast<double>(mark.freq_hz), QString::fromStdString(mark.demod),
                mark.passband_low, mark.passband_high, QString::fromStdString(mark.name), false);
}

void EngineLink::recallPlace(double absolute_hz, const QString& demod, int low, int high,
                             const QString& name, bool new_receiver)
{
    Bookmark mark;
    mark.name = name.toStdString();
    mark.freq_hz = static_cast<std::int64_t>(std::llround(absolute_hz));
    mark.demod = demod.toStdString();
    mark.passband_low = low;
    mark.passband_high = high;

    // Refused before anything moves. add_receiver_at would say the same in
    // the rack note and add nothing, and the edges applied after it would
    // then land on the focused receiver, which is not what was asked for.
    if (new_receiver && rack_.full()) {
        pending_recall_.reset();
        bookmark_fault_ = QStringLiteral("the rack is full, so %1 has no receiver to go in")
                              .arg(QString::fromStdString(bookmark_label(mark)));
        emit bookmarkFaultChanged();
        return;
    }

    // The span the DISPLAY is drawing, which is what plan_recall wants and why it
    // takes two doubles rather than a centre and a rate.
    const RecallPlan plan = plan_recall(mark, spanLowHz(), spanHighHz(), sourceCanRetune());

    switch (plan.action) {
    case RecallAction::PlaceHere:
        pending_recall_.reset();
        if (place_recall(mark, new_receiver)) {
            bookmark_fault_.clear();
            emit bookmarkFaultChanged();
        }
        return;

    case RecallAction::RetuneThenPlace:
        // Held, not placed. See the file header: the radio has not moved when
        // tuneSourceHz returns.
        pending_recall_ = mark;
        pending_recall_new_ = new_receiver;
        bookmark_fault_ = QStringLiteral("moving the radio to reach %1")
                              .arg(QString::fromStdString(bookmark_label(mark)));
        emit bookmarkFaultChanged();
        tuneSourceHz(static_cast<double>(plan.retune_center_hz));
        return;

    case RecallAction::OutOfReach:
        pending_recall_.reset();
        bookmark_fault_ = QStringLiteral("%1 is outside this source and it cannot tune")
                              .arg(QString::fromStdString(bookmark_label(mark)));
        emit bookmarkFaultChanged();
        return;

    case RecallAction::NoSource:
        pending_recall_.reset();
        bookmark_fault_ = QStringLiteral("no source open yet");
        emit bookmarkFaultChanged();
        return;

    case RecallAction::Unusable:
        pending_recall_.reset();
        bookmark_fault_ = QStringLiteral("that bookmark is missing its frequency or its mode");
        emit bookmarkFaultChanged();
        return;
    }
}

bool EngineLink::place_recall(const Bookmark& mark, bool new_receiver)
{
    // Absolute hertz, and the mode named. tuneReceiver does the subtraction
    // against whatever source_center holds now, which is the whole reason a
    // pending recall waits for the granted centre before getting here.
    //
    // NAMING THE MODE RECORDS THE OPERATOR AS HAVING CHOSEN IT, through
    // demod_touched_, and that is correct rather than incidental: a bookmark
    // carries a mode because somebody set one, so a later click on a detection
    // should keep it instead of deriving one from a measured bandwidth.
    //
    // A new receiver goes through add_receiver_at, which parks the focused one
    // in the rack first and then makes the same tuneReceiver call. The rack
    // can have filled while a retune was pending, so it is asked again here:
    // the edges below would otherwise land on the focused receiver.
    if (new_receiver) {
        if (rack_.full()) {
            bookmark_fault_ = QStringLiteral("the rack is full, so %1 has no receiver to go in")
                                  .arg(QString::fromStdString(bookmark_label(mark)));
            emit bookmarkFaultChanged();
            return false;
        }
        add_receiver_at(static_cast<double>(mark.freq_hz), QString::fromStdString(mark.demod),
                        0.0);
    } else {
        tuneReceiver(static_cast<double>(mark.freq_hz), QString::fromStdString(mark.demod));
    }

    // The saved edges, if they were ever moved off the mode's default. Both zero
    // means they were not, and the engine has already answered with the default,
    // so sending a zero-width passband would replace a real filter with nothing.
    if (mark.passband_low != 0 || mark.passband_high != 0) {
        setReceiverPassband(mark.passband_low, mark.passband_high);
    }

    note_receiver_bookmarked();
    return true;
}

void EngineLink::resolve_pending_recall(bool granted)
{
    if (!pending_recall_) {
        return;
    }
    const Bookmark mark = *pending_recall_;
    const bool new_receiver = pending_recall_new_;
    pending_recall_.reset();
    pending_recall_new_ = false;

    if (!granted) {
        // The radio declined the move. Said here rather than left to the tune
        // fault alone, because the operator asked for a bookmark and the answer
        // has to be about the bookmark.
        bookmark_fault_ = QStringLiteral("the radio would not tune to %1")
                              .arg(QString::fromStdString(bookmark_label(mark)));
        emit bookmarkFaultChanged();
        return;
    }

    // The span moved, so ask again rather than assuming the grant landed where it
    // was asked to. A radio that granted a different centre than requested, or
    // one whose span shrank on the way, can still leave the bookmark out of
    // reach, and placing it anyway would put the receiver outside the span for
    // the engine to remove a moment later.
    const RecallPlan plan = plan_recall(mark, spanLowHz(), spanHighHz(), sourceCanRetune());
    if (plan.action != RecallAction::PlaceHere) {
        bookmark_fault_ = QStringLiteral("%1 is still outside the span after retuning")
                              .arg(QString::fromStdString(bookmark_label(mark)));
        emit bookmarkFaultChanged();
        return;
    }

    if (place_recall(mark, new_receiver)) {
        bookmark_fault_.clear();
        emit bookmarkFaultChanged();
    }
}

}  // namespace revenant::ui
