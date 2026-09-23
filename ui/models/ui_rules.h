// The Qt-free rules the QML needs to call, exposed to QML as one singleton.
//
// WHY THIS EXISTS. The window is layout and binding only, and the rules behind
// its controls live in headers under ui/models with cases in ui/tests. Most of
// those rules reach QML through EngineLink, because they are about the engine.
// A few are not: what one notch of a dial's wheel does, which bands a device
// can reach, how loudly the status strip should speak. Those are functions of
// numbers QML already has, so they are exposed here as plain calls on the
// numbers, and the headers they wrap stay free of Qt so the test binary can
// link them.
//
// Every method here is a one-line conversion around a function that is tested
// elsewhere. Anything longer belongs in the header it calls.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

#include "models/band_plan.h"
#include "models/composite_probe.h"
#include "models/frequency_dial.h"
#include "models/level_meter.h"
#include "models/mode_choice.h"
#include "models/receiver_palette.h"
#include "models/receiver_rack.h"
#include "models/ruler.h"
#include "models/scroll_tune.h"
#include "models/status_summary.h"

namespace revenant::ui {

class UiRules : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit UiRules(QObject* parent = nullptr) : QObject(parent) {}

    // ---------------------------------------------------------------------
    // Frequency dials. See models/frequency_dial.h.
    // ---------------------------------------------------------------------

    // Hertz arrive and leave as doubles because that is what QML has. Every
    // integer hertz a radio here can tune is exactly representable in one,
    // so the conversion is exact both ways.
    [[nodiscard]] Q_INVOKABLE double dialStep(double hz, int digit, int notches, double low,
                                              double high) const
    {
        return static_cast<double>(step_dial(to_hz(hz), digit, notches, limits(low, high)).hz);
    }

    [[nodiscard]] Q_INVOKABLE bool dialStepClamped(double hz, int digit, int notches,
                                                   double low, double high) const
    {
        return step_dial(to_hz(hz), digit, notches, limits(low, high)).clamped;
    }

    // One wheel event over a digit: {notches, carry}, the whole notches to
    // step it by and the travel to hand back on the next event.
    [[nodiscard]] Q_INVOKABLE QVariantMap dialWheel(double carry, double delta_x,
                                                    double delta_y) const
    {
        const DialWheel turn = dial_wheel(carry, delta_x, delta_y);
        return QVariantMap{{QStringLiteral("notches"), turn.notches},
                           {QStringLiteral("carry"), turn.carry_eighths}};
    }

    [[nodiscard]] Q_INVOKABLE int dialDigitCount(double hz, double low, double high) const
    {
        return dial_digit_count(to_hz(hz), limits(low, high));
    }

    [[nodiscard]] Q_INVOKABLE int dialDigit(double hz, int digit) const
    {
        return dial_digit(to_hz(hz), digit);
    }

    [[nodiscard]] Q_INVOKABLE int dialSignificantDigits(double hz) const
    {
        return dial_significant_digits(to_hz(hz));
    }

    // "." after the megahertz digit, " " after the kilohertz and gigahertz
    // digits, and nothing elsewhere.
    [[nodiscard]] Q_INVOKABLE QString dialSeparatorAfter(int digit) const
    {
        switch (dial_separator_after(digit)) {
            case DialSeparator::Point: return QStringLiteral(".");
            case DialSeparator::Gap: return QStringLiteral(" ");
            case DialSeparator::None: break;
        }
        return {};
    }

    // ---------------------------------------------------------------------
    // The frequency ruler. See models/ruler.h.
    // ---------------------------------------------------------------------

    // One map per tick: x, major, and label, which is empty on a minor tick
    // and on a major one whose label would not fit.
    [[nodiscard]] Q_INVOKABLE QVariantList rulerTicks(double low, double high, double width,
                                                      double char_px, double reserve_left) const
    {
        QVariantList out;
        const RulerPlan plan = plan_ruler(low, high, width, char_px, reserve_left);
        out.reserve(static_cast<qsizetype>(plan.ticks.size()));
        for (const RulerTick& tick : plan.ticks) {
            out.append(QVariantMap{{QStringLiteral("x"), tick.x_px},
                                   {QStringLiteral("major"), tick.major},
                                   {QStringLiteral("label"),
                                    QString::fromStdString(tick.label)}});
        }
        return out;
    }

    [[nodiscard]] Q_INVOKABLE QString rulerUnit(double low, double high) const
    {
        return QString::fromStdString(ruler_unit_name(ruler_unit_hz(low, high)));
    }

    [[nodiscard]] Q_INVOKABLE double rulerX(double hz, double low, double high,
                                            double width) const
    {
        return ruler_x(hz, low, high, width);
    }

    // ---------------------------------------------------------------------
    // The band plan. See models/band_plan.h.
    // ---------------------------------------------------------------------

    // Every band, in the table's order, as a map the menu can draw. The
    // index is the band's position and is what bandReachable takes.
    [[nodiscard]] Q_INVOKABLE QVariantList bands() const
    {
        QVariantList out;
        for (const Band& band : kBands) {
            out.append(QVariantMap{
                {QStringLiteral("group"), to_qstring(band.group)},
                {QStringLiteral("name"), to_qstring(band.name)},
                {QStringLiteral("low"), static_cast<double>(band.low_hz)},
                {QStringLiteral("high"), static_cast<double>(band.high_hz)},
                {QStringLiteral("centre"), static_cast<double>(band.centre_hz)},
                {QStringLiteral("mode"), to_qstring(band.mode)},
                {QStringLiteral("favourite"), band.favourite}});
        }
        return out;
    }

    [[nodiscard]] Q_INVOKABLE bool bandReachable(int index, double tune_low,
                                                 double tune_high) const
    {
        if (index < 0 || static_cast<std::size_t>(index) >= kBands.size()) {
            return false;
        }
        return band_reachable(kBands[static_cast<std::size_t>(index)], to_hz(tune_low),
                              to_hz(tune_high));
    }

    // ---------------------------------------------------------------------
    // The status pill and the receiver meter. See models/status_summary.h
    // and models/level_meter.h.
    // ---------------------------------------------------------------------

    // Takes the conditions by name, so a caller that forgets one gets the
    // default for it rather than an argument out of place. The level comes
    // back as 0 quiet, 1 note, 2 warn, 3 bad.
    [[nodiscard]] Q_INVOKABLE QVariantMap status(const QVariantMap& conditions) const
    {
        const auto flag = [&](const char* name) {
            return conditions.value(QLatin1StringView(name)).toBool();
        };
        StatusInputs in;
        in.connected = flag("connected");
        in.engine_running = flag("engineRunning");
        in.source_open = flag("sourceOpen");
        in.source_behind = flag("sourceBehind");
        in.front_end_fault = flag("frontEndFault");
        in.detection_fault = flag("detectionFault");
        in.tune_fault = flag("tuneFault");
        in.source_fault = flag("sourceFault");
        in.receiver_gone = flag("receiverGone");
        in.clamped = flag("clamped");
        in.stranded = flag("stranded");
        in.frames_dropped_by_engine =
            conditions.value(QStringLiteral("framesDroppedByEngine")).toULongLong();
        in.frame_rate = conditions.value(QStringLiteral("frameRate")).toDouble();
        const StatusSummary summary = summarise_status(in);
        return QVariantMap{{QStringLiteral("level"), static_cast<int>(summary.level)},
                           {QStringLiteral("headline"),
                            QString::fromStdString(summary.headline)}};
    }

    [[nodiscard]] Q_INVOKABLE double meterFraction(double dbfs) const
    {
        return meter_fraction(dbfs);
    }

    [[nodiscard]] Q_INVOKABLE bool meterHasReading(double dbfs) const
    {
        return meter_has_reading(dbfs);
    }

    // ---------------------------------------------------------------------
    // The receiver rack. See models/receiver_rack.h and
    // models/receiver_palette.h.
    // ---------------------------------------------------------------------

    // One "#rrggbb" per rack slot, in slot order.
    [[nodiscard]] Q_INVOKABLE QStringList receiverColours() const
    {
        QStringList out;
        for (const Rgb8& colour : kReceiverPalette) {
            out.append(QString::fromStdString(rgb8_hex(colour)));
        }
        return out;
    }

    // What a click on the span does, as the rack's hint line says it.
    [[nodiscard]] Q_INVOKABLE QString spanClickHint() const
    {
        return QString::fromUtf8(kSpanClickHint);
    }

    // What the rack says where the strips would be.
    [[nodiscard]] Q_INVOKABLE QString rackEmptyText() const
    {
        return QString::fromUtf8(kRackEmptyText);
    }

    // ---------------------------------------------------------------------
    // The mode selector. See models/mode_choice.h.
    // ---------------------------------------------------------------------

    // The names on the selector's row, in order.
    [[nodiscard]] Q_INVOKABLE QStringList rowModes() const { return names(mode_names(false)); }

    // The digital group's entries, each a map of name and label, in order.
    [[nodiscard]] Q_INVOKABLE QVariantList digitalModes() const
    {
        QVariantList out;
        for (const std::string_view name : mode_names(true)) {
            out.append(QVariantMap{{QStringLiteral("name"), to_qstring(name)},
                                   {QStringLiteral("label"), to_qstring(mode_label(name))}});
        }
        return out;
    }

    // What a mode is shown as: "P25" for p25p1, the name itself for the row.
    [[nodiscard]] Q_INVOKABLE QString modeLabel(const QString& demod) const
    {
        return to_qstring(mode_label(demod.toStdString()));
    }

    [[nodiscard]] Q_INVOKABLE bool modeIsDigital(const QString& demod) const
    {
        return mode_is_digital(demod.toStdString());
    }

    // The digital segment's text while `demod` is in force.
    [[nodiscard]] Q_INVOKABLE QString digitalGroupLabel(const QString& demod) const
    {
        return to_qstring(digital_group_label(demod.toStdString()));
    }

    // Whether the receiver window shows its RDS section at all. See
    // rds_offered in models/composite_probe.h, which only ever offers it on
    // wfm, so the name is compared against that one mode here.
    [[nodiscard]] Q_INVOKABLE bool rdsOffered(const QString& demod, double granted_low,
                                              double granted_high) const
    {
        const rpc::Demod mode =
            demod == QLatin1StringView("wfm") ? rpc::Demod::Wfm : rpc::Demod::Raw;
        return rds_offered(mode, to_hz(granted_low), to_hz(granted_high));
    }

    // The wheel's two axes resolved to one, the way the span displays do it.
    // See models/scroll_tune.h.
    [[nodiscard]] Q_INVOKABLE double scrollEighths(double delta_x, double delta_y) const
    {
        return scroll_tune_eighths(delta_x, delta_y);
    }

private:
    [[nodiscard]] static QString to_qstring(std::string_view text)
    {
        return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    }

    [[nodiscard]] static QStringList names(const std::vector<std::string_view>& list)
    {
        QStringList out;
        out.reserve(static_cast<qsizetype>(list.size()));
        for (const std::string_view name : list) {
            out.append(to_qstring(name));
        }
        return out;
    }

    [[nodiscard]] static std::int64_t to_hz(double hz)
    {
        return static_cast<std::int64_t>(hz < 0.0 ? hz - 0.5 : hz + 0.5);
    }

    [[nodiscard]] static DialLimits limits(double low, double high)
    {
        return DialLimits{to_hz(low), to_hz(high)};
    }
};

}  // namespace revenant::ui
