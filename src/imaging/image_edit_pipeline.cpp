#include "imaging/image_edit_pipeline.h"
#include "imaging/compute/edit_compute.h"

#include <QColorSpace>
#include <QRect>
#include <QThread>
#include <QTransform>
#include <QUrlQuery>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <thread>
#include <utility>
#include <vector>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

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
    value = std::clamp(value, 0.0, 255.0);
    // Subtract before testing the half: adding 0.5 can round a value just
    // below a half upward. Truncation is safe after the finite range checks.
    const int integer = static_cast<int>(value);
    return integer + (value - integer >= 0.5);
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
        : parameters_(parameters), warmthShift_(parameters.warmth * 42.0),
          tintShift_(parameters.tint * 36.0),
          saturationFactor_(std::max(0.0, 1.0 + parameters.saturation)),
          centerX_(std::max(1.0, (size.width() - 1) * 0.5)),
          centerY_(std::max(1.0, (size.height() - 1) * 0.5)),
          needsTonalWeights_(parameters.shadows != 0.0 || parameters.highlights != 0.0),
          monochrome_(parameters.saturation == -1.0 && parameters.vibrance <= 0.0)
    {
        const double exposureFactor = std::pow(2.0, parameters.exposure);
        const double contrastFactor = std::max(0.0, 1.0 + parameters.contrast);
        for (int value = 0; value < 256; ++value) {
            const double exposed = value * exposureFactor;
            exposedChannels_[value] = exposed;
            const double contrasted = (exposed - 127.5) * contrastFactor + 127.5;
            ColorChannels color{contrasted, contrasted, contrasted};
            if (!needsTonalWeights_) {
                // Retain the tone step's disabled shift before temperature.
                color.red += 0.0;
                color.green += 0.0;
                color.blue += 0.0;
                adjustTemperature(color);
            }
            channelColors_[value] = color;
        }
        if (!needsTonalWeights_ && parameters.saturation == 0.0 && parameters.vibrance == 0.0 &&
            parameters.vignette == 0.0) {
            finalChannelsExact_ = true;
            for (int value = 0; value < 256; ++value) {
                const ColorChannels color = channelColors_[value];
                // Normalized controls keep each channel within +/-1024 and
                // gray within +/-2048. In gray + (channel - gray), each rounded
                // operation therefore contributes less than 2^-41 error. A
                // 1e-9 clearance from every half-integer is conservative even
                // for their sum. Preserve the RGB calculation for requests with
                // any ambiguous channel; exact halves can round differently.
                const auto safelyRounded = [](double channel) {
                    return std::abs(channel - (std::floor(channel) + 0.5)) > 1e-9;
                };
                if (!safelyRounded(color.red) || !safelyRounded(color.green) ||
                    !safelyRounded(color.blue)) {
                    finalChannelsExact_ = false;
                    break;
                }
                finalChannels_[value] =
                    qRgb(clampColorChannel(color.red), clampColorChannel(color.green),
                         clampColorChannel(color.blue));
            }
        }
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

    bool hasFinalChannels() const { return finalChannelsExact_; }

    QRgb applyFinalChannels(QRgb pixel) const
    {
        if (qAlpha(pixel) == 0) {
            return pixel;
        }
        return (pixel & 0xff000000u) | (finalChannels_[qRed(pixel)] & 0x00ff0000u) |
               (finalChannels_[qGreen(pixel)] & 0x0000ff00u) |
               (finalChannels_[qBlue(pixel)] & 0x000000ffu);
    }

    QRgb apply(QRgb pixel, double vignetteFactor) const
    {
        if (qAlpha(pixel) == 0) {
            return pixel;
        }
        ColorChannels color{channelColors_[qRed(pixel)].red, channelColors_[qGreen(pixel)].green,
                            channelColors_[qBlue(pixel)].blue};
        if (needsTonalWeights_) {
            adjustTone(color, pixel);
            adjustTemperature(color);
        }
        if (monochrome_) {
            // Full desaturation with nonpositive vibrance always has a zero
            // saturation factor. All finite channels therefore become the
            // original weighted gray, including after temperature and tone.
            double gray = 0.299 * color.red + 0.587 * color.green + 0.114 * color.blue;
            if (parameters_.vignette > 0.0) {
                gray *= vignetteFactor;
            }
            const int channel = clampColorChannel(gray);
            return qRgba(channel, channel, channel, qAlpha(pixel));
        }
        adjustSaturation(color);
        if (parameters_.vignette > 0.0) {
            color.red *= vignetteFactor;
            color.green *= vignetteFactor;
            color.blue *= vignetteFactor;
        }
        return qRgba(clampColorChannel(color.red), clampColorChannel(color.green),
                     clampColorChannel(color.blue), qAlpha(pixel));
    }

    double vignetteFactor(int x, double verticalRadiusSquared) const
    {
        const double normalizedX = (x - centerX_) / centerX_;
        const double horizontal = horizontalRadiusSquared_.empty() ? normalizedX * normalizedX
                                                                   : horizontalRadiusSquared_[x];
        const double radius = std::clamp(std::sqrt(horizontal + verticalRadiusSquared), 0.0, 1.5);
        const double edge = std::clamp((radius - 0.28) / 0.92, 0.0, 1.0);
        const double smoothEdge = edge * edge * (3.0 - 2.0 * edge);
        return 1.0 - parameters_.vignette * 0.72 * smoothEdge;
    }

  private:
    void adjustTone(ColorChannels& color, QRgb pixel) const
    {
        const double luminance = std::clamp((0.299 * exposedChannels_[qRed(pixel)] +
                                             0.587 * exposedChannels_[qGreen(pixel)] +
                                             0.114 * exposedChannels_[qBlue(pixel)]) /
                                                255.0,
                                            0.0, 1.0);
        const double shadowWeight = std::pow(1.0 - luminance, 2.0);
        const double highlightWeight = std::pow(luminance, 2.0);
        const double tonalShift = parameters_.shadows * 72.0 * shadowWeight +
                                  parameters_.highlights * 72.0 * highlightWeight;
        color.red += tonalShift;
        color.green += tonalShift;
        color.blue += tonalShift;
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

    const ImageEditParameters& parameters_;
    const double warmthShift_;
    const double tintShift_;
    const double saturationFactor_;
    const double centerX_;
    const double centerY_;
    const bool needsTonalWeights_;
    const bool monochrome_;
    // Nine KiB per request replaces repeated per-channel arithmetic. Keep
    // RGB-dependent tone and saturation in their original arithmetic order.
    std::array<double, 256> exposedChannels_;
    std::array<ColorChannels, 256> channelColors_;
    std::array<QRgb, 256> finalChannels_;
    bool finalChannelsExact_ = false;
    // Cache at most 512 KiB on one axis, never a second full raster. Keep divisions and
    // additions in their original order to preserve exact output pixels.
    std::vector<double> horizontalRadiusSquared_;
};
int editWorkerCount(int width, int height)
{
    const quint64 area = quint64(width) * quint64(height);
    if (area < 4'000'000) {
        return 1;
    }
    bool workerOverrideValid = false;
    const int workerOverride =
        qEnvironmentVariableIntValue("LICASA_CPU_EDIT_WORKERS", &workerOverrideValid);
    const int availableWorkers = std::max(1, QThread::idealThreadCount());
    const int workerLimit = workerOverrideValid ? std::clamp(workerOverride, 1, 16)
                                                : std::min(16, std::max(1, availableWorkers - 2));
    const int sizeWorkers = int(std::min<quint64>(area / 2'000'000, quint64(workerLimit)));
    return std::max(1, std::min({sizeWorkers, height / 128, width}));
}

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
    const int width = image.width();
    const int height = image.height();
    const qsizetype stride = image.bytesPerLine();
    uchar* const pixels = image.bits(); // Detach once before worker threads touch disjoint rows.
    if (!pixels) {
        return false;
    }
    const bool vignette = parameters.vignette > 0.0;
    const bool symmetricRows = vignette && height >= 3;
    const bool symmetricColumns = vignette && width >= 3;
    const int workRows = symmetricRows ? (height + 1) / 2 : height;
    const auto processRows = [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            if (editCancelled(cancelled)) {
                return;
            }
            auto* row = reinterpret_cast<QRgb*>(pixels + qsizetype(y) * stride);
            if (!vignette) {
                if (plan.hasFinalChannels()) {
                    for (int x = 0; x < width; ++x) {
                        row[x] = plan.applyFinalChannels(row[x]);
                    }
                } else {
                    for (int x = 0; x < width; ++x) {
                        row[x] = plan.apply(row[x], 1.0);
                    }
                }
                continue;
            }
            const int oppositeY = symmetricRows ? height - 1 - y : y;
            auto* oppositeRow = reinterpret_cast<QRgb*>(pixels + qsizetype(oppositeY) * stride);
            const double verticalRadiusSquared = plan.rowRadiusSquared(y);
            const int columns = symmetricColumns ? (width + 1) / 2 : width;
            // Centers are exact integer/half-integer coordinates for dimensions
            // >= 3. Opposite coordinates differ only in sign before squaring, so
            // all four pixels use precisely the same original radial factor.
            for (int x = 0; x < columns; ++x) {
                const int oppositeX = symmetricColumns ? width - 1 - x : x;
                if ((qAlpha(row[x]) | qAlpha(row[oppositeX]) | qAlpha(oppositeRow[x]) |
                     qAlpha(oppositeRow[oppositeX])) == 0) {
                    continue;
                }
                const double factor = plan.vignetteFactor(x, verticalRadiusSquared);
                row[x] = plan.apply(row[x], factor);
                if (oppositeX != x) {
                    row[oppositeX] = plan.apply(row[oppositeX], factor);
                }
                if (oppositeY != y) {
                    oppositeRow[x] = plan.apply(oppositeRow[x], factor);
                    if (oppositeX != x) {
                        oppositeRow[oppositeX] = plan.apply(oppositeRow[oppositeX], factor);
                    }
                }
            }
        }
    };

    const int workers = editWorkerCount(width, height);
    if (workers <= 1) {
        processRows(0, workRows);
        return !editCancelled(cancelled);
    }

    std::vector<std::thread> threads;
    const auto boundary = [workRows, workers](int range) {
        return int(qint64(workRows) * range / workers);
    };
    int nextRange = 1;
    for (; nextRange < workers; ++nextRange) {
        const int first = boundary(nextRange);
        const int last = boundary(nextRange + 1);
        try {
            threads.emplace_back(processRows, first, last);
        } catch (const std::exception&) {
            break;
        }
    }
    processRows(0, boundary(1));
    for (int range = nextRange; range < workers; ++range) {
        processRows(boundary(range), boundary(range + 1));
    }
    for (auto& thread : threads) {
        thread.join();
    }
    return !editCancelled(cancelled);
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
    const int width = image.width();
    const int height = image.height();
    const int workers = editWorkerCount(width, height);
    const int rowsPerWorker = workers > 1 ? 3 : 2;
    // Each band preserves its overwritten rows and, when parallel, the original
    // next-band boundary. Copy every halo before any worker changes the raster.
    // Scratch remains proportional to row width, with at most 16 bands.
    QImage rows(width, rowsPerWorker * workers, QImage::Format_ARGB32);
    uchar* const pixels = image.bits(); // Detach before sharing disjoint row bands.
    if (rows.isNull() || !pixels) {
        return false;
    }
    const qsizetype stride = image.bytesPerLine();
    const qsizetype scratchStride = rows.bytesPerLine();
    uchar* const scratch = rows.bits();
    const size_t rowBytes = size_t(width) * sizeof(QRgb);
    const auto boundary = [height, workers](int range) {
        return 1 + int(qint64(height - 2) * range / workers);
    };
    for (int range = 0; range < workers; ++range) {
        if (editCancelled(cancelled)) {
            return false;
        }
        uchar* const previous = scratch + qsizetype(range * rowsPerWorker) * scratchStride;
        std::memcpy(previous, pixels + qsizetype(boundary(range) - 1) * stride, rowBytes);
        if (workers > 1) {
            std::memcpy(previous + 2 * scratchStride,
                        pixels + qsizetype(boundary(range + 1)) * stride, rowBytes);
        }
    }
    const double strength = bounded * 0.48;

    // Four 8-bit neighbors have only 1,021 possible sums. For larger rasters,
    // evaluate each center/sum pair once using the original double arithmetic
    // and rounding. The 256 KiB table is independent of image dimensions; small
    // images retain the direct stencil to avoid paying its setup cost. If the
    // optional table allocation fails, the direct stencil also remains usable.
    QImage channelTable;
    if (qint64(image.width() - 2) * (image.height() - 2) >= 262144) {
        channelTable = QImage(1024, 256, QImage::Format_Grayscale8);
    }
    if (!channelTable.isNull()) {
        for (int center = 0; center < 256; ++center) {
            if (editCancelled(cancelled)) {
                return false;
            }
            auto* tableRow = channelTable.scanLine(center);
            for (int sum = 0; sum <= 1020; ++sum) {
                const double detail = center * 4.0 - sum;
                tableRow[sum] = static_cast<uchar>(clampColorChannel(center + detail * strength));
            }
        }
    }
    const uchar* table = channelTable.constBits();

    const auto processRange = [&](int range) {
        auto* previous =
            reinterpret_cast<QRgb*>(scratch + qsizetype(range * rowsPerWorker) * scratchStride);
        auto* current = reinterpret_cast<QRgb*>(reinterpret_cast<uchar*>(previous) + scratchStride);
        const auto* halo = reinterpret_cast<const QRgb*>(
            scratch + qsizetype(range * rowsPerWorker + 2) * scratchStride);
        const int last = boundary(range + 1);
        for (int y = boundary(range); y < last; ++y) {
            if (editCancelled(cancelled)) {
                return;
            }
            auto* destination = reinterpret_cast<QRgb*>(pixels + qsizetype(y) * stride);
            std::memcpy(current, destination, rowBytes);
            const auto* next =
                workers > 1 && y + 1 == last
                    ? halo
                    : reinterpret_cast<const QRgb*>(pixels + qsizetype(y + 1) * stride);
            for (int x = 1; x < width - 1; ++x) {
                const QRgb center = current[x];
                const auto sharpenChannel = [strength, table](int centerValue, int left, int right,
                                                              int top, int bottom) {
                    if (table) {
                        return int(table[(centerValue << 10) + left + right + top + bottom]);
                    }
                    const double detail = centerValue * 4.0 - left - right - top - bottom;
                    return clampColorChannel(centerValue + detail * strength);
                };

                destination[x] = qRgba(
                    sharpenChannel(qRed(center), qRed(current[x - 1]), qRed(current[x + 1]),
                                   qRed(previous[x]), qRed(next[x])),
                    sharpenChannel(qGreen(center), qGreen(current[x - 1]), qGreen(current[x + 1]),
                                   qGreen(previous[x]), qGreen(next[x])),
                    sharpenChannel(qBlue(center), qBlue(current[x - 1]), qBlue(current[x + 1]),
                                   qBlue(previous[x]), qBlue(next[x])),
                    qAlpha(center));
            }
            std::swap(previous, current);
        }
    };
    if (workers <= 1) {
        processRange(0);
        return !editCancelled(cancelled);
    }
    std::vector<std::thread> threads;
    int nextRange = 1;
    for (; nextRange < workers; ++nextRange) {
        try {
            threads.emplace_back(processRange, nextRange);
        } catch (const std::exception&) {
            break;
        }
    }
    processRange(0);
    for (int range = nextRange; range < workers; ++range) {
        processRange(range);
    }
    for (auto& thread : threads) {
        thread.join();
    }
    return !editCancelled(cancelled);
}

QImage transposedForMirroredQuarterTurn(const QImage& source, bool positiveDirection)
{
    QImage result(source.height(), source.width(), source.format());
    if (result.isNull()) {
        return {};
    }
    result.setColorSpace(source.colorSpace());
    result.setDevicePixelRatio(source.devicePixelRatio());
    result.setDotsPerMeterX(source.dotsPerMeterX());
    result.setDotsPerMeterY(source.dotsPerMeterY());
    result.setOffset(source.offset());
    for (const QString& key : source.textKeys()) {
        result.setText(key, source.text(key));
    }
    const uchar* const original = source.constBits();
    uchar* const transposed = result.bits();
    const qsizetype sourceStride = source.bytesPerLine();
    const qsizetype destinationStride = result.bytesPerLine();
    // Square tiles keep both raster axes in cache. Copy the stored 32-bit pixel
    // rather than painting it: hidden RGB under alpha zero must survive geometry.
    constexpr int tileSize = 32;
    for (int top = 0; top < source.height();) {
        const int bottom = top + std::min(tileSize, source.height() - top);
        for (int left = 0; left < source.width();) {
            const int right = left + std::min(tileSize, source.width() - left);
            int x = left;
#if defined(__SSE2__)
            for (; x <= right - 4; x += 4) {
                int y = top;
                for (; y <= bottom - 4; y += 4) {
                    const auto load = [&](int row) {
                        return _mm_loadu_si128(reinterpret_cast<const __m128i*>(
                            original + qsizetype(row) * sourceStride + qsizetype(x) * 4));
                    };
                    const __m128i first = load(y);
                    const __m128i second = load(y + 1);
                    const __m128i third = load(y + 2);
                    const __m128i fourth = load(y + 3);
                    const __m128i firstPairLow = _mm_unpacklo_epi32(first, second);
                    const __m128i firstPairHigh = _mm_unpackhi_epi32(first, second);
                    const __m128i secondPairLow = _mm_unpacklo_epi32(third, fourth);
                    const __m128i secondPairHigh = _mm_unpackhi_epi32(third, fourth);
                    const __m128i columns[] = {_mm_unpacklo_epi64(firstPairLow, secondPairLow),
                                               _mm_unpackhi_epi64(firstPairLow, secondPairLow),
                                               _mm_unpacklo_epi64(firstPairHigh, secondPairHigh),
                                               _mm_unpackhi_epi64(firstPairHigh, secondPairHigh)};
                    for (int column = 0; column < 4; ++column) {
                        const int destinationY =
                            positiveDirection ? x + column : source.width() - 1 - x - column;
                        const int destinationX = positiveDirection ? y : source.height() - y - 4;
                        const __m128i values = positiveDirection
                                                   ? columns[column]
                                                   : _mm_shuffle_epi32(columns[column], 0x1b);
                        _mm_storeu_si128(reinterpret_cast<__m128i*>(
                                             transposed + destinationY * destinationStride +
                                             qsizetype(destinationX) * 4),
                                         values);
                    }
                }
                for (; y < bottom; ++y) {
                    const auto* row = reinterpret_cast<const quint32*>(original + y * sourceStride);
                    for (int column = 0; column < 4; ++column) {
                        const int destinationY =
                            positiveDirection ? x + column : source.width() - 1 - x - column;
                        auto* destination = reinterpret_cast<quint32*>(
                            transposed + destinationY * destinationStride);
                        destination[positiveDirection ? y : source.height() - 1 - y] =
                            row[x + column];
                    }
                }
            }
#endif
            for (; x < right; ++x) {
                const int destinationY = positiveDirection ? x : source.width() - 1 - x;
                auto* destination =
                    reinterpret_cast<quint32*>(transposed + destinationY * destinationStride);
                for (int y = top; y < bottom; ++y) {
                    const auto* row = reinterpret_cast<const quint32*>(original + y * sourceStride);
                    destination[positiveDirection ? y : source.height() - 1 - y] = row[x];
                }
            }
            left = right;
        }
        top = bottom;
    }
    return result;
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
    if (queryString.isEmpty()) {
        return {};
    }

    QUrlQuery query;
    query.setQuery(queryString);
    return editParametersFromQuery(query);
}

ImageEditParameters editParametersFromQuery(const QUrlQuery& query)
{
    ImageEditParameters parameters;
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
    const QStringList allowed = {QStringLiteral("format"),        QStringLiteral("scale"),
                                 QStringLiteral("quality"),       QStringLiteral("width"),
                                 QStringLiteral("height"),        QStringLiteral("compression"),
                                 QStringLiteral("progressive"),   QStringLiteral("optimized"),
                                 QStringLiteral("lossless"),      QStringLiteral("nearest"),
                                 QStringLiteral("keepAlpha"),     QStringLiteral("matte"),
                                 QStringLiteral("colorMode"),     QStringLiteral("paletteColors"),
                                 QStringLiteral("dither"),        QStringLiteral("threshold"),
                                 QStringLiteral("invert"),        QStringLiteral("ascii"),
                                 QStringLiteral("iconSize"),      QStringLiteral("iconCrop"),
                                 QStringLiteral("hotspotX"),      QStringLiteral("hotspotY"),
                                 QStringLiteral("dpi"),           QStringLiteral("srgb"),
                                 QStringLiteral("stripMetadata"), QStringLiteral("copyOnly"),
                                 QStringLiteral("symbol")};
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (!allowed.contains(it.key())) {
            options.validationError = QStringLiteral("Unknown export option: %1").arg(it.key());
            break;
        }
    }
    options.scale =
        boundedFinite(values.value(QStringLiteral("scale"), 1.0).toDouble(), 0.01, 2.0, 1.0);
    options.quality = std::clamp(values.value(QStringLiteral("quality"), 95).toInt(), 1, 100);
    options.format = values.value(QStringLiteral("format")).toByteArray().trimmed().toLower();
    if (options.format == QByteArrayLiteral("jpg")) {
        options.format = QByteArrayLiteral("jpeg");
    }
    if (options.format == QByteArrayLiteral("original")) {
        options.format.clear();
    }
    if (options.format == "tif") {
        options.format = "tiff";
    }
    if (options.format == "jfif") {
        options.format = "jpeg";
    }
    options.width = values.value(QStringLiteral("width"), 0).toInt();
    options.height = values.value(QStringLiteral("height"), 0).toInt();
    options.compression =
        std::clamp(values.value(QStringLiteral("compression"), 50).toInt(), 0, 100);
    options.progressive = values.value(QStringLiteral("progressive"), false).toBool();
    options.optimized = values.value(QStringLiteral("optimized"), true).toBool();
    options.lossless = values.value(QStringLiteral("lossless"), false).toBool();
    options.nearest = values.value(QStringLiteral("nearest"), false).toBool();
    options.keepAlpha = values.value(QStringLiteral("keepAlpha"), true).toBool();
    options.matte = values.value(QStringLiteral("matte"), QStringLiteral("#ffffff")).toString();
    options.colorMode = std::clamp(values.value(QStringLiteral("colorMode"), 0).toInt(), 0, 2);
    options.paletteColors =
        std::clamp(values.value(QStringLiteral("paletteColors"), 216).toInt(), 8, 216);
    options.dither = values.value(QStringLiteral("dither"), true).toBool();
    options.threshold = std::clamp(values.value(QStringLiteral("threshold"), 128).toInt(), 0, 255);
    options.invert = values.value(QStringLiteral("invert"), false).toBool();
    options.ascii = values.value(QStringLiteral("ascii"), false).toBool();
    options.iconSize = values.value(QStringLiteral("iconSize"), 0).toInt();
    options.iconCrop = values.value(QStringLiteral("iconCrop"), false).toBool();
    options.hotspotX = values.value(QStringLiteral("hotspotX"), 0).toInt();
    options.hotspotY = values.value(QStringLiteral("hotspotY"), 0).toInt();
    options.dpi = values.value(QStringLiteral("dpi"), 0).toInt();
    options.srgb = values.value(QStringLiteral("srgb"), false).toBool();
    options.stripMetadata = values.value(QStringLiteral("stripMetadata"), true).toBool();
    options.copyOnly = values.value(QStringLiteral("copyOnly"), false).toBool();
    options.symbol =
        values.value(QStringLiteral("symbol"), QStringLiteral("licasa_image")).toString();
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

    int quarterTurns = safeParameters.quarterTurns;
    bool flipHorizontal = safeParameters.flipHorizontal;
    bool flipVertical = safeParameters.flipVertical;
    bool croppedBeforeGeometry = false;
    if (safeParameters.hasCrop() && (quarterTurns != 0 || flipHorizontal || flipVertical) &&
        image.depth() == 32 && image.width() >= 3 && image.height() >= 3 &&
        image.offset().isNull() &&
        (!image.colorSpace().isValid() || image.colorSpace() == QColorSpace::SRgb)) {
        const QSize sourceSize = image.size();
        const QSize rotatedSize = quarterTurns % 2 ? sourceSize.transposed() : sourceSize;
        QRect selected = cropRectForImage(rotatedSize, safeParameters);
        if (selected.width() >= 3 && selected.height() >= 3) {
            if (flipHorizontal) {
                selected.moveLeft(rotatedSize.width() - selected.x() - selected.width());
            }
            if (flipVertical) {
                selected.moveTop(rotatedSize.height() - selected.y() - selected.height());
            }
            QRect sourceCrop = selected;
            if (quarterTurns == 1) {
                sourceCrop =
                    QRect(selected.y(), sourceSize.height() - selected.x() - selected.width(),
                          selected.height(), selected.width());
            } else if (quarterTurns == 2) {
                sourceCrop = QRect(sourceSize.width() - selected.x() - selected.width(),
                                   sourceSize.height() - selected.y() - selected.height(),
                                   selected.width(), selected.height());
            } else if (quarterTurns == 3) {
                sourceCrop = QRect(sourceSize.width() - selected.y() - selected.height(),
                                   selected.x(), selected.height(), selected.width());
            }
            // Orthogonal transforms commute with an integer crop mapped back
            // into source coordinates. Transform only the selected raster. Keep
            // Qt's existing path for formats/profiles with special metadata
            // behavior and for narrow transforms.
            image = image.copy(sourceCrop);
            if (image.isNull()) {
                return false;
            }
            croppedBeforeGeometry = true;
        }
    }
    if (quarterTurns == 2 && image.depth() == 32 && image.width() > 1 && image.height() > 1 &&
        image.offset().isNull() &&
        (!image.colorSpace().isValid() || image.colorSpace() == QColorSpace::SRgb)) {
        // A half turn is two mirrors. Compose the user's mirrors before touching
        // the raster so owned images can rotate in place, with no temporary image.
        // Other formats/profiles/offsets retain Qt's transform metadata behavior.
        quarterTurns = 0;
        flipHorizontal = !flipHorizontal;
        flipVertical = !flipVertical;
    } else if ((quarterTurns == 1 || quarterTurns == 3) && flipHorizontal && flipVertical) {
        quarterTurns = (quarterTurns + 2) % 4;
        flipHorizontal = false;
        flipVertical = false;
    }
    if (quarterTurns != 0) {
        QTransform transform;
        transform.rotate(quarterTurns * 90.0);
        if ((quarterTurns == 1 || quarterTurns == 3) && flipHorizontal != flipVertical &&
            (image.format() == QImage::Format_RGB32 || image.format() == QImage::Format_ARGB32 ||
             image.format() == QImage::Format_ARGB32_Premultiplied ||
             image.format() == QImage::Format_RGBX8888 ||
             image.format() == QImage::Format_RGBA8888 ||
             image.format() == QImage::Format_RGBA8888_Premultiplied) &&
            image.width() > 1 && image.height() > 1 && image.offset().isNull() &&
            (!image.colorSpace().isValid() || image.colorSpace() == QColorSpace::SRgb)) {
            // A quarter-turn followed by one mirror is a transpose or an
            // anti-transpose, requiring only one destination traversal.
            image = transposedForMirroredQuarterTurn(image, (quarterTurns == 1) == flipHorizontal);
            flipHorizontal = false;
            flipVertical = false;
        } else {
            image = image.transformed(transform, Qt::FastTransformation);
        }
        if (image.isNull()) {
            return false;
        }
    }

    if (editCancelled(cancelled)) {
        return false;
    }

    if (flipHorizontal || flipVertical) {
        image = std::move(image).mirrored(flipHorizontal, flipVertical);
        if (image.isNull()) {
            return false;
        }
    }

    if (safeParameters.hasCrop() && !croppedBeforeGeometry) {
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
