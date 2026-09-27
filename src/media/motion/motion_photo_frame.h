#pragma once

#include <QByteArray>
#include <QSize>
#include <QtGlobal>

#include <limits>

namespace Licasa {

// Owned, decoder-independent raster used only for explicit current-frame export.
// The private multimedia backend copies one requested frame into this Core-only
// structure; QVideoFrame/QImage objects never cross into QML or the lightweight
// MotionPhotoSession ABI. Pixels are RGBA8888 with an explicit row stride.
struct MotionPhotoFrameRaster {
    QByteArray rgba8888;
    QByteArray iccProfile;
    QSize size;
    int bytesPerLine = 0;
    qint64 timestampUs = -1;

    bool isValid() const noexcept
    {
        const qint64 minimumBytesPerLine = qint64(size.width()) * 4;
        if (!size.isValid() || size.width() <= 0 || size.height() <= 0 ||
            minimumBytesPerLine > std::numeric_limits<int>::max() ||
            bytesPerLine < minimumBytesPerLine) {
            return false;
        }
        const qint64 required = qint64(bytesPerLine) * qint64(size.height());
        return required > 0 && required <= std::numeric_limits<qsizetype>::max() &&
               rgba8888.size() == qsizetype(required);
    }
};

} // namespace Licasa
