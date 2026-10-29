#pragma once

#include <QImage>
#include <QString>
class QIODevice;

namespace Licasa {
struct ImageExportOptions;
namespace ImageExport {
QString encode(QIODevice* output, QImage image, const QByteArray& format,
               const ImageExportOptions& options);
}
} // namespace Licasa
