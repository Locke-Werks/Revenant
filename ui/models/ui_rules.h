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

#include <cstdint>

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQmlIntegration>

#include "models/frequency_dial.h"

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

private:
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
