#include "image_plugin_loader.h"

#include <QImageIOPlugin>
#include <QtEndian>
#include <array>

namespace {
bool rawSignature(QIODevice* device)
{
    if (!device || !device->isReadable() || device->isSequential()) {
        return false;
    }
    auto header = device->peek(32);
    if (header.startsWith("FUJIFILMCCD-RAW") || header.mid(8, 4) == QByteArray("CR\x02\0", 4) ||
        header.mid(4, 8) == "ftypcrx ") {
        return true;
    }
    const bool little = header.startsWith(QByteArray("II\x2a\0", 4));
    if (!little && !header.startsWith(QByteArray("MM\0\x2a", 4))) {
        return false;
    }
    const auto get32 = [little](const char* p) {
        return little ? qFromLittleEndian<quint32>(p) : qFromBigEndian<quint32>(p);
    };
    const auto get16 = [little](const char* p) {
        return little ? qFromLittleEndian<quint16>(p) : qFromBigEndian<quint16>(p);
    };
    if (header.size() < 8) {
        return false;
    }
    const quint32 offset = get32(header.constData() + 4);
    if (offset > 64 * 1024 - 2) {
        return false;
    }
    header = device->peek(64 * 1024);
    if (quint64(offset) + 2 > quint64(header.size())) {
        return false;
    }
    const quint16 count = get16(header.constData() + offset);
    if (quint64(offset) + 2 + quint64(count) * 12 > quint64(header.size())) {
        return false;
    }
    for (quint32 index = 0; index < count; ++index) {
        const char* entry = header.constData() + offset + 2 + index * 12;
        if (get16(entry) == 50706 && get16(entry + 2) == 1 && get32(entry + 4) == 4) {
            return true; // DNGVersion; native LibRaw validates its contents.
        }
    }
    return false;
}
} // namespace

class LicasaRawPlugin final : public QImageIOPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "raw.json")
  public:
    Capabilities capabilities(QIODevice* device, const QByteArray& format) const override
    {
        static constexpr std::array formats{"dng", "raw", "cr2", "cr3", "nef", "nrw",
                                            "arw", "srw", "rw2", "pef", "orf", "raf"};
        for (const char* candidate : formats) {
            if (format == candidate) {
                return CanRead;
            }
        }
        return format.isEmpty() && rawSignature(device) ? CanRead : Capabilities{};
    }
    QImageIOHandler* create(QIODevice* device, const QByteArray& format) const override
    {
        return Licasa::loadImageHandler(QStringLiteral("licasa_raw"), device, format);
    }
};

#include "raw_plugin.moc"
