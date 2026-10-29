#include "export/image_export_encoder.h"
#include "export/image_export_contract.h"
#include "imaging/image_edit_pipeline.h"

#include <QBuffer>
#include <QColorSpace>
#include <QImageWriter>
#include <QPainter>
#include <QRegularExpression>
#include <QtEndian>
#include <algorithm>
#include <cmath>

namespace Licasa::ImageExport {
namespace {
// Qt 6.4's PNG writer longjmps past its row-buffer cleanup when a device
// rejects a write. Remember the first failure and drain the remaining encoded
// bytes without storing them so Qt can finish and release its allocations.
// The caller still receives the error and cancels the atomic output file.
class PngOutput final : public QIODevice {
  public:
    explicit PngOutput(QIODevice& destination) : destination_(destination)
    {
        open(QIODevice::WriteOnly);
    }
    bool isSequential() const override { return true; }
    const QString& writeError() const { return writeError_; }

  protected:
    qint64 readData(char*, qint64) override { return -1; }
    qint64 writeData(const char* data, qint64 size) override
    {
        if (writeError_.isEmpty() && destination_.write(data, size) != size) {
            writeError_ = destination_.errorString();
            if (writeError_.isEmpty()) {
                writeError_ = QStringLiteral("The PNG output could not be written.");
            }
        }
        return size;
    }

  private:
    QIODevice& destination_;
    QString writeError_;
};

bool writeBytes(QIODevice* output, const QByteArray& bytes)
{
    return output->write(bytes) == bytes.size();
}

QByteArray variableLength(quint32 value)
{
    QByteArray bytes(1, char(value & 127));
    while ((value >>= 7) != 0) {
        bytes.prepend(char((value & 127) | 128));
    }
    return bytes;
}

QImage monochrome(const QImage& image, const ImageExportOptions& options)
{
    QImage mono(image.size(), QImage::Format_Mono);
    if (mono.isNull()) {
        return {};
    }
    mono.fill(0);
    mono.setColorTable({qRgb(255, 255, 255), qRgb(0, 0, 0)});
    constexpr int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        auto* target = mono.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const int threshold =
                options.threshold + (options.dither ? bayer[y % 4][x % 4] * 8 - 60 : 0);
            const bool black = (qGray(row[x]) < threshold) != options.invert;
            if (black) {
                target[x / 8] |= uchar(128 >> (x % 8));
            }
        }
    }
    return mono;
}

QImage indexed(const QImage& image, const ImageExportOptions& options, bool transparency)
{
    int levels = std::clamp(int(std::cbrt(options.paletteColors) + 0.0001), 2, 6);
    QList<QRgb> palette;
    for (int r = 0; r < levels; ++r) {
        for (int g = 0; g < levels; ++g) {
            for (int b = 0; b < levels; ++b) {
                palette.append(
                    qRgb(r * 255 / (levels - 1), g * 255 / (levels - 1), b * 255 / (levels - 1)));
            }
        }
    }
    QImage result(image.size(), QImage::Format_Indexed8);
    if (result.isNull()) {
        return {};
    }
    const int transparentIndex = palette.size();
    if (transparency) {
        palette.append(0);
    }
    result.setColorTable(palette);
    constexpr int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (int y = 0; y < image.height(); ++y) {
        auto* target = result.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (transparency && qAlpha(pixel) < options.threshold) {
                target[x] = uchar(transparentIndex);
                continue;
            }
            // Qt's supplied-palette conversion does not consistently apply
            // color dithering across the supported runtimes. Quantize into
            // the advertised color cube with a deterministic ordered dither.
            const double offset = options.dither ? (bayer[y % 4][x % 4] + .5) / 16.0 - .5 : 0;
            auto channel = [&](int value) {
                return std::clamp(int(std::lround(value * (levels - 1) / 255.0 + offset)), 0,
                                  levels - 1);
            };
            target[x] = uchar((channel(qRed(pixel)) * levels + channel(qGreen(pixel))) * levels +
                              channel(qBlue(pixel)));
        }
    }
    return result;
}

QString writePortable(QIODevice* output, const QImage& image, const QByteArray& format,
                      const ImageExportOptions& options)
{
    const bool mono = format == "pbm" || format == "wbmp" || format == "xbm";
    if (format == "wbmp") {
        if (!writeBytes(output, QByteArray(2, '\0') + variableLength(quint32(image.width())) +
                                    variableLength(quint32(image.height())))) {
            return output->errorString();
        }
    } else if (format == "xbm") {
        QString symbol = options.symbol.left(64);
        symbol.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_]")), QStringLiteral("_"));
        if (symbol.isEmpty() || symbol.front().isDigit()) {
            symbol.prepend(QStringLiteral("image_"));
        }
        const QByteArray name = symbol.toLatin1();
        if (!writeBytes(output, "#define " + name + "_width " + QByteArray::number(image.width()) +
                                    "\n#define " + name + "_height " +
                                    QByteArray::number(image.height()) + "\nstatic unsigned char " +
                                    name + "_bits[] = {\n")) {
            return output->errorString();
        }
    } else {
        const int magic = mono              ? (options.ascii ? 1 : 4)
                          : format == "pgm" ? (options.ascii ? 2 : 5)
                                            : (options.ascii ? 3 : 6);
        const QByteArray header = "P" + QByteArray::number(magic) + '\n' +
                                  QByteArray::number(image.width()) + ' ' +
                                  QByteArray::number(image.height()) + (mono ? "\n" : "\n255\n");
        if (!writeBytes(output, header)) {
            return output->errorString();
        }
    }
    for (int y = 0; y < image.height(); ++y) {
        QByteArray row;
        if (mono) {
            const int bytes = (image.width() + 7) / 8;
            row.resize(bytes);
            for (int i = 0; i < bytes; ++i) {
                row[i] = char(image.constScanLine(y)[i]);
            }
            // WBMP type 0 uses one for white; PBM/XBM use one for black.
            if (format == "wbmp") {
                for (int i = 0; i < row.size(); ++i) {
                    row[i] = char(~uchar(row[i]));
                }
                if (image.width() % 8) {
                    row[row.size() - 1] =
                        char(uchar(row.back()) & (0xff << (8 - image.width() % 8)));
                }
            }
            if (format == "xbm") {
                QByteArray text;
                for (uchar byte : row) {
                    uchar reversed = 0;
                    for (int bit = 0; bit < 8; ++bit) {
                        reversed |= ((byte >> bit) & 1) << (7 - bit);
                    }
                    text += "0x" + QByteArray::number(reversed, 16).rightJustified(2, '0') + ',';
                }
                row = text + '\n';
            } else if (format == "pbm" && options.ascii) {
                row.clear();
                for (int x = 0; x < image.width(); ++x) {
                    row += image.pixelIndex(x, y) ? "1 " : "0 ";
                }
                row += '\n';
            }
        } else {
            for (int x = 0; x < image.width(); ++x) {
                const QRgb pixel = image.pixel(x, y);
                const int values[3] = {format == "pgm" ? qGray(pixel) : qRed(pixel), qGreen(pixel),
                                       qBlue(pixel)};
                for (int channel = 0; channel < (format == "pgm" ? 1 : 3); ++channel) {
                    if (options.ascii) {
                        row += QByteArray::number(values[channel]) + ' ';
                    } else {
                        row += char(values[channel]);
                    }
                }
            }
            if (options.ascii) {
                row += '\n';
            }
        }
        if (!writeBytes(output, row)) {
            return output->errorString();
        }
    }
    if (format == "xbm" && !writeBytes(output, "};\n")) {
        return output->errorString();
    }
    return {};
}
} // namespace

QString encode(QIODevice* output, QImage image, const QByteArray& format,
               const ImageExportOptions& options)
{
    if (!options.validationError.isEmpty()) {
        return options.validationError;
    }
    if (options.dpi < 0 || options.dpi > 2400 || !QColor(options.matte).isValid()) {
        return QStringLiteral("Choose a valid background color and density (0–2400 DPI).");
    }
    if (isIcon(format)) {
        const QList<int> sizes = format == "icns" ? QList<int>{32, 64, 128, 256, 512, 1024}
                                                  : QList<int>{16, 24, 32, 48, 64, 128, 256};
        const QSize targetSize =
            options.iconSize == 0 ? image.size() : QSize(options.iconSize, options.iconSize);
        const bool validSize =
            format == "icns"
                ? targetSize.width() == targetSize.height() && sizes.contains(targetSize.width())
                : targetSize.width() > 0 && targetSize.height() > 0 && targetSize.width() <= 256 &&
                      targetSize.height() <= 256;
        if (!validSize || (options.iconSize != 0 && !sizes.contains(options.iconSize))) {
            return QStringLiteral("Choose a supported icon size.");
        }
        if (format == "cur" &&
            (options.hotspotX < 0 || options.hotspotY < 0 ||
             options.hotspotX >= targetSize.width() || options.hotspotY >= targetSize.height())) {
            return QStringLiteral("The cursor hotspot must be inside the image.");
        }
        if (options.iconSize != 0) {
            QImage canvas(targetSize, QImage::Format_ARGB32);
            if (canvas.isNull()) {
                return QStringLiteral("The icon canvas could not be allocated.");
            }
            canvas.fill(Qt::transparent);
            const QImage scaled = image.scaled(
                canvas.size(),
                options.iconCrop ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio,
                options.nearest ? Qt::FastTransformation : Qt::SmoothTransformation);
            QPainter painter(&canvas);
            painter.drawImage((canvas.width() - scaled.width()) / 2,
                              (canvas.height() - scaled.height()) / 2, scaled);
            painter.end();
            canvas.setColorSpace(image.colorSpace());
            image = canvas;
        }
    }
    const bool profileSupported = format == "png" || format == "jpeg" || format == "tiff";
    if (options.srgb || !profileSupported) {
        if (image.colorSpace().isValid()) {
            image.convertToColorSpace(QColorSpace::SRgb);
        } else {
            image.setColorSpace(QColorSpace::SRgb);
        }
    }
    if (options.stripMetadata) {
        for (const auto& key : image.textKeys()) {
            image.setText(key, QString());
        }
    }
    const bool mono = format == "pbm" || format == "wbmp" || format == "xbm";
    const bool alpha = options.keepAlpha && (format == "png" || format == "webp" ||
                                             format == "tiff" || isIcon(format) || format == "xpm");
    if (!alpha) {
        QImage flattened(image.size(), QImage::Format_RGB32);
        if (flattened.isNull()) {
            return QStringLiteral("The export raster could not be allocated.");
        }
        flattened.fill(QColor(options.matte));
        QPainter painter(&flattened);
        painter.drawImage(0, 0, image);
        painter.end();
        flattened.setColorSpace(image.colorSpace());
        flattened.setDotsPerMeterX(image.dotsPerMeterX());
        flattened.setDotsPerMeterY(image.dotsPerMeterY());
        image = flattened;
    }
    if (options.dpi > 0) {
        image.setDotsPerMeterX(qRound(options.dpi / 0.0254));
        image.setDotsPerMeterY(qRound(options.dpi / 0.0254));
    }
    const int dpmX = image.dotsPerMeterX(), dpmY = image.dotsPerMeterY();
    const QColorSpace colorSpace = image.colorSpace();
    if (mono) {
        image = monochrome(image.convertToFormat(QImage::Format_RGB32), options);
    } else if (options.colorMode == 1 || format == "pgm") {
        if (alpha && image.hasAlphaChannel()) {
            image = image.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < image.height(); ++y) {
                auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
                for (int x = 0; x < image.width(); ++x) {
                    const int gray = qGray(row[x]);
                    row[x] = qRgba(gray, gray, gray, qAlpha(row[x]));
                }
            }
        } else {
            image = image.convertToFormat(QImage::Format_Grayscale8);
        }
    } else if (options.colorMode == 2 || format == "xpm") {
        image = indexed(image, options, alpha);
    }
    if (image.isNull()) {
        return QStringLiteral("The output pixels could not be allocated.");
    }
    image.setDotsPerMeterX(dpmX);
    image.setDotsPerMeterY(dpmY);
    image.setColorSpace(colorSpace);
    if (mono || format == "pgm" || format == "ppm") {
        return writePortable(output, image, format, options);
    }
    if (format == "cur") {
        QByteArray data;
        QBuffer buffer(&data);
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer(&buffer, "ico");
        if (!writer.write(image)) {
            return writer.errorString();
        }
        if (data.size() < 22) {
            return QStringLiteral("The cursor image could not be encoded.");
        }
        qToLittleEndian<quint16>(2, data.data() + 2);
        qToLittleEndian<quint16>(quint16(options.hotspotX), data.data() + 10);
        qToLittleEndian<quint16>(quint16(options.hotspotY), data.data() + 12);
        return writeBytes(output, data) ? QString() : output->errorString();
    }
    // The XPM writer's alpha discovery must see pixels, not only an indexed
    // color table; otherwise a transparent palette entry can become opaque.
    if (format == "xpm") {
        image = image.convertToFormat(QImage::Format_ARGB32);
    }
    PngOutput pngOutput(*output);
    QImageWriter writer(format == "png" ? &pngOutput : output, format);
    if (format == "png") {
        writer.setCompression(options.compression);
    }
    if (format == "tiff") {
        writer.setCompression(options.compression == 0 ? 0 : 1);
    }
    if (format == "jpeg") {
        writer.setQuality(options.quality);
        writer.setProgressiveScanWrite(options.progressive);
        writer.setOptimizedWrite(options.optimized);
    }
    if (format == "webp") {
        writer.setQuality(options.lossless ? 100 : std::min(options.quality, 99));
    }
    const bool written = writer.write(image);
    if (!pngOutput.writeError().isEmpty()) {
        return pngOutput.writeError();
    }
    return written ? QString() : writer.errorString();
}
} // namespace Licasa::ImageExport
