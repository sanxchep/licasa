#pragma once

#include <QImageWriter>
#include <QVariantList>

namespace Licasa::ImageExport {
inline QByteArray canonicalFormat(QByteArray value)
{
    value = value.trimmed().toLower();
    if (value == "jpg" || value == "jfif") {
        return "jpeg";
    }
    if (value == "tif") {
        return "tiff";
    }
    return value;
}

inline bool isIcon(const QByteArray& format)
{
    return format == "ico" || format == "cur" || format == "icns";
}

inline QVariantList formats()
{
    const QList<QByteArray> writers = QImageWriter::supportedImageFormats();
    QVariantList result;
    const QList<QPair<QByteArray, QString>> names = {
        {"png", "PNG"}, {"jpeg", "JPEG"}, {"webp", "WebP"}, {"tiff", "TIFF"}, {"bmp", "BMP"},
        {"ico", "ICO"}, {"cur", "CUR"},   {"icns", "ICNS"}, {"pbm", "PBM"},   {"pgm", "PGM"},
        {"ppm", "PPM"}, {"wbmp", "WBMP"}, {"xbm", "XBM"},   {"xpm", "XPM"}};
    for (const auto& entry : names) {
        if (!writers.contains(entry.first) && !(entry.first == "cur" && writers.contains("ico"))) {
            continue;
        }
        const QString suffix =
            entry.first == "jpeg" ? QStringLiteral("jpg") : QString::fromLatin1(entry.first);
        result.append(QVariantMap{{"key", QString::fromLatin1(entry.first)},
                                  {"label", entry.second},
                                  {"suffix", suffix}});
    }
    return result;
}
} // namespace Licasa::ImageExport
