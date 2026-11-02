#pragma once

#include <QByteArray>
#include <QString>
#include <atomic>

namespace Licasa {
// Optional bounded single-member gzip input. Null means use Qt's original
// reader, including unsupported envelopes, size limits and failed checksums.
QByteArray readFastSvgzInput(const QString& path, quint64 maximumBytes,
                             const std::atomic_bool* cancelled);
} // namespace Licasa
