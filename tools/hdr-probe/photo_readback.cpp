#include "photo_readback.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtCore/qfloat16.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace PhotoReadback {
namespace {
using Pixel = std::array<qfloat16, 4>;

Pixel readPixel(const uchar* row, int x)
{
    Pixel pixel;
    std::memcpy(pixel.data(), row + x * sizeof(Pixel), sizeof(Pixel));
    return pixel;
}

QJsonObject sampleAt(const QImage& frame, QPoint point)
{
    const auto rgba = readPixel(frame.constScanLine(point.y()), point.x());
    return {{"x", point.x()},
            {"y", point.y()},
            {"rgba", QJsonArray{float(rgba[0]), float(rgba[1]), float(rgba[2]), float(rgba[3])}}};
}
} // namespace

std::optional<QList<QPoint>> samplePoints(QSize size, const QStringList& coordinates)
{
    if (coordinates.size() > 16) {
        return std::nullopt;
    }
    QList<QPoint> points;
    const QRect bounds(QPoint(), size);
    for (const auto& coordinate : coordinates) {
        const auto pair = coordinate.split(',');
        if (pair.size() != 2) {
            return std::nullopt;
        }
        bool xValid, yValid;
        const QPoint point(pair[0].toInt(&xValid), pair[1].toInt(&yValid));
        if (!xValid || !yValid || !bounds.contains(point)) {
            return std::nullopt;
        }
        points.append(point);
    }
    return points;
}

bool verify(const QImage& frame, const QImage& upload, float backing, const QList<QPoint>& points,
            QJsonObject& details)
{
    qsizetype mismatchCount = 0;
    qsizetype pixelsAboveWhite = 0;
    float maxError = 0;
    float minimum = std::numeric_limits<float>::infinity();
    float peak = -std::numeric_limits<float>::infinity();
    for (int y = 0; y < frame.height(); ++y) {
        const auto* readback = frame.constScanLine(y);
        const auto* source = upload.constScanLine(y);
        for (int x = 0; x < frame.width(); ++x) {
            const auto actual = readPixel(readback, x);
            const auto input = readPixel(source, x);
            const float contribution = backing * (1.0f - float(input[3]));
            bool matched = float(actual[3]) == 1.0f;
            bool aboveWhite = false;
            for (size_t channel = 0; channel < 3; ++channel) {
                const float value = float(actual[channel]);
                const float expected = float(input[channel]) + contribution;
                const float error = std::abs(value - expected);
                maxError = std::max(maxError, error);
                minimum = std::min(minimum, value);
                peak = std::max(peak, value);
                aboveWhite |= value > 1.0f;
                matched &= error <= std::max(0.0006f, std::abs(expected) * 0.001f);
            }
            pixelsAboveWhite += aboveWhite;
            if (!matched) {
                if (mismatchCount == 0) {
                    details["first_mismatch"] = sampleAt(frame, QPoint(x, y));
                }
                ++mismatchCount;
            }
        }
    }
    QJsonArray samples;
    for (const auto point : points) {
        samples.append(sampleAt(frame, point));
    }
    details["samples"] = samples;
    details["pixels_checked"] = qint64(frame.width()) * frame.height();
    details["mismatched_pixels"] = qint64(mismatchCount);
    details["max_rgb_error"] = maxError;
    details["min_linear_component"] = minimum;
    details["peak_linear_component"] = peak;
    details["pixels_above_sdr_white"] = qint64(pixelsAboveWhite);
    return mismatchCount == 0;
}
} // namespace PhotoReadback
