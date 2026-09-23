// The keyboard table and the palette's search, exposed to QML as one
// singleton.
//
// Every method here is a conversion around models/key_actions.h,
// models/key_steps.h or models/palette_match.h, which hold the rules and are
// tested in ui/tests. Anything longer than a conversion belongs there, on the
// rule models/ui_rules.h states for itself.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <QKeyCombination>
#include <QKeySequence>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

#include "models/frequency_dial.h"
#include "models/key_actions.h"
#include "models/key_steps.h"
#include "models/palette_match.h"

namespace revenant::ui {

// A key press as the table writes sequences: modifiers in the order Ctrl,
// Alt, Shift, Meta, then the key in Qt's portable text. Built here rather than
// by QKeySequence::toString on the whole combination, so the modifier order
// is this table's and not whatever Qt's version prints.
[[nodiscard]] inline KeyChord key_chord(int key, Qt::KeyboardModifiers modifiers)
{
    KeyChord chord = parse_key_chord(
        QKeySequence(QKeyCombination(Qt::NoModifier, static_cast<Qt::Key>(key)))
            .toString(QKeySequence::PortableText)
            .toStdString());
    chord.ctrl = modifiers.testFlag(Qt::ControlModifier);
    chord.alt = modifiers.testFlag(Qt::AltModifier);
    chord.shift = modifiers.testFlag(Qt::ShiftModifier);
    chord.meta = modifiers.testFlag(Qt::MetaModifier);
    return chord;
}

class KeyMap : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(int defaultTuningDigit READ defaultTuningDigit CONSTANT)
    Q_PROPERTY(int filterStepHz READ filterStepHz CONSTANT)

public:
    explicit KeyMap(QObject* parent = nullptr) : QObject(parent), entries_(palette_entries()) {}

    [[nodiscard]] int defaultTuningDigit() const { return kDefaultTuningDigit; }
    [[nodiscard]] int filterStepHz() const { return kFilterWidenStepHz; }

    // The window's actions that have a key, each a map of id, keys (a list
    // of sequences for Shortcut.sequences), handler and argument.
    [[nodiscard]] Q_INVOKABLE QVariantList shortcuts() const
    {
        QVariantList out;
        for (const KeyAction& action : kKeyActions) {
            if (action.context != KeyContext::Window || action.keys[0].empty()) {
                continue;
            }
            QStringList keys;
            for (const std::string_view key : action.keys) {
                if (!key.empty()) {
                    keys.append(text(key));
                }
            }
            out.append(QVariantMap{{QStringLiteral("id"), text(action.id)},
                                   {QStringLiteral("keys"), keys},
                                   {QStringLiteral("handler"), text(action.handler)},
                                   {QStringLiteral("argument"), text(action.argument)}});
        }
        return out;
    }

    // Every handler ui/qml/Commands.qml must hold: the window actions' and
    // the palette's bands'.
    [[nodiscard]] Q_INVOKABLE QStringList handlers() const
    {
        QStringList out;
        for (const PaletteEntry& entry : entries_) {
            const QString name = QString::fromStdString(entry.handler);
            if (!out.contains(name)) {
                out.append(name);
            }
        }
        return out;
    }

    // What the window has, by name, as the bits the table's needs are
    // written in. A name left out counts as false.
    [[nodiscard]] Q_INVOKABLE int have(const QVariantMap& state) const
    {
        const auto flag = [&](const char* name) {
            return state.value(QLatin1StringView(name)).toBool();
        };
        KeyState in;
        in.connected = flag("connected");
        in.source_open = flag("sourceOpen");
        in.can_retune = flag("canRetune");
        in.receiver = flag("receiver");
        in.second_receiver = flag("secondReceiver");
        in.aft_offered = flag("aftOffered");
        in.auto_filter_offered = flag("autoFilterOffered");
        in.spectrum_drawing = flag("spectrumDrawing");
        return static_cast<int>(key_have(in));
    }

    [[nodiscard]] Q_INVOKABLE bool enabled(const QString& id, int have) const
    {
        const KeyAction* action = find_key_action(id.toStdString());
        return action != nullptr &&
               key_action_enabled(*action, static_cast<std::uint32_t>(have));
    }

    // "Ctrl+K or Ctrl+Shift+P" for an action, empty for one with no key.
    [[nodiscard]] Q_INVOKABLE QString keysText(const QString& id) const
    {
        const KeyAction* action = find_key_action(id.toStdString());
        return action != nullptr ? QString::fromStdString(key_text(*action)) : QString();
    }

    // The overlay's command for a key press: "next", "previous", "run",
    // "close", or empty when the overlay does not use that key.
    [[nodiscard]] Q_INVOKABLE QString overlayCommand(int key, int modifiers) const
    {
        const KeyHit hit = find_key(KeyContext::Overlay,
                                    key_chord(key, Qt::KeyboardModifiers(modifiers)));
        if (hit.action == nullptr) {
            return {};
        }
        const auto command = overlay_command(hit.action->id);
        if (!command) {
            return {};
        }
        switch (*command) {
            case OverlayCommand::Next: return QStringLiteral("next");
            case OverlayCommand::Previous: return QStringLiteral("previous");
            case OverlayCommand::Run: return QStringLiteral("run");
            case OverlayCommand::Close: return QStringLiteral("close");
        }
        return {};
    }

    // The palette's matches for a query, best first. scope is "bands" for
    // the band jump and anything else for everything. accent is the colour
    // the matched letters are drawn in, since the palette is Theme's.
    [[nodiscard]] Q_INVOKABLE QVariantList palette(const QString& query, const QString& scope,
                                                   int have, double tune_low, double tune_high,
                                                   const QString& accent) const
    {
        PaletteState state;
        state.have = static_cast<std::uint32_t>(have);
        state.tune_low_hz = static_cast<std::int64_t>(tune_low);
        state.tune_high_hz = static_cast<std::int64_t>(tune_high);
        const std::vector<PaletteHit> hits = rank_palette(
            entries_, query.toStdString(),
            scope == QLatin1StringView("bands") ? PaletteScope::Bands : PaletteScope::Everything,
            state);

        QVariantList out;
        out.reserve(static_cast<qsizetype>(hits.size()));
        for (const PaletteHit& hit : hits) {
            const PaletteEntry& entry = entries_[hit.entry];
            out.append(QVariantMap{
                {QStringLiteral("id"), QString::fromStdString(entry.id)},
                {QStringLiteral("label"), marked(entry.label, hit.label_positions, accent)},
                {QStringLiteral("group"), QString::fromStdString(entry.group)},
                {QStringLiteral("keys"), QString::fromStdString(entry.keys)},
                {QStringLiteral("enabled"), hit.enabled},
                {QStringLiteral("reason"), QString::fromStdString(hit.reason)},
                {QStringLiteral("handler"), QString::fromStdString(entry.handler)},
                {QStringLiteral("argument"), QString::fromStdString(entry.argument)}});
        }
        return out;
    }

    // The key map as rows: a context heading with its note, then each
    // action with a key, as maps of kind ("context" or "action"), text, note,
    // group and keys.
    [[nodiscard]] Q_INVOKABLE QVariantList keyMap() const
    {
        QVariantList out;
        for (const KeyContextInfo& info : kKeyContexts) {
            out.append(QVariantMap{
                {QStringLiteral("kind"), QStringLiteral("context")},
                {QStringLiteral("text"), text(info.name)},
                {QStringLiteral("note"), QString::fromStdString(key_context_note(info.context))},
                {QStringLiteral("group"), QString()},
                {QStringLiteral("keys"), QString()}});
            for (const KeyAction& action : kKeyActions) {
                if (action.context != info.context || action.keys[0].empty()) {
                    continue;
                }
                out.append(QVariantMap{{QStringLiteral("kind"), QStringLiteral("action")},
                                       {QStringLiteral("text"), text(action.label)},
                                       {QStringLiteral("note"), QString()},
                                       {QStringLiteral("group"), text(action.group)},
                                       {QStringLiteral("keys"),
                                        QString::fromStdString(key_text(action))}});
            }
        }
        return out;
    }

    // ---------------------------------------------------------------------
    // Steps. See models/key_steps.h.
    // ---------------------------------------------------------------------

    [[nodiscard]] Q_INVOKABLE int moveTuningDigit(int digit, int places, int digit_count) const
    {
        return move_tuning_digit(digit, places, digit_count);
    }

    [[nodiscard]] Q_INVOKABLE double pageTuneHz(double centre, double span_low,
                                                double span_high, int direction,
                                                double tune_low, double tune_high) const
    {
        return static_cast<double>(page_tune_hz(to_hz(centre), to_hz(span_low),
                                                to_hz(span_high), direction,
                                                DialLimits{to_hz(tune_low), to_hz(tune_high)}));
    }

    [[nodiscard]] Q_INVOKABLE double stepVolume(double volume, int presses) const
    {
        return step_volume(volume, presses);
    }

private:
    [[nodiscard]] static QString text(std::string_view view)
    {
        return QString::fromUtf8(view.data(), static_cast<qsizetype>(view.size()));
    }

    [[nodiscard]] static std::int64_t to_hz(double hz)
    {
        return static_cast<std::int64_t>(hz < 0.0 ? hz - 0.5 : hz + 0.5);
    }

    // The label as styled text with the matched letters in the accent, and
    // everything else escaped, since a band name carries a "/".
    [[nodiscard]] static QString marked(const std::string& label,
                                        const std::vector<std::size_t>& positions,
                                        const QString& accent)
    {
        QString out;
        std::size_t next = 0;
        for (std::size_t i = 0; i < label.size(); ++i) {
            const QString letter = QString(QChar::fromLatin1(label[i])).toHtmlEscaped();
            if (next < positions.size() && positions[next] == i) {
                out += QStringLiteral("<font color=\"") + accent + QStringLiteral("\">") +
                       letter + QStringLiteral("</font>");
                ++next;
            } else {
                out += letter;
            }
        }
        return out;
    }

    std::vector<PaletteEntry> entries_;
};

}  // namespace revenant::ui
