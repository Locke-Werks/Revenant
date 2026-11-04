// The decoded images as a list a QML view can hold, and the image provider
// that paints them.
//
// The rules (what an entry is, the cap, rows landing at their index) are
// models/decoded_images.h's, with their cases in ui/tests. This file signals
// rows, turns an entry into a QImage and writes a PNG.
//
// HOW A VIEW DRAWS ONE. Each row's `source` is image://decoded/<key>/<rev>.
// The revision changes with every row that lands, so an Image bound to it
// reloads as the picture grows; the provider ignores it and serves the latest
// picture for the key. The provider can be called off the Qt thread, so it
// reads a shelf of QImages under a mutex that the model refreshes on every
// change rather than reaching into the store.
//
// Qt thread only, apart from the shelf. EngineLink applies events from its
// own drain on the Qt thread.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QQuickImageProvider>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>
#include <QtQmlIntegration>

#include "core/rpc/types.h"
#include "models/decoded_images.h"

namespace revenant::ui {

// The pictures the provider serves, keyed by DecodedImage::key.
struct DecodedImageShelf {
    std::mutex lock;
    QHash<QString, QImage> images;
};

class DecodedImageModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by EngineLink; see EngineLink::decodedImages.")

    Q_PROPERTY(int count READ count NOTIFY changed)

    // The row a viewer shows when the operator has not picked one: the newest
    // image still receiving, else the newest. -1 when empty.
    Q_PROPERTY(int liveIndex READ liveIndex NOTIFY changed)

    // Bumped on every change, for a binding that calls get() to depend on.
    Q_PROPERTY(qulonglong revision READ revision NOTIFY changed)

public:
    enum Role : int {
        KeyRole = Qt::UserRole + 1,
        LabelRole,
        DecoderRole,
        ModeRole,
        WidthRole,
        HeightRole,
        StateRole,
        SourceRole,
        LineRateRole,
        TimeRole,
    };

    explicit DecodedImageModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] int count() const { return static_cast<int>(store_.size()); }
    [[nodiscard]] int liveIndex() const;
    [[nodiscard]] qulonglong revision() const { return revision_; }

    // One row's roles by name, for a view showing a single entry.
    Q_INVOKABLE QVariantMap get(int row) const;

    void apply(const rpc::DecodedImageEvent& event, std::int64_t now_ms);
    void abort_open(std::uint64_t vrx, const std::string& decoder);

    [[nodiscard]] std::shared_ptr<DecodedImageShelf> shelf() const { return shelf_; }

    Q_INVOKABLE void clear();

    // A file name for the save dialog to start from.
    Q_INVOKABLE QString suggestedName(int row) const;

    // Writes the row's picture as it stands. Answers empty on success, or a
    // sentence saying why not.
    Q_INVOKABLE QString savePng(int row, const QUrl& file) const;

    // The entry as a QImage, null for none. Public for the provider's tests
    // of shape, and for savePng.
    [[nodiscard]] static QImage to_qimage(const DecodedImage& image);

signals:
    void changed();

private:
    void shelve(const DecodedImage& image);

    // Shelves every image a row has touched since the last flush and tells
    // the views. Rows are coalesced because shelving copies the whole
    // picture and each view reloads it, so doing it per row made the cost of
    // a line grow with the image's height and stalled the GUI thread.
    void flush();

    // Indices into store_ with rows not yet shelved. Flushed before any
    // change that moves indices.
    std::set<std::size_t> dirty_;
    QTimer flush_timer_;

    DecodedImageStore store_;
    qulonglong revision_ = 0;
    std::shared_ptr<DecodedImageShelf> shelf_ = std::make_shared<DecodedImageShelf>();
};

// Registered with the QML engine under "decoded". Serves image://decoded/<key>
// and anything after a further '/' is the cache-busting revision.
class DecodedImageProvider : public QQuickImageProvider {
public:
    explicit DecodedImageProvider(std::shared_ptr<DecodedImageShelf> shelf);

    QImage requestImage(const QString& id, QSize* size, const QSize& requested) override;

private:
    std::shared_ptr<DecodedImageShelf> shelf_;
};

}  // namespace revenant::ui
