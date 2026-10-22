// Where the receivers are, as state the window binds to and the settings
// remember: docked or popped out, shown or hidden, and how tall the dock is.
//
// models/receiver_placement.h has the owner's call and the layout rules; this
// is the state and its persistence, and nothing else.
//
// A SMOKE RUN READS NOTHING AND WRITES NOTHING HERE. It starts docked and
// shown, whatever the operator's settings say, so a photograph of the main
// window is a photograph of the default; --grab-receivers pops the panel out
// for its own photograph, and that is not remembered either.
//
// WHEN EACH IS WRITTEN. The two switches are one event each and are written as
// they change. The dock's height changes continuously while the divider is
// dragged, and QSettings on Windows is a registry write per setValue, so it is
// cached and written once, on the way out.
//
// READY. Nothing is shown in a window until main.cpp says the receiver window
// has had its swap interval set and its geometry restored, both of which have
// to happen before the window is first shown. Until then the QML holds the
// window hidden whatever poppedOut says.

#pragma once

#include <QObject>
#include <QSettings>

#include "models/settings.h"

namespace revenant::ui {

class ReceiverPlacement : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool poppedOut READ poppedOut WRITE setPoppedOut NOTIFY poppedOutChanged)
    Q_PROPERTY(bool shown READ shown WRITE setShown NOTIFY shownChanged)
    Q_PROPERTY(double dockHeight READ dockHeight WRITE setDockHeight NOTIFY dockHeightChanged)
    Q_PROPERTY(double dockWidth READ dockWidth WRITE setDockWidth NOTIFY dockWidthChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

public:
    explicit ReceiverPlacement(bool persist, QObject* parent = nullptr)
        : QObject(parent), persist_(persist)
    {
        if (!persist_) {
            return;
        }
        const QSettings store;
        popped_out_ = store.value(settings::kReceiversPoppedOut, false).toBool();
        shown_ = store.value(settings::kReceiversShown, true).toBool();
        dock_height_ = store.value(settings::kReceiversDockHeight, 0.0).toDouble();
        dock_width_ = store.value(settings::kReceiversDockWidth, 0.0).toDouble();
    }

    [[nodiscard]] bool poppedOut() const { return popped_out_; }
    [[nodiscard]] bool shown() const { return shown_; }
    [[nodiscard]] double dockHeight() const { return dock_height_; }
    [[nodiscard]] double dockWidth() const { return dock_width_; }
    [[nodiscard]] bool ready() const { return ready_; }

    void setPoppedOut(bool popped)
    {
        if (popped_out_ == popped) {
            return;
        }
        popped_out_ = popped;
        if (persist_) {
            QSettings().setValue(settings::kReceiversPoppedOut, popped_out_);
        }
        emit poppedOutChanged();
    }

    void setShown(bool shown)
    {
        if (shown_ == shown) {
            return;
        }
        shown_ = shown;
        if (persist_) {
            QSettings().setValue(settings::kReceiversShown, shown_);
        }
        emit shownChanged();
    }

    void setDockHeight(double height)
    {
        if (dock_height_ == height) {
            return;
        }
        dock_height_ = height;
        emit dockHeightChanged();
    }

    // Beside the span, when the main window is tall. Kept apart from the
    // height, since the two are dragged in different arrangements.
    void setDockWidth(double width)
    {
        if (dock_width_ == width) {
            return;
        }
        dock_width_ = width;
        emit dockWidthChanged();
    }

    // The receiver window can be shown from here on. See READY above.
    void setReady()
    {
        if (!ready_) {
            ready_ = true;
            emit readyChanged();
        }
    }

    // On the way out. See WHEN EACH IS WRITTEN above.
    void writeDockHeight() const
    {
        if (persist_ && dock_height_ > 0.0) {
            QSettings().setValue(settings::kReceiversDockHeight, dock_height_);
        }
        if (persist_ && dock_width_ > 0.0) {
            QSettings().setValue(settings::kReceiversDockWidth, dock_width_);
        }
    }

signals:
    void poppedOutChanged();
    void shownChanged();
    void dockHeightChanged();
    void dockWidthChanged();
    void readyChanged();

private:
    bool persist_ = false;
    bool popped_out_ = false;
    bool shown_ = true;
    double dock_height_ = 0.0;
    double dock_width_ = 0.0;
    bool ready_ = false;
};

}  // namespace revenant::ui
