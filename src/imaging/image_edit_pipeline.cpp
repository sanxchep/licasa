#include "imaging/image_edit_pipeline.h"
#include "imaging/compute/edit_compute.h"

#include <QColorSpace>
#include <QRect>
#include <QTransform>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace Licasa {
namespace {

constexpr qreal kMinimumCropExtent = 0.001;

qreal boundedFinite(qreal value, qreal minimum, qreal maximum, qreal fallback)
{
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

qreal clampSignedUnit(qreal value) { return boundedFinite(value, -1.0, 1.0, 0.0); }

qreal clampUnit(qreal value) { return boundedFinite(value, 0.0, 1.0, 0.0); }

bool parseBooleanQueryValue(const QString& value)
{
    const QString normalized = value.trimmed().toLower();
    return normalized == QStringLiteral("1") || normalized == QStringLiteral("true") ||
           normalized == QStringLiteral("yes") || normalized == QStringLiteral("on");
}

void normalizeCrop(ImageEditParameters& parameters)
{
    parameters.cropX = boundedFinite(parameters.cropX, 0.0, 1.0 - kMinimumCropExtent, 0.0);
    parameters.cropY = boundedFinite(parameters.cropY, 0.0, 1.0 - kMinimumCropExtent, 0.0);
    parameters.cropWidth = boundedFinite(parameters.cropWidth, kMinimumCropExtent,
                                         1.0 - parameters.cropX, 1.0 - parameters.cropX);
    parameters.cropHeight = boundedFinite(parameters.cropHeight, kMinimumCropExtent,
                                          1.0 - parameters.cropY, 1.0 - parameters.cropY);
}

ImageEditParameters normalizedEditParameters(ImageEditParameters parameters)
{
    parameters.exposure = clampSignedUnit(parameters.exposure);
    parameters.contrast = clampSignedUnit(parameters.contrast);
    parameters.highlights = clampSignedUnit(parameters.highlights);
    parameters.shadows = clampSignedUnit(parameters.shadows);
    parameters.saturation = clampSignedUnit(parameters.saturation);
    parameters.vibrance = clampSignedUnit(parameters.vibrance);
    parameters.warmth = clampSignedUnit(parameters.warmth);
    parameters.tint = clampSignedUnit(parameters.tint);
    parameters.soften = boundedFinite(parameters.soften, 0.0, 0.35, 0.0);
    parameters.sharpen = clampUnit(parameters.sharpen);
    parameters.vignette = clampUnit(parameters.vignette);
    parameters.quarterTurns = normalizedQuarterTurns(parameters.quarterTurns);
    normalizeCrop(parameters);
    return parameters;
}

int clampColorChannel(double value)
{
    if (!std::isfinite(value)) {
        return 0;
    }
    return static_cast<int>(std::clamp(std::round(value), 0.0, 255.0));
}

bool editCancelled(const std::atomic_bool* cancelled)
{
    return cancelled && cancelled->load(std::memory_order_relaxed);
}

struct ColorChannels {
    double red;
    double green;
    double blue;
};

// Per-image decisions and constants stay outside the pixel loop. Each stage
// retains the original arithmetic order so preview and export pixels agree.
class ColorAdjustmentPlan {
  public:
    ColorAdjustmentPlan(const ImageEditParameters& parameters, const QSize& size)
        : parameters_(parameters), exposureFactor_(std::pow(2.0, parameters.exposure)),
          contrastFactor_(std::max(0.0, 1.0 + parameters.contrast)),
          warmthShift_(parameters.warmth * 42.0), tintShift_(parameters.tint * 36.0),
          saturationFactor_(std::max(0.0, 1.0 + parameters.saturation)),
          centerX_(std::max(1.0, (size.width() - 1) * 0.5)),
          centerY_(std::max(1.0, (size.height() - 1) * 0.5)),
          needsTonalWeights_(parameters.shadows != 0.0 || parameters.highlights != 0.0)
    {
        if (parameters.vignette > 0.0 && size.width() <= 65536) {
            horizontalRadiusSquared_.reserve(size.width());
            for (int x = 0; x < size.width(); ++x) {
                const double normalized = (x - centerX_) / centerX_;
                horizontalRadiusSquared_.push_back(normalized * normalized);
            }
        }
    }

    double rowRadiusSquared(int y) const
    {
        const double normalized = (y - centerY_) / centerY_;
        return normalized * normalized;
    }

    QRgb apply(QRgb pixel, int x, double verticalRadiusSquared) const
    {
        if (qAlpha(pixel) == 0) {
            return pixel;
        }
        ColorChannels color{qRed(pixel) * exposureFactor_, qGreen(pixel) * exposureFactor_,
                            qBlue(pixel) * exposureFactor_};
        adjustTone(color);
        adjustTemperature(color);
        adjustSaturation(color);
        applyVignette(color, x, verticalRadiusSquared);
        return qRgba(clampColorChannel(color.red), clampColorChannel(color.green),
                     clampColorChannel(color.blue), qAlpha(pixel));
    }

  private:
    void adjustTone(ColorChannels& color) const
    {
        double tonalShift = 0.0;
        if (needsTonalWeights_) {
            const double luminance = std::clamp(
                (0.299 * color.red + 0.587 * color.green + 0.114 * color.blue) / 255.0, 0.0, 1.0);
            const double shadowWeight = std::pow(1.0 - luminance, 2.0);
            const double highlightWeight = std::pow(luminance, 2.0);
            tonalShift = parameters_.shadows * 72.0 * shadowWeight +
                         parameters_.highlights * 72.0 * highlightWeight;
        }
        color.red = (color.red - 127.5) * contrastFactor_ + 127.5 + tonalShift;
        color.green = (color.green - 127.5) * contrastFactor_ + 127.5 + tonalShift;
        color.blue = (color.blue - 127.5) * contrastFactor_ + 127.5 + tonalShift;
    }

    void adjustTemperature(ColorChannels& color) const
    {
        color.red += warmthShift_ + tintShift_ * 0.45;
        color.green -= tintShift_;
        color.blue += -warmthShift_ + tintShift_ * 0.45;
    }

    void adjustSaturation(ColorChannels& color) const
    {
        const double gray = 0.299 * color.red + 0.587 * color.green + 0.114 * color.blue;
        double factor = saturationFactor_;
        if (parameters_.vibrance != 0.0) {
            const double maximum = std::max({color.red, color.green, color.blue});
            const double minimum = std::min({color.red, color.green, color.blue});
            const double chroma = std::clamp((maximum - minimum) / 255.0, 0.0, 1.0);
            factor = std::max(0.0, 1.0 + parameters_.saturation +
                                       parameters_.vibrance * (1.0 - chroma) * 0.75);
        }
        color.red = gray + (color.red - gray) * factor;
        color.green = gray + (color.green - gray) * factor;
        color.blue = gray + (color.blue - gray) * factor;
    }

    void applyVignette(ColorChannels& color, int x, double verticalRadiusSquared) const
    {
        if (parameters_.vignette <= 0.0) {
            return;
        }
        const double normalizedX = (x - centerX_) / centerX_;
        const double horizontal = horizontalRadiusSquared_.empty() ? normalizedX * normalizedX
                                                                   : horizontalRadiusSquared_[x];
        const double radius = std::clamp(std::sqrt(horizontal + verticalRadiusSquared), 0.0, 1.5);
        const double edge = std::clamp((radius - 0.28) / 0.92, 0.0, 1.0);
        const double smoothEdge = edge * edge * (3.0 - 2.0 * edge);
        const double factor = 1.0 - parameters_.vignette * 0.72 * smoothEdge;
        color.red *= factor;
        color.green *= factor;
        color.blue *= factor;
    }

    const ImageEditParameters& parameters_;
    const double exposureFactor_;
    const double contrastFactor_;
    const double warmthShift_;
    const double tintShift_;
    const double saturationFactor_;
    const double centerX_;
    const double centerY_;
    const bool needsTonalWeights_;
    // Cache at most 512 KiB on one axis, never a second full raster. Keep divisions and
    // additions in their original order to preserve exact output pixels.
    std::vector<double> horizontalRadiusSquared_;
};

bool applyColorAdjustments(QImage& image, const ImageEditParameters& parameters,
                           const std::atomic_bool* cancelled)
{
    if (!parameters.hasColorAdjustments()) {
        return true;
    }
    image = std::move(image).convertToFormat(QImage::Format_ARGB32);
    if (image.isNull()) {
        return false;
    }

    const ColorAdjustmentPlan plan(parameters, image.size());
    for (int y = 0; y < image.height(); ++y) {
        if (editCancelled(cancelled)) {
            return false;
        }
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        const double verticalRadiusSquared = plan.rowRadiusSquared(y);
        for (int x = 0; x < image.width(); ++x) {
            row[x] = plan.apply(row[x], x, verticalRadiusSquared);
        }
    }
    return true;
}

QImage applySoften(const QImage& source, qreal amount)
{
    const qreal bounded = std::clamp(amount, 0.0, 0.35);
    if (bounded <= 0.0 || source.isNull() || source.width() < 2 || source.height() < 2) {
        return source;
    }

    const qreal scaleFactor = std::max<qreal>(0.28, 1.0 - bounded * 0.72);
    const QSize reducedSize(
        std::max(1, static_cast<int>(std::lround(source.width() * scaleFactor))),
        std::max(1, static_cast<int>(std::lround(source.height() * scaleFactor))));

    if (reducedSize == source.size()) {
        return source;
    }

    const QImage reduced =
        source.scaled(reducedSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return reduced.scaled(source.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

bool applySharpen(QImage& image, qreal amount, const std::atomic_bool* cancelled)
{
    const qreal bounded = clampUnit(amount);
    if (bounded <= 0.0 || image.isNull() || image.width() < 3 || image.height() < 3) {
        return true;
    }

    image = std::move(image).convertToFormat(QImage::Format_ARGB32);
    if (image.isNull()) {
        return false;
    }
    // Preserve only the two original rows that an in-place stencil overwrites.
    // The next row is still untouched. Detach once for shared caller images;
    // an owned ARGB32 raster needs no second full-size allocation.
    QImage rows(image.width(), 2, QImage::Format_ARGB32);
    if (rows.isNull() || !image.bits()) {
        return false;
    }
    const size_t rowBytes = size_t(image.width()) * sizeof(QRgb);
    auto* previous = reinterpret_cast<QRgb*>(rows.scanLine(0));
    auto* current = reinterpret_cast<QRgb*>(rows.scanLine(1));
    std::memcpy(previous, image.constScanLine(0), rowBytes);
    const double strength = bounded * 0.48;

    for (int y = 1; y < image.height() - 1; ++y) {
        if (editCancelled(cancelled)) {
            return false;
        }
        std::memcpy(current, image.constScanLine(y), rowBytes);
        const auto* next = reinterpret_cast<const QRgb*>(image.constScanLine(y + 1));
        auto* destination = reinterpret_cast<QRgb*>(image.scanLine(y));

        for (int x = 1; x < image.width() - 1; ++x) {
            const QRgb center = current[x];
            const auto sharpenChannel = [strength](int centerValue, int left, int right, int top,
                                                   int bottom) {
                const double detail = centerValue * 4.0 - left - right - top - bottom;
                return clampColorChannel(centerValue + detail * strength);
            };

            destination[x] =
                qRgba(sharpenChannel(qRed(center), qRed(current[x - 1]), qRed(current[x + 1]),
                                     qRed(previous[x]), qRed(next[x])),
                      sharpenChannel(qGreen(center), qGreen(current[x - 1]), qGreen(current[x + 1]),
                                     qGreen(previous[x]), qGreen(next[x])),
                      sharpenChannel(qBlue(center), qBlue(current[x - 1]), qBlue(current[x + 1]),
                                     qBlue(previous[x]), qBlue(next[x])),
                      qAlpha(center));
        }
        std::swap(previous, current);
    }
    return true;
}

QRect cropRectForImage(const QSize& imageSize, const ImageEditParameters& parameters)
{
    const int left = std::clamp(static_cast<int>(std::floor(parameters.cropX * imageSize.width())),
                                0, std::max(0, imageSize.width() - 1));
    const int top = std::clamp(static_cast<int>(std::floor(parameters.cropY * imageSize.height())),
                               0, std::max(0, imageSize.height() - 1));
    const int right = std::clamp(
        static_cast<int>(std::ceil((parameters.cropX + parameters.cropWidth) * imageSize.width())),
        left + 1, imageSize.width());
    const int bottom =
        std::clamp(static_cast<int>(
                       std::ceil((parameters.cropY + parameters.cropHeight) * imageSize.height())),
                   top + 1, imageSize.height());
    return QRect(left, top, right - left, bottom - top);
}

} // namespace

bool ImageEditParameters::hasColorAdjustments() const
{
    return !qFuzzyIsNull(exposure) || !qFuzzyIsNull(contrast) || !qFuzzyIsNull(highlights) ||
           !qFuzzyIsNull(shadows) || !qFuzzyIsNull(saturation) || !qFuzzyIsNull(vibrance) ||
           !qFuzzyIsNull(warmth) || !qFuzzyIsNull(tint) || !qFuzzyIsNull(vignette);
}

bool ImageEditParameters::hasCrop() const
{
    return !qFuzzyIsNull(cropX) || !qFuzzyIsNull(cropY) || !qFuzzyCompare(cropWidth, 1.0) ||
           !qFuzzyCompare(cropHeight, 1.0);
}

int normalizedQuarterTurns(int value)
{
    const int remainder = value % 4;
    return remainder < 0 ? remainder + 4 : remainder;
}

ImageEditParameters editParametersFromQuery(const QString& queryString)
{
    ImageEditParameters parameters;
    if (queryString.isEmpty()) {
        return parameters;
    }

    QUrlQuery query;
    query.setQuery(queryString);
    parameters.exposure = query.queryItemValue(QStringLiteral("b")).toDouble();
    parameters.contrast = query.queryItemValue(QStringLiteral("c")).toDouble();
    parameters.highlights = query.queryItemValue(QStringLiteral("hi")).toDouble();
    parameters.shadows = query.queryItemValue(QStringLiteral("sh")).toDouble();
    parameters.saturation = query.queryItemValue(QStringLiteral("s")).toDouble();
    parameters.vibrance = query.queryItemValue(QStringLiteral("vib")).toDouble();
    parameters.warmth = query.queryItemValue(QStringLiteral("w")).toDouble();
    parameters.tint = query.queryItemValue(QStringLiteral("t")).toDouble();
    parameters.soften = query.queryItemValue(QStringLiteral("blur")).toDouble();
    parameters.sharpen = query.queryItemValue(QStringLiteral("sharp")).toDouble();
    parameters.vignette = query.queryItemValue(QStringLiteral("vig")).toDouble();
    parameters.quarterTurns = query.queryItemValue(QStringLiteral("r")).toInt();
    parameters.flipHorizontal = parseBooleanQueryValue(query.queryItemValue(QStringLiteral("fh")));
    parameters.flipVertical = parseBooleanQueryValue(query.queryItemValue(QStringLiteral("fv")));
    parameters.cropX = query.queryItemValue(QStringLiteral("cx")).toDouble();
    parameters.cropY = query.queryItemValue(QStringLiteral("cy")).toDouble();
    parameters.cropWidth = query.hasQueryItem(QStringLiteral("cw"))
                               ? query.queryItemValue(QStringLiteral("cw")).toDouble()
                               : 1.0;
    parameters.cropHeight = query.hasQueryItem(QStringLiteral("ch"))
                                ? query.queryItemValue(QStringLiteral("ch")).toDouble()
                                : 1.0;
    return normalizedEditParameters(parameters);
}

ImageEditParameters editParametersFromMap(const QVariantMap& values)
{
    ImageEditParameters parameters;
    parameters.exposure = values.value(QStringLiteral("exposure")).toDouble();
    parameters.contrast = values.value(QStringLiteral("contrast")).toDouble();
    parameters.highlights = values.value(QStringLiteral("highlights")).toDouble();
    parameters.shadows = values.value(QStringLiteral("shadows")).toDouble();
    parameters.saturation = values.value(QStringLiteral("saturation")).toDouble();
    parameters.vibrance = values.value(QStringLiteral("vibrance")).toDouble();
    parameters.warmth = values.value(QStringLiteral("warmth")).toDouble();
    parameters.tint = values.value(QStringLiteral("tint")).toDouble();
    parameters.soften = values.value(QStringLiteral("soften")).toDouble();
    parameters.sharpen = values.value(QStringLiteral("sharpen")).toDouble();
    parameters.vignette = values.value(QStringLiteral("vignette")).toDouble();
    parameters.quarterTurns = values.value(QStringLiteral("quarterTurns")).toInt();
    parameters.flipHorizontal = values.value(QStringLiteral("flipHorizontal")).toBool();
    parameters.flipVertical = values.value(QStringLiteral("flipVertical")).toBool();
    parameters.cropX = values.value(QStringLiteral("cropX"), 0.0).toDouble();
    parameters.cropY = values.value(QStringLiteral("cropY"), 0.0).toDouble();
    parameters.cropWidth = values.value(QStringLiteral("cropWidth"), 1.0).toDouble();
    parameters.cropHeight = values.value(QStringLiteral("cropHeight"), 1.0).toDouble();
    return normalizedEditParameters(parameters);
}

ImageExportOptions exportOptionsFromMap(const QVariantMap& values)
{
    ImageExportOptions options;
    options.scale =
        boundedFinite(values.value(QStringLiteral("scale"), 1.0).toDouble(), 0.1, 2.0, 1.0);
    options.quality = std::clamp(values.value(QStringLiteral("quality"), 95).toInt(), 1, 100);
    options.format = values.value(QStringLiteral("format")).toByteArray().trimmed().toLower();
    if (options.format == QByteArrayLiteral("jpg")) {
        options.format = QByteArrayLiteral("jpeg");
    }
    if (options.format == QByteArrayLiteral("original")) {
        options.format.clear();
    }
    if (options.format != QByteArrayLiteral("png") && options.format != QByteArrayLiteral("jpeg") &&
        options.format != QByteArrayLiteral("webp")) {
        options.format.clear();
    }
    return options;
}

bool applyImageEdits(QImage& image, const ImageEditParameters& parameters,
                     const std::atomic_bool* cancelled, ImageEditExecution* execution)
{
    ImageEditExecution localExecution;
    ImageEditExecution& compute = execution ? *execution : localExecution;
    compute.used = ImageEditBackend::Cpu;
    compute.detail = QStringLiteral("CPU");
    if (compute.requested == ImageEditBackend::Automatic) {
        const QByteArray backend = qgetenv("LICASA_EDIT_BACKEND");
        if (backend == "cpu") {
            compute.requested = ImageEditBackend::Cpu;
        } else if (backend == "opencl") {
            compute.requested = ImageEditBackend::OpenCl;
        } else if (backend == "cuda") {
            compute.requested = ImageEditBackend::Cuda;
        }
    }
    if (image.isNull()) {
        return true;
    }
    if (editCancelled(cancelled)) {
        return false;
    }

    const ImageEditParameters safeParameters = normalizedEditParameters(parameters);

    if (safeParameters.quarterTurns != 0) {
        QTransform transform;
        transform.rotate(safeParameters.quarterTurns * 90.0);
        image = image.transformed(transform, Qt::FastTransformation);
        if (image.isNull()) {
            return false;
        }
    }

    if (editCancelled(cancelled)) {
        return false;
    }

    if (safeParameters.flipHorizontal || safeParameters.flipVertical) {
        image =
            std::move(image).mirrored(safeParameters.flipHorizontal, safeParameters.flipVertical);
        if (image.isNull()) {
            return false;
        }
    }

    if (safeParameters.hasCrop()) {
        image = image.copy(cropRectForImage(image.size(), safeParameters));
        if (image.isNull()) {
            return false;
        }
    }

    const bool color = safeParameters.hasColorAdjustments();
    const bool sharpen = safeParameters.sharpen > 0.0 && image.width() >= 3 && image.height() >= 3;
    // Keep color + sharpen on the device in one request when no CPU resampling
    // separates them. Geometry and Qt's exact soften filter retain their format
    // and rounding semantics; neither requires a GPU merely to move pixels.
    const bool gpuColorAndSharpen =
        safeParameters.soften == 0.0 &&
        tryGpuImageEdits(image, safeParameters, color, sharpen, cancelled, compute);
    const bool gpuColor = !gpuColorAndSharpen && safeParameters.soften > 0.0 && color &&
                          tryGpuImageEdits(image, safeParameters, true, false, cancelled, compute);
    if (!gpuColorAndSharpen && !gpuColor &&
        !applyColorAdjustments(image, safeParameters, cancelled)) {
        return false;
    }

    if (safeParameters.soften > 0.0) {
        image = applySoften(image, safeParameters.soften);
        if (image.isNull()) {
            return false;
        }
    }
    if (editCancelled(cancelled)) {
        return false;
    }
    if (!gpuColorAndSharpen && sharpen &&
        !(safeParameters.soften > 0.0 &&
          tryGpuImageEdits(image, safeParameters, false, true, cancelled, compute)) &&
        !applySharpen(image, safeParameters.sharpen, cancelled)) {
        return false;
    }

    if (!image.colorSpace().isValid()) {
        image.setColorSpace(QColorSpace::SRgb);
    }
    return !editCancelled(cancelled);
}

} // namespace Licasa
