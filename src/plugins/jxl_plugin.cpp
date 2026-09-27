#include "image_plugin_loader.h"

#include <QImageIOPlugin>

class LicasaJxlPlugin final : public QImageIOPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "jxl.json")
  public:
    Capabilities capabilities(QIODevice* device, const QByteArray& format) const override
    {
        if (format == "jxl") {
            return CanRead;
        }
        if (!format.isEmpty() || !device || !device->isReadable()) {
            return {};
        }
        const auto header = device->peek(12);
        return header.startsWith(QByteArray::fromHex("ff0a")) ||
                       header == QByteArray::fromHex("0000000c4a584c200d0a870a")
                   ? CanRead
                   : Capabilities{};
    }
    QImageIOHandler* create(QIODevice* device, const QByteArray& format) const override
    {
        return Licasa::loadImageHandler(QStringLiteral("licasa_jxl"), device, format);
    }
};

#include "jxl_plugin.moc"
