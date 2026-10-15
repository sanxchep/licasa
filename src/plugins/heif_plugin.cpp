#include "container_signatures.h"
#include "image_plugin_loader.h"

#include <QImageIOPlugin>

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
        return format.isEmpty() && Licasa::ContainerSignatures::heifSignature(device)
                   ? CanRead
                   : Capabilities{};
    }
    QImageIOHandler* create(QIODevice* device, const QByteArray& format) const override
    {
        return Licasa::loadImageHandler(QStringLiteral("licasa_heif"), device, format);
    }
};

#include "heif_plugin.moc"
