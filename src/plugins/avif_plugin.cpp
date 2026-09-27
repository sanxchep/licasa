#include "image_plugin_loader.h"

#include <QImageIOPlugin>
#include <QtEndian>

#include <algorithm>

namespace {
bool isAvifBrand(const QByteArray& brand) { return brand == "avif" || brand == "avis"; }

bool avifSignature(QIODevice* device)
{
    if (!device || !device->isReadable()) {
        return false;
    }
    const QByteArray header = device->peek(512);
    if (header.size() < 16 || header.mid(4, 4) != "ftyp") {
        return false;
    }

    const quint32 size32 = qFromBigEndian<quint32>(header.constData());
    qsizetype brandOffset = 8;
    quint64 boxSize = size32;
    if (size32 == 1) {
        if (header.size() < 24) {
            return false;
        }
        boxSize = qFromBigEndian<quint64>(header.constData() + 8);
        brandOffset = 16;
    } else if (size32 == 0) {
        const qint64 size = device->size();
        if (size <= 0) {
            return false;
        }
        boxSize = quint64(size);
    }
    if (boxSize < quint64(brandOffset + 8)) {
        return false;
    }
    if (isAvifBrand(header.mid(brandOffset, 4))) {
        return true;
    }

    const qsizetype scanEnd = std::min<quint64>(boxSize, quint64(header.size()));
    for (qsizetype offset = brandOffset + 8; offset + 4 <= scanEnd; offset += 4) {
        if (isAvifBrand(header.mid(offset, 4))) {
            return true;
        }
    }
    return false;
}
} // namespace

class LicasaAvifPlugin final : public QImageIOPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "avif.json")
  public:
    Capabilities capabilities(QIODevice* device, const QByteArray& format) const override
    {
        if (format == "avif") {
            return CanRead;
        }
        if (!format.isEmpty()) {
            return {};
        }
        return avifSignature(device) ? CanRead : Capabilities{};
    }

    QImageIOHandler* create(QIODevice* device, const QByteArray& format) const override
    {
        return Licasa::loadImageHandler(QStringLiteral("licasa_avif"), device,
                                        format.isEmpty() ? QByteArray("avif") : format);
    }
};

#include "avif_plugin.moc"
