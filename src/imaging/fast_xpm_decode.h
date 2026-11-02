#pragma once

#include <QImage>
#include <QString>
#include <atomic>

namespace Licasa {
// A narrow native decoder for plain, unescaped one/two-byte XPM palettes.
// Unsupported C syntax/color declarations return null for Qt's normal fallback.
QImage readFastXpm(const QString& path, quint64 maximumPixels, quint64 maximumBytes,
                   const std::atomic_bool* cancelled);
} // namespace Licasa
