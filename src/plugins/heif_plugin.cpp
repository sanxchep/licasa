#include "image_plugin_loader.h"

#include <QImageIOPlugin>
#include <QtEndian>
#include <algorithm>

namespace {
bool heifSignature(QIODevice* device)
{
    if (!device || !device->isReadable()) {
        return false;
    }
    const QByteArray header = device->peek(256);
    if (header.size() < 16 || header.mid(4, 4) != "ftyp") {
        return false;
    }
    const quint32 length = qFromBigEndian<quint32>(header.constData());
    if (length < 16 || length % 4 != 0) {
        return false;
    }
    bool compatible = false;
    for (qsizetype offset = 8; offset + 4 <= std::min<qsizetype>(length, header.size());
         offset += 4) {
        if (offset == 12) {
            continue;
        }
        const auto brand = header.mid(offset, 4);
        if (brand == "avif" || brand == "avis") {
            return false;
        }
        if (brand == "heic" || brand == "heix" || brand == "hevc" || brand == "hevx" ||
            brand == "mif1" || brand == "msf1") {
            compatible = true;
        }
    }
    return compatible;
}
} // namespace

class LicasaHeifPlugin final : public QImageIOPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "heif.json")
  public:
    Capabilities capabilities(QIODevice* device, const QByteArray& format) const override
    {
        if (format == "heic" || format == "heif" || format == "hif" || format == "heics" ||
            format == "heifs") {
            return CanRead;
        }
        return format.isEmpty() && heifSignature(device) ? CanRead : Capabilities{};
    }
    QImageIOHandler* create(QIODevice* device, const QByteArray& format) const override
    {
        return Licasa::loadImageHandler(QStringLiteral("licasa_heif"), device, format);
    }
};

#include "heif_plugin.moc"
