#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QVariantMap>

#include <atomic>

class QUrlQuery;

namespace Licasa {

struct ImageEditParameters {
    qreal exposure = 0.0;
    qreal contrast = 0.0;
    qreal highlights = 0.0;
    qreal shadows = 0.0;
    qreal saturation = 0.0;
    qreal vibrance = 0.0;
    qreal warmth = 0.0;
    qreal tint = 0.0;
    qreal soften = 0.0;
    qreal sharpen = 0.0;
    qreal vignette = 0.0;

    int quarterTurns = 0;
    bool flipHorizontal = false;
    bool flipVertical = false;

    qreal cropX = 0.0;
    qreal cropY = 0.0;
    qreal cropWidth = 1.0;
    qreal cropHeight = 1.0;

    bool hasColorAdjustments() const;
    bool hasCrop() const;
};

struct ImageExportOptions {
    qreal scale = 1.0;
    int quality = 95;
    QByteArray format;
    int width = 0;
    int height = 0;
    int compression = 50;
    bool progressive = false;
    bool optimized = true;
    bool lossless = false;
    bool nearest = false;
    bool keepAlpha = true;
    QString matte = QStringLiteral("#ffffff");
    int colorMode = 0; // RGB, grayscale, indexed
    int paletteColors = 216;
    bool dither = true;
    int threshold = 128;
    bool invert = false;
    bool ascii = false;
    int iconSize = 0; // zero preserves an existing legal icon canvas
    bool iconCrop = false;
    int hotspotX = 0;
    int hotspotY = 0;
    int dpi = 0; // zero preserves the decoded density
    bool srgb = false;
    bool stripMetadata = true;
    bool copyOnly = false;
    QString validationError;
    QString symbol = QStringLiteral("licasa_image");
};

enum class ImageEditBackend { Automatic, Cpu, OpenCl, Cuda };

struct ImageEditExecution {
    ImageEditBackend requested = ImageEditBackend::Automatic;
    ImageEditBackend used = ImageEditBackend::Cpu;
    QString detail;
};

int normalizedQuarterTurns(int value);
ImageEditParameters editParametersFromQuery(const QString& queryString);
ImageEditParameters editParametersFromQuery(const QUrlQuery& query);
ImageEditParameters editParametersFromMap(const QVariantMap& values);
ImageExportOptions exportOptionsFromMap(const QVariantMap& values);
bool applyImageEdits(QImage& image, const ImageEditParameters& parameters,
                     const std::atomic_bool* cancelled = nullptr,
                     ImageEditExecution* execution = nullptr);

} // namespace Licasa
