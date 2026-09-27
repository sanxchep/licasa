#pragma once

#include "imaging/image_decode_contract.h"

#include <QCoreApplication>
#include <QDir>
#include <QImageIOHandler>
#include <QLibrary>

namespace Licasa {
// Qt enumerates capabilities at startup. Keep that plugin cheap; load the
// native backend only after a matching file is opened. QLibrary retains it for
// outstanding QImage cleanup callbacks, which can outlive their reader.
inline QImageIOHandler* loadImageHandler(const QString& name, QIODevice* device,
                                         const QByteArray& format)
{
    using Factory = QImageIOHandler* (*)();
    for (const auto& root : QCoreApplication::libraryPaths()) {
        const QString path = QDir(root).filePath(QStringLiteral("licasa-codecs/") + name);
        const auto factory =
            reinterpret_cast<Factory>(QLibrary::resolve(path, "licasaCreateImageHandler"));
        if (factory) {
            auto* handler = factory();
            handler->setDevice(device);
            handler->setFormat(format);
            return handler;
        }
    }
    // A broken package still needs a handler object: Qt assumes create() works
    // after a plugin claims the format. Surface the packaging failure on read.
    class UnavailableHandler final : public QImageIOHandler {
      public:
        bool canRead() const override { return false; }
        bool read(QImage*) override
        {
            if (device()) {
                device()->setProperty(
                    ImageDecodeContract::errorProperty,
                    QStringLiteral("The image decoder is missing from this Licasa installation."));
            }
            return false;
        }
    };
    auto* handler = new UnavailableHandler;
    handler->setDevice(device);
    handler->setFormat(format);
    return handler;
}
} // namespace Licasa
