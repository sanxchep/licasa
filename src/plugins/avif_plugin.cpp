#include "container_signatures.h"
#include "image_plugin_loader.h"

#include <QImageIOPlugin>

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
        return Licasa::ContainerSignatures::avifSignature(device) ? CanRead : Capabilities{};
    }

    QImageIOHandler* create(QIODevice* device, const QByteArray& format) const override
    {
        return Licasa::loadImageHandler(QStringLiteral("licasa_avif"), device,
                                        format.isEmpty() ? QByteArray("avif") : format);
    }
};

#include "avif_plugin.moc"
