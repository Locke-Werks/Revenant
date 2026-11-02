#include "models/decoded_image_model.h"

#include <string>
#include <utility>

#include <QDateTime>
#include <QFileInfo>

namespace revenant::ui {

DecodedImageModel::DecodedImageModel(QObject* parent) : QAbstractListModel(parent) {}

int DecodedImageModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return count();
}

QVariant DecodedImageModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) {
        return {};
    }
    const DecodedImage& image = store_.at(static_cast<std::size_t>(index.row()));
    switch (role) {
        case KeyRole: return QString::fromStdString(image.key());
        case LabelRole: return QString::fromStdString(image.label());
        case DecoderRole: return QString::fromStdString(image.decoder);
        case ModeRole: return QString::fromStdString(image.mode);
        case WidthRole: return static_cast<int>(image.width);
        case HeightRole: return static_cast<int>(image.height());
        case StateRole: return QString::fromLatin1(image.state_text());
        case LineRateRole: return image.line_rate_hz;
        case SourceRole:
            return QStringLiteral("image://decoded/%1/%2")
                .arg(QString::fromStdString(image.key()))
                .arg(image.revision);
        case TimeRole:
            return QDateTime::fromMSecsSinceEpoch(image.arrived_ms)
                .toString(QStringLiteral("HH:mm:ss"));
        default: return {};
    }
}

QHash<int, QByteArray> DecodedImageModel::roleNames() const
{
    return {
        {KeyRole, "key"},         {LabelRole, "label"},   {DecoderRole, "decoder"},
        {ModeRole, "mode"},       {WidthRole, "width"},   {HeightRole, "height"},
        {StateRole, "state"},     {SourceRole, "source"}, {LineRateRole, "lineRate"},
        {TimeRole, "time"},
    };
}

QVariantMap DecodedImageModel::get(int row) const
{
    QVariantMap out;
    if (row < 0 || row >= count()) {
        return out;
    }
    const QModelIndex at = index(row);
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it) {
        out.insert(QString::fromLatin1(it.value()), data(at, it.key()));
    }
    return out;
}

int DecodedImageModel::liveIndex() const
{
    const auto& images = store_.images();
    for (std::size_t i = images.size(); i-- > 0;) {
        if (images[i].state == DecodedImageState::Receiving) {
            return static_cast<int>(i);
        }
    }
    return count() - 1;
}

void DecodedImageModel::apply(const rpc::DecodedImageEvent& event, std::int64_t now_ms)
{
    // An add is signalled around the store's change, eviction first, so a
    // view never reads a row the model has not announced.
    const bool will_add = event.kind == rpc::DecodedImageEvent::Kind::Open &&
                          !store_.holds(event.vrx, event.decoder, event.image_id);
    if (will_add) {
        if (store_.size() >= kDecodedImageCapacity) {
            beginRemoveRows(QModelIndex(), 0, 0);
            {
                const std::scoped_lock held(shelf_->lock);
                shelf_->images.remove(QString::fromStdString(store_.at(0).key()));
            }
            store_.drop_front();
            endRemoveRows();
        }
        const int row = count();
        beginInsertRows(QModelIndex(), row, row);
        const DecodedImageChange change = store_.apply(event, now_ms);
        endInsertRows();
        shelve(store_.at(change.index));
        ++revision_;
        emit changed();
        return;
    }

    const DecodedImageChange change = store_.apply(event, now_ms);
    if (change.kind != DecodedImageChange::Kind::Updated) {
        return;
    }
    shelve(store_.at(change.index));
    const QModelIndex at = index(static_cast<int>(change.index));
    emit dataChanged(at, at);
    ++revision_;
    emit changed();
}

void DecodedImageModel::abort_open(std::uint64_t vrx, const std::string& decoder)
{
    for (const std::size_t i : store_.abort_open(vrx, decoder)) {
        const QModelIndex at = index(static_cast<int>(i));
        emit dataChanged(at, at);
    }
    ++revision_;
    emit changed();
}

void DecodedImageModel::clear()
{
    beginResetModel();
    store_.clear();
    {
        const std::scoped_lock held(shelf_->lock);
        shelf_->images.clear();
    }
    endResetModel();
    ++revision_;
    emit changed();
}

QString DecodedImageModel::suggestedName(int row) const
{
    if (row < 0 || row >= count()) {
        return {};
    }
    return QString::fromStdString(store_.at(static_cast<std::size_t>(row)).file_name());
}

QString DecodedImageModel::savePng(int row, const QUrl& file) const
{
    if (row < 0 || row >= count()) {
        return QStringLiteral("there is no image %1 to save").arg(row);
    }
    const QImage picture = to_qimage(store_.at(static_cast<std::size_t>(row)));
    if (picture.isNull()) {
        return QStringLiteral("the image has no rows yet, so there is nothing to save");
    }
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    if (!picture.save(path, "PNG")) {
        return QStringLiteral("could not write %1").arg(QFileInfo(path).fileName());
    }
    return {};
}

QImage DecodedImageModel::to_qimage(const DecodedImage& image)
{
    const std::uint32_t height = image.height();
    if (image.width == 0 || height == 0) {
        return {};
    }
    const QImage::Format format = image.colour == rpc::ImageColour::Rgb888
                                      ? QImage::Format_RGB888
                                      : QImage::Format_Grayscale8;
    // Wraps the store's bytes and copies them out, so the QImage owns its
    // memory and outlives the next row's resize.
    const QImage view(image.pixels.data(), static_cast<int>(image.width),
                      static_cast<int>(height), static_cast<qsizetype>(image.row_bytes()),
                      format);
    return view.copy();
}

void DecodedImageModel::shelve(const DecodedImage& image)
{
    QImage picture = to_qimage(image);
    const std::scoped_lock held(shelf_->lock);
    shelf_->images.insert(QString::fromStdString(image.key()), std::move(picture));
}

DecodedImageProvider::DecodedImageProvider(std::shared_ptr<DecodedImageShelf> shelf)
    : QQuickImageProvider(QQuickImageProvider::Image), shelf_(std::move(shelf))
{
}

QImage DecodedImageProvider::requestImage(const QString& id, QSize* size, const QSize& requested)
{
    const QString key = id.section(QLatin1Char('/'), 0, 0);
    QImage picture;
    {
        const std::scoped_lock held(shelf_->lock);
        picture = shelf_->images.value(key);
    }
    if (picture.isNull()) {
        // A one-pixel placeholder rather than null, which QML reports as a
        // load error for an image whose first row has not arrived.
        picture = QImage(1, 1, QImage::Format_Grayscale8);
        picture.fill(0);
    }
    if (size != nullptr) {
        *size = picture.size();
    }
    if (requested.isValid() && !requested.isEmpty()) {
        return picture.scaled(requested, Qt::KeepAspectRatio, Qt::FastTransformation);
    }
    return picture;
}

}  // namespace revenant::ui
