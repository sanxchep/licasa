#pragma once

#include <QString>
#include <QtGlobal>

class QImageReader;

namespace Licasa::ImageProcessing {
// Internal policy/error vocabulary shared by decoding and saving.
quint64 pixelsForMegapixels(int megapixels);
QString imageLimitError(int maximumMegapixels);
QString readerErrorOrFallback(const QImageReader& reader);
} // namespace Licasa::ImageProcessing
