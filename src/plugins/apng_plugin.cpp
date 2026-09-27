#include "image_plugin_loader.h"

#include <QIODevice>
#include <QImageIOPlugin>
#include <QtEndian>

#include <algorithm>
#include <limits>

namespace {
constexpr char pngSignatureBytes[] = "\x89PNG\r\n\x1a\n";
constexpr qint64 signatureSize = 8;
constexpr int maximumProbeChunks = 4096;
constexpr qsizetype sequentialProbeLimit = 512 * 1024;

bool validPngSignature(const QByteArray& bytes)
{
    return bytes.size() >= signatureSize &&
           bytes.left(signatureSize) == QByteArray::fromRawData(pngSignatureBytes, signatureSize);
}

bool apngInBytes(const QByteArray& bytes)
{
    if (!validPngSignature(bytes)) {
        return false;
    }

    qint64 offset = signatureSize;
    for (int chunks = 0; chunks < maximumProbeChunks; ++chunks) {
        if (offset < 0 || offset > bytes.size() - 12) {
            return false;
        }
        const auto* header = reinterpret_cast<const uchar*>(bytes.constData() + offset);
        const quint32 length = qFromBigEndian<quint32>(header);
        const QByteArray type(bytes.constData() + offset + 4, 4);
        const quint64 next = quint64(offset) + 12u + quint64(length);
        if (next > quint64(bytes.size())) {
            return false;
        }
        if (type == "acTL") {
            return length == 8;
        }
        if (type == "IDAT" || type == "IEND") {
            return false;
        }
        offset = qint64(next);
    }
    return false;
}

bool apngSignature(QIODevice* device)
{
    if (!device || !device->isReadable()) {
        return false;
    }

    if (device->isSequential()) {
        return apngInBytes(device->peek(sequentialProbeLimit));
    }

    const qint64 original = device->pos();
    const qint64 total = device->size();
    if (total < signatureSize + 12) {
        if (original >= 0) {
            device->seek(original);
        }
        return false;
    }

    auto restore = [device, original] {
        if (original >= 0) {
            device->seek(original);
        }
    };

    if (!device->seek(0)) {
        restore();
        return false;
    }
    const QByteArray signature = device->read(signatureSize);
    if (!validPngSignature(signature)) {
        restore();
        return false;
    }

    qint64 offset = signatureSize;
    for (int chunks = 0; chunks < maximumProbeChunks; ++chunks) {
        if (offset < 0 || offset > total - 12 || !device->seek(offset)) {
            restore();
            return false;
        }
        const QByteArray header = device->read(8);
        if (header.size() != 8) {
            restore();
            return false;
        }
        const quint32 length =
            qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(header.constData()));
        const QByteArray type = header.mid(4, 4);
        const quint64 next = quint64(offset) + 12u + quint64(length);
        if (next > quint64(total) || next > quint64(std::numeric_limits<qint64>::max())) {
            restore();
            return false;
        }
        if (type == "acTL") {
            restore();
            return length == 8;
        }
        if (type == "IDAT" || type == "IEND") {
            restore();
            return false;
        }
        offset = qint64(next);
    }

    restore();
    return false;
}
} // namespace

class LicasaApngPlugin final : public QImageIOPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "apng.json")
  public:
    Capabilities capabilities(QIODevice* device, const QByteArray& format) const override
    {
        const QByteArray normalized = format.toLower();
        if (normalized == "apng") {
            return !device || apngSignature(device) ? CanRead : Capabilities{};
        }
        if (normalized == "png" || normalized.isEmpty()) {
            return apngSignature(device) ? CanRead : Capabilities{};
        }
        return {};
    }

    QImageIOHandler* create(QIODevice* device, const QByteArray& format) const override
    {
        Q_UNUSED(format);
        return Licasa::loadImageHandler(QStringLiteral("licasa_apng"), device, QByteArray("apng"));
    }
};

#include "apng_plugin.moc"
